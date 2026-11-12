/* pomodoro.h — the pomodoro app's phase machine, pure (docs/specs/pomodoro-design.md §2, §3.2,
 * §5.2, §6, §7): the cycle of N focuses, the knob's three moves over it, the wall-clock service
 * pass, the store an in-place restart reads back, and what the OS ring should be for a state.
 *
 * No imports beyond the two below: no orb.h, no orb_stage.h, no <math.h> and no trig — the
 * fraction, the clamp and the ladder are all integer, so tools/host_pomodoro_test.py compiles
 * pomodoro.c as it stands with host gcc and mutates it (the radar.h / orb_wake.h split). Every
 * time-dependent entry takes its time as an argument: nothing here reads a clock.
 *
 * The chime is NOT here (§9): pm_service answers PM_EV_END and the caller sounds it, once, on
 * that pass. pm_t's remaining seconds are derived from the wall second each pass, never counted. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PM_STOP    0        /* ready: this phase, at its full length, not started          */
#define PM_RUN     1        /* counting down against the wall clock                        */
#define PM_PAUSE   2        /* held, its remaining kept                                    */
#define PM_WAIT    3        /* ended, chime owed                                           */
#define PM_WAITED  4        /* ended, already chimed (or the chime is off)                 */
#define PM_FOCUS   0        /* the three phases, and the index into len_s/cap_s/col         */
#define PM_SHORT   1
#define PM_LONG    2
#define PM_EV_NONE  0
#define PM_EV_END   1       /* a phase ended: sound the chime, flash                       */
#define PM_EV_START 2       /* a phase started on its own (autostart)                      */
#define PM_EV_LIMIT 4       /* the last turn hit an end and stopped                         */

/* The numbers this step fixes (design §4, §7). */
#define PM_MIN_S      60    /* a turn never goes below a minute                            */
#define PM_ARC_A    0xE6    /* the arc's base alpha (230)                                  */
#define PM_TRACK_A  0x33    /* the track: the same colour at 20%                           */
#define PM_BREATHE_MS 2500  /* the paused breath: rim-design's amendment 2, not a floor     */
#define PM_FLASH_MS   2000  /* twice a second: one second on, one off                      */
#define PM_STEP_PCT      4  /* the least fraction move worth a send (thousandths)          */
#define PM_PACK_MUL      8  /* pm_st = k * PM_PACK_MUL + state                             */
#define PM_PRESS_OFF       0  /* press_any's three choices, in app.json's order (§64.13): Off   */
#define PM_PRESS_CALL      1  /* leaves every press to the glass app, Calling borrows one only   */
#define PM_PRESS_EVERY     2  /* once a phase waits, Whenever borrows one in any live phase      */

#define PM_N_MIN   1        /* the cycle's bounds (§4): N focuses, 2N slots                 */
#define PM_N_MAX   12       /* pm_st's ceiling at 23*8 + 4 = 188 < 200                      */
#define PM_DOT_PITCH 17     /* the dots bank's cell pitch: pomodoro-bake.js DOT_SZ + DOT_GAP;
                             * bake_check.py asserts the baked rows stand at this pitch     */
#define PM_TEXT    24       /* the longest word this file writes, plus a NUL                */

typedef struct {
    int32_t len_s[3];       /* the three length settings, seconds                           */
    int32_t cap_s[3];       /* each one's max, seconds: the turn's ceiling                  */
    int32_t focuses;        /* N, 1..12                                                     */
    int32_t autostart;      /* 0 wait for a press, 1 chain on                                */
    int32_t ring_on;        /* 0 => the ring is cleared, not invisible                       */
    int32_t opacity;        /* ring_op, 0..100                                               */
    int32_t dir;            /* ring_dir: 0 clockwise from twelve, 1 anticlockwise (§8)        */
    int32_t start;          /* ring_start as degrees: 0/90/180/270 clockwise from twelve      */
    int32_t match;          /* "Match the app" while off the glass: the ORB_RING_MATCH ask     */
    uint32_t col[3];        /* 0xRRGGBB per phase, from the color settings                   */
    int32_t press_any;      /* PM_PRESS_OFF/CALL/EVERY: when the app may borrow the press      */
} pm_cfg_t;

