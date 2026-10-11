/* main/gfx/orb_sched.h — the OS-owned frame scheduler and delta bank.
 *
 * Extracted from main/clock/clock_view.c in P1 with the code unchanged: the
 * orb_bg_/orb_sched_ prefixes, the state passed back in at the callers that
 * still own it, and the per-tick policy callback are the only diffs.
 * gfx_parity_test.py proves that claim against the pinned pre-extraction
 * revision the same way it proves orb_gfx.c's.
 *
 * Two things live here:
 *
 * The delta bank — the GIF-mode PSRAM canvas, its descriptor, the classic
 * plate-frame descriptors, and the frame cycling (gif_mode/gif_tick_ms/
 * bg_live/build_bg_dscs/bg_frame_now/show_plate_frame/gif_apply_frame plus
 * their state). gfx_commit() already takes the canvas as a parameter, so the
 * bank is compositor state, not dial state: it has to outlive the clock view
 * for any second app to composite at all.
 *
 * The tick cadence — the lv_timer, the deadline ladder that keeps the beat
 * honest against the flush path, the gif due clock, and the per-second
 * throughput report. Cadence is the OS's: it is the same ladder whoever owns
 * the pixels, and the app only decides the period it aims at.
 *
 * The seam is a policy struct installed at start (orb_sched_start): the three
 * functions the tick drives that ARE dial policy (compose_now, the tourbillon
 * advance, the L4 full-box mark pass, the bench plan) stay in clock_view.c and
 * are called through it. This TU includes no clock header, so the dependency
 * stays one-way — app -> gfx, never gfx -> app. Same shape as
 * gfx_set_band_hook(), which already carries a commit callback from here.
 *
 * What deliberately did NOT move: the hand-beat state (s_bt_ctr/sec/frac),
 * which only hand_angles() reads and clock_view_set_bounce() re-anchors, and
 * the layer/shadow/tourbillon widget sets the mark pass and commit hook walk.
 * Those are dial policy, and moving them would need an interface nobody has
 * asked for yet — P2's loader and P3's first foreign app will.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* L3/L4: rather than invalidating the plate through LVGL whenever a GIF delta
 * rect or a hand rotation dirties it — the software image blit measured
 * ~250-300 cycles/px (~85 ms/frame on the orb ticks; experiment A: dropping the
 * plate invalidation took render~ 100->14 ms and fps 8->23) — the tick marks
 * the affected rows and the L4 commit hook (clock_layer_commit/paint_row)
 * rebuilds them straight in the panel mirror: background restored out of the
 * canvas, then the hands/shadows/glass composited over it. 0 = old
 * all-through-LVGL path (A/B fallback).
 *
 * Lives here because both halves of the arrangement read it: the scheduler's
 * delta push and the app's commit hook have to agree, and one number that
 * silently disagrees across a TU boundary is a path that stops marking. */
/* The fallback beat the boot log quotes when a bake recorded no per-frame
 * delays at all: orb_bg_gif_tick_ms()'s real floor is the baked table's own,
 * clamped to 20 ms. Read by clock_view_init()'s cadence line. */
#define CLK_GIF_TICK_MS 40

#ifndef CLK_BG_DIRECT_PUSH
#define CLK_BG_DIRECT_PUSH 1
#endif

/* The dial policy the tick drives, installed once at orb_sched_start(). Every
 * member is required by the clock view; the scheduler NULL-guards them anyway
 * so a future app can own only the ones it has. */
typedef struct {
    /* read the wall clock, pose the hands, advance the tourbillon frame bank */
    void (*tick)(lv_timer_t *timer);
    /* re-mark every owned layer's FULL box at its present pose (the L4 pass
     * that makes the hook the complete owner of the rows it rebuilds) */
    void (*mark_all)(void);
    /* the DISPLAY_BENCH synthetic-ink plan (walks the blocks, marks their rows) */
    void (*bench)(void);
} orb_sched_policy_t;

