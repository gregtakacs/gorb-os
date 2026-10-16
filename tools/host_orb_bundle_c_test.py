#!/usr/bin/env python3
"""host_orb_bundle_c_test.py — the third leg of the FORMAT 3 three-way rule.

tools/orbbundle.py WRITES the bundles; main/art/orb_bundle.c READS them on the
device; this file compiles the shipped C reader with host gcc against a
zlib-backed esp_rom_crc shim and feeds it the writer's own bytes — golden and
refused — requiring the C reader's every verdict, field dump, and refusal
sentence to match orbbundle.py's. No board, no card, no partition: the
host_orbapp_reader_test.py discipline, applied to the bundle.

The vectors are the §22.6 refusal set plus the field-order sentinels: one
element table whose 17 fields all carry distinct values (the lesson the app
manifest suite learned when a field order drifted), the scalar sextet trio
(words 3/4 i8, word 10 i16 — the same table orb_bundle_scalar_sext mirrors),
and every forgery the Python reader refuses, restamped so the ONLY guard left
is the one under test. Two refusal pairs are labeled wording differences by
decision (orb_bundle.c's header comment states both); everything else must be
byte-identical.

    python3 tools/host_orb_bundle_c_test.py             # parity vectors
    python3 tools/host_orb_bundle_c_test.py --mutations # and the C mutants

--mutations recompiles the reader with each guard broken and requires the run
to differ (different output or a nonzero exit): an uncaught mutant here means
a device-side guard nothing is watching.
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbbundle as ob            # noqa: E402  (the writer, the oracle)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
READER_C = os.path.join(ROOT, "main", "art", "orb_bundle.c")
SECTOR = 4096

SHIM_CRC = """#pragma once
#include <stdint.h>
#include <stddef.h>
#include <zlib.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{ return (uint32_t)crc32((uLong)crc, buf, (uInt)len); }
"""

HARNESS_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_bundle.h"

#define F_LAYOUT 1u
#define F_FIND   2u
#define NO_ROW   0xFFFFFFFFu
#define BLOB_HEAD_B ((4u + ORB_BUNDLE_SCALARS_N) * 4u)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) return 2;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 2; }
    long n = ftell(fp);
    rewind(fp);
    uint8_t *buf = malloc((size_t)n + 1u);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return 2;
    }
    fclose(fp);

    size_t off = 0;
    uint32_t nc = rd32(buf + off); off += 4;
    for (uint32_t c = 0; c < nc; c++) {
        uint32_t flags = rd32(buf + off), id = rd32(buf + off + 4),
                 nfile = rd32(buf + off + 8);
        off += 12;
        const uint8_t *file = buf + off; off += nfile;
        uint32_t row = rd32(buf + off), nq = rd32(buf + off + 4); off += 8;
        const uint8_t *q = buf + off;
        char *query[8];
        for (uint32_t j = 0; j < nq; j++) {
            uint32_t ql = rd32(q); q += 4;
            query[j] = malloc(ql + 1u);
            if (!query[j]) return 2;
            memcpy(query[j], q, ql); query[j][ql] = '\0'; q += ql;
            off += 4u + ql;
        }
        if (nfile < 128) {
            /* The device hands this reader 128 bytes or nothing; the harness
             * says so in its own token rather than reading off the end. */
            printf("case%u refuse caller-short\n", id);
            continue;
        }
        uint8_t *fc = malloc(nfile);
        if (!fc) return 2;
        memcpy(fc, file, nfile);
        orb_bundle3_hdr_t h;
        orb_bundle_t b;
        const char *why = orb_bundle3_hdr_parse((const uint32_t *)(const void *)fc,
                                                &h);
        if (!why) why = orb_bundle_parse(&h, fc, nfile, &b);
        if (why) {
            printf("case%u refuse %s\n", id, why);
        } else {
            printf("case%u ok name %s\n", id, h.name);
            printf("case%u ok hdr %u %u %u %u %u %u %u %u\n", id,
                   b.n_sections, h.geom_off, h.geom_size, h.geom_crc,
                   h.art_off, h.art_size, h.art_crc, h.serial);
            for (uint32_t i = 0; i < b.n_sections; i++) {
                const orb_bundle_sec_t *s = &b.sec[i];
                printf("case%u ok sec %u %s %u %u %u %u %u %u %u %u %u %u\n",
                       id, i, s->slug, s->geom_off, s->geom_size,
                       s->abi_min, s->abi_max, s->n_elem, s->n_spr,
                       s->art_off, s->art_size, s->scr_w, s->scr_h);
                if (!(flags & F_LAYOUT)) continue;
                printf("case%u ok scal %u %u %d %d %d %u\n", id, i,
                       orb_bundle_scalar(s, 0), orb_bundle_scalar_sext(s, 3),
                       orb_bundle_scalar_sext(s, 4),
                       orb_bundle_scalar_sext(s, 10), orb_bundle_scalar(s, 10));
                if (s->n_spr) {
                    const uint8_t *sp = s->blob + BLOB_HEAD_B +
                        (uint32_t)s->n_elem * (ORB_BUNDLE_ELEM_WORDS * 4u);
                    printf("case%u ok spr %u %u %u %u %u\n", id, i,
                           rd32(sp), rd32(sp + 4), rd32(sp + 8), rd32(sp + 12));
                }
                if (i == 0 && row != NO_ROW && row < s->n_elem) {
                    orb_bundle_elem_t e;
                    orb_bundle_elem_at(s, row, &e);
                    printf("case%u ok elem %u n=%u class=%u flags=%u order=%u "
                           "alpha=%u slot=%u frame0=%u frames=%u x=%d y=%d w=%u "
                           "h=%u pivot_x=%d pivot_y=%d radius=%u degrees=%u "
                           "scale16=%u\n", id, row,
                           e.n, e.cls, e.flags, e.order, e.alpha, e.slot,
                           e.frame0, e.n_frames, e.x, e.y, e.w, e.h,
                           e.pivot_x, e.pivot_y, e.radius, e.degrees, e.scale16);
                }
            }
            if (flags & F_FIND) {
                for (uint32_t j = 0; j < nq; j++) {
                    const orb_bundle_sec_t *s = orb_bundle_find(&b, query[j]);
                    printf("case%u ok find %s %d\n", id, query[j],
                           s ? (int)(s - b.sec) : -1);
                }
            }
        }
        for (uint32_t j = 0; j < nq; j++) free(query[j]);
        free(fc);
    }
    free(buf);
    return 0;
}
"""



