"""The FORMAT 3 bundle writer and reader (P6.1, D32/D29/D31).

FORMAT 2 holds ONE app's bank (the clock's) behind fixed offsets. FORMAT 3's job
is N of those in one file, addressed by app slug (§22.2). D32 voided the draft's
"opaque geometry": a section's geometry region is a LEGIBLE blob whose head is the
theme's element table for that app (theme_schema.ELEMENT_FIELDS).

The header keeps FORMAT 2's shape exactly — 32 words, name in 14..27, crc at 28 —
and changes only the FORMAT and ABI stamps, so hdr_parse's 128-byte torn-header
check stays byte-identical across formats and §22.2's promise ("a wider header
needs no new rule") holds: the header did not need to widen. Everything new rides
the geometry region: an index head, n fixed records, then one self-describing blob
per section. n_sections rides the header's scr_w word and the panel pair moves INTO
each record: a dial refuses a section baked for the wrong panel where FORMAT 2
refused a file for it. A v2 reader refuses on the FORMAT word long before its panel
check could read them as sizes.

  region   file-relative (installed: the region at slot +0x1000 and the banks at
           +0x10000 + art_off — the same dual reading FORMAT 2 documents)
    +128   index head: n_sections, reserved            (2 words)
    +136   n records of IDX_WORDS, slug-sorted
    next   blob[0], blob[1], ...  each: counts(4) scalars(26) elements(17*n)
                                  sprites(4*m) slug(padded to words)
  banks    each section's pixels, sector-aligned, byte-identical to the FORMAT 2
           payload of the same art — one bake serves both paths. Bank offsets
           DERIVE from a sequential walk; a stored art offset free to disagree with
           the bytes is a bug waiting for its first torn install, so none is stored.

The D29 budget gate opens HERE, at the writer, and closes again at the reader
(P6.2, §22.3): geometry region under the 60 KB the slot reserves for tables,
pixel payload under the ORB_THEME_SLOT_CAP the banks have left in the slot. The
boundaries are the INSTALL sums, not the file's shape — §21's lesson, learned on
the app container: geometry lands at slot +0x1000 and its erase covers it
sector-aligned, so 61440 B lands exactly on +0x10000 and == cap is legal; banks
land at +0x10000, so the payload has SLOT_CAP - SLOT_ART_OFF to live in and the
trailing pad (excluded from art_size by the walk) is never installed. check_budget()
carries both sums in the sentences the device prints, reads the caps as module
attributes, and answers to orb_bundle3_hdr_parse() on the device — the three-way
rule in force. A section with no art is D31's silent-default question at resolve
time, not corruption; a duplicate slug is corruption, and refused.
"""
import struct
import zlib

import orbtheme as _ot
import theme_schema as _ts

FORMAT3      = 3
GEOM_ABI3    = 3
SECT_MAX     = 8                    # B5's app cap, the index's width
IDX_WORDS    = 10                   # IDX_PACK below: 4H + 7I + 2H = 40 B
SCALARS_N    = len(_ot.THEME_FIELDS)   # 26 today; the clock dialect, carried opaque
SPR_WORDS    = 4                    # off, size, w, h — a bank entry's byte range
BLOB_HEAD    = 4 + SCALARS_N        # counts + scalars, before the element table
SLOT_CAP     = 0x00400000           # orb_theme.c SLOT_CAP: 4 MB per slot
SLOT_ART_OFF = 0x00010000           # orb_theme.c SLOT_ART_OFF: banks install here
GEOM_CAP     = 0xF000               # orb_theme.c SLOT_GEOM_CAP: 60 KB of tables
SLOT_ART_CAP = SLOT_CAP - SLOT_ART_OFF   # orb_theme.h ORB_THEME_SLOT_CAP: banks' room
SLUG_MAX     = 23                   # ORBAPP_SLUG_MAX - 1: the NUL's room
_SLUG_OK     = frozenset(b"abcdefghijklmnopqrstuvwxyz0123456789_")
                                    # check_slug's charset, on bytes: read_bundle
                                    # and orb_bundle.c's slug rule both apply it
                                    # to forged names without decoding them

