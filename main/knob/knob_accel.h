/* knob_accel.h — the knob's acceleration for an app that takes every turn (plan §51).
 *
 * Slow clicks stay one step each; detents that follow each other quickly in one
 * direction count double, then four times. The app sees |delta| > 1 only on a fast spin,
 * which is also how apps/sdk/orb_dial.h knows to stop a spin on the app's home. Pure,
 * so tools/host_knob_accel_test.py compiles it as it stands. The two gaps are a first
 * cut: a deliberate click is 200-300 ms from the last, a flick 25-50 ms. */
#pragma once
#include <stdint.h>

#define KNOB_ACCEL_X4_US 45000
#define KNOB_ACCEL_X2_US 90000

typedef struct {
    int64_t last_us;   /* when the last detent landed; 0 = none yet */
    int8_t  dir;       /* its direction */
} knob_accel_t;

static inline int32_t knob_accel(knob_accel_t *a, int32_t delta, int64_t now_us)
{
    if (!delta) return 0;
    const int8_t dir = delta > 0 ? 1 : -1;
    const int64_t gap = a->last_us ? now_us - a->last_us : INT64_MAX;
    int32_t mult = 1;
    if (dir == a->dir) {
        if (gap < KNOB_ACCEL_X4_US) mult = 4;
        else if (gap < KNOB_ACCEL_X2_US) mult = 2;
    }
    a->last_us = now_us;
    a->dir = dir;
    return delta * mult;
}