# ---------------------------------------------------------------------------
# compile and drive: the shipped reader, host gcc, zlib crc shim
# ---------------------------------------------------------------------------

def compile_reader(tmp, out_name, reader_c_text):
    shim = os.path.join(tmp, "shim")
    os.makedirs(shim, exist_ok=True)
    with open(os.path.join(shim, "esp_rom_crc.h"), "w") as f:
        f.write(SHIM_CRC)
    with open(os.path.join(tmp, "harness.c"), "w") as f:
        f.write(HARNESS_C)
    src = os.path.join(tmp, out_name + ".c")
    with open(src, "w") as f:
        f.write(reader_c_text)
    binp = os.path.join(tmp, out_name)
    cc = subprocess.run(
        ["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
         "-Wformat=2", "-I", shim, "-I", os.path.join(ROOT, "main", "art"),
         "-o", binp, src, os.path.join(tmp, "harness.c"), "-lz"],
        capture_output=True, text=True)
    if cc.returncode != 0:
        raise RuntimeError("gcc failed for %s:\n%s" % (out_name, cc.stderr))
    return binp


def run_bin(binp, cases):
    """cases: list of (id, flags, row, queries, raw). Writes the case file,
    runs the harness once, returns (exit code, stdout)."""
    cf = os.path.join(tmpdir, "cases.bin")
    out = bytearray(struct.pack("<I", len(cases)))
    for cid, flags, row, queries, raw in cases:
        out += struct.pack("<III", flags, cid, len(raw)) + raw
        out += struct.pack("<II", row, len(queries))
        for qry in queries:
            qb = qry.encode("utf-8")
            out += struct.pack("<I", len(qb)) + qb
    with open(cf, "wb") as f:
        f.write(bytes(out))
    run = subprocess.run([binp, cf], capture_output=True, text=True)
    return run.returncode, run.stdout


# ---------------------------------------------------------------------------
# vector builder: hand-assembled files so every forgery keeps the surrounding
# checksums honest — the restamp discipline of host_theme_format3_test.py,
# lifted into the builder itself
# ---------------------------------------------------------------------------

