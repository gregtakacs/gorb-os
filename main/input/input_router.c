/**
 * input_router.c — knob-to-screen routing (see header for the contract).
 *
 * Two levels, one knob. Roaming, a turn swaps screens and a press arms the one
 * you are on; armed, the screen owns the turn and a long-press comes back. The
 * router decides which level applies by asking the screen whether it is armed —
 * the menu and the picker each own that bit of state, because only they know
 * what their own armed view looks like.
 */

#include "input_router.h"
#include "knob.h"
#include "idle_dim.h"
#include "clock_view.h"
#include "color_picker.h"
#include "menu_view.h"

#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "display.h"        /* display_bench_snapshot(): the M1 switch probe's frame counters */
#include "lvgl.h"

static const char *TAG = "input";

#define POLL_PERIOD_MS 10   /* knob sampling cadence; well under a human detent */

static input_screen_t s_current = INPUT_SCREEN_CLOCK;
static lv_timer_t    *s_poll_timer;

/* Screen requested from another task (GET /api/v1/screen). Held as an int and
 * 32-bit wide so the write is atomic against the LVGL task that reads it;
 * INPUT_SCREEN_COUNT means "nothing pending", so a torn read cannot land on a
 * real screen. */
static volatile int s_requested = INPUT_SCREEN_COUNT;

/* Synthetic knob events queued by input_router_inject() -- see the header for
 * why. Merged into the real detent/press streams below rather than handled
 * separately, so an injected gesture travels the identical code path a finger
 * produces. */
static volatile int32_t s_inj_delta;
static volatile bool  s_inj_press;
static volatile bool  s_inj_long;   /* the escape-hatch gesture, see input_router_inject */

static const char *screen_name(input_screen_t s)
{
    switch (s) {
        case INPUT_SCREEN_MENU:  return "menu";
        case INPUT_SCREEN_CLOCK: return "clock";
        default:                 return "color";
    }
}

/* Does the visible screen currently want the turn for itself? */
static bool screen_armed(input_screen_t s)
{
    switch (s) {
        case INPUT_SCREEN_MENU:  return menu_view_armed();
        case INPUT_SCREEN_COLOR: return color_picker_armed();
        default:                 return false;   /* the clock has nothing to enter */
    }
}

/* Both screens with an armed view are disarmed together, so a screen can never
 * be left armed off-screen and then steal the next turn from the one showing. */
static void disarm_all(void)
{
    menu_view_set_armed(false);
    color_picker_set_armed(false);
}

/* The deferred half of the M1 probe below. Every path that opens a window runs on
 * the LVGL task — the poll timer's own switch handling and menu_view's exit — except
 * boot, which opens one before the timer exists. So it needs no lock, and a zero
 * s_sw_open_us means no window is open. */
static int64_t     s_sw_open_us;
static uint32_t    s_sw_frames;
static uint64_t    s_sw_us;
static const char *s_sw_name;

/* If the new view has not painted inside this, that is itself the finding, and it
 * is reported as one instead of waiting indefinitely for a commit that never
 * comes. Comfortably longer than the 40 ms beat the fastest theme bakes. */
#define SW_WINDOW_MS 1500

/* Close the window set_screen() opened, once a real commit has landed since. */
static void switch_window_close(int64_t now)
{
    uint32_t f;
    uint64_t u;
    uint32_t mx;
    display_bench_snapshot(&f, &u, &mx);
    if (f == s_sw_frames) {
        if (now - s_sw_open_us > (int64_t)SW_WINDOW_MS * 1000) {
            ESP_LOGW(TAG, "switch %s: no display commit within %d ms — the new view never painted",
                     s_sw_name, SW_WINDOW_MS);
            s_sw_open_us = 0;
        }
        return;
    }
    /* `avg` is the cost of the commit(s) this switch provoked. The worst figure is
     * display's all-time high-water mark, not a window delta — there is no honest
     * per-window maximum to take from a monotonic max — so it says so in the label. */
    ESP_LOGI(TAG, "switch %s -> glass +%lld ms: %u commit(s), avg %llu us (all-time worst flush %llu us)",
             s_sw_name, (long long)((now - s_sw_open_us) / 1000),
             (unsigned)(f - s_sw_frames),
             (unsigned long long)((u - s_sw_us) / (f - s_sw_frames)),
             (unsigned long long)mx);
    s_sw_open_us = 0;
}

