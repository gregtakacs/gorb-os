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
 * The look is the copy arc's and the theme rings': a bright arc clockwise from
 * twelve over a dim track, in the same 7 px rim annulus (R_OUT 231, R_IN 224 on
 * the 466 px glass, CX = CY = 233), blended into the DMA tiles as they leave and
 * never into the mirror. The stamp (ring-2) does only the blend per pixel: this
 * core hands it the row's spans, split at the fraction's ray, so no trigonometry
 * runs per pixel (ring-design's orchestrator amendment 1).
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

/* The clamps the OS applies whatever the app asks (ring-design §6). */
#define ORB_RING_STEP_MS            500    /* at most 2 repaints a second            */
#define ORB_RING_BREATHE_MIN_MS     4000   /* breathe is slow only                    */
#define ORB_RING_FLASH_MIN_MS       1000   /* flash at <= 1 Hz                        */
#define ORB_RING_STEP_PCT              5   /* a steady move re-sends at >= 5 per mille */
#define ORB_RING_DEFAULT_PERIOD_MS  4000   /* period_ms == 0                           */

/* The record an app sets, and one row of the ownership table. The layout is the
 * SDK's orb_ring_t (apps/sdk/orb.h, ring-2) and must not drift from it. */
typedef struct {
    uint16_t fraction;    /* 0..1000, clockwise from twelve; > 1000 clamps to 1000     */
    uint8_t  effect;      /* ORB_RING_STEADY | _FLASH | _BREATHE; other values => STEADY */
    uint8_t  _r0;         /* reserved, 0                                                */
    uint32_t arc;         /* 0xAARRGGBB — arc alpha in bits 31..24 (0 => the arc draws nothing) */
    uint32_t track;       /* 0xAARRGGBB — track alpha in bits 31..24 (0 => a bare arc)  */
    uint16_t period_ms;   /* effect period, clamped by the OS; 0 => the OS default      */
    uint16_t _r1;         /* reserved, 0                                                */
} orb_ring_rec;           /* 16 bytes, ILP32 LE */

_Static_assert(sizeof(orb_ring_rec) == 16, "orb_ring_rec is the SDK's orb_ring_t layout");

/* One piece of the annulus on one glass row: the closed column range [x0, x1],
 * to be blended with `arc ? rec.arc : rec.track`. A row is at most four pieces (its
 * two spans, the span the fraction's ray crosses split either side of it), in
 * increasing x, and they cover the annulus exactly once. */
typedef struct {
    int16_t x0;
    int16_t x1;
    uint8_t arc;
    uint8_t _pad;
} orb_ring_part_t;
#define ORB_RING_PARTS_MAX 4   /* size the caller's array this; a fuller row answers no more */

/** One tile pixel, wire-order (byte-swapped) RGB565 in and out; argb's top byte is
 *  the alpha. a == 0 returns bg565_swapped byte for byte (alpha 0 draws nothing);
 *  a == 255 answers the source colour, quantised to 565. */
uint16_t ring_blend565(uint16_t bg565_swapped, uint32_t argb);

/** The glass rows the annulus can touch, both ends included: 0..1000 -> [2, 464].
 *  The row SET the ring may mark, not the arc's extent (ring-design §4): a track
 *  paints the whole annulus, so the answer does not depend on the fraction. */
void ring_rows_for(int fraction, int *y0, int *y1);

/** The pieces of row `y` for this fraction: up to `max` written to `parts`, the
 *  count returned (0 when the row touches no annulus, or the arguments are void).
 *  Covers the annulus exactly once, split arc/track at the fraction's ray. */
int ring_row_parts(int y, int fraction, orb_ring_part_t *parts, int max);

/** The effect resolved to one instant, as an arc+track alpha multiplier 0..255:
 *  steady 255, flash 255 for the first half of the clamped period and 0 for the
 *  second, breathe a smooth 0..255..0 cosf ramp over the clamped period. Off-phase
 *  is 0, and the caller blends nothing, so the glass is untouched (ring-design §13).
 *  The record's own colour alphas are multiplied by this, not folded into it. */
int ring_effect_alpha(const orb_ring_rec *r, int64_t now_ms);

/** Clamp a record in place: fraction to 0..1000, an unknown effect to STEADY, and
 *  period_ms up to the effect's floor (0 first becomes ORB_RING_DEFAULT_PERIOD_MS). */
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
void orb_ring_status(orb_ring_status_t *out);

/** display.c, LVGL task: the app ring's rim dispatch, the copy arc's twin. Only ever
 *  called when orb_edge_arc_shown() is false (ring-design §8: the copy arc wins). Blends
 *  the owned record's annulus into `rows` rows of a wire-order tile. */
void orb_ring_disp_stamp(int32_t y0, int rows, uint16_t *tile, int32_t x0, int32_t w);

#ifdef __cplusplus
}
#endif