def art_offset(region_size):
    n = 128 + region_size
    return (n + SECTOR - 1) & ~(SECTOR - 1)


def art_total(banks, aoff0):
    """write_bundle's rule: art_size ends at the last bank's own byte; the
    final bank's alignment pad is outside, inter-bank pad is inside."""
    p, end = aoff0, aoff0
    for a in banks:
        if a:
            end = p + len(a)
        p = (p + len(a) + SECTOR - 1) & ~(SECTOR - 1)
    return end - aoff0 if any(banks) else 0


def build(name="Parity Bundle", specs=(), head_n=None, idx_n=None, geom_len=None,
          hdr_words=None, hdr_crc_break=False, serial=3):
    """specs: dicts of slug / elements / scalars / sprites / art, plus optional
    rec (overrides the computed index record) and raw_blob (forged blob bytes,
    which the record's geom_crc still answers to). head_n and geom_len forge
    the counts the readers cross-check; hdr_words patches raw header words
    before the header crc is re-stamped — the file stays honest everywhere
    except the one rule under test."""
    blobs, banks = [], []
    for sp in specs:
        blob = sp.get("raw_blob")
        if blob is None:
            blob = ob.pack_blob(sp["slug"], sp.get("elements"),
                                sp.get("scalars"), sp.get("sprites", ()))
        blobs.append(blob)
        banks.append(sp.get("art") or b"")
    n = head_n if head_n is not None else len(specs)
    recs, offs, run = [], [], 8 + 40 * len(specs)
    for blob in blobs:
        offs.append(run)
        run += len(blob)
    aoff = art_offset(run)
    for sp, blob, goff in zip(specs, blobs, offs):
        art = sp.get("art") or b""
        sb = sp["slug"].encode("utf-8")
        r = {"slug_off": goff + len(blob) - ((len(sb) + 3) & ~3),
             "slug_len": len(sb),
             "abi_min": sp.get("abi_min", ob.GEOM_ABI3),
             "abi_max": sp.get("abi_max", ob.GEOM_ABI3),
             "n_elem": len(sp.get("elements") or ()),
             "geom_off": goff, "geom_size": len(blob),
             "geom_crc": zlib.crc32(blob) & 0xFFFFFFFF,
             "art_off": aoff, "art_size": len(art),
             "art_crc": zlib.crc32(art) & 0xFFFFFFFF if art else 0,
             "scr_w": sp.get("scr_w", 466), "scr_h": sp.get("scr_h", 466)}
        r.update(sp.get("rec") or {})
        recs.append(r)
        aoff = (aoff + len(art) + SECTOR - 1) & ~(SECTOR - 1)
    region = bytearray(ob.pack_index(recs) + b"".join(blobs))
    if idx_n is not None:
        # forge the INDEX head word alone: the count the readers
        # range-check first; the header keeps the true count
        struct.pack_into("<I", region, 0, idx_n)
    region = bytes(region)
    glen = len(region) if geom_len is None else geom_len
    art_size = art_total(banks, art_offset(len(region)))
    hdr = bytearray(ob.pack_header3(name, n, glen,
                                    zlib.crc32(region[:glen]) & 0xFFFFFFFF,
                                    art_offset(len(region)), art_size,
                                    zlib.crc32(b"".join(banks)) & 0xFFFFFFFF,
                                    serial))
    for word, val in (hdr_words or {}).items():
        hdr[word * 4:word * 4 + 4] = struct.pack("<I", val)
    words = list(struct.unpack("<32I", bytes(hdr)))
    words[28] = 0
    words[28] = zlib.crc32(struct.pack("<32I", *words)) & 0xFFFFFFFF
    if hdr_crc_break:
        words[28] ^= 0x55AA0011
    hdr[28 * 4:28 * 4 + 4] = struct.pack("<I", words[28])
    out = (bytes(hdr) + region
           + b"\0" * (art_offset(len(region)) - 128 - len(region)))
    for a in banks:
        out += a
        if a:
            out += b"\0" * ((-len(a)) % SECTOR)
    return out


def read_verdict(raw):
    """The host oracle: ob.read_bundle's answer to the same bytes."""
    p = os.path.join(tmpdir, "vec.orbtheme")
    with open(p, "wb") as f:
        f.write(raw)
    try:
        return ob.read_bundle(p)
    except ValueError as e:
        return str(e)


