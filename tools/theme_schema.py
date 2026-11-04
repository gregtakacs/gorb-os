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
CLK_AUX_MAX    = 8     # FORMAT 2's pointer table (retired with the dialect)
CLK_ROW_MAX    = 32    # D42: the AUX rows, every hand and pointer (orb_clock3.c); 24 until the
                       # tracker's plane pool (flight-tracker-design §11.2)
CLK_BANK_MAX   = 256   # shared RGBA sprites: ring slots, strip glyphs, flares
CLK_STRIP_GLYPHS = 8   # glyph image objects created per strip
CLK_COMP_WORDS = 26    # == len(COMP_FIELDS); the C mirror and the size check use it
CLK_AUX_WORDS  = 24    # == len(AUX_FIELDS) == 15 sprite words + 9 driving words
CLK_BANK_WORDS = 15    # == orbtheme.SPRITE_FIELDS

# ---------------------------------------------------------------------------
# The slot a theme installs into, exported so every tool IMPORTS the cap instead
# of restating it (§7.4: four tools carried the retired 4 MB A/B pair's figure
# long after Migration 1 made the slot the whole 8 MB partition). The authority
# is main/art/orb_bundle.h ORB_THEME_SLOT_CAP; the private layout that produces
# it is main/art/orb_theme.c's SLOT_*, bound to the export by a _Static_assert
# there; tools/host_sd_test.py re-derives all of it from the C on every run.
# ---------------------------------------------------------------------------
SLOT_GEOM_OFF      = 0x00001000              # geometry lands here, within the slot
SLOT_ART_OFF       = 0x00010000              # banks land here
SLOT_CAP           = 0x00800000              # the whole `assets`: one slot
ORB_THEME_SLOT_CAP = SLOT_CAP - SLOT_ART_OFF           # 8,323,072 B of banks
GEOM_BUDGET        = SLOT_ART_OFF - SLOT_GEOM_OFF      # 61,440 B: SLOT_GEOM_CAP

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
                     # authority (docs/history/behaviour-vocabulary.md G2; the
                     # earlier "0..3" comment was wrong, no bake reached it)
    "value",         # raw: index == the comp's own value, for bake-decided art
    "year",          # D43 P5d.3: the year itself (2026); no window domain, so a row
                     # reads it by digit (a year wheel's four)
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
    orb_stage.c, which is what the loader's bounds check is compared against."""
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
# The value -> frame maths. orb_stage.c carries the same arithmetic, so it lives
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
                           # AUX: SHAPES id, what the row's art is (D42).
    ("src",      "u8"),    # BANK and AUX: SOURCES id, what value drives the row.
    # D34's four drive columns, for an AUX row and for no other class. D42 reads them
    # as the angular half of the row's two poses: `zero` is P0's angle and `span` the
    # turn from P0 to P1, both in tenths of a degree, across the domain vmin..vmax
    # (t = (u - vmin) / (vmax - vmin)). The pivot's travel rides dx/dy, appended below.
    ("span",     "i16"),   # AUX: the turn from P0 to P1, tenths of a degree
    ("zero",     "i16"),   # AUX: P0's angle, tenths of a degree
    ("vmin",     "u16"),   # AUX: the value at P0
    ("vmax",     "u16"),   # AUX: the value at P1 (a wrap's next P0). Above vmin
    # C4's delta dialect, appended exactly as D32's ruling A did. Both columns
    # mean nothing below the v4 clock stamp, and the stamp gate (orb_clock3.c,
    # mirrored here and in the baker) is what says so; a row packed into a
    # narrower stride reads them at their defaults — parent NONE, delay 0 —
    # the same append-only answer kind/src and the drive four got.
    ("parent",   "u16"),   # row `n` of the drawing row this one hangs on;
                           # 0xFFFF = none. 0 is the plate's tag and a refusal.
    ("delay",    "u16"),   # per-frame ms for this row's animation; 0 = default
    # D41's motion columns (docs/history/hand-motion-plan.md §4), appended exactly as C4's
    # pair was. They mean nothing below the v5 clock stamp: a v4 pack refuses a
    # non-default one, and a narrower row reads each as 0 — inherit, no argument,
    # no ease — which is every row's pre-D41 motion.
    ("cadence",  "u8"),    # AUX/BANK: CADENCES id, when the row moves
    ("cad_arg",  "u16"),   # step N, stop-to-go P (tenths of a second), drag window (min)
    ("ease",     "u16"),   # settle on a step/jump, flyback on a flyback row, in ms
    # D42's motion columns (docs/history/element-model-plan.md §8), the clock's v6 row. An AUX
    # row moves from P0 (its own placement, turned to `zero`) to P1 (turned a further
    # `span`, its pivot moved by dx/dy) across its domain, and `end` says what happens
    # past the end. Zero on every other class.
    ("dx",       "i16"),   # AUX: the pivot's travel from P0 to P1, px
    ("dy",       "i16"),
    ("end",      "u8"),    # AUX: ENDS id — wrap, flyback, hold
    ("digit",    "u8"),    # AUX: DIGITS id — the whole value, its units, its tens
    ("str",      "u16"),   # AUX text row: its fonts by tone, a nibble each (theme-text §2.2); 0
)
ELEMENT_WORDS = len(ELEMENT_FIELDS)          # 33 words, 132 bytes per row
# Every app row but the clock's is the 23-word v3 row, whatever the container's stamp.
ELEMENT_WORDS_V3 = 23
# The clock section's own geometry stamp. D42 ruled out legacy: the clock reads v6
# and nothing older, at exactly ELEMENT_WORDS a row (orb_clock3.c and
# check_clock_stamp() here gate it). The container header carries 6 whenever a clock
# section rides along and 3 (APP_ABI) for a bundle of app sections alone.
APP_ABI = 3           # == orbbundle.GEOM_ABI3 == orb_bundle.h ORB_BUNDLE3_ABI
CLOCK_ABI = 6         # == orbbundle.GEOM_ABI6 == orb_bundle.h ORB_BUNDLE3_ABI6 (D42)
PARENT_NONE = 0xFFFF
# The narrowest row the FORMAT 3 dialect has ever written, before D32's ruling A
# appended kind and src. A blob's element-row stride is derived from its own
# length (§23.5), and a stride below this floor is not a shorter row of this
# dialect but a torn file; the reader refuses it rather than read a width that
# cannot be the current schema's. orb_bundle.c's ORB_BUNDLE_ELEM_WORDS_MIN agrees.
ELEMENT_WORDS_MIN = 17
# The clock section's scalar words D42 retired: a v6 section carries each as 0, and a
# non-zero one is a bake from before the ruling. orb_clock3.c refuses the same list.
# (Names are orbtheme.THEME_FIELDS'; the tempo they carried is derived from the rows.)
CLOCK_RETIRED_SCALARS = ("plate_follow", "shadow_on", "shadow_dx", "shadow_dy",
                         "second_sweep", "second_railway", "text_over_hands",
                         "tick_rate", "order0", "order1", "order2", "order3", "order4",
                         "order_n", "matte_mask", "tourbillon_n", "tourbillon_fps")

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
EF_INSTANCE = 1 << 5    # C4: one seated plane standing for the row's whole frame
                        # window (§4.2's first_stride=0 reading rule: the comp
                        # maths draw n_frames slots of the one plate). Legal on a
                        # fixed-domain WINDOW row under the v4 clock stamp and
                        # nowhere else; orb_clock3.c refuses the bit under v3
                        # rather than draw the single frame the row must be.
EF_DELTA_WRAP = 1 << 6  # §37: the delta plate's LAST sprite, one past its own frame
                        # window, is the rect that returns the finished canvas to the
                        # keyframe — the delta the device applies when the loop wraps to
                        # frame 0. Without it a wrap is a keyframe blit no delta table
                        # can express: a stalled tick once per loop and a stale base
                        # under frame 1. The baker's rowDelta sets it in the same breath
                        # as shipping the row; class plate only (check_element below),
                        # and orb_clock3.c refuses the bit on any other shape.
