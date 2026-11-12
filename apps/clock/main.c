/**
 * clock — the dial's behaviour, as an app (apps/sdk/orb.h, apps/sdk/orb_stage.h).
 *
 * The OS draws the themed dial: plate, delta bank, rotated sprites, complication
 * windows and strips, glass, damage marks, frame rate (the stage, main/stage/
 * orb_stage.c). Everything that decides what the dial SAYS is here: how each row is
 * driven (D41's source, path, cadence and ease, the parent it rides, the twin that
 * follows it), what each complication reads, when the tick loop runs and which chime
 * or stroke sounds at which instant (D39), the minute repeater on a press, and the
 * face a dial with no theme shows (D31: the app supplies the default).
 *
 * Moved out of the firmware by §43. The functions below the "lifted" line are the
 * firmware's own policy code, carried over verbatim (only the calls into the OS are
 * respelled as orb.* imports), so a theme poses and sounds exactly as it did.
 */
#include "orb_stage.h"

/* The clock theme's sound slots and hour patterns: the meaning this app gives its
 * theme's numbered sound slots (main/art/orb_clock3.h names the same numbers). */
enum {
    CLK_SND_TICK_LOOP = 0, CLK_SND_CHIME_Q1, CLK_SND_CHIME_HALF, CLK_SND_CHIME_Q3,
    CLK_SND_CHIME_HOUR, CLK_SND_STRIKE_HOUR, CLK_SND_JUMP_MINUTE, CLK_SND_JUMP_HOUR,
    CLK_SND_JUMP_DATE, CLK_SND_MORSE_DIT, CLK_SND_MORSE_DAH, CLK_SND_STRIKE_QUARTER,
    CLK_SND_STRIKE_MINUTE, CLK_SND_SLOTS
};
enum {
    CLK_PAT_COUNT = 0, CLK_PAT_SHIPS_BELLS, CLK_PAT_MORSE, CLK_PAT_REPEATER,
    CLK_PAT_NONE, CLK_PAT_N
};
#define CLK_SND_RATE ORB_STAGE_SND_RATE

typedef struct {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
} tm_t;
typedef int64_t time_t;

static bool localtime_of(time_t t, tm_t *out)
{
    orb_tm_t o;
    if (orb_localtime(t, &o) != ORB_OK) return false;
    out->tm_sec = o.sec;   out->tm_min = o.min;   out->tm_hour = o.hour;
    out->tm_mday = o.mday; out->tm_mon = o.mon;   out->tm_year = o.year;
    out->tm_wday = o.wday; out->tm_yday = o.yday; out->tm_isdst = o.isdst;
    return true;
}

/* ---- the theme, as the stage handed it over --------------------------------------- */

typedef struct {
    clk_row_t  rows[CLK_ROW_MAX];
    clk_comp_t comps[CLK_COMP_MAX];
    uint16_t   n_rows, n_comps;
} geo_t;
static geo_t           G;
static orb_stage_info_t s_info;
#define SND_MAX 32
static clk_sound_t     s_snds[SND_MAX];
static uint32_t        s_nsnd;
static uint8_t         s_pattern = CLK_PAT_NONE;

/* ---- the firmware's policy vocabulary (orb_stage.c), kept with its code ---------- */

typedef struct {
    float    shown, from, to, amp;
    uint32_t t0;
    uint8_t  kind;
    bool     init;
} ease_t;
enum { EASE_NONE = 0, EASE_SETTLE, EASE_FLY };
#define EASE_SETTLE_DEG   1.5f
#define EASE_SETTLE_PX    3.0f

typedef struct {
    const tm_t *ti;
    time_t      t;
    double      frac;
} drive_now_t;

#define MOON_NEW_MOON_S  947182440LL
#define MOON_SYNODIC_S   2551443LL        /* 29.530588853 d */

typedef struct {
    int     loop;
    int64_t loop_anchor;
    int     min, hour, mday;
    time_t  last_t;
    int64_t queued[3];
    uint8_t steps[3];
    bool    steps_known;
} snd_state_t;
static snd_state_t  s_snd_store;
static snd_state_t *const s_snd = &s_snd_store;
static bool         s_snd_init;
enum { SND_STEP_MIN, SND_STEP_HOUR, SND_STEP_CAL };

/* The owner's two levels (app.json tick_vol, chime_vol; §59): the ticking and the
 * mechanism's clicks at one, every chime and strike at the other, both under the OS
 * volume. Re-read when the settings generation moves; a tick level that changed
 * restarts the loop, which re-enters at the second it is on. */
static int32_t  s_tick_vol = 70, s_chime_vol = 100;
static uint32_t s_cfg_gen;
static bool     s_cfg_read;
static bool snd_levels_read(void)
{
    const uint32_t g = orb_cfg_gen();
    if (s_cfg_read && g == s_cfg_gen) return false;
    const int32_t was = s_tick_vol;
    s_cfg_gen = g; s_cfg_read = true;
    (void)orb_cfg_get("tick_vol", &s_tick_vol);
    (void)orb_cfg_get("chime_vol", &s_chime_vol);
    return s_tick_vol != was;
}
static int32_t snd_level(int slot)
{
    return (slot == CLK_SND_TICK_LOOP || slot == CLK_SND_JUMP_MINUTE ||
            slot == CLK_SND_JUMP_HOUR || slot == CLK_SND_JUMP_DATE) ? s_tick_vol : s_chime_vol;
}

/* The voices: the app's own theme sounds, by table index, leased by the stage. */
static void snd_at_flags(const clk_sound_t *s, int64_t at_us, uint32_t extra)
{
    if (s) (void)orb_stage_snd_play_v((int)(s - s_snds), extra, at_us, 0, snd_level(s->slot));
}
static void snd_at(const clk_sound_t *s, int64_t at_us) { snd_at_flags(s, at_us, 0); }

/* ---- lifted ------------------------------------------------------------------------ */

