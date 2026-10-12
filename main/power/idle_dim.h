#pragma once
/**
 * idle_dim -- the dial dims itself when nobody has touched it for a while, and
 * comes back the instant somebody does.
 *
 * The owner's rule: after ten minutes without a touch the face drops to 20 % of
 * its brightness, and a touch returns it to *wherever it was* -- not to a fixed
 * full bright. That distinction is why the level is snapshotted when the dim
 * fires rather than assumed: the day a brightness control lands in the menu, the
 * dim must duck from the user's chosen level and come back to it, and the wake
 * path must not have to know the control exists. Until then the recorded user
 * level is 100 % and the boot value is left exactly as the panel's own init set
 * it, so this feature changes nothing about a freshly powered-up dial.
 *
 * What counts as a touch (both call idle_dim_activity(), which is the whole
 * interface the rest of the firmware has):
 *   - the rotary encoder, from input_router's poll -- a detent, a tap or a long
 *     press, including the ones injected over HTTP, because the bench turns the
 *     dial with the same numbers the encoder does;
 *   - the accelerometer, but credited as a touch only while dimmed: awake, motion
 *     tells us nothing we would act on, so the IMU is polled at 20 Hz in the dark
 *     and 1 Hz in the light. "Any real motion" wakes it -- a pick-up, a knock on
 *     the desk, the case shifting -- so the test is a deviation from the learned
 *     rest vector, not a magnitude, and the baseline tracks slow drift instead of
 *     latching. The lit poll exists to keep the deviation high-water warm, which
 *     is the number ORB_IDLE_MOTION_MG is set from.
 *
 * Runs as one low-priority task on its own, not on the LVGL task: the brightness
 * write is a QSPI transaction of its own and the frame budget documented in
 * docs/fps-budget-findings-2026-10-10.md is not theirs to spend.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the idle/motion task. Call after display_init() and imu_init(). */
esp_err_t idle_dim_init(void);

/** Note that the user touched the orb. Safe from any task: it only stamps a
 *  32-bit counter, and the policy task applies it on its next tick.
 *
 *  `why` names the source, and is logged at a rate-limited one line per second.
 *  The strings input_router passes are "turn", "turn+press", "press", "hold" and
 *  "http" (the last for a detent injected by the bench over the LAN). A dial that
 *  never dims is almost always a dial whose clock keeps being stamped, and until
 *  now the firmware could say nothing about who was doing it; a knob that bounces
 *  on a vibrating desk looks exactly like a timeout that does not work. */
void idle_dim_activity(const char *why);

/** How many times anything has claimed a touch since boot. On
 *  /api/v1/status as idle.stamps: a dial that will not dim has either a stalled
 *  policy task or a clock that never stops being stamped, and this is the number
 *  that tells the two apart without a serial cable. */
uint32_t idle_dim_activity_count(void);

/** Record the level the user wants the dial at (a future menu control calls
 *  this). While dimmed it also counts as a touch, so the change is visible. */
void idle_dim_set_user_level(uint8_t percent);

/* ---- the runtime settings, behind the DISPLAY menu ------------------------
 * The Kconfig values are the boot defaults, not the shipped behaviour: the
 * owner turns the feature off or picks a different timeout on the dial, the
 * choice is written to NVS, and from then on the NVS value wins on every boot.
 * Both getters are safe from any task (one 32-bit read each). Both setters
 * write NVS and so belong on a worker task, not the LVGL one -- the same rule
 * orb_net_tz_select() follows from the menu worker, because an NVS write is
 * normally milliseconds but can hit a sector erase, and a dial that drops
 * frames while somebody chooses is the un-reproducible bug this file keeps
 * refusing. */

/** True while the dim policy is in force: the feature compiled in AND the
 *  user has not switched it off. /api/v1/status reports this, not the
 *  compile-time option -- a switch you cannot verify is a switch you cannot
 *  trust. */
bool idle_dim_enabled(void);

/** Apply and persist the switch. A failed NVS write still takes effect until
 *  the next boot, and says so through the returned code. */
esp_err_t idle_dim_set_enabled(bool on);

/** The timeout actually in force, seconds. */
uint32_t idle_dim_timeout_s(void);

/** Apply and persist the timeout; clamped to idle_dim_clamp_timeout_s(). */
esp_err_t idle_dim_set_timeout_s(uint32_t seconds);

/* ---- the pure decisions, exposed for the host bench -----------------------
 * The two new ones are the ones with silent wrong answers: an elapsed test
 * that mishandles the 49-day wrap dims a freshly touched dial, and a clamp
 * that passes 0 dims the instant the task starts. Both are one line to read
 * and impossible to see on a bench that runs for minutes, so both are pinned
 * like the motion decisions above. */

/* Below one tick's rounding the feature reads broken; the DISPLAY menu's
 * smallest preset (1 min) sits above it. The 24 h ceiling is a wall-clock's
 * sane maximum and the figure a wild persisted value is clamped to -- the
 * bench lifts these two defines verbatim (tools/host_idle_dim_test.py), so
 * keep each on its own unbroken line. */
#define IDLE_DIM_TIMEOUT_MIN_S   5u
#define IDLE_DIM_TIMEOUT_MAX_S   86400u

/** The timeout the setter actually stores for a requested one. */
uint32_t idle_dim_clamp_timeout_s(uint32_t s);

/** True when the face should sit at the dim level: the feature on, and the
 *  unsigned elapsed subtraction -- same argument as the task's: a wrap of the
 *  49-day clock reads as a big idle time, never a negative one. */
bool idle_dim_elapsed_decide(uint32_t now_ms, uint32_t last_ms,
                             uint32_t timeout_ms, bool enabled);

