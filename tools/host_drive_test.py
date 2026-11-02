#!/usr/bin/env python3
"""Host bench for the row drive (docs/element-model-plan.md §3-§4, D42 E1).

Every moving row on the dial — hand, pointer, ring, carriage — is posed by one drive:
its source (or one digit of it), as much of the next finer field as its cadence shows,
the fraction t of the way across its domain, and the two poses that fraction lands
between (an angle and the pivot's travel). This bench lifts the shipping functions OUT
OF orb_stage.c VERBATIM, compiles them with host gcc -Werror, and pins them against
hand-worked vectors: each cadence, each end rule, the digit selectors, a travel and a
turn at once, the ease in fraction space, and the step scan the jump sounds read.

D41's v4-parity half (a day of pre-G1 hand_angles, bit for bit) proved a promise D42
no longer makes — no old theme is read — and went with the hand slots it covered.

Run:  python3 tools/host_drive_test.py              (rc 0 = every vector holds)
      python3 tools/host_drive_test.py --mutations  (proves the checks bite)
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# §43: the dial's drive is the clock app's own code now, carried verbatim.
CLOCK = os.path.join(ROOT, "apps", "clock", "main.c")
ART = os.path.join(ROOT, "main", "art")

def lift(src, sig):
    """One function's text, from its signature to the first column-0 }."""
    i = src.index(sig)
    j = src.index("\n}", i) + 2
    return src[i:j]

def lift_block(src, start, end):
    """From `start` through the first `end` after it, inclusive."""
    i = src.index(start)
    j = src.index(end, i) + len(end)
    return src[i:j]

C_PRE = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "clk_types.h"
#define CLK_ROLE_AUX 7
/* the geometry the step scan walks: only the fields it reads */
typedef struct { uint16_t n_comps, n_rows; clk_comp_t comps[CLK_COMP_MAX]; clk_row_t rows[CLK_ROW_MAX]; } orb_theme_geo_t;
static orb_theme_geo_t s_geo;
static const orb_theme_geo_t *orb_theme_geo(void) { return &s_geo; }
/* the app's spellings of the same things (apps/clock/main.c) */
typedef struct tm tm_t;
typedef orb_theme_geo_t geo_t;
#define G s_geo
"""

C_HARNESS = r"""
static unsigned long fails = 0, checks = 0;

static struct tm mk(int y, int mon, int d, int h, int mi, int s)
{
    struct tm t; memset(&t, 0, sizeof t);
    t.tm_year = y - 1900; t.tm_mon = mon - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    time_t tt = timegm(&t);
    gmtime_r(&tt, &t);                      /* fills wday and yday */
    return t;
}

/* A row's pose — angle in tenths, travel in px — against a hand-worked figure. */
static void pose(const char *what, clk_row_t r, struct tm ti, double frac,
                 int32_t want_a, int want_x, int want_y)
{
    const drive_now_t now = { .ti = &ti, .t = timegm(&ti), .frac = frac };
    double u;
    checks++;
    if (!drive_units(&r, &now, &u)) { fails++; printf("FAIL %s: no input\n", what); return; }
    int32_t a; int16_t ox, oy;
    row_pose(&r, drive_t(&r, u), &a, &ox, &oy);
    if (a != want_a || ox != want_x || oy != want_y) {
        fails++;
        printf("FAIL %s: want %d (%d,%d) got %d (%d,%d) (units %.6f)\n", what, (int)want_a,
               want_x, want_y, (int)a, ox, oy, u);
    }
}

static void units(const char *what, clk_row_t r, struct tm ti, double frac, double want)
{
    const drive_now_t now = { .ti = &ti, .t = timegm(&ti), .frac = frac };
    double u = -1;
    checks++;
    if (!drive_units(&r, &now, &u) || fabs(u - want) > 1e-9) {
        fails++; printf("FAIL %s: want %.6f got %.6f\n", what, want, u);
    }
}

