/**
 * orb_carousel.c — the knob's text carousel (see header; ruling C5).
 *
 * One label on LVGL's top layer, shown while a flick is in flight and hidden
 * the moment the destination is real. Two rules in here are load-bearing and
 * both are easy to undo by accident:
 *
 *  1. The chip lives on lv_layer_top(), not inside an app's screen. An app's
 *     screen is its own; chrome that walks between apps may not be a child of
 *     one of them, or every app would have to keep the chip alive, keep it
 *     above its own widgets, and keep it out of its own repaint maths.
 *  2. The beat is parked for the length of the peek. clock_layer_commit()
 *     rebuilds marked rows from the GIF canvas plus an explicit layer list
 *     (clock_view.c:938), and the chip is in neither, so an unparked tick
 *     erases it inside one frame. Park the tick and there is nothing to erase
 *     it. That is orb_sched_pause(), NOT the app's suspend hook: suspend also
 *     releases the art bank, and the view it feeds is still the one on the
 *     glass. art_bank_extra_ptr() returning NULL under a live compositor is the
 *     silent garbage paint art_bank.h fences off.
 */

#include "orb_carousel.h"
#include "orb_app.h"
#include "orb_sched.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "carousel";

/* The chip stays inside the centred ~320 px column the menu and the picker both
 * use, so the round glass never clips a label. */
#define CHIP_W        320
#define CHIP_H         64
#define CHIP_BOTTOM    96    /* px up from the bottom edge, clear of the curve */

/* How long the named entry must hold still before it is committed.
 *
 * Two windows, because the hand turned out to have two regimes and one number
 * could not serve both. Measured on the bench knob (owner's hand, 16 detents,
 * device-side peek timestamps, docs/app-platform-plan.md §12.9):
 *
 *   in a flick   detent-to-detent gaps of 33-157 ms.
 *   to read      stopping on a name to decide produced pauses of 510-1936 ms.
 *
 * One window above the read-pauses taxes every clean flick by seconds and
 * breaches the ~1 s at which a selection feels dropped; one below them dumps
 * the user onto an entry they were still deciding on — the exact complaint
 * that killed the single 400 ms. So a detent arriving less than BURST_GAP
 * after the previous one is one gesture and commits SETTLE later; a detent
 * after a pause, or alone (a pause the shell never witnessed ending), gets
 * READ, and no switch happens while it is open. Pressing the knob commits
 * instantly at any point (input_router.c, "that one, now"), so the person
 * who has finished reading never waits on the timer.
 *
 * SETTLE keeps its old floors: it clears the encoder's recovery window
 * (KNOB_SETTLE_MS 30 ms, knob.c:82) and the picker's velocity-idle reset
 * (VEL_IDLE_US 220 ms, color_picker.c:38), and is 2.5x the widest flick the
 * hand produced. BURST_GAP is cut between the two observed regimes; READ
 * covers the longest pause observed. tools/host_carousel_test.py asserts all
 * of these bounds so a future edit cannot drift back into the one-timer bug. */
#define ORB_CAROUSEL_SETTLE_MS      400   /* in-burst: commit this after the last detent */
#define ORB_CAROUSEL_READ_MS       2000   /* after a pause: hold the preview this long   */
#define ORB_CAROUSEL_BURST_GAP_MS   300   /* at/beyond this gap, the next detent is a read */

/* The peek, written on the LVGL task only. s_pending is the entry the chip names
 * and the one a due() hands back to activate; NULL means the shell is not on the
 * glass. s_hops counts detents consumed by the current burst, which is the
 * number the bench judges: N hops must still cost ONE activate. */
static const orb_app_t *s_pending;
static int64_t          s_last_us;    /* last detent that moved the name        */
static int64_t          s_due_us;     /* commit instant, chosen per detent      */
static int              s_hops;
static bool             s_parked;     /* WE stopped the beat; only we restart it */

