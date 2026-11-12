/* The OS ring's pure core — contract in orb_ring.h (plan §64.1, ring-design §4..§6).
 *
 * Geometry here is the copy arc's (orb_edge_arc.c) restated so a row comes back as at
 * most four x-pieces instead of a trig test per pixel: the two radii are compared in
 * exact integer arithmetic (four times squared, so a pixel centre's half offset is an
 * odd integer and no rounding can put a pixel on the wrong side of a radius), and the
 * arc/track edge is the record's wedge, cut by two half-plane tests per row solved for x. The
 * stamp that consumes
 * this does only the blend per pixel; tools/host_ring_test.py checks these pieces
 * against a per-pixel reference for every row, a sweep of fractions and the start/direction
 * matrix of rim-design §3. */
#include "orb_ring.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define GLASS    466                    /* the glass is 466 x 466 */
#define CC         233                  /* CX = CY, as the copy arc's */
#define KPC      232.5                  /* a pixel centre is at coord - KPC from CX */
#define R_OUT_I    231                  /* a pixel in from the glass's rim */
#define R_IN_I     224                  /* 7 px: theme_strip's ring at the glass's size */
#define RIN4  ((int64_t)4 * R_IN_I * R_IN_I)   /* the same radii, four times squared */
#define ROT4  ((int64_t)4 * R_OUT_I * R_OUT_I)
#define TAU      6.2831853071795864769  /* math.h's M_PI is not C11; the ring names its own */

/* wire order in and out, exactly as the copy arc's wire(): the tile holds byte-swapped
 * RGB565, which its 0x66DF arc over its 0x3186 track already proves. */
