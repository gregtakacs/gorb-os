#!/usr/bin/env python3
"""host_pomodoro_test.py — the pomodoro phase machine, pure, on host gcc (§64.3; design §2, §3,
§5.2, §6, §7).

apps/pomodoro/pomodoro.c is the app's whole brain: the cycle of N focuses, the knob's three moves
over it, the wall-clock pass that ends a phase, the four hidden settings an in-place restart reads
back, and what the OS ring should be. None of it reaches a dial until pomodoro-2 wires it, and
almost all of it can be wrong with a clean build: a turn that can reach 0 reads as a finished
phase, a service pass that fires twice chimes a boundary twice, a restore that trusts a stored
second charges the owner for time they did not work, an alpha in the wrong byte is an invisible
arc. So the shipped file is compiled VERBATIM beside a harness and pinned on the edges the design
names. The radar / orb_wake pattern: gcc -std=c11 -Wall -Wextra -Werror, and under --mutations
every mutant must make a check fail — an escaped mutant is a non-zero exit (ring amendment 3).

Run:  python3 tools/host_pomodoro_test.py              (rc 0 = pass)
      python3 tools/host_pomodoro_test.py --mutations  (proves the checks bite; rc 0 = all bite)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP = os.path.join(ROOT, "apps", "pomodoro")
MIN_CHECKS = 55          # the harness's own count: a check that vanished is not a pass

# design §3.2's surface, which this step writes word for word.
FUNCS = ["pm_cycle_len", "pm_phase_at", "pm_focus_at", "pm_next_k", "pm_len_s", "pm_press", "pm_turn",
         "pm_at_start", "pm_pos_step", "pm_land", "pm_service", "pm_restore", "pm_store", "pm_changed",
         "pm_ring_alpha", "pm_ring_for", "pm_fraction", "pm_ring_effect", "pm_ring_phase",
         "pm_state_text", "pm_phase_text", "pm_count_text", "pm_raw_needs_clock"]
TYPES = ["pm_cfg_t", "pm_t", "pm_raw_t", "pm_ring_t"]
CONSTS = ["PM_STOP", "PM_RUN", "PM_PAUSE", "PM_WAIT", "PM_WAITED", "PM_FOCUS", "PM_SHORT", "PM_LONG",
          "PM_EV_NONE", "PM_EV_END", "PM_EV_START", "PM_EV_LIMIT", "PM_MIN_S", "PM_ARC_A", "PM_TRACK_A",
          "PM_BREATHE_MS", "PM_FLASH_MS", "PM_STEP_PCT", "PM_PACK_MUL"]


def shape(fail, h_src, c_src):
    """What the split is for, read off the text: no SDK, no math, the whole surface present."""
    inc = lambda s: re.findall(r'^\s*#\s*include\s*[<"]([^">]+)[">]', s, re.M)
    code = re.sub(r"/\*.*?\*/", " ", h_src + c_src, flags=re.S)      # comments may name the SDK, code may not

    def some(tag, xs, pat, src):
        gone = [x for x in xs if not re.search(pat % x, src, re.M)]
        fail("%s%s" % (tag, ": missing " + ",".join(gone) if gone else ""), not gone)
    fail("the core imports only its own header: %s" % inc(c_src), inc(c_src) == ["pomodoro.h"])
    fail("the header imports only stddef and stdint: %s" % inc(h_src), sorted(inc(h_src)) == ["stddef.h", "stdint.h"])
    fail("no <math.h> and no floating point: the fraction, the clamp and the ladder are integer",
         "<math.h>" not in code and not re.search(r"\b(double|float)\b", code))
    fail("no SDK header and no orb_* call in the code",
         not re.search(r'#include\s*[<"]orb', code) and not re.search(r"\borb_", code))
    some("§3.2's 22 functions are declared", FUNCS, r"^\s*(?:int|void)\s+%s\s*\(", h_src)
    some("and defined in the shipped source", FUNCS, r"^(?:int|void)\s+%s\s*\(", c_src)
    some("the four records are the header's", TYPES, r"}\s*%s\s*;", h_src)
    some("§3.2's constants are all there", CONSTS, r"#define\s+%s\b", h_src)

    def num(k):
        m = re.search(r"#define\s+%s\s+(0[xX][0-9a-fA-F]+|\d+)" % k, h_src)
        return int(m.group(1), 0) if m else None
    fail("the numbers §4 and §7 fix: 60, 0xE6, 0x33, 4000, 2000, 4, 8",
         [num(k) for k in ("PM_MIN_S", "PM_ARC_A", "PM_TRACK_A", "PM_BREATHE_MS", "PM_FLASH_MS",
                           "PM_STEP_PCT", "PM_PACK_MUL")] == [60, 0xE6, 0x33, 4000, 2000, 4, 8])
    fail("pm_ring_t is orb_ring_t's five values in that order, and says so",
         "offsetof(pm_ring_t, effect)" in h_src and "offsetof(pm_ring_t, track)" in h_src)
    # §6.3's skip is the one rule the ladder's answers cannot tell apart once clamped (both
    # read -1 at k=0), so the guard's position before the clamp is pinned on the text.
    fail("the ladder's -1 skip tests at_start before clamping (§6.3)",
         bool(re.search(r"pos == 0 && at_start && p < 0", c_src)))


# Each is a behavioural edit to one line of the shipped source. An escaped mutant means the check
# that names that rule is not watching it.
MUTATIONS = [
    ("raw->st_k % PM_PACK_MUL == PM_RUN", "raw->st_k % PM_PACK_MUL == PM_PAUSE"),
    ("    return focuses < PM_N_MIN ? PM_N_MIN : focuses > PM_N_MAX ? PM_N_MAX : focuses;", "    return focuses;"),  # N indexed off the cycle
    ("    if (k == last) return PM_LONG;", "    if (k == last - 1) return PM_LONG;"),                              # the long break a slot early
    ("    return (k & 1) ? PM_SHORT : PM_FOCUS;", "    return (k & 1) ? PM_FOCUS : PM_SHORT;"),
    ("    return pm_phase_at(k, focuses) == PM_FOCUS ? k / 2 + 1 : 0;", "    return pm_phase_at(k, focuses) == PM_FOCUS ? k / 2 : 0;"),  # the count off by one
    ("    return k + 1 >= n ? 0 : k + 1;", "    return k + 1 > n ? 0 : k + 1;"),                                    # the cycle wraps a slot late
    ("    if (v < PM_MIN_S) v = PM_MIN_S;", "    if (v < PM_MIN_S) v = 0;"),                                        # a 0-second phase
    ("    if (c->cap_s[phase] >= PM_MIN_S && v > c->cap_s[phase]) v = c->cap_s[phase];", ""),                       # a setting past its cap
    ("        s->st = PM_PAUSE;\n        s->end_s = 0;", "        s->st = PM_PAUSE;"),                              # a pause keeps its end
    ("        if (s->len < PM_MIN_S) s->len = pm_len_s(c, pm_phase_at(s->k, c->focuses));", "        s->len = pm_len_s(c, PM_FOCUS);"),  # a press over the turned length
    ("        s->k = (int16_t)pm_next_k(s->k, c->focuses);\n        s->len", "        s->len"),                     # the press never moves on
    ("        s->len = pm_len_s(c, pm_phase_at(s->k, c->focuses));\n        s->rem = s->len;\n        s->st = PM_RUN;", "        s->len = pm_len_s(c, PM_FOCUS);\n        s->rem = s->len;\n        s->st = PM_RUN;"),  # the next phase at the focus's length
    ("    want = s->rem + delta * 60;", "    want = s->rem + delta * 30;"),                                         # a click is not a minute
    ("    if (want > cap) { want = cap; lim = 1; }", "    if (want > cap) { want = cap; }"),                        # the stop never reported
    ("    if (want > cap) { want = cap; lim = 1; }", "    if (want > cap + 60) { want = cap; lim = 1; }"),          # the ceiling leaks a click
    ("    if (want < PM_MIN_S) { want = PM_MIN_S; lim = 1; }", "    if (want < PM_MIN_S) { want = 0; lim = 1; }"),  # turned to zero
    ("        s->len += want - s->rem;", "        s->len = want;"),                                                  # the elapsed arc rewound
    ("        s->end_s = now_s + want;", "        s->end_s = now_s;"),                                               # the turn leaves the end
    ("        if (s->st == PM_WAIT || s->st == PM_WAITED) s->st = PM_PAUSE;", "        if (s->st == PM_WAIT) s->st = PM_PAUSE;"),  # a chimed phase stays chimed
    ("int pm_at_start(const pm_t *s) { return s->len <= 0 || s->rem >= s->len; }", "int pm_at_start(const pm_t *s) { return s->len <= 0 || s->rem > s->len; }"),  # a fresh phase is not fresh
    ("    if (pos == 0 && at_start && p < 0) p -= 1;", "    if (pos == 0 && !at_start && p < 0) p -= 1;"),           # -1 skipped when it should not be
    ("    const int lo = k > 0 ? -k - 1 : -1;", "    const int lo = k > 0 ? -k : -1;"),                             # one slot off the front of the cycle
    ("        return p > hi ? hi : p;", "        return p;"),                                                        # the ladder runs off the end
    ("    if (pos == 0) return;", "    if (pos < 0) return;"),                                                       # letting go at 0 moves
    ("    slot = pos < 0 ? s->k + pos + 1 : s->k + pos;", "    slot = pos < 0 ? s->k + pos : s->k + pos;"),          # the ladder a slot out
    ("    if (s->st != PM_RUN) return PM_EV_NONE;", "    if (s->st > PM_WAITED) return PM_EV_NONE;"),                # the same boundary chimes twice
    ("    if (s->end_s == 0) s->end_s = now_s + (s->rem > 0 ? s->rem : 0);", "    if (s->end_s == 0) s->end_s = now_s;"),  # a press ends at once
    ("    if (now_s < s->end_s) {", "    if (now_s <= s->end_s) {"),                                                 # the last second is called over
    ("        s->rem = (int32_t)(s->end_s - now_s);", "        s->rem = (int32_t)(s->end_s);"),                      # the countdown frozen
    ("        s->st = PM_WAIT;\n        s->rem = 0;\n        s->end_s = 0;\n        return PM_EV_END;", "        s->st = PM_WAITED;\n        s->rem = 0;\n        s->end_s = 0;\n        return PM_EV_END;"),  # a boundary chimes no more
    ("    return PM_EV_END | PM_EV_START;", "    return PM_EV_START;"),                                              # autostart owes no chime
    ("        if (left > 0) {", "        if (left >= 0) {"),                                                         # over by 0 s is still running
    ("    if (st > PM_WAITED) st = PM_WAITED;", "    if (st > PM_WAITED) st = PM_STOP;"),                             # a torn key read as ready
    ("    if (k > n - 1) k = n - 1;", "    if (k > n) k = n;"),                                                      # a torn key off the cycle
    ("    rem = clamp_len(raw->rem, cap);", "    rem = raw->rem;"),                                                  # a negative remaining trusted
    ("    if (st == PM_WAIT || st == PM_WAITED) {", "    if (st == PM_WAITED) {"),                                    # a stored WAIT chimes again
    ("    s->len = s->rem = pm_len_s(c, PM_FOCUS);", "    s->len = s->rem = pm_len_s(c, PM_SHORT);"),                # a first run ready at the break's length
    ("    s->st = PM_PAUSE;                            /* a RUN the clock cannot place is held */", "    s->st = PM_RUN;                            /* a RUN the clock cannot place is held */"),  # a 1970 guess taken for elapsed time
    ("    return base_alpha * opacity / 100;", "    return (base_alpha * opacity + 50) / 100;"),                     # scaled up, not down
    ("    default:                           return -1;", "    default:                           return 0;"),        # STOP owns a steady ring
    ("    case PM_PAUSE:                     return 2;", "    case PM_PAUSE:                     return 1;"),        # a pause flashes
    ("    case PM_RUN:                       return 0;", "    case PM_RUN:                       return 2;"),        # a run breathes
    ("    if (s->len <= 0) return s->rem > 0 ? 1000 : 0;", "    if (s->len <= 0) return 0;"),                        # a full arc read as an empty one
    ("    if (f > 1000) f = 1000;", "    if (f > 2000) f = 1000;"),                                                  # a past-full arc
    ("    if (eff < 0 || !c->ring_on || c->opacity <= 0) return 0;", "    if (eff < 0 || !c->ring_on) return 0;"),    # an invisible record owned
    ("    out->period_ms = eff == 2 ? PM_BREATHE_MS : eff == 1 ? PM_FLASH_MS : 0;", "    out->period_ms = eff == 2 ? PM_FLASH_MS : eff == 1 ? PM_BREATHE_MS : 0;"),  # the periods swapped
    ("    const uint32_t rgb = c->col[pm_ring_phase(s, c->focuses)] & 0x00FFFFFFu;", "    const uint32_t rgb = c->col[PM_FOCUS] & 0x00FFFFFFu;"),  # every phase ruby
    ("    while (lit[i] && i < cap - 1) { buf[i] = lit[i]; i++; }", "    while (lit[i] && i < cap) { buf[i] = lit[i]; i++; }"),  # a word past the caller's cap

    ("    else if (s->st == PM_WAIT || s->st == PM_WAITED) put(buf, cap, \"Waiting\");", "    else if (s->st == PM_WAIT) put(buf, cap, \"Waiting\");"),  # WAITED says nothing
    ("    raw->st_k = (int32_t)s->k * PM_PACK_MUL + s->st;", "    raw->st_k = (int32_t)s->k + s->st;"),              # the packing broken
]

# The checks themselves live in C, beside the machine: the driver prints one line per check
# and a final "CHECKS <n> <fails>"; a crashed or unparseable driver counts as a caught mutant
# (the check that vanished is not a pass). shape() pins the split's point on the text.
HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "pomodoro.h"

static int tot = 0, bad = 0;
static char gbuf[64];
static char slab[32];                       /* file-scope so the canary byte really follows the cap */
static char *const slab_canary = slab + 31;

static void ckk(int got, int want, const char *what)
{
    tot++;
    if (got != want) {
        bad++;
        printf("FAIL  %s: got %d want %d\n", what, got, want);
    }
}
static void tx(const char *want, const char *what)
{
    tot++;
    if (strcmp(gbuf, want) != 0) {
        bad++;
        printf("FAIL  %s: got '%s' want '%s'\n", what, gbuf, want);
    }
}

static pm_cfg_t cfg4(int autostart, int op, int ring_on)
{
    pm_cfg_t c;
    c.len_s[PM_FOCUS] = 1500; c.len_s[PM_SHORT] = 360;  c.len_s[PM_LONG] = 900;
    c.cap_s[PM_FOCUS] = 3600; c.cap_s[PM_SHORT] = 600; c.cap_s[PM_LONG] = 1800;
    c.focuses = 4; c.autostart = autostart; c.ring_on = ring_on; c.opacity = op;
    c.col[PM_FOCUS] = 0xBF0A0C; c.col[PM_SHORT] = 0x0A14B4; c.col[PM_LONG] = 0x0AA628;
    return c;
}
static pm_t mk(int st, int k, int len, int rem, int64_t end_s)
{
    pm_t s;
    s.st = (int16_t)st; s.k = (int16_t)k; s.len = len; s.rem = rem; s.end_s = end_s;
    return s;
}

int main(void)
{
    const int ph4[8] = { PM_FOCUS, PM_SHORT, PM_FOCUS, PM_SHORT, PM_FOCUS, PM_SHORT, PM_FOCUS, PM_LONG };
    const int fo4[8] = { 1, 0, 2, 0, 3, 0, 4, 0 };
    int n, i;
    gbuf[0] = 'C';   /* the canary: put() may never write at or past the cap it was given */

    /* ---- the cycle (design §2) ---- */
    ckk(pm_cycle_len(4), 8, "cycle_len(4) == 8");
    ckk(pm_cycle_len(0), 2, "N under 1 clamps: cycle_len(0) == 2");
    ckk(pm_cycle_len(-5), 2, "negative N clamps too");
    ckk(pm_cycle_len(1), 2, "cycle_len(1) == 2");
    ckk(pm_cycle_len(12), 24, "cycle_len(12) == 24");
    ckk(pm_cycle_len(99), 24, "N over 12 clamps: cycle_len(99) == 24");
    for (n = 1; n <= 12; n++) {
        char w[64];
        sprintf(w, "phase_at walks a whole cycle for N=%d", n);
        int long_only_last = pm_phase_at(2 * n - 1, n) == PM_LONG && pm_phase_at(0, n) == PM_FOCUS;
        for (i = 0; i < 2 * n - 1; i++) long_only_last -= pm_phase_at(i, n) == PM_LONG;
        ckk(long_only_last, 1, w);   /* the long break sits only at the cycle's last slot */
    }
    for (i = 0; i < 8; i++) {
        ckk(pm_phase_at(i, 4), ph4[i], "the N=4 cycle: focus, short, ..., focus, long");
        ckk(pm_focus_at(i, 4), fo4[i], "focus_at: 1..N on a focus, 0 on either break");
    }
    ckk(pm_phase_at(1, 1), PM_LONG, "N=1: focus then long, no short break");
    ckk(pm_next_k(7, 4) == 0 && pm_next_k(0, 4) == 1 && pm_next_k(6, 4) == 7, 1, "next_k wraps at the last slot");
    ckk(pm_at_start(&(pm_t){PM_RUN, 0, 1500, 1500, 0}) == 1 && pm_at_start(&(pm_t){PM_STOP, 0, 0, 0, 0}) == 1, 1,
        "a phase none of which has gone, and a 0-length one, are both at their start");
    ckk(pm_at_start(&(pm_t){PM_RUN, 0, 1500, 1499, 0}) == 0 && pm_at_start(&(pm_t){PM_PAUSE, 0, 600, 0, 0}) == 0, 1,
        "one second gone, or all of it gone, is not at the start");
    {
        pm_cfg_t c = cfg4(0, 100, 1);
        ckk(pm_len_s(&c, PM_FOCUS), 1500, "a setting inside its cap passes through");
        c.cap_s[PM_FOCUS] = 90;
        ckk(pm_len_s(&c, PM_FOCUS), 90, "a setting past its own cap clamps to the cap");
        c.cap_s[PM_FOCUS] = 3600; c.len_s[PM_FOCUS] = 5;
        ckk(pm_len_s(&c, PM_FOCUS), PM_MIN_S, "a setting under a minute clamps up to 60");
    }

    /* ---- press (design §6.1) ---- */
    {
        pm_cfg_t c = cfg4(0, 100, 1);
        pm_t s = mk(PM_STOP, 0, 1500, 1500, 0);
        pm_press(&s, &c);
        ckk(s.st, PM_RUN, "press over ready starts the phase");
        ckk(s.rem, 1500, "it starts at its full length");
        ckk(s.len, 1500, "the denominator matches");
        pm_service(&s, &c, 1000);
        ckk(s.end_s == 2500 && s.rem == 1500, 1, "the caller's first wall second anchors end_s at now + rem");
        pm_service(&s, &c, 1100);
        ckk(s.rem, 1400, "rem follows the clock, never a count");
        pm_press(&s, &c);
        ckk(s.st == PM_PAUSE && s.rem == 1400 && s.end_s == 0, 1, "press over a run holds it: rem kept, end dropped");
        pm_press(&s, &c);
        ckk(s.st == PM_RUN && s.rem == 1400 && s.end_s == 0, 1, "press again resumes the kept seconds");
        pm_service(&s, &c, 1400);
        ckk(s.end_s == 2800, 1, "the resume re-anchors from the caller's now");
        c.autostart = 0;
        s.st = PM_WAIT; s.k = 5; s.len = 1500; s.rem = 0; s.end_s = 0;
        pm_press(&s, &c);
        ckk(s.k == 6 && s.st == PM_RUN && s.len == 1500 && s.rem == 1500, 1,
            "a press over a finished short break starts the 25-min focus, not the 6 that ran");
        s.st = PM_WAITED; s.k = 6;
        pm_press(&s, &c);
        ckk(s.k == 7 && s.len == 900 && s.rem == 900, 1, "the press onto the long break takes its setting length");
        c.autostart = 0;
        s.st = PM_RUN; s.k = 7; s.len = 900; s.rem = 900; s.end_s = 900;
        pm_service(&s, &c, 900);                       /* the long break runs out: WAIT again */
        ckk(s.st == PM_WAIT && s.k == 7 && s.rem == 0, 1, "the long break ends into waiting, a second time");
        pm_press(&s, &c);
        ckk(s.k == 0 && s.len == 1500 && s.rem == 1500 && s.st == PM_RUN, 1, "the press over the long break returns to k=0 fresh");
        c.autostart = 1;
        s.st = PM_STOP; s.k = 0; s.len = 1500; s.rem = 1500; s.end_s = 0;
        pm_press(&s, &c);
        for (i = 0; i < 7; i++) {                    /* the six phases and the third break */
            pm_service(&s, &c, s.end_s);
            pm_service(&s, &c, (int64_t)s.end_s - 1);
        }
        ckk(s.k == 6 && s.st == PM_RUN && s.len == 1500, 1, "seven autostart boundaries land on the last focus");
        pm_service(&s, &c, s.end_s);
        ckk(s.k == 7 && s.len == 900 && s.rem == 900, 1, "the eighth starts the long break");
        pm_service(&s, &c, s.end_s);
        ckk(s.k == 0 && s.st == PM_RUN && s.rem == 1500, 1, "and the next wraps the cycle to a fresh k=0");
    }

    /* the pressed STOP starts at the length the knob left, not the config's (design §6.1) */
    {
        pm_cfg_t c = cfg4(0, 100, 1);
        pm_t s = mk(PM_STOP, 0, 1200, 900, 0);
        pm_press(&s, &c);
        ckk(s.len == 1200 && s.rem == 1200, 1, "a turned STOP starts at the turned length");
        pm_land(&s, &c, 1);                          /* a landed short break, untouched */
        pm_press(&s, &c);
        ckk(s.k == 1 && s.len == 360 && s.rem == 360, 1, "an untouched short break starts at its setting, not the focus's");
    }

    /* ---- turn (design §6.2) ---- */
    {
        pm_cfg_t c = cfg4(0, 100, 1);
        pm_cfg_t s25 = cfg4(0, 100, 1);
        pm_cfg_t s60 = cfg4(0, 100, 1);
        pm_t s = mk(PM_STOP, 0, 900, 900, 0);
        pm_t w = mk(PM_STOP, 0, 600, 600, 0), r = mk(PM_RUN, 0, 1500, 900, 4000), q;
        s25.cap_s[PM_FOCUS] = 90;
        s60.cap_s[PM_FOCUS] = 60;
        ckk(pm_turn(&s, &c, 1, 5000), PM_EV_NONE, "one clockwise click asks under the cap");
        ckk(s.rem, 960, "a click is 60 seconds");
        ckk(s.len, 960, "STOP: the length follows the turn");
        ckk(pm_turn(&s, &c, -1, 5000), PM_EV_NONE, "a back click asks under the floor");
        ckk(s.rem, 900, "and 60 seconds the other way too");
        ckk(pm_turn(&w, &s25, 1, 5000) == PM_EV_LIMIT && w.rem == 90 && w.len == 90, 1,
            "the ceiling is cap_s, not the setting: a click past a cap of 90 stops at 90");
        ckk(pm_turn(&w, &s25, 1, 5000) == PM_EV_LIMIT && w.rem == 90, 1, "a click at the cap reports the limit");
        w = mk(PM_STOP, 0, 1500, 1500, 0);
        ckk(pm_turn(&w, &c, 6, 5000) == PM_EV_NONE && w.rem == 1860 && w.len == 1860, 1,
            "the ceiling is the setting's cap, not its value: 25 turns past 30 while len_s stays 25");
        ckk(pm_turn(&w, &c, 30, 5000) == PM_EV_LIMIT && w.rem == 3600 && w.len == 3600, 1,
            "and 60 min is the cap: the ask over it stops at it and reports");
        w = mk(PM_STOP, 0, 90, 90, 0);
        ckk(pm_turn(&w, &s25, 3, 5000) == PM_EV_LIMIT && w.rem == 90 && w.len == 90, 1, "a wild ask is the cap and the limit both");
        ckk(pm_turn(&w, &c, -1, 5000) == PM_EV_LIMIT && w.rem == PM_MIN_S && w.len == PM_MIN_S, 1,
            "a click under a cap of 90 lands on the floor and says so");
        q = mk(PM_STOP, 0, 600, 600, 0);
        pm_turn(&q, &c, -9, 5000);
        ckk(q.rem == PM_MIN_S && q.len == PM_MIN_S, 1, "the floor is PM_MIN_S, never 0: nine clicks down sit on it");
        q = mk(PM_RUN, 0, 1500, 900, 4000);
        ckk(pm_turn(&q, &c, -6, 5000) == PM_EV_NONE && q.rem == 540 && q.len == 1140, 1,
            "in RUN a long turn back moves len with rem: the elapsed 600 s stay gone");
        q = mk(PM_STOP, 0, 60, 60, 0);
        ckk(pm_turn(&q, &s60, -1, 5000) == PM_EV_LIMIT && q.rem == PM_MIN_S, 1, "under a cap of 60 the floor still holds");
        ckk(pm_turn(&r, &c, 1, 5000) == PM_EV_NONE && r.rem == 960 && r.len == 1560, 1,
            "in RUN the elapsed arc is kept: len grows with rem");
        ckk(r.end_s == 5960, 1, "and the end moves to now + the new rem");
        ckk(pm_fraction(&r) == 615 && pm_fraction(&r) >= 600, 1, "the fraction never goes back up on a lengthen");
        q = mk(PM_WAIT, 1, 360, 0, 0);
        pm_turn(&q, &c, 1, 5000);
        ckk(q.st, PM_PAUSE, "a turn over a waiting phase ends the waiting");
        q = mk(PM_WAITED, 1, 900, 0, 0);
        pm_turn(&q, &c, 1, 5000);
        ckk(q.st, PM_PAUSE, "a turn over an already-chimed phase ends the waiting too");
    }

    /* ---- the ladder (design §6.3) ---- */
    ckk(pm_pos_step(0, -1, 0, 2, 4), -1, "mid-phase, one back is this phase at its start");
    ckk(pm_pos_step(0, -9, 0, 2, 4), -3, "k=2's anticlockwise end is the first focus: two clicks of ladder");
    ckk(pm_pos_step(0, -9, 0, 6, 4), -7, "k=6 can walk the whole cycle back: seven clicks of ladder");
    ckk(pm_pos_step(0, -1, 1, 4, 4), -2, "a phase at its start skips -1: the first back click is the phase before");
    ckk(pm_pos_step(0, -9, 1, 4, 4), -5, "the anticlockwise end is the first focus: k=4 sees five clicks of ladder");
    ckk(pm_pos_step(-4, -1, 0, 4, 4), -5, "one more click at the front stays at the front");
    ckk(pm_pos_step(-2, -1, 0, 0, 4), -1, "the ladder stops at k=0 and the ends repeat");
    ckk(pm_pos_step(0, 1, 0, 2, 4), 1, "clockwise steps to the next phase");
    ckk(pm_pos_step(0, 1, 0, 7, 4), 0, "the long break is the clockwise end: a click there stays put");
    ckk(pm_pos_step(0, 9, 0, 7, 4), 0, "and a wild clockwise ask stops there too");
    ckk(pm_pos_step(4, 3, 0, 2, 4), 5, "the ladder clamps onto the cycle's end, then stays");
    {
        pm_cfg_t c = cfg4(0, 100, 1);
        pm_t s;
        s = mk(PM_RUN, 2, 1500, 1230, 0);
        pm_land(&s, &c, 0);
        ckk(s.st == PM_RUN && s.k == 2 && s.rem == 1230, 1, "pm_land(0) changes nothing at all");
        s = mk(PM_RUN, 2, 1500, 1230, 0);
        pm_land(&s, &c, -1);
        ckk(s.st == PM_STOP && s.k == 2 && s.len == 1500 && s.rem == 1500, 1, "landing at -1: this phase back at its start");
        s = mk(PM_RUN, 2, 1500, 1230, 0);
        pm_land(&s, &c, -2);
        ckk(s.k == 1 && s.len == 360, 1, "landing at -2: the short break before, at its setting length");
        s = mk(PM_RUN, 2, 1500, 1230, 0);
        pm_land(&s, &c, 1);
        ckk(s.k == 3 && s.len == 360 && s.st == PM_STOP, 1, "landing at +1: the next phase, ready");
    }

    /* ---- service (design §5.1) ---- */
    {
        pm_cfg_t c = cfg4(0, 100, 1);
        pm_t s = mk(PM_RUN, 0, 300, 300, 1000);
        ckk(pm_service(&s, &c, 940) == PM_EV_NONE && s.rem == 60, 1, "nothing before the end, rem follows the seconds");
        ckk(pm_service(&s, &c, 1000) == PM_EV_END && s.st == PM_WAIT && s.rem == 0 && s.end_s == 0, 1,
            "at end_s exactly: one END, WAIT, rem 0, end cleared");
        ckk(pm_service(&s, &c, 1001) == PM_EV_NONE && s.st == PM_WAIT, 1, "a second pass over the WAIT chimes again: no");
        s = mk(PM_RUN, 0, 300, 300, 1000);
        ckk(pm_service(&s, &c, 11800) == PM_EV_END && s.st == PM_WAIT, 1, "a boundary 3 hours late returns one END");
        ckk(pm_service(&s, &c, 11801) == PM_EV_NONE, 1, "and no more: the lost hours never chime twice");
        c.autostart = 1;
        s = mk(PM_RUN, 0, 300, 300, 1000);
        ckk(pm_service(&s, &c, 1000) == (PM_EV_END | PM_EV_START), 1, "autostart owes the chime for that boundary and a start");
        ckk(s.k == 1 && s.st == PM_RUN && s.len == 360 && s.rem == 360 && s.end_s == 1360, 1,
            "autostart chains into the next phase at its setting length");
        s = mk(PM_PAUSE, 1, 600, 300, 0);
        ckk(pm_service(&s, &c, 5000) == PM_EV_NONE && s.st == PM_PAUSE && s.rem == 300 && s.end_s == 0, 1,
            "a held phase is passed over: no anchor, no countdown");
        s = mk(PM_WAITED, 1, 600, 0, 0);
        ckk(pm_service(&s, &c, 5000) == PM_EV_NONE && s.st == PM_WAITED, 1, "a chimed phase is passed over too");
    }

    /* ---- store / restore (design §5.2) ---- */
    {
        pm_cfg_t c = cfg4(0, 100, 1);
        pm_raw_t raw, r2;
        pm_t s, b;
        pm_restore(&s, &c, &(pm_raw_t){0, 0, 0, 0}, 1, 1000);
        ckk(s.st == PM_STOP && s.k == 0 && s.len == 1500 && s.rem == 1500 && s.end_s == 0, 1,
            "an all-zero key restores a fresh cycle at the focus setting's length");
        pm_restore(&s, &c, &(pm_raw_t){41, 1500, 1010, 900}, 1, 1000);
        ckk(s.st == PM_RUN && s.k == 5 && s.rem == 10 && s.end_s == 1010, 1, "a synced RUN 10 s ahead comes back RUN at ~10 s");
        pm_restore(&s, &c, &(pm_raw_t){41, 1500, 960, 900}, 1, 1000);
        ckk(s.st == PM_WAITED && s.rem == 0 && s.end_s == 0, 1, "a RUN that ended while away: WAITED, silent");
        pm_restore(&s, &c, &(pm_raw_t){41, 1500, 1000, 900}, 1, 1000);
        ckk(s.st == PM_WAITED && s.rem == 0 && s.end_s == 0, 1, "a RUN due at this very second is over, not running");
        pm_restore(&s, &c, &(pm_raw_t){42, 1500, 0, 240}, 0, 1000);
        ckk(s.st == PM_PAUSE && s.rem == 240, 1, "no clock: a stored RUN comes back PAUSE at pm_rem");
        ckk(pm_service(&s, &c, 1001) == PM_EV_NONE, 1, "the held restore does not count down");
        ckk(pm_raw_needs_clock(&(pm_raw_t){41, 1500, 1010, 900}) == 1, 1,
            "a stored RUN needs the clock: its end is a wall second (held until sync, Bronze 2026-11-11)");
        ckk(pm_raw_needs_clock(&(pm_raw_t){42, 1500, 0, 240}) == 0 && pm_raw_needs_clock(&(pm_raw_t){0, 0, 0, 0}) == 0
            && pm_raw_needs_clock(&(pm_raw_t){27, 360, 0, 0}) == 0 && pm_raw_needs_clock(&(pm_raw_t){8, 360, 0, 0}) == 0, 1,
            "PAUSE, no key, WAIT and a STOP at k 1 do not need the clock");
        pm_restore(&s, &c, &(pm_raw_t){41, 1500, 1010, 900}, 0, 1000);
        ckk(s.st == PM_PAUSE, 1, "no clock: a stored RUN is held as PAUSE (the app keeps the record and finishes on sync)");
        pm_restore(&s, &c, &(pm_raw_t){41, 1500, 1010, 900}, 1, 1000);
        ckk(s.st == PM_RUN && s.rem == 10, 1, "the same record restored after the sync runs on, ~10 s left");
        pm_restore(&s, &c, &(pm_raw_t){199, 1500, 0, 900}, 1, 1000);
        ckk(s.st == PM_WAITED && s.k == 7, 1, "a torn pm_st of 199 is clamped, not trusted");
        pm_restore(&s, &c, &(pm_raw_t){10, 900, 0, -500}, 0, 1000);
        ckk(s.rem == PM_MIN_S, 1, "a negative pm_rem is clamped up, not trusted");
        pm_restore(&s, &c, &(pm_raw_t){27, 360, 0, 0}, 1, 1000);
        ckk(s.st == PM_WAITED, 1, "a stored WAIT comes back already chimed: no chime out of a restore");
        s = mk(PM_RUN, 5, 600, 321, 777);
        pm_store(&s, &raw);
        ckk(raw.st_k == 5 * PM_PACK_MUL + PM_RUN, 1, "pm_st packs k * PM_PACK_MUL + state");
        ckk(raw.len == 600 && raw.rem == 321 && raw.end == 777, 1, "the other three settings are the state's plainly");
        pm_restore(&s, &c, &raw, 1, 456);
        ckk(s.st == PM_RUN && s.k == 5 && s.rem == 321 && s.len == 600 && s.end_s == 777, 1, "a RUN round-trips");
        s = mk(PM_PAUSE, 3, 600, 300, 0);
        pm_store(&s, &raw);
        pm_restore(&b, &c, &raw, 1, 456);
        ckk(b.st == PM_PAUSE && b.k == 3 && b.rem == 300, 1, "a PAUSE round-trips");
        s = mk(PM_WAITED, 1, 360, 0, 0);
        pm_store(&s, &raw);
        pm_restore(&b, &c, &raw, 1, 456);
        ckk(b.st == PM_WAITED && b.k == 1, 1, "a WAITED round-trips");
        pm_store(&b, &raw);                              /* the restored state stores back the same quartet */
        pm_store(&s, &r2);                              /* r2 holds a second copy of raw */
        ckk(pm_changed(&raw, &r2), 0, "pm_changed: an identical quartet writes nothing");
        r2.rem = raw.rem + 1;
        ckk(pm_changed(&raw, &r2) != 0, 1, "any one of the four moving is a change");
    }

    /* ---- the ring (design §7) ---- */
    {
        pm_cfg_t c = cfg4(0, 100, 1);
        pm_cfg_t off = cfg4(0, 100, 0), glass = cfg4(0, 0, 1);
        pm_ring_t out;
        pm_t r = mk(PM_RUN, 0, 1500, 1500, 9999);
        ckk(pm_ring_for(&r, &c, &out), 1, "a RUN owns the ring");
        ckk((int)(out.arc >> 24), PM_ARC_A, "opacity 100 leaves the arc's base alpha");
        ckk((int)(out.track >> 24), PM_TRACK_A, "and the track's");
        ckk((int)(out.arc & 0x00FFFFFFu), 0xBF0A0C, "the arc wears the focus colour's low 24 bits");
        ckk((int)(out.track & 0x00FFFFFFu), 0xBF0A0C, "the track is the same colour, dimmer alpha");
        ckk(out.effect == 0 && out.period_ms == 0 && out.fraction == 1000, 1, "RUN: steady, no period, a full arc");
        r.rem = 1230;
        ckk(pm_fraction(&r), 820, "fraction = rem*1000/len: 20:30 of 25:00");
        r.rem = 750;
        ckk(pm_fraction(&r), 500, "12:30 of 25:00 is 500");
        r.rem = 2010;                                /* 1340 by the quotient: inside any honest clamp */
        ckk(pm_fraction(&r) == 1000, 1, "a past-full rem still reads exactly a full arc");
        ckk(pm_fraction(&(pm_t){PM_RUN, 0, 0, 0, 0}) == 0 && pm_fraction(&(pm_t){PM_RUN, 0, 0, 300, 0}) == 1000, 1,
            "0 s of a 0-length phase divides nothing: 0, and a positive rem reads full");
        c.opacity = 85;
        pm_ring_for(&r, &c, &out);
        ckk((int)(out.arc >> 24), 0xC3, "opacity 85 scales 0xE6 to 0xC3");
        ckk((int)(out.track >> 24), 0x2B, "and 0x33 to 0x2B");
        ckk((int)(out.track >> 24) <= (int)(out.arc >> 24), 1, "the track is never brighter than the arc");
        c.opacity = 50;
        pm_ring_for(&r, &c, &out);
        ckk((int)(out.arc >> 24), 115, "the scale rounds down, never up");
        c.opacity = 100;
        ckk(pm_ring_phase(&r, 4), PM_FOCUS, "a focus wears the focus colour");
        ckk(pm_ring_for(&(pm_t){PM_WAIT, 1, 360, 0, 0}, &c, &out) == 1 && out.effect == 1
            && out.period_ms == PM_FLASH_MS && out.fraction == 0, 1, "WAIT: flash at 2000 with an empty arc");
        pm_ring_for(&(pm_t){PM_WAIT, 1, 360, 0, 0}, &c, &out);
        c.col[PM_SHORT] = 0x1F0A14B4u;               /* a colour setting with an alpha byte */
        pm_ring_for(&(pm_t){PM_WAIT, 1, 360, 0, 0}, &c, &out);
        ckk(out.arc == 0xE60A14B4u, 1, "the setting's alpha is masked off; the phase colour's low 24 win");
        ckk(pm_ring_for(&(pm_t){PM_STOP, 0, 1500, 1500, 0}, &c, &out) == 0, 1, "STOP clears the ring");
        ckk(pm_ring_effect(&(pm_t){PM_STOP, 0, 0, 0, 0}) == -1
            && pm_ring_effect(&(pm_t){PM_PAUSE, 0, 0, 0, 0}) == 2 && pm_ring_effect(&r) == 0, 1,
            "effect: STOP -1, PAUSE breathe, RUN steady");
        ckk(pm_ring_for(&r, &off, &out) == 0, 1, "ring_on 0 clears rather than owns");
        ckk(pm_ring_for(&r, &glass, &out) == 0, 1, "opacity 0 clears rather than an invisible record");
        ckk(pm_ring_for(&(pm_t){PM_PAUSE, 2, 600, 300, 0}, &c, &out) == 1 && out.effect == 2
            && out.period_ms >= 4000, 1, "PAUSE breathes, slow only");
        ckk(PM_STEP_PCT == 4 && 3 < PM_STEP_PCT, 1, "PM_STEP_PCT is the send floor: 3 per mille is under it");
    }

    /* ---- the words (design §10.1) ---- */
    for (i = 0; i < 32; i++) slab[i] = '.';
    *slab_canary = 'C';                              /* one past a cap of 31: put() may not touch it */
    pm_count_text(&(pm_t){PM_RUN, 22, 0, 0, 0}, 12, slab, 31);
    ckk(slab[8] == 0 && *slab_canary == 'C', 1,
        "a count ends inside the cap it was given, and the byte past that cap is untouched");
    for (i = 0; i < 32; i++) slab[i] = '.';
    *slab_canary = 'C';
    pm_phase_text(&(pm_t){PM_RUN, 3, 0, 0, 0}, 4, slab, 8);                /* "Short break" into 8 bytes */
    ckk(slab[6] == 'b' && slab[7] == 0 && slab[8] == '.' && *slab_canary == 'C', 1,
        "a too-long word truncates at the cap: the NUL sits at cap-1, nothing above it is touched");
    pm_state_text(&(pm_t){PM_RUN, 0, 0, 0, 0}, gbuf, 24);    tx("", "RUN says nothing");
    pm_state_text(&(pm_t){PM_STOP, 0, 0, 0, 0}, gbuf, 24);   tx("", "ready says nothing");
    pm_state_text(&(pm_t){PM_PAUSE, 0, 0, 0, 0}, gbuf, 24);  tx("Paused", "PAUSE says Paused");
    pm_state_text(&(pm_t){PM_WAIT, 0, 0, 0, 0}, gbuf, 24);   tx("Waiting", "WAIT says Waiting");
    pm_state_text(&(pm_t){PM_WAITED, 0, 0, 0, 0}, gbuf, 24); tx("Waiting", "WAITED says Waiting too");
    pm_phase_text(&(pm_t){PM_RUN, 2, 0, 0, 0}, 4, gbuf, 24); tx("Focus", "phase text: Focus");
    pm_phase_text(&(pm_t){PM_RUN, 3, 0, 0, 0}, 4, gbuf, 24); tx("Short break", "phase text: Short break");
    pm_phase_text(&(pm_t){PM_RUN, 7, 0, 0, 0}, 4, gbuf, 24); tx("Long break", "phase text: Long break");
    pm_count_text(&(pm_t){PM_RUN, 4, 0, 0, 0}, 4, gbuf, 24); tx("3 of 4", "count text: 3 of 4");
    pm_count_text(&(pm_t){PM_RUN, 1, 0, 0, 0}, 4, gbuf, 24); tx("break", "count text: break on a short one");
    pm_count_text(&(pm_t){PM_RUN, 7, 0, 0, 0}, 4, gbuf, 24); tx("break", "count text: break on the long one");
    pm_count_text(&(pm_t){PM_RUN, 22, 0, 0, 0}, 12, gbuf, 24); tx("12 of 12", "count text at N=12");
    for (n = 1; n <= 12; n++)
        for (i = 0; i < 2 * n; i++) {
            char w[64];
            pm_count_text(&(pm_t){PM_RUN, (int16_t)i, 0, 0, 0}, n, gbuf, 24);
            sprintf(w, "a count always fits PM_TEXT at N=%d k=%d", n, i);
            ckk((int)strlen(gbuf) < 24, 1, w);
        }

    printf("CHECKS %d %d\n", tot, bad);
    return bad ? 1 : 0;
}
"""


