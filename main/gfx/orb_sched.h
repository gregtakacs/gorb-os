/* main/gfx/orb_sched.h — the OS-owned frame scheduler and delta bank.
 *
 * Extracted from main/stage/orb_stage.c in P1 with the code unchanged: the
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
 * in orb_stage.c and are called through it. This TU includes no clock header,
 * so the dependency
 * stays one-way — app -> gfx, never gfx -> app. Same shape as
 * gfx_set_band_hook(), which already carries a commit callback from here.
 *
 * What deliberately did NOT move: the hand-beat state (s_bt_ctr/sec/frac),
 * which only hand_angles() reads and stage_run() re-anchors, and
 * the layer/shadow/tourbillon widget sets the classic path renders.
 * Those are dial policy, and moving them would need an interface nobody has
 * asked for yet — P2's loader and P3's first foreign app will.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"   /* CONFIG_ORB_DEBUG_HTTP: the span bench */
#endif

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
 * clamped to 20 ms. Read by stage_init()'s cadence line. */
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
 * down in docs/history/rows-capacity-2026-10-10.md.) */
typedef struct {
    /* read the wall clock, pose the hands, advance the tourbillon frame bank,
     * and re-pose this dial's rows in the app layer table */
    void (*tick)(lv_timer_t *timer);
    /* the DISPLAY_BENCH synthetic-ink plan (walks the blocks, marks their rows) */
    void (*bench)(void);
} orb_sched_policy_t;

/* ---- the delta bank ------------------------------------------------------ */
/* The bank base is registered once by the app rather than read here: gfx must
 * not learn about art_bank, so stage_init() hands its own blob() value to
 * orb_bg_init() and this TU never maps the assets partition itself. */
void orb_bg_init(const uint8_t *base);
/* theme-pool-plan P4: forget the plate, the delta canvas and the frame bookkeeping, before
 * the stage builds for another app's theme. */
void orb_bg_release(void);

/* Allocate + seed the GIF-mode canvas from the keyframe and return the plate
 * source it produces, or NULL when there is no canvas (alloc failed, or the
 * bank is oversized) and the classic plate must be used instead. */
const lv_image_dsc_t *orb_bg_gif_canvas_init(void);
/* Fill the still-plate descriptor and build the classic plate-frame bank from
 * it. Only reached after orb_bg_gif_canvas_init() returned NULL, exactly as
 * stage_init() ordered the two. */
void orb_bg_classic_init(void);
const lv_image_dsc_t *orb_bg_plate_dsc(void);
/* The plate widget, handed in once after stage_init() creates it: the
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
/* The stage's word that its app put widgets of its own on the page: L4 paints the theme
 * straight into the panel mirror over whatever LVGL drew there, so a page with an app's
 * labels and rings over its theme renders through LVGL, which stacks them (Bronze,
 * 2026-11-04: the tracker's header, rings and footer painted over by its sweep). */
void     orb_sched_set_l4_veto(bool veto);

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
/* D41's ease: tick at the panel's ceiling (33 ms) for the next `ms`, then fall back to
 * the dial's own beat. Called from the dial's tick, so the period picked at the end of
 * that same tick already honours it. A dial that never eases never calls it. */
void orb_sched_burst(uint32_t ms);
/* A knob event changed what an app shows: tick now, at the panel's ceiling, for the next
 * `ms`, instead of waiting out the beat. A still theme beats once a second (Weather's
 * Scope bakes `step 1/s`), so without this a click reached the glass up to a second
 * late (plan §51, measured on Gold 2026-11-06). The deadline ladder re-anchors on the
 * early tick, so the beat resumes cleanly when the burst runs out. */
void orb_sched_kick(uint32_t ms);
void orb_sched_resume(void);
void orb_sched_pause(void);
#if CONFIG_ORB_DEBUG_HTTP
/* The span bench's beat: rate_fps > 0 forces the tick period (1..60), freeze stops
 * the GIF background so only the hands move, and gif_x (1..8) plays the GIF that many
 * times faster than its baked delays. 0/false/1 = the shipped beat. */
void orb_sched_bench(uint32_t rate_fps, bool freeze, uint32_t gif_x);
uint32_t orb_sched_bench_gif_frames(void);   /* running count of GIF frames applied */
/* The grid bench (plan §49.7): pace10 > 0 runs the beat at pace10/10 panel refreshes and
 * presents each frame on that exact edge; gif_every > 0 lands a gear frame on every Nth
 * tick instead of at its baked delay. 0/0 = the shipped beat. */
