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
import re
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
READER_C = os.path.join(ROOT, "main", "app", "orb_app_manifest.c")
READER_H = os.path.join(ROOT, "main", "app", "orb_app_manifest.h")
ASSETS_H = os.path.join(ROOT, "main", "assets", "orb_assets.h")
# P5b: the registry is on this harness's link line, so its rules are mutable too and
# the mutant set covers them. A suite that could only break the reader's own file
# would leave the reference grammar — the one rule D23 rests on — unwatched.
ASSETS_C = os.path.join(ROOT, "main", "assets", "orb_assets.c")

_spec = importlib.util.spec_from_file_location("orbapp", os.path.join(HERE, "orbapp.py"))
orbapp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(orbapp)

NL = "\n"
SHIM_ESP_ERR = """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
/* P5b: orb_assets.c is now on this harness's link line, and the registry reports
 * a bad bind table with the two codes the real header defines. The VALUES are the
 * ones esp_err.h carries, so a vector that asserts on a returned code asserts on
 * the same number the device would print. */
#define ESP_ERR_INVALID_ARG  0x102
#define ESP_ERR_INVALID_SIZE 0x104
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
        /* P5b: one more optional stage after the settings', so an asset vector can
         * reach the row rules without the settings table being re-forged to satisfy
         * the same count the header carries. The blob is the writer's pack_assets
         * output, and `n_assets` comes from the header's own word — the count a
         * caller lies with belongs to the unpack call, not to the file. */
        const uint32_t abw = rd32();
        if (abw > 6 * 16) return 3;
        static uint32_t abuf[6 * 16];
        rdb(abuf, abw * 4);
        const uint32_t n_aunpack = rd32();

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
        /* P5b. F_ASSETS (bit 16): the row rules on the table the header declares,
         * then D26's gate over the rows that survived them — the install path in
         * two calls and not one, because they catch different damage. unpack reads
         * bytes against the class vocabulary and the panel; check_assets reads the
         * declarations against the bound content sources, which is why a vector
         * that reached only the first would bless a required slot no theme carries.
         * The slug is the header's own and not the stem argument: ns 0 is the
         * OWNER's namespace, and the owner is who the file says it is. */
        orb_asset_decl_t aout[16];
        if (!why && (flags & 16)) {
            snprintf(tag, sizeof tag, "case%u.aunpack", i);
            why = orbapp_unpack_assets(abuf, abw, n_aunpack, h.slug, aout,
                                       reason, sizeof reason);
            stage(tag, why, reason);
            if (!why) {
                /* Row-shaped, like .set: the class, the required flag, the
                 * reference and the box. The reference prints whole — prefix and
                 * all — because assembling it is this function's one irreversible
                 * act, and a row that came back `bg` under a global/ declaration
                 * would look identical in a name-only dump. */
                for (uint32_t k = 0; k < n_aunpack; k++) {
                    printf("case%u.arow %u %u %d ", i, k, aout[k].cls,
                           aout[k].required);
                    puthex((const unsigned char *)aout[k].ref,
                           strnlen(aout[k].ref, ORB_ASSET_REF_MAX));
                    printf(" %u %u %d %d\n", aout[k].at.w, aout[k].at.h,
                           aout[k].at.x, aout[k].at.y);
                }
                snprintf(tag, sizeof tag, "case%u.agate", i);
                const orb_asset_ans_t a = orbapp_check_assets(aout, (int)n_aunpack,
                                                              h.slug, reason,
                                                              sizeof reason);
                /* The gate's answer is a code and not a refusal string: stage()
                 * prints REFUSE for any non-NULL, and here the non-NULL of it is
                 * the ladder's reason for a slot that EXISTS. Printed as the enum
                 * value so a vector pins the answer and not the prose. */
                if (a == ORB_ASSET_OK) printf("%s OK\n", tag);
                else                   printf("%s GATE %d %s\n", tag, (int)a, reason);
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

# One legal row, the subject of every asset-refusal vector below. A sprite at a
# plain box, in the app's own namespace: the shape whose byte the vector breaks, and
# nothing about it that a forged field could hide behind.
ROW_DIAL = [{"name": "dial", "cls": "sprite", "x": 8, "y": 8, "w": 16, "h": 16}]


def build_hdr(slug=GOLDEN_SLUG, title=GOLDEN_TITLE, caps=0x10, period=3000,
              abi=None, counts=(1, 1, 1, 1), settings=SETTINGS_GOLDEN,
              art_size=0, geom_size=16, sig=0, app_id=None, assets=()):
    """The writer's own pack_header at its legal best, then the payload-size
    words the caller fills, then a RE-computed hdr_crc — so every vector that
    reaches the semantic stage really did pass the checksum stage too. D21
    (§0.9) made the golden art-free: a non-zero art_size kw cannot come from
    pack_header any more (the writer sheds the field), so the vector FORGES it
    into the packed words, header CRC and all — exactly the pre-D21 file shape
    a stranger's card could still carry, which is precisely what the gate must
    refuse. The forged claim also drags art_req with it, matching the old
    tie art_req=art_size that pre-D21 files carried.

    `assets` (P5b) is the D23 table and goes through pack_assets, so a vector that
    declares a table gets the header's derived asset_off/geom_off for free. A
    vector that wants a WRONG offset must patch the word afterwards: forging the
    placement here would test the writer's arithmetic instead of the reader's rule.
    """
    sblob = orbapp.pack_settings(orbapp.check_settings(list(settings)))
    ablob = orbapp.pack_assets(assets, slug) if assets else b""
    hdr = orbapp.pack_header(app_id if app_id is not None else orbapp.app_id_of(slug),
                             slug, title, caps, period, art_size, counts,
                             sblob, abi=abi, sig=sig, asset_blob=ablob)
    hdr[orbapp.W_GEOM_SIZE] = geom_size
    if art_size:
        hdr[orbapp.W_ART_SIZE] = art_size
        hdr[orbapp.W_ART_REQ] = art_size
    hdr[orbapp.W_HDR_CRC] = orbapp._hdr_crc(hdr)
    return hdr, sblob, ablob


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
    of the reason, compared with the sentinel's full fury.

    P5b adds `ablob`/`n_aunpack` for the asset stage and `arows`/`agate` for its
    expectations. The blob rides the protocol on EVERY case, empty where the vector
    does not reach the stage, because the harness reads fields positionally: an
    optional tail would make every older vector's shape depend on its flags."""
    def __init__(self, name, hdr, sblob=b"", flags=0, expect_id=0, stem=None,
                 n_unpack=0, parse=None, check=None, layout=None, unpack=None,
                 sets=None, hash_calls=1, hash_in=None, py=("ok", None),
                 note="", ablob=b"", n_aunpack=0, aunpack=None, arows=None,
                 agate=None):
        self.name, self.hdr, self.sblob = name, hdr, sblob
        self.flags, self.expect_id, self.stem = flags, expect_id, stem
        self.n_unpack = n_unpack
        self.ablob, self.n_aunpack = ablob, n_aunpack
        self.expect = {"parse": parse, "check": check, "layout": layout,
                       "unpack": unpack}
        # Separate dict, not two more keys in the one above: compare() walks the
        # shared table and would demand an OK for every stage a vector does not
        # exercise, turning 40 older vectors into failures instead of skipping them.
        self.aexpect = {"aunpack": aunpack, "agate": agate} if (flags & 16) else {}
        self.arows = arows          # list of "IDX CLS REQ REFHEX W H X Y" lines
        self.sets, self.hash_calls, self.hash_in = sets, hash_calls, hash_in
        self.py, self.note = py, note