EF_SMOOTH = 1 << 7      # the unified render path's slice 5: this layer samples bilinear,
                        # in the LVGL widget and in the L4 painter alike. The BAKE decides
                        # (orbtheme-baker.js estimate3: smoothing where the commit's price
                        # with the AA term still holds 12 fps) and the dial reads the bit;
                        # nothing on the device re-prices it. Hand, shadow (class sprite)
                        # and aux rows only. A flag and not a scalar: orb_bundle.c refuses a
                        # 27th scalar on every installed theme, while the reader never
                        # checks flag bits, so an old theme reads 0 (nearest) and old
                        # firmware ignores the bit on a new one.
EF_ALL = (EF_ALPHA | EF_LOOP | EF_OS_ADV | EF_HIDDEN | EF_AUX_SHADOW | EF_INSTANCE
          | EF_DELTA_WRAP | EF_SMOOTH)

ELEM_ORDER_N    = 64   # depth rungs: one scale for every row the clock draws (D42)

# ---- the clock's slot vocabulary (D32's join, ruled by the app that declares) ----
# A theme's element row carries (class, slot) and never a name: slot is "which
# declaration of this class this row fulfils" (ELEMENT_FIELDS). Names therefore
# belong to the APP, and this table is the one place the clock's names, classes and
# slots are written down. Three readers depend on it and none may paraphrase it:
# `main/stage/clock_slots.c` mirrors it for the compiled app,
# `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` mirrors it for the bake that fills the
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
    # D34's pointer slots, widened by D42 into the clock's one moving-row vocabulary:
    # every hand, pointer, ring, still layer and carriage is an AUX row, claimed by
    # ordinal. Names are `ptr0..ptr31` and NOT `hour` or `subdial_seconds`: the clock
    # does not know what a row indicates, only how its own drive moves it, and a name
    # that claimed the role would be a second source of truth about the art.
    ("ptr0",          "aux",    0,    "row 0: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr1",          "aux",    1,    "row 1: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr2",          "aux",    2,    "row 2: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr3",          "aux",    3,    "row 3: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr4",          "aux",    4,    "row 4: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr5",          "aux",    5,    "row 5: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr6",          "aux",    6,    "row 6: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr7",          "aux",    7,    "row 7: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr8",          "aux",    8,    "row 8: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr9",          "aux",    9,    "row 9: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr10",         "aux",    10,   "row 10: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr11",         "aux",    11,   "row 11: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr12",         "aux",    12,   "row 12: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr13",         "aux",    13,   "row 13: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr14",         "aux",    14,   "row 14: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr15",         "aux",    15,   "row 15: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr16",         "aux",    16,   "row 16: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr17",         "aux",    17,   "row 17: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr18",         "aux",    18,   "row 18: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr19",         "aux",    19,   "row 19: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr20",         "aux",    20,   "row 20: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr21",         "aux",    21,   "row 21: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr22",         "aux",    22,   "row 22: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr23",         "aux",    23,   "row 23: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr24",         "aux",    24,   "row 24: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr25",         "aux",    25,   "row 25: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr26",         "aux",    26,   "row 26: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr27",         "aux",    27,   "row 27: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr28",         "aux",    28,   "row 28: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr29",         "aux",    29,   "row 29: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr30",         "aux",    30,   "row 30: one plate (or a carriage's frames), moved by its own drive"),
    ("ptr31",         "aux",    31,   "row 31: one plate (or a carriage's frames), moved by its own drive"),
    # D39: the sound slots, one per event (docs/history/sound-plan.md §3.1). The order IS the
    # slot number main/art/orb_clock3.h's CLK_SND_* enum reads, and the suite pins it.
    # Played by the clock's sound policy, never drawn, so none is in CLOCK_SLOTS_DRAWN.
    ("tick_loop",     "sound",  0,    "the tick loop, anchored to the wall second"),
    ("chime_q1",      "sound",  1,    "the :15 chime"),
    ("chime_half",    "sound",  2,    "the :30 chime"),
    ("chime_q3",      "sound",  3,    "the :45 chime"),
    ("chime_hour",    "sound",  4,    "the :00 chime, before the hour pattern"),
    ("strike_hour",   "sound",  5,    "the hour stroke the pattern repeats"),
    ("jump_minute",   "sound",  6,    "a minute row's step (D41)"),
    ("jump_hour",     "sound",  7,    "an hour row's step"),
    ("jump_date",     "sound",  8,    "a calendar row's step at midnight"),
    ("morse_dit",     "sound",  9,    "the morse pattern's short symbol"),
    ("morse_dah",     "sound",  10,   "the morse pattern's long symbol"),
    ("strike_qtr",    "sound",  11,   "the repeater's quarter ting-tang"),
    ("strike_min",    "sound",  12,   "the repeater's minute stroke"),
)

# D39: the clock's sound vocabulary, read back out of CLOCK_SLOTS so the two cannot
# drift: the slot names in slot order, the hour patterns (orb_clock3.h CLK_PAT_*),
# and the one encoding a sound row may name.
CLOCK_SOUND_SLOTS = tuple(n for n, c, _s, _w in CLOCK_SLOTS if c == "sound")
HOUR_PATTERNS = ("count", "ships_bells", "morse", "repeater", "none")
SOUND_RATE = 22050           # the one rate the engine runs (orb_sound.h ORB_SOUND_RATE)
SOUND_ENC_PCM16 = 0          # a sound row's `src`: 16-bit LE mono PCM at SOUND_RATE

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
# of them now — plate, the thirty-two moving rows (hands, shadows, the carriage),
# and the sixteen complication slots — so the refusal a row can still meet is the projection's own
# (a class this app's manifest does not offer, a plate whose art the bundle does
# not hold), and ruling D32-F makes any of those refusals fall the whole flip to
# the dial's digits rather than draw a partial theme.
CLOCK_SLOTS_DRAWN = (("bg", "glass")
                     + tuple("comp%d" % i for i in range(CLK_COMP_MAX))
                     + tuple("ptr%d" % i for i in range(CLK_ROW_MAX)))


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

ELEM_MAX        = 64   # rows one app's table may carry (the clock: 2 + 24 + 16 + 1 + 13)
ELEM_FRAMES_MAX = 1024  # bound on frame0+n_frames (av-kit's busiest bank uses 208; a stage
                        # with four fonts of 100 glyphs and a 49-frame plane pool ~460,
                        # theme-text-plan T6). The device bounds rows by the catalogue.

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
CLASS_NAMES = ("text", "sprite", "hand", "plate", "ring", "bank", "layer", "status",
               "aux", "preview", "sound")
CLASS_TEXT, CLASS_SPRITE, CLASS_HAND, CLASS_PLATE = 0, 1, 2, 3
CLASS_RING, CLASS_BANK, CLASS_LAYER, CLASS_STATUS = 4, 5, 6, 7
CLASS_AUX = 8
# The preview class: one opaque RGB565 plane a theme carries so a picker can draw
# the dial before it is installed. A theme's art bank costs it `w*h*2` for the
# same reason orb_asset_slot_bytes() returns that width: the third byte per pixel
# an RGB565A8 class pays would be a plane of 255s no reader consults. Appended at
# 9 without moving CLASS_ABI or the enum's earlier order, exactly as D34 appended
# AUX at 8: a theme that predates this build never names the slot, and a theme
# that does names it against a class this writer stamps and this schema reads.
CLASS_PREVIEW = 9
# D39's append: a sound. One waveform per event slot an app declares — 16-bit mono PCM
# at SOUND_RATE, carried as the catalogue row's plane with w samples and h 1, so a
# theme's bank pays w*h*2 for it, the shape orb_asset_slot_bytes() gives the class.
# Its facts ride the row's own columns (check_clock_sounds), never a header in the
# bank: the device's projection is pure and runs before the bank is resident, so only
# what the table says can be refused at the door. Appended at 10 without moving
# CLASS_ABI, the AUX and PREVIEW precedent.
CLASS_SOUND = 10