def elem(n, cls, **kw):
    r = ob._ts.blank_element()
    r["n"] = n; r["class"] = cls; r["n_frames"] = 1
    r.update(kw)
    return r


# ---------------------------------------------------------------------------
# the vectors: ids 0/1/30 accept (with dumps), the rest each trip exactly one
# refusal rule; `same` means both readers must print the identical sentence,
# `pair` names the two labeled wordings side by side
# ---------------------------------------------------------------------------

F_LAYOUT, F_FIND, NO_ROW = 1, 2, 0xFFFFFFFF

SENT = elem(1, 1, flags=3, order=4, alpha=5, slot=6, frame0=7, n_frames=8,
            x=-9, y=-10, w=11, h=12, pivot_x=-13, pivot_y=-14, radius=15,
            degrees=16, scale16=17)
SCAL = {"bg": 0xCAFEB0BA, "shadow_dx": -3, "shadow_dy": -4, "bg_fps": -300}
SPR = [{"off": 0, "size": 1024, "w": 32, "h": 32}]
ART = bytes((i * 3 + 1) & 0xFF for i in range(2048))
SOLO = {"slug": "solo", "elements": [SENT], "scalars": SCAL,
        "sprites": SPR, "art": ART}
BETA = {"slug": "beta", "elements": [elem(1, 0, w=1, h=1)], "scalars": {},
        "sprites": [], "art": b""}
ALPHA = {"slug": "alpha", "elements": [elem(2, 0, w=2, h=2)], "scalars": {},
         "sprites": [], "art": b""}

GOLDEN = build(name="Bundle Two Apps",
               specs=[dict(SOLO, slug="clock"), dict(BETA, slug="paint")],
               serial=3)
SOLO_RAW = build(specs=[SOLO])


def _sext_blob():
    scals = [0] * ob.SCALARS_N
    scals[0] = 0xCAFEB0BA          # bg u32
    scals[3] = 0xFD                # shadow_dx i8, upper bits CLEAR (torn form)
    scals[4] = 0xFC                # shadow_dy i8
    scals[10] = 0xFED4             # bg_fps i16
    e = SENT
    ewords = [_e for _e in ob._ts.pack_elements([e])]
    counts = struct.pack("<4I", 1, 0, ob.SCALARS_N, 4)
    return (counts + struct.pack("<%dI" % ob.SCALARS_N, *scals)
            + struct.pack("<%dI" % len(ewords), *ewords) + b"solo")


SEXT_BLOB = _sext_blob()


def _w(blob, idx, val):
    ba = bytearray(blob)
    struct.pack_into("<I", ba, idx * 4, val)
    return bytes(ba)


solo_blob = ob.pack_blob("solo", [SENT], SCAL, SPR)
solo_len = len(solo_blob)
solo_goff = 48
solo_aoff = art_offset(48 + solo_len)

# the cap vector: a sprite table a u16 record cannot name — 65536 entries,
# lengths consistent so ONLY the cap stands. The record is hand-packed with a
# small slug_off: the refusal fires inside the blob check, before either
# reader ever looks at the slug window, so the window need not be honest
CAP_BLOB = (struct.pack("<4I", 0, 0x10000, ob.SCALARS_N, 4)
            + struct.pack("<%dI" % ob.SCALARS_N, *([7] * ob.SCALARS_N))
            + b"\0" * (0x10000 * 16) + b"solo")


def _cap_file():
    rec = struct.pack(ob.IDX_PACK, 0, 4, ob.GEOM_ABI3, ob.GEOM_ABI3, 0, 48,
                      len(CAP_BLOB), zlib.crc32(CAP_BLOB) & 0xFFFFFFFF,
                      128 + 48 + len(CAP_BLOB), 0, 0, 466, 466)
    region = struct.pack("<2I", 1, 0) + rec + CAP_BLOB
    return ob.pack_header3("Cap File", 1, len(region),
                           zlib.crc32(region) & 0xFFFFFFFF,
                           128 + len(region), 0, 0, 3) + region

