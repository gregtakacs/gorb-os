#pragma once
/**
 * orb_ring — the OS ring any app may set (plan §64.1, docs/specs/ring-design.md).
 *
 * The PURE core: geometry, the blend over a tile pixel, the effect clock, the OS
 * clamps and the one-record-per-app ownership table. No ESP-IDF, no LVGL, no
 * `esp_timer` and no allocation — every time-dependent entry takes the caller's
 * `now_*`, and the caller's arrays are the table, so the same code drives the
 * firmware's PSRAM `s_own[ORB_APP_CAP]` and tools/host_ring_test.py's stack arrays.
 *
 * The look is the copy arc's and the theme rings': a bright arc over a dim track, from
 * the record's start angle (twelve by default) in the record's direction (clockwise by
 * default), in the same 7 px rim annulus (R_OUT 231, R_IN 224 on
 * the 466 px glass, CX = CY = 233), blended into the DMA tiles as they leave and
 * never into the mirror. The stamp (ring-2) does only the blend per pixel: this
 * core hands it the row's spans, cut by the arc's two rays, so no trigonometry
 * runs per pixel (ring-design's orchestrator amendment 1, rim-design §3).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_RING_STEADY  0   /* on while shown; repaints only when the fraction moves */
#define ORB_RING_FLASH   1   /* on/off twice a period: "waiting for you"              */
#define ORB_RING_BREATHE 2   /* slow in/out: "paused"                                 */

/* The record's byte-1 ask (overlay-design §5 and its amendment 1, plan §64.8/§64.9, rim-design §2).
 * Both words ride the record's old reserved byte, not a bit of `effect`: ring_clamp validates the
 * effect and three readers consume it, so a flag bit there would turn FLASH|0x80 into a silent
 * steady. An app that sets either on firmware without this slice keeps its own colours and a
 * twelve-clockwise ring — the fields it wrote were reserved. */
#define ORB_RING_MATCH      0x01u  /* arc/track take the live theme's overlay colour (§64.9) */
#define ORB_RING_CCW        0x02u  /* the arc runs anticlockwise from its start (§64.8)      */
#define ORB_RING_BACKED     0x04u  /* the whole rim band is drawn black with the arc on it,
                                    * at full opacity whatever the alpha byte says (§64.12)   */
#define ORB_RING_FLAGS_MASK 0x07u  /* ring_clamp drops every bit outside these three          */

/* The backed breathe's floor: the arc's alpha never falls under this, so the trough reads as the
 * colour at 96/255 on black — dimmed, never gone, never into the app beneath (§64.12, alerts-design
 * §2.2 and orchestrator amendment 3, which moved it off the design's original 64). */
#define ORB_RING_BACKED_MIN_ALPHA 96

/* The clamps the OS applies whatever the app asks (ring-design §6; the rim numbers are
 * rim-design §6.1: the lanes made the 2 Hz grid and the 4 s breath the owner's complaint, so the
 * grid is 25 Hz, the floors are design numbers the bench may raise, and two new words bound what
 * one step may spend — the alpha gate that keeps a fast grid from re-sending an unchanged pixel,
 * and the governor's ceiling, which is the base build's own grid so a starved ring is never worse
 * than the slice it came from). */
#define ORB_RING_STEP_MS             40    /* the grid a lane batch is allowed to run at: 25 Hz  */
#define ORB_RING_BREATHE_MIN_MS    1500    /* breathe's floor: 37 steps of 8 alpha at 25 Hz      */
#define ORB_RING_FLASH_MIN_MS       400    /* flash's floor: 5 transitions a second              */
#define ORB_RING_STEP_PCT              2   /* a steady move re-sends at >= 2 per mille           */
#define ORB_RING_STEP_ALPHA            8   /* an effect step sends when its alpha moved by >= 8   */
#define ORB_RING_STEP_MAX_MS         400   /* the governor's ceiling: the base build's grid      */
#define ORB_RING_DEFAULT_PERIOD_MS  4000   /* period_ms == 0                                     */

/* The OS's confirmation of a lent press (plan §64.13, alerts-design §3.5): the claimant's
 * OWN ring record is held at full paint for this long — never OS art, and never at all when
 * the claimant owns no record or another record wins the rim (the pulse is the claimant's
 * ring, and a claimant with no ring gets no pulse). */
#define ORB_RING_PULSE_MS           160