static inline uint16_t wire(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
static inline unsigned x5(unsigned c) { return (c << 3) | (c >> 2); }
static inline unsigned x6(unsigned c) { return (c << 2) | (c >> 4); }
static inline unsigned q5(unsigned v) { return (v * 31u + 127u) / 255u; }
static inline unsigned q6(unsigned v) { return (v * 63u + 127u) / 255u; }

uint16_t ring_blend565(uint16_t bg565_swapped, uint32_t argb)
{
    const int a = (int)(argb >> 24);
    if (a == 0) return bg565_swapped;                       /* alpha 0 draws nothing */
    const uint16_t bg = wire(bg565_swapped);
    const int br = (int)x5(bg >> 11), bgv = (int)x6((bg >> 5) & 0x3Fu), bb = (int)x5(bg & 0x1Fu);
    const int sr = (int)((argb >> 16) & 0xFFu), sg = (int)((argb >> 8) & 0xFFu), sb = (int)(argb & 0xFFu);
    const int dr = br + (sr - br) * a / 255;
    const int dg = bgv + (sg - bgv) * a / 255;
    const int db = bb + (sb - bb) * a / 255;
    const uint16_t packed = (uint16_t)((q5((unsigned)dr) << 11) | (q6((unsigned)dg) << 5) | q5((unsigned)db));
    return wire(packed);
}

void ring_rows_for(int fraction, int *y0, int *y1)
{
    (void)fraction;                       /* the rows the ring may touch, not the arc's extent */
    if (y0) *y0 = CC - R_OUT_I;
    if (y1) *y1 = CC + R_OUT_I;
}

/* four times the squared distance from the glass centre to pixel (y, x)'s centre */
static int64_t pd2(int y, int x)
{
    const int64_t u = 2 * (int64_t)x - (GLASS - 1), v = 2 * (int64_t)y - (GLASS - 1);
    return u * u + v * v;
}
/* inner = 1 asks "inside the hole", inner = 0 "inside the outer circle". Both are runs
 * through the row's middle, so either edge is a bisection away and exact. */
static int p_hit(int y, int x, int inner)
{
    const int64_t d = pd2(y, x);
    return inner ? d < RIN4 : d <= ROT4;
}
static int e_first(int y, int a, int b, int inner)
{
    while (a < b) { const int m = a + (b - a) / 2; if (p_hit(y, m, inner)) b = m; else a = m + 1; }
    return a;
}
static int e_last(int y, int a, int b, int inner)
{
    while (a < b) { const int m = a + (b - a + 1) / 2; if (p_hit(y, m, inner)) a = m; else b = m - 1; }
    return a;
}

/* the ray's crossing of a row's line, as a column: -1 below the glass, GLASS above it */
static int fl(double b) { return b < -1.0 ? -1 : (b > (double)GLASS ? GLASS : (int)floor(b)); }
static int ce(double b) { return b < -1.0 ? -1 : (b > (double)GLASS ? GLASS : (int)ceil(b)); }
static int clip_x(int e) { return e > GLASS - 1 ? GLASS - 1 : e; }   /* the last column, not one past it */

/* One side of a ray, as the closed column interval of the row's line that lies on it. The ray at
 * angle a clockwise from twelve leaves the centre towards (sin a, -cos a), and a pixel's centre is
 * clockwise of it when cs*(x - KPC) - sn*v > 0 with v the row's height above the centre, so the
 * side is a prefix or a suffix of the row and the crossing column is b = KPC + sn*v/cs. A
 * horizontal ray (|cs| < eps) crosses no row's line and the whole row is on one side of it, which
 * is the sign test the shipped twelve-ray algebra already used. `cw` picks which side to return;
 * an empty side is the interval [0, -1]. */
static void ray_side(int y, double sn, double cs, int cw, int iv[2])
{
    const double v = (double)CC - 0.5 - (double)y;
    if (cs > -1e-12 && cs < 1e-12) {                       /* horizontal: it crosses no row line */
        iv[0] = 0;
        iv[1] = ((-sn * v > 0.0) == (cw != 0)) ? GLASS - 1 : -1;
        return;
    }
    const double b = KPC + sn * v / cs;
    if ((cs > 0.0) == (cw != 0)) { iv[0] = ce(b); iv[1] = GLASS - 1; }   /* that side is to its right */
    else                         { iv[0] = 0;     iv[1] = clip_x(fl(b)); }
}

/* Two column intervals, clamped back onto the glass: 1 when the overlap names columns. */
static int isect(const int a[2], const int b[2], int o[2])
{
    o[0] = a[0] > b[0] ? a[0] : b[0];
    o[1] = a[1] < b[1] ? a[1] : b[1];
    if (o[0] < 0) o[0] = 0;
    if (o[1] > GLASS - 1) o[1] = GLASS - 1;
    return o[0] <= o[1];
}

/* The arc's columns on row y, as at most two closed runs in increasing x. The arc is a WEDGE, not
 * a prefix (rim-design §3): it runs clockwise from a0 to a1, which is [start, start + sweep] for a
 * clockwise record and [start - sweep, start] for an anticlockwise one, so both directions are one
 * cyclic interval and a row solves it as one interval cut by two rays — the monotone-angle lemma's
 * two crossings, which is why a row still needs no more than the four pieces the caller sized.
 * Under half a circle the wedge is the intersection of two half-planes and so one run; over half
 * a circle its complement (the track) is the intersection and the arc is what that leaves, which
 * is how the shipped twelve-prefixed version was solved: at start 0 clockwise the from-ray IS
 * twelve, and these lines are that arithmetic. */
static int wedge_runs(int y, int start_deg, int fraction, int ccw, int r0[2], int r1[2])
{
    const double s = (double)fraction * TAU / 1000.0;              /* the sweep, radians */
    const double a = (double)start_deg * TAU / 360.0;              /* the start, radians */
    double a0 = ccw ? a - s : a;
    double a1 = ccw ? a : a + s;
    int x[2], z[2], t[2];
    if (fraction < 500) {                                  /* the wedge is the convex side */
        ray_side(y, sin(a0), cos(a0), 1, x);                /* clockwise of the from-ray  */
        ray_side(y, sin(a1), cos(a1), 0, z);                /* anticlockwise of the to-ray */
        if (!isect(x, z, t)) return 0;
        r0[0] = t[0]; r1[0] = t[1];
        return 1;
    }
    ray_side(y, sin(a0), cos(a0), 0, x);                    /* the track is outside the wedge */
    ray_side(y, sin(a1), cos(a1), 1, z);
    if (!isect(x, z, t)) { r0[0] = 0; r1[0] = GLASS - 1; return 1; }
    if (t[0] > 0) {
        r0[0] = 0; r1[0] = t[0] - 1; r0[1] = t[1] + 1; r1[1] = GLASS - 1;
        return t[1] + 1 <= GLASS - 1 ? 2 : 1;
    }
    r0[0] = t[1] + 1; r1[0] = GLASS - 1;
    return r0[0] <= r1[0] ? 1 : 0;
}

/* append one piece, joining on to the previous one when they touch and agree */
static int emit(orb_ring_part_t *p, int n, int max, int x0, int x1, int arc)
{
    if (x0 <= x1 && n && p[n - 1].arc == (uint8_t)(arc ? 1 : 0) && p[n - 1].x1 == (int16_t)(x0 - 1)) {
        p[n - 1].x1 = (int16_t)x1;
        return n;
    }
    if (x0 > x1 || n >= max) return n;
    p[n].x0 = (int16_t)x0; p[n].x1 = (int16_t)x1; p[n].arc = (uint8_t)(arc ? 1 : 0); p[n]._pad = 0;
    return n + 1;
}

int ring_row_parts(int y, int fraction, int start_deg, int ccw, orb_ring_part_t *parts, int max)
{
    if (!parts || max < 1 || y < 0 || y >= GLASS) return 0;
    if (fraction < 0) fraction = 0;                  /* the same clamps the record takes */
    if (fraction > 1000) fraction = 1000;
    start_deg %= 360;                                /* the wedge reads its start mod 360 */
    if (start_deg < 0) start_deg += 360;
    if (!p_hit(y, CC - 1, 0)) return 0;                       /* the row misses the annulus */
    const int lo = e_first(y, 0, CC - 1, 0), hi = e_last(y, CC, GLASS - 1, 0);
    int spans[2][2], ns = 1, a0[2], a1[2], na;
    if (p_hit(y, CC - 1, 1)) {                                /* the row passes the inner circle */
        const int hl = e_first(y, 0, CC - 1, 1), hr = e_last(y, CC, GLASS - 1, 1);
        spans[0][0] = lo; spans[0][1] = hl - 1; spans[1][0] = hr + 1; spans[1][1] = hi;
        ns = 2;
    } else {
        spans[0][0] = lo; spans[0][1] = hi;
    }
    if (fraction <= 0) {                              /* the two degenerate asks, before the trig */
        na = 0;                                       /* nothing is arc: the annulus is all track */
    } else if (fraction >= 1000) {
        na = 1; a0[0] = 0; a1[0] = GLASS - 1;         /* the whole annulus is arc, whatever the start */
    } else {
        na = wedge_runs(y, start_deg, fraction, ccw ? 1 : 0, a0, a1);
    }
    int n = 0;
    for (int k = 0; k < ns && n < max; k++) {
        int x = spans[k][0];
        for (int i = 0; i < na; i++) {
            const int ax0 = a0[i] > x ? a0[i] : x, ax1 = a1[i] < spans[k][1] ? a1[i] : spans[k][1];
            if (ax0 > ax1) continue;
            n = emit(parts, n, max, x, ax0 - 1, 0);
            n = emit(parts, n, max, ax0, ax1, 1);
            x = ax1 + 1;
        }
        n = emit(parts, n, max, x, spans[k][1], 0);
    }
    return n;
}

/* The period a record really runs on: the OS default for 0, then up to the effect's floor.
 * ring_clamp and the clock both use it, so a raw record can never rush the rim. */
static uint16_t period_of(const orb_ring_rec *r)
{
    uint32_t p = r->period_ms ? (uint32_t)r->period_ms : ORB_RING_DEFAULT_PERIOD_MS;
    const uint32_t low = r->effect == ORB_RING_BREATHE ? ORB_RING_BREATHE_MIN_MS
                       : r->effect == ORB_RING_FLASH    ? ORB_RING_FLASH_MIN_MS : 0u;
    if (p < low) p = low;
    return (uint16_t)p;                              /* both bounds fit a uint16 by construction */
}

void ring_clamp(orb_ring_rec *r)
{
    if (!r) return;
    if (r->fraction > 1000) r->fraction = 1000;               /* uint16: the floor at 0 is free */
    if (r->effect != ORB_RING_FLASH && r->effect != ORB_RING_BREATHE) r->effect = ORB_RING_STEADY;
    r->period_ms = period_of(r);
    r->flags &= ORB_RING_FLAGS_MASK;                    /* an unknown bit is dropped, not honoured */
    r->start = (uint16_t)(r->start % 360);              /* the start wraps; a uint16 cannot go below 0 */
}

int ring_effect_alpha(const orb_ring_rec *r, int64_t now_ms)
{
    if (!r) return 0;
    if (r->effect == ORB_RING_FLASH) {
        const int64_t p = period_of(r);
        return ((now_ms / (p / 2 > 0 ? p / 2 : 1)) & 1) ? 0 : 255;   /* lit for half the period, then glass */
    }
    if (r->effect == ORB_RING_BREATHE) {
        const int64_t p = period_of(r);
        int64_t t = now_ms % p;
        if (t < 0) t += p;
        const float ph = (float)t / (float)p;
        const int a = (int)(127.5f * (1.0f - cosf((float)TAU * ph)) + 0.5f);
        return a < 0 ? 0 : (a > 255 ? 255 : a);               /* slow in and out, never a step */
    }
    return 255;                                               /* steady, and anything unknown */
}

/* The gate on an effect step (rim-design §6.1). A 25 Hz grid with no gate would re-send the same
 * breath at full alpha twice before it moved a single 565 step, which is the whole cost of the
 * slice bought for nothing; 8 units is the widest jump a 1.5 s breathe makes between two grid
 * steps, so the floor is still smooth. Both ends are never gated: a step that landed on 0 sends
 * (the glass must see the band go away) and the step that leaves 0 sends (the first frame back),
 * which is also what makes an off-phase flash answer the mirror rather than nothing. */
int ring_alpha_moved(int was_alpha, int now_alpha)
{
    if (was_alpha <= 0 || now_alpha <= 0) return 1;                /* out and back: always sent */
    const int d = now_alpha > was_alpha ? now_alpha - was_alpha : was_alpha - now_alpha;
    return d >= ORB_RING_STEP_ALPHA;
}

/* A backed band's pixel (§64.12, alerts-design §2.2). The whole rim is painted: a non-arc pixel is
 * pure black, an arc pixel is the caller's argb blended against black (never the destination), so
 * the colour's alpha here is the effect's own — a solid's 255 returns the source, a breathe trough's
 * floor returns the colour dimmed to black. ring_blend565() over a 0x0000 background is exactly that
 * blend, in the wire order the tile already holds. */
uint16_t ring_backed565(uint32_t argb, int arc)
{
    if (!arc) return 0x0000;                              /* wire(0) == 0: the band's black, byte for byte */
    return ring_blend565(0x0000, argb);
}

/* The alpha the band paints at, for both the backed colour and the send gate (§64.12). Unbacked is
 * the effect's alpha unchanged (ring_effect_alpha()'s own answer: steady 255, flash 255/0, breathe
 * its ramp), so today's paths are byte for byte. Backed throws the opacity byte away: a steady (or
 * unknown) band is solid at 255, a breathe is floored so the trough never reveals the app beneath,
 * and a flash keeps its phase — but the off-phase is a black band, not nothing, which is what
 * ring_backed_moved() below decides on, not ring_alpha_moved(). */
int ring_paint_alpha(const orb_ring_rec *r, int effect_alpha)
{
    if (!r) return effect_alpha;
    if (!(r->flags & ORB_RING_BACKED)) return effect_alpha;
    if (r->effect == ORB_RING_FLASH)   return effect_alpha;                 /* 255 lit, 0 black — both paint */
    if (r->effect == ORB_RING_BREATHE)
        return effect_alpha < ORB_RING_BACKED_MIN_ALPHA ? ORB_RING_BACKED_MIN_ALPHA : effect_alpha;
    return 255;                                                             /* backed solid: full, byte ignored */
}

/* The gate for a band whose off-phase is black, not nothing (§64.12, alerts-design §2.3). A fully
 * black and a fully coloured flash both paint, so the old never-zero clause would resend an
 * unchanged black band and swallow the transition home; the backed rule is simply that the paint
 * moved. Two black frames rest, two coloured frames rest, colour-to-black and back both send. */
int ring_backed_moved(int was_paint, int now_paint)
{
    return was_paint != now_paint;
}

/* The lent-press pulse window (§64.13), pure so the timer has a host test and the glue is
 * a read. Half-open: an armed stamp `until_ms` pulses for every clock strictly before it and
 * restores at the exact expiry, so a pulse never runs a frame into the record's normal gate. */
int ring_pulse_open(int64_t until_ms, int64_t now_ms)
{
    return now_ms < until_ms;
}

/* The governor (rim-design §6.3): the batch's own length times three, floored at the grid the
 * slice asks for and ceiled at the grid the slice started from. The floor of the clamp is the
 * same constant as the ceiling's partner, so a batch nobody measured still ticks at 25 Hz; the
 * ceiling is what a starved bus gets, i.e. today's cadence with today's pixels already gone.
 * The multiply is bounded first so a garbage `batch_us` cannot overflow into a small grid. */
int ring_next_step_ms(int batch_us)
{
    if (batch_us <= 0) return ORB_RING_STEP_MS;
    if (batch_us > ORB_RING_STEP_MAX_MS * 1000) batch_us = ORB_RING_STEP_MAX_MS * 1000;   /* the x3 below must not overflow */
    int g = (batch_us * 3) / 1000;
    if (g < ORB_RING_STEP_MS)     g = ORB_RING_STEP_MS;
    if (g > ORB_RING_STEP_MAX_MS) g = ORB_RING_STEP_MAX_MS;
    return g;
}

/* overlay-design §6: the substitution, kept pure so it has a host test and the glue stays three
 * lines. Only a 0x01RRGGBB word is a colour (tag 0x00 absent, 0x02+ unassigned read as none, so a
 * later meaning is never mistaken for one); a record that did not ask — its ORB_RING_MATCH bit
 * clear, whatever its other flags — and an absent theme colour are byte-for-byte no-ops. The
 * alphas ride the top byte and are preserved untouched. */
void ring_apply_overlay(orb_ring_rec *r, uint32_t overlay)
{
    if (!r || !(r->flags & ORB_RING_MATCH)) return;                /* not asked: unchanged */
    if ((overlay & 0xFF000000u) != 0x01000000u) return;       /* absent or unknown tag: unchanged */
    const uint32_t rgb = overlay & 0x00FFFFFFu;
    r->arc   = (r->arc   & 0xFF000000u) | rgb;                /* keep each alpha, take the RGB */
    r->track = (r->track & 0xFF000000u) | rgb;
}

void ring_init(orb_ring_rec *tbl, int *owner, int n)
{
    if (!owner || n < 1) return;
    for (int i = 0; i < n; i++) {
        owner[i] = -1;                                        /* nothing owned, so nothing on the glass */
        if (tbl) tbl[i] = (orb_ring_rec){ 0 };
    }
}

void ring_set(orb_ring_rec *tbl, int *owner, int64_t *seq, int n, int slot,
              const orb_ring_rec *r, int64_t now_us)
{
    if (!tbl || !owner || !seq || !r || n < 1 || slot < 0 || slot >= n) return;
    orb_ring_rec c = *r;
    ring_clamp(&c);
    tbl[slot] = c;
    owner[slot] = slot;
    seq[slot] = now_us / 1000;                     /* a ms stamp: two calls in one ms still order */
}

void ring_clear(orb_ring_rec *tbl, int *owner, int n, int slot)
{
    (void)tbl;
    if (!owner || n < 1 || slot < 0 || slot >= n) return;
    owner[slot] = -1;                              /* the next most recent surfaces on the next pick */
}

int ring_pick(const orb_ring_rec *tbl, const int *owner, const int64_t *seq, int n, orb_ring_rec *out)
{
    if (!tbl || !owner || !seq || !out || n < 1) return 0;
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (owner[i] < 0) continue;
        if (best < 0 || seq[i] > seq[best] || (seq[i] == seq[best] && i > best)) best = i;
    }
    if (best < 0) return 0;
    *out = tbl[best];
    ring_clamp(out);
    return 1;
}

