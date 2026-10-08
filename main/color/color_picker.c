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

#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "color";

/* Per-detent adjustment for each channel. Hue wraps, s/v clamp. */
#define HUE_STEP  5
#define SAT_STEP  5
#define VAL_STEP  5

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

static void refresh(void)
{
    lv_color_t c = lv_color_hsv_to_rgb(s_hue, s_sat, s_val);
    lv_obj_set_style_bg_color(s_swatch, c, 0);

    char buf[24];
    if (s_ch == CH_HUE)      snprintf(buf, sizeof(buf), "HUE  %u",   (unsigned)s_hue);
    else if (s_ch == CH_SAT) snprintf(buf, sizeof(buf), "SAT  %u%%", (unsigned)s_sat);
    else                     snprintf(buf, sizeof(buf), "BRI  %u%%", (unsigned)s_val);
    lv_label_set_text(s_value_lbl, buf);

    for (int i = 0; i < CH_COUNT; i++) style_chip((channel_t)i);
}

static void adjust(int32_t delta)
{
    if (delta == 0) return;
    switch (s_ch) {
        case CH_HUE:
            s_hue = (uint16_t)(((int32_t)s_hue + delta * HUE_STEP % 360 + 360) % 360);
            break;
        case CH_SAT: {
            int v = (int)s_sat + delta * SAT_STEP;
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            s_sat = (uint8_t)v;
            break;
        }
        case CH_VAL: {
            int v = (int)s_val + delta * VAL_STEP;
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            s_val = (uint8_t)v;
            break;
        }
        default: break;
    }
    refresh();
}

static void cycle_channel(void)
{
    s_ch = (channel_t)((s_ch + 1) % CH_COUNT);
    refresh();
}

/* ---- Public API ---- */
esp_err_t color_picker_init(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_scrollable(s_scr, false);

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

    refresh();
    ESP_LOGI(TAG, "color picker ready (HUE=%u SAT=%u BRI=%u)",
             (unsigned)s_hue, (unsigned)s_sat, (unsigned)s_val);
    return ESP_OK;
}

lv_obj_t *color_picker_screen(void) { return s_scr; }

void color_picker_apply_input(int32_t delta, bool pressed)
{
    if (pressed) {
        const channel_t before = s_ch;
        cycle_channel();
        ESP_LOGI(TAG, "push %s -> %s", CH_LETTER[before], CH_LETTER[s_ch]);
    } else if (delta) {
        adjust(delta);
        uint16_t h; uint8_t s, v; color_picker_get(&h, &s, &v);
        ESP_LOGI(TAG, "turn %+ld -> %s = %u", (long)delta,
                 CH_LETTER[s_ch], (unsigned)(s_ch == CH_HUE ? h : (s_ch == CH_SAT ? s : v)));
    }
}

void color_picker_get(uint16_t *h, uint8_t *s, uint8_t *v)
{
    if (h) *h = s_hue;
    if (s) *s = s_sat;
    if (v) *v = s_val;
}
