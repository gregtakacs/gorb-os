#!/usr/bin/env python3
"""The hub's map tool against its originals (radar-plan.md §2.4, R4).

web/runtime/maps@1/orbmap.js is three ports, and this holds each to its source:

- the writer: the same vector tiles built into .orbmap levels by tools/orbmap.py and by
  the JS (its own MVT reader included) must be the same bytes, level for level;
- the rasteriser: the same files rendered by main/map/orb_map_raster.c (gcc) and by the
  JS, at several scales, rotations, widths and every dash pattern, must be the same
  raster, nibble for nibble. The C works in single-precision floats and the JS rounds
  every step to one, so "close" is a failure here;
- the level picker and the file name, against the C's numbers.

The tiles are made here, by a small MVT encoder, from seeded random geometry in every
layer the tool reads (holes, lines crossing the window, maritime borders, places past
the window, long and non-ASCII names): no map data lives in the repo (§0 ruling 7).

The JS half needs node; the host-suite CI job has none, so there it is skipped and
`make check-editor` runs it.

Run:  python3 tools/host_orbmap_js_test.py              (rc 0 = pass)
      python3 tools/host_orbmap_js_test.py --mutations  (proves the comparisons bite)
"""
import json, math, os, random, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import orbmap
import host_map_raster_test as HR

JS = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime", "maps@1", "orbmap.js")
NODE = shutil.which("node") or next((p for p in [os.path.expanduser(
    "~/.local/opt/node-v22.11.0-linux-x64/bin/node"), os.path.expanduser(
    "~/.local/share/nodeenv-gorb/bin/node")] if os.path.exists(p)), None)

PLACES = [(32.8900, -97.1600), (51.5072, -0.1276), (-33.8688, 151.2093)]

# ---- a minimal MVT v2 encoder ------------------------------------------------------------

def _vi(v):
    out = bytearray()
    while True:
        b = v & 0x7F; v >>= 7
        if v: out.append(b | 0x80)
        else: out.append(b); return bytes(out)

def _key(f, t): return _vi(f << 3 | t)
def _ld(f, b): return _key(f, 2) + _vi(len(b)) + b
def _zz(v): return (v << 1) ^ (v >> 63)

def _value(v):
    if isinstance(v, bool): return _key(7, 0) + _vi(int(v))
    if isinstance(v, str): return _ld(1, v.encode("utf-8"))
    if isinstance(v, float): return _key(3, 1) + struct.pack("<d", v)
    if v >= 0: return _key(5, 0) + _vi(v)                 # uint
    return _key(6, 0) + _vi(_zz(v))                       # sint

def _geom(gtype, rings):
    cmds, x, y = [], 0, 0
    for r in rings:
        pts = r[:-1] if gtype == 3 else r
        cmds.append(1 | 1 << 3)
        cmds += [_zz(pts[0][0] - x), _zz(pts[0][1] - y)]; x, y = pts[0]
        if len(pts) > 1:
            cmds.append(2 | (len(pts) - 1) << 3)
            for px, py in pts[1:]:
                cmds += [_zz(px - x), _zz(py - y)]; x, y = px, py
        if gtype == 3: cmds.append(7 | 1 << 3)
    return b"".join(_vi(c) for c in cmds)

def tile_bytes(layers):
    """layers: {name: [(gtype, props, rings)]} -> MVT bytes."""
    out = b""
    for name, feats in layers.items():
        keys, vals, kix, vix, body = [], [], {}, {}, b""
        for gtype, props, rings in feats:
            tags = []
            for k, v in props.items():
                if k not in kix: kix[k] = len(keys); keys.append(k)
                vk = (type(v).__name__, v)
                if vk not in vix: vix[vk] = len(vals); vals.append(v)
                tags += [kix[k], vix[vk]]
            fb = _ld(2, b"".join(_vi(t) for t in tags)) + _key(3, 0) + _vi(gtype) + _ld(4, _geom(gtype, rings))
            body += _ld(2, fb)
        lb = _key(15, 0) + _vi(2) + _ld(1, name.encode()) + body
        lb += b"".join(_ld(3, k.encode()) for k in keys) + b"".join(_ld(4, _value(v)) for v in vals)
        lb += _key(5, 0) + _vi(4096)
        out += _ld(3, lb)
    return out

