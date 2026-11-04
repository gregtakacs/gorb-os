/* knob_edit.h — a value turned by the knob in the shell's edit mode
 * (docs/knob-edit-plan.md §1.1).
 *
 * Pure C, no LVGL and no clock: the caller hands in the time of each turn, so
 * tools/host_knob_edit_test.py drives it on a host. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The colour picker's speed model (apps/color_picker/main.c): detents a second, smoothed,
 * 1 per detent when slow and up to the row's largest step when fast, slow again after a
 * pause. */
#define KNOB_EDIT_SLOW_DPS 6.0f
#define KNOB_EDIT_FAST_DPS 30.0f
#define KNOB_EDIT_IDLE_US  220000
#define KNOB_EDIT_EMA      0.35f

typedef struct {
    int32_t min, max;     /* inclusive */
    bool    wrap;         /* past an end comes round to the other (an hour of the day) */
    int32_t step_max;     /* the largest step a fast turn takes; 1 = never faster */
} knob_range_t;

typedef struct {
    int64_t last_us;      /* the last turn, 0 = none yet */
    float   dps;          /* smoothed detents a second */
} knob_speed_t;

/** Forget the turn speed: the next turn is a slow one. */
void knob_speed_reset(knob_speed_t *s);

/** The step for a turn of `delta` detents at `now_us`: 1..step_max. */
int32_t knob_speed_step(knob_speed_t *s, int32_t delta, int64_t now_us, int32_t step_max);

/** `v` moved by `delta` detents at `step` each. A step above 1 lands on the next multiple
 *  of itself in the turn's direction, so a fast turn rests on round numbers. The result
 *  clamps to [min, max], or wraps when the range does. */
int32_t knob_edit_move(int32_t v, int32_t delta, int32_t step, const knob_range_t *r);
