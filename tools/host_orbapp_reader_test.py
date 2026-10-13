#!/usr/bin/env python3
"""tools/host_orbapp_reader_test.py — the third leg of the .orbapp mirror.

theme_schema.py's rule names the three: the Python writer (tools/orbapp.py),
the C reader (main/app/orb_app_manifest.c), and THIS sentinel. It compiles the
SHIPPING reader with host gcc -Werror against the shim headers below and
feeds it the writer's own bytes, so the device's refusals and the pack tool's
refusals are pinned to each other without a board, a card, or a partition in
the loop. A container accepted by one side and refused by the other — or
refused in different words — fails here, in seconds, not on the dial.

Where the reader is deliberately STRICTER than the writer's reader (never
looser; each is its own labeled vector below):

  * The stamped abi must equal this BUILD's dialect, not merely sit inside
    the header's own declared range (the writer's check_header compares the
    field against itself — sound only because pack_header can never emit a
    different number; a hand-edited card can). The message also truthfully
    prints the build's own ABI; the writer's prints the header's field under
    a label that is only accidentally right. Vectors label that WORDING-DIFF.
  * slug/title/settings-id/def_str must carry their NUL INSIDE the field;
    the writer's _getstr would silently truncate. The writer zero-pads, so
    only a crafted card trips it, and those bytes are inside the CRC — the
    "damaged" sentence is the honest one.
  * A settings blob short of its count is refused here AND by the writer's
    unpack (same rule, mirrored); an unterminated id is only refused here.

Run:  python3 tools/host_orbapp_reader_test.py              (rc 0 = pass)
      python3 tools/host_orbapp_reader_test.py --mutations  (each must fail)
"""
import hashlib
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
READER_C = os.path.join(ROOT, "main", "app", "orb_app_manifest.c")
READER_H = os.path.join(ROOT, "main", "app", "orb_app_manifest.h")

_spec = importlib.util.spec_from_file_location("orbapp", os.path.join(HERE, "orbapp.py"))
orbapp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(orbapp)

NL = "\n"
SHIM_ESP_ERR = """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
"""
SHIM_CRC = """#pragma once
#include <stdint.h>
#include <stddef.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
"""
SHIM_SHA = """#pragma once
#include <stddef.h>
int mbedtls_sha256(const unsigned char *in, size_t ilen, unsigned char out[32], int is224);
"""

# ---- the harness: runs the shipping reader over a binary case file ----------
# Case file layout (little endian): u32 n_cases, then per case:
#   u32 flags (1=check 2=layout 4=unpack 8=have_expect), u32 expect_app_id,
#   u32[ORBAPP_HDR_WORDS] header words, u32 stem_len + stem bytes,
#   u8[32] sha256 digest the hook must hand back,
#   u32 sblob_words + words, u32 n_unpack.
# stdout lines: "caseN.STAGE OK" | "caseN.STAGE|reason"; "caseN.hash CALLS
# IS224 hexin"; "caseN.set IDX TYPE IDHEX MIN MAX DEF DEFSTRHEX"; the
# caps battery prints "caps HEX RET NAMESHEX" for fixed inputs at the end.

HARNESS_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "orb_app_manifest.h"

static unsigned char g_digest[32];
static const unsigned char *g_hash_in;
static size_t g_hash_len;
static int g_hash_is224, g_hash_calls;
int mbedtls_sha256(const unsigned char *in, size_t ilen, unsigned char out[32], int is224)
{
    g_hash_in = in; g_hash_len = ilen; g_hash_is224 = is224; g_hash_calls++;
    memcpy(out, g_digest, 32);
    return 0;
}

