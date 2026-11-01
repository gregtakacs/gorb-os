#!/usr/bin/env python3
"""Host bench for D41's drive function (docs/hand-motion-plan.md §3, slice G1).

G1 made the hands and the pointers one drive function: the five HAND slots are
pre-filled driven rows, an AUX row is its own record's columns, and every row
reads CLK_CAD_INHERIT until v5 gives it a cadence column. The gate is that no
v4 dial moves: this bench lifts the shipping functions OUT OF clock_view.c
VERBATIM, compiles them with host gcc -Werror, and samples a whole day at twelve
frames a second under every hand-motion word a v4 theme can carry (sweep,
railway, tick_rate 0/1/2/4/8, the delta-bank beat on and off). Each frame's five
angles must equal, bit for bit, what the pre-G1 hand_angles computed. That
function is kept below as LEGACY_HAND_ANGLES, its text as it shipped.

The cadences no v4 row can name yet (sweep, step, jump, stop-to-go, drag) are new
arithmetic, so they are pinned against hand-worked vectors instead.

Run:  python3 tools/host_drive_test.py              (rc 0 = parity)
      python3 tools/host_drive_test.py --mutations  (proves the checks bite)
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CLOCK = os.path.join(ROOT, "main", "clock", "clock_view.c")
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

# hand_angles as it shipped before G1 (v1.1.43+cd50ff0), with its beat statics
# renamed so it keeps its own state beside the shipping copy.
LEGACY_HAND_ANGLES = r"""
static uint32_t l_bt_ctr = 0;
static int      l_bt_sec = -1;
static double   l_bt_frac = 0.0;
static void legacy_hand_angles(double wall_frac, int hour, int min, int sec,
                               float ang[5])
{
    const bool sweep   = clk_theme.second_sweep;
    const bool railway = clk_theme.second_railway;
    if (sweep && clk_theme.tick_rate > 0 && orb_bg_gif_mode()) {
        const uint32_t tpb = hand_beat_ticks();
        if (tpb > 1) {
            if (sec != l_bt_sec) {
                l_bt_sec = sec;
                l_bt_ctr = 0;
            }
            if ((l_bt_ctr % tpb) == 0) l_bt_frac = wall_frac;
            l_bt_ctr++;
            wall_frac = l_bt_frac;
        }
    }
    float secF;
    if (sweep)
        secF = (float)sec + (float)wall_frac;
    else if (clk_theme.tick_rate > 1)
        secF = (float)sec + (float)((int)(wall_frac * (float)clk_theme.tick_rate))
                           / (float)clk_theme.tick_rate;
    else
        secF = (float)sec;
    float minF = (railway && sweep) ? (float)min
                                    : (float)min + secF / 60.0f;
    float hrF  = (float)(hour % 12) + minF / 60.0f;

    ang[0] = hrF  * 30.0f;
    ang[1] = minF *  6.0f;
    ang[2] = secF *  6.0f;
    ang[3] = 0.0f;
    ang[4] = 0.0f;
}
"""

C_PRE = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "clk_types.h"
static clk_theme_t clk_theme;
static bool     s_gif;
static uint32_t s_gif_tick = 83;
static bool     orb_bg_gif_mode(void)    { return s_gif; }
static uint32_t orb_bg_gif_tick_ms(void) { return s_gif_tick; }
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

/* A whole day at twelve frames a second, one theme's hand words. */
static void day_parity(int sweep, int railway, int rate, int gif)
{
    clk_theme.second_sweep = (uint8_t)sweep;
    clk_theme.second_railway = (uint8_t)railway;
    clk_theme.tick_rate = (uint8_t)rate;
    s_gif = gif;
    s_bt_ctr = 0; s_bt_sec = -1; s_bt_frac = 0.0;
    l_bt_ctr = 0; l_bt_sec = -1; l_bt_frac = 0.0;
    unsigned long bad = 0;
    for (int s = 0; s < 86400; s++) {
        struct tm ti = mk(2026, 11, 1, s / 3600, (s / 60) % 60, s % 60);
        for (int k = 0; k < 12; k++) {
            const double frac = k / 12.0 + 0.013;
            float a[5], b[5];
            legacy_hand_angles(frac, ti.tm_hour, ti.tm_min, ti.tm_sec, a);
            hand_angles(frac, &ti, b);
            checks++;
            if (memcmp(a, b, sizeof a)) {
                if (bad++ < 3)
                    printf("FAIL parity sweep=%d railway=%d rate=%d gif=%d %02d:%02d:%02d+%.3f"
                           " legacy %.6f %.6f %.6f now %.6f %.6f %.6f\n", sweep, railway, rate,
                           gif, ti.tm_hour, ti.tm_min, ti.tm_sec, frac,
                           a[0], a[1], a[2], b[0], b[1], b[2]);
            }
        }
    }
    fails += bad;
}

/* A pointer's v4 pose through drive_aux equals the pre-G1 aux_update arithmetic. */
static void aux_parity(const clk_aux_t *a)
{
    for (int s = 0; s < 86400 * 40; s += 3607) {
        struct tm ti = mk(2026, 1, 1, 0, 0, 0);
        time_t tt = timegm(&ti) + (time_t)s * 9;
        gmtime_r(&tt, &ti);
        for (int k = 0; k < 4; k++) {
            const double frac = k / 4.0;
            const drive_now_t now = { .ti = &ti, .t = tt, .frac = frac };
            int32_t got = 0, want;
            int64_t v1000;
            const bool okw = aux_value_1000(a, &ti, tt, frac, &v1000);
            const bool okg = drive_aux(a, &now, &got);
            checks++;
            if (okw != okg) { fails++; printf("FAIL aux input src=%u\n", a->src); return; }
            if (!okw) continue;
            want = MOTION_IS_SLIDE(a->kind) ? aux_offset_px(a, v1000) : aux_angle_10(a, v1000);
            if (got != want) {
                fails++;
                printf("FAIL aux src=%u kind=%u t=%ld want %d got %d\n", a->src, a->kind,
                       (long)tt, (int)want, (int)got);
                return;
            }
        }
    }
}

/* A new cadence's pose, tenths of a degree (or px), against a hand-worked figure. */
static void pose(const char *what, clk_drive_t d, struct tm ti, double frac, int32_t want)
{
    const drive_now_t now = { .ti = &ti, .t = timegm(&ti), .frac = frac };
    double u;
    checks++;
    if (!drive_units(&d, &now, &u)) { fails++; printf("FAIL %s: no input\n", what); return; }
    const int32_t got = drive_travel(&d, u);
    if (got != want) { fails++; printf("FAIL %s: want %d got %d (units %.6f)\n", what,
                                        (int)want, (int)got, u); }
}

static void units(const char *what, clk_drive_t d, struct tm ti, double frac, double want)
{
    const drive_now_t now = { .ti = &ti, .t = timegm(&ti), .frac = frac };
    double u = -1;
    checks++;
    if (!drive_units(&d, &now, &u) || fabs(u - want) > 1e-9) {
        fails++; printf("FAIL %s: want %.6f got %.6f\n", what, want, u);
    }
}
"""

