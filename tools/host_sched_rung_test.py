#!/usr/bin/env python3
"""host_sched_rung_test.py — the P8.1 rung-pick rule, mirrored and mutated.

The shipped orb_sched_pick_rung() in main/gfx/orb_sched.c is pure (reads no
globals), but the TU cannot be verbatim-compiled on a host the way idle_dim.c
is — it drags lvgl/display/orb_gfx headers. So this is a mirror, the
host_art_residency_test.py discipline: a Python restatement of the rule, a lint
that reads the SHIPPING C and asserts every branch the mirror depends on is
still present, a truth table, and a mutation harness. If someone edits the pick,
the lint fails and forces a re-read.

The rule (D16/D32, docs §33/§40, P8.2): the OS owns one flush cadence, and the
pick reads the MEASURED floor (never an ask). Each element asks with a mode:
FREE rides whatever the working cap allows and contributes no constraint;
FLUID asks a continuous motion capped at its rate (a ceiling, not a divisor);
DISTINCT names a fixed beat it must not alias (a divisor). The working cap is
the measured floor clamped to [4..15], tightened lower by every fluid ask. The
answer is the highest LADDER RUNG {12,8,4,2,1} at or under the cap that every
distinct ask divides into, so no constrained element aliases (a 4 fps hand at a
15 fps flush advances 3.75 frames a tick and jitters). A distinct ask is a
ladder rung by install-time construction. When no rung under the cap is a common
multiple, fall back to the set's own floor (the slowest distinct ask, clamped
into the band — which may sit above the working cap; that is the alias-free
honest answer). With no distinct ask at all, run at the working cap.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "main", "gfx", "orb_sched.c")


# ---- the mirror (an exec-able string, so mutants patch text, not this file) --
MIRROR_SRC = '''
def pick_rung(asks, floor_fps):
    CEIL = 15
    FLOOR = 4
    LADDER = (12, 8, 4, 2, 1)
    def clamp(f):
        if f < FLOOR:
            return FLOOR
        if f > CEIL:
            return CEIL
        return f
    if floor_fps == 0:
        floor_fps = 1
    cap = clamp(floor_fps)
    lcm = 1
    over = False
    any_distinct = False
    slowest = 0
    for fps, mode in asks:
        if fps == 0:
            continue
        if mode == 'free':
            continue
        if mode == 'fluid':
            c = clamp(fps)
            if c < cap:
                cap = c
            continue
        any_distinct = True
        if slowest == 0 or fps < slowest:
            slowest = fps
        if not over:
            g = gcd(lcm, fps)
            l = lcm // g * fps
            if l == 0 or l > 1000:
                over = True
            else:
                lcm = l
    if not any_distinct:
        return cap
    if not over:
        for rung in LADDER:
            if rung > cap:
                continue
            if rung % lcm != 0:
                continue
            return rung
    return clamp(slowest)
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
        ("clamp into band", "if (fps < ORB_SCHED_PANEL_FLOOR) return ORB_SCHED_PANEL_FLOOR;"),
        ("working cap starts at measured floor", "uint32_t cap = rung_clamp(floor_fps ? floor_fps : 1);"),
        ("0 ask binds nothing", "if (a->fps == 0) continue;"),
        ("free ask rides the cap", "if (a->mode == ORB_SCHED_FREE) continue;"),
        ("fluid is a cap not a divisor", "if (a->mode == ORB_SCHED_FLUID) {"),
        ("fluid tightens the cap", "if (c < cap) cap = c;"),
        ("slowest floor tracked", "if (slowest == 0 || a->fps < slowest)"),
        ("gcd reduce before multiply", "uint32_t g = rung_gcd(lcm, a->fps);"),
        ("lcm product", "uint32_t l = lcm / g * a->fps;"),
        ("lcm overflow sets over", "if (l == 0 || l > 1000) over = 1;"),
        ("no distinct runs the cap", "if (!any_distinct) return cap;"),
        ("rung skipped over cap", "if (rung > cap) continue;"),
        ("rung must divide cleanly", "if (rung % lcm != 0) continue;"),
        ("fallback clamps the set floor", "return rung_clamp(slowest);"),
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
    # No constraint at all: run at the measured floor (clamped into [4..15]).
    ([], 5, 5),
    ([(0, 'free')], 15, 15),
    ([(5, 'free')], 15, 15),           # a nonzero FREE ask still rides the cap
    # FLUID is a ceiling, not a divisor: it only tightens the working cap.
    ([(8, 'fluid')], 15, 8),           # continuous motion capped at 8
    ([(20, 'fluid')], 15, 15),         # a fluid ask above the ceiling no-ops
    ([(2, 'fluid')], 15, 4),           # a fluid ask below the band clamps to the floor
    ([(8, 'fluid'), (4, 'distinct')], 15, 8),   # fluid binds cap, distinct divides rung
    # DISTINCT is a divisor of the rung, so a slow ask need not slow the tick.
    ([(2, 'distinct')], 15, 12),       # a 2 fps ask still lets the tick run 12
    ([(4, 'distinct')], 15, 12),       # 15 not a multiple of 4; highest clean rung 12
    ([(4, 'distinct')], 8, 8),         # cap 8: rung 8 divides clean
    ([(8, 'distinct')], 15, 8),        # 12 aliases 8 (1.5), so pick 8
    ([(12, 'distinct')], 15, 12),
    ([(6, 'distinct')], 15, 12),       # 6 divides 12 clean
    # A distinct ask the ladder cannot serve under the cap falls back to the
    # set's own floor (the slowest ask), clamped.
    ([(5, 'distinct')], 15, 5),        # no rung <=15 is a multiple of 5 -> floor 5
    ([(60, 'distinct')], 15, 15),      # ask above the ceiling: clamp to the band
    ([(4, 'distinct')], 3, 4),         # floor below the band clamps the cap up to 4
    # Two distinct asks: the rung must divide into BOTH (their LCM).
    ([(4, 'distinct'), (6, 'distinct')], 15, 12),   # lcm 12
    ([(4, 'distinct'), (6, 'distinct')], 11, 4),    # lcm 12 > cap 11 -> set floor min=4
    ([(8, 'distinct'), (12, 'distinct')], 15, 8),   # lcm 24 exceeds the ladder -> set floor min=8
    # The fallback returns the SLOWEST ask, not the first: pins min-tracking.
    ([(6, 'distinct'), (4, 'distinct')], 11, 4),    # first ask 6 is not the min (4)
    # The LCM overflow guard is separate from the (small) band: a huge LCM sets
    # over and skips the ladder, falling back to the clamped set floor.
    ([(40, 'distinct'), (27, 'distinct')], 15, 15), # lcm 1080 > 1000 -> over -> clamp(27)=15
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
    ("free element no longer rides (treated as distinct)",
     "        if mode == 'free':\n            continue",
     "        if False:\n            continue"),
    ("fluid no longer tightens the cap",
     "            if c < cap:\n                cap = c",
     "            if False:\n                cap = c"),
    ("ladder stops respecting the cap",
     "            if rung > cap:\n                continue",
     "            if False:\n                continue"),
    ("ladder stops checking the alias",
     "            if rung % lcm != 0:\n                continue",
     "            if False:\n                continue"),
    ("no-distinct returns 1, not the cap",
     "    if not any_distinct:\n        return cap",
     "    if not any_distinct:\n        return 1"),
    ("fallback skips the band clamp",
     "    return clamp(slowest)",
     "    return slowest"),
    ("slowest floor not tracked (first wins)",
     "        if slowest == 0 or fps < slowest:\n            slowest = fps",
     "        if slowest == 0:\n            slowest = fps"),
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