F_CHECK, F_LAYOUT, F_UNPACK, F_EXPECT = 1, 2, 4, 8
F_ASSETS = 16                                  # P5b: the D23 table's own stage

# orb_asset_ans_t, as the gate prints it. Named here and not read out of the header
# because the harness emits the enum's VALUE and this file must say which value it
# expected; the assert below is what keeps the local names from drifting from the
# shipped enum, which is the drift these suites exist to catch.
A_OK, A_APP, A_BOARD, A_NO_SLOT, A_NO_CONTENT = 0, 1, 2, 3, 4
A_REQUIRED, A_UNDECLARED, A_CLASS, A_NAME, A_ROOM = 5, 6, 7, 8, 9
ORB_ANS_ENUM = None                            # filled by lint() from the header


def asset_rows(rows, slug):
    """Expected `caseN.arow` payloads, in the harness's line shape, from the
    writer's own unpacked dicts — the C reader's twin.

    The class goes back to an index here because the writer's unpack hands a name
    (a manifest author reads `sprite`, a switch statement does not) and the wire
    and the C struct both carry the number. The conversion is the writer's own table
    looked up rather than a second list of class names, so an appended class moves
    both sides from one place. The reference is re-assembled from the row's own name
    and namespace rather than from the vector's input, because assembling it is the
    function under test's job and the one step a row cannot recover from."""
    return ["%d %d %d %s %d %d %d %d" % (
        i, orbapp.ASSET_CLASSES.index(r["cls"]), r["required"],
        orbapp.asset_ref(r, slug).encode("utf-8").hex(),
        r["w"], r["h"], r["x"], r["y"])
        for i, r in enumerate(rows)]


def _golden_settings16():
    return [{"id": "s%02d" % i, "type": "int", "min": 0, "max": 5, "default": 2}
            for i in range(16)]


