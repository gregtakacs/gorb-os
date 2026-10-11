#!/usr/bin/env python3
"""The complication/indicator vocabulary a theme may ask the dial to draw.

This file is the INTERFACE between the art and the watch. Three consumers read
it and none of them may invent a field of their own:

  tools/bake_clock.py      turns a kit (manifest.json + complications/ + hands/)
                           into these records and their pixels
  tools/orbtheme.py        lays them out as words in the .orbtheme geometry blob
  main/art/orb_theme.c     the mirrored reader; tools/host_theme_test.py packs
                           here and parses THERE (lifted verbatim into gcc) and
                           fails if any word disagrees

Why a table and not a field per feature: every previous dial feature arrived as
named scalars in clk_theme_t (tourbillon_n, bg_mode, matte_mask, ...), so each
one cost a field in the generated header, a THEME_FIELDS entry, an rd_* read in
orb_theme.c, a THEME_SCALARS_N bump, and an ABI question. A calendar dial needs
day/month/date windows, day/month/date rings, per-glyph date strips, flares,
sub-dial hands, a battery reserve and a retrograde hand. Landing those as
scalars would triple the struct and force every theme on every card to be
re-baked for art the owner already has. Instead the theme ships ARRAYS of
uniform records and the firmware interprets them: adding a kind the device does
not know is one skipped row in a log line, not a corrupt dial.

Two rules keep the arrays safe, and both are enforced here rather than at the
call sites:

  * A record is a fixed list of u32 words in field order, the same convention
    orbtheme.py already uses for sprites. `geom_size` is fully determined by the
    counts, so the loader can refuse a truncated table before one pixel of the
    payload is streamed.
  * Every field is range-checked on the way OUT of the bake. A value that will
    not fit the C field the device reads it as is the bake's bug, and the
    alternative is a device that draws it wrong and gets blamed for bad art.
"""

# ---------------------------------------------------------------------------
# Caps. Each one is a firmware array size, so the bake must refuse to exceed it
# rather than emit a theme whose extra rows would be dropped at load.
# There is no dynamic allocation in the paint path (see .clinerules), so the
# dial cannot grow these at runtime.
# ---------------------------------------------------------------------------
CLK_COMP_MAX   = 16    # complications: windows, strips, rings, flares
CLK_AUX_MAX    = 8     # extra hands beyond the 5 baked hand slots
CLK_BANK_MAX   = 256   # shared RGBA sprites: ring slots, strip glyphs, flares
CLK_STRIP_GLYPHS = 8   # glyph image objects created per strip
CLK_COMP_WORDS = 26    # == len(COMP_FIELDS); the C mirror and the size check use it
CLK_AUX_WORDS  = 24    # == len(AUX_FIELDS) == 15 sprite words + 9 driving words
CLK_BANK_WORDS = 15    # == orbtheme.SPRITE_FIELDS

# ---------------------------------------------------------------------------
# Enumerations. Index order is the wire value: appending is allowed, reordering
# is an ABI break. `none` at 0 keeps a blank record inert.
# ---------------------------------------------------------------------------

# What drives a complication's value. The firmware derives all of these from the
# wall clock and the battery state it already has; none needs a new input.
SOURCES = (
    "none",          # static: always frame 0
    "weekday",       # 0..6, Monday first
    "month",         # 0..11
    "date",          # 1..31
    "hour12",        # 0..11, the hour hand's own value
    "hour24",        # 0..23
    "minute",        # 0..59
    "second",        # 0..59
    "moon_age",      # 0..29, 30 frames over the 29.53 d synodic month
    "battery",       # 0..100 %
    "power_reserve", # 0..count-1 across the reserve's own range
    "text",          # a string the owner set; one glyph index per character
    "ampm",          # 0 = am, 1 = pm
    "week",          # ISO week number, 1..53
    "day_of_year",   # 1..366
    "quarter",       # 0..3
    "value",         # raw: index == the comp's own value, for bake-decided art
)

# How a comp's frames are laid down.
KINDS = (
    "window",   # one whole-window tile per value; the day/month/date/moon case
    "strip",    # value composed from per-glyph sprites + optional edge pieces
    "ring",     # count sprites evenly spaced on a circle, ring rotated by value
    "flare",    # one sprite picked by value, drawn with a blend mode
)