static void set_screen(input_screen_t s, bool load)
{
    if (s >= INPUT_SCREEN_COUNT) return;
    s_current = s;

    /* M1 of the app platform plan (docs/app-platform-plan.md §8): what it costs to
     * point the compositor at a different already-resident view decides whether app
     * switching ever needs a reboot (D5). This is the only such switch the firmware
     * has today, so it is timed here and the app host is judged against the number.
     *
     * Two halves, because the switch is not the wait. `sync` is the pointer swap
     * plus the widget rebuild, logged at the end of this function; the commit that
     * puts the view on the glass is asynchronous, so the frame window opened here
     * closes in switch_window_close(). Sampling the counters at switch time, as this
     * probe first did, measured nothing: at a 250 ms tempo a 1 ms window holds no
     * commit, so it printed `frames 0`, and it printed display's all-time flush
     * maximum as a bare `worst` on every line — reading like a 303 ms switch. */
    const int64_t sw_t0 = esp_timer_get_time();
    display_bench_snapshot(&s_sw_frames, &s_sw_us, NULL);
    s_sw_open_us = sw_t0;
    s_sw_name    = screen_name(s);

    /* Only the visible screen's animation should run: the GIF + hand beat burns
     * flush work on every tick, so park it when the clock is not on the glass. */
    clock_view_set_bounce(s == INPUT_SCREEN_CLOCK);

    if (load) {
        lv_obj_t *scr = (s == INPUT_SCREEN_CLOCK) ? clock_view_screen()
                  : (s == INPUT_SCREEN_MENU)      ? menu_view_screen()
                                                  : color_picker_screen();
        if (scr) lv_screen_load(scr);
        /* Re-push this screen's model to its widgets and force a clean repaint.
         * lv_screen_load() already invalidates, but the picker and the menu keep
         * their values in module statics (not in the widget tree), so we re-sync
         * explicitly — this is what makes the chosen color survive a trip to the
         * clock and back instead of reverting to whatever the last frame showed. */
        if (s == INPUT_SCREEN_COLOR)     color_picker_show();
        else if (s == INPUT_SCREEN_MENU) menu_view_show();
    }
    /* The deferred half logs from poll_timer_cb once the view is actually painted. */
    ESP_LOGI(TAG, "switch %s sync %lld ms", screen_name(s),
             (long long)((esp_timer_get_time() - sw_t0) / 1000));
    ESP_LOGI(TAG, "screen -> %s", screen_name(s));
}

/* Arm the visible screen, if it has an armed view at all. */
static void arm_current(void)
{
    switch (s_current) {
        case INPUT_SCREEN_MENU:  menu_view_set_armed(true);  break;
        case INPUT_SCREEN_COLOR: color_picker_set_armed(true); break;
        default:
            /* The clock is inert by design: it has no list, so arming it would
             * swallow a press into nothing and leave the user holding a knob
             * that appears not to work. */
            return;
    }
    ESP_LOGI(TAG, "armed %s", screen_name(s_current));
}