/* =====================================================================================
 * RIM LANES (rim-design §4). The annulus as ≤64 even-aligned rectangles: the cover a
 * ring-only refresh must send, so it does not re-pay the whole-row tax. The greedy is
 * pure and deterministic — the same constants give the same cover everywhere — and the
 * only geometry it uses is the same four-times-squared integer test ring_row_parts
 * uses (no second source of truth, no float in the pixel arithmetic).
 * ===================================================================================== */

/* The annulus spans of one row: 1 span (cap/middle) or 2 spans (left slab, right slab). */
#define LANE_ROW_LO 2
#define LANE_ROW_HI 463

static int row_ann_spans(int y, int sp[2][2])
{
    const int64_t u = 2 * (int64_t)(CC - 1) - (GLASS - 1), v = 2 * (int64_t)y - (GLASS - 1);
    const int64_t d = u * u + v * v;
    if (d > ROT4) return 0;
    const int lo = e_first(y, 0, CC - 1, 0), hi = e_last(y, CC, GLASS - 1, 0);
    if (d < RIN4) {
        const int hl = e_first(y, 0, CC - 1, 1), hr = e_last(y, CC, GLASS - 1, 1);
        sp[0][0] = lo; sp[0][1] = hl - 1; sp[1][0] = hr + 1; sp[1][1] = hi;
        return 2;
    }
    sp[0][0] = lo; sp[0][1] = hi;
    return 1;
}

