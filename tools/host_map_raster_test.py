"""The map renderer on the host (radar-plan.md §2.4, R0): main/map/orb_map_raster.c
compiled with gcc and driven through ctypes over .orbmap files written by
tools/orbmap.py.

    python3 tools/host_map_raster_test.py                     # checks on a synthetic file
    python3 tools/host_map_raster_test.py <dir_of_orbmaps> [png_out_dir]

Checks: the header and CRC refusals, a fill with a hole, a dashed line's duty, and,
given real files, every level rendered at its own scale, at 1.41x and 0.71x, and
rotated, with the time each took on this host (the device's time is R0's bench)."""
import ctypes, glob, math, os, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import orbmap

W = 466
TW = (W + 7) // 8
RASTER_B = TW * TW * 32          # 8x8 tiles of 32 B (orb_map_raster.h)

class Style(ctypes.Structure):
    _fields_ = [("scale", ctypes.c_float), ("rot_rad", ctypes.c_float), ("role_on", ctypes.c_uint16),
                ("mark_on", ctypes.c_uint8), ("width", ctypes.c_uint8 * 16), ("dash", ctypes.c_uint8 * 16),
                ("clock_us", ctypes.c_void_p), ("mark", ctypes.c_void_p), ("mark_ctx", ctypes.c_void_p)]

class Stats(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in ("features", "fills", "lines", "edges", "segments", "marks", "skipped",
                                               "fill_us", "line_us", "max_fill_edges")]

def build_lib(mutation=None):
    d = tempfile.mkdtemp(prefix="orbmap-")
    so = os.path.join(d, "liborbmap.so")
    src = os.path.join(ROOT, "main/map/orb_map_raster.c")
    if mutation:
        text = open(src).read()
        if mutation[0] not in text:
            raise SystemExit("mutation %r does not apply -- rewrite the bench" % (mutation[0][:50],))
        src = os.path.join(d, "orb_map_raster.c")
        open(src, "w").write(text.replace(mutation[0], mutation[1], 1))
    r = subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-std=c11", "-shared", "-fPIC",
                        "-I", os.path.join(ROOT, "main/map"), src, "-o", so, "-lm"], capture_output=True, text=True)
    if r.returncode:
        return None
    lib = ctypes.CDLL(so)
    lib.orb_map_check.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_int)]
    lib.orb_map_render.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(Style), ctypes.c_char_p,
                                   ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(Stats)]
    lib.orb_map_render_src.argtypes = [ctypes.POINTER(Src), ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t,
                                       ctypes.POINTER(Style), ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                                       ctypes.POINTER(Stats)]
    return lib

# radar-plan §6.20: a file read in pieces (orb_map_src_t)
READ = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32)
class Src(ctypes.Structure):
    _fields_ = [("read", READ), ("ctx", ctypes.c_void_p), ("len", ctypes.c_uint32)]

def render_src(lib, buf, st, win):
    def rd(_ctx, off, dst, n):
        if off + n > len(buf): return -1
        ctypes.memmove(dst, buf[off:off + n], n)
        return n
    cb = READ(rd)
    src = Src(cb, None, len(buf))
    ras = ctypes.create_string_buffer(RASTER_B); stats = Stats()
    hdr = ctypes.create_string_buffer(36); w = ctypes.create_string_buffer(win)
    rc = lib.orb_map_render_src(ctypes.byref(src), hdr, w, win, ctypes.byref(st), ras, SCRATCH, len(SCRATCH), ctypes.byref(stats))
    return rc, ras.raw, stats

def style(scale=1.0, rot=0.0, dashes=None):
    s = Style(); s.scale = scale; s.rot_rad = rot; s.role_on = 0xFFFF; s.mark_on = 0x0B   # city, town, airport
    for r, w in ((4, 1), (5, 1), (6, 2), (7, 1), (8, 2), (12, 2)): s.width[r] = w
    for r, d in (dashes or {}).items(): s.dash[r] = d
    return s

