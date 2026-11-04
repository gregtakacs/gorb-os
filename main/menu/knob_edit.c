/* knob_edit.c — a value turned by the knob (docs/knob-edit-plan.md §1.1). See knob_edit.h. */
#include "knob_edit.h"

void knob_speed_reset(knob_speed_t *s)
{
    s->last_us = 0;
    s->dps = 0.0f;
}

int32_t knob_speed_step(knob_speed_t *s, int32_t delta, int64_t now_us, int32_t step_max)
{
    if (s->last_us == 0 || now_us - s->last_us > KNOB_EDIT_IDLE_US) {
        s->dps = 0.0f;
    } else if (now_us > s->last_us) {
        const float dt = (float)(now_us - s->last_us) / 1e6f;
        const float inst = (float)(delta < 0 ? -delta : delta) / dt;
        s->dps += KNOB_EDIT_EMA * (inst - s->dps);
    }
    s->last_us = now_us;
    if (step_max <= 1 || s->dps <= KNOB_EDIT_SLOW_DPS) return 1;
    if (s->dps >= KNOB_EDIT_FAST_DPS) return step_max;
    const float t = (s->dps - KNOB_EDIT_SLOW_DPS) / (KNOB_EDIT_FAST_DPS - KNOB_EDIT_SLOW_DPS);
    int32_t m = 1 + (int32_t)(t * (float)(step_max - 1) + 0.5f);
    return m < 1 ? 1 : m > step_max ? step_max : m;
}

/* floor(a / b) for b > 0, whatever a's sign */
static int32_t floor_div(int32_t a, int32_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int32_t knob_edit_move(int32_t v, int32_t delta, int32_t step, const knob_range_t *r)
{
    if (step < 1) step = 1;
    for (int32_t n = delta < 0 ? -delta : delta; n > 0; n--) {
        if (step == 1) v += delta > 0 ? 1 : -1;
        else if (delta > 0) v = (floor_div(v, step) + 1) * step;           /* next multiple up */
        else v = (floor_div(v + step - 1, step) - 1) * step;               /* next multiple down */
        if (r->wrap) {
            const int32_t span = r->max - r->min + 1;
            v = r->min + ((v - r->min) % span + span) % span;
        } else if (v < r->min) {
            v = r->min;
        } else if (v > r->max) {
            v = r->max;
        }
    }
    return v;
}