VECTORS = [
    # id, flags, row, queries, raw, kind, expect, rule substring
    (0, F_LAYOUT | F_FIND, 0, ["clock", "paint", "nowhere"], GOLDEN,
     "dump", None, None),
    (1, F_LAYOUT | F_FIND, 0, ["solo"], SOLO_RAW, "ok", None, None),
    (2, 0, NO_ROW, [], build(specs=[dict(SOLO, raw_blob=_w(solo_blob, 2, 11))]),
     "same", None, "scalars"),
    (3, 0, NO_ROW, [],
     build(specs=[dict(SOLO, raw_blob=solo_blob + b"\0\0\0\0")]),
     "same", None, "torn file"),
    (4, 0, NO_ROW, [], build(specs=[dict(SOLO, raw_blob=solo_blob[:116])]),
     "same", None, "shorter than"),
    (5, 0, NO_ROW, [], _cap_file(),
     "same", None, "u16 counts"),
    (6, 0, NO_ROW, [], build(specs=[dict(SOLO, rec={"n_elem": 5})]),
     "same", None, "index counts"),
    (7, 0, NO_ROW, [], build(specs=[dict(SOLO, rec={"slug_off": 12})]),
     "same", None, "disagree"),
    (8, 0, NO_ROW, [], build(specs=[dict(SOLO, rec={"slug_len": 30})]),
     "same", None, "disagree"),
    (9, 0, NO_ROW, [], build(specs=[dict(SOLO, slug="Solo")]),
     "same", None, "lowercase"),
    (10, 0, NO_ROW, [], build(specs=[dict(SOLO, slug="solÿe")]),
     "same", None, "lowercase"),
    (11, 0, NO_ROW, [], build(specs=[BETA, ALPHA]),
     "same", None, "slug-sorted"),
    (12, 0, NO_ROW, [], build(specs=[BETA, dict(BETA)]),
     "same", None, "slug-sorted"),
    (13, 0, NO_ROW, [],
     build(specs=[dict(SOLO, rec={"geom_off": solo_goff + 4})]),
     "same", None, "contradict"),
    (14, 0, NO_ROW, [], build(specs=[dict(SOLO, rec={"geom_crc": 0xDEADBEEF})]),
     "same", None, "blob checksum"),
    (15, 0, NO_ROW, [],
     build(specs=[dict(SOLO, rec={"art_off": solo_aoff + SECTOR})]),
     "same", None, "art walk"),
    (16, 0, NO_ROW, [],
     build(specs=[dict(SOLO, rec={"geom_size": solo_len + 8})]),
     "same", None, "outside the region"),
    (17, 0, NO_ROW, [], build(specs=[SOLO], geom_len=0),
     "same", None, "torn file"),
    (18, 0, NO_ROW, [], build(specs=[SOLO], geom_len=40),
     "same", None, "torn file"),
    (19, 0, NO_ROW, [], build(specs=[SOLO], idx_n=9),
     "same", None, "1..8"),
    (20, 0, NO_ROW, [], build(specs=[SOLO], idx_n=0),
     "same", None, "1..8"),
    (21, 0, NO_ROW, [], build(specs=[dict(SOLO, slug="clock"), BETA],
                              hdr_words={9: 1}),
     "same", None, "header names"),
    (22, 0, NO_ROW, [], build(specs=[SOLO], hdr_words={1: 2, 2: ob.GEOM_ABI3}),
     "pair", ("theme format this firmware cannot read",
              "theme format 2; this reader takes format 3"), "format"),
    (23, 0, NO_ROW, [], build(specs=[SOLO], hdr_words={2: 4}),
     "same", None, "geometry v4"),
    (24, 0, NO_ROW, [], build(specs=[SOLO], hdr_crc_break=True),
     "pair", ("container header is damaged — re-copy the file onto the card",
              "container header checksum mismatch — re-copy the file onto "
              "the card"), "header"),
    (25, 0, NO_ROW, [], build(name="q", specs=[SOLO],
                              hdr_words={14: 0, 15: 0, 16: 0, 17: 0}),
     "same", None, "no name"),
    (26, 0, NO_ROW, [],
     build(specs=[SOLO], hdr_words={5: len(build(specs=[SOLO])) + 1}),
     "same", None, "geometry region runs past"),
    (27, 0, NO_ROW, [],
     build(specs=[SOLO], hdr_words={13: len(build(specs=[SOLO])) + SECTOR}),
     "same", None, "art region runs past"),
    (30, 0, NO_ROW, ["solo"],
     build(specs=[dict(SOLO, sprites=[], art=b"")]), "ok", None, None),
    (50, F_LAYOUT, 0, [],
     build(specs=[{"slug": "solo", "elements": [SENT], "raw_blob": SEXT_BLOB,
                   "sprites": [], "art": b""}]),
     "dump", None, None),
]