static float ease_wrap(float d, float turn)
{
    if (turn <= 0.0f) return d;
    while (d >  turn / 2) d -= turn;
    while (d <= -turn / 2) d += turn;
    return d;
}

static float ease_pose(ease_t *e, float target, uint32_t now_ms, uint16_t ms, bool stepping,
                       bool retro, int span_sign, float turn, float amp, uint32_t *busy_ms)
{
    *busy_ms = 0;
    if (!ms) { e->init = false; return target; }
    if (!e->init) {
        e->init = true; e->to = e->shown = target; e->kind = EASE_NONE;
        return target;
    }
    if (target != e->to) {
        const float d = ease_wrap(target - e->to, turn);
        const bool back = retro && (span_sign >= 0 ? d < 0.0f : d > 0.0f);
        if (back) { e->kind = EASE_FLY; e->from = e->shown; e->t0 = now_ms; }
        else if (e->kind == EASE_FLY && now_ms - e->t0 < ms) {
            /* a sweeping hand's target creeps on every frame of its flyback: the fly keeps
             * its start and clock and lands on wherever the sweep has got to */
        }
        else if (stepping) { e->kind = EASE_SETTLE; e->amp = d > 0.0f ? amp : -amp; e->t0 = now_ms; }
        else { e->kind = EASE_NONE; e->t0 = now_ms; }
        e->to = target;
    }
    if (e->kind == EASE_NONE) { e->shown = target; return target; }
    const uint32_t gone = now_ms - e->t0;
    if (gone >= ms) { e->kind = EASE_NONE; e->shown = target; return target; }
    *busy_ms = ms - gone;
    const float tau = (float)gone / (float)ms;
    if (e->kind == EASE_SETTLE) {
        e->shown = target + e->amp * sinf((float)M_PI * tau);
    } else {
        /* back along the fan, the way the span runs reversed -- never the short way
         * round the dial, which on a wide fan would sweep through the empty side */
        float d = target - e->from;
        if (turn > 0.0f) {
            if (span_sign >= 0) { while (d > 0.0f) d -= turn; }
            else                { while (d < 0.0f) d += turn; }
        }
        const float sm = tau * tau * (3.0f - 2.0f * tau);
        e->shown = e->from + d * sm;
    }
    return e->shown;
}

static bool cad_steps_slow(uint8_t cad, uint8_t src)
{
    return (cad == CLK_CAD_JUMP || cad == CLK_CAD_STEP) && src != CLK_SRC_SECOND;
}

static bool comp_year_leap(int y) { return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0); }

static int32_t comp_iso_week(const tm_t *ti)
{
    const int yday1 = ti->tm_yday + 1;
    const int wd7   = (ti->tm_wday + 6) % 7 + 1;           /* Mon==1 .. Sun==7 */
    const int pdays = comp_year_leap(ti->tm_year + 1900) ? 366 : 365;
    const int th    = yday1 + 4 - wd7;                     /* week's Thursday  */
    if (th > pdays) return 1;                              /* next year's W1   */
    if (th >= 1)    return (yday1 + 10 - wd7) / 7;
    const int py    = ti->tm_year + 1900 - 1;
    const int pyd   = comp_year_leap(py) ? 366 : 365;
    const int jan1  = ((wd7 - 1 - (ti->tm_yday % 7)) % 7 + 7) % 7 + 1;
    const int d31   = ((jan1 - 2) % 7 + 7) % 7 + 1;        /* prev Dec 31      */
    const int d28   = ((d31 - 4) % 7 + 7) % 7 + 1;         /* three days on    */
    return (pyd - 3 + 10 - d28) / 7;
}

static bool comp_value(uint8_t src, const tm_t *ti, time_t t, int32_t *out)
{
    switch (src) {
    case CLK_SRC_NONE:        *out = 0;                            break;  /* a FORMAT 3 static window: one plate, frame 0 (D32 R-B: no fixed domain to consult, the row is one tile) */
    case CLK_SRC_WEEKDAY:     *out = (ti->tm_wday + 6) % 7;   break;
    case CLK_SRC_MONTH:       *out = ti->tm_mon;              break;
    case CLK_SRC_DATE:        *out = ti->tm_mday;             break;
    case CLK_SRC_HOUR12:      *out = ti->tm_hour % 12;        break;
    case CLK_SRC_HOUR24:      *out = ti->tm_hour;             break;
    case CLK_SRC_MINUTE:      *out = ti->tm_min;              break;
    case CLK_SRC_SECOND:      *out = ti->tm_sec;              break;
    case CLK_SRC_AMPM:        *out = ti->tm_hour >= 12;       break;
    case CLK_SRC_WEEK:        *out = comp_iso_week(ti);       break;
    case CLK_SRC_DAY_OF_YEAR: *out = ti->tm_yday + 1;         break;
    case CLK_SRC_QUARTER:     *out = ti->tm_mon / 3 + 1;      break;
    case CLK_SRC_YEAR:        *out = ti->tm_year + 1900;      break;
    case CLK_SRC_MOON_AGE: {
        int64_t m = ((int64_t)t - MOON_NEW_MOON_S) % MOON_SYNODIC_S;
        if (m < 0) m += MOON_SYNODIC_S;
        *out = (int32_t)(m / 86400);
        break; }
    default:                  return false;  /* none/text/value/battery/reserve */
    }
    return true;
}

