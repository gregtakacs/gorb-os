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
 * The seam is a policy struct installed at start (orb_sched_start): the
 * functions the tick drives that ARE dial policy (compose_now plus the
 * tourbillon advance plus the layer-table pose pass, and the bench plan) stay
 * in clock_view.c and are called through it. This TU includes no clock header,
 * so the dependency
 * stays one-way — app -> gfx, never gfx -> app. Same shape as
 * gfx_set_band_hook(), which already carries a commit callback from here.
 *
 * What deliberately did NOT move: the hand-beat state (s_bt_ctr/sec/frac),
 * which only hand_angles() reads and clock_view_set_bounce() re-anchors, and
 * the layer/shadow/tourbillon widget sets the classic path renders.
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
 * the affected rows and the L4 commit pass (orb_sched_commit_layers through
 * the display flush hook, over the app's layer table, down into paint_row)
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
 * so a future app can own only the ones it has.
 * (Migration 2, Commit 4 deleted the third member, mark_all: the full-box
 * re-mark it served was already a no-op at the shipped CLOCK_L4_MARK_STATIC_-
 * BOXES=0, and the layer-table commit welds every mark the real mark sites —
 * apply_angles, gif_apply_frame, tourbillon_advance, the comp/aux update
 * passes — leave behind. The rows-capacity lesson it carried stays written
 * down in docs/rows-capacity-2026-10-10.md.) */
typedef struct {
    /* read the wall clock, pose the hands, advance the tourbillon frame bank,
     * and re-pose this dial's rows in the app layer table */
    void (*tick)(lv_timer_t *timer);
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
/* The plane the commit pass restores a marked row from: the PSRAM canvas for a
 * delta bank, the baked plate in XIP flash for a still plate, NULL for a keyframe
 * bank or a delta bank whose canvas failed. Non-NULL means the painter owns the
 * dial's pixels whenever the table gate below lets it. */
const uint16_t *orb_sched_restore_src(void);
/* The dial's layer-table claim, pushed by its build pass: rows claimed and rows
 * refused. (0, 0) at the top of every build, the real counts at its end. */
void     orb_sched_set_layer_claim(int rows, int refused);
/* L4 owns the glass: DIRECT_PUSH built, a plane to restore from, and a layer
 * table that claimed every row. The one test every mark site asks before it
 * marks. */
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

/* ---- the rung pick (P8.1, D16/D32; mode-aware per the owner ruling §40) -----
 *
 * The OS owns ONE flush cadence; every live element rides on top of it. Since
 * §40 an ask carries a MODE, not just a rate:
 *
 *   ORB_SCHED_DISTINCT  the element has an authored frame animation that must
 *                        meet its frames on an even tick boundary. Its rate is
 *                        one of the ladder {1,2,4,8,12} and it contributes a
 *                        DIVISOR: the tick must be a ladder rung that is a
 *                        common multiple of every distinct ask, so none aliases
 *                        (a 4 fps hand at a 15 fps flush advances 3.75 frames a
 *                        tick and jitters).
 *   ORB_SCHED_FLUID     continuous motion (a sweep, a slide) that wants
 *                        steadiness, not a rung. It contributes a CAP, not a
 *                        divisor: the pick is any integer fps up to the fluid
 *                        max, the measured floor, and the ceiling.
 *   ORB_SCHED_FREE      no fixed beat ("as fast as the glass goes"); rides the
 *                        floor. Contributes no divisor and no cap.
 *
 * The pick is therefore the highest ladder rung <= the working cap that is a
 * common multiple of every DISTINCT ask. The working cap is the tighter of the
 * measured floor and every fluid cap. A fluid-only or free-only stack runs at
 * the working cap. If the distinct asks admit no clean rung (their LCM is past
 * the ladder), it falls back to the slowest distinct ask — P8.1's ruling kept.
 * `floor_fps` is the delivered figure this build measured on the glass (docs
 * §33), never the ask. The animated result clamps into [4..15]: the owner
 * benchmarked 15 as the hardware ceiling and 4 as the full-panel floor.
 *
 * Pure and host-mirrored (tools/host_sched_rung_test.py); it reads no globals so
 * it is testable without the dial, and a rung that cannot fire gets deleted. */

/* The two hardware constants the owner signed 2026-10-22 (§40): the ANIMATED
 * ceiling (no animated stack beat 15 in any §33 run) and the animated full-panel
 * floor (a whole-panel repaint measured 5 sustained; the floor sits under the
 * measurement, D9's discipline). Static screens are not scheduled by the pick. */
#define ORB_SCHED_FPS_CEILING 15
#define ORB_SCHED_PANEL_FLOOR  4

/* Hysteresis, signed by the owner as drafted (2026-10-22, §40). A run demotes
 * after two report-seconds delivering under the pick; it promotes after ten
 * stable seconds carrying at least 25% model slack; demotes are rate-limited to
 * one per 30 s. They land as named constants so the bench can re-tune them and
 * the number moves in exactly one place. */
#define ORB_SCHED_HYST_DEMOTE_SEC  2    /* report-seconds under pick before a demote */
#define ORB_SCHED_HYST_PROMOTE_SEC 10   /* stable seconds of slack before a promote   */
#define ORB_SCHED_HYST_SLACK_PCT   25   /* model slack % that licenses a promote      */
#define ORB_SCHED_HYST_DEMOTE_MS   30000 /* minimum ms between two demotes            */

typedef enum {
    ORB_SCHED_FREE     = 0,   /* no fixed beat: rides the working cap      */
    ORB_SCHED_DISTINCT = 1,   /* authored frames: a ladder divisor         */
    ORB_SCHED_FLUID    = 2,   /* continuous motion: a cap, never a divisor */
} orb_sched_mode_t;

typedef struct {
    uint16_t fps;    /* the element's ask, in fps; 0 imposes nothing          */
    uint8_t  mode;   /* an orb_sched_mode_t                                  */
} orb_sched_ask_t;

uint32_t orb_sched_pick_rung(const orb_sched_ask_t *asks, unsigned n,
                            uint32_t floor_fps);
/* The synthetic ink the DISPLAY_BENCH paint pass blended, so the report can
 * prove the ink it booked really reached the glass. */
void orb_sched_ink_px_add(uint32_t n);

/* ---- the layer-table commit pass (Migration 2, P8 Commit 2) ---------------
 *
 * The registry row type, forward-declared the way orb_svc.h declares it: an
 * identical typedef of the same struct tag. Nothing here needs the row's
 * contents — only its address, which is the one thing the broker's getter
 * accepts. */
typedef struct orb_app orb_app_t;

/** Composite the active app's layer table: the job list `orb_layers_jobs()`
 *  builds for the app's live shown rows, handed to `gfx_commit()` with the
 *  delta bank's own canvas. Called from the display flush hook this TU
 *  registers (Migration 2, Commit 4) — after every dirty chunk has mirrored
 *  and before the bands DMA out — so an app never touches a paint context, a
 *  sprite pointer or `gfx_mark_rows`, and the weld point is the same one the
 *  dial's private hook always had: one commit, one owner of the mark set, no
 *  half-composited frame reaching the panel.
 *
 *  `active` is the entry on the glass (orb_app_current()), or NULL: no row, no
 *  table, no commit. An app that never called layer_add owns no table, and the
 *  getter answers NULL without minting one — see orb_svc_layer_table(). Since
 *  Commit 4 the clock feeds this table and owns no other L4 commit path; the
 *  dial's rows wake on the tick's pose pass (clock_view.c's
 *  clock_layers_refresh). */
void orb_sched_commit_layers(const orb_app_t *active);
