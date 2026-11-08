#!/usr/bin/env python3
"""Host check for apps/radar/radar.c, the radar app's model (docs/radar-plan.md §2.2, §2.5).

radar.c is compiled as is, beside the OS's own tokenizer (main/app/orb_json.c, the one the
app's orb_json_next runs). What it pins:

  list     LibreWXR's weather-maps.json shape: 12 past and 6 nowcast frames read, the
           satellite list and every other member walked past, a torn or out-of-order list
           refused, a path too long for the record refused
  url      the frame and list URLs, four decimals rounded half away from zero
  ln       the model's own natural log (the guest's libm has none) against libm's
  window   the frame's draw window against LibreWXR's window_origin, computed here as the
           server does (Python's round, the clamp), for places on four continents at every
           image zoom the ladder fetches
  ladder   each step's radius makes the OS's pick_level choose the step's level at its own
           scale (orb_map.c's formula, here), so the map and the rain share a pixel
  words    "Now", "-40 min", "+30 min"; the range in km, mi and nm
  values   rd_values_t is 1,176 B; the defaults are radar-bake.js's Scope byte for byte
           (with node; skipped without)

Run:  python3 tools/host_radar_test.py              (rc 0 = pass)
      python3 tools/host_radar_test.py --mutations  (proves the checks bite)
"""
import json, math, os, shutil, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "apps", "radar", "radar.c")
INC = os.path.join(ROOT, "apps", "radar")
JSON_C = os.path.join(ROOT, "main", "app", "orb_json.c")
JSON_INC = os.path.join(ROOT, "main", "app")
BAKE_JS = os.path.join(ROOT, "apps", "radar", "editor", "radar-bake.js")
NODE = shutil.which("node") or next((p for p in [os.path.expanduser("~/.local/opt/node-v22.11.0-linux-x64/bin/node")]
                                     if os.path.exists(p)), None)

T0 = 1794060000
LIST = {"version": "2.0", "generated": T0 + 7000, "host": "https://api.librewxr.net",
        "radar": {"past": [{"time": T0 + 600 * i, "path": "/v2/radar/%d" % (T0 + 600 * i)} for i in range(12)],
                  "nowcast": [{"time": T0 + 600 * (12 + i), "path": "/v2/radar/%d" % (T0 + 600 * (12 + i))} for i in range(6)]},
        "satellite": {"infrared": [{"time": T0, "path": "/v2/satellite/x"}]}}