static double drive_finer(uint8_t src, const tm_t *ti, time_t t, double frac, double *unit_min)
{
    const double day = ((double)ti->tm_hour * 3600.0 + ti->tm_min * 60.0 + ti->tm_sec + frac)
                       / 86400.0;
    switch (src) {
    case CLK_SRC_SECOND:  *unit_min = 1.0 / 60.0; return frac;
    case CLK_SRC_MINUTE:  *unit_min = 1.0;        return (ti->tm_sec + frac) / 60.0;
    case CLK_SRC_HOUR12:
    case CLK_SRC_HOUR24:  *unit_min = 60.0;
                          return (ti->tm_min * 60.0 + ti->tm_sec + frac) / 3600.0;
    case CLK_SRC_DATE:
    case CLK_SRC_WEEKDAY:
    case CLK_SRC_DAY_OF_YEAR: *unit_min = 1440.0; return day;
    case CLK_SRC_MONTH: {
        static const uint8_t DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int dim = DIM[ti->tm_mon % 12];
        if (ti->tm_mon == 1 && comp_year_leap(ti->tm_year + 1900)) dim = 29;
        *unit_min = 1440.0 * dim;
        return (ti->tm_mday - 1 + day) / dim; }
    case CLK_SRC_MOON_AGE: {          /* the moon's day: comp_value's age is whole days */
        int64_t m = ((int64_t)t - MOON_NEW_MOON_S) % MOON_SYNODIC_S;
        if (m < 0) m += MOON_SYNODIC_S;
        *unit_min = 1440.0;
        return ((double)(m % 86400) + frac) / 86400.0; }
    case CLK_SRC_YEAR: {
        const int ydays = comp_year_leap(ti->tm_year + 1900) ? 366 : 365;
        *unit_min = 1440.0 * ydays;
        return (ti->tm_yday + day) / ydays; }
    default:              *unit_min = 0.0;        return 0.0;
    }
}

/* D43 P5d.3: a digit reads the number the dial shows, not the source's index: a 12-hour
 * source's 0 is 12 and a month's 0..11 is 1..12. */
static int32_t digit_of(uint8_t src, uint8_t digit, int32_t v)
{
    if (digit == CLK_DIGIT_WHOLE) return v;
    if (src == CLK_SRC_HOUR12) v = v ? v : 12;
    else if (src == CLK_SRC_MONTH) v = v + 1;
    switch (digit) {
    case CLK_DIGIT_UNITS:     return v % 10;
    case CLK_DIGIT_TENS:      return v / 10 % 10;
    case CLK_DIGIT_HUNDREDS:  return v / 100 % 10;
    default:                  return v / 1000 % 10;
    }
}

/* The value the next unit starts on, from the calendar rather than v + 1: the 31st
 * runs on to the 1st and so does the 28th of a February. A roll needs it to know
 * whether its figure is about to change. */
static bool comp_next(uint8_t src, const tm_t *ti, int32_t v, int32_t *out)
{
    static const uint8_t DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    switch (src) {
    case CLK_SRC_WEEKDAY: *out = (v + 1) % 7;  break;
    case CLK_SRC_MONTH:   *out = (v + 1) % 12; break;
    case CLK_SRC_DATE: {
        int dim = DIM[ti->tm_mon % 12];
        if (ti->tm_mon == 1 && comp_year_leap(ti->tm_year + 1900)) dim = 29;
        *out = v >= dim ? 1 : v + 1;
        break; }
    case CLK_SRC_HOUR12:  *out = (v + 1) % 12; break;
    case CLK_SRC_HOUR24:  *out = (v + 1) % 24; break;
    case CLK_SRC_MINUTE:
    case CLK_SRC_SECOND:  *out = (v + 1) % 60; break;
    case CLK_SRC_DAY_OF_YEAR:
        *out = v >= (comp_year_leap(ti->tm_year + 1900) ? 366 : 365) ? 1 : v + 1;
        break;
    case CLK_SRC_YEAR:    *out = v + 1;        break;
    case CLK_SRC_MOON_AGE: *out = v + 1;       break;   /* a moon disc's next face, never back to 0 */
    default:              return false;
    }
    return true;
}

static bool drive_units(const clk_row_t *r, const drive_now_t *n, double *out)
{
    int32_t v;
    if (!comp_value(r->src, n->ti, n->t, &v)) return false;
    double unit_min;
    double f = drive_finer(r->src, n->ti, n->t, n->frac, &unit_min);
    if (r->cadence == CLK_CAD_ROLL) {
        /* D43 P5d.3: hold the figure; in the last unit before it changes, roll on to
         * the next one, forward round the wheel's vmax - vmin figures (an odometer). */
        const int32_t figs = (int32_t)r->value_max - (int32_t)r->value_min;
        int32_t nx;
        const int32_t a = digit_of(r->src, r->digit, v);
        if (figs <= 0 || !comp_next(r->src, n->ti, v, &nx)) { *out = a; return true; }
        const int32_t b = digit_of(r->src, r->digit, nx);
        *out = a + (double)((((b - a) % figs) + figs) % figs) * f;
        return true;
    }
    if (r->digit == CLK_DIGIT_UNITS) {
        v = digit_of(r->src, r->digit, v);
    } else if (r->digit != CLK_DIGIT_WHOLE) {
        /* D42 §4: a higher digit's sweep counts the lower ones toward its next figure */
        const int32_t place = r->digit == CLK_DIGIT_TENS ? 10 : r->digit == CLK_DIGIT_HUNDREDS ? 100 : 1000;
        int32_t w = r->src == CLK_SRC_HOUR12 ? (v ? v : 12) : r->src == CLK_SRC_MONTH ? v + 1 : v;
        f = ((w % place) + f) / (double)place;
        unit_min *= (double)place;
        v = digit_of(r->src, r->digit, v);
    }
    double shown = 0.0;
    switch (r->cadence) {
    case CLK_CAD_SWEEP:
        shown = f;
        break;
    case CLK_CAD_STEP: {
        const unsigned steps = r->cad_arg ? r->cad_arg : 1;   /* refused 0 at the door */
        shown = floor(f * steps) / steps;
        break; }
    case CLK_CAD_STOP_TO_GO: {
        /* The whole dial in P tenths of a second, then a rest at the top until the
         * minute releases it: the value runs fast and stops at value_max. */
        const double p = r->cad_arg ? r->cad_arg : 585;
        const double run = (v + f) * 600.0 / p;
        *out = run < r->value_max ? run : r->value_max;
        return true; }
    case CLK_CAD_DRAG: {
        /* Held, then a creep across the last cad_arg minutes of the unit. */
        const double w = r->cad_arg ? r->cad_arg : 120;
        if (unit_min > 0.0) {
            const double into = f * unit_min - (unit_min - (w < unit_min ? w : unit_min));
            shown = into > 0.0 ? into / (w < unit_min ? w : unit_min) : 0.0;
        }
        break; }
    default:                                    /* JUMP: the boundary only */
        break;
    }
    *out = v + shown;
    return true;
}