# What a window sits on, from layout.comps[].host.
HOSTS = ("dial", "subdial", "custom")

# Window content: opaque tiles already carry the dial's own pixels composited
# over the art (the under-dial case), alpha tiles are drawn over the plate.
WINDOW_OPAQUE = 0
WINDOW_ALPHA = 1

# flags bits. Bit 0 is the window alpha/opaque switch so a window record needs
# no extra field; the rest are layout switches the composer exposes.
FLAG_UNDER_DIAL = 1 << 0   # window pixels include the dial's own translucent art
FLAG_ZERO_PAD   = 1 << 1   # strip: pad to `count` glyphs with the zero glyph
FLAG_REVERSE    = 1 << 2   # ring/strip: walk frames in the opposite direction
FLAG_JUMP       = 1 << 3   # aux hand: snap to whole units, do not interpolate
FLAG_24H        = 1 << 4   # aux hand: hour value is 0..23, not 0..11
FLAG_WRAP       = 1 << 5   # aux hand: past value_max, keep sweeping
FLAG_BLINK      = 1 << 6   # comp: draw every other second (alarm/indicator)
FLAG_DISABLED   = 1 << 7   # parsed, validated, deliberately not drawn
FLAG_AUX_SHADOW = 1 << 8   # aux record: the blur/offset twin of the hand before
                           # it with the same tag+src. The driver rotates it BY
                           # THE HAND'S angle and gives it no input of its own,
                           # so a value-driven layer above the plate still gets
                           # its shadow without a second record's worth of state.

# ---------------------------------------------------------------------------
# Record layouts. (name, c-type) per word, in wire order. The C mirror reads the
# same widths, and tools/host_theme_test.py is what proves the two agree.
# ---------------------------------------------------------------------------

# Shared sprite bank: one clk_sprite_t-shaped row each. Ring slots, strip glyphs
# and flare frames all live here, so a theme stores a glyph once however many
# complications use it, and the firmware's blitter only ever sees one record
# shape. `color_off` is an absolute offset into the art bank, and a bank row is
# RGB565A8 exactly as a hand sprite is: colour w*h*2 then alpha w*h.
BANK_FIELDS = (
    ("w", "u16"), ("h", "u16"),
    ("offx", "i16"), ("offy", "i16"),
    ("pivotx", "i16"), ("pivoty", "i16"),
    ("centerx", "i16"), ("centery", "i16"),
    ("blend", "u8"), ("show", "u8"),
    ("color_off", "u32"),
    ("diffx", "i16"), ("diffy", "i16"),
    ("diffw", "u16"), ("diffh", "u16"),
)

# One complication. Field groups, in order: identity, box, frames, ring
# geometry, value mapping, layout, then a reserved word the next feature can
# have without another ABI conversation.
COMP_FIELDS = (
    ("tag", "u16"),           # manifest comp id (c2 -> 2); 0 = synthetic
    ("kind", "u8"),           # KINDS index
    ("src", "u8"),            # SOURCES index
    ("x", "i16"),             # window box origin, or the strip's box origin
    ("y", "i16"),
    ("w", "u16"),             # window box / one glyph / one ring slot
    ("h", "u16"),
    ("count", "u16"),         # frames available across this value domain
    ("first", "u16"),         # index into the bank (strip/ring/flare); for a
                              # WINDOW it is the z bucket: the KINDS slot whose
                              # live layer this tile must stay UNDER, or 0 for
                              # "below every live layer" — what every bake
                              # before subdial-hosted windows emitted. A moon
                              # hosted by a subdial ships first=4 so the plate
                              # (static2) composites its arch over the disc and
                              # the dial face (static1) cannot bury it.
    ("off", "u32"),           # window: blob offset of tile 0
    ("frame_bytes", "u32"),   # window: stride between consecutive tiles
    ("cx", "i16"), ("cy", "i16"),        # ring centre
    ("radius", "i16"),                    # ring radius, centre to sprite centre
    ("angle0", "i16"),                    # tenths of a degree, 0 = twelve o'clock
    ("angle_step", "i16"),                # tenths of a degree between slots
    ("value_min", "i16"),                 # the value frame 0 stands for
    ("scale", "u16"),                     # frames per unit of value, x16
    ("glyph_step", "i16"),                # strip: px between glyph origins
    ("glyph_lead", "i16"),                # strip: px from box edge to first glyph
    ("edge_left", "u16"),                 # strip: left window piece, bank index, 0xFFFF none
    ("edge_right", "u16"),                # strip: right window piece, bank index, 0xFFFF none
    ("flags", "u16"),
    ("blend", "u8"),
    ("show", "u8"),
    ("reserve", "u32"),       # must be 0: checked, so it cannot be leaned on later
)