def random_tile(rng):
    P = lambda: (rng.randrange(-80, 4176), rng.randrange(-80, 4176))
    def poly(n, cx=None, cy=None, r=None):
        cx = rng.randrange(0, 4096) if cx is None else cx; cy = rng.randrange(0, 4096) if cy is None else cy
        r = rng.randrange(80, 1800) if r is None else r
        pts = []
        for i in range(n):
            a = 2 * math.pi * i / n; rr = r * rng.uniform(0.5, 1.0)
            pts.append((int(cx + rr * math.cos(a)), int(cy + rr * math.sin(a))))
        return pts + [pts[0]]
    def line(n): return [P() for _ in range(n)]
    L = {}
    L["water"] = [(3, {"class": rng.choice(["ocean", "lake", "river"])}, [poly(rng.randrange(5, 40))])
                  for _ in range(rng.randrange(1, 4))]
    cx, cy = rng.randrange(800, 3300), rng.randrange(800, 3300)
    L["water"].append((3, {"class": "lake"}, [poly(30, cx, cy, 700), poly(12, cx, cy, 200)[::-1]]))   # a hole
    L["landuse"] = [(3, {"class": rng.choice(["residential", "commercial", "industrial", "retail", "farmland"])},
                     [poly(rng.randrange(4, 60))]) for _ in range(rng.randrange(2, 7))]
    L["waterway"] = [(2, {"class": rng.choice(["river", "canal", "stream"])}, [line(rng.randrange(2, 30))])
                     for _ in range(rng.randrange(1, 5))]
    L["transportation"] = [(2, {"class": rng.choice(["motorway", "trunk", "primary", "secondary", "tertiary", "minor"])},
                            [line(rng.randrange(2, 40)), line(rng.randrange(2, 6))]) for _ in range(rng.randrange(3, 12))]
    L["boundary"] = [(2, {"admin_level": rng.choice([2, 4, 6]), "maritime": rng.choice([0, 0, 1])}, [line(rng.randrange(2, 20))])
                     for _ in range(rng.randrange(1, 4))]
    L["aeroway"] = [(3, {"class": "runway"}, [poly(4)]), (2, {"class": "runway"}, [line(2)]), (2, {"class": "taxiway"}, [line(3)])]
    names = ["Fort Worth", "Dallas", "Grapevine", "Saint-Étienne-du-Rouvray-sur-Mer", "Zürich", "東京都", "Arlington"]
    L["place"] = [(1, dict({"class": rng.choice(["city", "town", "village", "hamlet"]), "rank": rng.randrange(1, 20)},
                           **{rng.choice(["name", "name_en", "name:latin"]): rng.choice(names)}), [[P()]])
                  for _ in range(rng.randrange(1, 8))]
    L["aerodrome_label"] = [(1, dict({"icao": "K%03d" % rng.randrange(1000)}, **({"iata": "D%02d" % rng.randrange(99)}
                             if rng.random() < 0.5 else {})), [[P()]]) for _ in range(rng.randrange(0, 3))]
    # a runway and an airport in every tile, drawn without a random draw so the stream (and the
    # f32 mutant's coverage) is unchanged: the OS v1.6.9 bare rule is then compared on random tiles
    L["aeroway"].append((2, {"class": "runway"}, [[(2000, 2000), (2100, 2060)]]))
    L["aerodrome_label"].append((1, {"icao": "KZZZ", "iata": "ZZ1"}, [[(3700, 300)]]))
    return L

def write_tiles(d, lat, lon, rng):
    """Every tile each level's window touches, as orbmap.py and the JS both read them."""
    for z in range(6, 12):
        cx, cy = orbmap.world(lat, lon, z)
        for x in range(int(math.floor((cx - 330) / 256)), int(math.floor((cx + 330) / 256)) + 1):
            for y in range(int(math.floor((cy - 330) / 256)), int(math.floor((cy + 330) / 256)) + 1):
                open(os.path.join(d, "%d_%d_%d.pbf" % (z, x, y)), "wb").write(tile_bytes(random_tile(rng)))

# ---- the JS side --------------------------------------------------------------------------

