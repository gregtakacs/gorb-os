#!/usr/bin/env python3
"""The `.orbapp` container — one installable card app in one file (plan §4.1).

`tools/orbapp.py` is the ONLY sanctioned writer of this container (ruling C11),
and it stands beside `tools/orbtheme.py` on purpose: same all-little-endian u32
word discipline, same "no field narrower than a word, so no padding rule can
disagree", same header-CRC32-plus-two-payload-CRC32 shape. §8's P4 line runs the
install ladder against files this module wrote, so what is refused HERE is what
the device never has to survive.

WHY THE MANIFEST SITS INSIDE THE CHECKSUMMED HEADER. The capability bitmask at
W_CAPS is not data, it is the owner's consent (ruling D19, §0.8): the declared
set IS the grant and it is also the ceiling the dial can never raise. A word
that lived in the payload could be rewritten on the card without disturbing any
checksum, and an app would wake up holding `net` it never asked for. So every
field the broker's gate consults — caps, period_ms, the settings table's count
and offset — sits inside the header block `hdr_crc` covers, and a one-byte edit
anywhere in them fails a checksum before a payload is streamed. The theme
container has no equivalent stake, which is why its geometry blob may live
outside.

Layout (§4.1's words, in §4.1's order; every name below is the paper's):

  header + manifest   HDR_WORDS words, word-wide, CRC32'd whole
    magic 'GRA'  FORMAT  GEOM_ABI  hdr_size  serial  baked_ms
    abi_min  abi_max          <- C4: the OS refuses one not covering its own ABI
    sig                       <- C3: reserved, 0 today; signing moves nothing
    app_id  slug[24]  title[32]
    behaviour_kind            <- opaque: D15 refuses to validate it
    art_req  n_layer n_comp n_aux n_bank
    period_ms  caps
    n_settings  setting_off
    geom_off geom_size geom_crc   art_off art_size art_crc
    hdr_crc
  settings   n_settings x SETTINGS_WORDS words: id[16] type min max def
    def_str[32]               <- C9's four types, frozen
  geometry   the app's own records, OPAQUE to the OS (D15)
  payload    NONE since D21 (§0.9): art_size and art_crc are written 0 and the
             file ends at the sector pad; art_req is a declaration, not bytes

Three rules this writer enforces, all of them from `.clinerules:444-458` and
§4.1's own bullets:

  * app_id == slug == the filename stem, checked through ONE helper (§4.1's
    `filed_as_its_own_slug()` shape) reached by upload, scan, boot and stage.
    The theme path already learned that an identity written in two places must
    be checked in one: a package that disagrees with its own filename reads as a
    broken install on the dial and as a working file on the bench, and the owner
    gets shown the wrong one of the two.
  * the four FREE caps (time, config, ui, art) are refused as DECLARATIONS.
    §4.2 grants them unconditionally, so recording one in the mask would put a
    revocation on the owner's ladder that nothing implements, and would let an
    app's ledger row claim a permission no code consults — the exact complaint
    D19 made against B3. Only power, net, sd_read and themes may be declared.
  * a half-written file is invisible: `.part` names are refused here, and the
    scan never globs them (§4.1's upload rule).

Geometry content is never inspected, range-checked or refused: the OS registers
no behaviour vocabulary (D15), so `behaviour_kind` and the geometry bytes are
the app's own dialect. What the OS does price is the SIZE — geom_size — because
that is what §3.2's slot arithmetic is about. Art is priced nowhere because
D21 (§0.9) withdrew it: `.orbapp` never ships art bytes, so art_size and art_crc
are always 0 and this writer refuses to write otherwise; art_req survives as
D18's request-as-declaration, answered at runtime by the theme and asset
registries, and bounded here only against mis-count.

The three-way mirror rule from tools/theme_schema.py applies to this file: the
Python writer, the C reader (main/app/orb_app_manifest.c), and the sentinel
tools/host_orbapp_reader_test.py move together. The mirror stands on all
THREE legs: the sentinel compiles the shipping reader with host
gcc and feeds it this writer's bytes, device refusals and pack-tool refusals
pinned to each other — in the same words, not merely the same verdicts.
"""

import hashlib
import os
import re
import struct
import zlib

MAGIC      = 0x00415247        # 'G','R','A'. Deliberately unlike the theme's
                               # 0x474F5254, so a .orbtheme handed to the app
                               # loader dies on the magic word rather than
                               # parsing as an app with a surprising name.
FORMAT     = 1                 # this container layout; a reader refuses others
GEOM_ABI   = 2                 # The dialect this writer stamps. 1 -> 2 is P5b's
                               # asset table (D23): the geometry records did not
                               # move, the vocabulary did, and D23 names THIS word
                               # as the class enum's version so there is exactly one
                               # number that says whether a dial can read a manifest.
                               # C4's ladder refuses anything outside abi_min..abi_max,
                               # so an app that needs a class this build cannot spell
                               # dies with that sentence and not as a blank dial.
SECTOR     = 4096              # payload alignment, as the theme path's is
PAGE       = 65536             # the MMU page an art bank is cut on

SLUG_MAX   = 24                # header bytes, NUL included (23 usable chars)
TITLE_MAX  = 32                # header bytes, NUL included (31 usable chars)
ID_MAX     = 16                # settings id bytes; the NVS key limit is 15+NUL
DEFSTR_MAX = 32                # STRING default bytes, NUL included

# D21 (§0.9) withdrew the container's art payload: art_size and art_crc are
# always 0 and pack_header refuses otherwise. ART_BYTES_MAX survives as the
# plausibility bound on art_req — a DECLARED need past the old slot size is a
# mis-count, not a wish — and the C reader mirrors this number and this rule.
ART_BYTES_MAX  = 0x003D0000    # 3.81 MB — art_req DECLARATION bound, nothing more
SETTINGS_MAX   = 16            # the descriptor table the loader is sized for
GEOM_BYTES_MAX = 0x00040000    # 256 KB of the app's own records

# D16 (§0.6): the manifest's period_ms is the FASTEST beat the app may ever run,
# which is the number the owner prices the app at on the install confirm. 0 is a
# real answer — no tick at all, the vector-only app. Below the floor it is not an
# answer but a request for frames the dial cannot commit: the measured commit pass
# costs 185 ms, so 3000 ms is the honest lower bound for a beat the shared
# scheduler can honour, and this writer will not emit a manifest promising what
# the OS cannot deliver.
BEAT_MIN_MS = 3000
BEAT_MAX_MS = 86400000         # a day; past this the app means "never", and 0
                               # is the word for that

# The owner-granted caps, in orb_svc.h's orb_cap_t order (POWER=4 NET=5
# SD_READ=6 THEMES=7). tools/host_orbapp_test.py lifts the C enum and fails if
# this tuple's order ever disagrees: a silent renumber would move every installed
# app's D19 consent onto a different permission.
CAP_FREE  = ("time", "config", "ui", "art")     # declarable NEVER
CAP_OWNER = ("power", "net", "sd_read", "themes")
CAP_BITS  = len(CAP_FREE) + len(CAP_OWNER)      # 8 == CAP_N

# C9's settings types, frozen, in orb_svc.h's orb_set_type_t order.
SET_TYPES = ("int", "enum", "toggle", "string")
CFG_QUOTA_BYTES = 2048                          # ORB_CFG_QUOTA_BYTES (§9.4.4)


def cap_bit(name):
    """The wire bit of an owner cap name: its orb_cap_t number, never a re-order."""
    return 1 << (len(CAP_FREE) + CAP_OWNER.index(name))


CAP_OWNER_MASK = 0
for _n in CAP_OWNER:
    CAP_OWNER_MASK |= cap_bit(_n)


