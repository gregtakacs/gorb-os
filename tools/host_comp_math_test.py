#!/usr/bin/env python3
"""Host parity bench for the complication render maths in clock_view.c.

tools/host_sd_test.py proves the READER. This proves the PAINTER's arithmetic:
it lifts the pure value/frame/digit functions OUT OF THE SHIPPING FILE VERBATIM,
compiles them with host gcc -Werror, and pins them against tools/theme_schema.py
(the bake's own frame_index/strip_indices) and python datetime (the calendar
truth the bake's tile ORDER promises). Drift between what the bake laid out and
what the dial selects is a wrong tile on the wrist; this is the only bench that
sees it without a wrist.

Run:  python3 tools/host_comp_math_test.py              (rc 0 = parity)
      python3 tools/host_comp_math_test.py --mutations   (proves checks bite)
"""
import datetime, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CLOCK = os.path.join(ROOT, "main", "clock", "clock_view.c")
HEADER = os.path.join(ROOT, "main", "art", "orb_theme.h")
sys.path.insert(0, HERE)
import theme_schema as ts
import bake_clock as bc

# The moon spec is owned HERE, so a firmware edit cannot silently move it: one
# known new moon (2000-01-06 18:14 UTC) and the mean synodic month, seconds.
MOON_NEW_MOON_S = 947182440
MOON_SYNODIC_S  = 2551443

def lift(src, sig):
    """One function's text, from its signature to the first column-0 }."""
    i = src.index(sig)
    j = src.index("\n}", i) + 2
    return src[i:j]

C_PRE = """
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
"""

C_HARNESS = """
static uint64_t fails = 0;
static void chk_frame(int32_t v, uint16_t count, int16_t vmin, uint16_t scale,
                      uint16_t want)
{
    uint16_t got = comp_frame_index(v, count, vmin, scale);
    if (got != want) { fails++;
        printf("FAIL frame value=%d count=%u min=%d scale=%u want=%u got=%u\\n",
               (int)v, (unsigned)count, (int)vmin, (unsigned)scale,
               (unsigned)want, (unsigned)got); }
}
static void chk_digits(int32_t value, int npos, bool zp, uint16_t count,
                       const int16_t *want)
{
    int16_t out[16];
    for (int p = 0; p < npos; p++) out[p] = 123;      /* must overwrite all */
    comp_strip_digits(value, npos, zp, count, out);
    for (int p = 0; p < npos; p++)
        if (out[p] != want[p]) { fails++;
            printf("FAIL digits value=%d npos=%d zp=%d count=%u pos=%d"
                   " want=%d got=%d\\n", (int)value, npos, (int)zp,
                   (unsigned)count, p, (int)want[p], (int)out[p]); }
}
typedef struct { int wd, mon, mday, hour, min, sec, yday, year;
                 int v_wd, v_mon, v_dom, v_h12, v_h24, v_min, v_sec, v_ampm,
                 v_doy, v_q, v_week; } tv_t;
static void chk_tm(const tv_t *t)
{
    struct tm ti;
    memset(&ti, 0, sizeof ti);
    ti.tm_wday = t->wd; ti.tm_mon = t->mon; ti.tm_mday = t->mday;
    ti.tm_hour = t->hour; ti.tm_min = t->min; ti.tm_sec = t->sec;
    ti.tm_yday = t->yday; ti.tm_year = t->year;
    const struct { uint8_t src; int want; } cases[] = {
        { CLK_SRC_WEEKDAY, t->v_wd },     { CLK_SRC_MONTH, t->v_mon },
        { CLK_SRC_DATE, t->v_dom },       { CLK_SRC_HOUR12, t->v_h12 },
        { CLK_SRC_HOUR24, t->v_h24 },     { CLK_SRC_MINUTE, t->v_min },
        { CLK_SRC_SECOND, t->v_sec },     { CLK_SRC_AMPM, t->v_ampm },
        { CLK_SRC_DAY_OF_YEAR, t->v_doy },{ CLK_SRC_QUARTER, t->v_q },
        { CLK_SRC_WEEK, t->v_week },
    };
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        int32_t out = -999;
        bool ok = comp_value(cases[k].src, &ti, 0, &out);
        if (!ok || out != cases[k].want) { fails++;
            printf("FAIL src %u yday=%d wd=%d want=%d got=%d\\n",
                   (unsigned)cases[k].src, ti.tm_yday, ti.tm_wday,
                   cases[k].want, (int)out); }
    }
}
static void chk_moon(long long t, int want)
{
    int32_t out = -999;
    bool ok = comp_value(CLK_SRC_MOON_AGE, (const struct tm *)0, (time_t)t, &out);
    if (!ok || out != want) { fails++;
        printf("FAIL moon t=%lld want=%d got=%d\\n", t, want, (int)out); }
}
static void chk_novalue(uint8_t src)
{
    int32_t out = 7;
    if (comp_value(src, (const struct tm *)0, 0, &out)) { fails++;
        printf("FAIL src %u claimed an input it has no source for\\n",
               (unsigned)src); }
}
"""