/** The dimmed flag the task is allowed to hold after ONE brightness attempt:
 *  `want` if the panel took the write, `dimmed` (unchanged) if it refused. A
 *  refused write that is believed anyway is a dark dial that believes it is lit
 *  -- no later tick disagrees with the flag, so the write is never retried, and
 *  the 20 Hz dark poll that carries the motion wake stops with it. Retry needs
 *  no timer of its own: the next tick re-asks at the rate of the state the panel
 *  is actually in. Pinned by tools/host_idle_dim_test.py. */
bool idle_dim_level_apply_decide(bool want, bool dimmed, bool wrote);

/** Brightness writes the panel has refused since boot, on /api/v1/status as
 *  idle.writes_failed. A retry that converges is otherwise invisible: "the knob
 *  woke it on the third try" and "the knob never woke it" are the same story to
 *  anyone watching the glass. */
uint32_t idle_dim_level_write_fail_count(void);

/* ---- the pure decision, exposed for tools/host_idle_dim_test.py ----------
 * A motion detector's threshold maths has to be testable without a hand on the
 * device: it is the one part of this file that can silently wake the dial for
 * ever (threshold too low: desk vibration) or never (too high: the orb looks
 * dead). No hardware, no floats, no globals. */
typedef struct {
    int32_t base[3];   /* learned rest vector, in raw LSB   */
    int32_t worst;     /* widest |sample - base| ever seen, in LSB: the number a
                        * threshold is actually set from -- see idle_dim_worst_mg */
    bool    have_base; /* false until the first sample seeds it */
} idle_motion_t;

/** True when this sample is motion: any axis deviates from the learned rest
 *  vector by more than thresh_lsb. A cold state seeds the baseline and reports
 *  still, so re-entering the dark after the orb was carried somewhere does not
 *  wake itself.
 *
 *  The baseline follows EVERY sample at 1/8, motion included -- judged against
 *  the pre-update value, so the follow cannot absorb the motion it reports. That
 *  is what makes a tilt-then-hold settle back to still: a follow reserved for
 *  still samples never runs while the lag is large, so the detector latches and
 *  the dial, read as touched on every tick, never dims again. */
bool idle_motion_decide(idle_motion_t *st, const int16_t s[3], int16_t thresh_lsb);

/* ---- the wake debounce, also exposed for the host bench -------------------
 * The dark poll runs at 20 Hz, which is what a knock on the case needs -- and is
 * also 20x more likely than the 1 Hz lit poll to land on a single-sample spike.
 * Measured on the bench, an untouched orb on a desk produced two ~55 mg jolts on
 * the gravity axis 24 s apart, enough to dim the face and un-dim it again before
 * anybody could see it: the owner's symptom is "it never stays dimmed", and it is
 * indistinguishable from a broken timer from outside.
 *
 * A pick-up is not one sample. The same measurements that pin the follow say a
 * held tilt of 160 mg reads motion on 12 consecutive samples and 400 mg on 19, so
 * requiring a short run separates a real lift from a desk transient without
 * touching the threshold -- which is the knob the owner does not have to reach. */
typedef struct {
    uint8_t run;    /* consecutive samples judged motion */
    bool    fired;  /* this run has already been reported, so a continuing shake
                     * lifts the dial once rather than logging a wake every 50 ms */
} idle_wake_t;

/** True on the sample that completes a run of `need` motion verdicts, once per
 *  run. Any still sample ends the run and re-arms it. `need` 0 or 1 makes this a
 *  pass-through, which is the pre-debounce behaviour and what the Kconfig
 *  documents as the A/B setting. */
bool idle_motion_wake_decide(idle_wake_t *st, bool motion, uint8_t need);

/* ---- what the bench needs to see, from /api/v1/status ---------------------
 * The dim is a timer you cannot point a camera at, and the motion threshold is a
 * compiled-in constant with no dial to read. Both were debuggable only by the
 * serial line and a guess, so each gets a number here: the countdown that says
 * when the face will drop, and the widest deviation the accelerometer has ever
 * reported, which is what ORB_IDLE_MOTION_MG is set against. */

/** Milliseconds since the last touch of any source, clamped to the elapsed
 *  direction only: unsigned, so a counter wrap reads as a big idle time, never
 *  as a negative one. */
uint32_t idle_dim_ms_since_touch(void);

/** True while the dim level is applied. */
bool idle_dim_dimmed(void);

/** The level the dial returns to when it wakes, which is the level a brightness
 *  control would set. It is reported on /api/v1/status precisely because it is
 *  NOT yet controllable: nothing in the firmware calls idle_dim_set_user_level(),
 *  so this reads the boot level forever, and a page that shows it as 100 while the
 *  panel was dimmed to 20 tells the difference between a dim that failed to lift
 *  and a level that was never set. */
uint8_t idle_dim_user_level(void);

/** How many times motion alone has been credited with a wake, since boot. */
uint32_t idle_dim_motion_wakes(void);

/** Widest accelerometer deviation from the learned rest vector since boot,
 *  converted to milli-g. Zero when the IMU is absent or has never been polled,
 *  which is the difference between "nothing moved it" and "there is nothing to
 *  read": idle_dim_motion_wakes() cannot tell those apart on its own. */
int idle_dim_worst_mg(void);

/** The same high-water taken from the dimmed poll only (20 Hz), in milli-g.
 *  Separate because the lit poll is 1 Hz and systematically misses the rare
 *  spike: a threshold calibrated on idle_dim_worst_mg() alone was measured at
 *  7 mg lit against a 62 mg reading seconds into the dark, which dims the face
 *  and immediately un-dims it. Settle ORB_IDLE_MOTION_MG against this one. */
int idle_dim_worst_dark_mg(void);

#ifdef __cplusplus
}
#endif