SCRATCH = ctypes.create_string_buffer(384 * 1024)   # the device's size (map_bench)

def render(lib, buf, st):
    ras = ctypes.create_string_buffer(RASTER_B); stats = Stats()
    t0 = time.perf_counter()
    rc = lib.orb_map_render(buf, len(buf), ctypes.byref(st), ras, SCRATCH, len(SCRATCH), ctypes.byref(stats))
    return rc, ras.raw, stats, (time.perf_counter() - t0) * 1000

def nib(x, y): return (((y >> 3) * TW + (x >> 3)) << 6) | ((y & 7) << 3 | (x & 7))

def role_at(ras, x, y):
    i = nib(x, y); b = ras[i >> 1]
    return b & 0x0F if i & 1 else b >> 4

failures = []
QUIET = False
def check(ok, what):
    if not QUIET: print(("ok    " if ok else "FAIL  ") + what)
    if not ok: failures.append(what)

MUTATIONS = [
    ("if (keep) memmove(s->win, r->p, keep);", ";"),
    ("    return ~s.crc == rd32(hdr + 32) ? ORB_MAP_OK : ORB_MAP_E_CRC;", "    return ORB_MAP_OK;"),
    ("    while (refill(&tail)) tail.p = tail.end;", ""),
    ("if (crc32_z(buf + hl, blen) != rd32(buf + 32)) return ORB_MAP_E_CRC;", ";"),
    ("if (hl < HDR || hl > len || len - hl < blen) return ORB_MAP_E_TRUNC;\n    if (crc32_z", "if (crc32_z"),
    ("return (int32_t)(v >> 1) ^ -(int32_t)(v & 1);", "return (int32_t)(v >> 1);"),
    ("for (int i = 0; i + 1 < nx; i += 2)", "for (int i = 0; i + 1 < nx; i += 1)"),
    ("if (on && np > 2 && fit) fit = add_edge(edges, ecap, &ne, px, py, fx0, fy0);", ";"),
    ("if (t < p->pat[i]) return !(i & 1);", "if (t < p->pat[i]) return 1;"),
    ("const xf_t t = { k * cs, -k * sn, k * sn, k * cs };", "const xf_t t = { k * cs, k * sn, -k * sn, k * cs };"),
    ("const float k = st->scale / (float)ORB_MAP_UNIT", "const float k = st->scale / 8.0f"),
    ("int r0 = (int)ceilf(y0 - 0.5f), r1 = (int)ceilf(y1 - 0.5f);", "int r0 = (int)y0, r1 = (int)y1;"),
    ("if (tx0 == 0 && tx1 == 8) { t[0] = t[1] = t[2] = t[3] = vv; }", "if (tx0 == 0 && tx1 == 8) { t[0] = t[1] = vv; }"),
]