def run_c(src, d):
    """Compile the shipped source verbatim beside the driver and run it. None = no verdict."""
    open(os.path.join(d, "pomodoro.c"), "w").write(src)
    open(os.path.join(d, "h.c"), "w").write(HARNESS)
    shutil.copy(os.path.join(APP, "pomodoro.h"), os.path.join(d, "pomodoro.h"))
    exe = os.path.join(d, "h")
    r = subprocess.run(["gcc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", d,
                        "-o", exe, os.path.join(d, "pomodoro.c"), os.path.join(d, "h.c")],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("compile failed:\n" + r.stderr)
    p = subprocess.run([exe], capture_output=True, text=True)
    m = re.search(r"CHECKS (\d+) (\d+)", p.stdout + p.stderr)
    if not m:
        return None
    run_c.output = p.stdout
    return int(m.group(1)), int(m.group(2))


def verdict(src, d, quiet):
    """One round of checks: the C driver's assertions plus the split's shape on the text."""
    fails = []

    def fail(what, ok):
        if not ok:
            fails.append(what)
        if not quiet:
            print(("ok    " if ok else "FAIL  ") + what)

    res = run_c(src, d)
    if res and not quiet:
        for line in run_c.output.splitlines():
            if line.startswith("FAIL"):
                print(line)
    if res:
        fail("the driver ran %d checks and %d failed (min %d)" % (res[0], res[1], MIN_CHECKS),
             res[1] == 0 and res[0] >= MIN_CHECKS)
    else:
        fail("the driver produced no CHECKS line", False)
    shape(fail, open(os.path.join(APP, "pomodoro.h")).read(), src)
    return bool(fails) or res is None


def main():
    src = open(os.path.join(APP, "pomodoro.c")).read()
    with tempfile.TemporaryDirectory() as d:
        if "--mutations" in sys.argv:
            caught = 0
            for a, b in MUTATIONS:
                if src.count(a) != 1:
                    raise SystemExit("mutation %r applies %d times -- rewrite the suite" % (a[:40], src.count(a)))
                bit = verdict(src.replace(a, b, 1), d, True)
                print(("bites " if bit else "MISSED") + "  " + a.strip()[:70])
                caught += bit
            print("mutants caught %d/%d" % (caught, len(MUTATIONS)))
            return 0 if caught == len(MUTATIONS) else 1
        if verdict(src, d, False):
            return 1
        print("all pomodoro checks pass")
        return 0


if __name__ == "__main__":
    sys.exit(main())
