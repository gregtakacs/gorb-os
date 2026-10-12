#!/usr/bin/env python3
"""Host bench for the idle dim's decisions in main/power/idle_dim.c.

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
Alongside them, three more that fail the same way — silently, on a real desk, with
nothing in the log to say so: the run-of-N that turns per-sample verdicts into a
wake, the DISPLAY menu's elapsed test and timeout clamp, and the level-commit
decision that says what the task may believe about the panel after ONE brightness
attempt. The last of those is the difference between a dial that wakes a tick late
and a dial that stays at 20% until the next timeout: a refused write that is
believed anyway is never retried, and it stops the 20 Hz dark poll that carries the
motion wake with it. Its vectors pin the predicate; a lint on the task's own text
pins the call site, because a correct predicate the task does not use is the same
dark dial.

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


def lifted_c(mutations=(), funcs=(("idle_motion_decide", "idle_motion_t", "bool"),),
             defines=""):
    """The shipping functions, extracted verbatim, in a compile harness.

    Parameterised by function because the file now ships four decisions worth
    pinning on a host: the per-sample motion verdict, the run-of-N that turns
    verdicts into a wake, the elapsed test behind the DISPLAY menu's timeout,
    and the clamp that keeps a bad persisted value from dimming instantly.
    All are pure, all are invisible until the orb is on a real desk or a real
    49-day-old clock, and each is the only thing standing between a Kconfig
    value and a dial that dims wrong.

    funcs: iterable of (name, first-arg-type, return-type) triples.
    """
    src = open(SRC).read()
    hdr = open(os.path.join(ROOT, "main", "power", "idle_dim.h")).read()
    # The timeout bounds are lifted from the header verbatim, not restated
    # here: a bench copy that drifts from the shipped clamp tests a clamp the
    # device does not run.
    bounds = "\n".join(re.findall(r"^#define IDLE_DIM_TIMEOUT_\w+[^\n]*", hdr, re.M))
    if bounds.count("#define") != 2:
        raise SystemExit("the timeout bounds vanished from %s -- the bench "
                         "cannot pin a clamp it cannot read" % hdr)
    bodies = []
    applied = {old: False for old, _ in mutations}
    for func, argtype, ret in funcs:
        # Lazy [\s\S] to the opening brace so two-line signatures (the elapsed
        # decide wraps its four args) lift too; [\n{] is impossible before the
        # body's brace because this file's style keeps every nested brace
        # indented, so the match can only land on the function's own {.
        m = re.search(r"^" + ret + " " + func + r"\(" + argtype +
                      r"[\s\S]*?\n\{", src, re.M)
        if not m:
            raise SystemExit(func + " not found in %s -- this bench is "
                             "pinning a function that no longer exists" % SRC)
        # Cut at the first brace in column 0 after the opening one. Counting
        # braces instead is wrong here: the prose comments carry braces and
        # parentheses, and a counter cannot tell a comment from code. This
        # file's style indents every nested block, so a column-0 brace is only
        # ever the function's own.
        tail = src[m.end():]
        end = re.search(r"^\}", tail, re.M)
        if not end:
            raise SystemExit(func + " never closes in %s" % SRC)
        body = src[m.start():m.end() + end.end()]
        for old, new2 in mutations:
            if old not in body:
                continue          # the mutation belongs to one of the funcs;
            body = body.replace(old, new2)
            applied[old] = True
        bodies.append(body)
    for old, _ in mutations:
        if not applied[old]:
            raise SystemExit("mutation %r does not apply -- rewrite the bench to "
                             "match the shipped code" % old)
    return """