def synthetic_checks(lib):
    U = orbmap.UNIT
    sq = lambda h: [(-h * U, -h * U), (h * U, -h * U), (h * U, h * U), (-h * U, h * U)]
    feats = [(orbmap.LAKE, orbmap.FILL, [sq(100), sq(40)]),                       # a lake with an island
             (orbmap.STATE, orbmap.LINE, [[(-200 * U, 150 * U), (200 * U, 150 * U)]])]
    buf = orbmap.encode(0.0, 0.0, 8, feats, [(orbmap.MARK_AIRPORT, 1, 0, -170 * U, "TST")])
    z = ctypes.c_int()
    check(lib.orb_map_check(buf, len(buf), ctypes.byref(z)) == 0 and z.value == 8, "a good file passes its check")
    bad = bytearray(buf); bad[-1] ^= 1
    check(lib.orb_map_check(bytes(bad), len(bad), None) == -3, "a flipped body byte fails the CRC")
    check(lib.orb_map_check(buf[:-5], len(buf) - 5, None) == -2, "a short file is refused as truncated")
    check(lib.orb_map_check(b"ORBX" + buf[4:], len(buf), None) == -1, "a wrong magic is refused")
    rc, ras, st, _ = render(lib, buf, style(dashes={orbmap.STATE: 1}))
    c = W // 2
    check(rc == 0, "the synthetic file renders")
    check(role_at(ras, c + 70, c) == orbmap.LAKE, "inside the lake is lake")
    check(role_at(ras, c, c) == orbmap.LAND, "the island is land (even-odd)")
    check(role_at(ras, c + 150, c) == orbmap.LAND, "outside the lake is land")
    row = [role_at(ras, x, c + 150) == orbmap.STATE for x in range(c - 180, c + 180)]
    duty = sum(row) / len(row)
    check(0.45 < duty < 0.70, "a short-dashed line is on about 4/7 of its length (%.2f)" % duty)
    check(role_at(ras, c, c - 170) == orbmap.AIRPORT, "the airport mark lands where it was put")
    rc, ras, st, _ = render(lib, buf, style(scale=0.5))
    check(role_at(ras, c + 35, c) == orbmap.LAKE and role_at(ras, c + 60, c) == orbmap.LAND, "scale 0.5 halves the lake")
    rc, ras, st, _ = render(lib, buf, style(rot=math.radians(90)))
    check(role_at(ras, c + 170, c) == orbmap.AIRPORT and role_at(ras, c, c - 170) != orbmap.AIRPORT,
          "rotated 90 degrees clockwise, the mark north of centre moves east")
    # A fill's rows are sampled at their centres: the top edge at y 132.75 takes row 133
    # (centre 133.5) and not row 132; the bottom edge at 333.25 takes row 332, not 333.
    q = lambda v: int(round(v * U))
    rect = [(q(-50), q(-100.25)), (q(50), q(-100.25)), (q(50), q(100.25)), (q(-50), q(100.25))]
    rbuf = orbmap.encode(0.0, 0.0, 8, [(orbmap.LAKE, orbmap.FILL, [rect])], [])
    rc, rras, st, _ = render(lib, rbuf, style())
    check(role_at(rras, c, c - 101) == orbmap.LAND and role_at(rras, c, c - 100) == orbmap.LAKE and
          role_at(rras, c, c + 99) == orbmap.LAKE and role_at(rras, c, c + 100) == orbmap.LAND,
          "a fill's edge between pixel centres takes the rows whose centres it covers")
    # §6.20: streamed through a small window, the same raster, and the CRC still bites
    big_feats = [(orbmap.LAKE, orbmap.FILL, [sq(100), sq(40)])] + [
        (orbmap.ROAD1 if i % 2 else orbmap.STATE, orbmap.LINE,
         [[((-220 + j * 9) * U, (-200 + i * 13 + (j * j) % 7) * U) for j in range(48)]]) for i in range(30)]
    marks = [(orbmap.MARK_CITY, 1, (-150 + 30 * i) * U, (120 - 20 * i) * U, "Place number %d with a long name" % i) for i in range(10)]
    bbuf = orbmap.encode(0.0, 0.0, 8, big_feats, marks)
    want = render(lib, bbuf, style(dashes={orbmap.STATE: 1}))[1]
    same = all(render_src(lib, bbuf, style(dashes={orbmap.STATE: 1}), w)[:2] == (0, want) for w in (512, 700, 4096, 65536))
    check(len(bbuf) > 2000 and same, "streamed through a 512 B to 64 KB window, a %d B file draws the same raster" % len(bbuf))
    MARK = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_float, ctypes.c_float,
                            ctypes.c_void_p, ctypes.c_uint8)
    def names_of(fn):
        got = []
        cb = MARK(lambda _c, k, r, x, y, nm, n: got.append(ctypes.string_at(nm, n).decode("utf-8", "replace")))
        st = style(); st.mark = ctypes.cast(cb, ctypes.c_void_p)
        fn(st)
        return got
    mem = names_of(lambda st: render(lib, bbuf, st))
    streamed = names_of(lambda st: render_src(lib, bbuf, st, 512))
    check(len(mem) == 10 and streamed == mem, "and its landmarks' names come whole across the window's refills")
    bad = bytearray(bbuf); bad[len(bad) // 2] ^= 0x40
    check(render_src(lib, bytes(bad), style(), 512)[0] == -3, "a streamed file with a flipped body byte answers the CRC's error")
    check(render_src(lib, bbuf[:-30], style(), 512)[0] == -2, "a streamed file cut short is truncated")
    rc, ras, st, _ = render(lib, buf, style(rot=math.radians(45)))
    # Rotated 45 degrees the corners lie on the axis: the island's reaches 40*sqrt2 = 57 px,
    # the lake's 100*sqrt2 = 141 px (unrotated, both stop at 40 and 100).
    check(role_at(ras, c + 50, c) == orbmap.LAND and role_at(ras, c + 120, c) == orbmap.LAKE
          and role_at(ras, c + 150, c) == orbmap.LAND,
          "rotated 45 degrees, the island's and the lake's corners reach out along the axis")

PAL = {0: (10, 16, 30), 1: (30, 36, 46), 2: (45, 51, 63), 3: (22, 46, 92), 4: (40, 82, 140), 5: (64, 70, 82),
       6: (150, 130, 80), 7: (200, 200, 212), 8: (240, 240, 250), 9: (200, 200, 212), 10: (255, 255, 255),
       11: (255, 210, 110), 12: (255, 210, 110)}

def png(ras, path):
    try:
        from PIL import Image
    except ImportError:
        return False
    im = Image.new("RGB", (W, W)); px = []
    for y in range(W):
        for x in range(W): px.append(PAL.get(role_at(ras, x, y), (255, 0, 255)))
    im.putdata(px); im.save(path); return True

def real_checks(lib, d, out):
    for path in sorted(glob.glob(os.path.join(d, "*.orbmap")), key=lambda p: int(p.rsplit("-z", 1)[1][:-7])):
        buf = open(path, "rb").read(); name = os.path.basename(path)[:-7]
        check(lib.orb_map_check(buf, len(buf), None) == 0, "%s passes its check" % name)
        for label, st in (("1.00x", style()), ("1.41x", style(1.41)), ("0.71x", style(0.71)),
                          ("rot 30", style(1.0, math.radians(30))),
                          ("dashed", style(1.0, 0, {5: 1, 7: 2, 8: 4}))):
            best = 1e9
            for _ in range(3):
                rc, ras, stats, ms = render(lib, buf, st); best = min(best, ms)
            check(rc == 0 and stats.skipped == 0, "%s %-6s renders: %4d fills %6d edges (largest %5d) %6d segments, %6.1f ms on this host"
                  % (name, label, stats.fills, stats.edges, stats.max_fill_edges, stats.segments, best))
            if out and label in ("1.00x", "dashed", "rot 30"):
                png(ras, os.path.join(out, "%s-%s.png" % (name, label.replace(" ", "").replace(".", ""))))

def run_mutations():
    global QUIET
    caught = 0
    for m in MUTATIONS:
        failures.clear()
        QUIET = True
        lib = build_lib(m)
        if lib is not None:
            synthetic_checks(lib)
        QUIET = False
        hit = lib is None or bool(failures)
        caught += hit
        print("%s mutant: %s" % ("caught" if hit else "MISSED", m[1][:60]))
    failures.clear()
    print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
    return caught == len(MUTATIONS)


def main(argv):
    if "--mutations" in argv:
        ok = run_mutations()
        lib = build_lib()
        synthetic_checks(lib)
        print("all map raster checks pass" if ok and not failures else "map raster checks FAILED")
        return 0 if ok and not failures else 1
    lib = build_lib()
    synthetic_checks(lib)
    if len(argv) > 1:
        out = argv[2] if len(argv) > 2 else None
        if out: os.makedirs(out, exist_ok=True)
        real_checks(lib, argv[1], out)
    print("\n%d failure(s)" % len(failures) if failures else "\nall map raster checks pass")
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
