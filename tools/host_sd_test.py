#!/usr/bin/env python3
"""Bench check for the microSD + FORMAT 3 theme-store feature.

    python3 tools/host_sd_test.py               # all checks
    python3 tools/host_sd_test.py --mutations   # do the checks actually bite?

The FORMAT 2 reader and its SD install/upload door retired with the compiled
pair (slice 4). FORMAT 3 app bundles are the only thing this firmware reads,
through one door: orb_theme_install_bundle(), reached only off
orb_theme_post_bundle(). What a bench with no card and no radio can still pin is
the surface that does not need either — checked here against the shipped source
rather than asserted in prose:

1. PATH SAFETY. orb_sd_name_ok()/orb_sd_theme_path()/theme_file() decide whether a
   string that arrived over the radio becomes a path on the card. The store and
   delete endpoints hand them only a ?id= — 12 chars the device issued itself,
   matched against sha256() of the bundle's own header name before any path is
   built. Pure functions, lifted and run below.

2. THE FORMAT 3 HEADER, AGAINST ITS OWN WRITER. main/art/orb_bundle.c reads byte
   for byte what tools/orbbundle.py writes — two implementations in two languages
   with no compiler between them. The harness lifts orb_bundle3_hdr_parse()
   verbatim and feeds it headers the writer packs, so a field reordered on ONE
   side goes red here and not as a dial that draws the wrong art.

3. THE /themes PAGE, parsed the way the browser will. One unbalanced paren in the
   embedded C fragments kills the whole <script> and the page sits at "loading".

4. THE CARD IDENTITY. theme_id()/theme_id_ok() are the one definition of what a
   card file is called; the store door, the walk and the list all apply it. A
   mistake is invisible until a row offers a Store button that answers with a
   refusal naming nothing. Lifted and run, and the golden FORMAT 3 pair under
   tests/fixtures/ is read back through the writer as a door-accept vector.

The lifts fail LOUDLY if the code they extract has moved: a lift that silently
came back empty would keep reporting green while testing nothing.

Needs esprima for the page check (pip install esprima); a missing parser is a
loud SKIP on that family, never a pass.
"""
import hashlib, json, os, re, struct, subprocess, sys, zlib

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
       else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)
SD_C, SD_H = P("main", "sd", "orb_sd.c"),   P("main", "sd", "orb_sd.h")
TH_C, TH_H = P("main", "art", "orb_theme.c"), P("main", "art", "orb_theme.h")
B3_C, B3_H = P("main", "art", "orb_bundle.c"), P("main", "art", "orb_bundle.h")
NET_C      = P("main", "net", "orb_net.c")
GEN_H      = P("tests", "fixtures", "avkit-pair.h")
GEN_B      = P("tests", "fixtures", "avkit-pair.bin")
for p in (SD_C, SD_H, TH_C, TH_H, B3_C, B3_H, NET_C, GEN_H, GEN_B):
    if not os.path.exists(p):
        sys.exit("missing " + p + " — the lifts below have no fallback")

sd_src, th_src = open(SD_C).read(), open(TH_C).read()
th_hdr, sd_src_h = open(TH_H).read(), open(SD_H).read()
net_src, gen_src = open(NET_C).read(), open(GEN_H).read()
gen_bin = open(GEN_B, "rb").read()
if "--src" in sys.argv:                      # mutation runs re-point the NET lifts
    net_src = open(sys.argv[sys.argv.index("--src") + 1]).read()

sys.path.insert(0, P("tools"))
import orbtheme
import orbbundle as ob3
import theme_schema as ts


def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is the "
                 "point: an empty lift would keep this suite green while testing a "
                 "copy that no longer ships." % (pattern, where))
    return m.group(0)


def _must(rx, src, what):
    m = re.search(rx, src)
    if not m:
        sys.exit("the test can no longer read %s out of the shipped source: %s" % (what, rx))
    return m.group(1)


# ---------------------------------------------------------------------------
# The lifts, verbatim from what compiles into the device.
# ---------------------------------------------------------------------------
sd_consts = "\n".join(lift(sd_src_h, r"#define " + k + r"\b[^\n]*", SD_H) for k in
                     ("ORB_SD_BASE", "ORB_SD_THEME_DIR", "ORB_SD_NAME_MAX", "ORB_SD_PATH_MAX"))
f_name_ok  = lift(sd_src, r"bool orb_sd_name_ok\(const char \*name\)[\s\S]*?\n\}", SD_C)
f_theme_pt = lift(sd_src, r"bool orb_sd_theme_path\(char \*out[\s\S]*?\n\}", SD_C)
# The theme layer's own destination builder — the last place the ".orbtheme" suffix
# is added. Tested rather than trusted: a client that could spell it would control
# the extension and, through it, what the scanner on the next page considers a theme.
f_theme_file = lift(th_src, r"static bool theme_file\(const char \*stem[\s\S]*?\n\}", TH_C)
ext_define = lift(th_hdr, r"#define ORB_THEME_EXT\b[^\n]*", TH_H)
id_hex     = lift(th_src, r"#define THEME_ID_HEX\b[^\n]*", TH_C)
orb_thdr   = lift(th_src, r"typedef struct \{[\s\S]*?\} orb_thdr_t;", TH_C)
name_max   = lift(th_src, r"#define THEME_NAME_MAX\b[^\n]*", TH_C)

# clk_theme_t / clk_sprite_t / clk_bg_rect_t and CLK_SCR_W/H, straight off the
# committed golden fixture header — not restated, because a restatement could pass
# while the device compiles against different types.
_i0 = gen_src.index("#define CLK_SCR_W")
gen_types = gen_src[_i0:gen_src.index("} clk_bg_rect_t;") + len("} clk_bg_rect_t;")]

# The §6 installed-slot comparison, lifted: it decides which row the page calls
# "on the dial" and which one it offers to re-store.
f_theme_state = lift(th_src, r"static void theme_state\(const orb_thdr_t \*h,[\s\S]*?\n\}", TH_C)
# The card identity: one hasher, one validator. Lifted so the vectors run the shipped
# hex loop and the shipped [0-9a-f] bounds rather than a Python copy of them.
f_theme_id   = lift(th_src, r"static void theme_id\(const char \*name, char out\[THEME_ID_HEX \+ 1\]\)[\s\S]*?\n\}", TH_C)
f_theme_idok = lift(th_src, r"static bool theme_id_ok\(const char \*id\)[\s\S]*?\n\}", TH_C)
for _s in ("f_name_ok", "f_theme_pt", "f_theme_file", "f_theme_state"):
    globals()[_s] = globals()[_s].replace("strlcpy", "host_strlcpy").replace("strlcat", "host_strlcat")

# The FORMAT 3 header reader, lifted standalone from orb_bundle.c. It needs its own
# word map (the .h's comment names orb_theme.c's as the layout the two share), the
# B3 magic/format/abi constants, the caps it refuses over, the b32/reject helpers and
# the typedef — nothing else. That is the whole unit under test, so the accept signal
# is the shipped reader's NULL return, and the refusals are its own sentences.
b3_enum = lift(b3_src := open(B3_C).read(),
               r"enum \{\s*W_MAGIC = 0[\s\S]*?W_WORDS = 32\s*\};", B3_C)
b3_magic = lift(b3_src, r"#define B3_MAGIC\b[^\n]*", B3_C)
b3_b32   = lift(b3_src, r"static uint32_t b32\(const uint8_t \*p\)[\s\S]*?\n\}", B3_C)
b3_rej   = lift(b3_src, r"static char s_msg\[160\];[\s\S]*?\n\}", B3_C)
b3_typedef = lift(open(B3_H).read(),
                  r"typedef struct \{\s*char\s+name\[57\];[\s\S]*?\} orb_bundle3_hdr_t;", B3_H)
b3_defines = lift(open(B3_H).read(),
                  r"#define ORB_BUNDLE3_FORMAT[\s\S]*?#define ORB_BUNDLE_ART_CAP[\s\S]*?\*/", B3_H)
B3_READER = ("#include <stdarg.h>\n" + b3_enum + "\n" + b3_magic + "\n"
             + b3_defines + "\n" + b3_typedef + "\n" + b3_b32 + "\n" + b3_rej + "\n"
             + lift(b3_src, r"const char \*orb_bundle3_hdr_parse\(const uint32_t \*w, orb_bundle3_hdr_t \*o\)\n\{[\s\S]*?\n\}", B3_C)
             + "\n")



