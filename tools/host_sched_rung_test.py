#!/usr/bin/env python3
"""host_sched_rung_test.py — the P8.1 rung-pick rule, mirrored and mutated.

The shipped orb_sched_pick_rung() in main/gfx/orb_sched.c is pure (reads no
globals), but the TU cannot be verbatim-compiled on a host the way idle_dim.c
is — it drags lvgl/display/orb_gfx headers. So this is a mirror, the
host_art_residency_test.py discipline: a Python restatement of the rule, a lint
that reads the SHIPPING C and asserts every branch the mirror depends on is
still present, a truth table, and a mutation harness. If someone edits the pick,
the lint fails and forces a re-read.

The rule (D16/D32, docs §33): the OS owns one flush cadence. Each element either
names a fixed beat (a constrained fps) or is free (0, rides as fast as the glass
goes). The pick is the highest rate <= the measured floor that is a common
multiple of every constrained ask, so no constrained element aliases (a 4 fps
hand at a 15 fps flush advances 3.75 frames a tick and jitters). When the floor
admits no such multiple, fall back to the set's floor (the slowest ask). With no
constrained element, run at the floor.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "main", "gfx", "orb_sched.c")


# ---- the mirror (an exec-able string, so mutants patch text, not this file) --
MIRROR_SRC = '''
def pick_rung(asks, floor_fps):
    if floor_fps == 0:
        floor_fps = 1
    lcm = 1
    any_ = False
    slowest = 0
    for a in asks:
        if a == 0:
            continue
        any_ = True
        if slowest == 0 or a < slowest:
            slowest = a
        g = gcd(lcm, a)
        l = lcm // g * a
        if l == 0 or l > 1000:
            return slowest
        lcm = l
    if not any_:
        return floor_fps
    if lcm > floor_fps:
        return slowest
    return (floor_fps // lcm) * lcm
'''


def gcd(a, b):
    while b:
        a, b = b, a % b
    return a


def _run(src):
    ns = {"gcd": gcd}
    exec(src, ns)
    return ns["pick_rung"]


pick_rung = _run(MIRROR_SRC)



# ---- the lint: the mirror must still match the shipped code -----------------
def lint():
    txt = open(SRC).read()
    # the pick and the gcd helper it calls form one block in the shipped file
    start = txt.index("static uint32_t rung_gcd(")
    body = txt[start:]
    body = body[:body.index("uint32_t orb_sched_pick_rung(")] + \
        txt[txt.index("uint32_t orb_sched_pick_rung("):]
    checks = [
        ("gcd helper present", "static uint32_t rung_gcd("),
        ("free element skips", "if (a == 0) continue;"),
        ("slowest floor tracked", "if (slowest == 0 || a < slowest)"),
        ("gcd reduce before multiply", "uint32_t g = rung_gcd(lcm, a);"),
        ("lcm product", "uint32_t l = lcm / g * a;"),
        ("lcm cap returns set floor", "if (l == 0 || l > 1000) return slowest;"),
        ("no-constrained runs floor", "if (!any) return floor_fps;"),
        ("floor under one cycle runs set floor", "if (lcm > floor_fps) return slowest;"),
        ("highest common multiple", "return (floor_fps / lcm) * lcm;"),
    ]
    fails = []
    for why, needle in checks:
        if needle not in txt[start:]:
            fails.append("%s: %r no longer in orb_sched_pick_rung()" % (why, needle))
    return fails


# ---- the truth table --------------------------------------------------------
# (asks, floor) -> expected. Each row pins a branch; a gate that cannot fire is
# deleted, not kept as decoration (this session's §32 discipline). Expectations
# are computed against the mirror's branches, not guessed.
VECTORS = [
    # no element constrained: the stack runs the measured floor (free/max).
    ([], 5, 5),
    ([0, 0], 8, 8),
    ([0], 15, 15),
    # one constrained hand at 4 fps, a floor that admits a clean multiple.
    ([4], 8, 8),        # 8 = 4*2, no alias
    ([4], 15, 12),      # 15 is NOT a multiple of 4; the hand would advance
                        # 3.75 ticks/frame. Pick 12 (=4*3), the highest clean rung.
    ([4], 16, 16),
    ([4], 3, 4),        # floor below one cycle: fall back to the set's floor (4)
    ([4], 11, 8),
    # second hand every 1 s (1 fps) is a divisor of everything: never binds.
    ([1], 15, 15),
    # two constrained elements: the rung must be a multiple of BOTH (their LCM).
    ([4, 6], 12, 12),   # lcm 12, floor 12 -> 12
    ([4, 6], 23, 12),   # lcm 12, floor 23 -> 23//12=1 -> 12
    ([4, 6], 11, 4),    # lcm 12 > floor 11 -> fall back to the set's floor,
                        # the slowest constrained ask = min(4,6) = 4
    ([3, 5], 30, 30),   # lcm 15, floor 30 -> 30
    ([3, 5], 14, 3),    # lcm 15 > floor 14 -> set's floor slowest = min(3,5) = 3
    # free element mixed with a constrained one: the free one rides, the beat binds.
    ([4, 0], 15, 12),
    # the fallback returns the SLOWEST ask, not the first: pins min-tracking.
    ([6, 4], 11, 4),    # lcm 12 > floor 11, but the first ask (6) is not the min;
                        # the set's floor is 4, so first-wins (6) is wrong
    # the LCM cap is an overflow guard, independent of the (small) panel floor:
    # it fires on lcm > 1000 even when the floor could still hold one cycle.
    ([40, 27], 1100, 27),  # lcm 1080 > the 1000 cap -> set's floor min(40,27)=27;
                           # without the cap the pick would read 1080 (a real ask,
                           # tested at a floor deliberately above the panel range)
    # a constrained ask past the panel's LCM range collapses to the set's floor.
    ([700, 800], 30, 700),  # lcm 5600 > 1000 -> return slowest (700)
    ([60], 30, 60),     # lcm 60 > floor 30 -> set's floor slowest = 60
]


def check_vectors():
    fails = []
    for asks, floor, exp in VECTORS:
        got = pick_rung(asks, floor)
        if got != exp:
            fails.append("pick_rung(%s, floor=%d) = %d, expected %d"
                         % (asks, floor, got, exp))
    return fails


# ---- mutation harness: each perturbation of the mirror must change an answer --
# A gate that no mutant can trip is a gate that cannot fire, so it is deleted.
# Each mutant patches MIRROR_SRC text, re-execs it, and must flip at least one
# truth-table answer.
MUTS = [
    ("drop free-element skip",
     "        if a == 0:\n            continue", "        if False:\n            continue"),
    ("ignore the lcm, run floor",
     "    return (floor_fps // lcm) * lcm", "    return floor_fps"),
    ("fallback to floor not set floor",
     "    if lcm > floor_fps:\n        return slowest", "    if lcm > floor_fps:\n        return floor_fps"),
    ("no-constrained returns 1",
     "    if not any_:\n        return floor_fps", "    if not any_:\n        return 1"),
    ("lcm cap disabled",
     "        if l == 0 or l > 1000:\n            return slowest", "        if False:\n            return slowest"),
    ("slowest floor not tracked (first wins)",
     "        if slowest == 0 or a < slowest:\n            slowest = a", "        if slowest == 0:\n            slowest = a"),
]


def mut_count():
    caught = 0
    total = 0
    for _why, old, new in MUTS:
        total += 1
        if old not in MIRROR_SRC:
            print("  (mutant text did not apply: %s — mirror drifted)" % _why)
            continue
        mutated = MIRROR_SRC.replace(old, new)
        f = _run(mutated)
        if any(f(asks, fl) != exp for asks, fl, exp in VECTORS):
            caught += 1
    return caught, total


def main():
    fails = []
    fails += lint()
    fails += check_vectors()
    n = 0
    for asks, floor, exp in VECTORS:
        n += 1
    caught, total = mut_count()
    print("host_sched_rung_test: %d vectors, lint %s, mutants caught %d/%d"
          % (n, "clean" if not lint() else "FAIL", caught, total))
    if fails:
        for f in fails:
            print("  FAIL: %s" % f)
        return 1
    if total and caught < total:
        print("  FAIL: %d/%d mutants escaped a gate (a gate that cannot fire is deleted)"
              % (total - caught, total))
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