# The index record's field order on the wire. geom_off is stored although the walk
# derives it: the reader requires stored == derived, so a blob moved by a repack
# tool says where to look and a torn file contradicts itself out loud.
IDX_ORDER = ("slug_off", "slug_len", "abi_min", "abi_max", "n_elem",
             "geom_off", "geom_size", "geom_crc", "art_off", "art_size",
             "art_crc", "scr_w", "scr_h")
IDX_PACK  = "<4H7I2H"


def check_slug(slug):
    """The one slug rule, restated here rather than imported from orbapp so a theme
    bake never needs the app container's module. Must stay byte-identical to
    orbapp.check_slug and orb_app_manifest.c's matcher: lowercase a-z 0-9 _, 1..23
    chars. A slug the installer can never match is an element table no app will
    ever be asked to paint."""
    if (not isinstance(slug, str) or not 1 <= len(slug) <= SLUG_MAX
            or any(not (("a" <= c <= "z") or ("0" <= c <= "9") or c == "_")
                   for c in slug)):
        raise ValueError("bundle slug %r is not lowercase a-z 0-9 _ of 1..%d chars"
                         % (slug, SLUG_MAX))
    return slug


def check_budget(region_len, art_len):
    """The D29 budget gate, in the device's own sums (P6.2, §22.3).

    The boundary is what the INSTALL asks for, not the file's shape — §21's
    lesson, learned the hard way on the app container. Geometry installs at slot
    +0x1000 and its erase covers it sector-aligned, so a 61440 B region lands
    exactly on the banks at +0x10000: == cap ACCEPTED. Banks install at +0x10000,
    so the payload has SLOT_CAP - SLOT_ART_OFF = ORB_THEME_SLOT_CAP of room; the
    trailing pad never installs (the walk excludes it from art_size), and the cap
    is sector-aligned, so == cap is legal there too. The sentences end in
    orb_theme.c's copy_into words — D26: the owner learns a cost where they
    already see refusals. The caps are module attributes on purpose: the suite
    moves them to prove the gate sums, not merely the strings. Byte-identical
    refusals belong to orb_bundle3_hdr_parse() in main/art/orb_bundle.c."""
    if region_len > GEOM_CAP:
        raise ValueError("geometry region is %d B; the dial's table area holds "
                         "%d B — geometry would overrun the pixels"
                         % (region_len, GEOM_CAP))
    if art_len > SLOT_ART_CAP:
        raise ValueError("pixel payload is %d B; a theme slot holds %d B — "
                         "theme is larger than a slot"
                         % (art_len, SLOT_ART_CAP))


def parse_header3(raw):
    """FORMAT 2's discipline on FORMAT 3's stamps: same 128 bytes, same crc span,
    refusal sentences the page shows verbatim. The FORMAT word is checked before
    the ABI word so a bundle and a v2 reader each hear the reason that matches how
    they were baked."""
    if len(raw) < _ot.HDR_WORDS * 4:
        raise ValueError("too short to hold a theme header")
    hdr = _ot.struct.unpack("<" + "I" * _ot.HDR_WORDS, raw[:_ot.HDR_WORDS * 4])
    if hdr[0] != _ot.MAGIC:
        raise ValueError("not an .orbtheme file")
    if hdr[1] != FORMAT3:
        raise ValueError("theme format %d; this reader takes format %d"
                         % (hdr[1], FORMAT3))
    if hdr[2] != GEOM_ABI3:
        raise ValueError("bundle geometry v%d; this build reads v%d — re-bake it"
                         % (hdr[2], GEOM_ABI3))
    if hdr[_ot.W_HDR_CRC] != _ot._hdr_crc(hdr):
        raise ValueError("container header checksum mismatch — re-copy the file "
                         "onto the card")
    # The budget gate closes at the reader too (P6.2, §22.3): a header that
    # claims more than a slot can hold is refused HERE, before any streaming
    # begins — the same sums, the same sentences as the writer and as
    # orb_bundle3_hdr_parse(), before the name or the bounds get a turn.
    check_budget(hdr[5], hdr[7])
    name = _ot._wordstr(hdr, _ot.W_NAME, _ot.NAME_MAX)
    if not name:
        raise ValueError("theme has no name in its header — re-bake it")
    return {"fmt": hdr[1], "abi": hdr[2], "hdr_size": hdr[3],
            "geom_off": hdr[4], "geom_size": hdr[5], "geom_crc": hdr[6],
            "art_size": hdr[7], "art_crc": hdr[8], "n_sections": hdr[9],
            "serial": hdr[_ot.W_SERIAL], "baked_ms": hdr[_ot.W_BAKED_MS],
            "art_off": hdr[_ot.W_ART_OFF], "name": name}


