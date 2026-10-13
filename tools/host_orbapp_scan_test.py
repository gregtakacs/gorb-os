#!/usr/bin/env python3
"""host_orbapp_scan_test.py — the sentinel for the /sd/apps listing.

row_decide() in main/app/orb_app_scan.c is the whole decision over one
listed container, and it is PURE: no lock, no fd, no NVS. This suite LIFTS
it verbatim — the way tools/host_sd_test.py lifts theme_state(), dying by
name if the function moves — compiles it with host gcc -Werror against the
SHIPPING reader (main/app/orb_app_manifest.c compiled, not imitated), and
drives it with containers the real writer (tools/orbapp.py) packed.

What it owns that the reader suite cannot reach:
  1. THE TWO CARD FACTS. A file that ends inside its own header block, and a
     file shorter than the art it claims, are volume truths the reader has
     no way to know; the writer has no twin sentence for either. They are
     pinned here, and the strictness choices around them (longer-than-claims
     is TOLERATED; the boundary end == size is ACCEPTED) are pinned too,
     because a mutation that tightens or loosens either must not pass.
  2. VERBATIM PASSTHROUGH. A refusal that came from the reader must arrive
     in .why byte-for-byte as the writer's ValueError reads — the expected
     strings here are CAPTURED from tools/orbapp.py at run time, never
     transcribed.
  3. THE ECHO RULES. The stem survives every refusal (it is the row's
     address), and the claimed fields survive a refusal that came AFTER the
     checksum — but not one before it. A scan that shows who a container
     claimed to be when its CRC is torn is a scan that blames the wrong file.

The device-only glue (scan_entry's pread loop, the walk, the registration)
is NOT compiled here — same line host_sd_test.py draws at the pure/function
boundary; the bench card run of the next slice is what exercises it.

Run:      python3 tools/host_orbapp_scan_test.py
Mutants:  python3 tools/host_orbapp_scan_test.py --mutations
Exit 0 = every vector matched (and, with --mutations, every mutant caught).
"""
import hashlib
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SCAN_C = os.path.join(ROOT, "main", "app", "orb_app_scan.c")
SCAN_H = os.path.join(ROOT, "main", "app", "orb_app_scan.h")
MANIFEST_C = os.path.join(ROOT, "main", "app", "orb_app_manifest.c")
MANIFEST_H = os.path.join(ROOT, "main", "app", "orb_app_manifest.h")
SD_H = os.path.join(ROOT, "main", "sd", "orb_sd.h")
NET_C = os.path.join(ROOT, "main", "net", "orb_net.c")
MAIN_C = os.path.join(ROOT, "main", "main.c")

NET_OVERRIDE = None            # set only by caller_mutation_fails(): a mutated
                               # copy of orb_net.c, read under its own path

_spec = importlib.util.spec_from_file_location("orbapp", os.path.join(HERE, "orbapp.py"))
orbapp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(orbapp)

NL = "\n"


def read(p):
    if p == NET_C and NET_OVERRIDE is not None:
        return NET_OVERRIDE                    # caller_mutation_fails' injected copy
    if not os.path.exists(p):
        sys.exit("missing " + p + " — the lifts below have no fallback")
    return open(p, encoding="utf-8").read()


def lift(text, pattern, where):
    m = re.search(pattern, text)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is "
                 "the point: an empty lift would keep this suite green while "
                 "testing nothing." % (pattern[:60], where))
    return m.group(0)


scan_src = read(SCAN_C)
scan_hdr = read(SCAN_H)
man_src = read(MANIFEST_C)
man_hdr = read(MANIFEST_H)
sd_hdr = read(SD_H)

# The lifts. row_decide ends at the first column-0 brace; every closing brace
# inside it is indented, so the anchor cannot stop early.
f_rowdecide = lift(scan_src, r"static void row_decide\(const orb_file_rec_t \*rec[\s\S]*?\n\}", SCAN_C)
f_entry = lift(scan_hdr, r"typedef struct \{[\s\S]*?\} orbapp_scan_entry_t;", SCAN_H)

# strlcpy: glibc only grew one in 2.38, and a machine with it must not get a
# static-vs-extern conflict here. Redirected by macro, so the lifted text
# compiles the same on both libcs (host_sd_test.py's renamed-lift convention).
f_rowdecide = f_rowdecide.replace("strlcpy", "orbhost_strlcpy")

