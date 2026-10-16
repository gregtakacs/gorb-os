#!/usr/bin/env python3
"""P6.1's FORMAT 3 suite: the bundle writer and reader, checked on the host.

Four things this proves and nothing else.

1. A bundle round-trips: two sections written, read back byte-identical, in slug
   order, with negative scalars intact and the resolver answering a miss with None.
2. The budget gate bites at BOTH ends (D29, P6.2): a region past the dial's table
   area and a payload past the slot's art budget are refused in the sentences the
   device prints, at the writer and at the reader alike, and == cap is accepted.
3. Every refusal names what it refuses. The sentinel rows carry the word their own
   rule must produce — a refusal "for some other rule" passes the file's check and
   fails the suite, the lesson mut_element.py taught the element suite.
4. The two formats refuse each other on the FORMAT word, in both directions.

Run: python3 tools/host_theme_format3_test.py
"""
import os
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import orbtheme as ot           # noqa: E402
import orbbundle as ob          # noqa: E402
import theme_schema as ts       # noqa: E402

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def elem(n, cls, slot, order, **kw):
    r = ts.blank_element()
    r["n"] = n; r["class"] = cls; r["slot"] = slot
    r["order"] = order; r["n_frames"] = 1
    r.update(kw)
    return r


ART_A = bytes((i * 7) & 0xFF for i in range(4096))
ART_B = bytes((i * 13 + 5) & 0xFF for i in range(2048))
SECTIONS = [
    {"slug": "paint_studio", "scr_w": 464, "scr_h": 464,
     "elements": [elem(0, ts.CLASS_SPRITE, 0, 0, x=-12, y=300)],
     "sprites": [{"off": 0, "size": 2048, "w": 64, "h": 64}],
     "art": ART_B},
    {"slug": "clock", "scr_w": 464, "scr_h": 464,
     "elements": [elem(0, ts.CLASS_PLATE, 0, 0, x=100, y=100, w=32, h=32),
                  elem(1, ts.CLASS_HAND, 0, 1, frame0=6, n_frames=12,
                       flags=ts.EF_OS_ADV, radius=90)],
     "scalars": {"tourbillon_n": 12, "shadow_dx": -3, "bg": 0x11223344},
     "sprites": [{"off": 0, "size": 1024, "w": 32, "h": 32}],
     "art": ART_A},
]

tmp = tempfile.mktemp(suffix=".orbtheme")


# ---------------------------------------------------------------------------
# 1. Round trip.
# ---------------------------------------------------------------------------
size = ob.write_bundle(tmp, "Two App Bundle", SECTIONS)
raw = open(tmp, "rb").read()
h, secs = ob.read_bundle(tmp)
check(h["fmt"] == ob.FORMAT3, "header does not stamp format 3")
check(h["abi"] == ob.GEOM_ABI3, "header does not stamp geometry v3")
check(h["n_sections"] == 2, "header word 9 lost the section count")
check(h["name"] == "Two App Bundle", "the bundle name came back wrong (R4 input)")
check(ot.theme_id(h["name"]) == ot.theme_id("Two App Bundle"),
      "a bundle's theme_id does not run through the plain name (R4)")
check([x["blob"]["slug"] for x in secs] == ["clock", "paint_studio"],
      "sections did not come back slug-sorted, although the writer sorts them")
c = ob.find_section(secs, "clock")
p = ob.find_section(secs, "paint_studio")
check(c is not None and p is not None, "the resolver lost a section that is there")
check(ob.find_section(secs, "weather") is None,
      "find_section invented an answer for a slug no theme carries (D31)")
check(c["art"] == ART_A and p["art"] == ART_B, "a bank came back not byte-identical")
check(c["blob"]["scalars"]["shadow_dx"] == -3,
      "a negative i8 scalar came back as %r, not -3" % c["blob"]["scalars"]["shadow_dx"])
check(c["blob"]["scalars"]["tourbillon_n"] == 12, "a u8 scalar came back wrong")
check(c["blob"]["scalars"]["bg"] == 0x11223344, "a u32 scalar came back wrong")
check(p["blob"]["elements"][0]["x"] == -12,
      "an element at x=-12 did not survive the round trip")
check(c["blob"]["elements"][1]["flags"] & ts.EF_OS_ADV, "EF_OS_ADV was lost in the pack")
check(c["blob"]["sprites"] == SECTIONS[1]["sprites"], "the sprite directory drifted")
check(h["art_off"] % ot.SECTOR == 0, "the art region starts off a sector boundary")
check(all(r["rec"]["art_off"] % ot.SECTOR == 0 for r in secs),
      "a bank starts off a sector boundary; the device mmaps them")
check(size == len(raw), "write_bundle reported a size the file does not have")

