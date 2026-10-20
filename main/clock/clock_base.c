/* clock_base.c — the digital face the dial shows with no theme. §26.
 *
 * Three labels, two fonts that are already in the image, and one time source shared
 * with every app. Nothing here reads a bank, a clk_theme_t or a blob: the module's
 * whole reason to exist is that it works when all three are absent, and the first
 * draft of a base render that reached for art would just be a theme with a missing
 * file. So there is no fallback logic in here, only a face.
 *
 * The fonts are 48 and 14 because those two are already compiled in for the menu and
 * the carousel (CONFIG_LV_FONT_MONTSERRAT_{48,14}=y). Adding a larger one for a bigger
 * readout costs flash on every board to improve a state a board leaves the moment it is
 * given a theme, which is the wrong trade: 48 px digits on a 466 px panel are readable
 * across a room, and the date line at 14 px is the same size the menu already uses for
 * its status. If the face ever wants a larger numeral, the honest fix is to shrink what
 * the carousel pays, not to grow the total.
 */
#include "clock_base.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "orb_svc.h"      /* orb_time_now(): the same wall second every app reads */

static lv_obj_t   *s_root, *s_time, *s_date, *s_status;
static lv_timer_t *s_timer;
static bool        s_visible;
static bool      s_synced_last = false;
static char      s_status_txt[24];

/* The status line's whole content, in the two states it has. An unsynced clock says
 * so rather than showing a wrong time quietly: with no theme there is no dial face to
 * blame, and a device that has never been on a network will sit in this state for its
 * first minutes on a shelf, which is exactly when an owner is deciding whether it
 * works. Wording is fixed-width-safe at 14 px inside the 466 px panel. */
static const char *STATUS_UNSYNCED = "waiting to sync";
static const char *STATUS_SYNCED   = "";

static void tick_cb(lv_timer_t *timer) { (void)timer; clock_base_tick(); }

static void set_text(lv_obj_t *o, const char *t)
{
    if (strcmp(lv_label_get_text(o), t) != 0) lv_label_set_text(o, t);
}

void clock_base_show(lv_obj_t *parent)
{
    if (s_visible && s_root && lv_obj_get_parent(s_root) == parent) {
        clock_base_tick();
        return;
    }
    if (!s_root) {
        s_root = lv_obj_create(parent);
        lv_obj_set_size(s_root, 466, 466);
        lv_obj_set_pos(s_root, 0, 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_root, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_root, 0, LV_PART_MAIN);
        lv_obj_set_scrollable(s_root, false);
        lv_obj_set_flag(s_root, LV_OBJ_FLAG_SCROLLABLE, false);

        /* Vertical stack, centred by hand rather than by a flex layout: three objects
         * and a fixed panel make the arithmetic shorter than the layout code, and it
         * keeps the time's optical centre where the dial's own pivot is (233, 233), so
         * a board that gains a theme later shows the time in roughly the place the
         * theme will put its hands. */
        s_time = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(s_time, lv_color_white(), 0);
        lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -34);

        s_date = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_date, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_date, lv_color_hex(0xBFBFBF), 0);
        lv_obj_align(s_date, LV_ALIGN_CENTER, 0, 18);

        s_status = lv_label_create(s_root);
        lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_status, lv_color_hex(0x8A8A8A), 0);
        lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 46);
    } else {
        lv_obj_set_parent(s_root, parent);
        lv_obj_set_hidden(s_root, false);
    }
    s_visible = true;
    clock_base_run();
}

void clock_base_hide(void)
{
    if (!s_visible) return;
    s_visible = false;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_root) lv_obj_set_hidden(s_root, true);
}

void clock_base_run(void)
{
    if (s_timer || !s_visible) return;
    /* One second, not the dial's 12 fps. The readout changes on a minute boundary; a
     * faster tick would dirty nothing and cost the flash-drawing budget the themed dial
     * needs, and this board has exactly one screen. The period is a floor rather than a
     * guarantee of on-time digit flips: worst case a minute rolls one tick late, which
     * reads as a rounded second, not as a clock that is wrong. */
    s_timer = lv_timer_create(tick_cb, 1000, NULL);
    clock_base_tick();
}

bool clock_base_visible(void) { return s_visible; }

void clock_base_tick(void)
{
    if (!s_visible || !s_time) return;

    bool   synced = false;
    time_t t;
    double frac;
    orb_time_now(&t, &frac, &synced);

    /* localtime_r, not gmtime: orb_net owns TZ and applies it with setenv()+tzset(), so
     * this is the call that makes the base face agree with a themed one after a zone
     * change — the same reason clock_view.c uses it. The floor on frac is deliberate: a
     * second that flickers early reads as a broken clock to a person, and the
     * sub-second fraction belongs to a sweep hand this face does not have. */
    const time_t wall = t + (frac >= 1.0 ? 1 : 0);
    struct tm tmv;
    if (!localtime_r(&wall, &tmv)) {
        set_text(s_time, "--:--");
        set_text(s_date, "");
        return;
    }

    char hhmm[8];
    snprintf(hhmm, sizeof hhmm, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    set_text(s_time, hhmm);

    char date[24];
    if (strftime(date, sizeof date, "%a %e %b", &tmv) == 0) date[0] = '\0';
    set_text(s_date, date);

    /* The status line changes on the edge, not every tick: rewriting a label the same
     * value dirties it, and this face is up on a board that may sit unthemed for days. */
    if (synced != s_synced_last || s_status_txt[0] == '\0') {
        s_synced_last = synced;
        strlcpy(s_status_txt, synced ? STATUS_SYNCED : STATUS_UNSYNCED,
                sizeof s_status_txt);
        set_text(s_status, s_status_txt);
        /* A synced clock shows nothing here rather than a reassuring word: the time IS
         * the confirmation, and static text that never changes teaches nothing. */
        lv_obj_set_style_text_opa(s_status, s_status_txt[0] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}
