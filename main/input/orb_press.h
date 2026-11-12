#pragma once
/**
 * orb_press — the OS lends a plain press (plan §64.13, docs/specs/alerts-design.md §3).
 *
 * One app (usually a background service, the pomodoro) may claim the press: while its
 * claim stands, a plain press on whatever app is on the glass is queued for the claimant
 * and the glass app never sees it. The claimant reads the queue from its orb_service()
 * with orb_press_lent()/orb_press_take(); the OS never wakes or raises it (§8's ruling:
 * a lent press does not bring the claimant to the front).
 *
 * This is the PURE core: a claimant slot word, one saturated per-slot queue count and a
 * swallowed counter, all PSRAM, so the whole state machine compiles on host gcc and
 * tools/host_press_test.py drives it verbatim. No ESP-IDF, no LVGL, no allocation and no
 * task. The claim is keyed by flash slot (the same key the ring table uses), so the
 * broker's revoke, an engine unload and a trap all reclaim it with one orb_press_drop.
 *
 * Latest caller wins, one claimant at a time (as with the ring, §64.1). The queue never
 * grows past ORB_PRESS_MAX_LENT: a fifth press while none has been read is swallowed and
 * counted, never wrapped or spilled, so a stalled service cannot replay a flood when it
 * wakes. Claim state is RAM only — lost on boot and reboot, never persisted.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pending presses one claimant may hold unread. The fifth in a row is swallowed. */
#define ORB_PRESS_MAX_LENT 4

/** Reset the whole machine: no owner, every queue empty, counters zero. Called on the
 *  boot ladder (and by the host suite between scenarios). RAM only; nothing to erase. */
void orb_press_init(void);

/** Claim or release the lent press on behalf of `slot`. `on` non-zero makes `slot` the
 *  one claimant — latest caller wins, so a steal discards the previous claimant's queued
 *  presses (and this claimant starts with an empty queue). `on` zero releases, and only
 *  if `slot` is the current claimant: a release from a non-owner is ignored. A slot the
 *  table cannot name claims nothing. Answers 1 when `slot` holds the claim after the
 *  call, 0 when it does not. */
int orb_press_claim(int slot, int on);

/** The flash slot that owns the claim right now, or -1 when nobody does. */
int orb_press_owner(void);

/** Queue one plain press for the claimant, the router's act when it lends. Returns 1 when
 *  the press landed on the claimant's queue, 0 when it was swallowed: no claimant, the
 *  queue already full (the fifth in a row), or `slot` names the claimant but the queue is
 *  saturated. A press delivered for a non-claimant `slot` is ignored (0), never queued. */
int orb_press_lend(int slot);

/** Consume and return one queued press (1), or 0 when the queue is empty. Does not
 *  release the claim: the claimant keeps lending until it drops or the owner revokes.
 *  Only the claimant's own take sees its queue; a take by a non-owner answers 0. */
int orb_press_take(int slot);

/** The number of queued presses `slot` may still take without releasing its claim (0 for
 *  a non-claimant or an empty queue). The polled count behind the app's orb_press_lent(). */
int orb_press_lent(int slot);

/** Drop `slot`'s claim and its whole queue, whatever state it is in. The lifecycle door:
 *  revoke, uninstall/engine unload, an import refused without the cap, and a trap all go
 *  through here so a dead app's unread presses can never be handed to a later app in the
 *  same slot. A clear of a slot that never claimed is a no-op. */
void orb_press_drop(int slot);

/* §64.17 (owner, 2026-11-12): the lent press is a DOUBLE press, so the single press, the turn
 * and the press-and-turn all stay the app's. While a claim stands, a plain press is held for
 * ORB_TAP_WINDOW_MS: a second within it is the claimant's, none and it is the app's single
 * press, late by the window. Pure timing, no clock of its own: the caller passes ms. */
#define ORB_TAP_WINDOW_MS 450   /* a relaxed hand's double press, event to event (Bronze) */
enum { ORB_TAP_NONE = 0, ORB_TAP_SINGLE = 1, ORB_TAP_DOUBLE = 2 };

/** A lendable plain press arrived: ORB_TAP_DOUBLE when it completes a pair inside the window
 *  (the pair goes to the claimant), ORB_TAP_NONE when it starts one (held). */
int orb_tap_press(uint32_t now_ms);
/** The held press's fate by now: ORB_TAP_SINGLE once the window passed without a second
 *  press (deliver it to the app), else ORB_TAP_NONE. */
int orb_tap_due(uint32_t now_ms);
/** Another gesture arrived first: ORB_TAP_SINGLE when a press was held (deliver it now,
 *  ahead of that gesture), else ORB_TAP_NONE. */
int orb_tap_cancel(void);
/** Forget a held press without delivering it (a hold, the carousel, a switch). */
void orb_tap_drop(void);
/** True while a press is held. */
int orb_tap_pending(void);

/** How many presses have been swallowed at the ceiling since the last init (the counter
 *  a fifth simultaneous press moves; it says nothing about who was refused). */
int orb_press_swallowed(void);

#ifdef __cplusplus
}
#endif