static void tfrac(const char *what, clk_row_t r, double u, double want)
{
    checks++;
    const double t = drive_t(&r, u);
    if (fabs(t - want) > 1e-9) { fails++; printf("FAIL %s: want t %.6f got %.6f\n", what, want, t); }
}
"""

C_BODY = r"""
    const clk_row_t SEC  = { .src = CLK_SRC_SECOND, .span = 3600, .value_max = 60, .end = CLK_END_WRAP };
    const clk_row_t MIN  = { .src = CLK_SRC_MINUTE, .span = 3600, .value_max = 60, .end = CLK_END_WRAP };
    const clk_row_t DATE = { .src = CLK_SRC_DATE,   .span = 3600, .value_min = 1, .value_max = 32, .end = CLK_END_WRAP };
    clk_row_t d;

    d = SEC; d.cadence = CLK_CAD_STEP; d.cad_arg = 2;
    pose("step 2: 10.6 s shows 10.5", d, mk(2026, 11, 1, 1, 2, 10), 0.6, 630, 0, 0);
    pose("step 2: 10.4 s shows 10.0", d, mk(2026, 11, 1, 1, 2, 10), 0.4, 600, 0, 0);
    d.cad_arg = 8;
    pose("step 8: 10.3 s shows 10.25", d, mk(2026, 11, 1, 1, 2, 10), 0.3, 615, 0, 0);

    d = MIN; d.cadence = CLK_CAD_JUMP;
    pose("jump minute: 12:34:59.9 still on 34", d, mk(2026, 11, 1, 12, 34, 59), 0.9, 2040, 0, 0);
    d.cadence = CLK_CAD_SWEEP;
    pose("sweep minute: 12:34:30 is 34.5", d, mk(2026, 11, 1, 12, 34, 30), 0.0, 2070, 0, 0);
    pose("sweep minute reads the second's own fraction", d, mk(2026, 11, 1, 12, 34, 30), 0.9, 2071, 0, 0);

    d = SEC; d.cadence = CLK_CAD_STOP_TO_GO; d.cad_arg = 585;
    pose("stop-to-go: 29.25 s is half way", d, mk(2026, 11, 1, 1, 2, 29), 0.25, 1800, 0, 0);
    pose("stop-to-go: 58.5 s reaches the top", d, mk(2026, 11, 1, 1, 2, 58), 0.5, 0, 0, 0);
    pose("stop-to-go: 59.9 s rests at the top", d, mk(2026, 11, 1, 1, 2, 59), 0.9, 0, 0, 0);
    units("stop-to-go's rest is the domain's end, not past it", d, mk(2026, 11, 1, 1, 2, 59), 0.9, 60.0);
    d.cad_arg = 0;
    pose("stop-to-go: P defaults to 58.5 s", d, mk(2026, 11, 1, 1, 2, 29), 0.25, 1800, 0, 0);

    /* A date ring: 1..31 on a half-open domain of 31 days, so the 1st is P0. */
    d = DATE; d.cadence = CLK_CAD_DRAG; d.cad_arg = 120;
    pose("drag: the 5th at 21:00 holds", d, mk(2026, 11, 5, 21, 0, 0), 0.0, 465, 0, 0);
    pose("drag: the 5th at 22:00 starts", d, mk(2026, 11, 5, 22, 0, 0), 0.0, 465, 0, 0);
    pose("drag: the 5th at 23:00 is half way", d, mk(2026, 11, 5, 23, 0, 0), 0.0, 523, 0, 0);
    units("drag: the 5th at 23:30 is 5.75", d, mk(2026, 11, 5, 23, 30, 0), 0.0, 5.75);
    d.cadence = CLK_CAD_JUMP; d.cad_arg = 0;
    pose("a date ring: the 1st is P0", d, mk(2026, 11, 1, 12, 0, 0), 0.0, 0, 0, 0);
    pose("a date ring: the 31st is a step short of P0", d, mk(2026, 10, 31, 12, 0, 0), 0.0, 3484, 0, 0);

    d = (clk_row_t){ .src = CLK_SRC_MONTH, .span = 3600, .value_max = 12, .cadence = CLK_CAD_DRAG,
                     .end = CLK_END_WRAP };
    units("drag month: 28 Feb 2027 23:00 is half through its window", d,
          mk(2027, 2, 28, 23, 0, 0), 0.0, 1.5);
    units("drag month: a leap February still has a day to go", d,
          mk(2028, 2, 28, 23, 0, 0), 0.0, 1.0);

    /* A flyback fan: hour12 over 180 degrees, P0 at 12. */
    d = (clk_row_t){ .src = CLK_SRC_HOUR12, .span = 1800, .value_max = 12,
                     .cadence = CLK_CAD_SWEEP, .end = CLK_END_FLYBACK };
    pose("fan: 06:00 is the middle", d, mk(2026, 11, 1, 6, 0, 0), 0.0, 900, 0, 0);
    pose("fan: 11:30", d, mk(2026, 11, 1, 11, 30, 0), 0.0, 1725, 0, 0);
    pose("fan: 23:59:59 stops at the fan's end", d, mk(2026, 11, 1, 23, 59, 59), 0.99, 1800, 0, 0);
    d.zero = 2100; d.span = 1200;
    pose("fan from 210 over 120: 06:00 at 270", d, mk(2026, 11, 1, 6, 0, 0), 0.0, 2700, 0, 0);
    pose("fan from 210 over 120: 00:00 at 210", d, mk(2026, 11, 1, 0, 0, 0), 0.0, 2100, 0, 0);
    d.src = CLK_SRC_HOUR24; d.cadence = CLK_CAD_JUMP; d.zero = 0; d.span = 1800;
    pose("a flyback past its domain rests at P1", d, mk(2026, 11, 1, 18, 0, 0), 0.0, 1800, 0, 0);
    tfrac("a flyback clamps below P0", d, -3.0, 0.0);
    d.end = CLK_END_WRAP; d.span = 3600;
    tfrac("a wrap takes the fraction modulo one", d, 18.0, 0.5);
    tfrac("a wrap below P0 comes round from the top", d, -3.0, 0.75);

    /* A slide: the pivot travels 160 px across a minute, no turn. */
    d = (clk_row_t){ .src = CLK_SRC_SECOND, .dx = 160, .value_max = 60,
                     .cadence = CLK_CAD_SWEEP, .end = CLK_END_FLYBACK };
    pose("a slide is half way at 30 s", d, mk(2026, 11, 1, 1, 2, 30), 0.0, 0, 80, 0);
    d.dx = 0; d.dy = -310; d.src = CLK_SRC_DATE; d.value_min = 1; d.value_max = 31;
    d.cadence = CLK_CAD_JUMP; d.end = CLK_END_HOLD;
    pose("a held slide rests at P1 and never wraps", d, mk(2026, 10, 31, 12, 0, 0), 0.0, 0, 0, -310);
    /* both at once: a pointer that slides and tilts */
    d = (clk_row_t){ .src = CLK_SRC_SECOND, .zero = 300, .span = 900, .dx = 100, .dy = -40,
                     .value_max = 60, .cadence = CLK_CAD_STEP, .cad_arg = 1, .end = CLK_END_FLYBACK };
    pose("a turn and a travel at once, a quarter in", d, mk(2026, 11, 1, 1, 2, 15), 0.4, 525, 25, -10);

    /* D42 §4: digit selectors. The units ring of the 23rd reads 3, the tens ring 2. */
    d = (clk_row_t){ .src = CLK_SRC_DATE, .digit = CLK_DIGIT_UNITS, .span = -3600, .value_max = 10,
                     .cadence = CLK_CAD_JUMP, .end = CLK_END_WRAP };
    pose("units ring: the 23rd reads 3", d, mk(2026, 10, 23, 12, 0, 0), 0.0, 2520, 0, 0);
    pose("units ring: the 30th reads 0", d, mk(2026, 10, 30, 12, 0, 0), 0.0, 0, 0, 0);
    units("units ring: the 23rd's digit itself is 3", d, mk(2026, 10, 23, 12, 0, 0), 0.0, 3.0);
    d.digit = CLK_DIGIT_TENS; d.value_max = 4;
    pose("tens ring: the 23rd reads 2", d, mk(2026, 10, 23, 12, 0, 0), 0.0, 1800, 0, 0);
    {
        clk_row_t y = d; y.src = CLK_SRC_DAY_OF_YEAR; y.value_max = 10;
        units("tens of day 245 is 4, not 24", y, mk(2026, 9, 2, 12, 0, 0), 0.0, 4.0);
    }
    d.cadence = CLK_CAD_SWEEP;
    units("tens ring: a sweep counts the units toward the next ten", d,
          mk(2026, 10, 25, 12, 0, 0), 0.0, 2.55);

    /* D43 P5d.3: a year's digits, the shown number's digits, and the roll. */
    d = (clk_row_t){ .src = CLK_SRC_YEAR, .digit = CLK_DIGIT_THOUSANDS, .span = -3600,
                     .value_max = 10, .cadence = CLK_CAD_JUMP, .end = CLK_END_WRAP };
    units("year thousands: 2026 reads 2", d, mk(2026, 11, 2, 12, 0, 0), 0.0, 2.0);
    d.digit = CLK_DIGIT_HUNDREDS;
    units("year hundreds: 2026 reads 0", d, mk(2026, 11, 2, 12, 0, 0), 0.0, 0.0);
    d.digit = CLK_DIGIT_UNITS;
    units("year units: 2026 reads 6", d, mk(2026, 11, 2, 12, 0, 0), 0.0, 6.0);
    d = (clk_row_t){ .src = CLK_SRC_HOUR12, .digit = CLK_DIGIT_TENS, .span = 3600,
                     .value_max = 2, .cadence = CLK_CAD_JUMP, .end = CLK_END_WRAP };
    units("a 12-hour tens digit reads the 12 the dial shows at noon", d, mk(2026, 11, 2, 12, 5, 0), 0.0, 1.0);
    d = (clk_row_t){ .src = CLK_SRC_MONTH, .digit = CLK_DIGIT_UNITS, .span = 3600,
                     .value_max = 10, .cadence = CLK_CAD_JUMP, .end = CLK_END_WRAP };
    units("a month's units digit reads November as 1", d, mk(2026, 11, 2, 12, 0, 0), 0.0, 1.0);
    d = (clk_row_t){ .src = CLK_SRC_DATE, .digit = CLK_DIGIT_TENS, .span = -3600,
                     .value_max = 4, .cadence = CLK_CAD_ROLL, .end = CLK_END_WRAP };
    units("roll: the tens holds on the 15th", d, mk(2026, 10, 15, 18, 0, 0), 0.0, 1.0);
    units("roll: the tens rolls through the 19th", d, mk(2026, 10, 19, 18, 0, 0), 0.0, 1.75);
    units("roll: the 31st rolls the tens 3 on to 0", d, mk(2026, 10, 31, 12, 0, 0), 0.0, 3.5);
    units("roll: the 30th of a 31-day month holds its 3", d, mk(2026, 10, 30, 12, 0, 0), 0.0, 3.0);
    units("roll: the 30th of November rolls on to the 1st's 0", d, mk(2026, 11, 30, 12, 0, 0), 0.0, 3.5);
    d.digit = CLK_DIGIT_UNITS; d.value_max = 10;
    units("roll: the units creep every day", d, mk(2026, 10, 23, 6, 0, 0), 0.0, 3.25);
    units("roll: a February's 28th rolls 8 on to the 1st's 1 (three figures)", d, mk(2026, 2, 28, 12, 0, 0), 0.0, 9.5);
    d = (clk_row_t){ .src = CLK_SRC_DATE, .span = 3600, .value_min = 1, .value_max = 32,
                     .cadence = CLK_CAD_ROLL, .end = CLK_END_WRAP };
    units("roll on a whole date wheel: the 28th of February runs four figures to the 1st", d, mk(2026, 2, 28, 18, 0, 0), 0.0, 31.0);
    d = (clk_row_t){ .src = CLK_SRC_MOON_AGE, .span = 3600, .value_max = 60,
                     .cadence = CLK_CAD_ROLL, .end = CLK_END_WRAP };
    {
        struct tm ti = mk(2026, 11, 2, 0, 0, 0);
        const time_t t0 = timegm(&ti);
        const int64_t m = ((int64_t)t0 - MOON_NEW_MOON_S) % MOON_SYNODIC_S;
        const double want = (double)(m / 86400) + (double)(m % 86400) / 86400.0;
        units("roll: a moon disc creeps through its day", d, ti, 0.0, want);
    }

    /* A source with no input on this orb poses nothing. */
    d = (clk_row_t){ .src = CLK_SRC_BATTERY, .dx = 80, .value_max = 100,
                     .cadence = CLK_CAD_SWEEP, .end = CLK_END_HOLD };
    {
        struct tm ti = mk(2026, 11, 1, 6, 0, 0);
        const drive_now_t now = { .ti = &ti, .t = timegm(&ti), .frac = 0.0 };
        double u;
        checks++;
        if (drive_units(&d, &now, &u)) { fails++; printf("FAIL a battery row had input\n"); }
    }
    d = (clk_row_t){ .src = CLK_SRC_WEEKDAY, .span = 3600, .value_max = 7, .cadence = CLK_CAD_SWEEP };
    units("sweep weekday: Sunday noon", d, mk(2026, 11, 1, 12, 0, 0), 0.0, 6.5);

    /* Ease: a settle overshoots and returns; a flyback goes back along the fan. */
    {
        ease_t e = { 0 }; uint32_t busy;
        float p = ease_pose(&e, 30.0f, 0, 120, true, false, 1, 360.0f, 1.5f, &busy);
        checks++; if (p != 30.0f || busy) { fails++; printf("FAIL ease: the first pose is the pose\n"); }
        p = ease_pose(&e, 36.0f, 1000, 120, true, false, 1, 360.0f, 1.5f, &busy);
        checks++; if (p != 36.0f || busy != 120) { fails++; printf("FAIL ease: a jump starts its settle (%f, %u)\n", p, busy); }
        p = ease_pose(&e, 36.0f, 1060, 120, true, false, 1, 360.0f, 1.5f, &busy);
        checks++; if (fabsf(p - 37.5f) > 1e-3f || busy != 60) { fails++; printf("FAIL ease: half way the settle is 1.5 over (%f)\n", p); }
        p = ease_pose(&e, 36.0f, 1120, 120, true, false, 1, 360.0f, 1.5f, &busy);
        checks++; if (p != 36.0f || busy || e.kind != EASE_NONE) { fails++; printf("FAIL ease: the settle ends on the pose (%f)\n", p); }
        p = ease_pose(&e, 30.0f, 2000, 120, true, false, 1, 360.0f, 1.5f, &busy);
        p = ease_pose(&e, 30.0f, 2060, 120, true, false, 1, 360.0f, 1.5f, &busy);
        checks++; if (fabsf(p - 28.5f) > 1e-3f) { fails++; printf("FAIL ease: a backward step settles backward (%f)\n", p); }
        ease_t f = { 0 };
        ease_pose(&f, 60.0f, 0, 400, false, true, 1, 360.0f, 1.5f, &busy);    /* a fan 300..60, at its end */
        p = ease_pose(&f, 300.0f, 100, 400, false, true, 1, 360.0f, 1.5f, &busy);
        checks++; if (p != 60.0f || busy != 400) { fails++; printf("FAIL ease: the flyback starts at the fan's end (%f)\n", p); }
        p = ease_pose(&f, 300.0f, 300, 400, false, true, 1, 360.0f, 1.5f, &busy);
        checks++; if (fabsf(p - 0.0f) > 1e-3f) { fails++; printf("FAIL ease: half way back is the fan's middle, through 12 (%f)\n", p); }
        p = ease_pose(&f, 300.5f, 900, 400, false, true, 1, 360.0f, 1.5f, &busy);
        checks++; if (p != 300.5f || busy) { fails++; printf("FAIL ease: forward travel after the flyback is plain (%f)\n", p); }
        ease_t g = { 0 };
        ease_pose(&g, 10.0f, 0, 0, true, false, 1, 360.0f, 1.5f, &busy);
        p = ease_pose(&g, 16.0f, 10, 0, true, false, 1, 360.0f, 1.5f, &busy);
        checks++; if (p != 16.0f || busy) { fails++; printf("FAIL ease: no ease, no animation\n"); }
    }
    /* The rows ease their fraction. A jumping minute's 59 -> 0 settles FORWARD (the
     * fraction wraps), by 1.5 degrees; a fan's reset flies back along it; a slide's
     * settle is 3 px of its travel. */
    {
        uint32_t busy;
        clk_row_t m = MIN; m.cadence = CLK_CAD_JUMP; m.ease = 120;
        ease_t e = { 0 };
        row_ease(&m, &e, 59.0 / 60.0, 0, &busy);
        double t = row_ease(&m, &e, 0.0, 1000, &busy);
        t = row_ease(&m, &e, 0.0, 1060, &busy);
        checks++;
        if (fabs(t - 15.0 / 3600.0) > 1e-6 || busy != 60) {
            fails++; printf("FAIL row ease: the minute's wrap settles forward by 1.5 deg (%f)\n", t);
        }
        clk_row_t fan = { .src = CLK_SRC_HOUR12, .span = 1200, .value_max = 12, .ease = 400,
                          .cadence = CLK_CAD_SWEEP, .end = CLK_END_FLYBACK };
        ease_t f = { 0 };
        row_ease(&fan, &f, 1.0, 0, &busy);
        t = row_ease(&fan, &f, 0.0, 100, &busy);
        t = row_ease(&fan, &f, 0.0, 300, &busy);
        checks++;
        if (fabs(t - 0.5) > 1e-6) { fails++; printf("FAIL row ease: half way back is half the fan (%f)\n", t); }
        clk_row_t bar = { .src = CLK_SRC_MINUTE, .dx = 120, .value_max = 60, .ease = 100,
                          .cadence = CLK_CAD_JUMP, .end = CLK_END_FLYBACK };
        ease_t b = { 0 };
        row_ease(&bar, &b, 0.5, 0, &busy);
        row_ease(&bar, &b, 31.0 / 60.0, 1000, &busy);
        t = row_ease(&bar, &b, 31.0 / 60.0, 1050, &busy);
        checks++;
        if (fabs(t - (31.0 / 60.0 + 3.0 / 120.0)) > 1e-6) {
            fails++; printf("FAIL row ease: a slide settles 3 px of its travel (%f)\n", t);
        }
    }

    /* G5: what steps on a dial decides which jump slots sound. */
    {
        uint8_t st[3];
        memset(&s_geo, 0, sizeof s_geo);
        s_geo.n_rows = 3;
        s_geo.rows[0] = (clk_row_t){ .src = CLK_SRC_HOUR12, .cadence = CLK_CAD_SWEEP, .spr = { .show = 1 } };
        s_geo.rows[1] = (clk_row_t){ .src = CLK_SRC_MINUTE, .cadence = CLK_CAD_SWEEP, .spr = { .show = 1 } };
        s_geo.rows[2] = (clk_row_t){ .src = CLK_SRC_SECOND, .cadence = CLK_CAD_STEP, .cad_arg = 1, .spr = { .show = 1 } };
#define STEPS(m, h, c, what) do { snd_steps_scan(st); checks++; \
        if (st[0] != (m) || st[1] != (h) || st[2] != (c)) { fails++; \
            printf("FAIL G5 %s: want %d/%d/%d got %d/%d/%d\n", what, m, h, c, st[0], st[1], st[2]); } } while (0)
        STEPS(0, 0, 0, "a creeping dial sounds no jump");
        s_geo.rows[1].cadence = CLK_CAD_JUMP;
        STEPS(1, 0, 0, "a jumping minute");
        s_geo.rows[1].spr.show = 0;
        STEPS(0, 0, 0, "a hidden minute sounds nothing");
        s_geo.rows[1].spr.show = 1; s_geo.rows[1].cadence = CLK_CAD_STEP; s_geo.rows[1].cad_arg = 2;
        STEPS(2, 0, 0, "a half-minute row steps twice a minute");
        s_geo.rows[1].cadence = CLK_CAD_SWEEP; s_geo.rows[1].cad_arg = 0;
        s_geo.rows[0].cadence = CLK_CAD_JUMP;
        STEPS(0, 1, 0, "a jump hour");
        s_geo.rows[0].cadence = CLK_CAD_SWEEP;
        s_geo.n_rows = 5;
        s_geo.rows[3] = (clk_row_t){ .src = CLK_SRC_MINUTE, .cadence = CLK_CAD_JUMP,
                                     .flags = CLK_FLAG_AUX_SHADOW, .spr = { .show = 1 } };
        s_geo.rows[4] = (clk_row_t){ .src = CLK_SRC_DATE, .digit = CLK_DIGIT_UNITS,
                                     .cadence = CLK_CAD_JUMP, .spr = { .show = 1 } };
        STEPS(0, 0, 1, "a shadow twin does not count; a date's units ring steps at midnight");
        s_geo.n_rows = 3; s_geo.n_comps = 2;
        s_geo.comps[0].src = CLK_SRC_MOON_AGE; s_geo.comps[1].src = CLK_SRC_DATE;
        STEPS(0, 0, 1, "a date window turns at midnight");
