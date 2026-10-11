#!/usr/bin/env python3
"""Host bench for the idle-dim motion decision in main/power/idle_dim.c.

The dim's timing is Kconfig and the brightness write is the vendor driver's, so
neither needs a bench. What does is the one predicate that decides whether a
person gets their dial back: idle_motion_decide. Lifted VERBATIM from the
shipping file (the rule tools/host_comp_math_test.py follows), compiled with host
gcc -Werror, and pinned against the two ways a threshold is wrong:

  too sensitive  -> the panel's refresh harmonics or a desk vibration hold the
                    face awake, and the owner turns the feature off believing
                    it broken;
  not sensitive  -> a pick-up leaves a dark dial, which on an AMOLED reads as a
                    dead device.

Both are invisible in the code and obvious on the wall, so they are pinned here.

Run:  python3 tools/host_idle_dim_test.py              (rc 0 = pass)
      python3 tools/host_idle_dim_test.py --mutations  (proves the checks bite)
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "main", "power", "idle_dim.c")

# The scale imu.c configures: ±8 g at 4096 LSB per g, matching the default
# ORB_IDLE_MOTION_MG of 40 -> 163 LSB. Kept in one place so the vectors below
# read in milli-g, the unit the Kconfig help uses.
LSB_PER_G = 4096
THRESH_MG = 40
THRESH = (THRESH_MG * LSB_PER_G) // 1000

FAILS = []
NL = chr(10)   # the file builds generated C by string concatenation; this keeps
            # the escapes in that generated code down to one level


def ok(cond, label):
    print(("ok    " if cond else "FAIL  ") + label)
    if not cond:
        FAILS.append(label)


def lifted_c(mutations=(), func="idle_motion_decide", argtype="idle_motion_t"):
    """The shipping function, extracted verbatim, in a compile harness.

    Parameterised by function because the file now ships two decisions worth
    pinning on a host: the per-sample motion verdict, and the run-of-N that turns
    verdicts into a wake. Both are pure, both are invisible until the orb is on a
    real desk, and the second one is what stops an untouched dial dimming and
    immediately un-dimming itself.
    """
    src = open(SRC).read()
    m = re.search(r"^bool " + func + r"\(" + argtype + r"[^\n]*\n\{", src, re.M)
    if not m:
        raise SystemExit(func + " not found in %s — this bench is "
                         "pinning a function that no longer exists" % SRC)
    # Cut at the first brace in column 0 after the opening one. Counting braces
    # instead is wrong here: the prose comments carry braces and parentheses, and
    # a counter cannot tell a comment from code. This file's style indents every
    # nested block, so a column-0 brace is only ever the function's own.
    tail = src[m.end():]
    end = re.search(r"^\}", tail, re.M)
    if not end:
        raise SystemExit(func + " never closes in %s" % SRC)
    body = src[m.start():m.end() + end.end()]
    for old, new2 in mutations:
        if old not in body:
            raise SystemExit("mutation %r does not apply — rewrite the bench to "
                             "match the shipped code" % old)
        body = body.replace(old, new2)
    return """