# ---------------------------------------------------------------------------
# 2. The budget gate, both ends (D29, P6.2). Bake end: temporary caps, restored
#    after — they prove the gate SUMS, not the constants. Read end: a forged
#    header (word re-stamped, crc refreshed) proves the reader refuses a bundle
#    the slot cannot hold, in the same sentences, and that == cap passes.
# ---------------------------------------------------------------------------
SENT = []


def sent(name, must, fn):
    SENT.append((name, must, fn))


_real_geom, _real_slot = ob.GEOM_CAP, ob.SLOT_ART_CAP


def _gate(name, must, fn):
    """Sentinel shape for the budget rows, run NOW rather than queued: their
    trigger is a temporary cap, and a queue would run them after it is restored."""
    try:
        fn()
    except ValueError as _e:
        check(must in str(_e), "%s: refused for some other rule (%s)" % (name, _e))
    except Exception as _e:
        failures.append("%s: raised %s, not ValueError" % (name, type(_e).__name__))
    else:
        failures.append("%s: ACCEPTED, and a card would have been handed it" % name)


def _patch_hdr(raw, word, val):
    """One forged header word on the golden file, crc refreshed: every other
    promise the header made still holds, so only the gate under test can refuse."""
    b = bytearray(raw)
    struct.pack_into("<I", b, word * 4, val)
    words = list(struct.unpack("<32I", bytes(b[:128])))
    words[28] = 0
    words[28] = zlib.crc32(struct.pack("<32I", *words)) & 0xFFFFFFFF
    struct.pack_into("<I", b, 28 * 4, words[28])
    return bytes(b)


def _read_forged(word, val, suffix):
    p = tmp + suffix
    with open(p, "wb") as f:
        f.write(_patch_hdr(raw, word, val))
    ob.read_bundle(p)


try:
    # SECTIONS[1] alone: region 328 B (48 index + 280 blob), one 4096 B bank.
    ob.GEOM_CAP = 259
    _gate("region past the table area", "table area",
          lambda: ob.write_bundle(tmp + ".x", "Gate", [SECTIONS[1]]))
    _gate("the refusal is the device's pre-erase sentence", "overrun the pixels",
          lambda: ob.write_bundle(tmp + ".x", "Gate", [SECTIONS[1]]))
    ob.GEOM_CAP = _real_geom
    ob.SLOT_ART_CAP = 4095
    _gate("payload past the art budget", "larger than a slot",
          lambda: ob.write_bundle(tmp + ".x", "Gate", [SECTIONS[1]]))
    # The moved boundary, §21's lesson stated for themes: == cap is legal.
    ob.SLOT_ART_CAP = 4096
    ob.GEOM_CAP = 328
    try:
        ob.write_bundle(tmp + ".x", "Edge", [SECTIONS[1]])
        _h, _s = ob.read_bundle(tmp + ".x")
        check(_h["art_size"] == 4096 and _s[0]["blob"]["slug"] == "clock",
              "the == cap file wrote and read but not intact")
    except ValueError as _e:
        failures.append("== cap refused (%s): the gate is one byte too strict" % _e)
finally:
    ob.GEOM_CAP, ob.SLOT_ART_CAP = _real_geom, _real_slot

# The reader half, at the real caps: over refuses with the budget sentence
# BEFORE the name or the bounds get a turn; == cap passes the gate and dies at
# the file bounds — proof the gate accepted it.
sent("reader: region over the real cap", "table area",
     lambda: _read_forged(5, ob.GEOM_CAP + 1, ".f1"))
sent("reader: region refusal is the device's sentence", "overrun the pixels",
     lambda: _read_forged(5, ob.GEOM_CAP + 1, ".f1b"))
sent("reader: payload over the real cap", "larger than a slot",
     lambda: _read_forged(7, ob.SLOT_ART_CAP + 1, ".f2"))
sent("reader: region == cap passes the gate", "runs past",
     lambda: _read_forged(5, ob.GEOM_CAP, ".f3"))
sent("reader: payload == cap passes the gate", "runs past",
     lambda: _read_forged(7, ob.SLOT_ART_CAP, ".f4"))

# §21's deferred bound, paid: the FORMAT 2 writer takes the same two device
# sums, in copy_into's verbatim sentences. bake_clock.py --orbtheme reaches
# this function, and so does every host-side emitter of a legacy container.
sent("format 2 writer: oversized geometry", "geometry would overrun the pixels",
     lambda: ot.write_container(tmp + ".v2g", "BigGeom",
                                b"\x00" * (ot.GEOM_BUDGET + 1),
                                b"\x00" * 4096, 464, 464))
sent("format 2 writer: oversized payload", "theme is larger than a slot",
     lambda: ot.write_container(tmp + ".v2a", "BigArt",
                                b"\x00" * 64,
                                b"\x00" * (ot.ART_BUDGET + 1), 464, 464))