typedef struct {
    int16_t st;             /* PM_STOP .. PM_WAITED                                          */
    int16_t k;              /* the cycle slot, 0 .. 2N-1                                     */
    int32_t len;            /* this phase's length, seconds (the ring's denominator)          */
    int32_t rem;            /* remaining, seconds (its meaning follows st)                    */
    int64_t end_s;          /* the wall second a PM_RUN phase ends; 0 while not yet anchored   */
} pm_t;

typedef struct { int32_t st_k, len, end, rem; } pm_raw_t;   /* the four hidden settings (§5)  */

/* What the ring should be: orb_ring_t's five values in that order and meaning (the arc's and
 * the track's alpha ride bits 31..24), plus the two words rim-1 adds and the ask that comes with
 * one of them, named plainly so this file never speaks of the SDK. pomodoro-2 copies the eight
 * fields across, as rim-1's orb_ring_rec mirrors orb.h: `dir` is the anticlockwise ask, `start`
 * the degrees it begins at and `match` the theme ask — which the caller sets only while the app is
 * off the glass, the way amendment H's colour choice already is. */
typedef struct {
    int fraction;           /* 0..1000, from twelve in this record's own direction            */
    int effect;             /* pm_ring_effect's answer                                        */
    uint32_t arc;           /* 0xAARRGGBB                                                     */
    uint32_t track;         /* 0xAARRGGBB                                                     */
    int period_ms;          /* 0 for a steady ring                                            */
    int dir;                /* 0 clockwise from the start, 1 anticlockwise (ORB_RING_CCW)     */
    int start;              /* degrees clockwise from twelve the arc begins at, folded 0..359  */
    int match;              /* 1 asks the ring to wear the live theme's overlay colour        */
    int backed;             /* 1 asks the whole rim band black with the arc on it (alerts §64.12) */
} pm_ring_t;
_Static_assert(offsetof(pm_ring_t, effect) == sizeof(int) &&
               offsetof(pm_ring_t, arc) == 2 * sizeof(int) &&
               offsetof(pm_ring_t, track) == 3 * sizeof(int) &&
               offsetof(pm_ring_t, dir) == 5 * sizeof(int) &&
               offsetof(pm_ring_t, start) == 6 * sizeof(int) &&
               offsetof(pm_ring_t, match) == 7 * sizeof(int) &&
               offsetof(pm_ring_t, backed) == 8 * sizeof(int),
               "pm_ring_t is fraction, effect, arc, track, period_ms, dir, start, match, backed in that order");

/* ---- the cycle (§2) ------------------------------------------------------------------ */
int pm_cycle_len(int focuses);                       /* 2N, with N clamped into 1..12        */
int pm_phase_at(int k, int focuses);                 /* PM_FOCUS|PM_SHORT|PM_LONG            */
int pm_focus_at(int k, int focuses);                 /* 1..N, 0 on either break              */
/* What the two dots rows show for slot k of a cycle of `focuses` focuses (alerts-0 amendment
 * 2): *cells is N clamped to 1..12, *filled the focuses the cycle counts — the one in hand
 * during a focus (so the third focus reads 3 of 4), the ones done during a short break, the
 * whole set during the long break. The track row (slot 13: 12 frames of N hollow cells) is
 * picked at *cells - 1, the fill row (slot 14: 13 frames of n filled cells, frame 0 empty)
 * at *filled; both are left-aligned banks the app centres with one shared pose offset,
 * (PM_N_MAX - *cells) * PM_DOT_PITCH / 2. Both arguments are clamped, never trusted. */
void pm_dots(int k, int focuses, int *cells, int *filled);
int pm_next_k(int k, int focuses);                   /* wraps at the cycle's end             */
int pm_len_s(const pm_cfg_t *c, int phase);          /* the setting, into PM_MIN_S .. cap    */

/* ---- the gestures (§6) --------------------------------------------------------------- */
/* Start, pause, resume, or — over and waiting — the next phase at its setting's length. A
 * press into PM_RUN leaves end_s 0: this file has no clock, so the caller's wall second
 * anchors it, which pm_service does on its first pass and pm_turn does at once. */
