/**
 * input_router.c — knob-to-screen routing (see header for the contract).
 *
 * Two levels, one knob. Armed, the screen owns the turn: an app that keeps a dial
 * (orb_knob_dial, plan §51) is armed whenever it shows, and the menu once opened.
 * Roaming (the clock, an app that still arms on a press), a turn opens the app list
 * and a press arms. A hold always leaves for the app list. The router decides which
 * level applies by asking the screen whether it is armed, because only the screen
 * knows what its own armed view looks like.
 */

#include "input_router.h"
#include "knob.h"
#include "idle_dim.h"
#include "orb_rotation.h"     /* §59: the owner's rotation */
#include "orb_app.h"
#include "orb_press.h"      /* §64.13: the OS lends a plain press to a background claimant */
#include "orb_ring.h"       /* §64.13: the OS confirms a lent press with the claimant's ring pulse */
#include "orb_carousel.h"   /* cancel only: §51 retired the walk, so nothing puts a peek up */
#include "orb_fx.h"         /* the transition bench's run, served from this poll */
#include "orb_strip.h"      /* plan §54: the carousel owns the knob and the glass while up */
#include "theme_strip.h"    /* §61: and the theme carousel */
#include "nvs.h"            /* the boot-page memory: last shown page, one string key  */
#include <stdio.h>
#include <string.h>

#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"      /* esp_restart(): the unexplained-silence heal below */
#include "orb_svc.h"         /* orb_svc_note_clean_reboot(): that restart is not a crash */
#include "orb_sd.h"          /* orb_sd_lock_idle_ms(): the stall probe's card excuse */
#include "display.h"        /* display_bench_snapshot(): the M1 switch probe's frame counters */
#include "orb_theme.h"      /* orb_theme_glass_held(): a store owns the glass until its reboot */
#include "lvgl.h"

static const char *TAG = "input";

#define POLL_PERIOD_MS 10   /* knob sampling cadence; well under a human detent */

/* The entry on the glass. A pointer into the registry's table rather than an
 * index, because the table can gain an entry (or lose one to a failed init)
 * without this file being rebuilt, and an index burned into a log line is a
 * claim about position nobody can check. NULL until the first switch. */
static const orb_app_t *s_current;
static int64_t s_quiet_from;     /* §59: the last knob event or screen switch, for the rotation */
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
static volatile int32_t s_inj_push;   /* radar-plan §2.10: an injected press-and-turn */
static volatile bool s_inj_push_end;  /* and its end */
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
static int64_t     s_sw_since_us;      /* window-origin, never re-armed: total silence */
static bool        s_sw_lent_logged;   /* the with-explanation line prints once */

/* An app switch is under way: from the press until the new view's first frame. Read from any
 * task (a 64-bit word written on the LVGL task: a torn read only says "busy" a moment). */
bool input_router_switching(void) { return s_sw_open_us != 0; }

/* If the new view has not painted inside this, that is itself the finding, and it
 * is reported as one instead of waiting indefinitely for a commit that never
 * comes. Comfortably longer than the 40 ms beat the fastest theme bakes. */
#define SW_WINDOW_MS 1500

/* How recently the card must have touched its lock to excuse a silent glass.
 * A healthy-but-slow sequence — a multi-MB store copying at flash speed, a
 * chunked upload whose writer blinks the lock between 4 KB chunks — keeps
 * orb_sd_lock_idle_ms() under a few hundred ms by construction; nothing on a
 * working card gaps for seconds. 5 s is therefore generous cover for every
 * real sequence and still short enough that a genuinely dead card path stops
 * excusing itself in half a swipe. This replaced a fixed 10 s loan bound,
 * which a real 2 MB store on the bench (owner report, orb-B, v1.1.24) outstayed
 * while perfectly healthy: the heal rebooted a working store mid-copy and the
 * theme never landed. A guessed wall clock was the wrong instrument; the
 * card's own lock is the right one. */
#define SW_CARD_QUIET_MS 5000

