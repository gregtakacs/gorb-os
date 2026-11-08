/**
 * color_picker — the HSB editor, as an app (apps/sdk/orb.h).
 *
 * Three state numbers (hue 0..359, saturation 0..100, brightness 0..100) plus a "which
 * channel is selected" cursor. A turn nudges the selected channel, a push moves the
 * cursor. Unarmed, the page is a title card; a press opens the editor and a hold
 * closes it, which is the moment the three numbers are committed to the app's own
 * declared settings (a turn is a gesture, and a gesture would write dozens of times a
 * second).
 *
 * Moved out of the firmware (it was main/color/color_picker.c) with its look and feel
 * unchanged: the same layout, the same velocity-adaptive step, and the same HSV maths
 * the OS's widget library uses, so the swatch is the colour it always was.
 */
#include "orb.h"

/* ---- velocity-adaptive stepping (slow = 1 per detent, fast = 5) -------------------
 * Instantaneous speed is |detents| / seconds between successive turn events, smoothed
 * with an EMA so one burst does not slam the step; a gap past VEL_IDLE_US starts slow
 * again, so accelerate-stop-start feels natural. */
#define STEP_MIN      1
#define STEP_MAX      5
#define VEL_SLOW_DPS  6.0f
#define VEL_FAST_DPS  30.0f
#define VEL_IDLE_US   220000
#define VEL_EMA       0.35f

static int64_t s_last_turn_us;
static float   s_dps;

static int current_step(void)
{
    if (s_dps <= VEL_SLOW_DPS) return STEP_MIN;
    if (s_dps >= VEL_FAST_DPS) return STEP_MAX;
    const float t = (s_dps - VEL_SLOW_DPS) / (VEL_FAST_DPS - VEL_SLOW_DPS);
    int m = STEP_MIN + (int)(t * (STEP_MAX - STEP_MIN) + 0.5f);
    if (m < STEP_MIN) m = STEP_MIN;
    if (m > STEP_MAX) m = STEP_MAX;
    return m;
}

static int update_velocity(int32_t raw_delta)
{
    const int64_t now = orb_uptime_us();
    if (s_last_turn_us == 0 || now - s_last_turn_us > VEL_IDLE_US) {
        s_dps = 0.0f;
    } else {
        const double dt_s = (double)(now - s_last_turn_us) / 1e6;
        if (dt_s > 0.0) {
            const float inst = (float)(raw_delta < 0 ? -raw_delta : raw_delta) / (float)dt_s;
            s_dps += VEL_EMA * (inst - s_dps);
        }
    }
    s_last_turn_us = now;
    return current_step();
}

static void velocity_reset(void)
{
    s_last_turn_us = 0;
    s_dps = 0.0f;
}

/* ---- model ---------------------------------------------------------------------- */

typedef enum { CH_HUE = 0, CH_SAT, CH_VAL, CH_COUNT } channel_t;
static const char *const CH_LETTER[CH_COUNT] = { "H", "S", "B" };

static channel_t s_ch = CH_HUE;
static int32_t   s_hue = 210, s_sat = 80, s_val = 90;   /* the manifest's defaults */
static bool      s_armed, s_dirty;

