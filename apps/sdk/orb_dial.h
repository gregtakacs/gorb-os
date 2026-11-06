/* orb_dial.h — one dial per app, centred on its home (docs/app-platform-plan.md §51).
 *
 * The positions run lo..hi with 0 as home: one kind of thing clockwise of it, another
 * anticlockwise. Header-only and free of imports, so an app that keeps no dial pays
 * nothing, and tools/host_dial_test.py compiles it as it stands.
 *
 *   orb_dial_t d = { .lo = -5, .hi = 23, .idle_ms = ORB_DIAL_IDLE_MS };
 *   orb_input:  if (orb_dial_turn(&d, delta, orb_uptime_us() / 1000)) redraw();
 *   orb_tick:   if (orb_dial_idle(&d, orb_uptime_us() / 1000)) redraw();
 *
 * - Each end stops; nothing wraps.
 * - Home is sticky: a fast step (|delta| > 1, which orb_knob_dial's acceleration
 *   produces only on a quick spin) that would reach or cross home stops on it, and the
 *   rest of that spin is swallowed. A pause or a turn the other way lets go.
 * - Idle returns home after idle_ms (0 = never).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ORB_DIAL_IDLE_MS    20000   /* §51: a glance shows home */
#define ORB_DIAL_SPIN_GAP_MS  250   /* detents closer than this are one spin */

typedef struct {
    int32_t lo, hi;      /* the ends, lo <= 0 <= hi */
    int32_t pos;         /* 0 = home */
    uint32_t idle_ms;
    int64_t last_ms;     /* uptime of the last turn */
    int8_t  held;        /* a spin stopped on home: its direction, swallowed while it lasts */
} orb_dial_t;

/* Move by one knob event. True when the position changed. */
static inline bool orb_dial_turn(orb_dial_t *d, int32_t delta, int64_t now_ms)
{
    if (!delta) return false;
    const int8_t dir = delta > 0 ? 1 : -1;
    const bool same_spin = now_ms - d->last_ms < ORB_DIAL_SPIN_GAP_MS;
    d->last_ms = now_ms;
    if (d->held) {
        if (d->held == dir && same_spin) return false;
        d->held = 0;
    }
    int32_t to = d->pos + delta;
    const bool fast = delta > 1 || delta < -1;
    if (fast && ((d->pos > 0 && to <= 0) || (d->pos < 0 && to >= 0))) {
        to = 0;
        d->held = dir;
    }
    if (to < d->lo) to = d->lo;
    if (to > d->hi) to = d->hi;
    const bool moved = to != d->pos;
    d->pos = to;
    return moved;
}

/* Back home once idle_ms has passed since the last turn. True when it moved. */
static inline bool orb_dial_idle(orb_dial_t *d, int64_t now_ms)
{
    if (!d->idle_ms || !d->pos || now_ms - d->last_ms < (int64_t)d->idle_ms) return false;
    d->pos = 0;
    d->held = 0;
    return true;
}

/* New ends (the data behind them changed); the position is kept inside them. */
static inline void orb_dial_range(orb_dial_t *d, int32_t lo, int32_t hi)
{
    d->lo = lo > 0 ? 0 : lo;
    d->hi = hi < 0 ? 0 : hi;
    if (d->pos < d->lo) d->pos = d->lo;
    if (d->pos > d->hi) d->pos = d->hi;
}

static inline void orb_dial_home(orb_dial_t *d) { d->pos = 0; d->held = 0; }