#include <stdint.h>
#include <stdbool.h>
typedef struct { int32_t base[3]; int32_t worst; bool have_base; } idle_motion_t;
typedef struct { uint8_t run; bool fired; } idle_wake_t;
%s
%s
%s
""" % (defines, bounds, NL.join(bodies))


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
    open(cf, "w").write(lifted_c(mutations,
                                 funcs=(("idle_motion_wake_decide",
                                         "idle_wake_t", "bool"),)) +
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


# ---- the DISPLAY menu's two pure decisions ---------------------------------
#
# idle_dim_elapsed_decide: the gate the timeout picker feeds. Two silent wrong
# answers are pinned here. Ignore the enabled flag and the switch is cosmetic;
# signed elapsed maths and the 49-day clock reads as a negative idle time, so
# a dial that has been on the wall six weeks dims late (or never) with every
# other case still passing. The wrap vectors exist because no bench session
# ever sees them from behaviour alone.
#
# (label, enabled, now_ms, last_ms, timeout_ms, expect)
ELAPSED_VECTORS = [
    ("off_blocks_a_long_idle",  False, 7_200_000, 0,          600_000, False),
    ("on_after_timeout",        True,    600_000, 0,          600_000, True),
    ("on_one_ms_short",         True,    599_999, 0,          600_000, False),
    ("equal_is_dim",            True,        100, 0,              100, True),
    ("zero_timeout_dims",       True,          0, 0,                0, True),
    # now has wrapped past last: elapsed 6912 ms across the 49-day boundary.
    ("wrap_long_idle_dims",     True, 0x0000_0B00, 0xFFFF_F000,   6000, True),
    # Touched 2560 ms ago, near the wrap: must NOT dim, and a subtraction that
    # reads direction the wrong way sees "huge idle" here instead.
    ("wrap_fresh_touch_stays",  True, 0x0000_0200, 0xFFFF_F800,   5000, False),
    # Elapsed 4294967294 ms: unsigned says six weeks idle, signed says -2 and
    # never dims. Only this shape of case separates the two.
    ("wrap_huge_idle_dims",     True, 0xFFFF_FFFF, 0x0000_0001, 600_000, True),
]

ELAPSED_MUTATIONS = [
    # neuter the gate rather than delete it: removing the line leaves `enabled`
    # an unused parameter and -Werror kills the mutant before it can be judged
    # (same trick the wake suite's reset mutant uses).
    ("if (!enabled) return false;", "if (!enabled && false) return false;"),
    (">= timeout_ms", "> timeout_ms"),                          # boundary lost
    ("(uint32_t)(now_ms - last_ms)", "(uint32_t)(last_ms - now_ms)"),  # direction
    # whole statement, both sides signed: an int-vs-uint comparison would die
    # on -Wsign-compare before the vectors can judge it
    ("return (uint32_t)(now_ms - last_ms) >= timeout_ms;",
     "return (int32_t)((int32_t)now_ms - (int32_t)last_ms) >= (int32_t)timeout_ms;"),
]

# idle_dim_clamp_timeout_s: the setter clamps, and settings_load clamps on READ
# too, because the value that reaches the task can arrive from a hand-flashed
# NVS, not from this firmware's picker. An unclamped 0 dims the instant the
# task starts -- the silent wrong answer the MIN bound exists for.
#
# (label, requested, expected)
CLAMP_VECTORS = [
    ("zero_becomes_min",              0,          5),
    ("below_min_becomes_min",         4,          5),
    ("min_stays",                     5,          5),
    ("passthrough",                 600,        600),
    ("max_stays",                 86400,      86400),
    ("above_max_becomes_max",     86401,      86400),
    ("wild_u32_becomes_max",  0xFFFF_FFFF,     86400),
]

CLAMP_MUTATIONS = [
    ("if (s < IDLE_DIM_TIMEOUT_MIN_S) return IDLE_DIM_TIMEOUT_MIN_S;", ""),
    ("if (s < IDLE_DIM_TIMEOUT_MIN_S) return IDLE_DIM_TIMEOUT_MIN_S;",
     "if (s < IDLE_DIM_TIMEOUT_MIN_S) return 0;"),
    ("if (s > IDLE_DIM_TIMEOUT_MAX_S) return IDLE_DIM_TIMEOUT_MAX_S;",
     "if (s > IDLE_DIM_TIMEOUT_MAX_S) return s;"),
]

# ---- the app voice: idle_dim_app_decide (plan §4.2, P3) -----------------------
#
# The third input to the dim decision. The silent wrong answers here are the
# expensive kind: an app's hold that the owner's DISPLAY override cannot beat
# is an app owning the power state (that half is enforced in the SETTERS and
# the base predicate's own enabled gate, pinned by the battery above); an
# expiry test that reads direction through the 49-day wrap is a dial that
# stays lit for six weeks on one badly-timed request. The vectors exist
# because no bench session waits out the wrap, same argument as the elapsed
# battery's own.
#
# (label, enabled, app_hold, now_ms, last_ms, timeout_ms, app_until_ms, expect)

def app_ceiling_ms():
    """The SHIPPED ceiling, read from the stamped sdkconfig.h — not restated:
    a bench copy that drifts prices a rule the device does not enforce. A
    fresh Kconfig option absent here is the workspace .clinerules trap: append
    it to sdkconfig and rebuild before the bench may trust anything."""
    h = os.path.join(ROOT, "build", "config", "sdkconfig.h")
    try:
        txt = open(h).read()
    except OSError:
        raise SystemExit("no %s -- the bench cannot pin a ceiling the build "
                         "has never stamped" % h)
    m = re.search(r"#define CONFIG_ORB_APP_POWER_MAX_S (\d+)", txt)
    if not m:
        raise SystemExit("CONFIG_ORB_APP_POWER_MAX_S absent from %s -- a new "
                         "Kconfig default does not appear in an existing "
                         "sdkconfig by itself: append it, rebuild, rerun" % h)
    return int(m.group(1)) * 1000

CEIL_MS = app_ceiling_ms()

APP_VECTORS = [
    ("owner_off_no_hold",            False, False,  700_000,      0, 600_000,        0, False),
    ("hold_beats_elapsed",            True,  True,  700_000,      0, 600_000,        0, False),
    ("no_request_idle_dims",          True, False,  700_000,      0, 600_000,        0,  True),
    ("no_request_fresh_stays",        True, False,  300_000,      0, 600_000,        0, False),
    ("window_extends_past_elapsed",   True, False,  700_000,      0, 600_000,   900_000, False),
    ("window_expired_dims",           True, False,  700_000,      0, 600_000,   600_000,  True),
    # The wrap pair: `until` BEHIND now across the boundary reads expired —
    # the direction mutant sees it as a far-future window and keeps the dial
    # lit for six weeks, which is exactly the silent wrong answer.
    ("wrap_expired_dims",             True, False, 0x0000_0B00, 0xFFFF_F000, 6_000, 0xFFFF_F500, True),
    ("edge_closed_at_ceiling",        True, False, 2_000_000,      0, 600_000, 2_000_000 + CEIL_MS, True),
    ("just_inside_ceiling_stays",     True, False, 2_000_000,      0, 600_000, 2_000_000 + CEIL_MS - 1, False),
]

APP_MUTATIONS = [
    # The hold becomes advisory: the elapsed clock dims over a live request.
    ("if (app_hold) return false;", "if (app_hold && false) return false;"),
    # The window test reads direction the wrong way (the wrap vector judges).
    ("(uint32_t)(app_until_ms - now_ms)", "(uint32_t)(now_ms - app_until_ms)"),
    # The window never opens. Neutered with `&& false` rather than deleted:
    # deleting it strands app_until_ms/now_ms and -Wextra kills the mutant
    # before the vectors can judge it (the same trick the elapsed battery
    # uses on its own gate). window_extends_past_elapsed judges.
    ("if (app_until_ms != 0 &&", "if (app_until_ms != 0 && false &&"),
    # The boundary included: a window aged to exactly the ceiling stays open.
    ("now_ms) <", "now_ms) <="),
    # The wrapper asks the base and throws the answer away: never dims
    # again. The call stays so last_ms/timeout_ms stay referenced.
    ("return idle_dim_elapsed_decide(now_ms, last_ms, timeout_ms, enabled);",
     "return idle_dim_elapsed_decide(now_ms, last_ms, timeout_ms, enabled) && false;"),
]


# ---- the level-commit decision: what the task may believe after ONE write ----
#
# The dim is a QSPI write to the panel's brightness register, and the wake is the
# same write in the other direction with nobody waiting for its answer. Commit the
# intended state before the write has landed and one refused transaction is a dial
# that stays at 20% forever: the flag says lit, so no later tick ever disagrees
# with it, the write is never retried, and the 20 Hz dark poll that reads the
# accelerometer -- the second wake source -- stops with it. The owner sees "the
# knob does nothing"; the log has one line saying the write failed. Pinned because
# it is a one-line regression with a ten-minute penalty and no repro that does not
# involve getting the bus to refuse on purpose.
#
# (label, want, dimmed, wrote, expected_dimmed_after)
LEVEL_VECTORS = [
    # The bug, both directions: refused, the flag must not move.
    ("wake_refused_stays_dimmed",  False, True,  False, True),
    ("dim_refused_stays_lit",       True, False, False, False),
    # Accepted, it must move: a predicate that never commits is a dial that dims
    # and un-dims forever without ever changing the glass.
    ("wake_accepted_lights",       False, True,  True,  False),
    ("dim_accepted_goes_dark",      True, False, True,  True),
    # Nothing was asked for: a lit dial staying lit must not be read as dimmed, and
    # a dimmed one staying dimmed must not be read as lit.
    ("no_change_while_lit",        False, False, True,  False),
    ("no_change_while_dimmed",      True, True,  True,  True),
    # A refused transition followed by an accepted one -- the retry this exists
    # for. No single call separates a predicate that retries from one that latched
    # the intent and moved on, so this vector carries no `wrote` and runs the pair.
    ("refused_then_accepted",      False, True,  None,  False),
]

LEVEL_MUTATIONS = [
    # The regression itself: believe the intent whatever the panel answered. The
    # tautology keeps `wrote` referenced so -Wextra's unused-parameter rule does not
    # kill the mutant before the vectors can judge it (same trick the clamp and
    # elapsed batteries use).
    ("return wrote ? want : dimmed;", "return (wrote || !wrote) ? want : dimmed;"),
    # A commit that never lets the dial back to the light: the dim lands, the wake
    # is believed by the flag and refused by the glass, and the owner has a dark
    # dial with a clock running behind it.
    ("return wrote ? want : dimmed;", "return (wrote && want) || dimmed;"),
    # Asks the question backwards: refuses what the panel took and takes what it
    # refused.
    ("return wrote ? want : dimmed;", "return wrote ? dimmed : want;"),
    # Gates the commit on the *request* as well as the answer, so a wake the panel
    # accepted is not committed unless the dial also wanted to be lit.
    ("wrote ? want : dimmed",        "wrote && want ? want : dimmed"),
]


def run_level(vectors, mutations=()):
    """Replay every commit vector through the lifted predicate.

    The refused_then_accepted case carries no `wrote` value (None): it is the only
    one that runs the two-tick sequence, because a single call cannot tell a
    predicate that retries from one that latched the intent and moved on.
    """
    d = tempfile.mkdtemp(prefix="orb_level_commit_")
    cf, bf = os.path.join(d, "t.c"), os.path.join(d, "t")
    calls = []
    for vec in vectors:
        label, want, dimmed, wrote, expect = vec
        b = lambda v: "true" if v else "false"
        if wrote is None:
            calls.append(
                "    {" + NL +
                '        bool dimmed = %s;' % b(dimmed) + NL +
                '        dimmed = idle_dim_level_apply_decide(%s, dimmed, false);'
                    % b(want) + NL +
                '        fails += dimmed ? 0 : 1;' + NL +
                '        dimmed = idle_dim_level_apply_decide(%s, dimmed, true);'
                    % b(want) + NL +
                '        fails += (dimmed != %s) ? 1 : 0;' % b(expect) + NL +
                '        if (dimmed != %s && !bad) bad = %d;' % (b(expect),
                                                                  len(calls) + 1) + NL +
                '    }')
            continue
        calls.append(
            "    {" + NL +
            '        bool got = idle_dim_level_apply_decide(%s, %s, %s);'
                % (b(want), b(dimmed), b(wrote)) + NL +
            '        fails += (got != %s) ? 1 : 0;' % b(expect) + NL +
            '        if (got != %s && !bad) bad = %d;' % (b(expect),
                                                          len(calls) + 1) + NL +
            '    }')
    open(cf, "w").write(lifted_c(mutations, funcs=(
            ("idle_dim_level_apply_decide", "bool", "bool"),
        )) +
        '#include <stdio.h>' + NL +
        'static int bad = 0;' + NL +
        'static int run_all(void) {' + NL +
        '    int fails = 0;' + NL +
        chr(10).join(calls) + NL +
        '    if (bad) {' + NL +
        '        puts("the level-commit decision came out wrong: bad vector #");' + NL +
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


# ---- the lint the vectors cannot be mutated past --------------------------------
#
# A pure function pinned by vectors still does not stop the task from going
# `s_dimmed = want;` beside it and ignoring the answer -- the exact one-line change
# that reintroduces a dial that never wakes, and one the vectors above cannot see
# because they never look at the task. So the task's own text is checked: the only
# assignment to s_dimmed inside idle_task may be the one that asks the predicate.
# Reading the shipping file rather than a copy is the point, and the check dies
# loudly if the function or the task is renamed, exactly as lifted_c does.
DIM_ASSIGN_RE  = re.compile(r"\bs_dimmed\s*=(?!=)")
DIM_COMMIT_RE  = re.compile(r"\bs_dimmed\s*=\s*idle_dim_level_apply_decide\s*\("
                            r"\s*want\s*,\s*s_dimmed\s*,\s*wrote\s*\)\s*;")


def lint_task_commits(mutations=()):
    """True when idle_task still commits the dimmed state only via the predicate."""
    text = open(SRC).read()
    m = re.search(r"^static void idle_task\(void \*arg\)", text, re.M)
    if not m:
        raise SystemExit("idle_task not found in %s -- the lint has nothing to "
                         "check, which means it silently stopped working" % SRC)
    tail = text[m.end():]
    end = re.search(r"^\}", tail, re.M)
    if not end:
        raise SystemExit("idle_task never closes in %s" % SRC)
    body = tail[:end.end()]
    for old, new in mutations:
        body = body.replace(old, new)
    assigns, commits = DIM_ASSIGN_RE.findall(body), DIM_COMMIT_RE.findall(body)
    if len(assigns) != 1 or len(commits) != 1:
        print("FAIL  idle_task assigns s_dimmed %d time(s), %d of them through "
              "idle_dim_level_apply_decide: a state the panel has not accepted is a "
              "state that is never retried" % (len(assigns), len(commits)))
        return False
    if "apply_level(" not in body:
        print("FAIL  idle_task no longer calls apply_level -- nothing asks the "
              "panel, so there is no answer for the commit decision to use")
        return False
    return True


# The task's own line, spelled the way it ships: the mutation battery proves the
# lint bites by reintroducing the bug through this file's own text.
LINT_MUTATIONS = [
    ("s_dimmed = idle_dim_level_apply_decide(want, s_dimmed, wrote);",
     "s_dimmed = want;"),
    ("s_dimmed = idle_dim_level_apply_decide(want, s_dimmed, wrote);",
     "s_dimmed = wrote;"),
    ("s_dimmed = idle_dim_level_apply_decide(want, s_dimmed, wrote);",
     "if (wrote) s_dimmed = want;"),
]


def run_cfg(elapsed_vectors, clamp_vectors, mutations=()):
    """Compile both new decisions in one host binary and replay every case.

    Lifted together rather than one at a time: the settings the menu writes are
    the settings the elapsed test reads, and a bench that links them in the same
    translation unit at least compiles them against the same header truth.
    """
    d = tempfile.mkdtemp(prefix="orb_idle_cfg_")
    cf, bf = os.path.join(d, "t.c"), os.path.join(d, "t")
    calls = []
    for vec in elapsed_vectors:
        label, en, now, last, to, expect = vec
        calls.append(
            "    {" + NL +
            '        bool got = idle_dim_elapsed_decide(%uU, %uU, %uU, %s);'
            % (now, last, to, "true" if en else "false") + NL +
            '        fails += (got != %s) ? 1 : 0;' % ("true" if expect else "false") + NL +
            '        if (got != %s && !bad) bad = %d;' % ("true" if expect else "false",
                                                          len(calls) + 1) + NL +
            '    }')
    for vec in clamp_vectors:
        label, req, want = vec
        calls.append(
            "    {" + NL +
            '        uint32_t got = idle_dim_clamp_timeout_s(%uU);' % req + NL +
            '        fails += (got != %uU) ? 1 : 0;' % want + NL +
            '        if (got != %uU && !bad) bad = %d;' % (want, len(calls) + 1) + NL +
            '    }')
    open(cf, "w").write(lifted_c(mutations, funcs=(
            ("idle_dim_elapsed_decide", "uint32_t", "bool"),
            ("idle_dim_clamp_timeout_s", "uint32_t", "uint32_t"),
        )) +
        '#include <stdio.h>' + NL +
        'static int bad = 0;' + NL +
        'static int run_all(void) {' + NL +
        '    int fails = 0;' + NL +
        chr(10).join(calls) + NL +
        '    if (bad) {' + NL +
        '        puts("a DISPLAY-menu decision came out wrong: first bad vector #");' + NL +
        '        printf("%d", bad);' + NL +
        '        puts("(elapsed 1-%d, then clamp)");' % len(elapsed_vectors) + NL +
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


def run_app(vectors, mutations=()):
    """Compile the lifted app-voice gate WITH the shipped base it wraps and
    replay every vector. Lifted together on purpose: the wrapper's whole
    contract is to defer correctly to idle_dim_elapsed_decide, and a binary
    that contained only the wrapper could not see a mutant that stops asking.
    """
    d = tempfile.mkdtemp(prefix="orb_idle_app_")
    cf, bf = os.path.join(d, "t.c"), os.path.join(d, "t")
    calls = []
    for i, vec in enumerate(vectors):
        label, en, hold, now, last, to, until, expect = vec
        calls.append(
            "    {" + NL +
            '        bool got = idle_dim_app_decide(%uU, %uU, %uU, %s, %s, %uU);'
            % (now, last, to, "true" if en else "false",
               "true" if hold else "false", until) + NL +
            '        fails += (got != %s) ? 1 : 0;' % ("true" if expect else "false") + NL +
            '        if (got != %s && !bad) bad = %d;' % ("true" if expect else "false",
                                                          i + 1) + NL +
            '    }')
    open(cf, "w").write(lifted_c(mutations, funcs=(
            # base first in the tuple: the lifted bodies follow the tuple
            # order, and the wrapper calls the base.
            ("idle_dim_elapsed_decide", "uint32_t", "bool"),
            ("idle_dim_app_decide", "uint32_t", "bool"),
        ),
        defines="#define CONFIG_ORB_APP_POWER_MAX_S %d" % (CEIL_MS // 1000)) +
        '#include <stdio.h>' + NL +
        'static int bad = 0;' + NL +
        'static int run_all(void) {' + NL +
        '    int fails = 0;' + NL +
        chr(10).join(calls) + NL +
        '    if (bad) {' + NL +
        '        puts("an app-voice decision came out wrong: first bad vector #");' + NL +
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
        for mut in ELAPSED_MUTATIONS + CLAMP_MUTATIONS:
            try:
                bad = run_cfg(ELAPSED_VECTORS, CLAMP_VECTORS, mutations=[mut])
            except SystemExit as e:
                ok(False, "config mutant does not compile (%s): %s"
                   % (mut[0][:40], str(e).splitlines()[0] if str(e) else "?"))
                continue
            if bad:
                print("ok    config mutant caught: %s" % mut[0][:50])
            else:
                survivors += 1
                print("FAIL  config mutant SURVIVED: %s" % mut[0][:50])
        for mut in APP_MUTATIONS:
            try:
                bad = run_app(APP_VECTORS, mutations=[mut])
            except SystemExit as e:
                ok(False, "app mutant does not compile (%s): %s"
                   % (mut[0][:40], str(e).splitlines()[0] if str(e) else "?"))
                continue
            if bad:
                print("ok    app mutant caught: %s" % mut[0][:50])
            else:
                survivors += 1
                print("FAIL  app mutant SURVIVED: %s" % mut[0][:50])
        for mut in LEVEL_MUTATIONS:
            try:
                bad = run_level(LEVEL_VECTORS, mutations=[mut])
            except SystemExit as e:
                ok(False, "commit mutant does not compile (%s): %s"
                   % (mut[0][:40], str(e).splitlines()[0] if str(e) else "?"))
                continue
            if bad:
                print("ok    commit mutant caught: %s" % mut[0][:50])
            else:
                survivors += 1
                print("FAIL  commit mutant SURVIVED: %s" % mut[0][:50])
        for mut in LINT_MUTATIONS:
            # The lint's mutants are the regressions the vectors cannot reach: a
            # correct predicate that the task stops asking.
            if not lint_task_commits(mutations=[mut]):
                print("ok    lint mutant caught: %s" % mut[0][:50])
            else:
                survivors += 1
                print("FAIL  lint mutant SURVIVED: %s" % mut[0][:50])
        total = (len(MUTATIONS) + len(WAKE_MUTATIONS)
                 + len(ELAPSED_MUTATIONS) + len(CLAMP_MUTATIONS)
                 + len(LEVEL_MUTATIONS) + len(LINT_MUTATIONS)
                 + len(APP_MUTATIONS))
        ok(survivors == 0, "%d mutants, %d survived" % (total, survivors))
    else:
        bad = run(VECTORS)
        ok(not bad, "every motion vector came out as expected "
                    "(%d cases at %d mg / %d LSB)" % (len(VECTORS), THRESH_MG, THRESH))
        bad = run_wake(WAKE_VECTORS)
        ok(not bad, "every wake run-of-N vector came out as expected "
                    "(%d cases)" % len(WAKE_VECTORS))
        bad = run_cfg(ELAPSED_VECTORS, CLAMP_VECTORS)
        ok(not bad, "every DISPLAY-menu decision came out as expected "
                    "(%d elapsed + %d clamp cases)"
                    % (len(ELAPSED_VECTORS), len(CLAMP_VECTORS)))
        bad = run_app(APP_VECTORS)
        ok(not bad, "every app-voice vector came out as expected "
                    "(%d cases, ceiling %d s)"
                    % (len(APP_VECTORS), CEIL_MS // 1000))
        bad = run_level(LEVEL_VECTORS)
        ok(not bad, "every level-commit vector came out as expected "
                    "(%d cases: a refused write never moves the flag)"
                    % len(LEVEL_VECTORS))
        ok(lint_task_commits(),
           "idle_task commits the dimmed state only through "
           "idle_dim_level_apply_decide, and only after apply_level answers")

    print("\n%s" % ("ALL PASS" if not FAILS else "%d FAILED" % len(FAILS)))
    for f in FAILS:
        print("  - " + f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