/* The record an app sets, and one row of the ownership table. The layout is the
 * SDK's orb_ring_t (apps/sdk/orb.h, ring-2) and must not drift from it. */
typedef struct {
    uint16_t fraction;    /* 0..1000, from twelve in the record's direction; > 1000 clamps to 1000 */
    uint8_t  effect;      /* ORB_RING_STEADY | _FLASH | _BREATHE; other values => STEADY */
    uint8_t  flags;       /* ORB_RING_MATCH | _CCW | _BACKED; ring_clamp masks to ORB_RING_FLAGS_MASK */
    uint32_t arc;         /* 0xAARRGGBB — arc alpha in bits 31..24 (0 => the arc draws nothing) */
    uint32_t track;       /* 0xAARRGGBB — track alpha in bits 31..24 (0 => a bare arc)  */
    uint16_t period_ms;   /* effect period, clamped by the OS; 0 => the OS default      */
    uint16_t start;       /* degrees clockwise from twelve; > 359 wraps, ring_clamp folds */
} orb_ring_rec;           /* 16 bytes, ILP32 LE — offsets 0, 2, 3, 4, 8, 12, 14 */

_Static_assert(sizeof(orb_ring_rec) == 16, "orb_ring_rec is the SDK's orb_ring_t layout");

/* One piece of the annulus on one glass row: the closed column range [x0, x1],
 * to be blended with `arc ? rec.arc : rec.track`. A row is at most four pieces (rim-design §3's
 * monotone-angle lemma: its two spans, a wedge cutting each of them in at most one interval), in
 * increasing x, and they cover the annulus exactly once. */
typedef struct {
    int16_t x0;
    int16_t x1;
    uint8_t arc;
    uint8_t _pad;
} orb_ring_part_t;
#define ORB_RING_PARTS_MAX 4   /* size the caller's array this; a fuller row answers no more */

/* A rim lane: one even-aligned rectangle of the band's cover (rim-design §4). Closed intervals;
 * a lane is (x1-x0+1)*(y1-y0+1) pixels. Built once by ring_lanes(), never by the caller. */
typedef struct { int16_t x0, y0, x1, y1; } orb_ring_lane_t;
#define ORB_RING_LANES_MAX      64    /* size the caller's array this                  */
#define ORB_RING_LANE_ROWS_MAX  64    /* the greedy's band-height bound                */
#define ORB_RING_LANE_EXTRA_MAX 384   /* the greedy's per-band slack bound (A/B, §4.4) */
#define ORB_RING_LANE_MAX_PX    1024  /* every single lane is at most this many pixels */

/** One tile pixel, wire-order (byte-swapped) RGB565 in and out; argb's top byte is
 *  the alpha. a == 0 returns bg565_swapped byte for byte (alpha 0 draws nothing);
 *  a == 255 answers the source colour, quantised to 565. */
uint16_t ring_blend565(uint16_t bg565_swapped, uint32_t argb);

/** The glass rows the annulus can touch, both ends included: 0..1000 -> [2, 464].
 *  The row SET the ring may mark, not the arc's extent (ring-design §4): a track
 *  paints the whole annulus, so the answer does not depend on the fraction. */
void ring_rows_for(int fraction, int *y0, int *y1);

/** The pieces of row `y` for this fraction, start angle and direction: up to `max` written
 *  to `parts`, the count returned (0 when the row touches no annulus, or the arguments are
 *  void). `start_deg` is degrees clockwise from twelve — any value; it wraps at 360. `ccw` is
 *  the ORB_RING_CCW bit's 0/1: the arc is the wedge [start_deg, start_deg + fraction*360°)
 *  clockwise, or (start_deg - fraction*360°, start_deg] anticlockwise. Covers the annulus
 *  exactly once, split arc/track at the wedge's two rays (rim-design §3). */
int ring_row_parts(int y, int fraction, int start_deg, int ccw,
                   orb_ring_part_t *parts, int max);

/** The effect resolved to one instant, as an arc+track alpha multiplier 0..255:
 *  steady 255, flash 255 for the first half of the clamped period and 0 for the
 *  second, breathe a smooth 0..255..0 cosf ramp over the clamped period. Off-phase
 *  is 0, and the caller blends nothing, so the glass is untouched (ring-design §13).
 *  The record's own colour alphas are multiplied by this, not folded into it. */
int ring_effect_alpha(const orb_ring_rec *r, int64_t now_ms);

