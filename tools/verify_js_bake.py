#!/usr/bin/env python3
"""Runs the SHIPPED compiled-clock projection over a JS-baked bundle.

The browser's bakeClock3 claims a bundle the dial will project, not merely one the door
opens. host_clock3_test.py already builds orb_clock3.c (with orb_bundle.c behind it) into
a host harness, so this drives that same binary with --dump3f over any file and prints
what the projection settled on: every moving row (D42), the carriage, the complication
windows and the plate bank. Exit 0 means the dial's own code accepted the bake.

    python3 tools/verify_js_bake.py /tmp/bench_bake.orbtheme

--rows holds the browser baker's moving-row rules to theme_schema's and to the dial
(theme-pool-plan: 32 rows, rows whose frames the app picks). Every case below is judged by
the JS checks (in node) and by theme_schema; both must accept, or both refuse for the
same reason. Then GorbBake.stage3 bakes a plane pool (one bank of frames, many rows naming
it), which orbbundle.read_bundle must read and the shipped orb_clock3.c must project:
    python3 tools/verify_js_bake.py --rows
"""
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import host_clock3_test as H3
from theme_schema import SOURCES, SHAPES, ENDS, CADENCES


# --rows: each case is a list of rows (the AUX table) and the reason both sides must give,
# or None when both must accept. A row is a jumping minute hand unless it says otherwise;
# "pick": n frames and no frame time; "car": n frames with one; "twin": a shadow twin.
ROW_CASES = [
    ("the 32nd seat, ptr31",                  [{"slot": 31}], None),
    ("a 33rd seat, ptr32",                    [{"slot": 32}], "seats 32 moving rows"),
    ("a picked row of 2 frames",              [{"slot": 0, "pick": 2}], None),
    ("a picked row of 64 frames",             [{"slot": 0, "pick": 64}], None),
    ("a picked row of 65 frames",             [{"slot": 0, "pick": 65}], "frames to pick from"),
    ("a carriage of 16 frames",               [{"slot": 0, "car": 16}], None),
    ("a carriage of 17 frames",               [{"slot": 0, "car": 17}], "at most 16 frames"),
    ("a twin under a single plate",           [{"slot": 0}, {"slot": 1, "twin": 1}], None),
    ("a twin under a picked row",             [{"slot": 0, "pick": 7}, {"slot": 1, "twin": 1}], "shade one frame of many"),
    ("sixteen picked rows",                   [{"slot": i, "pick": 49} for i in range(16)], None),
    ("two carriages",                         [{"slot": 0, "car": 4}, {"slot": 1, "car": 4}], "all animated"),
    ("a carriage beside picked rows",         [{"slot": 0, "car": 4}, {"slot": 1, "pick": 9}, {"slot": 2, "pick": 9}], None),
    # theme-text-plan §2: fonts ("font": slot, with overrides) and labels ("label": its columns)
    ("a font and a label reading it",         [{"font": 0}, {"slot": 0, "label": {}}], None),
    ("a label naming tones 0 and 1",          [{"font": 0}, {"font": 1, "n_frames": 95}, {"slot": 0, "label": {"str": 0xFF10}}], None),
    ("a label naming an absent font",         [{"font": 0}, {"slot": 0, "label": {"str": 0xFF20}}], "does not carry"),
    ("a label with no tone 0",                [{"font": 0}, {"slot": 0, "label": {"str": 0xFFFF}}], "no font for tone 0"),
    ("a label that moves",                    [{"font": 0}, {"slot": 0, "label": {"dx": 4}}], "names no motion"),
    ("a label padded shut",                   [{"font": 0}, {"slot": 0, "label": {"radius": 12}}], "leaves no room"),
    ("a font of 94 glyphs",                   [{"font": 0, "n_frames": 94}], "ASCII whole"),
    ("a font at slot 15",                     [{"font": 15}], "seats 15 fonts"),
    ("a font off the origin",                 [{"font": 0, "x": 2}], "sits at 0,0"),
]