/* Align a raw span [a, b] to the panel's even-column rule and clip to glass. */
static void lane_align(int a, int b, int16_t *x0, int16_t *x1)
{
    int lo = a & ~1;
    int hi = b;
    if (((hi - lo + 1) & 1) != 0) hi++;
    if (lo < 0) lo = 0;
    if (hi > GLASS - 1) hi = GLASS - 1;
    *x0 = (int16_t)lo; *x1 = (int16_t)hi;
}

/* Count the annulus pixels in rows [y, y+b). */
static int band_ann_count(int y, int b)
{
    int c = 0;
    for (int r = 0; r < b; r++) {
        int sp[2][2];
        const int ns = row_ann_spans(y + r, sp);
        for (int s = 0; s < ns; s++) c += sp[s][1] - sp[s][0] + 1;
    }
    return c;
}

/* Emit one lane's spans for a band of rows [y, y+b). Returns the number of lanes written (1 or 2). */
static int emit_band_lanes(orb_ring_lane_t *tmp, int base, int y, int b)
{
    int all_side = 1, lx0 = GLASS, lx1 = -1, rx0 = GLASS, rx1 = -1, mx0 = GLASS, mx1 = -1;
    for (int r = 0; r < b; r++) {
        int sp[2][2];
        const int ns = row_ann_spans(y + r, sp);
        if (ns == 0) continue;
        if (ns == 1) all_side = 0;
        for (int s = 0; s < ns; s++) {
            if (sp[s][0] < mx0) mx0 = sp[s][0];
            if (sp[s][1] > mx1) mx1 = sp[s][1];
        }
        if (ns == 2) {
            if (sp[0][0] < lx0) lx0 = sp[0][0];
            if (sp[0][1] > lx1) lx1 = sp[0][1];
            if (sp[1][0] < rx0) rx0 = sp[1][0];
            if (sp[1][1] > rx1) rx1 = sp[1][1];
        }
    }
    const int16_t ry0 = (int16_t)y, ry1 = (int16_t)(y + b - 1);
    if (all_side && lx1 >= lx0) {
        if (base + 2 > ORB_RING_LANES_MAX) return base;
        tmp[base].y0 = ry0; tmp[base].y1 = ry1;
        lane_align(lx0, lx1, &tmp[base].x0, &tmp[base].x1);
        base++;
        tmp[base].y0 = ry0; tmp[base].y1 = ry1;
        lane_align(rx0, rx1, &tmp[base].x0, &tmp[base].x1);
        base++;
    } else {
        if (base >= ORB_RING_LANES_MAX) return base;
        tmp[base].y0 = ry0; tmp[base].y1 = ry1;
        lane_align(mx0, mx1, &tmp[base].x0, &tmp[base].x1);
        base++;
    }
    return base;
}

int ring_lanes(orb_ring_lane_t *lanes, int max)
{
    if (!lanes || max <= 0) return 0;
    orb_ring_lane_t tmp[ORB_RING_LANES_MAX];
    int n = 0;
    int y = LANE_ROW_LO;
    while (y <= LANE_ROW_HI) {
        int hi_b = LANE_ROW_HI - y + 1;
        if (hi_b > ORB_RING_LANE_ROWS_MAX) hi_b = ORB_RING_LANE_ROWS_MAX;
        if (hi_b & 1) hi_b--;
        if (hi_b < 2) { hi_b = 2; if (y + 1 > LANE_ROW_HI) break; }
        int best_b = 0;
        for (int b = hi_b; b >= 2; b -= 2) {
            const int a = band_ann_count(y, b);
            orb_ring_lane_t t[2];
            const int nn = emit_band_lanes(t, 0, y, b);
            int area = 0;
            for (int i = 0; i < nn; i++)
                area += (t[i].x1 - t[i].x0 + 1) * (t[i].y1 - t[i].y0 + 1);
            if (area - a <= ORB_RING_LANE_EXTRA_MAX) { best_b = b; break; }
        }
        if (best_b == 0) best_b = 2;
        n = emit_band_lanes(tmp, n, y, best_b);
        if (n >= ORB_RING_LANES_MAX) break;
        y += best_b;
    }
    if (n > max) return 0;
    for (int i = 0; i < n; i++) lanes[i] = tmp[i];
    return n;
}