/** 1 when an effect step at `now_alpha` owes the glass a refresh after one at `was_alpha`
 *  (rim-design §6.1): the resolved alpha moved by at least ORB_RING_STEP_ALPHA, or either side
 *  is 0 — a band fading out sends its last frame and its first frame back, so the gate never
 *  swallows a transition into or out of nothing. Pure, so the gate has a test and the glue is
 *  three lines. */
int ring_alpha_moved(int was_alpha, int now_alpha);

/** A backed record's band pixel, wire-order RGB565 out: non-arc pixels (`arc == 0`) are pure
 *  black; an arc pixel is `argb`'s colour blended against black under its own top-byte alpha, so a
 *  full-alpha argb is the source colour and a floored breathe is the colour dimmed toward black.
 *  The record's own alpha byte never reaches this — the caller hands it the effect's alpha (§64.12). */
uint16_t ring_backed565(uint32_t argb, int arc);

/** The alpha the band is painted at for the send gate and the backed colour: 255 when the record is
 *  unbacked and steady (a non-effect path), and when it is backed and steady (the band is opaque, the
 *  byte ignored); the effect's own alpha when unbacked; for a backed breathe the effect floored to
 *  ORB_RING_BACKED_MIN_ALPHA (never into the app beneath); for a backed flash the effect (255 lit, 0
 *  black band — and the black phase still paints). Unbacked paths are byte-for-byte today's. */
int ring_paint_alpha(const orb_ring_rec *r, int effect_alpha);

/** 1 when a band whose off-phase is black (not nothing) owes a refresh between two paint alphas:
 *  the paint moved. Used only when either side was backed, in place of ring_alpha_moved() — a
 *  fully black and a fully coloured flash both paint, so the rule is the change, never the
 *  never-zero clause ring_alpha_moved() leans on (§64.12, alerts-design §2.3). */
int ring_backed_moved(int was_paint, int now_paint);

/** 1 while a lent-press pulse window is open (§64.13): the pulse was armed for a stamp
 *  `until_ms` and the clock `now_ms` is still inside it (a half-open [arm, until), so a
 *  stamp exactly at the expiry restores the record's normal gate). Pure and host-proven
 *  (tools/host_ring_test.py's "pulse" case); the IDF glue arms it and the service consumes
 *  it to force that record's paint and send for ORB_RING_PULSE_MS. */
int ring_pulse_open(int64_t until_ms, int64_t now_ms);

/** The next step's grid in microseconds of the wall clock converted to ms (rim-design §6.3),
 *  given how long the last lane batch took in microseconds `batch_us`: never below
 *  ORB_RING_STEP_MS, never above ORB_RING_STEP_MAX_MS, and never less than three times the
 *  batch's own length — the ring's promise to occupy at most a third of its own grid. A
 *  negative `batch_us` (a failed measurement, garbage from a starved clock) counts as 0. */
int ring_next_step_ms(int batch_us);

/* Fold the live theme's overlay colour into a picked record (§64.9): `overlay` is 0x01RRGGBB or 0,
 * and anything that is not 0x01RRGGBB — including a record that did not ask — changes nothing.
 * Where it does apply, arc and track keep their own alphas and take this RGB; an alpha of 0 still
 * draws nothing, so a theme cannot unmute a record its app muted. Idempotent. */
void ring_apply_overlay(orb_ring_rec *r, uint32_t overlay);

/** Clamp a record in place: fraction to 0..1000, an unknown effect to STEADY, period_ms up to
 *  the effect's floor (0 first becomes ORB_RING_DEFAULT_PERIOD_MS), flags to ORB_RING_FLAGS_MASK
 *  (an unknown bit is dropped, not honoured) and start wrapped into 0..359. Every word is
 *  idempotent: clamping a clamped record changes nothing. */
void ring_clamp(orb_ring_rec *r);

/** The ownership table (ring-design §5): parallel caller-owned arrays, `owner[i]`
 *  the owning slot or -1 when cleared, `seq[i]` the monotonic stamp. No statics. */
void ring_init (orb_ring_rec *tbl, int *owner, int n);
void ring_set  (orb_ring_rec *tbl, int *owner, int64_t *seq, int n, int slot,
                const orb_ring_rec *r, int64_t now_us);
void ring_clear(orb_ring_rec *tbl, int *owner, int n, int slot);

/** Fold the newest owned record, clamped, into `out`: the greatest `seq`, ties
 *  broken by the higher slot. 1 when some record is owned, 0 when none is. */
