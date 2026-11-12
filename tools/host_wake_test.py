#!/usr/bin/env python3
"""host_wake_test.py — orb_wake's pure decision, compiled on host gcc (plan §64.6, pomodoro-0-wake).

main/app/orb_wake.c is the whole policy behind the SDK's orb_wake(seconds): the 3..15
clamp, the once-a-minute rate limit, and the two policy refusals (the OS's quiet hours,
and a display the owner switched off). None of it shows on a dial a worker can reach —
it runs in the firmware glue, which this sandbox has no ESP-IDF to build — and every
part of it can be wrong silently: a clamp that lets a 0 or a 60 through hands an app a
glass it does not own or dims one that only wanted a glance; a rate limit that reads a
clock wrap as a fresh wake bars a timer that has not gone off in days; a refusal that
slips behind the rate-limit answer tells an app to retry a wake that is never coming.

So the shipped file is compiled VERBATIM here beside a harness that reads back the
decision, and pinned on the exact edges the design names: the clamp's ends, the 60 s
edge on both sides and across the uptime wrap, the never-woken sentinel, and the two
refusals with their precedence over the rate limit. This is the ring-1 / host_ring_test
pattern: pure C, gcc -Werror, and mutations that must each make a check fail (an escaped
mutant is a non-zero exit under --mutations).

Run:  python3 tools/host_wake_test.py              (rc 0 = pass)
      python3 tools/host_wake_test.py --mutations  (proves the checks bite; rc 0 = all bite)
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP = os.path.join(ROOT, "main", "app")

# (verdict, seconds) as the harness prints them: the three esp_err_t numbers and the
# clamped span. Named here so the checks read in the SDK's terms, not raw hex.
OK = 0
INVALID_STATE = 0x103
NOT_ALLOWED = 0x10D

MUTATIONS = [
    # the clamp's low end: without the floor a request of 0 asks for no light at all.
    ("    if (s < ORB_WAKE_MIN_S) return ORB_WAKE_MIN_S;",
     "    if (s < ORB_WAKE_MIN_S) return s;"),
    # the clamp's high end: without the ceiling an app holds a glass it does not own.
    ("    if (s > ORB_WAKE_MAX_S) return ORB_WAKE_MAX_S;",
     "    if (s > ORB_WAKE_MAX_S) return s;"),
    # the quiet-hours refusal, dropped (display-off still refuses, quiet does not).
    ("    if (quiet || display_off) {", "    if (display_off && !quiet) {"),
    # the display-off refusal, dropped (quiet still refuses, display-off does not).
    ("    if (quiet || display_off) {", "    if (quiet && !display_off) {"),
    # both refusals answer OK.
    ("        d.verdict = ORB_WAKE_NOT_ALLOWED;", "        d.verdict = ORB_WAKE_OK;"),
    # the rate limit never refuses.
    ("    if (last_wake_ms != 0 &&", "    if (0 &&"),
    # the never-woken sentinel inverted: a first ask is read as a rate-limit refusal.
    ("    if (last_wake_ms != 0 &&", "    if (last_wake_ms == 0 &&"),
    # the 60 s edge made inclusive: exactly a minute since the last wake is barred.
    ("        (uint32_t)(now_ms - last_wake_ms) < ORB_WAKE_MIN_GAP_MS) {",
     "        (uint32_t)(now_ms - last_wake_ms) <= ORB_WAKE_MIN_GAP_MS) {"),
]

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_wake.h"

static unsigned long ul(const char *s) { return strtoul(s, NULL, 0); }

int main(int argc, char **argv)
{
    if (argc == 7 && !strcmp(argv[1], "decide")) {
        orb_wake_decision_t d = orb_wake_decide(
            (uint32_t)ul(argv[2]), (uint32_t)ul(argv[3]), (uint32_t)ul(argv[4]),
            ul(argv[5]) != 0, ul(argv[6]) != 0);
        printf("DEC %d %u\n", (int)d.verdict, (unsigned)d.seconds);
        return 0;
    }
    fprintf(stderr, "usage: h decide now_ms last_wake_ms requested_s quiet display_off\n");
    return 2;
}
"""


def build(src, d):
    open(os.path.join(d, "orb_wake.c"), "w").write(src)
    open(os.path.join(d, "h.c"), "w").write(HARNESS)
    exe = os.path.join(d, "h")
    r = subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", APP,
                        "-o", exe, os.path.join(d, "orb_wake.c"), os.path.join(d, "h.c")],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("compile failed:\n" + r.stderr)
    return exe