# ---- shims ------------------------------------------------------------------
SHIM_ESP_ERR = """#pragma once
typedef int esp_err_t;
#define ESP_OK                 0
#define ESP_FAIL               -1
#define ESP_ERR_NO_MEM         0x101
#define ESP_ERR_INVALID_ARG    0x102
#define ESP_ERR_INVALID_SIZE   0x103
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

# ---- harness: the lifted row_decide, fed one case at a time ------------------
# Case file (little endian): u32 n_cases, then per case:
#   u32 hdr_bytes_read, u32 file_size, u32 stem_len + stem bytes,
#   u32[ORBAPP_HDR_WORDS] header words, u8[32] digest the sha hook hands back.
# stdout per case:
#   caseN ERR FSZ APPID CAPS PERIOD SERIAL NSET ART <hex stem\0slug\0title\0why>
# hex keeps the em dashes and spaces of the reason out of the field split.

HARNESS_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_app_manifest.h"

/* the walk's record, spelled once here — the shipped row_decide sees the
 * same two fields through sd/orb_files.h (orb_file_rec_t: name[33], size) */
typedef struct { char name[33]; uint32_t size; } orb_file_rec_t;

static size_t orbhost_strlcpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    if (n) {
        for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
        dst[i] = '\0';
    }
    return strlen(src);
}

@EXT@
@ENTRY@
@ROWDECIDE@

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
static void puthex(const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}
static void puthex0(const char *s) { puthex((const unsigned char *)s, strlen(s)); }

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    F = fopen(argv[1], "rb");
    if (!F) return 2;
    const uint32_t n = rd32();
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t hb = rd32(), fsz = rd32();
        const uint32_t slen = rd32();
        char stem[80];
        if (slen >= sizeof stem) return 3;
        rdb(stem, slen); stem[slen] = '\0';
        uint32_t w[ORBAPP_HDR_WORDS];
        rdb(w, sizeof w);
        rdb(g_digest, 32);

        orb_file_rec_t rec;
        memset(&rec, 0, sizeof rec);
        memcpy(rec.name, stem, slen);
        rec.size = fsz;

        orbapp_scan_entry_t e;
        memset(&e, 0, sizeof e);
        g_hash_calls = 0;
        /* the shipping decision, called exactly as scan_entry() calls it:
         * the bytes read and FATFS's size in, one filled row out */
        row_decide(&rec, w, (size_t)hb, &e);

        printf("case%u %d %u %u %u %u %u %u %u ", i, (int)e.err, e.file_size,
               e.app_id, e.caps, e.period_ms, e.serial, e.n_settings, e.art_size);
        puthex0(e.stem); fputs("00", stdout);
        puthex0(e.slug); fputs("00", stdout);
        puthex0(e.title); fputs("00", stdout);
        puthex0(e.why);
        printf(" %d %d ", g_hash_calls, g_hash_is224);
        if (g_hash_calls) puthex(g_hash_in, g_hash_len);
        printf("\n");
    }
    fclose(F);
    return 0;
}
"""


def compile_harness(tmp, rowdecide_text, ext_define, entry_text):
    shim = os.path.join(tmp, "shim")
    os.makedirs(os.path.join(shim, "mbedtls"), exist_ok=True)
    open(os.path.join(shim, "esp_err.h"), "w").write(SHIM_ESP_ERR)
    open(os.path.join(shim, "esp_rom_crc.h"), "w").write(SHIM_CRC)
    open(os.path.join(shim, "mbedtls", "sha224.h"), "w").write("#pragma once\n")
    open(os.path.join(shim, "mbedtls", "sha256.h"), "w").write(SHIM_SHA)
    src = os.path.join(tmp, "orb_app_manifest.c")
    hdr = os.path.join(tmp, "orb_app_manifest.h")
    open(src, "w").write(open(MANIFEST_C, encoding="utf-8").read())
    open(hdr, "w").write(open(MANIFEST_H, encoding="utf-8").read())
    harness = (HARNESS_C
               .replace("@EXT@", ext_define)
               .replace("@ENTRY@", entry_text)
               .replace("@ROWDECIDE@", rowdecide_text))
    hac = os.path.join(tmp, "harness.c")
    open(hac, "w").write(harness)
    binp = os.path.join(tmp, "harness")
    cc = subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra",
                         "-Wno-unused-function", "-Werror",
                         "-I", tmp, "-I", os.path.join(ROOT, "main"),
                         "-I", shim, "-o", binp, src, hac],
                        capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("harness build failed:" + NL + cc.stderr[-3000:])
    return binp