int ring_lanes_in(const orb_ring_lane_t *lanes, int n, int y0, int y1,
                  orb_ring_lane_t *out, int max)
{
    if (!lanes || !out || n <= 0 || max <= 0 || y1 < y0) return 0;
    orb_ring_lane_t tmp[ORB_RING_LANES_MAX];
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (lanes[i].y1 < y0 || lanes[i].y0 > y1) continue;
        if (c >= ORB_RING_LANES_MAX) return 0;
        tmp[c++] = lanes[i];
    }
    if (c > max) return 0;
    for (int i = 0; i < c; i++) out[i] = tmp[i];
    return c;
}

/* =====================================================================================
 * THE IDF GLUE (ring-design §3, §8; ring-2). Everything above is the pure, host-tested
 * core; this section is compiled only under ESP-IDF and is the ring's sole IDF half. It
 * mirrors orb_edge_arc_service/orb_edge_arc_stamp exactly (the arc's shape is the
 * contract): a change re-marks the rows it touched, one off-glass corner pixel is
 * invalidated so a still page carries the frame, and the blend runs only into the DMA
 * tiles as they leave, never into the mirror. The rim dispatch in display.c reaches the
 * stamp only when the copy arc is not shown (ring-design §8: the copy arc wins).
 * ===================================================================================== */
#ifdef ESP_PLATFORM

#include "esp_attr.h"        /* EXT_RAM_BSS_ATTR: the ownership table lives in PSRAM  */
#include "esp_log.h"         /* the one log line rim-design §6.3 promises             */
#include "esp_timer.h"       /* the service and stamp read the wall clock             */
#include "board_config.h"    /* SCREEN_HEIGHT: the full-band mark, the arc's own      */
#include "orb_app.h"         /* ORB_APP_CAP, orb_app_row, flash_slot: the owner slug  */
#include "orb_theme.h"       /* §64.9: orb_theme_overlay(), the live theme's colour    */
#include "orb_edge_arc.h"    /* §11: the copy arc wins the rim, so the ring asks it     */
#include "display.h"         /* display_mark_rows, display_tiles, the lane batch        */
#include "lvgl.h"            /* the corner invalidate, exactly as the arc does it     */

#include <string.h>

/* The ownership table (design §5, §7): PSRAM, sized by the hard install cap, keyed by the
 * app's flash slot (the same key slot_index() hands the wasm shim). The shim drives the
 * pure ring_set/ring_clear on these directly; a refusal, a revoke or an uninstall drops
 * the row through orb_ring_drop. */
EXT_RAM_BSS_ATTR orb_ring_rec orb_ring_tbl[ORB_APP_CAP];
EXT_RAM_BSS_ATTR int           orb_ring_owner[ORB_APP_CAP];
EXT_RAM_BSS_ATTR int64_t       orb_ring_seq[ORB_APP_CAP];

/* What the tiles are stamping right now, so a still steady ring re-marks nothing and an
 * animating effect re-marks the annulus each grid step (design §6). LVGL task only. */
static orb_ring_rec s_rec;          /* the record last stamped (a copy of the latest pick) */
static bool         s_have;         /* s_rec names an owned record the glass shows         */
static uint32_t     s_ovl;          /* the last overlay colour seen, so a theme swap repaints (§6) */
static int64_t      s_step_us;      /* the last grid tick (the governor's current grid)      */
static bool         s_init;

/* The ring's own log tag (§6.3's one line). A literal, not a `static const char *`: a
 * pointer would be four bytes of internal .data, and §7's promise is zero of those. */
#define RING_TAG "orb_ring"

/* ---- the lane batch (rim-design §5.2, §7) --------------------------------------
 * All of it PSRAM: this slice adds zero internal .bss and zero internal DMA
 * allocations, and the tile ring it sends through was funded by the flush. The
 * cover is built once, at init, and never recomputed — the pending set is an
 * index pair into it (rim-design §7's "two int16 row bounds" widened to the lane
 * indices they select, so no step re-runs the greedy and no step copies 60
 * rectangles). A pending set is only ever written and read on the LVGL task: the
 * service records one, the commit spends one. */
EXT_RAM_BSS_ATTR static orb_ring_lane_t s_lanes[ORB_RING_LANES_MAX];   /* the cover, in row order */
EXT_RAM_BSS_ATTR static orb_ring_lane_t s_sweep[ORB_RING_LANES_MAX];   /* ring_lanes_in's answer  */
EXT_RAM_BSS_ATTR static const orb_ring_lane_t *s_pend;   /* the pending set's array              */
EXT_RAM_BSS_ATTR static int16_t s_pend_i0, s_pend_i1;    /* its first and last lane (i1 < i0: none) */
EXT_RAM_BSS_ATTR static bool    s_lanes_due;             /* s_pend is owed to the next commit     */
EXT_RAM_BSS_ATTR static int     s_lane_n;                /* the cover, 0 = pinned to the row path */
EXT_RAM_BSS_ATTR static int     s_alpha_sent;            /* the effect alpha the glass last holds  */
EXT_RAM_BSS_ATTR static int     s_grid_ms;               /* the governor's current grid            */
EXT_RAM_BSS_ATTR static int     s_batch_n;               /* lane batches reported, for the log line */
EXT_RAM_BSS_ATTR static bool    s_old_log;               /* the degradation line, printed once     */

/* The lent-press confirmation pulse (§64.13): PSRAM only, one slot and one expiry stamp.
 * Armed by orb_ring_disp_pulse when the router lends a press, consumed by the service to hold
 * that claimant's own record at full paint for ORB_RING_PULSE_MS. */
EXT_RAM_BSS_ATTR static int     s_pulse_slot;            /* the pulsing claimant, or -1            */
EXT_RAM_BSS_ATTR static int64_t s_pulse_until_ms;        /* the window's open-until stamp          */

#define RING_CY   233.0f
#define RING_ROUT 231.0f
#define RING_RIN  224.0f

static const char *effect_name(uint8_t e)
{
    return e == ORB_RING_FLASH ? "flash" : e == ORB_RING_BREATHE ? "breathe" : "steady";
}

/* The winner's flash slot, from the same newest-by-seq rule ring_pick uses (a tie breaks
 * to the higher slot), so /status can name the owning app. -1 when the table owns none. */
static int pick_slot(void)
{
    int best = -1;
    for (int i = 0; i < ORB_APP_CAP; i++) {
        if (orb_ring_owner[i] < 0) continue;
        if (best < 0 || orb_ring_seq[i] > orb_ring_seq[best] ||
            (orb_ring_seq[i] == orb_ring_seq[best] && i > best)) best = i;
    }
    return best;
}