CRC_STUB = r'''
/* esp_rom_crc32_le is a ROM routine. This host copy is only trustworthy because of
 * the known-answer row in main(): if it and the silicon ever disagree, every real
 * bundle reads as 'damaged' on the device and green here. */
static uint32_t host_crc32(uint32_t crc, const uint8_t *b, uint32_t n)
{
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}
#define esp_rom_crc32_le host_crc32
/* theme_id() asks mbedtls for the SHA-256 of the name. The device uses the ROM
 * implementation; this host build substitutes a HOOK that RECORDS the exact bytes and
 * length the shipped theme_id() handed to the hash and returns the digest Python
 * computed for those same bytes — so the vectors pin WHICH BYTES reach the hash, not
 * just the twelve characters that come back. */
static const unsigned char *s_hash_in;
static size_t               s_hash_len;
static const unsigned char *s_hash_want;
static int                  s_hash_is224;
static int host_sha256_hook(const unsigned char *in, size_t ilen,
                           unsigned char out[32], int is224)
{
    s_hash_in = in; s_hash_len = ilen; s_hash_is224 = is224;
    if (!s_hash_want) return -1;
    memcpy(out, s_hash_want, 32);
    return 0;
}
#define mbedtls_sha256 host_sha256_hook
'''

# The lifted helpers the shipped code calls. Renamed rather than link-interposed:
# glibc's strlcpy appeared only in 2.38, so a definition here would collide on a new
# distro and be missing on an old one.
HELPERS = r'''
static size_t host_strlcpy(char *dst, const char *src, size_t siz)
{
    char *d = dst; const char *s = src; size_t n = siz;
    if (n) { while (--n && *s) *d++ = *s++; *d = '\0'; }
    while (*s) s++;
    return (size_t)(s - src);
}
static size_t host_strlcat(char *dst, const char *src, size_t siz)
{
    size_t dl = strlen(dst), sl = strlen(src);
    if (dl + 1 >= siz) return siz + sl;
    memcpy(dst + dl, src, sl + 1);
    return dl + sl;
}
'''

CKV = (r'#define CKV(got,want) do { long long g_ = (long long)(got); '
       r'if (g_ != (long long)(want)) { fails++; '
       r'printf("FAIL %s got %lld want %lld  <== the two sides disagree\n", '
       r'#got, g_, (long long)(want)); } } while (0)' + "\n")

_NAME_CAP = int(_must(r"#define ORB_SD_NAME_MAX\s+(\d+)", sd_consts, "ORB_SD_NAME_MAX"))
_sd_base   = _must(r'#define ORB_SD_BASE\s+"([^"]*)"', sd_consts, "ORB_SD_BASE")
_theme_dir = _sd_base + _must(r'#define ORB_SD_THEME_DIR\s+ORB_SD_BASE\s+"([^"]*)"',
                              sd_consts, "ORB_SD_THEME_DIR")
_theme_ext = _must(r'"([^"]*)"', ext_define, "ORB_THEME_EXT")
_long_stem = "0" * _NAME_CAP
_long_path = _theme_dir + "/" + _long_stem + _theme_ext

NAME_ROWS = r'''    ncase("orbital-arrow-3", 1);
    ncase("theme_2.orbtheme", 1);
    ncase("A9.-_z", 1);
    ncase("@CAP31@", 1);                              /* exactly the cap */
    ncase("@CAP32@", 0);                              /* one over it */
    ncase("", 0);
    ncase(".", 0);
    ncase("..", 0);
    ncase(".hidden", 0);                             /* would list as no theme */
    ncase("../x", 0);
    ncase("a/../b", 0);
    ncase("a/b", 0);
    ncase("a\\b", 0);
    ncase("a b", 0);
    ncase("caf\xe9", 0);                             /* UTF-8: refused, not mangled */
    ncase(NULL, 0);'''.replace("@CAP31@", "0" * _NAME_CAP).replace("@CAP32@", "0" * (_NAME_CAP + 1))

PATH_ROWS = '''    pcase("kit3", 1, "%s");
    pcase("a.b-c_1", 1, "%s");
    pcase("../etc", 0, 0);
    pcase("a/b", 0, 0);
    pcase("", 0, 0);
    pcase(NULL, 0, 0);''' % (_theme_dir + "/kit3", _theme_dir + "/a.b-c_1")

# theme_file() is the last thing between a card-side stem and a path on the card, and
# the only place the extension is added, so its boundary is measured rather than
# assumed: the size that must fit exactly, and one byte less, which must be refused
# instead of writing a truncated stem that points at a different file.
FILE_ROWS = '''    fcase("kit3", 1, "%s", %d);
    fcase("a.b-c_1", 1, "%s", %d);
    fcase("%s", 1, "%s", %d);                                /* the cap, exact fit */
    fcase("%s", 0, 0, %d);                                    /* one byte short */
    fcase("x" ORB_THEME_EXT, 1, "%s", sizeof fbuf);           /* doubled on purpose */
    fcase("../etc", 0, 0, sizeof fbuf);
    fcase("a/b", 0, 0, sizeof fbuf);
    fcase("", 0, 0, sizeof fbuf);
    fcase(NULL, 0, 0, sizeof fbuf);''' % (
    _theme_dir + "/kit3" + _theme_ext, len(_theme_dir) + 5 + len(_theme_ext) + 1,
    _theme_dir + "/a.b-c_1" + _theme_ext, len(_theme_dir) + 8 + len(_theme_ext) + 1,
    _long_stem, _long_path, len(_long_path) + 1,
    _long_stem, len(_long_path),
    _theme_dir + "/x" + _theme_ext + _theme_ext)



# ---------------------------------------------------------------------------
# The FORMAT 3 door vectors: the writer's own header words, fed to the lifted
# reader, plus the refusals it owes. Built from orbbundle.pack_header3 so no word
# here is transcribed by eye. art_size is the fixture .bin's own byte count, so the
# accept row is the door the store opens for THIS theme, not a made-up bundle.
# ---------------------------------------------------------------------------
def _recrc(ws):
    ws = list(ws); ws[orbtheme.W_HDR_CRC] = 0
    ws[orbtheme.W_HDR_CRC] = orbtheme._hdr_crc(ws)
    return ws


def _hdr3(name="av-kit", serial=7, art_size=None):
    geom = 4096
    art = len(gen_bin) if art_size is None else art_size
    raw = ob3.pack_header3(name, 1, geom, 0xAABBCCDD, 8192, art, 0x11223344,
                           serial=serial)
    return list(struct.unpack("<32I", raw))


ART_F3   = len(gen_bin)
_GEOM3   = 4096
_H3_OK   = _hdr3()
_H3_TORN = list(_H3_OK); _H3_TORN[orbtheme.W_HDR_CRC] ^= 0x55AA   # stale checksum
_H3_ABI  = _recrc([w if i != 2 else 99 for i, w in enumerate(_H3_OK)])
_H3_BIG  = _recrc([w if i != orbtheme.W_ART_SIZE else 0x3F0001
                   for i, w in enumerate(_H3_OK)])
_H3_NAME = _recrc([0 if 14 <= i < 28 else w for i, w in enumerate(_H3_OK)])
# A real FORMAT 2 container header: same magic, same header shape, its own checksum
# honestly stamped — so the ONLY reason the door refuses it is the format word. This is
# the vector the P6 pass line asks for ("a FORMAT 2 file is refused at the format gate
# with no fallback reader"); the format check runs before the self-checksum, so a
# well-formed FORMAT 2 header hears the format sentence and not the damaged one.
_H3_FMT2 = _recrc([orbtheme.FORMAT if i == orbtheme.W_FORMAT else w
                   for i, w in enumerate(_H3_OK)])
# The one sentence the reader returns verbatim (not through reject's vsnprintf), and
# the two the caller sees for an oversized art region and a torn header. Pinned to the
# shipped strings, which the accept/refusal rows below re-check against.
_H3_FMT_SENT  = "theme format this firmware cannot read"
_H3_TORN_SENT = "container header is damaged — re-copy the file onto the card"
_H3_NONAME_SENT = "theme has no name in its header — re-bake it"
_H3_ABISIZE_SENT = "bundle geometry v99; this build reads v3 — re-bake it"
_H3_BIG_SENT = ("pixel payload is %d B; a theme slot holds %d B — theme is larger "
                "than a slot" % (0x3F0001, 0x3F0000))


def arr(name, words):
    body = ",".join(str(w & 0xFFFFFFFF) for w in words)
    return "static const uint32_t %s[%d] = {%s};\n" % (name, len(words), body)