ROWS_JS = r"""
const B = require(process.argv[1]), cases = JSON.parse(process.argv[2]), out = { verdicts: [] };
const AUX = B.classId('aux'), MIN = B.SOURCES.indexOf('minute');
const row = (c, n) => Object.assign(B.blankElement(), { n: n, class: AUX, slot: c.slot, w: 8, h: 90, pivot_x: 4, pivot_y: 80, n_frames: 1,
  src: c.twin ? 0 : MIN, span: c.twin ? 0 : 3600, vmax: c.twin ? 0 : 60, cadence: c.twin ? 0 : 3, end: 0, flags: c.twin ? 16 : 0 },
  c.pick ? { n_frames: c.pick, delay: 0 } : c.car ? { n_frames: c.car, delay: 250 } : {});
const TEXT = B.classId('text'), TSRC = B.SOURCES.indexOf('text');
const font = (c, n) => Object.assign(B.blankElement(), { n: n, class: TEXT, slot: c.font, w: 8, h: 14, n_frames: 100,
  pivot_x: 3, pivot_y: 1 }, Object.fromEntries(Object.entries(c).filter(([k]) => k !== 'font')));
const label = (c, n) => Object.assign(B.blankElement(), { n: n, class: AUX, slot: c.slot, x: 100, y: 20, w: 120, h: 24,
  pivot_x: 60, pivot_y: 12, n_frames: 1, kind: 2, src: TSRC, str: 0xFFF0, radius: 4, digit: 1, end: 1 }, c.label);
for (const [name, rows] of cases) {
  const recs = rows.map((c, i) => 'font' in c ? font(c, i + 1) : c.label ? label(c, i + 1) : row(c, i + 1));
  let why = null;
  try { recs.forEach((r, i) => B.checkElement(r, 'elem[' + i + ']')); B.checkClockRows(recs); } catch (e) { why = String(e.message); }
  out.verdicts.push({ name, why, recs });
}
// a plane pool from GorbBake.stage3: 7 shapes x 7 bands = 49 frames, 16 rows naming them
const S = B.stage3({}), W = 466, plate = { w: W, h: W, rgba: new Uint8Array(W * W * 4).fill(30) };
for (let i = 3; i < plate.rgba.length; i += 4) plate.rgba[i] = 255;
S.row({ n: S.id(), class: B.classId('plate'), slot: 0, x: 0, y: 0, w: W, h: W, order: 0, alpha: 255, n_frames: 1 }, plate, false, true);
const frames = [];
for (let f = 0; f < 49; f++) { const p = { w: 38, h: 38, rgba: new Uint8Array(38 * 38 * 4) };
  for (let y = 4; y < 34; y++) for (let x = 4 + (f % 7); x < 34 - (f % 7); x++) p.rgba.set([40 + 30 * Math.floor(f / 7), 200, 90, 255], (y * 38 + x) * 4);
  frames.push(p); }
const rows = Array.from({ length: 16 }, (_, i) => ({ slot: i, x: 40 + (i % 4) * 100, y: 40 + Math.floor(i / 4) * 100, order: 2 + i }));
const recs = S.pool({ class: AUX, w: 38, h: 38, pivot_x: 19, pivot_y: 19, alpha: 255 }, frames, rows, true, false);
let refusedOne = null;
try { S.rowPick({ n: S.id(), class: AUX, slot: 20, w: 38, h: 38, pivot_x: 19, pivot_y: 19 }, frames.concat(frames.slice(0, 16)), true, false); } catch (e) { refusedOne = e.message; }
let refusedSize = null;
try { S.rowPick({ n: S.id(), class: AUX, slot: 21, w: 38, h: 38, pivot_x: 19, pivot_y: 19 }, [frames[0], { w: 40, h: 38, rgba: new Uint8Array(40 * 38 * 4) }], true, false); } catch (e) { refusedSize = e.message; }
// a font of 100 glyph cells and a label naming it (theme-text-plan §2), through the same bake
const glyphs = Array.from({ length: 100 }, (_, g) => { const p = { w: 8, h: 14, rgba: new Uint8Array(8 * 14 * 4) };
  for (let y = 2; y < 12; y++) for (let x = 0; x < 1 + (g % 7); x++) p.rgba.set([230, 230, 230, 255], (y * 8 + x) * 4);
  return p; });
S.rowFrames({ n: S.id(), class: TEXT, slot: 0, w: 8, h: 14, n_frames: 100, pivot_x: 3, pivot_y: 1 }, glyphs, true, false);
const panel = { w: 120, h: 24, rgba: new Uint8Array(120 * 24 * 4).fill(60) };
S.row({ n: S.id(), class: AUX, slot: 30, kind: 2, src: TSRC, x: 100, y: 20, w: 120, h: 24, pivot_x: 60, pivot_y: 12,
        n_frames: 1, order: 30, alpha: 255, str: 0xFFF0, radius: 4, digit: 1, end: 1 }, panel, true, false);
const b = S.finish({ name: 'Plane pool', slug: 'flights', abi: B.GEOM_ABI6 });
require('fs').writeFileSync(process.argv[3], Buffer.from(b.bytes));
out.pool = { frame0s: recs.map((r) => r.frame0), n_frames: recs.map((r) => r.n_frames), delays: recs.map((r) => r.delay), bytes: b.bytes.length, refusedOne, refusedSize };
process.stdout.write(JSON.stringify(out));
"""


