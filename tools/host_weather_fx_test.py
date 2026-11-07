#!/usr/bin/env python3
"""Host bench for the weather's particles (apps/weather/fx.c, weather-plan §2.7).

fx.c is pure, so it compiles here as it stands with host gcc -Werror, against wx.h's
condition numbers. The harness sets a pool for each condition and runs it for 20 s of
frames at the rates the orb draws (30 fps, with a 300 ms stall in the middle), checking:

- the count each condition draws, the rows a theme offers capping it, a clear day none;
- every particle is drawn inside the glass (or just off its edge where it enters), and
  the pool keeps its count: nothing falls off the world for good;
- rain and drizzle fall, faster than snow; snow sways; mist and wisps cross sideways;
- stars stand still and twinkle within their bounds;
- a stall moves nothing further than a tenth of a second's travel;
- the same kind again keeps the particles where they are; a new kind re-seeds;
- the editor's preview (apps/weather/editor/fx.js) puts every particle where fx.c does:
  both run every condition from one seed for 20 s and print each frame's places, to the
  bit. That half runs in node; the host-suite CI job has none, so there it is skipped and
  said so, and `make check-editor` runs this suite whole.

Run:  python3 tools/host_weather_fx_test.py              (rc 0 = pass)
      python3 tools/host_weather_fx_test.py --mutations  (proves the checks bite)
"""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP = os.path.join(ROOT, "apps", "weather")
FX_C = os.path.join(APP, "fx.c")
JSON_INC = os.path.join(ROOT, "main", "app")
FX_JS = os.path.join(APP, "editor", "fx.js")
NODE = shutil.which("node") or next((p for p in [os.path.expanduser(
    "~/.local/share/nodeenv-gorb/bin/node")] if os.path.exists(p)), None)

MUTATIONS = [
    ("if (dt > 0.1f) dt = 0.1f;", "if (dt > 1.0f) dt = 1.0f;"),
    ("if (gone) spawn(fx, p, false);", "(void)gone;"),
    ("case WX_RAIN:    return 8;", "case WX_RAIN:    return 0;"),
    ("    if (n > rows) n = rows;", "    (void)rows;"),
    ("if (kind == fx->kind && n == fx->n) return;", ";"),
    ("return 10.0f * sin_turns(fx->p[i].ph);", "return 0;"),
    ("return (int)(60 + (p->opa - 60) * s);", "return p->opa;"),
    ("default:         return 0;                                       /* a clear day */",
     "default:         return 3;"),
    # only the editor's parity sees these: fx.c drifting from fx.js by a hair
    ("p->opa = light ? 170 : 210;", "p->opa = light ? 171 : 210;"),
    ("if (fx->kind != WX_THUNDER) return;", "if (fx->kind == WX_CLEAR) return;"),
    ("return t < 0.07f ? 255 : t < 0.12f ? 0 :", "return t < 0.07f ? 255 : t < 0.12f ? 255 :"),
    ("b->x = FX_R + rndf(fx, -90, 90);", "b->x = FX_R + rndf(fx, -190, 190);"),
    ("b->wait = rndf(fx, 2.5f, 7.0f);", "b->wait = rndf(fx, 2.5f, 7.1f);"),
    ("return lo + (hi - lo) * (float)(rnd(fx) & 0xFFFFu) / 65535.0f;",
     "return lo + (hi - lo) * ((float)(rnd(fx) & 0xFFFFu) / 65535.0f);"),
]

