#pragma once
/**
 * main/gfx/orb_layers.h — the compositor face of the app ABI (P5.5).
 *
 * Four calls a registered app makes to put something on the glass: add, set,
 * touch, del. They own a small per-app table of layer rows, and the OS walks that
 * table into the job list `gfx_commit()` consumes. This is where the frozen
 * `layer_*` entries of `orb_svc_t` stop being stubs: the gate and the ledger stay
 * in orb_svc.c, the table lives here, and the broker holds one table per slot.
 *
 * The art reference is an `orb_asset_t` the resolver handed back and nothing else
 * (D25). An app never names an offset into a bank it mapped for itself; this module
 * has no entry point that takes a pixel address. A tier-1 answer points at theme
 * pixels; a tier-2/tier-3 answer names a generated primitive, and the bake that
 * produces its pixels is orb_assets.c's — the same one the host suites drive.
 *
 * The plan's section 1.2b recorded the three disproved assumptions this shape
 * fixes: a fallback CANNOT bake into the app's bank (art_bank_extra_ptr is
 * read-only), so each baked row owns heap freed on del/wipe/suspend/revoke; the
 * registry carries geometry but no styling, so the descriptor carries the style;
 * and the frozen `layer_add(app, desc)` signature forces both, since it passes no
 * plane and no z: paint order IS table order, and re-ordering means del re-add.
 *
 * Two things this deliberately is NOT, and what became of the P6 hand-off:
 *  - the clock does not route through here (its art reaches the registry only once
 *    .orbapp ingest exists, and it still drives clock_layer_commit directly);
 *  - no commit hook calls the job builder yet. P5.5 named P6 as the owner of the
 *    caller; P6's rewrite (D21/D24/D29) re-scoped it to the bundle format and the
 *    hand-off was never taken. As of 2026-10-21 it is RE-ASSIGNED TO P8, which is
 *    where the per-element repaint that needs it is specified. The reason it is
 *    not wired now is a consumer count, not a design gap: `layer_add` has no
 *    caller anywhere in main/ — no built-in app uses the table (the carousel is
 *    an LVGL chip on lv_layer_top) and the wasm tier that would use it first is
 *    P7 — so a wired caller would be a commit path with nothing ever on it, the
 *    machinery-without-a-consumer refusal D16 already sets. What P5.5's pass line
 *    did prove stands: every compositor primitive reachable through the gate,
 *    refused or accepted for exactly the documented reason, and counted either
 *    way, on the host.
 *
 * This header stays LVGL-free (orb_assets.h's own rule, and the reason the host
 * suite can compile the whole table). The job builder therefore takes the paint
 * context by forward-declared tag only.
 */

#include <stdbool.h>
#include <stdint.h>

#include "orb_assets.h"     /* orb_asset_t, and clk_sprite_t via clock_assets.h */

struct _lv_area_t;
struct gfx_paint_ctx;

/* One layer, as an app asks for it. The field order is the frozen ABI of the
 * layer_* entries in orb_svc.h: a descriptor that grows appends, never reorders.
 * The first field is the resolver's answer, embedded whole — an app that resolved
 * nothing has nothing to add, and no field here can smuggle an address. */
typedef struct orb_layer_desc {
    orb_asset_t art;         /* the resolved slot: cls, tier, box, generator   */
    int16_t     x, y;        /* pivot placement; 0,0 = the resolver's art.x/y  */
    int16_t     pivotx, pivoty;   /* 0,0 = plate centre (the bake's own rule)  */
    int16_t     ang_10;      /* tenths of a degree; the COMPOSITOR turns it    */
    uint8_t     blend;       /* 0 normal, 1 multiply, 2 screen                 */
    uint8_t     show;        /* 0 keeps the row live and draws nothing         */
    uint8_t     role;        /* fps-line attribution only: < CLK_PAINT_ROLES   */
    uint8_t     gen_prim;    /* generator override, an orb_prim_t; 0xFF = use
                              * art.prim. 0xFF rather than 0 because 0 IS a
                              * generator (RECT): an unset field must never pick
                              * a rectangle for a HAND slot (plan §1.2b.6). */
    /* Styling for a generated fallback, read only when art.tier >= 2. The
     * registry carries no colour by design — a theme's palette has no business
     * in a slot record — so the app's own claim lives here, appended. */
    uint32_t    color;       /* RGB565                                        */
    const char *text;        /* TEXT/STATUS: the string; NULL refuses the bake */
    uint16_t    r;           /* RRECT corner, RING radius                     */
    uint16_t    thick;       /* RING stroke, BAR width                        */
    uint8_t     alpha;       /* 0..255 coverage over the whole plate          */
    uint8_t     pad2[3];
} orb_layer_desc_t;

