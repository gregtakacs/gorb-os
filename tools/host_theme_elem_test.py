#!/usr/bin/env python3
"""P6.1's element-table suite: the schema D32 rules, checked on the host.

Three things this proves and nothing else.

1. CLASS_NAMES matches orb_asset_class_t in main/assets/orb_assets.h, in order. The
   copy in theme_schema.py exists so a bake can name a class without the firmware's
   header, and a copy of an enum that is allowed to drift is a bug waiting for the
   next entry appended. This lint reads the header, so it drifts loudly instead.
2. A row round-trips pack -> unpack with its negative i16s intact. A theme element at
   x=-10 (a hand's art hanging off the panel's left edge) must read back as -10 and
   not as 4294967286.
3. Every refusal names the row that caused it. "element table invalid" is not an
   error a theme author can act on; "elem[3].order" is.

Run: python3 tools/host_theme_elem_test.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import theme_schema as ts  # noqa: E402

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


# ---------------------------------------------------------------------------
# 1. The class vocabulary mirrors the firmware's enum, in declaration order.
# ---------------------------------------------------------------------------
src = open(os.path.join(ROOT, "main", "assets", "orb_assets.h")).read()
block = re.search(r"typedef enum \{(.*?)\} orb_asset_class_t;", src, re.S)
check(block is not None, "orb_asset_class_t not found in orb_assets.h — the mirror is "
                         "unverified, which is the failure this lint exists to catch")
if block:
    # `= 0` on the first member is why a plain "NAME," regex loses it: ORB_ASSET_TEXT
    # is the one entry with an explicit initializer, and losing the first class would
    # make the mirror look short by exactly the entry that matters most.
    names = [n.lower() for n in
             re.findall(r"ORB_ASSET_([A-Z_]+)\s*(?:=\s*\d+)?\s*,", block.group(1))
             if n != "CLASS"]                       # ORB_ASSET_CLASS_N is a bound, not a class
    check(tuple(names) == ts.CLASS_NAMES,
          "class vocabulary drifted: header %s vs theme_schema %s" % (names, ts.CLASS_NAMES))
abi = re.search(r"#define\s+ORB_ASSET_CLASS_ABI\s+(\d+)", src)
check(abi is not None and int(abi.group(1)) == 2,
      "ORB_ASSET_CLASS_ABI moved off 2; the element table stamps that same dialect")

check(ts.class_id("hand") == ts.CLASS_HAND, "class_id disagrees with the enum order")
check(ts.class_id("Hand") is None, "class_id is case-sensitive on the wire; it took 'Hand'")
check(ts.class_id("nope") is None, "class_id invented a class")
for _n in ts.CLASS_NAMES:
    check(ts.class_id(_n) is not None, "class_id lost %r" % _n)

# ---------------------------------------------------------------------------
# 2. Round trip, including the signed columns.
# ---------------------------------------------------------------------------
def row(**over):
    r = dict(ts.blank_element(), **{"n": 0, "class": ts.CLASS_HAND, "slot": 0,
                                   "flags": ts.EF_OS_ADV, "order": 1, "alpha": 200,
                                   "frame0": 0, "n_frames": 1, "x": 10, "y": 20,
                                   "w": 8, "h": 90, "pivot_x": 4, "pivot_y": 88,
                                   "degrees": 9000, "scale16": 16})
    r.update(over)
    return r


recs = [row(),
        row(n=1, **{"class": ts.CLASS_SPRITE, "x": -10, "y": -1, "w": 100, "h": 40,
                    "pivot_x": 0, "pivot_y": 0, "flags": ts.EF_ALPHA | ts.EF_LOOP}),
        row(n=2, **{"class": ts.CLASS_RING, "slot": 1, "radius": 160, "degrees": 35999,
                    "pivot_x": 0, "pivot_y": 0, "alpha": 255, "flags": 0}),
        row(n=3, **{"class": ts.CLASS_BANK, "slot": 0, "flags": 0, "n_frames": 60,
                    "frame0": 300, "pivot_x": 0, "pivot_y": 0}),
        row(n=4, **{"class": ts.CLASS_PLATE, "slot": 0, "flags": ts.EF_HIDDEN,
                    "x": -32768, "y": 32767, "pivot_x": 0, "pivot_y": 0})]
ts.check_table(recs)
words = ts.pack_elements(recs)
check(len(words) == len(recs) * ts.ELEMENT_WORDS,
      "pack wrote %d words for %d rows of %d" % (len(words), len(recs), ts.ELEMENT_WORDS))
back = ts.unpack_elements(words, len(recs))
check(back == recs, "element table did not round-trip")
check(back[1]["x"] == -10 and back[1]["y"] == -1,
      "negative i16 read back as %s/%s — sign extension is wrong"
      % (back[1]["x"], back[1]["y"]))
check(back[4]["x"] == -32768 and back[4]["y"] == 32767, "i16 extremes lost sign or edge")
check(ts.unpack_elements(words[:ts.ELEMENT_WORDS], 1)[0] == recs[0],
      "a one-row read from a longer buffer must agree with the whole-table read")
# ---------------------------------------------------------------------------
# 3. Refusals, and the row each one names.
# ---------------------------------------------------------------------------
# Each sentinel carries the word its own refusal must contain. Without that third
# field the suite proves "something refused", and three of these passed on a mutant
# because an earlier rule spoke first — a check that cannot tell which rule fired is
# not a check of that rule.
def _n(recs):
    """Distinct identities so only a whole-table rule can refuse the batch."""
    out = []
    for i in range(recs):
        out.append(row(n=i, **{"class": ts.CLASS_SPRITE, "slot": i, "flags": 0,
                               "w": 20, "h": 20, "pivot_x": 0, "pivot_y": 0}))
    return out


SENT = (
    # The first id past the vocabulary, derived: D34 made 8 a real class, and a
    # literal here would have quietly retargeted this vector onto my new AUX rule
    # (which also names .src) and reported a pass for the wrong reason.
    ("unknown class id", lambda: ts.check_element(
        row(**{"class": len(ts.CLASS_NAMES)})), "class"),
    ("class id negative", lambda: ts.check_element(row(**{"class": -1})), "class"),
    ("unknown flag bit", lambda: ts.check_element(row(flags=ts.EF_ALPHA | 0x80)), "flags"),
    ("order past the rungs", lambda: ts.check_element(row(order=ts.ELEM_ORDER_N)), "order"),
    ("order far past", lambda: ts.check_element(row(order=999)), "order"),
    ("zero frames", lambda: ts.check_element(row(n_frames=0)), "n_frames"),
    ("frames past the art bound", lambda: ts.check_element(row(frame0=359, n_frames=2)), "frames"),
    ("degrees past a turn", lambda: ts.check_element(row(degrees=36000)), "degrees"),
    ("OS-advanced bank", lambda: ts.check_element(row(**{"class": ts.CLASS_BANK, "flags": ts.EF_OS_ADV, "n_frames": 4, "frame0": 0, "pivot_x": 0, "pivot_y": 0})), "state"),
    ("OS-advanced text", lambda: ts.check_element(row(**{"class": ts.CLASS_TEXT, "slot": 1, "radius": 0})), "state"),
    ("animated composited layer", lambda: ts.check_element(row(flags=0, **{"class": ts.CLASS_LAYER, "n_frames": 2, "radius": 0, "pivot_x": 0, "pivot_y": 0})), "composited"),
    ("hand pivot off the plate", lambda: ts.check_element(row(pivot_x=99)), "pivot"),
    ("hand pivot negative", lambda: ts.check_element(row(pivot_y=-1)), "pivot"),
    ("radius on a text row", lambda: ts.check_element(row(flags=0, **{"class": ts.CLASS_TEXT, "slot": 1, "radius": 40, "pivot_x": 0, "pivot_y": 0})), "radius"),
    ("sub-1/16 scale", lambda: ts.check_element(row(scale16=8)), "scale16"),
    ("table past the cap", lambda: ts.check_table(_n(ts.ELEM_MAX + 1)), "bound of"),
    ("two rows, one slot", lambda: ts.check_table([row(), row(n=1)]), "slot 0"),
    ("duplicate row identity", lambda: ts.check_table([row(), row(**{"class": ts.CLASS_TEXT, "slot": 7, "flags": 0, "radius": 0})]), "identity"),
    ("alpha past a byte", lambda: ts.pack_elements([row(alpha=256)]), "u8"),
    ("width past a u16", lambda: ts.pack_elements([row(w=65536)]), "u16"),
    ("missing column", lambda: ts.pack_record({k: v for k, v in row().items() if k != "order"},
                                              ts.ELEMENT_FIELDS, "elem[0]"), "order"),
)
for _name, _fn, _must in SENT:
    try:
        _fn()
    except ValueError as _e:
        _m = str(_e)
        check("elem" in _m, "%s: refused, but the message names no row: %s" % (_name, _m))
        check(_must in _m, "%s: refused for some other rule (%s) — the sentinel does "
                          "not isolate the rule it claims" % (_name, _m))
    except Exception as _e:
        failures.append("%s: raised %s, not ValueError" % (_name, type(_e).__name__))
    else:
        failures.append("%s: ACCEPTED, and the dial would have been handed it" % _name)

print("== ELEMENT TABLE SUITE: %d checks, %d failure(s)"
      % (17 + len(SENT) + len(ts.CLASS_NAMES), len(failures)))
for _f in failures:
    print("  FAIL " + _f)
sys.exit(1 if failures else 0)