def pack_blob(slug, elements, scalars=None, sprites=()):
    """One section's geometry blob, self-describing from its count head.

    `elements` are theme_schema dicts and pass check_table HERE — the refusal
    direction D32's amended D26 exists to make real: an element a theme cannot
    even name never reaches a card. `scalars` is a sparse dict of the clock
    dialect's names (the app's D31 defaults, opaque to any other app); all 26
    words are always present, absent ones zero. `sprites` catalogue the section's
    bank by byte range — validated against the bank's length by write_bundle, the
    only caller that holds those bytes.
    """
    elements = list(elements or ())
    _ts.check_table(elements)
    sb = slug.encode("utf-8")
    if not 1 <= len(sb) <= SLUG_MAX:
        raise ValueError("bundle slug %r is %d bytes, outside 1..%d"
                         % (slug, len(sb), SLUG_MAX))
    scal_words = [0] * SCALARS_N
    for name, v in (scalars or {}).items():
        hit = [i for i, (n, _t) in enumerate(_ot.THEME_FIELDS) if n == name]
        if not hit:
            raise ValueError("blob %r names scalar %r the dialect does not carry"
                             % (slug, name))
        scal_words[hit[0]] = _ot._val("%s.scalars.%s" % (slug, name),
                                      _ot.THEME_FIELDS[hit[0]][1], v)
    spr_words = []
    for i, sp in enumerate(sprites or ()):
        for fname in ("off", "size", "w", "h"):
            if fname not in sp:
                raise ValueError("blob %r sprite[%d] is missing %r" % (slug, i, fname))
            spr_words.append(_ot._val("%s.sprite[%d].%s" % (slug, i, fname),
                                      "u32", sp[fname]))
    if len(elements) > 0xFFFF or len(spr_words) // SPR_WORDS > 0xFFFF:
        raise ValueError("blob %r outgrew its u16 counts" % slug)
    counts = struct.pack("<4I", len(elements), len(spr_words) // SPR_WORDS,
                         SCALARS_N, len(sb))
    body = struct.pack("<%dI" % SCALARS_N, *scal_words)
    if elements:
        body += struct.pack("<%dI" % (len(elements) * _ts.ELEMENT_WORDS),
                            *_ts.pack_elements(elements))
    if spr_words:
        body += struct.pack("<%dI" % len(spr_words), *spr_words)
    return counts + body + sb + b"\0" * (-len(sb) % 4)


def unpack_blob(blob):
    """The device-side reading, mirrored: the counts decide every later offset, and
    a blob whose length the counts cannot produce is refused, never truncated-read
    — the discipline geom_counts() enforces on FORMAT 2. The refusal sentences
    here are orb_bundle.c's blob_check() sentences; the refusal order is too."""
    if len(blob) < BLOB_HEAD * 4:
        raise ValueError("section blob is shorter than its counts and scalars")
    n_elem, n_spr, n_scal, n_slug = struct.unpack_from("<4I", blob, 0)
    if n_scal != SCALARS_N:
        raise ValueError("blob carries %d scalars; this dialect has %d"
                         % (n_scal, SCALARS_N))
    want = (4 + n_scal + n_elem * _ts.ELEMENT_WORDS + n_spr * SPR_WORDS) * 4
    want += n_slug + (-n_slug % 4)
    if want != len(blob):
        raise ValueError("blob is %d bytes; its counts say %d — a torn file, not a "
                         "short one" % (len(blob), want))
    if n_elem > 0xFFFF or n_spr > 0xFFFF:
        # The record's counts are u16 and the device reader stores them as
        # such; orb_bundle.c's blob_check refuses the same rule in the same
        # words. A file big enough to show this cannot reach a device at all
        # — the door's budget ends at the slot cap.
        raise ValueError("blob outgrew its u16 counts")
    scal = struct.unpack_from("<%dI" % n_scal, blob, 16)
    off = BLOB_HEAD * 4
    elems = _ts.unpack_elements(list(struct.unpack_from(
        "<%dI" % (n_elem * _ts.ELEMENT_WORDS), blob, off)), n_elem) if n_elem else []
    off += n_elem * _ts.ELEMENT_WORDS * 4
    sprs = []
    for i in range(n_spr):
        o, sz, w, h = struct.unpack_from("<4I", blob, off + i * 16)
        sprs.append({"off": o, "size": sz, "w": w, "h": h})
    off += n_spr * SPR_WORDS * 4
    # The slug travels as BYTES, and read_bundle() compares it as bytes: a
    # forged blob can carry anything at all, and the device reader memcpys
    # those bytes without decoding. surrogateescape keeps the display form
    # lossless and byte-reversible either way; the C mirror never decodes.
    sb = blob[off:off + n_slug]
    return {"slug": sb.decode("utf-8", "surrogateescape"), "slug_b": sb,
            "elements": elems, "sprites": sprs,
            "scalars": {n: _ot._sext(scal[i], _ot.THEME_FIELDS[i][1])
                        for i, (n, _t) in enumerate(_ot.THEME_FIELDS)}}