C_BODY = r"""
    for (int sweep = 0; sweep < 2; sweep++)
        for (int railway = 0; railway < 2; railway++)
            for (int ri = 0; ri < 5; ri++) {
                static const int RATES[5] = { 0, 1, 2, 4, 8 };
                day_parity(sweep, railway, RATES[ri], 0);
                day_parity(sweep, railway, RATES[ri], 1);
            }
    s_gif = 0;

    /* The pointers a v4 theme ships: a date ring, a retrograde date, a sub-dial
     * second that sweeps, a jumping minute, a 24 h hand, a wrapping ring, a slide. */
    const clk_aux_t AUX[] = {
        { .src = CLK_SRC_DATE,    .kind = CLK_MOTION_ROTATE,     .span = 3600, .zero = 0,   .value_min = 1, .value_max = 31 },
        { .src = CLK_SRC_DATE,    .kind = CLK_MOTION_RETROGRADE, .span = 1800, .zero = 2700, .value_min = 1, .value_max = 31 },
        { .src = CLK_SRC_SECOND,  .kind = CLK_MOTION_ROTATE,     .span = 3600, .value_max = 60 },
        { .src = CLK_SRC_MINUTE,  .kind = CLK_MOTION_ROTATE,     .span = 3600, .value_max = 60, .flags = CLK_FLAG_JUMP },
        { .src = CLK_SRC_HOUR24,  .kind = CLK_MOTION_ROTATE,     .span = 3600, .value_max = 24 },
        { .src = CLK_SRC_WEEKDAY, .kind = CLK_MOTION_ROTATE,     .span = -3600, .value_max = 7, .flags = CLK_FLAG_WRAP },
        { .src = CLK_SRC_MONTH,   .kind = CLK_MOTION_SLIDE_X,    .span = 120,  .zero = -20, .value_max = 11 },
        { .src = CLK_SRC_BATTERY, .kind = CLK_MOTION_SLIDE_Y,    .span = 80,   .value_max = 100 },
    };
    for (unsigned i = 0; i < sizeof AUX / sizeof AUX[0]; i++) aux_parity(&AUX[i]);

    /* The HAND slots' own rows, as G2 will fill them: three rotations and two stills. */
    checks++;
    if (HAND_ROW[0].src != CLK_SRC_HOUR12 || HAND_ROW[0].value_max != 12 ||
        HAND_ROW[1].src != CLK_SRC_MINUTE || HAND_ROW[1].value_max != 60 ||
        HAND_ROW[2].src != CLK_SRC_SECOND || HAND_ROW[2].value_max != 60 ||
        HAND_ROW[3].src != CLK_SRC_NONE   || HAND_ROW[4].src != CLK_SRC_NONE)
    { fails++; printf("FAIL the HAND slots' rows moved\n"); }

    const clk_drive_t SEC  = { .src = CLK_SRC_SECOND, .path = CLK_MOTION_ROTATE, .span = 3600, .value_max = 60 };
    const clk_drive_t MIN  = { .src = CLK_SRC_MINUTE, .path = CLK_MOTION_ROTATE, .span = 3600, .value_max = 60 };
    const clk_drive_t DATE = { .src = CLK_SRC_DATE,   .path = CLK_MOTION_ROTATE, .span = 3600, .value_min = 1, .value_max = 31 };
    clk_drive_t d;

    d = SEC; d.cadence = CLK_CAD_STEP; d.cad_arg = 2;
    pose("step 2: 10.6 s shows 10.5", d, mk(2026, 11, 1, 1, 2, 10), 0.6, 630);
    pose("step 2: 10.4 s shows 10.0", d, mk(2026, 11, 1, 1, 2, 10), 0.4, 600);
    d.cad_arg = 8;
    pose("step 8: 10.3 s shows 10.25", d, mk(2026, 11, 1, 1, 2, 10), 0.3, 615);

    d = MIN; d.cadence = CLK_CAD_JUMP;
    pose("jump minute: 12:34:59.9 still on 34", d, mk(2026, 11, 1, 12, 34, 59), 0.9, 2040);
    d.cadence = CLK_CAD_SWEEP;
    pose("sweep minute: 12:34:30 is 34.5", d, mk(2026, 11, 1, 12, 34, 30), 0.0, 2070);
    pose("sweep minute reads the second's own fraction", d, mk(2026, 11, 1, 12, 34, 30), 0.9, 2071);

    d = SEC; d.cadence = CLK_CAD_STOP_TO_GO; d.cad_arg = 585;
    pose("stop-to-go: 29.25 s is half way", d, mk(2026, 11, 1, 1, 2, 29), 0.25, 1800);
    pose("stop-to-go: 58.5 s reaches the top", d, mk(2026, 11, 1, 1, 2, 58), 0.5, 0);
    pose("stop-to-go: 59.9 s rests at the top", d, mk(2026, 11, 1, 1, 2, 59), 0.9, 0);
    d.cad_arg = 0;
    pose("stop-to-go: P defaults to 58.5 s", d, mk(2026, 11, 1, 1, 2, 29), 0.25, 1800);
    d.flags = CLK_FLAG_WRAP;
    pose("stop-to-go rests at the top even on a wrapping row", d, mk(2026, 11, 1, 1, 2, 59), 0.9, 0);
    units("stop-to-go's rest is the domain's end, not past it", d, mk(2026, 11, 1, 1, 2, 59), 0.9, 60.0);

    d = DATE; d.cadence = CLK_CAD_DRAG; d.cad_arg = 120;
    pose("drag: the 5th at 21:00 holds", d, mk(2026, 11, 5, 21, 0, 0), 0.0, 581);
    pose("drag: the 5th at 22:00 starts", d, mk(2026, 11, 5, 22, 0, 0), 0.0, 581);
    pose("drag: the 5th at 23:00 is half way", d, mk(2026, 11, 5, 23, 0, 0), 0.0, 639);
    units("drag: the 5th at 23:30 is 5.75", d, mk(2026, 11, 5, 23, 30, 0), 0.0, 5.75);

    d = (clk_drive_t){ .src = CLK_SRC_MONTH, .path = CLK_MOTION_ROTATE, .span = 3600,
                       .value_max = 12, .cadence = CLK_CAD_DRAG };
    units("drag month: 28 Feb 2027 23:00 is half through its window", d,
          mk(2027, 2, 28, 23, 0, 0), 0.0, 1.5);
    units("drag month: a leap February still has a day to go", d,
          mk(2028, 2, 28, 23, 0, 0), 0.0, 1.0);

    d = (clk_drive_t){ .src = CLK_SRC_HOUR12, .path = CLK_MOTION_RETROGRADE, .span = 1800,
                       .value_max = 12, .cadence = CLK_CAD_SWEEP, .hand = 1 };
    pose("retrograde hour: 06:00 is the middle of the fan", d, mk(2026, 11, 1, 6, 0, 0), 0.0, 900);
    pose("retrograde hour: 11:30", d, mk(2026, 11, 1, 11, 30, 0), 0.0, 1725);
    pose("retrograde hour: 23:59:59 stops at the fan's end", d, mk(2026, 11, 1, 23, 59, 59), 0.99, 1800);
    {
        clk_drive_t w = d;
        w.src = CLK_SRC_HOUR24; w.flags = CLK_FLAG_WRAP; w.cadence = CLK_CAD_JUMP;
        pose("retrograde past its domain on a wrapping row stops at the span", w,
             mk(2026, 11, 1, 18, 0, 0), 0.0, 1800);
    }
    {
        struct tm ti = mk(2026, 11, 1, 6, 0, 0);
        const drive_now_t now = { .ti = &ti, .t = timegm(&ti), .frac = 0.0 };
        checks++;
        if (drive_hand_deg(&d, &now) != 90.0f) {
            fails++; printf("FAIL a HAND row with its own cadence takes the general path\n");
        }
    }

    d = (clk_drive_t){ .src = CLK_SRC_WEEKDAY, .path = CLK_MOTION_ROTATE, .span = 3600,
                       .value_max = 7, .cadence = CLK_CAD_SWEEP };
    units("sweep weekday: Sunday noon", d, mk(2026, 11, 1, 12, 0, 0), 0.0, 6.5);
    d = (clk_drive_t){ .src = CLK_SRC_BATTERY, .path = CLK_MOTION_SLIDE_X, .span = 80,
                       .value_max = 100, .cadence = CLK_CAD_SWEEP };
    {
        struct tm ti = mk(2026, 11, 1, 6, 0, 0);
        const drive_now_t now = { .ti = &ti, .t = timegm(&ti), .frac = 0.0 };
        double u;
        checks++;
        if (drive_units(&d, &now, &u)) { fails++; printf("FAIL a battery row had input\n"); }
    }
    d = (clk_drive_t){ .src = CLK_SRC_DATE, .path = CLK_MOTION_SLIDE_Y, .span = -310,
                       .value_min = 1, .value_max = 31, .cadence = CLK_CAD_JUMP };
    pose("a slide never wraps", d, mk(2026, 10, 31, 12, 0, 0), 0.0, -310);
"""