DRIVER = r"""
const fs = require('fs'), path = require('path');
const G = require(process.argv[2]);
const job = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
const out = {};
if (job.build) {
  const b = job.build;
  out.levels = {};
  for (const z of G.ZOOMS) {
    const want = G.tilesFor(b.lat, b.lon, z).map(t => t.ox + '_' + t.y);
    const tiles = fs.readdirSync(b.dir).filter(f => f.startsWith(z + '_')).map(f => {
      const [, x, y] = f.slice(0, -4).split('_').map(Number);
      return { x, y, layers: G.readTile(new Uint8Array(fs.readFileSync(path.join(b.dir, f)))) };
    });
    const got = tiles.map(t => t.x + '_' + t.y).sort();
    if (JSON.stringify(got) !== JSON.stringify(want.slice().sort())) out.tiles_differ = z;
    const r = G.build(tiles, b.lat, b.lon, z);
    fs.writeFileSync(path.join(b.out, 'js-z' + z + '.orbmap'), r.bytes);
    out.levels[z] = r.raw;
  }
}
if (job.render) {
  out.renders = [];
  for (const r of job.render) {
    const buf = new Uint8Array(fs.readFileSync(r.file));
    const res = G.render(buf, r.style);
    fs.writeFileSync(r.out, res.raster);
    out.renders.push({ err: res.err, marks: res.marks.length });
  }
}
if (job.misc) {
  out.misc = job.misc.picks.map(p => G.pickLevel(p[0], p[1]));
  out.names = job.misc.names.map(n => G.mapName(n[0], n[1], n[2], n[3]));
  out.info = G.info(new Uint8Array(fs.readFileSync(job.misc.file)));
}
process.stdout.write(JSON.stringify(out));
"""

