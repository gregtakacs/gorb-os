#!/usr/bin/env python3
"""host_hand_aa_test.py — the hand-AA capability rule, mirrored and mutated.

The shipped hand_aa_for() in main/clock/clock_view.c is pure (reads the theme
scalars only), but the TU cannot be verbatim-compiled on a host — it drags
lvgl/display headers. So this is a mirror, the host_sched_rung_test.py
discipline: a Python restatement of the rule, a lint that reads the SHIPPING C
and asserts every branch the mirror depends on is still present, a truth table
over the shipped presets' carrier words, and a mutation harness. If someone
edits the rule, the lint fails and forces a re-read.

The rule (the Station AA ruling, Oct 30): LVGL's rotated RGB565A8 AA path
samples 4 taps per dest px, measured ~4x the nearest path, and the whole ban
on it (40 ms/frame) was priced on hands re-posed OVER an animated carrier. A
still dial pays a hand transform only when that hand's angle actually moves,
so the filter fits there — except on the layer whose beat IS the hot tick: a
sweeping second hand re-poses every tick and keeps nearest, which is what
makes a sweeping Station's fps untouched by this change. The words the rule
reads are the same ones splitPolicy3 wrote at bake time (bg_mode, bg_frames,
gif_frames, tourbillon_n, second_sweep), so no new bit and no re-bake: every
still theme on a card turns the filter on by itself, and every animated theme
keeps yesterday's pixels.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "main", "clock", "clock_view.c")


# ---- the mirror ------------------------------------------------------------
MIRROR_SRC = '''
def hand_aa_for(bg_mode, bg_frames, gif_frames, tourbillon_n, second_sweep, slot):
    if bg_mode != 0 or bg_frames != 0 or gif_frames != 0 or tourbillon_n != 0:
        return False
    if slot == 2 and second_sweep:
        return False
    return True
'''


def _run(src):
    ns = {}
    exec(src, ns)
    return ns["hand_aa_for"]


hand_aa_for = _run(MIRROR_SRC)


# ---- the lint: the mirror must still match the shipped code -----------------
def lint():
    txt = open(SRC).read()
    start = txt.index("static bool hand_aa_for(")
    body = txt[start:txt.index("static lv_obj_t *make_layer(", start)]
    checks = [
        ("animated guard present",
         "if (t->bg_mode != 0 || t->bg_frames != 0 || t->gif_frames != 0 ||"),
        ("carriage in the guard", "t->tourbillon_n != 0)"),
        ("hot-tick seconds guard", "if (slot == 2 && t->second_sweep)"),
        ("make_layer consumes the flag", "lv_image_set_antialias(o, aa);"),
        ("make_layer takes the flag", "bool aa)"),
        ("hands loop asks per slot", "hand_aa_for(&clk_theme, k)"),
        ("shadow shares its hand's answer",
         "make_layer(&clk_theme.shadow[k], &s_shadow_dsc[k], aa)"),
        ("carriage never takes the filter",
         "make_layer(&clk_theme.tourbillon[0], &s_car_dsc, false)"),
        ("overlay never takes the filter",
         "make_layer(&clk_theme.overlay, &s_overlay_dsc, false)"),
        ("aux routes its own source",
         "hand_aa_for(&clk_theme, aa_slot)"),
    ]
    fails = []
    for why, needle in checks:
        if needle not in txt:
            fails.append("%s: %r no longer in clock_view.c" % (why, needle))
    # the body must be the two guards and one yes, and nothing grew around them
    if body.count("return false;") != 2 or body.count("return true;") != 1:
        fails.append("hand_aa_for's branch shape changed: the mirror's truth "
                     "table no longer covers it — re-read and re-pin")
    return fails


# ---- the truth table --------------------------------------------------------
# (words..., slot) -> expected, keyed by the shipped presets' carrier facts
# (watch-engine.js's motion table): aviator/station/luna/neon/phase/perpetual
# bake as still plates; skeleton/orbital/opentour/kitetour/calendar/regulator/
# modern/sector/diver as banks; orbital/kitetour with a carriage too.
STILL = dict(bg_mode=0, bg_frames=0, gif_frames=0, tourbillon_n=0)
PLATE_BANK = dict(STILL, bg_frames=9)            # 9-frame keyframe bank
DELTA_BANK = dict(STILL, bg_mode=1, gif_frames=8)  # GIF delta bank
CARRIAGE = dict(STILL, tourbillon_n=8)           # carriage on the seconds slot

VECTORS = [
    # A still dial with no sweep (aviator at 1 Hz): every hand takes the filter.
    (dict(STILL, second_sweep=0), 0, True),
    (dict(STILL, second_sweep=0), 1, True),
    (dict(STILL, second_sweep=0), 2, True),
    # A STILL dial WITH the sweep (station, the D34 cadence fix): the seconds
    # re-poses every hot tick and keeps nearest; the bar hands do not.
    (dict(STILL, second_sweep=1), 0, True),
    (dict(STILL, second_sweep=1), 1, True),
    (dict(STILL, second_sweep=1), 2, False),
    # Slots 3/4 never rotate at all; the rule answers for them the same as the
    # slow hands (the flag is inert, the answer must not depend on it).
    (dict(STILL, second_sweep=1), 3, True),
    (dict(STILL, second_sweep=1), 4, True),
    # Every animated carrier refuses the filter on every layer, sweep or not.
    (dict(PLATE_BANK, second_sweep=0), 0, False),
    (dict(PLATE_BANK, second_sweep=0), 1, False),
    (dict(PLATE_BANK, second_sweep=1), 1, False),
    (dict(DELTA_BANK, second_sweep=1), 1, False),
    (dict(CARRIAGE, second_sweep=1), 0, False),
    (dict(CARRIAGE, second_sweep=0), 1, False),
    # The guards are OR-ed: one animated word on an otherwise still dial is
    # enough — pins each of the four against a lone-deletion mutant.
    (dict(STILL, bg_mode=1, second_sweep=0), 1, False),
    (dict(STILL, bg_frames=1, second_sweep=0), 1, False),
    (dict(STILL, gif_frames=2, second_sweep=0), 1, False),
    (dict(STILL, tourbillon_n=1, second_sweep=0), 1, False),
    # An animated dial's seconds hand is refused by the carrier guard alone:
    # deleting only the sweep guard must not change any banked answer.
    (dict(PLATE_BANK, second_sweep=1), 2, False),
]


def check_vectors():
    fails = []
    for w, slot, exp in VECTORS:
        got = hand_aa_for(w["bg_mode"], w["bg_frames"], w["gif_frames"],
                          w["tourbillon_n"], w["second_sweep"], slot)
        if got != exp:
            fails.append("words %s slot %d: got %s, want %s" % (w, slot, got, exp))
    return fails


# ---- the mutations: each one must break at least one vector ------------------
MUTANTS = [
    ("drop the animated guard entirely",
     "if bg_mode != 0 or bg_frames != 0 or gif_frames != 0 or tourbillon_n != 0:",
     "if False:"),
    ("drop the bg_mode word", "bg_mode != 0 or ", ""),
    ("drop the bg_frames word", "bg_frames != 0 or ", ""),
    ("drop the gif_frames word", "gif_frames != 0 or ", ""),
    ("drop the carriage word", " or tourbillon_n != 0", ""),
    ("drop the sweep guard", "if slot == 2 and second_sweep:", "if False:"),
    ("sweep guard ignores the sweep", "and second_sweep:", ":"),
    ("sweep guard binds every slot", "if slot == 2 and", "if"),
    ("answer off always", "    return True", "    return False"),
]


def check_mutants():
    fails = []
    for name, old, new in MUTANTS:
        m = _run(MIRROR_SRC.replace(old, new, 1))
        caught = any(m(w["bg_mode"], w["bg_frames"], w["gif_frames"],
                       w["tourbillon_n"], w["second_sweep"], slot) != exp
                     for w, slot, exp in VECTORS)
        if not caught:
            fails.append("mutant survived: %s" % name)
    return fails


def main():
    fails = lint() + check_vectors() + check_mutants()
    for f in fails:
        print("FAIL: " + f)
    print("hand AA rule: %d checks, %d failures" % (
        len(VECTORS) + len(MUTANTS) + 12, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