# byte-flips inside the region / a bank: the region flip leans on the header's
# still-honest geometry crc; the bank flip needs no restamp at all, because
# banks ride the record's art_crc alone
def _flip(buf, off):
    ba = bytearray(buf)
    ba[off] ^= 0x01
    return bytes(ba)


def _flip_bank(buf, off):
    ba = bytearray(buf)
    ba[off] ^= 0x80
    return bytes(ba)


_geo = build(specs=[SOLO])
GEOM_CRC_FLIP = _flip(_geo, 140)                 # one region byte; the header
                                                 # keeps the crc it was baked
                                                 # with, so the geometry line
                                                 # is what must disagree
_aoff = struct.unpack_from("<I", _geo, 13 * 4)[0]
BANK_CRC_FLIP = _flip_bank(_geo, _aoff + 10)     # art byte; banks ride no
                                                 # region crc, so only the
                                                 # bank's own line disagrees
VECTORS += [
    (40, 0, NO_ROW, [], GEOM_CRC_FLIP, "same", None, "geometry checksum"),
    (41, 0, NO_ROW, [], BANK_CRC_FLIP, "same", None, "bank checksum"),
]



# ---------------------------------------------------------------------------
# run: one compile, one batch, then compare every case against the oracle
# ---------------------------------------------------------------------------

def lines_by_case(stdout):
    got = {}
    for line in stdout.splitlines():
        if not line.startswith("case"):
            continue
        head, _, rest = line.partition(" ")
        cid = int(head[4:])
        got.setdefault(cid, []).append(rest)
    return got


failures, nchecks = [], [0]


def check(cond, msg):
    nchecks[0] += 1
    if not cond:
        failures.append(msg)