def node(js_path, job, d):
    jp = os.path.join(d, "job.json"); open(jp, "w").write(json.dumps(job))
    dp = os.path.join(d, "driver.js"); open(dp, "w").write(DRIVER)
    r = subprocess.run([NODE, dp, js_path, jp], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr[-800:]
    return json.loads(r.stdout), None

STYLES = [
    dict(scale=1.0, rot=0.0, dash={}),
    dict(scale=1.41, rot=0.0, dash={5: 1, 7: 2, 8: 4}),
    dict(scale=0.71, rot=math.radians(33.3), dash={4: 3, 6: 2}),
    dict(scale=1.2, rot=math.radians(-120), dash={5: 4, 7: 1, 12: 3}, widths={4: 3, 5: 4, 6: 4, 8: 3}),
    # v1.2.38 (flights-map-plan §2.1): the marks' looks and rank caps, the fills' floors, the clip
    dict(scale=1.0, rot=0.0, dash={0: 3, 2: 8, 3: 5, 10: 4, 11: 1}, widths={10: 1 | 3 << 3, 11: 7 | 6 << 3}, clip=92),
    dict(scale=1.3, rot=math.radians(17), dash={3: 12, 10: 2}, widths={10: 2 | 5 << 3, 11: 3 | 7 << 3}, clip=110, bare=True),
    dict(scale=0.8, rot=0.0, dash={}, widths={10: 5 | 2 << 3, 11: 4 | 9 << 3}),
    dict(scale=1.0, rot=0.0, dash={11: 8}, widths={10: 6 | 4 << 3, 11: 6 | 15 << 3}, clip=116, bare=True),
    dict(scale=1.1, rot=math.radians(8), dash={4: 3, 6: 1}, widths={4: 2 | 6 << 3, 6: 3 | 2 << 3, 12: 1 | 31 << 3}),
]

def c_style(s):
    st = HR.style(s["scale"], s["rot"], s["dash"])
    for r, w in s.get("widths", {}).items(): st.width[r] = w
    st.clip = s.get("clip", 0)
    if s.get("bare"):                                   # OS v1.6.9: the AIRPORT role off, RUNWAY on, marks BARE
        st.role_on = st.role_on & ~(1 << orbmap.AIRPORT)
        st.mark_on = st.mark_on | 0x80
    return st

def js_style(s):
    st = c_style(s)
    return {"scale": st.scale, "rot_rad": st.rot_rad, "role_on": st.role_on, "mark_on": st.mark_on,
            "width": list(st.width), "dash": list(st.dash), "clip": st.clip}

def suite(js_path, quiet=False):
    fails = []
    def ck(ok, what):
        if not quiet: print(("ok    " if ok else "FAIL  ") + what)
        if not ok: fails.append(what)
    lib = HR.build_lib()
    if lib is None:
        ck(False, "the C rasteriser compiles"); return fails
    d = tempfile.mkdtemp(prefix="orbmap-js-")
    try:
        rng = random.Random(20261107)
        files = []
        for pi, (lat, lon) in enumerate(PLACES):
            td = os.path.join(d, "tiles%d" % pi); os.makedirs(td)
            write_tiles(td, lat, lon, rng)
            out, err = node(js_path, {"build": {"dir": td, "lat": lat, "lon": lon, "out": td}}, d)
            if out is None:
                ck(False, "the JS builder runs (%s)" % err); return fails
            ck("tiles_differ" not in out, "place %d: the JS asks for the tiles orbmap.py reads" % pi)
            same = 0
            for z in range(6, 12):
                py, raw = orbmap.build(td, lat, lon, z)
                js = open(os.path.join(td, "js-z%d.orbmap" % z), "rb").read()
                same += py == js
                files.append(os.path.join(td, "js-z%d.orbmap" % z))
            ck(same == 6, "place %d (%.2f, %.2f): six levels, the JS writes orbmap.py's bytes (%d/6)" % (pi, lat, lon, same))
        # a dense synthetic file: many-edged fills with holes, long lines, every role
        feats = []
        for role in range(13):
            for _ in range(4):
                n = rng.randrange(3, 400)
                pts = [(rng.randrange(-5300, 5300), rng.randrange(-5300, 5300)) for _ in range(n)]
                feats.append((role, rng.choice([0, 1]), [pts, pts[: max(3, n // 3)]]))
        marks = [(rng.randrange(4), rng.randrange(1, 30), rng.randrange(-5300, 5300), rng.randrange(-5300, 5300),
                  "M%d" % i) for i in range(40)]
        dense = os.path.join(d, "dense.orbmap"); open(dense, "wb").write(orbmap.encode(1.0, 2.0, 9, feats, marks))
        files.append(dense)

        jobs, want = [], []
        for fi, f in enumerate(files):
            buf = open(f, "rb").read()
            for si, s in enumerate(STYLES):
                if fi % 3 and si % 2 and f != dense: continue        # every style on a third of the files
                err, ras = HR.render(lib, buf, c_style(s))[:2]
                o = os.path.join(d, "r%d_%d.bin" % (fi, si))
                jobs.append({"file": f, "style": js_style(s), "out": o}); want.append((o, bytes(ras), err, s, f))
        out, err = node(js_path, {"render": jobs}, d)
        if out is None:
            ck(False, "the JS rasteriser runs (%s)" % err); return fails
        diff_files, worst = 0, 0
        for (o, ras, cerr, s, f), r in zip(want, out["renders"]):
            js = open(o, "rb").read()
            if js != ras or r["err"] != cerr:
                diff_files += 1
                worst = max(worst, sum(1 for y in range(466) for x in range(466)
                                       if HR.role_at(js, x, y) != HR.role_at(ras, x, y)))
        ck(diff_files == 0, "%d renders (%d files x up to %d styles: scales 0.71-1.41, rotated, widths 1-4, "
           "every dash): the JS raster is the C's, nibble for nibble (%d differ, worst %d px)"
           % (len(want), len(files), len(STYLES), diff_files, worst))

        picks = [(32890000, 15000), (32890000, 60000), (32890000, 160000), (51507200, 40000), (-33868800, 900000),
                 (0, 5000), (70000000, 300000)]
        names = [(0, 32890000, -97160000, 9), (3, -33868850, 151209349, 11), (1, -50, 49, 6), (2, 51507249, -127651, 7)]
        out, err = node(js_path, {"misc": {"picks": picks, "names": names, "file": files[0]}}, d)
        if out is None:
            ck(False, "the JS misc runs (%s)" % err); return fails
        cz = []
        for lat_e6, r in picks:
            c = math.cos(lat_e6 / 1e6 * math.pi / 180.0); want_m = r / (466 / 2)
            v = math.log2(156543.03392 * c / want_m)
            z = int(math.floor(abs(v) + 0.5)) * (1 if v >= 0 else -1)
            z = max(6, min(11, z))
            cz.append((z, struct.unpack("<f", struct.pack("<f", 156543.03392 * c / 2.0 ** z / want_m))[0]))
        ck([(m["z"], m["scale"]) for m in out["misc"]] == cz, "the level picker is orb_map.c's (z and float scale)")
        ck(out["names"] == [orbmap.map_name(*n) for n in names], "file names are the OS's (e4 rounding included)")
        ck(out["info"] and out["info"]["zoom"] == 6, "info reads a header and checks the CRC")
    finally:
        shutil.rmtree(d, ignore_errors=True)
    return fails

MUTATIONS = [
    # (Math.round is no mutant: it rounds halves up too, the same rule as floor(v + 0.5))
    ("function rnd(v) { return Math.floor(v + 0.5); }", "function rnd(v) { return Math.trunc(v); }"),
    ("var CLIP = (HALF + 4) * UNIT, TOL = UNIT / 4;", "var CLIP = (HALF + 4) * UNIT, TOL = UNIT / 3;"),
    ("return p[0][1] - q[0][1] || k(p[0]) - k(q[0]) || p[1] - q[1];", "return p[0][1] - q[0][1] || p[0][0] - q[0][0] || p[1] - q[1];"),
    ("ROAD_FROM[ROAD4] = 9;", "ROAD_FROM[ROAD4] = 8;"),
    ("DRAW[ROAD4] = 1; DRAW[ROAD3] = 2;", "DRAW[ROAD4] = 2; DRAW[ROAD3] = 1;"),
    ("x: f32(x0 + f32(f32(f32(f32(r0) + 0.5) - y0) * dxdy))", "x: x0 + (r0 + 0.5 - y0) * dxdy"),
    ("g.x = f32(g.x + g.dxdy);", "g.x = g.x + g.dxdy;"),
    ("pen.phase = f32(pen.phase + f32(f32(len - lead) - f32(stepl * f32(steps))));", "pen.phase = f32(pen.phase);"),
    ("var DASH = [[0, 0, 0, 0], [4, 3, 0, 0],", "var DASH = [[0, 0, 0, 0], [4, 4, 0, 0],"),
    ("if (floorPx > 0 && f32(bx1 - bx0) < floorPx && f32(by1 - by0) < floorPx)", "if (floorPx > 0 && f32(bx1 - bx0) < floorPx)"),
    ("if (st.dash[mrole] && rank > st.dash[mrole]) continue;", ";"),
    ("var hw = Math.trunc(f32(Math.sqrt(rr * rr - ddy * ddy)));", "var hw = Math.round(Math.sqrt(rr * rr - ddy * ddy));"),
    ("o = r - ady; i = o - th - 1;", "o = r - ady; i = o - th;"),
    ("if (shape === 5 || ady > r - th) span(", "if (shape === 5 || ady >= r - th) span("),
    ("var sq = function (n) { return Math.trunc(f32(Math.sqrt(f32(n + 0.5)))); };", "var sq = function (n) { return Math.round(Math.sqrt(n)); };"),
    ("var lfloor = 8 * (st.width[role & 15] >> 3);", "var lfloor = 4 * (st.width[role & 15] >> 3);"),
    ("var pen = { v: role, w: st.width[role & 15] & 7,", "var pen = { v: role, w: st.width[role & 15] & 15,"),
    ("if (layer === 'boundary' && p.maritime !== 1) {", "if (layer === 'boundary') {"),
    ("rank = props.iata ? 1 : 8;", "rank = 1;"),
    ("var nb = utf8e.encode(m[4]).subarray(0, 23);", "var nb = utf8e.encode(m[4]).subarray(0, 24);"),
    ("if (kd === FILL) {\n              var q = quantise(simplify(clipPoly(pts, -CLIP, CLIP), TOL, true));",
     "if (kd === FILL) {\n              var q = quantise(simplify(clipPoly(pts, -CLIP, CLIP), TOL, false));"),
    ("if (Math.abs(v - Math.trunc(v)) === 0.5) z = v < 0 ? Math.floor(v) : Math.ceil(v);\n    z = Math.max(6",
     "z = Math.max(5"),
]

def main():
    if not NODE:
        print("skip  the hub's map tool: no node here (make check-editor runs it)")
        return 0
    fails = suite(JS)
    ok = not fails
    if "--mutations" in sys.argv:
        src, caught = open(JS).read(), 0
        d = tempfile.mkdtemp(prefix="orbmap-mut-")
        try:
            for old, new in MUTATIONS:
                if old not in src:
                    print("mutation does not apply: %r" % old[:60]); ok = False; continue
                p = os.path.join(d, "orbmap.js"); open(p, "w").write(src.replace(old, new, 1))
                hit = bool(suite(p, quiet=True))
                caught += hit
                print("%s  %s" % ("caught" if hit else "MISSED", new[:70].replace("\n", " ")))
        finally:
            shutil.rmtree(d, ignore_errors=True)
        print("%d/%d mutants caught" % (caught, len(MUTATIONS)))
        ok = ok and caught == len(MUTATIONS)
    print("all map-tool checks pass" if ok else "map-tool checks FAILED")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
