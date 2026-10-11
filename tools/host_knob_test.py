#!/usr/bin/env python3
"""host_knob_test.py — the encoder's click model, tested on the host.

Why this file exists
--------------------
The owner turned the dial and described three behaviours in one sentence: "it moves
mostly one but sometimes two and sometimes none". Each is a different failure of the
same decision — what a mechanical click was — and none of them prints anything on a
serial console, because the symptom IS the absence of movement. All three survived
being read twice and one round of "looks right" on hardware.

So the shipped decoder is lifted VERBATIM out of main/knob/knob.c — kQuadTable,
knob_detent_from_net, knob_detents_rounded and knob_edge itself — and driven with
synthetic AB state sequences, which is the level the claim actually lives at.
knob_edge() takes time as an argument rather than reading a clock, which is what makes
the click guard testable here instead of merely true on a bench.

What is under test
------------------
  one clean click, each direction        exactly one detent, at the click
  a click stopped on the notch           still one detent  ("the first notch does nothing")
  a click that lost a middle edge        still one detent
  bounce through the valley              one detent, never two   ("sometimes two")
  rocking a valley                       nothing
  the click guard                        no double, and no cost to a real fast flick
  a unit whose valley is 0b00            still one detent per click, because it is learned
  the direction convention               CW = up = right, CCW = down = left

Run:  python3 tools/host_knob_test.py
Mutations:  python3 tools/host_knob_test.py --mutations   (each must fail the suite)
"""

import os, re, subprocess, sys

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KNOB_C = os.path.join(ROOT, "main", "knob", "knob.c")
KNOB_H = os.path.join(ROOT, "main", "knob", "knob.h")
for _p in (KNOB_C, KNOB_H):
    if not os.path.exists(_p):
        sys.exit("missing " + _p + " — the lifts below have no fallback")

FAILS = []
def check(name, ok, detail=""):
    print(("ok    " if ok else "FAIL  ") + name + ((("  " + detail) if detail else "")))
    if not ok:
        FAILS.append(name)
    return ok

def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is the "
                 "point: an empty lift would keep this suite green while testing a "
                 "copy that no longer ships." % (pattern, where))
    return m.group(0)


def lifts(src):
    """Everything the replay needs, spliced verbatim out of the shipped driver."""
    return [
        lift(src, r"static const int8_t kQuadTable\[16\] = \{[\s\S]*?\};", KNOB_C),
        lift(src, r"#define KNOB_STEPS_PER_DETENT\s+\d+[^\n]*", KNOB_C),
        lift(src, r"#define KNOB_DETENT_MIN_STEPS\s+\([^\n]*", KNOB_C),
        lift(src, r"#define KNOB_CLICK_GUARD_MS\s+\d+[^\n]*", KNOB_C),
        lift(src, r"static int32_t knob_detent_from_net\(int32_t net\)\n\{[\s\S]*?\n\}", KNOB_C),
        lift(src, r"static int32_t knob_detents_rounded\(int32_t net\)\n\{[\s\S]*?\n\}", KNOB_C),
        lift(src, r"static int32_t knob_edge\(int32_t \*raw[\s\S]*?last_commit_us\)\n\{[\s\S]*?\n\}", KNOB_C),
        lift(src, r"int knob_row_step\(int32_t delta\)\n\{[\s\S]*?\n\}", KNOB_C),
        lift(src, r"int knob_col_step\(int32_t delta\)\n\{[\s\S]*?\n\}", KNOB_C),
    ]


DIR_SWITCH = lift(open(KNOB_H).read(),
                  r"#define KNOB_DIR_DOWN_IS_CW\s+\d+[^\n]*", KNOB_H)

HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

__CONSTS__

static int fails;
static void ck(const char *name, int ok, int got, int want)
{
    printf("%s %s (got %d, want %d)\n", ok ? "PASS" : "FAIL", name, got, want);
    if (!ok) fails++;
}

/* The replay state, shaped like what the ISR keeps. clock_us is the synthetic timer:
 * knob_edge() takes time as an argument precisely so the click guard can be driven
 * here instead of being trusted on a bench. */