HARNESS = r"""
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fx.h"
#include "wx.h"
static int fails;
#define CK(c, ...) do { if (c) { printf("ok    "); printf(__VA_ARGS__); printf("\n"); } \
                        else { printf("FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct { float minvy, maxvy, sumvy, sumvx, maxsway, mintw, maxtw, maxjump, maxout; int keep; } stats_t;

static stats_t run(int kind, int rows)
{
    fx_t fx; memset(&fx, 0, sizeof fx); fx.kind = -1; fx.seed = 12345;
    fx_set(&fx, kind, rows);
    const int n0 = fx.n;
    stats_t s = { 1e9f, -1e9f, 0, 0, 0, 1e9f, -1e9f, 0, 0, 1 };
    float px[FX_MAX], py[FX_MAX];
    for (int i = 0; i < fx.n; i++) { px[i] = fx.p[i].x; py[i] = fx.p[i].y; }
    for (int f = 0; f < 600; f++) {
        const float dt = f == 300 ? 0.3f : 1.0f / 30.0f;       /* one stalled frame */
        fx_step(&fx, dt);
        if (fx.n != n0) s.keep = 0;
        for (int i = 0; i < fx.n; i++) {
            const fx_part_t *p = &fx.p[i];
            const float dx = p->x - FX_R, dy = p->y - FX_R, r = sqrtf(dx * dx + dy * dy) - FX_R;
            if (r > s.maxout) s.maxout = r;
            const float jx = p->x - px[i], jy = p->y - py[i];
            const float jump = sqrtf(jx * jx + jy * jy);
            /* a respawn jumps by design; travel is what a stall must not stretch */
            const bool respawned = jump > 150;
            if (!respawned && jump > s.maxjump) s.maxjump = jump;
            px[i] = p->x; py[i] = p->y;
            if (p->vy < s.minvy) s.minvy = p->vy;
            if (p->vy > s.maxvy) s.maxvy = p->vy;
            s.sumvy += p->vy; s.sumvx += p->vx;
            const float sw = fabsf(fx_sway(&fx, i));
            if (sw > s.maxsway) s.maxsway = sw;
            const int o = fx_opa(&fx, i);
            if (o < s.mintw) s.mintw = (float)o;
            if (o > s.maxtw) s.maxtw = (float)o;
        }
    }
    return s;
}

int main(void)
{
    fx_t fx; memset(&fx, 0, sizeof fx); fx.kind = -1;
    fx_set(&fx, WX_CLEAR, FX_MAX);
    CK(fx.n == 0 && !fx_live(&fx), "a clear day draws nothing");
    fx_set(&fx, WX_RAIN, FX_MAX);   CK(fx.n == 8, "rain draws 8 (%d)", fx.n);
    fx_set(&fx, WX_THUNDER, FX_MAX); CK(fx.n == 10, "thunder pours 10 (%d)", fx.n);
    fx_set(&fx, WX_DRIZZLE, FX_MAX); CK(fx.n == 6, "drizzle draws 6 (%d)", fx.n);
    fx_set(&fx, WX_SNOW, FX_MAX);   CK(fx.n == 10, "snow draws 10 (%d)", fx.n);
    fx_set(&fx, WX_FOG, FX_MAX);    CK(fx.n == 6, "fog draws 6 mist (%d)", fx.n);
    fx_set(&fx, WX_CLOUDY, FX_MAX); CK(fx.n == 5, "cloud draws 5 wisps (%d)", fx.n);
    fx_set(&fx, WX_CLEAR_N, FX_MAX); CK(fx.n == 8, "a clear night has 8 stars (%d)", fx.n);
    fx_set(&fx, WX_RAIN, 4);        CK(fx.n == 4, "a theme of 4 rows caps rain at 4 (%d)", fx.n);
    fx_set(&fx, WX_RAIN, 0);        CK(fx.n == 0, "a theme with no pool draws nothing");

    fx_set(&fx, WX_SNOW, FX_MAX);
    const float x0 = fx.p[3].x; fx_step(&fx, 0.05f); const float x1 = fx.p[3].y;
    fx_set(&fx, WX_SNOW, FX_MAX);
    CK(fx.p[3].y == x1, "the same kind again keeps the flakes where they are");
    fx_set(&fx, WX_RAIN, FX_MAX);
    CK(fx.p[3].f == FX_F_DROP, "a new kind re-seeds the pool (a flake is now a drop)");
    (void)x0;

    stats_t r = run(WX_RAIN, FX_MAX), d = run(WX_DRIZZLE, FX_MAX), s = run(WX_SNOW, FX_MAX);
    stats_t f = run(WX_FOG, FX_MAX), c = run(WX_CLOUDY, FX_MAX), n = run(WX_CLEAR_N, FX_MAX);
    CK(r.keep && d.keep && s.keep && f.keep && c.keep && n.keep, "every pool keeps its count for 20 s");
    CK(r.minvy > 200 && d.minvy > 120 && d.maxvy < r.minvy + 60, "rain falls fast, drizzle lighter (%.0f, %.0f px/s)", r.minvy, d.minvy);
    CK(s.maxvy < 70 && s.minvy > 20, "snow drifts down slowly (%.0f..%.0f px/s)", s.minvy, s.maxvy);
    CK(s.maxsway > 8 && s.maxsway <= 10.01f && r.maxsway == 0, "snow sways (%.1f px), rain does not", s.maxsway);
    CK(f.sumvx > 0 && f.sumvy == 0 && c.sumvx > 0 && c.sumvy == 0, "mist and wisps cross sideways");
    CK(n.sumvx == 0 && n.sumvy == 0 && n.mintw >= 60 && n.maxtw <= 200 && n.maxtw - n.mintw > 80,
       "stars stand still and twinkle %.0f..%.0f", n.mintw, n.maxtw);
    CK(r.maxout < 40 && s.maxout < 40 && f.maxout < 60 && c.maxout < 60 && n.maxout <= 0,
       "drawn inside the glass, or just past its edge where they enter (%.0f %.0f %.0f %.0f px)",
       r.maxout, s.maxout, f.maxout, c.maxout);
    CK(r.maxjump < 36.5f, "a 300 ms stall moves rain no further than 100 ms of travel (%.1f px)", r.maxjump);
    /* thunder strikes: a bolt every few seconds, in a strike's rhythm, over the icon's sky */
    {
        int strikes = 0, lit = 0, dark_mid = 0, rain_lit = 0, prev = 0, far = 0;
        fx_t t; memset(&t, 0, sizeof t); t.kind = -1; t.seed = 99;
        fx_set(&t, WX_THUNDER, FX_MAX);
        for (int f = 0; f < 1200; f++) {                       /* 40 s at 30 fps */
            fx_step(&t, 1.0f / 30.0f);
            float bx, by; const int o = fx_bolt(&t, &bx, &by);
            if (o && !prev && t.bolt.t < 0.05f) strikes++;
            if (o == 255) lit++;
            if (!o && t.bolt.t >= 0.07f && t.bolt.t < 0.12f) dark_mid++;
            if (o && (bx < FX_R - 91 || bx > FX_R + 91 || by < 109 || by > 171)) far++;
            prev = o;
        }
        fx_t r2; memset(&r2, 0, sizeof r2); r2.kind = -1; r2.seed = 99;
        fx_set(&r2, WX_RAIN, FX_MAX);
        for (int f = 0; f < 1200; f++) { fx_step(&r2, 1.0f / 30.0f); float bx, by; rain_lit += fx_bolt(&r2, &bx, &by) > 0; }
        CK(strikes >= 4 && strikes <= 16, "thunder strikes every few seconds (%d in 40 s)", strikes);
        CK(lit > 0 && dark_mid > 0, "a strike flashes, goes dark, and flashes again");
        CK(!far, "every bolt stands over the icon's sky");
        CK(!rain_lit, "rain never strikes");
    }
    printf(fails ? "RESULT: %d failed\n" : "RESULT: pass\n", fails);
    return fails ? 1 : 0;
}
"""