ODD = json.loads(json.dumps(LIST))
ODD["radar"]["past"][5]["time"] = T0                      # out of order
GAPS = json.loads(json.dumps(LIST))
del GAPS["radar"]["past"][4]["path"]                      # an entry with no path is no frame
GAPS["radar"]["past"][7]["time"] = 0
LONG = json.loads(json.dumps(LIST))
LONG["radar"]["past"][3]["path"] = "/v2/radar/" + "9" * 40
PLACES = [(32889000, -97162000), (51507200, -127600), (-33868800, 151209300), (35676400, 139650300),
          (64146600, -21942600), (-54801900, -68303000), (1352100, 103819800), (40712800, -74006000),
          (85000000, 15000000), (-85000000, -60000000)]          # the window clamped at the world's edge


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def py_world(z, lat_e6, lon_e6):
    n = 512.0 * 2 ** z
    lat = max(-85.0511, min(85.0511, lat_e6 / 1e6))
    s = math.sin(lat * math.pi / 180.0)
    return (lon_e6 / 1e6 + 180.0) / 360.0 * n, (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n


def e4(e6):
    v = e6 + 50 if e6 >= 0 else e6 - 50
    return int(v / 100)                                    # C truncation


def py_window(z, lat_e6, lon_e6):
    """LibreWXR's window_origin (round is Python's: ties to even), then the draw window."""
    ix, iy = py_world(z, e4(lat_e6) * 100, e4(lon_e6) * 100)
    mx, my = py_world(z, lat_e6, lon_e6)
    n = 512 * 2 ** z
    ox, oy = round(ix - 256), round(iy - 256)
    oy = min(max(oy, 0), n - 512)
    return math.floor(mx - ox - 233 + 0.5), math.floor(my - oy - 233 + 0.5)


def pick_level(lat_e6, radius_m):
    """orb_map.c's pick_level."""
    c = math.cos(lat_e6 / 1e6 * math.pi / 180.0)
    want = radius_m / (466 / 2)
    v = math.log2(156543.03392 * c / want)
    zz = int(math.floor(abs(v) + 0.5)) * (1 if v >= 0 else -1)
    zz = max(6, min(11, zz))
    return zz, 156543.03392 * c / 2.0 ** zz / want


DRIVER = r"""
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "radar.h"

static int fails;
#define CK(c, label) do { if (c) printf("ok    %s\n", label); else { printf("FAIL  %s\n", label); fails++; } } while (0)

static bool read_list(const char *body, rd_list_t *l)
{
    orb_json_t j;
    orb_json_begin(&j, body, strlen(body));
    rd_list_begin(l);
    orb_json_tok_t t;
    for (int guard = 0; guard < 100000; guard++) {
        const int32_t k = orb_json_next(&j, &t);
        if (!rd_list_token(l, &t) || k == ORB_JSON_END || k == ORB_JSON_ERROR) break;
    }
    return rd_list_ok(l);
}

int main(int argc, char **argv)
{
    static rd_list_t L;
    const bool ok = read_list(LIST_BODY, &L);
    CK(ok && L.n_past == 12 && L.n_now == 6, "a whole list: twelve past frames and six nowcast");
    CK(L.past[0].t == T0 && L.past[11].t == T0 + 6600 && L.now[0].t == T0 + 7200 && L.now[5].t == T0 + 10200,
       "the frames' times, past and nowcast, in order");
    CK(!strcmp(L.past[11].path, "/v2/radar/1794066600") && !strcmp(L.now[5].path, "/v2/radar/1794070200"),
       "the frames' paths");
    char torn[8192];
    const size_t n = strlen(LIST_BODY);
    memcpy(torn, LIST_BODY, n / 3); torn[n / 3] = 0;
    CK(!read_list(torn, &L) || L.n_past < 12, "a torn list is not taken whole");
    CK(!read_list(ODD_BODY, &L), "a list out of time order is refused");
    CK(!read_list(LONG_BODY, &L), "a path too long for the record is refused, not cut");
    CK(read_list(GAPS_BODY, &L) && L.n_past == 10 && L.past[4].t == T0 + 3000,
       "an entry with no path, or no time, is skipped, and the rest read");
    CK(rd_round_even(2.5) == 2 && rd_round_even(3.5) == 4 && rd_round_even(-2.5) == -2 && rd_round_even(2.51) == 3,
       "the window's origin rounds as Python's round does: halves to even");
    CK(!read_list("[1,2,3]", &L) && !read_list("{\"radar\":{}}", &L), "an array, or a list with no frames, is no list");

    char u[200];
    CK(rd_frame_url(u, sizeof u, "https://api.librewxr.net", "/v2/radar/1794066600", 512, 6, 32889000, -97162000, false) &&
       !strcmp(u, "https://api.librewxr.net/v2/radar/1794066600/512/6/32.8890/-97.1620/255/0_0.png"),
       "a frame's URL: the source, the path, 512, the image zoom, four decimals, raw 255");
    CK(rd_frame_url(u, sizeof u, "http://nas.lan:8080", "/v2/radar/1", 256, 5, -127650, 151209350, false) &&
       !strcmp(u, "http://nas.lan:8080/v2/radar/1/256/5/-0.1277/151.2094/255/0_0.png"),
       "four decimals round half away from zero, the sign kept under one degree; the side asked");
    CK(rd_frame_url(u, sizeof u, "https://api.librewxr.net", "/v2/radar/1", 512, 9, 0, 0, true) &&
       !strcmp(u + strlen(u) - 7, "1_0.png"), "a close step asks for the server's smoothing");
    CK(rd_frame_url(u, 40, "https://api.librewxr.net", "/v2/radar/1794066600", 512, 6, 0, 0, false) == 0, "no URL that does not fit");
    CK(rd_list_url(u, sizeof u, "https://api.librewxr.net") && !strcmp(u, "https://api.librewxr.net/public/weather-maps.json"),
       "the list's URL");

    double worst = 0;
    for (double x = 1e-6; x < 1e6; x *= 1.37) {
        const double e = fabs(rd_ln(x) - log(x)) / (fabs(log(x)) + 1e-300);
        if (e > worst && fabs(log(x)) > 1e-3) worst = e;
    }
    CK(worst < 1e-14, "rd_ln is libm's log to 1e-14 over twelve decades");

    for (int i = 1; i < argc; i += 1) {                 /* "z lat lon": print the window */
        int z; long la, lo;
        if (sscanf(argv[i], "%d:%ld:%ld", &z, &la, &lo) == 3) {
            int32_t sx, sy;
            rd_window(z, (int32_t)la, (int32_t)lo, (int32_t)la, (int32_t)lo, &sx, &sy);
            printf("W %d %ld %ld %d %d\n", z, la, lo, (int)sx, (int)sy);
        }
    }
    for (int i = 1; i < argc; i += 1) {                 /* the magnified windows */
        int z; long la, lo;
        if (sscanf(argv[i], "%d:%ld:%ld", &z, &la, &lo) == 3 && z == 7)
            for (int k = 2; k <= 8; k *= 2) {
                int32_t sx, sy, dx, dy;
                rd_window_k(7, k, 256, (int32_t)la, (int32_t)lo, (int32_t)la, (int32_t)lo, &sx, &sy, &dx, &dy);
                printf("K %d %ld %ld %d %d %d %d\n", k, la, lo, (int)sx, (int)sy, (int)dx, (int)dy);
            }
    }
    CK(rd_img_z(0) == 5 && rd_img_z(2) == 7 && rd_img_z(5) == 7 && rd_img_k(2) == 1 && rd_img_k(3) == 2 && rd_img_k(5) == 8,
       "the frames stop at Local's z7; Area, City and Close magnify them 2, 4 and 8 times");
    CK(rd_img_side(2) == 512 && rd_img_zurl(2) == 7 && rd_img_side(3) == 256 && rd_img_zurl(3) == 8 && rd_img_zurl(5) == 8,
       "a magnified step asks for the 256 px centre one zoom deeper; Local and wider the whole 512");
    for (int s = 0; s < RD_ZOOMS; s++) printf("R %d %d\n", s, (int)rd_radius_m(s, 32889000));
    for (int s = 0; s < RD_ZOOMS; s++) printf("R %d %d\n", s + 10, (int)rd_radius_m(s, 64146600));

    char b[32];
    rd_time_text(b, sizeof b, 0);     const bool t0 = !strcmp(b, "Now");
    rd_time_text(b, sizeof b, -40);   const bool t1 = !strcmp(b, "-40 min");
    rd_time_text(b, sizeof b, 30);    const bool t2 = !strcmp(b, "+30 min");
    CK(t0 && t1 && t2, "the time: Now, -40 min, +30 min");
    rd_range_text(b, sizeof b, 120000, 0); const bool r0 = !strcmp(b, "120 km");
    rd_range_text(b, sizeof b, 120000, 1); const bool r1 = !strcmp(b, "75 mi");
    rd_range_text(b, sizeof b, 120000, 2); const bool r2 = !strcmp(b, "65 nm");
    rd_range_text(b, sizeof b, 479000, 1); const bool r3 = !strcmp(b, "298 mi");
    CK(r0 && r1 && r2 && r3, "the range in the orb's unit: 120 km, 75 mi, 65 nm; 479 km is 298 mi");
    int px, sc_ok = 1;
    for (int st = 0; st < RD_ZOOMS; st++)
        for (int u = 0; u < 3; u++)
            for (int la = 0; la <= 64000000; la += 32000000) {
                const int L = rd_scale(rd_radius_m(st, la), u, 150, &px);
                const int one = L == 1 || L == 2 || L == 5 || L % 10 == 0;
                int px2; const int up = rd_scale(rd_radius_m(st, la), u, 400, &px2);
                if (!one || px > 150 || px < 59 || up <= L) sc_ok = 0;   /* 1-2-5: the next is <= 2.5x */
            }
    const int L120 = rd_scale(120000, 0, 150, &px);
    CK(sc_ok && L120 == 50 && px == 97, "the scale bar: a 1-2-5 length, 60-150 px wide at every step, unit and latitude; 120 km radius: 50 km in 97 px");

    static rd_values_t v;
    rd_values_default(&v);
    CK(sizeof v == 1176 && RD_VALUES_V2_B == 1144 && offsetof(rd_values_t, scale_ink) == 1144 &&
       RD_VALUES_V3_B == 1160 && offsetof(rd_values_t, under_argb) == 1160,
       "rd_values_t is 1,176 B (layout 4: the under-rain 16 B after layout 3's scale, after layout 2's 1,144)");
    CK(rd_floor_value(0) == 84 && rd_floor_value(1) == 94 && rd_floor_value(2) == 104 && rd_floor_value(3) == 114,
       "the rain floor: 10, 15, 20 or 25 dBZ, as raw values (dBZ + 32) x 2");
    CK(rd_roles_at(&v, 2) == (0x7FFF & ~((1u << 5) | (1u << 13) | (1u << 14))) && rd_roles_at(&v, 3) == (0x7FFF & ~((1u << 13) | (1u << 14))) &&
       rd_roles_at(&v, 4) == (0x7FFF & ~(1u << 14)) && rd_roles_at(&v, 5) == 0x7FFF,
       "the roads by step: motorways and trunks to Local, primary from Area, secondary from City, tertiary at Close");
    CK(v.rain[83] == 0 && v.rain[84] != 0 && v.rain[255] == 0xFFFFFFFF, "the ramp is clear under 10 dBZ (value 84) and white at the top");
    FILE *f = fopen("defaults.bin", "wb");
    if (f) { fwrite(&v, sizeof v, 1, f); fclose(f); }
    return fails ? 1 : 0;
}
"""

JS_VALUES = r"""
global.window = undefined;
const B = (require(process.argv[2]), globalThis.RadarBake);
process.stdout.write(Buffer.from(B.valuesBytes(B.preset('scope'))).toString('hex'));
"""


def build(src_text, d):
    exe = os.path.join(d, "t")
    drv = os.path.join(d, "drv.c")
    rd = os.path.join(d, "radar.c")
    open(rd, "w").write(src_text)
    open(drv, "w").write("#define T0 %dLL\n#define LIST_BODY %s\n#define ODD_BODY %s\n#define LONG_BODY %s\n#define GAPS_BODY %s\n%s"
                         % (T0, c_str(json.dumps(LIST)), c_str(json.dumps(ODD)), c_str(json.dumps(LONG)), c_str(json.dumps(GAPS)), DRIVER))
    r = subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Werror", "-D_GNU_SOURCE", "-I", INC, "-I", JSON_INC,
                        "-o", exe, drv, rd, JSON_C, "-lm"], capture_output=True, text=True)
    return None if r.returncode else exe, r.stderr