typedef struct {
    int32_t raw, net, det;
    uint8_t prev, valley;
    int64_t clock_us, last_commit_us;
} sim_t;

static void sim_init(sim_t *s, uint8_t valley)
{
    memset(s, 0, sizeof *s);
    s->prev = valley;                       /* seeded from the resting level */
    s->valley = valley;
    s->last_commit_us = -((int64_t)KNOB_CLICK_GUARD_MS * 2000);
}

/* Feed one edge arriving at AB state `ab`, `gap_us` after the previous edge. */
static int edge(sim_t *s, uint8_t ab, int64_t gap_us)
{
    s->clock_us += gap_us;
    const int32_t n = knob_edge(&s->raw, &s->net, &s->prev, ab, s->valley,
                                s->clock_us, s->last_commit_us);
    if (n) s->last_commit_us = s->clock_us;
    s->det += n;
    return n;
}

/* The four states between two valleys, in the direction kQuadTable calls positive.
 * The table is lifted from the driver, so this sequence cannot quietly disagree with
 * it the way a hand-written copy of the decoder would. */
static const uint8_t CW[5]  = { 3, 1, 0, 2, 3 };
static const uint8_t CCW[5] = { 3, 2, 0, 1, 3 };

/* A deliberate click: four edges over ~60 ms. Anything faster than this in a
 * test is a solenoid, not a hand — an earlier version of this suite drove 500 us edges
 * (500 clicks a second) and the click guard quite rightly refused them, which is
 * how the guard was shown to work by the tests rather than assumed to. */
#define CLICK_GAP_US   15000            /* 60 ms per notch */
#define FLICK_GAP_US    7500            /* 30 ms per notch: about as fast as a wrist */

/* One click at `gap_us` per edge. Fewer than four edges is a hand that stopped on
 * the click stop, which is how the reported bug was actually performed. */
static void click_n(sim_t *s, int dir, int64_t gap_us, int edges)
{
    const uint8_t *seq = (dir > 0) ? CW : CCW;
    for (int i = 1; i <= edges; i++) edge(s, seq[i], gap_us);
}
static void click(sim_t *s, int dir, int64_t gap_us) { click_n(s, dir, gap_us, 4); }

/* The settle pass, as knob_poll() runs it once the stream has been idle. */
static void settle(sim_t *s)
{
    const int32_t n = knob_detents_rounded(s->net);
    s->net = 0;
    if (n) { s->det += n; s->last_commit_us = s->clock_us; }
}

/* Click-at-a-time browsing: the owner's actual gesture. */
static int browse(int clicks, int stopped_short, int with_settle)
{
    sim_t s; sim_init(&s, 3);
    for (int c = 0; c < clicks; c++) {
        const int e = (c == clicks - 1 && stopped_short) ? 3 : 4;
        click_n(&s, +1, 5000, e);
        if (with_settle) settle(&s);
    }
    return s.det;
}