static double drive_t(const clk_row_t *r, double units)
{
    const double w = (double)r->value_max - (double)r->value_min;
    double t = w > 0.0 ? (units - (double)r->value_min) / w : 0.0;
    if (r->end == CLK_END_WRAP) {
        t -= floor(t);
    } else {
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
    }
    return t;
}

static void row_pose(const clk_row_t *r, double t, int32_t *ang_10, int16_t *ox, int16_t *oy)
{
    const double a = (double)r->zero + t * (double)r->span;
    int32_t ai = (int32_t)(a >= 0.0 ? floor(a + 0.5) : -floor(-a + 0.5));
    ai %= 3600;
    if (ai < 0) ai += 3600;
    *ang_10 = ai;
    const double x = t * (double)r->dx, y = t * (double)r->dy;
    *ox = (int16_t)(x >= 0.0 ? floor(x + 0.5) : -floor(-x + 0.5));
    *oy = (int16_t)(y >= 0.0 ? floor(y + 0.5) : -floor(-y + 0.5));
}

static double row_ease(const clk_row_t *r, ease_t *e, double t, uint32_t now, uint32_t *busy)
{
    *busy = 0;
    if (!r->ease) { e->init = false; return t; }
    const int32_t trav = abs(r->dx) > abs(r->dy) ? abs(r->dx) : abs(r->dy);
    const float amp = r->span ? EASE_SETTLE_DEG * 10.0f / (float)abs(r->span)
                              : (trav ? EASE_SETTLE_PX / (float)trav : 0.0f);
    float v = ease_pose(e, (float)t, now, r->ease, cad_steps_slow(r->cadence, r->src),
                        r->end == CLK_END_FLYBACK, 1, r->end == CLK_END_WRAP ? 1.0f : 0.0f,
                        amp, busy);
    if (r->end == CLK_END_WRAP) v -= floorf(v);
    return v;
}

static int snd_step_class(uint8_t src)
{
    switch (src) {
    case CLK_SRC_MINUTE: return SND_STEP_MIN;
    case CLK_SRC_HOUR12: case CLK_SRC_HOUR24: return SND_STEP_HOUR;
    case CLK_SRC_DATE: case CLK_SRC_WEEKDAY: case CLK_SRC_MONTH: case CLK_SRC_DAY_OF_YEAR:
        return SND_STEP_CAL;
    default: return -1;
    }
}

static uint8_t snd_cad_steps(uint8_t cad, uint16_t arg)
{
    if (cad == CLK_CAD_JUMP) return 1;
    if (cad == CLK_CAD_STEP) return (uint8_t)(arg ? (arg > 60 ? 60 : arg) : 1);
    return 0;
}

static void snd_note_step(uint8_t out[3], int cls, uint8_t n)
{
    if (cls >= 0 && n > out[cls]) out[cls] = n;
}

static void snd_steps_scan(uint8_t out[3])
{
    memset(out, 0, 3);
    const geo_t *g = &G;
    for (uint16_t i = 0; i < g->n_rows && i < CLK_ROW_MAX; i++) {
        const clk_row_t *r = &g->rows[i];
        if ((r->flags & CLK_FLAG_AUX_SHADOW) || !r->spr.show) continue;
        snd_note_step(out, snd_step_class(r->src), snd_cad_steps(r->cadence, r->cad_arg));
    }
    for (uint16_t i = 0; i < g->n_comps && i < CLK_COMP_MAX; i++)
        if (snd_step_class(g->comps[i].src) == SND_STEP_CAL) out[SND_STEP_CAL] = 1;
}

static const clk_sound_t *snd_find(const clk_sound_t *t, uint32_t n, int slot)
{
    for (uint32_t i = 0; i < n; i++)
        if (t[i].slot == slot) return &t[i];
    return NULL;
}

static int64_t snd_us(const clk_sound_t *s)      /* a sound's length in µs */
{
    return (int64_t)s->n * 1000000 / CLK_SND_RATE;
}

static int snd_pattern(const clk_sound_t *t, uint32_t n, uint8_t theme_pat)
{
    orb_stage_sound_cfg_t c;
    orb_stage_snd_cfg(&c);
    const int p = c.pattern;
    const bool strike = snd_find(t, n, CLK_SND_STRIKE_HOUR) != NULL;
    switch (p) {
    case CLK_PAT_COUNT: case CLK_PAT_SHIPS_BELLS:
        return strike ? p : theme_pat;
    case CLK_PAT_MORSE:
        return (snd_find(t, n, CLK_SND_MORSE_DIT) && snd_find(t, n, CLK_SND_MORSE_DAH))
               ? p : theme_pat;
    case CLK_PAT_REPEATER:
        return (strike && snd_find(t, n, CLK_SND_STRIKE_QUARTER) &&
                snd_find(t, n, CLK_SND_STRIKE_MINUTE)) ? p : theme_pat;
    case CLK_PAT_NONE:
        return p;
    default:
        return theme_pat;
    }
}

