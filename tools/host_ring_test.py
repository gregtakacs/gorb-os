#!/usr/bin/env python3
"""host_ring_test.py — the OS ring's pure core, compiled on host gcc (plan §64.1, ring-design §4..§6).

main/display/orb_ring.c is the ring's brain: the rim annulus's geometry, the blend over a
wire-order RGB565 tile pixel, the effect clock, the OS clamps and the one-record-per-app
ownership table. None of it is visible on a dial before ring-2 wires the stamp, and all of
it can be wrong in ways a build cannot see — a forgotten byte swap is a red/blue ring, an
unclamped breathe is a band of rows re-sent ten times a second over a live app, and a table
that forgets `owner` shows a cleared app. So the shipped file is compiled VERBATIM here
beside a harness, and its spans are checked against a per-pixel reference written the copy
arc's own way (atan2f per pixel, as orb_edge_arc.c does it): the pieces must paint the same
pixels, for every row and a sweep of fractions (ring-design's orchestrator amendment 1).

Run:  python3 tools/host_ring_test.py              (rc 0 = pass)
      python3 tools/host_ring_test.py --mutations  (proves the checks bite; rc 0 = all bite)
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DISP = os.path.join(ROOT, "main", "display")
FRACS = [0, 1, 125, 250, 499, 500, 501, 750, 999, 1000] + [(i * 37 + 11) % 1001 for i in range(50)]

MUTATIONS = [
    ("    return wire(packed);", "    return packed;"),
    ("const uint16_t bg = wire(bg565_swapped);", "const uint16_t bg = bg565_swapped;"),
    ("    if (a == 0) return bg565_swapped;", "    if (a == 0) return wire(bg565_swapped);"),
    ("if (y1) *y1 = CC + R_OUT_I;", "if (y1) *y1 = CC + R_IN_I;"),
    ("spans[0][1] = hl - 1;", "spans[0][1] = hl;"),
    ("spans[1][0] = hr + 1;", "spans[1][0] = hr;"),
    ("if (p_hit(y, CC - 1, 1)) {", "if (0) {"),
    ("r1[0] = flat ? GLASS - 1 : (c > 0.0 ? clip_x(fl(b)) : GLASS - 1);",
     "r1[0] = flat ? GLASS - 1 : (c > 0.0 ? GLASS - 1 : clip_x(fl(b)));"),
    ("else if (c < 0.0) { t0 = 0; t1 = lhalf(fl(b)); }",
     "else if (c < 0.0) { t0 = lhalf(fl(b)) + 1; t1 = CC - 1; }"),
    ("static int lhalf(int e) { return e > CC - 1 ? CC - 1 : e; }", "static int lhalf(int e) { return e; }"),
    ("if (p_hit(y, m, inner)) b = m; else a = m + 1;", "if (p_hit(y, m, inner)) a = m + 1; else b = m;"),
    ("if (r->fraction > 1000) r->fraction = 1000;", "if (0) r->fraction = 1000;"),
    ("if (r->effect != ORB_RING_FLASH && r->effect != ORB_RING_BREATHE) r->effect = ORB_RING_STEADY;", ";"),
    ("? ORB_RING_BREATHE_MIN_MS", "? 0u"),
    ("? ORB_RING_FLASH_MIN_MS : 0u;", "? 0u : 0u;"),
    ("return ((now_ms / (p / 2 > 0 ? p / 2 : 1)) & 1) ? 0 : 255;", "return ((now_ms / (p / 2 > 0 ? p / 2 : 1)) & 0) ? 0 : 255;"),
    ("return ((now_ms / (p / 2 > 0 ? p / 2 : 1)) & 1) ? 0 : 255;", "return ((now_ms / (p > 0 ? p : 1)) & 1) ? 0 : 255;"),
    ("(1.0f - cosf((float)TAU * ph))", "(0.5f - cosf((float)TAU * ph))"),
    ("(seq[i] == seq[best] && i > best)", "(seq[i] == seq[best] && i < best)"),
    ("        if (owner[i] < 0) continue;", "        if (0) continue;"),
    ("    owner[slot] = -1;", "    ;"),
    ("seq[slot] = now_us / 1000;", "seq[slot] = now_us;"),
]

HARNESS = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_ring.h"

#define GLASS 466

static int ref_pix(int y, int x, int frac)      /* -1 outside the annulus, else the arc flag */
{
    const float dy = (float)y + 0.5f - 233.0f, dx = (float)x + 0.5f - 233.0f, d2 = dx * dx + dy * dy;
    if (d2 < 224.0f * 224.0f || d2 > 231.0f * 231.0f) return -1;
    float a = atan2f(dx, -dy);
    if (a < 0.0f) a += 6.28318531f;
    return a <= (float)frac * 6.28318531f / 1000.0f ? 1 : 0;
}
static int at_boundary(int y, int x, int frac)  /* a reference arc/track edge within one pixel */
{
    const int r = ref_pix(y, x, frac);
    for (int d = -1; d <= 1; d += 2) {
        const int q = ref_pix(y, x + d, frac);
        if (q >= 0 && q != r) return 1;
    }
    return 0;
}
static void sweep(int frac)
{
    int bad_ann = 0, arc_rows = 0, arc_max = 0, off_bound = 0, dup = 0, outside = 0, pmax = 0, nref = 0, ngot = 0;
    unsigned char got[GLASS];
    orb_ring_part_t p[8];
    for (int y = 0; y < GLASS; y++) {
        const int np = ring_row_parts(y, frac, p, 8);
        if (np > pmax) pmax = np;
        memset(got, 0xFF, sizeof got);
        for (int i = 0; i < np; i++)
            for (int x = p[i].x0; x <= p[i].x1; x++) {
                if (x < 0 || x >= GLASS || p[i].x0 < 0 || p[i].x1 >= GLASS) { outside++; continue; }
                if (got[x] != 0xFF) dup++;
                got[x] = p[i].arc; ngot++;
            }
        int rowdiff = 0;
        for (int x = 0; x < GLASS; x++) {
            const int r = ref_pix(y, x, frac);
            if (r >= 0) nref++;
            if ((r < 0) != (got[x] == 0xFF)) { bad_ann++; continue; }
            if (r < 0 || got[x] == (unsigned char)r) continue;
            rowdiff++;
            if (!at_boundary(y, x, frac)) off_bound++;
        }
        if (rowdiff) { arc_rows++; if (rowdiff > arc_max) arc_max = rowdiff; }
    }
    printf("FR %d bad_ann %d arc_rows %d arc_max %d off_bound %d dup %d outside %d parts_max %d px %d/%d\n",
           frac, bad_ann, arc_rows, arc_max, off_bound, dup, outside, pmax, ngot, nref);
}

static long lg(const char *s) { return strtol(s, NULL, 0); }
static orb_ring_rec tbl[16];
static int ownr[16];
static int64_t seqq[16];
int main(int argc, char **argv)
{
    const char *m = argc > 1 ? argv[1] : "";
    if (!strcmp(m, "size"))
        printf("SIZE %u %u\n", (unsigned)sizeof(orb_ring_rec), (unsigned)sizeof(orb_ring_part_t));
    else if (!strcmp(m, "blend"))
        printf("BLEND %04X\n", ring_blend565((uint16_t)lg(argv[2]), (uint32_t)strtol(argv[3], NULL, 0)));
    else if (!strcmp(m, "rows")) {
        int y0 = -9, y1 = -9; ring_rows_for((int)lg(argv[2]), &y0, &y1); printf("ROWS %d %d\n", y0, y1);
    } else if (!strcmp(m, "clamp")) {
        orb_ring_rec r = { (uint16_t)lg(argv[2]), (uint8_t)lg(argv[3]), 0, 0, 0, (uint16_t)lg(argv[4]), 0 };
        ring_clamp(&r);
        printf("REC %u %u %u\n", r.fraction, r.effect, r.period_ms);
    } else if (!strcmp(m, "eff")) {
        orb_ring_rec r = { 500, (uint8_t)lg(argv[2]), 0, 0, 0, (uint16_t)lg(argv[3]), 0 };
        printf("EFF %d\n", ring_effect_alpha(&r, strtoll(argv[4], NULL, 0)));
    } else if (!strcmp(m, "null")) {
        orb_ring_rec g;
        orb_ring_part_t p[4];
        ring_clamp(NULL);
        ring_init(NULL, NULL, 0);
        ring_rows_for(500, NULL, NULL);
        ring_set(NULL, NULL, NULL, 8, 0, NULL, 1000);
        ring_set(tbl, ownr, seqq, 8, 9, &g, 1000);
        ring_clear(NULL, NULL, 8, 2);
        ring_clear(tbl, ownr, 8, -2);
        printf("NULL %d %d %d %d\n", ring_pick(NULL, NULL, NULL, 8, &g), ring_effect_alpha(NULL, 5),
               ring_row_parts(233, 500, NULL, 4), ring_row_parts(233, 500, p, 0));
    } else if (!strcmp(m, "one")) {
        orb_ring_part_t p[8];
        const int n = ring_row_parts((int)lg(argv[2]), (int)lg(argv[3]), p, argc > 4 ? (int)lg(argv[4]) : 8);
        printf("ONE %d %d n %d", (int)lg(argv[2]), (int)lg(argv[3]), n);
        for (int i = 0; i < n; i++) printf(" [%d,%d]%d", p[i].x0, p[i].x1, p[i].arc);
        printf("\n");
    } else if (!strcmp(m, "sweep")) {
        for (int i = 2; i < argc; i++) sweep((int)lg(argv[i]));
    } else if (!strcmp(m, "own")) {
        const int n = (int)lg(argv[2]);
        for (int i = 3; i < argc; i++) {
            const char *o = argv[i];
            int a, b, c, d; long long e;
            if (o[0] == 'I') ring_init(tbl, ownr, n);
            else if (sscanf(o, "S %d %d %d %d %lld", &a, &b, &c, &d, &e) == 5) {
                orb_ring_rec r = { (uint16_t)b, (uint8_t)c, 0, 0, 0, (uint16_t)d, 0 };
                ring_set(tbl, ownr, seqq, n, a, &r, (int64_t)e);
            } else if (sscanf(o, "C %d", &a) == 1) ring_clear(tbl, ownr, n, a);
            else if (o[0] == 'P' || o[0] == 'A') {
                orb_ring_rec g;
                const int ok = ring_pick(tbl, ownr, seqq, n, &g);
                if (o[0] == 'P') printf("PICK %d %u %u\n", (unsigned)ok, (unsigned)(ok ? g.fraction : 0), (unsigned)(ok ? g.effect : 0));
                else printf("ALPHA %d %d\n", ok, ok ? ring_effect_alpha(&g, (int64_t)(o[1] ? atoi(o + 1) : 0)) : -1);
            }
        }
    } else return 2;
    return 0;
}
"""