def pack_index(recs):
    """The index region: a two-word head, then one fixed record per section."""
    out = struct.pack("<2I", len(recs), 0)
    for r in recs:
        out += struct.pack(IDX_PACK, *[int(r[f]) for f in IDX_ORDER])
    return out


def unpack_index(region):
    """Read the head, and trust it only as far as the region's length agrees."""
    n, _rsvd = struct.unpack_from("<2I", region, 0)
    if not 1 <= n <= SECT_MAX:
        raise ValueError("index claims %d sections; a bundle holds 1..%d"
                         % (n, SECT_MAX))
    if len(region) < 8 + n * IDX_WORDS * 4:
        raise ValueError("index claims %d sections in %d region bytes — torn file"
                         % (n, len(region)))
    recs = [dict(zip(IDX_ORDER,
                     struct.unpack_from(IDX_PACK, region, 8 + i * IDX_WORDS * 4)))
            for i in range(n)]
    return n, recs


def pack_header3(name, n_sections, geom_size, geom_crc, art_off, art_size,
                 art_crc, serial=0, baked_ms=0):
    """FORMAT 2's header, stamped FORMAT 3 (see the module docstring for the swapped
    meaning of words 9/10)."""
    hdr = [0] * _ot.HDR_WORDS
    hdr[0], hdr[1], hdr[2], hdr[3] = (_ot._u(_ot.MAGIC), _ot._u(FORMAT3),
                                      _ot._u(GEOM_ABI3), _ot._u(_ot.HDR_WORDS * 4))
    hdr[4] = _ot._u(_ot.HDR_WORDS * 4)      # the geometry region follows the header
    hdr[5], hdr[6] = _ot._u(geom_size), _ot._u(geom_crc)
    hdr[7], hdr[8] = _ot._u(art_size), _ot._u(art_crc)
    hdr[9], hdr[10] = _ot._u(n_sections), _ot._u(0)
    hdr[11], hdr[12], hdr[13] = _ot._u(serial), _ot._u(baked_ms), _ot._u(art_off)
    _ot._setword(hdr, _ot.W_NAME, name, _ot.NAME_MAX)
    hdr[_ot.W_HDR_CRC] = _ot._hdr_crc(hdr)
    return b"".join(struct.pack("<I", v) for v in hdr)