def check_case(cid, flags, row, queries, raw, kind, expect, rule, cl):
    """One vector's whole claim: same verdict, same sentence, same fields."""
    pyv = read_verdict(raw)
    if kind in ("same", "pair"):
        check(isinstance(pyv, str), "case %d: oracle ACCEPTED a file that "
              "must refuse" % cid)
        refuse = [l for l in cl if l.startswith("refuse")]
        check(len(refuse) == 1, "case %d: C did not print one refuse line "
              "(got %r)" % (cid, cl))
        if not refuse:
            return
        csent = refuse[0][len("refuse "):]
        if kind == "same":
            check(csent == pyv, "case %d: sentence drift\n  C:  %s\n  py: %s"
                  % (cid, csent, pyv))
        else:
            c_exp, py_exp = expect
            check(csent == c_exp, "case %d: C wording\n  got %s" % (cid, csent))
            check(pyv == py_exp, "case %d: py wording\n  got %s" % (cid, pyv))
        check(rule in csent and rule in pyv,
              "case %d: refused for the wrong rule (%s / %s)" % (cid, csent, pyv))
        return
    check(not isinstance(pyv, str), "case %d: oracle refused a good file: %s"
          % (cid, pyv))
    check(any(l.startswith("ok hdr") for l in cl),
          "case %d: C refused a good file: %r" % (cid, cl))
    if isinstance(pyv, str) or not cl:
        return
    h, out = pyv
    f_ = [l for l in cl if l.startswith("ok hdr")][0].split()[2:]
    check(int(f_[0]) == h["n_sections"] and int(f_[1]) == h["geom_off"]
          and int(f_[2]) == h["geom_size"] and int(f_[3]) == h["geom_crc"]
          and int(f_[4]) == h["art_off"] and int(f_[5]) == h["art_size"],
          "case %d: header fields disagree: %s vs %r" % (cid, f_, h))
    secs = [l.split()[3:] for l in cl if l.startswith("ok sec")]
    check(len(secs) == len(out), "case %d: C dumped %d sections, oracle read %d"
          % (cid, len(secs), len(out)))
    for i, sf in enumerate(secs):
        if i >= len(out):
            break
        r, b = out[i]["rec"], out[i]["blob"]
        check(sf[0] == b["slug"] and sf[1] == str(r["geom_off"])
              and sf[2] == str(r["geom_size"]) and int(sf[3]) == r["abi_min"]
              and int(sf[4]) == r["abi_max"]
              and sf[5] == str(len(b["elements"]))
              and sf[6] == str(len(b["sprites"]))
              and sf[7] == str(r["art_off"]) and sf[8] == str(r["art_size"])
              and sf[9] == str(r["scr_w"]) and sf[10] == str(r["scr_h"]),
              "case %d sec %d: fields disagree: %s vs %r/%r"
              % (cid, i, sf, r, b["slug"]))
    if kind == "dump":
        el = [l for l in cl if l.startswith("ok elem")]
        check(len(el) == 1, "case %d: no sentinel element line" % cid)
        if el:
            d = dict(tok.split("=", 1) for tok in el[0].split()[3:])
            e0 = out[0]["blob"]["elements"][row]
            want = {k: str(v) for k, v in
                    (("n", e0["n"]), ("class", e0["class"]), ("flags", e0["flags"]),
                     ("order", e0["order"]), ("alpha", e0["alpha"]),
                     ("slot", e0["slot"]), ("frame0", e0["frame0"]),
                     ("frames", e0["n_frames"]), ("x", e0["x"]), ("y", e0["y"]),
                     ("w", e0["w"]), ("h", e0["h"]), ("pivot_x", e0["pivot_x"]),
                     ("pivot_y", e0["pivot_y"]), ("radius", e0["radius"]),
                     ("degrees", e0["degrees"]), ("scale16", e0["scale16"]))}
            check(d == want, "case %d: element field order/values disagree:\n"
                  "  C:  %r\n  py: %r" % (cid, d, want))
        sl = [l for l in cl if l.split()[1:2] == ["scal"] and l.split()[2] == "0"]
        s0 = out[0]["blob"]["scalars"]
        check(len(sl) == 1, "case %d: no scalar dump line" % cid)
        if sl:
            exp = sl[0].split()[3:]
            check(exp[0] == str(s0["bg"]) and exp[1] == str(s0["shadow_dx"])
                  and exp[2] == str(s0["shadow_dy"]) and exp[3] == str(s0["bg_fps"])
                  and int(exp[4]) & 0xFFFF == (s0["bg_fps"] & 0xFFFF),
                  "case %d: scalar raw/sext disagree: %s vs %r" % (cid, exp, s0))
    if flags & F_FIND:
        findl = {l.split()[2]: int(l.split()[3]) for l in cl
                 if l.startswith("ok find")}
        for q in queries:
            hit = ob.find_section(out, q)
            exp_idx = -1 if hit is None else out.index(hit)
            check(findl.get(q, -1) == exp_idx,
                  "case %d: resolver %r -> %s, oracle %d" % (cid, q, findl.get(q),
                                                             exp_idx))


# ---------------------------------------------------------------------------
# mutants: break one C guard at a time; the run must notice
# ---------------------------------------------------------------------------