# ---- cases -------------------------------------------------------------------
# The golden container and the patch helpers are the reader suite's own —
# imported, not copied, so the two sentinels can never drift apart on how a
# header gets built. (host_orbapp_reader_test.py's module level does no work
# beyond path constants and loading the writer; its main() is guarded.)
sys.path.insert(0, HERE)
import host_orbapp_reader_test as R  # noqa: E402  (the shared case builder)

GOLDEN_SLUG, GOLDEN_TITLE = R.GOLDEN_SLUG, R.GOLDEN_TITLE
ERR_OK, ERR_ARG, ERR_SIZE = 0, 258, 259   # shim's ESP_OK/INVALID_ARG/INVALID_SIZE

# The two sentences this layer owns and the writer has no twin for. Hardcoded
# HERE and re-pinned to the shipped source in lint(): drift on either side is
# a red suite, not a page that shows the owner a new spelling of old news.
SHORT_WHY = ("the file ends inside its own header block — re-copy"
             " the file onto the card")


def size_why(claims, holds):
    return ("the container claims %d B of file and the card holds %d B"
            " — re-copy the file onto the card" % (claims, holds))


def python_scan_outcome(hdr, fname):
    """The writer's whole scan ladder over the same words — parse, check with
    the filename, layout. (ok, message). The size rule has no twin: it reads
    the volume, and the volume has no Python side."""
    try:
        h = orbapp.parse_header(R.pack_words(hdr))
        orbapp.check_header(h, stem=fname)
        orbapp.check_file_layout(h)
        return True, None
    except ValueError as e:
        return False, str(e)


class Row:
    """One vector. A field expectation of None = don't check, "" = must be
    empty, a value = must equal. hin is the slug whose bytes the app_id hash
    must have been computed over."""
    def __init__(self, name, hdr, stem, hdr_bytes, file_size, err, why="",
                 slug=None, title=None, app_id=None, caps=None, period=None,
                 serial=None, n_set=None, art=None, hcalls=1, hin=GOLDEN_SLUG,
                 py=("ok", None), note=""):
        self.__dict__.update(name=name, hdr=hdr, stem=stem, hdr_bytes=hdr_bytes,
                             file_size=file_size, err=err, why=why, slug=slug,
                             title=title, app_id=app_id, caps=caps,
                             period=period, serial=serial, n_set=n_set,
                             art=art, hcalls=hcalls, hin=hin, py=py, note=note)


def _end_of(hdr):
    return hdr[orbapp.W_ART_OFF] + hdr[orbapp.W_ART_SIZE]


def _golden(**kw):
    return R.build_hdr(**kw)[0]




