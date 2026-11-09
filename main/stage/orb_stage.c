/**
 * orb_stage.c — themed ANALOG clock (Steam Punk et al.), ported from the
 * orb-os (Arduino) theme compositor onto LVGL 9 / ESP-IDF.
 *
 * The dial is composed as LVGL sibling objects rather than one CPU canvas:
 *
 *   plate (static RGB565 lv_image, never invalidated)
 *     -> per hand, back-to-front in the theme's order:
 *          shadow (planar RGB565A8 lv_image, rotated about the hand pivot)
 *          hand   (planar RGB565A8 lv_image, rotated about the hand pivot)
 *     -> overlay (hub cap), drawn last / on top
 *        (the overlay is where a baked-on GLASS PNG lives: one full-coverage
 *        A8 layer composited above everything, e.g. watch-kit glass+bezel
 *        merged at bake time — this theme's is exactly that)
 *  The second layer may instead be an ANIMATED hand: a --tourbillon frame
 *  bank (clk_theme.tourbillon[]) swapped by wall-clock frame and rotated by
 *  the seconds angle like any other hand (see tourbillon_advance).
 *
 * A shadow belongs to its own hand and sits directly under IT, not under every
 * hand: the minute hand casts onto the plane the second hand lives on, so the
 * minute shadow must overpaint the second hand it falls across. (orb-os lays
 * all the shadows down first; that reads as a smudge across a hand the theme
 * orders below, so this port interleaves instead — see stage_init and
 * the layer table's depth sort, which must and do agree on the stack.)
 *
 * Because LVGL runs in LV_DISPLAY_RENDER_MODE_PARTIAL it renders and flushes
 * only the invalidated rectangle. The hands are baked TRIMMED to their opaque
 * ink bounding box (tools/bake_clock.py), so rotating one dirties only its
 * swept wedge — not the whole 466x466 page. The plate is drawn once and never
 * re-flushed while it is static. This is what makes the dial cheap to animate,
 * and it is the whole reason the source PNGs (a 542x542 canvas that is 3.4% ink
 * for the minute hand) must be cropped before they reach LVGL.
 *
 * Geometry — per-hand pivot, screen centre, blend, draw order, shadow offset,
 * sweep/railway, plate-follow, background animation — comes from a generated
 * header (assets/clock_assets.h) produced by tools/bake_clock.py from the
 * theme's clock_style.json, so any theme folder drops in without touching the C.
 * The JSON is parsed OFFLINE; there is no runtime JSON on the device.
 *
 * Both the second tick and the (optional) smooth-sweep beat run as LVGL timers,
 * i.e. inside lv_timer_handler on the one LVGL task — the only context allowed
 * to touch widgets.
 *
 * Fidelity notes / known gaps for THIS build:
 *   - blend 2 ("screen") has no LVGL image blend mode -> mapped to ADDITIVE
 *     with a one-shot warning. (Steam Punk uses blend 0 everywhere.)
 *   - Text banners (text1/text2) are not drawn. Steam Punk sets show=false on
 *     both, and the theme's custom baked fonts / curved / glow text need a font
 *     pipeline this port does not yet carry. A theme that turns them on will
 *     show the dial without the words. Tracked as a follow-on.
 *
 * Background modes (clk_theme.bg_mode, chosen by tools/bake_clock.py):
 *   0 = classic theme plate + baked bgAnim plate frames (full-page source
 *       swaps; kept for non-GIF themes).
 *   1 = GIF DELTA BANK: one full keyframe + one changed-region rect per frame,
 *       §37 included: frame 0's rect is the closing delta that returns the
 *       wrapped canvas to the keyframe, so the chain is a cycle and every tick
 *       applies a delta (packed offline against the simulated device canvas,
 *       perceptual threshold). The keyframe is copied into a PSRAM canvas at init; the
 *       plate widget is an RGB565 image whose data IS that canvas. Each tick
 *       memcpy's the next frame's rects into the canvas and invalidates just
 *       those areas, so LVGL renders + flushes only the changed background
 *       plus the wedges the rotating hands cover/uncover — never a full-page
 *       source swap. The tick runs as fast as the flush pipeline allows
 *       (target 25 fps, self-throttled); a per-second log reports the
 *       achieved rate and average flush cost via display_bench_snapshot.
 */

#include "orb_disc.h"   /* a glass overlay stored as the panel's disc */
#include "orb_stage.h"
#include "orb_label.h"   /* theme-text-plan §2.3: a label's pixels */
#include "board_config.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   /* stage_bench_live waits a frame */
#include "freertos/semphr.h" /* the back copies' mutex (OS v1.2.38) */
#include "lvgl.h"
#include "display.h"
#include "orb_gfx.h"   /* the OS compositor: gfx_commit() paints the layers this file picks */
#include "orb_sched.h" /* the delta bank + frame tick: the canvas, the plate frames, the ladder */
#include "orb_map_credit.h"  /* the live bench draws a map as an app would */
#include "orb_ui.h"      /* §43: the stage builds on its app's own page */
#include "orb_app_wasm.h" /* orb_wasm_tick: the app's frame, run from the scheduler */
#include "orb_svc.h"    /* orb_time_now(): the dial's wall second, OS-owned since the P3 time commit */
#include "orb_layers.h" /* orb_layer_desc_t + orb_asset_class_prim: the rows clock_layers_build claims */
#include "orb_elems.h"  /* orb_elems_at: the element dialect's reader — Migration 2's first caller */
#include "orb_clock3.h" /* the section projection the stage draws (clk_sound_t, CLK_*) */

#include "clk_types.h"   /* clk_theme, clk_sprite_t, clk_bg_frames, CLK_BLOB_BYTES */
/* Second, deliberately: this header #defines the names clock_assets.h just defined
 * as objects, so including it before would expand a macro inside the generated
 * struct definition. Everything below this line reads the theme through
 * orb_theme_geo() — which is the whole firmware change for a theme that arrives on
 * an SD card instead of in the build. */
#include "orb_theme_geo.h"
#include "art_bank.h"       /* the bank the above offsets index into */
#include "orb_files.h"      /* the source this init re-declares through orb_theme_src_register() */
#include "orb_sd.h"         /* ORB_SD_THEME_DIR, the directory that source names */
#include "orb_theme.h"      /* orb_theme_src_register(), and the geo the dial draws */
#include "orb_sound.h"      /* D39: the engine the sound policy below schedules into */
#include "orb_glass.h"    /* §6.21: the glass buffers' own region */

static const char *TAG = "clock";

static lv_obj_t *s_scr;
static lv_obj_t *s_overlay;
static lv_image_dsc_t s_overlay_dsc;

/* ---- D42: the moving rows (clk_row_t, docs/history/element-model-plan.md) ----------
 * Every hand, pointer, ring, still layer, shadow twin and the carriage is one row,
 * and this is each row's tick state: its widget for the classic path, the pose it
 * shows now (an angle in tenths and the pivot's travel in px), the sprite at that
 * travel, and its ease. The L4 hook paints from the poses, the classic path from
 * the widgets, and both read ONE truth per tick. PSRAM, once: 32 rows of widget
 * descriptors and poses would sit on the internal heap boundary httpd_start needs
 * (the 2026-10-24 lesson the layer keys below record), and a row is read a handful
 * of times a frame. ROW_HIDDEN is the nothing-drawn mark; no pose reaches it. */

typedef struct {
    lv_obj_t      *obj;
    lv_image_dsc_t dsc;
    int32_t        ang_10;     /* the angle shown, tenths; ROW_HIDDEN = not drawn */
    int16_t        ox, oy;     /* the pivot's travel shown, px                    */
    clk_sprite_t   pose;       /* the row's sprite moved by (ox, oy)              */
    bool           logged;     /* "no input" printed for this row                 */
    int16_t        frame;      /* the picked frame shown (theme-pool-plan §2.6)   */
    uint8_t        opa;        /* the opacity shown (theme-text-plan T2)          */
    clk_sprite_t   fspr;       /* a picked row's sprite at that frame             */
    uint8_t       *tplane;     /* a label's composed plane (theme-text-plan §2.3),
                                * PSRAM: its panel with the app's string over it  */
    uint32_t       tseq;       /* the string it shows: s_text->seq at compose     */
} row_state_t;
static row_state_t *s_row;
#define ROW_HIDDEN INT32_MIN

/* §43: what the app posted this frame (stage_pose / stage_comp_set) and the reading it
 * posted it from (stage_now). A row the app never posed stays hidden. */
static int32_t s_g_ang[CLK_ROW_MAX];
static int16_t s_g_ox[CLK_ROW_MAX], s_g_oy[CLK_ROW_MAX];
static int16_t s_g_frame[CLK_ROW_MAX];   /* the frame the app picked (stage_frame) */
static uint8_t s_g_opa[CLK_ROW_MAX];     /* the opacity the app gave (stage_opacity) */

/* ---- theme-text-plan §2.3: fonts and labels --------------------------------------------
 * A font is a TEXT row of glyph cells in the charset's order; a label is a text row whose
 * plane the stage composes from its panel and the app's string. The fonts, each label's
 * fonts and padding, and the app's pending strings live here, PSRAM, once: a label is
 * read when its string changes, not every frame. */
#define LABEL_TEXT_MAX 192
/* A label's pixels start this far into its plane: a sprite at offset 0 reads as unset
 * (gfx_sprite_drawable), the rule the layer table's own bakes keep with their 4-byte lead
 * (ORB_LAYER_PLANE_MAX). */
#define LABEL_PLANE_OFF 4u
typedef struct {
    orb_font_t font[CLK_FONT_SLOTS];     /* glyphs found on first use (orb_label.h) */
    uint32_t frame0[CLK_FONT_SLOTS];     /* each font's first glyph, catalogue row  */
    bool     present[CLK_FONT_SLOTS];
    uint8_t  tone_font[CLK_ROW_MAX][4];  /* a label's fonts by tone; CLK_FONT_NONE  */
    uint8_t  pad[CLK_ROW_MAX];
    char     text[CLK_ROW_MAX][LABEL_TEXT_MAX];   /* the app's string, pending     */
    uint8_t  tone[CLK_ROW_MAX];
    volatile uint32_t seq[CLK_ROW_MAX];  /* bumped after each new string lands    */
} text_state_t;
static text_state_t *s_text;
static const orb_bundle_sec_t *s_text_sec;   /* the section the fonts' rows index */
static int32_t s_g_cval[CLK_COMP_MAX];
static uint8_t s_g_clive[CLK_COMP_MAX];
static time_t  s_now_t;
static double  s_now_frac, s_now_wall;
static bool    s_base;            /* no resident theme: the stage draws nothing */
static bool    s_opened;          /* built for s_lay_app (theme-pool-plan P4: one app at a time) */
static lv_obj_t *s_plate_obj;     /* the plate widget, so a release can take it down */
static uint32_t s_stage_objs;     /* how many of the page's children the stage built */

/* radar-plan §2.9 (R3c): the live layers. Up to two full-glass RGB565A8 planes the bound
 * app draws into, each stacked at a depth on its rows' scale (depth_key(order, false)): a
 * row in the layer table like a label, whose plane the stage owns, and on the classic path
 * an image at the same place in the page's children. Two, so a radar keeps its map in one
 * and redraws only the rain in the other. At one depth, layer 0 is under layer 1. PSRAM,
 * 651 KB each, freed on release. */
#define LIVE_OFF  16u
#define LIVE_PICK 0xFFFFu          /* a layer's pick in clock_layers_build: no theme class */
static uint8_t *s_live[STAGE_LIVE_N];
/* OS v1.2.38 (radar-plan §6.27): each layer's back copy, drawn by the live worker
 * (orb_live_work.c) off the frame path and swapped in whole by stage_live_present. The
 * worker holds s_back_mux while it draws; whatever frees a layer or its back takes it too. */
static uint8_t *s_live_back[STAGE_LIVE_N];
static SemaphoreHandle_t s_back_mux;
static void back_lock(void)
{
    if (!s_back_mux) s_back_mux = xSemaphoreCreateMutex();
    if (s_back_mux) xSemaphoreTake(s_back_mux, portMAX_DELAY);
}
static void back_unlock(void) { if (s_back_mux) xSemaphoreGive(s_back_mux); }
static void back_free_all(void)
{
    back_lock();
    for (int l = 0; l < STAGE_LIVE_N; l++) { orb_glass_free(s_live_back[l]); s_live_back[l] = NULL; }
    back_unlock();
}
static int32_t s_live_order[STAGE_LIVE_N] = { -1, -1 };
static lv_obj_t *s_live_obj[STAGE_LIVE_N];
static lv_image_dsc_t s_live_dsc[STAGE_LIVE_N];
/* radar-plan §6.19: layer 1 is layer 0's copy with the app's drawing on it, every pixel
 * covering, so layer 0 is hidden (still a draw target: the backing) and layer 1 copied,
 * not blended. Off whenever the layers go. */
static bool s_live_merged;
/* radar-plan §7.9 (OS v1.6.1): each layer's own format, merged or not. Colour and alpha is the
 * default; opaque is colour alone (a layer that covers, radar's map: 434 KB); indexed is a value
 * a pixel and the picture's palette (radar's rain: 218 KB). Layout of an indexed buffer, after
 * the head: the palette as the compositor reads it (RGB565, then alpha), the values in use, the
 * palette as LVGL's I8 wants it (ARGB8888, right before the values), then the values. */
enum { LF_RGBA = 0, LF_RGB = 1, LF_I8 = 2 };
static uint8_t s_live_fmt[STAGE_LIVE_N];
#define LIVE_I8_565   LIVE_OFF
#define LIVE_I8_A     (LIVE_I8_565 + 512u)
#define LIVE_I8_USED  (LIVE_I8_A + 256u)
#define LIVE_I8_ARGB  (LIVE_I8_USED + 32u)
#define LIVE_I8_IDX   (LIVE_I8_ARGB + 1024u)
_Static_assert(LIVE_I8_IDX - LIVE_I8_565 == GFX_I8_C565_BACK && LIVE_I8_IDX - LIVE_I8_A == GFX_I8_A_BACK,
               "the compositor finds an indexed job's palette behind its values");
static int live_fmt(int l) { return s_live_merged ? LF_RGB : s_live_fmt[l]; }
/* The buffer's palette, for an indexed target */
static void live_pal(uint8_t *buf, orb_ui_pal_t *p)
{
    p->c565 = (uint16_t *)(void *)(buf + LIVE_I8_565);
    p->a    = buf + LIVE_I8_A;
    p->used = buf + LIVE_I8_USED;
    p->argb = (uint32_t *)(void *)(buf + LIVE_I8_ARGB);
}
/* The pointer a layer's job holds as its alpha (pre-paint's key, the swap's identity): the
 * values of an indexed buffer, else the alpha plane's place. */
static const uint8_t *live_id(int l, const uint8_t *buf)
{
    if (!buf) return NULL;
    return live_fmt(l) == LF_I8 ? buf + LIVE_I8_IDX : buf + LIVE_OFF + 2 * (size_t)CLK_SCR_W * CLK_SCR_H;
}
static void live_drop_all(void);

/* CLK_BG_DIRECT_PUSH, the L3/L4 switch the block below and the scheduler's delta
 * push both key on, lives in orb_sched.h: one number, both TUs, so the push and
 * the hook can never disagree about who owns a row. */


/* GIF-mode beat state (compose_now): tick counter, the true seconds it was last
 * re-anchored to, and the wall fraction held between beats. */
static uint32_t         s_bt_ctr = 0;
static int              s_bt_sec = -1;
static double           s_bt_frac = 0.0;

/* ---- tourbillon: an animated second hand (frame bank + device rotation) ----
 * The kit's seconds layer is N full-canvas frames that share geometry and
 * differ only in internal motion (an oscillating balance); the ORBIT around
 * the dial centre is the same rotation every hand already gets, driven by the
 * seconds angle (the kit manifest's seconds/60 x 360 formula). Between frame
 * swaps only the pixels inside the baked diff rect vs the PREVIOUS frame can
 * differ, so the per-tick re-mark is that rotated diff box — not the
 * carriage's full swept box, which would re-composite ~40% of the page every
 * tick and re-create the full-page plateau the static re-mark was retired
 * over (docs/history/rows-capacity-2026-10-10.md). */
static int32_t        s_tourb_shown = -1;   /* frame composited now, -1 = never */
static uint32_t       s_tourb_t0;           /* lv_tick ms the frame clock started */
static lv_image_dsc_t s_tourb_dsc[CLK_TOURB_MAX];

/* Orbit marks (sweep perf). The generic hand marking would take the union
 * of the two rotated bounding boxes of the carriage (~260x350 swung around
 * the dial centre): most of the page on EVERY tick, so the commit hook
 * restores + re-composites ~330k px of dial+glass for a carriage that only
 * touches ~75k of them — that is what held sweep at 5 fps once the DMA
 * itself was gated away. Instead each sweep tick marks the rotated ink
 * footprint of both poses: every source row's ink extent (x0,x1) is kept
 * per frame (PSRAM chords, built on first use), rotated as a line segment
 * with 16.16 fixed-point trig, and accumulated into per-device-row x-spans
 * — marks are as wide as the ink and no wider. The commit band's flush gate
 * (orb_gfx.c, fed by the scheduler's table commit) then skips DMA for rows
 * whose rebuild came out byte-identical anyway. If the chord tables cannot be built the caller
 * falls back to the swept-box union. */
static uint16_t *s_tourb_cx0[CLK_TOURB_MAX];
static uint16_t *s_tourb_cx1[CLK_TOURB_MAX];

/* One stamp pass's device-row span accumulation (x1 < x0 = untouched row).
 * 2026-10-24: PSRAM, lazy in tourbillon_ink_mark — 2×932 B of .bss here sat
 * on the heap boundary inside the one contiguous block httpd_start needs.
 * The stamp pass already reads the chord tables from PSRAM (s_tourb_cx0/1),
 * so this rides the same bus at the same rate; a failed alloc returns false,
 * the same answer the chord model gives when it cannot build. */
static int16_t *s_wp_x0, *s_wp_x1;
static int32_t  s_wp_lo, s_wp_hi;



static bool     s_warned_screen = false;

/* Mapped base of the art bank, filled in by stage_init() (NULL until then).
 * Kept as a file static rather than calling art_bank_ptr() on every sprite read:
 * blob() is on the per-frame paint path and this keeps it a single load. */
static const uint8_t *s_art;

/* Every pixel of the dial comes from here: the bank now lives in the `assets`
 * flash partition and is mapped once at boot by art_bank_init() below, so this
 * stays a bare pointer return and sprite reads cost exactly what they did when
 * the bank was linked into .rodata. See main/art/art_bank.h. */
static const uint8_t *blob(void) { return s_art; }

/* The tick's policy callback (below) reads the time through it; defined with the
 * compose block further down, like tick_base_ms() used to be declared for
 * tick_timer_cb. */
static void compose_now(void);
static void clock_layers_refresh(void);








/* Map an orb-os blend (0 normal, 1 multiply, 2 screen) onto LVGL's image blend. */
static lv_blend_mode_t map_blend(uint8_t b)
{
    switch (b) {
        case 1:  return LV_BLEND_MODE_MULTIPLY;
        case 2:
            if (!s_warned_screen) {
                ESP_LOGW(TAG, "blend==2 (screen) has no LVGL image mode; using ADDITIVE");
                s_warned_screen = true;
            }
            return LV_BLEND_MODE_ADDITIVE;
        default: return LV_BLEND_MODE_NORMAL;
    }
}



/* Hand AA by capability, read from the bake's own motion words (mirrored and
 * truth-tabled by tools/host_hand_aa_test.py). The dial affords bilinear on a
 * layer when that layer's 4-tap cost cannot land on a tick that is already
 * busy: an animated carrier (plate bank, delta bank, carriage) re-composites
 * the wedge every tick, and 4-tap sampling over that re-composite is the
 * ~40 ms/frame the Oct-10 findings measured, so those dials keep every layer
 * nearest exactly as before. On a still dial the tick idles between wall
 * beats, and a layer that re-poses at most once a second costs one 4-tap
 * wedge per second inside a flush budget built for the whole page — so its
 * hands may take the filter. The exception is the layer whose beat IS the hot
 * tick: a sweeping second hand stays nearest so a sweeping still dial (the
 * Station case) never pays the filter on the tick that drives its fps.
 *
 * A dial the painter owns answers from the bake instead (the unified render path's
 * slice 5): the row's EF_SMOOTH bit, which the baker sets where the commit's price
 * WITH the AA term still holds 12 fps and never on the hot-tick seconds. That one
 * answer drives both renderers — make_layer's LVGL widget here and the painter's
 * sampling through the row's layer descriptor (clk_row_desc) — because the widgets
 * stay live as the painter's fallback: if the two disagreed, any LVGL-initiated
 * repaint (the activation bounce, an overlay closing) would redraw a hand with the
 * other edge until it next moved. The test is the restore source, not the L4 gate,
 * so a painter-owned dial whose table the gate refused still reads its flag. No bit
 * (an older theme, the base face) reads as nearest. A CLK_BG_DIRECT_PUSH 0 build
 * has no painter and keeps the motion-word rule above whole. `src` names which rule
 * answered, for the boot line. */
#define CLK_EF_SMOOTH 0x80u    /* theme_schema EF_SMOOTH */
static bool row_smooth(uint8_t cls, uint16_t slot)
{
    const orb_bundle_sec_t *sec = NULL;
    uint32_t off = 0;
    if (!orb_theme_live_sec(orb_theme_live_slug(), &sec, &off)) return false;
    for (uint32_t i = 0; i < sec->n_elem; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        if (e.cls == cls && e.slot == slot) return (e.flags & CLK_EF_SMOOTH) != 0;
    }
    return false;
}

static bool row_aa(const clk_theme_t *t, const clk_row_t *r, const char **src)
{
#if CLK_BG_DIRECT_PUSH
    if (orb_sched_restore_src() != NULL) {
        if (src) *src = "flag";
        return row_smooth(ORB_ASSET_AUX, r->tag);
    }
#endif
    if (src) *src = "classic-rule";
    if (t->bg_mode != 0 || t->bg_frames != 0 || t->gif_frames != 0 ||
        t->tourbillon_n != 0)
        return false;
    if (r->src == CLK_SRC_SECOND &&
        (r->cadence == CLK_CAD_SWEEP || r->cadence == CLK_CAD_STOP_TO_GO))
        return false;
    return true;
}