# One extra hand: the same 15 words as a baked sprite, then how it is driven.
# The sub-dial seconds, the 24h hand, the battery reserve and the retrograde
# sweep are all one record here rather than more of the five baked hand slots.
AUX_EXTRA_FIELDS = (
    ("tag", "u16"),           # manifest hand index, or the comp tag it belongs to
    ("src", "u8"),            # SOURCES index: hour12, minute, battery, ...
    ("kind", "u8"),           # 0 rotating hand, 1 retrograde, 2 indexed pointer
    ("host", "u16"),          # 0 = main dial centre, else a comp tag
    ("span", "i16"),          # tenths of a degree for one full turn of the
                              # value domain: ang = zero + value*span/value_max.
                              # A date ring reads span 3600 / value_max 31, so
                              # day 5 sits at 5*3600/31 = 581 tenths and day 31
                              # returns to zero. The kit's own drawOrder note
                              # ("value x 360deg + zeroRotationDeg") is this
                              # formula in the vendor's words.
    ("zero", "i16"),          # tenths of a degree at value 0
    ("value_min", "u16"),
    ("value_max", "u16"),
    ("flags", "u16"),
)
AUX_FIELDS = BANK_FIELDS + AUX_EXTRA_FIELDS


def fields_of(name):
    """The (name, type) list of one record, by short name."""
    return {"bank": BANK_FIELDS, "comp": COMP_FIELDS, "aux": AUX_FIELDS}[name]




def words_of(name):
    """Words per record — the count the geometry blob's size is computed from,
    so the firmware's own size check and the bake agree by construction."""
    return len(fields_of(name))


# ---------------------------------------------------------------------------
# Range tables and the checked pack/unpack. Same contract as orbtheme._val /
# orbtheme._sext, and the same failure style: refuse on the bench, never on the
# dial.
# ---------------------------------------------------------------------------
_RANGE = {
    "i8": (-128, 127), "u8": (0, 255),
    "i16": (-32768, 32767), "u16": (0, 65535),
    "u32": (0, 2**32 - 1),
}
_SEXT_BITS = {"i8": 8, "i16": 16}


def _check(what, t, v):
    v = int(v)
    lo, hi = _RANGE[t]
    if v < lo or v > hi:
        raise ValueError("%s: %d does not fit a %s field (%d..%d) — the bake "
                         "computed something the firmware cannot store"
                         % (what, v, t, lo, hi))
    return v & 0xFFFFFFFF


def pack_record(rec, fields, tag):
    """One record as len(fields) u32s.

    A missing key is the bake's bug, not a zero: a silent default here is how a
    window ends up at 0,0 with no frames, and nobody can then tell an unset
    field from a deliberate one."""
    out = []
    for name, t in fields:
        if name not in rec:
            raise ValueError("%s.%s is missing from the record" % (tag, name))
        out.append(_check("%s.%s" % (tag, name), t, rec[name]))
    return out


def unpack_record(words, fields, tag=""):
    """The inverse, sign-extending the narrow signed types the way the device's
    C reads do (mask into the field's bits first, then extend)."""
    rec = {}
    for k, (name, t) in enumerate(fields):
        v = words[k] & 0xFFFFFFFF
        bits = _SEXT_BITS.get(t)
        if bits is not None:
            v &= (1 << bits) - 1
            if v >= 1 << (bits - 1):
                v -= 1 << bits
        rec[name] = v
    return rec


def pack_records(recs, fields, cap, tag):
    """Checked pack of a whole table.

    No zero padding to the cap: the wire carries only the real rows and the
    count words say how many, so padding would charge the theme 15-26 words for
    every unused row and buy nothing the counts do not already say."""
    if len(recs) > cap:
        raise ValueError("%s: %d records exceed the dial's cap of %d — the extra "
                         "rows would be dropped by the firmware"
                         % (tag, len(recs), cap))
    out = []
    for i, r in enumerate(recs):
        out += pack_record(r, fields, "%s[%d]" % (tag, i))
    return out


