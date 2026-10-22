/**
 * color_picker.c — HSB editor driven by the rotary knob.
 *
 * Three state numbers (hue 0..359, saturation 0..100, brightness/value 0..100)
 * plus a "which channel is selected" cursor. A turn nudges the selected channel;
 * a push moves the cursor. Everything renders through lv_color_hsv_to_rgb so the
 * swatch is the actual resulting color, and the active chip is tinted to match.
 *
 * All LVGL calls arrive via color_picker_apply_input(), invoked by input_router
 * from its poll timer on the LVGL task — the only context allowed to touch
 * widgets (same rule clock_view follows).
 */

#include "color_picker.h"
#include "board_config.h"
#include "orb_svc.h"      /* D35: the row's settings persist through the broker's cfg */

#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "color";

/* ---- Velocity-adaptive stepping (issue: slow=1, fast=5) ----
 *
 * We can't use LVGL's tick here (this runs from the router's own lv_timer, and
 * several turns can land in one 10 ms poll), so we measure real time with
 * esp_timer. Instantaneous speed = |detents| / seconds between successive turn
 * events. We smooth it with an EMA so a single burst doesn't slam the step, and
 * map that speed onto a per-detent multiplier from STEP_MIN (slow) to STEP_MAX
 * (fast). If the knob has been idle past VEL_IDLE_US we treat the next turn as
 * a fresh slow start (multiplier drops back toward STEP_MIN), so an
 * accelerate-then-stop-then-start again feels natural. */
#define STEP_MIN        1      /* per-detent change when turning slowly       */
#define STEP_MAX        5      /* per-detent change when turning fast         */
#define VEL_SLOW_DPS    6.0f   /* detents/sec at/below which step == STEP_MIN */
#define VEL_FAST_DPS    30.0f  /* detents/sec at/above which step == STEP_MAX */
#define VEL_IDLE_US     220000 /* gap that resets the smoothed velocity       */
#define VEL_EMA         0.35f  /* new-sample weight in the speed smoother     */

static int64_t s_last_turn_us = 0;   /* esp_timer time of the last turn event */
static float   s_dps = 0.0f;         /* smoothed detents/sec                  */

static int current_step(void)
{
    if (s_dps <= VEL_SLOW_DPS) return STEP_MIN;
    if (s_dps >= VEL_FAST_DPS) return STEP_MAX;
    float t = (s_dps - VEL_SLOW_DPS) / (VEL_FAST_DPS - VEL_SLOW_DPS);
    int m = STEP_MIN + (int)(t * (STEP_MAX - STEP_MIN) + 0.5f);
    if (m < STEP_MIN) m = STEP_MIN;
    if (m > STEP_MAX) m = STEP_MAX;
    return m;
}

/* Feed one turn reading (raw net detents since last poll) into the estimator
 * and return the per-detent multiplier to apply this event. */
static int update_velocity(int32_t raw_delta)
{
    int64_t now = esp_timer_get_time();
    if (s_last_turn_us == 0 || (now - s_last_turn_us) > VEL_IDLE_US) {
        s_dps = 0.0f;                    /* fresh slow start after an idle gap */
    } else {
        double dt_s = (double)(now - s_last_turn_us) / 1e6;
        if (dt_s > 0.0) {
            float inst = (float)abs(raw_delta) / (float)dt_s;
            s_dps += VEL_EMA * (inst - s_dps);
        }
    }
    s_last_turn_us = now;
    return current_step();
}

/* Per-detent adjustment baseline for each channel. Hue wraps, s/v clamp; the
 * actual per-event nudge is baseline * current_step(). */
#define HUE_STEP  1
#define SAT_STEP  1
#define VAL_STEP  1

typedef enum { CH_HUE = 0, CH_SAT, CH_VAL, CH_COUNT } channel_t;

/* ---- model ---- */
static channel_t s_ch  = CH_HUE;
static uint16_t  s_hue = 210;   /* pleasant blue to start */
static uint8_t   s_sat = 80;
static uint8_t   s_val = 90;