def rows_mode():
    """--rows: the JS row rules against theme_schema's, and a stage3 pool through both readers."""
    import json
    import shutil
    import subprocess
    import tempfile
    import orbbundle as ob
    import theme_schema as ts
    node = shutil.which("node") or os.path.expanduser("~/.local/share/nodeenv-gorb/bin/node")
    baker = os.path.join(TOOLS, "orb-manager", "web", "runtime", "baker@1", "orbtheme-baker.js")
    pool = os.path.join(tempfile.mkdtemp(prefix="rows."), "pool.orbtheme")
    js = subprocess.run([node, "-e", ROWS_JS, baker, json.dumps([[n, r] for n, r, _w in ROW_CASES]), pool],
                        capture_output=True, text=True, timeout=300)
    if js.returncode:
        sys.exit("the JS side threw:\n" + (js.stderr or js.stdout)[:2000])
    got = json.loads(js.stdout)
    bad = 0
    for (name, _rows, want), v in zip(ROW_CASES, got["verdicts"]):
        recs = v["recs"]
        try:
            for i, r in enumerate(recs):
                ts.check_element(r, "elem[%d]" % i)
            ts.check_clock_rows(recs)
            pywhy = None
        except ValueError as e:
            pywhy = str(e)
        jsw = v["why"]
        agree = (jsw is None) == (pywhy is None) and (want is None or (want in (jsw or "") and want in (pywhy or "")))
        bad += not agree
        print("%s %-34s JS %s | schema %s" % ("ok  " if agree else "FAIL", name,
              "accepts" if jsw is None else "refuses: " + jsw[:70], "accepts" if pywhy is None else "refuses: " + pywhy[:70]))
    p = got["pool"]
    one = p["refusedOne"] and "frames to pick from" in p["refusedOne"]
    size = p["refusedSize"] and "frames of one size" in p["refusedSize"]
    print("%s stage3 refuses a 65-frame bank and a frame of another size at the bake" % ("ok  " if one and size else "FAIL"))
    bad += not (one and size)
    shared = len(set(p["frame0s"])) == 1 and set(p["n_frames"]) == {49} and set(p["delays"]) == {0}
    print("%s stage3 pool: 16 rows naming one bank of 49 frames, no frame time, a font and a label (%d B)" % ("ok  " if shared else "FAIL", p["bytes"]))
    bad += not shared
    try:
        hdr, secs = ob.read_bundle(pool)
        els = secs[0]["blob"]["elements"]
        aux = [e for e in els if int(e["class"]) == ts.CLASS_AUX]
        planes = [e for e in aux if int(e["kind"]) != ts.SHAPE_TEXT]
        fonts = [e for e in els if int(e["class"]) == ts.CLASS_TEXT]
        read_ok = (secs[0]["blob"]["slug"] == "flights" and len(planes) == 16 and len(fonts) == 1
                   and len(aux) == 17 and all(int(e["n_frames"]) == 49 and not int(e["delay"]) for e in planes))
        ts.check_clock_rows(els)
    except ValueError as e:
        read_ok = False
        print("     orbbundle / theme_schema refused it: %s" % e)
    print("%s orbbundle.read_bundle reads the pool back, and theme_schema seats its rows" % ("ok  " if read_ok else "FAIL"))
    bad += not read_ok
    exe = H3.build()
    rc, out, err = H3.run(exe, "--dump3f", pool, "flights")
    picks = []
    if rc == 0:
        for line in out.splitlines():
            if line.startswith("scalar pick_n"):
                picks = [int(x) for x in line.split()[2:]]
    c_ok = rc == 0 and picks[:16] == [49] * 16 and not any(picks[16:])
    if rc == 0:
        c_ok = c_ok and int(H3.parsed(out)["rows"][-1]["shape"]) == ts.SHAPE_TEXT
    print("%s orb_clock3.c projects it: pick_n %s" % ("ok  " if c_ok else "FAIL", picks[:17] if rc == 0 else (out or err).strip()[:300]))
    bad += not c_ok
    print("%d of %d checks %s" % (bad, len(ROW_CASES) + 4, "FAILED" if bad else "disagree: none"))
    return 1 if bad else 0


