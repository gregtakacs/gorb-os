#!/usr/bin/env python3
"""Host bench for the knob's dial (docs/app-platform-plan.md §51).

Two pure headers, compiled as they stand with host gcc -Werror:

- main/knob/knob_accel.h, the OS's acceleration for an app that calls orb_knob_dial:
  slow clicks stay one step, quick ones in one direction count 2 then 4, a turn the
  other way starts over.
- apps/sdk/orb_dial.h, the app's dial: the ends stop and never wrap, a fast step that
  reaches or crosses home stops on it and the rest of that spin is swallowed, a pause
  or a reversal lets go, and idle returns home.

Run:  python3 tools/host_dial_test.py              (rc 0 = pass)
      python3 tools/host_dial_test.py --mutations  (proves the checks bite)
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ACCEL = os.path.join(ROOT, "main", "knob", "knob_accel.h")
DIAL = os.path.join(ROOT, "apps", "sdk", "orb_dial.h")

MUTATIONS = [
    (ACCEL, "if (dir == a->dir) {", "if (1) {"),
    (ACCEL, "if (gap < KNOB_ACCEL_X4_US) mult = 4;", "if (gap < KNOB_ACCEL_X4_US) mult = 2;"),
    (ACCEL, "a->dir = dir;", "a->dir = 1;"),
    (DIAL, "(d->pos > 0 && to <= 0) || (d->pos < 0 && to >= 0)", "(d->pos > 0 && to < 0) || (d->pos < 0 && to > 0)"),
    (DIAL, "const bool fast = delta > 1 || delta < -1;", "const bool fast = delta > 1;"),
    (DIAL, "if (d->held == dir && same_spin) return false;", "if (d->held == dir) return false;"),
    (DIAL, "if (to < d->lo) to = d->lo;", ""),
    (DIAL, "now_ms - d->last_ms < (int64_t)d->idle_ms", "now_ms - d->last_ms < 0"),
    (DIAL, "if (d->pos > d->hi) d->pos = d->hi;", ""),
]

HARNESS = r"""
#include <stdio.h>
static int fails;
#define CK(c, label) do { if (c) printf("ok    %s\n", label); else { printf("FAIL  %s\n", label); fails++; } } while (0)

