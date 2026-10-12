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
#include "orb_app.h"
#include "orb_carousel.h"   /* the peek: a turn names the next stop, it does not switch */

#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "display.h"        /* display_bench_snapshot(): the M1 switch probe's frame counters */
#include "lvgl.h"

static const char *TAG = "input";

#define POLL_PERIOD_MS 10   /* knob sampling cadence; well under a human detent */

/* The entry on the glass. A pointer into the registry's table rather than an
 * index, because the table can gain an entry (or lose one to a failed init)
 * without this file being rebuilt, and an index burned into a log line is a
 * claim about position nobody can check. NULL until the first switch. */
static const orb_app_t *s_current;
static lv_timer_t      *s_poll_timer;

/* Screen requested from another task (GET /api/v1/screen). Held as a single
 * 32-bit pointer so the write is atomic against the LVGL task that reads it;
 * NULL means "nothing pending", and the registry's table is const-address
 * stable, so a posted pointer cannot go stale under the poll. */
static volatile const orb_app_t *s_requested;

/* Synthetic knob events queued by input_router_inject() -- see the header for
 * why. Merged into the real detent/press streams below rather than handled
 * separately, so an injected gesture travels the identical code path a finger
 * produces. */
static volatile int32_t s_inj_delta;
static volatile bool  s_inj_press;
static volatile bool  s_inj_long;   /* the escape-hatch gesture, see input_router_inject */

static const char *screen_name(const orb_app_t *app)
{
    /* The slug, not a switch statement: what the log calls a screen is what
     * the wire calls it, so a `screen -> hue` line and a `?to=hue` request
     * cannot drift apart the way the enum and the URL names did. */
    return app ? app->slug : "none";
}

/* Does the visible entry currently want the turn for itself? The registry
 * asks the app's own armed hook, so a new app needs no edit here — which is
 * the whole point: the router used to name menu_view_armed() and
 * color_picker_armed() by hand and default the clock to false. */
static bool screen_armed(const orb_app_t *app)
{
    (void)app;
    return orb_app_current_armed();
}

/* Everything is disarmed together rather than only the entry leaving, so an
 * app can never be left armed off-screen and steal the next turn from the one
 * showing. The registry walks the table, so it covers apps the router has
 * never heard of. */
static void disarm_all(void)
{
    orb_app_disarm_all();
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

/* Put a registered entry on the glass, and open the M1 measurement window
 * around it. Returns the registry's refusal, if any, with the dial left exactly
 * where it was: a switch that failed has not switched, and reporting one as a
 * new screen is how a broken app turns into a misleading log. */
static esp_err_t set_screen(const orb_app_t *app, bool load)
{
    if (!app) return ESP_ERR_INVALID_ARG;
    const orb_app_t *previous = s_current;
    s_current = app;

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
    s_sw_name    = screen_name(app);

    /* Everything the old switch statement did by hand — park the outgoing
     * tick, claim the incoming app's secondary art bank, lv_screen_load(),
     * then re-push the model into widgets the load just invalidated — is
     * orb_app_activate()'s job now, in that order. The picker and the menu
     * keep their values in module statics rather than in the widget tree, so
     * the re-sync inside activate is still what makes the chosen color survive
     * a trip to the clock and back. */
    if (load) {
        const esp_err_t err = orb_app_activate(app);
        if (err != ESP_OK) {
            s_current = previous;
            s_sw_open_us = 0;      /* no window: nothing was switched, nothing to wait for */
            ESP_LOGE(TAG, "switch to %s refused (%s) — staying on %s",
                     screen_name(app), esp_err_to_name(err), screen_name(previous));
            return err;
        }
    }
    /* The deferred half logs from poll_timer_cb once the view is actually painted. */
    ESP_LOGI(TAG, "switch %s sync %lld ms", screen_name(app),
             (long long)((esp_timer_get_time() - sw_t0) / 1000));
    ESP_LOGI(TAG, "screen -> %s", screen_name(app));
    return ESP_OK;
}

/* Arm the visible entry, if it has an armed view at all. */
static void arm_current(void)
{
    /* The registry says whether arming took effect: the dial has no armed view,
     * so a press there stays inert by design rather than being swallowed into
     * nothing and leaving the user holding a knob that appears not to work. */
    if (!orb_app_arm_current()) return;
    ESP_LOGI(TAG, "armed %s", screen_name(s_current));
}

/* Runs on the LVGL task (created via lv_timer), so widget calls are legal here. */
static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* One timestamp for the whole poll, read before any knob work: the M1 frame
     * window below, the carousel's settle window, and the injection fold all
     * compare against the same instant, so a poll that spent time in knob_poll()
     * cannot make a peek look older or newer than the turn that opened it. */
    const int64_t now = esp_timer_get_time();
    /* First, and before the knob work can skew the timestamp: has the view this
     * task last switched to actually reached the glass yet? */
    if (s_sw_open_us) switch_window_close(now);
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
    const orb_app_t *req = (const orb_app_t *)s_requested;
    if (req) {
        s_requested = NULL;
        /* A requested screen beats a peek in flight: the user (or the bench)
         * named a destination, so a name still up from the last flick is wrong
         * and would otherwise commit itself over the top of what was asked for. */
        orb_carousel_cancel();
        disarm_all();
        set_screen(req, true);
        return;                          /* this poll is spent on the switch */
    }

    /* Every app's worker jobs are reaped here, visible or not: a card walk that
     * finishes while the user has rotated to the clock must still land, or the
     * list would come back showing a stale answer (C7). */
    orb_app_service_all();

    /* Long-press is the escape hatch out of every armed state, and it swallows
     * everything that made it so a hold never also lands as a turn/press on the
     * screen underneath. It deliberately no longer cycles screens: rotate owns
     * that gesture now.
     *
     * It also cancels any peek in flight: a name still on the glass after the
     * user has chosen to go home would commit itself over the top of the dial a
     * settle later. */
    if (lpress) {
        orb_carousel_cancel();
        disarm_all();
        set_screen(orb_app_home(), true);   /* C8: the dial, by table flag not by name */
        return;
    }

    /* Roaming: the turn belongs to the router, not the screen. */
    if (!screen_armed(s_current)) {
        if (pressed) {
            /* A press while a name is on the glass means "that one, now": it
             * commits the peek rather than arming the view that is still
             * physically showing underneath it. Arming the showing view would be
             * arming a screen the user just said they are leaving. */
            const orb_app_t *named = orb_carousel_pending();
            if (named) {
                orb_carousel_commit();
                /* The same press also ENTERS (owner ruling, 2026-10-12): rotate
                 * to name, press once, and you are inside the app — the armed
                 * state a press on an already-showing screen produces, so the
                 * HSB editor or the menu list is open the moment the switch
                 * lands. Armed only on a switch that landed: on a refused one
                 * set_screen leaves the old view on the glass, and arming that
                 * would arm a screen the user never named. Arming the dial is
                 * inert by design — no armed view exists beneath the clock. */
                if (set_screen(named, true) == ESP_OK) arm_current();
                return;
            }
            arm_current();
            return;
        }
        if (delta) {
            disarm_all();
            /* One step per poll in the direction turned. A fast burst reports
             * several detents and must not fly past every screen and stop
             * somewhere nobody aimed at. */
            /* Screens sit side by side, so the carousel is the horizontal axis and
             * asks the knob layer what "right" means, like any other screen would.
             * Which screens exist, and in what order, is the registry's answer —
             * (INPUT_SCREEN_COUNT - 1) as a fake minus-one is gone with the enum.
             *
             * A turn now NAMES the next stop instead of loading it (ruling C5's
             * carousel): the chip goes up over the view still on the glass, the
             * beat parks so the compositor cannot erase it, and the activate
             * happens on the poll after the detents stop. That is what makes a
             * three-detent flick cost one full-page commit instead of three, which
             * matters because the dial's own reveal measured 411-476 ms (§12.7). */
            const int step = knob_col_step(delta) > 0 ? 1 : -1;
            if (!orb_carousel_step(step, s_current, now) && !orb_carousel_ready()) {
                /* No chip was ever built (the heap said no at boot), so the peek
                 * has nothing to show — and a destination the user cannot see
                 * being chosen is worse than the immediate switch that was here
                 * before. Walk exactly as far as one poll used to walk. */
                const orb_app_t *next = orb_app_neighbour(s_current, step);
                if (next) set_screen(next, true);
            }
            return;
        }
        /* Quiet, with a name still up: it has held still long enough to be worth
         * the commit, and this is the poll that pays for it. */
        const orb_app_t *named = orb_carousel_pending();
        if (named && orb_carousel_due(now)) {
            orb_carousel_commit();
            set_screen(named, true);
        }
        return;
    }

    /* Armed: the entry on the glass owns the turn, and what a detent or a tap
     * means is its own iface's business. The router used to name
     * menu_view_apply_input() and color_picker_apply_input() here and hardcode
     * the clock as unreachable; a new app now needs no edit in this file. */
    orb_app_input(delta, pressed);
}