def write_bundle(path, name, sections, serial=0, baked_ms=0):
    """Emit one FORMAT 3 `.orbtheme`. `sections` is a list of dicts: slug,
    elements, and optional scalars / sprites / art (bytes). Slug order is
    normalised here — a person reading --inspect should not have to know the
    bake's build order, and the reader must find a slug the same way twice.

    The budget gate, bake end (D29, P6.2): check_budget(run, art_size) — the two
    INSTALL sums, geometry region against GEOM_CAP and pixel payload against
    SLOT_CAP - SLOT_ART_OFF, == cap accepted. Refusals are the sentences the
    composer shows and the reader and device repeat verbatim (D26)."""
    if not name:
        raise ValueError("a bundle needs a name: it is the theme_id input (R4)")
    seen = set()
    for s in sections:
        slug = check_slug(s["slug"])
        if slug in seen:
            raise ValueError("bundle lists app %r twice; one slug resolves to one "
                             "section and cannot hold two answers" % slug)
        seen.add(slug)
    if not 1 <= len(sections) <= SECT_MAX:
        raise ValueError("a bundle holds 1..%d app sections, not %d"
                         % (SECT_MAX, len(sections)))
    sections = sorted(sections, key=lambda s: s["slug"])

    blobs = []
    for s in sections:
        art = s.get("art") or b""
        for i, sp in enumerate(s.get("sprites") or ()):
            if int(sp["off"]) + int(sp["size"]) > len(art):
                raise ValueError("section %r sprite[%d] runs past its own bank "
                                 "(%d+%d of %d)" % (s["slug"], i, sp["off"],
                                                     sp["size"], len(art)))
        if not s.get("elements") and not art:
            raise ValueError("section %r carries neither elements nor art" % s["slug"])
        blobs.append(pack_blob(s["slug"], s.get("elements"), s.get("scalars"),
                               s.get("sprites", ())))

    # Pass 1 sizes the region; pass 2 stamps the records once every offset exists.
    # geom_off is derived, never guessed — stating it is the point, and read_bundle
    # holds the walk against it.
    head = 8 + IDX_WORDS * 4 * len(sections)
    offs, run = [], head
    for b in blobs:
        offs.append(run)
        run += len(b)

    art_off = _ot.art_offset(run)          # sector-aligned after header + region
    recs, art_blobs, at = [], [], art_off
    for s, blob, goff in zip(sections, blobs, offs):
        a = s.get("art") or b""
        recs.append({"slug_off": goff + len(blob) - ((len(s["slug"].encode("utf-8")) + 3) & ~3),
                     "slug_len": len(s["slug"].encode("utf-8")),
                     "abi_min": s.get("abi_min", GEOM_ABI3),
                     "abi_max": s.get("abi_max", GEOM_ABI3),
                     "n_elem": len(s.get("elements") or ()),
                     "geom_off": goff, "geom_size": len(blob),
                     "geom_crc": zlib.crc32(blob) & 0xFFFFFFFF,
                     "art_off": at, "art_size": len(a),
                     "art_crc": zlib.crc32(a) & 0xFFFFFFFF if a else 0,
                     "scr_w": s.get("scr_w", 0), "scr_h": s.get("scr_h", 0)})
        art_blobs.append(a)
        at += len(a)
        at = (at + _ot.SECTOR - 1) & ~(_ot.SECTOR - 1)
    any_art = any(len(a) for a in art_blobs)
    # art_size EXCLUDES the final bank's alignment pad: the installed slot reads
    # art_size bytes and pads nothing. Inter-bank pad is inside, because the walk
    # that finds each bank's start has to cross it.
    ends = []
    p = art_off
    for a in art_blobs:
        p += len(a)
        ends.append(p)
        p = (p + _ot.SECTOR - 1) & ~(_ot.SECTOR - 1)
    art_size = (max(e for e, a in zip(ends, art_blobs) if len(a))
                - art_off) if any_art else 0
    art_crc_all = zlib.crc32(b"".join(art_blobs)) & 0xFFFFFFFF if any_art else 0
    check_budget(run, art_size)     # the same sums the reader and the device check

    region = bytearray(pack_index(recs))
    for b in blobs:
        region += b
    geom_crc = zlib.crc32(bytes(region)) & 0xFFFFFFFF
    hdr = pack_header3(name, len(sections), len(region), geom_crc, art_off,
                       art_size, art_crc_all, serial, baked_ms)
    out = bytearray(hdr)
    out += region
    out += b"\0" * (art_off - len(out))
    for a in art_blobs:
        out += a
        if len(a):
            out += b"\0" * ((-len(a)) % _ot.SECTOR)
    out = bytes(out[:art_off + art_size])  # trim the last bank's alignment pad
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def read_bundle(path):
    """Whole-file read and every checksum, for the host suite and the bench; the
    device streams instead. The mirror of the writer's promises: header crc, region
    crc, per-section blob crc and length, per-bank crc, the geom_off and art_off
    the index stores against the ones the walks derive, and the slug each blob
    answers to. The refusal order below is the C reader's refusal order —
    tools/orb_bundle.c and its sentinel suite move with any change here."""
    with open(path, "rb") as f:
        raw = f.read()
    h = parse_header3(raw)
    if h["geom_off"] + h["geom_size"] > len(raw):
        raise ValueError("geometry region runs past the end of the file")
    region = raw[h["geom_off"]:h["geom_off"] + h["geom_size"]]
    if zlib.crc32(region) & 0xFFFFFFFF != h["geom_crc"]:
        raise ValueError("geometry checksum mismatch — the copy on the card is damaged")
    if len(region) < 8:
        # unpack_index() cannot even read its head; the header's own count is
        # the honest number to name, which is the sentence C reads back.
        raise ValueError("index claims %d sections in %d region bytes — torn file"
                         % (h["n_sections"], len(region)))
    n, recs = unpack_index(region)
    if n != h["n_sections"]:
        raise ValueError("header names %d sections, the index holds %d"
                         % (h["n_sections"], n))
    if h["art_off"] + h["art_size"] > len(raw):
        raise ValueError("art region runs past the end of the file")
    walk = 8 + IDX_WORDS * 4 * n           # the sequential truth, record by record
    aoff = h["art_off"]                    # the bank walk, sector after sector
    out = []
    for i, r in enumerate(recs):
        if r["geom_off"] != walk:
            raise ValueError("section %d stores geom_off %d where the walk says %d "
                             "— the index and the bytes contradict"
                             % (i, r["geom_off"], walk))
        walk += r["geom_size"]
        if walk > len(region):
            raise ValueError("section %d's blob range is outside the region" % i)
        blob = region[r["geom_off"]:r["geom_off"] + r["geom_size"]]
        if zlib.crc32(blob) & 0xFFFFFFFF != r["geom_crc"]:
            raise ValueError("section %d blob checksum mismatch" % i)
        b = unpack_blob(blob)
        if r["slug_off"] + r["slug_len"] > h["geom_size"]:
            # the index's own name does not fit the region it lives in
            raise ValueError("section %d: the index's slug and the blob's disagree" % i)
        want = raw[h["geom_off"] + r["slug_off"]:
                   h["geom_off"] + r["slug_off"] + r["slug_len"]]
        if b["slug_b"] != want:   # bytes, not text: see unpack_blob
            raise ValueError("section %d: the index's slug and the blob's disagree" % i)
        # The blob's own name must be one an app could later claim — the same
        # rule check_slug gates at write time, now on the read side, on BYTES
        # (orb_bundle.c's orb_bundle_slug_rule) so a forged non-UTF-8 slug
        # hears this sentence rather than a codec error.
        sb = b["slug_b"]
        if not (1 <= len(sb) <= SLUG_MAX) or any(c not in _SLUG_OK for c in sb):
            raise ValueError("bundle slug %r is not lowercase a-z 0-9 _ of 1..%d chars"
                             % (b["slug"], SLUG_MAX))
        if r["n_elem"] != len(b["elements"]):
            raise ValueError("section '%s': the index counts %d elements, the blob "
                             "carries %d" % (b["slug"], r["n_elem"], len(b["elements"])))
        if r["art_off"] != aoff:
            raise ValueError("section %d stores art_off %d where the art walk says "
                             "%d — the index and the bytes contradict"
                             % (i, r["art_off"], aoff))
        a = raw[r["art_off"]:r["art_off"] + r["art_size"]]
        if zlib.crc32(a) & 0xFFFFFFFF != r["art_crc"]:
            raise ValueError("section '%s' bank checksum mismatch — the copy on the "
                             "card is damaged" % b["slug"])
        aoff += (r["art_size"] + _ot.SECTOR - 1) & ~(_ot.SECTOR - 1)
        out.append({"rec": r, "blob": b, "art": a})
    slugs = [x["blob"]["slug"] for x in out]
    # Adjacent pairs, STRICT inequality: equal neighbors are a duplicate, and
    # the C reader's strcmp >= 0 refuses them too. Two records for one slug is
    # exactly the ambiguity a sorted index exists to make impossible.
    if any(b <= a for a, b in zip(slugs, slugs[1:])):
        raise ValueError("index is not slug-sorted; the resolver and --inspect both "
                         "walk it in order, and a shuffled one hides a lost section")
    return h, out


def find_section(sections, slug):
    """The resolver's one lookup: exact slug match, or None — D31's question stays
    a question here, and answering it (defaults, refusal, hiding the element) is
    the app's, at paint time. Kept a function, not an inline `next()`, so the
    firmware's orb_bundle_find() has ONE shape to mirror."""
    for x in sections:
        if x["blob"]["slug"] == slug:
            return x
    return None