def suite(src_text, quiet=False):
    fails = []
    def ck(ok, what):
        if not quiet: print(("ok    " if ok else "FAIL  ") + what)
        if not ok: fails.append(what)
    d = tempfile.mkdtemp(prefix="radar-")
    try:
        exe, err = build(src_text, d)
        if not exe:
            if not quiet: print(err)
            return ["compile"]
        args = ["%d:%d:%d" % (z, la, lo) for (la, lo) in PLACES for z in (5, 6, 7)]
        r = subprocess.run([exe] + args, capture_output=True, text=True, cwd=d)
        for line in r.stdout.splitlines():
            if line.startswith(("ok", "FAIL")):
                if not quiet: print(line)
                if line.startswith("FAIL"): fails.append(line)
        W = [l.split() for l in r.stdout.splitlines() if l.startswith("W ")]
        bad = [w for w in W if (int(w[4]), int(w[5])) != py_window(int(w[1]), int(w[2]), int(w[3]))]
        ck(len(W) == 30 and not bad, "the frame's window is the server's window_origin's, at 10 places x 3 zooms, two at the world's edge (%d differ)" % len(bad))
        ck(all(21 <= int(w[4]) <= 25 for w in W if abs(int(w[2])) < 80000000), "and it sits about 23 px in: the image's centre is the place's")
        K = [l.split() for l in r.stdout.splitlines() if l.startswith("K ")]
        worst = 0.0
        for kk in K:
            k, la, lo, sx, sy, dx, dy = map(int, kk[1:])
            ix, iy = py_world(7, e4(la) * 100, e4(lo) * 100)
            mx, my = py_world(7, la, lo)
            n = 512 * 2 ** 7
            ox, oy = round(ix - 128), min(max(round(iy - 128), 0), n - 256)   # the 256 px centre
            ux, uy = mx - ox, my - oy                      # the place in the image
            cx, cy = sx + (233 - dx) / k, sy + (233 - dy) / k   # the image under the glass's centre
            worst = max(worst, abs(cx - ux) * k, abs(cy - uy) * k)
            polar = abs(la) >= 80000000                    # the server clamps y at the world's edge
            if dx > 0 or dy > 0 or sx < 0 or sx + (466 - dx) / k > 255 + 1 or \
                    (not polar and (sy < 0 or sy + (466 - dy) / k > 255 + 1)):
                worst = 99
            if sx < ux - 233 / k - 2 or sy < uy - 233 / k - 2:      # no rows decoded for nothing
                worst = 98
        ck(len(K) == 30 and worst <= 0.5 + 1e-9,
           "magnified 2, 4 and 8 times, the place lands on the glass's centre within half a glass pixel (worst %.3f)" % worst)
        R = {int(l.split()[1]): int(l.split()[2]) for l in r.stdout.splitlines() if l.startswith("R ")}
        okl = all(pick_level(lat, R[k + off])[0] == 6 + k and abs(pick_level(lat, R[k + off])[1] - 1) < 1e-4
                  for off, lat in ((0, 32889000), (10, 64146600)) for k in range(6))
        ck(okl, "each step's radius picks its own level (z6 .. z11) at scale 1 (the radius is whole metres), at 33 and 64 degrees north")
        ck(abs(R[0] - 479000) < 1500 and abs(R[2] - 120000) < 500 and abs(R[5] - 15000) < 100,
           "Wide is ~479 km, Local ~120 km and Close ~15 km at DFW (§2.4's table)")
        if NODE:
            jp = os.path.join(d, "v.js"); open(jp, "w").write(JS_VALUES)
            j = subprocess.run([NODE, jp, BAKE_JS], capture_output=True, text=True)
            c = open(os.path.join(d, "defaults.bin"), "rb").read() if os.path.exists(os.path.join(d, "defaults.bin")) else b""
            ck(j.returncode == 0 and bytes.fromhex(j.stdout) == c,
               "the C defaults are radar-bake.js's Scope values byte for byte")
        elif not quiet:
            print("skip  the Scope parity: no node here")
    finally:
        shutil.rmtree(d, ignore_errors=True)
    return fails


