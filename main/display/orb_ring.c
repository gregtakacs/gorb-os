/* The OS ring's pure core — contract in orb_ring.h (plan §64.1, ring-design §4..§6).
 *
 * Geometry here is the copy arc's (orb_edge_arc.c) restated so a row comes back as at
 * most four x-pieces instead of a trig test per pixel: the two radii are compared in
 * exact integer arithmetic (four times squared, so a pixel centre's half offset is an
 * odd integer and no rounding can put a pixel on the wrong side of a radius), and the
 * arc/track edge is one half-plane test per row solved for x. The stamp that consumes
 * this does only the blend per pixel; tools/host_ring_test.py checks these pieces
 * against a per-pixel reference for every row and a sweep of fractions. */
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
static int lhalf(int e) { return e > CC - 1 ? CC - 1 : e; }           /* no track crosses twelve */

/* The arc's columns on row y, as at most two closed runs in increasing x. A pixel's angle
 * from twelve clockwise is a = atan2(u, v) with u = x - KPC and v = KPC - y, and the arc is
 * {a <= A}; v never vanishes (a row's centre is a half integer), so the ray either crosses
 * the row's line or leaves the whole row on one side of it. */
static int arc_runs(int y, int fraction, int r0[2], int r1[2])
{
    if (fraction <= 0) return 0;
    if (fraction >= 1000) { r0[0] = 0; r1[0] = GLASS - 1; return 1; }
    const double A = (double)fraction * TAU / 1000.0;
    const double s = sin(A), c = cos(A), v = (double)CC - 0.5 - (double)y;
    const int flat = c > -1e-12 && c < 1e-12;        /* a horizontal ray: it crosses no row's line */
    const double b = flat ? 0.0 : KPC + s * v / c;
    if (fraction < 500) {                           /* arc = the right half, cut short by the ray */
        if (flat && s * v < 0.0) return 0;          /* a vertical ray with the row below it: all track */
        r0[0] = CC;
        r1[0] = flat ? GLASS - 1 : (c > 0.0 ? clip_x(fl(b)) : GLASS - 1);
        if (!flat && c < 0.0) { const int e = ce(b); r0[0] = e > CC ? e : CC; }
        return r0[0] <= r1[0] ? 1 : 0;
    }
    /* track = the left half, from the ray on; the arc is what that leaves */
    int t0, t1;
    if (flat) { if (s * v <= 0.0) { t0 = 0; t1 = CC - 1; } else { t0 = CC; t1 = -1; } }
    else if (c < 0.0) { t0 = 0; t1 = lhalf(fl(b)); }
    else { t0 = ce(b); t1 = CC - 1; }
    if (t0 > t1) { r0[0] = 0; r1[0] = GLASS - 1; return 1; }
    if (t0 > 0) {
        r0[0] = 0; r1[0] = t0 - 1; r0[1] = t1 + 1; r1[1] = GLASS - 1;
        return t1 + 1 <= GLASS - 1 ? 2 : 1;
    }
    r0[0] = t1 + 1; r1[0] = GLASS - 1;
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

int ring_row_parts(int y, int fraction, orb_ring_part_t *parts, int max)
{
    if (!parts || max < 1 || y < 0 || y >= GLASS) return 0;
    if (fraction < 0) fraction = 0;                  /* the same clamps the record takes */
    if (fraction > 1000) fraction = 1000;
    if (!p_hit(y, CC - 1, 0)) return 0;                       /* the row misses the annulus */
    const int lo = e_first(y, 0, CC - 1, 0), hi = e_last(y, CC, GLASS - 1, 0);
    int spans[2][2], ns = 1, a0[2], a1[2];
    if (p_hit(y, CC - 1, 1)) {                                /* the row passes the inner circle */
        const int hl = e_first(y, 0, CC - 1, 1), hr = e_last(y, CC, GLASS - 1, 1);
        spans[0][0] = lo; spans[0][1] = hl - 1; spans[1][0] = hr + 1; spans[1][1] = hi;
        ns = 2;
    } else {
        spans[0][0] = lo; spans[0][1] = hi;
    }
    const int na = arc_runs(y, fraction, a0, a1);
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
#include "esp_timer.h"       /* the service and stamp read the wall clock             */
#include "board_config.h"    /* SCREEN_HEIGHT: the full-band mark, the arc's own      */
#include "orb_app.h"         /* ORB_APP_CAP, orb_app_row, flash_slot: the owner slug  */
#include "display.h"         /* display_mark_rows, display_tiles                       */
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
static int64_t      s_step_us;      /* the last grid tick (ORB_RING_STEP_MS cadence)       */
static bool         s_init;

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
    s_step_us = 0;
    s_init = true;
}

void orb_ring_drop(int slot)
{
    if (slot < 0 || slot >= ORB_APP_CAP) return;
    if (!s_init) orb_ring_disp_init();
    ring_clear(orb_ring_tbl, orb_ring_owner, ORB_APP_CAP, slot);
}

/* The annulus rows the arc's leading edge swept between two fractions — design §6's
 * steady move re-sends tens of rows, not the whole band — the copy arc's own angle sweep. */
static void mark_span(int f0, int f1)
{
    if (f0 > f1) { const int t = f0; f0 = f1; f1 = t; }
    const float a0 = (float)f0 * (2.0f * (float)M_PI) / 1000.0f;
    const float a1 = (float)f1 * (2.0f * (float)M_PI) / 1000.0f;
    float y_lo = (float)SCREEN_HEIGHT, y_hi = 0.0f;
    for (float a = a0;; a += 0.02f) {
        if (a > a1) a = a1;
        const float c = cosf(a);
        const float ya = RING_CY - RING_ROUT * c, yb = RING_CY - RING_RIN * c;
        if (ya < y_lo) y_lo = ya;
        if (yb < y_lo) y_lo = yb;
        if (ya > y_hi) y_hi = ya;
        if (yb > y_hi) y_hi = yb;
        if (a >= a1) break;
    }
    display_mark_rows((int32_t)y_lo - 1, (int32_t)y_hi + 1);
}

static void mark_band(void)
{
    int y0, y1;
    ring_rows_for(0, &y0, &y1);
    display_mark_rows(y0, y1);
}

static int s_alpha;                              /* the effect alpha of this grid step */

void orb_ring_disp_service(void)
{
    if (!s_init) orb_ring_disp_init();
    const int64_t now = esp_timer_get_time();
    if (now - s_step_us < (int64_t)ORB_RING_STEP_MS * 1000) return;   /* the 2 Hz grid */
    s_step_us = now;

    const bool tiled = display_tiles() > 0;      /* the untiled fallback funds no rim   */
    orb_ring_rec cur;
    const bool owned = ring_pick(orb_ring_tbl, orb_ring_owner, orb_ring_seq,
                                 ORB_APP_CAP, &cur) != 0;
    const bool anim  = owned && cur.effect != ORB_RING_STEADY;
    /* The effect's alpha is fixed once a grid step, here, and the stamp uses it: a stamp that
     * read the clock per tile would give an app's partial redraw a different breath from the
     * rows marked at the step, banding the rim. */
    s_alpha = owned ? ring_effect_alpha(&cur, now / 1000) : 0;

    if (!owned || !tiled) {
        if (s_have) mark_band();                 /* restore the glass under a gone ring */
        s_have = false;
    } else if (!s_have || memcmp(&s_rec, &cur, sizeof cur) != 0) {
        mark_band();                             /* a new owner, or its ask changed     */
        s_rec = cur;
        s_have = true;
    } else if (anim) {
        mark_band();                             /* breathe/flash step the whole annulus*/
    } else {
        int d = cur.fraction - s_rec.fraction;
        if (d < 0) d = -d;
        if (d >= ORB_RING_STEP_PCT) mark_span(s_rec.fraction, cur.fraction);
        s_rec = cur;                             /* fraction advanced, marked or not    */
    }

    /* A frame for the marks to ride: the arc's own pixel in the corner, outside the glass,
     * which no compositor paints and LVGL renders as it already stands. */
    lv_obj_t *scr = lv_screen_active();
    if (scr) {
        const lv_area_t corner = { 0, 0, 1, 1 };
        lv_obj_invalidate_area(scr, &corner);
    }
}

void orb_ring_disp_stamp(int32_t y0, int rows, uint16_t *tile, int32_t x0, int32_t w)
{
    if (!tile || !s_have || rows <= 0 || w <= 0) return;
    const int alpha = s_alpha;                   /* fixed at the service's grid step     */
    if (alpha <= 0) return;                      /* off-phase or alpha-0: draw nothing   */
    const uint32_t ac = s_rec.arc, tc = s_rec.track;
    const uint32_t aa = (uint32_t)(((ac >> 24) & 0xFFu) * (uint32_t)alpha / 255u);
    const uint32_t ta = (uint32_t)(((tc >> 24) & 0xFFu) * (uint32_t)alpha / 255u);
    const uint32_t argb_arc = (ac & 0x00FFFFFFu) | (aa << 24);
    const uint32_t argb_trk = (tc & 0x00FFFFFFu) | (ta << 24);
    orb_ring_part_t parts[ORB_RING_PARTS_MAX];
    for (int r = 0; r < rows; r++) {
        const int np = ring_row_parts((int)(y0 + r), s_rec.fraction, parts, ORB_RING_PARTS_MAX);
        if (np <= 0) continue;
        uint16_t *d = tile + (size_t)r * (size_t)w - x0;
        for (int i = 0; i < np; i++) {
            int xa = parts[i].x0, xb = parts[i].x1;
            if (xa < x0) xa = x0;
            if (xb > x0 + w - 1) xb = x0 + w - 1;
            const uint32_t argb = parts[i].arc ? argb_arc : argb_trk;
            for (int x = xa; x <= xb; x++) d[x] = ring_blend565(d[x], argb);
        }
    }
}

void orb_ring_status(orb_ring_status_t *out)
{
    if (!out) return;
    out->owner = NULL; out->fraction = 0; out->effect = "steady"; out->on_glass = false;
    if (!s_init) return;
    orb_ring_rec cur;
    if (!ring_pick(orb_ring_tbl, orb_ring_owner, orb_ring_seq, ORB_APP_CAP, &cur)) return;
    out->owner    = slug_of_slot(pick_slot());
    out->fraction = cur.fraction;
    out->effect   = effect_name(cur.effect);
    const int alpha = ring_effect_alpha(&cur, esp_timer_get_time() / 1000);
    out->on_glass   = display_tiles() > 0 && alpha > 0;
}

#endif /* ESP_PLATFORM */