MUTATIONS = [
    ("shown = floor(f * steps) / steps;", "shown = f;"),
    ("const double run = (v + f) * 600.0 / p;", "const double run = (v + f) * 60.0 / p;"),
    ("*out = run < d->value_max ? run : d->value_max;", "*out = run;"),
    ("if (ti->tm_mon == 1 && comp_year_leap(ti->tm_year + 1900)) dim = 29;", ""),
    ("shown = into > 0.0 ? into / (w < unit_min ? w : unit_min) : 0.0;", "shown = into / w;"),
    ("        if (d->src == CLK_SRC_SECOND)      units = secF;",
     "        if (d->src == CLK_SRC_SECOND)      units = (float)n->ti->tm_sec;"),
    ("                               ? (float)n->ti->tm_min\n",
     "                               ? (float)n->ti->tm_min + secF / 60.0f\n"),
    ("    if (d->src == CLK_SRC_NONE) return 0.0f;\n    if (d->cadence == CLK_CAD_INHERIT) {",
     "    if (d->src == CLK_SRC_NONE) return 0.0f;\n    if (d->cadence == CLK_CAD_INHERIT && d->src != CLK_SRC_HOUR12) {"),
    ("    if (d->src == CLK_SRC_NONE) return 0.0f;\n", ""),
    ("    if (d.cadence == CLK_CAD_INHERIT) {", "    if (d.cadence != CLK_CAD_INHERIT) {"),
    ("    if (MOTION_IS_SLIDE(d->path)) return t;", ""),
    ("    if (d->path == CLK_MOTION_RETROGRADE &&", "    if (0 &&"),
    ("    { .src = CLK_SRC_MINUTE, .path = CLK_MOTION_ROTATE, .hand = 1, .span = 3600, .value_max = 60 },",
     "    { .src = CLK_SRC_MINUTE, .path = CLK_MOTION_ROTATE, .hand = 1, .span = 3600, .value_max = 30 },"),
    ("    case CLK_SRC_MINUTE:  *unit_min = 1.0;        return (ti->tm_sec + frac) / 60.0;",
     "    case CLK_SRC_MINUTE:  *unit_min = 1.0;        return ti->tm_sec / 60.0;"),
]