def vectors_accept(add):
    g, sblob, _ab = build_hdr()
    want_sets = orbapp.unpack_settings(sblob, 4)
    add(Case("golden_full", g, sblob, flags=F_CHECK | F_LAYOUT | F_UNPACK,
             n_unpack=4, sets=want_sets))
    # ---- the asset table (P5b, D23) ----------------------------------------
    # The golden table is three rows and not one, because the shape that matters is
    # a MIXTURE: an owner-scoped sprite, a `global/` plate, and a HAND whose box the
    # compositor rotates about. One row per shape would let a writer that assembled
    # every reference the same way pass, and D23 exists for the two namespaces.
    A3 = [{"name": "dial", "cls": "sprite", "x": 24, "y": 24, "w": 200, "h": 200},
          {"name": "bg", "cls": "plate", "ns": "global", "required": 1,
           "x": 0, "y": 0, "w": 240, "h": 240},
          {"name": "hour", "cls": "hand", "x": 112, "y": 40, "w": 16, "h": 80}]
    ga, sab, ab3 = build_hdr(assets=A3)
    # The expected rows, in the harness's own line shape, built from the writer's
    # unpack and not from the literals above: the vector then proves the C reader
    # and the Python reader agree, which is the point of the mirror, and a test that
    # restated the input would only prove the test was written.
    add(Case("assets_three_rows_round_trip", ga, sab, flags=F_CHECK | F_LAYOUT | F_ASSETS,
             ablob=ab3, n_aunpack=3,
             arows=asset_rows(orbapp.unpack_assets(ab3, 3), GOLDEN_SLUG),
             agate=A_REQUIRED,   # bg is required and nothing is bound
             note="the gate's answer IS the point here: with no source bound a"
                  " required slot must refuse, which is D26's install gate firing on"
                  " a table that unpacked clean"))
    ao, sob, abo = build_hdr(assets=A3[:1] + A3[2:])
    add(Case("assets_optional_rows_gate", ao, sob, flags=F_CHECK | F_LAYOUT | F_ASSETS,
             ablob=abo, n_aunpack=2,
             arows=asset_rows(orbapp.unpack_assets(abo, 2), GOLDEN_SLUG),
             note="no required row and no source bound: the ladder answers APP for"
                  " the sprite and the hand, which is the compiled-in default and"
                  " not a failure, so the gate passes and install proceeds"))
    add(Case("assets_empty_table_accepted", g, sblob,
             flags=F_CHECK | F_LAYOUT | F_ASSETS, n_aunpack=0, arows=[],
             note="a container with no slots is legal (D23): the pure-geometry and"
                  " text-only apps declare nothing and must still install"))
    s16 = _golden_settings16()
    g16, b16, _ab = build_hdr(settings=s16)
    add(Case("golden_16_settings", g16, b16, flags=F_CHECK | F_LAYOUT,
             note="the > vs >= cliff at SETTINGS_MAX; catches mutation 3"))
    g0, _, _ab = build_hdr(period=0)
    add(Case("period_zero_vector_only_app", g0, flags=F_CHECK | F_LAYOUT |
             F_UNPACK, n_unpack=0,
             note="0 is the D16 word for never ticks — accepted whole"))
    gf, _, _ab = build_hdr(caps=0xF0)
    add(Case("caps_all_four_owner_legal", gf, flags=F_CHECK | F_LAYOUT |
             F_UNPACK, n_unpack=0,
             note="D19 positive control: the max legal declaration passes"))
    s23 = "w" * 23
    g23, _, _ab = build_hdr(slug=s23)
    add(Case("slug_23_chars_field_edge", g23, flags=F_CHECK | F_LAYOUT |
             F_UNPACK, n_unpack=0, hash_in=s23,
             note="the off-by-one that would refuse the writer's own longest slug"))
    gu, _sb, _ab = build_hdr()
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
    f2 = patch(build_hdr()[0][:], orbapp.W_FORMAT, orbapp.FORMAT + 1)
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
    bs, _, _ab = build_hdr(slug="Bad-Slug", app_id=0x12345678)
    ok, msg = python_outcome(bs)
    add(Case("slug_bad_charset", bs, flags=F_CHECK, check=msg, hash_calls=0,
             py=("refuse", msg)))
    wa, _, _ab = build_hdr(app_id=0x12345678)
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
    g, _sb, _ab = build_hdr()
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
    # -- P7 (D20/D27): the code region and its memory bound ------------------------
    # A hand-forged code triple inside a golden header, CRC kept honest by patch so
    # each vector probes the rule that owns the field and not the checksum. The
    # writer's check_header and the device's orbapp_check_header must refuse the same
    # damage in the same words — the mirror's whole point — so py=("refuse", msg) pins
    # that agreement, and the ACCEPT vector pins that a coherent module-bearing
    # header is admitted by both voices and not just refused by the stricter one.
    def code_hdr(code_size, code_crc, mem):
        w = build_hdr()[0][:]
        w[orbapp.W_CODE_SIZE] = code_size
        w[orbapp.W_CODE_CRC] = code_crc
        w[orbapp.W_WASM_MEM] = mem
        w[orbapp.W_HDR_CRC] = orbapp._hdr_crc(w)
        return w
    # ACCEPT: a coherent code triple — a real module, a bounded memory ask. Only the
    # check stage runs (layout is the next loop's subject), so the words that gate
    # D27 are what this proves, and the reader must say OK where the writer says OK.
    acc = code_hdr(256, 0x1234abcd, 65536)
    ok, msg = python_outcome(acc)
    assert ok, "the coherent code header must pass the writer's check: %s" % msg
    add(Case("code_bearing_header_accepted", acc, flags=F_CHECK, check=None,
             hash_calls=1, py=("ok", None),
             note="P7 D20/D27 positive control: a coherent module + bounded memory "
                  "is the shape every wasm app will ship; both voices accept it"))
    # REFUSE: the D27 ceiling. wasm_mem_max past 128 KB is the refusal the plan's
    # P7 pass line names ("install-time refuses > 128 KB"), reached at pack, at read
    # and on the dial in one sentence.
    over = code_hdr(256, 0x1234abcd, orbapp.WASM_MEM_MAX + 1)
    ok, msg = python_outcome(over)
    add(Case("wasm_mem_past_ceiling_refused", over, flags=F_CHECK, check=msg,
             hash_calls=1, py=("refuse", msg),
             note="D27: the module could not be allocated on the dial, so it must "
                  "not install — the refusal the P7 pass line benches"))
    # REFUSE: a module with no bound. Zero memory on a code-bearing app is no ceiling
    # for the gate to hold, the inverse of the D27 intent.
    zmem = code_hdr(256, 0x1234abcd, 0)
    ok, msg = python_outcome(zmem)
    add(Case("code_with_zero_mem_refused", zmem, flags=F_CHECK, check=msg,
             hash_calls=1, py=("refuse", msg),
             note="D27: a zero linear-memory bound is no bound at all"))
    # REFUSE: a bound with no module. The native shape is code_size 0 and mem 0; a
    # non-zero mem on a codeless header is a miscount, mirrored from pack.
    nocode = patch(build_hdr()[0][:], orbapp.W_WASM_MEM, 65536)
    ok, msg = python_outcome(nocode)
    add(Case("mem_bound_with_no_code_refused", nocode, flags=F_CHECK, check=msg,
             hash_calls=1, py=("refuse", msg),
             note="ships a memory bound for a module it does not carry"))
    # REFUSE: code past the slot budget — it cannot fit the flash it would write.
    big = code_hdr(orbapp.CODE_BYTES_MAX + 4, 0, 65536)
    ok, msg = python_outcome(big)
    add(Case("code_past_slot_refused", big, flags=F_CHECK, check=msg,
             hash_calls=1, py=("refuse", msg),
             note="D27's flash half: a module past a slot's payload cannot install"))
    # LAYOUT: the code region mislaid. Reuses the file-layout loop's shape so a code
    # region that could sit anywhere is refused, exactly as a mislaid geometry is.
    lay = patch(build_hdr()[0][:], orbapp.W_CODE_OFF, 449)
    h = orbapp.parse_header(pack_words(lay))
    try:
        orbapp.check_file_layout(h)
        raise AssertionError("code_off_unaligned: writer accepted the mislayout")
    except ValueError as e:
        msg = str(e)
    add(Case("code_off_unaligned", lay, flags=F_CHECK | F_LAYOUT, layout=msg,
             py=("skip", None)))
    # -- the settings table on its own --------------------------------------------------
    g, sblob, _ab = build_hdr()
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
    # Built from the writer's own dialect word and not from a literal 1: P5b moved
    # the vocabulary's ABI to 2 with the asset table, and a vector that names the
    # number it probes goes stale on every bump, reporting a refusal that never
    # happened as one that did.
    def abi_refuse(lo, hi):
        return ("app needs geometry ABI %d..%d; this build speaks %d —"
                " re-package it (C4)" % (lo, hi, orbapp.GEOM_ABI))
    a2 = patch(build_hdr()[0][:], orbapp.W_GEOM_ABI, orbapp.GEOM_ABI + 1)
    ok, msg = python_outcome(a2)
    add(Case("abi_stamp_out_of_own_range", a2, flags=F_CHECK,
             check=abi_refuse(orbapp.GEOM_ABI, orbapp.GEOM_ABI), hash_calls=0,
             py=("refuse", msg),
             note="WORDING-DIFF: the writer prints the header's own field "
                  "under its build label; the reader prints the build — both "
                  "refuse, the numbers on the page come from the reader"))
    r23 = patch(patch(patch(build_hdr()[0][:], orbapp.W_GEOM_ABI, orbapp.GEOM_ABI + 1),
                      orbapp.W_ABI_MIN, orbapp.GEOM_ABI + 1),
                 orbapp.W_ABI_MAX, orbapp.GEOM_ABI + 2)
    add(Case("abi_range_excludes_this_build_STRICTER", r23, flags=F_CHECK,
             check=abi_refuse(orbapp.GEOM_ABI + 1, orbapp.GEOM_ABI + 2),
             hash_calls=0, py=("ok", None),
             note="the writer's reader waves this through (its own stamped "
                  "abi sits in its own range); the device must not install "
                  "geometry it reads wrong"))
    r00 = patch(patch(build_hdr()[0][:], orbapp.W_ABI_MIN, 0),
                orbapp.W_ABI_MAX, 0)
    ok, msg = python_outcome(r00)
    add(Case("abi_range_excludes_everything", r00, flags=F_CHECK, check=msg,
             hash_calls=0, py=("refuse", msg)))
    # ---- the asset table's own refusals (P5b, D26) --------------------------
    # Each vector forges ONE byte of an otherwise legal row and carries the header
    # that counts it, so the vector reaches the row rule and not the count check two
    # calls earlier. `ahdr()` re-derives the placement the writer would have derived,
    # then patches the row; a forged OFFSET belongs to the layout vectors above and
    # not here, where the subject is a row's content.
    def arow_vector(nm, rows, want, forge, n_aunpack=None):
        h, sb, ab = build_hdr(assets=rows)
        b = bytearray(ab)
        forge(b)                       # mutates; returns nothing on purpose
        w = list(h)
        # The count stays the declared one; the blob's byte count changed only in
        # content, so the header stays honest about what it holds and the CRC is
        # recomputed for the same reason the settings vectors do it.
        w[orbapp.W_N_ASSETS] = len(rows)
        w[orbapp.W_HDR_CRC] = orbapp._hdr_crc(w)
        add(Case(nm, w, sb, flags=F_CHECK | F_LAYOUT | F_ASSETS, ablob=bytes(b),
                 n_aunpack=len(rows) if n_aunpack is None else n_aunpack,
                 aunpack=want, py=("skip", None)))

    # Each forge breaks one field of row 0 and nothing else, so the refusal names
    # the field and the vector proves the reader read it from the right place. The
    # row's words are 0=class/req/ns, 1-3=name, 4=x|y, 5=w|h — laid out here in the
    # numbers and not by name, because a vector that recomputed the offsets from the
    # constants would keep passing if a constant and the C reader disagreed.
    # Word 0 carries class in byte 0, the required flag in byte 1 and the namespace
    # in byte 2, so each value below is a WHOLE word and keeps the sprite's class of
    # 1 in place. A helper that set one byte by shifting a naked number would have
    # made "required byte is 2" a vector about class 2 instead.
    #
    # Slice assignment and nothing narrower: `b[0:4].__setitem__(...)` mutates the
    # copy a slice returns, so the first draft of these forgers forged nothing at all
    # and every row below reported a clean reader. A test that silently stops testing
    # is worse than no test, which is why each forger is a def that assigns.
    def forge0(v):
        def f(b):
            b[0:4] = struct.pack("<I", v)
        return f

    def forgew(i, v):
        def f(b):
            b[i * 4:i * 4 + 4] = struct.pack("<I", v)
        return f

    def forgedeck(x, y, w, h):
        def f(b):
            b[16:24] = struct.pack("<II", (x & 0xFFFF) | ((y & 0xFFFF) << 16),
                                   (w & 0xFFFF) | ((h & 0xFFFF) << 16))
        return f

    SPRITE = 1                                        # orb_asset_class_t index
    # The first id past the vocabulary, DERIVED from the enum this file already lifted
    # rather than written as a literal. The vector's whole value is that it names the
    # boundary; a hard-coded 8 became a false pass the day D34 appended AUX at that
    # index, and a passing boundary test that no longer tests a boundary is the exact
    # kind of green this suite exists to refuse.
    _ae = re.search(r"typedef enum \{(.*?)\} orb_asset_class_t;",
                    open(READER_C).read() + open(ASSETS_H).read(), re.S)
    _first_past = len([n for n in re.findall(
        r"^\s*(ORB_ASSET_[A-Z_]+)\s*(?:=\s*\d+)?\s*(?:,|/)", _ae.group(1), re.M)
        if n != "ORB_ASSET_CLASS_N"])
    arow_vector("asset_class_past_the_vocabulary", ROW_DIAL,
                "asset 0 declares class %d, which this build's vocabulary does not"
                " carry: the app needs an ABI this dial does not speak" % _first_past,
                forge0(_first_past))
    arow_vector("asset_class_way_past", ROW_DIAL,
                "asset 0 declares class 200, which this build's vocabulary does not"
                " carry: the app needs an ABI this dial does not speak", forge0(200))
    arow_vector("asset_required_not_a_bit", ROW_DIAL,
                "asset 0's required byte is 2: D31's selector is one bit, and a value"
                " that is neither 0 nor 1 is a writer that guessed",
                forge0(SPRITE | (2 << 8)))
    arow_vector("asset_namespace_third", ROW_DIAL,
                "asset 0's namespace byte is 3: there are two namespaces and no third"
                " (D23)", forge0(SPRITE | (3 << 16)))
    # The boundary and not the well-past value, because `ns > 1u` off-by-one'd to
    # `ns > 2u` refuses 3 and accepts 2 — and 2 is the first illegal one. The
    # settings suite learned this as settings_cliff_offbyone; a count check is only
    # proven at the number where its own bound flips.
    arow_vector("asset_namespace_cliff", ROW_DIAL,
                "asset 0's namespace byte is 2: there are two namespaces and no third"
                " (D23)", forge0(SPRITE | (2 << 16)))
    arow_vector("asset_fourth_byte_set", ROW_DIAL,
                "asset 0's fourth byte is not zero: the row has no fourth field, and a"
                " writer that filled one means something this reader cannot honour",
                forge0(SPRITE | (4 << 24)))
    arow_vector("asset_name_empty", ROW_DIAL,
                "asset 0 has an empty name: a slot nobody can name cannot be resolved"
                " and cannot be refused by name, so it can only be refused here",
                forgew(1, 0))
    arow_vector("asset_box_off_panel", ROW_DIAL,
                "asset 0 (\"weather/dial\") is a 1x1 box at %d,0, which is not inside"
                " the %dx%d panel" % (orbapp.PANEL, orbapp.PANEL, orbapp.PANEL),
                forgedeck(orbapp.PANEL, 0, 1, 1))
    # The count the CALLER lies with, against a blob that holds one row: the same
    # shape as the settings suite's unpack_count_over_table, and the reason the two
    # numbers travel separately through the protocol.
    h, sb, ab = build_hdr(assets=ROW_DIAL)
    add(Case("asset_unpack_count_over_the_table", h, sb,
             flags=F_CHECK | F_LAYOUT | F_ASSETS, ablob=ab, n_aunpack=17,
             aunpack="17 assets; the loader's table holds 16", py=("skip", None)))
    add(Case("asset_unpack_blob_short", h, sb,
             flags=F_CHECK | F_LAYOUT | F_ASSETS, ablob=ab[:20], n_aunpack=1,
             aunpack="asset table is 20 B, short of the 24 B its count claims",
             py=("skip", None)))
    h17 = patch(build_hdr()[0][:], orbapp.W_N_ASSETS, 17)
    ok, msg = python_outcome(h17)
    add(Case("asset_n_assets_over_the_table", h17, flags=F_CHECK, check=msg,
             py=("refuse", msg)))
    hun = patch(build_hdr()[0][:], orbapp.W_ASSET_OFF, 178)
    ok, msg = python_outcome(hun)
    add(Case("asset_offset_unaligned", hun, flags=F_CHECK, check=msg,
             py=("refuse", msg)))
    hbig = patch(build_hdr()[0][:], orbapp.W_ASSET_OFF, orbapp.GEOM_BYTES_MAX + 4)
    ok, msg = python_outcome(hbig)
    add(Case("asset_offset_past_the_region", hbig, flags=F_CHECK, check=msg,
             py=("refuse", msg)))
    # The placement equalities, forged one word at a time: this is the pair of rules
    # that makes an overlapping container unrepresentable, and the theme
    # container's FORMAT-1 scar is the precedent that says it must be a rule.
    for nm, word, bad in (("asset_off_moved", orbapp.W_ASSET_OFF, 400),
                          ("geom_off_overlaps_assets", orbapp.W_GEOM_OFF, 300)):
        lay = patch(build_hdr()[0][:], word, bad)
        ok, msg = python_outcome(lay)
        assert ok, "%s must survive the semantic stage to reach layout" % nm
        try:
            orbapp.check_file_layout(orbapp.parse_header(pack_words(lay)))
            raise AssertionError(nm + ": writer accepted the mislayout")
        except ValueError as e:
            msg = str(e)
        add(Case(nm, lay, flags=F_CHECK | F_LAYOUT, layout=msg, py=("skip", None)))