/* ---- view ---- */
static lv_obj_t *s_scr;
static lv_obj_t *s_swatch;
static lv_obj_t *s_chip[CH_COUNT];
static lv_obj_t *s_chip_lbl[CH_COUNT];
static lv_obj_t *s_value_lbl;
static lv_obj_t *s_hint_lbl;

/* The title card shown while unarmed: a big "HSB" and the one gesture that
 * leaves it. It is a separate label rather than a retextured s_value_lbl so the
 * armed editor's layout is never half-drawn behind the title. */
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_title_hint;
static bool      s_armed;

static const char *const CH_LETTER[CH_COUNT] = { "H", "S", "B" };

/* Paint one chip: active = white text on the current color; inactive = dim. */
static void style_chip(channel_t c)
{
    lv_color_t accent = lv_color_hsv_to_rgb(s_hue, 70, 85);
    if (c == s_ch) {
        lv_obj_set_style_bg_color(s_chip[c], accent, 0);
        lv_obj_set_style_text_color(s_chip_lbl[c], lv_color_white(), 0);
        lv_obj_set_style_border_color(s_chip[c], lv_color_white(), 0);
        lv_obj_set_style_border_width(s_chip[c], 2, 0);
    } else {
        lv_obj_set_style_bg_color(s_chip[c], lv_color_make(0x18, 0x18, 0x18), 0);
        lv_obj_set_style_text_color(s_chip_lbl[c], lv_color_make(0x80, 0x80, 0x80), 0);
        lv_obj_set_style_border_width(s_chip[c], 0, 0);
    }
}

/* Show exactly one of the two views. LVGL keeps drawing hidden objects out of
 * the tree cost, so the editor is hidden rather than moved off-screen — the
 * swatch and chips must not bleed into the title card. */
static void set_views(bool armed)
{
    lv_obj_set_hidden(s_title_lbl,  armed);
    lv_obj_set_hidden(s_title_hint, armed);
    lv_obj_set_hidden(s_swatch,    !armed);
    lv_obj_set_hidden(s_value_lbl, !armed);
    lv_obj_set_hidden(s_hint_lbl,  !armed);
    for (int i = 0; i < CH_COUNT; i++) lv_obj_set_hidden(s_chip[i], !armed);
}

static void refresh(void)
{
    if (!s_armed) {
        /* Title card: nothing editable is drawn, and the swatch is not painted
         * either, so the screen cannot hint at a color it is not showing. */
        lv_label_set_text(s_title_lbl, "HSB");
        lv_label_set_text(s_title_hint, "push to edit");
        set_views(false);
        return;
    }
    set_views(true);

    lv_color_t c = lv_color_hsv_to_rgb(s_hue, s_sat, s_val);
    lv_obj_set_style_bg_color(s_swatch, c, 0);

    char buf[24];
    if (s_ch == CH_HUE)      snprintf(buf, sizeof(buf), "HUE  %u",   (unsigned)s_hue);
    else if (s_ch == CH_SAT) snprintf(buf, sizeof(buf), "SAT  %u%%", (unsigned)s_sat);
    else                     snprintf(buf, sizeof(buf), "BRI  %u%%", (unsigned)s_val);
    lv_label_set_text(s_value_lbl, buf);

    for (int i = 0; i < CH_COUNT; i++) style_chip((channel_t)i);
}

/* D35's commit edge set (see color_picker_bind_app in the Public API section):
 * any adjust since the last commit must land in NVS when the editor closes. */
static bool s_dirty;
static void picker_persist(void);

static void adjust(int32_t delta, int step)
{
    if (delta == 0) return;
    switch (s_ch) {
        case CH_HUE:
            s_hue = (uint16_t)(((int32_t)s_hue + delta * step * HUE_STEP % 360 + 360) % 360);
            break;
        case CH_SAT: {
            int v = (int)s_sat + delta * step * SAT_STEP;
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            s_sat = (uint8_t)v;
            break;
        }
        case CH_VAL: {
            int v = (int)s_val + delta * step * VAL_STEP;
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            s_val = (uint8_t)v;
            break;
        }
        default: break;
    }
    s_dirty = true;      /* commit happens when the editor closes, not per turn */
    refresh();
}