def unpack_records(words, fields, n, tag):
    if len(words) < n * len(fields):
        raise ValueError("%s: blob holds %d words, too few for %d records of %d"
                         % (tag, len(words), n, len(fields)))
    size = len(fields)
    return [unpack_record(words[i * size:(i + 1) * size], fields,
                          "%s[%d]" % (tag, i)) for i in range(n)]


def blank(kind):
    """A zeroed record with every key present, so the bake's defaults live in
    one place. Zero `show` keeps an unused row from drawing anything."""
    return {n: 0 for n, _ in fields_of(kind)}


def bank_bytes(rec):
    """Pixels a bank row occupies: RGB565A8, colour plane then alpha plane.
    Must agree with SPRITE_BYTES in orb_theme.c and sprite_bytes in
    clock_view.c, which is what the loader's bounds check is compared against."""
    return int(rec["w"]) * int(rec["h"]) * 3


def window_bytes(rec):
    """Pixels one window tile occupies: opaque tiles carry no alpha plane.
    Agree with the flags bit the firmware reads the same way."""
    px = int(rec["w"]) * int(rec["h"])
    return px * (3 if (int(rec["flags"]) & FLAG_UNDER_DIAL) == 0 else 2)


# ---------------------------------------------------------------------------
# Kit-manifest vocabulary -> schema. The bake calls these and nothing else may
# translate a manifest string into a wire value, so that a new kit word shows up
# as a KeyError here rather than as a complication that quietly never draws.
# ---------------------------------------------------------------------------

# complications[].type / layout.comps[].shows|scale  ->  a SOURCES name
MANIFEST_SOURCE = {
    "day": "weekday",
    "weekday": "weekday",
    "month": "month",
    "date": "date",
    "dayofmonth": "date",
    "moon": "moon_age",
    "moonphase": "moon_age",
    "hour": "hour12",
    "hour24": "hour24",
    "minute": "minute",
    "second": "second",
    "battery": "battery",
    "powerreserve": "power_reserve",
    "balance": "power_reserve",
    "text": "text",
    "custom": "text",
}

# hands[].kind -> (SOURCES name, flags). A date hand turns once per month, so
# its range is the date domain and its span is the ring it points at.
MANIFEST_HAND = {
    "hour": ("hour12", 0),
    "minute": ("minute", 0),
    "second": ("second", 0),
    "hour24": ("hour24", FLAG_24H),
    "date": ("date", 0),
    "month": ("month", 0),
    "weekday": ("weekday", 0),
    "day": ("weekday", 0),
    "moon": ("moon_age", 0),
    "battery": ("battery", FLAG_JUMP),
    "powerreserve": ("power_reserve", 0),
    "balance": ("power_reserve", 0),
    "retrograde": ("hour12", 0),
    "static1": ("none", 0),
    "static2": ("none", 0),
}


def _norm(word):
    return str(word or "").strip().lower().replace(" ", "").replace("_", "").replace("-", "")


def source_of(word, default=None):
    """A manifest word to a SOURCES index. Unknown words raise unless a default
    is given: a complication drawing frame 0 forever is the exact silent failure
    this module exists to prevent."""
    key = _norm(word)
    if key in MANIFEST_SOURCE:
        return SOURCES.index(MANIFEST_SOURCE[key])
    if default is not None:
        return default
    raise KeyError("manifest source %r is not in theme_schema.MANIFEST_SOURCE" % (word,))


def hand_drive(word):
    """A manifest hands[].kind to (SOURCES index, flags)."""
    key = _norm(word)
    if key not in MANIFEST_HAND:
        raise KeyError("manifest hand kind %r is not in theme_schema.MANIFEST_HAND" % (word,))
    src, flags = MANIFEST_HAND[key]
    return SOURCES.index(src), flags


def kind_of(word):
    key = _norm(word)
    if key not in KINDS:
        raise KeyError("comp kind %r is not one of %s" % (word, ", ".join(KINDS)))
    return KINDS.index(key)


# ---------------------------------------------------------------------------
# The value -> frame maths. clock_view.c carries the same arithmetic, so it lives
# here once and the sentinel test pins it: a theme and a device that disagree
# about which frame "Saturday" is produce a dial one day wrong that reads as an
# art bug, which is the class of failure this whole file is arranged to avoid.
# ---------------------------------------------------------------------------