# D42: what an AUX row's art is, read from its `kind` column (clk_types.h CLK_SHAPE_*).
# The shape says nothing about motion: an arm and a ring move by the same two poses. A
# text row is a label: the app's string in the theme's fonts (theme-text-plan §2.2).
SHAPES = ("arm", "ring", "text", "drum")
SHAPE_ARM, SHAPE_RING, SHAPE_TEXT, SHAPE_DRUM = 0, 1, 2, 3   # D43: drum, a barrel the OS rolls
# theme-text-plan §2.1: a FONT is a TEXT row in a stage section. Its frames are its glyphs,
# one cell each, in FONT_CHARSET order: printable ASCII, then the arrows and the degree sign.
# A font carries ASCII whole and any prefix of the extras. `pivot_x` is the space's advance
# and `pivot_y` the tracking between one glyph's ink and the next; a glyph's advance is its
# ink, measured by the OS. Fonts sit at slots 0..FONT_SLOTS-1.
FONT_ASCII = tuple(chr(c) for c in range(0x20, 0x7F))
FONT_EXTRAS = ("\u2191", "\u2193", "\u2192", "\u2190", "\u00b0")
FONT_CHARSET = FONT_ASCII + FONT_EXTRAS
FONT_SLOTS = 15
# §2.2: a LABEL is an AUX row of shape text and source text. Its one frame is its panel;
# `str` names its fonts by tone, a nibble each, tone 0 lowest, FONT_NONE for a tone the
# theme leaves to tone 0; `radius` is its padding; `digit` and `end` align the text.
TONES = ("normal", "accent", "warning", "quiet")
FONT_NONE = 0xF
ALIGNS = ("start", "centre", "end")
LABEL_PAD_MAX = 32
LABEL_STILL_COLUMNS = ("span", "zero", "vmin", "vmax", "dx", "dy", "cadence", "cad_arg", "ease")


def label_fonts(str_col):
    """A label's fonts by tone: the four nibbles of its `str`, tone 0 first."""
    return [(int(str_col) >> (4 * k)) & 0xF for k in range(len(TONES))]


def check_label(rec, where):
    """theme-text-plan §2.2's row-local rules for a label. Mirrored by orb_clock3.c's
    row_check and the baker's checkElement."""
    if SOURCES[int(rec["src"])] != "text":
        raise ValueError("%s.src: a text row draws the app's string, so it reads source "
                         "text, not %r" % (where, SOURCES[int(rec["src"])]))
    for c in LABEL_STILL_COLUMNS:
        if int(rec.get(c, 0)):
            raise ValueError("%s.%s: %d on a text row — the app places a label, so it "
                             "names no motion" % (where, c, int(rec[c])))
    if not 0 <= int(rec.get("digit", 0)) < len(ALIGNS) or not 0 <= int(rec.get("end", 0)) < len(ALIGNS):
        raise ValueError("%s: a text row aligns by digit %d and end %d, each one of %s"
                         % (where, int(rec.get("digit", 0)), int(rec.get("end", 0)), "/".join(ALIGNS)))
    if label_fonts(rec.get("str", 0))[0] == FONT_NONE:
        raise ValueError("%s.str: a text row names no font for tone 0 (normal), which "
                         "every other tone falls back to" % where)
    if int(rec["n_frames"]) != 1 or int(rec["flags"]) & EF_AUX_SHADOW:
        raise ValueError("%s: a text row's art is its one panel, with no frames and no "
                         "shadow" % where)
    pad, w, h = int(rec.get("radius", 0)), int(rec["w"]), int(rec["h"])
    if pad > LABEL_PAD_MAX or 2 * pad >= w or 2 * pad >= h:
        raise ValueError("%s.radius: a padding of %d leaves no room in a %dx%d panel "
                         "(at most %d)" % (where, pad, w, h, LABEL_PAD_MAX))


def check_fonts(recs):
    """§2.1-2.2's table rules: each font row, and every font a label names present.
    Mirrored by orb_clock3.c and the baker's checkClockRows."""
    fonts = {}
    for r in recs:
        if int(r["class"]) != CLASS_TEXT:
            continue
        slot, where = int(r["slot"]), "font%d" % int(r["slot"])
        if slot >= FONT_SLOTS:
            raise ValueError("%s: a stage seats %d fonts (font0..font%d)"
                             % (where, FONT_SLOTS, FONT_SLOTS - 1))
        nf, w = int(r["n_frames"]), int(r["w"])
        if not len(FONT_ASCII) <= nf <= len(FONT_CHARSET):
            raise ValueError("%s: a font carries printable ASCII whole (%d glyphs) and at "
                             "most %d, not %d" % (where, len(FONT_ASCII), len(FONT_CHARSET), nf))
        if int(r["x"]) or int(r["y"]):
            raise ValueError("%s: a font is never drawn where it stands, so it sits at 0,0"
                             % where)
        if not (0 < int(r["pivot_x"]) <= w and 0 <= int(r["pivot_y"]) <= w):
            raise ValueError("%s: a space of %d px and tracking of %d px in a %d px cell"
                             % (where, int(r["pivot_x"]), int(r["pivot_y"]), w))
        fonts[slot] = r
    for r in recs:
        if int(r["class"]) != CLASS_AUX or int(r["kind"]) != SHAPE_TEXT:
            continue
        for tone, f in enumerate(label_fonts(r.get("str", 0))):
            if f != FONT_NONE and f not in fonts:
                raise ValueError("ptr%d: tone %d (%s) names font%d, which this theme does "
                                 "not carry" % (int(r["slot"]), tone, TONES[tone], f))


# D42 §3: what a row does past the end of its domain (clk_types.h CLK_END_*). A wrap
# starts the next unit at P0 with no visible jump, so it is legal only where P1 looks
# like P0: a whole turn (±3600 tenths) and no travel. A flyback returns to P0 when the
# source resets, eased by the row's `ease`; a hold rests at P1 until it does.
ENDS = ("wrap", "flyback", "hold")
END_WRAP, END_FLYBACK, END_HOLD = 0, 1, 2
# D42 §4: which part of the source value a row reads (clk_types.h CLK_DIGIT_*). The
# units and tens of a date drive a double-rotator calendar's two rings.
# D43 P5d.3 adds a year's hundreds and thousands. A digit reads the number the dial
# shows: a 12-hour source's 0 is 12, a month's 0..11 is 1..12.
DIGITS = ("whole", "units", "tens", "hundreds", "thousands")
DIGIT_WHOLE, DIGIT_UNITS, DIGIT_TENS, DIGIT_HUNDREDS, DIGIT_THOUSANDS = 0, 1, 2, 3, 4
# The bounds a pose is written within: tenths of a degree for an angle, px for travel.
TURN_10 = 3600
PANEL_PX = 466

# D41 §2.1: when a driven row moves, in the order clk_types.h's CLK_CAD_* reads it.
# D42 retired `inherit` (there is nothing older to inherit from): 0 is `none`, the
# cadence of a still row and of nothing else.
# D43 P5d.3's roll is a wheel's smooth: the figure holds, and in the last unit before
# it changes it rolls on to the next (an odometer); vmax - vmin is its figure count.
CADENCES = ("none", "sweep", "step", "jump", "stop-to-go", "drag", "roll")
CAD_NONE, CAD_SWEEP, CAD_STEP, CAD_JUMP, CAD_STOP_TO_GO, CAD_DRAG, CAD_ROLL = 0, 1, 2, 3, 4, 5, 6
# The sources a drag may hold a calendar row on.
CAD_DRAG_SOURCES = ("date", "weekday", "month", "day_of_year")
# A sweep's own frame rate rides its cad_arg (0 = no figure of its own): the seconds
# row is the dial's beat, and the scheduler asks for that rate (orb_clock3.c).
SWEEP_TIER = 15        # a sweep that names no rate and rides nothing animated
SWEEP_FPS_MAX = 30     # the panel's ceiling
# The sources no row can move by: text and value are never inputs of the dial.
UNDRIVABLE_SOURCES = ("text", "value")
# The columns a still row leaves zero, and the D42 motion columns no class but AUX uses.
MOTION_COLUMNS = ("span", "zero", "vmin", "vmax", "dx", "dy", "end", "digit")
D42_COLUMNS = ("dx", "dy", "end", "digit", "str")
# The most frames an animated row (the carriage) carries: clk_types.h CLK_TOURB_MAX.
ROW_FRAMES_MAX = 16
# theme-pool-plan §2.6: a row with frames and no frame time is one whose frames the app
# picks (the tracker's plane pool), up to clk_types.h CLK_PICK_MAX of them.
CLK_PICK_MAX = 64