B3_ROWS = "\n".join([
    arr("h3ok", _H3_OK),
    arr("h3torn", _H3_TORN),
    arr("h3abi", _H3_ABI),
    arr("h3big", _H3_BIG),
    arr("h3noname", _H3_NAME),
    arr("h3fmt2", _H3_FMT2),
    '    r3why("FORMAT 2 at the door: refused at the format gate, no fallback reader", '
    '"%s", h3fmt2);' % _H3_FMT_SENT,
    '    r3why("torn header: the self-checksum, not the name, is the first word", '
    '"%s", h3torn);' % _H3_TORN_SENT,
    '    r3why("unknown abi: the reader names the version it wants", '
    '"%s", h3abi);' % _H3_ABISIZE_SENT,
    '    r3why("art over the slot budget", "%s", h3big);' % _H3_BIG_SENT,
    '    r3why("nameless bundle", "%s", h3noname);' % _H3_NONAME_SENT,
    '    r3fields("the writer\'s own golden header parses whole and fills every field", '
    'h3ok, "av-kit", 1, 7, 0x11223344u, 0xAABBCCDDu, 8192u, %du);' % _GEOM3,
])

# The §6 pair, run against the lifted theme_state: name + the two checksums the store
# door keeps. Each row names the mistake the other way round.
_STATE_ROWS = []
for _nm, _g, _a, _wn, _wg, _wa, _act, _chg, _why in [
        ("Wall",    111, 222, "Wall",  111, 222, 1, 0, "the file the dial draws is the one on the card"),
        ("Wall",    111, 999, "Wall",  111, 222, 0, 1, "same name, pixels changed on the card since store"),
        ("Wall",    333, 222, "Wall",  111, 222, 0, 1, "same name, geometry changed on the card since store"),
        ("Wall",    333, 999, "Wall",  111, 222, 0, 1, "both changed since it was stored"),
        ("Another", 111, 222, "Wall",  111, 222, 0, 0, "a byte-identical file for a DIFFERENT name is neither"),
        ("Another", 333, 999, "Wall",  111, 222, 0, 0, "another theme's file is neither, whatever its bytes"),
        ("Wall",    111, 222, "Neon",  111, 222, 0, 0, "nothing is the wall's while the wall holds another name"),
]:
    _STATE_ROWS.append('    scase("%s", %du, %du, "%s", %du, %du, %d, %d, "%s");'
                       % (_nm, _g, _a, _wn, _wg, _wa, _act, _chg, _why))
_STATE_ROWS.append('    scase_none("a board that has never stored a theme");')
STATE_ROWS = "\n".join(_STATE_ROWS)

# The card identity rows: every name, hashed by the shipped theme_id through the
# recording hook, must (a) be the digest of exactly the name to its NUL and (b) come
# back as the id theme_id_ok admits. The padded-tail row is the one that fails if the
# hash input ever becomes the NAME_MAX array instead of the string.
_ID_NAMES = ["av-kit", "Neon", "Ünïcödé 日本語 🔆", "a" * (orbtheme.NAME_MAX - 1), "0"]
_ID_ROWS = []
for _i, _nm in enumerate(_ID_NAMES):
    _b = _nm.encode("utf-8")
    _ID_ROWS.append('    static const unsigned char sdig%d[32] = {%s};'
                    % (_i, ",".join(str(x) for x in hashlib.sha256(_b).digest())))
    _ID_ROWS.append('    icase("%s", sdig%d, %d, "%s");'
                    % (_nm.replace("\\", "\\\\").replace('"', '\\"'), _i, len(_b),
                       orbtheme.theme_id(_nm)))
_PAD = "Padded Tail"
_ID_ROWS.append('    static const unsigned char sdigpad[32] = {%s};'
                % ",".join(str(x) for x in hashlib.sha256(_PAD.encode()).digest()))
_ID_ROWS.append('    icase_pad("%s", sdigpad, "%s");' % (_PAD, orbtheme.theme_id(_PAD)))
for _id, _want, _cmt in (
        ("a1b2c3d4e5f6", 1, "a real issued id"), ("", 0, "empty"),
        ("a1b2c3d4e5f", 0, "eleven: one too short"),
        ("a1b2c3d4e5f60", 0, "thirteen: one too long"),
        ("A1B2C3D4E5F6", 0, "uppercase is not the id"),
        ("a1b2c3d4e5g6", 0, "not hex"),
        ("../../../../etc/passwd", 0, "no traversal reaches the id"),
        ("a1b2c3d4e5f6/../", 0, "slash inside")):
    _ID_ROWS.append('    idcase("%s", %d);                            /* %s */' % (_id, _want, _cmt))
_ID_ROWS.append('    idcase(NULL, 0);')
ID_ROWS = "\n".join(_ID_ROWS)
NAME_DIG = hashlib.sha256("av-kit".encode()).digest()


# ---------------------------------------------------------------------------
# The C harness: the lifted path-safety, identity and §6 code, the lifted FORMAT 3
# reader, and the vector emitter functions main() drives. The reader unit under test
# is standalone (its own word map and helpers), so nothing here needs the retired
# FORMAT 2 reader to compile.
# ---------------------------------------------------------------------------
HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ORB_THEME_NAME_MAX 56
@NAMEMAX@
@HELPERS@
@CRC@
@TYPES@
@ORBTHDR@
@IDHEX@
@CONSTS@
@NAMEOK@
@THEMEPATH@
@EXT@
@THEMEFILE@
@B3READER@
@CKV@

/* the §6 identity module state theme_state reads */
static char     s_active_name[ORB_THEME_NAME_MAX + 1];
static uint32_t s_active_geom_crc, s_active_art_crc;
static bool     s_active_valid;
@THEMESTATE@
@IDFN@
@IDOK@

static int fails = 0;

static void ncase(const char *n, int want)
{
    const int got = orb_sd_name_ok(n);
    if (got != want) fails++;
    printf("%-5s name_ok(%-24s) -> %d (want %d)\n", got == want ? "ok" : "FAIL",
           n ? (n[0] ? n : "(empty)") : "(null)", got, want);
}
static void pcase(const char *n, int want, const char *wantpath)
{
    char p[ORB_SD_PATH_MAX];
    const int ok = orb_sd_theme_path(p, sizeof p, n);
    const int pass = (ok == want) && (!want || strcmp(p, wantpath) == 0);
    if (!pass) fails++;
    printf("%-5s theme_path(%-20s) -> %s%s\n", pass ? "ok" : "FAIL", n ? n : "(null)",
           ok ? p : "(refused)", pass ? "" : "   <== WRONG PATH");
}
static char fbuf[128 + 16];
static void fcase(const char *n, int want, const char *wantpath, unsigned outsz)
{
    memset(fbuf, 0x5a, sizeof fbuf);
    const int ok = theme_file(n, fbuf, outsz);
    int pass = (ok == want) && (!want || strcmp(fbuf, wantpath) == 0);
    if (pass && !want) pass = (fbuf[0] == '\0');          /* a refusal clears out[0] */
    for (unsigned i = outsz; pass && i < sizeof fbuf; i++)
        if ((unsigned char)fbuf[i] != 0x5a) pass = 0;      /* the canary tail held */
    if (!pass) fails++;
    printf("%-5s theme_file(%-20s, outsz=%2u) -> %s%s\n", pass ? "ok" : "FAIL",
           n ? n : "(null)", outsz, ok ? fbuf : "(refused)", pass ? "" : "   <== BAD DESTINATION");
}
static void icase(const char *name, const unsigned char *dig, size_t n, const char *want)
{
    char got[16];
    s_hash_want = dig; s_hash_is224 = -1;
    theme_id(name, got);
    const bool hex_ok = strcmp(got, want) == 0;
    const bool pass = hex_ok && s_hash_len == n && s_hash_is224 == 0
                      && memcmp(s_hash_in, name, n) == 0 && s_hash_in[n] == '\0';
    if (!pass) fails++;
    printf("%-5s id(\"%s\") -> %s (want %s)%s%s%s\n", pass ? "ok" : "FAIL", name, got, want,
           s_hash_len != n ? "   <== HASHED THE WRONG BYTE COUNT" : "",
           (hex_ok || s_hash_len != n) ? "" : "   <== HEX CONVERSION OR THE DIGEST IS WRONG",
           s_hash_is224 ? "   <== ASKED FOR SHA-224, WHICH TRUNCATES" : "");
    if (theme_id_ok(got) != hex_ok) { fails++; printf("FAIL id \"%s\" issued but theme_id_ok() disagrees\n", got); }
}
static void icase_pad(const char *name, const unsigned char *dig, const char *want)
{
    char arr[ORB_THEME_NAME_MAX + 1]; memset(arr, 0xA5, sizeof arr);
    memcpy(arr, name, strlen(name) + 1);                   /* NUL, then 0xA5 pad to the tail */
    char got[16]; s_hash_want = dig; s_hash_len = 0; s_hash_is224 = -1;
    theme_id(arr, got);
    const bool pass = strcmp(got, want) == 0 && s_hash_len == strlen(name) && s_hash_is224 == 0;
    if (!pass) fails++;
    printf("%-5s padded id(\"%s\") -> %s (want %s, hashed %u bytes)%s\n", pass ? "ok" : "FAIL",
           name, got, want, (unsigned)s_hash_len, pass ? "" : "   <== HASHED THE PAD, NOT THE NAME");
}
static void idcase(const char *id, int want)
{
    const int got = theme_id_ok(id);
    if (got != want) fails++;
    printf("%-5s theme_id_ok(%-24s) -> %d (want %d)\n", got == want ? "ok" : "FAIL",
           id ? (id[0] ? id : "(empty)") : "(null)", got, want);
}
static void scase(const char *name, uint32_t g, uint32_t a, const char *act_name,
                  uint32_t ag, uint32_t aa, int want_active, int want_changed, const char *why)
{
    orb_thdr_t h; bool active = true, changed = true; memset(&h, 0, sizeof h);
    host_strlcpy(h.name, name, sizeof h.name); h.geom_crc = g; h.art_crc = a;
    host_strlcpy(s_active_name, act_name, sizeof s_active_name);
    s_active_geom_crc = ag; s_active_art_crc = aa; s_active_valid = true;
    theme_state(&h, &active, &changed);
    const bool pass = (active == (bool)want_active) && (changed == (bool)want_changed);
    if (!pass) fails++;
    printf("%-5s state %-8s geom=%-3u art=%-3u vs wall %-8s -> active=%d changed=%d (want %d/%d)  %s\n",
           pass ? "ok" : "FAIL", name, g, a, act_name, active, changed, want_active, want_changed, why);
}
static void scase_none(const char *why)
{
    orb_thdr_t h; bool active = true, changed = true; memset(&h, 0, sizeof h);
    host_strlcpy(h.name, "anything", sizeof h.name); h.geom_crc = 1; h.art_crc = 2;
    s_active_valid = false; theme_state(&h, &active, &changed);
    const bool pass = !active && !changed;
    if (!pass) fails++;
    printf("%-5s state with no stored theme: active=%d changed=%d  %s\n",
           pass ? "ok" : "FAIL", active, changed, why);
}