# The parity trace: every condition from one seed, 600 frames with the stall, each frame's
# places and brightness printed as the float's bits (%a in C, the same hex in JS).
TRACE_C = r"""
#include <stdio.h>
#include <string.h>
#include "fx.h"
int main(void)
{
    for (int kind = 0; kind < 10; kind++) {
        fx_t fx; memset(&fx, 0, sizeof fx); fx.kind = -1; fx.seed = 777u + (unsigned)kind;
        fx_set(&fx, kind, FX_MAX);
        for (int f = 0; f <= 600; f++) {
            if (f) fx_step(&fx, f == 300 ? 0.3f : 1.0f / 30.0f);
            for (int i = 0; i < fx.n; i++) {
                float sw = fx_sway(&fx, i);
                unsigned bx, by, bs;
                memcpy(&bx, &fx.p[i].x, 4); memcpy(&by, &fx.p[i].y, 4); memcpy(&bs, &sw, 4);
                printf("%d %d %d %08x %08x %08x %d %d\n", kind, f, i, bx, by, bs, fx_opa(&fx, i), fx.p[i].f);
            }
            float qx, qy; const int qo = fx_bolt(&fx, &qx, &qy);
            unsigned ux, uy; memcpy(&ux, &qx, 4); memcpy(&uy, &qy, 4);
            if (qo) printf("B %d %d %d %08x %08x\n", kind, f, qo, ux, uy);
        }
    }
    return 0;
}
"""
TRACE_JS = r"""
const X = require(process.argv[1]);
const b = new DataView(new ArrayBuffer(4));
const hex = v => { b.setFloat32(0, v); return b.getUint32(0).toString(16).padStart(8, '0'); };
const out = [];
for (let kind = 0; kind < 10; kind++) {
  const fx = X.create(777 + kind);
  X.set(fx, kind, X.FX_MAX);
  for (let f = 0; f <= 600; f++) {
    if (f) X.step(fx, f === 300 ? 0.3 : 1 / 30);
    for (let i = 0; i < fx.n; i++)
      out.push([kind, f, i, hex(fx.p[i].x), hex(fx.p[i].y), hex(X.sway(fx, i)), X.opa(fx, i), fx.p[i].f].join(' '));
    const q = X.bolt(fx);
    if (q.opa) out.push(['B', kind, f, q.opa, hex(q.x), hex(q.y)].join(' '));
  }
}
process.stdout.write(out.join('\n') + '\n');
"""