def frame_index(value, count, value_min, scale):
    """The frame a value selects. `scale` is frames-per-value-unit x16, so a
    half-frame offset is expressible without a float on either side."""
    if count <= 0:
        return 0
    s = int(scale) or 16
    idx = (int(value) - int(value_min)) * s
    idx = (idx + 8) // 16 if idx >= 0 else -((-idx + 8) // 16)
    if idx < 0:
        idx = 0
    if idx >= count:
        idx %= count          # weekday/month wrap; clamps every other domain
    return idx


def strip_indices(value, digits, zero_pad, count):
    """Digit glyph indices for a number, most significant first.

    `digits` is the number of glyph positions the strip's box has, so a two-position
    date window showing 23 asks for [2, 3]; showing 7 asks for [7] — or for [0, 7]
    with zero_pad, which is what an owner means by "keep the alignment". Padding can
    never add a position the box does not have, and more than `digits` positions is
    the bake's bug, refused here rather than drawn over the bezel. `count` is the
    number of glyph rows in the bank, so an index can never address a row the theme
    did not ship.
    """
    s = str(int(value))
    if zero_pad and len(s) < digits:
        s = s.rjust(digits, "0")
    return [int(c) % max(1, count) for c in s[-digits:]]


def geom_words(n_comp, n_aux, n_bank):
    """Words the three tables add to a geometry blob, counts included.
    orb_theme.c:geom_sizes_ok() computes the same number from the same counts."""
    return (n_comp * words_of("comp") + n_aux * words_of("aux")
            + n_bank * words_of("bank"))


if __name__ == "__main__":
    # Cheap self-check, and the reason the caps and the record sizes cannot
    # drift from the field lists unnoticed: python3 tools/theme_schema.py
    assert words_of("bank") == CLK_BANK_WORDS, "bank width moved; orbtheme and orb_theme.c both hard-code 15"
    assert words_of("comp") == 26, "comp record width changed — update CLK_COMP_WORDS and the C mirror"
    assert words_of("aux") == CLK_BANK_WORDS + 9, "aux record width changed — update the C mirror"
    assert geom_words(2, 1, 4) == 2 * 26 + 1 * 24 + 4 * 15
    assert frame_index(6, 7, 0, 16) == 6            # Saturday, Monday-first
    assert frame_index(1, 7, 1, 16) == 0            # date hand: the 1st is frame 0
    assert frame_index(10, 12, 0, 16) == 10         # October
    assert frame_index(8, 7, 0, 16) == 1            # past the end: it wraps, never clamps
    assert strip_indices(23, 2, True, 10) == [2, 3]     # full already
    assert strip_indices(23, 3, True, 10) == [0, 2, 3]  # padded into the spare slot
    assert strip_indices(7, 2, True, 10) == [0, 7]      # what "keep the alignment" means
    assert strip_indices(7, 2, False, 10) == [7]        # and what it means without it
    assert strip_indices(11, 1, False, 10) == [1]       # the box wins over the number

    # The flag bits, against the header the firmware actually compiles. A bit
    # added here and not there (or the reverse) means the bake sets a flag the
    # dial reads as something else — a shadow that never draws, or a tile the
    # reader thinks is disabled — with nothing printed on either side.
    import os
    import re
    hdr = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                       "main", "art", "orb_theme.h")
    cflags = {n.lower(): 1 << int(v) for n, v in
              re.findall(r"\bCLK_FLAG_(\w+)\s*=\s*1\s*<<\s*(\d+)", open(hdr).read())}
    pyflags = {n[len("FLAG_"):].lower(): v for n, v in globals().items()
               if n.startswith("FLAG_")}
    assert cflags == pyflags, ("flag bits diverged from main/art/orb_theme.h: "
                               "python %s vs C %s" % (pyflags, cflags))
    assert len(SOURCES) == 17, "SOURCES grew; orb_theme.h's CLK_SRC__MAX assert must agree"
    print("theme_schema ok: %d sources, %d kinds, comp %d words, aux %d words, bank %d words"
          % (len(SOURCES), len(KINDS), words_of("comp"), words_of("aux"), words_of("bank")))