void orb_sched_bench_grid(int pace10, uint32_t gif_every);
/* §49.15: re-derive the live theme's plan (pace, gear every-N, pre-paint) and play it; the
 * bench's reset calls it so a bench run leaves the dial on its shipped behaviour. */
void orb_sched_plan_restore(void);
#endif
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

/* The dial's smoothness, as a reader with no serial line can price it
 * (docs/flight-tracker-design.md §6): every tick's start-to-start gap, binned
 * against the beat it was asked to keep. A gap up to 1.25 x the period is the
 * ~5 ms wake quantum lv_timer already costs; anything past 1.5 x is a beat the
 * glass visibly held. Monotonic since boot; read as deltas. */
typedef struct {
    uint32_t ticks;          /* gaps measured (the first after a resume is not)  */
    uint32_t le125;          /* gap <= 1.25 x period                             */
    uint32_t le150;          /* <= 1.5 x                                         */
    uint32_t le200;          /* <= 2 x: one beat held                            */
    uint32_t le300;          /* <= 3 x                                           */
    uint32_t gt300;          /* longer                                           */
    uint32_t max_gap_ms;     /* worst gap since boot                             */
    uint32_t period_ms;      /* the beat the last gap was measured against       */
    uint32_t rung_fps;       /* the current pick                                 */
    uint32_t rung_moves;     /* RUNG changes since boot: a demote is a jank too  */
} orb_sched_jank_t;
void orb_sched_jank_snapshot(orb_sched_jank_t *out);

/* A background burst the dial should ride out rather than re-rate for: a TLS
 * handshake (orb_https.c), ~2 s of PSRAM-heavy maths that slowed the flush by
 * ~20% for its length on the bench and twice cost a permanent 10->9 demote,
 * because a promote then needs 25% slack the dial never shows
 * (docs/flight-tracker-design.md §9.1). While a burst is open, and for one
 * report-second after it closes, measured seconds do not count toward a
 * demote. Nesting-safe; callable from any task. Owner ruling 2026-11-01:
 * handshakes are rare, the frame rate rebounds the moment one ends. */
void orb_sched_bg_burst(bool begin);
uint32_t orb_sched_bg_bursts_ignored(void);   /* demote-seconds forgiven, for the bench */

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
 * measurement, D9's discipline). Static screens are not scheduled by the pick.
 *
 * The ceiling moved to 20 on 2026-11-04 (owner): the §33 runs were at a 40 MHz
 * panel clock with each guest call refilling a 64 KB PSRAM stack; at 80 MHz and
 * without the refill, flights with fifteen plane rows costs ~33 ms a frame. 20 is
 * three panel scans. The ladder below stays {12,8,4,2,1}, so a dial with authored
 * frames picks what it always did; only a sweep that asks for more than 15 moves. */
#define ORB_SCHED_FPS_CEILING 20
#define ORB_SCHED_PANEL_FLOOR  4

/* Hysteresis, signed by the owner as drafted (2026-10-22, §40). A run demotes
 * after two report-seconds delivering under the pick; demotes are rate-limited to
 * one per 30 s. They land as named constants so the bench can re-tune them and
 * the number moves in exactly one place.
 *
 * The promote changed 2026-11-04 (owner). It asked for 25% more than the pick
 * delivered, but the pick paces the tick, so a demoted rung could never measure
 * above itself and never came back. A promote now probes: ten seconds delivering
 * the whole pick, and the probe wait since the last demote; a probe the dial
 * cannot hold demotes inside the wait and doubles it, up to the cap. */
#define ORB_SCHED_HYST_DEMOTE_SEC  2    /* report-seconds under pick before a demote */
#define ORB_SCHED_HYST_PROMOTE_SEC 10   /* seconds delivering the whole pick before a probe */
#define ORB_SCHED_HYST_DEMOTE_MS   30000 /* minimum ms between two demotes            */
#define ORB_SCHED_PROBE_WAIT_MS    30000 /* ms after a demote before the first probe  */
#define ORB_SCHED_PROBE_WAIT_MAX   300000 /* the doubled wait's cap                   */

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
 *  dial's rows wake on the tick's pose pass (orb_stage.c's
 *  clock_layers_refresh). */
void orb_sched_commit_layers(const orb_app_t *active);
