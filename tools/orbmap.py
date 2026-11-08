"""The .orbmap container (radar-plan.md §2.4), FORMAT 2: one place's map at one whole
Mercator zoom, as vectors in drawing order plus its landmarks. This is the reference
writer and reader; the hub's JS writer (web/runtime/maps@1/orbmap.js) produces the same
bytes (tools/host_orbmap_js_test.py), and the C
reader (main/map/orb_map_raster.c) is held to it by tools/host_map_raster_test.py.

R0 prototype: it builds a place's six files from OpenMapTiles vector tiles already on
disk (decoded with tools/mvt.py), which is what the hub tool will do in the browser.

    python3 tools/orbmap.py build <tiles_dir> <lat> <lon> <out_dir> [name | --place N]
    python3 tools/orbmap.py info  <file.orbmap>

--place N names the files as the OS looks them up under /System/Maps (map_name below,
main/map/orb_map.c's orb_map_file_name): place 0 is home, 1-3 the places.

Layout (little-endian):
  header, 36 B:
    char  magic[4]   "ORBM"
    u16   format     1
    u16   header_len 36
    i32   lat_e6, lon_e6
    u8    zoom; u8 pad[3]
    u32   n_features, n_marks
    u32   body_len, body_crc32  (zlib CRC-32 over the body)
  body:
    features, fills then lines, each in drawing order (DRAW below): u8 role, u8 kind (0 fill, 1 line), varint n_rings,
      then per ring varint n_points and n_points zigzag-varint (dx, dy) pairs, each
      the delta from the ring's previous point (the first from 0,0). Units are 1/16 px
      at the file's zoom, relative to the place.
    marks: u8 kind (0 city, 1 town, 2 village, 3 airport), u8 rank, zigzag-varint x, y
      (1/16 px), u8 name_len, name bytes (UTF-8, at most 23).
"""
import glob, math, os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MAGIC, FORMAT, HEADER = b"ORBM", 2, 36
UNIT = 16                       # 1/16 px
HALF = 330                      # the window: ±330 px at the file's zoom
CLIP = (HALF + 4) * UNIT        # clipped a little outside it, so no edge is drawn
TOL = UNIT // 4                 # Douglas-Peucker tolerance: a quarter pixel

WATER, LAND, URBAN, LAKE, RIVER, ROAD2, ROAD1, STATE, BORDER, COAST, CITY, AIRPORT, RUNWAY = range(13)
ROAD3, ROAD4 = 13, 14           # FORMAT 2: the roads in four roles (radar-plan §6.9)
FILL, LINE = 0, 1
# FORMAT 2's roads: motorway and trunk (ROAD1), primary (ROAD2), secondary (ROAD3), tertiary
# (ROAD4), each written only from the level it can first be shown at, a step before an app
# shows it by default (radar's road_step), so the wide levels stay light (owner, 2026-11-07:
# "way too high" a density at DFW and London).
ROAD_FROM = {ROAD1: 6, ROAD2: 7, ROAD3: 8, ROAD4: 9}
# The order lines are drawn in, minor roads under major ones; fills first, then lines.
DRAW = {RIVER: 0, ROAD4: 1, ROAD3: 2, ROAD2: 3, ROAD1: 4, STATE: 5, BORDER: 6, COAST: 7, RUNWAY: 8}
MARK_CITY, MARK_TOWN, MARK_VILLAGE, MARK_AIRPORT = range(4)

# What each OpenMapTiles class becomes. Order here is drawing order: fills, then lines.
def classify(layer, p, gtype, zoom=11):
    c = p.get("class")
    if layer == "landuse" and c in ("residential", "commercial", "industrial", "retail") and gtype == 3:
        return URBAN, FILL
    if layer == "water" and gtype == 3:
        return (WATER if c == "ocean" else LAKE), FILL
    if layer == "waterway" and c in ("river", "canal"):
        return RIVER, LINE
    if layer == "transportation":
        role = {"motorway": ROAD1, "trunk": ROAD1, "primary": ROAD2, "secondary": ROAD3,
                "tertiary": ROAD4}.get(c)
        return (role, LINE) if role is not None and zoom >= ROAD_FROM[role] else None
    if layer == "boundary" and p.get("maritime") != 1:
        if p.get("admin_level") == 4: return STATE, LINE
        if p.get("admin_level") == 2: return BORDER, LINE
    if layer == "aeroway" and c == "runway":
        return RUNWAY, (FILL if gtype == 3 else LINE)
    return None