# ---- run: compile the SHIPPING reader, drive it, compare --------------------
def compile_harness(tmp, reader_c_text, reader_h_text, assets_c_text=None,
                    assets_h_text=None):
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
    # The registry compiles from its shipped path unless a caller handed over text,
    # which happens only for a mutant: writing the copy into the harness dir is what
    # puts it ahead of the real one on the include path, so an asset-side mutation
    # mutates the code the reader actually links rather than a file nobody reads.
    asrc = ASSETS_C
    if assets_c_text is not None:
        asrc = os.path.join(tmp, "orb_assets.c")
        open(asrc, "w").write(assets_c_text)
    # Same for the registry's header: a "d" mutant rewrites the class vocabulary or a
    # bound, and the copy has to land in the harness dir, which -I lists first, or the
    # mutation edits a file the build never opens.
    ainc = os.path.join(ROOT, "main", "assets")
    if assets_h_text is not None:
        open(os.path.join(tmp, "orb_assets.h"), "w").write(assets_h_text)
        ainc = tmp
    cc = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra",
                         "-Wno-unused-function", "-Werror",
                         "-I", tmp, "-I", os.path.join(ROOT, "main"),
                         # P5b: orb_app_manifest.h includes orb_assets.h for the
                         # class vocabulary, and that TU's code is compiled in —
                         # the reader's refusal of a bad class byte is the shipped
                         # orb_asset_class_known(), not a re-typed bound. The
                         # registry needs no shim: it reaches nothing but libc.
                         "-I", ainc,
                         "-I", os.path.join(ROOT, "assets"),