#undef STEPS
        memset(&s_geo, 0, sizeof s_geo);
    }
"""

MUTATIONS = [
    ("shown = floor(f * steps) / steps;", "shown = f;"),
    ("const double run = (v + f) * 600.0 / p;", "const double run = (v + f) * 60.0 / p;"),
    ("*out = run < r->value_max ? run : r->value_max;", "*out = run;"),
    ("if (ti->tm_mon == 1 && comp_year_leap(ti->tm_year + 1900)) dim = 29;", ""),
    ("shown = into > 0.0 ? into / (w < unit_min ? w : unit_min) : 0.0;", "shown = into / w;"),
    ("    case CLK_SRC_MINUTE:  *unit_min = 1.0;        return (ti->tm_sec + frac) / 60.0;",
     "    case CLK_SRC_MINUTE:  *unit_min = 1.0;        return ti->tm_sec / 60.0;"),
    ("    case CLK_DIGIT_UNITS:     return v % 10;", "    case CLK_DIGIT_UNITS:     return v;"),
    ("        f = ((w % place) + f) / (double)place;\n", ""),
    ("    case CLK_DIGIT_TENS:      return v / 10 % 10;", "    case CLK_DIGIT_TENS:      return v / 10;"),
    ("    case CLK_DIGIT_HUNDREDS:  return v / 100 % 10;", "    case CLK_DIGIT_HUNDREDS:  return v / 10 % 10;"),
    ("    if (src == CLK_SRC_HOUR12) v = v ? v : 12;", ""),
    ("    else if (src == CLK_SRC_MONTH) v = v + 1;", ""),
    ("        *out = v >= dim ? 1 : v + 1;", "        *out = v + 1;"),
    ("        *out = a + (double)((((b - a) % figs) + figs) % figs) * f;", "        *out = a + f;"),
    ("    case CLK_SRC_YEAR:        *out = ti->tm_year + 1900;      break;", "    case CLK_SRC_YEAR:        *out = ti->tm_year;      break;"),
    ("        return ((double)(m % 86400) + frac) / 86400.0; }", "        return frac; }"),
    ("    double t = w > 0.0 ? (units - (double)r->value_min) / w : 0.0;",
     "    double t = w > 0.0 ? units / w : 0.0;"),
    ("        t -= floor(t);\n", ""),
    ("        if (t > 1.0) t = 1.0;\n", ""),
    ("        if (t < 0.0) t = 0.0;\n", ""),
    ("    const double a = (double)r->zero + t * (double)r->span;",
     "    const double a = t * (double)r->span;"),
    ("    const double x = t * (double)r->dx, y = t * (double)r->dy;",
     "    const double x = t * (double)r->dx, y = t * (double)r->dx;"),
    ("    if (ai < 0) ai += 3600;", ""),
    ("        e->shown = target + e->amp * sinf((float)M_PI * tau);", "        e->shown = target;"),
    ("            if (span_sign >= 0) { while (d > 0.0f) d -= turn; }", "            if (span_sign >= 0) { }"),
    ("        const float sm = tau * tau * (3.0f - 2.0f * tau);", "        const float sm = 1.0f;"),
    ("        const bool back = retro && (span_sign >= 0 ? d < 0.0f : d > 0.0f);", "        const bool back = retro;"),
    ("    if (gone >= ms) { e->kind = EASE_NONE; e->shown = target; return target; }",
     "    if (gone > 10 * ms) { e->kind = EASE_NONE; e->shown = target; return target; }"),
    ("                        r->end == CLK_END_FLYBACK, 1, r->end == CLK_END_WRAP ? 1.0f : 0.0f,",
     "                        r->end == CLK_END_FLYBACK, 1, 0.0f,"),
    ("    const float amp = r->span ? EASE_SETTLE_DEG * 10.0f / (float)abs(r->span)",
     "    const float amp = r->span ? EASE_SETTLE_DEG / (float)abs(r->span)"),
    ("                              : (trav ? EASE_SETTLE_PX / (float)trav : 0.0f);",
     "                              : (trav ? EASE_SETTLE_PX : 0.0f);"),
    ("        if ((r->flags & CLK_FLAG_AUX_SHADOW) || !r->spr.show) continue;",
     "        if (!r->spr.show) continue;"),
    ("    if (cad == CLK_CAD_STEP) return (uint8_t)(arg ? (arg > 60 ? 60 : arg) : 1);",
     "    if (cad == CLK_CAD_STEP) return 1;"),
    ("        if (snd_step_class(g->comps[i].src) == SND_STEP_CAL) out[SND_STEP_CAL] = 1;", ""),
]

def lifted_c(mutations=()):
    cview = open(CLOCK).read()
    for old, new in mutations:
        assert old in cview, "mutation target vanished: %r" % old
        cview = cview.replace(old, new, 1)
    moon = re.search(r"#define MOON_NEW_MOON_S[^\n]*\n#define MOON_SYNODIC_S[^\n]*", cview)
    assert moon, "the moon constants moved out of apps/clock/main.c"
    funcs = "\n".join([
        lift_block(cview, "typedef struct {\n    float    shown, from, to, amp;", "} ease_t;"),
        lift_block(cview, "enum { EASE_NONE = 0,", "};"),
        lift(cview, "static float ease_wrap("),
        lift(cview, "static float ease_pose("),
        lift_block(cview, "#define EASE_SETTLE_DEG", "#define EASE_SETTLE_PX    3.0f"),
        lift(cview, "static bool cad_steps_slow("),
        lift_block(cview, "typedef struct {\n    const tm_t *ti;", "} drive_now_t;"),
        lift(cview, "static bool comp_year_leap("),
        lift(cview, "static bool comp_value("),
        lift(cview, "static double drive_finer("),
        lift(cview, "static int32_t digit_of("),
        lift(cview, "static bool comp_next("),
        lift(cview, "static bool drive_units("),
        lift(cview, "static double drive_t("),
        lift(cview, "static void row_pose("),
        lift(cview, "static double row_ease("),
        lift_block(cview, "enum { SND_STEP_MIN,", "};"),
        lift(cview, "static int snd_step_class("),
        lift(cview, "static uint8_t snd_cad_steps("),
        lift(cview, "static void snd_note_step("),
        lift(cview, "static void snd_steps_scan("),
    ])
    return (C_PRE + moon.group(0) + "\n" + funcs + "\n" + C_HARNESS
            + "int main(void)\n{\n" + C_BODY
            + '    printf("%lu checks, %lu failed\\n", checks, fails);\n'
            + "    return fails ? 1 : 0;\n}\n")

def run_c(mutations=()):
    with tempfile.TemporaryDirectory() as td:
        cf, bf = os.path.join(td, "t.c"), os.path.join(td, "t")
        open(cf, "w").write(lifted_c(mutations))
        cc = subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                             "-Wno-unused-function", "-I", ART, "-o", bf, cf, "-lm"],
                            capture_output=True, text=True)
        if cc.returncode:
            return -1, cc.stderr
        rr = subprocess.run([bf], capture_output=True, text=True)
        return rr.returncode, rr.stdout

def main():
    rc, out = run_c()
    print(out.strip())
    if rc != 0:
        raise SystemExit("the row drive failed a vector")
    print("ok    the row drive: every cadence, end rule, digit, pose and ease vector holds")
    if "--mutations" in sys.argv[1:]:
        survived = []
        for old, new in MUTATIONS:
            rc2, _ = run_c(((old, new),))
            if rc2 == 0:
                survived.append(old)
        for s in survived:
            print("FAIL  mutant survived: %r" % s)
        if survived:
            raise SystemExit("%d of %d mutants survived" % (len(survived), len(MUTATIONS)))
        print("%d mutants all caught" % len(MUTATIONS))

if __name__ == "__main__":
    main()