# Classes whose frame is app state, not a clock (R6). A BANK is indexed "one plate
# per value", so OS advancement would march a date wheel on a tick nobody asked for.
CLASS_APP_ADV = (CLASS_BANK, CLASS_TEXT, CLASS_STATUS)
# Classes the OS composites rather than an app paints: a row claiming several frames
# of a composited slot is a mistake about what the class is. A PREVIEW is one plane
# (theme_schema.CLASS_PREVIEW's slot_bytes is RGB565, and the picker reads it
# transiently as one picture): a second frame is a mistake about that too.
CLASS_NO_ANIM = (CLASS_LAYER, CLASS_STATUS, CLASS_PREVIEW, CLASS_SOUND)


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
    # `kind` is a KINDS id and on an AUX row a SHAPES id (D42), and the class decides
    # which of the two vocabularies the byte is read in.
    kind, src = int(rec["kind"]), int(rec["src"])
    # D39 widens the pair once more, the same way D34 did: on a SOUND row `kind` is the
    # app's hour pattern and `src` the encoding. check_clock_sounds reads them.
    if cls == CLASS_SOUND:
        if src != SOUND_ENC_PCM16:
            raise ValueError("%s.src: a sound in encoding %d — this build plays 16-bit "
                             "PCM at %d Hz (encoding %d) and nothing else"
                             % (where, src, SOUND_RATE, SOUND_ENC_PCM16))
        if not 0 <= kind < len(HOUR_PATTERNS):
            raise ValueError("%s.kind: %d is not an hour pattern (%s, 0..%d)"
                             % (where, kind, "/".join(HOUR_PATTERNS), len(HOUR_PATTERNS) - 1))
    elif kind and cls not in (CLASS_BANK, CLASS_AUX):
        raise ValueError("%s.kind: %d on a %s row — how a row draws or moves is a "
                         "driven element's question, and this class answers it with "
                         "its slot" % (where, kind, CLASS_NAMES[cls]))
    if src and cls not in (CLASS_BANK, CLASS_AUX, CLASS_SOUND):
        raise ValueError("%s.src: %d on a %s row — what drives a row is a driven "
                         "element's question, and this class answers it with "
                         "its slot" % (where, src, CLASS_NAMES[cls]))
    # ---- D42: the moving row ----
    # An AUX row is the clock's one row that moves: an arm, a ring, a carriage, a still
    # layer. A still row (source none) names nothing about motion. A moving row names its
    # source and cadence, a domain wide enough to divide, and somewhere to go. Mirrored
    # rule for rule by main/art/orb_clock3.c's row projection.
    cad, carg, ease = (int(rec.get("cadence", 0)), int(rec.get("cad_arg", 0)),
                       int(rec.get("ease", 0)))
    if cls != CLASS_AUX:
        for c in D42_COLUMNS:
            if int(rec.get(c, 0)):
                raise ValueError("%s.%s: %d on a %s row — a row's poses are a moving row's "
                                 "question (class aux)" % (where, c, int(rec[c]),
                                                          CLASS_NAMES[cls]))
        if cls != CLASS_SOUND and any(int(rec.get(c, 0)) for c in ("span", "zero", "vmin", "vmax")):
            raise ValueError("%s: span, zero, vmin and vmax are a moving row's poses, and "
                             "class %r does not move — every row that moves is an AUX row "
                             "(D42)" % (where, CLASS_NAMES[cls]))
    if cls == CLASS_AUX:
        if not 0 <= kind < len(SHAPES):
            raise ValueError("%s.kind: %d is not a row's shape (%s, 0..%d)"
                             % (where, kind, "/".join(SHAPES), len(SHAPES) - 1))
        if kind == SHAPE_TEXT:
            check_label(rec, where)
        elif int(rec.get("str", 0)):
            raise ValueError("%s.str: %d — only a text row names fonts"
                             % (where, int(rec["str"])))
        if not 0 <= src < len(SOURCES):
            raise ValueError("%s.src: %d is not a value this dial can read (0..%d)"
                             % (where, src, len(SOURCES) - 1))
        if SOURCES[src] in UNDRIVABLE_SOURCES and kind != SHAPE_TEXT:
            raise ValueError("%s.src: %r is no input of this dial, and a row that moves by "
                             "it would never move" % (where, SOURCES[src]))
        span, zero, vmin, vmax = (int(rec["span"]), int(rec["zero"]),
                                  int(rec["vmin"]), int(rec["vmax"]))
        dx, dy = int(rec.get("dx", 0)), int(rec.get("dy", 0))
        end, digit = int(rec.get("end", 0)), int(rec.get("digit", 0))
        if int(rec["degrees"]):
            raise ValueError("%s.degrees: %d — a row's angle at rest is its `zero`"
                             % (where, int(rec["degrees"])))
        if kind == SHAPE_TEXT:
            pass                                  # check_label said it all
        elif SOURCES[src] == "none":
            for c in MOTION_COLUMNS + ("cadence", "cad_arg", "ease"):
                if int(rec.get(c, 0)):
                    raise ValueError("%s.%s: %d on a still row — a row with no source "
                                     "never moves, so it names no motion"
                                     % (where, c, int(rec[c])))
        else:
            if not cad:
                raise ValueError("%s.cadence: a row moving by %r names when it moves "
                                 "(sweep, step, jump, stop-to-go, drag)"
                                 % (where, SOURCES[src]))
            if not 0 <= end < len(ENDS):
                raise ValueError("%s.end: %d is not an end rule (%s, 0..%d)"
                                 % (where, end, "/".join(ENDS), len(ENDS) - 1))
            if not 0 <= digit < len(DIGITS):
                raise ValueError("%s.digit: %d is not a digit selector (%s, 0..%d)"
                                 % (where, digit, "/".join(DIGITS), len(DIGITS) - 1))
            if vmax <= vmin:
                raise ValueError("%s.vmax: %d is not above vmin %d — a domain has to hold "
                                 "at least one unit to divide" % (where, vmax, vmin))
            if not (span or dx or dy):
                raise ValueError("%s: a moving row that neither turns nor travels goes "
                                 "nowhere; bake it as a still row" % where)
            if abs(zero) > TURN_10 or abs(span) > TURN_10:
                raise ValueError("%s: zero %d and span %d stay within one turn (±%d "
                                 "tenths)" % (where, zero, span, TURN_10))
            if abs(dx) > PANEL_PX or abs(dy) > PANEL_PX:
                raise ValueError("%s: a travel of (%d,%d) px cannot stay on a %d px panel"
                                 % (where, dx, dy, PANEL_PX))
            if end == END_WRAP and kind != SHAPE_DRUM and (abs(span) != TURN_10 or dx or dy):
                raise ValueError("%s.end: a wrap starts the next unit at P0, so P1 must "
                                 "look like P0 — a whole turn (±%d) and no travel, not "
                                 "span %d and (%d,%d)" % (where, TURN_10, span, dx, dy))
        if kind == SHAPE_DRUM:
            # D43: orb_clock3.c's row_check, rule for rule: a drum rolls its strip (one turn
            # of the drum) through a window 2 x radius tall, and wraps onto its first figure
            radius, h = int(rec.get("radius", 0)), int(rec["h"])
            if SOURCES[src] == "none":
                raise ValueError("%s: a drum that never rolls is plate art" % where)
            if span or zero or dx:
                raise ValueError("%s: a drum only rolls; it neither turns nor travels across" % where)
            if not 2 <= radius <= PANEL_PX // 2:
                raise ValueError("%s.radius: a drum of radius %d needs 2..%d px" % (where, radius, PANEL_PX // 2))
            if abs(dy) != h:
                raise ValueError("%s.dy: a drum rolls %d px over a strip %d tall; one turn is the "
                                 "whole strip" % (where, dy, h))
            if end != END_WRAP:
                raise ValueError("%s.end: a drum turns on past its last figure to its first (wrap)" % where)
            if int(rec["n_frames"]) != 1 or int(rec.get("flags", 0)) & 16:
                raise ValueError("%s: a drum is one strip, with no frames and no shadow" % where)
        if int(rec["w"]) and int(rec["h"]):
            px, py = int(rec["pivot_x"]), int(rec["pivot_y"])
            w, h = int(rec["w"]), int(rec["h"])
            if not (0 <= px < w and 0 <= py < h):
                raise ValueError("%s: a row's pivot (%d,%d) sits outside its own %dx%d "
                                 "plate, so it would turn about a point off the glass"
                                 % (where, px, py, w, h))
        nf = int(rec["n_frames"])
        if nf > 1 and int(rec.get("delay", 0)) and nf > ROW_FRAMES_MAX:
            raise ValueError("%s: an animated row carries at most %d frames, not %d"
                             % (where, ROW_FRAMES_MAX, nf))
        if nf > 1 and not int(rec.get("delay", 0)) and nf > CLK_PICK_MAX:
            raise ValueError("%s: a row offers the app at most %d frames to pick from, "
                             "not %d" % (where, CLK_PICK_MAX, nf))
        if flags & EF_AUX_SHADOW and (nf != 1 or src or cad):
            raise ValueError("%s: a shadow twin takes its hand's whole pose — one plate, "
                             "no source, no cadence" % where)
    # ---- D41: the cadence columns (§2.2's row-local refusals) ----
    if cad or carg or ease:
        if cls not in (CLASS_AUX, CLASS_BANK):
            raise ValueError("%s.cadence: when a row moves is a driven row's question "
                             "(a moving row, a bank); class %r does not move by a value"
                             % (where, CLASS_NAMES[cls]))
        if not 0 <= cad < len(CADENCES):
            raise ValueError("%s.cadence: %d is not a cadence (%s, 0..%d)"
                             % (where, cad, "/".join(CADENCES), len(CADENCES) - 1))
        eff = SOURCES[src] if 0 <= src < len(SOURCES) else "none"
        name = CADENCES[cad]
        if cad == CAD_STEP and carg < 1:
            raise ValueError("%s.cad_arg: step 0 divides a unit into no steps — name how "
                             "many per unit" % where)
        if cad in (CAD_NONE, CAD_JUMP, CAD_ROLL) and carg:
            raise ValueError("%s.cad_arg: %d on a %s row, which takes no argument"
                             % (where, carg, name))
        if cad == CAD_SWEEP and carg > SWEEP_FPS_MAX:
            raise ValueError("%s.cad_arg: a sweep at %d frames a second — the panel draws "
                             "%d at most" % (where, carg, SWEEP_FPS_MAX))
        if cad == CAD_STOP_TO_GO and eff != "second":
            raise ValueError("%s.cadence: stop-to-go is a seconds hand's rhythm, and this "
                             "row reads %r" % (where, eff))
        if cad == CAD_DRAG and eff not in CAD_DRAG_SOURCES:
            raise ValueError("%s.cadence: drag holds a calendar row (%s), and this row "
                             "reads %r" % (where, "/".join(CAD_DRAG_SOURCES), eff))
        if cls == CLASS_BANK and cad not in (CAD_NONE, CAD_JUMP):
            raise ValueError("%s.cadence: a complication's plates are whole values, so it "
                             "jumps — %s waits for a ring the dial can turn" % (where, name))
        if ease:
            fly = cls == CLASS_AUX and int(rec.get("end", 0)) == END_FLYBACK
            steps = cad in (CAD_JUMP, CAD_STEP) and eff != "second" and cls != CLASS_BANK
            if ease > 1000:
                raise ValueError("%s.ease: %d ms — an ease runs at most a second" % (where, ease))
            if not (fly or steps):
                raise ValueError("%s.ease: %d ms with nothing to ease — a settle needs a jump "
                                 "or a step slower than the second, a flyback a flyback row"
                                 % (where, ease))
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
    # ---- C4: the delta columns and the instance bit ----
    # Refuse-don't-guess, at the door that can still say it cheaply. The stamp
    # itself is the baker's and the loader's business (check_clock_stamp and
    # orb_clock3.c); these are the rules that hold under either stamp, because a
    # v3 pack refuses the columns outright before any of them can ride.
    parent = int(rec.get("parent", PARENT_NONE))
    if parent != PARENT_NONE:
        if parent == 0:
            raise ValueError("%s.parent: 0 is the plate's own row tag, and a parent "
                             "of none is %d (0x%X) — refused, not guessed"
                             % (where, PARENT_NONE, PARENT_NONE))
        if cls not in (CLASS_AUX, CLASS_BANK):
            raise ValueError("%s.parent: class %r draws, it is not hung from — only a "
                             "pointer or a complication names a parent"
                             % (where, CLASS_NAMES[cls]))
    if int(rec.get("delay", 0)) and int(rec["n_frames"]) < 2:
        raise ValueError("%s.delay: %d ms on a row that carries %d frame — a tempo "
                         "nobody will keep is a bake that lost its animation"
                         % (where, int(rec["delay"]), int(rec["n_frames"])))
    if flags & EF_INSTANCE:
        if cls != CLASS_BANK or kind != KINDS.index("window"):
            raise ValueError("%s.flags: the instance bit stands for a window's plates "
                             "and means nothing on class %r kind %r"
                             % (where, CLASS_NAMES[cls], kind))
        if SOURCES[src] not in CLOCK_WINDOW_DOMAIN:
            raise ValueError("%s.flags: an instanced window needs a fixed value domain "
                             "to stand over, and %r has none"
                             % (where, SOURCES[src]))
    if flags & EF_SMOOTH and cls not in (CLASS_HAND, CLASS_SPRITE, CLASS_AUX):
        raise ValueError("%s.flags: smoothing is a rotated layer's sampling (hand, shadow, "
                         "pointer); class %r is not turned by the painter"
                         % (where, CLASS_NAMES[cls]))
    if flags & EF_DELTA_WRAP and cls != CLASS_PLATE:
        raise ValueError("%s.flags: the closing delta (§37) belongs to a looping v4 delta "
                         "plate and nowhere else; class %r has no background loop for it "
                         "to close" % (where, CLASS_NAMES[cls]))
    return rec



def check_clock_sounds(recs, sprites, second_sweep, tick_rate):
    """D39: the clock's sound rows, refused for the reasons main/art/orb_clock3.c
    refuses them, sentence for sentence where a sentence can be shared. `sprites` is
    the section's catalogue as (off, size, w, h) tuples; `second_sweep`/`tick_rate`
    are the section scalars the tick loop's beat is held against. Returns the hour
    pattern the rows agree on, or None when there are none."""
    at = {}
    for i, r in enumerate(recs):
        if int(r["class"]) != CLASS_SOUND:
            continue
        slot = int(r["slot"])
        if slot >= len(CLOCK_SOUND_SLOTS):
            raise ValueError("a sound at slot %d: the clock declares %d sound slots"
                             % (slot, len(CLOCK_SOUND_SLOTS)))
        if int(r["n_frames"]) != 1:
            raise ValueError("a sound at slot %d names %d planes: a sound is one waveform"
                             % (slot, int(r["n_frames"])))
        if int(r["w"]) or int(r["h"]):
            raise ValueError("element %d is a sound sized %dx%d: a sound places nothing, "
                             "and its length is its plane's"
                             % (int(r["n"]), int(r["w"]), int(r["h"])))
        at[slot] = r
    pattern = None
    for slot in sorted(at):
        r = at[slot]
        off, size, w, h = sprites[int(r["frame0"])][:4]
        if h != 1 or w < 2 or size != w * 2:
            raise ValueError("sound slot %d's plane is %dx%d in %d B: a sound is w samples "
                             "by 1, two bytes each" % (slot, w, h, size))
        if off & 1:
            raise ValueError("sound slot %d starts at odd offset %d: its samples would "
                             "load unaligned" % (slot, off))
        kind = int(r["kind"])
        if pattern is not None and kind != pattern:
            raise ValueError("sound slot %d names hour pattern %d and an earlier sound "
                             "names %d: a theme has one pattern" % (slot, kind, pattern))
        pattern = kind
        loop = bool(int(r["flags"]) & EF_LOOP)
        tick = slot == CLOCK_SOUND_SLOTS.index("tick_loop")
        if tick and not loop:
            raise ValueError("the tick loop is not flagged to loop: it would tick once "
                             "through and stop")
        if loop and not tick:
            raise ValueError("sound slot %d is flagged to loop: only the tick loop runs "
                             "until it is stopped" % slot)
        vmax, vmin = int(r["vmax"]), int(r["vmin"])
        if tick:
            if not vmax:
                raise ValueError("the tick loop names no beat: the dial cannot hold it "
                                 "against the seconds hand")
            if (w * vmax) % (SOUND_RATE * 100):
                raise ValueError("the tick loop's %d samples are not a whole number of "
                                 "beats at %d beats per 100 s" % (w, vmax))
            step = tick_rate or 1
            if not second_sweep and vmax != step * 100:
                raise ValueError("the tick loop beats %d.%02d times a second and the "
                                 "seconds hand steps %d: the sound would walk off the hand"
                                 % (vmax // 100, vmax % 100, step))
            if vmin * vmax >= SOUND_RATE * 100:
                raise ValueError("the tick loop's first beat at sample %d sits past one "
                                 "beat" % vmin)
        elif vmax or vmin:
            raise ValueError("sound slot %d carries a beat: only the tick loop has one"
                             % slot)
    if pattern is not None:
        name = HOUR_PATTERNS[pattern]
        has = lambda n: CLOCK_SOUND_SLOTS.index(n) in at
        if name in ("count", "ships_bells") and not has("strike_hour"):
            raise ValueError("the hour pattern strikes and the theme carries no hour stroke")
        if name == "morse" and not (has("morse_dit") and has("morse_dah")):
            raise ValueError("the hour pattern is morse and the theme lacks a dit or a dah")
        if name == "repeater" and not (has("strike_hour") and has("strike_qtr")
                                       and has("strike_min")):
            raise ValueError("the hour pattern is a repeater and the theme lacks one of "
                             "its three strokes")
    return pattern


def sound_row(slot, frame0, pattern="none", beat_chz=0, first_beat=0, n=None):
    """A blank element row filled as a clock sound row (CLOCK_SOUND_SLOTS name or
    index). The writer places the plane; this only states the row's facts."""
    rec = blank_element()
    rec["class"] = CLASS_SOUND
    rec["slot"] = slot if isinstance(slot, int) else CLOCK_SOUND_SLOTS.index(slot)
    rec["frame0"] = frame0
    rec["n_frames"] = 1
    rec["kind"] = pattern if isinstance(pattern, int) else HOUR_PATTERNS.index(pattern)
    rec["src"] = SOUND_ENC_PCM16
    tick = rec["slot"] == CLOCK_SOUND_SLOTS.index("tick_loop")
    rec["flags"] = EF_LOOP if tick else 0
    rec["vmax"] = beat_chz if tick else 0
    rec["vmin"] = first_beat if tick else 0
    if n is not None:
        rec["n"] = n
    return rec


def check_table(recs):
    """Whole-table rules: the ones that need more than one row to state.

    Identity uniqueness is the load-bearing one. An app resolves an element by
    (class, slot), so two rows claiming one slot leave it painting one of them on a
    coin flip, and the order column then decides what the owner sees instead.
    C4 adds the parent graph: a claim must name a row that exists, may not point
    at itself, may not take more than two links, and may not close a cycle — the
    same refusals orb_clock3.c repeats on the bytes it was handed.
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
    for i, r in enumerate(recs):
        par = int(r.get("parent", PARENT_NONE))
        if par == PARENT_NONE:
            continue
        if par == int(r["n"]):
            raise ValueError("elem[%d]: row %d names itself as its parent"
                             % (i, int(r["n"])))
        hops, cur, seen = 0, int(r["n"]), set()
        while True:
            if cur in seen:
                raise ValueError("elem[%d]: the parent chain of row %d is a cycle"
                                 % (i, int(r["n"])))
            seen.add(cur)
            j = seen_n.get(par)
            if j is None:
                raise ValueError("elem[%d]: row %d names parent %d and no row in this "
                                 "table carries that tag" % (i, cur, par))
            hops += 1
            if hops > 2:
                raise ValueError("elem[%d]: row %d hangs three deep; the compositor "
                                 "carries two links" % (i, int(r["n"])))
            cur = int(recs[j]["n"])
            par = int(recs[j].get("parent", PARENT_NONE))
            if par == PARENT_NONE:
                break
    return recs


def check_clock_stamp(rec):
    """The clock section's dialect gate, host side (D42: v6 and nothing older). `rec`
    carries the index record's stamp and the blob reader's derived widths. Refusals
    name both the stride and the stamp, mirroring orb_clock3.c's sentences."""
    abi = rec.get("abi_max", APP_ABI)
    ew = rec.get("elem_words", ELEMENT_WORDS)
    spw = rec.get("spr_words") or 6
    if abi != CLOCK_ABI:
        raise ValueError("a clock section stamped geometry v%d; this reader's clock "
                         "dialect is v%d and nothing older (D42) — re-bake it"
                         % (abi, CLOCK_ABI))
    if ew != ELEMENT_WORDS:
        raise ValueError("clock rows are %d words under geometry v%d; that stamp writes "
                         "exactly %d" % (ew, abi, ELEMENT_WORDS))
    if spw != 6:
        raise ValueError("clock catalogue rows are %d words under geometry v%d; that "
                         "stamp writes 6" % (spw, abi))


def pack_elements(recs, cap=ELEM_MAX):
    """Checked pack of a clock table at the v6 width (33 words): real rows only, no
    padding to the cap. The appended columns default HERE (parent NONE, the rest 0)
    rather than at pack_record's door, so a row dict that names nothing about them
    carries exactly what the defaults say."""
    check_table(recs)
    if len(recs) > cap:
        raise ValueError("element table: %d rows exceed the cap of %d"
                         % (len(recs), cap))
    out = []
    for i, r in enumerate(recs):
        r = dict(r)
        r.setdefault("parent", PARENT_NONE)
        for c in ("delay", "cadence", "cad_arg", "ease") + D42_COLUMNS:
            r.setdefault(c, 0)
        out += pack_record(r, ELEMENT_FIELDS, "elem[%d]" % i)
    return out


def clock_tempo(recs, scalars=None):
    """The beat a v6 dial keeps, derived from its rows (D42 §4; the scheduler reads it
    as clk_theme's second_sweep and tick_rate): (sweep, rate). A seconds row that sweeps
    or runs stop-to-go asks for the sweep, at its own frame rate when a sweep names one
    (cad_arg), else at the fastest animation the dial carries (the plate's, the
    carriage's), else at SWEEP_TIER. A stepping seconds row asks for its N a second; a
    dial with no moving seconds row beats once a second. `scalars` are the section's
    (bg_fps: an animated plate's named rate). Mirrors orb_clock3.c's derivation."""
    sweep, rate, sweep_fps = 0, 0, 0
    for r in recs:
        if int(r["class"]) != CLASS_AUX or int(r["flags"]) & EF_HIDDEN:
            continue
        if SOURCES[int(r["src"])] != "second" or int(r.get("digit", 0)):
            continue
        cad = int(r.get("cadence", 0))
        if cad in (CAD_SWEEP, CAD_STOP_TO_GO):
            sweep = 1
        if cad == CAD_SWEEP:
            sweep_fps = max(sweep_fps, int(r.get("cad_arg", 0)))
        elif cad == CAD_STEP:
            rate = max(rate, int(r["cad_arg"]))
    if not sweep:
        return 0, rate or 1
    if sweep_fps:
        return 1, sweep_fps
    sc = scalars or {}
    fps = 0
    for r in recs:
        if int(r["class"]) == CLASS_PLATE and int(r["slot"]) == 0 and int(r["n_frames"]) > 1:
            if int(sc.get("bg_fps", 0)) > 0:           # the rate its author named
                fps = int(sc["bg_fps"])
        if int(r["class"]) == CLASS_AUX and int(r["n_frames"]) > 1 and int(r.get("delay", 0)):
            fps = max(fps, (1000 + int(r["delay"]) // 2) // int(r["delay"]))
    return 1, min(255, fps if fps >= 2 else SWEEP_TIER)


def check_clock_rows(recs, scalars=None):
    """D42's whole-table rules for the clock's moving rows, the refusals that need more
    than one row to state. Returns clock_tempo(recs). Mirrors orb_clock3.c:
      - stop-to-go rests until a jumping minute row releases it;
      - every stepping seconds row steps at one rate, the dial's beat;
      - at most one row is animated (the carriage, frames and a frame time); a row with
        frames and no frame time is one whose frames the app picks, and any number may;
      - a shadow twin follows a moving or still row, never another twin, in slot order;
      - a parent is a moving row, and a parent that turns shares its child's centre;
      - fonts and the fonts labels name (check_fonts)."""
    check_fonts(recs)
    aux = {int(r["slot"]): r for r in recs if int(r["class"]) == CLASS_AUX}
    by_n = {int(r["n"]): r for r in recs}
    minute_jumps = any(SOURCES[int(r["src"])] == "minute" and not int(r.get("digit", 0))
                       and int(r.get("cadence", 0)) == CAD_JUMP for r in aux.values())
    steps, animated = set(), []
    for slot in sorted(aux):
        r = aux[slot]
        where = "ptr%d" % slot
        if slot >= CLK_ROW_MAX:
            raise ValueError("%s: the clock seats %d moving rows (ptr0..ptr%d)"
                             % (where, CLK_ROW_MAX, CLK_ROW_MAX - 1))
        cad = int(r.get("cadence", 0))
        if cad == CAD_STOP_TO_GO and not minute_jumps:
            raise ValueError("%s: stop-to-go rests until the minute releases it, and no "
                             "minute row jumps" % where)
        if SOURCES[int(r["src"])] == "second" and cad == CAD_STEP and not int(r.get("digit", 0)):
            steps.add(int(r["cad_arg"]))
        if int(r["n_frames"]) > 1 and int(r.get("delay", 0)):
            animated.append(where)
        if int(r["flags"]) & EF_AUX_SHADOW:
            prev = aux.get(slot - 1)
            if prev is None:
                raise ValueError("%s: a shadow twin with no row before it — the twin "
                                 "takes its pose from the row in the slot under it" % where)
            if int(prev["flags"]) & EF_AUX_SHADOW:
                raise ValueError("%s: a shadow twin under another twin — a twin darkens "
                                 "a row, not a shadow" % where)
            if int(prev["n_frames"]) > 1 and not int(prev.get("delay", 0)):
                raise ValueError("%s: a shadow twin under a row whose frames the app "
                                 "picks — the twin would shade one frame of many" % where)
        par = int(r.get("parent", PARENT_NONE))
        if par != PARENT_NONE:
            p = by_n.get(par)
            if p is None or int(p["class"]) != CLASS_AUX:
                raise ValueError("%s: names parent %d, which is not a moving row — a row "
                                 "rides another row's pose" % (where, par))
            if int(p["span"]) and (int(p["x"]) + int(p["pivot_x"]) != int(r["x"]) + int(r["pivot_x"])
                                   or int(p["y"]) + int(p["pivot_y"]) != int(r["y"]) + int(r["pivot_y"])):
                raise ValueError("%s: rides a parent that turns about another centre — a "
                                 "child of a turning row shares its arbour" % where)
    if len(steps) > 1:
        raise ValueError("seconds rows step at %s a second: a dial keeps one beat"
                         % "/".join(str(n) for n in sorted(steps)))
    if len(animated) > 1:
        raise ValueError("%s are all animated: a dial carries one animated row (the "
                         "carriage)" % ", ".join(animated))
    return clock_tempo(recs, scalars)


def pack_elements_v3(recs, cap=ELEM_MAX):
    """The v3-width pack: every app section but the clock's. A column past the 23rd
    is refused at this door, not silently truncated — the writer cannot undo a claim."""
    check_table(recs)
    for i, r in enumerate(recs):
        par = r.get("parent", PARENT_NONE)
        later = [c for c, _t in ELEMENT_FIELDS[ELEMENT_WORDS_V3:]
                 if c != "parent" and int(r.get(c, 0))]
        if par != PARENT_NONE or later:
            raise ValueError("elem[%d] names %s at stride %d: an app row is 23 words and "
                             "carries none of the clock's appended columns"
                             % (i, ", ".join(later) or "parent %d" % par,
                                ELEMENT_WORDS_V3))
    if len(recs) > cap:
        raise ValueError("element table: %d rows exceed the cap of %d"
                         % (len(recs), cap))
    out = []
    for i, r in enumerate(recs):
        out += pack_record(r, ELEMENT_FIELDS[:ELEMENT_WORDS_V3], "elem[%d]" % i)
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
    bake fills that before it packs: blank is a starting point, not a default.
    C4's parent column defaults to the NONE sentinel — a zeroed row would name
    the plate — and delay to zero, which is the dial's own default tempo."""
    rec = {n: 0 for n, _ in ELEMENT_FIELDS}
    rec["parent"] = PARENT_NONE
    return rec

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
    assert len(SOURCES) == 18, "SOURCES grew; clk_types.h's CLK_SRC__MAX assert must agree"
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
    assert ELEMENT_WORDS == 33 == len(ELEMENT_FIELDS), "element row width moved"
    assert ELEMENT_WORDS_V3 == 23 and ELEMENT_FIELDS[23][0] == "parent" \
        and ELEMENT_FIELDS[24][0] == "delay", "C4's append moved"
    assert [n for n, _t in ELEMENT_FIELDS[25:]] == \
        ["cadence", "cad_arg", "ease", "dx", "dy", "end", "digit", "str"], "D41/D42's append moved"
    assert len(CLASS_NAMES) == 11, "class vocabulary moved; orb_assets.h's bound must agree"
    assert (len(SHAPES), len(ENDS), len(DIGITS), len(CADENCES)) == (4, 3, 5, 7), \
        "the moving row's vocabulary moved; clk_types.h must agree"
    assert class_id("hand") == CLASS_HAND and class_id("nope") is None
    assert class_id("aux") == CLASS_AUX == 8, "D34's class must append at 8, never move"
    assert class_id("preview") == CLASS_PREVIEW == 9, "preview must append at 9, never move"
    # A plain (app) hand row: the generic class rules still hold for every app.
    _e = dict(blank_element(), **{"n": 0, "class": CLASS_HAND, "slot": 0,
                                  "flags": EF_OS_ADV, "order": 1, "alpha": 255,
                                  "frame0": 0, "n_frames": 1, "x": 10, "y": 20,
                                  "w": 8, "h": 90, "pivot_x": 4, "pivot_y": 88,
                                  "radius": 0, "degrees": 9000, "scale16": 16})
    assert unpack_elements(pack_elements([_e]), 1)[0] == _e, "element row must round-trip"
    assert unpack_elements(pack_elements_v3([_e]), 1, stride=ELEMENT_WORDS_V3)[0] == _e
    # ---- D42: the moving row, accepted and refused, and round-tripped ----
    _ax = dict(blank_element(), **{"n": 5, "class": CLASS_AUX, "slot": 0, "order": 3,
                                   "alpha": 255, "frame0": 4, "n_frames": 1,
                                   "x": 300, "y": 120, "w": 40, "h": 8,
                                   "pivot_x": 4, "pivot_y": 4,
                                   "kind": SHAPE_ARM, "src": SOURCES.index("second"),
                                   "span": 3600, "zero": 0, "vmin": 0, "vmax": 60,
                                   "cadence": CAD_STEP, "cad_arg": 1, "end": END_WRAP})
    assert unpack_elements(pack_elements([_ax]), 1)[0] == _ax, "a row must round-trip"
    _fan = dict(_ax, slot=1, n=6, src=SOURCES.index("hour12"), span=1800, zero=-900,
                vmax=12, cadence=CAD_JUMP, cad_arg=0, end=END_FLYBACK, ease=400)
    _bar = dict(_ax, slot=2, n=7, span=0, dx=160, dy=-20, cadence=CAD_SWEEP, cad_arg=0,
                end=END_FLYBACK)
    _still = dict(_ax, slot=3, n=8, src=0, span=0, vmax=0, cadence=0, cad_arg=0)
    _units = dict(_ax, slot=4, n=9, kind=SHAPE_RING, src=SOURCES.index("date"),
                  digit=DIGIT_UNITS, vmin=0, vmax=10, span=-3600, cadence=CAD_JUMP,
                  cad_arg=0)
    _twin = dict(_still, slot=1, n=10, flags=EF_AUX_SHADOW)
    _label = dict(_still, slot=5, n=11, kind=SHAPE_TEXT, src=SOURCES.index("text"), w=120,
                  h=24, pivot_x=60, pivot_y=12, str=0xFF10, radius=4, digit=1, end=1)
    _font = dict(blank_element(), **{"n": 12, "class": CLASS_TEXT, "slot": 0, "w": 10, "h": 18,
                                     "n_frames": len(FONT_CHARSET), "pivot_x": 4, "pivot_y": 1})
    for _ok in (_fan, _bar, _still, _units, _twin, _label, _font):
        assert check_element(_ok, "sentinel-ok") == _ok
    check_fonts([_label, _font, dict(_font, n=13, slot=1, n_frames=len(FONT_ASCII))])
    for _bad, _why in (
        ([_label, _font], "names font1"),
        ([dict(_font, slot=FONT_SLOTS)], "seats 15 fonts"),
        ([dict(_font, n_frames=40)], "ASCII whole"),
        ([dict(_font, x=3)], "sits at 0,0"),
        ([dict(_font, pivot_x=0)], "a space of 0"),
    ):
        try:
            check_fonts(_bad)
            raise AssertionError("check_fonts accepted: %s" % _why)
        except ValueError as e:
            assert _why in str(e), (str(e), _why)
    assert check_table([_e, _ax])
    for _bad, _why in (
        (dict(_ax, kind=4), "not a row's shape"),
        (dict(_ax, kind=SHAPE_TEXT), "reads source text"),
        (dict(_label, cadence=CAD_JUMP), "names no motion"),
        (dict(_label, dx=3), "names no motion"),
        (dict(_label, digit=3), "aligns by"),
        (dict(_label, str=0xFFFF), "no font for tone 0"),
        (dict(_label, n_frames=2), "one panel"),
        (dict(_label, radius=12), "leaves no room"),
        (dict(_ax, str=1), "only a text row names fonts"),
        (dict(_ax, src=SOURCES.index("text")), "no input"),
        (dict(_ax, cadence=0, cad_arg=0), "names when it moves"),
        (dict(_ax, vmax=0), "not above vmin"),
        (dict(_ax, span=0), "goes nowhere"),
        (dict(_ax, span=3700), "within one turn"),
        (dict(_bar, dx=500), "466 px panel"),
        (dict(_ax, span=1800), "whole turn"),
        (dict(_bar, end=END_WRAP), "whole turn"),
        (dict(_ax, end=3), "not an end rule"),
        (dict(_ax, digit=5), "not a digit selector"),
        (dict(_ax, degrees=900), "angle at rest"),
        (dict(_still, dx=4), "still row"),
        (dict(_still, cadence=CAD_JUMP), "still row"),
        (dict(_ax, pivot_x=44), "outside its own"),
        (dict(_ax, n_frames=17, delay=60), "at most 16"),
        (dict(_ax, n_frames=65), "to pick from"),
        (dict(_twin, src=SOURCES.index("second"), cadence=CAD_SWEEP, span=3600, vmax=60),
         "whole pose"),
        (dict(_ax, cadence=CAD_STOP_TO_GO, cad_arg=0, src=SOURCES.index("minute")), "seconds hand"),
        (dict(_ax, cadence=CAD_DRAG, cad_arg=0), "calendar"),
        (dict(_ax, cadence=CAD_STEP, cad_arg=0), "step 0"),
        (dict(_ax, cadence=CAD_SWEEP, cad_arg=31), "30 at most"),
        (dict(_ax, cadence=CAD_JUMP, cad_arg=2), "takes no argument"),
        (dict(_ax, ease=120), "nothing to ease"),
        (dict(_fan, ease=1500), "at most a second"),
        (dict(_e, dx=3), "moving row's question"),
        (dict(_e, span=3600, vmax=12), "does not move"),
        (dict(_e, kind=1), "driven element"),
    ):
        try:
            check_element(_bad, "sentinel"); raise AssertionError("D42 let through: " + _why)
        except ValueError as e:
            assert _why in str(e), (str(e), _why)
    # ---- the table: the tempo is the rows', and the rows agree on it ----
    _min = dict(_ax, slot=5, n=11, src=SOURCES.index("minute"), cadence=CAD_JUMP, cad_arg=0)
    _stg = dict(_ax, slot=6, n=12, cadence=CAD_STOP_TO_GO, cad_arg=585)
    assert check_clock_rows([_ax]) == (0, 1)
    assert check_clock_rows([dict(_ax, cad_arg=4)]) == (0, 4)
    assert check_clock_rows([_min, _stg]) == (1, SWEEP_TIER)
    assert check_clock_rows([dict(_ax, cadence=CAD_SWEEP, cad_arg=8)]) == (1, 8)
    assert check_clock_rows([dict(_ax, cadence=CAD_SWEEP, cad_arg=0),
                             dict(_still, slot=9, n=20, n_frames=4, delay=100)]) == (1, 10)
    assert check_clock_rows([_ax, _twin]) == (0, 1)
    for _rows, _why in (([_stg], "releases"),
                        ([dict(_ax, slot=32)], "seats 32 moving rows"),
                        ([_ax, dict(_ax, slot=7, n=13, cad_arg=2)], "one beat"),
                        ([dict(_ax, n_frames=4, delay=60),
                          dict(_ax, slot=7, n=13, n_frames=4, delay=60)], "one animated"),
                        ([dict(_twin, slot=0)], "no row before"),
                        ([_ax, _twin, dict(_twin, slot=2, n=14)], "another twin"),
                        ([_ax, dict(_fan, parent=5, x=0)], "another centre"),
                        ([_e, dict(_fan, parent=0 + 0 or 99)], "not a moving row")):
        try:
            check_clock_rows(_rows); raise AssertionError(_why)
        except ValueError as e:
            assert _why in str(e), (str(e), _why)
    # A D32-width row still reads: the columns past it land on the zeros they were
    # written after, which is the append-only promise the stride derivation exists for.
    _narrow = unpack_elements(pack_record(_e, ELEMENT_FIELDS[:19], "e"), 1, stride=19)[0]
    assert _narrow == _e, "a narrow row must widen"
    def _with(**over):
        r = dict(_e)
        r.update({str(k): v for k, v in over.items()})
        return r
    assert check_table([_e, _with(n=1, **{"class": CLASS_TEXT, "slot": 3, "radius": 0,
                                          "pivot_x": 0, "pivot_y": 0, "flags": 0})])
    for _bad, _why in (
        (_with(**{"class": len(CLASS_NAMES)}), "unknown class"),
        (_with(**{"class": 200}), "class far past the vocabulary"),
        (_with(flags=EF_ALPHA | 0x100), "unknown flag bit"),
        (_with(flags=EF_SMOOTH, **{"class": CLASS_RING, "slot": 1, "radius": 40,
                                   "pivot_x": 0, "pivot_y": 0}), "smoothing off a turned row"),
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
        (_with(flags=EF_DELTA_WRAP), "a closing delta on a hand row"),
        (_with(**{"class": CLASS_PREVIEW, "n_frames": 2}), "an animated preview"),
        (_with(**{"class": CLASS_PREVIEW, "kind": 1}), "a preview naming a driven kind"),
        (_with(**{"class": CLASS_PREVIEW, "src": 1}), "a preview naming a driven source"),
    ):
        try:
            check_element(_bad, "sentinel")
        except ValueError:
            pass
        else:
            raise AssertionError("check_element accepted %s" % _why)
    try:
        pack_elements_v3([_ax]); raise AssertionError("an app pack carried a clock column")
    except ValueError as e:
        assert "23 words" in str(e), str(e)
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
    assert clock_slot("ptr31") == ("aux", 31) and clock_slot("carriage") is None
    print("             %d clock slots, %d drawn, names within %d chars"
          % (len(CLOCK_SLOTS), len(CLOCK_SLOTS_DRAWN), 11))
