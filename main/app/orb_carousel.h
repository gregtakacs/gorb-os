#pragma once
/**
 * orb_carousel — the knob's text carousel (docs/app-platform-plan.md ruling C5).
 *
 * The shell's own chrome between apps: one opaque name chip over whatever is
 * already on the glass, while a flick decides where the knob is going. Nothing
 * is activated until the detents stop arriving, so a three-detent flick costs
 * one activate and one full-page commit instead of three. That is the whole
 * reason this exists: the dial's own reveal measured 411-476 ms (§12.7), and
 * every intermediate screen a burst passed through used to pay that bill for a
 * frame nobody ever looked at.
 *
 * Text only, per C5: an app's `title` is its entire visual identity in the
 * walk, and no art tile is ever asked for here.
 *
 * ---- why it parks the scheduler instead of calling suspend() -------------
 *
 * The chip is an ordinary LVGL object, and the dial's L4 pass would wipe it:
 * clock_layer_commit() rebuilds every marked row straight out of the GIF canvas
 * plus an explicit layer list (clock_view.c:938), and the chip is in neither.
 * So the beat that marks those rows has to stop for the length of the peek.
 *
 * That is `orb_sched_pause()`, not the app's `suspend` hook. `suspend` is
 * documented as park-the-tick AND release-the-bank, and the bank must stay
 * mapped here: the view it feeds is still the one on the glass, and
 * art_bank_extra_ptr() returning NULL under a live compositor is the silent
 * garbage paint art_bank.h fences off. When a second app owns a beat of its own
 * (P3), this module needs a tick-only park hook in the ABI rather than a second
 * caller of orb_sched_pause().
 *
 * ---- threading ------------------------------------------------------------
 *
 * Every call runs on the LVGL task, from input_router's poll — the one context
 * allowed to touch widgets. Time is always passed IN as `now_us`: the poll
 * already holds the session's timestamp, and taking it as an argument is what
 * lets tools/host_carousel_test.py drive the settle window without a board.
 * `orb_carousel_init()` builds the chip once, on the boot ladder after
 * display_init and before input_router_init(), so no flick ever allocates.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct orb_app;

/** Build the chip. Call once, after display_init, before input_router_init. */
esp_err_t orb_carousel_init(void);

/** True when orb_carousel_init() built the chip. The router falls back to the
 *  pre-carousel immediate switch when this is false: a deferred destination the
 *  user cannot see being chosen is worse than no carousel at all, and the heap
 *  can say no at boot on a board whose PSRAM did not come up. */
bool orb_carousel_ready(void);

/** One roaming detent arrived while nothing was armed: name the next stop in
 *  `step` (+1/-1, already resolved through knob_col_step — this module never
 *  reads a raw sign) and park the beat. `on_glass` is the entry currently
 *  showing; the walk continues from whatever is already named, so a burst
 *  advances instead of re-naming the same neighbour. The commit deadline is
 *  chosen here, per detent: ORB_CAROUSEL_SETTLE_MS after this one when it
 *  continues a burst (arrived within ORB_CAROUSEL_BURST_GAP_MS of the last),
 *  ORB_CAROUSEL_READ_MS after it when the knob had gone quiet — a stop to
 *  read, which must not switch the glass underneath the reader. Returns the
 *  entry now named, or NULL when there is nowhere to walk. */
const struct orb_app *orb_carousel_step(int step, const struct orb_app *on_glass,
                                        int64_t now_us);

/** True once the named entry has held still past the deadline step() chose
 *  for it — the fast one inside a flick, the read one after a pause — without
 *  a further detent, which is the signal to commit it. False with nothing
 *  pending, so the caller can ask every poll. */
bool orb_carousel_due(int64_t now_us);

/** The entry currently named, or NULL. */
const struct orb_app *orb_carousel_pending(void);

/** Put the chip away because the commit is happening on this call: the caller
 *  switches screens next, and the switch owns the beat. Does NOT unpark —
 *  resuming here would burn a commit the activation is about to redo. */
void orb_carousel_commit(void);

/** Put the chip away WITHOUT switching: the flick ended on the entry already
 *  on the glass, so the cheapest correct result is no activation at all. Unparks
 *  the beat this module parked. */
void orb_carousel_cancel(void);

#ifdef __cplusplus
}
#endif