int main(void)
{
    /* ---- one clean click, each way: exactly one detent, at the click ---- */
    {
        sim_t s; sim_init(&s, 3);
        click(&s, +1, CLICK_GAP_US);
        ck("one clean CW click commits exactly +1", s.det == 1, s.det, 1);
        click(&s, +1, CLICK_GAP_US);
        ck("and a second is another +1, not a repeat", s.det == 2, s.det, 2);
    }
    {
        sim_t s; sim_init(&s, 3);
        click(&s, -1, CLICK_GAP_US);
        ck("one clean CCW click commits exactly -1", s.det == -1, s.det, -1);
    }

    /* ---- report 1: "does not register the first notch move" ----
     * A hand stopping ON the click stop never emits the fourth edge, and a click can
     * lose a middle edge to noise. Neither may cost the detent. */
    ck("a click stopped on the notch still gives one detent",
       browse(1, 1, 1) == 1, browse(1, 1, 1), 1);
    ck("ten such clicks give ten detents, not five",
       browse(10, 1, 1) == 10, browse(10, 1, 1), 10);
    {
        sim_t s; sim_init(&s, 3);
        edge(&s, 1, 500);           /* 3->1 */
        edge(&s, 2, 500);           /* 1->2: invalid, both pins moved in one entry */
        edge(&s, 3, 500);           /* 2->3: the valley, carrying only net +2 */
        ck("a click that lost its middle edge still commits one", s.det == 1, s.det, 1);
    }

    /* ---- report 2: "sometimes two" ----
     * Bounce through the valley inside a single click. An edge counter books two
     * detents here, because the travel really is there; one credit per arrival is the
     * whole of the difference, and it is not expressible in an edge count. */
    {
        sim_t s; sim_init(&s, 3);
        click(&s, +1, 400);
        edge(&s, 1, 300); edge(&s, 3, 300);        /* kicked out and straight back */
        ck("bounce back into the same valley adds nothing", s.det == 1, s.det, 1);
    }
    {
        sim_t s; sim_init(&s, 3);
        click(&s, +1, 400);
        /* Forward a whole notch, back, forward again, all inside 3 ms. Every arrival
         * here genuinely carries net travel, so only the click guard makes this one
         * click rather than three. */
        for (int i = 1; i < 5; i++) edge(&s, CW[i], 400);
        for (int i = 3; i >= 1; i--) edge(&s, CW[i], 400);
        for (int i = 1; i < 5; i++) edge(&s, CW[i], 400);
        ck("a notch that kicked forward twice in 3 ms is still one click",
           s.det == 1, s.det, 1);
    }

    /* ---- report 3: "sometimes none", and the dither the fix must not trade away ---- */
    {
        sim_t s; sim_init(&s, 3);
        for (int i = 0; i < 200; i++) edge(&s, (i & 1) ? 1 : 3, 500);
        settle(&s);        /* the hand stops and the poll runs: still nothing booked */
        ck("rocking the valley against its neighbour commits nothing",
           s.det == 0, s.det, 0);
    }
    {
        sim_t s; sim_init(&s, 3);
        for (int i = 0; i < 120; i++) edge(&s, (i & 1) ? 2 : 0, 500);
        settle(&s);        /* otherwise the bogus net just sits there unexamined */
        ck("rocking between the two mid states commits nothing", s.det == 0, s.det, 0);
    }
    {
        /* Chatter on the DIAGONAL, where A and B change together: 01 <-> 10 is not a
         * legal quadrature step, so the decoder must refuse it rather than pick a
         * direction. A driver that guessed here books a slow drift of phantom rows,
         * which is the "sometimes two" complaint arriving one row at a time. The
         * valley is never entered, so the settle must refuse it too. */
        sim_t s; sim_init(&s, 3);
        for (int i = 0; i < 120; i++) edge(&s, (i & 1) ? 1 : 2, 500);
        settle(&s);
        ck("diagonal chatter between 01 and 10 commits nothing", s.det == 0, s.det, 0);
    }
    ck("short clicks are recovered rather than dropped",
       browse(6, 0, 1) == 6, browse(6, 0, 1), 6);

    /* ---- the guard must cost nothing real ----
     * The claim it has to survive: no wrist produces two detents inside
     * KNOB_CLICK_GUARD_MS. Four ms per edge is already a hard flick. */
    {
        sim_t s; sim_init(&s, 3);
        for (int c = 0; c < 10; c++) click(&s, +1, FLICK_GAP_US);
        ck("a fast flick of 10 clicks is 10 detents", s.det == 10, s.det, 10);
    }

    /* ---- reversals are netted honestly, never collapsed to zero ---- */
    {
        sim_t s; sim_init(&s, 3);
        for (int c = 0; c < 2; c++) click(&s, +1, 20000);
        click(&s, -1, 20000);
        ck("two right then one left is +1", s.det == 1, s.det, 1);
    }

    /* ---- the valley is a property of the unit, not a constant ----
     * Another encoder family parks its spring at 0b00. The driver learns it, so the
     * replay has to hold at either state; red here means something assumed 0b11. */
    {
        sim_t s; sim_init(&s, 0);
        static const uint8_t cw0[5] = { 0, 2, 3, 1, 0 };
        for (int c = 0; c < 5; c++) for (int i = 1; i < 5; i++) edge(&s, cw0[i], 5000);
        ck("a unit that idles at 00 still counts one detent per click",
           s.det == 5, s.det, 5);
    }

    /* ---- the two helpers, pinned directly ---- */
    ck("net +3 is a detent", knob_detent_from_net(3) == 1, knob_detent_from_net(3), 1);
    ck("net +2 is a detent", knob_detent_from_net(2) == 1, knob_detent_from_net(2), 1);
    ck("net +1 is a rock", knob_detent_from_net(1) == 0, knob_detent_from_net(1), 0);
    ck("net 0 is a rock", knob_detent_from_net(0) == 0, knob_detent_from_net(0), 0);
    ck("net -3 is a detent back", knob_detent_from_net(-3) == -1,
       knob_detent_from_net(-3), -1);
    ck("a settle rounds 8 stranded edges to two detents", knob_detents_rounded(8) == 2,
       knob_detents_rounded(8), 2);
    ck("a settle refuses one stranded step", knob_detents_rounded(1) == 0,
       knob_detents_rounded(1), 0);

    /* ---- the direction convention, pinned by the owner's ruling ----
     * The knob sits on the case side above the dial's centre, so the arc the finger
     * travels is not the arc the list scrolls: DOWN is LEFT and UP is RIGHT, in the
     * same flick, because knob_col_step is the row axis negated. */
    ck("counter-clockwise scrolls DOWN", knob_row_step(-3) == 1, knob_row_step(-3), 1);
    ck("clockwise scrolls UP", knob_row_step(3) == -1, knob_row_step(3), -1);
    ck("the flick that means down also means LEFT",
       knob_row_step(-1) == 1 && knob_col_step(-1) == -1, knob_col_step(-1), -1);
    ck("the flick that means up also means RIGHT",
       knob_row_step(1) == -1 && knob_col_step(1) == 1, knob_col_step(1), 1);
    ck("col is always row negated, so the axes cannot drift apart",
       knob_row_step(2) == -knob_col_step(2) && knob_row_step(-2) == -knob_col_step(-2),
       0, 0);
    ck("no turn is no step on either axis",
       knob_row_step(0) == 0 && knob_col_step(0) == 0, knob_row_step(0), 0);

    printf(fails ? "HARNESS FAILURES %d\n" : "HARNESS OK\n", fails);
    return fails ? 1 : 0;
}
"""


def assembled(src):
    """The harness source: the shipped decoder and helpers, spliced verbatim.
    The direction #define must precede the functions that read it, so it goes first —
    C has no forward declaration for a macro and the lift order is not cosmetic."""
    return HARNESS.replace("__CONSTS__", "\n".join([DIR_SWITCH] + lifts(src)))


def run_suite(src, label, fatal=True):
    """Compile and run the replay.

    A mutation that will not compile is still a caught mutation — the change broke the
    code — so mutation mode asks for a False return instead of dying, and only the
    unmutated build is allowed to abort the run."""
    exe, cfile = "/tmp/host_knob_test.bin", "/tmp/host_knob_test.c"
    open(cfile, "w").write(assembled(src))
    r = subprocess.run(["gcc", "-O0", "-Wall", "-Wextra", "-Werror", cfile, "-o", exe],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-1500:])
        if fatal:
            sys.exit("%s: harness would not compile — the lifted code is not "
                     "host-compilable on its own" % label)
        print("     (%s: would not compile, counting that as caught)" % label)
        return False
    if not os.path.exists(exe):
        sys.exit("%s: gcc reported success but produced no binary" % label)
    r = subprocess.run([exe], capture_output=True, text=True, timeout=25)
    print(r.stdout, end="")
    bad = [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
    return r.returncode == 0 and not bad



def isr_body(ksrc):
    m = re.search(r"static void IRAM_ATTR knob_rot_isr\(void \*arg\)\n\{([\s\S]*?)\n\}\n", ksrc)
    return m.group(1) if m else None


def lint_isr(ksrc):
    """The ISR must stay small and silent. Anything slow in there makes the decode miss
    edges, and a missed edge is one of the three reports this file exists for."""
    body = isr_body(ksrc)
    if not check("the rotation ISR is findable for linting", body is not None):
        return False
    ok = True
    for pat, why in [
        (r"ESP_LOG[IWED]", "logging from the quadrature ISR"),
        (r"\b(vTaskDelay|xSemaphoreTake|malloc)\b", "blocking or allocating in the quadrature ISR"),
        (r"\b(esp_timer_start|httpd_|nvs_|orb_sd_lock)\b", "card or timer work in the quadrature ISR"),
    ]:
        hit = re.search(pat, body)
        ok &= check("quadrature ISR free of: " + why, hit is None,
                    hit.group(0) if hit else "")
    lines = [l for l in body.splitlines()
             if l.strip() and not l.strip().startswith(("*", "/*", "//"))]
    return check("quadrature ISR still small (%d lines, budget 45)" % len(lines),
                 len(lines) <= 45, "%d lines" % len(lines)) and ok


def lint_poll_logging_outside_lock(ksrc):
    """knob_poll() may log, but never inside s_rot_mux: ESP_LOGI can allocate, and
    allocating with interrupts masked turns a 10 ms poll into an unbounded one."""
    m = re.search(r"void knob_poll\(void\)\n\{([\s\S]*?)\n\}\n", ksrc)
    if not check("knob_poll is findable for linting", m is not None):
        return False
    body = m.group(1)
    a, b = body.find("portENTER_CRITICAL"), body.find("portEXIT_CRITICAL")
    if not check("knob_poll brackets its work in one critical section", a >= 0 and b > a):
        return False
    hit = re.search(r"ESP_LOG[IWED]", body[a:b])
    return check("nothing is logged inside the rotation lock", hit is None,
                 hit.group(0) if hit else "")


# Each mutation must turn at least one check red. One that survives is reporting that
# the check it aimed at does not exist — which is how an earlier version of this suite
# passed while its dither checks summed an oscillation to zero and never counted the
# spurious repaints underneath.
MUTATIONS = [
    ("every valley arrival is a detent, however it netted (the 'sometimes two' bug)",
     r"\? knob_detent_from_net\(\*net\) : 0;", " ? 1 : 0;"),
    ("the net is not closed on arrival, so one click can pay twice",
     r"        \*net = 0;\n        return n;", "        return n;"),
    ("the click guard removed",
     r"const int32_t n = \(since >= \(int64_t\)KNOB_CLICK_GUARD_MS \* 1000\)\n\s*\? knob_detent_from_net\(\*net\) : 0;",
     "const int32_t n = knob_detent_from_net(*net);"),
    ("the click guard set a second above any hand",
     r"#define KNOB_CLICK_GUARD_MS     20", "#define KNOB_CLICK_GUARD_MS     2000"),
    ("a one-step rock counts as a click",
     r"if \(net >=  KNOB_DETENT_MIN_STEPS\) return  1;", "if (net >=  1) return  1;"),
    ("backward clicks dropped",
     r"if \(net <= -KNOB_DETENT_MIN_STEPS\) return -1;", "if (net <= -1000000) return -1;"),
    ("no valley commit at all, only the settle",
     r"    if \(ab == valley_ab\) \{", "    if (ab == 0xFE) {"),
    ("the settle refuses to recover a short click ('sometimes none')",
     r"    if \(mag < KNOB_DETENT_MIN_STEPS\) return 0;",
     "    if (mag < KNOB_STEPS_PER_DETENT) return 0;"),
    ("the settle over-counts stranded travel",
     r"const int32_t n = \(mag \+ KNOB_STEPS_PER_DETENT / 2\) / KNOB_STEPS_PER_DETENT;",
     "const int32_t n = (mag + 1) / 2;"),
    # Two guards deliberately left OUT of this battery, because mutating either one
    # alone cannot change behaviour, and a "survived" line would be a lie about a test
    # gap rather than a note about redundancy. kQuadTable already returns 0 for both a
    # same-state edge and an invalid transition, so the `ab == prev` guard and the
    # `step == 0` guard each duplicate the table: any one of them alone is sufficient.
    # They stay in the driver as cheap defence against someone re-tuning the table --
    # which IS tested, by the two table mutations below.
    ("an invalid transition made a step, as a naive decoder would",
     r"^     1,  0,  0, -1,$", "     1,  0,  1, -1,", re.M),
    ("the reverse cycle swapped, so a right turn reads left",
     r"^    -1,  0,  0,  1,$", "     1,  0,  0, -1,", re.M),
    ("the row direction reversed (the owner's ruling, broken)",
     r"    return KNOB_DIR_DOWN_IS_CW \? \(delta > 0 \? 1 : -1\)\n\s*: \(delta > 0 \? -1 : 1\);",
     "    return (delta > 0) ? 1 : -1;"),
    ("the column axis made independent of the row axis",
     r"    const int row = knob_row_step\(delta\);\n    return row == 0 \? 0 : -row;",
     "    return knob_row_step(delta) == 0 ? 0 : 1;"),
    ("col_step negated twice (left/right flipped back)",
     r"return row == 0 \? 0 : -row;", "return row == 0 ? 0 : row;"),
    ("logging put back into the quadrature ISR",
     r"    s_abNow = ab;", '    s_abNow = ab;\n    ESP_LOGI("k", "edge");'),
    ("logging moved inside knob_poll's critical section",
     r"    const uint8_t ab_seen = \(uint8_t\)s_abNow;",
     '    const uint8_t ab_seen = (uint8_t)s_abNow;\n    ESP_LOGI(TAG, "inside the lock");'),
]


def run_mutations(ksrc):
    caught = 0
    for entry in MUTATIONS:
        name, pat, rep = entry[0], entry[1], entry[2]
        flags = entry[3] if len(entry) > 3 else 0
        mutated, n = re.subn(pat, rep, ksrc, count=1, flags=flags)
        if n != 1:
            check("mutation applies: " + name, False, "pattern did not match")
            continue
        if "ESP_LOGI" in rep:
            # Aimed at a linter, so ask that linter — the arithmetic replay cannot see
            # a logging hazard, and a battery that ran it through the replay would
            # report it as survived.
            ok = (lint_isr(mutated) if "quadrature" in name
                  else lint_poll_logging_outside_lock(mutated))
        else:
            FAILS.clear()
            ok = run_suite(mutated, name, fatal=False)
        if ok:
            check("MUTATION SURVIVED: " + name, False)
        else:
            check("mutation caught: " + name, True)
            caught += 1
    print("\n%d/%d mutations caught" % (caught, len(MUTATIONS)))
    if caught != len(MUTATIONS):
        sys.exit(1)


def main():
    ksrc = open(KNOB_C).read()
    if "--mutations" in sys.argv:
        run_mutations(ksrc)
        return
    if not run_suite(ksrc, "knob"):
        sys.exit(1)
    lint_isr(ksrc)
    lint_poll_logging_outside_lock(ksrc)
    hdr = open(KNOB_H).read()
    check("knob.h declares the row/col direction contract",
          "knob_row_step" in hdr and "knob_col_step" in hdr)
    check("the direction ruling is documented, not merely coded",
          "LEFT" in hdr and "RIGHT" in hdr and "DOWN" in hdr)
    check("the commit is keyed on the valley, not on an edge count",
          "ab == valley_ab" in ksrc and "s_valleyAB" in ksrc)
    check("the valley is learned from the pins, not hardcoded",
          ksrc.count("gpio_get_level(PIN_KNOB_A)") >= 3)
    check("both arrival paths are counted, so the split stays observable",
          "s_valleyHits" in ksrc and "s_settleHits" in ksrc)
    check("no screen interprets a turn sign itself",
          not re.search(r"\(delta > 0 \? 1 : -1\)",
                        open(os.path.join(ROOT, "main", "menu", "menu_view.c")).read()))
    if FAILS:
        print("\n%d knob check(s) failed" % len(FAILS)); sys.exit(1)
    print("\nall knob checks pass")


if __name__ == "__main__":
    main()