static FILE *F;
static uint32_t rd32(void) { uint32_t v; if (fread(&v, 4, 1, F) != 1) exit(2); return v; }
static void rdb(void *d, size_t n) { if (fread(d, 1, n, F) != n) exit(2); }
static void stage(const char *tag, const char *e, const char *reason)
{
    if (!e) printf("%s OK\n", tag);
    else    printf("%s REFUSE %s\n", tag, reason);
}
static void puthex(const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    F = fopen(argv[1], "rb");
    if (!F) return 2;
    const uint32_t n = rd32();
    char reason[768], stem[80], tag[40];
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t flags = rd32(), expect = rd32();
        uint32_t w[ORBAPP_HDR_WORDS];
        rdb(w, sizeof w);
        const uint32_t slen = rd32();
        rdb(stem, slen); stem[slen] = 0;
        rdb(g_digest, 32);
        const uint32_t sw = rd32();
        uint32_t sbuf[16 * 16];
        if (sw > 16 * 16) return 3;
        rdb(sbuf, sw * 4);
        const uint32_t n_unpack = rd32();

        orbapp_hdr_t h;
        memset(&h, 0, sizeof h);
        g_hash_calls = 0;
        const char *why = NULL;
        snprintf(tag, sizeof tag, "case%u.parse", i);
        const char *e = orbapp_parse_header(w, &h, reason, sizeof reason);
        stage(tag, e, reason);
        why = e;
        if (!why && (flags & 1)) {
            snprintf(tag, sizeof tag, "case%u.check", i);
            why = orbapp_check_header(&h, expect, (flags & 8) != 0,
                                      slen ? stem : NULL, reason, sizeof reason);
            stage(tag, why, reason);
        }
        if (!why && (flags & 2)) {
            snprintf(tag, sizeof tag, "case%u.layout", i);
            why = orbapp_check_file_layout(&h, reason, sizeof reason);
            stage(tag, why, reason);
        }
        if (!why && (flags & 4)) {
            orb_setting_desc_t out[16];
            snprintf(tag, sizeof tag, "case%u.unpack", i);
            why = orbapp_unpack_settings(sbuf, sw, n_unpack, out, reason, sizeof reason);
            stage(tag, why, reason);
            if (!why)
                for (uint32_t k = 0; k < n_unpack; k++) {
                    printf("case%u.set %u %d ", i, k, (int)out[k].type);
                    puthex((const unsigned char *)out[k].id, strnlen(out[k].id, 16));
                    printf(" %d %d %d ", out[k].min, out[k].max, out[k].def);
                    puthex((const unsigned char *)out[k].def_str, strnlen(out[k].def_str, 32));
                    printf("\n");
                }
        }
        printf("case%u.hash %d %d ", i, g_hash_calls, g_hash_is224);
        if (g_hash_calls) { puthex(g_hash_in, g_hash_len); }
        printf("\n");
    }
    fclose(F);
    const uint32_t probe[] = { 0x00u, 0x10u, 0x20u, 0xB0u, 0xF0u, 0xFFu };
    char names[128];
    for (unsigned p = 0; p < sizeof probe / sizeof probe[0]; p++) {
        const int r = orbapp_caps_names(probe[p], names, sizeof names);
        printf("caps %X %d ", probe[p], r);
        if (r >= 0) puthex((const unsigned char *)names, (size_t)r);
        printf("\n");
    }
    const int tr = orbapp_caps_names(0xF0u, names, 5);   /* too small: -1, never */
    printf("caps TRUNC %d\n", tr);                        /* a truncated line     */
    return 0;
}
"""

# ---- the writer's own golden container, patched per vector ------------------
SETTINGS_GOLDEN = [
    {"id": "unit", "type": "enum", "min": 0, "max": 2, "default": 1},
    {"id": "city", "type": "string", "def_str": "Austin TX"},
    {"id": "dark", "type": "toggle", "default": 1},
    {"id": "neg", "type": "int", "min": -10, "max": 20, "default": -5},
]
GOLDEN_SLUG, GOLDEN_TITLE = "weather", "Weather"


def build_hdr(slug=GOLDEN_SLUG, title=GOLDEN_TITLE, caps=0x10, period=3000,
              abi=None, counts=(1, 1, 1, 1), settings=SETTINGS_GOLDEN,
              art_size=0, geom_size=16, sig=0, app_id=None):
    """The writer's own pack_header at its legal best, then the payload-size
    words the caller fills, then a RE-computed hdr_crc — so every vector that
    reaches the semantic stage really did pass the checksum stage too. D21
    (§0.9) made the golden art-free: a non-zero art_size kw cannot come from
    pack_header any more (the writer sheds the field), so the vector FORGES it
    into the packed words, header CRC and all — exactly the pre-D21 file shape
    a stranger's card could still carry, which is precisely what the gate must
    refuse. The forged claim also drags art_req with it, matching the old
    tie art_req=art_size that pre-D21 files carried."""
    sblob = orbapp.pack_settings(orbapp.check_settings(list(settings)))
    hdr = orbapp.pack_header(app_id if app_id is not None else orbapp.app_id_of(slug),
                             slug, title, caps, period, art_size, counts,
                             sblob, abi=abi, sig=sig)
    hdr[orbapp.W_GEOM_SIZE] = geom_size
    if art_size:
        hdr[orbapp.W_ART_SIZE] = art_size
        hdr[orbapp.W_ART_REQ] = art_size
    hdr[orbapp.W_HDR_CRC] = orbapp._hdr_crc(hdr)
    return hdr, sblob


def patch(hdr, word, value):
    """Mutate one word and keep the checksum honest: the vector then probes
    exactly the check that owns that field, not the CRC."""
    hdr[word] = value
    hdr[orbapp.W_HDR_CRC] = orbapp._hdr_crc(hdr)
    return hdr


def pack_words(hdr):
    return b"".join(struct.pack("<I", v) for v in hdr)


def python_outcome(hdr, app_id=None, stem="__unset__"):
    """What the WRITER's reader says about the same header — the mirror's
    other voice. Returns (ok, message). parse_header takes raw bytes; the
    container here is exactly one header block."""
    try:
        h = orbapp.parse_header(pack_words(hdr), app_id=app_id)
        kw = {}
        if stem != "__unset__":
            kw["stem"] = stem
        if app_id is not None:
            kw["app_id"] = app_id
        orbapp.check_header(h, **kw)
        return True, None
    except ValueError as e:
        return False, str(e)


class Case:
    """One vector: the header words, the harness flags, and the per-stage
    expectations. None = the stage must print OK; a string = the exact bytes
    of the reason, compared with the sentinel's full fury."""
    def __init__(self, name, hdr, sblob=b"", flags=0, expect_id=0, stem=None,
                 n_unpack=0, parse=None, check=None, layout=None, unpack=None,
                 sets=None, hash_calls=1, hash_in=None, py=("ok", None),
                 note=""):
        self.name, self.hdr, self.sblob = name, hdr, sblob
        self.flags, self.expect_id, self.stem = flags, expect_id, stem
        self.n_unpack = n_unpack
        self.expect = {"parse": parse, "check": check, "layout": layout,
                       "unpack": unpack}
        self.sets, self.hash_calls, self.hash_in = sets, hash_calls, hash_in
        self.py, self.note = py, note