def e4(e6):
    """Millionths to ten-thousandths, half away from zero, with C's truncating division."""
    v = e6 + 50 if e6 >= 0 else e6 - 50
    return v // 100 if v >= 0 else -((-v) // 100)


def map_name(place, lat_e6, lon_e6, z):
    """The OS's file name for a place's level: "<place>-<lat_e4>_<lon_e4>-z<z>.orbmap".
    FORMAT 2's name ("-f2"): each FORMAT names its files apart (orb_map.h), so the hub's sweep
    of names no place uses is what retires an older format's files."""
    return "%d-%d_%d-z%d-f2.orbmap" % (place, e4(lat_e6), e4(lon_e6), z)


def world(lat, lon, z):
    n = 256.0 * 2 ** z; s = math.sin(math.radians(max(-85.0, min(85.0, lat))))
    return (lon + 180) / 360 * n, (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n

# ---- geometry ---------------------------------------------------------------------

def clip_poly(ring, lo, hi):
    """Sutherland-Hodgman against the square [lo, hi]^2."""
    def edge(pts, inside, cut):
        out = []
        for i, cur in enumerate(pts):
            prev = pts[i - 1]
            if inside(cur):
                if not inside(prev): out.append(cut(prev, cur))
                out.append(cur)
            elif inside(prev):
                out.append(cut(prev, cur))
        return out
    def cx(v):
        return lambda a, b: (v, a[1] + (b[1] - a[1]) * (v - a[0]) / (b[0] - a[0]))
    def cy(v):
        return lambda a, b: (a[0] + (b[0] - a[0]) * (v - a[1]) / (b[1] - a[1]), v)
    pts = ring[:-1] if len(ring) > 1 and ring[0] == ring[-1] else ring
    for inside, cut in ((lambda p: p[0] >= lo, cx(lo)), (lambda p: p[0] <= hi, cx(hi)),
                        (lambda p: p[1] >= lo, cy(lo)), (lambda p: p[1] <= hi, cy(hi))):
        if not pts: return []
        pts = edge(pts, inside, cut)
    return pts

def clip_line(line, lo, hi):
    """Liang-Barsky per segment; consecutive visible pieces join into one run."""
    runs, cur = [], []
    for a, b in zip(line, line[1:]):
        t0, t1, dx, dy = 0.0, 1.0, b[0] - a[0], b[1] - a[1]
        ok = True
        for p, q in ((-dx, a[0] - lo), (dx, hi - a[0]), (-dy, a[1] - lo), (dy, hi - a[1])):
            if p == 0:
                if q < 0: ok = False; break
            else:
                t = q / p
                if p < 0: t0 = max(t0, t)
                else: t1 = min(t1, t)
        if not ok or t0 > t1:
            if len(cur) > 1: runs.append(cur)
            cur = []; continue
        pa = (a[0] + t0 * dx, a[1] + t0 * dy); pb = (a[0] + t1 * dx, a[1] + t1 * dy)
        if not cur or cur[-1] != pa:
            if len(cur) > 1: runs.append(cur)
            cur = [pa]
        cur.append(pb)
        if t1 < 1.0:
            if len(cur) > 1: runs.append(cur)
            cur = []
    if len(cur) > 1: runs.append(cur)
    return runs

def simplify(pts, tol, closed):
    """Douglas-Peucker, iterative. A closed ring keeps its first point and the point
    farthest from it as anchors."""
    n = len(pts)
    if n < 3: return pts
    keep = [False] * n; keep[0] = keep[-1] = True
    spans = [(0, n - 1)]
    if closed:
        far = max(range(n), key=lambda i: (pts[i][0] - pts[0][0]) ** 2 + (pts[i][1] - pts[0][1]) ** 2)
        keep[far] = True; spans = [(0, far), (far, n - 1)]
    t2 = tol * tol
    while spans:
        i, j = spans.pop()
        if j - i < 2: continue
        ax, ay = pts[i]; bx, by = pts[j]; dx, dy = bx - ax, by - ay; L = dx * dx + dy * dy
        best, bi = -1.0, -1
        for k in range(i + 1, j):
            px, py = pts[k]
            if L == 0: d = (px - ax) ** 2 + (py - ay) ** 2
            else:
                t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L))
                d = (px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2
            if d > best: best, bi = d, k
        if best > t2:
            keep[bi] = True; spans += [(i, bi), (bi, j)]
    return [p for p, k in zip(pts, keep) if k]

def rnd(v):
    """The one rounding rule both writers use, half up: Python's round() goes to even, the
    hub's JS port cannot, and the bytes must agree (tools/host_orbmap_js_test.py)."""
    return math.floor(v + 0.5)

def quantise(pts):
    out = []
    for x, y in pts:
        q = (rnd(x), rnd(y))
        if not out or out[-1] != q: out.append(q)
    return out

# ---- encoding ---------------------------------------------------------------------

def varint(v, out):
    while True:
        b = v & 0x7F; v >>= 7
        if v: out.append(b | 0x80)
        else: out.append(b); return

def zz(v): return (v << 1) ^ (v >> 31) if v >= 0 else ((-v) << 1) - 1

def encode(lat, lon, zoom, features, marks):
    """features: [(role, kind, [ring, ...])], rings of (x, y) in 1/16 px; marks:
    [(kind, rank, x, y, name)]. Fills are written by role, lines in DRAW order, stable."""
    body = bytearray()
    feats = sorted(features, key=lambda f: (f[1], DRAW.get(f[0], 16 + f[0]) if f[1] else f[0]))   # fills by role, lines by DRAW
    for role, kind, rings in feats:
        body += bytes((role, kind)); varint(len(rings), body)
        for ring in rings:
            varint(len(ring), body); px = py = 0
            for x, y in ring:
                varint(zz(x - px), body); varint(zz(y - py), body); px, py = x, y
    for kind, rank, x, y, name in marks:
        nb = name.encode("utf-8")[:23]
        body += bytes((kind, min(rank, 255))); varint(zz(x), body); varint(zz(y), body)
        body.append(len(nb)); body += nb
    head = struct.pack("<4sHHiiB3xIIII", MAGIC, FORMAT, HEADER, rnd(lat * 1e6),
                       rnd(lon * 1e6), zoom, len(feats), len(marks), len(body),
                       zlib.crc32(bytes(body)) & 0xFFFFFFFF)
    assert len(head) == HEADER
    return head + bytes(body)

def decode(buf):
    magic, fmt, hl, lat, lon, zoom, nf, nm, blen, crc = struct.unpack_from("<4sHHiiB3xIIII", buf)
    if magic != MAGIC or fmt != FORMAT: raise ValueError("not an orbmap FORMAT %d" % FORMAT)
    body = buf[hl:hl + blen]
    if len(body) != blen or zlib.crc32(body) & 0xFFFFFFFF != crc: raise ValueError("body crc")
    i = 0
    def rv():
        nonlocal i
        v = s = 0
        while True:
            b = body[i]; i += 1; v |= (b & 0x7F) << s; s += 7
            if b < 0x80: return v
    def rz():
        v = rv(); return (v >> 1) ^ -(v & 1)
    feats = []
    for _ in range(nf):
        role, kind = body[i], body[i + 1]; i += 2; rings = []
        for _ in range(rv()):
            n = rv(); x = y = 0; ring = []
            for _ in range(n):
                x += rz(); y += rz(); ring.append((x, y))
            rings.append(ring)
        feats.append((role, kind, rings))
    marks = []
    for _ in range(nm):
        kind, rank = body[i], body[i + 1]; i += 2; x = rz(); y = rz()
        n = body[i]; i += 1; name = body[i:i + n].decode("utf-8"); i += n
        marks.append((kind, rank, x, y, name))
    return dict(lat=lat / 1e6, lon=lon / 1e6, zoom=zoom, features=feats, marks=marks)

# ---- building a place's files from tiles ----------------------------------------------

def build(tiles, lat, lon, zoom):
    import mvt
    cx, cy = world(lat, lon, zoom)
    feats, marks, seen, raw = [], [], set(), 0
    found = [tuple(map(int, os.path.basename(p)[:-4].split("_"))) + (p,)
             for p in glob.glob(os.path.join(tiles, "%d_*.pbf" % zoom))]
    for _, tx, ty, p in sorted(found, key=lambda f: (f[1], f[2])):    # by column, then row
        for layer, (ext, fs) in mvt.read(p).items():
            for gtype, props, rings in fs:
                to = lambda g: ((tx * 256 + g[0] * 256 / ext - cx) * UNIT, (ty * 256 + g[1] * 256 / ext - cy) * UNIT)
                if layer in ("place", "aerodrome_label") and rings:
                    x, y = to(rings[0][0])
                    if abs(x) > HALF * UNIT or abs(y) > HALF * UNIT: continue
                    if layer == "place":
                        kind = {"city": MARK_CITY, "town": MARK_TOWN, "village": MARK_VILLAGE}.get(props.get("class"))
                        name = props.get("name:latin") or props.get("name_en") or props.get("name")
                        rank = int(props.get("rank", 20))
                    else:
                        kind, name, rank = MARK_AIRPORT, props.get("iata") or props.get("icao"), (1 if props.get("iata") else 8)
                    if kind is None or not name or (kind, name) in seen: continue
                    seen.add((kind, name)); marks.append((kind, rank, rnd(x), rnd(y), name))
                    continue
                c = classify(layer, props, gtype, zoom)
                if not c: continue
                role, kind = c; out = []
                for r in rings:
                    pts = [to(g) for g in r]; raw += len(pts)
                    if kind == FILL:
                        q = quantise(simplify(clip_poly(pts, -CLIP, CLIP), TOL, True))
                        if len(q) >= 3: out.append(q)
                    else:
                        for run in clip_line(pts, -CLIP, CLIP):
                            q = quantise(simplify(run, TOL, False))
                            if len(q) >= 2: out.append(q)
                if out: feats.append((role, kind, out))
    marks.sort(key=lambda m: (m[1], m[0]))
    return encode(lat, lon, zoom, feats, marks), raw

def main(argv):
    if len(argv) >= 6 and argv[1] == "build":
        tiles, lat, lon, out = argv[2], float(argv[3]), float(argv[4]), argv[5]
        place = int(argv[7]) if len(argv) > 7 and argv[6] == "--place" else None
        name = argv[6] if len(argv) > 6 and place is None else "map"
        os.makedirs(out, exist_ok=True)
        for z in range(6, 12):
            buf, raw = build(tiles, lat, lon, z)
            fn = (map_name(place, rnd(lat * 1e6), rnd(lon * 1e6), z) if place is not None
                  else "%s-z%d.orbmap" % (name, z))
            path = os.path.join(out, fn)
            open(path, "wb").write(buf)
            d = decode(buf); verts = sum(len(r) for _, _, rings in d["features"] for r in rings)
            print("z%d  %8d B  features %5d  vertices %7d (tiles %7d)  marks %3d  %s"
                  % (z, len(buf), len(d["features"]), verts, raw, len(d["marks"]), path))
        return 0
    if len(argv) == 3 and argv[1] == "info":
        d = decode(open(argv[2], "rb").read())
        verts = sum(len(r) for _, _, rings in d["features"] for r in rings)
        print("z%d at %.6f,%.6f: %d features, %d vertices, %d marks"
              % (d["zoom"], d["lat"], d["lon"], len(d["features"]), verts, len(d["marks"])))
        return 0
    print(__doc__); return 2

if __name__ == "__main__":
    sys.exit(main(sys.argv))