/* The FORMAT 3 reader's own refusal for a verified-format header. */
static void r3why(const char *what, const char *want, const uint32_t *w)
{
    orb_bundle3_hdr_t b; const char *got = orb_bundle3_hdr_parse(w, &b);
    if (!got)                   { fails++; printf("FAIL %s: reader ACCEPTED; wanted '%s'\n", what, want); }
    else if (strcmp(got, want)) { fails++; printf("FAIL %s: reader said '%s'; wanted '%s'\n", what, got, want); }
    else                        { printf("ok    %s: %s\n", what, got); }
}
/* The reader fills every shared field from the writer's header. A dropped or swapped
 * field is the tuple the store door's duplicate check compares, exactly. */
static void r3fields(const char *what, const uint32_t *w, const char *name,
                     unsigned ns, unsigned ser, unsigned acrc, unsigned gcrc,
                     unsigned aoff, unsigned gsz)
{
    orb_bundle3_hdr_t b; const char *r = orb_bundle3_hdr_parse(w, &b);
    int pass = !r && strcmp(b.name, name) == 0 && b.n_sections == ns && b.serial == ser &&
               b.art_crc == acrc && b.geom_crc == gcrc && b.art_off == aoff && b.geom_size == gsz;
    if (!pass) fails++;
    printf("%-5s %-46s reader fills every shared field: %s\n", pass ? "ok" : "FAIL",
           what, r ? r : (pass ? name : "FIELDS WRONG"));
}

