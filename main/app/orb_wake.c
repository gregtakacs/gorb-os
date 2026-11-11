/* orb_wake's pure decision — the contract is in orb_wake.h (plan §64.6,
 * pomodoro-0-wake spec, pomodoro-design round-1 amendment 2). No ESP-IDF: the
 * caller hands the clock, the quiet-hours truth and the owner's DISPLAY truth
 * in as arguments, and this only reasons about them. tools/host_wake_test.py
 * compiles this file verbatim beside a harness and pins the clamp's ends, the
 * 60 s rate limit's edge (both sides, and the never-woken sentinel), and the
 * two policy refusals — the silent wrong answers a build cannot see. */
#include "orb_wake.h"

/* The requested span into the 3..15 window, whatever the verdict reports it at.
 * A clamp, not a refusal: an app that asks for a minute's wake gets the fifteen
 * seconds the OS will let it hold a glass it does not own. */
static uint32_t wake_clamp(uint32_t s)
{
    if (s < ORB_WAKE_MIN_S) return ORB_WAKE_MIN_S;
    if (s > ORB_WAKE_MAX_S) return ORB_WAKE_MAX_S;
    return s;
}

orb_wake_decision_t orb_wake_decide(uint32_t now_ms, uint32_t last_wake_ms,
                                    uint32_t requested_s, bool quiet, bool display_off)
{
    orb_wake_decision_t d;
    d.seconds = wake_clamp(requested_s);

    /* The policy refusals first, before the rate limit: with the OS quiet or the
     * face off by its owner this app cannot wake the glass at all, and that is
     * the truer answer than "you already did that a moment ago". The owner's
     * DISPLAY truth outranks the app exactly as it outranks a dim-request hold. */
    if (quiet || display_off) {
        d.verdict = ORB_WAKE_NOT_ALLOWED;
        return d;
    }

    /* The once-a-minute limit. `last_wake_ms` 0 is the never-woken sentinel — a
     * first ask is never a rate-limit refusal, whatever how young `now_ms` is.
     * The elapsed test is the unsigned subtraction idle_dim uses for the same
     * reason: a wrap of the 49-day uptime clock reads as a long gap (allowed),
     * never as a fresh wake; and exactly 60 s since the last is allowed, so only
     * a strictly younger wake refuses. */
    if (last_wake_ms != 0 &&
        (uint32_t)(now_ms - last_wake_ms) < ORB_WAKE_MIN_GAP_MS) {
        d.verdict = ORB_WAKE_INVALID_STATE;
        return d;
    }

    d.verdict = ORB_WAKE_OK;
    return d;
}