static lv_obj_t *s_chip;
static lv_obj_t *s_lbl;

/* ---- the widget ---------------------------------------------------------- */

static void chip_hide(void)
{
    if (s_chip) lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
}

/* Name `app` on the chip and raise it. Re-texts one pre-built label rather than
 * allocating, because this runs once per detent. */
static void chip_show(const orb_app_t *app)
{
    if (!s_chip || !s_lbl) return;
    lv_label_set_text(s_lbl, app->title && app->title[0] ? app->title : app->slug);
    lv_obj_remove_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    /* The top layer is drawn after every screen, so no z-order negotiation with
     * the app underneath is possible, or needed. */
    lv_obj_move_foreground(s_chip);
}

esp_err_t orb_carousel_init(void)
{
    /* The top layer outlives lv_screen_load(), which is precisely what the peek
     * needs: the chip must hold position while the glass underneath it changes. */
    lv_obj_t *top = lv_layer_top();
    if (!top) return ESP_FAIL;

    s_chip = lv_obj_create(top);
    if (!s_chip) return ESP_ERR_NO_MEM;
    s_lbl = lv_label_create(s_chip);
    if (!s_lbl) return ESP_ERR_NO_MEM;

    lv_obj_set_size(s_chip, CHIP_W, CHIP_H);
    lv_obj_align(s_chip, LV_ALIGN_BOTTOM_MID, 0, -CHIP_BOTTOM);
    /* Opaque on purpose. Over a live dial no translucency reads as chrome — it
     * just fogs the hands it covers, and LVGL blends it per pixel inside the
     * very flush the fps budget measures. */
    lv_obj_set_style_bg_color(s_chip, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_chip, CHIP_H / 2, 0);
    lv_obj_set_style_border_width(s_chip, 0, 0);
    lv_obj_set_style_pad_all(s_chip, 0, 0);
    lv_obj_set_scrollable(s_chip, false);
    lv_obj_set_scrollbar_mode(s_chip, LV_SCROLLBAR_MODE_OFF);
    /* Floating so a parent scroll never moves it, and never clickable: the knob
     * is this device's only pointer, and a chip that could swallow a press is a
     * screen the user cannot leave. */
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(s_chip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_text_color(s_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl, &lv_font_montserrat_48, 0);
    lv_label_set_long_mode(s_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_lbl, CHIP_W - 32);
    lv_label_set_text(s_lbl, "");
    lv_obj_center(s_lbl);

    chip_hide();
    ESP_LOGI(TAG, "chip ready (settle %d ms, read %d ms, burst gap %d ms)",
             ORB_CAROUSEL_SETTLE_MS, ORB_CAROUSEL_READ_MS,
             ORB_CAROUSEL_BURST_GAP_MS);
    return ESP_OK;
}


/* ---- the peek ------------------------------------------------------------ */

/* Stop the beat for the length of the peek, remembering that we were the one who
 * did it. Both guards matter: on the picker and the menu there is no tick
 * running to park, and claiming we parked it would have cancel() resume a beat
 * belonging to whoever owns the glass. */
static void park_beat(void)
{
    if (s_parked || !orb_sched_started() || !orb_sched_running()) return;
    orb_sched_pause();
    s_parked = true;
}

static void unpark_beat(void)
{
    if (!s_parked) return;
    orb_sched_resume();
    s_parked = false;
}

const orb_app_t *orb_carousel_step(int step, const orb_app_t *on_glass, int64_t now_us)
{
    /* No chip, no peek. Deferring a destination the user cannot see being chosen
     * is the one failure mode worse than no carousel, so the caller falls back to
     * the immediate switch it used before this module existed. */
    if (!s_chip || !s_lbl) return NULL;
    if (!on_glass) return NULL;

    /* Walk from the entry ALREADY named, not from the glass. The registry steps
     * one entry per call, so restarting every call at on_glass would re-name the
     * same neighbour on each detent of a long flick and the burst could never
     * travel further than one screen — the exact bug the deferred activate makes
     * visible, because nothing swaps under the chip to notice. */
    const orb_app_t *from = s_pending ? s_pending : on_glass;
    const orb_app_t *next = orb_app_neighbour(from, step);
    if (!next) return NULL;

    /* Named the entry that is already showing: nothing to peek at and nothing to
     * commit. A single-walkable table lands here every detent, as does a burst
     * that wraps the whole way round. Hide rather than paint a name over its own
     * screen, and leave the beat to the view that is actually up. */
    if (next == on_glass) {
        orb_carousel_cancel();
        return next;
    }

    /* Classify BEFORE anything changes: this detent less than BURST_GAP after
     * the previous one is the same gesture and commits on the fast window; one
     * that lands after a pause — or with nothing pending yet, a pause the shell
     * never saw end — is the user stopping to read, and holds the preview on
     * the read window. The predicate must read the pending state as it was on
     * the PREVIOUS detent: s_pending is overwritten two lines below, and a
     * check placed after that overwrite is always true, silently making every
     * detent look like a continuing burst — cancel() would then leak its
     * window into the next flick's classification. */
    const bool in_burst = s_last_us && s_pending &&
        (now_us - s_last_us) < (int64_t)ORB_CAROUSEL_BURST_GAP_MS * 1000;
    s_hops++;
    s_pending  = next;
    s_last_us  = now_us;
    s_due_us   = now_us +
                 (int64_t)(in_burst ? ORB_CAROUSEL_SETTLE_MS
                                    : ORB_CAROUSEL_READ_MS) * 1000;
    park_beat();
    chip_show(next);

    /* One line per named entry, so the bench counts names against the activates
     * the burst costs: the pass line for this whole module is that a 3-detent
     * flick prints three of these and exactly one "switch" line from the router. */
    ESP_LOGI(TAG, "peek %s (%d hop%s)", next->slug, s_hops,
             s_hops == 1 ? "" : "s");
    return next;
}

bool orb_carousel_due(int64_t now_us)
{
    return s_pending && now_us >= s_due_us;
}

const orb_app_t *orb_carousel_pending(void)
{
    return s_pending;
}

bool orb_carousel_ready(void)
{
    return s_chip && s_lbl;
}

/* Both exits below clear the state first and touch the widget second, because
 * the caller switches screens straight after, and lv_screen_load() from inside
 * the same LVGL callback is what §12.3's ordering exists to keep honest. */

void orb_carousel_commit(void)
{
    if (!s_pending) { unpark_beat(); return; }
    ESP_LOGI(TAG, "settle %s after %d hop%s", s_pending->slug, s_hops,
             s_hops == 1 ? "" : "s");
    s_pending = NULL;
    s_hops    = 0;
    chip_hide();
    /* The park is HANDED OVER, not released: s_parked clears so the next peek's
     * park_beat() actually runs, but orb_sched_resume() is not called here. The
     * activation the caller runs next starts the beat against the INCOMING view;
     * resuming it here would put a frame of the outgoing view back on the glass
     * between two halves of one switch — the flicker the park exists to prevent,
     * self-inflicted. If that activation is refused, every refusal path in
     * orb_app_activate() funnels through app_restore(), which puts the beat back.
     *
     * The handover needs a name, not just a comment: had this cleared nothing, a
     * commit to the picker (whose activate leaves the clock's tick parked) would
     * leave s_parked stuck true, the next peek would skip its own park, and the
     * cancel after it would resume a beat belonging to nobody. */
    s_parked = false;
}

void orb_carousel_cancel(void)
{
    const orb_app_t *was = s_pending;
    s_pending = NULL;
    s_hops    = 0;
    chip_hide();
    /* Nobody is switching, so the view still on the glass owns the beat again. */
    unpark_beat();
    if (was) ESP_LOGI(TAG, "cancelled on %s — no switch", was->slug);
}