def lifted_c(mutations=()):
    cview = open(CLOCK).read()
    for old, new in mutations:
        assert old in cview, "mutation target vanished: %r" % old
        cview = cview.replace(old, new, 1)
    slide = re.search(r"#define MOTION_IS_SLIDE\(k\)[^\n]*", cview)
    moon = re.search(r"#define MOON_NEW_MOON_S[^\n]*\n#define MOON_SYNODIC_S[^\n]*", cview)
    assert slide and moon, "MOTION_IS_SLIDE / the moon constants moved out of clock_view.c"
    beat = lift_block(cview, "static uint32_t         s_bt_ctr", "static double           s_bt_frac = 0.0;")
    funcs = "\n".join([
        lift(cview, "static uint32_t hand_beat_ticks("),
        lift_block(cview, "typedef struct {\n    const struct tm *ti;", "static const clk_drive_t HAND_ROW[5] = {"),
        lift_block(cview, "    { .src = CLK_SRC_HOUR12, .path = CLK_MOTION_ROTATE, .hand = 1", "};"),
        lift(cview, "static void hand_angles("),
        lift(cview, "static bool comp_year_leap("),
        lift(cview, "static bool comp_value("),
        lift(cview, "static bool aux_value_1000("),
        lift(cview, "static int32_t aux_travel("),
        lift(cview, "static int32_t aux_angle_10("),
        lift(cview, "static int32_t aux_offset_px("),
        lift(cview, "static double drive_finer("),
        lift(cview, "static bool drive_units("),
        lift(cview, "static int32_t drive_travel("),
        lift(cview, "static float drive_hand_deg(const clk_drive_t *d, const drive_now_t *n)\n{"),
        lift(cview, "static bool drive_aux("),
    ])
    return (C_PRE + slide.group(0) + "\n" + moon.group(0) + "\n" + beat + "\n" + funcs + "\n"
            + LEGACY_HAND_ANGLES + C_HARNESS
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
        raise SystemExit("the drive function moved a v4 dial, or a cadence vector failed")
    print("ok    every v4 hand word, a day at 12 fps: angles identical to pre-G1 hand_angles")
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