# ---- the header word map ----------------------------------------------------
# Derived from the field widths, never spelled twice. tools/orbtheme.py records
# the bug this prevents: FORMAT 1 hand-wrote two name ranges, and a long slug
# silently overwrote the head of the title. Here the ranges come from the sizes,
# so a collision moves both the size and the test.
W_MAGIC      = 0
W_FORMAT     = 1
W_GEOM_ABI   = 2
W_HDR_SIZE   = 3
W_SERIAL     = 4
W_BAKED_MS   = 5
W_ABI_MIN    = 6
W_ABI_MAX    = 7
W_SIG        = 8
W_APP_ID     = 9
W_SLUG       = 10                                   # SLUG_MAX bytes
W_TITLE      = W_SLUG      + ((SLUG_MAX   + 3) // 4)
W_BEHAVIOUR  = W_TITLE     + ((TITLE_MAX  + 3) // 4)
W_ART_REQ    = W_BEHAVIOUR + 1
W_N_LAYER    = W_ART_REQ   + 1
W_N_COMP     = W_N_LAYER   + 1
W_N_AUX      = W_N_COMP    + 1
W_N_BANK     = W_N_AUX     + 1
W_PERIOD_MS  = W_N_BANK    + 1
W_CAPS       = W_PERIOD_MS + 1
W_N_SETTINGS = W_CAPS      + 1
W_SET_OFF    = W_N_SETTINGS + 1
W_GEOM_OFF   = W_SET_OFF   + 1
W_GEOM_SIZE  = W_GEOM_OFF  + 1
W_GEOM_CRC   = W_GEOM_SIZE + 1
W_ART_OFF    = W_GEOM_CRC  + 1
W_ART_SIZE   = W_ART_OFF   + 1
W_ART_CRC    = W_ART_SIZE  + 1
W_HDR_CRC    = W_ART_CRC    + 1
# P5b (D23): the asset table's two fields take words this header left reserved for
# exactly this, so HDR_WORDS does not move and the header CRC still covers up to
# W_HDR_CRC. One word at the end stays free and the reader checks it zero.
W_ASSET_OFF  = W_HDR_CRC    + 1
W_N_ASSETS   = W_ASSET_OFF  + 1
HDR_WORDS    = W_HDR_CRC   + 4                      # 2 taken by P5b, 2 still free
HDR_BYTES    = HDR_WORDS * 4
SET_OFF      = HDR_BYTES                            # settings follow the header

# One settings descriptor on the wire, mirroring orb_setting_desc_t field for
# field: char id[16], orb_set_type_t type, int32 min, max, def, char def_str[32].
S_ID, S_TYPE, S_MIN, S_MAX, S_DEF, S_STR = 0, 4, 5, 6, 7, 8
SETTINGS_WORDS = S_STR + ((DEFSTR_MAX + 3) // 4)    # 16 words = 64 B

SLUG_RE = re.compile(r"^[a-z0-9_]{1,%d}$" % (SLUG_MAX - 1))
SET_RE  = re.compile(r"^[a-z0-9_]{1,%d}$" % (ID_MAX - 1))
EXT       = ".orbapp"
PART_EXT  = ".orbapp.part"
THEME_EXT = ".orbtheme"


def _u(v, what="value"):
    if not isinstance(v, int) or isinstance(v, bool):
        raise ValueError("%s must be an integer, got %r" % (what, v))
    if v < 0 or v > 0xFFFFFFFF:
        raise ValueError("%s %r does not fit a u32 word" % (what, v))
    return v


def _s32(v):
    """Wire u32 -> the int32_t the C reader sees. min/max/def are signed fields
    in orb_setting_desc_t, so an unsigned-only round trip would turn a -10 into
    4294967286 and the config gate would refuse the app's own default."""
    return v - 0x100000000 if v & 0x80000000 else v


def _words(b):
    return [int.from_bytes(b[i:i + 4], "little") for i in range(0, len(b), 4)]


def _putstr(buf, base, text, nbytes, what):
    b = text.encode("utf-8")
    # Refuse, never truncate: these bytes ARE the identity (the filename is
    # derived from them and the loader refuses a disagreement), so silently
    # cutting one in half would file an app under a name nobody typed and make
    # its own manifest look corrupt. Same rule as orbtheme._setword.
    if len(b) > nbytes - 1:
        raise ValueError("%s is %d UTF-8 bytes; the header holds at most %d"
                         % (what, len(b), nbytes - 1))
    for i, c in enumerate(b):
        buf[base + i // 4] |= c << (8 * (i % 4))


def _getstr(buf, base, nbytes):
    out = bytearray()
    for i in range(nbytes):
        c = (buf[base + i // 4] >> (8 * (i % 4))) & 0xFF
        if c == 0:
            break
        out.append(c)
    return out.decode("utf-8", "replace")


# ---- the D19 consent: caps --------------------------------------------------


def check_caps(caps):
    """Validate a declared capability mask and return it, or raise with the
    reason in the words the install confirm will show.

    This is the consent check, not a range check. Once the file is on a card an
    over-declared cap is a permission the owner never read, and the only surface
    that shows it is the ladder D19 rebuilt. A refusal here is the cheap half of
    that check.
    """
    _u(caps, "caps")
    if caps >> CAP_BITS:                            # a bit at or above CAP_N
        raise ValueError(
            "caps 0x%X sets a bit at or above CAP_N (%d): this build's orb_cap_t "
            "stops at 'themes'. Refused rather than ignored, because a cap this "
            "OS cannot gate must not install as one it can." % (caps, CAP_BITS))
    free = caps & ~CAP_OWNER_MASK
    if free:
        named = [CAP_FREE[i] for i in range(len(CAP_FREE)) if free & (1 << i)]
        raise ValueError(
            "declares %s: §4.2 grants the free caps unconditionally, so "
            "declaring one buys nothing and costs the owner a revocation the OS "
            "does not implement. Declare only %s."
            % (", ".join("'%s'" % n for n in named), ", ".join(CAP_OWNER)))
    return caps


def caps_names(caps):
    """The declared list in orb_cap_t order — the words the disclosure prints."""
    return [n for n in CAP_OWNER if caps & cap_bit(n)]


def disclosure(slug, title, caps, period_ms, n_settings):
    """The install-time disclosure text (D19, §4.2's last sentence).

    The words the owner consents to, built from the same bytes the gate will
    consult. One function rather than strings at each call site because P4's UI,
    this CLI's --disclose and the host suite must print exactly the same thing:
    a disclosure the reader can disagree with is the ceremony D19 deleted.
    Art is not in the text: D21 (§0.9) withdrew the container's payload, so an
    app ships no pixels to consent to — pixels arrive only in theme bundles.
    """
    beat = ("no animation (period_ms 0 = never ticks)" if period_ms == 0
            else "%d ms fastest beat (set_beat may only slow from here)" % period_ms)
    caps_txt = ", ".join(caps_names(caps)) or "(nothing declared)"
    return ('Install "%s" (%s)\n'
            '  may use:    %s\n'
            '  will run:   %s\n'
            '  will store: %d setting(s) inside a %d B quota\n'
            'Revoke any of it on the dial: MENU -> PERMISSIONS -> %s.'
            % (title, slug, caps_txt, beat, n_settings,
               CFG_QUOTA_BYTES, slug))


# ---- the identity rules -----------------------------------------------------


def check_slug(slug):
    """An app id is lowercase a-z, digits and underscore, at most 23 chars
    (.clinerules:450-452). One charset for three consumers — the NVS namespace,
    the ?to= slug and the card filename — because a name that survives being
    copied between all three is the only kind a person can debug."""
    if not isinstance(slug, str) or not SLUG_RE.match(slug):
        raise ValueError(
            "app id %r is not lowercase a-z 0-9 _ of 1..%d chars: it is the NVS "
            "namespace, the ?to= route and the filename stem at once, and a "
            "character any of those three treats specially installs an app that "
            "cannot be found again" % (slug, SLUG_MAX - 1))
    return slug


def app_id_of(slug):
    """app_id is the first four bytes of sha256(UTF-8 slug), big-endian
    (.clinerules:450-452). A slug typed by hand drifts from a slug pasted into a
    manifest, and a 32-bit cross-check catches the disagreement without a second
    naming authority kept in step across reflashes. The firmware mirrors this
    derivation byte for byte; the host suite pins the pair on known answers the
    way theme_id() is pinned."""
    check_slug(slug)
    return int.from_bytes(hashlib.sha256(slug.encode("utf-8")).digest()[:4], "big")


def filed_as_its_own_slug(path, app_id, slug):
    """The one helper upload, scan, boot and stage all reach (§4.1). Returns the
    stem on agreement, and names both spellings on any disagreement.

    Refusing a `.part` or `.orbtheme` name HERE, and not leaning on the magic
    word alone, is the belt-and-braces the bench rules ask for: the two
    containers do have different magics, but they share a card, a page and an
    owner, and "that is a theme" is a truer reason than "that is not a
    container".
    """
    name = os.path.basename(str(path))
    low = name.lower()
    if low.endswith(THEME_EXT):
        raise ValueError("%s is a theme, not an app: themes live in /sd/themes "
                         "and the OS package manager alone installs them" % name)
    if low.endswith(PART_EXT):
        raise ValueError("%s is a staging name: a .part file is invisible to the "
                         "scan by design and nothing is ever committed under it"
                         % name)
    if not low.endswith(EXT):
        raise ValueError("%s does not end in %s" % (name, EXT))
    stem = name[: -len(EXT)]
    check_slug(stem)
    if stem != slug:
        raise ValueError("filename stem '%s' is not the header's slug '%s': the "
                         "app would install where nothing looks for it"
                         % (stem, slug))
    want = app_id_of(stem)
    if app_id != want:
        raise ValueError("header app_id 0x%08x is not the slug '%s' it claims "
                         "(that slug is 0x%08x): mis-filed or hand-edited"
                         % (app_id, stem, want))
    return stem



# ---- the settings table (C9, frozen) ----------------------------------------


def _s(v, what):
    """An int32_t the C reader can actually hold. Returns the SIGNED value: the
    bounds are compared signed, because a min of -10 masked to 4294967286 would
    otherwise fail `min <= max` and refuse a legitimate range with a message
    about the app's own descriptor."""
    if not isinstance(v, int) or isinstance(v, bool):
        raise ValueError("%s must be an integer, got %r" % (what, v))
    if v < -0x80000000 or v > 0x7FFFFFFF:
        raise ValueError("%s %r does not fit the int32_t the device reads it as"
                         % (what, v))
    return v


def check_settings(settings):
    """Normalise and validate the declared settings, returning the wire rows.

    Every bound is what the config gate really enforces: orb_svc.c's find_setting
    resolves an id BEFORE any NVS handle opens, and svc_cfg_set refuses outside
    min..max, so a package that passes this cannot ship a setting its app is
    unable to write. Returning rows instead of mutating the caller's dicts is
    deliberate — pack_settings consumes exactly what was checked, so nothing can
    slip between the two, and nothing can be added after the check.
    """
    if not isinstance(settings, (list, tuple)):
        raise ValueError("settings must be a list of dicts, got %r" % (settings,))
    if len(settings) > SETTINGS_MAX:
        raise ValueError("%d settings; the loader's table holds %d"
                         % (len(settings), SETTINGS_MAX))
    rows, seen = [], set()
    for s in settings:
        if not isinstance(s, dict):
            raise ValueError("setting %r is not a dict" % (s,))
        sid = s.get("id")
        if not isinstance(sid, str) or not SET_RE.match(sid):
            raise ValueError(
                "setting id %r is not lowercase a-z 0-9 _ of 1..%d chars: it is "
                "the NVS key inside orb.app<slot>, and the 15-char key limit is "
                "the reason, not a style rule" % (sid, ID_MAX - 1))
        if sid in seen:
            raise ValueError("setting id '%s' declared twice: the gate resolves "
                             "the first row, so the second is unreachable and the "
                             "app reads a value it never set" % sid)
        seen.add(sid)
        tname = s.get("type")
        if tname not in SET_TYPES:
            raise ValueError("setting '%s' type %r is not one of %s (C9, frozen)"
                             % (sid, tname, ", ".join(SET_TYPES)))
        t = SET_TYPES.index(tname)
        dstr = s.get("def_str", "") or ""
        if t != SET_TYPES.index("string"):
            if dstr:
                raise ValueError("setting '%s' carries a def_str but its type is "
                                 "'%s': only STRING has a string default, and the "
                                 "gate will never read this one" % (sid, tname))
            mn = _s(s.get("min", 0), "setting '%s' min" % sid)
            mx = _s(s.get("max", 0), "setting '%s' max" % sid)
            df = _s(s.get("default", s.get("def", 0)),
                    "setting '%s' default" % sid)
            if t == SET_TYPES.index("toggle"):
                # The gate accepts 0 or 1 whatever the descriptor claims, so a
                # manifest that disagrees describes a setting no one can use.
                # Frozen here rather than argued at read time.
                mn, mx = 0, 1
                if df not in (0, 1):
                    raise ValueError("setting '%s' is a toggle and defaults to %d: "
                                     "svc_cfg_set returns INVALID_ARG for anything "
                                     "but 0 or 1" % (sid, df))
            elif mn > mx:
                raise ValueError("setting '%s' has min %d above max %d: the gate "
                                 "refuses every value, this default included"
                                 % (sid, mn, mx))
            elif not (mn <= df <= mx):
                raise ValueError("setting '%s' defaults to %d, outside its own "
                                 "min..max %d..%d: first run hands the app a value "
                                 "the app can never write back"
                                 % (sid, df, mn, mx))
        else:
            if len(dstr.encode("utf-8")) > DEFSTR_MAX - 1:
                raise ValueError("setting '%s' default is %d UTF-8 bytes; the "
                                 "descriptor holds %d"
                                 % (sid, len(dstr.encode("utf-8")), DEFSTR_MAX - 1))
            mn = mx = df = 0
            # A STRING pays strlen+1 against the quota at EVERY set, not at this
            # default's length: the charge lives in cfg_charge() and in the
            # broker's own accounting (orb_svc.c), which §9.4.4 priced.
        rows.append((sid, t, mn, mx, df, dstr))
    return rows


def cfg_charge(rows):
    """The NVS bytes the declared DEFAULTS imply, against ORB_CFG_QUOTA_BYTES.

    Reported, not refused, and the reason is worth keeping: SETTINGS_MAX (16) x
    DEFSTR_MAX (32) is 512 B, so no legal manifest can exceed the 2048 B quota
    with its defaults. The bytes that actually consume the quota are the values
    the app writes at runtime, and orb_svc.c's cfg_set_str already charges and
    refuses those — tested by host_svc_test.py. A writer-side refusal here would
    be a check that can never fire, which is worse than none: it reads as
    coverage and is a comment.
    """
    return sum(4 if t != SET_TYPES.index("string") else len(d) + 1
               for _, t, _, _, _, d in rows)


def pack_settings(rows):
    """The settings blob, in orb_setting_desc_t's declaration order.

    Takes check_settings' OWN output. Re-validating the caller's raw dicts here
    would let a dict mutated between the two calls reach the wire unchecked, so
    the only accepted input is the tuple the validator returned."""
    out = []
    for sid, t, mn, mx, df, dstr in rows:
        w = [0] * SETTINGS_WORDS
        _putstr(w, S_ID, sid, ID_MAX, "setting id")
        _putstr(w, S_STR, dstr, DEFSTR_MAX, "setting default")
        w[S_TYPE] = t
        w[S_MIN], w[S_MAX], w[S_DEF] = (mn & 0xFFFFFFFF, mx & 0xFFFFFFFF,
                                        df & 0xFFFFFFFF)
        out += w
    return b"".join(struct.pack("<I", v) for v in out)


def unpack_settings(blob, n):
    """Read n descriptors back — pack_settings' inverse and the C reader's mirror."""
    if n > SETTINGS_MAX:
        raise ValueError("%d settings; the loader's table holds %d"
                         % (n, SETTINGS_MAX))
    need = n * SETTINGS_WORDS * 4
    if len(blob) < need:
        raise ValueError("settings blob is %d B, short of the %d B its count claims"
                         % (len(blob), need))
    w = _words(blob[:need])
    out = []
    for i in range(n):
        b = w[i * SETTINGS_WORDS:(i + 1) * SETTINGS_WORDS]
        t = b[S_TYPE]
        if t >= len(SET_TYPES):
            raise ValueError("setting %d has type word %d, outside C9's frozen %d"
                             % (i, t, len(SET_TYPES)))
        out.append({"id": _getstr(b, S_ID, ID_MAX), "type": SET_TYPES[t],
                    "min": _s32(b[S_MIN]), "max": _s32(b[S_MAX]),
                    "default": _s32(b[S_DEF]),
                    "def_str": _getstr(b, S_STR, DEFSTR_MAX)})
    return out


# ---- the header -------------------------------------------------------------


# ---- the asset vocabulary and its table (D23, D31; P5b; D34) ---------------
# The nine classes, in main/assets/orb_assets.h's orb_asset_class_t order. The
# sentinel lifts that enum and fails if this tuple's order ever disagrees — the same
# rule CAP_OWNER obeys, and for the same reason: the wire carries an index into this
# list, so a silent renumber would resolve every installed app's `dial` slot as
# somebody else's plate and nothing downstream could tell.
# `aux` is D34's append at the END, which is the only place a class may enter: a
# manifest written before it indexes 0..7 and reads unchanged, and an OS that predates
# it refuses a row naming 8 at its own bound instead of drawing a pointer that sits
# still. An app offers it the way it offers a hand — one named slot, one plane — and
# what moves is the row's own drive, which the manifest has no business naming.
ASSET_CLASSES = ("text", "sprite", "hand", "plate", "ring", "bank", "layer", "status",
                 "aux")
ASSET_MAX      = 16            # rows in one manifest's table; == ORBAPP_ASSETS_MAX
ASSET_NAME_MAX = 12            # bytes incl NUL, and the name carries NO prefix
ASSET_OWNER_MAX = SLUG_MAX - 1 # the `<slug>` half of an owner-scoped reference
# 36, and the arithmetic says why: an owner reference is a whole slug, a slash and a
# whole name, and the field that holds it must be sized for the widest shape and not
# for the `global/` one. Sized from the two bounds above so a row that grows on the
# C side moves this number the same way.
ASSET_REF_MAX  = ASSET_OWNER_MAX + 1 + ASSET_NAME_MAX   # ORB_ASSET_REF_MAX
PANEL          = 466           # CLK_SCR_W/H: the panel a declared box must fit

# One row on the wire, mirroring orb_app_manifest.h's ORBAPP_A_*: word 0 packs
# class | required<<8 | ns<<16 with byte 3 reserved, words 1-3 hold the name, and
# words 4-5 are x|y and w|h. Six words, so 16 rows cost 384 B and the worst-case
# blob (1024 settings + 640 strings + 384 assets) fits the slot header's slack.
A_CLASS, A_NAME, A_XY, A_WH = 0, 1, 4, 5
ASSET_WORDS = 6

ASSET_RE = re.compile(r"^[a-z0-9_]{1,%d}$" % (ASSET_NAME_MAX - 1))


def asset_class(name):
    """The wire index of a class name. A name outside the frozen eight is refused by
    the writer, because a class this build cannot spell is a slot that can never
    resolve, and D26 says an unresolved slot is not a thing to install."""
    if name not in ASSET_CLASSES:
        raise ValueError("asset class %r is not one of the eight the registry carries "
                         "(%s): a PR promotes a class, a manifest never does"
                         % (name, ", ".join(ASSET_CLASSES)))
    return ASSET_CLASSES.index(name)


def _asset_words(i, r):
    """One validated row's six wire words. Every refusal here names the row, because
    a table of sixteen with one bad box is otherwise a hunt."""
    cls = asset_class(r["cls"])
    req = r.get("required", 0)
    if req not in (0, 1, True, False):
        raise ValueError("asset %d's required flag is %r: D31's selector is one bit, "
                         "and a value that is neither is a caller guessing"
                         % (i, req))
    ns = r.get("ns", "owner")
    if ns not in ("owner", "global"):
        raise ValueError("asset %d names namespace %r: there are two, the app's own "
                         "and global/, and nothing between them (D23)" % (i, ns))
    name = r["name"]
    if not isinstance(name, str) or not ASSET_RE.match(name):
        raise ValueError("asset %d's name %r is not lowercase a-z 0-9 _ of 1..%d chars:"
                         " it is the half of a reference the resolver splits, and a"
                         " character it cannot split is a slot nobody can re-request"
                         % (i, name, ASSET_NAME_MAX - 1))
    x, y, w, h = (r.get("x", 0), r.get("y", 0), r.get("w", 0), r.get("h", 0))
    for v, nm in ((x, "x"), (y, "y"), (w, "w"), (h, "h")):
        if not isinstance(v, int) or isinstance(v, bool):
            raise ValueError("asset %d's %s is %r, not a count" % (i, nm, v))
    if x < 0 or y < 0 or x + w > PANEL or y + h > PANEL:
        raise ValueError("asset %d (%s) is a %dx%d box at %d,%d, which is not inside"
                         " the %dx%d panel: a slot off the edge draws nothing and still"
                         " costs the rows it marks" % (i, name, w, h, x, y, PANEL, PANEL))
    words = [0] * ASSET_WORDS
    words[A_CLASS] = (cls | (1 if req else 0) << 8
                      | (ASSET_NS_GLOBAL << 16 if ns == "global" else 0))
    for j, b in enumerate(name.encode()):
        words[A_NAME + j // 4] |= b << (8 * (j % 4))
    words[A_XY] = (x & 0xFFFF) | ((y & 0xFFFF) << 16)
    words[A_WH] = (w & 0xFFFF) | ((h & 0xFFFF) << 16)
    return words


def check_assets(rows):
    """Validate a declared table and return its wire words — the writer's door, and
    the same rules the C reader re-applies to bytes it did not write."""
    if isinstance(rows, dict):
        rows = [rows]
    if len(rows) > ASSET_MAX:
        raise ValueError("%d assets declared; the loader's table holds %d"
                         % (len(rows), ASSET_MAX))
    words = []
    for i, r in enumerate(rows):
        words.extend(_asset_words(i, r))
    return words

ASSET_NS_OWNER, ASSET_NS_GLOBAL = 0, 1


def asset_ref(r, slug):
    """The reference a row means in full: `global/<name>`, or the app's own slug with
    the name. Assembled in one place, because the resolver splits it again and a row
    whose assembled form the splitter rejects is a table that would install a slot
    nothing can ask for."""
    return (("global/" if r.get("ns", "owner") == "global" else slug + "/") + r["name"])


def pack_assets(rows, slug):
    """check_assets' rows as wire bytes, cross-checked against the app's OWN slug.

    The row's namespace byte says which half of the registry it lives in, so an
    owner-scoped row's full reference is `<slug>/<name>` and the slug is the row's
    subject. A container that declares a slot in a namespace it does not own is
    refused here, before the bytes exist: the identity triple the C reader checks
    (slug, app_id, stem) gains a fourth member, and a mismatch is the same class of
    bug as a file whose name disagrees with its own header."""
    words = check_assets(rows)
    seq = [rows] if isinstance(rows, dict) else list(rows)
    for i, r in enumerate(seq):
        if r.get("ns", "owner") == "global":
            continue
        full = asset_ref(r, slug)
        if len(full.encode()) + 1 > ASSET_REF_MAX:
            raise ValueError("asset %d's reference for slug %r is %d B, past the %d the"
                             " registry's own field holds: the name shrinks, not the"
                             " slug, which is an NVS namespace"
                             % (i, slug, len(full.encode()) + 1, ASSET_REF_MAX))
    return b"".join(struct.pack("<I", w) for w in words)


def _asset_name(words, base):
    """The name half of the row at word offset `base`, with the reader's termination
    rule: the first NUL ends it, and a field full of bytes with none is damage."""
    b = bytearray()
    for j in range(ASSET_NAME_MAX):
        c = (words[base + A_NAME + j // 4] >> (8 * (j % 4))) & 0xFF
        if not c:
            break
        b.append(c)
    return b.decode("utf-8", "replace")


def unpack_assets(blob, n=None):
    """Decode an asset table into dicts — orbapp_unpack_assets' twin. The writer's
    reader is not its own writer run backwards: the bytes it checks came from
    somewhere else, which is the whole reason the function exists."""
    if len(blob) % (ASSET_WORDS * 4):
        raise ValueError("asset table is %d B, not a whole number of %d B rows"
                         % (len(blob), ASSET_WORDS * 4))
    words = list(struct.unpack("<" + "I" * (len(blob) // 4), blob))
    cnt = len(words) // ASSET_WORDS if n is None else n
    if cnt > ASSET_MAX:
        raise ValueError("%d assets; the loader's table holds %d" % (cnt, ASSET_MAX))
    if len(words) < cnt * ASSET_WORDS:
        raise ValueError("asset table is %d B, short of the %d B its count claims"
                         % (len(blob), cnt * ASSET_WORDS * 4))
    out = []
    for i in range(cnt):
        base = i * ASSET_WORDS
        w = words[base:base + ASSET_WORDS]
        cls, req = w[A_CLASS] & 0xFF, (w[A_CLASS] >> 8) & 0xFF
        ns = (w[A_CLASS] >> 16) & 0xFF
        if cls >= len(ASSET_CLASSES):
            raise ValueError("asset %d declares class %d, which this build's vocabulary"
                             " does not carry" % (i, cls))
        if req > 1:
            raise ValueError("asset %d's required byte is %d: D31's selector is one bit"
                             % (i, req))
        if ns > 1:
            raise ValueError("asset %d's namespace byte is %d: there are two namespaces"
                             " and no third (D23)" % (i, ns))
        if w[A_CLASS] >> 24:
            raise ValueError("asset %d's fourth byte is not zero: the row has no fourth"
                             " field, and a writer that filled one means something this"
                             " reader cannot honour" % i)
        name = _asset_name(words, base)
        if not name:
            raise ValueError("asset %d has an empty name: a slot nobody can name cannot"
                             " be resolved and cannot be refused by name, so it can only"
                             " be refused here" % i)
        xy, wh = w[A_XY], w[A_WH]
        out.append(dict(name=name, cls=ASSET_CLASSES[cls], required=req,
                        ns=("global" if ns else "owner"),
                        x=_s32((xy & 0xFFFF) << 16) >> 16,
                        y=_s32(xy & 0xFFFF0000) >> 16,
                        w=wh & 0xFFFF, h=(wh >> 16) & 0xFFFF))
    return out


def art_offset(geom_off, geom_size):
    """Payload start for a blob at geom_off: sector-aligned, so a copy into a
    64 KB-aligned assets slot stays 64 KB-aligned. Same reason as
    orbtheme.art_offset — an unaligned start spends an MMU entry on rounding."""
    return (geom_off + geom_size + SECTOR - 1) & ~(SECTOR - 1)


def _hdr_crc(hdr):
    """crc32 over the header words with the checksum word itself zeroed — the
    theme container's rule, and the reason a torn header is caught by the first
    HDR_BYTES read instead of after a multi-megabyte payload is streamed."""
    z = list(hdr)
    z[W_HDR_CRC] = 0
    return zlib.crc32(b"".join(struct.pack("<I", v) for v in z)) & 0xFFFFFFFF


def _period(period_ms):
    """D16's floor, enforced on the way out. 0 is a legal answer (no tick); any
    other value under the commit pass the dial can honour is not."""
    _u(period_ms, "period_ms")
    if period_ms == 0:
        return 0
    if period_ms < BEAT_MIN_MS:
        raise ValueError("period_ms %d is under the %d ms the measured commit pass "
                         "costs: the app would declare a beat the OS cannot deliver, "
                         "and the owner prices the app at this number (D16)"
                         % (period_ms, BEAT_MIN_MS))
    if period_ms > BEAT_MAX_MS:
        raise ValueError("period_ms %d exceeds %d ms: past a day the app means it "
                         "never ticks, and 0 is the word for that"
                         % (period_ms, BEAT_MAX_MS))
    return period_ms


def _art_req(art_req):
    """D21 (§0.9): the container carries no payload to reserve against, so
    art_req is purely a DECLARATION of need (D18's requests-not-bytes) — the
    theme and asset registries answer it at runtime. It is bounded only against
    mis-count: the number itself may not exceed the old slot's size."""
    _u(art_req, "art_req")
    if art_req > ART_BYTES_MAX:
        raise ValueError("declares an art need of %d B past the %d B ever "
                         "budgeted — a demand that size is a miscount"
                         % (art_req, ART_BYTES_MAX))
    return art_req


def _geom_size(geom_size, counts):
    """The geometry blob is opaque (D15), so its LENGTH is the only thing
    checkable — and it is checkable precisely because the counts decide it. A
    blob shorter than the tables it claims would be read off the end of the
    mapping, so the arithmetic is refused here and the reader may trust the pair."""
    _u(geom_size, "geom_size")
    if geom_size > GEOM_BYTES_MAX:
        raise ValueError("geometry blob is %d B, past the %d B an app's own records "
                         "may occupy" % (geom_size, GEOM_BYTES_MAX))
    if geom_size % 4:
        raise ValueError("geometry blob is %d B: the whole format is u32 words, and "
                         "a ragged blob is a mis-count, not a dialect" % geom_size)
    n_layer, n_comp, n_aux, n_bank = counts
    need = (n_layer + n_comp + n_aux + n_bank) * 4
    if geom_size < need:
        raise ValueError("geometry blob is %d B but its counts (%d/%d/%d/%d words) "
                         "need %d: a truncated table is read as pixels"
                         % (geom_size, n_layer, n_comp, n_aux, n_bank, need))
    return geom_size


def pack_header(app_id, slug, title, caps, period_ms, art_req, counts,
                settings_blob, serial=0, baked_ms=0, abi=None,
                sig=0, behaviour_kind=0, asset_blob=b""):
    """Lay out the header+manifest block. Every field is checked here, because a
    header this function refuses is a header the device never has to meet.

    There is no art argument: D21 (§0.9) withdrew the container's payload, so
    art_size and art_crc go to the block as constants — zero, the only shape a
    reader will accept. art_req stays declarable; see _art_req.

    The CRC word goes last and covers the WHOLE block including the caps word:
    that is the D19 property, and the one respect in which this container is
    stricter than the theme's.
    """
    hdr = [0] * HDR_WORDS
    abi_min, abi_max = (GEOM_ABI, GEOM_ABI) if abi is None else abi
    hdr[W_MAGIC], hdr[W_FORMAT]      = MAGIC, FORMAT
    hdr[W_GEOM_ABI], hdr[W_HDR_SIZE] = GEOM_ABI, HDR_BYTES
    hdr[W_SERIAL], hdr[W_BAKED_MS]   = _u(serial, "serial"), _u(baked_ms, "baked_ms")
    hdr[W_ABI_MIN], hdr[W_ABI_MAX]   = _u(abi_min, "abi_min"), _u(abi_max, "abi_max")
    hdr[W_SIG] = _u(sig, "sig")
    if abi_min > abi_max:
        raise ValueError("abi_min %d is above abi_max %d: no OS satisfies both, so "
                         "the package would be refused by every board"
                         % (abi_min, abi_max))
    if not (abi_min <= GEOM_ABI <= abi_max):
        raise ValueError("this writer speaks geometry ABI %d, outside the declared "
                         "range %d..%d" % (GEOM_ABI, abi_min, abi_max))
    if sig != 0:
        raise ValueError("sig is reserved and must be 0 (C3): signing arrives as a "
                         "new FORMAT with its own field, not in this word")
    hdr[W_APP_ID]    = _u(app_id, "app_id")
    hdr[W_BEHAVIOUR] = _u(behaviour_kind, "behaviour_kind")   # opaque, D15
    hdr[W_PERIOD_MS] = _period(period_ms)
    hdr[W_CAPS]      = check_caps(caps)
    n_layer, n_comp, n_aux, n_bank = counts
    hdr[W_ART_REQ] = _art_req(art_req)
    for w, v, nm in ((W_N_LAYER, n_layer, "n_layer"), (W_N_COMP, n_comp, "n_comp"),
                     (W_N_AUX, n_aux, "n_aux"), (W_N_BANK, n_bank, "n_bank")):
        hdr[w] = _u(v, nm)
    if settings_blob and len(settings_blob) % (SETTINGS_WORDS * 4):
        raise ValueError("settings blob is %d B, not a whole number of %d B "
                         "descriptors" % (len(settings_blob), SETTINGS_WORDS * 4))
    n_set = len(settings_blob) // (SETTINGS_WORDS * 4)
    if n_set > SETTINGS_MAX:
        raise ValueError("%d settings; the loader's table holds %d" % (n_set, SETTINGS_MAX))
    hdr[W_N_SETTINGS] = n_set
    hdr[W_ART_SIZE]   = 0            # D21 (§0.9): a container never carries art
    if len(asset_blob) % (ASSET_WORDS * 4):
        raise ValueError("asset table is %d B, not a whole number of %d B rows"
                         % (len(asset_blob), ASSET_WORDS * 4))
    n_asset = len(asset_blob) // (ASSET_WORDS * 4)
    if n_asset > ASSET_MAX:
        raise ValueError("%d assets; the loader's table holds %d" % (n_asset, ASSET_MAX))
    hdr[W_N_ASSETS] = n_asset
    # The offsets are DERIVED from the sizes, never supplied: a caller that could
    # place the parts anywhere could also overlap them, and the theme path's
    # FORMAT-1 scar is exactly a hand-placed pair of ranges disagreeing. The asset
    # table rides the same rule — laid after the settings and before the geometry,
    # so its offset is arithmetic on the blob's own parts and not a number a caller
    # picks, which is what makes a self-overlapping container unwriteable here.
    hdr[W_SET_OFF]   = SET_OFF
    hdr[W_ASSET_OFF] = SET_OFF + len(settings_blob)
    hdr[W_GEOM_OFF]  = hdr[W_ASSET_OFF] + len(asset_blob)
    hdr[W_GEOM_SIZE] = 0                             # filled below from the blob
    hdr[W_ART_OFF]  = art_offset(hdr[W_GEOM_OFF], 0)
    hdr[W_GEOM_CRC], hdr[W_ART_CRC] = 0, 0           # filled by write_container
    _putstr(hdr, W_SLUG,  slug,  SLUG_MAX,  "slug")
    _putstr(hdr, W_TITLE, title, TITLE_MAX, "title")
    hdr[W_HDR_CRC] = _hdr_crc(hdr)
    return hdr


def parse_header(raw, app_id=None):
    """Decode and validate a header block, raising ValueError with the reason in
    words a page can show verbatim — the same shape as orbtheme.parse_header.

    What the fixed layout and the whole-header checksum decide on their own
    lives here: not a container, a newer FORMAT, a header size that is not
    this format's, a torn CRC, a non-zero sig. The semantic refusals — the C4
    ABI range, the caps declaration, the identity triple, the counts that have
    to add up — are check_header's, so a partition-slot reader runs the same
    rules as a file reader.
    """
    if len(raw) < HDR_BYTES:
        raise ValueError("too short to hold an app header")
    hdr = list(struct.unpack("<" + "I" * HDR_WORDS, raw[:HDR_BYTES]))
    if hdr[W_MAGIC] != MAGIC:
        # Named separately from the generic case: an .orbtheme handed to this
        # reader is the confusion the two extensions exist to prevent, and the
        # owner deserves to be told that is what happened (.clinerules:453).
        that = (" (that is an .orbtheme: themes live in /sd/themes and the OS "
                "package manager installs them alone)" if hdr[W_MAGIC] == 0x474F5254
                else "")
        raise ValueError("not an .orbapp file" + that)
    if hdr[W_FORMAT] != FORMAT:
        raise ValueError("app format %d; this build reads format %d"
                         % (hdr[W_FORMAT], FORMAT))
    if hdr[W_HDR_SIZE] != HDR_BYTES:
        raise ValueError("header claims %d B; this format is %d B"
                         % (hdr[W_HDR_SIZE], HDR_BYTES))
    if hdr[W_HDR_CRC] != _hdr_crc(hdr):
        # "header", not "file damaged": not one payload byte has been looked at,
        # and the difference decides whether the owner re-copies the file or
        # re-bakes it. The theme reader draws the same line.
        raise ValueError("container header checksum mismatch — re-copy the file "
                         "onto the card")
    if hdr[W_SIG] != 0:
        raise ValueError("sig is non-zero and this build verifies no signatures "
                         "(C3): refusing to install bytes nobody vouched for")
    h = {k: hdr[w] for k, w in (
        ("fmt", W_FORMAT), ("abi", W_GEOM_ABI), ("hdr_size", W_HDR_SIZE),
        ("serial", W_SERIAL), ("baked_ms", W_BAKED_MS), ("abi_min", W_ABI_MIN),
        ("abi_max", W_ABI_MAX), ("sig", W_SIG), ("app_id", W_APP_ID),
        ("behaviour_kind", W_BEHAVIOUR), ("art_req", W_ART_REQ),
        ("n_layer", W_N_LAYER), ("n_comp", W_N_COMP), ("n_aux", W_N_AUX),
        ("n_bank", W_N_BANK), ("period_ms", W_PERIOD_MS), ("caps", W_CAPS),
        ("n_settings", W_N_SETTINGS), ("setting_off", W_SET_OFF),
        ("geom_off", W_GEOM_OFF), ("geom_size", W_GEOM_SIZE),
        ("geom_crc", W_GEOM_CRC), ("art_off", W_ART_OFF),
        ("art_size", W_ART_SIZE), ("art_crc", W_ART_CRC),
        ("asset_off", W_ASSET_OFF), ("n_assets", W_N_ASSETS))}
    h["slug"]  = _getstr(hdr, W_SLUG, SLUG_MAX)
    h["title"] = _getstr(hdr, W_TITLE, TITLE_MAX)
    return h


def check_header(h, app_id=None, stem=None):
    """The semantic checks, run on a parsed header whose CRC already passed.

    Split from parse_header because the installer reads a header out of a
    partition slot where the offsets mean something else: the shape checks below
    are the ones that hold anywhere, and the file-layout rule is the caller's —
    exactly the division orb_bundle3_hdr_parse() draws against its caller.
    """
    abi = h["abi"]
    if not (h["abi_min"] <= abi <= h["abi_max"]):
        raise ValueError("app needs geometry ABI %d..%d; this build speaks %d — "
                         "re-package it (C4)" % (h["abi_min"], h["abi_max"], abi))
    if not h["slug"]:
        # The slug is the NVS namespace, the ?to= route and the filename stem.
        # Every nameless app would otherwise share one identity on the card.
        raise ValueError("app declares no slug in its header — re-package it")
    check_slug(h["slug"])
    if not h["title"]:
        raise ValueError("app declares no title: the install confirm and the "
                         "carousel row both read this field, and an empty one is "
                         "an unlabeled permission request")
    if h["caps"] >> CAP_BITS or h["caps"] & ~CAP_OWNER_MASK:
        raise ValueError("header caps 0x%X are not a legal declaration — owner "
                         "caps only (%s), and nothing at or above CAP_N"
                         % (h["caps"], ", ".join(CAP_OWNER)))
    want = app_id_of(h["slug"])
    if h["app_id"] != want:
        raise ValueError("header app_id 0x%08x is not the slug '%s' it claims "
                         "(that slug is 0x%08x)" % (h["app_id"], h["slug"], want))
    if app_id is not None and h["app_id"] != app_id:
        raise ValueError("header app_id 0x%08x is not the 0x%08x its caller expects"
                         % (h["app_id"], app_id))
    if stem is not None:
        filed_as_its_own_slug(stem, h["app_id"], h["slug"])
    if h["n_settings"] > SETTINGS_MAX:
        raise ValueError("%d settings declared; the loader's table holds %d"
                         % (h["n_settings"], SETTINGS_MAX))
    # P5b (D23): the same shape of check on the table the settings check covers —
    # the count and the placement, not the rows. A row that breaks a rule dies in
    # unpack_assets, and the install path runs both: a table whose count fits a
    # struct while its rows do not fit a slot is the half-check that made the old
    # art_size bound a lie, and the ledger would have shown a clean install.
    if h["n_assets"] > ASSET_MAX:
        raise ValueError("%d assets declared; the loader's table holds %d"
                         % (h["n_assets"], ASSET_MAX))
    if h["asset_off"] % 4 or h["asset_off"] > GEOM_BYTES_MAX:
        raise ValueError("asset table offset %d is not word-aligned or is past the "
                         "%d B a slot's payload region can hold"
                         % (h["asset_off"], GEOM_BYTES_MAX))
    # No rule that an empty table's offset must be zero: the offset is DERIVED from
    # the settings' size, so a container with four settings and no declared slot
    # legitimately points at the byte the geometry starts on. check_file_layout owns
    # that equality, and it is the rule with a subject — "the table claims room the
    # geometry then also claims" is the damage, and an offset of 432 alone is news
    # about nobody.
    # D21 (§0.9): no container carries art. These two refusals are the device's
    # own (orb_app_manifest.c mirrors them in the same words), and the tear-test
    # property stays: an art-bearing file is refused at pack, at read and on the
    # dial with the one sentence the owner can act on.
    if h["art_size"] != 0 or h["art_crc"] != 0:
        raise ValueError("carries a %d B art payload: no container ships art "
                         "(D21) — re-package it" % h["art_size"])
    if h["art_req"] > ART_BYTES_MAX:
        raise ValueError("declares an art need of %d B past the %d B ever "
                         "budgeted — a demand that size is a miscount"
                         % (h["art_req"], ART_BYTES_MAX))
    if h["geom_size"] > GEOM_BYTES_MAX or h["geom_size"] % 4:
        raise ValueError("geometry blob is %d B: past the cap, or not a whole "
                         "number of words" % h["geom_size"])
    # The counts decide the blob's length, so a header whose numbers do not add
    # up is refused before one byte of payload is streamed — orb_theme.c's rule,
    # and the reason a truncated table cannot be read as pixels.
    need = sum(h[c] for c in ("n_layer", "n_comp", "n_aux", "n_bank")) * 4
    if h["geom_size"] < need:
        raise ValueError("geometry blob is %d B but its counts need %d"
                         % (h["geom_size"], need))
    # The pair that lived here (art_req-with-no-payload refused, art_req under
    # its payload refused) is D21-inverted: no payload is the only legal shape,
    # so the art_size gate above owns the whole question and art_req stands
    # alone as the declaration it always meant to be.
    if h["period_ms"] and h["period_ms"] < BEAT_MIN_MS:
        raise ValueError("period_ms %d is below the %d ms the OS can commit (D16)"
                         % (h["period_ms"], BEAT_MIN_MS))
    return h


def check_file_layout(h):
    """The container FILE's own placement rule, which a slot read does not share."""
    if h["setting_off"] != SET_OFF:
        raise ValueError("the settings table is not where this format puts it")
    # The asset table sits BETWEEN the settings and the geometry, so its offset is
    # derived from the settings' size and the geometry's from both. Derived and not
    # stored-then-checked: a reader that accepted any placement would accept two
    # tables sharing the same bytes, and the CRC would pass on the overlap.
    if h["asset_off"] != SET_OFF + h["n_settings"] * SETTINGS_WORDS * 4:
        raise ValueError("the asset table does not follow the settings table")
    if h["geom_off"] != h["asset_off"] + h["n_assets"] * ASSET_WORDS * 4:
        raise ValueError("the geometry blob does not follow the asset table")
    if h["art_off"] != art_offset(h["geom_off"], h["geom_size"]):
        raise ValueError("the payload is not sector-aligned off the geometry blob")
    return h


def write_container(path, slug, title, caps=0, settings=(), geometry=b"",
                   period_ms=0, art_req=0, counts=(0, 0, 0, 0),
                   behaviour_kind=0, serial=0, baked_ms=0, abi=None, assets=()):
    """Emit one `.orbapp`, its name derived from the header that is its identity.

    Nothing is written unless every check passes, and the CRC is computed over
    the bytes as they will sit in the file — the same rule that lets the device
    stream a payload and check it instead of holding it in RAM. A container
    that reaches the card is one this function already refused to write if it was
    wrong. There is no `art` argument (D21, §0.9): the payload is geometry and
    nothing else; art_req is a declaration, not bytes.

    `assets` is the D23 table — the app's named slots — and it is the only art the
    container may describe. It ships NO pixels: every row is a reference, a class
    and a box, and the bytes a slot draws come from a theme bundle or from one of
    the five generators. A writer that could ship a plate here would be a writer
    that reintroduced the payload D21 withdrew.
    """
    if not isinstance(geometry, (bytes, bytearray)):
        raise ValueError("geometry must be bytes")
    check_slug(slug)
    rows = check_settings(settings)
    s_blob = pack_settings(rows)
    a_blob = pack_assets(assets, slug)
    geom_bytes = bytes(geometry)
    hdr = pack_header(app_id_of(slug), slug, title, caps, period_ms, art_req,
                      counts, s_blob, serial=serial,
                      baked_ms=baked_ms, abi=abi, behaviour_kind=behaviour_kind,
                      asset_blob=a_blob)
    # The packer left geom_size/geom_crc/art_crc at zero because only it knows
    # the blob it is about to lay down; fill and re-checksum in one place. The
    # art crc stays zero — zlib.crc32 of the empty payload the format forbids.
    hdr[W_GEOM_SIZE] = _geom_size(len(geom_bytes), counts)
    hdr[W_GEOM_OFF]  = hdr[W_ASSET_OFF] + len(a_blob)
    # The device's slot boundary is geom_off + geom_size (orb_app_install.c:62): the
    # manifest, settings and asset table ahead of the blob are part of the bill.
    # GEOM_BYTES_MAX alone caps the blob only, so a full-ceiling blob written here was
    # refused pre-erase on the dial (§19.3's witness: "262320 B: a slot holds
    # 262144 B"). Refuse it here, in the device's words, before a file exists.
    if hdr[W_GEOM_OFF] + hdr[W_GEOM_SIZE] > GEOM_BYTES_MAX:
        raise ValueError("its manifest and geometry are %d B: a slot holds %d B —"
                         " re-bake the app smaller"
                         % (hdr[W_GEOM_OFF] + hdr[W_GEOM_SIZE], GEOM_BYTES_MAX))
    hdr[W_ART_OFF]   = art_offset(hdr[W_GEOM_OFF], hdr[W_GEOM_SIZE])
    hdr[W_GEOM_CRC]  = zlib.crc32(geom_bytes) & 0xFFFFFFFF
    hdr[W_ART_CRC]   = 0
    hdr[W_HDR_CRC]   = _hdr_crc(hdr)
    raw = b"".join(struct.pack("<I", v) for v in hdr) + s_blob + a_blob + geom_bytes
    raw += b"\0" * (hdr[W_ART_OFF] - len(raw))
    # The NAME is derived from the slug, exactly as the theme upload renames a
    # .part into theme_id(header.name): a writer that honoured the caller's
    # filename could be talked into filing slug "bar" as "foo.orbapp", and the
    # disagreement would then be discovered on the dial instead of here. A name
    # ending .part is honoured as staging, because that is the one name the scan
    # is designed not to see.
    d = os.path.dirname(str(path))
    given = os.path.basename(str(path))
    low = given.lower()
    staging = low.endswith(PART_EXT)
    stem = (given[: -len(EXT)] if low.endswith(EXT) else
            given[: -len(PART_EXT)] if staging else slug)
    if stem != slug:
        raise ValueError("writing %s under stem '%s' while the header declares slug "
                         "'%s': the filename belongs to the slug, not to the caller"
                         % (given, stem, slug))
    path = os.path.join(d or ".", slug + (PART_EXT if staging else EXT))
    with open(path, "wb") as f:
        f.write(raw)
    return len(raw)


def read_container(path, verify=True):
    """Whole-file read plus the geometry CRC. Host and bench only: the device
    streams, so it never holds an app's blob in RAM. Returns (header, settings,
    geometry, art) — the fourth is always empty: check_header refuses any
    container that ships art (D21), so the tuple's shape survives from the
    pre-D21 days and nothing fills it.

    The header dict gains an `assets` key holding the unpacked D23 table, because
    the table is the container's own words and not a payload: a caller that asked
    for the manifest should not have to ask twice for the half of it that says what
    the app intends to draw. The row dicts carry the assembled reference, so a
    caller never rebuilds a namespace from a flag."""
    with open(path, "rb") as f:
        raw = f.read()
    h = parse_header(raw)
    stem = os.path.basename(str(path))
    check_header(h, stem=stem)
    check_file_layout(h)
    end = h["geom_off"] + h["geom_size"]
    if end > len(raw):
        raise ValueError("geometry blob runs past the end of the file")
    if h["setting_off"] + h["n_settings"] * SETTINGS_WORDS * 4 > len(raw):
        raise ValueError("the settings table runs past the end of the file")
    a_end = h["asset_off"] + h["n_assets"] * ASSET_WORDS * 4
    if a_end > len(raw):
        raise ValueError("asset table runs past the end of the file")
    s_blob = raw[SET_OFF:SET_OFF + h["n_settings"] * SETTINGS_WORDS * 4]
    a_blob = raw[h["asset_off"]:a_end]
    geom = raw[h["geom_off"]:end]
    art  = b""
    if verify:
        # The one payload CRC, named: the art word's twin is zero-gated by
        # check_header, so a damaged copy can only be the geometry's. The asset
        # table needs no checksum of its own — it lies inside the range the slot
        # CRC covers on the device (bytes [0, geom_off+geom_size)) and inside the
        # header the file CRC covers here, and a second checksum over bytes two
        # other checksums already reach is a third number that can disagree.
        if zlib.crc32(geom) & 0xFFFFFFFF != h["geom_crc"]:
            raise ValueError("geometry checksum mismatch — the copy on the card "
                             "is damaged")
    h["assets"] = unpack_assets(a_blob, h["n_assets"])
    for r in h["assets"]:
        r["ref"] = ("global/" + r["name"]) if r["ns"] == "global" else \
                   (h["slug"] + "/" + r["name"])
    return h, unpack_settings(s_blob, h["n_settings"]), geom, art


def read_header(path):
    """Header only — the scan's cheap path, which must not stream a bank to
    decide whether to list a row."""
    with open(path, "rb") as f:
        raw = f.read(HDR_BYTES)
    h = parse_header(raw)
    check_header(h, stem=os.path.basename(str(path)))
    check_file_layout(h)
    return h


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 2 and sys.argv[1] == "--inspect":
        h, st, geom, art = read_container(sys.argv[2])
        print("%-10s 0x%08x  '%s'  caps=%s" % (h["slug"], h["app_id"], h["title"],
                                               ",".join(caps_names(h["caps"])) or "-"))
        print("  fmt %d abi %d (%d..%d)  period %d ms  settings %d  assets %d  "
              "geom %d B  art %d B (req %d)  serial %d"
              % (h["fmt"], h["abi"], h["abi_min"], h["abi_max"], h["period_ms"],
                 h["n_settings"], h["n_assets"], h["geom_size"], h["art_size"],
                 h["art_req"], h["serial"]))
        for s in st:
            print("  setting %-15s %-6s min=%d max=%d def=%d %s"
                  % (s["id"], s["type"], s["min"], s["max"], s["default"],
                     ("def_str=%r" % s["def_str"]) if s["type"] == "string" else ""))
        # P5b (D23): the declared table, in the container's own words. Printed here so
        # --inspect answers the D26 question a builder asks before handing a package
        # over: does every required slot the app will draw have a name to resolve.
        for a in h["assets"]:
            print("  asset   %-24s %-6s %-8s %4dx%-4d @%d,%d%s"
                  % (a["ref"], a["cls"], a["ns"], a["w"], a["h"], a["x"], a["y"],
                     "  REQUIRED" if a["required"] else ""))
    elif len(sys.argv) > 2 and sys.argv[1] == "--disclose":
        # The D19 text: what the install confirm prints, from the file's own
        # words. Run it on a package before handing it to anybody.
        h = read_header(sys.argv[2])
        print(disclosure(h["slug"], h["title"], h["caps"], h["period_ms"],
                         h["n_settings"]))
    else:
        print("usage: python3 tools/orbapp.py --inspect <file.orbapp>\n"
              "       python3 tools/orbapp.py --disclose <file.orbapp>")
        sys.exit(2)