/* Create one rotated-hand/shadow layer from a sprite record. The object is
 * placed so its stored pivot (in cropped coords) lands on the sprite's screen
 * centre; LVGL then rotates it about that pivot. `aa` carries the sampling
 * decision for THIS layer from row_aa() above: LVGL defaults rotation to
 * 4-tap bilinear, ~4x the per-px cost of the nearest path on the S3, and the
 * old build forced nearest on every dial because the hands-over-animated-orb
 * re-composite could not carry it. The bake's motion words now say which
 * dials can, per layer. A layer that never rotates (the overlay) takes
 * nearest for free: LVGL skips the transform outright at angle 0. */
static lv_obj_t *make_layer_at(const clk_sprite_t *spr, lv_image_dsc_t *dsc,
                               bool aa, const uint8_t *base)
{
    if (!gfx_sprite_drawable(spr)) return NULL;

    gfx_fill_sprite_dsc(dsc, spr, base);

    lv_obj_t *o = lv_image_create(s_scr);
    lv_obj_set_size(o, spr->w, spr->h);
    lv_obj_set_align(o, LV_ALIGN_TOP_LEFT);
    lv_obj_set_scrollable(o, false);
    lv_image_set_src(o, dsc);
    lv_image_set_pivot(o, spr->pivotx, spr->pivoty);
    lv_obj_set_pos(o, spr->centerx - spr->pivotx, spr->centery - spr->pivoty);
    lv_image_set_blend_mode(o, map_blend(spr->blend));
    lv_image_set_antialias(o, aa);
    return o;
}

static lv_obj_t *make_layer(const clk_sprite_t *spr, lv_image_dsc_t *dsc, bool aa)
{
    return make_layer_at(spr, dsc, aa, blob());
}

/* OS v1.2.12: a glass overlay stored as the panel's masked disc (orb_disc.h) is expanded
 * into an RGB565A8 square here, so the LVGL layer and the L4 glass row both read a square
 * (corners clear). One buffer, kept across binds, refilled by each. */
static uint8_t *s_overlay_sq;
/* The square sits this far into its buffer: a layer row's colour offset of 0 is the
 * table's "unset" signature (orb_layers.c desc_ok), so the pixels start past it, as a
 * label's do at LABEL_PLANE_OFF. */
#define OVERLAY_SQ_OFF 16u

static const uint8_t *overlay_square(void)
{
    if (!clk_theme.overlay_disc) return NULL;
    if (!s_overlay_sq)
        s_overlay_sq = orb_glass_alloc(OVERLAY_SQ_OFF + (size_t)CLK_SCR_W * CLK_SCR_H * 3u);
    if (!s_overlay_sq) {
        ESP_LOGE(TAG, "disc glass: no PSRAM for its square; the glass is left off");
        return NULL;
    }
    orb_disc_expand_masked(blob() + clk_theme.overlay.color_off, s_overlay_sq + OVERLAY_SQ_OFF,
                           CLK_SCR_W);
    return s_overlay_sq;
}

/* Ticks per hand beat in GIF mode: theme tickRate (beats/s) against the
 * beat the tick actually delivers. 4 bps on an 83 ms tick -> 3 ticks. */
static uint32_t hand_beat_ticks(void)
{
    int rate = clk_theme.tick_rate;
    if (rate < 1) rate = 1;
    const uint32_t t = orb_bg_gif_tick_ms();
    const uint32_t n = (uint32_t)((1000.0 / (double)rate) / (double)t + 0.5);
    return n < 1 ? 1 : n;
}

/* ---- D41: ease -------------------------------------------------------------
 * A settle: after a jump (or a step slower than the second) the row overshoots its
 * new pose by a small amount and comes back, sin(pi*tau) over `ease` ms. A flyback:
 * when a flyback row returns to P0, it travels back over `ease` ms (smoothstep)
 * instead of landing in one frame. Both run on frames the dial asks for
 * (orb_sched_burst) only while they last; a row without an ease costs nothing.
 *
 * ease_pose is pure: the caller passes the clock in, so the host bench drives it. The
 * rows ease their fraction t (row_ease), so `turn` is 1 on a wrapping row, whose
 * fraction wraps, and 0 on every other. */





/* D41 §3: one reading of the wall clock, handed to the drive function for every row
 * a frame poses. frac is the wall fraction, beat-held on a delta-bank dial. */

/* Sweep marking by the carriage's rotated ink chords; defined with the tourbillon
 * block below. Returns true when it placed the marks for both poses, false when the
 * caller must fall back to the swept-box union (no bank, no prior composite, or the
 * classic path). */
static bool tourbillon_ink_mark(int f, int32_t prev_ang_10, int32_t new_ang_10);
static void tourb_mark(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out);

/* A row's sprite with its pivot moved by a travel: the record every mark and every
 * paint of a travelled pose reads, so a mark and a paint never disagree. */
static void pose_spr(const clk_sprite_t *s, int16_t ox, int16_t oy, clk_sprite_t *out)
{
    *out = *s;
    out->centerx = (int16_t)(out->centerx + ox);
    out->centery = (int16_t)(out->centery + oy);
}

/* ---- tourbillon: pick the wall-clock frame and swap it in -----------------
 * The only pixels that can differ after a frame swap are the changed rects of
 * the OUTGOING and INCOMING frame at the current angle (both are chained
 * against neighbours by the baker, like the delta bank); a first-ever draw has
 * no prior composite of this layer, so it takes the full swept box. The angle
 * changes (the orbit step) are marked by rows_update itself, exactly as for
 * any other hand. */
static void tourb_mark(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out)
{
    if (spr->diffw && spr->diffh)
        gfx_swept_rect_box(spr, ang_10, spr->diffx, spr->diffy,
                       (int32_t)spr->diffx + (int32_t)spr->diffw - 1,
                       (int32_t)spr->diffy + (int32_t)spr->diffh - 1, out);
    else
        gfx_swept_box(spr, ang_10 == INT32_MIN ? 0 : ang_10, out);
}

/* Build frame f's per-source-row ink chords (min/max x with alpha>0), once.
 * Sequential alpha-plane scan (~90 kB from PSRAM per frame), lazy so a theme
 * only pays for the frames it shows. Returns false = no table: box fallback. */
static bool tourb_chords(int f)
{
    if (f < 0 || f >= (int)clk_theme.tourbillon_n || f >= CLK_TOURB_MAX)
        return false;
    if (s_tourb_cx0[f]) return true;
    const clk_sprite_t *s = &clk_theme.tourbillon[f];
    if (!gfx_sprite_drawable(s) || !s->w || !s->h) return false;

    const size_t sz = (size_t)s->h * sizeof(uint16_t);
    uint16_t *c0 = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *c1 = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c0 || !c1) {
        if (c0) heap_caps_free(c0);
        if (c1) heap_caps_free(c1);
        return false;
    }
    const uint8_t *alp = blob() + s->color_off + (size_t)s->w * s->h * 2;
    for (int32_t y = 0; y < (int32_t)s->h; y++) {
        const uint8_t *row = alp + (size_t)y * s->w;
        int32_t lo = -1, hi = -1;
        for (int32_t x = 0; x < (int32_t)s->w; x++)
            if (row[x]) { lo = x; break; }
        for (int32_t x = (int32_t)s->w - 1; x >= 0; x--)
            if (row[x]) { hi = x; break; }
        c0[y] = (uint16_t)(lo < 0 ? 1 : lo);   /* empty row: c1 < c0 */
        c1[y] = (uint16_t)(hi < 0 ? 0 : hi);
    }
    s_tourb_cx0[f] = c0;
    s_tourb_cx1[f] = c1;
    return true;
}

/* Rotate frame f's source-row chords by ang_10 (TENTHS of a degree, the
 * gfx_swept_rect_box convention) and accumulate the device rows each segment
 * passes through into the s_wp_* spans. All-integer inner loop — the earlier
 * double-trig, gfx_mark_rows-per-segment version cost ~90k calls per tick at the
 * worst carriage angle and starved the tick task to 1-2 fps with an
 * angle-periodic wave; the same work done here is tens of thousands of ALU
 * ops into static memory.
 *
 * The forward rotation reuses paint_layer's exact trig pair (gfx_rot_sin_cos),
 * operation for operation, so the stamped pose cannot disagree with what the
 * commit paints: any residual is nearest-neighbour texel spread (±1 px/row),
 * inside the pads. */
static void tourb_stamp_pose(const clk_sprite_t *spr, int f, int32_t ang_10)
{
    const uint16_t *c0 = s_tourb_cx0[f], *c1 = s_tourb_cx1[f];
    /* Forward map = transpose of paint_layer's inverse map: sin(+a) =
     * -sinma(-a), cos(+a) = cosma(-a). The products below land in a >> 16,
     * so the pair must be sin/cos in 16.16 (1.0 == 65536): sinma is LVGL's
     * /1024 value, hence *64. (The first builds used gfx_trigo_sin's /32768
     * table — or sinma*32 — straight into the >>16 form: the stamped pose
     * sat at HALF radius, so the carriage's outer ring never got marked,
     * never got restored or repainted, and ghosted through the whole orbit
     * while the double-math diff boxes rebuilt a crisp rectangle around the
     * escapement against it — the exact three reported artifacts. LVGL's
     * own consumers pair lv_trigo_sin with >> 15, not >> 16.) */
    int32_t sma, cma;
    gfx_rot_sin_cos(ang_10, &sma, &cma);
    const int32_t si = -sma * 64, co = cma * 64;
    const int32_t pvx = (int32_t)spr->pivotx, pvy = (int32_t)spr->pivoty;
    const int32_t cxs = (int32_t)spr->centerx, cys = (int32_t)spr->centery;
    const int32_t rpad = 1 + ((int32_t)spr->w >> 7);   /* texel spread at row ends */

    for (int32_t ys = 0; ys < (int32_t)spr->h; ys++) {
        if (c1[ys] < c0[ys]) continue;                /* row carries no ink */
        const int32_t dy  = ys - pvy;
        const int32_t dxa = (int32_t)c0[ys] - pvx;
        const int32_t dxb = (int32_t)c1[ys] - pvx;
        const int32_t ax = cxs + ((co*dxa - si*dy) >> 16);
        const int32_t ay = cys + ((si*dxa + co*dy) >> 16);
        const int32_t bx = cxs + ((co*dxb - si*dy) >> 16);
        const int32_t by = cys + ((si*dxb + co*dy) >> 16);

        const bool up = (ay <= by);
        const int32_t ya  = up ? ay : by,  yb  = up ? by : ay;
        const int32_t xya = up ? ax : bx,  xyb = up ? bx : ax;
        const int32_t den = yb - ya;
        const int32_t y0 = ya - rpad, y1 = yb + rpad;

        const int32_t adx = xyb - xya >= 0 ? xyb - xya : xya - xyb;
        int32_t x16, step16;
        if (den == 0 || adx > den) {
            /* Shallow segment (horizontal included): the source row's band
             * lands across the FULL chord width on essentially the same
             * device rows, so the row-walk — one ~7 px window per row —
             * would mark a sliver of it and ghost the rest (this is what
             * smeared the counterweight tail whenever the carriage rode near
             * 12 o'clock). Mark the full span on every row the segment
             * (plus its texel pad) touches: the pad rows beyond the ends
             * still catch corner-round-off ink several px outside an
             * endpoint window — the sim rejects anything narrower. */
            const int32_t xl = (xya < xyb ? xya : xyb) - SWEPT_PAD;
            const int32_t xr = (xya < xyb ? xyb : xya) + SWEPT_PAD;
            for (int32_t y = y0; y <= y1; y++) {
                if (y < 0 || y >= CLK_SCR_H) continue;
                if ((int16_t)xl < s_wp_x0[y]) s_wp_x0[y] = (int16_t)xl;
                if ((int16_t)xr > s_wp_x1[y]) s_wp_x1[y] = (int16_t)xr;
                if (y < s_wp_lo) s_wp_lo = y;
                if (y > s_wp_hi) s_wp_hi = y;
            }
            continue;
        }
        step16 = ((xyb - xya) << 16) / den;          /* x travel per device row */
        x16 = (xya << 16) - step16 * rpad;           /* back up to the first pad row */
        for (int32_t y = y0; y <= y1; y++) {
            const int32_t q0 = x16 >> 16, q1 = (x16 + step16) >> 16;
            x16 += step16;
            if (y < 0 || y >= CLK_SCR_H) continue;
            const int32_t xl = (q0 < q1 ? q0 : q1) - SWEPT_PAD;
            const int32_t xr = (q0 < q1 ? q1 : q0) + SWEPT_PAD;
            if ((int16_t)xl < s_wp_x0[y]) s_wp_x0[y] = (int16_t)xl;
            if ((int16_t)xr > s_wp_x1[y]) s_wp_x1[y] = (int16_t)xr;
            if (y < s_wp_lo) s_wp_lo = y;
            if (y > s_wp_hi) s_wp_hi = y;
        }
    }
}

/* The sweeping carriage's mark for one tick: the union of the poses' rotated
 * ink for frame f, released in <= 466 gfx_mark_rows calls. prev_ang_10 ==
 * INT32_MIN stamps only the new pose (the frame swap calls it that way: the
 * old frame's ink was already marked at this angle by its own moves, and the
 * INCOMING frame's ink — the balance at its new position — is what the baked
 * diff rect can fall a few pixels short of, which showed up as a visible
 * rectangle around the escapement). The rest of the frame swap's own pixels
 * are still marked by tourbillon_advance's diff boxes; chords plus those
 * boxes cover every pixel that can differ on the glass — everything the old
 * full-box union touched beyond them was dial and glass re-blended onto
 * identical output. */