/* ---- the delta bank ------------------------------------------------------ */
/* The bank base is registered once by the app rather than read here: gfx must
 * not learn about art_bank, so clock_view_init() hands its own blob() value to
 * orb_bg_init() and this TU never maps the assets partition itself. */
void orb_bg_init(const uint8_t *base);

/* Allocate + seed the GIF-mode canvas from the keyframe and return the plate
 * source it produces, or NULL when there is no canvas (alloc failed, or the
 * bank is oversized) and the classic plate must be used instead. */
const lv_image_dsc_t *orb_bg_gif_canvas_init(void);
/* Fill the still-plate descriptor and build the classic plate-frame bank from
 * it. Only reached after orb_bg_gif_canvas_init() returned NULL, exactly as
 * clock_view_init() ordered the two. */
void orb_bg_classic_init(void);
const lv_image_dsc_t *orb_bg_plate_dsc(void);
/* The plate widget, handed in once after clock_view_init() creates it: the
 * frame cycling and the 1 px commit nudge both invalidate through it. */
void orb_bg_set_plate(lv_obj_t *plate);

/* The gif-mode canvas the commit band restores out of (NULL until created). */
const uint16_t *orb_bg_canvas(void);
/* Ticks-per-frame holder for orb_sched_bg_rate()'s out-parameter, so the init
 * ladder reads as the single call it was. */
uint32_t *orb_bg_every_p(void);

/* The three pacing predicates. gif_mode() is the canvas+theme test, bg_live()
 * is "the background has motion to show", both read only theme geometry and
 * bank state. Exported because the app's hand policy and boot log must agree
 * with the scheduler's, not re-derive a second answer. */
bool     orb_bg_gif_mode(void);
uint32_t orb_bg_gif_tick_ms(void);
bool     orb_bg_bg_live(void);
/* L4 owns the glass: DIRECT_PUSH built, a canvas to restore from, and a GIF
 * bank to push. The one test every mark site asks before it marks. */
bool     orb_sched_l4_active(void);

/* Which plate frame the bgAnim beat is on, and the act of putting one up.
 * Both no-op when the background is not live; frame 0 is the still. */
int  orb_bg_frame_now(void);
void orb_bg_show_plate_frame(int idx);
/* What the beat resets: the frame-on-glass bookkeeping at view build, and the
 * whole non-loop play state when the view comes back. */
void orb_bg_init_frames(void);
void orb_bg_reset_play(void);

/* ---- the tick cadence ---------------------------------------------------- */
/* Create the frame timer at period_ms, parked: the clock only runs it once the
 * dial is on the glass. policy is copied, so the caller's struct may be
 * automatic. Started-ness is what the app's bounce gate asks before it resizes
 * the view's state. */
void orb_sched_start(uint32_t period_ms, const orb_sched_policy_t *policy);
bool orb_sched_started(void);
bool orb_sched_running(void);
/* The tick's park gate, set by the router's bounce before it asks whether the
 * timer exists at all — the shipped bounce did those two in that order, and
 * keeping them separate is what lets the tick read the gate verbatim. */
void orb_sched_set_running(bool on);
void orb_sched_resume(void);
void orb_sched_pause(void);
/* Re-anchor the gif due clock and the beat ladder to now after a park; re-zero
 * the per-second report's baselines. Two calls, in the shipped order, so the
 * lv_tick_get() reads stay paired the way the old bounce paired them. */
void orb_sched_reanchor_gif(void);
void orb_sched_reset_fps(void);
/* The hand-rate policy, moved verbatim: 1000/tick_rate clamped to the 33 ms
 * panel ceiling, 1 s when the dial ticks, the GIF's own delay in GIF mode.
 * bg_rate() picks the fastest period that divides the background's frame
 * period, holding *every ticks per frame. */
uint32_t orb_sched_tick_base_ms(void);
uint32_t orb_sched_bg_rate(uint32_t base, uint32_t *every);
/* The synthetic ink the DISPLAY_BENCH paint pass blended, so the report can
 * prove the ink it booked really reached the glass. */
void orb_sched_ink_px_add(uint32_t n);
