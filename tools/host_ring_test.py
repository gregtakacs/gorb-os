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
pixels, for every row and a sweep of fractions (ring-design's orchestrator amendment 1), for
every start and both directions of rim-design §3's matrix — and at start 0 clockwise the numbers
themselves are pinned to the base commit's own output, not re-derived.

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
# rim-design §3's matrix: these starts, both directions, the fraction sweep above, every row.
STARTS = [0, 90, 180, 270, 45, 359]
CCWS = [0, 1]

# The shipped case, captured from the base commit's binary (delegate/rim-design, before the wedge):
# start 0 clockwise is byte-for-byte the ring that ships, so these numbers are the old expectations,
# not a re-derivation of them. "row:fraction" -> the pieces, exactly as `one` prints them.
BASE_CASES = [
    "2:1: [218,232]0 [233,233]1 [234,247]0",
    "2:125: [218,232]0 [233,247]1",
    "2:250: [218,232]0 [233,247]1",
    "2:499: [218,232]0 [233,247]1",
    "2:500: [218,232]0 [233,247]1",
    "2:501: [218,232]0 [233,247]1",
    "2:750: [218,232]0 [233,247]1",
    "2:999: [218,231]1 [232,232]0 [233,247]1",
    "5:1: [193,232]0 [233,233]1 [234,272]0",
    "5:125: [193,232]0 [233,272]1",
    "5:250: [193,232]0 [233,272]1",
    "5:499: [193,232]0 [233,272]1",
    "5:500: [193,232]0 [233,272]1",
    "5:501: [193,232]0 [233,272]1",
    "5:750: [193,232]0 [233,272]1",
    "5:999: [193,231]1 [232,232]0 [233,272]1",
    "9:1: [175,217]0 [248,290]0",
    "9:125: [175,217]0 [248,290]1",
    "9:250: [175,217]0 [248,290]1",
    "9:499: [175,217]0 [248,290]1",
    "9:500: [175,217]0 [248,290]1",
    "9:501: [175,217]0 [248,290]1",
    "9:750: [175,217]0 [248,290]1",
    "9:999: [175,217]1 [248,290]1",
    "72:1: [67,76]0 [389,398]0",
    "72:125: [67,76]0 [389,393]1 [394,398]0",
    "72:250: [67,76]0 [389,398]1",
    "72:499: [67,76]0 [389,398]1",
    "72:500: [67,76]0 [389,398]1",
    "72:501: [67,76]0 [389,398]1",
    "72:750: [67,76]0 [389,398]1",
    "72:999: [67,76]1 [389,398]1",
    "100:1: [44,51]0 [414,421]0",
    "100:125: [44,51]0 [414,421]0",
    "100:250: [44,51]0 [414,421]1",
    "100:499: [44,51]0 [414,421]1",
    "100:500: [44,51]0 [414,421]1",
    "100:501: [44,51]0 [414,421]1",
    "100:750: [44,51]0 [414,421]1",
    "100:999: [44,51]1 [414,421]1",
    "233:1: [2,8]0 [457,463]0",
    "233:125: [2,8]0 [457,463]0",
    "233:250: [2,8]0 [457,463]0",
    "233:499: [2,8]0 [457,463]1",
    "233:500: [2,8]0 [457,463]1",
    "233:501: [2,8]0 [457,463]1",
    "233:750: [2,8]1 [457,463]1",
    "233:999: [2,8]1 [457,463]1",
    "300:1: [12,18]0 [447,453]0",
    "300:125: [12,18]0 [447,453]0",
    "300:250: [12,18]0 [447,453]0",
    "300:499: [12,18]0 [447,453]1",
    "300:500: [12,18]0 [447,453]1",
    "300:501: [12,18]0 [447,453]1",
    "300:750: [12,18]1 [447,453]1",
    "300:999: [12,18]1 [447,453]1",
    "440:1: [131,148]0 [317,334]0",
    "440:125: [131,148]0 [317,334]0",
    "440:250: [131,148]0 [317,334]0",
    "440:499: [131,148]0 [317,334]1",
    "440:500: [131,148]0 [317,334]1",
    "440:501: [131,148]0 [317,334]1",
    "440:750: [131,148]1 [317,334]1",
    "440:999: [131,148]1 [317,334]1",
    "457:1: [179,286]0",
    "457:125: [179,286]0",
    "457:250: [179,286]0",
    "457:499: [179,233]0 [234,286]1",
    "457:500: [179,232]0 [233,286]1",
    "457:501: [179,231]0 [232,286]1",
    "457:750: [179,286]1",
    "457:999: [179,286]1",
    "463:1: [218,247]0",
    "463:125: [218,247]0",
    "463:250: [218,247]0",
    "463:499: [218,233]0 [234,247]1",
    "463:500: [218,232]0 [233,247]1",
    "463:501: [218,231]0 [232,247]1",
    "463:750: [218,247]1",
    "463:999: [218,247]1",
]

MUTATIONS = [
    ("    return wire(packed);", "    return packed;"),
    ("const uint16_t bg = wire(bg565_swapped);", "const uint16_t bg = bg565_swapped;"),
    ("    if (a == 0) return bg565_swapped;", "    if (a == 0) return wire(bg565_swapped);"),
    ("if (y1) *y1 = CC + R_OUT_I;", "if (y1) *y1 = CC + R_IN_I;"),
    ("spans[0][1] = hl - 1;", "spans[0][1] = hl;"),
    ("spans[1][0] = hr + 1;", "spans[1][0] = hr;"),
    ("if (p_hit(y, CC - 1, 1)) {", "if (0) {"),
    # the wedge's two rays: the arc_runs boundary mutants, re-pointed at the two-ray algebra
    ("if ((cs > 0.0) == (cw != 0)) { iv[0] = ce(b); iv[1] = GLASS - 1; }",
     "if ((cs > 0.0) != (cw != 0)) { iv[0] = ce(b); iv[1] = GLASS - 1; }"),                  # a ray's half-plane flipped
    ("    else                         { iv[0] = 0;     iv[1] = clip_x(fl(b)); }",
     "    else                         { iv[0] = clip_x(fl(b)) + 1; iv[1] = GLASS - 1; }"),  # the other side inverted too
    ("        iv[1] = ((-sn * v > 0.0) == (cw != 0)) ? GLASS - 1 : -1;",
     "        iv[1] = ((-sn * v < 0.0) == (cw != 0)) ? GLASS - 1 : -1;"),                    # the horizontal ray's sign test
    ("    return o[0] <= o[1];", "    return o[0] < o[1];"),                                # a one-column sliver dropped
    # rim-design §3's named wedge mutants
    ("    double a0 = ccw ? a - s : a;\n    double a1 = ccw ? a : a + s;",
     "    double a0 = ccw ? a : a - s;\n    double a1 = ccw ? a + s : a;"),                  # the CCW branch inverted
    ("    double a1 = ccw ? a : a + s;", "    double a1 = ccw ? a : a - s;"),                # A1 taken as A0 - s
    ("    const double a = (double)start_deg * TAU / 360.0;", "    const double a = (double)start_deg * TAU / 720.0;"),  # the start read at half the angle
    ("    start_deg %= 360;", "    start_deg = start_deg > 359 ? 0 : start_deg;"),           # the modulo-360 fold broken
    # ring_clamp's two new words
    ("    r->flags &= ORB_RING_FLAGS_MASK;", "    ;"),                                       # unknown flag bits honoured
    ("    r->start = (uint16_t)(r->start % 360);", "    ;"),                                 # the record's start never wraps
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
    ("if (!r || !(r->flags & ORB_RING_MATCH)) return;", "if (!r) return;"),
    ("r->arc   = (r->arc   & 0xFF000000u) | rgb;", "r->arc   = 0xFF000000u | rgb;"),
    ("if ((overlay & 0xFF000000u) != 0x01000000u) return;", "if (overlay == 0) return;"),
    # rim-2: lane greedy, alignment and filter mutants
    ("    if (best_b == 0) best_b = 2;", "    best_b = 2;"),                              # 2-row fallback deleted: greedy forced to every band being 2-row
    ("    int lo = a & ~1;", "    int lo = a;"),                                           # x-alignment dropped: odd x0 reaches the panel
    ("        y += best_b;", "        y += best_b + 1;"),                                 # row-count evening dropped: next band starts odd
    ("if (area - a <= ORB_RING_LANE_EXTRA_MAX) { best_b = b; break; }",
     "if (area - a <= 0x7FFFFFFF) { best_b = b; break; }"),                                # extra bound ignored: totals move
    ("    for (int b = hi_b; b >= 2; b -= 2) {", "    for (int b = hi_b; b >= 2; b -= 1) {"),  # band height allowed to go odd
    ("    for (int i = 0; i < n; i++) lanes[i] = tmp[i];",
     "    for (int i = 0; i < n; i++) lanes[n - 1 - i] = tmp[i];"),                      # emitted order reversed
    ("        if (lanes[i].y1 < y0 || lanes[i].y0 > y1) continue;",
     "        if (lanes[i].y1 <= y0 || lanes[i].y0 >= y1) continue;"),                   # ring_lanes_in overlap off by one row
    ("    if (all_side && lx1 >= lx0) {", "    if (all_side == 0 && lx1 >= lx0) {"),     # widest-lane bound broken: shoulder bands merged
    # rim-3: the gate and the governor
    ("    return d >= ORB_RING_STEP_ALPHA;", "    return d <= ORB_RING_STEP_ALPHA;"),  # the gate's comparison inverted: a still band sends, a moving one rests
    ("    if (was_alpha <= 0 || now_alpha <= 0) return 1;", "    ;"),                   # the two ends ungated: a band fading out is never sent home
    ("    int g = (batch_us * 3) / 1000;", "    int g = batch_us / 1000;"),             # the 3x factor dropped: the ring may fill its own window
    ("    if (g > ORB_RING_STEP_MAX_MS) g = ORB_RING_STEP_MAX_MS;", "    ;"),           # the ceiling dropped: a starved bus is worse than the base build
    ("    if (g < ORB_RING_STEP_MS)     g = ORB_RING_STEP_MS;", "    ;"),               # the floor dropped: a measured batch slower than the grid runs slower than the grid
    # alerts-1: the backed ring — the mask width, the colour, the paint alpha, the gate
    ("    r->flags &= ORB_RING_FLAGS_MASK;", "    r->flags &= 0x03u;"),                # the mask not widened: a backed record's ask is dropped
    ("    if (!arc) return 0x0000;", "    if (!arc) return ring_blend565(0x0000, argb);"),  # the band's black is not black
    ("    return ring_blend565(0x0000, argb);", "    return ring_blend565(0x0000, argb | 0xFF000000u);"),  # the arc ignores its alpha: no fade to black
    ("    if (!(r->flags & ORB_RING_BACKED)) return effect_alpha;", "    return effect_alpha;"),            # the backed branch dropped: the band obeys the byte
    ("    if (r->effect == ORB_RING_FLASH)   return effect_alpha;", "    if (r->effect == ORB_RING_FLASH)   return 255;"),  # a backed flash off-phase forced lit
    ("        return effect_alpha < ORB_RING_BACKED_MIN_ALPHA ? ORB_RING_BACKED_MIN_ALPHA : effect_alpha;", "        return effect_alpha;"),  # the breathe floor gone: reveals the app
    ("    return was_paint != now_paint;", "    return was_paint <= 0 || now_paint <= 0;"),  # the backed gate borrows the never-zero rule: an unchanged black band resends
    ("    return was_paint != now_paint;", "    return was_paint == now_paint;"),            # the backed gate inverted: a transition rests, a still band sends
]

HARNESS = r"""
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_ring.h"

#define GLASS 466

#define TAU 6.28318531f
#define MAXC 1024

/* The reference is the copy arc's own per-pixel test — atan2f and the two radii — with the wedge
 * of rim-design §3 replacing the prefix: the inside test measures from the record's start in the
 * record's own direction, so a clockwise arc is [start, start + frac*360) and an anticlockwise one
 * is (start - frac*360, start]. */
typedef struct { int frac, start, ccw; } cse_t;

static int in_wedge(float a, float a0, float sp, int ccw)
{
    const float d = ccw ? (a0 - a) : (a - a0);              /* how far in, in the record's direction */
    return (d < 0.0f ? d + TAU : d) <= sp ? 1 : 0;          /* the test keeps the direction it came from */
}
static int wedge_flag(const cse_t *c, float a)              /* the reference's arc flag for one angle */
{
    if (c->frac <= 0) return 0;
    if (c->frac >= 1000) return 1;
    const int st = ((c->start % 360) + 360) % 360;          /* a start is read mod 360 */
    return in_wedge(a, (float)st * TAU / 360.0f, (float)c->frac * TAU / 1000.0f, c->ccw);
}

static int y_row;                                           /* the row the matrix is on */
static float sang[GLASS];                                   /* this row's angle, -1 off the annulus */
static void row_ref(int y)                                  /* atan2f once per row, shared by every case */
{
    const float dy = (float)y + 0.5f - 233.0f;
    for (int x = 0; x < GLASS; x++) {
        const float dx = (float)x + 0.5f - 233.0f, d2 = dx * dx + dy * dy;
        float a = -1.0f;
        if (d2 >= 224.0f * 224.0f && d2 <= 231.0f * 231.0f) {
            a = atan2f(dx, -dy);
            if (a < 0.0f) a += TAU;
        }
        sang[x] = a;
    }
}

/* A pixel whose centre lies exactly on one of the wedge's two rays is the tie the reference cannot
 * call either way: atan2f and this file's sin/cos round the same angle differently by an ulp, and
 * the row can be entirely on one side of it (so no neighbour disagrees either). rim-design §3 lets
 * a disagreement stand only on a ray boundary, and an exact tie IS a ray boundary, so the test
 * names it directly: within 1e-6 px of the half-line, and in front of the centre along it. */
static int on_ray(const cse_t *c, int x)
{
    const double dx = (double)x + 0.5 - 233.0, dy = (double)y_row + 0.5 - 233.0;
    const double sp = (double)c->frac * 6.283185307179586 / 1000.0;
    const double st = (double)(((c->start % 360) + 360) % 360) * 6.283185307179586 / 360.0;
    const double a[2] = { st, c->ccw ? st - sp : st + sp };
    for (int i = 0; i < 2; i++) {
        const double ux = sin(a[i]), uy = -cos(a[i]);         /* the ray, in glass coordinates */
        if (fabs(ux * dy - uy * dx) < 1e-6 && ux * dx + uy * dy > 0.0) return 1;
    }
    return 0;
}

typedef struct { int bad_ann, arc_rows, arc_max, off_bound, dup, outside, pmax, nref, ngot; } acc_t;
static acc_t acc[MAXC];
static unsigned char got[GLASS];
static orb_ring_part_t p[8];

static void one_case(const cse_t *c, int i)                 /* this row, this case */
{
    acc_t *A = &acc[i];
    const int np = ring_row_parts(y_row, c->frac, c->start, c->ccw, p, 8);
    if (np > A->pmax) A->pmax = np;
    memset(got, 0xFF, sizeof got);
    for (int k = 0; k < np; k++)
        for (int x = p[k].x0; x <= p[k].x1; x++) {
            if (x < 0 || x >= GLASS || p[k].x0 < 0 || p[k].x1 >= GLASS) { A->outside++; continue; }
            if (got[x] != 0xFF) A->dup++;
            got[x] = p[k].arc; A->ngot++;
        }
    int rowdiff = 0;
    for (int x = 0; x < GLASS; x++) {
        const int r = sang[x] < 0.0f ? -1 : wedge_flag(c, sang[x]);
        if (r >= 0) A->nref++;
        if ((r < 0) != (got[x] == 0xFF)) { A->bad_ann++; continue; }
        if (r < 0 || got[x] == (unsigned char)r) continue;
        rowdiff++;
        int bound = on_ray(c, x);                           /* a reference edge within one pixel */
        for (int d = -1; d <= 1 && !bound; d += 2) {
            const int nb = x + d;
            if (nb < 0 || nb >= GLASS || sang[nb] < 0.0f) continue;
            if (wedge_flag(c, sang[nb]) != r) bound = 1;
        }
        if (!bound) A->off_bound++;
    }
    if (rowdiff) { A->arc_rows++; if (rowdiff > A->arc_max) A->arc_max = rowdiff; }
}

static void matrix(cse_t *cs, int n)
{
    memset(acc, 0, sizeof *acc * (size_t)n);
    for (y_row = 0; y_row < GLASS; y_row++) {
        row_ref(y_row);
        for (int i = 0; i < n; i++) one_case(&cs[i], i);
    }
    for (int i = 0; i < n; i++)
        printf("FR %d bad_ann %d arc_rows %d arc_max %d off_bound %d dup %d outside %d parts_max %d px %d/%d ST %d CC %d\n",
               cs[i].frac, acc[i].bad_ann, acc[i].arc_rows, acc[i].arc_max, acc[i].off_bound, acc[i].dup,
               acc[i].outside, acc[i].pmax, acc[i].ngot, acc[i].nref, cs[i].start, cs[i].ccw);
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
    } else if (!strcmp(m, "ap")) {
        /* ring_apply_overlay (overlay-design §6/§8 check 4): build a record with the ask and two
         * colours, apply an overlay `times` times, dump all 16 bytes so a no-op is proven byte for
         * byte. argv: match arc track overlay times. */
        orb_ring_rec r = { 500, 0, (uint8_t)lg(argv[2]),
                           (uint32_t)strtoul(argv[3], NULL, 0), (uint32_t)strtoul(argv[4], NULL, 0), 0, 0 };
        const uint32_t ovl = (uint32_t)strtoul(argv[5], NULL, 0);
        const int tm = (int)lg(argv[6]);
        for (int i = 0; i < tm; i++) ring_apply_overlay(&r, ovl);
        const unsigned char *p = (const unsigned char *)&r;
        printf("AP");
        for (unsigned i = 0; i < (unsigned)sizeof r; i++) printf(" %02X", p[i]);
        printf("\n");
    } else if (!strcmp(m, "mk")) {
        /* ring_clamp owns the flags byte (§8 check 5, rim-design §2): argv fraction effect flags period. */
        orb_ring_rec r = { (uint16_t)lg(argv[2]), (uint8_t)lg(argv[3]), (uint8_t)lg(argv[4]),
                           0, 0, (uint16_t)lg(argv[5]), 0 };
        ring_clamp(&r);
        printf("MK %u %u %u %u\n", r.fraction, r.effect, r.period_ms, r.flags);
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
               ring_row_parts(233, 500, 0, 0, NULL, 4), ring_row_parts(233, 500, 0, 0, p, 0));
    } else if (!strcmp(m, "one") || !strcmp(m, "onew")) {
        const int w = !strcmp(m, "onew");
        const int st = w ? (int)lg(argv[4]) : 0, cc = w ? (int)lg(argv[5]) : 0;
        const int cap = argc > (w ? 6 : 4) ? (int)lg(argv[w ? 6 : 4]) : 8;
        orb_ring_part_t p[8];
        const int n = ring_row_parts((int)lg(argv[2]), (int)lg(argv[3]), st, cc, p, cap);
        printf("ONE %d %d n %d", (int)lg(argv[2]), (int)lg(argv[3]), n);
        for (int i = 0; i < n; i++) printf(" [%d,%d]%d", p[i].x0, p[i].x1, p[i].arc);
        printf("\n");
    } else if (!strcmp(m, "cl2")) {
        /* the two words rim-1 adds: argv fraction effect flags start period, twice through */
        orb_ring_rec r = { (uint16_t)lg(argv[2]), (uint8_t)lg(argv[3]), (uint8_t)lg(argv[4]),
                           0, 0, (uint16_t)lg(argv[6]), (uint16_t)lg(argv[5]) };
        ring_clamp(&r);
        const orb_ring_rec a = r;
        ring_clamp(&r);
        printf("CL2 %u %u %u %u %u %d\n", r.fraction, r.effect, r.period_ms, r.flags, r.start,
               memcmp(&a, &r, sizeof r) == 0);
    } else if (!strcmp(m, "sweep") || !strcmp(m, "wedges")) {
        /* argv: start ccw frac, start ccw frac, ... — §3's matrix is the whole cross product */
        cse_t cs[MAXC];
        int n = 0;
        for (int i = 2; i + 2 < argc && n < MAXC; i += 3) {
            cs[n].frac = (int)lg(argv[i + 2]); cs[n].start = (int)lg(argv[i]); cs[n].ccw = (int)lg(argv[i + 1]);
            n++;
        }
        matrix(cs, n);
    } else if (!strcmp(m, "own")) {
        const int n = (int)lg(argv[2]);
        for (int i = 3; i < argc; i++) {
            const char *o = argv[i];
            int a, b, c, d, f, h; long long e;
            if (o[0] == 'I') ring_init(tbl, ownr, n);
            else if (sscanf(o, "S %d %d %d %d %lld", &a, &b, &c, &d, &e) == 5) {
                orb_ring_rec r = { (uint16_t)b, (uint8_t)c, 0, 0, 0, (uint16_t)d, 0 };
                ring_set(tbl, ownr, seqq, n, a, &r, (int64_t)e);
            } else if (sscanf(o, "C %d", &a) == 1) ring_clear(tbl, ownr, n, a);
            else if (sscanf(o, "W %d %d %d %d %d %d %lld", &a, &b, &c, &d, &f, &h, &e) == 7) {
                /* the same set with the two words rim-1 adds:
                 * slot fraction effect flags start period us */
                orb_ring_rec r = { (uint16_t)b, (uint8_t)c, (uint8_t)d, 0, 0, (uint16_t)h, (uint16_t)f };
                ring_set(tbl, ownr, seqq, n, a, &r, e);
            } else if (o[0] == 'G') {
                orb_ring_rec g2;
                if (!ring_pick(tbl, ownr, seqq, n, &g2)) printf("GET 0 0 0 0\n");
                else printf("GET %u %u %u %u\n", g2.fraction, g2.effect, g2.flags, g2.start);
            }
            else if (o[0] == 'P' || o[0] == 'A') {
                orb_ring_rec g;
                const int ok = ring_pick(tbl, ownr, seqq, n, &g);
                if (o[0] == 'P') printf("PICK %d %u %u\n", (unsigned)ok, (unsigned)(ok ? g.fraction : 0), (unsigned)(ok ? g.effect : 0));
                else printf("ALPHA %d %d\n", ok, ok ? ring_effect_alpha(&g, (int64_t)(o[1] ? atoi(o + 1) : 0)) : -1);
            }
        }
    } else if (!strcmp(m, "consts")) {
        /* the numbers rim-3 shipped, as the compiled translation unit sees them (§6.1) */
        printf("CONST %d %d %d %d %d %d %d\n", ORB_RING_STEP_MS, ORB_RING_BREATHE_MIN_MS,
               ORB_RING_FLASH_MIN_MS, ORB_RING_STEP_PCT, ORB_RING_STEP_ALPHA,
               ORB_RING_STEP_MAX_MS, ORB_RING_DEFAULT_PERIOD_MS);
    } else if (!strcmp(m, "gate")) {
        printf("GATE %d\n", ring_alpha_moved((int)lg(argv[2]), (int)lg(argv[3])));
    } else if (!strcmp(m, "gov")) {
        printf("GOV %d\n", ring_next_step_ms((int)lg(argv[2])));
    } else if (!strcmp(m, "lanes")) {
        orb_ring_lane_t la[ORB_RING_LANES_MAX];
        const int n = ring_lanes(la, ORB_RING_LANES_MAX);
        printf("LN %d\n", n);
        for (int i = 0; i < n; i++)
            printf("L %d %d %d %d\n", la[i].x0, la[i].y0, la[i].x1, la[i].y1);
        orb_ring_lane_t lb[ORB_RING_LANES_MAX];
        const int nf = ring_lanes_in(la, n, 2, 463, lb, ORB_RING_LANES_MAX);
        printf("LIF %d\n", nf);
        for (int i = 0; i < nf; i++)
            printf("F %d %d %d %d\n", lb[i].x0, lb[i].y0, lb[i].x1, lb[i].y1);
        const int n1 = ring_lanes_in(la, n, 100, 140, lb, ORB_RING_LANES_MAX);
        printf("LI1 %d\n", n1);
        for (int i = 0; i < n1; i++)
            printf("I %d %d %d %d\n", lb[i].x0, lb[i].y0, lb[i].x1, lb[i].y1);
        const int no = ring_lanes_in(la, n, 0, 1, lb, ORB_RING_LANES_MAX);
        printf("LIOB %d\n", no);
        const int ni = ring_lanes_in(la, n, 200, 100, lb, ORB_RING_LANES_MAX);
        printf("LINV %d\n", ni);
        const int nb = ring_lanes_in(la, n, 9, 9, lb, ORB_RING_LANES_MAX);
        printf("LBDY %d\n", nb);
        orb_ring_lane_t lc[ORB_RING_LANES_MAX];
        const int n2 = ring_lanes(lc, ORB_RING_LANES_MAX);
        const int det = n2 == n && memcmp(la, lc, (size_t)n * sizeof *la) == 0;
        printf("DETER %d\n", det);
    } else if (!strcmp(m, "lnedge")) {
        orb_ring_lane_t la[ORB_RING_LANES_MAX];
        const int full = ring_lanes(la, ORB_RING_LANES_MAX);
        printf("LNE0 %d\n", ring_lanes(NULL, 0));
        printf("LNE1 %d\n", ring_lanes(NULL, ORB_RING_LANES_MAX));
        printf("LNE2 %d\n", ring_lanes(la, 0));
        printf("LNE3 %d\n", ring_lanes(la, -1));
        printf("LNE4 %d\n", ring_lanes(la, full - 1));
        printf("LNE5 %d\n", full);
        printf("LNE6 %d\n", ring_lanes(la, full));
        printf("LNE7 %d\n", ring_lanes_in(NULL, full, 2, 463, la, ORB_RING_LANES_MAX));
        printf("LNE8 %d\n", ring_lanes_in(la, 0, 2, 463, la, ORB_RING_LANES_MAX));
        printf("LNE9 %d\n", ring_lanes_in(la, full, 2, 463, la, 0));
    } else if (!strcmp(m, "bk565")) {
        /* ring_backed565 (alerts-1 §64.12): argv arc argb -> the wire-order band pixel */
        printf("BK565 %04X\n", ring_backed565((uint32_t)strtoul(argv[3], NULL, 0), (int)lg(argv[2])));
    } else if (!strcmp(m, "bkpa")) {
        /* ring_paint_alpha (alerts-1 §64.12): argv flags effect effect_alpha */
        orb_ring_rec r = { 500, (uint8_t)lg(argv[3]), (uint8_t)lg(argv[2]), 0, 0, 0, 0 };
        printf("BKPA %d\n", ring_paint_alpha(&r, (int)lg(argv[4])));
    } else if (!strcmp(m, "bkmo")) {
        printf("BKMO %d\n", ring_backed_moved((int)lg(argv[2]), (int)lg(argv[3])));
    } else if (!strcmp(m, "pulse")) {
        /* ring_pulse_open (alerts-2 §64.13): argv until_ms now_ms — the half-open window gate
         * the lent-press confirmation leans on. The glue arms `until` and asks it each step. */
        printf("PULSE %d\n", ring_pulse_open(lg(argv[2]), lg(argv[3])));
    } else if (!strcmp(m, "bkcover")) {
        /* the annulus a backed band must paint, as the stamp sees it: argv fraction start ccw.
         * The pieces ring_row_parts returns (arc and track) must cover every annulus pixel and no
         * pixel outside it, whatever the fraction — a backed record paints the track black, so the
         * whole band is a paint and the app underneath is never left showing through. */
        const int frac = (int)lg(argv[2]), st = (int)lg(argv[3]), cc = (int)lg(argv[4]);
        long ann = 0, covin = 0, covout = 0, arcpx = 0, trkpx = 0;
        orb_ring_part_t pp[8];
        for (int y = 0; y < GLASS; y++) {
            const int np = ring_row_parts(y, frac, st, cc, pp, 8);
            unsigned char inm[GLASS], cov[GLASS];
            for (int x = 0; x < GLASS; x++) {
                const long u = 2 * (long)x - 465, v = 2 * (long)y - 465;
                cov[x] = 0;
                inm[x] = (200704 <= u * u + v * v && u * u + v * v <= 213444) ? 1 : 0;
                if (inm[x]) ann++;
            }
            for (int i = 0; i < np; i++)
                for (int x = pp[i].x0; x <= pp[i].x1; x++) {
                    if (x < 0 || x >= GLASS || cov[x]) continue;
                    cov[x] = 1;
                    if (inm[x]) covin++; else covout++;
                    if (pp[i].arc) arcpx++; else trkpx++;
                }
        }
        printf("BKCOVER ann %ld covin %ld covout %ld arc %ld trk %ld\n", ann, covin, covout, arcpx, trkpx);
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

    def trip(st, cc, fr):                       # the harness's argv: start ccw fraction, per case
        return [x for f in fr for x in (st, cc, f)]

    frs = [ln.split() for ln in run(exe, "sweep", *trip(0, 0, FRACS)).splitlines()]
    ck(len(frs) == len(FRACS), "the sweep answers for every fraction asked")
    ck(all(f[-4] == "ST" and f[-2] == "CC" and f[-3] == "0" and f[-1] == "0" for f in frs),
       "the shipped sweep is the wedge at start 0, clockwise")

    def good(f):
        g = dict(zip(f[2::2], f[3::2]))
        n = [int(v) for v in g["px"].split("/")]
        return (int(g["bad_ann"]) == 0 and int(g["dup"]) == 0 and int(g["outside"]) == 0
                and 1 <= int(g["parts_max"]) <= 4 and n[0] == n[1] > 0
                and (int(g["arc_rows"]) == 0 or (int(g["arc_max"]) <= 1 and int(g["off_bound"]) == 0)))

    bad = [f for f in frs if not good(f)]
    ck(not bad, "the spans paint the pixels the per-pixel reference paints, every row of %d fractions%s"
       % (len(FRACS), (": " + " ".join(bad[0])) if bad else ""))

    # rim-design §3's matrix: every start, both directions, the same fraction sweep, all 466 rows
    args = []
    for st in STARTS:
        for cc in CCWS:
            args += trip(st, cc, FRACS)
    mx = [ln.split() for ln in run(exe, "wedges", *args).splitlines()]
    ck(len(mx) == len(STARTS) * len(CCWS) * len(FRACS),
       "the wedge matrix answers for every case asked: %d of %d" % (len(mx), len(STARTS) * len(CCWS) * len(FRACS)))
    mbad = [f for f in mx if not good(f)]
    ck(not mbad, "the wedge cuts the annulus the reference wedge cuts, %d cases over all 466 rows%s"
       % (len(mx), (": " + " ".join(mbad[0])) if mbad else ""))
    seen = max(int(dict(zip(f[2::2], f[3::2]))["parts_max"]) for f in mx)
    ck(seen <= 4, "a wedge never needs more pieces than the shipped ring: ORB_RING_PARTS_MAX 4, the matrix saw %d" % seen)

    def parts(y, f, cap=8):
        w = run(exe, "one", y, f, cap).split()
        return (int(w[4]), w[5:]) if len(w) > 4 else (-1, [])
    def partsw(y, f, st, cc, cap=8):
        w = run(exe, "onew", y, f, st, cc, cap).split()
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
    # rim-design §6.1 moved both floors with the lanes: breathe's is 1500 ms and flash's 400, so a
    # record can only be slowed down to the new numbers. Retargeted from the base commit's 4000/1000,
    # which named the floors and not the behaviour: a fast ask is still raised, and still cannot be
    # sped up by asking twice.
    ck(rec(500, 2, 100) == ["500", "2", "1500"], "a fast breathe is raised to the slow-breathe floor")
    ck(rec(500, 1, 100) == ["500", "1", "400"], "a fast flash is raised to the flash floor")
    ck(rec(500, 0, 0) == ["500", "0", "4000"] and rec(500, 2, 0) == ["500", "2", "4000"],
       "no period at all is the OS default")
    ck(rec(500, 0, 10) == ["500", "0", "10"], "steady keeps the period it was given, and ignores it")

    def eff(e, p, now):
        return int(run(exe, "eff", e, p, now).split()[1])
    ck(all(eff(0, 1000, t) == 255 for t in (0, 1, 999, 12345678)), "steady is simply on")
    ck(eff(1, 100, 0) == 255 and eff(1, 100, 199) == 255 and eff(1, 100, 200) == 0 and eff(1, 100, 399) == 0
       and eff(1, 100, 400) == 255, "flash's period is one whole cycle: at the 400 ms floor, 200 ms lit, 200 ms dark")
    ck(eff(1, 2000, 999) == 255 and eff(1, 2000, 1000) == 0 and eff(1, 2000, 1999) == 0 and eff(1, 2000, 2000) == 255,
       "a 2 s flash is one second on, one second off (the pomodoro's waiting)")
    br = [eff(2, 100, t) for t in (0, 187, 375, 562, 750, 937, 1125, 1312, 1499)]
    ck(br[0] == 0 and br[4] == 255 and br[-1] < 3 and all(0 <= v <= 255 for v in br)
       and br[:5] == sorted(br[:5]) and br[4:] == sorted(br[4:], reverse=True),
       "breathe ramps in and out over a clamped 1.5 s, never a step: %s" % br)
    ck(eff(7, 4000, 3000) == 255, "the clock reads an unknown effect as steady")
    bs = [eff(2, 4000, t) for t in range(0, 4000, 100)]
    ck(max(abs(b - a) for a, b in zip(bs, bs[1:])) <= 24, "breathe never jumps: the widest 100 ms step is %d"
       % max(abs(b - a) for a, b in zip(bs, bs[1:])))
    ck(rec(500, 2, 100) == rec(500, 2, 1500) and rec(500, 1, 100) == rec(500, 1, 400),
       "clamping a clamped record changes nothing")

    # ---- overlay-2: the match byte and the substitution (§8 checks 4 and 5) --------------
    def mk(f, e, match, p):
        return run(exe, "mk", f, e, match, p).split()[1:]
    ck(mk(500, 0, 7, 4000) == ["500", "0", "4000", "7"], "a flags byte of 7 keeps its three known bits, masks to 7, nothing else moves")
    ck(mk(500, 1, 0, 100) == ["500", "1", "400", "0"], "a flags byte of 0 stays 0 (a zeroed record is not backed)")
    ck(mk(500, 0, 1, 4000) == ["500", "0", "4000", "1"], "a flags byte of 1 (ORB_RING_MATCH) stays 1")
    ck(mk(500, 0, 2, 4000) == ["500", "0", "4000", "2"], "a flags byte of 2 (ORB_RING_CCW) stays 2")
    ck(mk(500, 0, 4, 4000) == ["500", "0", "4000", "4"], "a flags byte of 4 (ORB_RING_BACKED) survives the widened mask")
    ck(mk(500, 0, 0xF8, 4000) == ["500", "0", "4000", "0"], "flags 0xF8 is all unknown bits: it clamps to 0")
    ck(mk(500, 0, 5, 4000) == ["500", "0", "4000", "5"], "MATCH beside BACKED keeps both: 5 is MATCH|BACKED")

    def ap(match, arc, track, ovl, times=1):
        return "".join(run(exe, "ap", match, hex(arc), hex(track), hex(ovl), times).split()[1:])
    def rec_bytes(match, arc, track):
        # the record as the harness built it (fraction 500, effect 0, period 0) with these fields
        import struct
        return struct.pack("<HBBIIHH", 500, 0, match, arc, track, 0, 0).hex().upper()
    ck(ap(0, 0x80FF0000, 0x3300FF00, 0x01123456) == rec_bytes(0, 0x80FF0000, 0x3300FF00),
       "a record that did not ask is unchanged byte for byte, over any theme colour")
    ck(ap(1, 0x80FF0000, 0x3300FF00, 0x01123456) == rec_bytes(1, 0x80123456, 0x33123456),
       "a matching record takes the theme RGB and keeps each alpha")
    ck(ap(1, 0x80FF0000, 0x3300FF00, 0x01123456, 2) == ap(1, 0x80FF0000, 0x3300FF00, 0x01123456, 1),
       "applying the substitution twice is applying it once")
    ck(ap(1, 0x80FF0000, 0x3300FF00, 0) == rec_bytes(1, 0x80FF0000, 0x3300FF00),
       "overlay 0 (a theme that names no colour) changes nothing — amendment H's 'behaves as From the look'")
    ck(ap(1, 0x00FF0000, 0x0000FF00, 0x01123456) == rec_bytes(1, 0x00123456, 0x00123456),
       "a muted band stays muted: alpha 0 keeps its alpha over any theme colour")
    ck(ap(1, 0x80FF0000, 0x3300FF00, 0x01000000) == rec_bytes(1, 0x80000000, 0x33000000),
       "0x01000000 (black, present) applies")
    ck(ap(1, 0x80FF0000, 0x3300FF00, 0x02000000) == rec_bytes(1, 0x80FF0000, 0x3300FF00)
       and ap(1, 0x80FF0000, 0x3300FF00, 0x00123456) == rec_bytes(1, 0x80FF0000, 0x3300FF00),
       "a presence test, not a nonzero test: 0x02rrggbb and 0x00rrggbb are absence and change nothing")

    # ---- rim-1: the wedge, and the record's two new words --------------------------------
    base = {tuple(t.split(":", 2)[:2]): t.split(":", 2)[2].strip() for t in BASE_CASES}
    gone = []
    for (ys, fs), v in base.items():
        n, pc = partsw(int(ys), int(fs), 0, 0)
        if " ".join(pc) != v:
            gone.append("%s:%s got [%s] want [%s]" % (ys, fs, " ".join(pc), v))
    ck(not gone, "start 0 clockwise is byte-for-byte the shipped ring: %d rows and fractions re-read%s"
       % (len(base), ("" if not gone else ": " + gone[0])))

    ck(partsw(233, 250, 0, 0) == partsw(233, 250, 3600, 0),
       "a start is read mod 360: 0 and 3600 are the same ask")
    ck(partsw(100, 625, 91, 0) == partsw(100, 625, 451, 0), "451 drains where 91 does: one full turn off the start")
    ck(partsw(300, 375, -45, 1) == partsw(300, 375, 315, 1), "a negative start wraps to the same wedge")
    ck(partsw(100, 250, 90, 1) == (2, ["[44,51]0", "[414,421]1"]),
       "start 90 anticlockwise at 250 is the wedge (0, 90]: start minus the sweep to the start: %s"
       % (partsw(100, 250, 90, 1),))
    ck(partsw(365, 250, 270, 1) == (2, ["[44,51]1", "[414,421]0"]),
       "start 270 anticlockwise at 250 is the wedge (180, 270]: the left slab below the centre: %s"
       % (partsw(365, 250, 270, 1),))
    ck(partsw(100, 250, 90, 0) == (2, ["[44,51]0", "[414,421]0"])
       and partsw(365, 250, 90, 0) == (2, ["[44,51]0", "[414,421]1"]),
       "start 90 clockwise at 250 is the wedge [90, 180): the quarter below three, mirror of the same ask anticlockwise")
    ck(partsw(9, 1000, 359, 0) == partsw(9, 1000, 123, 1) and partsw(9, 0, 359, 1) == partsw(9, 0, 7, 0),
       "fraction 1000 is the whole annulus and fraction 0 none of it, whatever the start and the direction")
    st4 = [(y, f, st, cc) for y in (2, 9, 60, 233, 400, 457, 463) for f in (1, 2, 5, 250, 750, 999)
           for st in (0, 1, 5, 45, 90, 270, 315, 355, 359) for cc in (0, 1)]
    ck(max(partsw(y, f, st, cc)[0] for (y, f, st, cc) in st4) <= 4,
       "%d wedged rows never need more than ORB_RING_PARTS_MAX pieces" % len(st4))
    ck(partsw(9, 1, 5, 0) == (4, ["[175,217]0", "[248,252]0", "[253,253]1", "[254,290]0"])
       and partsw(9, 1, 355, 0) == (4, ["[175,212]0", "[213,214]1", "[215,217]0", "[248,290]0"]),
       "a sliver either side of twelve really does need all four: track, inner arc, outer track, track: %d cases answer 4"
       % sum(1 for (y, f, st, cc) in st4 if partsw(y, f, st, cc)[0] == 4))

    def clmp(f, e, fl_, sd, p):
        return run(exe, "cl2", f, e, fl_, sd, p).split()[1:]
    ck(clmp(500, 0, 0xF8, 0, 4000) == ["500", "0", "4000", "0", "0", "1"],
       "flags 0xF8 is all unknown bits: the record comes back 0, and its other words unmoved")
    ck(clmp(500, 0, 3, 359, 4000) == ["500", "0", "4000", "3", "359", "1"], "359 degrees is a whole start: it stays")
    ck(clmp(500, 0, 3, 360, 4000) == ["500", "0", "4000", "3", "0", "1"], "360 wraps to twelve")
    ck(clmp(500, 0, 3, 361, 4000) == ["500", "0", "4000", "3", "1", "1"], "361 wraps to one degree past twelve")
    ck(clmp(500, 0, 3, 719, 4000) == ["500", "0", "4000", "3", "359", "1"], "719 wraps to 359")
    ck(clmp(500, 0, 3, 720, 4000) == ["500", "0", "4000", "3", "0", "1"], "720 wraps a full twice-round to 0")
    ck(clmp(1500, 7, 7, 4000, 4000) == ["1000", "0", "4000", "7", "40", "1"],
       "one clamp does all five words at once: fraction, effect, flags, start, period")
    ck(clmp(-5, 2, 5, -90, 90000) == ["1000", "2", "24464", "5", "286", "1"],
       "each word clamps on its own type: a uint16 start of -90 is 65446, and 65446 wraps to 286")
    ck(clmp(500, 0, 1, 90, 4000) == clmp(500, 0, 1, 90, 4000),
       "clamping a clamped record changes nothing, the two new words included: %s"
       % (clmp(500, 0, 1, 90, 4000),))


    def own(*ops, n=8):
        return [ln.split() for ln in run(exe, "own", n, *ops).splitlines()]
    one = own("I", "S 2 300 0 0 1000000", "P")
    ck(one[-1][1:] == ["1", "300", "0"], "an app that sets the ring owns it")
    ws = own("I", "W 2 300 0 3 45 4000 1000", "S 5 700 0 0 2000000", "W 2 300 0 3 45 4000 3000000", "P")
    ck(ws[-1][1:] == ["1", "300", "0"], "a record that asks for both new words still owns on a re-set")
    ws2 = own("I", "W 2 300 0 3 45 4000 1000", "G")
    ck(ws2[-1][1:] == ["300", "0", "3", "45"],
       "the table carries the two new words across a set and a pick unchanged: %s" % (ws2[-1][1:],))
    ws3 = own("I", "W 2 300 0 252 4000 4000 1000", "G")
    ck(ws3[-1][1:] == ["300", "0", "4", "40"],
       "what a pick hands back is clamped in both new words too: flags 0xFC keeps its BACKED bit, start 4000 wraps: %s" % (ws3[-1][1:],))
    ws4 = own("I", "W 4 500 0 1 90 4000 1000", "C 4", "G")
    ck(ws4[-1][1:] == ["0", "0", "0", "0"], "a cleared row asks nothing, the two new words included")
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

    # ---- rim-2: ring_lanes() and ring_lanes_in() -------------------------------------------
    lane_out = run(exe, "lanes").splitlines()
    lane_hdr = [ln for ln in lane_out if ln.startswith("LN ")]
    ck(len(lane_hdr) == 1, "the lanes mode answers one count line")
    n_lanes = int(lane_hdr[0].split()[1]) if lane_hdr else 0
    lanes = []
    for ln in lane_out:
        if ln.startswith("L "):
            p = ln.split()
            lanes.append((int(p[1]), int(p[2]), int(p[3]), int(p[4])))
    ck(n_lanes > 0 and n_lanes <= 64, "(a) lane count > 0 and <= ORB_RING_LANES_MAX: %d" % n_lanes)
    for i, (x0, y0, x1, y1) in enumerate(lanes):
        ck(x0 % 2 == 0, "(b) lane %d x0 even: %d" % (i, x0))
        ck((x1 - x0 + 1) % 2 == 0, "(b) lane %d width even: %d" % (i, x1 - x0 + 1))
        ck(y0 % 2 == 0, "(b) lane %d y0 even: %d" % (i, y0))
        ck((y1 - y0 + 1) % 2 == 0, "(b) lane %d row count even: %d" % (i, y1 - y0 + 1))
        ck(x0 >= 0 and x1 <= 465 and y0 >= 2 and y1 <= 463,
           "(b) lane %d inside glass and rows 2..463: x%d..%d y%d..%d" % (i, x0, x1, y0, y1))
    for i, (x0, y0, x1, y1) in enumerate(lanes):
        px = (x1 - x0 + 1) * (y1 - y0 + 1)
        ck(px <= 1024, "(c) lane %d <= ORB_RING_LANE_MAX_PX: %d px" % (i, px))
    ann = set()
    for ry in range(466):
        for rx in range(466):
            u = 2 * rx - 465
            v = 2 * ry - 465
            if 200704 <= u * u + v * v <= 213444:
                ann.add((ry, rx))
    ck(len(ann) == 10004, "(ref) the annulus has 10,004 pixels: %d" % len(ann))
    covered = set()
    for (x0, y0, x1, y1) in lanes:
        for ry in range(y0, y1 + 1):
            for rx in range(x0, x1 + 1):
                covered.add((ry, rx))
    missing = ann - covered
    ck(not missing, "(d) lanes cover every annulus pixel (missing: %d)" % len(missing))
    slack = covered - ann
    sent = sum((x1 - x0 + 1) * (y1 - y0 + 1) for (x0, y0, x1, y1) in lanes)
    extra = sent - len(ann)
    ck(sent <= len(ann) + n_lanes * 384,
       "(d) sent (%d) <= annulus (%d) + lanes (%d) * 384 = %d" % (sent, len(ann), n_lanes, len(ann) + n_lanes * 384))
    from collections import Counter
    c2 = Counter()
    for (x0, y0, x1, y1) in lanes:
        for ry in range(y0, y1 + 1):
            for rx in range(x0, x1 + 1):
                c2[(ry, rx)] += 1
    dups_ann = sum(1 for k, v in c2.items() if v > 1 and k in ann)
    ck(dups_ann == 0, "(e) no annulus pixel claimed by two lanes: %d" % dups_ann)
    for i in range(1, len(lanes)):
        ck(lanes[i][1] >= lanes[i - 1][1], "(f) lane %d y0 >= previous: %d >= %d" % (i, lanes[i][1], lanes[i - 1][1]))
    ck(n_lanes == 60, "(g) shipped total: 60 lanes")
    ck(sent == 19912, "(g) shipped total: 19912 px sent")
    ck(extra == 9908, "(g) shipped total: 9908 extra px")
    ck(slack == set(k for k in covered if k not in ann), "(d) slack pixels are those outside the annulus")

    lif_hdr = [ln for ln in lane_out if ln.startswith("LIF ")]
    n_full = int(lif_hdr[0].split()[1]) if lif_hdr else 0
    ck(n_full == n_lanes, "(i) ring_lanes_in full range returns the full cover: %d" % n_full)
    full_lanes = []
    for ln in lane_out:
        if ln.startswith("F "):
            p = ln.split()
            full_lanes.append((int(p[1]), int(p[2]), int(p[3]), int(p[4])))
    ck(full_lanes == lanes, "(i) ring_lanes_in full range matches ring_lanes in order")

    li1_hdr = [ln for ln in lane_out if ln.startswith("LI1 ")]
    n_140 = int(li1_hdr[0].split()[1]) if li1_hdr else 0
    li1_lanes = []
    for ln in lane_out:
        if ln.startswith("I "):
            p = ln.split()
            li1_lanes.append((int(p[1]), int(p[2]), int(p[3]), int(p[4])))
    li1_px = sum((l[2] - l[0] + 1) * (l[3] - l[1] + 1) for l in li1_lanes)
    ck(n_140 == 6 and li1_px == 1856,
       "(i) ring_lanes_in 100..140: 6 lanes, 1856 px (got %d, %d)" % (n_140, li1_px))

    liob = [ln for ln in lane_out if ln.startswith("LIOB ")]
    ck(int(liob[0].split()[1]) == 0, "(i) ring_lanes_in out-of-band (0..1) returns 0")
    linv = [ln for ln in lane_out if ln.startswith("LINV ")]
    ck(int(linv[0].split()[1]) == 0, "(i) ring_lanes_in y1 < y0 returns 0")
    lbdy = [ln for ln in lane_out if ln.startswith("LBDY ")]
    ck(int(lbdy[0].split()[1]) == 1, "(i) ring_lanes_in 9..9 returns 1 (first lane y 2..9 touches it)")
    det = [ln for ln in lane_out if ln.startswith("DETER ")]
    ck(int(det[0].split()[1]) == 1, "(det) two calls to ring_lanes give the same array")

    edge = run(exe, "lnedge").splitlines()
    ed = {ln.split()[0]: ln.split()[1] for ln in edge}
    ck(ed.get("LNE0") == "0", "(h) ring_lanes(NULL, 0) returns 0")
    ck(ed.get("LNE1") == "0", "(h) ring_lanes(NULL, max) returns 0")
    ck(ed.get("LNE2") == "0", "(h) ring_lanes(la, 0) returns 0")
    ck(ed.get("LNE3") == "0", "(h) ring_lanes(la, -1) returns 0")
    ck(int(ed.get("LNE4", "0")) == 0, "(h) max one less than needed returns 0")
    ck(ed.get("LNE5") == "60", "(h) ring_lanes full max returns 60")
    ck(ed.get("LNE6") == "60", "(h) ring_lanes exact max returns 60")
    ck(ed.get("LNE7") == "0", "(h) ring_lanes_in NULL lanes returns 0")
    ck(ed.get("LNE8") == "0", "(h) ring_lanes_in n=0 returns 0")
    ck(ed.get("LNE9") == "0", "(h) ring_lanes_in max=0 returns 0")

    ck(n_lanes == 0 or sent <= len(ann) + n_lanes * 384,
       "(inv) sent (%d) <= annulus + lanes * LANE_EXTRA_MAX (%d)" % (sent, len(ann) + n_lanes * 384))
    widest = max(((x1 - x0 + 1) * (y1 - y0 + 1) for (x0, y0, x1, y1) in lanes), default=0)
    ck(widest <= 1024, "(inv) widest lane %d <= ORB_RING_LANE_MAX_PX 1024" % widest)

    # ---- rim-3: the clock the lanes afford — the constants, the gate, the governor -----------
    # §6.1's numbers, as the compiled translation unit sees them. Everything else in this slice
    # (the grid the service keeps, the pending set it records, the batch the commit sends) is IDF
    # glue an idf.py build owns; these three lines are the whole decision it makes, and they are
    # here so the glue cannot disagree with them.
    c = run(exe, "consts").split()
    ck(c[:2] == ["CONST", "40"], "the grid is 25 Hz: ORB_RING_STEP_MS is %s, not the base build's 500" % c[1])
    ck(c[2] == "1500" and c[3] == "400", "the floors are design numbers now: breathe %s, flash %s" % (c[2], c[3]))
    ck(c[4] == "2", "a steady move re-sends at 2 per mille, the drain's own granularity: %s" % c[4])
    ck(c[5] == "8", "the alpha gate is 8 units: %s" % c[5])
    ck(c[6] == "400", "the governor's ceiling is the grid the slice started from: %s" % c[6])
    ck(c[7] == "4000", "an app that names no period still gets a slow breath: %s" % c[7])

    def gate(w, n):
        return int(run(exe, "gate", w, n).split()[1])
    ck(gate(0, 255) == 1 and gate(255, 0) == 1,
       "the gate never holds a band going out or coming back: 0->255 %d, 255->0 %d" % (gate(0, 255), gate(255, 0)))
    ck(gate(100, 104) == 0, "four units of breathe is the same pixel on the glass: gated off")
    ck(gate(100, 108) == 1 and gate(100, 92) == 1, "eight units either way is not: ORB_RING_STEP_ALPHA is the whole rule")
    ck(gate(0, 0) == 1, "alpha 0 on both words still answers the glass: the lanes go and are the app, untouched (§11)")
    ck(gate(255, 255) == 0, "a steady effect at full alpha re-sends nothing, however fast the grid runs")

    def gov(us):
        return int(run(exe, "gov", us).split()[1])
    ck([gov(x) for x in (0, 5000, 13000)] == [40, 40, 40],
       "a batch of 13 ms or less keeps the full 25 Hz: %s" % [gov(x) for x in (0, 5000, 13000)])
    ck(gov(30000) == 90 and gov(90000) == 270,
       "a slower board gives up its rate three times over: 30 ms -> %d, 90 ms -> %d" % (gov(30000), gov(90000)))
    ck(gov(200000) == 400, "no batch, however starved, pushes past the grid the base build ran: %d" % gov(200000))
    ck(gov(-7) == 40, "a failed measurement reads as a free one, not as a stopped clock: %d" % gov(-7))
    absurd = [gov(b) for b in (1, 7, 12345, 99999, 400000, 1 << 20, (1 << 31) - 1)]
    ck(all(40 <= g <= 400 for g in absurd),
       "the grid stays between its two bounds over absurd batch times, a garbage 2^31-1 included: %s" % absurd)

    # ---- alerts-1: the backed ring (§64.12) ------------------------------------------------
    def bk565(arc, argb):
        w = run(exe, "bk565", arc, hex(argb)).split()
        return int(w[1], 16) if len(w) > 1 else -1
    def bkpa(flags, eff, ea):
        return int(run(exe, "bkpa", flags, eff, ea).split()[1])
    def bkmo(w, n):
        return int(run(exe, "bkmo", w, n).split()[1])
    def pulse(until, now):
        return int(run(exe, "pulse", until, now).split()[1])

    # the band's black is a real pixel (a paint), and the arc is the colour blended against black
    ck(bk565(0, 0xFF0FA36B) == 0x0000 and bk565(0, 0xFFFFFFFF) == 0x0000 and bk565(0, 0x00FF0000) == 0x0000,
       "a backed non-arc pixel is pure black whatever the colour it was handed (the band is painted, not skipped)")
    ck(all(bk565(1, c) == exp_blend(0x0000, c) for c in
           (0xFF0FA36B, 0xFFFFFFFF, 0xFFD62828, 0x600FA36B, 0x00123456)),
       "a backed arc pixel is argb blended over black, not over the destination")
    ck(bk565(1, 0xFF0FA36B) == wire(0x150D), "a full-alpha backed arc is the source colour on the wire: %04X" % bk565(1, 0xFF0FA36B))
    ck(bk565(1, 0x600FA36B) == exp_blend(0x0000, 0x600FA36B) != bk565(1, 0xFF0FA36B),
       "a breathe trough's 96/255 is the colour dimmed toward black, not the full colour and not the app beneath")

    # the paint alpha: unbacked is today's effect byte for byte; backed throws the byte away
    ck(bkpa(0x00, 0, 255) == 255 and bkpa(0x00, 1, 0) == 0 and bkpa(0x00, 1, 255) == 255 and bkpa(0x00, 2, 30) == 30,
       "an unbacked record's paint alpha is the effect's own alpha, unchanged")
    ck(bkpa(0x04, 0, 123) == 255, "a backed solid band paints at 255, the alpha byte ignored")
    ck(bkpa(0x04, 1, 0) == 0 and bkpa(0x04, 1, 255) == 255,
       "a backed flash keeps its phase: lit at 255, black band at 0 — both are a paint")
    ck(bkpa(0x04, 2, 0) == 96 and bkpa(0x04, 2, 30) == 96 and bkpa(0x04, 2, 96) == 96 and bkpa(0x04, 2, 200) == 200,
       "a backed breathe never dips under the floor: 96, not the design's old 64")

    # the gate for a band whose off-phase is black: the movement rule, not the never-zero rule
    ck(bkmo(255, 0) == 1 and bkmo(0, 255) == 1 and bkmo(96, 120) == 1,
       "a backed flash transition (colour<->black) and a moving breathe both re-send")
    ck(bkmo(0, 0) == 0 and bkmo(255, 255) == 0 and bkmo(96, 96) == 0,
       "two identical backed frames rest — the black band is not re-sent against the rule that paint ignores alpha")

    # the lent-press pulse window (§64.13): half-open [arm, until), so the record's normal
    # gate resumes at the exact expiry and never a frame late.
    ck(pulse(1000, 999) == 1 and pulse(1000, 0) == 1,
       "a pulse armed for t=1000 is open the whole step before its expiry")
    ck(pulse(1000, 1000) == 0 and pulse(1000, 1001) == 0,
       "the pulse restores at the exact expiry and after it — half-open, never a frame into the next beat")
    ck(pulse(0, 0) == 0, "no window armed (until 0) is never open at the clock's origin")

    # the whole band the backed stamp paints, for the fraction the pomodoro alerts at
    def bkcover(frac, st, cc):
        d = dict(zip(run(exe, "bkcover", frac, st, cc).split()[1::2],
                     run(exe, "bkcover", frac, st, cc).split()[2::2]))
        return {k: int(v) for k, v in d.items()}
    for fr in (0, 1, 125, 500, 999, 1000):
        c = bkcover(fr, 0, 0)
        ck(c["ann"] == 10004 and c["covin"] == 10004 and c["covout"] == 0 and c["arc"] + c["trk"] == 10004,
           "a backed band paints every annulus pixel and nothing outside it at fraction %d (cost: no extra pixels)" % fr)
    c0 = bkcover(0, 0, 0); c1 = bkcover(1000, 0, 0); c125 = bkcover(125, 0, 0)
    ck(c0["arc"] == 0 and c0["trk"] == 10004, "fraction 0 backs a whole band of black (all track): the off-phase band")
    ck(c1["arc"] == 10004 and c1["trk"] == 0, "fraction 1000 makes the whole band the arc")
    ck(c125["arc"] > 0 and c125["trk"] > 0,
       "a quarter arc on a black band: both the arc and the black track are painted (got %d/%d)" % (c125["arc"], c125["trk"]))

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
