#!/usr/bin/env python3
"""Host suite for main/menu/knob_edit.c: the shell's knob-edited values
(docs/knob-edit-plan.md §1.1).

    python3 tools/host_knob_edit_test.py [--mutations]

Compiles the shipped knob_edit.c with gcc and checks the speed model (slow turns step 1,
fast ones up to the row's largest step, a pause starts slow again) and the move (snapping
to multiples of a fast step, clamping, wrapping an hour). --mutations breaks each guarded
line in a private copy and expects a check to fail.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "menu", "knob_edit.c")

HARNESS = r"""
#include <stdio.h>
#include "knob_edit.h"
static int fails, checks;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    const knob_range_t vol = { 0, 100, false, 5 }, hour = { 0, 23, true, 3 }, mins = { 1, 120, false, 5 };
    /* the move */
    CK(knob_edit_move(37, 1, 1, &vol) == 38, "slow up: 38");
    CK(knob_edit_move(37, -1, 1, &vol) == 36, "slow down: 36");
    CK(knob_edit_move(37, 1, 5, &vol) == 40, "fast up snaps to 40: %d", knob_edit_move(37, 1, 5, &vol));
    CK(knob_edit_move(40, 1, 5, &vol) == 45, "fast up from a multiple: 45");
    CK(knob_edit_move(37, -1, 5, &vol) == 35, "fast down snaps to 35: %d", knob_edit_move(37, -1, 5, &vol));
    CK(knob_edit_move(35, -1, 5, &vol) == 30, "fast down from a multiple: 30");
    CK(knob_edit_move(37, 2, 5, &vol) == 45, "two fast detents: 45");
    CK(knob_edit_move(98, 1, 5, &vol) == 100, "clamps at the top: %d", knob_edit_move(98, 1, 5, &vol));
    CK(knob_edit_move(2, -1, 5, &vol) == 0, "clamps at the bottom");
    CK(knob_edit_move(100, 1, 1, &vol) == 100, "the top holds");
    const knob_range_t odd = { 0, 99, false, 5 };
    CK(knob_edit_move(98, 1, 5, &odd) == 99, "a fast step past a top that is no multiple clamps to it: %d", knob_edit_move(98, 1, 5, &odd));
    CK(knob_edit_move(1, -1, 1, &mins) == 1, "a minimum above zero holds");
    CK(knob_edit_move(3, -1, 5, &mins) == 1, "a fast turn down clamps to the minimum, not 0: %d", knob_edit_move(3, -1, 5, &mins));
    CK(knob_edit_move(23, 1, 1, &hour) == 0, "an hour wraps 23 -> 0");
    CK(knob_edit_move(0, -1, 1, &hour) == 23, "an hour wraps 0 -> 23");
    CK(knob_edit_move(22, 1, 3, &hour) == 0, "a fast hour wraps onto 24 = 0: %d", knob_edit_move(22, 1, 3, &hour));
    CK(knob_edit_move(1, -1, 3, &hour) == 0, "a fast hour down snaps to 0");
    CK(knob_edit_move(0, -1, 3, &hour) == 21, "a fast hour down wraps to 21: %d", knob_edit_move(0, -1, 3, &hour));

    /* the speed */
    knob_speed_t s; knob_speed_reset(&s);
    CK(knob_speed_step(&s, 1, 1000000, 5) == 1, "the first turn is slow");
    CK(knob_speed_step(&s, 1, 1150000, 5) == 1, "a turn 150 ms on is still slow");
    int64_t t = 1150000; int32_t st = 1;
    for (int i = 0; i < 12; i++) { t += 20000; st = knob_speed_step(&s, 1, t, 5); }
    CK(st == 5, "a run at 50 detents a second reaches the largest step: %d", st);
    CK(knob_speed_step(&s, 1, t + 20000, 1) == 1, "a row whose largest step is 1 never speeds up");
    CK(knob_speed_step(&s, 1, t + 400000, 5) == 1, "a pause past 220 ms starts slow again");
    knob_speed_reset(&s);
    t = 1000000; knob_speed_step(&s, 1, t, 5);
    for (int i = 0; i < 12; i++) { t += 55000; st = knob_speed_step(&s, 1, t, 5); }
    CK(st > 1 && st < 5, "a middling run (18 a second) steps between: %d", st);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
"""

MUTANTS = (
    ("the snap up", "v = (floor_div(v, step) + 1) * step;", "v += step;"),
    ("the snap down", "v = (floor_div(v + step - 1, step) - 1) * step;", "v -= step;"),
    ("the wrap", "v = r->min + ((v - r->min) % span + span) % span;", "(void)span;"),
    ("the clamp low", "} else if (v < r->min) {", "} else if (0) {"),
    ("the clamp high", "} else if (v > r->max) {", "} else if (0) {"),
    ("the idle reset", "now_us - s->last_us > KNOB_EDIT_IDLE_US", "0"),
    ("the per-detent loop", "for (int32_t n = delta < 0 ? -delta : delta; n > 0; n--) {", "for (int32_t n = 1; n > 0; n--) {"),
)


def run(text):
    d = tempfile.mkdtemp(prefix="knobedit.")
    src, h, exe = os.path.join(d, "knob_edit.c"), os.path.join(d, "h.c"), os.path.join(d, "t")
    open(src, "w").write(text)
    open(h, "w").write(HARNESS)
    c = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Werror", "-I", os.path.dirname(SRC), src, h, "-o", exe],
                       capture_output=True, text=True)
    if c.returncode:
        return None, c.stderr
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout


def main():
    text = open(SRC).read()
    rc, out = run(text)
    if rc is None:
        print("COMPILE FAILED\n" + out)
        return 1
    print(out.strip())
    bad = rc != 0
    if "--mutations" in sys.argv:
        caught = 0
        for name, a, b in MUTANTS:
            if text.count(a) != 1:
                print("FAIL mutant anchor %r found %d times" % (name, text.count(a)))
                bad = True
                continue
            mrc, _ = run(text.replace(a, b))
            if mrc == 0:
                print("FAIL uncaught mutant: %s" % name)
                bad = True
            else:
                caught += 1
        print("mutants: %d of %d caught" % (caught, len(MUTANTS)))
    print("ALL PASS" if not bad else "FAILED")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