static bool tourbillon_ink_mark(int f, int32_t prev_ang_10, int32_t new_ang_10)
{
#if CLK_BG_DIRECT_PUSH
    if (!orb_sched_l4_active()) return false;
#endif
    /* The chord model covers the carriage's own art. It used to decline on a
     * shadowed second layer because the bank WAS that layer and its shadow
     * moved with it; since ruling C the shadow belongs to the stationary
     * needle, so a moving frame leaves it untouched and the chords stand. */
    if (clk_theme.tourbillon_n <= 0) return false;
    if (f < 0 || f >= (int)clk_theme.tourbillon_n || f >= CLK_TOURB_MAX)
        return false;                          /* no composite on the glass yet */
    if (!tourb_chords(f)) return false;        /* fall back to the box union */
    if (!s_wp_x0) {
        s_wp_x0 = heap_caps_malloc(sizeof *s_wp_x0 * CLK_SCR_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_wp_x1 = heap_caps_malloc(sizeof *s_wp_x1 * CLK_SCR_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_wp_x0 || !s_wp_x1) {
            heap_caps_free(s_wp_x0); s_wp_x0 = NULL;   /* the chord fallback's own
                                                        * shape: box union paints */
            s_wp_x1 = NULL;
            return false;
        }
    }

    for (int32_t y = 0; y < CLK_SCR_H; y++) {          /* empty spans */
        s_wp_x0[y] = 0x7fff;
        s_wp_x1[y] = (int16_t)-0x8000;
    }
    s_wp_lo = CLK_SCR_H; s_wp_hi = -1;
    if (prev_ang_10 != INT32_MIN)
        tourb_stamp_pose(&clk_theme.tourbillon[f], f, prev_ang_10);
    tourb_stamp_pose(&clk_theme.tourbillon[f], f, new_ang_10);
    if (s_wp_hi < s_wp_lo) return true;        /* zero-ink bank: nothing to mark */

    for (int32_t y = s_wp_lo; y <= s_wp_hi; y++)
        if (s_wp_x1[y] >= s_wp_x0[y])
            gfx_mark_rows(y, y, s_wp_x0[y], s_wp_x1[y], false);
    return true;
}

static void tourbillon_advance(void)
{
    const int n = (int)clk_theme.tourbillon_n;
    const uint8_t cr = clk_theme.car_row;
    if (n <= 0 || cr >= CLK_ROW_MAX || !s_row || !s_row[cr].obj) return;
    row_state_t *st = &s_row[cr];

    const uint32_t fps = clk_theme.tourbillon_fps ? clk_theme.tourbillon_fps : 15;
    const int f = (int)(((uint64_t)(lv_tick_get() - s_tourb_t0) * fps)
                        / 1000u % (uint32_t)n);
    if (f == s_tourb_shown) return;

    /* The carriage stands at its own row's pose — the one rows_update just set on
     * it, whatever that was when the frame clock started and between its ticks. */
    const int32_t a = st->ang_10;
    clk_sprite_t fo, fn;
    pose_spr(&clk_theme.tourbillon[f], st->ox, st->oy, &fn);
    lv_area_t u;
    if (s_tourb_shown < 0 || a == ROW_HIDDEN) {
        /* First draw of this layer: no prior composite exists to diff against,
         * so the whole (rotation-0 if not posed yet) swept box goes dirty. */
        gfx_swept_box(&fn, a == ROW_HIDDEN ? 0 : a, &u);
    } else {
        pose_spr(&clk_theme.tourbillon[s_tourb_shown], st->ox, st->oy, &fo);
        tourb_mark(&fo, a, &u);
        lv_area_t v;
        tourb_mark(&fn, a, &v);
        gfx_area_union(&u, &v);
    }

    /* Mirror rows_update's etiquette: the src setter would invalidate the
     * widget's whole transformed box; the marks below cover exactly what
     * needs rebuilding. Under L4 the commit painter owns the rows outright. */
    lv_display_t *disp = lv_obj_get_display(s_scr);
    lv_display_enable_invalidation(disp, false);
    lv_image_set_src(st->obj, &s_tourb_dsc[f]);
    lv_display_enable_invalidation(disp, true);
#if CLK_BG_DIRECT_PUSH
    if (orb_sched_l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
    else
#endif
        lv_obj_invalidate_area(s_scr, &u);
    /* The INCOMING frame's own ink at this angle, marked by chords too: the
     * baked diff rect is an axis-aligned box that a sub-degree rotation can
     * push a few pixels of balance ink outside — that seam was the visible
     * rectangle around the escapement. The stamps cover the exact rotated
     * ink; the diff-box mark above stays as the belt to the chords' braces
     * (and is what paints when the chord tables cannot be built). */
    if (s_tourb_shown >= 0 && a != ROW_HIDDEN && !st->ox && !st->oy)
        tourbillon_ink_mark(f, INT32_MIN, a);
    s_tourb_shown = f;
}


#if CLK_BG_DIRECT_PUSH
/* ---- L4: direct layer compositing into the panel mirror -------------------
 * With L3 the raw GIF pixels land in the mirror instantly, but everything
 * that PAINTS OVER them (hands, shadows, glass overlay) still had to come
 * back through LVGL: measured 36-48 ms/frame of render~, because for every
 * pushed row LVGL re-rasterized each overlapping layer's FULL rotated box
 * through its generic transform+blend pipeline (~500+ cyc/px even with the
 * widget's antialiasing off, and shrinking the invalidation does not shrink
 * the rotated scan).
 *
 * So skip LVGL for the re-composite entirely: the tick marks the rows its
 * push dirtied (plus the wedges of any hand that moved), and at commit --
 * after every chunk is in the mirror, before the bands DMA it out -- we
 * paint each affected layer straight into the mirror. The maths below copies
 * lv_draw_sw_transform()'s ROTATION-ONLY, ANTI-ALIAS-OFF nearest-neighbour
 * path (and lv_color_16_16_mix_inlined / rgb565_image_blend for the blend)
 * operation for operation, so our pixels are bit-identical to what LVGL
 * draws for the first paint and for the wedges it still renders itself.
 * Since D43 §5.4 both sides ROUND that blend: orb_gfx.c's mix_16_16 and
 * LVGL's RGB565 image blends through main/gfx/orb_lv_blend.c's hooks.
 * Cost is the clipped box with two early-outs (source bounds, alpha==0):
 * the needles are mostly empty box, so the mask skip drops ~3/4 of it. */
/* The per-tick dial policy the scheduler drives: read the time, pose the hands
 * off it, and step the tourbillon's frame bank. Everything else the tick used to
 * do — the background step, the marks, the commit nudge, the deadline ladder, the
 * throughput report — is the scheduler's. Kept as ONE callback rather than two
 * because the pair has no meaning apart: the frame bank advances against the
 * angle compose_now just settled on, and a tick that ran one without the other
 * would composite last second's carriage at this second's angle. */
static void clock_tick_policy(lv_timer_t *timer)
{
    (void)timer;
    compose_now();
    tourbillon_advance();          /* no-op unless the theme has a frame bank */
    /* Then the app's side of the frame: every row of this dial's layer table
     * takes the pose the two passes above just settled (Migration 2, Commit 4).
     * Damage already sits in the row bank — the mark sites run inside them,
     * and each marks both the pose it vacates and the one it takes. The
     * scheduler's flush hook welds whatever this pass changed, so the panel
     * cannot catch a half-painted dial: the marks and the table agree before
     * the first restore row does. */
    clock_layers_refresh();
}








#if DISPLAY_BENCH
/* ---- Synthetic live layer, to price a blended pixel --------------------------
 * docs/history/fps-budget-benchmark-plan.md Phase 2. DISPLAY_BENCH_PX solid 60x60
 * blocks (3600 px each, so 18/36/72/112 add the planned 65k/130k/259k/403k
 * px/tick) are painted through the very gfx_blend_px() the real layers use, with
 * the colour plane in PSRAM like a real sprite. Positions are chosen at tick
 * time and consumed by the hook at commit, exactly like a hand's swept box,
 * and each block walks every tick so the flush gate cannot settle the ink out
 * of the DMA — CAD bands/px are the proof it shipped.
 *
 * Seed and wrap INSIDE the band: a block starting at y=0 would mark rows the
 * art never touches, turning a px sweep into a px+rows sweep. Rows to know:
 * the shipped commit DMA's ~265 full-width rows, and the GIF bank's 208-row
 * union is NOT what sets that — the carriage's rotating diff box forces ~255
 * on its own. Pinning rows therefore means keeping the ink inside that band,
 * and DISPLAY_BENCH_SPREAD=1 is the deliberate way out, used to price the row
 * term alone. Measured numbers: docs/history/fps-budget-findings-2026-10-10.md. */
#define BENCH_BLK        60
#define BENCH_MAX_BLK    128
#define BENCH_BAND_Y0    128
#define BENCH_BAND_ROWS  208
static uint16_t *s_bench_solid;                       /* PSRAM colour plane   */
static int16_t   s_bench_x[BENCH_MAX_BLK], s_bench_y[BENCH_MAX_BLK];
static uint32_t  s_bench_n;                           /* blocks this tick     */
/* The ink count lives in the scheduler (orb_sched_ink_px_add), which is the TU
 * that reports it next to the flush numbers it has to agree with. */

static void bench_paint_band(int32_t y0, int nr);   /* gfx band hook, below */

#if ORB_ART_M3_PROBE
/* M3 graft: point the blocks at the bank the M3 probe left resident instead
 * of the synthetic mag plane — same px count, same blend, the ONLY variable
 * is where the colours live (flash MMU map on arm A, PSRAM arena on arm B).
 * Offsets re-stripe every tick in bench_plan so each pass reads every page of
 * the bank; a bench that reread one cache line proves nothing. If the plane
 * never arrived the bench says so and stays dark — mag ink under an "M3"
 * label would be the confident lie the probe already refuses to print. */
static size_t  s_m3_off[BENCH_MAX_BLK];
static size_t  s_m3_bytes;
static unsigned s_m3_pass;

static bool m3_arm_plane(void)
{
    s_m3_bytes = art_bank_m3_bytes();
    const uint8_t *plane = art_bank_m3_plane();
    const size_t need = (size_t)BENCH_MAX_BLK * BENCH_BLK * BENCH_BLK * 2u;
    if (!plane || s_m3_bytes < need + (size_t)BENCH_BLK * BENCH_BLK * 2u) {
        ESP_LOGE(TAG, "bench M3: no bench plane (bank %u B, need >%u B) — NO blocks will "
                      "paint; read the m3: lines above for why",
                 (unsigned)s_m3_bytes, (unsigned)need);
        return false;
    }
    s_bench_solid = (uint16_t *)(void *)(uintptr_t)plane;   /* read-only from here */
    ESP_LOGI(TAG, "bench M3: colour source := %s bank, %u B — the reads ARE the measurement",
             art_bank_m3_is_psram() ? "PSRAM arena" : "flash-map", (unsigned)s_m3_bytes);
    return true;
}
#endif /* ORB_ART_M3_PROBE */

static void bench_init(void)
{
    if (DISPLAY_BENCH_PX == 0 || s_bench_solid) return;
    /* The commit loop moved to gfx; the synthetic ink still has to ride inside
     * its timed paint window, so it is registered as the band hook rather than
     * called from the hook that no longer exists here. */
    gfx_set_band_hook(bench_paint_band);
#if ORB_ART_M3_PROBE
    if (!m3_arm_plane()) return;
#else
    const size_t n = (size_t)BENCH_BLK * BENCH_BLK * sizeof(uint16_t);
    s_bench_solid = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bench_solid) return;
    uint16_t *p = s_bench_solid;
    for (size_t i = 0; i < n / sizeof(uint16_t); i++) p[i] = 0xF81F;   /* mag */
#endif
    /* Seed inside the band: a block that starts at y=0 marks rows the GIF
     * never touches, and the first ~60 ticks would then measure a row change
     * as well as a px change -- exactly the confound this sweep must not have. */
    for (uint32_t i = 0; i < DISPLAY_BENCH_PX; i++) {
        s_bench_x[i] = (int16_t)((i * 61) % (CLK_SCR_W - BENCH_BLK));
#if DISPLAY_BENCH_SPREAD
        s_bench_y[i] = (int16_t)((i * 37) % (CLK_SCR_H - BENCH_BLK));
#else
        s_bench_y[i] = (int16_t)(BENCH_BAND_Y0
                                 + (i * 37) % (BENCH_BAND_ROWS - BENCH_BLK));
#endif
    }
    ESP_LOGI(TAG, "bench: %u blocks of %dx%d = %u px/tick, alpha %u",
             (unsigned)DISPLAY_BENCH_PX, BENCH_BLK, BENCH_BLK,
             (unsigned)(DISPLAY_BENCH_PX * BENCH_BLK * BENCH_BLK),
             (unsigned)DISPLAY_BENCH_ALPHA);
}

/* Tick side: walk the blocks and mark their rows through the hand's own path,
 * so the hook restores+repaints exactly the pixels the ink will land on. Both
 * axes wrap INSIDE the band, so the marked row set stays a subset of the
 * shipped GIF union and CAD bands/px report the same rows at every point. */
static void bench_plan(void)
{
    if (!s_bench_solid) return;
    s_bench_n = DISPLAY_BENCH_PX < BENCH_MAX_BLK ? DISPLAY_BENCH_PX : BENCH_MAX_BLK;
    for (uint32_t i = 0; i < s_bench_n; i++) {
        int32_t x = s_bench_x[i] + 7;
        int32_t y = s_bench_y[i] + 5;
        if (x + BENCH_BLK > CLK_SCR_W) x = (int32_t)((i * 29) % (CLK_SCR_W - BENCH_BLK));
#if DISPLAY_BENCH_SPREAD
        if (y + BENCH_BLK > CLK_SCR_H) y = (int32_t)((i * 13) % (CLK_SCR_H - BENCH_BLK));
#else
        if (y + BENCH_BLK > BENCH_BAND_Y0 + BENCH_BAND_ROWS)
            y = BENCH_BAND_Y0 + (int32_t)((i * 13) % (BENCH_BAND_ROWS - BENCH_BLK));
#endif
        s_bench_x[i] = (int16_t)x;
        s_bench_y[i] = (int16_t)y;
        gfx_mark_rows(y, y + BENCH_BLK - 1, x, x + BENCH_BLK - 1, false);
    }
#if ORB_ART_M3_PROBE
    /* Re-stripe the colour-source windows so every pass sweeps the WHOLE bank:
     * one block per band of it, rotated by the pass counter — no block pair
     * shares a source page, and a tick that read only the cache would show up
     * as a zero row-cost difference between arms. This is the M3 variable:
     * identical blends, different source location. */
    if (s_bench_n && s_m3_bytes) {
        const size_t win  = (size_t)BENCH_BLK * BENCH_BLK * 2u;
        const size_t band = s_m3_bytes / s_bench_n;
        const size_t span = s_m3_bytes - win;
        for (uint32_t i = 0; i < s_bench_n; i++)
            s_m3_off[i] = (((size_t)i * band + (size_t)s_m3_pass * 0x1150u) % span)
                        & ~(size_t)1u;
        s_m3_pass++;
    }
#endif
}

/* Hook side: blend the planned blocks into the staged band rows, after the
 * layer loop and inside the same timed paint window, so the CAD hook/px
 * fields carry the ink's true cost. */
static void bench_paint_band(int32_t y0, int nr)
{
    if (!s_bench_solid || !s_bench_n) return;
    const gfx_rows_t *R = gfx_rows();
    for (uint32_t i = 0; i < s_bench_n; i++) {
        const int32_t by0 = s_bench_y[i];
        if (by0 > y0 + nr - 1 || by0 + BENCH_BLK - 1 < y0) continue;
        const int32_t bx0 = s_bench_x[i];
        for (int r = 0; r < nr; r++) {
            const int32_t y = y0 + r;
            if (y < by0 || y > by0 + BENCH_BLK - 1) continue;
            int32_t xa = bx0, xb = bx0 + BENCH_BLK - 1;
            if (xa < R->x0[y]) xa = R->x0[y];
            if (xb > R->x1[y]) xb = R->x1[y];
            if (xa > xb) continue;
#if ORB_ART_M3_PROBE
            /* Source walks the bank, not a 7.2 KB mag plane: this line's
             * memory location IS the A-vs-B question, at cadence. */
            const uint16_t *src = s_bench_solid + (s_m3_off[i] >> 1)
                                + (size_t)(y - by0) * BENCH_BLK;
#else
            const uint16_t *src = &s_bench_solid[(size_t)(y - by0) * BENCH_BLK];
#endif
            uint16_t *row = gfx_band(r);
            for (int32_t x = xa; x <= xb; x++)
                gfx_blend_px(&row[x], src[x - bx0], DISPLAY_BENCH_ALPHA, 0);
            orb_sched_ink_px_add((uint32_t)(xb - xa + 1));
        }
    }
}
#endif /* DISPLAY_BENCH */

#endif /* CLK_BG_DIRECT_PUSH */

/* ---- complications: baked windows and strips under the live dial -----------
 *
 * The policy is locked with the baker (tools/bake_clock.py::bake_complications):
 * window tiles pack RAW — colour plane w*h*2 then planar alpha w*h — never
 * composited against the dial. The tile a value selects goes down as the
 * bottom-most paint job and the live dial's own window frame composites over
 * its edge, so a tile sits in its bezel the way a real disc sits behind a
 * real aperture. Windows address the art blob directly
 * (c->off + frame*c->frame_bytes); the shared sprite bank serves strips only
 * — glyphs and window pieces — exactly as tools/orbtheme.py pack_geom packed
 * them.
 *
 * The frame-selection maths is the exact mirror of
 * tools/theme_schema.py::frame_index: the bake laid tiles out by that
 * function, so the rounding, the clamp and the wrap must match to the pixel.
 * Everything stays integer, so no libm floor can drift from the Python.
 *
 * What this pass draws: WINDOW and STRIP. RING and FLARE validate at
 * orb_theme.c but ship on no bake and nothing here fixes how a value rotates
 * one or blends: logged once, hidden, the same discipline the reader applies
 * to a kind newer than the dial. Under-dial windows (opaque, bake-composited
 * — see orb_theme.h) and battery-driven comps (the orb is wall-powered:
 * main/net/orb_net.c) are likewise parsed, validated, and not drawn. */

#define CLK_COMP_SLOTS (CLK_STRIP_GLYPHS_MAX + 2)  /* glyph positions + window pieces L/R */
static lv_obj_t       *s_comp_obj[CLK_COMP_MAX][CLK_COMP_SLOTS];
EXT_RAM_BSS_ATTR static lv_image_dsc_t  s_comp_dsc[CLK_COMP_MAX][CLK_COMP_SLOTS];   /* PSRAM: internal RAM is the DMA ring's */
/* The frame drawn right now, per comp: the window's tile index; the strip's
 * digit at each glyph position and a 0 for each window piece; -1 = nothing
 * drawn. Sole truth for both paths: the layer table's pose pass reads these
 * to pick each BANK row's plane, and the classic path reads the LVGL widgets
 * comp_slot_show keeps in step. Any drift between the two is the one-tick
 * ghost the hands paid for before the redraw gate learned to hold its angles. */
static int32_t         s_comp_frame[CLK_COMP_MAX][CLK_COMP_SLOTS];
static uint32_t        s_comp_logged;        /* bit i: hide reason for comp i printed */

/* theme_schema.frame_index() verbatim: sign-aware round-to-nearest at
 * scale = frames-per-value-unit x16, clamp below the domain, and WRAP —
 * never clamp — at count, so weekday and month keep cycling. */
static uint16_t comp_frame_index(int32_t value, uint16_t count,
                                 int16_t value_min, uint16_t scale)
{
    if (!count) return 0;
    const int32_t s = scale ? (int32_t)scale : 16;
    int64_t idx = (int64_t)(value - (int32_t)value_min) * s;
    idx = (idx >= 0) ? (idx + 8) / 16 : -((-idx + 8) / 16);
    if (idx < 0)               idx = 0;
    if (idx >= (int64_t)count) idx %= (int64_t)count;
    return (uint16_t)idx;
}

/* How many glyph positions a strip box holds: the same ceil(w / glyph_step)
 * the reader took when it accepted the bake. That side refuses a bake this
 * side cannot lay out, so this side only repeats the division. */
static int comp_strip_positions(const clk_comp_t *c)
{
    if (c->glyph_step <= 0) return 0;             /* the reader refused such a strip */
    const uint32_t pos = ((uint32_t)c->w + (uint32_t)c->glyph_step - 1u)
                       / (uint32_t)c->glyph_step;
    return (pos > CLK_STRIP_GLYPHS_MAX) ? CLK_STRIP_GLYPHS_MAX : (int)pos;
}

/* theme_schema.strip_indices() verbatim: the value's decimal, most
 * significant first, each glyph reduced into the glyph set, cut to the last
 * npos digits the box can hold. out[p] == -1 leaves position p empty, and
 * the drawn run sits right-aligned like a real date drum; a zero-padded bake
 * fills the spare left positions with the zero glyph instead. */
static void comp_strip_digits(int32_t value, int npos, bool zero_pad,
                              uint16_t count, int16_t *out)
{
    for (int p = 0; p < npos; p++) out[p] = -1;
    if (npos <= 0) return;
    char buf[12];
    if (value < 0) value = 0;                     /* no source goes negative; belt */
    snprintf(buf, sizeof buf, "%d", (int)value);
    int len = (int)strlen(buf);
    if (zero_pad && len < npos) {
        memmove(buf + (npos - len), buf, (size_t)len);
        memset(buf, '0', (size_t)(npos - len));
        len = npos;
    }
    if (len > npos) { memmove(buf, buf + len - npos, (size_t)npos); len = npos; }
    const int first = npos - len;
    const int gmax  = count ? (int)count : 1;
    for (int p = 0; p < len; p++)
        out[first + p] = (int16_t)((buf[p] - '0') % gmax);
}

/* One known new moon (2000-01-06 18:14 UTC) and the mean synodic month, both
 * whole seconds: the age stays integer end to end, 0..29 days. The 30-tile
 * moon a bake lays out indexes the age once per day — the half-day the mean
 * month carries over 29.53 costs a tile-edge rounding at most, which is what
 * a float-free dial path buys. */




/* Point one comp slot's image at blob pixels and un-hide it. Under L4 the
 * widget never paints — nothing invalidates it — but it must still say what
 * the job list says: whenever LVGL renders a page itself (bounce-on, a
 * refused direct push) it renders the widget, and a tick of drift between
 * the two is a ghost on the glass. */
static void comp_slot_show(uint16_t i, int p, uint32_t color_off,
                           uint16_t w, uint16_t h, int32_t x, int32_t y)
{
    lv_image_dsc_t *d = &s_comp_dsc[i][p];
    memset(d, 0, sizeof *d);
    d->header.magic  = LV_IMAGE_HEADER_MAGIC;
    d->header.cf     = LV_COLOR_FORMAT_RGB565A8;   /* planar colour+alpha, as packed */
    d->header.w      = w;
    d->header.h      = h;
    d->header.stride = (uint32_t)w * 2u;
    d->data_size     = (uint32_t)w * (uint32_t)h * 3u;
    d->data          = blob() + color_off;
    lv_obj_t *o = s_comp_obj[i][p];
    if (!o) return;
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, (lv_coord_t)x, (lv_coord_t)y);
    lv_image_set_src(o, d);
    lv_obj_set_hidden(o, false);
}

static void comp_slot_hide(uint16_t i, int p)
{
    if (s_comp_obj[i][p]) lv_obj_set_hidden(s_comp_obj[i][p], true);
}

/* Ask whoever owns the pixels to rebuild this comp's box: L4 marks the rows
 * (the hook restores the canvas under them and repaints EVERY job, comps
 * included), the classic path invalidates so LVGL re-renders plate and
 * children over the area. box_h runs the mark down to the tallest thing the
 * strip actually draws — the reader bounds the RECORD's box against the
 * panel and the glyphs' art against the payload, not the glyphs against the
 * box. */
static void comp_touch_box(const clk_comp_t *c, int32_t box_h)
{
    int32_t x0 = c->x, y0 = c->y;
    int32_t x1 = c->x + (int32_t)c->w - 1;
    int32_t y1 = c->y + box_h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CLK_SCR_W - 1) x1 = CLK_SCR_W - 1;
    if (y1 > CLK_SCR_H - 1) y1 = CLK_SCR_H - 1;
    if (x1 < x0 || y1 < y0) return;
#if CLK_BG_DIRECT_PUSH
    if (orb_sched_l4_active()) { gfx_mark_rows(y0, y1, x0, x1, false); return; }
#endif
    lv_area_t b = { x0, y0, x1, y1 };
    lv_obj_invalidate_area(s_scr, &b);
}

/* The per-tick pass. A comp only dirties pixels when its SELECTION moved —
 * tile index, glyph digits, shown/hidden — so a hung calendar costs one
 * integer compare per comp per tick, and a theme with no complications costs
 * one loop-exit. */
static void comps_update(void)
{
    const orb_theme_geo_t *g = orb_theme_geo();
    for (uint16_t i = 0; i < g->n_comps && i < CLK_COMP_MAX; i++) {
        const clk_comp_t *c = &g->comps[i];
        const bool window = (c->kind == CLK_KIND_WINDOW);
        const bool strip  = (c->kind == CLK_KIND_STRIP);
        const int  npos   = strip ? comp_strip_positions(c) : 0;
        const int  slots  = window ? 1 : (strip ? npos + 2 : 0);
        bool dirty = false;

        /* What to draw, with every valid-but-hidden case logged once per
         * comp: a kind with no painter, an opaque under-dial tile, a source
         * with no input, an explicit DISABLED, a blink off-beat. */
        bool live = c->show && !(c->flags & CLK_FLAG_DISABLED);
        if (live && !window && !strip) {
            live = false;
            if (!(s_comp_logged & (1u << i))) {
                s_comp_logged |= 1u << i;
                ESP_LOGW(TAG, "comp %" PRIu32 ": kind %u has no painter yet — not drawn",
                         (uint32_t)i, (unsigned)c->kind);
            }
        }
        if (live && window && (c->flags & CLK_FLAG_UNDER_DIAL)) {
            live = false;
            if (!(s_comp_logged & (1u << i))) {
                s_comp_logged |= 1u << i;
                ESP_LOGW(TAG, "comp %" PRIu32 ": bake-composited under-dial tile — this dial "
                              "draws raw tiles under the live frame only", (uint32_t)i);
            }
        }
        /* §43: the value is the app's (its sources, its blink); a source this
         * device has no input for comes back not live, and stays hidden. */
        const int32_t value = s_g_cval[i];
        if (live && !s_g_clive[i]) live = false;

        int32_t box_h = (int32_t)c->h;
        if (strip && npos > 0 && c->count) {
            const clk_sprite_t *b = &g->bank[c->first];
            if ((int32_t)b->h > box_h) box_h = b->h;
        }

        if (!live) {
            for (int p = 0; p < slots; p++)
                if (s_comp_frame[i][p] != -1) {
                    comp_slot_hide(i, p);
                    s_comp_frame[i][p] = -1;
                    dirty = true;
                }
            if (dirty) comp_touch_box(c, box_h);
            continue;
        }

        if (window) {
            const int32_t f = (int32_t)comp_frame_index(value, c->count,
                                                        c->value_min, c->scale);
            if (s_comp_frame[i][0] != f) {
                comp_slot_show(i, 0, c->off + (uint32_t)f * c->frame_bytes,
                               c->w, c->h, c->x, c->y);
                s_comp_frame[i][0] = f;
                dirty = true;
            }
        } else {
            int16_t dig[CLK_STRIP_GLYPHS_MAX];
            comp_strip_digits(value, npos, (c->flags & CLK_FLAG_ZERO_PAD) != 0,
                              c->count, dig);
            for (int p = 0; p < npos; p++) {
                if (s_comp_frame[i][p] == dig[p]) continue;
                if (dig[p] < 0) {
                    comp_slot_hide(i, p);
                } else {
                    const clk_sprite_t *b = &g->bank[c->first + (uint16_t)dig[p]];
                    comp_slot_show(i, p, b->color_off, b->w, b->h,
                                   c->x + c->glyph_lead + (int32_t)p * c->glyph_step,
                                   c->y);
                }
                s_comp_frame[i][p] = dig[p];
                dirty = true;
            }
            /* Window pieces: fixed pixels, drawn whenever the strip is, so
             * their frame goes to -1 only while the whole comp is hidden. */
            if (c->edge_left != 0xFFFFu && s_comp_frame[i][npos] < 0) {
                const clk_sprite_t *b = &g->bank[c->edge_left];
                comp_slot_show(i, npos, b->color_off, b->w, b->h, c->x, c->y);
                s_comp_frame[i][npos] = 0;
                dirty = true;
            }
            if (c->edge_right != 0xFFFFu && s_comp_frame[i][npos + 1] < 0) {
                const clk_sprite_t *b = &g->bank[c->edge_right];
                comp_slot_show(i, npos + 1, b->color_off, b->w, b->h,
                               c->x + (int32_t)c->w - (int32_t)b->w, c->y);
                s_comp_frame[i][npos + 1] = 0;
                dirty = true;
            }
        }
        if (dirty) comp_touch_box(c, box_h);
    }
}

/* One comp's widgets, hidden until comp_slot_show points them at pixels — the
 * same widgets the L4 hook mirrors. stage_init calls this at the comp's place
 * in the depth walk, because creation order is the classic path's only notion of
 * z-order and a window must sit where its depth says: under the live layer that
 * frames it, over the plate it is cut into. */
static void complication_create(const orb_theme_geo_t *g, uint16_t i)
{
    const clk_comp_t *c = &g->comps[i];
    if (!c->show || (c->flags & CLK_FLAG_DISABLED)) return;
    if (s_comp_obj[i][0]) return;                     /* created already */
    int slots = 0;
    if      (c->kind == CLK_KIND_WINDOW) slots = 1;
    else if (c->kind == CLK_KIND_STRIP)  slots = comp_strip_positions(c) + 2;
    for (int p = 0; p < slots && p < CLK_COMP_SLOTS; p++) {
        lv_obj_t *o = lv_image_create(s_scr);
        lv_obj_set_align(o, LV_ALIGN_TOP_LEFT);
        lv_obj_set_scrollable(o, false);
        lv_image_set_blend_mode(o, map_blend(c->blend));
        lv_obj_set_hidden(o, true);
        s_comp_obj[i][p] = o;
    }
}

static void complications_reset(void)
{
    memset(s_comp_obj,   0,    sizeof s_comp_obj);
    memset(s_comp_frame, 0xFF, sizeof s_comp_frame);      /* -1: nothing drawn yet */
}


