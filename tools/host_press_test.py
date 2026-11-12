#!/usr/bin/env python3
"""host_press_test.py — the OS lent press's pure state machine, compiled on host gcc.

main/input/orb_press.c is a claim, a saturating queue and a drop, and every one of
those decisions is invisible until a real pomodoro claims the press over a real app
on the glass — which the sandbox cannot do. But the machine itself is pure C with
only a claimant slot word, one saturated count per flash slot and a swallowed
counter, so it is compiled VERBATIM here beside a harness that drives the shipped
public API and checks the ruling: latest caller wins, a steal discards the old
claimant's queue, a re-claim keeps the queue, a fifth simultaneous press is swallowed
and never spilled, a non-owner's lend/take is ignored, and a drop (revoke, unload,
trap) clears the claim and every queued press so a dead app never hands them to its
successor. alerts-design §3.2, §6 and §64.13's rulings, on the host, with mutants
that must bite.

Run:  python3 tools/host_press_test.py              (rc 0 = pass)
      python3 tools/host_press_test.py --mutations  (proves the checks bite; rc 0 = all bite)
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INP = os.path.join(ROOT, "main", "input")
SRC = os.path.join(INP, "orb_press.c")
CAP = 8   # orb_press.c's ORB_APP_CAP default (no orb_app.h on the host)

MUTATIONS = [
    # valid_slot bound: a slot the table cannot name would be claimable and queueable
    ("    return slot >= 0 && slot < ORB_APP_CAP;", "    return slot >= 0;"),
    # latest caller wins: without the steal a new claimant neither owns nor clears
    ("        if (s_owner != slot) {", "        if (0) {"),
    # a release is only the claimant's to make
    ("    if (s_owner == slot) {\n        s_count[slot] = 0;\n        s_owner = -1;\n    }\n    return s_owner == slot ? 1 : 0;",
     "    s_count[slot] = 0;\n        s_owner = -1;\n    return s_owner == slot ? 1 : 0;"),
    # a lend aimed at a non-owner is ignored
    ("    if (slot != s_owner) return 0;                 /* a delivery aimed at a non-owner is ignored */",
     "    if (0) return 0;                                /* a delivery aimed at a non-owner is ignored */"),
    # the ceiling is 4: a `>` lets the fifth press through (no swallow)
    ("    if (s_count[slot] >= ORB_PRESS_MAX_LENT) {", "    if (s_count[slot] > ORB_PRESS_MAX_LENT) {"),
    # the swallowed press is still swallowed, just not counted
    ("        s_swallowed++;\n#ifdef ESP_PLATFORM", "        ;\n#ifdef ESP_PLATFORM"),
    # an empty queue answers no press
    ("    if (s_count[slot] <= 0) return 0;\n    s_count[slot]--;",
     "    if (s_count[slot] <= -99) return 0;\n    s_count[slot]--;"),
    # the cold PSRAM image must not read slot 0 as a claimant
    ("        s_owner = -1;\n        for (int i = 0; i < ORB_APP_CAP; i++) s_count[i] = 0;",
     "        for (int i = 0; i < ORB_APP_CAP; i++) s_count[i] = 0;"),
    # §64.17 the double press
    ("    if (s_tap_held && (uint32_t)(now_ms - s_tap_t0) < ORB_TAP_WINDOW_MS) {",
     "    if (s_tap_held) {"),                                                          # a pair with no window
    ("    if (s_tap_held && (uint32_t)(now_ms - s_tap_t0) < ORB_TAP_WINDOW_MS) {",
     "    if (s_tap_held && (uint32_t)(now_ms - s_tap_t0) <= ORB_TAP_WINDOW_MS) {"),    # the edge counted twice
    ("    if (!s_tap_held || (uint32_t)(now_ms - s_tap_t0) < ORB_TAP_WINDOW_MS) return ORB_TAP_NONE;\n    s_tap_held = 0;",
     "    if (!s_tap_held || (uint32_t)(now_ms - s_tap_t0) < ORB_TAP_WINDOW_MS) return ORB_TAP_NONE;\n    ;"),  # delivered forever
    ("    if (!s_tap_held || (uint32_t)(now_ms - s_tap_t0) < ORB_TAP_WINDOW_MS) return ORB_TAP_NONE;",
     "    if ((uint32_t)(now_ms - s_tap_t0) < ORB_TAP_WINDOW_MS) return ORB_TAP_NONE;"),  # a single from nothing
    ("    if (!s_tap_held) return ORB_TAP_NONE;\n    s_tap_held = 0;\n    return ORB_TAP_SINGLE;",
     "    s_tap_held = 0;\n    return ORB_TAP_SINGLE;"),                                  # a cancel invents a press
    ("void orb_tap_drop(void) { s_tap_held = 0; }", "void orb_tap_drop(void) { }"),          # a dropped press lands late
    ("    if (s_tap_held && (uint32_t)(now_ms - s_tap_t0) < ORB_TAP_WINDOW_MS) {\n        s_tap_held = 0;",
     "    if (s_tap_held && now_ms < s_tap_t0 + ORB_TAP_WINDOW_MS) {\n        s_tap_held = 0;"),  # the wrap breaks the pair
]
# NOTE: the clears that run on a steal, a non-owner take and a drop are behaviourally
# equivalent through the public reads alone — a slot can only become owner through a
# path (a steal) that has just zeroed its stale count, and every queue is read only
# through its owner — so those defensive clears are asserted by the checks above (they
# pass) but not separately mutated: an equivalent mutant, which a suite should not list.

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_press.h"

/* One line per observable op, so the whole machine is read through its own API.
 * op grammar (argv from 2):
 *   I           init        C slot on   claim     L slot   lend
 *   T slot      take        G           owner     N slot   lent count
 *   D slot      drop        S           swallowed counter
 */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
        if (o[0] == 'I') { orb_press_init(); printf("INIT own %d\n", orb_press_owner()); }
        else if (o[0] == 'C') {
            int slot = atoi(argv[++i]), on = atoi(argv[++i]);
            int r = orb_press_claim(slot, on);
            printf("CLAIM slot %d r %d own %d lent %d\n", slot, r, orb_press_owner(), orb_press_lent(slot));
        } else if (o[0] == 'L') {
            int slot = atoi(argv[++i]);
            int r = orb_press_lend(slot);
            printf("LEND slot %d r %d own %d\n", slot, r, orb_press_owner());
        } else if (o[0] == 'T') {
            int slot = atoi(argv[++i]);
            int r = orb_press_take(slot);
            printf("TAKE slot %d r %d lent %d own %d\n", slot, r, orb_press_lent(slot), orb_press_owner());
        } else if (o[0] == 'G') {
            printf("OWNER %d\n", orb_press_owner());
        } else if (o[0] == 'N') {
            int slot = atoi(argv[++i]);
            printf("LENT slot %d n %d\n", slot, orb_press_lent(slot));
        } else if (o[0] == 'D') {
            int slot = atoi(argv[++i]);
            orb_press_drop(slot);
            printf("DROP slot %d own %d\n", slot, orb_press_owner());
        } else if (o[0] == 'S') {
            printf("SWALLOW %d\n", orb_press_swallowed());
        } else if (o[0] == 'P') {            /* §64.17: a lendable press at ms */
            unsigned ms = (unsigned)strtoul(argv[++i], NULL, 10);
            int r = orb_tap_press(ms); printf("TAP %d held %d\n", r, orb_tap_pending());
        } else if (o[0] == 'U') {            /* the held press's fate at ms */
            unsigned ms = (unsigned)strtoul(argv[++i], NULL, 10);
            int r = orb_tap_due(ms); printf("DUE %d held %d\n", r, orb_tap_pending());
        } else if (o[0] == 'X') {
            int r = orb_tap_cancel(); printf("CANCEL %d held %d\n", r, orb_tap_pending());
        } else if (o[0] == 'R') {
            orb_tap_drop();
            printf("DROPTAP held %d\n", orb_tap_pending());
        }
    }
    return 0;
}
"""


