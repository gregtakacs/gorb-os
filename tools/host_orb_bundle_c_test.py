#!/usr/bin/env python3
"""host_orb_bundle_c_test.py — the third leg of the FORMAT 3 three-way rule.

tools/orbbundle.py WRITES the bundles; main/art/orb_bundle.c READS them on the
device; this file compiles the shipped C reader with host gcc against a
zlib-backed esp_rom_crc shim and feeds it the writer's own bytes — golden and
refused — requiring the C reader's every verdict, field dump, and refusal
sentence to match orbbundle.py's. No board, no card, no partition: the
host_orbapp_reader_test.py discipline, applied to the bundle.

The vectors are the §22.6 refusal set plus the field-order sentinels: one
element table whose 19 fields all carry distinct values (the lesson the app
manifest suite learned when a field order drifted), the scalar sextet trio
(words 3/4 i8, word 10 i16 — the same table orb_bundle_scalar_sext mirrors),
and every forgery the Python reader refuses, restamped so the ONLY guard left
is the one under test. Two refusal pairs are labeled wording differences by
decision (orb_bundle.c's header comment states both); everything else must be
byte-identical.

The STREAM family (P6.3) pins the second door, the one the installer uses: the
geometry region on its own, each bank as bytes across a wire, and the rebase
that turns a container's offsets into a slot's. Its claim is one rule — every
check the whole-buffer reader makes before a bank's bytes says the same thing
through orb_bundle_parse_region(), and the bank check moves onto
orb_bundle_bank_rule(). Probes name the installer's calls, so a vector can ask
for the wire before the region and the door must say so. Cases 70-85. Expectations
come from the writer's own parse of the same bytes, never from the C.

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
import orbtheme as _OT            # noqa: E402  (the header's own word map)
import theme_schema as ts         # noqa: E402  (the row width the stride vectors forge)

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

#include "esp_rom_crc.h"
#include "orb_bundle.h"

#define F_LAYOUT 1u
#define F_FIND   2u
#define F_STREAM 4u
#define NO_ROW   0xFFFFFFFFu
#define BLOB_HEAD_B ((4u + ORB_BUNDLE_SCALARS_N) * 4u)
#define STR_ART    0x00010000u   /* orb_theme.c SLOT_ART_OFF: where banks install */
#define STR_CAP    (STR_ART + ORB_THEME_SLOT_CAP) /* orb_theme.c SLOT_CAP: derived from
                                                  * the one exported cap, not restated:
                                                  * this literal held the retired 4 MB
                                                  * pair while the device said 8 MB */

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
        const char *hw = orb_bundle3_hdr_parse((const uint32_t *)(const void *)fc,
                                               &h);
        const char *why = hw;
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
                           "scale16=%u kind=%u src=%u\n", id, row,
                           e.n, e.cls, e.flags, e.order, e.alpha, e.slot,
                           e.frame0, e.n_frames, e.x, e.y, e.w, e.h,
                           e.pivot_x, e.pivot_y, e.radius, e.degrees, e.scale16,
                           e.kind, e.src);
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
        /* ---- the streaming door (P6.3): region alone, then banks on a wire ----
         * Mirrors orb_theme.c's copy_bundle3() call order: parse the region the
         * card gave us, rebase the records onto the slot, then stream each bank
         * in chunks and settle it with orb_bundle_bank_rule(). The header is
         * parsed a second time here on purpose: the whole-buffer verdict above is
         * already on the books, and the region door owes the SAME sentence. */
        if ((flags & F_STREAM) && !hw) {
            orb_bundle3_hdr_t w = h;
            orb_bundle_t b2;
            memset(&b2, 0, sizeof b2);
            uint8_t *rb = NULL, ch[1009];
            uint32_t rbsz = 0, fbase = h.art_off, rbase = h.art_off;
            uint32_t bound = nfile;
            int have = 0;
            for (uint32_t j = 0; j < nq; j++) {
                const char *qq = query[j];
                if (!strcmp(qq, "region")) {
                    uint32_t av = (w.geom_off <= nfile) ? (nfile - w.geom_off) : 0u;
                    uint32_t cp = (w.geom_size < av) ? w.geom_size : av;
                    uint8_t *t = malloc(w.geom_size ? w.geom_size : 1u);
                    if (!t) { free(rb); return 2; }
                    memset(t, 0, w.geom_size ? w.geom_size : 1u);
                    memcpy(t, fc + w.geom_off, cp);   /* a truncated file stays truncated */
                    free(rb);
                    rb = t; rbsz = w.geom_size; have = 0;
                    const char *r = orb_bundle_parse_region(&w, rb, rbsz, nfile, &b2);
                    if (r) printf("case%u R refuse %s\n", id, r);
                    else {
                        have = 1;
                        printf("case%u R ok %u %u %u %u\n", id, b2.n_sections,
                               rbsz, w.geom_crc, w.art_off);
                    }
                } else if (!strncmp(qq, "bank:", 5)) {
                    unsigned si = 0; char md[8] = {0};
                    if (!have || sscanf(qq, "bank:%u:%7s", &si, md) != 2 ||
                        si >= b2.n_sections) {
                        printf("case%u B skip\n", id);
                        continue;
                    }
                    const orb_bundle_sec_t *s = &b2.sec[si];
                    uint32_t want = s->art_size, done = 0, crc = 0;
                    if (strcmp(md, "SHORT") && strcmp(md, "LYE") && strcmp(md, "LYEC")) {
                        want = s->art_size;
                    } else if (s->art_size) {
                        want = s->art_size - 1u;      /* the wire stopped early */
                    }
                    size_t pos = (size_t)fbase + (s->art_off - rbase);
                    while (done < want) {
                        uint32_t n = (want - done > sizeof ch) ? (uint32_t)sizeof ch
                                                               : want - done;
                        if (pos + n > (size_t)nfile)
                            n = (pos < (size_t)nfile)
                                ? (uint32_t)((size_t)nfile - pos) : 0u;
                        if (!n) break;
                        memcpy(ch, fc + pos, n);
                        crc = esp_rom_crc32_le(crc, ch, n);
                        pos += n; done += n;
                    }
                    if (!strcmp(md, "BAD")) crc ^= 0x80808080u;
                    if (!strcmp(md, "LYE")) crc = s->art_crc ^ 0x80808080u;
                    if (!strcmp(md, "LYEC")) crc = s->art_crc;
                    const char *r = orb_bundle_bank_rule(s, done, crc);
                    if (r) printf("case%u B refuse %u %s %u %u | %s\n", id, si,
                                  s->slug, done, crc, r);
                    else printf("case%u B ok %u %s %u %u\n", id, si, s->slug, done, crc);
                } else if (!strncmp(qq, "rebase", 6)) {
                    uint32_t off = STR_ART, cp = STR_CAP, old = 0;
                    if (sscanf(qq, "rebasecap:%u:%u", &off, &cp) != 2 &&
                        sscanf(qq, "rebase:%u", &off) != 1 &&
                        strcmp(qq, "rebase") != 0) {
                        printf("case%u X skip\n", id);
                        continue;
                    }
                    if (!have) { printf("case%u X skip\n", id); continue; }
                    orb_bundle3_hdr_t w2 = w;
                    orb_bundle_t b3 = b2;
                    const char *r = orb_bundle_rebase(&b3, &w2, rb, off, cp, &old);
                    if (r) {
                        printf("case%u X refuse %s\n", id, r);
                        continue;
                    }
                    printf("case%u X ok %u %u %u %u", id, old, w2.art_off,
                           w2.geom_crc, b3.n_sections);
                    for (uint32_t i = 0; i < b3.n_sections; i++)
                        printf(" %u", b3.sec[i].art_off);
                    printf("\n");
                    w = w2; b2 = b3; fbase = old; rbase = off; bound = cp;
                } else if (!strcmp(qq, "secdump")) {
                    for (uint32_t i = 0; have && i < b2.n_sections; i++)
                        printf("case%u S %u %s %u %u\n", id, i, b2.sec[i].slug,
                               b2.sec[i].art_off, b2.sec[i].art_size);
                } else if (!strcmp(qq, "recheck")) {
                    const char *r = have ? orb_bundle_parse_region(&w, rb, rbsz,
                                                                   bound, &b2)
                                         : "rechecked with no region parsed";
                    if (r) printf("case%u C refuse %s\n", id, r);
                    else printf("case%u C ok %u\n", id, b2.n_sections);
                } else {
                    printf("case%u probe unknown\n", id);
                }
            }
            free(rb);
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
        return ob.read_bundle(p, theme=False)  # parity is wire-level; the door rule has its own vectors
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

# Class BANK so the two appended columns may carry values at all (they belong to
# a complication row alone), kind=2/src=9 because both must be valid ids AND
# distinct sentinels; alpha moves to 18 so every field still carries its own
# value, which is the whole point of a field-order sentinel.
SENT = elem(1, 5, flags=3, order=4, alpha=18, slot=6, frame0=7, n_frames=8,
            x=-9, y=-10, w=11, h=12, pivot_x=-13, pivot_y=-14, radius=15,
            degrees=16, scale16=17, kind=2, src=9)
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
# The row width the writer emits TODAY, read from the schema that owns it. D34 took it
# from 19 words to 23; a literal 19 in the two stride vectors below would have spliced
# The row width the writer emits for a DEFAULT (v3) pack_blob, read from the
# schema that owns it. D34 took it 19->23; C4's delta columns live at 25, and
# the delta dialect is opt-in per section, so the default pack a parity vector
# builds still lays 23. A literal here would splice a legal row wrongly the
# moment a column moves.
EW = ts.ELEMENT_WORDS_V3
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
     build(specs=[dict(SOLO, raw_blob=solo_blob + b"\0\0")]),
     "same", None, "torn file"),
    (4, 0, NO_ROW, [], build(specs=[dict(SOLO, raw_blob=solo_blob[:116])]),
     "same", None, "shorter than"),
    # The cap file: a 65536-sprite blob whose lengths are consistent so the u16
    # count line is what STANDS — but since P6.2 the budget gate is what SPEAKS,
    # at both readers, before the blob check is reached. The claim is now that
    # order itself: over-cap geometry hears the table-area sentence, not the
    # count sentence, on either side of the parity line.
    (5, 0, NO_ROW, [], _cap_file(),
     "same", None, "table area"),
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
    (23, 0, NO_ROW, [], build(specs=[SOLO], hdr_words={2: 5}),
     "same", None, "geometry v5"),
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
    # §23.5's append-only rule read from the device end: a blob baked at the
    # pre-D32 stride of 17 words — the sentinel row with its kind and src words
    # spliced out — must parse whole, and the two missing columns must land in
    # the reader as the zeros those columns were written after. The expectations
    # come from the writer's parse of the same bytes, so this pins C and py to
    # the SAME stride derivation, not just to width 19.
    (86, F_LAYOUT, 0, [],
     build(specs=[{"slug": "solo", "elements": [SENT],
                   "raw_blob": solo_blob[:120 + 17 * 4] + solo_blob[120 + EW * 4:],
                   "sprites": SPR, "art": ART}]),
     "dump", None, None),
    # And one width below the floor: the same row spliced to 16 words. That is
    # no schema this dialect ever had, and both readers must say so in the same
    # sentence rather than divide out a stride and walk the table sideways.
    (87, 0, NO_ROW, [],
     build(specs=[dict(SOLO, raw_blob=solo_blob[:120 + 16 * 4] +
                       solo_blob[120 + EW * 4:])]),
     "same", None, "narrowest"),
    # P6.2's budget gate on the header: word 5 (geom_size) and word 7 (art_size)
    # forged past the caps refuse with the writer's own sentences; == cap PASSES
    # the gate and dies one line later at the file bounds — the refusal word
    # proves which guard spoke. Both caps forged over dies at geometry first:
    # the pair's order is the device's, and a swap hears the wrong sentence.
    (60, 0, NO_ROW, [], build(specs=[SOLO], hdr_words={5: ob.GEOM_CAP + 1}),
     "same", None, "table area"),
    (61, 0, NO_ROW, [], build(specs=[SOLO], hdr_words={5: ob.GEOM_CAP}),
     "same", None, "runs past"),
    (62, 0, NO_ROW, [], build(specs=[SOLO], hdr_words={7: ob.SLOT_ART_CAP + 1}),
     "same", None, "larger than a slot"),
    (63, 0, NO_ROW, [], build(specs=[SOLO], hdr_words={7: ob.SLOT_ART_CAP}),
     "same", None, "art region runs past"),
    (64, 0, NO_ROW, [], build(specs=[SOLO],
                              hdr_words={5: ob.GEOM_CAP + 1,
                                         7: ob.SLOT_ART_CAP + 1}),
     "same", None, "table area"),
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
# The streaming family (P6.3): the region door, the wire, and the rebase.
#
# One claim, stated once: every check the whole-buffer reader makes BEFORE a
# bank's bytes says the same thing through orb_bundle_parse_region(), and the
# bank check itself moves onto the wire as orb_bundle_bank_rule(). Expectations
# are computed from the writer's own parse of the same bytes, so the C cannot
# define its own pass. Probes are the installer's calls in the installer's order;
# out of order, the door says so.
# ---------------------------------------------------------------------------

F_STREAM      = 4
SLOT_ART_PY   = 0x00010000          # orb_theme.c SLOT_ART_OFF
SLOT_CAP_PY   = 0x00400000          # orb_theme.c SLOT_CAP
IDX_HEAD_PY   = 8                   # orb_bundle.c IDX_HEAD_BYTES
IDX_BYTES_PY  = 40                  # orb_bundle.c IDX_BYTES (4H + 7I + 2H)
REC_ART_OFF_PY = 24                 # word 7: the byte the rebase edits

TWIN = build(name="Twin Banks",
             specs=[dict(SOLO, slug="clock"),
                    dict(SOLO, slug="paint",
                         art=bytes((i * 7 + 3) & 0xFF for i in range(3000)))],
             serial=9)
_AO = struct.unpack_from("<I", SOLO_RAW, 13 * 4)[0]
TRUNC = SOLO_RAW[:_AO + 900]        # the card gave fewer bank bytes than declared


def _pytable(raw):
    """Header, index, region — the writer's own reading, available even for a file
    whose BANK is torn (only its bytes are, and unpack_index never looks)."""
    try:
        h = ob.parse_header3(raw)
    except ValueError:
        return None, None, None
    region = raw[h["geom_off"]:h["geom_off"] + h["geom_size"]]
    try:
        _n, recs = ob.unpack_index(region)
    except ValueError:
        recs = None
    return h, recs, region


def _slugs(raw, recs, region):
    return [region[r["slug_off"]:r["slug_off"] + r["slug_len"]].decode()
            for r in recs]


def _moves(recs, base, new_base, cap):
    """orb_bundle_rebase()'s sums, computed here so the C's arithmetic is under
    test rather than the thing defining its own pass."""
    delta = new_base - base
    moved = []
    for r in recs:
        m = r["art_off"] + delta
        if delta < 0 and -delta > r["art_off"]:
            return None, "section bank offset would run below the region's start"
        if m > cap or r["art_size"] > cap - m:
            return None, "section bank would run past the end of the slot"
        moved.append(m)
    return moved, None


def _edited_region(region, recs, moved):
    """The bytes a rebased slot holds: the region with every record's art_off
    rewritten. This is what orb_bundle_rebase() leaves in the caller's buffer, and
    what copy_bundle3() writes to flash — the crc alone is not the claim."""
    ed = bytearray(region)
    for i, m in enumerate(moved):
        struct.pack_into("<I", ed, IDX_HEAD_PY + i * IDX_BYTES_PY + REC_ART_OFF_PY, m)
    return bytes(ed)


def _edited_crc(region, recs, moved):
    return zlib.crc32(_edited_region(region, recs, moved)) & 0xFFFFFFFF


def _bank_sent(slug):
    return ("section '%s' bank checksum mismatch — the copy on the card is "
            "damaged" % slug)



def _slot_image(raw, h, recs, region, cur):
    """Rebuild the SLOT the installer would have written and read it back through
    ob.read_bundle. A bundle the writer cannot read after an install is a slot no
    --inspect can answer with — §22.1's third rule, checked on bytes and not on
    the promise that the offsets were carried across faithfully."""
    new_base = cur[0]
    end = max([c + r["art_size"] for r, c in zip(recs, cur)] + [new_base])
    img = bytearray(end)
    hdr = bytearray(ob.pack_header3(h["name"], len(recs), len(region),
                                    _edited_crc(region, recs, cur), new_base,
                                    end - new_base, 0, h["serial"]))
    # The writer stamps the FILE layout, where a bundle's region sits at 128. The
    # slot puts it at SLOT_GEOM_OFF, and the installer rewrites the header for
    # exactly that reason (build_slot_hdr3); a slot image that claimed geom_off
    # 128 while holding its tables at 0x1000 would test nothing.
    words = list(struct.unpack("<%dI" % _OT.HDR_WORDS, bytes(hdr)))
    words[_OT.W_GEOM_OFF] = 0x1000
    words[_OT.W_HDR_CRC] = _OT._hdr_crc(words)
    hdr = bytearray(struct.pack("<%dI" % _OT.HDR_WORDS, *words))
    img[0:len(hdr)] = hdr
    img[0x1000:0x1000 + len(region)] = _edited_region(region, recs, cur)
    for r, c in zip(recs, cur):
        img[c:c + r["art_size"]] = raw[r["art_off"]:r["art_off"] + r["art_size"]]
    p = os.path.join(tmpdir, "slot.orbtheme")
    with open(p, "wb") as f:
        f.write(bytes(img))
    try:
        return ob.read_bundle(p, theme=False), None  # parity is wire-level; the door rule has its own vectors
    except ValueError as e:
        return None, str(e)

def stream_expect(raw, qs):
    """The lines the streaming door must print, probe by probe, or [] when the
    header itself refuses: the door sits behind it on both readers."""
    pyv = read_verdict(raw)
    h, recs, region = _pytable(raw)
    if h is None or recs is None:
        return [], None
    bank_ref = pyv if (isinstance(pyv, str) and "bank checksum" in pyv) else None
    refuse = pyv if (isinstance(pyv, str) and not bank_ref) else None
    names = _slugs(raw, recs, region)
    cur = [r["art_off"] for r in recs]
    fpos = list(cur)
    have, base, bound = False, h["art_off"], len(raw)
    out = []
    for q in qs:
        if q == "region":
            if refuse:
                out.append("R refuse " + refuse)   # what the whole door says
                have = False
                continue
            out.append("R ok %u %u %u %u" % (h["n_sections"], h["geom_size"],
                                             h["geom_crc"], h["art_off"]))
            have = True
        elif q.startswith("bank:"):
            if not have:
                out.append("B skip")               # no region parsed, nothing to settle
                continue
            si, md = int(q[5:].split(":")[0]), q[5:].split(":")[1]
            if si >= len(recs):
                out.append("B skip")
                continue
            r = recs[si]
            whole = zlib.crc32(raw[fpos[si]:fpos[si] + r["art_size"]]) & 0xFFFFFFFF
            want, crc = r["art_size"], whole
            if md in ("SHORT", "LYE", "LYEC") and r["art_size"]:
                want = r["art_size"] - 1
                crc = zlib.crc32(raw[fpos[si]:fpos[si] + want]) & 0xFFFFFFFF
            if md == "BAD":
                crc = whole ^ 0x80808080
            if md == "LYE":
                crc = r["art_crc"] ^ 0x80808080
            if md == "LYEC":
                crc = r["art_crc"]
            if want == r["art_size"] and crc == r["art_crc"]:
                out.append("B ok %u %s %u %u" % (si, names[si], want, crc))
            else:
                sent = bank_ref if (md == "FULL" and bank_ref) else _bank_sent(names[si])
                out.append("B refuse %u %s %u %u | %s"
                           % (si, names[si], want, crc, sent))
        elif q.startswith("rebase"):
            if not have:
                out.append("X skip")
                continue
            new_base, cap = SLOT_ART_PY, SLOT_CAP_PY
            if q.startswith("rebasecap:"):
                new_base, cap = (int(v) for v in q[10:].split(":"))
            elif q != "rebase":
                new_base = int(q[7:])
            moved, why = _moves(recs, base, new_base, cap)
            if why:
                out.append("X refuse " + why)
                continue
            out.append("X ok %u %u %u %u %s"
                       % (base, new_base, _edited_crc(region, recs, moved), len(recs),
                          " ".join(str(m) for m in moved)))
            cur, base, bound = moved, new_base, cap
        elif q == "secdump":
            for i in range(len(recs) if have else 0):
                out.append("S %u %s %u %u" % (i, names[i], cur[i], recs[i]["art_size"]))
        elif q == "recheck":
            if not have:
                out.append("C refuse rechecked with no region parsed")
                continue
            out.append("C ok %u" % h["n_sections"])
        else:
            out.append("probe unknown")
    return out, (cur, fpos, bound, names)

_h2, _r2, _g2 = _pytable(TWIN)
TWIN_FLIP = _flip_bank(TWIN, _r2[1]["art_off"] + 10)   # the SECOND bank torn

VECTORS += [
    # The door accepts what the whole-buffer reader accepts, and says its words.
    (70, F_STREAM, NO_ROW, ["region", "bank:0:FULL", "bank:1:FULL"], GOLDEN,
     "stream", None, None),
    (71, F_STREAM, NO_ROW, ["region", "bank:0:FULL"], SOLO_RAW,
     "stream", None, None),
    # Out of order: the region is parsed first. Nothing else may come before it.
    (72, F_STREAM, NO_ROW, ["bank:0:FULL", "rebase", "recheck"], GOLDEN,
     "stream", None, None),
    # The wire's refusals, each isolated by the one parameter that lies.
    (73, F_STREAM, NO_ROW, ["region", "bank:0:SHORT"], GOLDEN,
     "stream", None, None),
    (74, F_STREAM, NO_ROW, ["region", "bank:0:BAD"], GOLDEN,
     "stream", None, None),
    (75, F_STREAM, NO_ROW, ["region", "bank:0:LYE"], GOLDEN,
     "stream", None, None),
    # A count short of art_size is refused even when the crc is the record's own.
    (76, F_STREAM, NO_ROW, ["region", "bank:0:LYEC"], GOLDEN,
     "stream", None, None),
    # The pairing: a torn BANK no longer condemns the region. These very bytes
    # pass the region door, and the wire is what refuses, in the same words.
    (77, F_STREAM, NO_ROW, ["region", "bank:0:FULL"], BANK_CRC_FLIP,
     "same", None, "bank checksum"),
    # And the section order is the settle order: a clean bank closes before the
    # torn one speaks. No half-installed bundle is a claim this pair makes.
    (78, F_STREAM, NO_ROW, ["region", "bank:0:FULL", "bank:1:FULL"], TWIN_FLIP,
     "same", None, "bank checksum"),
    # A check that needs no bank bytes still bites, same order, same sentence.
    (79, F_STREAM, NO_ROW, ["region"], GEOM_CRC_FLIP,
     "same", None, "geometry checksum"),
    (80, F_STREAM, NO_ROW, ["region"],
     build(specs=[dict(SOLO, rec={"art_off": solo_aoff + SECTOR})]),
     "same", None, "art walk"),
    (81, F_STREAM, NO_ROW, ["region"], TRUNC,
     "same", None, "art region runs past"),
    # The slot move, the device's own call, and the records it leaves behind.
    (82, F_STREAM, NO_ROW, ["region", "rebase", "secdump", "recheck",
                            "bank:0:FULL", "bank:1:FULL"], GOLDEN,
     "stream", None, None),
    # Two real banks, so the cap pair is about bytes and not about padding.
    (83, F_STREAM, NO_ROW, ["region", "rebasecap:65536:72632", "secdump",
                            "recheck", "bank:0:FULL", "bank:1:FULL"], TWIN,
     "stream", None, None),
    (84, F_STREAM, NO_ROW, ["region", "rebasecap:65536:72631", "secdump"], TWIN,
     "stream", None, None),
    # A refused rebase moves nothing: the dump still reads as the file did.
    (85, F_STREAM, NO_ROW, ["region", "rebasecap:65536:69631", "secdump"], GOLDEN,
     "stream", None, None),
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


def check_stream(cid, raw, qs, cl):
    """One streaming vector: the door's lines, in probe order, against the
    writer's numbers — and, after a move, the slot rebuilt from those numbers read
    back through the host reader."""
    sl = [l for l in cl if l[:2] in ("R ", "B ", "X ", "S ", "C ")
          or l.startswith("probe ") or l == "B skip" or l == "X skip"]
    exp, state = stream_expect(raw, qs)
    check(len(sl) == len(exp), "case %d: stream lines %r, expected %r"
          % (cid, sl, exp))
    for got, want in zip(sl, exp):
        check(got == want, "case %d: stream drift\n  C:  %s\n  py: %s"
              % (cid, got, want))
    if not state or not any(e.startswith("C ok") for e in exp):
        return
    cur, _fpos, _bound, _names = state
    h, recs, region = _pytable(raw)
    got, why = _slot_image(raw, h, recs, region, cur)
    check(got is not None, "case %d: the slot the rebase describes refuses the host "
          "reader: %s" % (cid, why))
    if got:
        hh, secs = got
        check([s["rec"]["art_off"] for s in secs] == cur,
              "case %d: the installed slot's banks are not where the records say: %r"
              % (cid, secs and [s["rec"]["art_off"] for s in secs]))
        check(hh["art_off"] == cur[0] and hh["n_sections"] == len(cur),
              "case %d: the installed slot's header disagrees: %r" % (cid, hh))


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
                     ("degrees", e0["degrees"]), ("scale16", e0["scale16"]),
                     ("kind", e0["kind"]), ("src", e0["src"]))}
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
     "if (ne == 0 ? eb != 0 : eb % ((uint64_t)ne * 4u) != 0)",
     "if (0 && (ne == 0 ? eb != 0 : eb % ((uint64_t)ne * 4u) != 0))", 3),
    ("allow a stride below the dialect floor",
     "if (st < ORB_BUNDLE_ELEM_WORDS_MIN || st > 0xFFFFu)",
     "if (0 && (st < ORB_BUNDLE_ELEM_WORDS_MIN || st > 0xFFFFu))", 87),
    ("ignore the scalar dialect word",
     "if (nsc != ORB_BUNDLE_SCALARS_N)", "if (0 && nsc != ORB_BUNDLE_SCALARS_N)", 2),
    # ("drop the u16 count cap" RETIRED at P6.2: the budget gate speaks first on
    # every file whose blob could outgrow a u16 — case 5 now pins that order —
    # so breaking the line below shows no reachable difference. The check stays
    # as the defense-in-depth backstop its own comment names.)
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
     "have) != s->art_crc) {", "have) == s->art_crc) {", 41),
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
    ("drop the geometry budget refusal",
     "if (o->geom_size > ORB_BUNDLE_GEOM_CAP)",
     "if (0 && o->geom_size > ORB_BUNDLE_GEOM_CAP)", 60),
    ("drop the art budget refusal",
     "if (o->art_size > ORB_BUNDLE_ART_CAP)",
     "if (0 && o->art_size > ORB_BUNDLE_ART_CAP)", 62),
    ("refuse the geometry budget at == cap",
     "o->geom_size > ORB_BUNDLE_GEOM_CAP)",
     "o->geom_size >= ORB_BUNDLE_GEOM_CAP)", 61),
    ("refuse the art budget at == cap",
     "o->art_size > ORB_BUNDLE_ART_CAP)",
     "o->art_size >= ORB_BUNDLE_ART_CAP)", 63),
    # P6.3's streaming door: the region alone, the wire, and the slot move.
    ("stream the region at its file offset instead of at 0",
     "local.geom_off  = 0;", "local.geom_off = h->geom_off;", 70),
    ("hash bank bytes the streaming caller never sent",
     "if (banks_present) {", "if (banks_present || !banks_present) {", 70),
    ("let a short count pass on the record's own crc",
     "if (got_bytes != s->art_size || got_crc != s->art_crc) {",
     "if (got_crc != s->art_crc + (got_bytes - got_bytes)) {", 76),
    ("settle nothing on the wire",
     "if (got_bytes != s->art_size || got_crc != s->art_crc) {",
     "if (0 && (got_bytes != s->art_size || got_crc != s->art_crc)) {", 73),
    ("rebase without a slot bound",
     "if (moved > slot_cap || s->art_size > slot_cap - moved)",
     "if (0 && (moved > slot_cap || s->art_size > slot_cap - moved))", 84),
    ("rebase and leave the region checksum stale",
     "h->geom_crc = esp_rom_crc32_le(0, region, h->geom_size);",
     "h->geom_crc = h->geom_crc ^ 1u;", 82),
    ("edit the wrong record when rebasing",
     "uint8_t *rec = region + IDX_HEAD_BYTES + i * IDX_BYTES;",
     "uint8_t *rec = region + IDX_HEAD_BYTES +"
     " (i + 1u) % b->n_sections * IDX_BYTES;", 82),
    ("move a bank before its bounds are known",
     """    for (uint32_t i = 0; i < b->n_sections; i++) {
        const orb_bundle_sec_t *s = &b->sec[i];
        const uint32_t moved = (uint32_t)((int64_t)s->art_off + (int64_t)delta);
        if (delta < 0 && (uint32_t)-delta > s->art_off)
            return "section bank offset would run below the region's start";
        if (moved > slot_cap || s->art_size > slot_cap - moved)
            return "section bank would run past the end of the slot";
    }""",
     """    for (uint32_t i = 0; i < b->n_sections; i++) {
        orb_bundle_sec_t *s = &b->sec[i];
        const uint32_t moved = (uint32_t)((int64_t)s->art_off + (int64_t)delta);
        s->art_off = moved;
        memcpy(region + IDX_HEAD_BYTES + i * IDX_BYTES + 24, &moved, 4);
        if (moved > slot_cap || s->art_size > slot_cap - moved)
            return "section bank would run past the end of the slot";
    }""", 85),
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
        if vec[1] & F_STREAM:
            check_stream(vec[0], vec[4], vec[3], got.get(vec[0], []))
    print("== orb_bundle_c parity: %d checks, %d failure(s)"
          % (nchecks[0], len(failures)))
    for m in failures:
        print("  FAIL " + m)
    code = 1 if failures else 0
    if "--mutations" in sys.argv:
        if mutants(binp, cases, stdout, tmpdir):
            code = 1
    sys.exit(code)