static void snd_hour_pattern(const clk_sound_t *t, uint32_t n, int pat,
                             int64_t t0, int h12, int hour24, int minute)
{
    const clk_sound_t *stroke = snd_find(t, n, CLK_SND_STRIKE_HOUR);
    switch (pat) {
    case CLK_PAT_COUNT:
        for (int i = 0; i < h12; i++) snd_at(stroke, t0 + (int64_t)i * 2000000);
        break;
    case CLK_PAT_SHIPS_BELLS: {
        /* Four-hour watches: 00:30 is one bell, 04:00 eight, 04:30 one again. */
        int bells = (hour24 % 4) * 2 + (minute >= 30 ? 1 : 0);
        if (!bells) bells = 8;
        for (int i = 0; i < bells; i++)
            snd_at(stroke, t0 + (int64_t)(i / 2) * 1600000 + (i % 2) * 450000);
        break; }
    case CLK_PAT_MORSE: {
        const clk_sound_t *dit = snd_find(t, n, CLK_SND_MORSE_DIT);
        const clk_sound_t *dah = snd_find(t, n, CLK_SND_MORSE_DAH);
        static const char *const DIGIT[10] = {
            "-----", ".----", "..---", "...--", "....-",
            ".....", "-....", "--...", "---..", "----.",
        };
        const int64_t unit = snd_us(dit);
        char num[12];
        snprintf(num, sizeof num, "%d", h12);
        int64_t at = t0;
        for (const char *d = num; *d; d++) {
            for (const char *e = DIGIT[*d - '0']; *e; e++) {
                const clk_sound_t *sym = *e == '.' ? dit : dah;
                snd_at(sym, at);
                at += snd_us(sym) + unit;              /* one unit between symbols */
            }
            at += 2 * unit;                            /* three between digits */
        }
        break; }
    case CLK_PAT_REPEATER:
        /* At the hour a repeater has no quarters and no minutes to sound. */
        for (int i = 0; i < h12; i++) snd_at(stroke, t0 + (int64_t)i * 900000);
        break;
    default:
        break;
    }
}

/* on_glass: the dial's tick runs it. Off the glass (theme-cache-plan §2.6) the service runs it
 * on wall time, and only the chimes and strikes sound: the tick loop and the hands' jumps are
 * the visible mechanism's, and stop with the glass. */
static void sound_policy(const tm_t *ti, time_t t, double frac, bool on_glass)
{
    const bool ready = orb_stage_snd_ready();   /* the engine up and the sound cap held */
    if (!s_snd_init) { s_snd->min = s_snd->hour = s_snd->mday = -1; s_snd_init = true; }
    if (!ready) {
        if (s_snd->loop) { orb_stage_snd_stop(s_snd->loop); s_snd->loop = 0; }
        return;
    }

    const uint32_t n = s_nsnd;
    const uint8_t theme_pat = s_pattern;
    const clk_sound_t *tab = s_snds;
    orb_stage_sound_cfg_t c;
    orb_stage_snd_cfg(&c);
    const bool quiet = c.volume == 0 || orb_stage_snd_quiet(ti->tm_hour);
    const int64_t sec_us = orb_uptime_us() - (int64_t)(frac * 1e6);   /* this second began */

    /* The tick loop. */
    const bool relevel = snd_levels_read();
    const clk_sound_t *loop = snd_find(tab, n, CLK_SND_TICK_LOOP);
    const bool want = on_glass && loop && c.tick && !quiet && s_tick_vol > 0;
    if (s_snd->loop && (relevel || !want || !orb_stage_snd_active(s_snd->loop))) {
        orb_stage_snd_stop(s_snd->loop);
        s_snd->loop = 0;
    }
    if (want) {
        /* Sample first_beat sounds on a wall second; the loop is a whole number of
         * seconds long, so any second is the anchor. Re-anchor when the wall clock
         * itself was re-anchored under the loop (orb_time_now's drift rule). */
        const int64_t anchor = sec_us - (int64_t)loop->first_beat * 1000000 / CLK_SND_RATE;
        if (!s_snd->loop) {
            s_snd->loop = orb_stage_snd_play_v((int)(loop - s_snds),
                                              ORB_STAGE_SND_LOOP | ORB_STAGE_SND_ANCHORED, 0, anchor,
                                              s_tick_vol);
            if (s_snd->loop < 0) s_snd->loop = 0;
            s_snd->loop_anchor = anchor;
        } else {
            /* Only the sub-second drift is corrected: the loop keeps its place in its own
             * pattern. A tick loop may be several seconds long (Skeleton's is 8 s), and
             * re-anchoring on "this second" shifted it by whole seconds -- the mixer saw
             * seconds of error and snapped, then snapped back on the next wobble: the
             * double and missing ticks the owner heard (Bronze, 2026-11-12). */
            int64_t d = (anchor - s_snd->loop_anchor) % 1000000;
            if (d < 0) d += 1000000;
            if (d >= 500000) d -= 1000000;             /* signed, -0.5 s .. +0.5 s */
            /* A one-frame glitch is not drift: the stage's wall fraction is now and then a
             * frame stale (156 ms on Skeleton's 6 fps), and following it re-anchored the loop
             * there and back, two snaps a beat (Bronze, 2026-11-12). Only a deviation that
             * holds, same sign, for three frames moves the anchor, and then by at most 20 ms
             * a frame, under the mixer's 50 ms snap, so it slews in silence. */
            static int s_drift_n;
            if (d > 2000 || d < -2000) {
                s_drift_n = (s_drift_n > 0) == (d > 0) ? s_drift_n + (d > 0 ? 1 : -1) : (d > 0 ? 1 : -1);
                if (s_drift_n >= 3 || s_drift_n <= -3) {
                    s_snd->loop_anchor += d > 20000 ? 20000 : d < -20000 ? -20000 : d;
                    orb_stage_snd_reanchor(s_snd->loop, s_snd->loop_anchor);
                }
            } else {
                s_drift_n = 0;
            }
        }
    }

    /* D41 G5: a jump slot is queued AHEAD of its step, for the step's own instant.
     * Queueing wakes the engine, so the amp is up and settled long before the sound:
     * started at the boundary instead, a dial with no tick loop woke the amp under
     * the clunk's first 10 ms -- all of its energy -- and the owner heard nothing
     * (orb-B, Station, 2026-11-01). The lead is 1.5 s; the instant is exact. */
    if (!s_snd->steps_known) { snd_steps_scan(s_snd->steps); s_snd->steps_known = true; }
    if (on_glass && !quiet && n && c.jumps) {
        const int64_t minute_us = sec_us - (int64_t)ti->tm_sec * 1000000;
        const double into = ti->tm_sec + frac;
        const int64_t next_min_us = minute_us + 60000000;
        const int nmin = (ti->tm_min + 1) % 60;
        /* A step is named by wall time, never by its microsecond instant: sec_us is
         * re-read every tick, so an instant compared by value would queue anew on
         * every frame. */
        const int64_t wall_min = (int64_t)(t - ti->tm_sec);   /* this minute's epoch second */
        const uint8_t mn = s_snd->steps[SND_STEP_MIN], hn = s_snd->steps[SND_STEP_HOUR];
        if (mn) {
            const int idx = (int)(into * mn / 60.0) + 1;          /* the next step, 1..mn */
            const double at_s = idx * 60.0 / mn;
            const int64_t at_us = minute_us + (int64_t)(at_s * 1e6);
            const int64_t key = wall_min * 64 + idx;
            if (at_s - into <= 1.5 && s_snd->queued[SND_STEP_MIN] != key) {
                s_snd->queued[SND_STEP_MIN] = key;
                snd_at(snd_find(tab, n, CLK_SND_JUMP_MINUTE), at_us);
            }
        }
        if (into >= 58.5) {
            if (hn && (nmin == 0 || (nmin * hn) / 60 != (ti->tm_min * hn) / 60) &&
                s_snd->queued[SND_STEP_HOUR] != wall_min) {
                s_snd->queued[SND_STEP_HOUR] = wall_min;
                snd_at(snd_find(tab, n, CLK_SND_JUMP_HOUR), next_min_us);
            }
            if (s_snd->steps[SND_STEP_CAL] && ti->tm_hour == 23 && ti->tm_min == 59 &&
                s_snd->queued[SND_STEP_CAL] != wall_min) {
                s_snd->queued[SND_STEP_CAL] = wall_min;
                snd_at(snd_find(tab, n, CLK_SND_JUMP_DATE), next_min_us);
            }
        }
    }

    /* Boundaries. The first reading after boot, and any reading that is not the
     * very next minute, only records where the clock is. */
    const bool first = s_snd->min < 0;
    const bool next = !first && t - s_snd->last_t < 120 &&
                      ti->tm_min == (s_snd->min + 1) % 60;
    const bool changed = first || ti->tm_min != s_snd->min;
    const int prev_hour = s_snd->hour, prev_mday = s_snd->mday;
    s_snd->last_t = t;
    if (!changed) return;
    s_snd->min = ti->tm_min; s_snd->hour = ti->tm_hour; s_snd->mday = ti->tm_mday;
    if (!next || ti->tm_sec > 2 || quiet || !n) return;

    const int64_t at = sec_us - (int64_t)ti->tm_sec * 1000000;   /* the minute's instant */
    /* The jump slots were queued ahead of this boundary (above). */
    (void)prev_hour; (void)prev_mday;
    const int pat = snd_pattern(tab, n, theme_pat);
    const int h12 = ti->tm_hour % 12 ? ti->tm_hour % 12 : 12;
    if (pat == CLK_PAT_SHIPS_BELLS) {
        /* Ship's bells replace the quarters: every half hour, nothing between. */
        if (c.strikes && (ti->tm_min == 0 || ti->tm_min == 30))
            snd_hour_pattern(tab, n, pat, at, h12, ti->tm_hour, ti->tm_min);
        return;
    }
    if (c.chimes) {
        if (ti->tm_min == 15) snd_at(snd_find(tab, n, CLK_SND_CHIME_Q1), at);
        if (ti->tm_min == 30) snd_at(snd_find(tab, n, CLK_SND_CHIME_HALF), at);
        if (ti->tm_min == 45) snd_at(snd_find(tab, n, CLK_SND_CHIME_Q3), at);
    }
    if (ti->tm_min == 0) {
        const clk_sound_t *ch = c.chimes ? snd_find(tab, n, CLK_SND_CHIME_HOUR) : NULL;
        snd_at(ch, at);
        /* The strokes follow the chime's own length and a breath. */
        const int64_t t0 = at + (ch ? snd_us(ch) + 400000 : 0);
        if (c.strikes) snd_hour_pattern(tab, n, pat, t0, h12, ti->tm_hour, 0);
    }
}


