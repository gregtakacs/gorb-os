#!/usr/bin/env python3
"""The `.orbtheme` container — one baked theme in one file, for the SD card.

Why this file exists at all: a bake has always emitted a PAIR,
`assets/clock_assets.bin` (the pixels, into the `assets` partition) and
`assets/clock_assets.h` (the geometry that describes them, COMPILED INTO THE
APP). A theme uploaded from a browser cannot change a compiled-in struct, so the
pair could never leave the bench. The container carries the geometry alongside
the pixels so the firmware can run a theme it was not built around.

The layout below is the single definition. main/art/orb_theme.c mirrors it field
for field, and the host suites prove the mirror: tools/host_tz_test.py and
tools/host_sd_test.py build a container with a distinct sentinel in every field
through THIS module and assert the lifted C reader recovers each one. Reordering a
field on one side only therefore fails a test rather than shipping a dial with the
hands' pivots in the overlay's slots — which is the exact class of bug a
struct-layout mismatch produces, and it would look like bad art, not like a bug.

FORMAT 2 — the theme's identity lives in the header's `name`, never in the
filename. A theme's one true name is the string the owner set when they built the
file, stored in the single NAME_MAX-byte field at W_NAME (words 14..27). The card
filename is derived from it by theme_id(name) = sha256(name)[:12] + ".orbtheme" —
opaque hex nobody types, reads, or labels with. The upload filename is meaningless
once the bytes land and is not stored. FORMAT 1 (a slug + title pair) is refused
outright: this reader has no legacy path and there is no migration. GEOM_ABI stays
1; only the name fields changed, so W_HDR_CRC stays 28 and HDR_WORDS stays 32.

Wire format, all little-endian u32 words (every field widened to 32 bits, so
there is no padding rule for either side to disagree about):

  header   (HDR_WORDS words, pack_header; one name field at W_NAME)
  geometry (geom_size bytes at geom_off):
      n_rects, n_first, n_delays, n_bgframes        <- 4 count words
      clk_theme_t scalars per THEME_FIELDS
      hand[5], shadow[5], overlay, tourbillon[16]   <- SPRITE_FIELDS each
      rects      : n_rects * 5  (x, y, w, h, off)
      rect_first : n_first * 1
      gif_delays : n_delays * 1
      bg_frames  : n_bgframes * 1
  pixel payload (art_size bytes at align_up(geom_off + geom_size, 4096))

The payload is byte-identical to `assets/clock_assets.bin`, which is what keeps
one bake serving both paths: `idf.py flash` still takes the pair, the card takes
the container.
"""

import hashlib
import struct
import zlib

MAGIC         = 0x474F5254        # 'T','R','O','G' little-endian on the wire
FORMAT        = 2                 # this file's container layout
GEOM_ABI      = 1                 # the clock_view.c geometry struct it describes
SECTOR        = 4096
HDR_WORDS     = 32                # 128 B, the rest reserved for the next format
NAME_MAX      = 56                # one name field; 55 usable bytes + NUL
SPRITE_FIELDS = ("w", "h", "offx", "offy", "pivotx", "pivoty",
                 "centerx", "centery", "blend", "show", "color_off",
                 "diffx", "diffy", "diffw", "diffh")

# Count words at the head of the geometry blob. The first four are the original
# layout ("v1"); the last three carry the complication/indicator tables described
# by tools/theme_schema.py. A blob's length is determined by these counts alone,
# so a reader can tell the two layouts apart from geom_size and refuse a blob
# that matches neither, instead of reading a truncated table as pixels.
GEOM_COUNTS = ("n_rects", "n_first", "n_delays", "n_frames",
               "n_comp", "n_aux", "n_bank")