/* The widget library's own HSV->RGB, integer for integer, so the colours match. */
static uint32_t hsv_rgb(uint32_t hue, uint32_t sat, uint32_t val)
{
    const uint8_t h = (uint8_t)(hue * 255u / 360u);
    const uint8_t s = (uint8_t)(sat * 255u / 100u);
    const uint8_t v = (uint8_t)(val * 255u / 100u);
    if (s == 0) return (uint32_t)v << 16 | (uint32_t)v << 8 | v;
    const uint8_t region = h / 43;
    const uint8_t rem = (uint8_t)((h - region * 43) * 6);
    const uint8_t p = (uint8_t)((v * (255 - s)) >> 8);
    const uint8_t q = (uint8_t)((v * (255 - ((s * rem) >> 8))) >> 8);
    const uint8_t t = (uint8_t)((v * (255 - ((s * (255 - rem)) >> 8))) >> 8);
    uint8_t r, g, b;
    switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

/* ---- view ----------------------------------------------------------------------- */

static int32_t s_swatch, s_chip[CH_COUNT], s_chip_lbl[CH_COUNT], s_value, s_hint;
static int32_t s_title, s_title_hint;

static void style_chip(channel_t c)
{
    if (c == s_ch) {
        orb_ui_set(s_chip[c], ORB_P_BG, hsv_rgb((uint32_t)s_hue, 70, 85));
        orb_ui_set(s_chip_lbl[c], ORB_P_TEXT_COLOR, 0xFFFFFF);
        orb_ui_set(s_chip[c], ORB_P_BORDER_COLOR, 0xFFFFFF);
        orb_ui_set(s_chip[c], ORB_P_BORDER_W, 2);
    } else {
        orb_ui_set(s_chip[c], ORB_P_BG, 0x181818);
        orb_ui_set(s_chip_lbl[c], ORB_P_TEXT_COLOR, 0x808080);
        orb_ui_set(s_chip[c], ORB_P_BORDER_W, 0);
    }
}

/* Exactly one of the two views: the editor is hidden rather than moved off-screen, so
 * the swatch and chips never bleed into the title card. */
static void set_views(bool armed)
{
    orb_ui_set(s_title, ORB_P_HIDDEN, armed);
    orb_ui_set(s_title_hint, ORB_P_HIDDEN, armed);
    orb_ui_set(s_swatch, ORB_P_HIDDEN, !armed);
    orb_ui_set(s_value, ORB_P_HIDDEN, !armed);
    orb_ui_set(s_hint, ORB_P_HIDDEN, !armed);
    for (int i = 0; i < CH_COUNT; i++) orb_ui_set(s_chip[i], ORB_P_HIDDEN, !armed);
}

static void refresh(void)
{
    if (!s_armed) {
        set_views(false);
        return;
    }
    set_views(true);
    orb_ui_set(s_swatch, ORB_P_BG, hsv_rgb((uint32_t)s_hue, (uint32_t)s_sat, (uint32_t)s_val));
    char buf[24];
    if (s_ch == CH_HUE)      snprintf(buf, sizeof buf, "HUE  %d", (int)s_hue);
    else if (s_ch == CH_SAT) snprintf(buf, sizeof buf, "SAT  %d%%", (int)s_sat);
    else                     snprintf(buf, sizeof buf, "BRI  %d%%", (int)s_val);
    orb_ui_text(s_value, buf);
    for (int i = 0; i < CH_COUNT; i++) style_chip((channel_t)i);
}

static int32_t clamp100(int32_t v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

static void adjust(int32_t delta, int step)
{
    if (!delta) return;
    switch (s_ch) {
    case CH_HUE: s_hue = ((s_hue + delta * step % 360) % 360 + 360) % 360; break;
    case CH_SAT: s_sat = clamp100(s_sat + delta * step); break;
    case CH_VAL: s_val = clamp100(s_val + delta * step); break;
    default: break;
    }
    s_dirty = true;
    refresh();
}

static void persist(void)
{
    if (!s_dirty) return;
    orb_cfg_set("hue", s_hue);
    orb_cfg_set("sat", s_sat);
    orb_cfg_set("bri", s_val);
    s_dirty = false;
}

static int32_t label(int32_t parent, int32_t font, uint32_t rgb, const char *text)
{
    const int32_t h = orb_ui_create(ORB_UI_LABEL, parent);
    orb_ui_set(h, ORB_P_TEXT_COLOR, (int32_t)rgb);
    orb_ui_set(h, ORB_P_FONT, font);
    orb_ui_text(h, text);
    return h;
}

/* ---- entry points --------------------------------------------------------------- */

ORB_EXPORT("orb_init") int32_t orb_init(void)
{
    (void)ORB_ICON();                    /* §61: the carousel's picture of this app */
    int32_t v;
    if (orb_cfg_get("hue", &v) == ORB_OK) s_hue = v;
    if (orb_cfg_get("sat", &v) == ORB_OK) s_sat = v;
    if (orb_cfg_get("bri", &v) == ORB_OK) s_val = v;

    /* The big circular swatch, upper centre of the disc. */
    s_swatch = orb_ui_create(ORB_UI_BOX, 0);
    if (!s_swatch) return ORB_ERR_NO_MEM;
    orb_ui_set(s_swatch, ORB_P_W, 170);
    orb_ui_set(s_swatch, ORB_P_H, 170);
    orb_ui_set(s_swatch, ORB_P_RADIUS, ORB_RADIUS_CIRCLE);
    orb_ui_set(s_swatch, ORB_P_BORDER_W, 2);
    orb_ui_set(s_swatch, ORB_P_BORDER_COLOR, 0x333333);
    orb_ui_align(s_swatch, ORB_AL_TOP_MID, 0, 40);

    /* Three option chips, centred as a row below the swatch. */
    const int chip_w = 70, chip_h = 40, gap = 14;
    const int total = CH_COUNT * chip_w + (CH_COUNT - 1) * gap;
    const int row_left = (ORB_SCREEN_W - total) / 2;
    for (int i = 0; i < CH_COUNT; i++) {
        s_chip[i] = orb_ui_create(ORB_UI_BOX, 0);
        orb_ui_set(s_chip[i], ORB_P_W, chip_w);
        orb_ui_set(s_chip[i], ORB_P_H, chip_h);
        orb_ui_set(s_chip[i], ORB_P_RADIUS, 10);
        orb_ui_set(s_chip[i], ORB_P_X, row_left + i * (chip_w + gap));
        orb_ui_set(s_chip[i], ORB_P_Y, 230);
        s_chip_lbl[i] = label(s_chip[i], ORB_FONT_14, 0x808080, CH_LETTER[i]);
        orb_ui_align(s_chip_lbl[i], ORB_AL_CENTER, 0, 0);
    }

    s_value = label(0, ORB_FONT_48, 0xFFFFFF, "HUE  210");
    orb_ui_align(s_value, ORB_AL_TOP_MID, 0, 290);
    s_hint = label(0, ORB_FONT_14, 0x909090, "rotate: adjust   push: option");
    orb_ui_align(s_hint, ORB_AL_BOTTOM_MID, 0, -30);

    /* The unarmed title card, swapped for the editor by set_views(). */
    s_title = label(0, ORB_FONT_48, 0xFFFFFF, "HSB");
    orb_ui_align(s_title, ORB_AL_CENTER, 0, -20);
    s_title_hint = label(0, ORB_FONT_14, 0x909090, "push to edit");
    orb_ui_align(s_title_hint, ORB_AL_CENTER, 0, 40);

    s_armed = false;
    refresh();
    orb_logf("color picker ready (HUE=%d SAT=%d BRI=%d)", (int)s_hue, (int)s_sat, (int)s_val);
    return ORB_OK;
}

ORB_EXPORT("orb_show") void orb_show(void)
{
    velocity_reset();
    refresh();
    orb_ui_refresh();
}

ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    if (!s_armed) return;
    if (pressed) {
        const channel_t before = s_ch;
        s_ch = (channel_t)((s_ch + 1) % CH_COUNT);
        refresh();
        velocity_reset();          /* a push is a selection, not a turn */
        orb_logf("push %s -> %s", CH_LETTER[before], CH_LETTER[s_ch]);
    } else if (delta) {
        /* The raw delta, not a list step: a value is not a list position, and
         * clockwise raises it. */
        const int step = update_velocity(delta);
        adjust(delta, step);
        orb_logf("turn %+d x%d -> %s = %d", (int)delta, step, CH_LETTER[s_ch],
                 (int)(s_ch == CH_HUE ? s_hue : s_ch == CH_SAT ? s_sat : s_val));
    }
}

ORB_EXPORT("orb_set_armed") int32_t orb_set_armed(int32_t armed)
{
    if (s_armed == (armed != 0)) return s_armed;
    if (!armed && s_armed) persist();        /* the commit edge */
    s_armed = armed != 0;
    velocity_reset();
    refresh();
    orb_ui_refresh();
    orb_log(s_armed ? "editor open" : "title card");
    return s_armed;
}
