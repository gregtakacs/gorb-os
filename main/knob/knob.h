#pragma once
/**
 * knob.h — Rotary encoder (KY-040) input, ESP-IDF port of orb-os/src/knob.cpp.
 *
 * The physical knob on the 8-pin header (H2), wired exactly as in the Arduino
 * reference (see board_config.h):
 *   Encoder CLK (A)   -> GPIO18   quadrature phase A
 *   Encoder DT  (B)   -> GPIO17   quadrature phase B
 *   Encoder SW (push) -> GPIO16   active-low momentary switch
 *   Encoder +         -> 3V3
 *   Encoder GND       -> GND
 *
 * Like the reference, ALL edge detection lives in GPIO ISRs, not in a poll
 * loop, for the two reasons orb-os discovered on hardware:
 *   - A press-and-release that both happen inside one slow loop iteration must
 *     not be missed (a hardware interrupt cannot miss it).
 *   - The detent stream that decides a gesture and the stream that moves the UI
 *     must be the SAME counter, so they can never disagree about what happened.
 *
 * The consuming API below mirrors orb-os (takeDelta / takePress / takeLongPress):
 * each returns pending input since the last call and then clears it. They read
 * volatile ISR state and are safe to call from the LVGL task via input_router.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the three GPIOs + install the edge ISRs. Call once from app_main. */
esp_err_t knob_init(void);

/**
 * Periodic housekeeping, call from a single task (the LVGL task) every ~10 ms.
 * Catches a sustained hold (long-press), which by definition spans many calls
 * and so does not need the ISR's edge precision. Rotation and the initial press
 * edge are handled entirely in the ISRs and need no poll.
 */
void knob_poll(void);

/** Net detents since last call: >0 turned right (CW), <0 turned left (CCW).
 *
 * This is the ELECTRICAL sense and it stays that way on purpose — it is what the
 * quadrature table says happened, and the bench door hands the same numbers to the
 * screens. What a turn means to the user is a separate decision, made once, in
 * knob_row_step()/knob_col_step() below. Do not negate a delta inside a screen.
 */
int32_t knob_take_delta(void);

/* ===========================================================================
 * Direction convention: which way a turn moves the highlight.
 * ===========================================================================
 *
 * The encoder is mounted on the side of the case, above the dial's centre, so the
 * arc your finger travels is not the arc the list scrolls along. A clockwise turn
 * — the way the raw stream reports as positive — therefore pushed the highlight
 * DOWN the old list, and the owner reads that as backwards: the hand reaches
 * clockwise and expects the top of the list to come to him.
 *
 * KNOB_DIR_DOWN_IS_CW is the single switch for that. It is 0 now: a clockwise
 * turn moves the highlight UP (toward row 0), and a counter-clockwise turn moves
 * it DOWN. Flip it to 1 and every screen follows, because nothing else is allowed
 * to interpret a sign.
 *
 *   knob_row_step(delta)   +1 = one row DOWN     (later rows, list scrolls down)
 *                          -1 = one row UP       (toward row 0)
 *   knob_col_step(delta)   +1 = one column RIGHT (later cards, list scrolls right)
 *                          -1 = one column LEFT
 *
 * And the pairing the owner asked to keep on record, because the knob's position
 * fixes it for both axes at once and it is not derivable from the code:
 *
 *      the direction that means DOWN is also the good direction for LEFT
 *      the direction that means UP   is also the good direction for RIGHT
 *
 * i.e. col_step is row_step rotated 90° rather than negated independently, so a
 * vertical list and a horizontal strip of cards turn the same physical flick the
 * same physical way. A future left/right screen MUST call knob_col_step(); if it
 * needs the opposite sense, that is a second constant next to this one, and it
 * must be named, not written inline.
 */
#define KNOB_DIR_DOWN_IS_CW  0

/** Signed detents -> row step: +1 down the list, -1 up it, 0 for no turn. */
int knob_row_step(int32_t delta);

/** Signed detents -> column step: +1 right, -1 left, 0 for no turn. */
int knob_col_step(int32_t delta);

/** True exactly once if the knob was tapped (released under the hold time). */
bool knob_take_press(void);

/** True exactly once if the knob was held at least the long-press time. */
bool knob_take_long_press(void);

/** Milliseconds the knob has been continuously held right now (0 if released). */
uint32_t knob_held_ms(void);

/** The hold duration that promotes a hold to a long press (for a countdown UI). */
uint32_t knob_long_press_ms(void);
/** Raw quadrature count, before the detent hysteresis. For the bench only: it is
 * the difference between "the encoder is fine and the commit rule is stingy" and
 * "the encoder is missing edges", which cannot be told apart from behaviour. */
int32_t knob_raw_position(void);

/** Committed detents so far — read it beside knob_raw_position() to tell a
 * missing edge from a commit rule that is asking for too much travel. */
int32_t knob_detent_count(void);

/** Mean raw steps consumed per committed detent, in TENTHS (40 == 4.0), or 0 if no
 * detent has committed yet. Read this before changing KNOB_STEPS_PER_DETENT. */
int32_t knob_raw_steps_per_detent(void);

/** Most raw steps ever consumed by a single committed detent (burst detection). */
int32_t knob_raw_span_max(void);

/** How detents were arrived at, and what the pins read. A valley hit is a click whose
 * valley the ISR saw; a settle hit is one recovered by the idle pass. A session that is
 * mostly settle hits means edges are being LOST between pin and ISR — an interrupt
 * latency or wiring problem, not a tuning problem — while a healthy dial is almost
 * entirely valley hits. ab/valley are 2-bit quadrature states, valley being the one the
 * detent spring parks the shaft in. */
int32_t knob_valley_hits(void);
int32_t knob_settle_hits(void);
int32_t knob_ab(void);
int32_t knob_valley(void);

#ifdef __cplusplus
}
#endif