try:                                     # == cap ACCEPTED at both sums, for real
    n_g = ot.write_container(tmp + ".v2eg", "EdgeGeom", b"\x00" * ot.GEOM_BUDGET,
                             b"\x00" * 4096, 464, 464)
    n_a = ot.write_container(tmp + ".v2ea", "EdgeArt", b"\x00" * 64,
                             b"\x00" * ot.ART_BUDGET, 464, 464)
    check(n_g == ot.art_offset(ot.GEOM_BUDGET) + 4096
          and n_a == ot.art_offset(64) + ot.ART_BUDGET,
          "a == cap FORMAT 2 container wrote the wrong length")
except ValueError as _e:
    failures.append("format 2 == cap refused (%s): the bound is a byte too strict" % _e)

# ---------------------------------------------------------------------------
# 3. Refusals, each isolated by the word its own rule must produce.
# ---------------------------------------------------------------------------
sent("duplicate slug", "twice",
     lambda: ob.write_bundle(tmp + ".x", "Dup", [dict(SECTIONS[1]), dict(SECTIONS[1])]))
sent("slug charset", "lowercase",
     lambda: ob.write_bundle(tmp + ".x", "Bad", [dict(SECTIONS[1], slug="Clock")]))
sent("slug too long", "1..23", lambda: ob.check_slug("x" * 30))
sent("nine sections", "1..8",
     lambda: ob.write_bundle(tmp + ".x", "Nine",
                             [dict(SECTIONS[1], slug="s%d" % i) for i in range(9)]))
sent("nameless bundle", "name", lambda: ob.write_bundle(tmp + ".x", "", [SECTIONS[1]]))
sent("empty section", "neither",
     lambda: ob.write_bundle(tmp + ".x", "Void", [{"slug": "clock"}]))
sent("sprite past its bank", "past its own bank",
     lambda: ob.write_bundle(tmp + ".x", "Run",
                             [dict(SECTIONS[1], sprites=[{"off": 0, "size": 10 ** 7,
                                                          "w": 1, "h": 1}])]))
sent("unknown class row", "class", lambda: ob.pack_blob("clock", [elem(0, 99, 0, 0)]))
sent("unknown scalar name", "dialect",
     lambda: ob.pack_blob("clock", [elem(0, 1, 0, 0)], scalars={"nope": 1}))
sent("scalar out of range", "does not fit",
     lambda: ob.pack_blob("clock", [elem(0, 1, 0, 0)], scalars={"shadow_dx": 300}))
sent("index head count 9", "1..8",
     lambda: ob.unpack_index(struct.pack("<2I", 9, 0)))
sent("index longer than region", "torn file",
     lambda: ob.unpack_index(struct.pack("<2I", 1, 0) + b"\0" * 8))
sent("blob length contradicts counts", "torn file",
     lambda: ob.unpack_blob(ob.pack_blob("clock", [elem(0, 1, 0, 0)]) + b"\0\0\0\0"))

for _name, _must, _fn in SENT:
    try:
        _fn()
    except ValueError as _e:
        _m = str(_e)
        check(_must in _m, "%s: refused for some other rule (%s) — the sentinel does "
                          "not isolate the rule it claims" % (_name, _m))
    except Exception as _e:
        failures.append("%s: raised %s, not ValueError" % (_name, type(_e).__name__))
    else:
        failures.append("%s: ACCEPTED, and a card would have been handed it" % _name)


# ---------------------------------------------------------------------------
# 4. Cross-format refusal, both directions, on the FORMAT word.
# ---------------------------------------------------------------------------
v2 = tempfile.mktemp(suffix=".orbtheme")
ot.write_container(v2, "Legacy Clock", ot.pack_geom(
    {k: 0 for k, _t in ot.THEME_FIELDS},
    [ot.blank_spr()] * 5, [ot.blank_spr()] * 5, ot.blank_spr(),
    [], [], [], [], []), b"\0" * 4096, 464, 464)
try:
    ob.read_bundle(v2)
    failures.append("read_bundle swallowed a FORMAT 2 file")
except ValueError as _e:
    check("format 2" in str(_e) and "format 3" in str(_e),
          "FORMAT 2 met FORMAT 3 without naming both: %s" % _e)
try:
    ot.read_container(tmp)
    failures.append("orbtheme.read_container swallowed a FORMAT 3 file")
except ValueError as _e:
    check("format 3" in str(_e),
          "FORMAT 3 met the FORMAT 2 reader without naming 3: %s" % _e)