/* ---- D42: the moving rows -------------------------------------------------------
 *
 * One clk_row_t is a sprite plus how it moves (docs/history/element-model-plan.md §3): its
 * art sits at P0 — the row's own placement, turned to `zero` — and across its domain
 * it turns a further `span` and its pivot travels (dx, dy) to P1. The value comes
 * from comp_value(), so a date ring and the date WINDOW can never disagree about
 * what day it is, and every row reads the one struct tm compose_now sampled.
 *
 * A shadowed row is TWO rows: the row, then its blur/offset twin flagged
 * CLK_FLAG_AUX_SHADOW, adjacent in slot order (orbtheme-baker.js packs the pair;
 * row flag bit 4 — AUX_F_SHADOW3, mapped in orb_clock3.c — carries it into this
 * vocabulary). The twin owns pixels and a centre (its row's, moved by the light
 * vector) and no input of its own — the tick gives it its row's whole pose — so it
 * can never come loose from the pixels it exists to darken: row and twin turn as
 * one rigid pair, and the light's offset stays fixed in screen space.
 *
 * Depth is one number per row, on the scale the complication windows share, and
 * both paint paths stack by it (stage_init's creation walk and the layer
 * table's sort read the same key). */






/* The fps bucket a row's blends count under on the scheduler's line: the hour,
 * minute and seconds slots the report has always named, the still layers, and the
 * rest as pointers. A label only — nothing paints by it. */
static uint8_t row_role(const clk_row_t *r)
{
    switch (r->src) {
    case CLK_SRC_HOUR12: case CLK_SRC_HOUR24: return 0;
    case CLK_SRC_MINUTE: return 1;
    case CLK_SRC_SECOND: return 2;
    case CLK_SRC_NONE:   return 3;
    default:             return CLK_ROLE_AUX;
    }
}

/* A row whose frames the app picks (theme-pool-plan §2.6): its frames are consecutive
 * catalogue planes of the row's own size (the projection refused any other), each a
 * colour plane and its alpha plane, so frame f sits f planes past the row's own. */
static bool row_picks(uint16_t i)
{
    return i < CLK_ROW_MAX && clk_theme.pick_n[i] > 1u;
}

static void pick_sprite(uint16_t i, int16_t f, clk_sprite_t *out)
{
    *out = orb_theme_geo()->rows[i].spr;
    out->color_off += (uint32_t)f * ((uint32_t)out->w * out->h * 3u);
}

/* The sprite a row draws: the carriage's shown frame, a picked row's frame, or the
 * row's own plate. */
static const clk_sprite_t *row_spr(uint16_t i)
{
    const clk_row_t *r = &orb_theme_geo()->rows[i];
    if (i == clk_theme.car_row && clk_theme.tourbillon_n)
        return &clk_theme.tourbillon[s_tourb_shown >= 0 ? s_tourb_shown : 0];
    if (row_picks(i) && s_row)
        return &s_row[i].fspr;
    return &r->spr;
}

/* The box a row covers at a pose. */
static void row_mark_box(uint16_t i, int32_t ang_10, int16_t ox, int16_t oy, lv_area_t *out)
{
    /* D43: a drum stays in its window whatever it shows; its travel is the strip's roll */
    if (i < CLK_ROW_MAX && clk_theme.drum_r[i]) { gfx_drum_box(row_spr(i), clk_theme.drum_r[i], out); return; }
    clk_sprite_t p;
    pose_spr(row_spr(i), ox, oy, &p);
    gfx_swept_box(&p, ang_10, out);
}

#define ROW_REDRAW_EPS_10 2

#if CONFIG_ORB_DEBUG_HTTP
/* The span bench (/api/v1/debug/dial, orb_gfx.h): mark a rotated row by its ink
 * per screen row instead of its swept box. False = the caller marks the box. */
static bool row_mark_spans(uint16_t i, int32_t ang_10, int16_t ox, int16_t oy)
{
    if (!gfx_bench_spans() || !orb_sched_l4_active()) return false;
    if (i < CLK_ROW_MAX && clk_theme.drum_r[i]) return false;
    clk_sprite_t p;
    pose_spr(row_spr(i), ox, oy, &p);
    gfx_paint_ctx_t pc;
    if (!gfx_paint_ctx_init(&pc, &p, blob(), ang_10, 0)) return false;
    pc.smooth = true;                 /* the wider pad: a mark may only over-cover */
    return gfx_mark_spans(&pc);
}

/* The bench's forced sweep: the minute row and its shadow twin turn at this many
 * degrees a second on top of the app's own pose. 0 = the app's pose alone. */
static volatile int32_t s_bench_spin_dps;
static volatile bool    s_bench_sweep;      /* the second hand sweeps between its steps */
static volatile int32_t s_bench_osc_deg;    /* the hour row rocks +-this, a balance-wheel proxy */
void stage_bench_spin(int32_t dps, bool sweep, int32_t osc_deg)
{
    s_bench_spin_dps = dps; s_bench_sweep = sweep; s_bench_osc_deg = osc_deg;
}
#define ROW_MARK_SPANS(i, a, ox, oy) row_mark_spans((i), (a), (ox), (oy))
#else
#define ROW_MARK_SPANS(i, a, ox, oy) false
#endif

/* ---- theme-text-plan §2.3: composing a label ------------------------------------------ */

/* The fonts and every label's own columns, read from the bound section. */
static void text_scan(void)
{
    s_text_sec = NULL;
    if (!s_text)
        s_text = heap_caps_calloc(1, sizeof *s_text, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_text) return;
    memset(s_text, 0, sizeof *s_text);
    memset(s_text->tone_font, CLK_FONT_NONE, sizeof s_text->tone_font);
    const orb_bundle_sec_t *sec = NULL;
    uint32_t off = 0;
    if (s_base || !orb_theme_live_sec(orb_theme_live_slug(), &sec, &off)) return;
    const orb_theme_geo_t *g = orb_theme_geo();
    for (uint32_t k = 0; k < sec->n_elem; k++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, k, &e);
        if (e.cls == ORB_ASSET_TEXT && e.slot < CLK_FONT_SLOTS) {
            orb_font_t *f = &s_text->font[e.slot];
            f->w = e.w; f->h = e.h; f->n = e.n_frames;
            f->space = (uint8_t)e.pivot_x; f->track = (uint8_t)e.pivot_y;
            f->c0 = e.vmin; f->c1 = e.vmax;      /* a coverage font's colours (orb_label.h) */
            s_text->frame0[e.slot] = e.frame0;
            s_text->present[e.slot] = true;
        } else if (e.cls == ORB_ASSET_AUX && e.kind == CLK_SHAPE_TEXT) {
            for (uint16_t i = 0; i < g->n_rows && i < CLK_ROW_MAX; i++)
                if (g->rows[i].tag == e.slot) {
                    for (int t = 0; t < 4; t++)
                        s_text->tone_font[i][t] = (uint8_t)((e.str >> (4 * t)) & 0xFu);
                    s_text->pad[i] = (uint8_t)e.radius;
                    break;
                }
        }
    }
    s_text_sec = sec;
    int nf = 0, nl = 0;
    for (int k = 0; k < CLK_FONT_SLOTS; k++) nf += s_text->present[k];
    for (int k = 0; k < CLK_ROW_MAX; k++) nl += s_text->tone_font[k][0] != CLK_FONT_NONE;
    if (nf || nl) ESP_LOGI(TAG, "text: %d font%s, %d label%s", nf, nf == 1 ? "" : "s", nl, nl == 1 ? "" : "s");
}

/* Compose row i's label: its panel, then the app's string in its tone's font (tone 0's
 * when the theme gives the tone none), aligned by the row's digit and end. */
static void label_compose(uint16_t i)
{
    row_state_t *st = &s_row[i];
    const clk_row_t *r = &orb_theme_geo()->rows[i];
    const uint8_t *panel = blob() + r->spr.color_off;
    if (!s_text || !s_text_sec) {
        orb_label_compose(st->tplane + LABEL_PLANE_OFF, r->spr.w, r->spr.h, panel, NULL, NULL,
                          0, 0, 0);
        return;
    }
    char buf[LABEL_TEXT_MAX];
    const uint32_t seq = s_text->seq[i];
    memcpy(buf, s_text->text[i], sizeof buf);
    buf[sizeof buf - 1] = 0;
    st->tseq = seq;              /* a string that lands mid-copy bumps seq: next frame */
    uint8_t fi = s_text->tone_font[i][s_text->tone[i] & 3u];
    if (fi >= CLK_FONT_SLOTS || !s_text->present[fi]) fi = s_text->tone_font[i][0];
    orb_font_t *f = fi < CLK_FONT_SLOTS && s_text->present[fi] ? &s_text->font[fi] : NULL;
    if (f && !f->glyphs) {
        /* the cells it carries, in order (orb_clock3.c proved them one form, contiguous,
         * at least one); an empty frame is a glyph it lacks (OS v1.2.17) */
        uint32_t n = 0;
        for (uint16_t gi = 0; gi < f->n && gi < CLK_FONT_GLYPHS; gi++) {
            orb_bundle_spr_t sp;
            orb_bundle_spr_at(s_text_sec, s_text->frame0[fi] + gi, &sp);
            if (!sp.size) { f->rank[gi] = ORB_GLYPH_NONE; continue; }
            if (!n) {
                f->glyphs = blob() + sp.off;
                f->bpp = sp.size == (uint32_t)f->w * f->h ? 1u : 3u;
            }
            f->rank[gi] = (uint8_t)n++;
        }
        f->sparse = n < f->n;
    }
    orb_label_compose(st->tplane + LABEL_PLANE_OFF, r->spr.w, r->spr.h, panel, f, buf,
                      s_text->pad[i], r->digit, r->end);
}

/* The per-tick pass. Every row's own pose first, then the poses it rides (a parent's,
 * two links at most — the projection refused deeper), then each twin takes its row's,
 * then each row that moved marks the box it vacates and the one it takes, and its
 * widget follows. A row that stands still costs one compare.
 *
 * HARD GATE for sweeping dials (the hands' rule since the first port): re-posing a
 * row invalidates its whole rotated box, so a slow sweep — a minute hand advancing a
 * fraction of a pixel a tick — re-poses only once its angle has left the drawn one by
 * 0.2 degrees (~0.4 px at the longest tip), catching up exactly. The carriage's orbit
 * marks its rotated ink chords instead of the box when it can (tourbillon_ink_mark). */