def parity(src=None, quiet=False):
    """0 when fx.js traces fx.c to the bit, 1 when not, None with no node."""
    if not NODE:
        return None
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "fx.c"), "w").write(src or open(FX_C).read())
        open(os.path.join(d, "t.c"), "w").write(TRACE_C)
        exe = os.path.join(d, "t")
        r = subprocess.run(["gcc", "-std=c11", "-O1", "-ffp-contract=off", "-Wall", "-Werror", "-I", APP, "-I", JSON_INC,
                            "-o", exe, os.path.join(d, "fx.c"), os.path.join(d, "t.c")],
                           capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("trace compile failed:\n" + r.stderr)
        c = subprocess.run([exe], capture_output=True, text=True).stdout.splitlines()
    j = subprocess.run([NODE, "-e", TRACE_JS, FX_JS], capture_output=True, text=True)
    if j.returncode:
        raise SystemExit("fx.js threw:\n" + j.stderr)
    j = j.stdout.splitlines()
    bad = next((k for k, (a, b) in enumerate(zip(c, j)) if a != b), None)
    if bad is None and len(c) != len(j):
        bad = min(len(c), len(j))
    if not quiet:
        if bad is None:
            print("ok    the editor's fx.js puts every particle where fx.c does (%d places, 10 conditions, 20 s)" % len(c))
        else:
            print("FAIL  fx.js parts from fx.c at line %d: C %r, JS %r" % (bad, c[bad] if bad < len(c) else None,
                                                                         j[bad] if bad < len(j) else None))
    return 0 if bad is None else 1


def run(mutation=None, quiet=False):
    src = open(FX_C).read()
    if mutation:
        if mutation[0] not in src:
            raise SystemExit("mutation %r does not apply -- rewrite the bench" % mutation[0])
        src = src.replace(mutation[0], mutation[1], 1)
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "fx.c"), "w").write(src)
        open(os.path.join(d, "h.c"), "w").write(HARNESS)
        exe = os.path.join(d, "h")
        r = subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE",
                            "-I", APP, "-I", JSON_INC, "-o", exe, os.path.join(d, "fx.c"),
                            os.path.join(d, "h.c"), "-lm"], capture_output=True, text=True)
        if r.returncode:
            if quiet:
                return 2
            raise SystemExit("compile failed:\n" + r.stderr)
        p = subprocess.run([exe], capture_output=True, text=True)
        if not quiet:
            print(p.stdout, end="")
    if mutation:
        return p.returncode or (parity(src, quiet=True) or 0)
    q = parity()
    if q is None:
        print("skip  the editor's half: no node here (make check-editor runs it)")
    return p.returncode or (q or 0)


def main():
    if "--mutations" in sys.argv:
        caught = 0
        for m in MUTATIONS:
            bit = run(m, quiet=True) != 0
            print(("bites " if bit else "MISSED") + "  " + m[0][:64])
            caught += bit
        print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
        return 0 if caught == len(MUTATIONS) else 1
    return run()


if __name__ == "__main__":
    sys.exit(main())