# A torn header still dies at the header, before any pixel: flip one byte of it.
torn = bytearray(raw)
torn[60] ^= 0x01
tornpath = tempfile.mktemp(suffix=".orbtheme")
open(tornpath, "wb").write(bytes(torn))
try:
    ob.read_bundle(tornpath)
    failures.append("a torn header passed the header check")
except ValueError as _e:
    check("header" in str(_e),
          "torn header refused after the pixels, not at the header: %s" % _e)

# Region tamper: one geometry byte flipped, header untouched -> region crc fires.
rt = bytearray(raw)
rt[h["geom_off"] + h["geom_size"] // 2] ^= 0x04
rtpath = tempfile.mktemp(suffix=".orbtheme")
open(rtpath, "wb").write(bytes(rt))
try:
    ob.read_bundle(rtpath)
    failures.append("a flipped geometry byte read back clean")
except ValueError as _e:
    check("geometry checksum" in str(_e),
          "geometry tamper refused for the wrong reason: %s" % _e)

# Bank tamper: one pixel byte flipped -> a checksum sentence, not silence.
bt = bytearray(raw)
bt[secs[0]["rec"]["art_off"] + 8] ^= 0x01
btpath = tempfile.mktemp(suffix=".orbtheme")
open(btpath, "wb").write(bytes(bt))
try:
    ob.read_bundle(btpath)
    failures.append("a flipped pixel byte read back clean")
except ValueError as _e:
    check("checksum" in str(_e) and "damaged" in str(_e),
          "bank tamper refused without the re-copy sentence: %s" % _e)

# ---------------------------------------------------------------------------
# 5. The two inner guards, reached only when the OUTER checksum is re-stamped:
#    a repack tool that moves a blob without re-stamping geom_off, and a blob CRC
#    left stale under a fresh region CRC (the installer streams per §22.3 and
#    verifies each blob alone — the host reader must fail it the same way).
# ---------------------------------------------------------------------------
def restamp(buf):
    """Give a tampered region a fresh region crc and a fresh header crc, so the
    only guard left between the file and acceptance is the one under test."""
    buf = bytearray(buf)
    gsz = struct.unpack_from("<I", buf, 20)[0]          # W_GEOM_SIZE = word 5
    region = bytes(buf[128:128 + gsz])
    struct.pack_into("<I", buf, 24, zlib.crc32(region) & 0xFFFFFFFF)   # W_GEOM_CRC
    words = list(struct.unpack("<32I", bytes(buf[:128])))
    words[28] = 0
    words[28] = zlib.crc32(struct.pack("<32I", *words)) & 0xFFFFFFFF
    buf[112:116] = struct.pack("<I", words[28])
    return bytes(buf)


# Move section 1's stored geom_off by 4 and re-stamp: only the walk check stands.
bad_off = bytearray(raw)
# section 1's record is the second: at geom_off + 8 + 10 words.
r1 = 128 + 8 + ob.IDX_WORDS * 4
go_word = struct.unpack_from("<I", bytes(bad_off), r1 + 12)[0]  # geom_off: past the 4 u16s + slug_off
struct.pack_into("<I", bad_off, r1 + 12, go_word + 4)
offpath = tempfile.mktemp(suffix=".orbtheme")
open(offpath, "wb").write(restamp(bytes(bad_off)))
try:
    ob.read_bundle(offpath)
    failures.append("a blob the index mislocates passed the walk check")
except ValueError as _e:
    check("contradict" in str(_e),
          "geom_off lie refused for the wrong reason: %s" % _e)

# Flip one byte INSIDE section 0's blob, re-stamp region+header: only its own blob
# crc now disagrees with the bytes, which is exactly what a streaming installer sees.
bad_blob = bytearray(raw)
b0 = 128 + secs[0]["rec"]["geom_off"]
bad_blob[b0 + 20] ^= 0x10
blobpath = tempfile.mktemp(suffix=".orbtheme")
open(blobpath, "wb").write(restamp(bytes(bad_blob)))
try:
    ob.read_bundle(blobpath)
    failures.append("a blob with a stale crc read back clean")
except ValueError as _e:
    check("blob checksum" in str(_e),
          "per-blob tamper refused for the wrong reason: %s" % _e)

for f in (tmp, v2, tornpath, rtpath, btpath, offpath, blobpath,
          tmp + ".x", tmp + ".f1", tmp + ".f1b", tmp + ".f2", tmp + ".f3",
          tmp + ".f4", tmp + ".v2g", tmp + ".v2a", tmp + ".v2eg", tmp + ".v2ea"):
    try:
        os.unlink(f)
    except OSError:
        pass

print("== FORMAT 3 BUNDLE SUITE: %d checks, %d refusal(s), %d failure(s)"
      % (30 + len(SENT) + 2, len(SENT) + 2, len(failures)))
for _f in failures:
    print("  FAIL " + _f)
sys.exit(1 if failures else 0)