/* Close the window set_screen() opened, once a real commit has landed since. */
static void switch_window_close(int64_t now)
{
    uint32_t f;
    uint64_t u;
    uint32_t mx;
    display_bench_snapshot(&f, &u, &mx);
    if (f == s_sw_frames) {
        if (now - s_sw_open_us > (int64_t)SW_WINDOW_MS * 1000) {
            /* Silence is only a verdict once it has been asked for an
             * explanation. The card IS one: while anything holds the card
             * lock, or held it within SW_CARD_QUIET_MS, the flush path is
             * dropping frames on purpose (the tile ring travels with that
             * lock — see orb_sd_lock), so frozen counters here are a healthy
             * board mid-write. Re-arm this window and keep waiting: no
             * guessed ceiling on how long a real card sequence takes — the
             * card's own lock says whether the card is still working. */
            const int64_t idle = orb_sd_lock_idle_ms();
            if (display_dma_lent() || idle < SW_CARD_QUIET_MS) {
                if (!s_sw_lent_logged) {
                    ESP_LOGI(TAG, "switch %s: silent with the card at work (lock held %lld ms) — waiting",
                             s_sw_name, (long long)orb_sd_lock_held_ms());
                    s_sw_lent_logged = true;
                }
                s_sw_open_us = now;   /* re-arm; s_sw_since_us keeps the total */
                return;
            }
            /* Unexplained. The self-heal, measured on orb-B 2026-10-27 and
             * worth its reboot: one stalled switch froze the glass for two
             * hours, every screen switch died the same way, and only a reboot
             * cleared it. The probe runs on the LVGL task itself, so the task
             * is alive and cannot fix the flush path from here; esp_restart is
             * the one recovery left that the person holding the orb can get
             * without a cable in sight. The fuse is f > 0: a board that never
             * committed ANY frame since boot has a dead panel, not a stall,
             * and rebooting into the same dead panel forever is a loop, not a
             * heal. That case logs its finding and waits for a human. The
             * resume-page memory (page below, same file) puts the owner back
             * on the face that froze.
             *
             * The second fuse is the card lock, and the bench learned why the
             * hard way: rebooting WHILE the lock is held ends a fatfs write
             * mid-sector, and the v1.1.24 heal burned a healthy 2 MB store
             * that way on the owner's own send. A hold that outlasts the
             * quiet bound is a wedged card path — and a wedged card deserves a
             * loud log and a cable, not a reboot through its mutex. The heal
             * fires only when the bus is idle: same recovery for every real
             * stall, zero chance of a torn FAT. */
            if (f > 0 && orb_sd_lock_held_ms() == 0) {
                ESP_LOGE(TAG, "display stalled: switch to %s unpainted %lld ms on a pipeline that "
                              "already drew %u frame(s), card idle — rebooting to recover the glass",
                         s_sw_name, (long long)((now - s_sw_since_us) / 1000), (unsigned)f);
                orb_svc_note_clean_reboot();   /* this restart is a heal, not a crash */
                s_sw_open_us = 0;
                s_sw_since_us = 0;
                esp_restart();
            }
            ESP_LOGW(TAG, "switch %s: no display commit within %lld ms — card %s, the new view never painted",
                     s_sw_name, (long long)((now - s_sw_since_us) / 1000),
                     orb_sd_lock_held_ms() ? "lock HELD: refusing to reboot through it" : "idle");
            s_sw_open_us = 0;
            s_sw_since_us = 0;
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
    s_sw_since_us = 0;
}

/* Put a registered entry on the glass, and open the M1 measurement window
 * around it. Returns the registry's refusal, if any, with the dial left exactly
 * where it was: a switch that failed has not switched, and reporting one as a
 * new screen is how a broken app turns into a misleading log. */

/* The boot-page memory (one NVS string, its own namespace): the slug of the
 * last page that actually reached the glass. A reboot resumes there instead of
 * at home; an orb with no record, an erased storage partition, or a remembered
 * card that no longer resolves (uninstalled, or refusing its own activate)
 * boots the home entry exactly as every earlier boot did. The in-memory cache
 * keeps the write off unchanged repeat switches — NVS wears, and a page the
 * user walks over ten times a day must not cost ten commits. */
#define PAGE_NS  "orb_page"
#define PAGE_KEY "last"

static char s_last_slug[32];   /* "" = nothing remembered since this boot */

/* The one writer of the resume record. NVS is thread-safe per handle, and both
 * callers (a glass switch on the LVGL task, a store job on the worker) are
 * idempotent-by-comparison against s_last_slug, so the steady state costs zero
 * commits no matter which task walks over the same page repeatedly. */
static void page_write(const char *slug)
{
    if (!slug || !*slug || strcmp(s_last_slug, slug) == 0) return;
    snprintf(s_last_slug, sizeof s_last_slug, "%s", slug);
    nvs_handle_t h;
    if (nvs_open(PAGE_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_str(h, PAGE_KEY, s_last_slug);
    (void)nvs_commit(h);
    nvs_close(h);
}

static void page_remember(const orb_app_t *app)
{
    if (app && app->slug) page_write(app->slug);
}

void input_router_remember_slug(const char *slug) { page_write(slug); }

/* Where the shell was entered FROM, so leaving the menu can put the owner back
 * on the entry they were standing on rather than on the menu's landing page
 * (owner ruling, 2026-10-25). RAM-only, and deliberately so: this is a position
 * within one viewing of the menu, not a place to reboot to — that job is
 * s_last_slug's. A slug, not a pointer: a card row can be uninstalled while the
 * owner is inside the menu, and a stale pointer into a recycled slot is the bug
 * that sharding the registry by pointer was written to avoid. The empty string
 * means the shell has not been entered from anywhere this boot (the boot landed
 * on it), so a menu exit from that state has nowhere to go but the landing. */
static char s_before_shell[32];   /* "" = the shell was not entered from a row */

static void shell_return_forget(void) { s_before_shell[0] = '\0'; }

static esp_err_t set_screen(const orb_app_t *app, bool load)
{
    if (!app) return ESP_ERR_INVALID_ARG;
    /* A theme store holds the glass from its park to its reboot. Past the park the
     * bank every theme-drawn screen reads is unmapped, so loading one renders from
     * memory that is gone ("Cache disabled but cached memory region accessed"). Only
     * home, the screen the store parked on, may stand. */
    if (app != orb_app_home() && orb_theme_glass_held()) {
        ESP_LOGW(TAG, "switch to %s refused: a theme store holds the glass", screen_name(app));
        return ESP_ERR_INVALID_STATE;
    }
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
    s_sw_since_us = sw_t0;         /* total-silence origin for the lend-excuse bound */
    s_sw_lent_logged = false;
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
    page_remember(app);   /* this view is the glass's now — a reboot returns here */
    s_quiet_from = esp_timer_get_time();   /* §59: a new view starts its own interval */

    /* Remember what the glass was showing the moment the shell took over, so a
     * menu exit can walk the owner back there (owner ruling, 2026-10-25). Keyed
     * on the registry's own SHELL kind rather than a name, exactly as the rest of
     * this router keys on the table. Only a real move onto the shell from another
     * entry records; a re-show of the shell, or a switch to any non-shell entry,
     * clears the memory so it never points past the visit that set it. */
    if (app->kind == ORB_APP_KIND_SHELL) {
        if (previous && previous != app && previous->slug)
            snprintf(s_before_shell, sizeof s_before_shell, "%s", previous->slug);
    } else {
        shell_return_forget();
    }
    return ESP_OK;
}

/* Plan §51: the app list. The hold out of any app, and a turn on a face with no dial
 * of its own, both land on the home entry's list, the cursor on the app the owner
 * came from moved `step` rows. Through the registry, so this file still names no view;
 * a home entry with no list leaves the owner on it, which is the old hold's answer. */
static void open_switcher(int step)
{
    const orb_app_t *from = s_current;
    orb_carousel_cancel();
    /* §54: the carousel, when its previews are in, takes the glass from here; the app
     * under it stays the screen until the carousel opens one (input_router_show or
     * input_router_open_settings). The text list stays the answer without it. */
    /* `step` is a row step (the text list's, below); the carousel moves in columns, which
     * knob.h defines as the row axis turned round — so the flick that opened it from a dial
     * with no list of its own moved it the wrong way, every later turn the right one (owner,
     * 2026-11-09) */
    if (orb_strip_open(from, -step)) {
        ESP_LOGI(TAG, "carousel from %s, step %d", screen_name(from), step);
        return;
    }
    disarm_all();
    if (set_screen(orb_app_home(), true) != ESP_OK) return;
    if (!orb_app_open_switcher(from, step)) return;
    ESP_LOGI(TAG, "app list from %s, step %d", screen_name(from), step);
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

/* §64.13: lend one plain press to a background claimant. Answers true when the press was
 * spent on the queue (so it must NOT reach the glass app), false when the press belongs to
 * the entry on the glass exactly as it always has. Only ever called for a plain press — the
 * caller has already ruled out a turn, a press-and-turn and a hold (those stay with the app
 * or the OS). Never lent over an OS/shell/menu/Settings face, never while this very press
 * is being spent to wake a dimmed or dark glass (orchestrator amendment 4: it wakes the
 * face and goes nowhere, the next press is lent), and never to a claimant that is itself
 * the app on the glass (§64.13: on its own glass the app handles the press as always). The
 * queue owns the fifth-press swallow; the pulse is the OS's confirmation, on the claimant's
 * own ring and never OS art. */
static bool lend_target(const orb_app_t *app, bool was_dimmed)
{
    if (!app || app->kind != ORB_APP_KIND_APP) return false;
    if (was_dimmed) return false;   /* sampled before the touch was reported, not after */
    const int owner = orb_press_owner();
    return owner >= 0 && owner != app->flash_slot;
}

static void lend_now(void)
{
    const int owner = orb_press_owner();
    if (owner < 0) return;
    orb_press_lend(owner);
    orb_ring_disp_pulse(owner);
}

/* §64.17 (owner, 2026-11-12): the lent press is a DOUBLE press. While a claim stands and its
 * claimant is not the app on the glass, a bare press is held for ORB_TAP_WINDOW_MS; a second
 * inside it goes to the claimant (the ring pulses), none and the held press reaches the app as
 * its ordinary single press, late by the window. Any other gesture first delivers a held press
 * ahead of itself, so the press, the turn and the press-and-turn all stay the app's; a hold, a
 * carousel or a switch drops it. Returns the press the app should see this poll. */
static const orb_app_t *s_tap_app;      /* the app a held press was made on */
static bool tap_gate(bool pressed, int32_t delta, int32_t push, bool was_dimmed)
{
    const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool deliver = false;
    if (orb_tap_pending() && s_tap_app != s_current) orb_tap_drop();     /* the app changed under it */
    if (orb_tap_due(ms) == ORB_TAP_SINGLE) deliver = true;               /* no pair came in time */
    const bool lendable = lend_target(s_current, was_dimmed);
    if (orb_tap_pending() && (delta || push || !lendable))
        deliver = orb_tap_cancel() == ORB_TAP_SINGLE || deliver;         /* another gesture: first the press */
    if (pressed && !delta && !push && lendable) {
        if (orb_tap_press(ms) == ORB_TAP_DOUBLE) lend_now();              /* the pair is the claimant's */
        else s_tap_app = s_current;                                      /* held: the app may yet get it */
        return deliver;
    }
    return pressed || deliver;
}


/* Runs on the LVGL task (created via lv_timer), so widget calls are legal here. */
/* §59: the rotation. Quiet means no knob event and no screen switch since s_quiet_from;
 * the decision is orb_rotation_pick's, over the carousel's own walk. */
static void rotation_poll(int64_t now)
{
    orb_rotation_t r;
    orb_rotation_get(&r);
    if (!r.on || !s_current || s_current->kind != ORB_APP_KIND_APP) return;
    const char *walk[ORB_ROTATION_MAX + 4];
    const orb_app_t *row[ORB_ROTATION_MAX + 4];
    int n = 0, cur = -1;
    for (size_t i = 0; i < orb_app_count() && n < (int)(sizeof walk / sizeof walk[0]); i++) {
        const orb_app_t *a = orb_app_at(i);
        if (!a || a->kind != ORB_APP_KIND_APP) continue;
        if (a == s_current) cur = n;
        row[n] = a; walk[n++] = a->slug;
    }
    const int next = orb_rotation_pick(&r, walk, n, cur, now - s_quiet_from);
    if (next < 0) return;
    ESP_LOGI(TAG, "rotation: %s -> %s after %lu s", s_current->slug, row[next]->slug,
             (unsigned long)r.every_s);
    if (input_router_show(row[next]) != ESP_OK) s_quiet_from = now;   /* refused: wait a turn */
}

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

    /* Press-and-turn (radar-plan §2.10): the clicks turned with the knob held down, kept
     * apart until the armed entry is asked for them below; the press they started is spent
     * either way (knob.c). */
    int32_t push = knob_take_push_delta();
    bool push_end = knob_take_push_end();

    /* Fold in anything injected. Draining the whole pending delta keeps the
     * injected path honest: it feeds the same stream a real turn feeds. The three
     * flags are latched first, because draining them here is what would otherwise
     * erase the only record of whether the event came from the bench or from the
     * encoder itself — see the touch report just below. */
    const int32_t inj = s_inj_delta;
    const int32_t ipt = s_inj_push;
    const bool    ip  = s_inj_press;
    const bool    il  = s_inj_long;
    if (inj) { s_inj_delta = 0; delta += inj; }
    if (ipt) { s_inj_push = 0; push += ipt; }
    if (s_inj_push_end) { s_inj_push_end = false; push_end = true; }
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
    /* §64.13 amendment 4: whether this touch found the glass dimmed, read BEFORE the touch is
     * reported. idle_dim_activity() starts the wake; the flag only happens to stay set through
     * this poll because the wake's write waits on this very task, and the lend must not depend
     * on that scheduling: a press that wakes the glass is never lent. */
    const bool was_dimmed = idle_dim_dimmed();
    if (delta || pressed || lpress || push) s_quiet_from = now;   /* §59: a touch holds the rotation */
    if (delta || pressed || lpress || push)
        idle_dim_activity(inj || ipt || ip || il
                          ? "http"
                          : (delta ? (pressed ? "turn+press" : "turn")
                                    : (pressed ? "press" : "hold")));

    /* D38: a USB share window owns the card, and the card owns the glass — no
     * frame reaches the panel for the window's whole life, so every screen
     * beneath it is a painting of the past. The gate goes on the event stream
     * itself, above even a requested screen switch: the card paints over
     * whatever the freeze caught, a press or a hold anywhere is the door back
     * to a live dial, and turns are swallowed rather than spent moving a
     * cursor nobody can see. The idle dim above still heard the touch: waking
     * a frozen-but-honest card is not a lost event. The registry, not this
     * file, knows whose view the card is (C6). */
    if (orb_app_share_gate(pressed || lpress)) return;

#if CONFIG_ORB_DEBUG_HTTP
    /* The transition bench (/api/v1/debug/fx) borrows the glass for one run and
     * hands it back verbatim; it runs here because the frames must come from
     * the LVGL task, between flushes. */
    orb_fx_service();
#endif

    /* A screen requested over HTTP is applied here, on the LVGL task, before any
     * knob event is considered: lv_screen_load() off this task is the widget
     * race every other rule in this file exists to avoid. */
    const orb_app_t *req = (const orb_app_t *)s_requested;
    if (req && orb_strip_active()) orb_strip_abort();   /* a named screen beats the carousel */
    if (req && theme_strip_active()) theme_strip_abort();   /* §61: and the theme carousel */
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

    /* §59: the owner's rotation, on a poll that carried no knob event and with the
     * carousel shut. */
    theme_strip_service();
    orb_theme_service();                 /* theme-cache-plan ruling 7: a card that came (back) */
    if (!delta && !pressed && !lpress && !orb_strip_active() && !theme_strip_active()) rotation_poll(now);

    /* §61: the theme carousel, likewise: the knob is its own while it is up. */
    if (theme_strip_active()) {
        orb_tap_drop();                      /* §64.17: a held press never lands under a carousel */
        theme_strip_poll(delta, pressed, lpress, now);
        return;
    }

    /* §54: while the carousel is up, every knob event is its own, and its frames are
     * drawn from here. It hands the glass back itself. */
    if (orb_strip_active()) {
        orb_tap_drop();
        orb_strip_poll(delta, pressed, lpress, now);   /* the carousel has no press-and-turn */
        return;
    }

    /* Long-press is the escape hatch out of every armed state, and it swallows
     * everything that made it so a hold never also lands as a turn/press on the
     * screen underneath. It deliberately no longer cycles screens: rotate owns
     * that gesture now.
     *
     * It also cancels any peek in flight: a name still on the glass after the
     * user has chosen to go home would commit itself over the top of the dial a
     * settle later.
     *
     * D12 gives the entry first refusal, and only the entry: an app page with a
     * group open has to be able to close the group rather than lose the page, and
     * the router may not learn what an open group is to allow that. Nothing that
     * answered OPT_ROW or declared no hook takes the press, so C8's hold-to-home
     * is untouched for every other screen — including the picker, which has
     * nothing to collapse. */
    if (lpress && orb_app_opt_long(s_current)) {
        lpress = false;
    }
    if (lpress) {
        open_switcher(0);                    /* §51: out of the app, onto the app list */
        orb_tap_drop();                      /* §64.17: a hold leaves; the held press goes nowhere */
        return;
    }

    /* Roaming: the turn belongs to the router, not the screen. A face with no dial of
     * its own (the clock, an app that still arms) opens the app list on a turn, the
     * cursor already one app along (plan §51): no carousel walk, so a turn inside an
     * app can never leave it, and the way between apps is one list. A press still arms
     * a view that has one. */
    /* A press-and-turn's end (radar-plan §6.11): the app that took its clicks hears it as
     * orb_push(0), and acts on the choice only now. Clicks taken in the same pass go
     * first: an end ahead of them left the choice open (the bench door's push_turn and
     * push_end, one frame apart). */
    if (push_end) {
        if (push && screen_armed(s_current) && orb_app_push(push)) push = 0;
        orb_app_push_end();
    }

    if (!screen_armed(s_current)) {
        delta += push;                       /* nobody to take it: an ordinary turn */
        pressed = tap_gate(pressed, delta, 0, was_dimmed);   /* §64.17: a double press is the claimant's */
        if (pressed) { arm_current(); return; }
        /* The list's own sense of a detent (knob_row_step, as menu_view walks it), so
         * the turn that opens the list moves its cursor the way the next turn will. */
        if (delta) open_switcher(knob_row_step(delta) > 0 ? 1 : -1);
        return;
    }

    /* Armed: the entry on the glass owns the turn, and what a detent or a tap
     * means is its own iface's business. The router used to name
     * menu_view_apply_input() and color_picker_apply_input() here and hardcode
     * the clock as unreachable; a new app now needs no edit in this file. */
    pressed = tap_gate(pressed, delta, push, was_dimmed);   /* §64.17: a double press is the claimant's */
    if (push && !orb_app_push(push)) delta += push;   /* no orb_push: an ordinary turn */
    orb_app_input(delta, pressed);
}

/* §61 (owner, 2026-11-08): a boot lands on the first app of the carousel, in the owner's
 * order (orb_app_walk_gather), and never on Settings unless the orb has no app that will
 * start. This replaces D36's "boot where the glass last stood". */
static const orb_app_t *first_app(void)
{
    for (size_t i = 0; i < orb_app_count(); i++) {
        const orb_app_t *a = orb_app_at(i);
        if (a && a->kind != ORB_APP_KIND_SHELL) return a;
    }
    return NULL;
}

static esp_err_t boot_first(const orb_app_t *home)
{
    for (size_t i = 0; i < orb_app_count(); i++) {
        const orb_app_t *a = orb_app_at(i);
        if (!a || a->kind == ORB_APP_KIND_SHELL) continue;
        if (set_screen(a, /*load=*/true) == ESP_OK) {
            ESP_LOGI(TAG, "boot: the carousel's first app, %s", a->slug ? a->slug : "?");
            return ESP_OK;
        }
    }
    return set_screen(home, /*load=*/true);
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
    const esp_err_t err0 = boot_first(home);
    if (err0 != ESP_OK) return err0;

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

void input_router_open_settings(void)
{
    orb_carousel_cancel();
    disarm_all();
    if (set_screen(orb_app_home(), true) != ESP_OK) return;
    if (!orb_app_open_settings()) return;
    ESP_LOGI(TAG, "settings from the carousel");
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

void input_router_menu_exit(void)
{
    /* Leave the shell back where it was entered from (owner ruling,
     * 2026-10-25): the exit row and the top-level long press both land the
     * owner on the entry the glass showed before the menu, never on the
     * landing page. disarm_all runs the menu's set_armed(false), which is the
     * item-3 edge that stores where in the list the owner stopped. With nothing
     * remembered — the boot landed straight on the menu, or the row the owner
     * came from has since uninstalled — the exit is the plain C8 home. */
    orb_carousel_cancel();
    disarm_all();
    const orb_app_t *back = s_before_shell[0] ? orb_app_find_slug(s_before_shell) : NULL;
    if (!back) back = first_app();       /* §61: an app, never the Settings landing */
    set_screen(back ? back : orb_app_home(), true);
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

void input_router_inject_push(int32_t delta)
{
    s_inj_push += delta;
}

void input_router_inject_push_end(void)
{
    s_inj_push_end = true;
}