static void rows_update(void)
{
    const orb_theme_geo_t *g = orb_theme_geo();
    if (!s_row || !g->rows) return;
    const uint16_t n = g->n_rows < CLK_ROW_MAX ? g->n_rows : CLK_ROW_MAX;
    /* §43: every pose is the app's — its drive, cadence, ease, the parent it rides
     * and the twin that follows it, posted through stage_pose this frame. What is the
     * stage's is everything below: the marks, the staged pose, the widget. */
    const int32_t *ang = s_g_ang;
    const int16_t *ox = s_g_ox, *oy = s_g_oy;
#if CONFIG_ORB_DEBUG_HTTP
    static int32_t s_spin_ang[CLK_ROW_MAX];
    if (s_bench_spin_dps || s_bench_sweep || s_bench_osc_deg) {
        memcpy(s_spin_ang, s_g_ang, sizeof s_spin_ang);
        /* The escapement proxy: the hour row and its shadow rock +-osc degrees, one
         * swing per 400 ms (a 2.5 Hz balance), so every tick moves an escapement-sized
         * sprite (40 x 181, shadow 58 x 199) by a different amount. */
        int hr = -1;
        if (s_bench_osc_deg)
            for (uint16_t j = 0; j < n; j++)
                if (g->rows[j].src == CLK_SRC_HOUR12 || g->rows[j].src == CLK_SRC_HOUR24) { hr = j; break; }
        if (hr >= 0 && s_g_ang[hr] != ROW_HIDDEN) {
            const double ph = (double)(esp_timer_get_time() % 400000) / 400000.0 * 2.0 * M_PI;
            const int32_t add = (int32_t)(sin(ph) * s_bench_osc_deg * 10.0);
            const int32_t base = s_g_ang[hr];
            for (uint16_t j = 0; j < n; j++)
                if (j == hr || ((g->rows[j].flags & CLK_FLAG_AUX_SHADOW) && s_g_ang[j] == base))
                    s_spin_ang[j] = ((base + add) % 3600 + 3600) % 3600;
        }
        /* A true sweep of the seconds row: the app steps it on the wall second, so
         * the fraction of that second times one step's turn lands each frame where a
         * sweeping hand would be, and meets the next step exactly. */
        int sr = -1;
        if (s_bench_sweep)
            for (uint16_t j = 0; j < n; j++) if (g->rows[j].src == CLK_SRC_SECOND) { sr = j; break; }
        /* A row that already sweeps on its own cadence carries the fraction in its pose; adding
         * it again doubles the turn within each second and snaps the hand back a whole step on
         * the wrap — the retrograde the owner saw on Skeleton 5/20 (plan §49.15). */
        if (sr >= 0 && (g->rows[sr].cadence == CLK_CAD_SWEEP || g->rows[sr].cadence == CLK_CAD_STOP_TO_GO)) sr = -1;
        if (sr >= 0 && s_g_ang[sr] != ROW_HIDDEN && g->rows[sr].value_max > g->rows[sr].value_min) {
            const clk_row_t *R = &g->rows[sr];
            const double step = (double)R->span / (double)(R->value_max - R->value_min);
            const int32_t base = s_g_ang[sr];
            const int32_t add = (int32_t)(s_now_frac * step);
            for (uint16_t j = 0; j < n; j++)
                if (j == sr || ((g->rows[j].flags & CLK_FLAG_AUX_SHADOW) && s_g_ang[j] == base))
                    s_spin_ang[j] = ((base + add) % 3600 + 3600) % 3600;
        }
        int m = -1;
        for (uint16_t j = 0; j < n; j++) if (g->rows[j].src == CLK_SRC_MINUTE) { m = j; break; }
        if (s_bench_spin_dps && m >= 0 && s_g_ang[m] != ROW_HIDDEN) {
            const int64_t ms = esp_timer_get_time() / 1000;
            const int32_t add = (int32_t)((ms * s_bench_spin_dps / 100) % 3600);
            const int32_t base = s_g_ang[m];
            for (uint16_t j = 0; j < n; j++)
                if (j == m || ((g->rows[j].flags & CLK_FLAG_AUX_SHADOW) && s_g_ang[j] == base))
                    s_spin_ang[j] = (base + add) % 3600;
        }
        ang = s_spin_ang;
    }
#endif

    lv_display_t *disp = lv_obj_get_display(s_scr);
    for (uint16_t i = 0; i < n; i++) {
        row_state_t *st = &s_row[i];
        const int32_t a = ang[i], prev = st->ang_10;
        /* D43: a drum is drawn by the compositor alone (its classic widget would slide the
         * strip), so an LVGL render of its rows (the boot's first frame, a bounce) leaves
         * the plate there and nothing re-marks a drum standing still. It re-marks its small
         * window every tick, as a moving hand does; the flush gate sends only changed rows. */
        if (clk_theme.drum_r[i] && a != ROW_HIDDEN && prev != ROW_HIDDEN
#if CLK_BG_DIRECT_PUSH
            && orb_sched_l4_active()
#endif
            ) {
            lv_area_t w;
            gfx_drum_box(row_spr(i), clk_theme.drum_r[i], &w);
            gfx_mark_rows(w.y1 < 0 ? 0 : w.y1, w.y2 > CLK_SCR_H - 1 ? CLK_SCR_H - 1 : w.y2,
                          w.x1 < 0 ? 0 : w.x1, w.x2 > CLK_SCR_W - 1 ? CLK_SCR_W - 1 : w.x2, false);
        }
        /* A new picked frame redraws the row's box even where the pose stands still. */
        const bool fchg = row_picks(i) && s_g_frame[i] != st->frame;
        /* So does a new opacity, and a label's new string: the same box, other pixels. */
        const bool ochg = s_g_opa[i] != st->opa;
        const bool tchg = st->tplane && s_text && s_text->seq[i] != st->tseq;
#if CLK_BG_DIRECT_PUSH && CONFIG_ORB_DEBUG_HTTP
        /* A label's new string lands in the same plane under the same pose: pre-paint's
         * planes still hold the old one, and only rows rebuilt for something else showed
         * the new (the radar's time read half -40, half -50; 2026-11-08). Its box goes stale,
         * composed whole this tick from the new string. */
        if (tchg && orb_sched_l4_active() && st->ang_10 != ROW_HIDDEN) {
            lv_area_t tb;
            row_mark_box(i, st->ang_10, st->ox, st->oy, &tb);
            gfx_prep_stale(tb.y1 < 0 ? 0 : tb.y1, tb.y2 > CLK_SCR_H - 1 ? CLK_SCR_H - 1 : tb.y2,
                           tb.x1 < 0 ? 0 : tb.x1, tb.x2 > CLK_SCR_W - 1 ? CLK_SCR_W - 1 : tb.x2);
        }
#endif
        if (!fchg && !ochg && !tchg && a == prev && ox[i] == st->ox && oy[i] == st->oy) continue;
        if (!fchg && !ochg && !tchg && a != ROW_HIDDEN && prev != ROW_HIDDEN && ox[i] == st->ox && oy[i] == st->oy) {
            int32_t d = a - prev;
            if (d > 1800) d -= 3600;
            if (d < -1800) d += 3600;
            if (abs((int)d) < ROW_REDRAW_EPS_10) continue;
        }

        /* The wedge to rebuild: where the row was and where it is going. The carriage
         * alone on its arbour marks its rotated ink instead of the box when it can. */
        lv_area_t u;
        bool have = false;
        const bool car = i == clk_theme.car_row && clk_theme.tourbillon_n;
        const bool inked = car && prev != ROW_HIDDEN && a != ROW_HIDDEN &&
                           !ox[i] && !oy[i] && !st->ox && !st->oy &&
                           tourbillon_ink_mark((int)s_tourb_shown, prev, a);
        /* Each pose is marked by its ink spans, a new opacity included: the same ink at
         * another weight. (v1.2.14 marked the whole box on an opacity change, for the hour
         * bars that lit only along their inner part after a page; the two causes below
         * were found in v1.2.16, and with the box mark gone the ring pages back to the
         * same pixels, Gold 2026-11-07.) */
        if (!inked && prev != ROW_HIDDEN && !ROW_MARK_SPANS(i, prev, st->ox, st->oy)) {
            row_mark_box(i, prev, st->ox, st->oy, &u); have = true;
        }
        /* A new frame is taken between the two marks: the outgoing pose is marked by the
         * ink it shows, the incoming by the ink it will. Marked by the outgoing frame's
         * spans, a picked row under-covered its new ink (weather's hour bars lit only
         * along their inner part after a page, 2026-11-06; snow's flakes left standing
         * when the rain came, v1.2.16). The frames share the row's geometry, so a box
         * mark is the same either side. The src setter's own invalidation is off for the
         * carriage's reason (tourbillon_advance). */
        if (fchg) {
            st->frame = s_g_frame[i];
            pick_sprite(i, st->frame, &st->fspr);
        }
        if (!inked) {
            if (a != ROW_HIDDEN && !ROW_MARK_SPANS(i, a, ox[i], oy[i])) {
                lv_area_t nn;
                row_mark_box(i, a, ox[i], oy[i], &nn);
                if (have) gfx_area_union(&u, &nn); else { u = nn; have = true; }
            }
        }
        if (have) {
            if (u.x1 < 0) u.x1 = 0;
            if (u.y1 < 0) u.y1 = 0;
            if (u.x2 > CLK_SCR_W - 1) u.x2 = CLK_SCR_W - 1;
            if (u.y2 > CLK_SCR_H - 1) u.y2 = CLK_SCR_H - 1;
#if CLK_BG_DIRECT_PUSH
            if (orb_sched_l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
            else
#endif
                lv_obj_invalidate_area(s_scr, &u);
        }
        /* Stage the INCOMING pose before anything reads it: the commit hook and the
         * classic widget both draw from it, and a mark computed from one travel with a
         * paint reading another is the ghost the swept-box contract exists to prevent. */
        st->ang_10 = a;
        st->ox = ox[i]; st->oy = oy[i];
        if (fchg) {
            if (st->obj) {
                gfx_fill_sprite_dsc(&st->dsc, &st->fspr, blob());
                lv_display_enable_invalidation(disp, false);
                lv_image_set_src(st->obj, &st->dsc);
                lv_display_enable_invalidation(disp, true);
            }
        }
        if (tchg)                    /* in place: LVGL keeps no image cache here
                                      * (CONFIG_LV_CACHE_DEF_SIZE 0), so the box the
                                      * marks above cover redraws the new pixels */
            label_compose(i);
        if (ochg) {
            st->opa = s_g_opa[i];
            if (st->obj) {
                lv_display_enable_invalidation(disp, false);
                lv_obj_set_style_image_opa(st->obj, st->opa, LV_PART_MAIN);
                lv_display_enable_invalidation(disp, true);
            }
        }
        if (a != ROW_HIDDEN)
            pose_spr(row_picks(i) ? &st->fspr : &g->rows[i].spr, ox[i], oy[i], &st->pose);
        /* D43: only the compositor draws a drum (the classic widget would slide the
         * strip, not roll it); a theme with one is refused past the layer cap at bake */
        if (clk_theme.drum_r[i]) { if (st->obj) lv_obj_set_hidden(st->obj, true); continue; }
        if (!st->obj) continue;
        /* One pose, two setters, with LVGL's own invalidation off: a rotation setter
         * invalidates the FULL transformed box of both poses (the 36-48 ms a rotating
         * layer used to cost every tick), and the marks above already cover the change.
         * The first pose ever needs LVGL's own render, because nothing has painted
         * these pixels yet. */
        if (a == ROW_HIDDEN) { lv_obj_set_hidden(st->obj, true); continue; }
        if (prev == ROW_HIDDEN) lv_obj_set_hidden(st->obj, false);
        else lv_display_enable_invalidation(disp, false);
        const clk_sprite_t *sp = row_spr(i);
        lv_obj_set_pos(st->obj, (lv_coord_t)(sp->centerx - sp->pivotx + ox[i]),
                       (lv_coord_t)(sp->centery - sp->pivoty + oy[i]));
        lv_image_set_rotation(st->obj, a);
        if (prev != ROW_HIDDEN) lv_display_enable_invalidation(disp, true);
    }
}

/* ---- D39: the clock's sound policy (docs/history/sound-plan.md §5) --------------------
 *
 * The theme carries the sounds (orb_theme_sounds), the OS plays them (orb_sound),
 * and this is the part that is the clock's own: which sound, and when. It runs off
 * compose_now's one wall reading, so a chime and the hands agree on the minute.
 *
 *   - The tick loop runs while the dial ticks, anchored to a wall second so beat k
 *     lands with step k of the seconds hand (§4.2). The projection already refused
 *     a loop whose beat disagrees with a stepping hand.
 *   - At a minute boundary: :15/:30/:45 sound the quarter chimes, :00 the hour
 *     chime and then the hour pattern (§5.1), each queued for its exact instant.
 *   - Every minute, hour and date change sounds its jump slot when the theme
 *     carries one (D41's step events).
 *
 * Every voice is LEASED and the lease is renewed each tick, so when the scheduler
 * parks this dial the sounds stop with it. A boundary seen more than two seconds
 * late, or a clock that jumped (an SNTP step), sounds nothing: a chime belongs to
 * the instant, not to the catching-up.
 *
 * State lives in PSRAM behind one pointer: this board has no internal RAM to spare
 * (orb_sound.c's header has the measurement). */

/* D41 G5: a jump slot is the sound of a row stepping (docs/history/sound-plan.md §3.1), so it
 * plays only on a dial where some row reading that source steps, and on the instant
 * it does. Three classes, one per slot: minute, hour, calendar. */




static orb_app_t   *s_lay_app;   /* the clock's row (defined with the layer table below) */









#if CONFIG_ORB_DEBUG_HTTP
/* radar-plan R3c's bench (/api/v1/debug/live): the door posts what an app would do with
 * the live layer, and the stage does it here, on the LVGL task, as the app's frame would. */
static stage_live_bench_t s_lb;
static int s_lb_state;                   /* 0 idle, 1 posted, 3 running, 2 done */

static void live_bench_step(void)
{
    int posted = 1;
    if (!__atomic_compare_exchange_n(&s_lb_state, &posted, 3, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    if (s_lb.veto >= 0) {                /* as an app widget over the theme would */
        orb_sched_set_l4_veto(s_lb.veto != 0);
        lv_obj_invalidate(s_scr);
    }
    const int64_t t0 = esp_timer_get_time();
    s_lb.rc = stage_live_open(s_lb.layer, s_lb.order, s_lb.argb);
    const int64_t t1 = esp_timer_get_time();
    orb_ui_tgt_t t;
    orb_ui_pal_t tp;
    if (s_lb.rc == ESP_OK && stage_live_tgt(s_lb.layer, &t, &tp)) {
        if (s_lb.ras) {
            orb_ui_tgt_roles(&t, s_lb.ras, s_lb.pal, 0, 0);
            stage_live_damage(0, 0, CLK_SCR_W, CLK_SCR_H);
            orb_map_credit_drawn(s_lay_app);    /* as orb_map_draw does */
        }
        s_lb.roles_us = (uint32_t)(esp_timer_get_time() - t1);
        const int64_t t2 = esp_timer_get_time();
        if (s_lb.png) {
            s_lb.png_rc = orb_ui_tgt_png8(&t, s_lb.png, s_lb.png_len, s_lb.png_sx, s_lb.png_sy, 0, 0,
                                          CLK_SCR_W, CLK_SCR_H, 1, s_lb.png_pal);
            stage_live_damage(0, 0, CLK_SCR_W, CLK_SCR_H);
        }
        s_lb.png_us = (uint32_t)(esp_timer_get_time() - t2);
    }
    s_lb.open_us = (uint32_t)(t1 - t0);
    s_lb.l4 = orb_sched_l4_active();
    s_lb.rows = orb_svc_layers_live(s_lay_app);
    __atomic_store_n(&s_lb_state, 2, __ATOMIC_RELEASE);
}

bool stage_bench_live(stage_live_bench_t *io, int timeout_ms)
{
    if (__atomic_load_n(&s_lb_state, __ATOMIC_ACQUIRE) != 0) return false;
    s_lb = *io;
    __atomic_store_n(&s_lb_state, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < timeout_ms / 10 && __atomic_load_n(&s_lb_state, __ATOMIC_ACQUIRE) == 1; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    /* Not taken in time: withdraw it, so a late frame never reads the caller's buffers
     * after they are freed. Taken: it is running, and finishes. */
    int posted = 1;
    if (__atomic_compare_exchange_n(&s_lb_state, &posted, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;                      /* the stage is not ticking: nothing on the glass */
    while (__atomic_load_n(&s_lb_state, __ATOMIC_ACQUIRE) != 2) vTaskDelay(pdMS_TO_TICKS(5));
    *io = s_lb;
    __atomic_store_n(&s_lb_state, 0, __ATOMIC_RELEASE);
    return true;
}
#endif

static void compose_now(void)
{
    /* The wall reading is the OS's now (orb_time_now, plan §4.3). */
    bool   synced_unused;
    time_t t;
    double frac;
    orb_time_now(&t, &frac, &synced_unused);

    /* Delta-bank cadence: the tick delivers exactly one bank frame per tick, so a
     * sweeping dial's beat rides the TICK COUNTER, not a wall-clock grid. The dial's
     * beat (tick_rate, derived from its seconds rows) becomes ticks-per-beat
     * (hand_beat_ticks): 4 bps on a 12 fps tick = sample every 3rd tick, always the
     * 3rd. On each beat the fraction is sampled from the wall clock (re-anchored at
     * every second boundary); between beats it holds, so the redraw gate skips the
     * re-pose and the tick's commit carries bank rows only. This is the stage's
     * knowledge of its own frames, so it stays here; the app reads the held fraction
     * through stage_now and every row it poses beats together. */
    const double wall_frac = frac;
    const int sec = (int)(t % 60);
    if (clk_theme.second_sweep && clk_theme.tick_rate > 0 && orb_bg_gif_mode()) {
        const uint32_t tpb = hand_beat_ticks();
        if (tpb > 1) {
            if (sec != s_bt_sec) {              /* true seconds re-anchor the phase */
                s_bt_sec = sec;
                s_bt_ctr = 0;
            }
            if ((s_bt_ctr % tpb) == 0) s_bt_frac = frac;
            s_bt_ctr++;
            frac = s_bt_frac;
        }
    }
    s_now_t = t;
    s_now_frac = frac;
    s_now_wall = wall_frac;

    /* The app's frame: it poses every row and sets every complication off this one
     * reading. Its voices are leased, so renewing here is what keeps them sounding
     * exactly as long as the dial keeps ticking. */
    if (orb_sound_ready()) orb_sound_lease_renew(s_lay_app);   /* the bound app's loops alone */
    if (s_lay_app) orb_wasm_tick(s_lay_app, lv_tick_get());
#if CONFIG_ORB_DEBUG_HTTP
    live_bench_step();
#endif

    rows_update();
    comps_update();
}



/* ---- the layer table: the dial's rows, claimed the way every app claims ----
 * (Migration 2, Commit 3.)
 *
 * The rows come from the bundle, not from the geometry adapter: the adapter is
 * what the projection builds, and the projection is what this migration
 * deletes. So the adopted section is opened (orb_theme_live_sec — the same
 * bytes the dial is drawing, proven at the same door), its element table is
 * walked in the theme's own depth order, and every row is answered by
 * orb_elems_at(), the element dialect's reader, before it becomes an
 * orb_layer_desc_t the broker gates and the compositor commits.
 *
 * The fold. orb_elems_at() answers a plane's offset inside its section's own
 * bank; orb_asset_t.off is documented payload-relative (orb_assets.h), and the
 * commit pass resolves a base-less row against art_bank_ptr() (orb_sched.c).
 * The two conventions meet HERE, at the resolve step: off = plane + bank_off,
 * where bank_off is the figure the getter answers. Section 14 of
 * tools/host_orblayers_test.py pins both readings against the shipped hook:
 * the folded row lands the element's own pixels, an unfolded one lands its
 * neighbour's.
 *
 * Rows enter the table hidden — `show = 0`, live and counted, painting
 * nothing — and the tick's pose pass below (clock_layers_refresh) wakes and
 * drives them: the legacy renderer that owned this dial's composite until
 * Commit 4 is deleted, the scheduler's flush hook is the only commit path,
 * and the pass line is the ledger: the owner's app row reports the live count
 * equal to the bake's runtime rows, refused 0, and the frame byte-identical
 * against the dial-c3 baselines.
 *
 * What is not a row: the plate (class plate slot 0 — the canvas is the plate,
 * and a row for it would blend the background over its own restore), a sound,
 * and a switched-off row (EF_HIDDEN), which is a theme's own off switch and not a
 * refusal. A still layer the bake folded into the plate is simply not shipped.
 */

/* One claimed row: the identity the bundle named, what the pose pass needs
 * to derive its current pose, and the descriptor as last pushed through the
 * gate — the pose shadow the per-tick diff reads. Commit 4 grew the record
 * for exactly that; Commit 3 only remembered (cls, slot). */
typedef struct {
    uint16_t cls, slot;        /* the identity, echoed back for logs          */
    uint16_t frame0, n_frames; /* the element's own frame window into the bank */
    int16_t  x, y;             /* theme placement — what a slide displaces     */
    uint8_t  kind;             /* BANK: CLK_KIND_*; AUX: CLK_SHAPE_*; else 0   */
    uint8_t  gidx;             /* BANK/AUX: this row's index in the geo table
                                * the tick state is indexed by; 0xFF = none */
    orb_layer_desc_t d;        /* the pose the table holds right now          */
} clk_layer_key_t;
#define CLK_EF_AUX_TWIN   0x10u  /* theme_schema EF_AUX_SHADOW, the twin bit */
#define CLK_LAYER_KEYS_MAX (2 + CLK_ROW_MAX + CLK_COMP_MAX)
                                /* the clock's whole drawn vocabulary: 2 plates, the
                                 * moving rows, 16 complications. An ADOPTED table
                                 * cannot exceed it — orb_clock3.c refuses a row
                                 * outside those cells and a duplicate inside — so the
                                 * bound here is a belt, not a budget. */
/* 2026-10-24: PSRAM heap, lazy in clock_layers_build. As .bss these 2,960
 * bytes were the whole internal-RAM regression that starved httpd_start of
 * its 8 kB task stack (ESP_ERR_HTTPD_TASK, boot 2026-10-24: largest internal
 * block 10,240 with the ladder already logged). The pose pass reads the rows
 * once per tick and the broker's per-app row table has lived on PSRAM since
 * P5.5 (svc_layers_alloc, orb_svc.c) — same rate, same memory, now the same
 * heap. NULL until the first claim; every reader sits behind s_lay_n > 0 or
 * the build's own refusal path. */
static clk_layer_key_t *s_lay_key;
static int *s_lay_handle;
static int s_lay_n;
/* s_lay_app — the row the table is claimed under — is declared above sound_policy. */
/* The section and bank base the rows' frame offsets resolve against. The
 * adopted bundle changes only at boot — the store door stages, the flip
 * worker reboots — and this pass rebuilds on any re-show, so caching what
 * clock_layers_build opened is safe. NULL with s_lay_n stays louder than a
 * wrong answer: the pass keeps the glass it has. */
static const orb_bundle_sec_t *s_lay_sec;
static uint32_t s_lay_bank_off;

#define CLK_EF_HIDDEN  0x08u    /* theme_schema EF_HIDDEN, the mirror orb_clock3.c keeps */

/* The one stacking key both paint paths sort by (D42): the row's depth, and at a tie
 * a shadow twin goes under every row at that depth, so it falls beneath the row
 * casting it. Equal keys keep table order. stage_init's creation walk and
 * clock_layers_build's sort both call this, so a row LVGL renders and a row the L4
 * hook paints can never stack differently. */
static uint16_t depth_key(uint8_t order, bool twin)
{
    return (uint16_t)((uint16_t)order * 2u + (twin ? 0u : 1u));
}

/* The classic path's z is the page's child order, so each widget stage_init builds
 * carries its depth key + 1 as user data (the plate 1, the glass 0x10000): the live
 * layer's image goes in before the first widget deeper than it. Children from `*from`
 * to the last take `key`. */
static void live_tag_from(uint32_t *from, uint16_t key)
{
    const uint32_t n = lv_obj_get_child_count(s_scr);
    for (uint32_t i = *from; i < n; i++)
        lv_obj_set_user_data(lv_obj_get_child(s_scr, (int32_t)i), (void *)(uintptr_t)(key + 1u));
    *from = n;
}

/* The live layer as a table row: the full glass at 0,0, never turned, its pixels the
 * stage's plane (the glass overlay's recipe). */
static size_t live_bytes(int l);
static void live_desc(orb_layer_desc_t *d, int l)
{
    memset(d, 0, sizeof *d);
    d->art.cls   = ORB_ASSET_PLATE;
    d->art.tier  = ORB_ASSET_OK;
    d->art.w     = CLK_SCR_W;
    d->art.h     = CLK_SCR_H;
    d->art.prim  = (uint16_t)orb_asset_class_prim(ORB_ASSET_PLATE);
    d->art.off   = live_fmt(l) == LF_I8 ? LIVE_I8_IDX : LIVE_OFF;
    d->art.bytes = (uint32_t)(live_bytes(l) - d->art.off);
    d->role      = CLK_ROLE_AUX;
    d->gen_prim  = 0xFFu;
    d->alpha     = 255;
    d->plane     = s_live[l];
    d->solid     = (s_live_merged && l == 1) || (!s_live_merged && s_live_fmt[l] == LF_RGB);
    d->indexed   = live_fmt(l) == LF_I8;
}

/* One element's row. The descriptor's own placement, pivot and blend stay
 * zero: the resolver's box and pivot answer them (orb_layers.c's recipe); the
 * FORMAT 3 dialect carries no blend column (a shadow is its own alpha plate);
 * and the angle starts at the theme's fixed degrees — the wall angle is what
 * Commit 4's tick adds, per row, per beat. */
static void clk_row_desc(const orb_elem_t *e, uint32_t bank_off, uint8_t role,
                         orb_layer_desc_t *d)
{
    d->art.cls     = e->cls;
    d->art.tier    = ORB_ASSET_OK;                    /* the theme answered it  */
    d->art.x       = e->x;
    d->art.y       = e->y;
    d->art.w       = (int16_t)e->w;
    d->art.h       = (int16_t)e->h;
    d->art.pivotx  = e->pivot_x;
    d->art.pivoty  = e->pivot_y;
    d->art.prim    = (uint16_t)orb_asset_class_prim((orb_asset_class_t)e->cls);
    d->art.global  = 0;                               /* the clock's namespace  */
    d->art.off     = bank_off + e->off;               /* the fold — see above   */
    d->art.bytes   = e->size;
    d->ang_10      = (int16_t)(e->degrees / 10u);     /* hundredths -> tenths   */
    d->show        = 0;                               /* Commit 4 wakes it      */
    d->role        = role;
    d->gen_prim    = 0xFFu;                           /* the answer's own prim  */
    d->alpha       = 255;
    /* The painter's half of row_aa's answer: the bake's bit, on a moving row. */
    d->smooth      = e->cls == ORB_ASSET_AUX && (e->flags & CLK_EF_SMOOTH) ? 1u : 0u;
}

/* The claim, handed to the OS, and the verdict it buys printed beside it: one
 * line per build pass — first boot and every re-apply — so a capture names the
 * path the dial drew through rather than leaving it to be inferred. */
static void clock_layers_claim(int rows, int refused)
{
    orb_sched_set_layer_claim(rows, refused);
#if CLK_BG_DIRECT_PUSH
    const bool l4 = orb_sched_l4_active();
    ESP_LOGI(TAG, "render path: l4=%s src=%s rows=%d refused=%d", l4 ? "yes" : "no",
             !l4 ? "lvgl" : (orb_bg_gif_mode() ? "psram" : "xip"), rows, refused);
#else
    ESP_LOGI(TAG, "render path: l4=no src=lvgl rows=%d refused=%d (direct push off)",
             rows, refused);
#endif
}

static void clock_layers_build(orb_app_t *self)
{
    /* Reset before either early exit below: a build that claims nothing must not
     * leave the previous dial's claim answering the gate. */
    orb_sched_set_layer_claim(0, 0);
    const orb_svc_t *svc = orb_svc_table();
    /* The broker wipes an app's rows when it goes behind the glass
     * (orb_svc_app_suspended — P5.5's clause that an app behind the dial owns
     * no pixels), and the wipe kills the handles behind this file's back. So
     * the delete-first pass runs only over a live table; behind a wipe the
     * handles are already dead, the table itself survives for the re-add, and
     * deleting them here would bill the ledger refusals the app never made. */
    if (orb_svc_layers_live(self) > 0)
        for (int i = 0; i < s_lay_n; i++) svc->layer_del(self, s_lay_handle[i]);
    s_lay_n = 0;
    s_lay_sec = NULL;                          /* the pose pass parks with the  */
    s_lay_bank_off = 0;                        /* rows it cannot resolve        */

    const orb_bundle_sec_t *sec = NULL;
    uint32_t bank_off = 0;
    if (!orb_theme_live_sec(orb_theme_live_slug(), &sec, &bank_off)) {
        clock_layers_claim(0, 0);   /* no adopted bundle: no rows. The base face claims none. */
        return;
    }
    /* Claim needs the two side tables before its first row: one PSRAM block
     * each, allocated once and kept for every later rebuild. An allocation
     * failure here lands in the same shape as the geo pass finding nothing:
     * zero rows claimed, the base face, and the refusal printed. */
    if (!s_lay_key) {
        s_lay_key = heap_caps_malloc(sizeof *s_lay_key * CLK_LAYER_KEYS_MAX,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_lay_handle = heap_caps_malloc(sizeof *s_lay_handle * CLK_LAYER_KEYS_MAX,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_lay_key || !s_lay_handle) {
        ESP_LOGE(TAG, "layer keys: PSRAM alloc failed — no rows claimed this build");
        clock_layers_claim(0, 0);
        return;
    }
    s_lay_sec = sec; s_lay_bank_off = bank_off;
    const orb_theme_geo_t *g_geo = orb_theme_geo();
    const int n_comps_geo = (int)g_geo->n_comps  < CLK_COMP_MAX
                            ? (int)g_geo->n_comps  : CLK_COMP_MAX;
    const int n_rows_geo  = (int)g_geo->n_rows    < CLK_ROW_MAX
                            ? (int)g_geo->n_rows   : CLK_ROW_MAX;

    /* Depth first, table order second (depth_key, the classic path's key too).
     * The insert sort is over at most CLK_LAYER_KEYS_MAX entries before anything
     * is claimed. The glass rides above everything. */
    uint16_t pick_cls[CLK_LAYER_KEYS_MAX], pick_slot[CLK_LAYER_KEYS_MAX];
    uint16_t pick_ord[CLK_LAYER_KEYS_MAX];
    int n = 0, hidden = 0;
    for (uint32_t i = 0; i < sec->n_elem && n < CLK_LAYER_KEYS_MAX; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        if (e.cls == ORB_ASSET_PLATE && e.slot == 0u) continue;   /* the canvas */
        if (e.cls == ORB_ASSET_SOUND) continue;   /* D39: played by sound_policy, never drawn */
        if (e.cls == ORB_ASSET_VALUES) continue;  /* radar-plan §2.11: the app's settings, never drawn */
        if (e.cls == ORB_ASSET_PREVIEW) continue; /* the picker's picture, never on the glass:
                                                   * claimed, it cost a row of an app's 16 */
        if (e.cls == ORB_ASSET_TEXT) continue;    /* a font: its glyphs reach the glass in
                                                   * a label's plane, never where it stands
                                                   * (theme-text-plan §2.1) */
        if (e.flags & CLK_EF_HIDDEN) { hidden++; continue; }
        pick_cls[n] = e.cls; pick_slot[n] = e.slot;
        pick_ord[n] = e.cls == ORB_ASSET_PLATE ? 0xFFFFu
                    : depth_key(e.order, e.cls == ORB_ASSET_AUX && (e.flags & CLK_EF_AUX_TWIN));
        n++;
    }
    for (int l = 0; l < STAGE_LIVE_N && n < CLK_LAYER_KEYS_MAX; l++) {
        /* radar-plan §2.9: a live layer, last among the rows at its depth, layer 0 first */
        if (!s_live[l] || s_live_order[l] < 0) continue;
        pick_cls[n] = LIVE_PICK; pick_slot[n] = (uint16_t)l;
        pick_ord[n] = depth_key((uint8_t)s_live_order[l], false);
        n++;
    }
    for (int a = 1; a < n; a++) {                 /* stable: equal depths keep table order */
        const uint16_t kc = pick_cls[a], ks = pick_slot[a], ko = pick_ord[a];
        int b = a - 1;
        while (b >= 0 && pick_ord[b] > ko) {
            pick_ord[b + 1] = pick_ord[b];
            pick_cls[b + 1] = pick_cls[b];
            pick_slot[b + 1] = pick_slot[b];
            b--;
        }
        pick_ord[b + 1] = ko;
        pick_cls[b + 1] = kc;
        pick_slot[b + 1] = ks;
    }

    int refused = 0;
    for (int k = 0; k < n; k++) {
        if (pick_cls[k] == LIVE_PICK) {
            orb_layer_desc_t d;
            live_desc(&d, pick_slot[k]);
            const int h = svc->layer_add(self, &d);
            if (h < 0) { refused++; continue; }
            clk_layer_key_t *r = &s_lay_key[s_lay_n];
            memset(r, 0, sizeof *r);
            r->cls = LIVE_PICK;
            r->slot = pick_slot[k];
            r->gidx = 0xFFu;
            r->d = d;
            s_lay_handle[s_lay_n++] = h;
            continue;
        }
        orb_elem_t el;
        char why[160];
        const orb_elems_ans_t a = orb_elems_at(sec, pick_cls[k], pick_slot[k], 0u,
                                               &el, why, sizeof why);
        if (a != ORB_ELEMS_OK) {
            /* An adopted table answers OK to its own rows; anything else is
             * stated once and skipped rather than half-claimed. */
            ESP_LOGW(TAG, "layer row %u/%u: %s", (unsigned)pick_cls[k],
                     (unsigned)pick_slot[k], why);
            refused++;
            continue;
        }
        /* The tick state — s_comp_frame, s_row — is indexed by the GEO table's
         * own slot, which a dense bake makes the element slot and a sparse one
         * not. Resolve it once here, against the same projection the update
         * passes walk, and the pose pass never guesses. */
        uint8_t gidx = 0xFFu, role = CLK_ROLE_AUX;
        if (pick_cls[k] == ORB_ASSET_BANK && el.kind == CLK_KIND_WINDOW) {
            role = CLK_ROLE_COMP;
            for (int c = 0; c < n_comps_geo; c++)
                if (g_geo->comps[c].tag == el.slot) { gidx = (uint8_t)c; break; }
        } else if (pick_cls[k] == ORB_ASSET_AUX) {
            for (int c = 0; c < n_rows_geo; c++)
                if (g_geo->rows[c].tag == el.slot) {
                    gidx = (uint8_t)c;
                    role = row_role(&g_geo->rows[c]);
                    break;
                }
        } else if (pick_cls[k] == ORB_ASSET_PLATE) {
            role = CLK_ROLE_GLASS;
        }
        orb_layer_desc_t d;
        memset(&d, 0, sizeof d);
        clk_row_desc(&el, bank_off, role, &d);
        if (role == CLK_ROLE_GLASS && clk_theme.overlay_disc) {
            /* the expanded disc: this table may be built before the glass widget is */
            d.plane = s_overlay_sq ? s_overlay_sq : overlay_square();
            d.art.off = OVERLAY_SQ_OFF;
            d.art.bytes = (uint32_t)CLK_SCR_W * CLK_SCR_H * 3u;
        }
        if (gidx != 0xFFu && pick_cls[k] == ORB_ASSET_AUX && s_row && s_row[gidx].tplane) {
            d.plane = s_row[gidx].tplane;      /* a label: the plane the stage composes */
            d.art.off = LABEL_PLANE_OFF;
        }
        const int h = svc->layer_add(self, &d);
        if (h < 0) {                          /* full, malformed, or no heap: the
                                               * table counted it; the walk moves on */
            if (!refused)                     /* the first, named (a fill's landing, C5) */
                ESP_LOGW(TAG, "layer row %u/%u refused: tier %u cls %u %dx%d off %lu blend %u role %u prim %u",
                         (unsigned)pick_cls[k], (unsigned)pick_slot[k], (unsigned)d.art.tier,
                         (unsigned)d.art.cls, (int)d.art.w, (int)d.art.h, (unsigned long)d.art.off,
                         (unsigned)d.blend, (unsigned)d.role, (unsigned)d.gen_prim);
            refused++;
            continue;
        }
        clk_layer_key_t *r = &s_lay_key[s_lay_n];
        r->cls = pick_cls[k]; r->slot = pick_slot[k];
        r->frame0 = el.frame0; r->n_frames = el.n_frames;
        r->x = el.x; r->y = el.y;
        r->kind = (uint8_t)el.kind;
        r->gidx = gidx;
        r->d = d;                               /* the pose the table holds now */
        s_lay_handle[s_lay_n] = h;
        s_lay_n++;
    }

    ESP_LOGI(TAG, "layer table: %d row%s, %d hidden, %d refused (base %lu)",
             s_lay_n, s_lay_n == 1 ? "" : "s", hidden, refused, (unsigned long)bank_off);
    clock_layers_claim(s_lay_n, refused);
}

/* The per-tick pose pass (Migration 2, Commit 4): what the deleted
 * clock_layer_commit's job-list walk did at the flush, done against the wall
 * state at the tick instead — once per row, on the numbers that pass already
 * kept, and nothing reaches the gate twice.
 *
 * Each row's pose is DERIVED, never stored twice: a moving row reads the pose
 * rows_update just staged in s_row (the carriage at the frame tourbillon_advance
 * just composited), a BANK row the frame the complication pass just selected,
 * and the glass is the static it was baked as. A row never posed (ROW_HIDDEN)
 * or a comp frame of -1 wakes hidden and stays that way.
 *
 * Diff first: an hour hand that stands still costs one integer compare. A 0->1
 * transition also TOUCHES: that is the one damage a mark site cannot have
 * computed, because until now the row drew nothing and its box never entered the
 * row bank. Vacated poses need no touch here — every move-mark (the row wedges,
 * the comp boxes, the tourbillon diff) marks old and new together, and the flush
 * weld restores and repaints both. */
static void clock_layers_refresh(void)
{
    if (s_lay_n == 0 || !orb_sched_l4_active())
        return;                       /* base face, classic path: nothing to feed */
    const orb_svc_t *svc = orb_svc_table();
    for (int i = 0; i < s_lay_n; i++) {
        clk_layer_key_t *r = &s_lay_key[i];
        orb_layer_desc_t d = r->d;    /* start from the pose the table holds */
        uint8_t show = 0;

        /* The visibility gates: a row whose widget the dial did not build, or whose
         * pose the update pass hid, stays hidden here for the same reason. */
        switch (r->cls) {
        case ORB_ASSET_PLATE:
            if (s_overlay) show = 1;  /* the glass: built and static */
            break;
        case LIVE_PICK:
            show = r->slot < STAGE_LIVE_N && s_live[r->slot] && !(s_live_merged && r->slot == 0)
                   ? 1 : 0;                       /* its plane changes, its pose never; a
                                                  * merged layer 0 is layer 1's backing */
            if (show) d.plane = s_live[r->slot];  /* v1.2.38: a present swapped the front in */
            break;
        case ORB_ASSET_BANK: {
            /* FORMAT 3 admits no comp kind but the window, so one row is one
             * plate and the complication pass' own selection answers which. */
            const int32_t f = (r->gidx != 0xFFu && r->kind == CLK_KIND_WINDOW)
                              ? s_comp_frame[r->gidx][0] : -1;
            if (f >= 0 && (uint32_t)f < r->n_frames) {
                orb_bundle_spr_t sp;
                orb_bundle_spr_at(s_lay_sec, (uint32_t)(r->frame0 + (uint32_t)f), &sp);
                d.art.off = s_lay_bank_off + sp.off;
                d.art.bytes = sp.size;
                show = 1;
            }
            break; }
        case ORB_ASSET_AUX: {
            const row_state_t *st = (r->gidx != 0xFFu && s_row) ? &s_row[r->gidx] : NULL;
            if (st && st->obj && st->ang_10 != ROW_HIDDEN) {
                /* A travelled pose reaches the compositor as the plate at its placed
                 * origin moved by the update pass's own figure — the number the marks
                 * and the widget read — and turned about its moved pivot. */
                const uint16_t drum_r = r->gidx < CLK_ROW_MAX ? clk_theme.drum_r[r->gidx] : 0;
                if (drum_r) {
                    /* D43: a drum stays where it is placed and rolls its strip by the
                     * travel the app posed, the same number its marks read */
                    d.x = 0; d.y = 0;
                    d.drum = 1; d.drum_r = (int16_t)drum_r; d.drum_phase = st->oy;
                } else if (st->ox || st->oy) {
                    d.x = (int16_t)(r->x + st->ox);
                    d.y = (int16_t)(r->y + st->oy);
                    d.drum = 0;
                } else {
                    d.x = 0; d.y = 0;   /* the resolver's own placement */
                    d.drum = 0;
                }
                d.ang_10 = drum_r ? 0 : (int16_t)st->ang_10;
                d.alpha = st->opa;
                if (row_picks(r->gidx) && st->frame >= 0 && (uint32_t)st->frame < r->n_frames) {
                    /* A picked row, at the frame the app picked: the plane the widget
                     * shows (theme-pool-plan §2.6). Frame 0 too: d starts from the
                     * description the table holds, so a row picked back to its first
                     * frame kept showing the last one (weather's rain drew snow's
                     * flakes, its hour bars their old lengths; v1.2.16). */
                    orb_bundle_spr_t sp;
                    orb_bundle_spr_at(s_lay_sec, (uint32_t)(r->frame0 + st->frame), &sp);
                    d.art.off = s_lay_bank_off + sp.off;
                    d.art.bytes = sp.size;
                }
                if (r->gidx == clk_theme.car_row && clk_theme.tourbillon_n) {
                    /* The carriage, at the frame the wall clock just picked — the
                     * frame tourbillon_advance marks against. */
                    int f = s_tourb_shown;
                    if (f < 0 || f >= (int)r->n_frames) f = 0;
                    orb_bundle_spr_t sp;
                    orb_bundle_spr_at(s_lay_sec, (uint32_t)(r->frame0 + f), &sp);
                    d.art.off = s_lay_bank_off + sp.off;
                    d.art.bytes = sp.size;
                }
                show = 1;
            }
            break; }
        default:
            break;
        }
        d.show = show;
        if (memcmp(&d, &r->d, sizeof d) == 0) continue;
        /* A row's first box: no mark site knew it. A drum also wakes the first time it is
         * described as one (D43): its classic widget stays hidden, so the boot's LVGL render
         * never drew it, and a drum standing still never marks its own window. */
        const bool wake = (!r->d.show && show) || (d.drum && !r->d.drum);
        svc->layer_set(s_lay_app, s_lay_handle[i], &d);
        r->d = d;
        if (wake)                     /* the row's first box: no mark site knew */
            svc->layer_touch(s_lay_app, s_lay_handle[i], NULL);
    }
}

esp_err_t stage_init(orb_app_t *self)
{
    /* B1/B2, and the owner's rule that file management is the OS's: the clock does
     * not walk /sd itself and never will. It declares the one directory it reads
     * and the one extension a file must carry to be one of its themes, and the OS
     * (orb_files) owns the mount, the card lock, the name validation and the
     * enumeration. That declaration is also how the shell's options page and the
     * browser's copy of it can list this app's files without either one knowing
     * what an .orbtheme is — see main/sd/orb_files.h.
     *
     * The declaration itself lives in orb_theme.c, and this call re-declares it:
     * orb_theme_boot() put it in the table on the boot ladder, before any door
     * task could ask, so a hub that installed and enabled the clock over the wire
     * meets a theme door that already knows where the themes live. The values are
     * one copy in the tree, and a second registration is what orb_files_register
     * takes an app's init running twice to be. */
    const esp_err_t fe = orb_theme_src_register();
    if (fe != ESP_OK)
        ESP_LOGE(TAG, "themes file source refused (%s) — the theme list will be empty",
                 esp_err_to_name(fe));

    /* The bank was already found and mapped by orb_theme_boot(), which ran BEFORE
     * display_init() — a theme install must complete before anything reads pixels,
     * and only orb_theme knows which A/B slot the bank is in. §26: residency is
     * decided HERE, before either leg, and it is the one thing the two legs diverge
     * on. A resident bank — the flashed default at offset 0, or a slot-B container —
     * draws the dial on the themed leg below. No resident bank draws the base digital
     * face, the fallback for a board that has lost its theme. Residency is checked
     * before art is demanded so a bankless board is never failed at an art gate and
     * left on the panel's reset black.
     *
     * The middle term is the projection verdict, and it is not redundancy: mapping
     * and adoption are two separate facts, and the arithmetic alone reads
     * `bytes >= blob_off + art_size` over a zeroed geo — after a refusal's
     * use_no_art() that is `bytes >= 0`, true — sending this leg into orb_bg_init
     * over pixels that are not a bank. Bronze panicked in exactly that loop on
     * 2026-10-22. The theme answers the one honest question here — is there an
     * adopted, drawable theme — and the arithmetic stays what it always was: does
     * the mapping cover the blob. */
    const bool art_resident = art_bank_ready()
                    && orb_theme_resident()
                    && art_bank_bytes() >= orb_theme_geo()->blob_off
                                            + orb_theme_geo()->art_size;

    s_lay_app = self;
    s_scr = orb_ui_screen(self->flash_slot);
    if (!s_scr) return ESP_ERR_NO_MEM;
    for (int i = 0; i < CLK_ROW_MAX; i++) {
        s_g_ang[i] = ROW_HIDDEN; s_g_frame[i] = 0; s_g_opa[i] = LV_OPA_COVER;
    }

    /* §26: no resident art. The stage draws nothing and starts no frames; the page
     * stays the black one orb_ui built, and the app puts its own face on it (D31:
     * the app supplies the default, in primitives — a text readout). */
    if (!art_resident) {
        s_base = true;
        ESP_LOGW(TAG, "no resident art — the clock app shows its own face (§26)");
        return ESP_OK;
    }

    /* Themed leg: the residency flag is true here, so the mapping exists and covers
     * what the geometry says it must, and s_art is a real read rather than NULL plus
     * an offset. */
    s_art = art_bank_ptr() + orb_theme_geo()->blob_off;
    orb_bg_init(s_art);   /* the delta bank reads the same mapping, through the app */
    ESP_LOGI(TAG, "art bank mapped: %u B (theme needs %u B) from %s",
             (unsigned)art_bank_bytes(), (unsigned)orb_theme_geo()->art_size,
             orb_theme_source());

    /* Screen bg stays TRANSPARENT: the plate covers the full 466x466 opaquely,
     * so a black cover fill here would be rendered (and re-rendered per dirty
     * chunk) purely to be painted over — ~0.7 us/px of LVGL CPU time on the
     * GIF ticks. AMOLED is black out of reset; nothing can show through. */
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(s_scr, false);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);

    /* ---- plate / background source ----
     * GIF delta mode: allocate the PSRAM canvas, seed it with the keyframe,
     * and make the plate widget read ITS pixels. Classic mode: plate + baked
     * bgAnim frames come straight out of the blob (zero RAM). */
    const lv_image_dsc_t *plate_src = NULL;
    if (clk_theme.bg_mode == 1)
        plate_src = orb_bg_gif_canvas_init();
    if (!plate_src) {
        orb_bg_classic_init();
        plate_src = orb_bg_plate_dsc();
    }
    lv_obj_t *plate = lv_image_create(s_scr);
    s_plate_obj = plate;
    lv_obj_set_user_data(plate, (void *)(uintptr_t)1u);   /* depth 0: live_restack's floor */
    lv_obj_set_size(plate, CLK_SCR_W, CLK_SCR_H);
    lv_obj_set_pos(plate, 0, 0);
    lv_obj_set_scrollable(plate, false);
    lv_image_set_src(plate, plate_src);
    orb_bg_set_plate(plate);
    orb_bg_init_frames();
    /* Fresh widgets: forget every pose last pushed, so the first tick poses each new
     * widget and marks its box — a rebuilt view must never compare its first pose
     * against a dead theme's last one and skip the setter as "unchanged", and
     * ROW_HIDDEN is the honest "nothing drawn yet". */
    if (!s_row)
        s_row = heap_caps_calloc(CLK_ROW_MAX, sizeof *s_row,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_row)
        for (int i = 0; i < CLK_ROW_MAX; i++) {
            if (s_row[i].tplane) heap_caps_free(s_row[i].tplane);
            memset(&s_row[i], 0, sizeof s_row[i]);
            s_row[i].ang_10 = ROW_HIDDEN;
            s_row[i].opa = LV_OPA_COVER;
            if (row_picks((uint16_t)i)) pick_sprite((uint16_t)i, 0, &s_row[i].fspr);
        }
    else
        ESP_LOGE(TAG, "row state: PSRAM alloc failed — the dial draws its plate only");
    text_scan();                     /* theme-text-plan §2.3: the fonts, the labels' columns */

    /* The stack, by one key (D42): every moving row and every complication window
     * in depth order, a twin under the rows at its depth, table order at a tie —
     * depth_key, the sort the layer table runs too, so the classic path's creation
     * order (its only notion of z) and the L4 job list agree. The plate is below it
     * all and the glass above. A row's widget is a turned layer (make_layer: the
     * pivot/centre contract, the blend mapping, the sampling row_aa picks); the
     * carriage's starts on its first frame. */
    complications_reset();
    const orb_theme_geo_t *geo = orb_theme_geo();
    const orb_bundle_sec_t *zsec = NULL;
    uint32_t zoff = 0;
    if (orb_theme_live_sec(orb_theme_live_slug(), &zsec, &zoff)) {
        uint16_t zkey[CLK_LAYER_KEYS_MAX], zcls[CLK_LAYER_KEYS_MAX], zslot[CLK_LAYER_KEYS_MAX];
        int zn = 0;
        for (uint32_t i = 0; i < zsec->n_elem && zn < CLK_LAYER_KEYS_MAX; i++) {
            orb_bundle_elem_t e;
            orb_bundle_elem_at(zsec, i, &e);
            if (e.cls != ORB_ASSET_AUX && e.cls != ORB_ASSET_BANK) continue;
            const uint16_t k = depth_key(e.order, e.cls == ORB_ASSET_AUX &&
                                                  (e.flags & CLK_EF_AUX_TWIN));
            int at = zn++;
            while (at > 0 && zkey[at - 1] > k) {          /* stable insert */
                zkey[at] = zkey[at - 1]; zcls[at] = zcls[at - 1]; zslot[at] = zslot[at - 1];
                at--;
            }
            zkey[at] = k; zcls[at] = e.cls; zslot[at] = e.slot;
        }
        uint32_t tagged = lv_obj_get_child_count(s_scr);
        for (int z = 0; z < zn; z++) {
            if (z) live_tag_from(&tagged, zkey[z - 1]);
            if (zcls[z] == ORB_ASSET_BANK) {
                for (uint16_t c = 0; c < geo->n_comps && c < CLK_COMP_MAX; c++)
                    if (geo->comps[c].tag == zslot[z]) { complication_create(geo, c); break; }
                continue;
            }
            for (uint16_t i = 0; s_row && i < geo->n_rows && i < CLK_ROW_MAX; i++) {
                if (geo->rows[i].tag != zslot[z]) continue;
                const clk_row_t *r = &geo->rows[i];
                const bool car = i == clk_theme.car_row && clk_theme.tourbillon_n;
                const clk_sprite_t *sp = car ? &clk_theme.tourbillon[0] : &r->spr;
                if (car ? !sp->show : !r->spr.show) break;
                /* A twin shares its row's sampling, so a filtered hand never stands on
                 * an unfiltered shadow; the carriage's frames never take the filter. */
                const clk_row_t *ar = ((r->flags & CLK_FLAG_AUX_SHADOW) && i > 0)
                                    ? &geo->rows[i - 1] : r;
                if (r->shape == CLK_SHAPE_TEXT) {
                    /* theme-text-plan §2.3: a label draws its own plane, the panel
                     * until the app gives it a string. */
                    const size_t pb = (size_t)r->spr.w * r->spr.h * 3u;
                    s_row[i].tplane = heap_caps_malloc(LABEL_PLANE_OFF + pb,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (!s_row[i].tplane) {
                        ESP_LOGE(TAG, "label at slot %u: no PSRAM for its plane", (unsigned)r->tag);
                        break;
                    }
                    memcpy(s_row[i].tplane + LABEL_PLANE_OFF, blob() + r->spr.color_off, pb);
                    s_row[i].tseq = 0;
                    clk_sprite_t ts = r->spr;
                    ts.color_off = LABEL_PLANE_OFF;
                    s_row[i].obj = make_layer_at(&ts, &s_row[i].dsc, false, s_row[i].tplane);
                } else
                s_row[i].obj = make_layer(sp, &s_row[i].dsc,
                                          !car && row_aa(&clk_theme, ar, NULL));
                /* Hidden until its first pose, as its state already says (ROW_HIDDEN):
                 * a row the app hides before it ever shows would otherwise stand at P0
                 * on the LVGL path (Bronze, 2026-11-04: a plane at the scope's centre
                 * with no aircraft in range). L4 never drew it; its descriptor starts
                 * hidden. */
                if (s_row[i].obj) lv_obj_set_hidden(s_row[i].obj, true);
                break;
            }
        }
        if (zn) live_tag_from(&tagged, zkey[zn - 1]);
    }

    /* Glass / hub cap last, on top of everything. */
    if (clk_theme.overlay_disc) {
        const uint8_t *sq = overlay_square();
        clk_sprite_t ov = clk_theme.overlay;
        ov.color_off = OVERLAY_SQ_OFF;       /* the square, inside its buffer */
        s_overlay = sq ? make_layer_at(&ov, &s_overlay_dsc, false, sq) : NULL;
    } else {
        s_overlay = make_layer(&clk_theme.overlay, &s_overlay_dsc, false);
    }
    if (s_overlay) lv_obj_set_user_data(s_overlay, (void *)(uintptr_t)0x10000u);

    /* The layer table, from the same bundle the widgets above were projected
     * from (Migration 2, Commits 3 and 4). Rows enter hidden and the tick's
     * pose pass wakes them — see clock_layers_refresh. A failure here is a
     * smaller ledger row on the owner's app page and a log line — never a
     * dial that fails to boot: the table only feeds L4, and L4 takes the glass
     * only on a claim with no refusal (orb_sched_l4_active), so a dial whose
     * table fell short still draws through the widgets above. That clause holds
     * because of the gate, not by default: without it a still dial would mark
     * rows nobody welds. */
    s_lay_app = self;
#if CONFIG_ORB_DEBUG_HTTP
    orb_sched_plan_app(self->slug && strcmp(self->slug, "clock") != 0);   /* §6.27 */
#endif
    clock_layers_build(self);

    /* The carriage's frame bank: descriptors for its widget's swap. Reset the frame
     * state so the first tick (re)draws it whole. */
    for (int i = 0; i < (int)clk_theme.tourbillon_n && i < CLK_TOURB_MAX; i++)
        if (gfx_sprite_drawable(&clk_theme.tourbillon[i]))
            gfx_fill_sprite_dsc(&s_tourb_dsc[i], &clk_theme.tourbillon[i], blob());
    s_tourb_shown = -1;
    s_tourb_t0 = lv_tick_get();


    /* The wall pair is the broker's to seed now: the first orb_time_now()
     * below stamps the anchor, exactly where the deleted s_t0_us line did. */
    compose_now();
#if CLK_BG_DIRECT_PUSH && DISPLAY_BENCH
    /* The flush weld point is the scheduler's to register since Commit 4
     * (orb_sched.c's sched_flush_commit); only the bench probe init below is
     * still this file's. */
    bench_init();
#endif

    static const orb_sched_policy_t clock_policy = {
        .tick     = clock_tick_policy,
#if DISPLAY_BENCH
        .bench    = bench_plan,
#endif
    };
    uint32_t start_period;
    if (orb_bg_gif_mode())                               start_period = orb_bg_gif_tick_ms();
    else if (orb_bg_bg_live() && clk_theme.bg_loop)      start_period = orb_sched_bg_rate(orb_sched_tick_base_ms(), orb_bg_every_p());
    else                                          { *orb_bg_every_p() = 1;
                                                     start_period = orb_sched_tick_base_ms(); }
    orb_sched_start(start_period, &clock_policy);   /* parked until the dial is on the glass */

    /* Report the cadence the bake actually recorded instead of a literal that
     * drifts out of date: the GIF and the tick share clk_gif_delays[] (the
     * GIF's own per-frame delays), and the sweeping hand beats at
     * tickRate when the theme declares one. */
    /* 96, not the 64 this shipped with: gif_frames, tick_rate and the delays are
     * runtime values now that a theme can arrive on an SD card, so the compiler can
     * no longer prove the worst case fits and -Werror=format-truncation says so. The
     * strings are still short — this is headroom for the widest digits, not a guess. */
    char bgdesc[96];
    if (orb_bg_gif_mode()) {
        uint32_t d0 = clk_gif_delays[0] > 0 ? (uint32_t)clk_gif_delays[0]
                                            : (uint32_t)CLK_GIF_TICK_MS;
        snprintf(bgdesc, sizeof bgdesc,
                 "gif delta bank: %u frames @ %u ms (%u.%u fps), tick %u ms",
                 (unsigned)clk_theme.gif_frames, (unsigned)d0,
                 (unsigned)(1000 / d0), (unsigned)((1000 % d0) * 10 / d0),
                 (unsigned)orb_bg_gif_tick_ms());
    } else if (orb_bg_bg_live()) {
        snprintf(bgdesc, sizeof bgdesc, "plate anim");
    } else {
        snprintf(bgdesc, sizeof bgdesc, "still");
    }

    char sweepdesc[48] = "off";   /* 32 -> 48 for the same reason as bgdesc above */
    const int sw_bps = clk_theme.tick_rate > 0 ? (int)clk_theme.tick_rate : 1;
    if (clk_theme.second_sweep) {
        const uint32_t tpb = orb_bg_gif_mode() ? hand_beat_ticks() : 1;
        if (orb_bg_gif_mode() && tpb > 1)
            snprintf(sweepdesc, sizeof sweepdesc, "on(%u bps, every %u ticks)",
                     (unsigned)sw_bps, (unsigned)tpb);
        else if (orb_bg_gif_mode())
            snprintf(sweepdesc, sizeof sweepdesc, "on(%u bps, sweep, every tick)",
                     (unsigned)sw_bps);
        /* A plate or still dial beats on the unified ladder, so the theme's beat is
         * an ASK and the running `clock %u fps` line is the answer. Print both the
         * ask and the base the ladder starts from: "on" alone hid the Station's
         * tick_rate 1 behind a sweep that looked broken on the glass and healthy
         * in the pane, and a reader who sees `1 bps, base 1000 ms` next to a dial
         * running at 4 fps knows at once which of the two clamped. */
        else
            snprintf(sweepdesc, sizeof sweepdesc, "on(%u bps, base %u ms)",
                     (unsigned)sw_bps, (unsigned)orb_sched_tick_base_ms());
    } else if (sw_bps > 1) {
        /* A ticking dial names its own beat now, on every art class. */
        snprintf(sweepdesc, sizeof sweepdesc, "off(%u bps, base %u ms)",
                 (unsigned)sw_bps, (unsigned)orb_sched_tick_base_ms());
    }

    int n_moving = 0, n_still = 0, n_twin = 0;
    for (uint16_t i = 0; i < geo->n_rows && i < CLK_ROW_MAX; i++) {
        const clk_row_t *r = &geo->rows[i];
        if (r->flags & CLK_FLAG_AUX_SHADOW) n_twin++;
        else if (r->src == CLK_SRC_NONE) n_still++;
        else n_moving++;
    }
    ESP_LOGI(TAG, "analog clock: %d moving row%s, %d still, %d shadow%s, sweep %s, bg %s",
             n_moving, n_moving == 1 ? "" : "s", n_still, n_twin, n_twin == 1 ? "" : "s",
             sweepdesc, bgdesc);
    /* The sampling each turned layer was BUILT with, asked of the same function
     * make_layer and the layer table took it from, with the rule that answered:
     * `flag` is the bake's EF_SMOOTH on a painter-owned dial, `classic-rule` the
     * motion words on a dial LVGL draws. Never derived from theme fields alone —
     * that is how the old line reported the shipped rule under an ablation arm. */
    {
        char aab[240];
        int o = 0;
        for (uint16_t i = 0; i < geo->n_rows && i < CLK_ROW_MAX && o < (int)sizeof aab - 40; i++) {
            const clk_row_t *r = &geo->rows[i];
            if ((r->flags & CLK_FLAG_AUX_SHADOW) || r->src == CLK_SRC_NONE) continue;
            const char *why = "";
            const bool sm = row_aa(&clk_theme, r, &why);
            o += snprintf(aab + o, sizeof aab - (size_t)o, "%sptr%u %s=%s", o ? ", " : "",
                          (unsigned)r->tag, sm ? "smooth" : "nearest", why);
        }
        ESP_LOGI(TAG, "row AA: %s", o ? aab : "no turned rows");
    }
    if (clk_theme.tourbillon_n && clk_theme.car_row < geo->n_rows)
        ESP_LOGI(TAG, "carriage: ptr%u, %u frames @ %u fps, orbit about (%d,%d)",
                 (unsigned)geo->rows[clk_theme.car_row].tag,
                 (unsigned)clk_theme.tourbillon_n, (unsigned)clk_theme.tourbillon_fps,
                 (int)geo->rows[clk_theme.car_row].spr.centerx,
                 (int)geo->rows[clk_theme.car_row].spr.centery);
    return ESP_OK;
}

lv_obj_t *stage_screen(void)
{
    return s_scr;
}

/* When the dial last came onto the glass: the press that picked it from the
 * carousel also arms whatever lands (input_router's "press once, you are inside"),
 * and that press must not be read as a request for the repeater. */
void stage_run(bool on)
{
    /* Repurposed from the old digital label's bounce: this is the router's
     * "clock is the visible screen" gate. Park the tick timer when the clock is
     * not on the glass so a hidden dial burns no flush work; on re-show, repaint
     * the whole face cleanly and resume.
     *
     * C3 (§5.3) adds the one case a re-show must refuse: a store parked the dial
     * and unmapped its pixels, and the reboot that re-establishes the map has
     * not run. Waking the pose pass here would re-pose the layer table from
     * cached image descriptors into a mapping that no longer exists — a load
     * fault, not a repaint. The dial stays exactly as the glass holds it. */
    if (on && (s_base || !orb_theme_resident())) {
        /* theme-cache-plan §2.4: a stage bound to an app's bare face (its theme a fill
         * away) has no frames to run, and the dial's tables are empty under it: the
         * tick read the NULL geometry (Bronze, 2026-11-08, LoadProhibited at 0x34) */
        orb_sched_set_running(false);
        return;
    }
    if (on && orb_theme_store_torn()) {
        ESP_LOGW(TAG, "bounce refused: the store took the bank down, reboot pending");
        orb_sched_set_running(false);
        return;
    }
    if (!on && s_opened && !s_base) {
        const bool any = s_live[0] || s_live[1] || s_overlay_sq;
        live_drop_all();
        orb_glass_free(s_overlay_sq);
        s_overlay_sq = NULL;
        if (any) ESP_LOGI(TAG, "off the glass: the live layers and the glass's square freed");
    }
    orb_sched_set_running(on);
    if (!orb_sched_started()) return;
    if (on) {
        if (orb_bg_gif_mode()) {
            /* The canvas still mirrors exactly what the glass held when we
             * parked — repaint it whole and keep cycling from the current
             * frame; resetting the counter would desync canvas vs deltas. */
            orb_sched_reanchor_gif();
            s_bt_ctr = 0;                   /* first tick back is a beat */
        } else {
            orb_bg_reset_play();
        }
        /* Every dial re-seeds its rung on the way onto the glass, not only a GIF one:
         * the rung is the scheduler's, not the theme's, and a dial that came back to
         * the last one's demoted rung stayed there, because the promote gate measures
         * against a tick the rung itself paces (owner, 2026-11-04: flights held at
         * 5 fps after a trip to the clock). */
        orb_sched_reset_fps();
        /* Tourbillon: force a whole first draw and restart the frame clock,
         * same discipline as the angle reset — nothing was composited while
         * parked, so whatever s_tourb_shown claims is stale. */
        s_tourb_shown = -1;
        s_tourb_t0 = lv_tick_get();
        /* The broker wipes this dial's rows when it goes behind the glass
         * (orb_svc_app_suspended), so the layer table is re-claimed on the
         * way back: a dial that returns to a table that no longer exists is
         * a dial that silently stops feeding the ledger. Only an emptied
         * table rebuilds — an activation that finds its rows still standing
         * (the boot ladder's first show) leaves them and their handles alone.
         * (Migration 2, commit 3.) */
        /* The glass's square went with the app's last leave (live_drop_all's note): expand it
         * again, point its widget at it, and have the table take it too. */
        bool reglass = false;
        if (!s_base && clk_theme.overlay_disc && s_overlay && !s_overlay_sq) {
            const uint8_t *sq = overlay_square();
            if (sq) {
                clk_sprite_t ov = clk_theme.overlay;
                ov.color_off = OVERLAY_SQ_OFF;
                gfx_fill_sprite_dsc(&s_overlay_dsc, &ov, sq);
                lv_image_set_src(s_overlay, &s_overlay_dsc);
                reglass = true;
            }
        }
        if (s_lay_app && (orb_svc_layers_live(s_lay_app) == 0 || reglass))
            clock_layers_build(s_lay_app);
        /* An app with widgets of its own over its theme (a label, a ring) renders
         * through LVGL: L4 writes the theme into the panel mirror over whatever LVGL
         * drew in the boxes it redraws, and LVGL is what stacks the app's widgets on
         * top. The app built them in its init, after the stage, so they are counted
         * here, on the way onto the glass. */
        /* visible ones only: an app hides its themeless face's labels when a theme lands
         * (the clock, 2026-11-09), and a hidden label stacks nothing */
        bool own = false;
        for (uint32_t i = s_stage_objs; s_scr && i < lv_obj_get_child_count(s_scr) && !own; i++)
            own = !lv_obj_has_flag(lv_obj_get_child(s_scr, (int32_t)i), LV_OBJ_FLAG_HIDDEN);
        orb_sched_set_l4_veto(own);
        static const orb_app_t *s_vetoed_logged;
        if (own && s_vetoed_logged != s_lay_app) {
            s_vetoed_logged = s_lay_app;
            ESP_LOGI(TAG, "render path: %s has widgets over its theme — LVGL stacks them (l4 off)",
                     s_lay_app && s_lay_app->slug ? s_lay_app->slug : "?");
        }
        lv_obj_invalidate(s_scr);
        if (!orb_bg_gif_mode()) orb_bg_show_plate_frame(orb_bg_frame_now());
        compose_now();
        /* Pose the rebuilt table BEFORE the resume: a row woken on this
         * activation's first tick would leave one flush — this one, from the
         * bounce invalidate — welding a dial with missing hands. The wake
         * marks land in the same window the bounce dirty is flushing. */
        clock_layers_refresh();
        orb_sched_resume();
    } else {
        orb_sched_pause();
    }
}

bool stage_running(void)
{
    return orb_sched_running();
}




/* ---- §43: the stage's surface, bound as the dial_* imports (orb_app_wasm.c) ------- */

/* ---- theme-pool-plan P4: the stage is the app's on the glass ---------------------------
 *
 * Each theme-aware app wears its own theme from the pool, and one stage draws whichever
 * app is building or on the glass. Binding to another app releases everything the stage
 * built for the last one — its layer-table rows, its widgets on that app's page, the
 * plate and the delta canvas, its voices — then binds the new app's theme (its tables
 * projected, its banks mapped in place of the old) and builds on the new app's page. The
 * release comes first because every widget and row it deletes reads the map the bind
 * replaces. */
static void stage_release(void)
{
    if (!s_opened) return;
    stage_run(false);                              /* frames and voices parked */
    /* theme-cache-plan §2.6: its loops stop with the glass; its one-shots play on, from its
     * voice bank, which no bind moves */
    if (s_lay_app) orb_sound_stop_leased(s_lay_app);
    if (!s_base) {
        const orb_svc_t *svc = orb_svc_table();
        if (s_lay_app && orb_svc_layers_live(s_lay_app) > 0)
            for (int i = 0; i < s_lay_n; i++) svc->layer_del(s_lay_app, s_lay_handle[i]);
        s_lay_n = 0;
        s_lay_sec = NULL;
        s_lay_bank_off = 0;
        orb_sched_set_layer_claim(0, 0);
        for (int i = 0; s_row && i < CLK_ROW_MAX; i++) {
            if (s_row[i].obj) { lv_obj_delete(s_row[i].obj); s_row[i].obj = NULL; }
            if (s_row[i].tplane) { heap_caps_free(s_row[i].tplane); s_row[i].tplane = NULL; }
        }
        s_text_sec = NULL;
        for (int i = 0; i < CLK_COMP_MAX; i++)
            for (int p = 0; p < CLK_COMP_SLOTS; p++)
                if (s_comp_obj[i][p]) { lv_obj_delete(s_comp_obj[i][p]); s_comp_obj[i][p] = NULL; }
        complications_reset();
        if (s_overlay) { lv_obj_delete(s_overlay); s_overlay = NULL; }
        if (s_plate_obj) { lv_obj_delete(s_plate_obj); s_plate_obj = NULL; }
        orb_bg_release();
    }
    back_free_all();
    for (int l = 0; l < STAGE_LIVE_N; l++) {
        if (s_live_obj[l]) { lv_obj_delete(s_live_obj[l]); s_live_obj[l] = NULL; }
        orb_glass_free(s_live[l]);
        s_live[l] = NULL;
        s_live_order[l] = -1;
    }
    s_live_merged = false;
    ESP_LOGI(TAG, "stage released %s", s_lay_app && s_lay_app->slug ? s_lay_app->slug : "?");
    s_opened = false;
    s_base = false;
    s_art = NULL;
    s_lay_app = NULL;
#if CONFIG_ORB_DEBUG_HTTP
    orb_sched_plan_app(false);
#endif
}

bool stage_bound(const orb_app_t *app)
{
    return s_opened && app && s_lay_app == app;
}

esp_err_t stage_bind(orb_app_t *app)
{
    if (stage_bound(app)) return ESP_OK;
    stage_release();
    /* A theme that will not bind leaves the tables empty, and stage_init finds no
     * resident art and draws nothing: the app's base face, the D31 fallback. */
    (void)orb_theme_bind(app->slug);
    /* The app's own widgets (a label, a card) may already be on its page: a rebind
     * builds after them. The stage's objects go beneath them, in their own order —
     * the plate lowest — so a rebuilt dial never covers what the app drew. */
    lv_obj_t *page = orb_ui_screen(app->flash_slot);
    const uint32_t before = page ? lv_obj_get_child_count(page) : 0;
    const esp_err_t e = stage_init(app);
    if (e != ESP_OK) return e;
    s_stage_objs = page ? lv_obj_get_child_count(page) - before : 0;
    if (page && before) {
        for (uint32_t i = 0; i < s_stage_objs; i++)
            lv_obj_move_to_index(lv_obj_get_child(page, (int32_t)(before + i)), (int32_t)i);
    }
    s_opened = true;
    ESP_LOGI(TAG, "stage bound to %s%s", app->slug, s_base ? " (base face: no theme)" : "");
    return ESP_OK;
}

/* theme-cache-plan §2.4: bind again, a bare-face binding let go first (a fill just landed). */
esp_err_t stage_rebind(orb_app_t *app)
{
    if (s_opened && s_lay_app == app) stage_release();
    return stage_bind(app);
}

esp_err_t stage_open(orb_app_t *app, bool *base)
{
    const esp_err_t e = stage_bind(app);
    if (base) *base = s_base;
    return e;
}

void stage_info(uint16_t *n_rows, uint16_t *n_comps, uint8_t *car_row, uint8_t *tick_rate,
               uint8_t *second_sweep, uint8_t *bg_mode, uint16_t *tourbillon_n)
{
    /* No resident theme: there is no geometry to read (orb_theme_geo() answers NULL
     * with nothing adopted — orb-A, 2026-11-01, a LoadProhibited here on the first
     * boot after the partition move emptied the slot). */
    if (s_base) {
        *n_rows = *n_comps = 0; *car_row = 0xFF; *tick_rate = *second_sweep = *bg_mode = 0;
        *tourbillon_n = 0;
        return;
    }
    const orb_theme_geo_t *g = orb_theme_geo();
    *n_rows = s_base ? 0 : (g->n_rows < CLK_ROW_MAX ? g->n_rows : CLK_ROW_MAX);
    *n_comps = s_base ? 0 : (g->n_comps < CLK_COMP_MAX ? g->n_comps : CLK_COMP_MAX);
    *car_row = clk_theme.car_row;
    *tick_rate = clk_theme.tick_rate;
    *second_sweep = clk_theme.second_sweep;
    *bg_mode = clk_theme.bg_mode;
    *tourbillon_n = clk_theme.tourbillon_n;
}

const clk_row_t *stage_row(int i)
{
    if (s_base) return NULL;
    const orb_theme_geo_t *g = orb_theme_geo();
    return (i >= 0 && i < g->n_rows && i < CLK_ROW_MAX) ? &g->rows[i] : NULL;
}

const clk_comp_t *stage_comp(int i)
{
    if (s_base) return NULL;
    const orb_theme_geo_t *g = orb_theme_geo();
    return (i >= 0 && i < g->n_comps && i < CLK_COMP_MAX) ? &g->comps[i] : NULL;
}

const clk_sound_t *stage_sounds(uint32_t *n, uint8_t *pattern)
{
    if (s_base) { *n = 0; *pattern = CLK_PAT_NONE; return NULL; }
    return orb_theme_sounds(n, pattern);
}

uint32_t stage_values(void *buf, uint32_t cap, uint16_t *ver)
{
    *ver = 0;
    if (s_base) return 0;
    const orb_theme_geo_t *g = orb_theme_geo();
    const uint8_t *art = art_bank_ptr();
    if (!g || !g->values_len || !art || g->values_off + g->values_len > g->art_size) return 0;
    *ver = g->values_ver;
    memcpy(buf, art + g->blob_off + g->values_off, cap < g->values_len ? cap : g->values_len);
    return g->values_len;
}

/* ---- radar-plan §2.9 (R3c): the live layer --------------------------------------- */

/* A fill, inside the glass's disc alone once the layer is new (radar-plan §6.19: the
 * corners are never seen; a new plane is cleared whole, so they read clear, not noise). */
/* A layer's bytes: colour and alpha, or (merged, §6.21) colour alone: neither merged layer
 * is ever blended, so neither keeps an alpha plane (217 KB each). */
static size_t live_bytes(int l)
{
    const size_t n = (size_t)CLK_SCR_W * CLK_SCR_H;
    const int f = live_fmt(l);
    return f == LF_I8 ? LIVE_I8_IDX + n : LIVE_OFF + n * (f == LF_RGB ? 2u : 3u);
}

/* §7.9: an indexed buffer's disc filled with the value whose colour is argb (a clear: value 0,
 * transparent); `whole` resets its palette and the values in use first. */
static void live_fill_i8(uint8_t *buf, uint32_t argb, bool whole)
{
    orb_ui_pal_t p;
    live_pal(buf, &p);
    if (whole || !(argb >> 24)) {
        memset(p.used, 0, 32);
        for (int i = 0; i < 256; i++) orb_ui_pal_set(&p, i, 0);
    }
    const int v = orb_ui_pal_index(&p, (argb >> 24) ? argb : 0);
    const uint16_t *x0 = orb_ui_disc_left();
    for (int y = 0; y < CLK_SCR_H; y++) {
        const int a = whole ? 0 : x0[y];
        memset(buf + LIVE_I8_IDX + (size_t)y * CLK_SCR_W + a, v, (size_t)(CLK_SCR_W - 2 * a));
    }
}

static void live_fill(int l, uint32_t argb, bool whole)
{
    if (live_fmt(l) == LF_I8) { live_fill_i8(s_live[l], argb, whole); return; }
    const bool opaque = live_fmt(l) == LF_RGB;
    uint16_t *px = (uint16_t *)(s_live[l] + LIVE_OFF);
    uint8_t *al = opaque ? NULL : s_live[l] + LIVE_OFF + (size_t)CLK_SCR_W * CLK_SCR_H * 2u;
    const uint32_t rgb = argb & 0xFFFFFFu;
    const uint16_t c = (uint16_t)(((rgb >> 8) & 0xF800u) | ((rgb >> 5) & 0x07E0u) | ((rgb >> 3) & 0x001Fu));
    const uint16_t *x0 = orb_ui_disc_left();
    for (int y = 0; y < CLK_SCR_H; y++) {
        const int a = whole ? 0 : x0[y], n = CLK_SCR_W - 2 * a;
        const size_t i = (size_t)y * CLK_SCR_W + (size_t)a;
        if (al) memset(al + i, (int)(argb >> 24), (size_t)n);
        if (!(argb >> 24) && !opaque && !whole) continue;   /* clear: no colour shows, and a draw
                                                          * over a clear pixel takes its own
                                                          * whole (`over`) */
        for (int k = 0; k < n; k++) px[i + (size_t)k] = c;
    }
}

/* The classic path's z, for both layers at once. Each layer's place among the widgets the
 * stage built is the count of those at or above its depth below it (live_tag_from): the
 * glass and the app's own widgets stay above. The two are lifted out first, so neither
 * reads the other's old place; the lower goes in first, and at one place the one deeper,
 * or at one depth layer 1, goes over the other. */
static void live_restack(void)
{
    int32_t slot[STAGE_LIVE_N];
    uint32_t key[STAGE_LIVE_N];
    for (int l = 0; l < STAGE_LIVE_N; l++)
        if (s_live_obj[l]) lv_obj_move_foreground(s_live_obj[l]);
    for (int l = 0; l < STAGE_LIVE_N; l++) {
        if (!s_live_obj[l]) continue;
        key[l] = depth_key((uint8_t)s_live_order[l], false);
        slot[l] = 0;
        const uint32_t n = lv_obj_get_child_count(s_scr);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(s_scr, (int32_t)i);
            if (c == s_live_obj[0] || c == s_live_obj[1]) break;   /* lifted to the end */
            const uintptr_t tag = (uintptr_t)lv_obj_get_user_data(c);
            if (!tag || tag - 1u > key[l]) break;
            slot[l]++;
        }
    }
    const bool both = s_live_obj[0] && s_live_obj[1];
    const int lo = both && (slot[1] < slot[0] || (slot[1] == slot[0] && key[1] < key[0])) ? 1 : 0;
    const int hi = 1 - lo;
    if (s_live_obj[lo]) lv_obj_move_to_index(s_live_obj[lo], slot[lo]);
    if (s_live_obj[hi]) lv_obj_move_to_index(s_live_obj[hi], slot[hi] + (both ? 1 : 0));
}

/* Off the glass an app keeps no display buffers (owner, 2026-11-07): the live layers go
 * (the app opens them again when it is back), and so does the glass overlay's square,
 * expanded again from the theme on the way back. */
static void live_drop_all(void)
{
    back_free_all();
    for (int l = 0; l < STAGE_LIVE_N; l++) {
        if (s_live_obj[l]) { lv_obj_delete(s_live_obj[l]); s_live_obj[l] = NULL; if (s_stage_objs) s_stage_objs--; }
        orb_glass_free(s_live[l]);
        s_live[l] = NULL;
        s_live_order[l] = -1;
        s_live_fmt[l] = LF_RGBA;
    }
    s_live_merged = false;
}

/* The classic path's image of layer l, as its format reads (§6.21, §7.9). */
static void live_dsc_set(int l)
{
    const int f = live_fmt(l);
    const size_t n = (size_t)CLK_SCR_W * CLK_SCR_H;
    s_live_dsc[l].header.cf = f == LF_I8 ? LV_COLOR_FORMAT_I8 : f == LF_RGB ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
    s_live_dsc[l].header.stride = (uint32_t)CLK_SCR_W * (f == LF_I8 ? 1u : 2u);
    s_live_dsc[l].data = s_live[l] + (f == LF_I8 ? LIVE_I8_ARGB : LIVE_OFF);
    s_live_dsc[l].data_size = (uint32_t)(f == LF_I8 ? 1024u + n : live_bytes(l) - LIVE_OFF);
    lv_image_set_src(s_live_obj[l], &s_live_dsc[l]);
}

/* §7.9: layer l in format fmt (LF_*), its picture cleared; a merge refuses it. */
esp_err_t stage_live_format(int32_t layer, int32_t fmt)
{
    if (layer < 0 || layer >= STAGE_LIVE_N || fmt < LF_RGBA || fmt > LF_I8) return ESP_ERR_INVALID_ARG;
    const int l = (int)layer;
    if (!s_live[l] || s_live_merged) return ESP_ERR_INVALID_STATE;
    if (s_live_fmt[l] == fmt) return ESP_OK;
    back_lock();
    orb_glass_free(s_live_back[l]); s_live_back[l] = NULL;
    orb_glass_free(s_live[l]);
    s_live_fmt[l] = (uint8_t)fmt;
    s_live[l] = orb_glass_alloc(live_bytes(l));
    back_unlock();
    if (!s_live[l]) {
        s_live_fmt[l] = LF_RGBA;
        if (s_live_obj[l]) { lv_obj_delete(s_live_obj[l]); s_live_obj[l] = NULL; if (s_stage_objs) s_stage_objs--; }
        s_live_order[l] = -1;
        return ESP_ERR_NO_MEM;
    }
    live_fill(l, 0, true);
    if (s_live_obj[l]) live_dsc_set(l);
    if (s_lay_app && (orb_svc_layers_live(s_lay_app) > 0 || stage_running())) {
        clock_layers_build(s_lay_app);                    /* the row's art and solid change */
        clock_layers_refresh();
    }
    stage_live_damage(0, 0, CLK_SCR_W, CLK_SCR_H);
    return ESP_OK;
}

esp_err_t stage_live_open(int32_t layer, int32_t order, uint32_t argb)
{
    if (!s_opened || s_base || !s_scr) return ESP_ERR_INVALID_STATE;
    if (layer < 0 || layer >= STAGE_LIVE_N || order < -1 || order >= 64) return ESP_ERR_INVALID_ARG;
    const int l = (int)layer;
    const bool rebuild = order != s_live_order[l];
    if (order < 0) {
        if (s_live_obj[l]) { lv_obj_delete(s_live_obj[l]); s_live_obj[l] = NULL; s_stage_objs--; }
        back_lock();
        orb_glass_free(s_live_back[l]); s_live_back[l] = NULL;
        orb_glass_free(s_live[l]);
        s_live[l] = NULL;
        back_unlock();
    } else if (!s_live[l]) {
        s_live[l] = orb_glass_alloc(live_bytes(l));
        if (!s_live[l]) return ESP_ERR_NO_MEM;
        live_fill(l, 0, true);
    }
    s_live_order[l] = order;
    if (s_live[l]) live_fill(l, argb, false);
    if (s_live[l] && !s_live_obj[l]) {
        clk_sprite_t sp;
        memset(&sp, 0, sizeof sp);
        sp.w = CLK_SCR_W; sp.h = CLK_SCR_H;
        sp.pivotx = sp.centerx = CLK_SCR_W / 2;
        sp.pivoty = sp.centery = CLK_SCR_H / 2;
        sp.show = 1;
        sp.color_off = LIVE_OFF;
        s_live_obj[l] = make_layer_at(&sp, &s_live_dsc[l], false, s_live[l]);
        if (s_live_obj[l]) s_stage_objs++;  /* the stage's, not the app's: L4 keeps the glass */
        if (s_live_obj[l]) live_dsc_set(l);
        if (s_live_obj[l] && s_live_merged && l == 0) lv_obj_set_hidden(s_live_obj[l], true);
    }
    if (rebuild) {
        live_restack();
        /* L4's z is table order: a moved layer is every row re-claimed. Behind the glass
         * the table is wiped and the activation re-claims it, the layers with it. */
        if (s_lay_app && (orb_svc_layers_live(s_lay_app) > 0 || stage_running())) {
            clock_layers_build(s_lay_app);
            clock_layers_refresh();
        }
    }
    stage_live_damage(0, 0, CLK_SCR_W, CLK_SCR_H);
    return ESP_OK;
}

/* A draw target over buf in layer l's format; an indexed one's palette goes in *pal. */
static void live_tgt_of(int l, uint8_t *buf, orb_ui_tgt_t *t, orb_ui_pal_t *pal)
{
    memset(t, 0, sizeof *t);
    t->w = t->h = t->stride = CLK_SCR_W;
    if (live_fmt(l) == LF_I8) {
        live_pal(buf, pal);
        t->ix = buf + LIVE_I8_IDX;
        t->pal = pal;
        return;
    }
    t->px = (uint16_t *)(void *)(buf + LIVE_OFF);
    /* merged or opaque layers are colour alone (§6.21): a draw blends into the colour */
    t->a = live_fmt(l) == LF_RGB ? NULL : buf + LIVE_OFF + (size_t)CLK_SCR_W * CLK_SCR_H * 2u;
}

bool stage_live_tgt(int layer, orb_ui_tgt_t *t, orb_ui_pal_t *pal)
{
    if (layer < 0 || layer >= STAGE_LIVE_N || !s_live[layer]) return false;
    live_tgt_of(layer, s_live[layer], t, pal);
    return true;
}

esp_err_t stage_live_merge(bool on)
{
    if (on && (!s_live[0] || !s_live[1])) return ESP_ERR_INVALID_STATE;
    if (on && (s_live_fmt[0] != LF_RGBA || s_live_fmt[1] != LF_RGBA)) return ESP_ERR_INVALID_STATE;   /* §7.9 */
    if (on == s_live_merged) return ESP_OK;
    back_free_all();                                      /* the backs are the old size */
    s_live_merged = on;
    /* Both layers again at their new size (colour alone merged, §6.21): the old go first, so
     * the new fit where they were; the app draws both afresh after asking. */
    for (int l = 0; l < STAGE_LIVE_N; l++) {
        if (!s_live[l]) continue;
        orb_glass_free(s_live[l]);
        s_live[l] = orb_glass_alloc(live_bytes(l));
        if (!s_live[l]) {                                 /* nothing to merge into: as it was */
            for (int k = 0; k < STAGE_LIVE_N; k++) { orb_glass_free(s_live[k]); s_live[k] = NULL; }
            s_live_merged = false;
            ESP_LOGE(TAG, "live layers: no room to merge them; the app reopens them");
            if (s_lay_app && (orb_svc_layers_live(s_lay_app) > 0 || stage_running())) {
                clock_layers_build(s_lay_app);
                clock_layers_refresh();
            }
            return ESP_ERR_NO_MEM;
        }
        live_fill(l, 0, true);
        if (s_live_obj[l]) live_dsc_set(l);               /* the classic path's image, repointed */
    }
    if (s_live_obj[0]) lv_obj_set_hidden(s_live_obj[0], on);
    if (s_lay_app && (orb_svc_layers_live(s_lay_app) > 0 || stage_running())) {
        clock_layers_build(s_lay_app);                    /* the rows' show and solid change */
        clock_layers_refresh();
    }
    stage_live_damage(0, 0, CLK_SCR_W, CLK_SCR_H);
    return ESP_OK;
}

esp_err_t stage_live_copy(int dst, int src)
{
    if (dst < 0 || dst >= STAGE_LIVE_N || src < 0 || src >= STAGE_LIVE_N || dst == src || !s_live[dst] || !s_live[src] ||
        live_fmt(dst) != live_fmt(src))
        return ESP_ERR_INVALID_ARG;
    if (live_fmt(dst) == LF_I8) {                         /* §7.9: the values and the palette */
        memcpy(s_live[dst] + LIVE_OFF, s_live[src] + LIVE_OFF, live_bytes(dst) - LIVE_OFF);
        stage_live_damage(0, 0, CLK_SCR_W, CLK_SCR_H);
        return ESP_OK;
    }
    const size_t n = (size_t)CLK_SCR_W * CLK_SCR_H;
    const uint16_t *x0 = orb_ui_disc_left();
    for (int y = 0; y < CLK_SCR_H; y++) {                 /* the disc alone (§6.19) */
        const size_t i = (size_t)y * CLK_SCR_W + x0[y], k = (size_t)(CLK_SCR_W - 2 * x0[y]);
        memcpy(s_live[dst] + LIVE_OFF + 2 * i, s_live[src] + LIVE_OFF + 2 * i, 2 * k);
        if (live_fmt(dst) == LF_RGBA)
            memcpy(s_live[dst] + LIVE_OFF + 2 * n + i, s_live[src] + LIVE_OFF + 2 * n + i, k);
    }
    stage_live_damage(0, 0, CLK_SCR_W, CLK_SCR_H);
    return ESP_OK;
}

/* ---- OS v1.2.38: the back copies (radar-plan §6.27) ---------------------------------- */

esp_err_t stage_live_back_ensure(int l)
{
    if (l < 0 || l >= STAGE_LIVE_N || !s_live[l]) return ESP_ERR_INVALID_STATE;
    if (s_live_back[l]) return ESP_OK;
    uint8_t *b = orb_glass_alloc(live_bytes(l));
    if (!b) return ESP_ERR_NO_MEM;
    memcpy(b, s_live[l], LIVE_OFF);                       /* the image header, as the front's */
    back_lock();
    s_live_back[l] = b;
    back_unlock();
    return ESP_OK;
}

bool stage_live_back_lock(int l, orb_ui_tgt_t *t, orb_ui_pal_t *pal, orb_ui_tgt_t *src_t,
                          orb_ui_pal_t *src_pal, int src)
{
    back_lock();
    if (l < 0 || l >= STAGE_LIVE_N || !s_live_back[l]) { back_unlock(); return false; }
    live_tgt_of(l, s_live_back[l], t, pal);
    if (src_t) {
        if (src >= 0 && src < STAGE_LIVE_N && s_live[src]) live_tgt_of(src, s_live[src], src_t, src_pal);
        else memset(src_t, 0, sizeof *src_t);
    }
    return true;                                          /* held: stage_live_back_unlock */
}
void stage_live_back_unlock(void) { back_unlock(); }

/* The owner (2026-11-08): "slower hard blips may be better than faster partial mess". A
 * swap is one blip, and the next waits until the sweep has had SWAP_PACE times the last
 * blip's cost to itself: a small change steps at the app's own pace, a storm over the
 * whole disc more slowly, and never in slices. */
#define SWAP_PACE 4
static int64_t s_swap_at_us;
bool stage_live_swap_due(void)
{
    return esp_timer_get_time() - s_swap_at_us >= (int64_t)SWAP_PACE * gfx_swap_blip_us();
}

esp_err_t stage_live_present(int l, const int16_t *x0, const int16_t *x1)
{
    if (l < 0 || l >= STAGE_LIVE_N || !s_live[l] || !s_live_back[l]) return ESP_ERR_INVALID_STATE;
    back_lock();
    uint8_t *t = s_live[l];
    s_live[l] = s_live_back[l];
    s_live_back[l] = t;
    back_unlock();
    /* The classic path's image, repointed and no more: LVGL keeps no image cache (sdkconfig
     * LV_CACHE_DEF_SIZE 0) and reads the data at its next draw, which the damage below
     * asks for where the picture changed. A set_src invalidated the whole image, and LVGL
     * drew the disc in software under L4's own composite (Bronze: ~150 ms a present). */
    if (s_live_obj[l]) s_live_dsc[l].data = s_live[l] + (live_fmt(l) == LF_I8 ? LIVE_I8_ARGB : LIVE_OFF);
    /* The row's plane, repointed in place (refresh reads the front): a build deletes and
     * adds every row of the theme. */
    if (s_lay_app && orb_svc_layers_live(s_lay_app) > 0) clock_layers_refresh();
#if CLK_BG_DIRECT_PUSH && CONFIG_ORB_DEBUG_HTTP
    /* Pre-painted (an app's sweep, §6.27): the changed rows composed at once from the new
     * picture, one blip, and the planes rebuilt in the slack after. A merged layer 0 is no
     * job of its own. */
    s_swap_at_us = esp_timer_get_time();
    gfx_swap_note();
    if (orb_sched_l4_active() && !(s_live_merged && l == 0) &&
        gfx_prep_swap(x0, x1, live_id(l, s_live[l])))
        return ESP_OK;
#else
    s_swap_at_us = esp_timer_get_time();
    gfx_swap_note();
#endif
    if (!x0 || !x1) { stage_live_damage(0, 0, CLK_SCR_W, CLK_SCR_H); return ESP_OK; }
#if CLK_BG_DIRECT_PUSH
    if (orb_sched_l4_active()) {                          /* the changed rows alone */
        for (int y = 0; y < CLK_SCR_H; y++)
            if (x0[y] <= x1[y]) stage_live_damage(x0[y], y, x1[y] - x0[y] + 1, 1);
        return ESP_OK;
    }
#endif
    int y0 = CLK_SCR_H, y1 = -1, a = CLK_SCR_W, b = -1;  /* LVGL's path: one box round them */
    for (int y = 0; y < CLK_SCR_H; y++) {
        if (x0[y] > x1[y]) continue;
        if (y < y0) y0 = y;
        y1 = y;
        if (x0[y] < a) a = x0[y];
        if (x1[y] > b) b = x1[y];
    }
    if (y1 >= y0) stage_live_damage(a, y0, b - a + 1, y1 - y0 + 1);
    return ESP_OK;
}

/* radar-plan §7.7: the swap waiting in layer l's back copy, its changed rows x0/x1, built
 * ahead while its present is paced (the same planes and conditions the present's own path
 * takes). A new back drops it (stage_live_ahead_drop). */
bool stage_live_ahead(int l, const int16_t *x0, const int16_t *x1)
{
#if CLK_BG_DIRECT_PUSH && CONFIG_ORB_DEBUG_HTTP
    if (l < 0 || l >= STAGE_LIVE_N || !s_live[l] || !s_live_back[l] || !x0 || !x1) return false;
    if (!orb_sched_l4_active() || (s_live_merged && l == 0)) return false;
    const uint8_t *b = s_live_back[l];
    if (live_fmt(l) == LF_I8)                             /* §7.9: its palette behind its values */
        return gfx_prep_ahead(l, x0, x1, live_id(l, s_live[l]), (const uint16_t *)(void *)(b + LIVE_I8_565),
                              b + LIVE_I8_IDX, b + LIVE_I8_A);
    return gfx_prep_ahead(l, x0, x1, live_id(l, s_live[l]), (const uint16_t *)(void *)(b + LIVE_OFF),
                          live_id(l, b), NULL);
#else
    (void)l; (void)x0; (void)x1;
    return false;
#endif
}
void stage_live_ahead_drop(int l)
{
    gfx_prep_ahead_drop(l >= 0 && l < STAGE_LIVE_N ? live_id(l, s_live_back[l]) : NULL);
}
bool stage_live_ahead_ready(int l)
{
    if (l < 0 || l >= STAGE_LIVE_N || !s_live_back[l]) return false;
    return gfx_prep_ahead_ready(live_id(l, s_live_back[l]));
}

void stage_live_damage(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if ((!s_live[0] && !s_live[1]) || !s_scr || w < 1 || h < 1) return;
    int32_t x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CLK_SCR_W - 1) x1 = CLK_SCR_W - 1;
    if (y1 > CLK_SCR_H - 1) y1 = CLK_SCR_H - 1;
    if (x1 < x0 || y1 < y0) return;
#if CLK_BG_DIRECT_PUSH
    if (orb_sched_l4_active()) {
#if CONFIG_ORB_DEBUG_HTTP
        /* pre-paint holds the layer in its planes as a slow job whose pose never moves:
         * the box is rebuilt from the new pixels, not copied from the old */
        gfx_prep_stale(y0, y1, x0, x1);
#endif
        gfx_mark_rows(y0, y1, x0, x1, false);
        return;
    }
#endif
    lv_area_t b = { x0, y0, x1, y1 };
    lv_obj_invalidate_area(s_scr, &b);
}

void stage_now(time_t *t, double *frac, double *wall)
{
    if (!s_now_t) {                    /* asked before the first frame (the repeater) */
        bool synced;
        orb_time_now(&s_now_t, &s_now_frac, &synced);
        s_now_wall = s_now_frac;
    }
    *t = s_now_t; *frac = s_now_frac; *wall = s_now_wall;
}

void stage_pose(int i, int32_t ang_10, int32_t ox, int32_t oy)
{
    if (i < 0 || i >= CLK_ROW_MAX) return;
    s_g_ang[i] = ang_10 == INT32_MIN ? ROW_HIDDEN : ((ang_10 % 3600) + 3600) % 3600;
    s_g_ox[i] = (int16_t)ox;
    s_g_oy[i] = (int16_t)oy;
}

/* theme-pool-plan §2.6: the app picks a row's frame. The answer is the row's frame
 * count (1 for a row of one image, 0 for no such row); f in range shows frame f from
 * the next frame on, and f < 0 only asks. */
int stage_frame(int i, int32_t f)
{
    if (s_base || i < 0 || i >= CLK_ROW_MAX || i >= (int)orb_theme_geo()->n_rows) return 0;
    const int n = row_picks((uint16_t)i) ? (int)clk_theme.pick_n[i] : 1;
    if (f >= 0 && f < n && row_picks((uint16_t)i)) s_g_frame[i] = (int16_t)f;
    return n;
}

/* theme-text-plan §2.3: a label's string and tone, composed on the next frame. 1 for a
 * label, 0 for any other row; the same string and tone again costs nothing. */
int stage_text(int i, const char *str, int32_t tone)
{
    if (s_base || !s_text || !s_row || i < 0 || i >= CLK_ROW_MAX || !s_row[i].tplane) return 0;
    if (tone < 0 || tone > 3) tone = 0;
    if (!str) str = "";
    if (s_text->tone[i] == (uint8_t)tone &&
        strncmp(s_text->text[i], str, LABEL_TEXT_MAX - 1) == 0) return 1;
    snprintf(s_text->text[i], LABEL_TEXT_MAX, "%s", str);
    s_text->tone[i] = (uint8_t)tone;
    __sync_synchronize();        /* the string lands before the frame sees its number */
    s_text->seq[i]++;
    return 1;
}

/* theme-text-plan T2: a row's opacity over its art, 0..255, from the next frame on. */
void stage_opacity(int i, int32_t opa)
{
    if (i < 0 || i >= CLK_ROW_MAX) return;
    s_g_opa[i] = (uint8_t)(opa < 0 ? 0 : opa > 255 ? 255 : opa);
}

void stage_comp_set(int i, int32_t value, bool live)
{
    if (i < 0 || i >= CLK_COMP_MAX) return;
    s_g_cval[i] = value;
    s_g_clive[i] = live;
}

/* The app's voices: its own theme's PCM, owned by its row (so the registry's suspend
 * silences them) and leased (so a parked dial does too). The sound cap is read off the
 * grant word, not the counting gate: a revoked cap is a state the app asks about every
 * frame, not a stream of refusals. */
int stage_snd_play(int i, uint32_t flags, int64_t at_us, int64_t anchor_us)
{
    return stage_snd_play_v(s_lay_app, i, flags, at_us, anchor_us, 100);
}

/* §59: the same, at the app's own level for this voice (the clock's tick and chime
 * volumes). 0..100 on the OS volume's own law, half a dB a step down from full and 0
 * silent, so an app slider and the Sound page's feel alike; the OS volume still sits
 * on top of both, in the codec. */
uint16_t stage_snd_gain(int vol)
{
    if (vol <= 0) return 0;
    if (vol >= 100) return ORB_SOUND_GAIN_1;
    return (uint16_t)(ORB_SOUND_GAIN_1 * powf(10.0f, -(float)(100 - vol) * 0.5f / 20.0f) + 0.5f);
}

/* theme-cache-plan §2.6: from the app's voice bank, mapped from boot, so it plays whatever is
 * on the glass. A loop is leased to the app's ticks on the glass (the ticking stops when it
 * leaves); a one-shot is not (a chime plays to its end, and may start off the glass). */
int stage_snd_play_v(orb_app_t *app, int i, uint32_t flags, int64_t at_us, int64_t anchor_us, int vol)
{
    if (!app || !(orb_svc_grants(app) & (1u << CAP_SOUND))) return -1;
    const uint8_t *bank = NULL;
    const orb_voice_t *vb = orb_theme_voice(app->slug, &bank);
    if (!vb || !bank || i < 0 || (uint32_t)i >= vb->n) return -1;
    const clk_sound_t *t = &vb->s[i];
    const bool loop = (flags & ORB_SOUND_LOOP) || (t->flags & CLK_SND_F_LOOP);
    const uint32_t f = (loop ? ORB_SOUND_LEASED : 0u) |
                       (flags & (ORB_SOUND_LOOP | ORB_SOUND_ANCHORED | ORB_SOUND_ASKED));
    return orb_sound_play_enc(app, bank + t->pcm_off, t->n, t->enc,
                              stage_snd_gain(vol), f, at_us, anchor_us);
}