/* Runs on the LVGL task (created via lv_timer), so widget calls are legal here. */
static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* First, and before the knob work can skew the timestamp: has the view this
     * task last switched to actually reached the glass yet? */
    if (s_sw_open_us) switch_window_close(esp_timer_get_time());
    knob_poll();

    int32_t delta   = knob_take_delta();
    bool    pressed = knob_take_press();
    bool lpress = knob_take_long_press();

    /* Fold in anything injected. Draining the whole pending delta keeps the
     * injected path honest: it feeds the same stream a real turn feeds. The three
     * flags are latched first, because draining them here is what would otherwise
     * erase the only record of whether the event came from the bench or from the
     * encoder itself — see the touch report just below. */
    const int32_t inj = s_inj_delta;
    const bool    ip  = s_inj_press;
    const bool    il  = s_inj_long;
    if (inj) { s_inj_delta = 0; delta += inj; }
    if (s_inj_press) { s_inj_press = false; pressed = true; }
    if (s_inj_long)   { s_inj_long   = false; lpress   = true;  }

    /* Any knob event is a touch, and the idle dim is told about it here — the
     * one place every knob event, real or injected, is already gathered. After
     * the injection fold on purpose: a turn driven over HTTP wakes the dial just
     * as a hand does, which is what keeps the bench able to test the wake path
     * without a finger on the encoder.
     *
     * The source is named rather than left implicit: folded into one `delta != 0`,
     * a bench turn over HTTP and an encoder that bounced by itself are
     * indistinguishable, and telling "nobody touched it" from "the bench touched
     * it" is the whole question when the dial refuses to dim. */
    if (delta || pressed || lpress)
        idle_dim_activity(inj || ip || il
                          ? "http"
                          : (delta ? (pressed ? "turn+press" : "turn")
                                    : (pressed ? "press" : "hold")));

    /* A screen requested over HTTP is applied here, on the LVGL task, before any
     * knob event is considered: lv_screen_load() off this task is the widget
     * race every other rule in this file exists to avoid. */
    const int req = s_requested;
    if (req != INPUT_SCREEN_COUNT) {
        s_requested = INPUT_SCREEN_COUNT;
        if (req < INPUT_SCREEN_COUNT) {
            disarm_all();
            set_screen((input_screen_t)req, true);
            return;                      /* this poll is spent on the switch */
        }
    }

    /* The menu's card walk is reaped here even while another screen is visible:
     * a scan that finishes while the user has rotated to the clock must still
     * land, or the list would come back showing a stale answer. */
    menu_view_service();

    /* Long-press is the escape hatch out of every armed state, and it swallows
     * everything that made it so a hold never also lands as a turn/press on the
     * screen underneath. It deliberately no longer cycles screens: rotate owns
     * that gesture now. */
    if (lpress) {
        disarm_all();
        set_screen(INPUT_SCREEN_CLOCK, true);
        return;
    }

    /* Roaming: the turn belongs to the router, not the screen. */
    if (!screen_armed(s_current)) {
        if (pressed) { arm_current(); return; }
        if (delta) {
            disarm_all();
            /* One step per poll in the direction turned. A fast burst reports
             * several detents and must not fly past three screens and stop
             * somewhere nobody aimed at. */
            /* Screens sit side by side, so the carousel is the horizontal axis and
             * asks the knob layer what "right" means, like any other screen would. */
            const int step = (knob_col_step(delta) > 0) ? 1
                                                        : (INPUT_SCREEN_COUNT - 1);
            set_screen((input_screen_t)((s_current + step) % INPUT_SCREEN_COUNT), true);
        }
        return;
    }

    switch (s_current) {
        case INPUT_SCREEN_MENU:
            menu_view_apply_input(delta, pressed);
            break;
        case INPUT_SCREEN_COLOR:
            color_picker_apply_input(delta, pressed);
            break;
        case INPUT_SCREEN_CLOCK:
            /* Unreachable: screen_armed() never reports the clock armed. */
            break;
        default: break;
    }
}

esp_err_t input_router_init(void)
{
    /* Boot on the clock: with the GIF delta bank baked it is the live face of
     * the device (animating background + ticking hands), and it is the state a
     * device in a sealed case has to fall back to when nothing else works. Turn
     * the knob to reach the menu and the hue editor; hold it to come back. */
    disarm_all();
    set_screen(INPUT_SCREEN_CLOCK, /*load=*/true);

    s_poll_timer = lv_timer_create(poll_timer_cb, POLL_PERIOD_MS, NULL);
    if (!s_poll_timer) return ESP_FAIL;
    ESP_LOGI(TAG, "router running (poll %d ms, %d screens)",
             POLL_PERIOD_MS, (int)INPUT_SCREEN_COUNT);
    return ESP_OK;
}

void input_router_show(input_screen_t s)
{
    disarm_all();
    set_screen(s, true);
}

void input_router_home(void)
{
    disarm_all();
    set_screen(INPUT_SCREEN_CLOCK, true);
}

input_screen_t input_router_current(void)
{
    return s_current;
}

bool input_router_armed(void)
{
    return screen_armed(s_current);
}

void input_router_request(input_screen_t s)
{
    if (s >= INPUT_SCREEN_COUNT) return;
    s_requested = (int)s;
}

void input_router_inject(int32_t delta, bool pressed, bool lpress)
{
    /* Saturating add rather than assign: several turns asked for in one burst
     * are several rows of movement, and keeping only the last would make a long
     * list unreachable from the bench. */
    if (delta > 0 && s_inj_delta > (INT32_MAX - delta)) s_inj_delta = INT32_MAX;
    else s_inj_delta += delta;
    if (pressed) s_inj_press = true;
    if (lpress)  s_inj_long  = true;
}