# The eight single-token drifts that put a wrong tile on the glass. Each must
# make run_c() return non-zero, or the bench is blind (--mutations proves it).
MUTATIONS = [
    ("(idx + 8) / 16",               "idx / 16"),
    ("idx %= (int64_t)count",        "idx = (int64_t)count - 1"),
    ("(ti->tm_wday + 6) % 7",        "ti->tm_wday"),
    ("out[first + p]",               "out[p]"),
    ("yday1 + 10 - wd7",           "yday1 + 7 - wd7"),
    ("out = (int32_t)(m / 86400);",  "out = (int32_t)(m / 86000);"),
    ("ti->tm_mon / 3 + 1",           "ti->tm_mon / 3"),
    ("buf[p] - '0'",                 "((buf[p] - '0') + 1)"),
]

def build_body():
    """The main() body: C lines, interleaved decls and chk_* calls. The check
    count is the number of chk_ lines. Every expectation comes from
    theme_schema (the bake's own maths) or datetime (the calendar) — never
    from re-reading the C, which is the whole point."""
    lines = []
    def decl(s): lines.append("    " + s)
    def chk(s):  lines.append("    " + s)

    # frame_index across the grid a bake ships on: weekday 7, month 12, moon
    # 30, second 60; both date-style value_mins; sub/super-unit scales; and
    # the clamp/wrap edges the value domain can walk into.
    for count in (1, 2, 7, 12, 16, 30, 60):
        for vmin in (0, 1, 5):
            for scale in (1, 2, 16, 17, 32, 160):
                for v in list(range(0, 70, 3)) + [-5, -1, count + 11]:
                    chk("chk_frame(%d,%d,%d,%d,%d);"
                        % (v, count, vmin, scale,
                           ts.frame_index(v, count, vmin, scale)))

    # Strip digits: every box width, both paddings, and a 7-glyph set so the
    # reduce-into-the-glyph-set path runs off the decimal happy path.
    k = 0
    for npos in range(1, 9):
        for zp in (False, True):
            for count in (10, 7):
                for v in (0, 1, 7, 10, 23, 42, 99, 100, 123, 999, 65535):
                    idx = ts.strip_indices(v, npos, zp, count)
                    want = [-1] * (npos - len(idx)) + list(idx)
                    decl("static const int16_t w%d[] = {%s};"
                         % (k, ",".join(str(x) for x in want)))
                    chk("chk_digits(%d,%d,%s,%u,w%d);"
                        % (v, npos, "true" if zp else "false", count, k))
                    k += 1

    # Wall-clock sources: named calendar edges + a sweep of 2020..2031, with
    # expectations straight from datetime. tm_wday is Sun==0, and the bake's
    # tile 0 is MONDAY — the two Python asserts below are what pin that order
    # (weekday tile == the date's own name, same for the month) through the
    # same value the C produces via chk_tm.
    dates = [datetime.date(2024, 2, 29), datetime.date(2023, 1, 1),
             datetime.date(2021, 1, 1), datetime.date(2020, 12, 31),
             datetime.date(2026, 1, 1),  datetime.date(2026, 12, 31),
             datetime.date(2025, 3, 9),   datetime.date(2024, 12, 31),
             datetime.date(2025, 12, 29), datetime.date(2027, 1, 1),
             datetime.date(2030, 12, 31)]
    d = datetime.date(2020, 1, 1)
    while d <= datetime.date(2031, 12, 31):
        dates.append(d)
        d += datetime.timedelta(days=11)
    for j, dd in enumerate(dates):
        wd = (dd.weekday() + 1) % 7                     # Mon0 -> Sun==0 tm_wday
        yday = (dd - datetime.date(dd.year, 1, 1)).days
        hour, minute, sec = (j * 7) % 24, (j * 13) % 60, (j * 29) % 60
        decl("static const tv_t tv%d = { %d,%d,%d,%d,%d,%d,%d,%d,"
             " %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d };" % (
                 j, wd, dd.month - 1, dd.day, hour, minute, sec, yday,
                 dd.year - 1900,
                 (wd + 6) % 7, dd.month - 1, dd.day, hour % 12, hour, minute,
                 sec, int(hour >= 12), yday + 1, (dd.month - 1) // 3 + 1,
                 dd.isocalendar()[1]))
        chk("chk_tm(&tv%d);" % j)
        wf = ts.frame_index((wd + 6) % 7, len(bc._WEEKDAY_TILES), 0, 16)
        assert bc._WEEKDAY_TILES[wf] == dd.strftime("%a").upper(), dd
        mf = ts.frame_index(dd.month - 1, len(bc._MONTH_TILES), 0, 16)
        assert bc._MONTH_TILES[mf] == dd.strftime("%b").upper(), dd

    # Moon: new moon reads tile 0, whole days step whole frames, tile 29 holds
    # across its whole day, and the last second before the wrap still reads 29.
    for m in (0, 1, 2, 5, 100):
        base = MOON_NEW_MOON_S + m * MOON_SYNODIC_S
        chk("chk_moon(%dLL, 0);"  % base)
        chk("chk_moon(%dLL, 14);" % (base + 14 * 86400))
        chk("chk_moon(%dLL, 29);" % (base + 29 * 86400))
        chk("chk_moon(%dLL, 29);" % (base + 29 * 86400 + 11 * 3600))  # < 29d12h44m
        chk("chk_moon(%dLL, 29);" % (base - 1))
    for off in (0, 3, 7, 12, 14, 21, 27, 29):
        chk("chk_moon(%dLL, %d);" % (MOON_NEW_MOON_S + off * 86400 + 43200, off))
    # Hour-by-hour across the whole mean month: whole-day vectors cannot tell
    # the divisor from any other divisor that shares their quotients — the
    # first hour whose 86000-quotient runs ahead of its 86400-quotient does.
    for h in range(0, 709):                       # 708 h = 29 d 12 h < 29 d 12 h 44 m
        chk("chk_moon(%dLL, %d);" % (MOON_NEW_MOON_S + h * 3600,
                                     (h * 3600) // 86400))

    # Sources with no input on this hardware refuse, so the render hides them.
    for s in ("CLK_SRC_NONE", "CLK_SRC_TEXT", "CLK_SRC_VALUE",
              "CLK_SRC_BATTERY", "CLK_SRC_POWER_RESERVE"):
        chk("chk_novalue(%s);" % s)

    n = sum(1 for l in lines if l.strip().startswith("chk_"))
    return "\n".join(lines) + "\n", n

def lifted_c(mutations=()):
    """One TU: the CLK_SRC_* enum from the shipping header verbatim, the five
    pure functions from the shipping .c verbatim, the harness, and vectors."""
    cview = open(CLOCK).read()
    cheader = open(HEADER).read()
    m = re.search(r"enum \{[^{}]*CLK_SRC_NONE[^{}]*CLK_SRC__MAX[^{}]*\};", cheader)
    assert m, "CLK_SRC_* enum not found in orb_theme.h — did it change shape?"
    mo = re.search(r"#define MOON_NEW_MOON_S[^\n]*\n#define MOON_SYNODIC_S[^\n]*",
                   cview)
    assert mo, ("moon constants not found adjacent in clock_view.c — the vectors "
                "own the spec (MOON_*_S above); the C must match them verbatim")
    assert "947182440" in mo.group(0) and "2551443" in mo.group(0), \
        "clock_view.c moved the moon off this spec — realign it or update the " \
        "vectors deliberately, in the same commit"
    funcs = "\n".join([
        lift(cview, "static uint16_t comp_frame_index("),
        lift(cview, "static void comp_strip_digits("),
        # One lift, two functions: comp_year_leap is a one-liner, so the range
        # runs to the next column-0 } — which closes comp_iso_week, its only
        # caller. Lifting both separately would emit comp_iso_week twice.
        lift(cview, "static bool comp_year_leap("),
        lift(cview, "static bool comp_value("),
    ])
    body, _ = build_body()
    src = (C_PRE + m.group(0) + "\n" + mo.group(0) + "\n" + funcs + "\n" + C_HARNESS
           + "int main(void)\n{\n" + body + "    return fails ? 1 : 0;\n}\n")
    for old, new in mutations:
        assert old in src, "mutation target vanished: %r" % old
        src = src.replace(old, new, 1)
    return src

def run_c(mutations=()):
    """(rc, out). rc == -1 means the MUTATION no longer compiles — dead by
    another route, which the mutation loop counts as caught (a real source
    edit can never take that branch: build4 gates the shipping file)."""
    with tempfile.TemporaryDirectory() as td:
        cf, bf = os.path.join(td, "t.c"), os.path.join(td, "t")
        open(cf, "w").write(lifted_c(mutations))
        cc = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra",
                             "-Werror", "-o", bf, cf],
                            capture_output=True, text=True)
        if cc.returncode:
            return -1, cc.stderr
        rr = subprocess.run([bf], capture_output=True, text=True)
        return rr.returncode, rr.stdout

def main():
    _, n = build_body()
    rc, out = run_c()
    if rc != 0:                     # the unmutated lift MUST compile: a -1 here
        print(out)                   # is a broken file, not a caught mutant
        raise SystemExit("render maths diverged from the bake's — see FAIL lines")
    print("all complication render-maths checks passed (%d vectors)" % n)

    if "--mutations" in sys.argv[1:]:
        survived = []
        for old, new in MUTATIONS:
            rc2, _ = run_c(((old, new),))
            if rc2 == 0:
                survived.append("%s -> %s" % (old, new))
        for s in survived:
            print("SURVIVED:", s)
        if survived:
            raise SystemExit("mutation survivors — the bench is blind")
        print("%d mutants all caught" % len(MUTATIONS))

if __name__ == "__main__":
    main()