/* Rows per app. Eight: a hand, a ring, a plate, a card and a status line with
 * room to spare. The static cost of the feature is this times the 16 broker rows
 * (about 15 KB of .bss); heap is spent only when an app actually bakes. */
#define ORB_LAYER_MAX 8

/* The largest box a fallback bakes. A whole-panel PLATE is not a fallback — that
 * is a bake's job under D31 — so the cap covers what an app plausibly asks the OS
 * to draw: a hand bar, a ring, a rounded card, a status glyph run. 128x128. */
#define ORB_LAYER_PLANE_MAX (4u + 128u * 128u * 3u)

/* One app's layers. The broker owns this through a PSRAM pointer, allocated on
 * the first layer_add and freed on uninstall. It was embedded BY VALUE until the
 * P5.5 flash measured the cost: 16 rows x 920 B of .bss took 14,720 B from the
 * internal heap, and httpd_start answered ESP_ERR_HTTPD_TASK on the board. The
 * host suite, the compile and the link all passed; only the boot log saw it. A
 * live-frame fact now rides the heap exactly like the planes it points at:
 * PSRAM, paid for only by an app that actually claims rows, and a refusal that
 * costs a layer rather than the config server is the honest shape. */
typedef struct orb_layer_tab {
    orb_layer_desc_t row[ORB_LAYER_MAX];
    clk_sprite_t     spr[ORB_LAYER_MAX];   /* the committed record: tier 1 built
                                            * from the descriptor's geometry,
                                            * tiers 2/3 from the bake. jobs paint
                                            * these, never the descriptors. */
    void            *plane[ORB_LAYER_MAX]; /* baked pixels, row-owned, freed on
                                            * del/wipe. NULL for a theme row.    */
    const uint8_t   *base[ORB_LAYER_MAX];  /* what spr.color_off indexes: NULL
                                            * for a theme row (the caller's
                                            * payload answers at commit), the
                                            * plane for a bake. */
    uint8_t          used[ORB_LAYER_MAX];  /* 0 = free row                       */
    uint8_t          gen[ORB_LAYER_MAX];   /* handle generations, 0 = never given */
    uint8_t          n_live;
    uint8_t          pad[3];
    uint32_t         refuses;              /* every rejection this table answered:
                                            * full, stale, bad art, no heap. The
                                            * broker's ledger counts CALLS at the
                                            * gate; this counts the table's own
                                            * answers, and the two must not be
                                            * confused for each other. */
} orb_layer_tab_t;


/* ---- allocation hook (installed once, by orb_svc_init) ---------------------
 * A fallback's plane is heap: PSRAM on the target, the harness's own on the host.
 * A hook and not a direct call for orb_assets.h's own reason — the ESP headers
 * would put this TU out of the suite's reach, and the suite is what proves the
 * OOM path is a refusal and not a crash. NULL hooks refuse every bake: a board
 * with no heap wired draws themes and says so in its refusal count. */
typedef void *(*orb_layers_alloc_fn)(uint32_t bytes);
typedef void  (*orb_layers_free_fn)(void *p);
void orb_layers_set_alloc(orb_layers_alloc_fn alloc, orb_layers_free_fn free_fn);

/* ---- the four app calls, minus the gate (orb_svc.c owns that) --------------
 * A handle is NOT an index: it carries the row's generation in the high byte, so
 * a set/touch/del against a handle whose row was freed and re-issued refuses
 * instead of painting a stranger's art. 0 is never a handle. */