F_CHECK, F_LAYOUT, F_UNPACK, F_EXPECT = 1, 2, 4, 8


def _golden_settings16():
    return [{"id": "s%02d" % i, "type": "int", "min": 0, "max": 5, "default": 2}
            for i in range(16)]


def vectors_accept(add):
    g, sblob = build_hdr()
    want_sets = orbapp.unpack_settings(sblob, 4)
    add(Case("golden_full", g, sblob, flags=F_CHECK | F_LAYOUT | F_UNPACK,
             n_unpack=4, sets=want_sets))
    s16 = _golden_settings16()
    g16, b16 = build_hdr(settings=s16)
    add(Case("golden_16_settings", g16, b16, flags=F_CHECK | F_LAYOUT,
             note="the > vs >= cliff at SETTINGS_MAX; catches mutation 3"))
    g0, _ = build_hdr(period=0)
    add(Case("period_zero_vector_only_app", g0, flags=F_CHECK | F_LAYOUT |
             F_UNPACK, n_unpack=0,
             note="0 is the D16 word for never ticks — accepted whole"))
    gf, _ = build_hdr(caps=0xF0)
    add(Case("caps_all_four_owner_legal", gf, flags=F_CHECK | F_LAYOUT |
             F_UNPACK, n_unpack=0,
             note="D19 positive control: the max legal declaration passes"))
    s23 = "w" * 23
    g23, _ = build_hdr(slug=s23)
    add(Case("slug_23_chars_field_edge", g23, flags=F_CHECK | F_LAYOUT |
             F_UNPACK, n_unpack=0, hash_in=s23,
             note="the off-by-one that would refuse the writer's own longest slug"))
    gu, _ = build_hdr()
    add(Case("stem_upper_ext_agrees", gu, flags=F_CHECK | F_LAYOUT | F_UNPACK,
             n_unpack=0, stem="weather.ORBAPP",
             note="extension case folds; the stem itself never does"))
    gp = patch(build_hdr()[0][:], orbapp.W_PERIOD_MS, 100000000)
    add(Case("period_above_a_day_reader_accepts", gp, flags=F_CHECK |
             F_LAYOUT | F_UNPACK, n_unpack=0, py=("skip", None),
             note="D16's ceiling is the writer's alone; the scheduler's clamp "
                  "answers a crafted over-max card — a documented NOT-mirror"))