static const char *slug_of_slot(int slot)
{
    if (slot < 0) return NULL;
    for (size_t i = 0; i < orb_app_rows(); i++) {
        const orb_app_t *a = orb_app_row(i);
        if (a && a->flash_slot == slot && a->kind == ORB_APP_KIND_APP && a->installed)
            return a->slug;
    }
    return NULL;
}

void orb_ring_disp_init(void)
{
    ring_init(orb_ring_tbl, orb_ring_owner, ORB_APP_CAP);
    for (int i = 0; i < ORB_APP_CAP; i++) orb_ring_seq[i] = 0;
    s_rec = (orb_ring_rec){ 0 };
    s_have = false;
    s_ovl = 0;
    s_step_us = 0;

    /* rim-design §4.6/§7: the band's cover, once, into PSRAM, before any app can set a ring. An
     * app that never sets one still costs these 480 bytes and this one greedy, which is the same
     * shape the copy arc's geometry already pays. A cover the geometry refused to answer (0, or a
     * caller array shorter than the greedy needed) pins the ring to whole-row marks for the life
     * of the boot: a geometry failure costs the ring its shape, not the ring. */
    s_lane_n   = ring_lanes(s_lanes, ORB_RING_LANES_MAX);
    s_old_log  = false;
    s_pend     = s_lanes;
    s_pend_i0  = 0;
    s_pend_i1  = -1;
    s_lanes_due = false;
    s_alpha_sent = 0;
    s_batch_n   = 0;
    s_pulse_slot = -1;                                  /* no lent-press pulse armed (§64.13)      */
    s_pulse_until_ms = 0;
    s_grid_ms   = ORB_RING_STEP_MS;                    /* the governor starts at the ask (§6.3) */
    if (s_lane_n <= 0) {
        s_lane_n = 0;
        ESP_LOGW(RING_TAG, "rim lanes: ring_lanes() answered 0, the ring keeps whole-row marks");
    } else {
        int px = 0;
        for (int i = 0; i < s_lane_n; i++)
            px += (s_lanes[i].x1 - s_lanes[i].x0 + 1) * (s_lanes[i].y1 - s_lanes[i].y0 + 1);
        ESP_LOGI(RING_TAG, "rim lanes: %d windows, %d px cover, %d ms grid", s_lane_n, px, s_grid_ms);
    }
    s_init = true;
}

void orb_ring_drop(int slot)
{
    if (slot < 0 || slot >= ORB_APP_CAP) return;
    if (!s_init) orb_ring_disp_init();
    ring_clear(orb_ring_tbl, orb_ring_owner, ORB_APP_CAP, slot);
    if (s_pulse_slot == slot) s_pulse_slot = -1;   /* a dropped row cannot keep pulsing */
}

/* The OS's confirmation that it just spent a press on this claimant (§64.13): arm the
 * claimant's OWN record to paint full for ORB_RING_PULSE_MS. Called from the router's poll
 * (the LVGL task, the same task the service runs on) when a plain press is lent. No pulse is
 * drawn when there is no ring service yet, when the claimant owns no record, or when another
 * record wins the rim — the pulse is the claimant's ring and nothing else, so a claimant with
 * no ring (or one the copy arc is covering) simply gets none. */
void orb_ring_disp_pulse(int slot)
{
    if (!s_init) return;                                   /* the ring service is unavailable */
    if (slot < 0 || slot >= ORB_APP_CAP) return;
    if (orb_ring_owner[slot] < 0) return;                  /* the claimant owns no record: no pulse */
    if (pick_slot() != slot) return;                       /* another record owns the rim: no pulse */
    s_pulse_slot = slot;
    s_pulse_until_ms = esp_timer_get_time() / 1000 + ORB_RING_PULSE_MS;
}

/* The annulus rows the arc's leading edge swept between two fractions — design §6's
 * steady move re-sends tens of rows, not the whole band — the copy arc's own angle sweep.
 * The rows are the answer, and both refresh shapes consume them: whole rows to mark on the
 * old path, the lanes those rows intersect on the new one (rim-design §4.5). */
static void sweep_rows(int f0, int f1, int *y0, int *y1)
{
    if (f0 > f1) { const int t = f0; f0 = f1; f1 = t; }
    const float a0 = (float)f0 * (2.0f * (float)M_PI) / 1000.0f;
    const float a1 = (float)f1 * (2.0f * (float)M_PI) / 1000.0f;
    float lo = (float)SCREEN_HEIGHT, hi = 0.0f;
    for (float a = a0;; a += 0.02f) {
        if (a > a1) a = a1;
        const float c = cosf(a);
        const float ya = RING_CY - RING_ROUT * c, yb = RING_CY - RING_RIN * c;
        if (ya < lo) lo = ya;
        if (yb < lo) lo = yb;
        if (ya > hi) hi = ya;
        if (yb > hi) hi = yb;
        if (a >= a1) break;
    }
    if (y0) *y0 = (int32_t)lo - 1;
    if (y1) *y1 = (int32_t)hi + 1;
}

static void mark_band(void)
{
    int y0, y1;
    ring_rows_for(0, &y0, &y1);
    display_mark_rows(y0, y1);
}

/* Record the step's refresh as lanes rather than as marks (§5.2's step 3). The whole cover for
 * an effect, colour or teardown step; ring_lanes_in()'s answer for a fraction sweep, which is
 * how a drain costs the lanes in the rows it touched and nothing else. An empty sweep records
 * nothing at all — a wedge that lands between two bands is worth no pixels (§4.5) — and the
 * commit is asked, never told: the corner pixel invalidated below is what carries the frame. */
static void lanes_pending(int whole, int y0, int y1)
{
    if (!whole) {
        const int n = ring_lanes_in(s_lanes, s_lane_n, y0, y1, s_sweep, ORB_RING_LANES_MAX);
        if (n <= 0) { s_lanes_due = false; s_pend_i1 = (int16_t)(s_pend_i0 - 1); return; }
        s_pend    = s_sweep;
        s_pend_i0 = 0;
        s_pend_i1 = (int16_t)(n - 1);
    } else {
        s_pend    = s_lanes;
        s_pend_i0 = 0;
        s_pend_i1 = (int16_t)(s_lane_n - 1);
    }
    s_lanes_due = true;
}

/* The same step on the base build's refresh: whole rows, because the tile ring cannot carry a
 * lane batch right now (parked, one buffer, no mirror) or the cover was never built. One line
 * says so and the behaviour is the slice's starting point, byte for byte, at the ceiling grid. */