/* ---- the frame -------------------------------------------------------------------- */

static ease_t s_ease[CLK_ROW_MAX];
static bool   s_logged[CLK_ROW_MAX];
static uint32_t s_comp_logged;

/* Every row's own pose, then the poses it rides (a parent's, two links at most — the
 * projection refused deeper), then each twin takes its row's: the top half of the
 * firmware's rows_update, verbatim in its arithmetic. The stage marks and draws. */
static void rows_pose(const tm_t *ti, time_t t, double frac)
{
    const geo_t *g = &G;
    const uint16_t n = g->n_rows;
    int32_t ang[CLK_ROW_MAX];
    int16_t ox[CLK_ROW_MAX], oy[CLK_ROW_MAX];
    const drive_now_t now = { .ti = ti, .t = t, .frac = frac };
    const uint32_t tick = orb_uptime_ms();
    uint32_t want = 0;

    for (uint16_t i = 0; i < n; i++) {
        const clk_row_t *r = &g->rows[i];
        ox[i] = oy[i] = 0;
        if (!r->spr.show && i != s_info.car_row) { ang[i] = ORB_STAGE_HIDDEN; continue; }
        if (r->src == CLK_SRC_NONE) { ang[i] = 0; continue; }   /* a still row: unturned */
        double u;
        if (!drive_units(r, &now, &u)) {
            ang[i] = ORB_STAGE_HIDDEN;
            if (!s_logged[i]) {
                s_logged[i] = true;
                orb_logf("row %u: source %u has no input on this device — not drawn",
                         (unsigned)r->tag, (unsigned)r->src);
            }
            continue;
        }
        uint32_t busy = 0;
        const double tf = row_ease(r, &s_ease[i], drive_t(r, u), tick, &busy);
        if (busy > want) want = busy;
        row_pose(r, tf, &ang[i], &ox[i], &oy[i]);
    }
    if (want) orb_burst(want + 40);
    int32_t own_a[CLK_ROW_MAX];
    int16_t own_x[CLK_ROW_MAX], own_y[CLK_ROW_MAX];
    memcpy(own_a, ang, sizeof(int32_t) * n);
    memcpy(own_x, ox, sizeof(int16_t) * n);
    memcpy(own_y, oy, sizeof(int16_t) * n);
    for (uint16_t i = 0; i < n; i++) {
        if (!g->rows[i].parent_row || ang[i] == ORB_STAGE_HIDDEN) continue;
        for (uint16_t p = g->rows[i].parent_row, hops = 0; p && hops < 2; hops++) {
            const uint16_t pi = (uint16_t)(p - 1u);
            if (pi >= n || own_a[pi] == ORB_STAGE_HIDDEN) break;
            ang[i] = (ang[i] + own_a[pi]) % 3600;
            ox[i] = (int16_t)(ox[i] + own_x[pi]);
            oy[i] = (int16_t)(oy[i] + own_y[pi]);
            p = g->rows[pi].parent_row;
        }
    }
    for (uint16_t i = 1; i < n; i++)
        if (g->rows[i].flags & CLK_FLAG_AUX_SHADOW) {
            ang[i] = ang[i - 1]; ox[i] = ox[i - 1]; oy[i] = oy[i - 1];
        }
    for (uint16_t i = 0; i < n; i++) orb_stage_pose(i, ang[i], ox[i], oy[i]);
}