#include <stdint.h>
#include <stdbool.h>
typedef struct { int32_t base[3]; int32_t worst; bool have_base; } idle_motion_t;
typedef struct { uint8_t run; bool fired; } idle_wake_t;
%s
""" % body


def run(vectors, mutations=()):
    """Compile the lifted function with a driver replaying every case, and report
    whether any of them came out wrong (True == a mismatch happened).

    A vector is (label, samples, expect_motion) and optionally a fourth element:
    the deviation high-water, in LSB, that run must have recorded. That number is
    what ORB_IDLE_MOTION_MG gets set from on the bench, so it is pinned with the
    same seriousness as the verdict itself — a wrong worst is a dial that wakes
    for nothing (threshold too low) or for nobody (threshold too high).
    """
    d = tempfile.mkdtemp(prefix="orb_idle_dim_")
    cf, bf = os.path.join(d, "t.c"), os.path.join(d, "t")
    calls = []
    for vec in vectors:
        label, seq, expect = vec[0], vec[1], vec[2]
        want_worst = vec[3] if len(vec) > 3 else None
        arr = ", ".join("{ %d, %d, %d }" % tuple(int(v) for v in s) for s in seq)
        wchk = ("" if want_worst is None else
                NL + '        if (st.worst != %d) { fails += 1; bad_worst = %s; }'
                % (want_worst, len(calls) + 1))
        calls.append(
            "    {" + NL +
            '        static const int16_t v[][3] = { %s };' % arr + NL +
            '        idle_motion_t st = { .have_base = false };' + NL +
            '        bool motion = false;' + NL +
            '        for (unsigned i = 0; i < sizeof(v)/sizeof(v[0]); i++)' + NL +
            '            motion = idle_motion_decide(&st, v[i], THRESH);' + NL +
            '        printf("%%-8s %%s\\n", motion ? "MOTION" : "STILL", "%s");' % label + NL +
            '        fails += (motion != %s) ? 1 : 0;' % ("true" if expect else "false") +
            wchk + NL +
            '    }')
    open(cf, "w").write(lifted_c(mutations) +
        '#include <stdio.h>' + NL +
        '#define THRESH %d' % THRESH + NL +
        'static int bad_worst = 0;' + NL +
        'static int run_all(void) {' + NL +
        '    int fails = 0;' + NL +
        chr(10).join(calls) + NL +
        '    if (bad_worst) {' + NL +
        '        puts("worst-deviation high-water wrong; see the");' + NL +
        '        puts("ORB_IDLE_MOTION_MG help for how it is used.");' + NL +
        '        puts("First failing vector (in the order printed above):");' + NL +
        '        printf("%d", bad_worst);' + NL +
        '        puts("");' + NL +
        '    }' + NL +
        '    return fails;' + NL +
        '}' + NL +
        'int main(void) { return run_all(); }' + NL)
    cc = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra",
                         "-Werror", "-o", bf, cf], capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("host build failed:" + NL + cc.stderr)
    out = subprocess.run([bf], capture_output=True, text=True)
    sys.stdout.write(out.stdout)
    return out.returncode != 0


# A level orb at 4096 LSB/g: gravity down Z, X and Y at rest.
REST = (0, 0, 4096)


def dev(axis, mg):
    """A rest sample with `mg` milli-g added on `axis`."""
    s = list(REST)
    s[axis] += (mg * LSB_PER_G) // 1000
    return s


VECTORS = [
    # Perfectly still, forever: must never wake.
    ("still_forever", [REST] * 20, False),
    # Noise either side of rest, well under the threshold: must not wake.
    ("noise_10mg", [dev(0, 10), dev(0, -10), dev(2, 12), dev(1, -8)] * 5, False),
    # A pick-up: 300 mg on Z in one sample.
    ("pickup_300mg", [REST, dev(2, 300)], True),
    # A knock on the case: a sharp 120 mg transient on X.
    ("knock_120mg", [REST, dev(0, 120)], True),
    # Just over the line on any single axis, in either direction.
    ("edge_45mg_pos", [REST, dev(1, 45)], True),
    ("edge_45mg_neg", [REST, dev(1, -45)], True),
    # The anti-latch property the 1/8 follow exists for: the orb is TILTED to a
    # new attitude and then left there. The tilt itself reads as motion (the
    # owner wants "the case shifting" to wake it), but once it is held still the
    # baseline must catch up and the face must go idle again. A detector without
    # the follow latches MOTION forever here, which is the failure that makes the
    # dim look broken whenever the orb is moved to a shelf.
    ("settles_at_new_attitude", [REST] * 3 + [dev(0, 160)] * 24, False),
    # A continuing shake must stay MOTION on every sample (measured 40/40 at the
    # threshold): an orb being carried across the room must not dim mid-walk. The
    # alternation matters — a one-way step lets the baseline chase a constant
    # offset, which is exactly what the settle case above relies on.
    ("continuing_shake_stays_motion",
     [REST] + [dev(0, 250 if k % 2 else -250) for k in range(40)], True),
    # ...and settling must not cost sensitivity: a knock AFTER the settle still
    # wakes it. Without this pair, "always still" would pass the test above.
    ("knock_after_settle", [REST] * 3 + [dev(0, 160)] * 24 + [dev(1, 220)], True),
    # The boundary, stated as the header states it ("more than", not "at least"):
    # a sample exactly on the threshold is still, one LSB over is motion. Pinned
    # at 40 mg because that is 163 LSB on both sides of the divide, so an
    # off-by-one in the comparison shows up here and nowhere else.
    ("exactly_at_thresh", [REST, dev(0, THRESH_MG)], False),
    ("just_above_thresh", [REST, dev(0, THRESH_MG + 1)], True),
    # Cold seed: the first sample is never motion, whatever it says. This is
    # what stops a re-entry into the dark after the orb was carried somewhere
    # from waking the dial for no reason at all.
    ("cold_seed_never_motion", [dev(2, 900)], False),
    # ...and the second sample does judge it against that seed.
    ("second_sample_judges", [dev(2, 900), REST], True),
    # The high-water itself. A still orb must record a worst of EXACTLY zero:
    # the baseline converges onto rest, so nothing is left to subtract. That is
    # the number the bench procedure in Kconfig leans on — prop the orb still and
    # read it — and if it were anything but zero, no threshold could be set from
    # it and the "lower it until the dim stops holding" advice would be nonsense.
    ("still_records_zero_worst", [REST] * 20, False, 0),
    # A 300 mg pick-up must record its own height, judged against the converged
    # rest baseline: (300 * 4096) // 1000 = 1228 LSB. Pinned exactly, because a
    # high-water that silently tracked the baseline instead of the sample would
    # still report "motion" correctly and only ever mislead the threshold.
    ("pickup_records_its_height",
     [REST] * 3 + [dev(2, 300)] + [REST] * 3, False, (300 * LSB_PER_G) // 1000),
]

# Each mutant must make at least one vector come out wrong. A survivor means the
# vector set does not actually constrain that behaviour.
MUTATIONS = [
    ("> (int32_t)thresh_lsb", ">= (int32_t)thresh_lsb"),           # off-by-one
    ("st->base[i] += ((int32_t)s[i] - st->base[i]) >> 3;",         # no follow
     "st->base[i] += 0;"),
    ("st->have_base = true;", "st->have_base = false;"),           # never seeds
    ("if (!st->have_base) {", "if (false) {"),                     # seeds always
    ("if (a > worst) worst = a;", "if (a < worst) worst = a;"),    # worst-pick
    # forgetting the high-water entirely: verdicts stay right, threshold-setting
    # quietly reads a still orb as if it had never been sampled at all
    ("if (worst > st->worst) st->worst = worst;", ""),
    # high-water taken from the raw sample instead of the deviation: inflates on a
    # level orb (Z reads 1 g) and would push the threshold above a real pick-up
    ("if (worst > st->worst) st->worst = worst;",
     "if (s[2] > st->worst) st->worst = s[2];"),
]



# ---- the wake run-of-N, the part that decides whether the dial stays dimmed ---
#
# (label, sample verdicts, need, how many wakes the run must produce).
# Counted, not just "did it wake": a detector that wakes on every sample of a
# continuing shake looks identical from the wall and is a different bug.
# (label, sample verdicts, need, wakes, samples-held-lit)
#
# The fifth element is not decoration: it pins that the run actually reaches and
# stays at the run length, which is the quantity the task stamps its idle clock
# from. Stamping from the one-shot wake instead of from the run passes every
# wake-count check above and dims an orb being carried across a room — the other
# way this feature fails. The task's use of st.run is on hardware; what is pinned
# here is that st.run itself behaves.
WAKE_VECTORS = [
    ("single_jolt_rejected",        [True],                        3, 0, 0),
    ("run_of_three_wakes_once",     [True, True, True, False],     3, 1, 1),
    ("continuing_shake_wakes_once", [True] * 40,                   3, 1, 38),
    ("alternating_never_wakes",     [True, False] * 20,            3, 0, 0),
    ("need_one_is_passthrough",     [True],                        1, 1, 1),
    ("broken_run_must_not_accumulate",
     [True, True, False, True, True, True],                        3, 1),
    ("re_arms_after_a_still_sample",
     [True, True, True, False, True, True, True],                  3, 2),
    # A carried orb reports motion on every sample for minutes and must lift the
    # dial once, not once per poll: the fired latch is what does that, and 300
    # samples is long enough that a plain uint8_t run counter wraps inside it,
    # which is exactly why the latch has to be the thing that gates the wake.
    ("long_run_wakes_once",         [True] * 300,                  3, 1, 298),
    # ...and the latch must not be sticky past the run: still, then a fresh run of
    # three, has to be credited as a second wake.
    ("long_run_then_rearm",         [True] * 300 + [False] + [True] * 3, 3, 2, 299),
]

WAKE_MUTATIONS = [
    ("if (!motion) {", "if (!motion && false) {"),           # run never resets
    ("st->fired = false;", "/* dropped */"),               # never re-arms
    ("st->run++;", "st->run += 0;"),                      # run never grows
    ("st->run >= need", "st->run > need"),                 # off-by-one
    ("if (need < 1) need = 1;", "if (need < 2) need = 2;"),  # passthrough lost
    ("if (st->run < 255) st->run++;", "st->run++;"),         # wraps at 256
    ("&& !st->fired", ""),                                 # fires every sample
    ("if (st->run >= need && !st->fired) {",               # one-shot, not a run:
     "if (!st->fired) {"),                               #   dims an orb mid-carry
]


def run_wake(vectors, mutations=()):
    """Same lift-and-replay as run(), for the wake run-of-N. Reports the first
    vector whose wake COUNT came out wrong, which is the failure mode here: the
    verdict is still true, so a still/still check would have missed it."""
    d = tempfile.mkdtemp(prefix="orb_idle_wake_")
    cf, bf = os.path.join(d, "t.c"), os.path.join(d, "t")
    calls = []
    for n, vec in enumerate(vectors, start=1):
        label, seq, need, want = vec[0], vec[1], vec[2], vec[3]
        want_kept = vec[4] if len(vec) > 4 else None
        arr = ", ".join("true" if b else "false" for b in seq)
        calls.append(
            "    {" + NL +
            '        static const bool v[] = { %s };' % arr + NL +
            '        idle_wake_t st = { .run = 0, .fired = false };' + NL +
            '        int fires = 0, kept = 0;' + NL +
            '        for (unsigned i = 0; i < sizeof(v)/sizeof(v[0]); i++) {' + NL +
            '            if (idle_motion_wake_decide(&st, v[i], %d)) fires++;' % need + NL +
            '            if (st.run >= %d) kept++;' % need + NL +
            '        }' + NL +
            '        fails += (fires != %d) ? 1 : 0;' % want + NL +
            '        if (fires != %d && !bad) bad = %d;' % (want, n) + NL +
            ('        fails += (kept != %d) ? 1 : 0;' % want_kept + NL +
             '        if (kept != %d && !bad) bad = %d;' % (want_kept, n)
             if want_kept is not None else '') + NL +
            '    }')
        print("       wake vector %d: %s (need=%d, %d samples -> %d wakes, "
              "%s held)" % (n, label, need, len(seq), want,
                            "-" if want_kept is None else str(want_kept)))
    open(cf, "w").write(lifted_c(mutations, func="idle_motion_wake_decide",
                                 argtype="idle_wake_t") +
        '#include <stdio.h>' + NL +
        'static int bad = 0;' + NL +
        'static int run_all(void) {' + NL +
        '    int fails = 0;' + NL +
        chr(10).join(calls) + NL +
        '    if (bad) {' + NL +
        '        puts("a wake run-of-N came out wrong: first bad vector #");' + NL +
        '        printf("%d", bad);' + NL +
        '        puts("");' + NL +
        '    }' + NL +
        '    return fails;' + NL +
        '}' + NL +
        'int main(void) { return run_all(); }' + NL)
    cc = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra",
                         "-Werror", "-o", bf, cf], capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("host build failed:" + NL + cc.stderr)
    out = subprocess.run([bf], capture_output=True, text=True)
    sys.stdout.write(out.stdout)
    return out.returncode != 0


def main():
    if "--mutations" in sys.argv:
        survivors = 0
        for mut in MUTATIONS:
            try:
                bad = run(VECTORS, mutations=[mut])
            except SystemExit as e:
                # A mutant that will not compile is not a caught mutant: it means
                # the mutation string no longer matches the shipped code, and the
                # bench would report a clean run while testing nothing.
                ok(False, "mutant does not compile (%s): %s"
                   % (mut[0][:40], str(e).splitlines()[0] if str(e) else "?"))
                continue
            if bad:
                print("ok    mutant caught: %s" % mut[0][:50])
            else:
                survivors += 1
                print("FAIL  mutant SURVIVED: %s" % mut[0][:50])
        for mut in WAKE_MUTATIONS:
            try:
                bad = run_wake(WAKE_VECTORS, mutations=[mut])
            except SystemExit as e:
                ok(False, "wake mutant does not compile (%s): %s"
                   % (mut[0][:40], str(e).splitlines()[0] if str(e) else "?"))
                continue
            if bad:
                print("ok    wake mutant caught: %s" % mut[0][:50])
            else:
                survivors += 1
                print("FAIL  wake mutant SURVIVED: %s" % mut[0][:50])
        total = len(MUTATIONS) + len(WAKE_MUTATIONS)
        ok(survivors == 0, "%d mutants, %d survived" % (total, survivors))
    else:
        bad = run(VECTORS)
        ok(not bad, "every motion vector came out as expected "
                    "(%d cases at %d mg / %d LSB)" % (len(VECTORS), THRESH_MG, THRESH))
        bad = run_wake(WAKE_VECTORS)
        ok(not bad, "every wake run-of-N vector came out as expected "
                    "(%d cases)" % len(WAKE_VECTORS))

    print("\n%s" % ("ALL PASS" if not FAILS else "%d FAILED" % len(FAILS)))
    for f in FAILS:
        print("  - " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