def build(src, d):
    open(os.path.join(d, "orb_ring.c"), "w").write(src)
    open(os.path.join(d, "h.c"), "w").write(HARNESS)
    exe = os.path.join(d, "h")
    r = subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", DISP, "-o", exe,
                        os.path.join(d, "orb_ring.c"), os.path.join(d, "h.c"), "-lm"],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("compile failed:\n" + r.stderr)
    return exe


def run(exe, *args):
    return subprocess.run([exe] + [str(a) for a in args], capture_output=True, text=True).stdout


def wire(c):
    return ((c >> 8) | ((c & 0xFF) << 8)) & 0xFFFF


def exp_blend(bg_sw, argb):
    """The shipped blend, mirrored: wire in, unpack 5/6/5, lerp, repack, wire out."""
    a = argb >> 24
    if a == 0:
        return bg_sw
    bg = wire(bg_sw)

    def trunc(v):
        return int(v) if v >= 0 else -int(-v)

    r5, g6, b5 = bg >> 11, (bg >> 5) & 0x3F, bg & 0x1F
    r = (r5 << 3) | (r5 >> 2)
    g = (g6 << 2) | (g6 >> 4)
    b = (b5 << 3) | (b5 >> 2)
    ch = []
    for base, s in ((r, (argb >> 16) & 0xFF), (g, (argb >> 8) & 0xFF), (b, argb & 0xFF)):
        ch.append(base + trunc((s - base) * a / 255))
    packed = (ch[0] * 31 + 127) // 255 << 11 | ((ch[1] * 63 + 127) // 255) << 5 | (ch[2] * 31 + 127) // 255
    return wire(packed)