static void cycle_channel(void)
{
    s_ch = (channel_t)((s_ch + 1) % CH_COUNT);
    refresh();
}

/* ---- D35 settings: load once, commit on the disarm edge --------------------
 *
 * The picker's three numbers are the app's own config now: the factory
 * container declares hue/sat/bri as int settings and the broker stores them in
 * the row's orb.app<slot> namespace under the same quota, revocation and
 * uninstall wipe every other app's config pays. The broker's cfg_get answers
 * the DECLARED default for an absent key, so a first boot needs no seed write,
 * and the statics above keep the same numbers as compile-time fallback for a
 * caller that never binds a row (the host sentinel, and no other). */
static orb_app_t *s_app;

void color_picker_bind_app(orb_app_t *app)
{
    s_app = app;
    if (!app) return;
    const orb_svc_t *svc = orb_svc_table();
    int32_t v;
    if (svc->cfg_get(app, "hue", &v) == ESP_OK) s_hue  = (uint16_t)v;
    if (svc->cfg_get(app, "sat", &v) == ESP_OK) s_sat  = (uint8_t)v;
    if (svc->cfg_get(app, "bri", &v) == ESP_OK) s_val  = (uint8_t)v;
    s_dirty = false;
}

static void picker_persist(void)
{
    if (!s_app || !s_dirty) return;
    const orb_svc_t *svc = orb_svc_table();
    svc->cfg_set(s_app, "hue", s_hue);
    svc->cfg_set(s_app, "sat", s_sat);
    svc->cfg_set(s_app, "bri", s_val);
    s_dirty = false;
}

/* ---- Public API ---- */
esp_err_t color_picker_init(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_scrollable(s_scr, false);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);

    /* Big circular swatch — the resulting color, upper-centre of the disc. */
    s_swatch = lv_obj_create(s_scr);
    lv_obj_set_size(s_swatch, 170, 170);
    lv_obj_set_style_radius(s_swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_swatch, 2, 0);
    lv_obj_set_style_border_color(s_swatch, lv_color_make(0x33, 0x33, 0x33), 0);
    lv_obj_set_style_pad_all(s_swatch, 0, 0);
    lv_obj_set_scrollable(s_swatch, false);
    lv_obj_align(s_swatch, LV_ALIGN_TOP_MID, 0, 40);

    /* Three option chips, centred as a row below the swatch. */
    const int chip_w = 70, chip_h = 40, gap = 14;
    const int total  = CH_COUNT * chip_w + (CH_COUNT - 1) * gap;
    const int row_left = (SCREEN_WIDTH - total) / 2;
    for (int i = 0; i < CH_COUNT; i++) {
        s_chip[i] = lv_obj_create(s_scr);
        lv_obj_set_size(s_chip[i], chip_w, chip_h);
        lv_obj_set_style_radius(s_chip[i], 10, 0);
        lv_obj_set_style_pad_all(s_chip[i], 0, 0);
        lv_obj_set_scrollable(s_chip[i], false);
        lv_obj_set_pos(s_chip[i], row_left + i * (chip_w + gap), 230);

        s_chip_lbl[i] = lv_label_create(s_chip[i]);
        lv_label_set_text(s_chip_lbl[i], CH_LETTER[i]);
        lv_obj_set_style_text_font(s_chip_lbl[i], &lv_font_montserrat_14, 0);
        lv_obj_center(s_chip_lbl[i]);
    }

    /* Current channel + value, large. */
    s_value_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_value_lbl, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_value_lbl, "HUE  210");
    lv_obj_align(s_value_lbl, LV_ALIGN_TOP_MID, 0, 290);

    /* Usage hint at the very bottom. */
    s_hint_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_hint_lbl, lv_color_make(0x90, 0x90, 0x90), 0);
    lv_obj_set_style_text_font(s_hint_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_hint_lbl, "rotate: adjust   push: option");
    lv_obj_align(s_hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* The unarmed title card, built once and swapped for the editor by
     * set_views(). Same 48 px face the menu's landing uses. */
    s_title_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_title_lbl, "HSB");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, -20);

    s_title_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_title_hint, lv_color_make(0x90, 0x90, 0x90), 0);
    lv_obj_set_style_text_font(s_title_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_title_hint, "push to edit");
    lv_obj_align(s_title_hint, LV_ALIGN_CENTER, 0, 40);

    /* Boot unarmed: the picker is reached by rotating to it, and a screen that
     * is merely passing through must not be mid-edit. */
    s_armed = false;
    refresh();
    ESP_LOGI(TAG, "color picker ready (HUE=%u SAT=%u BRI=%u)",
             (unsigned)s_hue, (unsigned)s_sat, (unsigned)s_val);
    return ESP_OK;
}