def cases_all():
    V = []
    g = _golden()
    HB = len(g) * 4                      # the writer's own block size, not a copy
    aid = orbapp.app_id_of(GOLDEN_SLUG)
    FN = GOLDEN_SLUG + orbapp.EXT        # the filename row_decide rebuilds

    common = dict(slug=GOLDEN_SLUG, title=GOLDEN_TITLE, app_id=aid, caps=0x10,
                  period=3000, serial=0, n_set=4, art=8)
    V.append(Row("golden_junk_tail", g, GOLDEN_SLUG, HB, _end_of(g) + 512,
                 ERR_OK, **common,
                 note="trailing bytes are junk, not damage: TOLERATED by ruling"))
    g_b = _golden()
    V.append(Row("golden_exact_boundary", g_b, GOLDEN_SLUG, HB, _end_of(g_b),
                 ERR_OK, **common,
                 note="claims == holds is the > not >= cliff: an install must "
                      "be allowed to fit exactly"))
    g_v = _golden(art_size=0, geom_size=0, settings=[], counts=(0, 0, 0, 0))
    V.append(Row("golden_vector_only", g_v, GOLDEN_SLUG, HB, _end_of(g_v),
                 ERR_OK, slug=GOLDEN_SLUG, title=GOLDEN_TITLE, app_id=aid,
                 caps=0x10, period=3000, serial=0, n_set=0, art=0,
                 note="the color-picker shape: claims no payload past art_off"))
    gf = _golden(caps=0xF0)
    V.append(Row("caps_all_four_echoed", gf, GOLDEN_SLUG, HB, _end_of(gf),
                 ERR_OK, caps=0xF0, slug=GOLDEN_SLUG, title=GOLDEN_TITLE,
                 app_id=aid, period=3000, serial=0, n_set=4, art=8,
                 note="the disclosure line's source bits ride out unchanged"))

    # ---- the two card facts (this layer's own refusals) ----------------------
    g1 = _golden()
    V.append(Row("short_header_block", g1, GOLDEN_SLUG, HB - 4, _end_of(g1),
                 ERR_SIZE, why=SHORT_WHY, hcalls=0, py=("none", None),
                 note="one byte short of the block and the reader never speaks"))
    g2 = _golden()
    V.append(Row("zero_bytes_read", g2, GOLDEN_SLUG, 0, _end_of(g2),
                 ERR_SIZE, why=SHORT_WHY, hcalls=0, py=("none", None)))
    g3 = _golden()
    V.append(Row("file_short_by_one", g3, GOLDEN_SLUG, HB, _end_of(g3) - 1,
                 ERR_SIZE, why=size_why(_end_of(g3), _end_of(g3) - 1),
                 slug=GOLDEN_SLUG, title=GOLDEN_TITLE, app_id=aid, caps=0x10,
                 period=3000, serial=0, n_set=4, art=8, py=("none", None),
                 note="the half-copy D9 prices; named in bytes"))
    g4 = _golden()
    V.append(Row("file_grossly_short", g4, GOLDEN_SLUG, HB, 4,
                 ERR_SIZE, why=size_why(_end_of(g4), 4),
                 slug=GOLDEN_SLUG, title=GOLDEN_TITLE, app_id=aid, caps=0x10,
                 period=3000, serial=0, n_set=4, art=8, py=("none", None)))

    # ---- reader refusals arrive verbatim, with the echo rules ----------------
    mt = _golden()
    mt[orbapp.W_MAGIC] = 0x474F5254                    # an .orbtheme on the card
    mt[orbapp.W_HDR_CRC] = orbapp._hdr_crc(mt)
    ok, msg = python_scan_outcome(mt, FN)
    V.append(Row("theme_named_not_shrugged", mt, GOLDEN_SLUG, HB, _end_of(mt),
                 ERR_ARG, why=msg, hcalls=0, py=("refuse", msg)))

    tc = _golden()
    tc[orbapp.W_SERIAL] += 1                           # NO re-checksum
    ok, msg = python_scan_outcome(tc, FN)
    V.append(Row("torn_crc_blanks_the_echo", tc, GOLDEN_SLUG, HB, _end_of(tc),
                 ERR_ARG, why=msg, hcalls=0, py=("refuse", msg),
                 note="fields stay BLANK: a row may not name an app its own "
                      "header cannot vouch for"))

    sm = _golden()
    ok, msg = python_scan_outcome(sm, "weather2" + orbapp.EXT)
    V.append(Row("stem_misfiled", sm, "weather2", HB, _end_of(sm),
                 ERR_ARG, why=msg, slug=GOLDEN_SLUG, title=GOLDEN_TITLE,
                 app_id=aid, caps=0x10, period=3000, serial=0, n_set=4, art=8,
                 py=("refuse", msg),
                 note="filed as another app's slug: refused, and the row still "
                      "shows who it claimed to be — the post-CRC echo rule"))

    ac = R.patch(_golden(), orbapp.W_CAPS, 0x1)        # free bits: pack-time
    ok, msg = python_scan_outcome(ac, FN)              # would refuse them
    V.append(Row("caps_free_bit_time", ac, GOLDEN_SLUG, HB, _end_of(ac),
                 ERR_ARG, why=msg, slug=GOLDEN_SLUG, title=GOLDEN_TITLE,
                 app_id=aid, caps=0x1, period=3000, serial=0, n_set=4, art=8,
                 hcalls=0, py=("refuse", msg)))

    te = _golden()
    for i in range(orbapp.W_TITLE, orbapp.W_TITLE + 8):
        te[i] = 0
    te[orbapp.W_HDR_CRC] = orbapp._hdr_crc(te)
    ok, msg = python_scan_outcome(te, FN)
    V.append(Row("title_empty_refused_slug_kept", te, GOLDEN_SLUG, HB,
                 _end_of(te), ERR_ARG, why=msg, slug=GOLDEN_SLUG, title="",
                 app_id=aid, caps=0x10, period=3000, serial=0, n_set=4, art=8,
                 hcalls=0, py=("refuse", msg)))

    ai = R.patch(_golden(), orbapp.W_APP_ID, 0xDEADBEEF)
    ok, msg = python_scan_outcome(ai, FN)
    V.append(Row("app_id_not_the_slug", ai, GOLDEN_SLUG, HB, _end_of(ai),
                 ERR_ARG, why=msg, slug=GOLDEN_SLUG, title=GOLDEN_TITLE,
                 app_id=0xDEADBEEF, caps=0x10, period=3000, serial=0, n_set=4,
                 art=8, py=("refuse", msg),
                 note="the claimed id echoes so the owner can name the file; "
                      "nothing downstream may trust the echoed id"))

    lo = R.patch(_golden(), orbapp.W_SET_OFF, 999)
    ok, msg = python_scan_outcome(lo, FN)
    V.append(Row("settings_table_moved", lo, GOLDEN_SLUG, HB, _end_of(lo),
                 ERR_ARG, why=msg, slug=GOLDEN_SLUG, title=GOLDEN_TITLE,
                 app_id=aid, caps=0x10, period=3000, serial=0, n_set=4, art=8,
                 py=("refuse", msg)))

    pn = _golden()
    ok, msg = python_scan_outcome(pn, "weather.part" + orbapp.EXT)
    V.append(Row("staging_name_refused", pn, "weather.part", HB, _end_of(pn),
                 ERR_ARG, why=msg, slug=GOLDEN_SLUG, title=GOLDEN_TITLE,
                 app_id=aid, caps=0x10, period=3000, serial=0, n_set=4, art=8,
                 py=("refuse", msg),
                 note="an upload mid-flight is invisible to the walk; named "
                      "for what it claims if a caller ever conjures one"))
    return V