MUTATIONS = [
    ("if (t->depth == 3 && l->in && l->cur.t > 0 && l->cur.path[0]) {", "if (t->depth == 3 && l->in) {"),
    ("for (int i = 1; i < l->n_past; i++) if (l->past[i].t <= l->past[i - 1].t) return false;", ""),
    ("if (t->str_trunc || strlen(t->str) >= RD_PATH_MAX || t->str[0] != '/') { l->error = true; return false; }", ""),
    ("const int32_t v = e6 >= 0 ? e6 + 50 : e6 - 50;", "const int32_t v = e6 + 50;"),
    ("    for (int k = 1; k < 60; k += 2) {", "    for (int k = 1; k < 8; k += 2) {"),
    ("return fmod(f, 2.0) == 0 ? f : f + 1;", "return f + 1;"),
    ("    if (oy < 0) oy = 0;\n", ""),
    ("*sx = (int32_t)floor(mx - ox - RD_GLASS / 2.0 + 0.5);", "*sx = (int32_t)floor(mx - ox - RD_GLASS / 2.0);"),
    # (the image's centre taken at the place's own e6 rather than the URL's four decimals is
    # no mutant at these zooms: the two differ by under 0.01 px)
    ("pow2(rd_map_z(step)) * (RD_GLASS / 2)", "pow2(rd_map_z(step)) * (RD_GLASS / 2 + 1)"),
    ("unit == 1 ? km / 1.609344", "unit == 1 ? km / 1.6"),
    ("        if (i < 84) { v->rain[i] = 0; continue; }", "        if (i < 80) { v->rain[i] = 0; continue; }"),
    ("if (v->step_from[r] > step)", "if (v->step_from[r] >= step)"),
    ("255/%d_0.png", "255/0_0.png"),
    ("*dx = (int32_t)floor(k * (*sx - ux) + RD_GLASS / 2.0 + 0.5);", "*dx = (int32_t)floor(k * (*sx - ux) + RD_GLASS / 2.0);"),
    ("*sy = (int32_t)floor(uy - (RD_GLASS / 2.0) / k) - 1;", "*sy = (int32_t)floor(uy - (RD_GLASS / 2.0)) - 1;"),
]


def main():
    src = open(SRC).read()
    fails = suite(src)
    ok = not fails
    if "--mutations" in sys.argv:
        caught = 0
        for old, new in MUTATIONS:
            if old not in src:
                print("mutation does not apply: %r" % old[:60]); ok = False; continue
            hit = bool(suite(src.replace(old, new, 1), quiet=True))
            caught += hit
            print("%s  %s" % ("caught" if hit else "MISSED", (new or "(deleted)")[:70].replace("\n", " ")))
        print("%d/%d mutants caught" % (caught, len(MUTATIONS)))
        ok = ok and caught == len(MUTATIONS)
    print("all radar checks pass" if ok else "radar checks FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
