#pragma once
/**
 * orb_wake — the OS's one-shot glass wake an app may ask for from its service
 * (plan §64.6, pomodoro-0-wake spec, pomodoro-design round-1 amendment 2).
 *
 * THE PURE CORE: no ESP-IDF, no idle_dim, no `esp_timer`, no allocation. The
 * whole decision is one function over its arguments, so it is compiled verbatim
 * by tools/host_wake_test.py on host gcc (the ring-1 pattern) and pinned on the
 * edges a device cannot show: the clamp's ends, the 60 s rate-limit's edge, and
 * the two policy refusals. The firmware glue (orb_svc.c's service entry, the
 * wasm import and its s_binds row in orb_app_wasm.c, and the per-app last-wake
 * record dropped in slot_release) is thin and lives elsewhere; it reads the
 * clock, the OS's quiet hours and the owner's DISPLAY truth, calls this, and —
 * only on ORB_WAKE_OK — applies the wake through idle_dim's own request path so
 * idle_dim stays the policy owner of the glass.
 *
 * The rule the SDK states to apps (apps/sdk/orb.h, "power"):
 *   - an app off the glass lights a dimmed glass for a few seconds, taking
 *     nothing from the app on it and never switching apps;
 *   - `seconds` is clamped to 3..15 whatever is asked;
 *   - at most once per minute per app (a further ask answers the too-soon code);
 *   - refused during the OS's quiet hours, or with the display off by the owner;
 *   - the `power` cap gates it, as every other power ask.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The three verdicts, as the numeric esp_err_t values the app receives back
 * (esp_err.h: ESP_OK 0, ESP_ERR_INVALID_STATE 0x103, ESP_ERR_NOT_ALLOWED 0x10D).
 * Named here rather than included so this header stays host-compilable with no
 * ESP-IDF on the include path; the glue returns these ints as esp_err_t and the
 * numbers are the same. The cap refusal (no `power` cap) is the broker's own
 * SVC(CAP_POWER) gate before this is ever reached, so it is not one of these. */
#define ORB_WAKE_OK             0
#define ORB_WAKE_INVALID_STATE  0x103
#define ORB_WAKE_NOT_ALLOWED    0x10D

/* The clamp ends and the rate limit, one per line so the bench lifts them.
 * The 3 s floor is the shortest wake a phase-end glance needs; 15 s is the
 * longest a timer may hold a glass it does not own; 60 s is once a minute. */
#define ORB_WAKE_MIN_S       3u
#define ORB_WAKE_MAX_S      15u
#define ORB_WAKE_MIN_GAP_MS  60000u

/* The decision's answer: the verdict to hand the app, and the span to wake for
 * once the verdict is ORB_WAKE_OK. The seconds are the requested span clamped
 * into ORB_WAKE_MIN_S..ORB_WAKE_MAX_S, computed whatever the verdict. */
typedef struct {
    int32_t  verdict;   /* ORB_WAKE_OK | ORB_WAKE_INVALID_STATE | ORB_WAKE_NOT_ALLOWED */
    uint32_t seconds;   /* the requested span, clamped to 3..15                        */
} orb_wake_decision_t;

/** Decide one orb_wake ask, statelessly.
 *
 *  `now_ms` and `last_wake_ms` are the caller's uptime clock (the esp_timer
 *  millisecond count, unsigned, wrapping at ~49 days): `last_wake_ms` is this
 *  app's own last wake, and 0 means it has never woken the glass since it
 *  started. `requested_s` is the span the app asked for. `quiet` is the OS's
 *  quiet-hours truth right now; `display_off` is the owner's DISPLAY truth
 *  (the face switched off by the owner, not merely dimmed).
 *
 *  The policy refusals outrank the rate limit: quiet hours or a display the
 *  owner turned off answer ORB_WAKE_NOT_ALLOWED whatever the recent history, so
 *  an app that is barred and has also just woken sees the bar, not the minute.
 *  A wake inside the last 60 s answers ORB_WAKE_INVALID_STATE; the test is the
 *  unsigned elapsed subtraction (a clock wrap reads as a long gap, never a
 *  refusal), and exactly 60 s on the nose is allowed. Otherwise ORB_WAKE_OK. */
orb_wake_decision_t orb_wake_decide(uint32_t now_ms, uint32_t last_wake_ms,
                                    uint32_t requested_s, bool quiet, bool display_off);

#ifdef __cplusplus
}
#endif