def vectors_refuse(add):
    m = build_hdr()[0][:]
    m[orbapp.W_MAGIC] = 0xDEADBEEF
    m[orbapp.W_HDR_CRC] = orbapp._hdr_crc(m)
    ok, msg = python_outcome(m)
    add(Case("magic_generic", m, parse=msg, hash_calls=0, py=("refuse", msg)))
    mt = build_hdr()[0][:]
    mt[orbapp.W_MAGIC] = 0x474F5254
    mt[orbapp.W_HDR_CRC] = orbapp._hdr_crc(mt)
    ok, msg = python_outcome(mt)
    add(Case("magic_theme_is_named", mt, parse=msg, hash_calls=0,
             py=("refuse", msg)))
    f2 = patch(build_hdr()[0][:], orbapp.W_FORMAT, 2)
    ok, msg = python_outcome(f2)
    add(Case("format_future", f2, parse=msg, hash_calls=0, py=("refuse", msg)))
    hs = patch(build_hdr()[0][:], orbapp.W_HDR_SIZE, 200)
    ok, msg = python_outcome(hs)
    add(Case("hdr_size_lie", hs, parse=msg, hash_calls=0, py=("refuse", msg)))
    torn = build_hdr()[0][:]
    torn[orbapp.W_SERIAL] += 1                        # NO re-checksum: crc dies
    ok, msg = python_outcome(torn)
    add(Case("hdr_crc_torn", torn, parse=msg, hash_calls=0, py=("refuse", msg)))
    sg = patch(build_hdr()[0][:], orbapp.W_SIG, 1)
    ok, msg = python_outcome(sg)
    add(Case("sig_nonzero_refused", sg, parse=msg, hash_calls=0,
             py=("refuse", msg)))
    # -- the identity triple ---------------------------------------------------
    es = build_hdr()[0][:]
    for i in range(orbapp.W_SLUG, orbapp.W_SLUG + 6):
        es[i] = 0
    es[orbapp.W_HDR_CRC] = orbapp._hdr_crc(es)
    ok, msg = python_outcome(es)
    add(Case("slug_empty", es, flags=F_CHECK, check=msg, hash_calls=0,
             py=("refuse", msg)))
    bs, _ = build_hdr(slug="Bad-Slug", app_id=0x12345678)
    ok, msg = python_outcome(bs)
    add(Case("slug_bad_charset", bs, flags=F_CHECK, check=msg, hash_calls=0,
             py=("refuse", msg)))
    wa, _ = build_hdr(app_id=0x12345678)
    ok, msg = python_outcome(wa)
    add(Case("app_id_disagrees_with_slug", wa, flags=F_CHECK, check=msg,
             hash_calls=1, py=("refuse", msg)))
    ex = build_hdr()[0]
    ok, msg = python_outcome(ex, app_id=0xDEADBEEF)
    add(Case("app_id_not_what_caller_expected", ex,
             flags=F_CHECK | F_EXPECT, expect_id=0xDEADBEEF, check=msg,
             hash_calls=1, py=("refuse", msg)))
    Dmg = "container header checksum mismatch — re-copy the file onto the card"
    tt = build_hdr()[0][:]
    for i in range(orbapp.W_TITLE, orbapp.W_TITLE + 8):
        tt[i] = 0x74747474                            # 'tttt' words, no NUL
    tt[orbapp.W_HDR_CRC] = orbapp._hdr_crc(tt)
    add(Case("title_unterminated_STRICTER", tt, parse=Dmg, hash_calls=0,
             py=("ok", None),
             note="the writer's _getstr would read 32 chars silently; a label "
                  "byte the CRC cannot vouch for as terminated is damage"))
    sl = build_hdr()[0][:]
    for i in range(orbapp.W_SLUG, orbapp.W_SLUG + 6):
        sl[i] = 0x61616161                            # 'a' x 24, no NUL
    sl[orbapp.W_HDR_CRC] = orbapp._hdr_crc(sl)
    add(Case("slug_unterminated_earlier_word", sl, parse=Dmg, hash_calls=0,
             py=("refuse", None),
             note="both refuse; the writer's reader at check_slug, the device "
                  "at the field — same verdict, one stage earlier"))
    # -- the filename's side -----------------------------------------------------
    g, _ = build_hdr()
    for nm, stem in (("stem_agrees", "weather.orbapp"),
                     ("stem_is_theme", "weather.orbtheme"),
                     ("stem_is_part", "weather.orbapp.part"),
                     ("stem_wrong_ext", "weather.bin"),
                     ("stem_disagrees", "rain.orbapp"),
                     ("stem_bad_charset", "Bad-Slug.orbapp")):
        ok, msg = python_outcome(g, stem=stem)
        refuse = nm != "stem_agrees"
        add(Case(nm, g, flags=F_CHECK, stem=stem,
                 check=(msg if refuse else None),
                 hash_calls=1, py=("refuse", msg) if refuse else ("ok", None)))
    # -- the D19 consent word map -------------------------------------------------
    for nm, bits in (("caps_free_bit_time", 0x01), ("caps_free_bit_ui", 0x04),
                     ("caps_bit_at_cap_n", 0x100), ("caps_bit_above", 0x8000)):
        cp = patch(build_hdr()[0][:], orbapp.W_CAPS, bits)
        ok, msg = python_outcome(cp)
        add(Case(nm, cp, flags=F_CHECK, check=msg, hash_calls=0,
                 py=("refuse", msg)))
    # -- the size and count bounds (D21: art words are a gate, not a price) -----
    asz = patch(build_hdr()[0][:], orbapp.W_ART_SIZE, orbapp.ART_BYTES_MAX + 1)
    ok, msg = python_outcome(asz)
    add(Case("art_payload_claimed", asz, flags=F_CHECK, check=msg, py=("refuse", msg)))
    areq = patch(build_hdr()[0][:], orbapp.W_ART_REQ, orbapp.ART_BYTES_MAX + 4)
    ok, msg = python_outcome(areq)
    add(Case("art_req_past_bound", areq, flags=F_CHECK, check=msg, py=("refuse", msg)))
    amax = patch(build_hdr()[0][:], orbapp.W_ART_REQ, orbapp.ART_BYTES_MAX)
    ok, msg = python_outcome(amax)
    add(Case("art_req_at_bound_declared", amax, flags=F_CHECK, check=None,
             py=("ok", None),
             note="the bound is inclusive: the old slot size alone, declared and "
                  "unbacked, is D21's expected shape"))
    gos = patch(build_hdr()[0][:], orbapp.W_GEOM_SIZE, orbapp.GEOM_BYTES_MAX + 4)
    ok, msg = python_outcome(gos)
    add(Case("geom_past_cap", gos, flags=F_CHECK, check=msg, py=("refuse", msg)))
    gog = patch(build_hdr()[0][:], orbapp.W_GEOM_SIZE, 18)
    ok, msg = python_outcome(gog)
    add(Case("geom_not_whole_words", gog, flags=F_CHECK, check=msg,
             py=("refuse", msg)))
    gc = patch(build_hdr()[0][:], orbapp.W_GEOM_SIZE, 12)     # counts want 16
    ok, msg = python_outcome(gc)
    add(Case("geom_shorter_than_counts", gc, flags=F_CHECK, check=msg,
             py=("refuse", msg)))
    z = patch(build_hdr()[0][:], orbapp.W_ART_REQ, 400)
    ok, msg = python_outcome(z)
    add(Case("art_req_without_payload", z, flags=F_CHECK, check=None,
             py=("ok", None),
             note="D21 INVERTED this refusal: an unbacked art_req is now the "
                  "declaration D18 always meant — requests, never bytes"))
    crc = patch(build_hdr()[0][:], orbapp.W_ART_CRC, 0xDEAD)
    ok, msg = python_outcome(crc)
    add(Case("art_crc_without_payload", crc, flags=F_CHECK, check=msg,
             py=("refuse", msg),
             note="the empty payload's own crc is 0; any other value is a stale "
                  "pre-D21 bank claim wearing the right word offsets"))
    claim = build_hdr(art_size=100)[0][:]
    ok, msg = python_outcome(claim)
    add(Case("art_payload_small_claim", claim, flags=F_CHECK, check=msg,
             py=("refuse", msg),
             note="a forged 100 B bank (req tied to it, old-file shape): refused "
                  "as art-carrying, not priced — no size fits post-D21"))
    per = patch(build_hdr()[0][:], orbapp.W_PERIOD_MS, 2999)
    ok, msg = python_outcome(per)
    add(Case("beat_under_the_floor", per, flags=F_CHECK, check=msg,
             py=("refuse", msg)))
    n17 = patch(build_hdr()[0][:], orbapp.W_N_SETTINGS, 17)
    ok, msg = python_outcome(n17)
    add(Case("n_settings_over_the_table", n17, flags=F_CHECK, check=msg,
             py=("refuse", msg)))
    # -- the file layout (the scan's rule) --------------------------------------------
    for nm, word, bad in (("set_off_moved", orbapp.W_SET_OFF, 200),
                          ("geom_off_moved", orbapp.W_GEOM_OFF, 999),
                          ("art_off_unaligned", orbapp.W_ART_OFF, 449)):
        lay = patch(build_hdr()[0][:], word, bad)
        ok, msg = python_outcome(lay)
        assert ok, "%s must survive the semantic stage to reach layout" % nm
        h = orbapp.parse_header(pack_words(lay))
        try:
            orbapp.check_file_layout(h)
            raise AssertionError(nm + ": writer accepted the mislayout")
        except ValueError as e:
            msg = str(e)
        add(Case(nm, lay, flags=F_CHECK | F_LAYOUT, layout=msg, py=("skip", None)))
    # -- the settings table on its own --------------------------------------------------
    g, sblob = build_hdr()
    add(Case("unpack_count_over_table", g, sblob,
             flags=F_CHECK | F_LAYOUT | F_UNPACK, n_unpack=17,
             unpack="17 settings; the loader's table holds 16",
             py=("skip", None),
             note="slot-reader path: the header says 4, the caller lies 17"))
    add(Case("unpack_blob_truncated", g, sblob[:3 * 64],
             flags=F_CHECK | F_LAYOUT | F_UNPACK, n_unpack=4,
             unpack="settings blob is 192 B, short of the 256 B its count claims",
             py=("skip", None)))
    bad_blob = bytearray(sblob)
    bad_blob[16] = 4                                  # descriptor 0's type word
    add(Case("unpack_type_out_of_c9", g, bytes(bad_blob),
             flags=F_CHECK | F_LAYOUT | F_UNPACK, n_unpack=4,
             unpack="setting 0 has type word 4, outside C9's frozen 4",
             py=("skip", None)))
    nid = bytearray(sblob)
    for k in range(16):                               # id field, no NUL
        nid[k] = 0x78
    add(Case("unpack_id_unterminated_STRICTER", g, bytes(nid),
             flags=F_CHECK | F_LAYOUT | F_UNPACK, n_unpack=4,
             unpack="container header checksum mismatch — re-copy the file "
                    "onto the card", py=("skip", None),
             note="the writer's unpack hands back a 16-char id; the device "
                  "refuses an id the NVS key ring could never hold"))
    # -- the ABI range (C4) ----------------------------------------------------
    a2 = patch(build_hdr()[0][:], orbapp.W_GEOM_ABI, 2)
    ok, msg = python_outcome(a2)
    add(Case("abi_stamp_out_of_own_range", a2, flags=F_CHECK,
             check="app needs geometry ABI 1..1; this build speaks 1 — "
                   "re-package it (C4)", hash_calls=0, py=("refuse", msg),
             note="WORDING-DIFF: the writer prints the header's own field "
                  "under its build label; the reader prints the build — both "
                  "refuse, the numbers on the page come from the reader"))
    r23 = patch(patch(patch(build_hdr()[0][:], orbapp.W_GEOM_ABI, 2),
                      orbapp.W_ABI_MIN, 2), orbapp.W_ABI_MAX, 3)
    add(Case("abi_range_excludes_this_build_STRICTER", r23, flags=F_CHECK,
             check="app needs geometry ABI 2..3; this build speaks 1 — "
                   "re-package it (C4)", hash_calls=0, py=("ok", None),
             note="the writer's reader waves this through (its own stamped "
                  "abi sits in its own range); the device must not install "
                  "geometry it reads wrong"))
    r00 = patch(patch(build_hdr()[0][:], orbapp.W_ABI_MIN, 0),
                orbapp.W_ABI_MAX, 0)
    ok, msg = python_outcome(r00)
    add(Case("abi_range_excludes_everything", r00, flags=F_CHECK, check=msg,
             hash_calls=0, py=("refuse", msg)))