void pm_press(pm_t *s, const pm_cfg_t *c);
/* 60 s a click, both ways, between PM_MIN_S and cap_s[phase], PM_EV_LIMIT when a click asked
 * for more than that end allowed. A PM_RUN phase keeps its elapsed arc (len moves with rem);
 * a STOP or PAUSE phase takes the turned value for its whole length, and a turn over a
 * finished phase ends the waiting and holds it: PM_PAUSE. */
int  pm_turn(pm_t *s, const pm_cfg_t *c, int delta, int64_t now_s);
int  pm_at_start(const pm_t *s);                     /* this phase, none of it gone yet      */
/* The press-and-turn ladder off the phase in hand: 0 this phase as it stands, -1 this phase
 * back at its start, -2 the phase before, +1 the next. The ends stop, and a phase already at
 * its start has nothing left in -1, so its first anticlockwise click is -2 (§6.3). */
int  pm_pos_step(int pos, int delta, int at_start, int k, int focuses);
/* The let-go: STOP at the phase pos names, at that phase's setting length. 0 changes nothing. */
void pm_land(pm_t *s, const pm_cfg_t *c, int pos);

/* ---- the service and the store (§5) --------------------------------------------------- */
/* One wall-clock pass. PM_EV_NONE before the end, with rem following the seconds; PM_EV_END
 * at or after it, leaving PM_WAIT (or, with autostart, PM_RUN at the next k and answering
 * PM_EV_END|PM_EV_START). Any other state — including a second pass over the WAIT this pass
 * just made — answers PM_EV_NONE, which is what keeps a boundary chiming exactly once. */
int  pm_service(pm_t *s, const pm_cfg_t *c, int64_t now_s);
/* The four hidden settings back into a state (§5.2): nothing stored is trusted, and a run the
 * clock cannot place comes back PAUSE. Silent whatever it finds — a boundary missed entirely
 * comes back PM_WAITED, never a chime and never the phases it would have led. */
void pm_restore(pm_t *s, const pm_cfg_t *c, const pm_raw_t *raw, int synced, int64_t now_s);
/* 1 when the stored state is a running phase: its end is a wall second, so restoring it
 * needs a synced clock, and until then the caller must hold the record, not overwrite it. */
int pm_raw_needs_clock(const pm_raw_t *raw);
void pm_store(const pm_t *s, pm_raw_t *raw);
int  pm_changed(const pm_raw_t *a, const pm_raw_t *b);   /* 0 => write nothing */

/* Whether the app may hold the press the OS lends (§64.13, alerts-design §4.2): 1 asks for the
 * claim, 0 asks for it back. A stopped phase never borrows a press — nothing is running and
 * nothing is calling — and the answer is read afresh every service pass, so a phase ending or a
 * settings move claims and releases on its own. This is the whole rule; the caller does the
 * asking, and the OS answers 0 to both halves for an app that never declared the cap. */
int  pm_press_want(const pm_cfg_t *c, const pm_t *s);

/* ---- what the ring and the glass should show (§7, §10.1) ----------------------------- */
int  pm_ring_alpha(int base_alpha, int opacity);     /* base*op/100, rounded down            */
int  pm_ring_phase(const pm_t *s, int focuses);      /* whose colour this state wears        */
int  pm_ring_effect(const pm_t *s);   /* 0 ORB_RING_STEADY, 1 ORB_RING_FLASH, 2
                                         ORB_RING_BREATHE, or -1 for "clear the ring"       */
/* 1 to set (out filled), 0 to clear: PM_STOP, a ring_on of 0 and an opacity of 0 all clear
 * rather than own an invisible record. pm_ring_effect's -1 is the state's half of that. */
int  pm_ring_for(const pm_t *s, const pm_cfg_t *c, pm_ring_t *out);
int  pm_fraction(const pm_t *s);                     /* 0..1000, rem*1000/len, never divides */
void pm_state_text(const pm_t *s, char *buf, int cap);  /* "" | "Paused" | "Waiting"        */
void pm_phase_text(const pm_t *s, int focuses, char *buf, int cap);  /* "Focus", a break     */
void pm_count_text(const pm_t *s, int focuses, char *buf, int cap);  /* "3 of 4" | "break"  */