def build(src, d):
    open(os.path.join(d, "orb_press.c"), "w").write(src)
    open(os.path.join(d, "h.c"), "w").write(HARNESS)
    exe = os.path.join(d, "h")
    r = subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", INP,
                        "-DEXT_RAM_BSS_ATTR=", "-o", exe,
                        os.path.join(d, "orb_press.c"), os.path.join(d, "h.c")],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("compile failed:\n" + r.stderr)
    return exe


def run(exe, *ops):
    args = []
    for a in ops:
        args += str(a).split()
    return [ln.split() for ln in subprocess.run(
        [exe] + args, capture_output=True, text=True).stdout.splitlines()]


def field(ln, name):
    return ln[ln.index(name) + 1]


def checks(exe, quiet=False):
    fails = 0

    def ck(c, what):
        nonlocal fails
        if not quiet:
            print(("ok    " if c else "FAIL  ") + what)
        fails += not c

    def lines(*ops):
        return run(exe, *ops)

    # a cold machine owns nothing and holds no presses
    ls = lines("I", "G", "N 0", "N 3")
    ck(ls[0] == ["INIT", "own", "-1"], "init leaves no claimant: %s" % (ls[0],))
    ck(ls[1] == ["OWNER", "-1"], "a fresh machine lends to nobody")
    ck(field(ls[2], "n") == "0" and field(ls[3], "n") == "0", "an unowned queue reads empty, for every slot")

    # claim makes the caller the owner and answers that it holds
    l = lines("I", "C 2 1")[1]
    ck(field(l, "r") == "1" and field(l, "own") == "2" and field(l, "lent") == "0",
       "claim(2) makes slot 2 the one claimant and starts an empty queue")

    # a fresh claim discards the old claimant's queued presses (latest wins)
    ls = lines("I", "C 2 1", "L 2", "L 2", "C 5 1", "N 5", "G")
    ck(field(ls[4], "lent") == "0", "a steal discards the new claimant's inherited queue")
    ck(ls[6] == ["OWNER", "5"], "the new claimant owns the claim: %s" % (ls[6],))

    # release is only the owner's to make
    ls = lines("I", "C 2 1", "L 2", "C 2 0", "G", "N 2")
    ck(field(ls[3], "own") == "-1" and field(ls[5], "n") == "0",
       "claim(0) by the owner clears the owner and its queued presses")
    ls = lines("I", "C 2 1", "C 5 0", "G")
    ck(ls[3] == ["OWNER", "2"], "claim(0) by a NON-owner is ignored: owner stays 2, not %s" % (ls[3],))

    # a re-claim by the current owner keeps its unread queue (the service asserts every beat)
    ck(field(lines("I", "C 2 1", "L 2", "L 2", "C 2 1", "N 2")[-1], "n") == "2",
       "a re-claim by the same owner does not throw away presses it has not read")

    # saturation: the fifth simultaneous press is swallowed, never spilled
    ls = lines("I", "C 3 1", "L 3", "L 3", "L 3", "L 3", "L 3", "N 3", "S")
    ck(field(ls[6], "r") == "0", "the fifth simultaneous lent press is refused (r=0), not queued")
    ck(field(ls[7], "n") == "4", "the queue saturates at 4, not 5 or 3: %s" % (ls[7],))
    ck(ls[8] == ["SWALLOW", "1"], "the swallowed press is counted exactly once")

    # a take hands back one press and does not release the claim
    ls = lines("I", "C 4 1", "L 4", "L 4", "T 4", "G", "T 4", "T 4")
    ck(field(ls[4], "r") == "1" and field(ls[4], "lent") == "1", "take hands back one press, one still queued")
    ck(ls[5] == ["OWNER", "4"], "a take does not remove the claim")
    ck(field(ls[7], "r") == "0", "the third take finds an empty queue and answers 0")

    # a non-owner cannot drain, lend to, or read the queue
    ls = lines("I", "C 6 1", "L 6", "L 9", "T 9", "N 9")
    ck(field(ls[3], "r") == "0", "a lend aimed at a non-owner slot is ignored (9)")
    ck(field(ls[4], "r") == "0", "a take by a non-owner answers 0 and leaves the queue")
    ck(field(ls[5], "n") == "0", "a non-owner's polled lent() reads 0")

    # drop (revoke / unload / trap) clears the claim and the whole queue
    ls = lines("I", "C 7 1", "L 7", "L 7", "D 7", "G", "N 7")
    ck(field(ls[4], "own") == "-1", "a drop takes the claim back")
    ck(field(ls[6], "n") == "0", "a drop discards the queued presses with the claim")
    ck(lines("I", "D 4", "G")[2] == ["OWNER", "-1"], "dropping a slot that never claimed owns nothing")

    # a slot the table cannot name claims nothing
    ck(field(lines("I", "C %d 1" % (CAP + 1), "G")[1], "r") == "0", "a slot past the table cannot claim (r=0)")
    ck(lines("I", "C %d 1" % (CAP + 1), "G")[2] == ["OWNER", "-1"], "and the refusal leaves no owner")

    # the router's lend/take round trip, exactly as input_router and orb_service use it
    ls = lines("I", "C 2 1", "L 2", "L 2", "L 2", "T 2", "T 2", "T 2", "T 2")
    ck([field(l, "r") for l in ls[5:9]] == ["1", "1", "1", "0"],
       "three presses lent, three taken, the fourth empty — repeated presses replay like physical ones")
    ck(lines("I", "C 2 1", "S")[-1] == ["SWALLOW", "0"], "no swallow without hitting the ceiling")

    # §64.17: the lent press is a double press; a single press is the app's, late by the window
    W = 450   # ORB_TAP_WINDOW_MS
    ls = lines("R", "P 1000", "P 1100")
    ck(ls[1] == ["TAP", "0", "held", "1"], "a first press is held, not lent")
    ck(ls[2] == ["TAP", "2", "held", "0"], "a second inside the window is the pair: the claimant's")
    ls = lines("R", "P 1000", "U %d" % (1000 + W - 1), "U %d" % (1000 + W), "U %d" % (1000 + W + 100))
    ck(ls[2] == ["DUE", "0", "held", "1"], "inside the window the press is still held")
    ck(ls[3] == ["DUE", "1", "held", "0"], "at the window's end it is the app's single press")
    ck(ls[4] == ["DUE", "0", "held", "0"], "and it is delivered once, not again")
    ls = lines("R", "P 1000", "U %d" % (1000 + W), "P %d" % (1000 + W + 50))
    ck(ls[3] == ["TAP", "0", "held", "1"], "a press after a delivered single starts a new pair, never completes the old")
    ls = lines("R", "P 1000", "P %d" % (1000 + W))
    ck(ls[2] == ["TAP", "0", "held", "1"], "a second press at exactly the window is a new first press")
    ls = lines("R", "P 1000", "X", "X")
    ck(ls[2] == ["CANCEL", "1", "held", "0"], "another gesture delivers the held press first")
    ck(ls[3] == ["CANCEL", "0", "held", "0"], "and with nothing held a cancel delivers nothing")
    ls = lines("R", "P 1000", "R", "U 2000")
    ck(ls[2] == ["DROPTAP", "held", "0"] and ls[3] == ["DUE", "0", "held", "0"],
       "a dropped press goes nowhere, not even late")
    ls = lines("R", "P 4294967200", "P 4294967350")
    ck(ls[2] == ["TAP", "2", "held", "0"], "the pair holds across the ms counter's wrap")
    ls = lines("R", "P %d" % (2**32 - W + 100), "P %d" % (2**32 - W + 200))
    ck(ls[2] == ["TAP", "2", "held", "0"], "and when the window, not the clock, crosses the wrap")
    return fails


def main():
    src = open(SRC).read()
    with tempfile.TemporaryDirectory() as d:
        if "--mutations" in sys.argv:
            caught = 0
            for a, b in MUTATIONS:
                if src.count(a) != 1:
                    raise SystemExit("mutation %r applies %d times -- rewrite the bench"
                                     % (a[:40], src.count(a)))
                exe = build(src.replace(a, b, 1), d)
                bit = checks(exe, quiet=True) > 0
                print(("bites " if bit else "MISSED") + "  " + a.strip().replace("\n", "\\n")[:66])
                caught += bit
            print("mutants caught %d/%d" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
        fails = checks(build(src, d))
        print("RESULT: pass" if not fails else "RESULT: %d failed" % fails)
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