# ---- run: compile the SHIPPING reader, drive it, compare --------------------
def compile_harness(tmp, reader_c_text, reader_h_text):
    shim = os.path.join(tmp, "shim")
    os.makedirs(os.path.join(shim, "mbedtls"), exist_ok=True)
    open(os.path.join(shim, "esp_err.h"), "w").write(SHIM_ESP_ERR)
    open(os.path.join(shim, "esp_rom_crc.h"), "w").write(SHIM_CRC)
    open(os.path.join(shim, "mbedtls", "sha256.h"), "w").write(SHIM_SHA)
    src = os.path.join(tmp, "orb_app_manifest.c")
    hdr = os.path.join(tmp, "orb_app_manifest.h")
    hac = os.path.join(tmp, "harness.c")
    open(src, "w").write(reader_c_text)
    open(hdr, "w").write(reader_h_text)
    open(hac, "w").write(HARNESS_C)
    binp = os.path.join(tmp, "harness")
    cc = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra",
                         "-Wno-unused-function", "-Werror",
                         "-I", tmp, "-I", os.path.join(ROOT, "main"),
                         "-I", shim, "-o", binp, src, hac],
                        capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("harness build failed:" + NL + cc.stderr[-3000:])
    return binp


def hdr_bytes_for(c):
    """One case, in the byte order the harness reads: flags+expect,
    the header words, the stem as raw bytes, the digest the sha hook
    hands back, the settings words, and the unpack count."""
    w = c.hdr
    stem = (c.stem or "").encode("utf-8")
    blob = bytes(c.sblob)
    out = [struct.pack("<II", c.flags, c.expect_id),
           b"".join(struct.pack("<I", v) for v in w),
           struct.pack("<I", len(stem)), stem,
           hashlib.sha256((c.hash_in if c.hash_in is not None else
                           GOLDEN_SLUG).encode("utf-8")).digest(),
           struct.pack("<I", len(blob) // 4), blob,
           struct.pack("<I", c.n_unpack)]
    if len(stem) % 4:
        pass                                   # no pad: the harness reads
    return b"".join(out)                       # exactly slen bytes, next field


def run_harness(binp, cases):
    blob = [struct.pack("<I", len(cases))]
    for c in cases:
        blob.append(hdr_bytes_for(c))
    path = binp + ".cases"
    open(path, "wb").write(b"".join(blob))
    r = subprocess.run([binp, path], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("harness exited %d" % r.returncode)
    out, sets_out = {}, {}
    for ln in r.stdout.splitlines():
        parts = ln.split(" ", 2)
        if parts[0].startswith("case"):
            if parts[0].endswith(".set"):
                sets_out.setdefault(parts[0][:-4], []).append(ln.split(" ", 1)[1])
                continue
            if parts[0].endswith(".hash"):
                out[parts[0]] = ln.split(" ", 1)[1]
                continue
            out[parts[0]] = None if parts[1] == "REFUSE" else "OK"
            if parts[1] == "REFUSE":
                out[parts[0]] = parts[2] if len(parts) > 2 else ""
        else:
            out[ln] = ln                       # caps battery lines, verbatim
    return out, sets_out


def compare(cases, out, sets_out):
    fails = []
    for i, c in enumerate(cases):
        for stg, bit in (("parse", 1), ("check", 1), ("layout", 2), ("unpack", 4)):
            ran = stg == "parse" or bool(c.flags & bit)
            if not ran:
                if c.expect[stg] is not None:
                    fails.append("%s.%s: reason expected but the stage never ran"
                                 % (c.name, stg))
                continue
            got = out.get("case%u.%s" % (i, stg), "<MISSING>")
            want = c.expect[stg] if c.expect[stg] is not None else "OK"
            if got != want:
                fails.append("%s.%s: want %r got %r" % (c.name, stg, want, got))
        hl = (out.get("case%u.hash" % i) or "").split(" ")
        calls = int(hl[0]) if hl and hl[0].isdigit() else -1
        is224 = int(hl[1]) if len(hl) > 1 else -1
        hexin = hl[2] if len(hl) > 2 else None
        if calls != c.hash_calls:
            fails.append("%s: hash calls %d want %d" % (c.name, calls, c.hash_calls))
        if calls and is224 != 0:
            fails.append("%s: sha224 truncates the app_id — is224=%d" % (c.name, is224))
        if calls == 1:
            want_in = (c.hash_in if c.hash_in is not None else GOLDEN_SLUG)
            if hexin != want_in.encode("utf-8").hex():
                fails.append("%s: hashed %r, not the slug's UTF-8 bytes" % (c.name, hexin))
        kind, pymsg = c.py
        if kind != "skip":
            ok, msg = python_outcome(
                c.hdr,
                app_id=(c.expect_id if c.flags & F_EXPECT else None),
                stem=c.stem if c.stem is not None else "__unset__")
            if kind == "ok" and not ok:
                fails.append("%s: STRICTER vector lost its meaning — the "
                             "writer also refuses: %r" % (c.name, msg))
            if kind == "refuse":
                if ok:
                    fails.append("%s: the writer ACCEPTS this header — the "
                                 "mirror lost a refusal" % c.name)
                elif pymsg is not None and msg != pymsg:
                    fails.append("%s: writer's words drifted: %r vs %r"
                                 % (c.name, msg, pymsg))
        if c.sets is not None:
            got = sets_out.get("case%u" % i, [])
            want = ["%u %d %s %d %d %d %s" % (
                k, orbapp.SET_TYPES.index(s["type"]),
                s["id"].encode("utf-8").hex(), s["min"], s["max"], s["default"],
                s["def_str"].encode("utf-8").hex())
                for k, s in enumerate(c.sets)]
            if got != want:
                fails.append("%s: settings unpack differs:\n  got  %r\n  want %r"
                             % (c.name, got, want))
    for bits in (0x00, 0x10, 0x20, 0xB0, 0xF0, 0xFF):
        joined = ", ".join(orbapp.caps_names(bits))
        want = "caps %X %d %s" % (bits, len(joined), joined.encode("utf-8").hex())
        if want not in out:
            fails.append("caps battery %02X: want %r" % (bits, want))
    if "caps TRUNC -1" not in out:
        fails.append("caps truncation must return -1, never a torn-down line")
    return fails


def lint():
    """The residency-suite rule: the mirror passes only while it still matches
    the shipping text and the writer's live answers."""
    hc = open(READER_C).read()
    hh = open(READER_H).read()
    both = hc + hh
    for needle in ("ORBAPP_BEAT_MIN_MS 3000u", "ORBAPP_BEAT_MAX_MS 86400000u",
                   "ORBAPP_GEOM_BYTES_MAX 0x00040000u",
                   "ORBAPP_ART_BYTES_MAX  0x003D0000u",
                   "ORBAPP_SETTINGS_MAX   16u",
                   "ORBAPP_MAGIC      0x00415247u",
                   '{ "power", "net", "sd_read", "themes" }'):
        assert needle in both, "reader drifted from the agreed numbers: " + needle
    assert orbapp.BEAT_MIN_MS == 3000 and orbapp.BEAT_MAX_MS == 86400000
    assert orbapp.GEOM_BYTES_MAX == 0x40000 and orbapp.ART_BYTES_MAX == 0x3D0000
    assert orbapp.SETTINGS_MAX == 16 and orbapp.MAGIC == 0x00415247
    assert (orbapp.CAP_FREE, orbapp.CAP_OWNER) == \
           (("time", "config", "ui", "art"),
            ("power", "net", "sd_read", "themes"))
    assert orbapp.CAP_BITS == 8
    # the hand-written reasons, live-verified against the writer's own voice
    try:
        orbapp.unpack_settings(b"\0" * 64, 17)
        raise AssertionError("writer accepted 17 settings")
    except ValueError as e:
        assert str(e) == "17 settings; the loader's table holds 16", str(e)
    try:
        orbapp.unpack_settings(b"\0" * 192, 4)
        raise AssertionError("writer accepted a short blob")
    except ValueError as e:
        assert str(e) == ("settings blob is 192 B, short of the 256 B its "
                          "count claims"), str(e)
    t4 = bytearray(64)
    t4[16] = 4
    try:
        orbapp.unpack_settings(bytes(t4), 1)
        raise AssertionError("writer accepted type word 4")
    except ValueError as e:
        assert str(e) == "setting 0 has type word 4, outside C9's frozen 4", str(e)


MUTATIONS = [
    ("beat_floor_dropped", "h", "ORBAPP_BEAT_MIN_MS 3000u",
     "ORBAPP_BEAT_MIN_MS 2000u"),
    ("geom_cap_doubled", "h", "ORBAPP_GEOM_BYTES_MAX 0x00040000u",
     "ORBAPP_GEOM_BYTES_MAX 0x00080000u"),
    ("settings_cliff_offbyone", "c",
     "h->n_settings > ORBAPP_SETTINGS_MAX",
     "h->n_settings >= ORBAPP_SETTINGS_MAX"),
    ("owner_mask_check_dropped", "c", "(h->caps & ~owner_mask)",
     "(0 && (h->caps & ~owner_mask))"),
    # D21 (§0.9): the art-carry gate and the art_req bound are the two words
    # the phase flipped; each must have a vector that dies without it.
    ("d21_art_carry_gate_dropped", "c",
     "if (h->art_size != 0 || h->art_crc != 0)",
     "if (0 && (h->art_size != 0 || h->art_crc != 0))"),
    ("art_req_bound_doubled", "h", "ORBAPP_ART_BYTES_MAX  0x003D0000u",
     "ORBAPP_ART_BYTES_MAX  0x007A0000u"),
    # get_field refuses an unterminated field through TWO arms — the loop's
    # end-of-capacity guard and the loop-tail return. Each alone makes the
    # other's removal invisible (verified: both single-arm mutants escape, as
    # equivalence should); the vector set must catch their removal TOGETHER.
    ("unterminated_field_allowed", "c", [
        ("        if (i + 1 >= dst_cap) return damaged;   /* no room left to terminate */\n",
         ""),
        ("    return damaged;   /* ran the whole field with no NUL */",
         "    dst[nbytes - 1] = 0; return NULL;"),
    ]),
    ("app_id_bytes_swapped", "c", "((uint32_t)d[0] << 24)",
     "((uint32_t)d[3] << 24)"),
    ("stem_checked_against_title", "c", "strcmp(stem, h->slug) != 0",
     "strcmp(stem, h->title) != 0"),
]


def cases_all():
    V = []
    vectors_accept(V.append)
    vectors_refuse(V.append)
    return V


def main():
    cases = cases_all()
    lint()
    with tempfile.TemporaryDirectory() as tmp:
        src_c, src_h = open(READER_C).read(), open(READER_H).read()
        binp = compile_harness(tmp, src_c, src_h)
        out, sets_out = run_harness(binp, cases)
        fails = compare(cases, out, sets_out)
        if "--mutations" not in sys.argv:
            for f in fails:
                print("FAIL " + f)
            print("%d vectors, %d mismatches" % (len(cases), len(fails)))
            return 1 if fails else 0
        bad = 0
        muts = [(e[0], e[1], [(e[2], e[3])] if len(e) == 4 else e[2])
                for e in MUTATIONS]
        for nm, which, pairs in muts:
            c2, h2 = src_c, src_h
            for old, new in pairs:
                if which == "c":
                    assert c2.count(old) == 1, nm + ": anchor lost in the .c"
                    c2 = c2.replace(old, new)
                else:
                    assert h2.count(old) == 1, nm + ": anchor lost in the .h"
                    h2 = h2.replace(old, new)
            mtmp = os.path.join(tmp, "mut_" + nm)
            os.makedirs(mtmp, exist_ok=True)
            mb = compile_harness(mtmp, c2, h2)
            mout, msets = run_harness(mb, cases)
            mfails = compare(cases, mout, msets)
            if not mfails:
                print("MUTATION ESCAPED: %s — no vector caught it" % nm)
                bad += 1
            else:
                print("caught: %-28s (%s)" % (nm, mfails[0][:70]))
        print("%d mutants, %d escaped" % (len(MUTATIONS), bad))
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