def run(exe, now, last, req, quiet, off):
    out = subprocess.run([exe, "decide", str(now), str(last), str(req), str(quiet), str(off)],
                         capture_output=True, text=True).stdout.split()
    if len(out) != 3:
        return None
    return int(out[1]), int(out[2])


def checks(exe, quiet=False):
    fails = 0

    def ck(c, what):
        nonlocal fails
        if not quiet:
            print(("ok    " if c else "FAIL  ") + what)
        fails += not c

    def dec(now, last, req, q, off):
        return run(exe, now, last, req, q, off)

    # --- the clamp: the ends and the middle, computed whatever the verdict ---------
    ck(dec(100000, 0, 0, 0, 0) == (OK, 3), "a request of 0 clamps up to the 3 s floor")
    ck(dec(100000, 0, 1, 0, 0) == (OK, 3), "one second clamps up to the floor")
    ck(dec(100000, 0, 2, 0, 0) == (OK, 3), "two seconds clamps up to the floor")
    ck(dec(100000, 0, 3, 0, 0) == (OK, 3), "three seconds is the floor, left alone")
    ck(dec(100000, 0, 7, 0, 0) == (OK, 7), "a span inside the window is passed through")
    ck(dec(100000, 0, 15, 0, 0) == (OK, 15), "fifteen seconds is the ceiling, left alone")
    ck(dec(100000, 0, 16, 0, 0) == (OK, 15), "sixteen seconds clamps down to the ceiling")
    ck(dec(100000, 0, 100, 0, 0) == (OK, 15), "a minute's ask clamps down to the ceiling")
    ck(dec(100000, 0, 0xFFFFFFFF, 0, 0) == (OK, 15), "a wild ask clamps down to the ceiling")

    # --- the rate limit: the 60 s edge both sides, and the never-woken sentinel ------
    ck(dec(100000, 0, 5, 0, 0) == (OK, 5), "a first wake (never woken) is never rate-limited")
    ck(dec(30000, 0, 5, 0, 0) == (OK, 5), "a first wake on a young uptime answers OK (the sentinel short-circuits)")
    ck(dec(100000, 70000, 5, 0, 0) == (INVALID_STATE, 5), "a wake 30 s ago is refused as too soon")
    ck(dec(100000, 41000, 5, 0, 0) == (INVALID_STATE, 5), "a wake 59 s ago is refused as too soon")
    ck(dec(100000, 40000, 5, 0, 0) == (OK, 5), "exactly 60 s since the last wake is allowed")
    ck(dec(100000, 39999, 5, 0, 0) == (OK, 5), "61 s since the last wake is allowed")
    ck(dec(5000000, 1, 5, 0, 0) == (OK, 5), "a wake long ago is allowed")

    # --- the two policy refusals, and their precedence over the rate limit -----------
    ck(dec(100000, 0, 5, 1, 0) == (NOT_ALLOWED, 5), "quiet hours refuse the wake")
    ck(dec(100000, 0, 5, 0, 1) == (NOT_ALLOWED, 5), "the owner's display-off refuses the wake")
    ck(dec(100000, 0, 5, 1, 1) == (NOT_ALLOWED, 5), "quiet and display-off together refuse the wake")
    ck(dec(100000, 99000, 5, 1, 0) == (NOT_ALLOWED, 5),
       "quiet outranks a too-soon ask: an app barred and recently woken sees the bar")
    ck(dec(100000, 99000, 5, 0, 1) == (NOT_ALLOWED, 5),
       "display-off outranks a too-soon ask: an app barred and recently woken sees the bar")

    # --- the uptime-clock wrap: the elapsed test reads a wrap as age, never as fresh --
    ck(dec(100, 0xFFFFFF90, 5, 0, 0) == (INVALID_STATE, 5),
       "a real wake straddling the uptime wrap is still too soon (unsigned elapsed)")
    ck(dec(100000, 0xFFFF0000, 5, 0, 0) == (OK, 5),
       "a wake long before the wrap reads as old across it, and is allowed")

    return fails


def main():
    src = open(os.path.join(APP, "orb_wake.c")).read()
    with tempfile.TemporaryDirectory() as d:
        if "--mutations" in sys.argv:
            caught = 0
            for a, b in MUTATIONS:
                if src.count(a) != 1:
                    raise SystemExit("mutation %r applies %d times -- rewrite the bench" % (a[:40], src.count(a)))
                exe = build(src.replace(a, b, 1), d)
                bit = checks(exe, quiet=True) > 0
                print(("bites " if bit else "MISSED") + "  " + a.strip()[:70])
                caught += bit
            print("mutants caught %d/%d" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
        fails = checks(build(src, d))
        print("RESULT: pass" if not fails else "RESULT: %d failed" % fails)
        return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