static void rows_pending(int whole, int y0, int y1)
{
    if (!s_old_log) {
        s_old_log = true;
        ESP_LOGW(RING_TAG, "rim lanes unavailable: whole-row marks at the %d ms ceiling",
                 ORB_RING_STEP_MAX_MS);
    }
    if (whole) mark_band();
    else       display_mark_rows(y0, y1);
}

static int s_alpha;                              /* the effect alpha of this grid step */

void orb_ring_disp_service(void)
{
    if (!s_init) orb_ring_disp_init();

    /* §11's first row: a card copy owns the rim, and the ring does not merely stay quiet, it
     * does not advance — no phase, no record, no grid spent. The copy's own show and hide re-send
     * the whole page (with the ring stamped back into it the moment it is gone), so the ring is
     * on the glass again at the first step after the copy with nothing for the app to do
     * (ring-design §8, unchanged). */
    if (orb_edge_arc_shown()) return;

    /* The governed grid (§6.3): ORB_RING_STEP_MS while a batch costs under 13 ms, up to
     * ORB_RING_STEP_MAX_MS when it costs more. On the row path the grid never runs faster than
     * the ceiling, so a ring the tiles cannot carry keeps exactly the cadence the slice started
     * from — a degraded ring is never worse than the base build, which is why the ceiling is
     * that number. */
    const bool lanes_ok = (s_lane_n > 0) && display_ring_lanes_ready();
    const int  grid_ms  = lanes_ok ? s_grid_ms
                        : (s_grid_ms > ORB_RING_STEP_MAX_MS ? s_grid_ms : ORB_RING_STEP_MAX_MS);
    const int64_t now = esp_timer_get_time();
    if (now - s_step_us < (int64_t)grid_ms * 1000) return;
    s_step_us = now;

    /* A set the commit did not carry — the corner invalidate folded into a page LVGL decided had
     * not changed, or a card loan that opened between this step and that flush — goes out once
     * here, through the same one mux and the same tile ring, and is then dropped: the next step
     * decides again. Refused (held glass, no quiet bus) leaves it recorded for the next try. */
    if (s_lanes_due && lanes_ok && s_pend_i1 >= s_pend_i0) {
        const int sent = display_ring_lanes(s_pend + s_pend_i0, (int)(s_pend_i1 - s_pend_i0 + 1));
        if (sent > 0) orb_ring_disp_batch(sent, (int)display_ring_last_batch_us());
    }

    const bool tiled = display_tiles() > 0;      /* the untiled fallback funds no rim   */
    orb_ring_rec cur;
    const bool owned = ring_pick(orb_ring_tbl, orb_ring_owner, orb_ring_seq,
                                 ORB_APP_CAP, &cur) != 0;

    /* overlay-design §6: fold the live theme's colour into the winner, and notice a theme that
     * moved under a standing ring — the substitution rides the same grid the effect does, so
     * a colour is stale on the glass for at most one step. It runs on a freshly picked
     * record each tick, so a theme taken down hands the record's own colours straight back. */
    const uint32_t ovl = orb_theme_overlay();
    const bool     ovl_changed = (ovl != s_ovl);
    if (ovl_changed) s_ovl = ovl;
    ring_apply_overlay(&cur, ovl);

    const bool anim  = owned && cur.effect != ORB_RING_STEADY;
    /* The effect's alpha is fixed once a grid step, here, and the stamp uses it: a stamp that
     * read the clock per tile would give an app's partial redraw a different breath from the
     * rows marked at the step, banding the rim. One clock read per step, whichever shape the
     * refresh takes — the rule ring-design wrote, and the reason the batch is timed by the
     * layer that submits it and not here. */
    s_alpha = owned ? ring_effect_alpha(&cur, now / 1000) : 0;

    bool whole = true;                               /* the frame owed: the whole cover ...      */
    int  sy0 = 0, sy1 = -1;                          /* ... or the rows this step's sweep touched */
    bool owe = false;

    if (!owned || !tiled) {
        owe = s_have;                                /* restore the glass under a gone ring */
        s_have = false;
    } else if (!s_have || ovl_changed || memcmp(&s_rec, &cur, sizeof cur) != 0) {
        s_rec = cur;                                 /* a new owner, its ask, or the theme changed */
        s_have = true;
        owe = true;
    } else if (anim) {
        /* A backed band is gated on paint movement, not alpha movement: its off-phase is a black
         * band that still paints, and the never-zero clause ring_alpha_moved() leans on would
         * resend an unchanged black frame and swallow the transition home. Either side backed
         * routes through the paint gate; a wholly unbacked step keeps today's rule exactly (§64.12). */
        if ((s_rec.flags | cur.flags) & ORB_RING_BACKED)
            owe = ring_backed_moved(ring_paint_alpha(&s_rec, s_alpha_sent),
                                    ring_paint_alpha(&cur, s_alpha)) != 0;
        else
            owe = ring_alpha_moved(s_alpha_sent, s_alpha) != 0;   /* a breathe/flash step, gated (§6.1) */
    } else {
        int d = cur.fraction - s_rec.fraction;
        if (d < 0) d = -d;
        if (d >= ORB_RING_STEP_PCT) {
            sweep_rows(s_rec.fraction, cur.fraction, &sy0, &sy1);
            whole = false;
            owe   = true;                            /* the drain's own granularity, in lanes    */
        }
        s_rec = cur;                                 /* fraction advanced, sent or not            */
    }

    /* §64.13: a lent press confirmed. For ORB_RING_PULSE_MS the claimant's own record is held
     * at full paint and forced out — the pulse ignores the send gate the way a backed band
     * ignores the alpha byte — on a whole-band frame, so the ring reads solid for the beat.
     * Only while that claimant still owns the rim; once another record wins it, or the window
     * closes, the pulse stops and the normal gate resumes. */
    if (owned && s_pulse_slot >= 0 && pick_slot() == s_pulse_slot &&
        ring_pulse_open(s_pulse_until_ms, now / 1000)) {
        s_rec = cur;
        s_have = true;
        s_alpha = 255;
        whole = true;
        owe   = true;
    } else if (s_pulse_slot >= 0 && !ring_pulse_open(s_pulse_until_ms, now / 1000)) {
        s_pulse_slot = -1;                             /* the window closed: stop pulsing */
    }

    if (owe) {
        if (lanes_ok) lanes_pending(whole, sy0, sy1);
        else       { rows_pending(whole, sy0, sy1); s_alpha_sent = s_alpha; }
    }

    /* A frame for the marks — or the recorded lanes — to ride, and only for a step that owes one:
     * the arc's own pixel in the corner, outside the glass, which no compositor paints and LVGL
     * renders as it already stands. This is what carries a lane batch over a page that changed
     * nothing else, which is exactly the case this slice exists for: a breathing ring over a
     * paused dial. A step the gate held back asks for no commit at all, so a 25 Hz grid with a
     * still band is a 25 Hz decision and a 0 Hz flush. */
    if (owe) {
        lv_obj_t *scr = lv_screen_active();
        if (scr) {
            const lv_area_t corner = { 0, 0, 1, 1 };
            lv_obj_invalidate_area(scr, &corner);
        }
    }
}