# Header word map. Kept in one place because the name is a byte array spread over
# whole words. FORMAT 2 collapses the old slug+title pair into ONE name field that
# occupies exactly the bytes the pair used (words 14..27), so W_HDR_CRC stays 28 and
# HDR_WORDS stays 32. The first draft of this format put two name strings on
# overlapping words and a long slug silently overwrote the head of the title; two
# short strings hid it. The range is now derived from the single field rather than
# spelled twice, so it cannot collide again without the size and the test both moving.
W_MAGIC     = 0
W_FORMAT    = 1
W_GEOM_ABI  = 2
W_HDR_SIZE  = 3
W_GEOM_OFF  = 4
W_GEOM_SIZE = 5
W_GEOM_CRC  = 6
W_ART_SIZE  = 7
W_ART_CRC   = 8
W_SCR_W     = 9
W_SCR_H     = 10
W_SERIAL    = 11
W_BAKED_MS  = 12
W_ART_OFF   = 13
W_NAME      = 14                                  # NAME_MAX bytes (words 14..27)
W_HDR_CRC   = W_NAME + ((NAME_MAX + 3) // 4)      # crc32 over the whole header == 28
HDR_WORDS   = W_HDR_CRC + 4                       # +4 words spare for the next format


# clk_theme_t's scalars in clock_view.c's declaration order, as (name, type).
# The type only drives sign extension on read; every field is one u32 word on
# the wire. Keep this list and orb_theme.c's parse table in step — the sentinel
# test in tools/host_tz_test.py is what notices when they are not.
THEME_FIELDS = (
    ("bg", "u32"),
    ("plate_follow", "u8"), ("shadow_on", "u8"),
    ("shadow_dx", "i8"), ("shadow_dy", "i8"),
    ("second_sweep", "u8"), ("second_railway", "u8"),
    ("text_over_hands", "u8"), ("tick_rate", "u8"),
    ("bg_frames", "u16"), ("bg_fps", "i16"),
    ("bg_loop", "u8"), ("bg_every", "u16"),
    ("plate_off", "u32"),
    ("order0", "u8"), ("order1", "u8"), ("order2", "u8"),
    ("order3", "u8"), ("order4", "u8"),
    ("order_n", "u8"), ("matte_mask", "u8"),
    ("bg_mode", "u8"), ("bg_key_off", "u32"),
    ("gif_frames", "u16"), ("tourbillon_n", "u16"), ("tourbillon_fps", "u16"),
)


def _range(t):
    """The C field's own range for a declared wire type."""
    return {"i8": (-128, 127), "i16": (-32768, 32767), "i32": (-2**31, 2**31 - 1),
            "u8": (0, 255), "u16": (0, 65535), "u32": (0, 2**32 - 1)}[t]


def _u(v):
    """Any signed or unsigned int, wrapped into the u32 word the wire carries.

    Use _val() for anything that maps to a typed C field: wrapping a value that
    does not fit its own field is how -5 in an i8 becomes 4294967035 on the wire
    and -5 again on the device only by luck of the reader's mask.
    """
    return int(v) & 0xFFFFFFFF


def _val(what, t, v):
    """_u() with the range the C field actually has.

    The bake computes these numbers in Python and the device reads them as C
    types, so a value outside the field's range would be silently reinterpreted
    rather than reported: a shadow_dx of 1757 arrives as -35 and the shadow sits
    two thousand pixels off the dial. The baker's whole discipline is refusing to
    emit art it knows to be wrong, so it refuses here too.
    """
    v = int(v)
    lo, hi = _range(t)
    if v < lo or v > hi:
        raise ValueError("%s: %d does not fit a %s field (%d..%d) — the bake "
                         "computed something the firmware cannot store"
                         % (what, v, t, lo, hi))
    return v & 0xFFFFFFFF


def _sext(v, t):
    """Undo _u() for the signed types; unsigned types pass through.

    The mask first, then the extend: _u() wrapped the value into 32 bits on the
    way out, so an i8 field arrives as 0xFFFFFFFB, not as 0xFB. Extending without
    masking turns -5 into -2051 and the shadow offset lands two thousand pixels
    off the dial.
    """
    bits = {"i8": 8, "i16": 16, "i32": 32}.get(t)
    if bits is None:
        return v
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >= (1 << (bits - 1)) else v


def _sptype(f):
    """clk_sprite_t field types, for the same sign-extension reason."""
    return {"w": "u16", "h": "u16", "color_off": "u32", "blend": "u8",
            "show": "u8", "diffw": "u16", "diffh": "u16"}.get(f, "i16")


def blank_spr():
    return {f: 0 for f in SPRITE_FIELDS}


def pack_spr(rec):
    """One clk_sprite_t as len(SPRITE_FIELDS) u32s, in SPRITE_FIELDS order, each
    range-checked against the type the device reads it as."""
    return [_val("sprite." + f, _sptype(f), rec[f]) for f in SPRITE_FIELDS]


def pack_geom(scalars, hands, shadows, overlay, tourb,
              rects, rect_first, gif_delays, bg_frames,
              comps=(), aux=(), bank=()):
    """The geometry blob, from the same values _finish() prints as C.

    scalars is keyed by THEME_FIELDS names; hands/shadows carry 5 records each
    and tourb up to 16 (short lists are padded with blanks so the reader always
    advances the same amount, exactly as the generated header does).

    comps/aux/bank are the complication tables of tools/theme_schema.py, laid
    down in that order at the tail. They are optional: a bake with none of them
    emits the four original count words and nothing after bg_frames, which is the
    blob every theme baked before the calendar existed and the one the device
    still loads. Emitting any of them makes the blob seven-count, so one file
    never mixes the two layouts — geom_layout() is how a reader tells them apart
    from the length alone, without believing a version field. The firmware caps
    the three tables at CLK_COMP_MAX/CLK_AUX_MAX/CLK_BANK_MAX (16/8/256, and the
    host test's caps lint compares those against theme_schema), and pack_records
    refuses more rows than that here, where a refusal is an error message on the
    bench rather than art silently missing from someone's dial."""
    if len(rects) % 5:
        raise ValueError("rects must be (x, y, w, h, off) quintuples, got %d words"
                         % len(rects))
    import theme_schema as ts
    rows = {"comp": list(comps), "aux": list(aux), "bank": list(bank)}
    counts = (len(rects) // 5, len(rect_first), len(gif_delays), len(bg_frames),
              len(rows["comp"]), len(rows["aux"]), len(rows["bank"]))
    n_counts = 7 if (counts[4] or counts[5] or counts[6]) else 4
    if n_counts == 4 and any(rows.values()):
        raise ValueError("comp/aux/bank records need a seven-count blob; refusing "
                         "to emit a blob a reader would misparse")
    w = [_val(GEOM_COUNTS[i], "u32", c) for i, c in enumerate(counts[:n_counts])]
    w += [_val(n, t, scalars[n]) for n, t in THEME_FIELDS]
    for h in hands:
        w += pack_spr(h)
    for s in shadows:
        w += pack_spr(s)
    w += pack_spr(overlay)
    for t in list(tourb)[:16]:
        w += pack_spr(t)
    for _ in range(16 - min(16, len(tourb))):
        w += pack_spr(blank_spr())
    for i, v in enumerate(rects):
        # x, y, w, h are uint16_t in clk_bg_rect_t; off is uint32_t.
        w.append(_val("rect[%d].%s" % (i // 5, ("x", "y", "w", "h", "off")[i % 5]),
                      "u32" if i % 5 == 4 else "u16", v))
    w += [_val("rect_first[%d]" % i, "u16", v) for i, v in enumerate(rect_first)]
    w += [_val("gif_delays[%d]" % i, "u16", v) for i, v in enumerate(gif_delays)]
    w += [_val("bg_frames[%d]" % i, "u32", v) for i, v in enumerate(bg_frames)]
    for kind in ("comp", "aux", "bank"):
        w += ts.pack_records(rows[kind], ts.fields_of(kind),
                             getattr(ts, "CLK_%s_MAX" % kind.upper()), kind)
    return b"".join(struct.pack("<I", v) for v in w)


def geom_need(counts, n_counts):
    """The exact blob length, in words, that these counts declare.

    The one place the size rule is written, so the packer, the host reader and
    main/art/orb_theme.c's geom_sizes_ok() cannot each invent their own
    arithmetic — a mismatch there is a theme refused as damaged."""
    import theme_schema as ts
    need = n_counts + len(THEME_FIELDS) + (5 + 5 + 1 + 16) * len(SPRITE_FIELDS)
    need += counts[0] * 5 + counts[1] + counts[2] + counts[3]
    if n_counts == 7:
        need += (counts[4] * ts.words_of("comp") + counts[5] * ts.words_of("aux")
                 + counts[6] * ts.words_of("bank"))
    return need


def geom_layout(words):
    """(n_counts, counts) for a blob, or ValueError with the reason.

    The four-count rule is tried first and the two rules are mutually exclusive:
    a seven-count blob carries three head words the four-count rule cannot
    account for, so it always leaves three words over, and a four-count blob with
    no comp rows cannot satisfy the seven-count rule either. The length is the
    evidence; no version field has to be believed."""
    for n in (4, 7):
        if len(words) < n:
            continue
        counts = list(words[:n]) + [0] * (7 - n)
        if geom_need(counts, n) == len(words):
            return n, counts
    raise ValueError("geometry blob length %d words matches neither the four-count "
                     "nor the seven-count layout — re-bake it" % len(words))


def unpack_geom(b):
    """Inverse of pack_geom, as dicts. Used by the host test and --inspect."""
    import theme_schema as ts
    w = list(struct.unpack("<" + "I" * (len(b) // 4), b))
    n_counts, c = geom_layout(w)
    n_rects, n_first, n_delays, n_frames = c[0], c[1], c[2], c[3]
    n_comp, n_aux, n_bank = c[4], c[5], c[6]
    i = n_counts
    scalars = {}
    for n, t in THEME_FIELDS:
        scalars[n] = _sext(w[i], t); i += 1

    def take_spr():
        nonlocal i
        rec = {f: _sext(w[i + k], _sptype(f)) for k, f in enumerate(SPRITE_FIELDS)}
        i += len(SPRITE_FIELDS)
        return rec

    hands   = [take_spr() for _ in range(5)]
    shadows = [take_spr() for _ in range(5)]
    overlay = take_spr()
    tourb   = [take_spr() for _ in range(16)]
    rects   = w[i:i + n_rects * 5]; i += n_rects * 5
    first   = w[i:i + n_first];     i += n_first
    delays  = w[i:i + n_delays];    i += n_delays
    frames  = w[i:i + n_frames];    i += n_frames
    comps = ts.unpack_records(w[i:], ts.COMP_FIELDS, n_comp, "comp")
    i += n_comp * ts.words_of("comp")
    aux   = ts.unpack_records(w[i:], ts.AUX_FIELDS, n_aux, "aux")
    i += n_aux * ts.words_of("aux")
    bank  = ts.unpack_records(w[i:], ts.BANK_FIELDS, n_bank, "bank")
    i += n_bank * ts.words_of("bank")
    if i != len(w):
        raise ValueError("geometry blob has %d words left over at word %d"
                         % (len(w) - i, i))
    return {"scalars": scalars, "hands": hands, "shadows": shadows,
            "overlay": overlay, "tourbillon": tourb, "rects": rects,
            "rect_first": first, "gif_delays": delays, "bg_frames": frames,
            "comps": comps, "aux": aux, "bank": bank, "n_counts": n_counts}


def _wordstr(hdr, base, nbytes):
    out = bytearray()
    for i in range(nbytes):
        c = (hdr[base + i // 4] >> (8 * (i % 4))) & 0xFF
        if c == 0:
            break
        out.append(c)
    return out.decode("utf-8", "replace")


def _setword(hdr, base, text, nbytes):
    b = text.encode("utf-8")
    # Refuse, never truncate: the name inside the header IS the theme's identity
    # (the card filename is theme_id of these bytes). Silently cutting an over-long
    # name in half would file the theme under the hash of a string its owner never
    # typed. The studio's nameOk applies the same byte cap before the bake runs.
    if len(b) > nbytes - 1:
        raise ValueError("name is %d UTF-8 bytes; the header holds at most %d"
                         % (len(b), nbytes - 1))
    for i, c in enumerate(b):
        hdr[base + i // 4] |= c << (8 * (i % 4))


def art_offset(geom_size):
    """Payload start for a given blob size, sector-aligned: the device mmaps it
    straight out of the `assets` partition, and an unaligned start costs an MMU
    page entry to round."""
    return (HDR_WORDS * 4 + geom_size + SECTOR - 1) & ~(SECTOR - 1)


def theme_id(name):
    """The card identity of a theme name: sha256(name)[:12], lowercase hex.

    THE INPUT IS THE NAME UP TO ITS NUL, never the padded NAME_MAX-byte array, so
    "Neon" hashes exactly as hashlib.sha256(b"Neon") and a stored name never picks
    up trailing zero bytes as identity. This is the canonical implementation; the
    firmware's theme_id() in main/art/orb_theme.c mirrors it byte for byte, and
    tools/host_sd_test.py lifts the C version and pins it against this one on
    known-answer rows (ASCII, UTF-8, the 55-char boundary, and a row that proves
    the padded array and the NUL-terminated bytes hash differently). An empty name
    is refused: otherwise every nameless theme hashes to one constant.

    The card filename is theme_id(name) + ".orbtheme" — opaque hex no human types
    or reads. The upload filename carries no identity and is not stored."""
    if not name:
        raise ValueError("theme_id: an empty name is refused")
    return hashlib.sha256(name.encode("utf-8")).hexdigest()[:12]


def pack_header(name, geom_size, art_size, geom_crc, art_crc,
                scr_w, scr_h, serial=0, baked_ms=0):
    hdr = [0] * HDR_WORDS
    hdr[0], hdr[1], hdr[2], hdr[3] = (_u(MAGIC), _u(FORMAT), _u(GEOM_ABI),
                                      _u(HDR_WORDS * 4))
    hdr[4] = _u(HDR_WORDS * 4)              # geom_off: the blob follows the header
    hdr[5], hdr[6] = _u(geom_size), _u(geom_crc)
    hdr[7], hdr[8] = _u(art_size), _u(art_crc)
    hdr[9], hdr[10] = _u(scr_w), _u(scr_h)
    hdr[11], hdr[12], hdr[13] = _u(serial), _u(baked_ms), _u(art_offset(geom_size))
    _setword(hdr, W_NAME, name, NAME_MAX)
    # The header checksum covers the whole 128 bytes with its own word zeroed, so
    # a torn header (a bad count, a wrong art_off) is caught by the 128 bytes read
    # first rather than after streaming a multi-megabyte payload to the card.
    hdr[W_HDR_CRC] = _hdr_crc(hdr)
    return b"".join(struct.pack("<I", v) for v in hdr)


def _hdr_crc(hdr):
    """crc32 over the header bytes with the checksum word itself zeroed."""
    z = list(hdr)
    z[W_HDR_CRC] = 0
    return zlib.crc32(b"".join(struct.pack("<I", v) for v in z)) & 0xFFFFFFFF


def write_container(path, name, geom_bytes, art_bytes, scr_w, scr_h,
                    serial=0, baked_ms=0):
    """Emit one `.orbtheme`. The checksums are zlib CRC-32 — the same polynomial
    as esp_rom_crc32_le on the device — so a value computed here is checkable
    there without a second implementation free to drift."""
    hdr = pack_header(name, len(geom_bytes), len(art_bytes),
                      zlib.crc32(geom_bytes) & 0xFFFFFFFF,
                      zlib.crc32(art_bytes) & 0xFFFFFFFF,
                      scr_w, scr_h, serial, baked_ms)
    out = bytearray(hdr)
    out += geom_bytes
    out += b"\0" * (art_offset(len(geom_bytes)) - len(out))
    out += art_bytes
    with open(path, "wb") as f:
        f.write(bytes(out))
    return len(out)


def parse_header(raw):
    """Decode a container header from the first bytes of a file.

    Raises ValueError with the reason, in words the page can show verbatim. The
    three rejections a person actually hits are a file that is not a container
    at all, one from a newer format, and one baked for a different geometry ABI.
    The last is the dangerous case — reading it anyway is a dial with garbage
    sprites — so it is refused, not repaired.
    """
    if len(raw) < HDR_WORDS * 4:
        raise ValueError("too short to hold a theme header")
    hdr = struct.unpack("<" + "I" * HDR_WORDS, raw[:HDR_WORDS * 4])
    if hdr[0] != MAGIC:
        raise ValueError("not an .orbtheme file")
    if hdr[1] != FORMAT:
        raise ValueError("theme format %d; this build reads format %d" % (hdr[1], FORMAT))
    if hdr[2] != GEOM_ABI:
        raise ValueError("theme geometry v%d; this build reads v%d — re-bake it"
                         % (hdr[2], GEOM_ABI))
    if hdr[W_HDR_CRC] != _hdr_crc(hdr):
        # Say "header" and not "file damaged": at this point not one byte of the
        # pixels has been looked at, and the difference is whether the owner
        # re-copies the file (a torn copy) or re-bakes it (a broken bake).
        raise ValueError("container header checksum mismatch — re-copy the file onto the card")
    name = _wordstr(hdr, W_NAME, NAME_MAX)
    if not name:
        # Every nameless theme would hash to one constant and collide with each
        # other on the card, so an empty name is refused here, at parse, exactly as
        # the firmware's hdr_parse() refuses it.
        raise ValueError("theme has no name in its header — re-bake it")
    return {"fmt": hdr[1], "abi": hdr[2], "hdr_size": hdr[3], "geom_off": hdr[4],
            "geom_size": hdr[5], "geom_crc": hdr[6], "art_size": hdr[7],
            "art_crc": hdr[8], "scr_w": hdr[9], "scr_h": hdr[10],
            "serial": hdr[W_SERIAL], "baked_ms": hdr[W_BAKED_MS], "art_off": hdr[W_ART_OFF],
            "name": name}


def read_container(path):
    """Whole-file read + both checksums. For the host test and the bench; the
    device streams instead, so it never holds a 3.7 MB theme in RAM."""
    with open(path, "rb") as f:
        raw = f.read()
    h = parse_header(raw)
    if h["geom_off"] + h["geom_size"] > len(raw):
        raise ValueError("geometry blob runs past the end of the file")
    if h["art_off"] + h["art_size"] > len(raw):
        raise ValueError("pixel payload runs past the end of the file")
    geom = raw[h["geom_off"]:h["geom_off"] + h["geom_size"]]
    art = raw[h["art_off"]:h["art_off"] + h["art_size"]]
    if zlib.crc32(geom) & 0xFFFFFFFF != h["geom_crc"]:
        raise ValueError("geometry checksum mismatch — the copy on the card is damaged")
    if zlib.crc32(art) & 0xFFFFFFFF != h["art_crc"]:
        raise ValueError("pixel checksum mismatch — the copy on the card is damaged")
    return h, geom, art


if __name__ == "__main__":
    import sys
    if len(sys.argv) > 2 and sys.argv[1] == "--inspect":
        h, geom, art = read_container(sys.argv[2])
        g = unpack_geom(geom)
        print("%-12s '%s'  %dx%d  geom %d B  art %d B  serial %d"
              % (theme_id(h["name"]), h["name"], h["scr_w"], h["scr_h"],
                 h["geom_size"], h["art_size"], h["serial"]))
        print("  hands show=%s  tourbillon_n=%d  gif_frames=%d  rects=%d  bg_frames=%d"
              % ([x["show"] for x in g["hands"]], g["scalars"]["tourbillon_n"],
                 g["scalars"]["gif_frames"], len(g["rects"]) // 5,
                 len(g["bg_frames"])))
    else:
        print("usage: python3 tools/orbtheme.py --inspect <file.orbtheme>")


