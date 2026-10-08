/**
 * clock_view.c — digital text clock, refreshed every second, with an optional
 * "bounce" mode that drifts the label around the round AMOLED.
 *
 * Both the 1 s text tick and the ~30 FPS motion tick are LVGL timers, so they
 * run inside lv_timer_handler() on the LVGL task — the only context that is
 * allowed to touch widgets. No FreeRTOS timers, no cross-thread LVGL calls.
 */

#include "clock_view.h"
#include "board_config.h"

#include <stdio.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "clock";

static lv_obj_t   *s_clock_label;
static lv_obj_t   *s_scr;              /* the clock's own screen (switchable) */
static lv_timer_t *s_tick_timer;
static lv_timer_t *s_motion_timer;

/* Free-running time base: starts at a fixed epoch, ticks every second.
 * Replace with SNTP sync when WiFi is added. */
#define CLOCK_EPOCH 1700000000  /* Nov 2023 baseline */
static uint32_t s_elapsed_s = 0;

/* ---- Bounce state (all in logical screen pixels) ---- */
#define BOUNCE_PERIOD_MS  33        /* ~30 FPS, matches LV_DEF_REFR_PERIOD */
#define BOUNCE_SPEED_PPS  120.0f    /* px/sec along each axis at spawn     */

static bool     s_bounce_on = false;
/* Label's own bounding box, used as the bouncing body half-extents. */
static int32_t  s_body_w, s_body_h;
static float    s_pos_x, s_pos_y;   /* top-left corner of the label */
static float    s_vel_x, s_vel_y;   /* px per motion tick           */

/* Runs in the LVGL task context — safe to touch widgets directly. */
static void tick_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_elapsed_s++;

    time_t t = (time_t)CLOCK_EPOCH + (time_t)s_elapsed_s;
    struct tm tm_out;
    localtime_r(&t, &tm_out);

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             tm_out.tm_hour, tm_out.tm_min, tm_out.tm_sec);

    lv_label_set_text(s_clock_label, buf);
}

/* ---- Bounce physics (LVGL task context) ----
 *
 * The visible area is a disc of radius R centred at (cx, cy). We treat the
 * label as an axis-aligned box and keep its four corners inside the disc.
 * When the leading corner crosses the circle, we reflect the velocity across
 * the circle normal (n = (corner - centre)/|..|):  v' = v - 2(v.n)n.
 */
static void bounce_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* advance */
    s_pos_x += s_vel_x;
    s_pos_y += s_vel_y;

    const float cx = SCREEN_WIDTH  / 2.0f;
    const float cy = SCREEN_HEIGHT / 2.0f;
    const float R  = (SCREEN_WIDTH / 2.0f) - 2.0f;   /* keep off the very bezel */

    /* Corners of the label box (screen coords), top-left = (s_pos_x, s_pos_y). */
    const float x_left  = s_pos_x;
    const float x_right = s_pos_x + s_body_w;
    const float y_top   = s_pos_y;
    const float y_bot   = s_pos_y + s_body_h;

    /* Find the single corner that is deepest outside the circle. */
    float worst_pen = 0.0f;
    float nx = 0.0f, ny = 0.0f;
    const float corners_x[4] = { x_left, x_right, x_left,  x_right };
    const float corners_y[4] = { y_top,  y_top,   y_bot,   y_bot  };

    for (int i = 0; i < 4; i++) {
        float dx = corners_x[i] - cx;
        float dy = corners_y[i] - cy;
        float d  = sqrtf(dx * dx + dy * dy);
        if (d > R) {
            float pen = d - R;
            if (pen > worst_pen) {
                worst_pen = pen;
                nx = dx / d;               /* outward normal at that corner */
                ny = dy / d;
            }
        }
    }

    if (worst_pen > 0.0f) {
        /* push the box back inside along the normal */
        s_pos_x -= nx * worst_pen;
        s_pos_y -= ny * worst_pen;

        /* reflect velocity across the normal if moving outward */
        float vdotn = s_vel_x * nx + s_vel_y * ny;
        if (vdotn > 0.0f) {
            s_vel_x -= 2.0f * vdotn * nx;
            s_vel_y -= 2.0f * vdotn * ny;
        }
    }

    /* Commit position. LVGL positions are integers; snap to even columns so the
     * QSPI flush stays on the fast (no-repack) path — see lvgl_flush_cb. */
    int32_t px = (int32_t)(s_pos_x + 0.5f) & ~1;
    int32_t py = (int32_t)(s_pos_y + 0.5f) & ~1;
    lv_obj_set_x(s_clock_label, px);
    lv_obj_set_y(s_clock_label, py);
}