/* display.c, in the commit: the lanes the ring owes, as an index range. The set is asked for,
 * never pushed, and stays recorded until the batch is reported back, so a commit that could not
 * carry it (a loan opened after the readiness test, an arc that came up in between) loses
 * nothing. LVGL task, with s_ring_mux held. */
int orb_ring_disp_due_span(const orb_ring_lane_t **lanes, int *i0, int *i1)
{
    if (!lanes || !i0 || !i1) return 0;
    *lanes = NULL; *i0 = 0; *i1 = -1;
    if (!s_init || !s_lanes_due || !s_pend || s_pend_i1 < s_pend_i0) return 0;
    *lanes  = s_pend;
    *i0     = s_pend_i0;
    *i1     = s_pend_i1;
    return s_pend_i1 - s_pend_i0 + 1;
}

/* The batch is spent, in whichever sense of spent §5.3's merge allows: sent, covered by this
 * commit's own bands, or refused by the geometry. The glass holds the ring at this step's alpha
 * either way, which is what the gate compares the next effect step against. Then the governor:
 * the grid becomes three times the batch's own length, so the ring never occupies more than a
 * third of its own window whatever it decides to do, and the line naming the count, the µs and
 * the new grid is printed on the first batch and whenever the grid moves — the bench's whole
 * readout of this slice, in one line (§6.3), with no protocol change to pay for it. */
void orb_ring_disp_batch(int windows, int batch_us)
{
    s_lanes_due  = false;
    s_pend_i1    = (int16_t)(s_pend_i0 - 1);
    s_alpha_sent = s_alpha;
    if (windows <= 0) return;                        /* nothing went out: nothing to price       */
    s_batch_n++;
    const int g = ring_next_step_ms(batch_us);
    const bool moved = (g != s_grid_ms);
    s_grid_ms = g;
    if (moved || s_batch_n == 1)
        ESP_LOGI(RING_TAG, "rim lanes: %d windows in %d us, grid %d ms", windows, batch_us, s_grid_ms);
}

void orb_ring_disp_stamp(int32_t y0, int rows, uint16_t *tile, int32_t x0, int32_t w)
{
    if (!tile || !s_have || rows <= 0 || w <= 0) return;
    const int alpha = s_alpha;                   /* fixed at the service's grid step     */
    const int backed = (s_rec.flags & ORB_RING_BACKED) ? 1 : 0;
    if (alpha <= 0 && !backed) return;           /* unbacked off-phase or alpha-0: draw nothing   */
    /* The two words the geometry needs are read out of the record once here, not per row and never
     * per pixel (rim-design §3): the colours above already had the ORB_RING_MATCH substitution
     * folded in by the service, and the start and direction are the record's own. */
    const int start_deg = s_rec.start;
    const int ccw = (s_rec.flags & ORB_RING_CCW) ? 1 : 0;
    uint16_t on_px = 0, off_px = 0;              /* the backed band's two hoisted colours (§64.12) */
    uint32_t argb_arc = 0, argb_trk = 0;         /* the unbacked arc/track words, alpha folded in   */
    if (backed) {
        const int pa = ring_paint_alpha(&s_rec, alpha);
        const int cl = pa < 0 ? 0 : (pa > 255 ? 255 : pa);
        const uint32_t back_argb = (s_rec.arc & 0x00FFFFFFu) | ((uint32_t)cl << 24);
        on_px  = ring_backed565(back_argb, 1);   /* arc: the colour scaled toward black by the effect */
        off_px = ring_backed565(back_argb, 0);   /* band: black, even on a flash's off-phase          */
    } else {
        const uint32_t ac = s_rec.arc, tc = s_rec.track;
        const uint32_t aa = (uint32_t)(((ac >> 24) & 0xFFu) * (uint32_t)alpha / 255u);
        const uint32_t ta = (uint32_t)(((tc >> 24) & 0xFFu) * (uint32_t)alpha / 255u);
        argb_arc = (ac & 0x00FFFFFFu) | (aa << 24);
        argb_trk = (tc & 0x00FFFFFFu) | (ta << 24);
    }
    orb_ring_part_t parts[ORB_RING_PARTS_MAX];
    for (int r = 0; r < rows; r++) {
        const int np = ring_row_parts((int)(y0 + r), s_rec.fraction, start_deg, ccw,
                                      parts, ORB_RING_PARTS_MAX);
        if (np <= 0) continue;
        uint16_t *d = tile + (size_t)r * (size_t)w - x0;
        for (int i = 0; i < np; i++) {
            int xa = parts[i].x0, xb = parts[i].x1;
            if (xa < x0) xa = x0;
            if (xb > x0 + w - 1) xb = x0 + w - 1;
            if (backed) {
                const uint16_t px = parts[i].arc ? on_px : off_px;   /* arc on the black band */
                for (int x = xa; x <= xb; x++) d[x] = px;            /* opaque: no blend      */
            } else {
                const uint32_t argb = parts[i].arc ? argb_arc : argb_trk;
                for (int x = xa; x <= xb; x++) d[x] = ring_blend565(d[x], argb);
            }
        }
    }
}

void orb_ring_status(orb_ring_status_t *out)
{
    if (!out) return;
    out->owner = NULL; out->fraction = 0; out->effect = "steady"; out->on_glass = false;
    out->alpha = 0; out->sent = 0; out->grid_ms = 0; out->batch_us = 0; out->batches = 0;
    if (!s_init) return;
    out->alpha = s_alpha; out->sent = s_alpha_sent; out->grid_ms = s_grid_ms;
    out->batch_us = (int)display_ring_last_batch_us(); out->batches = s_batch_n;
    orb_ring_rec cur;
    if (!ring_pick(orb_ring_tbl, orb_ring_owner, orb_ring_seq, ORB_APP_CAP, &cur)) return;
    out->owner    = slug_of_slot(pick_slot());
    out->fraction = cur.fraction;
    out->effect   = effect_name(cur.effect);
    const int alpha = ring_effect_alpha(&cur, esp_timer_get_time() / 1000);
    out->on_glass   = display_tiles() > 0 && alpha > 0;
}

#endif /* ESP_PLATFORM */