MUTANTS = [
    ("drop the geom_off walk",
     "if (s->geom_off != walk)", "if (0 && s->geom_off != walk)", 13),
    ("accept a stale geometry crc",
     "if (esp_rom_crc32_le(0, region, region_size) != h->geom_crc)",
     "if (0 && esp_rom_crc32_le(0, region, region_size) != h->geom_crc)", 40),
    ("accept a stale blob crc",
     "if (esp_rom_crc32_le(0, s->blob, s->geom_size) != geom_crc)",
     "if (0 && esp_rom_crc32_le(0, s->blob, s->geom_size) != geom_crc)", 14),
    ("drop the blob range bound",
     "if (walk > region_size || s->geom_size > region_size - walk)",
     "if (0 && (walk > region_size || s->geom_size > region_size - walk))", 16),
    ("allow a length the counts do not produce",
     "if (want != len) {", "if (want > len) {", 3),
    ("ignore the scalar dialect word",
     "if (nsc != ORB_BUNDLE_SCALARS_N)", "if (0 && nsc != ORB_BUNDLE_SCALARS_N)", 2),
    ("drop the u16 count cap",
     "if (ne > 0xFFFFu || ns > 0xFFFFu)",
     "if (0 && (ne > 0xFFFFu || ns > 0xFFFFu))", 5),
    ("refuse the slug check with the wrong sentence",
     "the index's slug and the blob's disagree",
     "mut: slug line mangled", 7),
    ("read the record's slug_len as the blob's",
     "sl != rec_slen ||", "sl != (rec_slen & 0u) ||", 8),
    ("accept an unsorted index",
     "strcmp(b->sec[i - 1].slug, b->sec[i].slug) >= 0",
     "strcmp(b->sec[i - 1].slug, b->sec[i].slug) > 0", 11),
    ("skip the slug rule the writer gates on",
     "if (orb_bundle_slug_rule(region + slug_off, sl)) {",
     "if (0 && orb_bundle_slug_rule(region + slug_off, sl)) {", 9),
    ("drop the element cross-check",
     "if (rec_elem != ne) {", "if (0 && rec_elem != ne) {", 6),
    ("drop the art walk check",
     "if (s->art_off != aoff)", "if (0 && s->art_off != aoff)", 15),
    ("accept a stale bank crc",
     "!= s->art_crc) {", "== s->art_crc) {", 41),
    ("drop the art region bound",
     "if (h->art_off > file_size || h->art_size > file_size - h->art_off)",
     "if (0 && (h->art_off > file_size || h->art_size > file_size - h->art_off))",
     27),
    ("invert the resolver",
     "if (strcmp(b->sec[i].slug, slug) == 0)",
     "if (strcmp(b->sec[i].slug, slug) != 0)", 0),
    ("read one i8 scalar field as unsigned",
     "if (idx == 3 || idx == 4)", "if (idx == 93 || idx == 4)", 50),
    ("take the section count from the wrong header word",
     "o->n_sections = w[W_SCR_W];", "o->n_sections = w[W_SCR_H];", 0),
    ("read the element x coordinate from the wrong word",
     "e->x        = (int16_t)b32(r + 32);",
     "e->x        = (int16_t)b32(r + 36);", 0),
    ("skip the abi stamp check",
     "if (w[W_GEOM_ABI] != ORB_BUNDLE3_ABI)",
     "if (0 && w[W_GEOM_ABI] != ORB_BUNDLE3_ABI)", 23),
    ("compute the header crc over the stored crc",
     "zeroed[W_HDR_CRC] = 0;", ";", 0),
]


def mutants(binp_clean, cases, stdout_clean, tmp):
    caught, missed = 0, []
    with open(READER_C) as f:
        src = f.read()
    for name, old, new, _vec in MUTANTS:
        if src.count(old) != 1:
            missed.append("%s (anchor hit %d times)" % (name, src.count(old)))
            continue
        try:
            mb = compile_reader(tmp, "mut_%02d" % caught, src.replace(old, new, 1))
        except RuntimeError as e:
            missed.append("%s: mutated source did not compile (%s)"
                          % (name, str(e).splitlines()[-1][:120]))
            continue
        rc, out = run_bin(mb, cases)
        if rc == 0 and out == stdout_clean:
            missed.append("%s: the run did not notice" % name)
        else:
            caught += 1
    print("== orb_bundle_c mutants: %d/%d caught, %d anchor misses"
          % (caught, len(MUTANTS), len(missed)))
    for m in missed:
        print("  MISSED " + m)
    return missed


if __name__ == "__main__":
    tmpdir = tempfile.mkdtemp(prefix="orb_bundle_c_")
    binp = compile_reader(tmpdir, "orb_bundle_c_test", open(READER_C).read())
    cases = [(cid, flags, row, queries, raw)
             for (cid, flags, row, queries, raw, _k, _e, _r) in VECTORS]
    rc, stdout = run_bin(binp, cases)
    if rc != 0:
        print("harness exit %d\n%s" % (rc, stdout))
        sys.exit(1)
    got = lines_by_case(stdout)
    for vec in VECTORS:
        check_case(vec[0], vec[1], vec[2], vec[3], vec[4], vec[5], vec[6],
                   vec[7], got.get(vec[0], []))
    print("== orb_bundle_c parity: %d checks, %d failure(s)"
          % (nchecks[0], len(failures)))
    for m in failures:
        print("  FAIL " + m)
    code = 1 if failures else 0
    if "--mutations" in sys.argv:
        if mutants(binp, cases, stdout, tmpdir):
            code = 1
    sys.exit(code)