int main(void)
{
    /* ---- acceleration ---- */
    knob_accel_t a = { 0 };
    CK(knob_accel(&a, 1, 1000000) == 1, "the first click is one step");
    CK(knob_accel(&a, 1, 1300000) == 1, "a click 300 ms later is one step");
    CK(knob_accel(&a, 1, 1370000) == 2, "70 ms after the last, the same way: two");
    CK(knob_accel(&a, 1, 1400000) == 4, "30 ms after: four");
    CK(knob_accel(&a, -1, 1420000) == -1, "a turn the other way starts over at one");
    CK(knob_accel(&a, 0, 1430000) == 0, "no detent, no step");
    CK(knob_accel(&a, -1, 1440000) == -4, "and builds again the new way");
    CK(knob_accel(&a, -1, 1800000) == -1, "a pause brings it back to one");

    /* ---- the dial: Weather's shape, five days anticlockwise, 23 hours clockwise ---- */
    orb_dial_t d = { .lo = -5, .hi = 23, .idle_ms = ORB_DIAL_IDLE_MS };
    int64_t t = 1000;
    CK(orb_dial_turn(&d, 1, t) && d.pos == 1, "one click clockwise: the next hour");
    t += 300; orb_dial_turn(&d, 1, t); t += 300; orb_dial_turn(&d, 1, t);
    CK(d.pos == 3, "slow clicks step one at a time");
    t += 300; orb_dial_turn(&d, -1, t); t += 300; orb_dial_turn(&d, -1, t); t += 300; orb_dial_turn(&d, -1, t);
    CK(d.pos == 0, "slow clicks back land on home");
    t += 300; orb_dial_turn(&d, -1, t);
    CK(d.pos == -1, "and slow clicks cross it: tomorrow");
    t += 300; orb_dial_turn(&d, 1, t);

    /* a fast spin out from home goes, and stops at the end */
    t += 300; orb_dial_turn(&d, 1, t);
    for (int i = 0; i < 10; i++) { t += 30; orb_dial_turn(&d, 4, t); }
    CK(d.pos == 23, "a fast spin runs to the end and stops there");
    t += 30;
    CK(!orb_dial_turn(&d, 4, t) && d.pos == 23, "the end does not wrap");

    /* a fast spin back stops on home and swallows the rest of the spin */
    for (int i = 0; i < 5; i++) { t += 30; orb_dial_turn(&d, -4, t); }
    CK(d.pos == 3, "a fast spin back comes down four at a time");
    t += 30; orb_dial_turn(&d, -4, t);
    CK(d.pos == 0, "the step that would cross home stops on it");
    for (int i = 0; i < 6; i++) { t += 30; orb_dial_turn(&d, -4, t); }
    CK(d.pos == 0, "the rest of that spin is swallowed");
    t += 400; orb_dial_turn(&d, -1, t);
    CK(d.pos == -1, "a pause lets go: the next click crosses");
    t += 300; orb_dial_turn(&d, 1, t);
    CK(d.pos == 0, "back on home");

    /* the days side the same way, and a reversal lets go at once */
    t += 300; orb_dial_turn(&d, -1, t);
    for (int i = 0; i < 3; i++) { t += 30; orb_dial_turn(&d, -4, t); }
    CK(d.pos == -5, "a fast spin anticlockwise stops at the fifth day");
    t += 400; orb_dial_turn(&d, 1, t); t += 30; orb_dial_turn(&d, 4, t);
    CK(d.pos == 0, "a flick back from the days stops on home");
    t += 30; orb_dial_turn(&d, 4, t);
    CK(d.pos == 0, "still the same spin, still home");
    t += 30; orb_dial_turn(&d, -1, t);
    CK(d.pos == -1, "a turn the other way lets go at once");
    t += 300; orb_dial_turn(&d, 1, t);
    t += 300; orb_dial_turn(&d, 3, t);
    CK(d.pos == 3, "a fast step that starts on home leaves it");
    t += 300; orb_dial_turn(&d, -3, t);
    CK(d.pos == 0, "a fast step that lands exactly on home holds there");

    /* idle */
    t += 300; orb_dial_turn(&d, 1, t); orb_dial_turn(&d, 1, t + 300); t += 300;
    CK(!orb_dial_idle(&d, t + ORB_DIAL_IDLE_MS - 1) && d.pos == 2, "not yet idle");
    CK(orb_dial_idle(&d, t + ORB_DIAL_IDLE_MS) && d.pos == 0, "idle returns home");
    CK(!orb_dial_idle(&d, t + 2 * ORB_DIAL_IDLE_MS), "home stays home");

    /* new ends keep the position inside them */
    t += 300; orb_dial_turn(&d, 1, t);
    for (int i = 0; i < 10; i++) { t += 30; orb_dial_turn(&d, 4, t); }
    orb_dial_range(&d, -3, 10);
    CK(d.pos == 10 && d.lo == -3 && d.hi == 10, "a shorter answer pulls the position in");
    orb_dial_range(&d, 2, -2);
    CK(d.lo == 0 && d.hi == 0 && d.pos == 0, "ends that leave out home are pinned to it");
    return fails ? 1 : 0;
}
"""


def source(mutation=None):
    texts = {p: open(p).read() for p in (ACCEL, DIAL)}
    if mutation:
        path, old, new = mutation
        if old not in texts[path]:
            raise SystemExit("mutation %r does not apply -- rewrite the bench" % old)
        texts[path] = texts[path].replace(old, new, 1)
    return "".join(t.replace("#pragma once", "") + "\n" for t in texts.values()) + HARNESS


def run(code):
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "d.c"), os.path.join(d, "d")
        open(c, "w").write(code)
        r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-o", exe, c],
                           capture_output=True, text=True)
        if r.returncode:
            return 2, r.stderr
        p = subprocess.run([exe], capture_output=True, text=True)
        return p.returncode, p.stdout


def main():
    if "--mutations" in sys.argv:
        caught = 0
        for m in MUTATIONS:
            rc, _ = run(source(m))
            print(("bites " if rc else "MISSED") + "  " + os.path.basename(m[0]) + ": " + m[1][:60])
            caught += bool(rc)
        print("%d of %d mutants caught" % (caught, len(MUTATIONS)))
        return 0 if caught == len(MUTATIONS) else 1
    rc, out = run(source())
    print(out, end="")
    return rc


if __name__ == "__main__":
    sys.exit(main())