/** Add `d` as a layer. Returns a handle, or -1: table full, an answer tier that
 *  is not an art (every ORB_ASSET_* refusal passes through: an app that hands
 *  back a failed resolve gets counted, not surprised), an empty or over-cap box,
 *  a bake that will not run, or a plane the heap would not give. */
int  orb_layers_add(orb_layer_tab_t *t, const orb_layer_desc_t *d);

/** Overwrite the live row `h` names, re-baking if the new art needs pixels.
 *  False on a stale handle or an art that will not bake — and a refused set
 *  leaves the row's PREVIOUS art exactly where it was: what was on the glass
 *  stays on the glass, because a half-applied descriptor is the stale-art bug
 *  this table exists to prevent. */
bool orb_layers_set(orb_layer_tab_t *t, int h, const orb_layer_desc_t *d);

/** Read the live row `h` names. False on a stale handle. */
bool orb_layers_get(const orb_layer_tab_t *t, int h, orb_layer_desc_t *out);

/** Mark the rows the layer dirties so the next commit rebuilds them: the
 *  swept box of the row's own sprite, padded by SWEPT_PAD and clipped to the
 *  panel. The area argument of the frozen ABI is the app's claim about its ink;
 *  this is the OS's, and it is the wider, honest one. False on a stale handle. */
bool orb_layers_touch(orb_layer_tab_t *t, int h);

/** Drop the row and free its plane. A second del of the SAME handle refuses
 *  and counts: the generation moved, so the double-free is a caller bug worth
 *  seeing on the refusal line rather than swallowing. */
bool orb_layers_del(orb_layer_tab_t *t, int h);

/* ---- the OS side ---------------------------------------------------------- */

/** Every row gone, every plane freed, generations KEPT: handles an app still
 *  holds must stay dead, and restarting the counter would hand a dead handle
 *  back its old row's identity the moment the app added again. The broker calls
 *  this at uninstall and at suspend — the only two doors a free cap has. */
void orb_layers_wipe(orb_layer_tab_t *t);

int      orb_layers_count(const orb_layer_tab_t *t);
uint32_t orb_layers_refuses(const orb_layer_tab_t *t);

/** Build the job list for the live, shown rows, in table order: paint order IS
 *  list order, and list order is table order (§1.2a). A row that will not paint
 *  (a handle's sprite gone undrawable, or no payload for a theme row) is skipped
 *  and the frame draws without it, never as garbage.
 *
 *  Rows past `jmax` are dropped WITHOUT touching the refusal counter, on
 *  purpose. The counter answers "what did this app ask the OS for that the OS
 *  refused"; a full job array is the compositor's own capacity, not the app's
 *  misbehaviour — the app stayed inside ORB_LAYER_MAX and the dial ran out of
 *  slots beside its own hands and complications. Billing it there would let an
 *  app's ledger row move for a reason the owner controls, which is exactly the
 *  evidence the ledger exists to keep clean. An app must never be blamed for
 *  the size of jc[].
 *
 *  `theme_base` answers for every tier-1 row — the payload of the bundle the
 *  caller resolved against; a baked row carries its own base and ignores it.
 *  Returns the count written to `jc`, at most jmax. */
int orb_layers_jobs(const orb_layer_tab_t *t, const uint8_t *theme_base,
                    struct gfx_paint_ctx *jc, int jmax);

/** The compositor-side fallback entry orb_assets.h has promised since P5.5:
 *  bakes one primitive into the caller's plane and fills the ctx from it, so a
 *  generated plate lands INSIDE the role counters rather than beside them. The
 *  plane must hold orb_asset_prim_bytes(w, h) and stay alive as long as the
 *  ctx — the ctx points into it. role >= CLK_PAINT_ROLES clamps to the aux
 *  bucket: an app's layers are counted, and a rogue byte must not run off the
 *  counter arrays. False = nothing drawable, and the caller skips the job. It
 *  lives here rather than in orb_gfx.c because the seam it IS — bake output to
 *  paint input — needs no LVGL, and the suite below is what proves it. */
bool gfx_prim_ctx_init(struct gfx_paint_ctx *pc, const orb_prim_arg_t *a,
                       uint8_t *plane, uint32_t plane_cap, uint8_t role);

#ifdef __cplusplus
}
#endif