int main(void)
{
    puts("== the crc32 the device's ROM routine is supposed to agree with ==");
    { static const uint8_t v[9] = {'1','2','3','4','5','6','7','8','9'};
      if (host_crc32(0, v, 9) != 0xCBF43926u) { fails++; puts("FAIL crc32 stub"); }
      else puts("ok    matches CRC-32/ISO-HDLC's published check value"); }
    puts("== theme names a browser could send ==");
@NAMES@
    puts("== where a name is allowed to point ==");
@PATHS@
    puts("== the file the card will be written under ==");
@FILECASES@
    puts("== the card identity: which bytes reach the hash, and the id that comes back ==");
@IDCASES@
    puts("== which row the page calls the one on the dial (the §6 pair) ==");
@STATECASES@
    puts("== the FORMAT 3 door: the writer's header, fed to the shipped reader ==");
@B3ROWS@
    return fails != 0;
}
'''

for _k, _v in (("@HELPERS@", HELPERS), ("@CRC@", CRC_STUB), ("@TYPES@", gen_types),
               ("@ORBTHDR@", orb_thdr), ("@NAMEMAX@", name_max), ("@IDHEX@", id_hex),
               ("@CONSTS@", sd_consts),
               ("@NAMEOK@", f_name_ok), ("@THEMEPATH@", f_theme_pt), ("@EXT@", ext_define),
               ("@THEMEFILE@", f_theme_file), ("@B3READER@", B3_READER), ("@CKV@", CKV),
               ("@THEMESTATE@", f_theme_state), ("@IDFN@", f_theme_id), ("@IDOK@", f_theme_idok),
               ("@NAMES@", NAME_ROWS), ("@PATHS@", PATH_ROWS), ("@FILECASES@", FILE_ROWS),
               ("@IDCASES@", ID_ROWS), ("@STATECASES@", STATE_ROWS), ("@B3ROWS@", B3_ROWS)):
    HARNESS = HARNESS.replace(_k, _v)
if re.search(r"@[A-Z]+@", HARNESS):
    sys.exit("leftover token in the harness:\n" +
             "\n".join(l for l in HARNESS.split("\n") if re.search(r"@[A-Z]+@", l))[:800])

BIN = "/tmp/orb_sd_host_" + hashlib.md5((sd_src + th_src + b3_src).encode()).hexdigest()[:10]


def run_harness():
    """Compile and run the lifted C. -Werror is load-bearing: the lifted code is the
    shipped code, so anything that warns here warns in the IDF build too."""
    src = BIN + ".c"
    open(src, "w").write(HARNESS)
    cc = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Wno-unused-function",
                         "-Werror", "-o", BIN, src], capture_output=True, text=True)
    if cc.returncode:
        print("FAIL the lifted reader does not even compile on the host:")
        print(cc.stderr[-3000:])
        return 1
    r = subprocess.run([BIN], capture_output=True)
    print(r.stdout.decode("utf-8", "backslashreplace"), end="")
    if r.returncode:
        print("  (the harness itself reported failures)")
    return r.returncode


def c_page(sym):
    """Rebuild a page the way the compiler does — concatenate the adjacent C string
    fragments — so the bytes the browser receives are the bytes parsed here."""
    m = re.search(r'static const char ' + sym + r'\[\] =(.*?);\s*\n', net_src, re.S)
    if not m:
        return None, sym + " not found in orb_net.c — did it get renamed?"
    out = []
    for line in m.group(1).split("\n"):
        t = line.strip()
        if not t or t.startswith("/*") or t.startswith("*") or t.startswith("//"):
            continue
        if not re.fullmatch(r'"(?:[^"\\]|\\.)*"', t):
            return None, "%s: line is not a complete C string fragment: %r" % (sym, t[:70])
        out.append(t)
    import ast
    # Each fragment is a C string literal; C-unescape them individually the way the
    # compiler does, then concatenate. Unescaping the joined text instead would let one
    # fragment's trailing backslash eat the next one's opening quote.
    return "".join(ast.literal_eval(x) for x in out), None


def themes_page_js():
    page, err = c_page("HTML_THEMES")
    if err:
        print("FAIL " + err); return 1
    m = re.search(r"<script>(.*?)</script>", page, re.S)
    if not m:
        print("FAIL /themes has no <script> block"); return 1
    js = m.group(1)
    try:
        import esprima
    except ImportError:
        print("SKIP esprima is not installed — the /themes parse check did not run. "
              "Run: pip install esprima")
        js_ok = True
    else:
        try:
            esprima.parseScript(js); js_ok = True
        except Exception as e:
            print("FAIL /themes JS: %s — one syntax error kills the whole <script> and "
                  "the page sits at 'loading' with an empty list" % e)
            js_ok = False
    # Pins: the decisions the FORMAT 3 store page must keep making. The confirm() on
    # delete and store are the ones that matter — the first destroys a file on somebody's
    # card, the second writes flash. D13's truth: there is no staged state to undo.
    pins = [("onclick=doUp()", "an upload path exists"),
            ("function store(id){var nm=ROWS[id]||id;",
             "a store press names the theme it is about to copy"),
            ("function del(id){var nm=ROWS[id]||id;",
             "delete is offered, and names what it destroys"),
            ("if(!confirm('Delete ", "delete asks before destroying a file"),
            ("if(!confirm('Copy ", "a store press names the copy it is about to make"),
            ("if(!confirm('Go back to the factory dial", "the way back asks too"),
            ("ROWS[t.id]=nm;", "the row's name is read back from a map, never pasted into an onclick"),
            ("var ROWS={};", "and that map exists before the rows are drawn"),
            ("x.upload.onprogress", "the upload reports real progress (fetch cannot)"),
            ("/api/v1/sd/bundle?id='+encodeURIComponent(id)",
             "Store posts the streaming ingest by the url-encoded id, not by name"),
            ("/api/v1/sd/delete?id='+encodeURIComponent(id)",
             "delete addresses the row by the url-encoded id, the only handle that resolves"),
            ("/api/v1/sd/default", "the factory dial is one press away without the card"),
            ("data.live", "the banner states the choice, not a pending operation"),
            ("LAST TRY FAILED", "the store the worker refused explains itself on its row"),
            ("CANNOT LOAD", "an unloadable bundle says why instead of vanishing")]
    rc = 0 if js_ok else 1
    for want, why in pins:
        if want not in page:
            print("FAIL /themes: missing %r — %s" % (want, why)); rc = 1
    # No row label may reach a handler as markup: names cover the full printable range
    # including apostrophes, so the id is the only value a row hands to a function.
    for fn, args in re.findall(r'onclick="(store|del)\((.*?)"', page):
        if "nm" in args or "name" in args:
            print("FAIL /themes: the %s handler interpolates a name into markup (%s) — "
                  "pass the id alone" % (fn, args[:60])); rc = 1
    # The FORMAT 2 install door and its Load button are retired: a page that still names
    # them offers a button for a route that no longer exists.
    for gone, why in (("/api/v1/sd/install", "the FORMAT 2 install door is gone from the firmware"),
                      ("function load(", "there is no FORMAT 2 Load button any more"),
                      ("/api/v1/sd/unstage", "the staged load has no undo (ruling D13)")):
        if gone in page:
            print("FAIL /themes: still carries %r — %s" % (gone, why)); rc = 1
    defined = set(re.findall(r"function\s+([A-Za-z_$][\w$]*)", js))
    called  = set(re.findall(r"(?<![.\w$])([A-Za-z_$][\w$]*)\s*\(", js))
    builtin = {"function", "if", "for", "while", "switch", "catch", "return", "typeof",
               "fetch", "String", "Number", "JSON", "Math", "Date", "Array", "parseInt",
               "encodeURIComponent", "XMLHttpRequest", "confirm", "alert", "setTimeout"}
    undef = sorted(c for c in called - defined - builtin if not c[0].isupper())
    if undef:
        print("FAIL /themes JS calls functions that do not exist: %s" % undef); rc = 1
    if not rc:
        print("ok    /themes (%d B page) parses, is pinned to the FORMAT 3 store door, "
              "and calls only functions it has" % len(page))
    return rc


def _code_only(src):
    """The source with comments and string/char literals blanked, line count preserved.

    The lints read call sites out of the text. Without this, a doc comment that says
    "card_open() reserves it" becomes a call to card_open(), and so does any function
    name inside a log message.
    """
    out = []
    i, n = 0, len(src)
    state = None                                  # None | "//" | "/*" | '"' | "'"
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state = "//"; out.append("  "); i += 2; continue
            if c == "/" and nxt == "*":
                state = "/*"; out.append("  "); i += 2; continue
            if c in "\"'":
                state = c; out.append(c); i += 1; continue
            out.append(c); i += 1; continue
        if state == "//":
            if c == "\n":
                state = None; out.append("\n")
            else:
                out.append(" " if c not in "\t" else c)
            i += 1; continue
        if state == "/*":
            if c == "*" and nxt == "/":
                state = None; out.append("  "); i += 2; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
        q = state
        if c == "\\":
            out.append("  "); i += 2; continue
        if c == q:
            state = None; out.append(c); i += 1; continue
        out.append("\n" if c == "\n" else " "); i += 1; continue
    return "".join(out)


def _defs(lines):
    """Every column-0 definition as (name, first_body_line, last_body_line). The files
    are consistently formatted: a definition begins at column 0, its closing brace sits
    alone at column 0, and declarations (which end in ';') are skipped."""
    funcs = []
    i = 0
    while i < len(lines):
        L = lines[i]
        if L and L[0] not in " \t/#}" and "(" in L and not L.startswith(("typedef", "enum", "struct")):
            depth, seen, j = 0, False, i
            while j < len(lines):
                t = lines[j]
                if not seen and t.rstrip().endswith(";"):
                    break
                depth += t.count("{") - t.count("}")
                if "{" in t:
                    seen = True
                if seen and depth <= 0:
                    break
                j += 1
            if seen and j - i < 400:
                m = re.search(r"([A-Za-z_][A-Za-z_0-9]*)\s*\(", " ".join(lines[i:j + 1]))
                if m:
                    funcs.append((m.group(1), i, j))
                i = max(j, i) + 1
                continue
        i += 1
    return funcs


def err_t_lint():
    """Catch the esp_err_t-tested-as-boolean class of bug, whole.

    ESP_OK is 0, so `if (!orb_sd_ensure_mounted())` reads as "if the card IS mounted,
    bail out". It compiles, warns nothing, and no cardless bench sees it. The fix is a
    lint over the shipped sources: any `!` in front of a call to a function the header
    declares esp_err_t is a bug. bool returns are excluded by reading the type out of
    the header rather than guessing at the call site.
    """
    decls = {}
    for hdr in (sd_src_h, th_hdr):
        for typ, fn in re.findall(r"(?m)^\s*(esp_err_t|bool)\s+"
                                  r"(orb_(?:sd|theme)_[A-Za-z0-9_]+)\s*\(", hdr):
            decls.setdefault(fn, typ)
    errs = sorted(f for f, t in decls.items() if t == "esp_err_t")
    if not errs:
        sys.exit("the header declares no esp_err_t orb_sd_*/orb_theme_* functions — "
                 "this lint has nothing to check, which means it silently stopped working")
    rc = 0
    for where, src in (("main/art/orb_theme.c", th_src), ("main/net/orb_net.c", net_src)):
        for fn in errs:
            for m in re.finditer(r"!\s*" + fn + r"\s*\(", src):
                line = src[:m.start()].count("\n") + 1
                print("FAIL %s:%d: `!%s(...)` tests an esp_err_t for truth — ESP_OK is 0. "
                      "Compare against ESP_OK instead." % (where, line, fn))
                rc = 1
    if not rc:
        print("ok    %d esp_err_t entry points called, none tested as a boolean" % len(errs))
    return rc


def chunk_lint():
    """Every function that reads through the shared card buffer must reserve it.

    s_chunk is the one buffer every card operation reads through, and ensure_chunk() is
    the only thing that makes it non-NULL. A caller that skipped it pread()s into a NULL
    destination and FatFs stores through address 0 — a StoreProhibited at the end of an
    otherwise successful upload. The harness mallocs the buffer itself, so it cannot see
    a missing reserve; the check has to be over the shipped source.
    """
    lines = _code_only(th_src).split("\n")
    funcs = _defs(lines)
    users = [(n, a, b) for n, a, b in funcs
             if n != "ensure_chunk" and any("s_chunk" in lines[k] for k in range(a, b + 1))]
    if not users:
        sys.exit("no function in main/art/orb_theme.c mentions s_chunk — this lint has "
                 "nothing to check, which means it silently stopped working")
    rc = 0
    for n, a, b in users:
        if not any("ensure_chunk()" in lines[k] for k in range(a, b + 1)):
            print("FAIL main/art/orb_theme.c:%d: %s() reads through s_chunk without calling "
                  "ensure_chunk() — the buffer is NULL on any boot that has not staged a "
                  "theme, and pread() through a NULL destination stores over address 0"
                  % (a + 1, n))
            rc = 1
    if not rc:
        print("ok    %d functions read through s_chunk (%s), all reserve it themselves"
              % (len(users), ", ".join(sorted({n for n, _, _ in users}))))
    return rc


def lock_lint():
    """Nothing a card-lock holder can reach may take the card mutex again.

    orb_sd_lock() is a plain non-recursive mutex, so a second take by the task that
    already holds it never returns. This is not a plain call-graph check: an unlocked
    call to ensure_mounted is fine and the same call after the lock is the bug, so what
    is examined is the calls between a take and the matching give, followed through the
    local call graph, plus the functions that hold the lock on entry by contract.
    """
    lines = _code_only(th_src).split("\n")
    funcs = _defs(lines)
    span = {n: (a, b) for n, a, b in funcs}
    text = {n: "\n".join(lines[a:b + 1]) for n, a, b in funcs}
    cross = {"orb_sd_ensure_mounted", "orb_sd_remount", "orb_sd_init",
             "orb_files_walk", "orb_files_delete"}
    holders = {n for n, t in text.items() if "orb_sd_lock()" in t}
    contract = {n for n, t in text.items() if "orb_sd_unlock()" in t} - holders
    if not holders:
        sys.exit("no function in main/art/orb_theme.c takes orb_sd_lock — this lint has "
                 "nothing to check, which means it silently stopped working")
    danger = holders | contract | cross
    call_re = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)\s*\(")

    def calls(a, b, owner=None):
        return [m.group(1) for m in call_re.finditer("\n".join(lines[a:b + 1]))
                if m.group(1) not in ("orb_sd_lock", "orb_sd_unlock")
                and m.group(1) != owner]

    def held_spans(n):
        if n in contract:
            return [span[n]]
        a, b = span[n]
        takes = [k for k in range(a, b + 1) if "orb_sd_lock()" in lines[k]]
        if not takes:
            return []
        gives = [k for k in range(a, b + 1) if "orb_sd_unlock()" in lines[k]]
        return [(takes[0], gives[-1] if gives else b)]

    def walk(seeds, start):
        stack, seen = [(c, [c]) for c in seeds], set()
        while stack:
            nm, chain = stack.pop()
            if nm in seen:
                continue
            seen.add(nm)
            if nm in danger and nm != start:
                return chain
            if nm in span:
                a2, b2 = span[nm]
                stack += [(c, chain + [c]) for c in calls(a2, b2)]
        return None

    rc = 0
    for n in sorted(holders | contract):
        for a, b in held_spans(n):
            bad = walk(calls(a, b, n), n)
            if bad:
                print("FAIL main/art/orb_theme.c:%d: %s() holds orb_sd_lock() and reaches %s "
                      "while holding it — the second take never returns; call the _locked variant"
                      % (a + 1, n, " -> ".join(bad)))
                rc = 1
    for n in sorted(holders):
        a, b = span[n]
        first = next((k for k in range(a, b + 1) if "orb_sd_lock()" in lines[k]), None)
        if first is None:
            continue
        if not any("orb_sd_ensure_mounted()" in lines[k] for k in range(a, first)):
            print("FAIL main/art/orb_theme.c:%d: %s() takes orb_sd_lock() without calling "
                  "orb_sd_ensure_mounted() first — nothing else on this path mounts"
                  % (a + 1, n))
            rc = 1
    if not rc:
        print("ok    %d card-lock holders: none reaches a second take, all mount before "
              "locking (%d cross-file lockers accounted for)"
              % (len(holders | contract), len(cross)))
    return rc




def install_lint():
    """The FORMAT 3 store door may not make a slot live it did not verify the way boot
    reads it, and may not choose the theme it stores. A source check, not a call: the
    paths it orders touch FatFs and the flash driver. The ordering it pins:
    card_open_verified_id3 -> region parse -> rebase -> copy_bundle3 -> the header erase
    that makes the slot live, bracketed by staged_mark()/staged_clear(). Ruling D13: slot
    A is never a store target. D26: a stored FORMAT 3 slot is not chosen.
    """
    lines = _code_only(th_src).split("\n")
    spans = {}
    for n, a, b in _defs(lines):
        spans.setdefault(n, (a, b))
    if "orb_theme_install_bundle" not in spans:
        sys.exit("no orb_theme_install_bundle() in main/art/orb_theme.c — this lint has "
                 "nothing to check, which means it silently stopped working")
    ba, bb = spans["orb_theme_install_bundle"]
    body = "\n".join(lines[ba:bb + 1])
    rc = 0
    erase = body.find("esp_partition_erase_range")
    if erase < 0:
        print("FAIL orb_theme_install_bundle(): no header erase — the commit that makes a "
              "slot live has gone missing along with the discipline around it"); rc = 1
    for need in ("card_open_verified_id3(", "orb_bundle_parse_region(", "copy_bundle3("):
        at = body.find(need)
        if at < 0:
            print("FAIL orb_theme_install_bundle(): never calls %s — it can commit a slot "
                  "that boot will refuse to draw" % need)
            rc = 1
        elif erase >= 0 and at > erase:
            print("FAIL orb_theme_install_bundle(): %s runs AFTER the header erase — a "
                  "failure after that point has already destroyed the slot that worked" % need)
            rc = 1
    rb, cp = body.find("orb_bundle_rebase("), body.find("copy_bundle3(")
    if rb >= 0 and cp >= 0 and rb > cp:
        print("FAIL orb_theme_install_bundle(): rebases AFTER copying — the slot would hold "
              "the card file's own offsets, not the slot's"); rc = 1
    if "orb_theme_set_live" in body:
        print("FAIL orb_theme_install_bundle(): calls orb_theme_set_live — a FORMAT 3 slot "
              "is stored, not chosen, until the compositor can draw it (D26)"); rc = 1
    if "SLOT_A_HDR" in body or "SLOT_A_ART" in body:
        print("FAIL orb_theme_install_bundle(): names slot A — the factory default's pixels "
              "live there, and a store aimed at it erases the fallback with it"); rc = 1
    mk, cl = body.find("staged_mark("), body.find("staged_clear(")
    if mk < 0 or cl < 0:
        print("FAIL orb_theme_install_bundle(): does not bracket the copy with "
              "staged_mark()/staged_clear() — a power cut inside it leaves the row silent"); rc = 1
    elif erase >= 0 and mk > erase:
        print("FAIL orb_theme_install_bundle(): marks staged AFTER the first erase — the "
              "window the word testifies to was already open when it was written"); rc = 1
    if "bundle_job" in spans:
        jbody = "\n".join(lines[spans["bundle_job"][0]:spans["bundle_job"][1] + 1])
        if "orb_jobs_reboot_soon" in jbody:
            print("FAIL bundle_job() reboots — a stored bundle is not drawn"); rc = 1
        if "orb_theme_install_bundle(" not in jbody:
            print("FAIL bundle_job() does not call the streaming ingest"); rc = 1
    else:
        print("FAIL main/art/orb_theme.c has no bundle_job() — the ingest has no worker"); rc = 1
    if "orb_theme_post_bundle" in spans:
        pbody = "\n".join(lines[spans["orb_theme_post_bundle"][0]:spans["orb_theme_post_bundle"][1] + 1])
        if "orb_jobs_post(bundle_job" not in pbody:
            print("FAIL orb_theme_post_bundle() does not queue bundle_job — an ingest run "
                  "off the worker loses the busy gate and the card-lock rule"); rc = 1
    else:
        print("FAIL main/art/orb_theme.c has no orb_theme_post_bundle() — the route has "
              "nothing to post"); rc = 1
    if th_src.count("orb_theme_install_bundle(r->id") != 1:
        print("FAIL orb_theme_install_bundle() is not called from exactly one place"); rc = 1
    rc = _install_upload_check(rc)
    for need in ('{ .uri = "/api/v1/sd/bundle",   .method = HTTP_POST, .handler = h_bundle_install }',
                 "orb_theme_post_bundle(id)", "h_bundle_install(httpd_req_t"):
        if need not in net_src:
            print("FAIL the flip door is missing %s from orb_net.c" % need); rc = 1
    if not rc:
        print("ok    the FORMAT 3 store verifies identity, region and banks before the "
              "header erase, stores without choosing, brackets the copy with the staged word")
        print("ok    one caller, one route; the upload door verifies before the rename and "
              "the duplicate tuple is the whole §6 identity")
    return rc



def _install_upload_check(rc):
    """The FORMAT 3 upload door: header+region verified before the rename, and the
    duplicate tuple carries the format's own §6 identity."""
    lines = _code_only(th_src).split("\n")
    spans = {}
    for n, a, b in _defs(lines):
        spans.setdefault(n, (a, b))
    if "orb_theme_upload_end" not in spans:
        print("FAIL main/art/orb_theme.c has no orb_theme_upload_end() — the FORMAT 3 "
              "upload door is gone and the page would post to nothing")
        return 1
    ubody = "\n".join(lines[spans["orb_theme_upload_end"][0]:spans["orb_theme_upload_end"][1] + 1])
    for need in ("card_check3(", "verify_bundle3("):
        if need not in ubody:
            print("FAIL orb_theme_upload_end() lost %s — an unverified FORMAT 3 container "
                  "would reach the card name it is filed under" % need)
            rc = 1
    rpos = ubody.find("rename(")
    vpos = min((ubody.find(x) for x in ("card_check3(", "verify_bundle3(") if ubody.find(x) >= 0), default=-1)
    if rpos >= 0 and vpos >= 0 and vpos > rpos:
        print("FAIL orb_theme_upload_end() verifies after the rename — the window where a "
              "half-checked file carries a loadable name is the C8 bug"); rc = 1
    for pat, what in ((r"strcmp\s*\(\s*oh3\.name\s*,\s*b3\.name\s*\)", "the owner name"),
                      (r"oh3\.geom_crc\s*==\s*b3\.geom_crc", "the geometry checksum"),
                      (r"oh3\.art_crc\s*==\s*b3\.art_crc", "the pixel checksum"),
                      (r"oh3\.serial\s*==\s*b3\.serial", "the install serial")):
        if not re.search(pat, ubody):
            print("FAIL orb_theme_upload_end(): the duplicate check no longer compares %s "
                  "— two different themes can be called the same file" % what)
            rc = 1
    return rc



def naming_lint():
    """FORMAT 3's identity rule, checked over the shipped source.

    A bundle's one true name is the string inside its header, and the card filename is
    theme_id(name) — hex(sha256(name))[:12]. Nothing else is allowed to name a card file.
    The failure this pins: an entry listed as storable because one consumer applied the
    rule while the consumer that acted did not. Executing that needs FatFs, so this
    checks the text: the identity functions must be the single rule, and every FORMAT 3
    consumer must reach for them.
    """
    lines = _code_only(th_src).split("\n")
    raw = th_src.split("\n")
    spans = {}
    for n, a, b in _defs(lines):
        spans.setdefault(n, (a, b))
    rc = 0

    def body(name):
        a, b = spans.get(name, (0, -1))
        return "\n".join(raw[a:b + 1])               # reads literals _code_only would blank

    def calls(name):
        a, b = spans.get(name, (0, -1))
        return "\n".join(lines[a:b + 1])

    for need, why, sym in (
            (r"mbedtls_sha256\s*\(\s*\(const unsigned char \*\)\s*name\s*,\s*strlen\s*\(\s*name\s*\)",
             "hashes the name to its NUL, not the padded header array", "theme_id"),
            (r'"0123456789abcdef"\s*\[\s*digest\[i\]\s*>>\s*4\s*\]',
             "builds the id's hex from the high nibble, lower-case", "theme_id"),
            (r"out\[THEME_ID_HEX\]\s*=\s*'", "NUL-terminates the id", "theme_id")):
        if sym not in spans:
            print("FAIL %s() is not a column-0 definition any more" % sym); rc = 1
        elif not re.search(need, body(sym)):
            print("FAIL main/art/orb_theme.c: %s() no longer %s" % (sym, why)); rc = 1
    for need, why in (
            (r"if\s*\(\s*id\s*==\s*NULL\s*\)\s*return false;", "refuses a missing id"),
            (r"if\s*\(\s*n\s*>=\s*THEME_ID_HEX\s*\)\s*return false;", "refuses an id too long"),
            (r"return\s+n\s*==\s*THEME_ID_HEX\s*;", "refuses an id too short"),
            (r"\(c\s*>=\s*'0'\s*&&\s*c\s*<=\s*'9'\)\s*\|\|\s*\(c\s*>=\s*'a'\s*&&\s*c\s*<=\s*'f'\)",
             "admits nothing outside [0-9a-f]")):
        if not re.search(need, body("theme_id_ok")):
            print("FAIL main/art/orb_theme.c: theme_id_ok() no longer %s" % why); rc = 1
    CONSUMERS = ("scan_entry", "resolve3_entry", "card_open_verified_id3",
                 "orb_theme_upload_end", "orb_theme_delete")
    for n in CONSUMERS:
        if n not in spans:
            print("FAIL main/art/orb_theme.c: %s() is not a column-0 definition" % n); rc = 1
            continue
        seg = calls(n)
        # A consumer applies the rule itself, or reaches it through the door that does:
        # card_open_verified_id3 / card_check3 / theme_file all go by the one hash.
        if not any(k in seg for k in ("theme_id(", "theme_id_ok(",
                                      "card_open_verified_id3(", "card_check3(", "theme_file(")):
            print("FAIL main/art/orb_theme.c:%d: %s() touches a card theme without asking "
                  "theme_id()/theme_id_ok() what its name is" % (spans[n][0] + 1, n))
            rc = 1
    if "resolve3_entry" not in spans:
        print("FAIL resolve3_entry() is gone — nothing checks the card header against its id"); rc = 1
    elif "theme_id(" not in calls("resolve3_entry"):
        print("FAIL resolve3_entry() matches an entry without hashing its header name"); rc = 1
    if "card_open_verified_id3" in spans:
        seg = calls("card_open_verified_id3")
        opened = seg.find("card_open")
        if "theme_id_ok(" not in seg:
            print("FAIL card_open_verified_id3() stops validating the id before it walks"); rc = 1
        if opened < 0 or seg.find("theme_id(", opened) < 0:
            print("FAIL card_open_verified_id3() no longer re-hashes the header AFTER opening"); rc = 1
        if not re.search(r"strcmp\s*\(\s*id2\s*,\s*id\s*\)\s*!=\s*0", seg):
            print("FAIL card_open_verified_id3() re-hashes without comparing to the id asked for"); rc = 1
    if "scan_entry" in spans:
        if not re.search(r"strcmp\s*\(\s*bid\s*,\s*rec->name\s*\)", calls("scan_entry")):
            print("FAIL scan_entry() lists a theme without checking the filename against the "
                  "hash of its header name"); rc = 1
    if not rc:
        print("ok    card identity is one rule, applied by all %d FORMAT 3 consumers, "
              "verified on the walk and again on the open" % len(CONSUMERS))
    return rc


def caps_lint():
    """The numbers and enums the two sides share by name rather than by value.

    theme_schema.py is the single authority: the bake, the packer, the reader and the
    dial's arrays all take their widths and caps from it. Each line is one place a
    rename or an appended enum value on one side would otherwise ship as a dial that
    skips a window the theme paid for, with nothing logged. The FORMAT 3 reader's word
    map is shared with orb_bundle.c by copy, so the last check makes that sharing a
    fact rather than a comment.
    """
    print("== the caps, word counts and enums the two sides share ==")
    rc = 0

    def cdefine(sym):
        m = re.search(r"#define " + sym + r"\s+([A-Za-z_]\w*|\d+)[^\n]*", th_src)
        if not m:
            sys.exit("could not find #define " + sym + " in " + TH_C + " — did they move?")
        v = m.group(1)
        if not v.isdigit():
            m2 = re.search(r"#define " + v + r"\s+\(?(\d+)\)?[^\n]*", th_src)
            if not m2:
                sys.exit("#define " + sym + " is " + v + ", and " + v + " is undefined")
            v = m2.group(1)
        return int(v)

    for sym, want in (("COMPS_MAX", ts.CLK_COMP_MAX), ("AUX_MAX", ts.CLK_AUX_MAX),
                      ("BANK_MAX", ts.CLK_BANK_MAX), ("COMP_WORDS", ts.words_of("comp")),
                      ("AUX_WORDS", ts.words_of("aux")), ("BANK_WORDS", ts.words_of("bank")),
                      ("GEOM_COUNTS_V1", 4), ("GEOM_COUNTS_V2", 7)):
        got = cdefine(sym)
        ok = got == want
        print("%-5s %-16s reader %4d  schema %4d" % ("ok" if ok else "FAIL", sym, got, want))
        if not ok:
            rc = 1
    m = re.search(r"#define CLK_STRIP_GLYPHS_MAX\s+(\d+)", th_hdr)
    if not m:
        sys.exit("could not find CLK_STRIP_GLYPHS_MAX in " + TH_H)
    ok = int(m.group(1)) == ts.CLK_STRIP_GLYPHS
    print("%-5s %-16s reader %4d  schema %4d"
          % ("ok" if ok else "FAIL", "CLK_STRIP_GLYPHS", int(m.group(1)), ts.CLK_STRIP_GLYPHS))
    if not ok:
        rc = 1
    m = re.search(r"_Static_assert\(CLK_SRC__MAX == (\d+)", th_hdr)
    if not m:
        sys.exit("orb_theme.h no longer asserts the source enum's length — the theme_schema "
                 "driving values are unpinned")
    ok = int(m.group(1)) == len(ts.SOURCES)
    print("%-5s %-16s reader %4d  schema %4d" % ("ok" if ok else "FAIL", "CLK_SRC__MAX", int(m.group(1)), len(ts.SOURCES)))
    if not ok:
        rc = 1
    rec_types = lift(th_hdr, r"enum \{\s*CLK_KIND_WINDOW[\s\S]*?\} clk_aux_t;", TH_H)
    for stem, want in (("CLK_KIND_", ts.KINDS), ("CLK_SRC_", ts.SOURCES)):
        names = re.findall(r"\b(" + stem + r"[A-Z0-9_]+)\b", rec_types)
        names = [n for n in dict.fromkeys(names) if not n.endswith("__MAX")]
        got = tuple(n[len(stem):].lower() for n in names)
        ok = got == tuple(want)
        print("%-5s %-16s order matches the schema tuple" % ("ok" if ok else "FAIL", stem + "*"))
        if not ok:
            print("      reader:", got, "\n      schema:", tuple(want)); rc = 1
    # orb_bundle.c's reader shares orb_theme.c's word map by copy; C forbids one
    # enumerator twice in a translation unit, so the sharing is only sound while the two
    # enums are the same map. Drift here silently changes which word the reader calls
    # FORMAT, voiding every P6.x gate claim.
    _e_th = re.search(r"enum \{[^}]*W_MAGIC[^}]*\}", th_src)
    _e_b3 = re.search(r"enum \{[^}]*W_MAGIC[^}]*\}", b3_src)
    def _emap(m):
        return re.sub(r"\s+", "", re.sub(r"/\*[\s\S]*?\*/", "", m.group(0)))
    if not _e_th or not _e_b3:
        print("FAIL a word-index enum is unfindable — caps_lint no longer knows the map"); rc = 1
    elif _emap(_e_th) != _emap(_e_b3):
        print("FAIL orb_bundle.c and orb_theme.c word maps differ — the reader would read "
              "FORMAT from the wrong word"); rc = 1
    else:
        print("ok    orb_bundle.c's word map is orb_theme.c's, byte for byte")
    return rc


MUTATIONS = [
    # (name, which file, old text, new text). Each must make SOME check go red; a
    # pin you can delete without a failure is not a pin. Only surviving anchors.
    ("traversal: '/' allowed in a name",           "sd",
     "(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'",
     "(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/'"),
    ("name length cap raised away",                 "sd",
     "if (n >= ORB_SD_NAME_MAX) return false;", "/* no cap */"),
    ("path builder stops validating the name",      "sd",
     "if (!orb_sd_name_ok(name)) { out[0] = '\\0'; return false; }", ""),
    ("leading dot allowed (hides the entry)",       "sd",
     "if (name[0] == '.') return false;", ""),
    # The card identity: the bytes hashed and the charset admitted.
    ("identity: hashes the padded name array",       "th",
     "mbedtls_sha256((const unsigned char *)name, strlen(name), digest, /*is224=*/0);",
     "mbedtls_sha256((const unsigned char *)name, THEME_NAME_MAX, digest, /*is224=*/0);"),
    ("identity: asks for SHA-224, whose digest is shorter", "th",
     "mbedtls_sha256((const unsigned char *)name, strlen(name), digest, /*is224=*/0);",
     "mbedtls_sha256((const unsigned char *)name, strlen(name), digest, /*is224=*/1);"),
    ("identity: the high nibble comes out uppercase", "th",
     '        out[2 * i]     = "0123456789abcdef"[digest[i] >> 4];',
     '        out[2 * i]     = "0123456789ABCDEF"[digest[i] >> 4];'),
    ("identity validator admits uppercase",          "th",
     "        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;",
     "        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;"),
    ("identity validator has no upper length bound", "th",
     "        if (n >= THEME_ID_HEX) return false;          /* too long */",
     "        if (n > THEME_ID_HEX) return false;           /* too long */"),
    ("identity validator stops checking the lower bound", "th",
     "    return n == THEME_ID_HEX;                          /* and no shorter */",
     "    return n > 0;                                      /* and no shorter */"),
    # The §6 pair.
    ("§6 calls any same-named file active",          "th",
     "    *active  = same_name && !geom_diff && !art_diff;", "    *active  = same_name;"),
    ("§6 marks another theme's file changed",        "th",
     "    *changed = same_name && (geom_diff || art_diff);", "    *changed = (geom_diff || art_diff);"),
    # The FORMAT 3 store door's ordering, and its no-choose / slot-A rules.
    ("store commits without copying through the streaming writer", "th",
     "    esp_err_t e = copy_bundle3(fd, &b, region, file_art_base, p, install_target(),",
     "    esp_err_t e = ESP_OK; (void)file_art_base;"),
    ("store reboots to show a bundle nobody can draw", "th",
     "esp_err_t orb_theme_post_bundle(const char *id)\n{",
     "esp_err_t orb_theme_post_bundle(const char *id)\n{ orb_jobs_reboot_soon(\"bundle\");"),
    ("the upload door accepts a bundle without the region check", "th",
     "    if (!r) r = verify_bundle3(fd, &b3, fsz);", "    (void)verify_bundle3;"),
    ("upload duplicate check drops the owner name",  "th",
     "oh3.serial == b3.serial && strcmp(oh3.name, b3.name) == 0) {", "oh3.serial == b3.serial) {"),
    # The net route the flip page depends on.
    ("the flip route vanishes from the table",       "net",
     "        { .uri = \"/api/v1/sd/bundle\",   .method = HTTP_POST, .handler = h_bundle_install },\n", ""),
    # The FORMAT 3 reader's own word map — the door reads FORMAT from this word.
    ("reader reads FORMAT from the wrong word",      "b3",
     "    if (w[W_FORMAT] != ORB_BUNDLE3_FORMAT)", "    if (w[W_GEOM_ABI] != ORB_BUNDLE3_FORMAT)"),
    ("reader drops the self-checksum before trusting the name", "b3",
     "    if (w[W_HDR_CRC] != esp_rom_crc32_le(0, (const uint8_t *)zeroed, sizeof zeroed))",
     "    if (0)"),
]

_FILE_BY_WHICH = {"sd": (SD_C, lambda: sd_src), "th": (TH_C, lambda: th_src),
                  "net": (NET_C, lambda: net_src), "b3": (B3_C, lambda: b3_src)}


def run_mutations():
    import tempfile, shutil
    rc = 0
    for name, which, old, new in MUTATIONS:
        path, getter = _FILE_BY_WHICH[which]
        src = getter()
        if src.count(old) < 1:
            print("FAIL mutation %r: anchor found %dx, want at least 1 — the battery is stale"
                  % (name, src.count(old))); rc = 1; continue
        tmp = tempfile.mkdtemp()
        for real in (SD_C, SD_H, TH_C, TH_H, B3_C, B3_H, GEN_H, GEN_B, NET_C,
                     P("tools", "orbtheme.py"), P("tools", "orbbundle.py")):
            d = os.path.join(tmp, os.path.relpath(os.path.dirname(real), ROOT))
            os.makedirs(d, exist_ok=True); shutil.copy(real, d)
        open(os.path.join(tmp, os.path.relpath(path, ROOT)), "w").write(src.replace(old, new, 1))
        r = subprocess.run([sys.executable, __file__, "--root", tmp], capture_output=True, text=True)
        shutil.rmtree(tmp, ignore_errors=True)
        if r.returncode == 0:
            print("FAIL  mutation %r SURVIVED — a real regression would pass too" % name)
            print(r.stdout[-900:]); rc = 1
        else:
            first = next((l for l in r.stdout.splitlines() if "FAIL" in l), "?")
            print("ok    mutation %r caught: %s" % (name, first[:100]))
    return rc



if "--mutations" in sys.argv:
    sys.exit(run_mutations())

rc = run_harness()
if rc == 0:
    rc = caps_lint()
if rc == 0:
    rc = themes_page_js()
if rc == 0:
    rc = err_t_lint()
if rc == 0:
    rc = chunk_lint()
if rc == 0:
    rc = lock_lint()
if rc == 0:
    rc = naming_lint()
if rc == 0:
    rc = install_lint()
sys.exit(rc)


