/**
 * input_router.c — knob-to-screen routing (see header for the contract).
 */

#include "input_router.h"
#include "knob.h"
#include "clock_view.h"
#include "color_picker.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "input";

#define POLL_PERIOD_MS 10   /* knob sampling cadence; well under a human detent */

static input_screen_t s_current = INPUT_SCREEN_COLOR;
static lv_timer_t    *s_poll_timer;

static void set_screen(input_screen_t s, bool load)
{
    if (s >= INPUT_SCREEN_COUNT) return;
    s_current = s;

    /* Only the visible screen's animation should run: bounce burns flush work on
     * every tick, so park it when the clock is not on the glass. */
    clock_view_set_bounce(s == INPUT_SCREEN_CLOCK);

    if (load) {
        lv_obj_t *scr = (s == INPUT_SCREEN_CLOCK) ? clock_view_screen()
                                                   : color_picker_screen();
        if (scr) lv_screen_load(scr);
    }
    ESP_LOGI(TAG, "screen -> %s", s == INPUT_SCREEN_CLOCK ? "clock" : "color");
}

/* Runs on the LVGL task (created via lv_timer), so widget calls are legal here. */
static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    knob_poll();

    const int32_t delta   = knob_take_delta();
    const bool    pressed = knob_take_press();
    const bool    lpress  = knob_take_long_press();

    /* Long-press owns the switch gesture and swallows everything that made it, so
     * a hold never also lands as a turn/press on the screen underneath. */
    if (lpress) {
        set_screen((input_screen_t)((s_current + 1) % INPUT_SCREEN_COUNT), true);
        return;
    }

    switch (s_current) {
        case INPUT_SCREEN_COLOR:
            color_picker_apply_input(delta, pressed);
            break;
        case INPUT_SCREEN_CLOCK:
            /* Clock turn/press unclaimed in this MVP: the bounce stress-test is
             * already the display-path validation, and the color picker is the
             * input-path one. */
            break;
        default: break;
    }
}

esp_err_t input_router_init(void)
{
    set_screen(INPUT_SCREEN_COLOR, /*load=*/true);
    s_poll_timer = lv_timer_create(poll_timer_cb, POLL_PERIOD_MS, NULL);
    if (!s_poll_timer) return ESP_FAIL;
    ESP_LOGI(TAG, "router running (poll %d ms)", POLL_PERIOD_MS);
    return ESP_OK;
}

void input_router_show(input_screen_t s)
{
    set_screen(s, true);
}

input_screen_t input_router_current(void)
{
    return s_current;
}