/* Every complication's value off the same reading, and whether it is live: a source
 * this device has no input for, or a blink on its off second, is not. */
static void comps_value(const tm_t *ti, time_t t)
{
    for (uint16_t i = 0; i < G.n_comps; i++) {
        const clk_comp_t *c = &G.comps[i];
        int32_t value = 0;
        bool live = comp_value(c->src, ti, t, &value);
        if (!live && c->show && !(s_comp_logged & (1u << i))) {
            s_comp_logged |= 1u << i;
            orb_logf("comp %u: source %u has no input on this device — not drawn",
                     (unsigned)i, (unsigned)c->src);
        }
        if (live && (c->flags & CLK_FLAG_BLINK) && (t % 2) != 0) live = false;
        orb_stage_comp_set(i, value, live);
    }
}

/* ---- the base face: no theme resident (§26, D31) ------------------------------------ */

static int32_t s_b_time, s_b_date, s_b_status;
static char    s_b_last[16];
static bool    s_b_built;

static void base_build(void)
{
    s_b_time = orb_ui_create(ORB_UI_LABEL, 0);
    orb_ui_set(s_b_time, ORB_P_FONT, ORB_FONT_48);
    orb_ui_set(s_b_time, ORB_P_TEXT_COLOR, 0xFFFFFF);
    orb_ui_set(s_b_time, ORB_P_TEXT_ALIGN, 1);
    orb_ui_align(s_b_time, ORB_AL_CENTER, 0, -34);
    s_b_date = orb_ui_create(ORB_UI_LABEL, 0);
    orb_ui_set(s_b_date, ORB_P_FONT, ORB_FONT_14);
    orb_ui_set(s_b_date, ORB_P_TEXT_COLOR, 0xBFBFBF);
    orb_ui_align(s_b_date, ORB_AL_CENTER, 0, 18);
    s_b_status = orb_ui_create(ORB_UI_LABEL, 0);
    orb_ui_set(s_b_status, ORB_P_FONT, ORB_FONT_14);
    orb_ui_set(s_b_status, ORB_P_TEXT_COLOR, 0x8A8A8A);
    orb_ui_align(s_b_status, ORB_AL_CENTER, 0, 46);
}