static void start_motion_timer(void)
{
    if (!s_motion_timer) {
        s_motion_timer = lv_timer_create(bounce_timer_cb, BOUNCE_PERIOD_MS, NULL);
    }
    lv_timer_resume(s_motion_timer);
}

static void stop_motion_timer(void)
{
    if (s_motion_timer) {
        lv_timer_pause(s_motion_timer);
    }
}

esp_err_t clock_view_init(void)
{
    /* Own screen (created but not loaded): input_router decides which screen is
     * visible, so the clock and the color picker never draw over each other. */
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;

    /* True-black background for AMOLED */
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Remove default screen padding so the label can be positioned freely */
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    /* Create the time label. No alignment: we drive x/y ourselves so the label
     * never gets re-centred by a layout pass. Give it a size that comfortably
     * fits "88:88:88" at Montserrat-48 so it can bounce as a rigid box. */
    s_clock_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_clock_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_pad_all(s_clock_label, 0, 0);
    lv_label_set_text(s_clock_label, "88:88:88");

    /* Ask LVGL how big that text actually is, then adopt those as the box. */
    lv_obj_update_layout(s_clock_label);
    s_body_w = lv_obj_get_width(s_clock_label);
    s_body_h = lv_obj_get_height(s_clock_label);
    if (s_body_w <= 0) s_body_w = 300;
    if (s_body_h <= 0) s_body_h = 55;

    /* Start centred on the disc. */
    s_pos_x = (SCREEN_WIDTH  - s_body_w) / 2.0f;
    s_pos_y = (SCREEN_HEIGHT - s_body_h) / 2.0f;

    /* Spawn velocity along a fixed diagonal (px per motion tick). */
    const float step = BOUNCE_SPEED_PPS * (BOUNCE_PERIOD_MS / 1000.0f);
    s_vel_x =  step * 0.8f;
    s_vel_y =  step * 0.5f;

    lv_obj_set_x(s_clock_label, (int32_t)s_pos_x);
    lv_obj_set_y(s_clock_label, (int32_t)s_pos_y);

    /* Render the first value immediately */
    tick_timer_cb(NULL);

    /* 1-second LVGL timer (dispatched from lv_timer_handler) */
    s_tick_timer = lv_timer_create(tick_timer_cb, 1000, NULL);

    ESP_LOGI(TAG, "Clock view initialized (body %ldx%ld px)",
             (long)s_body_w, (long)s_body_h);
    return ESP_OK;
}

void clock_view_set_bounce(bool on)
{
    s_bounce_on = on;
    if (on) {
        /* Re-centre and re-spawn so every enable starts from the middle. */
        s_pos_x = (SCREEN_WIDTH  - s_body_w) / 2.0f;
        s_pos_y = (SCREEN_HEIGHT - s_body_h) / 2.0f;
        const float step = BOUNCE_SPEED_PPS * (BOUNCE_PERIOD_MS / 1000.0f);
        s_vel_x = step * 0.8f;
        s_vel_y = step * 0.5f;
        lv_obj_set_x(s_clock_label, (int32_t)(s_pos_x + 0.5f) & ~1);
        lv_obj_set_y(s_clock_label, (int32_t)(s_pos_y + 0.5f) & ~1);
        start_motion_timer();
        ESP_LOGI(TAG, "Bounce ON  (%.1f px/s, period %d ms)",
                 (double)BOUNCE_SPEED_PPS, BOUNCE_PERIOD_MS);
    } else {
        stop_motion_timer();
        lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);
        ESP_LOGI(TAG, "Bounce OFF (clock re-centred)");
    }
}

bool clock_view_bounce(void)
{
    return s_bounce_on;
}

lv_obj_t *clock_view_screen(void)
{
    return s_scr;
}