"-I", os.path.join(ROOT, "main", "art"),
                         "-I", shim, "-o", binp, src, asrc, hac],
                        capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("harness build failed:" + NL + cc.stderr[-3000:])
    return binp


def hdr_bytes_for(c):
    """One case, in the byte order the harness reads: flags+expect,
    the header words, the stem as raw bytes, the digest the sha hook
    hands back, the settings words and count, and the asset words and
    count."""
    w = c.hdr
    stem = (c.stem or "").encode("utf-8")
    sblob = bytes(c.sblob)
    ablob = bytes(c.ablob)
    out = [struct.pack("<II", c.flags, c.expect_id),
           b"".join(struct.pack("<I", v) for v in w),
           struct.pack("<I", len(stem)), stem,
           hashlib.sha256((c.hash_in if c.hash_in is not None else
                           GOLDEN_SLUG).encode("utf-8")).digest(),
           struct.pack("<I", len(sblob) // 4), sblob,
           struct.pack("<I", c.n_unpack),
           # P5b: the asset blob and the count to unpack it with, ALWAYS present so
           # the record's shape does not depend on a vector's flags. Two numbers the
           # install path keeps apart on purpose — the header's n_assets says what
           # the file claims, and this one says what the caller asked for, and the
           # gap between them is the vector.
           struct.pack("<I", len(ablob) // 4), ablob,
           struct.pack("<I", c.n_aunpack)]
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
            if parts[0].endswith(".arow"):
                # Accumulated like .set, under a key of its own: a vector that
                # unpacked settings AND assets in one run would otherwise land both
                # row types in one bucket, and a stage that printed three slots as
                # one is exactly the damage this line exists to catch.
                sets_out.setdefault(parts[0][:-5] + "#arow", []).append(
                    ln.split(" ", 1)[1])
                continue
            if parts[0].endswith(".hash"):
                out[parts[0]] = ln.split(" ", 1)[1]
                continue
            if parts[0].endswith(".agate") and parts[1] == "GATE":
                # Kept as the code and its reason, because the code is the answer
                # D26 routes on and the prose is what a ledger shows: a vector that
                # pinned only one of the two would let the other drift. Index 2 and
                # not 1: the harness's GATE marker is a stage verb, the same way a
                # REFUSE line's is, and leaving it in would make every expectation a
                # string that starts with a word no reader emits.
                out[parts[0]] = ln.split(" ", 3)[2:]
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
        for stg, bit in (("parse", 1), ("check", 1), ("layout", 2), ("unpack", 4),
                         ("aunpack", 16), ("agate", 16)):
            ran = stg == "parse" or bool(c.flags & bit)
            # The asset stages keep their own expectation table, and a vector that
            # did not set one is a vector that does not assert about them — the
            # older vectors carry no F_ASSETS bit and must stay exactly as loud as
            # they were before the protocol grew two fields.
            want_tbl = c.aexpect if stg in c.aexpect else c.expect
            # The asset gate runs over rows that unpacked, so a vector that expects
            # the row stage to refuse has no gate line to compare and no missing
            # stage to report: demanding one would fail a correct reader.
            if stg == "agate" and isinstance(c.aexpect.get("aunpack"), str):
                ran, want_tbl = False, {}
            if not ran:
                if want_tbl.get(stg) is not None:
                    fails.append("%s.%s: reason expected but the stage never ran"
                                 % (c.name, stg))
                continue
            got = out.get("case%u.%s" % (i, stg), "<MISSING>")
            want = want_tbl.get(stg) if want_tbl.get(stg) is not None else "OK"
            if isinstance(want, int):
                # The gate expects a bare answer code: the code is the contract a
                # caller branches on, and pinning its prose too would make every
                # rewording of a reason line a schema change. The reason the dial
                # prints still travels in the failure text below.
                want = "%d" % want
                if isinstance(got, list):
                    got = got[0] if got else "<EMPTY>"
            if got != want:
                fails.append("%s.%s: want %r got %r" % (c.name, stg, want, got))
        if c.arows is not None and not isinstance(c.aexpect.get("aunpack"), str):
            got = sets_out.get("case%u#arow" % i, [])
            if got != c.arows:
                fails.append("%s.arow: want %r got %r" % (c.name, c.arows, got))
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

    # ---- P5b: the asset mirror, read out of the shipped text ----------------
    lint_asset_mirror(hc, hh, open(ASSETS_H).read())


def lint_asset_mirror(hc, hh, ah):
    """The cross-header half of the mirror, as a function so a header-side mutant can
    be checked against the text it forged rather than only against the shipped file.

    The class vocabulary is the one field order in this format that a renumber
    corrupts silently: the wire carries an index, so a row written by a build that
    appended before `layer` resolves `dial` as somebody else's plate on every dial in
    the fleet, and every checksum in the container still passes. Lifting both the enum
    and the row layout out of the headers, rather than restating them, is what turns
    that from a shipping bug into a failed test."""
    a_enum = re.search(r"typedef enum \{(.*?)\} orb_asset_class_t;", hc + ah, re.S)
    assert a_enum, "orb_asset_class_t is gone from the shipped headers"
    # The terminator and the name shape both vary: the last enumerator carries an
    # underscore and no comma, and a reader that stopped at either would drop the very
    # value a count-over-table vector depends on. Assert the whole list rather than
    # trusting a regex to have found everything.
    names = re.findall(r"^\s*(ORB_ASSET_[A-Z_]+)\s*(?:=\s*\d+)?\s*(?:,|/)",
                       a_enum.group(1), re.M)
    assert names == ["ORB_ASSET_TEXT", "ORB_ASSET_SPRITE", "ORB_ASSET_HAND",
                     "ORB_ASSET_PLATE", "ORB_ASSET_RING", "ORB_ASSET_BANK",
                     "ORB_ASSET_LAYER", "ORB_ASSET_STATUS",
                     "ORB_ASSET_AUX",       # D34's append, at 8
                     "ORB_ASSET_CLASS_N"], \
        "the class vocabulary moved: %s" % (names,)
    assert orbapp.ASSET_CLASSES == tuple(n.lower().replace("orb_asset_", "")
                                        for n in names[:-1]), \
        "the writer's class tuple drifted from the C enum: %s" % (orbapp.ASSET_CLASSES,)
    # The ABI equality the two headers cannot express between themselves (they include
    # each other one way only), checked here so a bump on one side fails a suite even
    # when nobody builds the firmware. orb_assets.c carries the same _Static_assert;
    # this is the copy that runs on a machine with no toolchain.
    abi = re.search(r"#define\s+ORBAPP_GEOM_ABI\s+(\d+)u", hh)
    aabi = re.search(r"#define\s+ORB_ASSET_CLASS_ABI\s+(\d+)u", ah)
    assert abi and aabi, "a dialect word went missing from its own header"
    assert int(abi.group(1)) == int(aabi.group(1)) == orbapp.GEOM_ABI, \
        "the class vocabulary's ABI, the reader's dialect and the writer's disagree"
    for needle in ("ORBAPP_ASSETS_MAX     16u", "ORBAPP_ASSET_NAME_MAX 12u",
                   "ORB_ASSET_NAME_MAX      12u", "ORB_ASSET_OWNER_MAX     23u",
                   "ORB_ASSET_MAX  16"):
        assert needle in hc + hh + ah, "the asset mirror drifted: " + needle
    assert orbapp.ASSET_MAX == 16 and orbapp.ASSET_NAME_MAX == 12
    assert orbapp.ASSET_OWNER_MAX == 23, orbapp.ASSET_OWNER_MAX
    assert orbapp.ASSET_REF_MAX == orbapp.ASSET_OWNER_MAX + 1 + orbapp.ASSET_NAME_MAX
    assert orbapp.ASSET_WORDS == 6 and re.search(r"ORBAPP_A_WH\s*=\s*5", hh), \
        "the wire row grew a word on one side only"
    # An appended enumerator must grow the writer's tuple in the same commit.
    assert len(orbapp.ASSET_CLASSES) == len(names) - 1


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
    # ---- P5b: the asset table's own rules, each one mutated to nothing -------
    # Added because the first draft of the row forgers mutated a slice COPY and
    # every vector below silently reported a clean reader. A check nobody has
    # watched fail is a check that may already be dead.
    ("asset_class_bound_dropped", "c", "if (!orb_asset_class_known(cls))",
     "if (0 && !orb_asset_class_known(cls))"),
    ("asset_required_bit_unchecked", "c", "if (req > 1u)", "if (0 && req > 1u)"),
    ("asset_namespace_third_allowed", "c", "if (ns > 1u)", "if (ns > 2u)"),
    ("asset_reserved_byte_allowed", "c", "if ((b[ORBAPP_A_CLASS] >> 24) != 0)",
     "if (0 && (b[ORBAPP_A_CLASS] >> 24) != 0)"),
    ("asset_name_empty_allowed", "c", "if (!name[0])", "if (0 && !name[0])"),
    ("asset_count_bound_dropped", "c", "if (h->n_assets > ORBAPP_ASSETS_MAX)",
     "if (0 && h->n_assets > ORBAPP_ASSETS_MAX)"),
    # x|y and w|h read as one word each. Swapping the pair is the mutation this
    # vector set must die on: a reader that mixed them would accept every box in
    # the golden table, since those are square-ish, and misplace exactly the
    # non-square hand that the three-row vector carries.
    ("asset_box_halves_swapped", "c", [
        ("        out[i].at.x = (int16_t)(xy & 0xFFFFu);\n"
         "        out[i].at.y = (int16_t)((xy >> 16) & 0xFFFFu);\n"
         "        out[i].at.w = (uint16_t)(wh & 0xFFFFu);\n"
         "        out[i].at.h = (uint16_t)((wh >> 16) & 0xFFFFu);\n",
         "        out[i].at.w = (uint16_t)(xy & 0xFFFFu);\n"
         "        out[i].at.h = (uint16_t)((xy >> 16) & 0xFFFFu);\n"
         "        out[i].at.x = (int16_t)(wh & 0xFFFFu);\n"
         "        out[i].at.y = (int16_t)((wh >> 16) & 0xFFFFu);\n")]),
    # The placement equality, the rule that makes an overlapping container
    # unrepresentable rather than merely unusual.
    ("asset_layout_equality_dropped", "c",
     "    if (h->geom_off != (uint64_t)h->asset_off +\n"
     "                       (uint64_t)h->n_assets * ORBAPP_ASSET_WORDS * 4)",
     "    if (0 && h->geom_off != (uint64_t)h->asset_off +\n"
     "                       (uint64_t)h->n_assets * ORBAPP_ASSET_WORDS * 4)"),
    # The bug this batch was written for: a splitter that forbids the owner's slash
    # resolves nothing in an app's own namespace, and every reference the manifest
    # carries looks damaged. Restoring the pre-fix shape must fail the round trip.
    ("asset_owner_slash_forbidden", "a", [
        ("        const char *s = memchr(n, '/', len);\n"
         "        if (!s || s == n || (size_t)(s - n) > ORB_ASSET_OWNER_MAX) return false;\n"
         "        slash = (size_t)(s - n);\n",
         "        return false;\n")]),
    # D26's gate reduced to always-agree: the shape an install path takes when
    # someone moves the check "to the caller" and forgets to write the caller.
    ("asset_gate_always_passes", "a",
     "    for (int i = 0; i < n; i++) {",
     "    for (int i = 0; i < 0; i++) {"),
    # D31, and the corruption the lifted mirror exists for: a class promoted into the
    # middle of the vocabulary. Every enumerator keeps its name and every value after
    # it shifts by one, so a `hand` row a container wrote yesterday reads as a plate
    # tomorrow and no checksum in the file notices. Only the suite compares the enum's
    # ORDER against the writer's tuple.
    ("class_renumbered_in_the_middle", "d",
     "    ORB_ASSET_HAND,         /* a plate with a pivot: it turns                     */",
     "    ORB_ASSET_FRAME,\n    ORB_ASSET_HAND,         /* a plate with a pivot: it turns                     */"),
    # The vocabulary's ABI and the reader's dialect are separate words that must carry
    # the same number, and the two headers include each other one way only, so neither
    # can pin the pair. Bumping one side alone is the shape of a firmware that accepts
    # a container written for a class table it does not have.
    ("asset_abi_bumped_alone", "d",
     "#define ORB_ASSET_CLASS_ABI   2u",
     "#define ORB_ASSET_CLASS_ABI   3u"),
    # The registry's name field widened past the wire's twelve bytes: the writer keeps
    # accepting a name the device then truncates, and the truncated reference resolves
    # as a different slot rather than as a failure.
    ("asset_name_max_widened_past_the_wire", "d",
     "#define ORB_ASSET_NAME_MAX      12u",
     "#define ORB_ASSET_NAME_MAX      16u"),
    # ---- P7 (D20/D27): the code region's gates, each mutated to nothing ----------
    # The over-ceiling refusal is the exact rule the P7 pass line benches ("install
    # refuses > 128 KB"); dropping it lets an un-allocatable module reach the dial.
    ("d27_mem_ceiling_check_dropped", "c",
     "if (h->wasm_mem_max > ORBAPP_WASM_MEM_MAX)",
     "if (0 && h->wasm_mem_max > ORBAPP_WASM_MEM_MAX)"),
    # The ceiling itself doubled: the same over-ceiling vector then reads as legal,
    # so the number and its refusal must move together or a suite dies.
    ("d27_mem_ceiling_doubled", "h", "ORBAPP_WASM_MEM_MAX   0x00020000u",
     "ORBAPP_WASM_MEM_MAX   0x00040000u"),
    # A code-bearing module must carry a non-zero bound; dropping the rule lets a
    # module run with no ceiling the gate can hold, the inverse of D27.
    ("d27_zero_bound_allowed", "c",
     "if (h->code_size != 0 && h->wasm_mem_max == 0)",
     "if (0 && h->code_size != 0 && h->wasm_mem_max == 0)"),
    # A bound with no code behind it is a miscount; dropping it lets a native app
    # claim a wasm reservation and bill a runtime the reader never populates.
    ("d27_bound_without_code_allowed", "c",
     "if (h->code_size == 0 && (h->code_crc != 0 || h->wasm_mem_max != 0))",
     "if (0 && h->code_size == 0 && (h->code_crc != 0 || h->wasm_mem_max != 0))"),
    # The code region past the slot budget: dropping it lets a module too large to
    # copy reach install, where the slot gate then prices it too late.
    ("d27_code_size_bound_dropped", "c",
     "if (h->code_size > ORBAPP_CODE_BYTES_MAX)",
     "if (0 && h->code_size > ORBAPP_CODE_BYTES_MAX)"),
    # The layout equality that stops a code region overlapping the geometry.
    ("code_layout_equality_dropped", "c",
     "if ((uint64_t)h->code_off != art)",
     "if (0 && (uint64_t)h->code_off != art)"),
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
        src_a = open(ASSETS_C).read()
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
        # A "d" mutant edits the asset registry's own header. The two headers include
        # each other one way only, so the constants that must match across them are the
        # ones a single-source mutant cannot reach, and the suite is the only thing that
        # can: the class vocabulary's order, its ABI, and the name field's width.
        src_d = open(ASSETS_H).read()
        for nm, which, pairs in muts:
            c2, h2, a2, d2 = src_c, src_h, src_a, src_d
            for old, new in pairs:
                if which == "c":
                    assert c2.count(old) == 1, nm + ": anchor lost in the .c"
                    c2 = c2.replace(old, new)
                elif which == "a":
                    assert a2.count(old) == 1, nm + ": anchor lost in orb_assets.c"
                    a2 = a2.replace(old, new)
                elif which == "d":
                    assert d2.count(old) == 1, nm + ": anchor lost in orb_assets.h"
                    d2 = d2.replace(old, new)
                else:
                    assert h2.count(old) == 1, nm + ": anchor lost in the .h"
                    h2 = h2.replace(old, new)
            # A header mutant is a DRIFT, and drift is what the mirror is for: ask it
            # directly. Going through the compiler first would report a death the
            # vector set never verified, and renumbering the vocabulary in particular
            # leaves the build clean — every name still exists, every consumer still
            # compiles, and only the ORDER moved.
            if which == "d":
                try:
                    lint_asset_mirror(src_c, src_h, d2)
                    escaped_mirror = True
                except AssertionError as e:
                    escaped_mirror = False
                    print("caught: %-28s (%s)" % (nm, str(e)[:64]))
                if escaped_mirror:
                    print("MUTATION ESCAPED: %s — the mirror read the forged header "
                          "as clean" % nm)
                    bad += 1
                continue
            mtmp = os.path.join(tmp, "mut_" + nm)
            os.makedirs(mtmp, exist_ok=True)
            # A mutant that stops the build IS a killed mutant, and for the two
            # header-side ones that is the mechanism: orb_assets.c pins its ABI, its
            # count and its name width against the manifest header with a
            # _Static_assert, so a one-sided bump fails at compile time rather than at
            # run time. That is the whole point of the assert, so the loop has to count
            # it as a death instead of crashing on it — but it names the reason,
            # because a mutant killed by a stray syntax error proves nothing.
            try:
                mb = compile_harness(mtmp, c2, h2,
                                     a2 if which == "a" else None,
                                     d2 if which == "d" else None)
            except SystemExit as e:
                why = str(e)
                assert "static assertion" in why or "_Static_assert" in why, \
                    nm + ": build died for a reason that is not the pin: " + why[-400:]
                print("caught: %-28s (%s)" % (nm, "compile-time assert refused it"))
                continue
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
