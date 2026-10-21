#!/usr/bin/env python3
"""The complication/indicator vocabulary a theme may ask the dial to draw.

This file is the INTERFACE between the art and the watch. Three consumers read
it and none of them may invent a field of their own:

  tools/bake_clock.py      turns a kit (manifest.json + complications/ + hands/)
                           into these records and their pixels
  tools/orbtheme.py        lays them out as words in the .orbtheme geometry blob
  main/art/orb_theme.c     the mirrored reader; tools/host_comp_math_test.py
                           packs here and parses THERE (lifted verbatim into
                           gcc) and fails if any word disagrees

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
    "quarter",       # 1..4 — tm_mon/3+1; the firmware's comp_value is the
                     # authority (docs/behaviour-vocabulary.md G2; the
                     # earlier "0..3" comment was wrong, no bake reached it)
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
# same widths, and tools/host_comp_math_test.py is what proves the two agree.
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




# ===========================================================================
# The element table (D32). What a theme declares about ONE app.
# ===========================================================================
#
# Everything above this line is FORMAT 2's complication vocabulary: it describes a
# window the CLOCK draws, in a dialect no other app speaks. The element table is the
# other direction. A theme names the elements it wants present on an app's surface,
# in the order it wants them painted, and the app says how each one it was given is
# drawn. One schema, every app, no dialect (D32, R1).
#
# Identity is (class, slot), not a string. The app's manifest asset table is the
# OFFER and declares slots by class and ordinal (D23 as amended); a theme row names
# the slot it fills, so the check in both directions is an integer compare and the
# row stays fixed-width. Two rows claiming one slot is the duplicate check_table
# refuses.
#
# Fixed width means every column is one u32 on the wire, so a later schema appends
# columns past SCALE16 and a reader that knows fewer reads the ones it has — the
# same append-only rule the class enum follows, and the reason adding a column does
# not cost a format bump under D24's no-backward-path rule.
ELEMENT_FIELDS = (
    ("n",        "u16"),   # row identity within this app's table; unique
    ("class",    "u8"),    # the frozen orb_asset_class_t id, 0..CLASS_N-1
    ("flags",    "u8"),    # EF_* below; unknown bits are a refusal, not a zero
    ("order",    "u8"),    # depth: lower paints first. < ELEM_ORDER_N
    ("alpha",    "u8"),    # 0..255, read only when EF_ALPHA is set
    ("slot",     "u16"),   # which declaration of this class this row fulfils
    ("frame0",   "u16"),   # index of this element's first art slot
    ("n_frames", "u16"),   # 1..ELEM_FRAMES_MAX; an animation is n_frames > 1
    ("x",        "i16"),   # top-left, in app-local coordinates
    ("y",        "i16"),
    ("w",        "u16"),   # 0 = take the class's own default extent
    ("h",        "u16"),
    ("pivot_x",  "i16"),   # rotation origin, relative to (x, y)
    ("pivot_y",  "i16"),
    ("radius",   "u16"),   # RING: the circle its plates sit on
    ("degrees",  "u16"),   # fixed-point 100ths, 0..35999 (the dial's own rule)
    ("scale16",  "u16"),   # fixed-point x16, the comp maths' own scale
    # D32 ruling A, appended exactly as the header comment above promises. Both
    # belong to a BANK row and must be zero on every other class — check_element
    # refuses a non-zero one rather than let a row mean two things. `kind` is
    # this file's KINDS id (window/strip/ring/flare): how the dial draws the
    # row's plates. `src` is this file's SOURCES id: what value indexes them.
    # A row read from a table narrower than these two columns carries zero for
    # each, which is window/NONE — a plate that never changes — so the default
    # is an element every app may ignore, never a drawn value.
    ("kind",     "u8"),    # BANK: KINDS id, how the plates are drawn.
                           # AUX: MOTION_NAMES id, how the pointer moves.
    ("src",      "u8"),    # BANK and AUX: SOURCES id, what value drives the row.
    # D34's four drive columns, for an AUX row and for no other class. `kind` and `src`
    # already ride the row and already answer the same two questions for it — HOW it
    # moves (its MOTION_NAMES id, read straight as clk_aux_t.kind) and WHAT moves it
    # (its SOURCES id) — so they carry no new meaning here; these four finish the
    # sentence with how far, from where, across what domain. Every one is written in the
    # unit the motion names (MOTION_UNIT): tenths of a degree for an angle, pixels for a
    # slide. A row read from a table narrower than these carries zero for each, which
    # check_element refuses on an AUX row rather than draw as a pointer that never
    # leaves home — the same "refuse, never guess" the class gate itself runs by.
    ("span",     "i16"),   # AUX: travel across the whole value domain, in motion's unit
    ("zero",     "i16"),   # AUX: where the pointer sits at value_min, same unit
    ("vmin",     "u16"),   # AUX: the value `zero` stands for
    ("vmax",     "u16"),   # AUX: the domain `span` divides. 0 is a refusal, never a /0
)
ELEMENT_WORDS = len(ELEMENT_FIELDS)          # 23 words, 92 bytes per row
# The narrowest row the FORMAT 3 dialect has ever written, before D32's ruling A
# appended kind and src. A blob's element-row stride is derived from its own
# length (§23.5), and a stride below this floor is not a shorter row of this
# dialect but a torn file; the reader refuses it rather than read a width that
# cannot be the current schema's. orb_bundle.c's ORB_BUNDLE_ELEM_WORDS_MIN agrees.
ELEMENT_WORDS_MIN = 17

# Flag bits, kept in the low nibble so a high bit means "a schema this build does
# not know", which check_element refuses rather than painting wrongly.
EF_ALPHA  = 1 << 0   # draw with `alpha`, not opaquely
EF_LOOP   = 1 << 1   # animation wraps; clear means clamp at the last frame
EF_OS_ADV = 1 << 2   # the OS advances frames at the picked rate (R6); else the app
EF_HIDDEN = 1 << 3   # declared and bound, not drawn — a theme's own off switch
EF_AUX_SHADOW = 1 << 4  # aux row: the shadow TWIN of the row directly before it.
                        # The u8 element column cannot carry clk_types.h's 1<<8, so
                        # the flag lives here as bit 4; orb_clock3.c's AUX_F_SHADOW
                        # maps it onto CLK_FLAG_AUX_SHADOW, and class aux is the only
                        # place it means anything — a hand's shadow is its own class.
EF_ALL = EF_ALPHA | EF_LOOP | EF_OS_ADV | EF_HIDDEN | EF_AUX_SHADOW

ELEM_ORDER_N    = 64   # depth rungs; the clock's hand order is 0..4 today

# ---- the clock's slot vocabulary (D32's join, ruled by the app that declares) ----
# A theme's element row carries (class, slot) and never a name: slot is "which
# declaration of this class this row fulfils" (ELEMENT_FIELDS). Names therefore
# belong to the APP, and this table is the one place the clock's names, classes and
# slots are written down. Three readers depend on it and none may paraphrase it:
# `main/clock/clock_slots.c` mirrors it for the compiled app,
# `tools/watch-composer/orbtheme-baker.js` mirrors it for the bake that fills the
# slots, and `tools/host_orbelems_test.py` fails the build if a word drifts.
#
# A name is a manifest reference's `<name>` half, so it obeys the wire's own bound:
# [a-z0-9_], 1..ORB_ASSET_NAME_MAX-1 == 11 characters. "counterweight" is 13, which
# is why the row is named "counterw" and not more honestly — the field is 12 bytes
# with the NUL and nothing about the layout is negotiable at this end.
#
# What a BANK row means was this table's one deliberate hole until D32's ruling A
# closed it: an element row now carries `kind` and `src`, so a theme says both what
# draws in a complication slot and what value indexes its plates. What stays the
# app's own rule is the FRAME-TO-VALUE mapping — CLOCK_WINDOW_DOMAIN below. The
# domain of a source is a fact about the clock's calendar, not about the theme, and
# §25.6's refusal to trust a carried figure for it stands: a month window is twelve
# plates because a year has twelve months, not because a row said so.
CLOCK_SLOTS = (
    # name           class     slot   what the dial draws with it
    ("bg",            "plate",  0,    "the one static background, full panel"),
    ("glass",         "plate",  1,    "the crystal, over everything"),
    ("hour",          "hand",   0,    "the hour hand"),
    ("minute",        "hand",   1,    "the minute hand"),
    ("second",        "hand",   2,    "the seconds hand, or the carriage it rides"),
    ("dial",          "hand",   3,    "a rotating dial disc"),
    ("counterw",      "hand",   4,    "the counterweight on that disc"),
    ("carriage",      "hand",   5,    "an animated tourbillon: n_frames frames, OS-advanced"),
    ("sh_hour",       "sprite", 0,    "the hour hand's cast shadow"),
    ("sh_minute",     "sprite", 1,    "the minute hand's cast shadow"),
    ("sh_second",     "sprite", 2,    "the seconds hand's cast shadow"),
    ("sh_dial",       "sprite", 3,    "the rotating disc's cast shadow"),
    ("sh_counterw",   "sprite", 4,    "the counterweight's cast shadow"),
    ("comp0",         "bank",   0,    "complication 0: one plate per value"),
    ("comp1",         "bank",   1,    "complication 1: one plate per value"),
    ("comp2",         "bank",   2,    "complication 2: one plate per value"),
    ("comp3",         "bank",   3,    "complication 3: one plate per value"),
    ("comp4",         "bank",   4,    "complication 4: one plate per value"),
    ("comp5",         "bank",   5,    "complication 5: one plate per value"),
    ("comp6",         "bank",   6,    "complication 6: one plate per value"),
    ("comp7",         "bank",   7,    "complication 7: one plate per value"),
    ("comp8",         "bank",   8,    "complication 8: one plate per value"),
    ("comp9",         "bank",   9,    "complication 9: one plate per value"),
    ("comp10",        "bank",   10,   "complication 10: one plate per value"),
    ("comp11",        "bank",   11,   "complication 11: one plate per value"),
    ("comp12",        "bank",   12,   "complication 12: one plate per value"),
    ("comp13",        "bank",   13,   "complication 13: one plate per value"),
    ("comp14",        "bank",   14,   "complication 14: one plate per value"),
    ("comp15",        "bank",   15,   "complication 15: one plate per value"),
    # D34: the pointer slots. Identity is still (class, slot), so an AUX row claims one
    # of these eight by ordinal and the clock keeps them in CLK_AUX_MAX's own order.
    # Names are `ptr0..ptr7` and NOT `subdial_seconds` or `reserve`: the clock does not
    # know what a pointer indicates, only that one row turns it, and a name that claimed
    # the role would be a second source of truth about the art.
    ("ptr0",          "aux",    0,    "pointer 0: one plate, moved by the row's own drive"),
    ("ptr1",          "aux",    1,    "pointer 1: one plate, moved by the row's own drive"),
    ("ptr2",          "aux",    2,    "pointer 2: one plate, moved by the row's own drive"),
    ("ptr3",          "aux",    3,    "pointer 3: one plate, moved by the row's own drive"),
    ("ptr4",          "aux",    4,    "pointer 4: one plate, moved by the row's own drive"),
    ("ptr5",          "aux",    5,    "pointer 5: one plate, moved by the row's own drive"),
    ("ptr6",          "aux",    6,    "pointer 6: one plate, moved by the row's own drive"),
    ("ptr7",          "aux",    7,    "pointer 7: one plate, moved by the row's own drive"),
)

# The clock's own frame-to-value domain for a WINDOW complication (D32 ruling B):
# source name -> (value_min, plate count). Only fixed calendar domains appear; a
# source outside this table indexes by its own range, and the row's n_frames is
# checked against what the bake packed rather than against a figure the table
# would invent. orb_clock3.c mirrors this table for the device's re-check of a
# hostile bundle, and the suite pins the mirror — the same discipline as
# clock_slot_declares().
CLOCK_WINDOW_DOMAIN = {
    "weekday":     (0, 7),    "month":    (0, 12),   "date":     (1, 31),
    "hour12":      (0, 12),   "hour24":   (0, 24),   "minute":   (0, 60),
    "second":      (0, 60),   "moon_age": (0, 30),   "ampm":     (0, 2),
    "week":        (1, 53),   "day_of_year": (1, 366), "quarter": (1, 4),
}

# The slots the shipped dial draws, and the only ones a bundle may fill and boot.
# Ruling D32-E put every class the vocabulary names into scope; the dial draws all
# of them now — plate, hands including the carriage, shadows, and the sixteen
# complication slots — so the refusal a row can still meet is the projection's own
# (a class this app's manifest does not offer, a plate whose art the bundle does
# not hold), and ruling D32-F makes any of those refusals fall the whole flip to
# the dial's digits rather than draw a partial theme.
CLOCK_SLOTS_DRAWN = ("bg", "glass", "hour", "minute", "second", "dial", "counterw",
                     "carriage", "sh_hour", "sh_minute", "sh_second", "sh_dial",
                     "sh_counterw",
                     "comp0", "comp1", "comp2", "comp3", "comp4", "comp5", "comp6",
                     "comp7", "comp8", "comp9", "comp10", "comp11", "comp12",
                     "comp13", "comp14", "comp15",
                     "ptr0", "ptr1", "ptr2", "ptr3", "ptr4", "ptr5", "ptr6", "ptr7")


def clock_slot(name):
    """The (class, slot) a clock slot name denotes, or None."""
    for n, cls, slot, _why in CLOCK_SLOTS:
        if n == name:
            return cls, slot
    return None


def clock_slot_name(cls, slot):
    """The name a (class, slot) pair answers to, or None if the clock declares no
    such slot — which is the refusal a reader needs, not a silent zero."""
    for n, c, s, _why in CLOCK_SLOTS:
        if c == cls and s == slot:
            return n
    return None

ELEM_MAX        = 32   # rows one app's table may carry
ELEM_FRAMES_MAX = 360  # bound on frame0+n_frames (av-kit's busiest bank uses 208)

# Mirrors orb_asset_class_t in main/assets/orb_assets.h, in that enum's order. A
# lint reads the header and compares, because a hand-maintained copy of an enum is
# exactly the drift this file's whole purpose is to remove.
# AUX is D34's append: a theme-named pointer — a sub-dial hand, a retrograde sweep, a
# sliding reserve bar — that moves by a source value about an origin the row itself
# places. It enters as a class, not as a wider use of HAND's slot, because the clock
# declares six hand slots by ordinal and their rotation is the compiled dial's own
# hour/minute/second mapping; an AUX row names its own drive and its own motion, and
# a reader that has never heard of class 8 refuses it whole (D26) rather than let a
# pointer that should be turning sit still.
CLASS_NAMES = ("text", "sprite", "hand", "plate", "ring", "bank", "layer", "status", "aux")
CLASS_TEXT, CLASS_SPRITE, CLASS_HAND, CLASS_PLATE = 0, 1, 2, 3
CLASS_RING, CLASS_BANK, CLASS_LAYER, CLASS_STATUS = 4, 5, 6, 7
CLASS_AUX = 8

# How an AUX row moves, in the order clk_aux_t.kind reads it. The first three are the
# vocabulary clk_types.h has always carried; the two slide kinds are D34's append and
# the first thing a theme can ask for that is NOT an angle.
MOTION_NAMES = ("rotate", "retrograde", "indexed", "slide-x", "slide-y")
MOTION_ROTATE, MOTION_RETROGRADE, MOTION_INDEXED, MOTION_SLIDE_X, MOTION_SLIDE_Y = 0, 1, 2, 3, 4
# Which unit `span` and `zero` are written in, per motion. A theme that names
# slide-x and a span of 3600 means 3600 PIXELS, not a turn — the motion decides the
# unit, so no extra column has to be trusted to say what the number one means.
MOTION_UNIT = {"rotate": "tenths of a degree", "retrograde": "tenths of a degree",
               "indexed": "tenths of a degree", "slide-x": "px", "slide-y": "px"}
MOTION_SLIDES = (MOTION_SLIDE_X, MOTION_SLIDE_Y)

# Classes whose frame is app state, not a clock (R6). A BANK is indexed "one plate
# per value", so OS advancement would march a date wheel on a tick nobody asked for.
CLASS_APP_ADV = (CLASS_BANK, CLASS_TEXT, CLASS_STATUS)
# Classes the OS composites rather than an app paints: a row claiming several frames
# of a composited slot is a mistake about what the class is.
CLASS_NO_ANIM = (CLASS_LAYER, CLASS_STATUS)


def class_id(name):
    """The frozen class id a name denotes, or None. Same spellings the manifest's
    asset table uses, so one vocabulary serves both sides of the offer check."""
    return CLASS_NAMES.index(name) if name in CLASS_NAMES else None



def check_element(rec, where="elem"):
    """Every rule the field ranges cannot express. Raises ValueError naming `where`.

    The point of a legible table is that the OS can read it without trusting whoever
    drew it, so each rule here is a thing a reader would otherwise get wrong quietly:
    an unknown class id, an order past the depth rungs, a frame window running off the
    end of the art, an animation on a class that cannot be one.
    """
    cls, flags = int(rec["class"]), int(rec["flags"])
    if not 0 <= cls < len(CLASS_NAMES):
        raise ValueError("%s.class: %d is not a class this build knows (0..%d)"
                         % (where, cls, len(CLASS_NAMES) - 1))
    if flags & ~EF_ALL:
        raise ValueError("%s.flags: unknown flag bits 0x%02x — a schema this build "
                         "does not know, refused rather than drawn wrong"
                         % (where, flags & ~EF_ALL))
    if flags & EF_AUX_SHADOW and cls != CLASS_AUX:
        raise ValueError("%s.flags: the pointer shadow twin is a rule for class aux; "
                         "class %s already owns its shadow's placement (§25.2)"
                         % (where, CLASS_NAMES[cls]))
    if not 0 <= int(rec["order"]) < ELEM_ORDER_N:
        raise ValueError("%s.order: %d is past the dial's %d depth rungs"
                         % (where, int(rec["order"]), ELEM_ORDER_N))
    if int(rec["n_frames"]) < 1:
        raise ValueError("%s.n_frames: 0 draws nothing, and a declared element that "
                         "draws nothing is a row nobody meant to write" % where)
    if int(rec["frame0"]) + int(rec["n_frames"]) > ELEM_FRAMES_MAX:
        raise ValueError("%s: frames %d..%d run past the art region's bound of %d"
                         % (where, int(rec["frame0"]),
                            int(rec["frame0"]) + int(rec["n_frames"]) - 1,
                            ELEM_FRAMES_MAX))
    if int(rec["degrees"]) >= 36000:
        raise ValueError("%s.degrees: %d is not a direction (0..35999, 100ths)"
                         % (where, int(rec["degrees"])))
    # R6: a frame the OS advances must belong to a class that can be one.
    if flags & EF_OS_ADV and cls in CLASS_APP_ADV:
        raise ValueError("%s: class %r holds app state, so the OS must not advance its "
                         "frames — clear the OS_ADV flag and let the app pick"
                         % (where, CLASS_NAMES[cls]))
    if cls in CLASS_NO_ANIM and int(rec["n_frames"]) != 1:
        raise ValueError("%s: class %r is composited, not animated — it carries one "
                         "frame, not %d" % (where, CLASS_NAMES[cls], int(rec["n_frames"])))
    # A hand turns about a point inside itself — the same arithmetic orbapp.py demands
    # of the app's offer, so the two directions cannot disagree about one hand.
    if cls == CLASS_HAND and int(rec["w"]) and int(rec["h"]):
        px, py = int(rec["pivot_x"]), int(rec["pivot_y"])
        w, h = int(rec["w"]), int(rec["h"])
        if not (0 <= px < w and 0 <= py < h):
            raise ValueError("%s: a HAND's pivot (%d,%d) sits outside its own %dx%d "
                             "plate, so it would turn about a point off the glass"
                             % (where, px, py, w, h))
    if cls in (CLASS_TEXT, CLASS_STATUS) and int(rec["radius"]):
        raise ValueError("%s: class %r places glyphs and has no radius to give it; a "
                         "non-zero one means a row written for another class"
                         % (where, CLASS_NAMES[cls]))
    if int(rec["scale16"]) and int(rec["scale16"]) < 16:
        raise ValueError("%s.scale16: %d scales the element below one sixteenth of its "
                         "art, which no bake produces by intent" % (where, int(rec["scale16"])))
    # ---- D32 ruling A: the two complication columns ----
    # They belong to a BANK row and to an AUX row, and to nothing else. A non-zero one
    # on any other class is a row written for a driven element that names a plain slot —
    # the same lie the radius-on-a-text-row rule refuses, and refused in the same shape.
    # D34 widened the pair's scope rather than appending look-alikes: on a BANK row
    # `kind` is a KINDS id and on an AUX row it is a MOTION_NAMES id, and the class
    # decides which of the two vocabularies the byte is read in, exactly as the class
    # already decides whether `degrees` is a face or a carriage's start.
    kind, src = int(rec["kind"]), int(rec["src"])
    if kind and cls not in (CLASS_BANK, CLASS_AUX):
        raise ValueError("%s.kind: %d on a %s row — how a row draws or moves is a "
                         "driven element's question, and this class answers it with "
                         "its slot" % (where, kind, CLASS_NAMES[cls]))
    if src and cls not in (CLASS_BANK, CLASS_AUX):
        raise ValueError("%s.src: %d on a %s row — what drives a row is a driven "
                         "element's question, and this class answers it with "
                         "its slot" % (where, src, CLASS_NAMES[cls]))
    # ---- D34: the pointer row, and the four numbers that make it move ----
    # An AUX row is a sprite with a drive. The checks below are the ones that keep a
    # mis-declared pointer from shipping as a hand that sits at 12 forever and reads as
    # a watch that is simply wrong: the motion must exist, the source must be one the
    # dial can derive, the domain must be wide enough to divide, and an angle must stay
    # an angle. Mirrored rule for rule by main/art/orb_clock3.c's AUX projection.
    if cls == CLASS_AUX:
        if not 0 <= kind < len(MOTION_NAMES):
            raise ValueError("%s.kind: %d is not a way for a pointer to move (%s, 0..%d)"
                             % (where, kind, "/".join(MOTION_NAMES), len(MOTION_NAMES) - 1))
        if not 0 <= src < len(SOURCES):
            raise ValueError("%s.src: %d is not a value this dial can read (0..%d)"
                             % (where, src, len(SOURCES) - 1))
        if SOURCES[src] == "none":
            raise ValueError("%s.src: an AUX pointer driven by 'none' never moves, and a "
                             "row that never moves is plate art that was written as a "
                             "row" % where)
        vmax, vmin = int(rec["vmax"]), int(rec["vmin"])
        span, zero = int(rec["span"]), int(rec["zero"])
        if vmax < 1:
            raise ValueError("%s.vmax: %d cannot divide a %s — a pointer's span is shared "
                             "across its domain, so the domain has to hold at least one "
                             "unit" % (where, vmax, MOTION_NAMES[kind]))
        if span == 0:
            raise ValueError("%s.span: 0 is a pointer that goes nowhere; give it the %s it "
                             "travels, or bake the plate and name no row"
                             % (where, MOTION_UNIT[MOTION_NAMES[kind]]))
        if vmin > vmax:
            raise ValueError("%s.vmin: %d is above vmax %d — the domain runs low to high, "
                             "and frame 0 indexes the bottom of it" % (where, vmin, vmax))
        if kind in MOTION_SLIDES:
            # A slide is placed by its own origin and travels in pixels, so it has no
            # pivot to turn about and no angle to stay inside. A slide of more than the
            # panel's width is a bar that leaves the glass at one end of its domain.
            if abs(span) > 466 or abs(zero) > 466:
                raise ValueError("%s: a %s of %d px cannot stay on a 466 px panel — a "
                                 "slide's span and zero are pixels, not tenths"
                                 % (where, MOTION_NAMES[kind], span))
        else:
            # A rotation turns about a point inside its own plate, the same bound a HAND
            # is held to, and for the same reason: a pivot off the ink is a pointer
            # swinging about a point that is not on it.
            if int(rec["w"]) and int(rec["h"]):
                px, py = int(rec["pivot_x"]), int(rec["pivot_y"])
                w, h = int(rec["w"]), int(rec["h"])
                if not (0 <= px < w and 0 <= py < h):
                    raise ValueError("%s: an AUX pointer's pivot (%d,%d) sits outside its "
                                     "own %dx%d plate, so it would turn about a point off "
                                     "the glass" % (where, px, py, w, h))
            if abs(zero) > 3600:
                raise ValueError("%s.zero: %d is an offset past one full turn "
                                 "(±3600 tenths), which is not a direction"
                                 % (where, zero))
            if abs(span) > 3600:
                raise ValueError("%s.span: %d asks a pointer to travel more than one full "
                                 "turn; a %s that needs more is a disc, not a hand"
                                 % (where, span, MOTION_NAMES[kind]))
        # One plane, whatever the motion: a pointer is one plate that moves, and a row
        # naming several is a bank wearing an AUX label.
        if int(rec["n_frames"]) != 1:
            raise ValueError("%s: an AUX row carries one plate, not %d — several plates "
                             "indexed by a value is a BANK row"
                             % (where, int(rec["n_frames"])))
    if cls == CLASS_BANK:
        if not 0 <= kind < len(KINDS):
            raise ValueError("%s.kind: %d is not a way to draw plates (0..%d)"
                             % (where, kind, len(KINDS) - 1))
        if not 0 <= src < len(SOURCES):
            raise ValueError("%s.src: %d is not a value this dial can read (0..%d)"
                             % (where, src, len(SOURCES) - 1))
        # Ruling B: the frame-to-value map is the source's domain, not a carried
        # figure. A WINDOW whose source has a fixed calendar domain must carry
        # exactly that many plates — a month row with 13 frames is not a theme
        # being clever, it is a bake that counted the wrong thing.
        if kind == KINDS.index("window") and SOURCES[src] in CLOCK_WINDOW_DOMAIN:
            want = CLOCK_WINDOW_DOMAIN[SOURCES[src]][1]
            if int(rec["n_frames"]) != want:
                raise ValueError("%s: a window driven by %r indexes %d plates, and "
                                 "the row carries %d" % (where, SOURCES[src], want,
                                                         int(rec["n_frames"])))
    return rec



def check_table(recs):
    """Whole-table rules: the ones that need more than one row to state.

    Identity uniqueness is the load-bearing one. An app resolves an element by
    (class, slot), so two rows claiming one slot leave it painting one of them on a
    coin flip, and the order column then decides what the owner sees instead.
    """
    if len(recs) > ELEM_MAX:
        raise ValueError("element table holds %d rows, past the bound of %d"
                         % (len(recs), ELEM_MAX))
    seen_n, seen_slot = {}, {}
    for i, r in enumerate(recs):
        check_element(r, "elem[%d]" % i)
        n, key = int(r["n"]), (int(r["class"]), int(r["slot"]))
        if n in seen_n:
            raise ValueError("elem[%d]: row identity %d repeats the row at index %d"
                             % (i, n, seen_n[n]))
        if key in seen_slot:
            raise ValueError("elem[%d]: two rows claim class %r slot %d (rows %d and "
                             "%d), and the app cannot paint both"
                             % (i, CLASS_NAMES[key[0]], key[1], seen_slot[key], i))
        seen_n[n], seen_slot[key] = i, i
    return recs


def pack_elements(recs, cap=ELEM_MAX):
    """Checked pack of a whole table: real rows only, no padding to the cap, exactly
    as pack_records does its own tables."""
    check_table(recs)
    if len(recs) > cap:
        raise ValueError("element table: %d rows exceed the cap of %d"
                         % (len(recs), cap))
    out = []
    for i, r in enumerate(recs):
        out += pack_record(r, ELEMENT_FIELDS, "elem[%d]" % i)
    return out


def unpack_elements(words, n, stride=None):
    """The reader's side, at the row stride the blob carries (§23.5): columns past
    a narrow row's width read as the zeros they were written after, and columns
    before a wider row's belong to a schema this file does not own. Deliberately
    does NOT re-run check_element: a device that refused a theme it had already
    installed would leave the dial with no theme at all, so the firmware
    bounds-checks what it can and paints the rest."""
    if stride is None:
        stride = ELEMENT_WORDS
    out = []
    for i in range(n):
        row = words[i * stride:(i + 1) * stride]
        rec = blank_element()
        rec.update(unpack_record(row[:len(ELEMENT_FIELDS)],
                                 ELEMENT_FIELDS[:len(row)], "elem[%d]" % i))
        out.append(rec)
    return out


def blank_element():
    """A zeroed row with every key present. n_frames 0 is not a legal element, so a
    bake fills that before it packs: blank is a starting point, not a default."""
    return {n: 0 for n, _ in ELEMENT_FIELDS}

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
                       "main", "art", "clk_types.h")
    cflags = {n.lower(): 1 << int(v) for n, v in
              re.findall(r"\bCLK_FLAG_(\w+)\s*=\s*1\s*<<\s*(\d+)", open(hdr).read())}
    pyflags = {n[len("FLAG_"):].lower(): v for n, v in globals().items()
               if n.startswith("FLAG_")}
    assert cflags == pyflags, ("flag bits diverged from main/art/clk_types.h: "
                               "python %s vs C %s" % (pyflags, cflags))
    assert len(SOURCES) == 17, "SOURCES grew; clk_types.h's CLK_SRC__MAX assert must agree"
    # D32's ruling B mirror: CLOCK_WINDOW_DOMAIN here and the WINDOW_DOMAIN
    # table of main/art/orb_clock3.c are one calendar, and a drift would have
    # the device check a window against a year of some other length. Parse the
    # C designated initializers and compare the (min, count) pairs by source.
    ctext = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                              "main", "art", "orb_clock3.c")).read()
    cdom = {name.lower(): (int(vmin), int(cnt)) for name, vmin, cnt in
            re.findall(r"\[CLK_SRC_(\w+)\]\s*=\s*\{\s*(-?\d+)\s*,\s*(\d+)\s*\}", ctext)}
    assert cdom == CLOCK_WINDOW_DOMAIN, ("CLOCK_WINDOW_DOMAIN diverged from "
                                         "orb_clock3.c: python %s vs C %s"
                                         % (CLOCK_WINDOW_DOMAIN, cdom))
    # The element table (D32). Same habit as the rows above: the widths and the
    # bounds are what the firmware's mirror and the P6.1 suite are pinned against,
    # so they fail here on `python3 tools/theme_schema.py` and not on a dial.
    assert ELEMENT_WORDS == 23 == len(ELEMENT_FIELDS), "element row width moved"
    assert len(CLASS_NAMES) == 9, "class vocabulary moved; orb_assets.h's bound must agree"
    assert len(MOTION_NAMES) == 5, "the pointer's motion vocabulary moved"
    assert class_id("hand") == CLASS_HAND and class_id("nope") is None
    assert class_id("aux") == CLASS_AUX == 8, "D34's class must append at 8, never move"
    _e = dict(blank_element(), **{"n": 0, "class": CLASS_HAND, "slot": 0,
                                  "flags": EF_OS_ADV, "order": 1, "alpha": 255,
                                  "frame0": 0, "n_frames": 1, "x": 10, "y": 20,
                                  "w": 8, "h": 90, "pivot_x": 4, "pivot_y": 88,
                                  "radius": 0, "degrees": 9000, "scale16": 16})
    assert pack_elements([_e]) == pack_record(_e, ELEMENT_FIELDS, "elem[0]")
    assert unpack_elements(pack_elements([_e]), 1)[0] == _e, "element row must round-trip"
    # A D32-width row still reads: the four drive columns land on the zeros they were
    # written after, which is the append-only promise the stride derivation exists for.
    _narrow = unpack_elements(pack_record(_e, ELEMENT_FIELDS[:19], "e"), 1, stride=19)[0]
    assert _narrow == dict(_e, span=0, zero=0, vmin=0, vmax=0), "a narrow row must widen"
    # ---- D34: the pointer row, accepted and refused, and round-tripped ----
    _ax = dict(blank_element(), **{"n": 5, "class": CLASS_AUX, "slot": 0, "order": 3,
                                   "alpha": 255, "frame0": 4, "n_frames": 1,
                                   "x": 300, "y": 120, "w": 40, "h": 8,
                                   "pivot_x": 4, "pivot_y": 4,
                                   "kind": MOTION_ROTATE, "src": SOURCES.index("second"),
                                   "span": 3600, "zero": 0, "vmin": 0, "vmax": 60})
    # `class` is a Python keyword, so every override below is a dict KEY and the
    # helper applies it with update() — a kwarg here is a SyntaxError, not a test.
    def _with(**over):
        r = dict(_e)
        r.update({str(k): v for k, v in over.items()})
        return r
    assert check_table([_e, _with(n=1, **{"class": CLASS_TEXT, "slot": 3, "radius": 0,
                                          "pivot_x": 0, "pivot_y": 0, "flags": 0})])
    assert unpack_elements(pack_elements([_ax]), 1)[0] == _ax, "a pointer row must round-trip"
    assert check_table([_e, _ax])
    _slide = dict(_ax, **{"slot": 1, "kind": MOTION_SLIDE_X, "span": 90, "zero": -45,
                          "vmin": 0, "vmax": 100, "pivot_x": 0, "pivot_y": 0})
    assert check_element(_slide, "sentinel-aux-slide")
    # §26 slice 4: the twin row is a flagged aux row, legal on its class and only there.
    _twin = dict(_ax, **{"slot": 1, "flags": EF_AUX_SHADOW})
    assert check_element(_twin, "sentinel-aux-twin")
    assert unpack_elements(pack_elements([_twin]), 1)[0] == _twin, "a twin row must round-trip"
    for _bad, _why in (
        (_with(**{"class": 9}), "unknown class"),
        (_with(flags=EF_ALPHA | 0x80), "unknown flag bit"),
        (_with(flags=EF_AUX_SHADOW), "twin bit off class aux"),
        (_with(order=64), "order past the rungs"),
        (_with(n_frames=0), "zero frames"),
        (_with(frame0=359, n_frames=2), "frames past the bound"),
        (_with(degrees=36000), "degrees past a turn"),
        (_with(**{"class": CLASS_BANK, "n_frames": 4}), "OS-advanced bank"),
        (_with(flags=0, **{"class": CLASS_LAYER, "n_frames": 2}), "animated composited"),
        (_with(pivot_x=99), "hand pivot off the plate"),
        (_with(**{"class": CLASS_TEXT, "slot": 1, "radius": 40}), "radius on a text row"),
        (_with(scale16=8), "scale below one sixteenth"),
        (_with(**{"class": CLASS_AUX, "kind": 1}), "a drive on a plain hand row"),
        (_ax, "an accepted pointer refuses — the fixture broke"),
        (dict(_ax, kind=5), "motion past the vocabulary"),
        (dict(_ax, src=0), "a pointer driven by none"),
        (dict(_ax, vmax=0), "a domain that cannot divide"),
        (dict(_ax, span=0), "a pointer that goes nowhere"),
        (dict(_ax, vmin=61), "vmin above vmax"),
        (dict(_ax, n_frames=2), "a pointer carrying two plates"),
        (dict(_ax, zero=3700), "a pointer zero past a turn"),
        (dict(_ax, span=3700), "a pointer travelling past a turn"),
        (dict(_ax, pivot_x=44), "a pointer pivot off the plate"),
        (dict(_slide, span=900), "a slide off the panel"),
    ):
        if _why.startswith("an accepted"):
            assert check_element(_bad, "sentinel-aux") == _bad
            continue
        try:
            check_element(_bad, "sentinel")
        except ValueError:
            pass
        else:
            raise AssertionError("check_element accepted %s" % _why)
    for _dup, _why in (([_e, dict(_e)], "two rows on one slot"),
                       ([_e, _with(**{"class": CLASS_TEXT, "slot": 9, "n": 0})],
                        "duplicate n")):
        try:
            check_table(_dup)
        except ValueError:
            pass
        else:
            raise AssertionError("check_table accepted %s" % _why)
    print("theme_schema ok: %d sources, %d kinds, comp %d words, aux %d words, "
          "bank %d words, elem %d words x %d rows"
          % (len(SOURCES), len(KINDS), words_of("comp"), words_of("aux"),
             words_of("bank"), ELEMENT_WORDS, ELEM_MAX))
    # The clock's slot vocabulary is the join three readers copy, so it checks itself
    # the way the field tables do: a name too long for the wire, a class the enum has
    # no word for, a doubled (class, slot) or a doubled name all die here at import of
    # the module that owns them rather than in a bake or a draw.
    seen_name, seen_pair = set(), set()
    for _n, _c, _s, _why in CLOCK_SLOTS:
        assert _n and len(_n) <= 11, "clock slot %r is over the 11-char name field" % (_n,)
        assert all(ch.islower() or ch.isdigit() or ch == "_" for ch in _n), \
            "clock slot %r is not [a-z0-9_]" % (_n,)
        assert _n not in seen_name, "duplicate clock slot name %r" % (_n,)
        assert _c in CLASS_NAMES, "clock slot %r names class %r the enum lacks" % (_n, _c)
        assert (_c, _s) not in seen_pair, "clock slot %r reuses (%s, %d)" % (_n, _c, _s)
        seen_name.add(_n); seen_pair.add((_c, _s))
        assert clock_slot(_n) == (_c, _s), "clock_slot disagrees with CLOCK_SLOTS"
        assert clock_slot_name(_c, _s) == _n, "clock_slot_name is not the inverse"
    for _n in CLOCK_SLOTS_DRAWN:
        assert _n in seen_name, "CLOCK_SLOTS_DRAWN names %r, which is not a slot" % (_n,)
    # A slot the dial draws with must be one the shipped reader can seat, or the
    # vocabulary is promising a picture the projection refuses.
    assert clock_slot("bg") == ("plate", 0) and clock_slot("glass") == ("plate", 1)
    assert clock_slot("carriage") == ("hand", 5)
    print("             %d clock slots, %d drawn, names within %d chars"
          % (len(CLOCK_SLOTS), len(CLOCK_SLOTS_DRAWN), 11))