# ---- run + compare -------------------------------------------------------------
def run_harness(binp, cases):
    blob = [struct.pack("<I", len(cases))]
    for c in cases:
        stem = c.stem.encode("utf-8")
        blob.append(struct.pack("<II", c.hdr_bytes, c.file_size))
        blob.append(struct.pack("<I", len(stem)))
        blob.append(stem)
        blob.append(b"".join(struct.pack("<I", v) for v in c.hdr))
        blob.append(hashlib.sha256(c.hin.encode("utf-8")).digest())
    path = binp + ".cases"
    open(path, "wb").write(b"".join(blob))
    r = subprocess.run([binp, path], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("harness exited %d" % r.returncode)
    out = {}
    for ln in r.stdout.splitlines():
        out[ln.split(" ", 1)[0]] = ln
    return out


def compare(cases, out):
    fails = []
    for i, c in enumerate(cases):
        ln = out.get("case%u" % i)
        if ln is None:
            fails.append("%s: the harness printed no row" % c.name)
            continue
        t = ln.split(" ")
        got = dict(err=int(t[1]), file_size=int(t[2]), app_id=int(t[3]),
                   caps=int(t[4]), period=int(t[5]), serial=int(t[6]),
                   n_set=int(t[7]), art=int(t[8]))
        fields = bytes.fromhex(t[9]).split(b"\0")
        if len(fields) != 4:
            fails.append("%s: row strings torn: %r" % (c.name, fields))
            continue
        stem, slug, title, why = (f.decode("utf-8", "replace") for f in fields)

        def eq(what, want, have):
            if want is not None and have != want:
                fails.append("%s.%s: want %r got %r" % (c.name, what, want, have))

        # the stem echoes on EVERY row — refused rows keep their address
        eq("stem", c.stem, stem)
        eq("file_size", c.file_size, got["file_size"])
        eq("err", c.err, got["err"])
        eq("why", c.why, why)
        eq("slug", c.slug, slug)
        eq("title", c.title, title)
        eq("app_id", c.app_id, got["app_id"])
        eq("caps", c.caps, got["caps"])
        eq("period", c.period, got["period"])
        eq("serial", c.serial, got["serial"])
        eq("n_settings", c.n_set, got["n_set"])
        eq("art_size", c.art, got["art"])

        hcalls = int(t[10]) if len(t) > 10 else -1
        is224 = int(t[11]) if len(t) > 11 else -1
        hexin = t[12] if len(t) > 12 else None
        if hcalls != c.hcalls:
            fails.append("%s: hash calls %d want %d" % (c.name, hcalls, c.hcalls))
        if hcalls and (is224 != 0 or hexin != c.hin.encode("utf-8").hex()):
            fails.append("%s: app_id hash drifted (is224=%d in=%r)"
                         % (c.name, is224, hexin))

        kind, pymsg = c.py
        if kind in ("ok", "refuse"):
            ok, msg = python_scan_outcome(c.hdr, c.stem + orbapp.EXT)
            if kind == "ok" and not ok:
                fails.append("%s: the writer also refuses this container — the "
                             "vector lost its meaning: %r" % (c.name, msg))
            if kind == "refuse":
                if ok:
                    fails.append("%s: the writer ACCEPTS this container — the "
                                 "mirror lost a refusal" % c.name)
                elif pymsg is not None and msg != pymsg:
                    fails.append("%s: writer's words drifted: %r vs %r"
                                 % (c.name, msg, pymsg))
                if c.why != pymsg:      # the row must show the writer's sentence
                    fails.append("%s: .why is not the writer's own words: %r vs %r"
                                 % (c.name, c.why, pymsg))
    return fails



# ---- lint: the mirror stands only while it matches both live sources ----------
def lint():
    sc, sh = read(SCAN_C), read(SCAN_H)
    for needle in ('#define ORB_APP_DIR         ORB_SD_BASE "/apps"',
                   '#define ORB_APP_EXT         ".orbapp"',
                   '#define ORB_APP_SCAN_SLUG   "orb"',
                   '#define ORB_APP_SCAN_ID     "apps"',
                   "#define ORB_APP_SCAN_MAX    16",
                   "char      why[256];"):
        assert needle in sh, "scan header drifted from the agreed shape: " + needle
    assert '#define ORB_SD_BASE        "/sd"' in read(SD_H)
    assert orbapp.EXT == ".orbapp", "the writer changed extensions on the pair"
    # the two card sentences, verbatim in the shipping file — pinned here
    # because no writer twin exists to catch them drifting (the reader suite
    # pins its hand-written reasons the same way)
    assert ('"the file ends inside its own header block — re-copy"\n'
            '                       " the file onto the card"') in sc, \
        "the short-header sentence left orb_app_scan.c"
    assert "the container claims %llu B of file and the card holds %u B" in sc, \
        "the size sentence left orb_app_scan.c"
    # the identity rule: the rebuilt FILENAME, not the bare stem, reaches
    # check_header — pass the stem and check_stem's tail rule goes dark
    assert 'snprintf(fname, sizeof fname, "%s%s", rec->name, ORB_APP_EXT)' in sc, \
        "row_decide stopped rebuilding the filename"
    assert "orbapp_check_header(&h, 0, false, fname" in sc, \
        "row_decide stopped passing the filename to the reader"
    assert "orb_files_register(ORB_APP_SCAN_SLUG" in sc
    assert "orb_files_walk(src, scan_entry" in sc
    assert "pread(fd," in sc                   # the walk's fd, never lseek

    # ---- the caller side: the scan is no longer inert --------------------------
    # The boot ladder registers the drop zone; the log line says so if it does
    # not, because a silent refusal here is an apps page that lists nothing.
    mn = read(MAIN_C)
    assert "orb_app_scan_init()" in mn, "main.c no longer registers the drop zone"
    nn = read(NET_C)
    assert '{ .uri = "/apps",              .method = HTTP_GET,  .handler = h_apps },' in nn
    assert '{ .uri = "/api/v1/apps",       .method = HTTP_GET,  .handler = h_apps_list },' in nn
    assert "static orbapp_scan_entry_t ent[ORB_APP_SCAN_MAX];" in nn
    assert "orb_app_scan(ent, ORB_APP_SCAN_MAX)" in nn, \
        "the endpoint stopped asking the scan for its rows — it is serving a copy"
    # the chunked-encoding contract: repeated httpd_resp_send is NOT the
    # documented stream; the chunk API closed with a zero-length chunk is.
    # A handler that drops the terminator hangs every client waiting for a
    # body that never ends — invisible to a vector suite, fatal on a phone.
    assert "httpd_resp_send_chunk(req, NULL, 0)" in nn, \
        "the apps stream lost its terminating chunk"
    assert "httpd_resp_sendstr(req, head)" not in nn
    # one disclosure source: the handler renders orbapp_caps_names' answer and
    # never spells a capability word itself (the names belong to the reader,
    # and the reader suite pins them to the writer). Two of the four words are
    # unmistakable in plain text; a second list would have to name them all.
    assert "orbapp_caps_names(e->caps, caps" in nn
    i0 = nn.index("static esp_err_t h_apps_list")
    body = nn[i0:nn.index("\n/* ", i0)]
    for word in ("power", "sd_read"):
        assert word not in body, \
            "orb_net.c spelled a capability word (%r) beside its one source" % word
    # the page's only source of refusal text is the row's why, verbatim.
    assert "CANNOT INSTALL: '+esc(a.why)" in nn
    # the page must actually draw what the endpoint answered — anchored to
    # this page's own refresh body, because the themes page draws with the
    # same two statements and would mask a mutation of ours.
    assert "'bad');return;}data=r.d;draw();" in nn
    # the page parses as JavaScript. The C fragments concatenate with NO
    # newline between them, so one missing semicolon is a page stuck at
    # "loading" on a phone — the exact failure the themes page documents, and
    # the same esprima gate host_tz_test.py runs on its two pages. Only
    # column-0 whole-string fragments count as page bytes: a line with a
    # stray quote would otherwise be silently dropped from the test.
    try:
        import esprima
        m = re.search(r'static const char HTML_APPS\[\] =(.*?);\s*\n', nn, re.S)
        assert m, "HTML_APPS not found in orb_net.c"
        frags = []
        for ln in m.group(1).splitlines():
            t = ln.strip()
            if not t or t.startswith('/*') or t.startswith('//'):
                continue
        # the C terminator (;") is stripped before the quotes; a JS semicolon
        # may sit inside the last quote, and dropping it would silently alter
        # the program under test.
            assert re.fullmatch(r'"(?:[^"\\]|\\.)*";?', t), \
                "page line is not a whole C string fragment: " + t[:80]
            frags.append(t.rstrip(';')[1:-1])
        page = ''.join(f.encode('utf-8').decode('unicode_escape') for f in frags)
        js = page[page.index('<script>') + 8:page.index('</script>')]
        esprima.parseScript(js)
    except ImportError:
        pass                             # same rule as host_tz_test: no esprima, no parse
    # the row bound: recompute the handler's worst-case row from the sizes the
    # scan header and the manifest header actually ship, against the scratch it
    # actually allocates. Every field is at its double-escaped worst; the
    # skeleton counts keys, quotes, the longest "ok", digits, and the comma.
    m = re.search(r"e_stem\[2 \* sizeof ent\[0\]\.stem\], e_slug\[2 \* sizeof ent\[0\]\.slug\],"
                  r"\s*\n\s*e_title\[2 \* sizeof ent\[0\]\.title\], e_why\[2 \* sizeof ent\[0\]\.why\]", nn)
    assert m, "the handler's escape buffers left their pinned shape"
    cap_scratch = int(re.search(r"#define APPS_ROW_MAX (\d+)", nn).group(1))
    stem_n = int(re.search(r"stem\[(\d+)\]", scan_hdr).group(1))
    why_n = int(re.search(r"why\[(\d+)\]", scan_hdr).group(1))
    slug_n = int(re.search(r"ORBAPP_SLUG_MAX\s+(\d+)u", man_hdr).group(1))
    title_n = int(re.search(r"ORBAPP_TITLE_MAX\s+(\d+)u", man_hdr).group(1))
    worst = (2 * (stem_n + slug_n + title_n + why_n)      # all four escaped ×2
             + 40 + 8 + 5 * 10 + 12 + 160 + 2)            # caps, hex, digits, ok, skeleton
    assert worst < cap_scratch, \
        ("the worst-case apps row is %d B against an APPS_ROW_MAX of %d — "
         "enlarge the scratch or trim a field" % (worst, cap_scratch))


# ---- mutations that target the caller side ------------------------------------
# These red-pilot the bound check above: shrink the scratch, and the suite must
# go red on the arithmetic (the harness itself never recompiles — the shipped
# handler is Python-parsed here, so these mutate the TEXT the lint reads).
CALLER_MUTATIONS = [
    ("scratch lowered", "#define APPS_ROW_MAX 1024", "#define APPS_ROW_MAX 900"),
    ("scan call dropped", "orb_app_scan(ent, ORB_APP_SCAN_MAX)", "orb_app_scan(ent, 4)"),
    ("second caps list appears", "\"caps\\\":\\\"%s\\\"",
     "\"caps\\\":\\\"power\\\",\\\"caps2\\\":\\\"%s\\\""),
    ("page re-cuts the reason", "CANNOT INSTALL: '+esc(a.why)",
     "CANNOT INSTALL: 'see the log'"),
    ("page stops drawing", "'bad');return;}data=r.d;draw();", "'bad');return;}data=r.d;"),
    ("terminating chunk dropped", "httpd_resp_send_chunk(req, NULL, 0);", "0;"),
]


def caller_mutation_fails():
    """Run the lint against a mutated COPY of orb_net.c; return the names that
    turned it red. A mutation nothing notices is a pin that is not a pin."""
    reds = []
    for nm, old, new in CALLER_MUTATIONS:
        base = read(NET_C)
        assert base.count(old) == 1, "caller anchor lost: " + nm
        mutated = base.replace(old, new)
        global NET_OVERRIDE
        NET_OVERRIDE = mutated
        try:
            lint()
        except (AssertionError, AttributeError, ValueError):
            reds.append(nm)
        finally:
            NET_OVERRIDE = None
    return reds



# ---- mutations ------------------------------------------------------------------
# Each removal must cost the suite a vector. The first four are this layer's
# own strictness choices (the two card facts, the tolerance, the boundary);
# the last three are the shown-with-reason rules the theme scan bequeathed.
MUTATIONS = [
    ("short_header_guard_dropped",
     [("if (hdr_bytes < ORBAPP_HDR_BYTES) {",
       "if (0 && hdr_bytes < ORBAPP_HDR_BYTES) {")]),
    ("size_guard_dropped",
     [("if (end > (uint64_t)rec->size) {",
       "if (0 && end > (uint64_t)rec->size) {")]),
    ("size_boundary_tightened",
     [("if (end > (uint64_t)rec->size) {",
       "if (end >= (uint64_t)rec->size) {")]),
    ("junk_tail_rejected",
     [("if (end > (uint64_t)rec->size) {",
       "if (end != (uint64_t)rec->size) {")]),
    ("stem_dropped_from_triple",
     [("orbapp_check_header(&h, 0, false, fname, why, wsz);",
       "orbapp_check_header(&h, 0, false, NULL, why, wsz);")]),
    ("stem_echo_dropped",
     [('strlcpy(e->stem, rec->name, sizeof e->stem);',
       'e->stem[0] = 0; memset(e->stem, 0, sizeof e->stem);')]),
    # echo-before-trust, from the side that survives: the reader verifies the
    # CRC before it fills a single field, so an echo hoisted above parse copies
    # empties (that mutant is equivalence — verified, and dropped). The rule
    # worth a vector is the other half: a row that dies AFTER the crc must
    # KEEP its claimed name. Wiping the echo on a check-stage refusal is what
    # the page would silently render as an unnamed file.
    ("echo_wiped_on_check_refusal",
     [("r = orbapp_check_header(&h, 0, false, fname, why, wsz);\n"
       "    if (r) { e->err = ESP_ERR_INVALID_ARG; return; }",
       "r = orbapp_check_header(&h, 0, false, fname, why, wsz);\n"
       "    if (r) { e->slug[0] = 0; e->title[0] = 0;\n"
       "             e->err = ESP_ERR_INVALID_ARG; return; }")]),
]


def mutated_rowdecide(pairs):
    src = scan_src
    for old, new in pairs:
        assert src.count(old) == 1, "mutation anchor lost in orb_app_scan.c: " + old[:40]
        src = src.replace(old, new)
    f = lift(src, r"static void row_decide\(const orb_file_rec_t \*rec[\s\S]*?\n\}",
             "<mutant>")
    return f.replace("strlcpy", "orbhost_strlcpy")


def main():
    cases = cases_all()
    lint()
    ext = lift(scan_hdr, r'#define ORB_APP_EXT\s+"\.orbapp"', SCAN_H)
    with tempfile.TemporaryDirectory() as tmp:
        binp = compile_harness(tmp, f_rowdecide, ext, f_entry)
        fails = compare(cases, run_harness(binp, cases))
        n = 0
        if "--mutations" in sys.argv:
            for nm, pairs in MUTATIONS:
                mb = compile_harness(tmp, mutated_rowdecide(pairs), ext, f_entry)
                mout = run_harness(mb, cases)
                mfails = compare(cases, mout)
                if not mfails:
                    print("MUTATION ESCAPED: " + nm)
                    fails.append("mutant escaped: " + nm)
                else:
                    print("caught: %-30s (%d mismatches)" % (nm, len(mfails)))
                    n += 1
            # the caller-side battery: text mutations of orb_net.c checked
            # against lint(), the same "a survivor is an unpinned claim" rule
            # the compiled mutants follow.
            ceds = caller_mutation_fails()
            for nm, _, _ in CALLER_MUTATIONS:
                if nm not in ceds:
                    print("MUTATION ESCAPED (caller): " + nm)
                    fails.append("caller mutant escaped: " + nm)
                else:
                    print("caught: %-30s (lint red)" % nm)
                n += 1 if nm in ceds else 0
    for f in fails:
        print("FAIL " + f)
    print("%d vectors, %d mismatches" % (len(cases), len(fails)))
    if "--mutations" in sys.argv:
        print("%d mutants, %d escaped"
              % (len(MUTATIONS) + len(CALLER_MUTATIONS),
                 len(MUTATIONS) + len(CALLER_MUTATIONS) - n))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