def checks(exe, quiet=False):
    fails = 0

    def ck(c, what):
        nonlocal fails
        if not quiet:
            print(("ok    " if c else "FAIL  ") + what)
        fails += not c

    sz = run(exe, "size").split()
    ck(sz[:2] == ["SIZE", "16"], "the record is the 16 bytes the SDK struct is: %s" % sz)

    def blend(bg, argb):
        out = run(exe, "blend", hex(bg), hex(argb)).split()
        return int(out[1], 16) if len(out) > 1 else -1

    pas = all(blend(bg, a) == bg for bg in (0x0000, 0xFFFF, 0xDF66, 0x8631, 0x1234) for a in (0x00FF0000, 0x00123456))
    ck(pas, "alpha 0 answers the tile pixel back, byte for byte")
    pure = (blend(0x0000, 0xFFFF0000) == wire(0xF800) and blend(0x0000, 0xFF00FF00) == wire(0x07E0)
            and blend(0x0000, 0xFF0000FF) == wire(0x001F) and blend(0x0000, 0xFFFFFFFF) == 0xFFFF
            and blend(wire(0x0A15), 0xFF00FF00) == wire(0x07E0))
    ck(pure, "alpha 255 is the source colour, on the wire: %04X %04X" % (blend(0x0000, 0xFFFF0000), blend(0x0000, 0xFF00FF00)))
    grid = [(bg, argb) for bg in (0x0000, 0xFFFF, 0xDF66, 0x8631, 0x1234, 0x7BEF, 0x0A15, 0xF800)
            for argb in (0x8066DF00, 0x190FA36B, 0xFFD62828, 0x01000000, 0xFEEDEDF3, 0x660FBFB1, 0xB42D5BD0, 0xFF000000)]
    ck(all(blend(bg, a) == exp_blend(bg, a) for bg, a in grid), "the blend is the lerp over the wire-order pixel")
    def red(v):
        n = wire(v)
        return ((n >> 11) << 3) | ((n >> 11) >> 2)
    ck(red(blend(0x0000, 0x00808080)) == 0 and red(blend(0x0000, 0x20808080)) < red(blend(0x0000, 0x80808080))
       and red(blend(0x0000, 0x80808080)) < red(blend(0x0000, 0xFF808080)), "more alpha is closer to the source")

    def rows(f):
        return run(exe, "rows", f).split()[1:]
    ck(rows(0) == ["2", "464"] and rows(500) == ["2", "464"] and rows(1000) == ["2", "464"]
       and rows(-5) == ["2", "464"] and rows(1500) == ["2", "464"],
       "the rows the ring may touch are the whole annulus, whatever the fraction")

    frs = [ln.split() for ln in run(exe, "sweep", *FRACS).splitlines()]
    ck(len(frs) == len(FRACS), "the sweep answers for every fraction asked")

    def good(f):
        g = dict(zip(f[2::2], f[3::2]))
        n = [int(v) for v in g["px"].split("/")]
        return (int(g["bad_ann"]) == 0 and int(g["dup"]) == 0 and int(g["outside"]) == 0
                and 1 <= int(g["parts_max"]) <= 4 and n[0] == n[1] > 0
                and (int(g["arc_rows"]) == 0 or (int(g["arc_max"]) <= 1 and int(g["off_bound"]) == 0)))

    bad = [f for f in frs if not good(f)]
    ck(not bad, "the spans paint the pixels the per-pixel reference paints, every row of %d fractions%s"
       % (len(FRACS), (": " + " ".join(bad[0])) if bad else ""))

    def parts(y, f, cap=8):
        w = run(exe, "one", y, f, cap).split()
        return (int(w[4]), w[5:]) if len(w) > 4 else (-1, [])

    n0, pc0 = parts(233, 0)
    ck(n0 > 0 and all(p[-1] == "0" for p in pc0), "fraction 0 leaves every annulus pixel the track: %s" % pc0)
    n1, pc1 = parts(233, 1000)
    ck(n1 > 0 and all(p[-1] == "1" for p in pc1), "fraction 1000 makes the whole annulus the arc: %s" % pc1)
    n2, pc2 = parts(72, 125)
    ck(any(p[-1] == "1" for p in pc2) and any(p[-1] == "0" for p in pc2),
       "a row the ray crosses comes back arc and track: %s" % pc2)
    ck(parts(0, 500)[0] == 0 and parts(465, 500)[0] == 0 and parts(2, 500)[0] > 0,
       "the rows outside the band answer no pieces, the first row inside answers some")
    ck(parts(233, 1500)[1] == parts(233, 1000)[1] and parts(233, -5)[1] == parts(233, 0)[1],
       "the geometry takes the same fraction clamps the record does")
    ck(parts(72, 125, 1) == (1, ["[67,76]0"]), "a caller with room for one piece gets one, and no more")
    nl = run(exe, "null").split()
    ck(nl[1:5] == ["0", "0", "0", "0"], "a void argument answers nothing rather than crashing: %s" % nl)

    def rec(f, e, p):
        return run(exe, "clamp", f, e, p).split()[1:]
    ck(rec(1500, 0, 4000) == ["1000", "0", "4000"], "a fraction over the circle clamps to 1000")
    ck(rec(7, 7, 4000) == ["7", "0", "4000"], "an unknown effect becomes steady and still shows")
    ck(rec(500, 2, 100) == ["500", "2", "4000"], "a fast breathe is raised to the slow-breathe floor")
    ck(rec(500, 1, 500) == ["500", "1", "1000"], "a fast flash is raised to the flash floor")
    ck(rec(500, 0, 0) == ["500", "0", "4000"] and rec(500, 2, 0) == ["500", "2", "4000"],
       "no period at all is the OS default")
    ck(rec(500, 0, 10) == ["500", "0", "10"], "steady keeps the period it was given, and ignores it")

    def eff(e, p, now):
        return int(run(exe, "eff", e, p, now).split()[1])
    ck(all(eff(0, 1000, t) == 255 for t in (0, 1, 999, 12345678)), "steady is simply on")
    ck(eff(1, 500, 0) == 255 and eff(1, 500, 499) == 255 and eff(1, 500, 500) == 0 and eff(1, 500, 999) == 0
       and eff(1, 500, 1000) == 255, "flash's period is one whole cycle: at the 1 s floor, half a second lit, half dark")
    ck(eff(1, 2000, 999) == 255 and eff(1, 2000, 1000) == 0 and eff(1, 2000, 1999) == 0 and eff(1, 2000, 2000) == 255,
       "a 2 s flash is one second on, one second off (the pomodoro's waiting)")
    br = [eff(2, 100, t) for t in (0, 500, 1000, 1500, 2000, 2500, 3000, 3500, 3999)]
    ck(br[0] == 0 and br[4] == 255 and br[-1] < 3 and all(0 <= v <= 255 for v in br)
       and br[:5] == sorted(br[:5]) and br[4:] == sorted(br[4:], reverse=True),
       "breathe ramps in and out over a clamped 4 s, never a step: %s" % br)
    ck(eff(7, 4000, 3000) == 255, "the clock reads an unknown effect as steady")
    bs = [eff(2, 4000, t) for t in range(0, 4000, 100)]
    ck(max(abs(b - a) for a, b in zip(bs, bs[1:])) <= 24, "breathe never jumps: the widest 100 ms step is %d"
       % max(abs(b - a) for a, b in zip(bs, bs[1:])))
    ck(rec(500, 2, 100) == rec(500, 2, 4000) and rec(500, 1, 1000) == rec(500, 1, 500),
       "clamping a clamped record changes nothing")

    def own(*ops, n=8):
        return [ln.split() for ln in run(exe, "own", n, *ops).splitlines()]
    one = own("I", "S 2 300 0 0 1000000", "P")
    ck(one[-1][1:] == ["1", "300", "0"], "an app that sets the ring owns it")
    ab = own("I", "S 2 300 0 0 1000000", "S 5 700 0 0 2000000", "P")
    ck(ab[-1][1:] == ["1", "700", "0"], "the latest caller owns the one ring")
    cd = own("I", "S 2 300 0 0 1000000", "S 5 700 0 0 2000000", "C 5", "P")
    ck(cd[-1][1:] == ["1", "300", "0"], "a clear reveals the next most recent record")
    ef = own("I", "P", "S 3 100 0 0 5000000", "C 3", "P")
    ck([l[1] for l in ef] == ["0", "0"] and ef[0][2:] == ["0", "0"], "nothing owned is nothing on the glass")
    tw = own("I", "S 2 300 0 0 1000000", "S 2 500 0 0 3000000", "P")
    ck(tw[-1][1:] == ["1", "500", "0"], "one app has one record: a re-set replaces it")
    cl2 = own("I", "C 4", "P")
    ck(cl2[-1][1:] == ["0", "0", "0"], "clearing a slot that never set anything is a no-op")
    tk = own("I", "S 6 600 0 0 7000200", "S 1 200 0 0 7000900", "P")
    ck(tk[-1][1:] == ["1", "600", "0"], "two sets inside one ms: the stamp is the ms, so the higher slot owns")
    cl = own("I", "S 4 2000 9 100 7000499", "P")
    ck(cl[-1][1:] == ["1", "1000", "0"], "what a pick hands back is clamped")
    oo = own("I", "S 2 300 0 0 1000000", "S 5 700 0 0 2000000", "C 5", "C 2", "P")
    ck(oo[-1][1:] == ["0", "0", "0"], "a row is owned or it is not: two clears leave nothing")
    ors = own("I", "S 7 500 0 0 1000000", "S 1 300 0 0 2000000", "C 1", "A 500", "A 1500", n=8)
    ck(ors[-2:] == [["ALPHA", "1", "255"], ["ALPHA", "1", "255"]], "a steady owner shows through the clock")
    fl = own("I", "S 0 500 1 1000 1000000", "A 250", "A 750")
    ck(fl[-2:] == [["ALPHA", "1", "255"], ["ALPHA", "1", "0"]], "off-phase flash touches no glass")
    empty = own("I", "A 1000")
    ck(empty[-1] == ["ALPHA", "0", "-1"], "an empty table never reaches the effect clock")
    oor = own("I", "S 9 300 0 0 1000000", "S -1 300 0 0 1000000", "P")
    ck(oor[-1][1:] == ["0", "0", "0"], "a slot outside the table sets nothing")
    return fails


def main():
    src = open(os.path.join(DISP, "orb_ring.c")).read()
    with tempfile.TemporaryDirectory() as d:
        if "--mutations" in sys.argv:
            caught = 0
            for a, b in MUTATIONS:
                if src.count(a) != 1:
                    raise SystemExit("mutation %r applies %d times -- rewrite the bench" % (a[:40], src.count(a)))
                exe = build(src.replace(a, b, 1), d)
                bit = checks(exe, quiet=True) > 0
                print(("bites " if bit else "MISSED") + "  " + a.strip()[:66])
                caught += bit
            print("mutants caught %d/%d" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
        fails = checks(build(src, d))
        print("RESULT: pass" if not fails else "RESULT: %d failed" % fails)
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