def main():
    if sys.argv[1:2] == ["--rows"]:
        return rows_mode()
    if len(sys.argv) < 2:
        sys.exit("usage: verify_js_bake.py BUNDLE [EXTRA BUNDLE ...]")
    exe = H3.build()
    bad = 0
    for path in sys.argv[1:]:
        rc, out, err = H3.run(exe, "--dump3f", path)
        slug = os.path.basename(path)
        if rc != 0:
            bad += 1
            print("PROJECTION REFUSED %s: %s" % (slug, (out or err).strip()[:400]), file=sys.stderr)
            continue
        p = H3.parsed(out)
        sc = {k: v[0] for k, v in p["sc"].items()}
        rows = p["rows"]
        print("%s: projected — %d rows, %dx%d panel, bank %d B, %d complications, beat %s %d/s"
              % (slug, len(rows), p["scr"][0], p["scr"][1], p["art"][1], len(p["comps"]),
                 "sweep" if sc.get("second_sweep") else "step", sc.get("tick_rate", 0)))
        for r in rows:
            s = r["spr"]
            twin = r["flags"] & 256
            print("    row[%d] ptr%d %s src=%s box %dx%d at (%d,%d) colour_off %d turn %d from %d "
                  "travel (%d,%d) over %d..%d end %s cad %s%s%s"
                  % (r["i"], r["tag"], SHAPES[r["shape"]] if r["shape"] < len(SHAPES) else "?",
                     SOURCES[r["src"]], s["w"], s["h"], s["offx"], s["offy"], s["color_off"],
                     r["span"], r["zero"], r["dx"], r["dy"], r["vmin"], r["vmax"],
                     ENDS[r["end"]] if r["end"] < len(ENDS) else "?",
                     CADENCES[r["cadence"]] if r["cadence"] < len(CADENCES) else "?",
                     "" if s["show"] else " hidden", " shadow-twin" if twin else ""))
            if twin and r["i"] > 0:
                # §25.2's identity for a twin: its centre is its row's centre plus the
                # light, and its arbour in its own art is the crop pivot. A light offset
                # that survived the wire as (twin centre - row centre) is the composer's
                # shadow promise; the print is what lets a test say so.
                h = rows[r["i"] - 1]["spr"]
                print("    row[%d] shadow centre (%d,%d) = row centre (%d,%d) + light (%d,%d), "
                      "crop pivot (%d,%d)"
                      % (r["i"], s["centerx"], s["centery"], h["centerx"], h["centery"],
                         s["centerx"] - h["centerx"], s["centery"] - h["centery"],
                         s["pivotx"], s["pivoty"]))
        if sc.get("tourbillon_n"):
            tb = p["spr"]["tourbillon"]
            for f in range(sc["tourbillon_n"]):
                t = tb[f]
                print("    carriage[%d] %dx%d draws at (%d,%d) colour_off %d"
                      % (f, t[0], t[1], t[2], t[3], t[10]))
            print("    carriage: row %d, %d frames @ %d fps"
                  % (sc["car_row"], sc["tourbillon_n"], sc["tourbillon_fps"]))
        for c in p["comps"]:
            (_, tag, kind, src, x, y, cw, ch, count, first, off, fbytes, vmin, scale, show,
             prow, dms) = c
            print("    comp[%d] window src=%s box %dx%d at (%d,%d) %d plates off %d stride %d "
                  "value_min %d depth %d %s"
                  % (tag, SOURCES[src], cw, ch, x, y, count, off, fbytes, vmin, first,
                     "shown" if show else "hidden"))
        if p["bgl"]:
            print("    plate bank: %d extra keyframes at %s"
                  % (len(p["bgl"]), ", ".join(str(p["bgl"][k]) for k in sorted(p["bgl"]))))
    if bad:
        print("%d of %d bundles the compiled dial would refuse" % (bad, len(sys.argv) - 1),
              file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