static void base_tick(void)
{
    static const char *const WD[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const MO[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int64_t t;
    int32_t frac;
    const bool synced = orb_now(&t, &frac) != 0;
    tm_t ti;
    char hhmm[16], date[24];
    if (!localtime_of(t, &ti)) {
        snprintf(hhmm, sizeof hhmm, "--:--");
        date[0] = 0;
    } else {
        snprintf(hhmm, sizeof hhmm, "%02d:%02d", ti.tm_hour, ti.tm_min);
        snprintf(date, sizeof date, "%s %2d %s", WD[ti.tm_wday % 7], ti.tm_mday, MO[ti.tm_mon % 12]);
    }
    if (strcmp(hhmm, s_b_last)) {
        memcpy(s_b_last, hhmm, sizeof s_b_last);
        orb_ui_text(s_b_time, hhmm);
        orb_ui_text(s_b_date, date);
    }
    orb_ui_text(s_b_status, synced ? "" : "waiting to sync");
}

/* ---- the repeater: a press on the dial sounds the time (D39 §5.1) ------------------ */

static int64_t s_shown_us;

static void repeater(void)
{
    if (!orb_stage_snd_ready()) return;
    if (orb_uptime_us() - s_shown_us < 800000) return;   /* the press that picked the dial */
    const clk_sound_t *hr = snd_find(s_snds, s_nsnd, CLK_SND_STRIKE_HOUR);
    const clk_sound_t *qt = snd_find(s_snds, s_nsnd, CLK_SND_STRIKE_QUARTER);
    const clk_sound_t *mn = snd_find(s_snds, s_nsnd, CLK_SND_STRIKE_MINUTE);
    if (!hr || !qt || !mn) {
        orb_logf("repeater: the theme lacks a stroke (hour %d, quarter %d, minute %d)",
                 hr != NULL, qt != NULL, mn != NULL);
        return;
    }
    int64_t t;
    int32_t frac;
    (void)orb_now(&t, &frac);
    tm_t ti;
    if (!localtime_of(t, &ti)) return;
    const int h12 = ti.tm_hour % 12 ? ti.tm_hour % 12 : 12;
    const int quarters = ti.tm_min / 15, minutes = ti.tm_min % 15;
    int64_t at = orb_uptime_us() + 150000;
    for (int i = 0; i < h12; i++, at += 900000)       snd_at_flags(hr, at, ORB_STAGE_SND_ASKED);
    at += 600000;
    for (int i = 0; i < quarters; i++, at += snd_us(qt) + 250000) snd_at_flags(qt, at, ORB_STAGE_SND_ASKED);
    if (quarters) at += 600000;
    for (int i = 0; i < minutes; i++, at += 450000)   snd_at_flags(mn, at, ORB_STAGE_SND_ASKED);
    orb_logf("repeater: %d:%02d — %d, %d, %d strokes", h12, ti.tm_min, h12, quarters, minutes);
}

/* ---- entry points ------------------------------------------------------------------ */

/* The base face's labels shown, built the first time; or hidden, under a theme. */
static void base_show(bool on)
{
    if (!s_b_built) {
        if (!on) return;
        base_build();
        s_b_built = true;
    }
    orb_ui_set(s_b_time, ORB_P_HIDDEN, !on);
    orb_ui_set(s_b_date, ORB_P_HIDDEN, !on);
    orb_ui_set(s_b_status, ORB_P_HIDDEN, !on);
    if (on) { s_b_last[0] = 0; base_tick(); }
}

/* What the stage holds now: its theme's rows, comps and sounds, or the base face. Read at
 * init and again at every arrival — a theme can land while the clock runs (the OS fills an
 * evicted theme back from the card, or a store chooses another without a reboot; OS
 * theme-cache-plan §2.4, §4.1), and a dial read once kept its base face over the new theme
 * with no hands (owner, 2026-11-09). */
static void stage_apply(const orb_stage_info_t *in)
{
    s_info = *in;
    base_show(s_info.base);
    /* the manifest's beat ticks the base face's readout and a themed dial's hands alike: at 0
     * the hands stood still (Bronze, 2026-11-09) */
    orb_set_beat(1000);
    if (s_info.base) return;
    G.n_rows = s_info.n_rows < CLK_ROW_MAX ? s_info.n_rows : CLK_ROW_MAX;
    G.n_comps = s_info.n_comps < CLK_COMP_MAX ? s_info.n_comps : CLK_COMP_MAX;
    for (int i = 0; i < G.n_rows; i++) orb_stage_row(i, &G.rows[i]);
    for (int i = 0; i < G.n_comps; i++) orb_stage_comp(i, &G.comps[i]);
    s_nsnd = s_info.n_sounds < SND_MAX ? s_info.n_sounds : SND_MAX;
    for (uint32_t i = 0; i < s_nsnd; i++) orb_stage_sound((int32_t)i, &s_snds[i]);
    s_pattern = s_info.pattern;
    orb_logf("dial: %u rows, %u comps, %u sounds", (unsigned)G.n_rows, (unsigned)G.n_comps,
             (unsigned)s_nsnd);
}

static int32_t stage_read(void)
{
    orb_stage_info_t in;
    if (orb_stage_open(&in) != ORB_OK) return ORB_ERR_NOT_ALLOWED;
    stage_apply(&in);
    return ORB_OK;
}

ORB_EXPORT("orb_init") int32_t orb_init(void) { return stage_read(); }

/* The stage calls this every frame it composes; the base face gets the one-second
 * beat instead. */
/* Set at every arrival; the next tick re-reads the stage. Not in the hooks themselves: the
 * OS calls them before it binds the stage, so they would read the last app's binding. A
 * tick comes from the stage's frames (or the base face's beat), once the stage is bound. */
static bool s_reread;

ORB_EXPORT("orb_tick") void orb_tick(uint32_t now_ms)
{
    if (s_reread) {
        /* A stage still binding answers "no theme" for a moment, and ticks come from inside
         * the bind (Bronze, 2026-11-09: a dial that believed it lost its hands, and three ticks
         * in a row came within one bind). A themed dial believes "no theme" only once it is
         * still the answer 2 s after the arrival. */
        orb_stage_info_t in;
        if (orb_stage_open(&in) == ORB_OK) {
            if (in.base && !s_info.base && orb_uptime_us() - s_shown_us < 2000000) {
                /* ask again next tick */
            } else {
                s_reread = false;
                if (memcmp(&in, &s_info, sizeof in)) stage_apply(&in);   /* only what changed */
            }
        }
    }
    if (s_info.base) { base_tick(); return; }
    int64_t t;
    int32_t frac_ns, wall_ns;
    orb_stage_now(&t, &frac_ns, &wall_ns);
    tm_t ti;
    if (!localtime_of(t, &ti)) return;
    const double frac = frac_ns / 1e9, wall = wall_ns / 1e9;
    rows_pose(&ti, t, frac);
    comps_value(&ti, t);
    sound_policy(&ti, t, wall, true);
}

/* theme-cache-plan §2.6 (owner, 2026-11-08: Gold swapped to weather mid-chime and cut it off):
 * the clock chimes whatever is on the glass. Off it, the service keeps the hours. */
static bool s_on_glass;
ORB_EXPORT("orb_service") void orb_service(void)
{
    if (s_on_glass || s_info.base) return;
    int64_t t;
    int32_t frac_ns;
    (void)orb_now(&t, &frac_ns);
    tm_t ti;
    if (!localtime_of(t, &ti)) return;
    sound_policy(&ti, t, frac_ns / 1e9, false);
}
ORB_EXPORT("orb_suspend") void orb_suspend(void) { s_on_glass = false; }

ORB_EXPORT("orb_activate") void orb_activate(void) { s_shown_us = orb_uptime_us(); s_on_glass = true; s_reread = true; }
ORB_EXPORT("orb_resume")   void orb_resume(void)   { s_shown_us = orb_uptime_us(); s_on_glass = true; s_reread = true; }

/* The dial has no armed view: a press is the repeater's, and the dial stays unarmed so
 * the turn still walks the carousel. */
ORB_EXPORT("orb_set_armed") int32_t orb_set_armed(int32_t armed)
{
    if (armed && !s_info.base) repeater();
    return 0;
}