esp_err_t input_router_init(void)
{
    /* Boot on the home entry — the clock, per C8, read off the table rather
     * than named: with the GIF delta bank baked it is the live face of the
     * device (animating background + ticking hands), and it is the state a
     * device in a sealed case has to fall back to when nothing else works. Turn
     * the knob to reach the picker and the menu; hold it to come back.
     *
     * Runs on the main task, before the LVGL task exists, exactly as the boot
     * load always did: nothing else is painting yet, so the one widget call in
     * orb_app_activate() cannot race a second task. */
    disarm_all();
    const orb_app_t *home = orb_app_home();
    if (!home) {
        /* A table with no home is a build error, not a runtime one, but the
         * dial still has to come up on something it can show. */
        home = orb_app_neighbour(NULL, 1);
        if (!home) return ESP_ERR_NOT_FOUND;
        ESP_LOGE(TAG, "no home entry in the app table — booting on %s", home->slug);
    }
    const esp_err_t err = set_screen(home, /*load=*/true);
    if (err != ESP_OK) return err;

    s_poll_timer = lv_timer_create(poll_timer_cb, POLL_PERIOD_MS, NULL);
    if (!s_poll_timer) return ESP_FAIL;
    ESP_LOGI(TAG, "router running (poll %d ms, %d walkable app(s))",
             POLL_PERIOD_MS, (int)orb_app_count());
    return ESP_OK;
}

esp_err_t input_router_show(const orb_app_t *app)
{
    orb_carousel_cancel();
    disarm_all();
    return set_screen(app, true);
}

void input_router_home(void)
{
    /* Reached from a screen's own exit row, on the LVGL task inside this file's
     * poll. Cancel first: an exit pressed while a name was up means the user
     * chose the dial over the flick, and the chip must not outlive that. */
    orb_carousel_cancel();
    disarm_all();
    set_screen(orb_app_home(), true);
}

const orb_app_t *input_router_current(void)
{
    return s_current;
}

bool input_router_armed(void)
{
    return screen_armed(s_current);
}

void input_router_request(const orb_app_t *app)
{
    /* Only records the request; poll_timer_cb applies it on the LVGL task.
     * NULL is the "nothing pending" value, so a caller that could not resolve
     * a slug has already been refused at the route and cannot reach here. */
    if (!app) return;
    s_requested = app;
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