lv_obj_t *color_picker_screen(void) { return s_scr; }

void color_picker_show(void)
{
    /* Called when this screen becomes the active one: re-push the (persistent)
     * model to the widgets and force a full repaint so the swatch and chips
     * match s_hue/s_sat/s_val rather than whatever the frame last showed. Also
     * drop any stale velocity so returning to the picker starts "slow". */
    s_last_turn_us = 0;
    s_dps = 0.0f;
    refresh();
    if (s_scr) lv_obj_invalidate(s_scr);
}

void color_picker_apply_input(int32_t delta, bool pressed)
{
    /* Armed only. input_router keeps the turn while this screen is a title card,
     * so a stray call here must not edit a color nobody is looking at. */
    if (!s_armed) return;

    if (pressed) {
        const channel_t before = s_ch;
        cycle_channel();
        /* A push is a discrete selection act, not a turn — reset the velocity so
         * the next rotate starts from the slow baseline. */
        s_last_turn_us = 0;
        s_dps = 0.0f;
        ESP_LOGI(TAG, "push %s -> %s", CH_LETTER[before], CH_LETTER[s_ch]);
    } else if (delta) {
        /* The channel value deliberately reads the RAW delta, not knob_row_step():
         * a value is not a list position. Turning clockwise raises it, which agrees
         * with the documented axis anyway (CW = up = right, see knob.h) and is the
         * feel the owner already has under his hand. If that ever needs to change,
         * add a named constant beside KNOB_DIR_DOWN_IS_CW rather than negating here. */
        const int step = update_velocity(delta);
        adjust(delta, step);
        uint16_t h; uint8_t s, v; color_picker_get(&h, &s, &v);
        ESP_LOGI(TAG, "turn %+ld x%d -> %s = %u", (long)delta, step,
                 CH_LETTER[s_ch], (unsigned)(s_ch == CH_HUE ? h : (s_ch == CH_SAT ? s : v)));
    }
}

void color_picker_get(uint16_t *h, uint8_t *s, uint8_t *v)
{
    if (h) *h = s_hue;
    if (s) *s = s_sat;
    if (v) *v = s_val;
}

bool color_picker_armed(void) { return s_armed; }

void color_picker_set_armed(bool armed)
{
    if (s_armed == armed) return;
    /* D35's commit edge: leaving the editor is the last moment this task knows
     * the owner stopped editing, so the three numbers go to the row's own NVS
     * namespace here and nowhere mid-turn — a turn is a gesture, and a gesture
     * would commit dozens of writes a second against the same four keys. */
    if (!armed && s_armed) picker_persist();
    s_armed = armed;
    /* Drop any stale velocity on every transition, so the first turn after
     * entering the editor is always priced as a slow start rather than
     * inheriting how fast the last exit happened. */
    s_last_turn_us = 0;
    s_dps = 0.0f;
    refresh();
    if (s_scr) lv_obj_invalidate(s_scr);
    ESP_LOGI(TAG, "%s", armed ? "editor open" : "title card");
}