int ring_pick (const orb_ring_rec *tbl, const int *owner, const int64_t *seq,
               int n, orb_ring_rec *out);

/** The band's cover: the lane rectangles, in increasing row, into the caller's array; the count
 *  returned, 0 for bad arguments or when max is too small. Deterministic and pure: the same
 *  constants give the same cover on the host, in the simulator and on the orb (rim-design §4.2). */
int ring_lanes(orb_ring_lane_t *lanes, int max);

/** The lanes intersecting rows [y0, y1], in order, into the caller's array; the count returned.
 *  Pure, so the filter a steady drain runs through has a test: full range answers the full cover
 *  (rim-design §4.5). */
int ring_lanes_in(const orb_ring_lane_t *lanes, int n, int y0, int y1,
                  orb_ring_lane_t *out, int max);

/* ---- the IDF glue (ring-design §3, §8; ring-2) ------------------------------------
 * Defined only in orb_ring.c's ESP_PLATFORM section — the one place the ring may
 * include esp_timer.h / display.h — so the pure core above stays host-compilable
 * verbatim (tools/host_ring_test.py compiles it with gcc and no IDF). The prototypes
 * are always visible so the display.c rim dispatch, main.c, the wasm shim and
 * orb_net.c can reach them; a build that never links the glue never calls them. */

/* The ownership table (ring-design §5, §7): EXT_RAM_BSS_ATTR, PSRAM, sized ORB_APP_CAP
 * and indexed by the app's flash slot (the same key slot_index() gives the wasm shim).
 * The shim drives the pure ring_set/ring_clear on these directly; a refusal, a revoke
 * or an uninstall drops the row through orb_ring_drop. */
extern orb_ring_rec   orb_ring_tbl[];    /* design §5's s_own[ORB_APP_CAP]  */
extern int            orb_ring_owner[];  /* owning slot, or -1 when cleared  */
extern int64_t        orb_ring_seq[];    /* monotonic ms stamp, newest owns  */

typedef struct {
    const char *owner;    /* the owning app's slug, or NULL when none is owned         */
    int         fraction; /* the owned record's fraction 0..1000 (0 when unowned)       */
    const char *effect;   /* "steady" | "flash" | "breathe" ("steady" when unowned)     */
    bool        on_glass; /* owned && its effect is on now && the tiled path is live     */
} orb_ring_status_t;

void orb_ring_disp_init(void);              /* boot, before the first app builds (§5)   */
void orb_ring_disp_service(void);           /* LVGL task, beside orb_edge_arc_service   */
void orb_ring_drop(int slot);               /* refusal / revoke / uninstall: owner = -1  */
void orb_ring_disp_pulse(int slot);         /* §64.13: the router lends a press; pulse that claimant's ring */
void orb_ring_status(orb_ring_status_t *out);

/** display.c, LVGL task: the app ring's rim dispatch, the copy arc's twin. Only ever
 *  called when orb_edge_arc_shown() is false (ring-design §8: the copy arc wins). Blends
 *  the owned record's annulus into `rows` rows of a wire-order tile. */
void orb_ring_disp_stamp(int32_t y0, int rows, uint16_t *tile, int32_t x0, int32_t w);

/** display.c, inside the commit (rim-design §5.2): the lanes the ring owes the glass right now,
 *  as a contiguous index range into the cover it hands back — `*lanes` the array, `*i0`/`*i1` the
 *  first and last index to send, in row order. The whole cover for an effect or colour step,
 *  `ring_lanes_in()` for a fraction sweep. Answers 0 when the ring owes nothing. Does NOT clear
 *  the pending set: the caller reports the batch with orb_ring_disp_batch(), which does. LVGL
 *  task only, with s_ring_mux held. */
int orb_ring_disp_due_span(const orb_ring_lane_t **lanes, int *i0, int *i1);

/** display.c, at the end of the commit that carried them (rim-design §5.2/§6.3): how many lane
 *  windows went out and the batch's wall time in microseconds. This is where the pending set is
 *  dropped, the alpha just sent is remembered for the gate, and `ring_next_step_ms()` is applied
 *  to the grid — and where the one log line naming the lane count, the batch's µs and the new
 *  grid is printed. LVGL task only. */
void orb_ring_disp_batch(int windows, int batch_us);

#ifdef __cplusplus
}
#endif
