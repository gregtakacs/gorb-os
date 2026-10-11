#!/usr/bin/env python3
"""Bench check for the microSD + theme-install feature.

    python3 tools/host_sd_test.py               # all checks
    python3 tools/host_sd_test.py --mutations   # do the checks actually bite?

Why these checks exist on a bench that has no card in the socket and no radio:

1. PATH SAFETY. orb_sd_name_ok()/orb_sd_theme_path() decide whether a string that
   arrived as a browser query parameter becomes a path on the card. Pure functions,
   exhaustively testable here, and the only thing between ?name=../../ and the rest
   of the volume.

2. THE WIRE FORMAT, AGAINST ITS OWN MIRROR. main/art/orb_theme.c reads byte for byte
   what tools/orbtheme.py writes — two implementations in two languages with no
   compiler between them. The harness LIFTS the C reader verbatim, packs a container
   through the Python writer with a distinct sentinel in every field, and asserts the
   C recovers each one. That is the only check that can notice a field reordered on
   ONE side; that bug does not crash, it draws a dial whose hand pivots came from the
   overlay's fields — bad art, not an error message.

3. THE /themes PAGE, parsed the way the phone's browser will parse it. One unbalanced
   paren in those C fragments kills the whole <script> and the page sits at
   "loading" — invisible here, glaring there. (Same disease tools/host_tz_test.py
   already guards on the other two pages.)

The lifts fail LOUDLY if the code they extract has moved: a lift that silently came
back empty would keep reporting green while testing nothing.

Needs esprima for the page check (pip install esprima); a missing parser is a loud
SKIP on that family, never a pass.
"""
import hashlib, os, re, struct, subprocess, sys, zlib

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
       else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)
SD_C, SD_H = P("main", "sd", "orb_sd.c"),   P("main", "sd", "orb_sd.h")
TH_C, TH_H = P("main", "art", "orb_theme.c"), P("main", "art", "orb_theme.h")
NET_C      = P("main", "net", "orb_net.c")
GEN_H      = P("assets", "clock_assets.h")
for p in (SD_C, SD_H, TH_C, TH_H, NET_C, GEN_H):
    if not os.path.exists(p):
        sys.exit("missing " + p + " — the lifts below have no fallback")

sd_src, th_src = open(SD_C).read(), open(TH_C).read()
th_hdr, sd_src_h = open(TH_H).read(), open(SD_H).read()
net_src, gen_src = open(NET_C).read(), open(GEN_H).read()
if "--src" in sys.argv:                      # mutation runs re-point the NET lifts
    net_src = open(sys.argv[sys.argv.index("--src") + 1]).read()

sys.path.insert(0, P("tools"))
import orbtheme

def lift(text, pattern, where):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not lift %r from %s — did the code move? Dying here is the "
                 "point: an empty lift would keep this suite green while testing a "
                 "copy that no longer ships." % (pattern, where))
    return m.group(0)

# ---------------------------------------------------------------------------
# The lifts, verbatim from what compiles into the device.
# ---------------------------------------------------------------------------
sd_consts = "\n".join(lift(sd_src_h, r"#define " + k + r"\b[^\n]*", SD_H) for k in
                     ("ORB_SD_BASE", "ORB_SD_THEME_DIR", "ORB_SD_NAME_MAX", "ORB_SD_PATH_MAX"))
f_name_ok  = lift(sd_src, r"bool orb_sd_name_ok\(const char \*name\)[\s\S]*?\n\}", SD_C)
f_theme_pt = lift(sd_src, r"bool orb_sd_theme_path\(char \*out[\s\S]*?\n\}", SD_C)
# The theme layer's own destination builder, which is what the upload, delete and
# open paths actually call. Tested rather than trusted because it is the last place
# the ".orbtheme" suffix gets added: a client that could spell it would control both
# the extension and, through it, what the scanner on the next page considers a theme.
f_theme_file = lift(th_src, r"static bool theme_file\(const char \*slug[\s\S]*?\n\}", TH_C)
ext_define = lift(th_hdr, r"#define ORB_THEME_EXT\b[^\n]*", TH_H)

# clk_theme_t / clk_sprite_t / clk_bg_rect_t and CLK_SCR_W/H, from the committed
# GENERATED header — not restated, because a restatement could pass while the device
# compiles against different types.
i0 = gen_src.index("#define CLK_SCR_W")
gen_types = gen_src[i0:gen_src.index("} clk_bg_rect_t;") + len("} clk_bg_rect_t;")]

geo_t = lift(th_hdr, r"typedef struct \{\s*clk_theme_t\s+\*theme;[\s\S]*?\} orb_theme_geo_t;", TH_H)

# Everything pure in the theme reader: word map, format constants, the slot geometry
# the declared sizes are checked against, the cursor, and both parsers. The end of the
# region is found by counting braces from the parser's own signature, NOT by matching
# one of its message strings: --mutations deletes those, and a lift anchored to one
# used to fall off the end and die with a ValueError that only looked like a catch.
START = "enum {\n    W_MAGIC"
if START not in th_src:
    sys.exit("could not find the theme reader's word map in " + TH_C)
a = th_src.index(START)
_sig = "const char *orb_theme_geom_parse("
if _sig not in th_src:
    sys.exit("could not find the geometry parser's signature in " + TH_C +
             " — the lift below no longer knows what to copy")
_b = th_src.index(_sig)
_depth, _i = 0, th_src.index("{", _b)
while True:
    if th_src[_i] == "{":
        _depth += 1
    elif th_src[_i] == "}":
        _depth -= 1
        if _depth == 0:
            break
    _i += 1
pure_theme = th_src[a:_i + 2]          # +2 takes the closing brace and the newline

CRC_STUB = """
/* esp_rom_crc32_le is a ROM routine. This host copy is only trustworthy because of
 * the known-answer row in main(): if it and the silicon ever disagree, every real
 * container reads as 'damaged' on the device and green here. */
static uint32_t host_crc32(uint32_t crc, const uint8_t *b, uint32_t n)
{
    /* The ROM routine complements on the way in and on the way out, and that is
     * exactly what makes esp_rom_crc32_le(0, buf, len) equal to zlib.crc32(buf),
     * the value tools/orbtheme.py writes. Drop either '~' and every real container
     * reads as damaged on the device while this host test stays green; the
     * known-answer row in main() is what would catch that. */
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}
#define esp_rom_crc32_le host_crc32
#define ESP_LOGE(a, ...) do { } while (0)
#define ESP_LOGW(a, ...) do { } while (0)
#define ESP_LOGI(a, ...) do { } while (0)
#define TAG "host"
typedef int esp_err_t;
#define ESP_OK 0
"""

# ---------------------------------------------------------------------------
# Sentinels: one distinct value per field, so a reader that is off by ONE field
# cannot accidentally produce the expected number for anything.
# ---------------------------------------------------------------------------
def sv(t, seed):
    """A value inside `t`'s range, distinct per (table, seed)."""
    lo, hi = orbtheme._range(t)
    span = hi - lo
    return lo + (seed * 37 + 11) % (span if span > 1 else 2)

SCALARS, SC_NAMES = {}, [n for n, _t in orbtheme.THEME_FIELDS]
for i, (n, t) in enumerate(orbtheme.THEME_FIELDS):
    SCALARS[n] = sv(t, 1000 + i)
# The two fields whose values the reader cross-checks against the tables have to be
# inside what the sentinel tables actually carry, or the parser is right to refuse.
SCALARS["gif_frames"] = 3
SCALARS["tourbillon_n"] = 5
SCALARS["bg_mode"] = 1
SCALARS["order_n"] = 4
SCALARS["shadow_on"] = 1

def spr(base):
    return {f: sv(orbtheme._sptype(f), base + 7 * j)
            for j, f in enumerate(orbtheme.SPRITE_FIELDS)}

HANDS   = [spr(2000 + 500 * i) for i in range(5)]
SHADOWS = [spr(5000 + 500 * i) for i in range(5)]
OVERLAY = spr(8000)
TOURB   = [spr(9000 + 300 * i) for i in range(8)]
for i in range(5):                       # hands 3 and shadow 2 are not shown
    HANDS[3]["show"] = 0
    SHADOWS[2]["show"] = 0

# Sprites need bespoke sentinels: the shipped reader checks every extent against
# the declared payload, so a generic full-range value for w/h would be correctly
# refused and the row would prove nothing. Small ink, distinct offsets, real pivots.
def spr(base):
    d = {}
    for j, f in enumerate(orbtheme.SPRITE_FIELDS):
        t = orbtheme._sptype(f)
        if f in ("w", "h"):        d[f] = 3 + (base + j) % 6          # 3..8
        elif f == "blend":         d[f] = (base + j) % 3              # 0..2 (blend 0/1/2)
        elif f == "show":          d[f] = 1
        elif f == "color_off":     d[f] = 4096 + ((base * 61) % 380000)
        elif f in ("diffw", "diffh"): d[f] = 1 + (base + j) % 40
        else:                      d[f] = sv(t, base + 7 * j)        # signed fields: any
        d[f] = orbtheme._u(d[f]) if not t.startswith("i") else d[f]
    return d

HANDS   = [spr(2000 + 500 * i) for i in range(5)]
SHADOWS = [spr(5000 + 500 * i) for i in range(5)]
OVERLAY = spr(8000)
TOURB   = [spr(9000 + 300 * i) for i in range(5)]      # tourbillon_n sentinel = 5
for i in range(5):                                      # the reader only checks
    HANDS[3]["show"] = 0                                # shown layers, so blanking
    SHADOWS[2]["show"] = 0                              # two is the honest case

PLATE  = 466 * 466 * 2          # one full-screen RGB565 plane, as the reader computes it
ART_BYTES = 2 * PLATE           # two plates: the reader checks plate/key extents against this
PLATE_OFF, KEY_OFF = 0, PLATE
SCALARS["bg_frames"] = 2
SCALARS["plate_off"] = PLATE_OFF
SCALARS["bg_key_off"] = KEY_OFF

N_RECTS, N_FIRST, N_DELAY, N_BGF = 4, 4, 3, 2
RECTS = []
for i in range(N_RECTS):
    RECTS += [10 + i, 20 + i, 30 + i, 25 + i, 1024 * (i + 1)]        # x,y,w,h,off
for i in range(N_RECTS):                                             # stay on the panel
    RECTS[5 * i + 0] = 100 + 5 * i
    RECTS[5 * i + 1] = 120 + 5 * i
    RECTS[5 * i + 2] = 40 + 5 * i
    RECTS[5 * i + 3] = 35 + 5 * i
FIRSTS = [0, 1, 2, N_RECTS]
DELAYS = [40, 80, 120]
BGF    = [0, PLATE // 2]

GEOM = orbtheme.pack_geom(SCALARS, HANDS, SHADOWS, OVERLAY, TOURB,
                          RECTS, FIRSTS, DELAYS, BGF)
WORDS = list(struct.unpack("<" + "I" * (len(GEOM) // 4), GEOM))
HDR = orbtheme.pack_header("kit3-test-theme", "Kit Three (test)", len(GEOM),
                          ART_BYTES, zlib.crc32(GEOM) & 0xFFFFFFFF,
                          0, 466, 466, serial=7, baked_ms=99)
ART_OFF = orbtheme.art_offset(len(GEOM))
HDR_WORDS = list(struct.unpack("<" + "I" * orbtheme.HDR_WORDS, HDR))

def arr(name, words):
    body = ",".join(str(w & 0xFFFFFFFF) for w in words)
    return "static const uint32_t %s[%d] = {%s};\n" % (name, len(words), body)

# Per-field expectations, generated from the same tables the packer consumed: a field
# added to orbtheme.THEME_FIELDS adds its own assertion, so no field ships unchecked.

# Per-field expectations, generated from the same tables the packer consumed: a
# field added to orbtheme.THEME_FIELDS adds its own assertion, so no field can ship
# without being checked. RAW strings throughout this block on purpose -- the C these
# build needs real backslash escapes, and double-escaping them inside ordinary
# strings is how a printf ends up printing a literal "\n".
CKV = (r'#define CKV(got,want) do { long long g_ = (long long)(got); '
       r'if (g_ != (long long)(want)) { fails++; '
       r'printf("FAIL %s got %lld want %lld  <== the two sides disagree\n", '
       '#got, g_, (long long)(want)); } } while (0)' + "\n")
CK = (r'#define CK(st,f,want) do { long long g_ = (long long)((st).f); '
      r'if (g_ != (long long)(want)) { fails++; '
      r'printf("FAIL %s.%s got %lld want %lld  <== the two sides disagree\n", '
      '#st, #f, g_, (long long)(want)); } } while (0)' + "\n")

rows = []
# The wire names are positional; the struct keeps the five layer-order entries in
# one array. That is the only place the two vocabularies differ, and the mapping
# lives here rather than in the reader so a renamed field still has to be listed.
def acc(n):
    return "order[%s]" % n[5:] if n.startswith("order") and n != "order_n" else n
for n in SC_NAMES:
    rows.append('    CK(*G.theme, %s, %d);' % (acc(n), int(SCALARS[n])))
for tmpl, lst in (("hand[%d]", HANDS), ("shadow[%d]", SHADOWS)):
    for i, rec in enumerate(lst):
        for f in orbtheme.SPRITE_FIELDS:
            rows.append('    CK(G.theme->%s, %s, %d);' % (tmpl % i, f, int(rec[f])))
for f in orbtheme.SPRITE_FIELDS:
    rows.append('    CK(G.theme->overlay, %s, %d);' % (f, int(OVERLAY[f])))
for i, rec in enumerate(TOURB):
    for f in orbtheme.SPRITE_FIELDS:
        rows.append('    CK(G.theme->tourbillon[%d], %s, %d);' % (i, f, int(rec[f])))
for i in range(len(TOURB), 16):                       # the padded blanks
    for f in orbtheme.SPRITE_FIELDS:
        rows.append('    CK(G.theme->tourbillon[%d], %s, 0);' % (i, f))
for i in range(N_RECTS):
    for j, f in enumerate(("x", "y", "w", "h", "off")):
        rows.append('    CK(G.rects[%d], %s, %d);' % (i, f, int(RECTS[5 * i + j])))
for i, v in enumerate(FIRSTS): rows.append('    CKV(G.rect_first[%d], %d);' % (i, v))
for i, v in enumerate(DELAYS): rows.append('    CKV(G.gif_delays[%d], %d);' % (i, v))
for i, v in enumerate(BGF):    rows.append('    CKV(G.bg_frames[%d], %d);' % (i, v))
CHECKS = "\n".join(rows)
NROWS = len(rows)

# ---------------------------------------------------------------------------
# Refusal cases: the reader must say NO to the containers that would otherwise
# leave the dial reading outside its own mapping. Each is built by damaging the
# good one in exactly one place, so the only difference between an accept and a
# refuse here is the thing under test.
# ---------------------------------------------------------------------------
def mutate_words(ws, idx, val):
    ws2 = list(ws); ws2[idx] = val & 0xFFFFFFFF; return ws2

# Offsets into the blob's fixed part: 4 counts, the 26 scalars, then the sprite
# table in the order pack_geom writes it -- 5 hands, 5 shadows, the overlay and the
# 16-frame tourbillon bank -- then the variable tables, sized by those counts. A
# refusal case that aims at the wrong word aims at nothing, so the layout the writer
# uses is spelled out here once rather than guessed per case.
OFF_SCALAR  = 4
OFF_SPRITE  = OFF_SCALAR + len(orbtheme.THEME_FIELDS)
SPR_BYTES_N = len(orbtheme.SPRITE_FIELDS)
FIELD_W     = orbtheme.SPRITE_FIELDS.index("w")
SPRITE_N    = 5 + 5 + 1 + 16                       # hands, shadows, overlay, tourb
RECT_WORDS  = 5                                    # x, y, w, h, off
OFF_RECTS   = OFF_SPRITE + SPRITE_N * SPR_BYTES_N
OFF_FIRST   = OFF_RECTS + N_RECTS * RECT_WORDS
OFF_DELAY   = OFF_FIRST + N_FIRST

def scalar_i(name):
    return OFF_SCALAR + SC_NAMES.index(name)

# Each case pins the EXACT reason string the shipped reader must answer with.
# Asserting only "it refused" is not enough, and was not enough: with the tourbillon
# count check deleted the blob was still rejected by the pixel-extent check further
# down, the suite stayed green, and a regression that lets a theme whose second-hand
# bank runs past the array clock_view indexes would have shipped. A refusal for a
# different reason than the one under test is a failure.
REFUSALS = [
    ("trailing words the reader was not told about",
     "geometry blob has words this build does not recognise",
     lambda: (list(WORDS) + [0], ART_BYTES)),
    ("hand ink outside the payload (its width blown up)",
     "hand pixels lie outside the declared payload",
     lambda: (mutate_words(WORDS, OFF_SPRITE + 0 * SPR_BYTES_N + FIELD_W, 60000), ART_BYTES)),
    ("more tourbillon frames than the dial holds",
     "more tourbillon frames than the dial holds",
     lambda: (mutate_words(WORDS, scalar_i("tourbillon_n"), 999), ART_BYTES)),
    ("more GIF frames than the dial holds",
     "more GIF frames than the dial holds",
     lambda: (mutate_words(WORDS, scalar_i("gif_frames"), 900), ART_BYTES)),
    ("layer order longer than 5",
     "layer order longer than 5",
     lambda: (mutate_words(WORDS, scalar_i("order_n"), 9), ART_BYTES)),
    # Both delta-rect extents, the two checks clock_view.c:469 depends on: the
    # payload one (the byte run off+x..off+x+w*2 must exist) and the panel one
    # (mirrored at x,y it must stay on screen). Without these, deleting either check
    # from the reader left the suite green.
    ("delta rect past the end of the payload",
     "delta rect lies outside the declared payload",
     lambda: (mutate_words(WORDS, OFF_RECTS + 0 * RECT_WORDS + 4, 0x0FFFFFF0), ART_BYTES)),
    ("delta rect off the right edge of the panel",
     "delta rect lies outside the panel",
     lambda: (mutate_words(WORDS, OFF_RECTS + (N_RECTS - 1) * RECT_WORDS + 0, 500), ART_BYTES)),
    # The frame index table is walked as [first[i], first[i+1]) by clock_view, so an
    # index past the rect table reads whatever follows it.
    # The frame index table is walked as [first[i], first[i+1]) by clock_view, so an
    # index past the rect table reads whatever follows it.
    ("GIF frame index past the rect table",
     "frame index runs past the rect table",
     lambda: (mutate_words(WORDS, OFF_FIRST + 1, 4000), ART_BYTES)),
    # The two layers that are not sprites: the plate is read in every mode and the
    # GIF keyframe in mode 1, both full-screen, both straight from the card.
    ("plate plane past the end of the payload",
     "plate pixels lie outside the declared payload",
     lambda: (mutate_words(WORDS, scalar_i("plate_off"), ART_BYTES), ART_BYTES)),
    ("GIF keyframe past the end of the payload",
     "GIF keyframe lies outside the declared payload",
     lambda: (mutate_words(WORDS, scalar_i("bg_key_off"), ART_BYTES), ART_BYTES)),
    # A shadow is only read when shadow_on, which the sentinel blob sets.
    ("shadow ink past the end of the payload",
     "shadow pixels lie outside the declared payload",
     lambda: (mutate_words(WORDS, OFF_SPRITE + (5 + 1) * SPR_BYTES_N
                          + orbtheme.SPRITE_FIELDS.index("color_off"), 0x7FFF0000), ART_BYTES)),
    # The overlay is composited over the whole dial whenever the bake carries ink for it.
    ("overlay ink past the end of the payload",
     "overlay pixels lie outside the declared payload",
     lambda: (mutate_words(WORDS, OFF_SPRITE + 10 * SPR_BYTES_N
                          + orbtheme.SPRITE_FIELDS.index("color_off"), 0x7FFF0000), ART_BYTES)),
]

REFUSE_CALLS = []
for i, (label, why, mk) in enumerate(REFUSALS):
    ws, art = mk()
    REFUSE_CALLS.append(arr("bad%d" % i, ws))
    # The reason travels as a plain argument, so a quote or a non-ASCII dash inside
    # one of these messages can never corrupt the generated source; it just has to
    # strcmp equal to what the shipped reader returns.
    REFUSE_CALLS.append('    rblob("%s", "%s", bad%d, %d, %uu);'
                        % (label, why, i, len(ws), art))
REFUSE_CALLS = "\n".join(REFUSE_CALLS)

# The header's own refusals, the same idea one level up. Mutating a word invalidates
# the self-checksum, which would answer every one of these before the check under
# test ran; the checksum is therefore recomputed per case, and the torn-header case
# is the one that deliberately leaves it stale.
def _recrc(hw):
    hw = list(hw); hw[orbtheme.W_HDR_CRC] = 0
    hw[orbtheme.W_HDR_CRC] = orbtheme._hdr_crc(hw)
    return hw

HDR_BAD = [("wrong magic", orbtheme.W_MAGIC, 0xDEADBEEF, "not an .orbtheme file"),
           ("unknown format", orbtheme.W_FORMAT, 99,
            "theme format this firmware cannot read"),
           ("geometry ABI from another firmware", orbtheme.W_GEOM_ABI, 99,
            "baked for a different dial — re-bake it"),
           ("a panel this firmware was not built for", orbtheme.W_SCR_W, 240,
            "baked for a different panel size"),
           ("implausible geometry length", orbtheme.W_GEOM_SIZE, 8,
            "geometry blob size is not credible for this dial"),
           ("pixel payload bigger than a slot", orbtheme.W_ART_SIZE, 0x0FFFFFFF,
            "pixel payload is bigger than a theme slot — re-bake it")]
HDR_REFUSALS = []
for j, (label, widx, val, why) in enumerate(HDR_BAD):
    hw = list(HDR_WORDS); hw[widx] = val & 0xFFFFFFFF
    HDR_REFUSALS.append(arr("hbad%d" % j, _recrc(hw)))
    HDR_REFUSALS.append(('    rhdr("%s", "%s", hbadJ);' % (label, why))
                        .replace("hbadJ", "hbad%d" % j))
# A torn header: one byte flipped in the title, which the self-checksum must catch.
_t = list(HDR_WORDS); _t[orbtheme.W_TITLE + 1] ^= 0x20
HDR_REFUSALS.append(arr("hbad_torn", _t))
HDR_REFUSALS.append('    rhdr("torn header, stale checksum", '
                    '"container header is damaged — re-copy the file onto the card", hbad_torn);')
HDR_REFUSALS = "\n".join(HDR_REFUSALS)


# The lifted helpers the shipped code calls. Renamed rather than link-interposed:
# glibc's own strlcpy appeared only in 2.38, so a definition here would collide with
# the system's on a new distro and be missing on an old one. host_strlcpy is a
# faithful copy for these two call sites, and the rename keeps both cases building.
HELPERS = r'''
static size_t host_strlcpy(char *dst, const char *src, size_t siz)
{
    char *d = dst;
    const char *s = src;
    size_t n = siz;
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
f_name_ok  = f_name_ok.replace("strlcpy", "host_strlcpy").replace("strlcat", "host_strlcat")
f_theme_pt = f_theme_pt.replace("strlcpy", "host_strlcpy").replace("strlcat", "host_strlcat")
pure_theme = pure_theme.replace("strlcpy", "host_strlcpy").replace("strlcat", "host_strlcat")

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
    ncase("a\tb", 0);
    ncase("caf\xe9", 0);                             /* UTF-8: refused, not mangled */
    ncase(NULL, 0);'''

# Built from the constant the lifted code uses, never counted by eye: the pair this
# replaces was meant to straddle the 31-byte cap and actually measured 30 and 31, so
# it proved nothing at the boundary while still printing "ok".
# The two paths the device will use come from its own defines, not from strings
# restated here: the day ORB_SD_BASE moves off /sd, a test that hardcoded it would
# keep passing against a firmware writing somewhere else.
def _must(rx, src, what):
    m = re.search(rx, src)
    if not m:
        sys.exit("the test can no longer read %s out of the shipped source: %s" % (what, rx))
    return m.group(1)

_NAME_CAP = int(_must(r"#define ORB_SD_NAME_MAX\s+(\d+)", sd_consts, "ORB_SD_NAME_MAX"))

_sd_base  = _must(r'#define ORB_SD_BASE\s+"([^"]*)"', sd_consts, "ORB_SD_BASE")
_theme_dir = _sd_base + _must(r'#define ORB_SD_THEME_DIR\s+ORB_SD_BASE\s+"([^"]*)"',
                              sd_consts, "ORB_SD_THEME_DIR")
_theme_ext  = _must(r'"([^"]*)"', ext_define, "ORB_THEME_EXT")
_long_slug  = "0" * _NAME_CAP
_long_path  = _theme_dir + "/" + _long_slug + _theme_ext

NAME_ROWS = (NAME_ROWS.replace("@CAP31@", "0" * _NAME_CAP)
                       .replace("@CAP32@", "0" * (_NAME_CAP + 1)))

PATH_ROWS = '''    pcase("kit3", 1, "%s");
    pcase("a.b-c_1", 1, "%s");
    pcase("../etc", 0, 0);
    pcase("a/b", 0, 0);
    pcase("", 0, 0);
    pcase(NULL, 0, 0);''' % (_theme_dir + "/kit3", _theme_dir + "/a.b-c_1")

# theme_file() is the last thing between ?name= and a path on the card and the only
# place the extension is added, so its boundary is measured rather than assumed: the
# size that must fit exactly, and one byte less, which must be refused instead of
# writing a truncated name that points at a different file.
# The doubled-extension row is a decision, not an oversight: stripping a trailing
# ".orbtheme" would make the names "x" and "x.orbtheme" land on one file, so one
# upload could quietly overwrite another theme. Keeping the doubling means a slug maps
# to exactly one path, which is what the page's load and delete buttons rely on.
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
    _long_slug, _long_path, len(_long_path) + 1,
    _long_slug, len(_long_path),
    _theme_dir + "/x" + _theme_ext + _theme_ext)

HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define ORB_THEME_SLUG_MAX 31
@HELPERS@
@CRC@
@TYPES@
@GEOT@
@CONSTS@
@NAMEOK@
@THEMEPATH@
@EXT@
@THEMEFILE@
@PURE@
@CKV@
@CK@
@ARRAYS@

static orb_theme_geo_t G;
static clk_theme_t     T1, T2;
static clk_bg_rect_t   r1[64], r2[64];
static uint16_t        f1[64], d1[64], f2[64], d2[64];
static uint32_t        b1[64], b2[64];
static int fails = 0;

static void ncase(const char *n, int want)
{
    const int got = orb_sd_name_ok(n);
    if (got != want) fails++;
    printf("%-5s name_ok(%-34s) -> %d (want %d)\n", got == want ? "ok" : "FAIL",
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
/* The destination the device writes to, exercised with a buffer the caller sized by
 * hand. The canary tail is the point: the fit check inside theme_file() is what
 * stops a long slug coming back as a cut-off filename, and deleting that check has
 * to be visible as bytes written past the caller's outsz, not merely as a wrong
 * return code. */
static char fbuf[128 + 16];
static void fcase(const char *n, int want, const char *wantpath, unsigned outsz)
{
    memset(fbuf, 0x5a, sizeof fbuf);
    const int ok = theme_file(n, fbuf, outsz);
    int pass = (ok == want) && (!want || strcmp(fbuf, wantpath) == 0);
    /* A refused name must clear out[0]: a caller that logged the path it was handed
     * would otherwise name a file the card never got, and the page would list a theme
     * that is not there. */
    if (pass && !want) pass = (fbuf[0] == '\0');
    for (unsigned i = outsz; pass && i < sizeof fbuf; i++)
        if ((unsigned char)fbuf[i] != 0x5a) pass = 0;
    if (!pass) fails++;
    printf("%-5s theme_file(%-20s, outsz=%2u) -> %s%s\n", pass ? "ok" : "FAIL",
           n ? n : "(null)", outsz, ok ? fbuf : "(refused)", pass ? "" : "   <== BAD DESTINATION");
}

/* One refusal under test: build the geo destination fresh each time so a previous
 * case cannot leave a field that makes the reader stop somewhere else, and compare
 * the reason string exactly. Silent acceptance and a refusal for an unrelated
 * reason are both failures -- see the note by REFUSALS. */
static void rblob(const char *what, const char *want, const uint32_t *w,
                  uint32_t n, uint32_t art)
{
    orb_theme_geo_t R;
    memset(&R, 0, sizeof R);
    R.theme = &T2; R.rects = r2; R.rect_first = f2; R.gif_delays = d2; R.bg_frames = b2;
    const char *got = orb_theme_geom_parse(w, n, art, &R);
    if (!got)     { fails++; printf("FAIL blob '%s' was ACCEPTED; wanted '%s'\n", what, want); }
    else if (strcmp(got, want))
                  { fails++; printf("FAIL blob '%s' refused as '%s'; wanted '%s'\n", what, got, want); }
    else          { printf("ok    blob refused as designed: %s\n", got); }
}
static void rhdr(const char *what, const char *want, const uint32_t *w)
{
    orb_thdr_t o;
    const char *got = hdr_parse(w, &o);
    if (!got)     { fails++; printf("FAIL header '%s' was ACCEPTED; wanted '%s'\n", what, want); }
    else if (strcmp(got, want))
                  { fails++; printf("FAIL header '%s' refused as '%s'; wanted '%s'\n", what, got, want); }
    else          { printf("ok    header refused as designed: %s\n", got); }
}

int main(void)
{
    puts("== the crc32 the device's ROM routine is supposed to agree with ==");
    {
        static const uint8_t v[9] = {'1','2','3','4','5','6','7','8','9'};
        if (host_crc32(0, v, 9) != 0xCBF43926u) { fails++; puts("FAIL crc32 stub"); }
        else puts("ok    matches CRC-32/ISO-HDLC's published check value");
    }
    puts("== theme names a browser could send ==");
@NAMES@
    puts("== where a name is allowed to point ==");
@PATHS@
    puts("== the file the card will be written under ==");
@FILECASES@
    puts("== the container header, read by the shipped reader ==");
    {
        orb_thdr_t o;
        const char *r = hdr_parse(hdr_ok, &o);
        if (r) { fails++; printf("FAIL good header refused: %s\n", r); }
        else {
            puts("ok    shipped reader accepted the shipped writer's header");
            CK(o, geom_size, @GEOMLEN@);
            CK(o, art_size,  @ART@);
            CK(o, serial,    7);
            CKV(strcmp(o.slug,  "kit3-test-theme"), 0);
            CKV(strcmp(o.title, "Kit Three (test)"), 0);
        }
    }
    puts("== the geometry blob, field by field, through the shipped reader ==");
    {
        G.theme = &T1; G.rects = r1; G.rect_first = f1; G.gif_delays = d1; G.bg_frames = b1;
        const char *why = orb_theme_geom_parse(good, @NW@, @ART@u, &G);
        if (why) { fails++; printf("FAIL the writer's own blob was refused: %s\n", why); }
        else printf("ok    @NROWS@ fields agreed between the Python writer and the C reader\n");
@CHECKS@
    }
    puts("== the same reader's refusals ==");
@REFUSES@
    puts("== the header's refusals ==");
@HDRREFUSES@
    return fails != 0;
}
'''

for k, v in (("@HELPERS@", HELPERS), ("@CRC@", CRC_STUB), ("@TYPES@", gen_types),
             ("@GEOT@", geo_t), ("@CONSTS@", sd_consts), ("@NAMEOK@", f_name_ok),
             ("@THEMEPATH@", f_theme_pt), ("@PURE@", pure_theme), ("@CKV@", CKV),
             ("@EXT@", ext_define), ("@THEMEFILE@", f_theme_file),
             ("@FILECASES@", FILE_ROWS),
             ("@CK@", CK), ("@ARRAYS@", arr("good", WORDS) + arr("hdr_ok", HDR_WORDS)),
             ("@NAMES@", NAME_ROWS), ("@PATHS@", PATH_ROWS), ("@CHECKS@", CHECKS),
             ("@REFUSES@", REFUSE_CALLS), ("@HDRREFUSES@", HDR_REFUSALS),
             ("@NW@", str(len(WORDS))), ("@ART@", str(ART_BYTES)),
             ("@GEOMLEN@", str(len(GEOM))), ("@NROWS@", str(NROWS))):
    HARNESS = HARNESS.replace(k, v)
if re.search(r"@[A-Z]+@", HARNESS):
    # An unfilled token means a template hole; finding it as a wall of gcc errors
    # three layers down costs more than saying it now. (Matching the token shape
    # rather than any '@' is deliberate: the lifted comments legitimately contain
    # the card-id example string "@20000kHz".)
    sys.exit("leftover token in the harness:\n" +
             "\n".join(l for l in HARNESS.split("\n") if re.search(r"@[A-Z]+@", l))[:800])

BIN = "/tmp/orb_sd_host_" + hashlib.md5((sd_src + th_src).encode()).hexdigest()[:10]

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
    # bytes, not text: one case feeds the reader a raw 0xE9 in a name, and the
    # harness echoes it back inside its verdict. Decoding strictly would raise here
    # and look like a test failure when it is only a print.
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
    # Each fragment arrives as a C string literal; C-unescape them individually, the
    # way the compiler does, then concatenate. Unescaping the joined text instead would
    # let one fragment's trailing double backslash eat the next one's opening quote --
    # the exact class of error that hides a broken page from every other check here.
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
            esprima.parseScript(js)
            js_ok = True
        except Exception as e:
            print("FAIL /themes JS: %s — one syntax error kills the whole <script> and "
                  "the page sits at 'loading' with an empty list" % e)
            js_ok = False
    # Pins: the decisions the page must keep making, each a substring that a lazy
    # edit would drop. The confirm() on delete is the one that matters most — this
    # button deletes a file from somebody's card.
    pins = [("onclick=doUp()", "an upload path exists"),
            ("function del(n)", "delete is offered"),
            ("if(!confirm(", "delete asks before destroying a file"),
            ("x.upload.onprogress", "the upload reports real progress (fetch cannot)"),
            ("/api/v1/sd/load?name=", "load goes through the staging endpoint"),
            ("/api/v1/reboot", "the page asks before rebooting the orb"),
            ("CANNOT LOAD", "an unloadable theme says why instead of vanishing")]
    rc = 0 if js_ok else 1
    for want, why in pins:
        # the page, not just the script: the upload button lives in the markup, and
        # searching only the JS would report a present feature as missing
        if want not in page:
            print("FAIL /themes: missing %r — %s" % (want, why)); rc = 1
    # The call lint: a call to a function that is not defined parses fine and simply
    # does nothing on the phone (this exact bug already happened once on these pages).
    defined = set(re.findall(r"function\s+([A-Za-z_$][\w$]*)", js))
    # (?<![.]) is the whole trick here: without it every method call -- Math.floor,
    # document.getElementById, x.send -- reads as a missing global function.
    called  = set(re.findall(r"(?<![.\w$])([A-Za-z_$][\w$]*)\s*\(", js))
    builtin = {"function", "if", "for", "while", "switch", "catch", "return", "typeof",
               "fetch", "String", "Number", "JSON", "Math", "Date", "Array", "parseInt",
               "encodeURIComponent", "XMLHttpRequest", "confirm", "alert", "setTimeout"}
    undef = sorted(c for c in called - defined - builtin if not c[0].isupper())
    if undef:
        print("FAIL /themes JS calls functions that do not exist: %s" % undef); rc = 1
    if not rc:
        print("ok    /themes (%d B page) parses, is pinned, and calls only functions it has"
              % len(page))
    return rc

MUTATIONS = [
    # (name, which file, old text, new text). Each must make SOME check go red; a
    # pin you can delete without a failure is not a pin.
    # The guard is an allow-list, not a block-list, so the regression that matters is
    # the list growing a '/' -- which is what hands ?name=../x back a real path.
    ("traversal: '/' allowed in a name",            "sd",  "(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'",
                                                     "(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/'"),
    ("name length cap raised away",                  "sd",  "if (n >= ORB_SD_NAME_MAX) return false;", "/* no cap */"),
    ("path builder stops validating the name",       "sd",  "if (!orb_sd_name_ok(name)) { out[0] = '\\0'; return false; }", ""),
    ("leading dot allowed (hides the entry)",        "sd",  "if (name[0] == '.') return false;", ""),
    ("reader stops checking trailing words",         "th",  "if (r.i != r.n) return \"geometry blob has words this build does not recognise\";", ""),
    ("reader stops checking rect vs panel",          "th",  "return \"delta rect lies outside the panel\";", "return NULL;"),
    ("reader stops checking rect vs payload",        "th",  "return \"delta rect lies outside the declared payload\";", "return NULL;"),
    ("reader stops checking the frame index table",  "th",  "if (out->rect_first[i] > n_rects) return \"frame index runs past the rect table\";", ""),
    ("reader stops checking the plate extent",       "th",  "return \"plate pixels lie outside the declared payload\";", "return NULL;"),
    ("reader stops checking shadows",                "th",  "return \"shadow pixels lie outside the declared payload\";", "return NULL;"),
    ("reader stops checking the GIF frame count",    "th",  "if (t->gif_frames > GIFS_MAX)      return \"more GIF frames than the dial holds\";", ""),
    ("theme file stops checking the extension fits", "th",  "if (blen + sizeof(ORB_THEME_EXT) > outsz) { out[0] = '\\0'; return false; }",
                                                     "if (0 && blen + sizeof(ORB_THEME_EXT) > outsz) { out[0] = '\\0'; return false; }"),
    ("theme file trusts the caller over the validator", "th", "if (!orb_sd_theme_path(base, sizeof base, slug)) { out[0] = '\\0'; return false; }",
                                                     "if (!orb_sd_theme_path(base, sizeof base, slug)) { return false; }"),
    ("reader stops checking tourbillon count",       "th",  "if (t->tourbillon_n > TOURB_MAX)   return \"more tourbillon frames than the dial holds\";", ""),
    ("header stops checking its own checksum",       "th",  "return \"container header is damaged — re-copy the file onto the card\";", "return NULL;"),
    ("/themes loses the delete confirmation",        "net", "if(!confirm(", "if(true && ("),
    ("/themes loses the upload progress handler",    "net", "x.upload.onprogress", "x.noupload_onprogress"),
    ("/themes calls a function that does not exist", "net", "function refresh()", "function refreshX()"),
    # The bug this file found for real: an esp_err_t read as a truth value. ESP_OK is
    # 0, so this inverted form refuses every card that is actually in the socket.
    ("install tests the mount result as a boolean",  "th",  "if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;",
                                                     "if (!orb_sd_ensure_mounted()) return ESP_ERR_NOT_FOUND;"),
    # The bug the hardware found for real: verify_container() reached s_chunk while it
    # was still NULL, because only the OTHER two users of that buffer reserved it. The
    # device rebooted at the end of a completed 3.87 MB upload.
    ("upload verify reads through an unreserved buffer", "th",
     'if (ensure_chunk() != ESP_OK) return "out of memory for the card read buffer";', ""),
    ("installer copies through an unreserved buffer",   "th",
     'if (ensure_chunk() != ESP_OK) { *why = "out of memory for the card read buffer"; return ESP_ERR_NO_MEM; }', ""),
    # The bug the card found for real: scan holds orb_sd_lock() and card_open() takes it
    # again. The walk ran zero times on an empty card, so only hardware could show it.
    ("scan re-asks for the card lock inside its walk", "th",
     "const int fd = card_open_locked(slug, &h, &why);", "const int fd = card_open(slug, &h, &why);"),
    # The second one hardware found: the lock-free core asking for the mount, which is
    # the same mutex again. This is the bug the plain call-graph version of the lint
    # missed, because ensure_mounted is legitimate a few lines EARLIER in the caller.
    ("locked core re-asks for the mount it is holding", "th",
     "if (!orb_sd_mounted())", "if (orb_sd_ensure_mounted() != ESP_OK)"),
    # Rule 2's own regression: the mount check deleted from the wrapper. The device still
    # boots, still lists the card, and the install burns an attempt for a file that is
    # there — the only thing that catches it is this line plus a real boot.
    ("card_open stops mounting before it locks", "th",
     "    if (orb_sd_ensure_mounted() != ESP_OK) { *why = \"no card in the socket\"; return -1; }\n",
     ""),
]

def run_mutations():
    import tempfile, shutil
    rc = 0
    for name, which, old, new in MUTATIONS:
        path = {"sd": SD_C, "th": TH_C, "net": NET_C}[which]
        src  = {"sd": sd_src, "th": th_src, "net": net_src}[which]
        # An anchor may legitimately occur twice: orb_sd.c carries the pure helpers
        # once for the card-on build and once for the card-off stub. The lift copies
        # the FIRST occurrence, so mutating the first keeps the battery and the
        # harness aimed at the same text; zero occurrences still means the anchor no
        # longer exists and the battery is stale.
        if src.count(old) < 1:
            print("FAIL mutation %r: anchor found %dx, want at least 1 — the battery is stale"
                  % (name, src.count(old))); rc = 1; continue
        tmp = tempfile.mkdtemp()
        # Reproduce the tree the lifts read from, with the one file mutated.
        for real in (SD_C, SD_H, TH_C, TH_H, GEN_H, NET_C, P("tools", "orbtheme.py")):
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

def err_t_lint():
    """Catch the esp_err_t-tested-as-boolean class of bug, whole.

    ESP_OK is 0, so `if (!orb_sd_ensure_mounted())` reads as "if the card IS
    mounted, bail out" — an upload that refuses every good card and proceeds on an
    empty socket. It compiles, it warns about nothing, and no bench without a card in
    the socket can see it. The fix is a lint over the shipped sources: any `!` in
    front of a call to a function the header declares esp_err_t is a bug. bool
    returns (orb_sd_name_ok, orb_sd_theme_path, orb_theme_upload_busy) are excluded by
    reading the return type out of the header rather than guessing at the call site.
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
                print("FAIL %s:%d: `!%s(...)` tests an esp_err_t for truth — ESP_OK is 0, "
                      "so this is the mount/install result inverted. Compare against "
                      "ESP_OK instead." % (where, line, fn))
                rc = 1
    if not rc:
        print("ok    %d esp_err_t entry points called, none tested as a boolean"
              % len(errs))
    return rc


def _code_only(src):
    """The source with comments and string/char literals blanked, line count preserved.

    Both lints read call sites out of the text. Without this, a doc comment that says
    "card_open() reserves it" becomes a call to card_open(), and so does any function
    name that happens to appear inside a log message — which produced three deadlocks
    on the first run of this check, none of them real.
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
        if c == "\\":                          # keep the escape pair from ending the literal
            out.append("  "); i += 2; continue
        if c == q:
            state = None; out.append(c); i += 1; continue
        out.append("\n" if c == "\n" else " "); i += 1; continue
    return "".join(out)


def _defs(lines):
    """Every column-0 definition in a source as (name, first_body_line, last_body_line).

    These files are consistently formatted: a definition begins at column 0 and its
    closing brace is alone at column 0. Declarations end with ';' and are skipped, so a
    forward declaration cannot claim whatever body happens to follow it.
    """
    funcs = []
    i = 0
    while i < len(lines):
        L = lines[i]
        if L and L[0] not in " \t/#}" and "(" in L and not L.startswith(("typedef", "enum", "struct")):
            # Count braces from the signature line: a definition may open and close on
            # one line (the card-off stubs are written that way), and waiting for a line
            # that ENDS in '{' makes the scan walk into the next real function and claim
            # its body, its locks and its buffers.
            depth, seen, j = 0, False, i
            while j < len(lines):
                t = lines[j]
                if not seen and t.rstrip().endswith(";"):
                    break                                   # a declaration, not a body
                depth += t.count("{") - t.count("}")
                if "{" in t:
                    seen = True
                if seen and depth <= 0:
                    break
                j += 1
            if seen and j - i < 400:                        # a runaway parse is not a function
                m = re.search(r"([A-Za-z_][A-Za-z_0-9]*)\s*\(", " ".join(lines[i:j + 1]))
                if m:
                    funcs.append((m.group(1), i, j))
                i = max(j, i) + 1      # always advance: a one-line definition balances
                continue               # braces on its own line and j would never move
        i += 1
    return funcs


def chunk_lint():
    """Every function that reads through the shared card buffer must reserve it.

    s_chunk is the one 4 KB internal-RAM buffer every card operation reads through, and
    ensure_chunk() is the only thing that makes it non-NULL. Two callers reserved it and
    the third user did not: orb_theme_upload_end() opens its .part with a bare open(), so
    on a boot that had never staged a theme verify_container() pread() into a NULL buffer,
    FatFs stored through the NULL, and the dial died with a StoreProhibited at the end of
    an otherwise successful 3.87 MB upload. The harness lifts these functions and mallocs
    the buffer itself, so it cannot see a missing reserve — the check has to be over the
    shipped source, the same way the esp_err_t lint is.
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
    already holds it never returns. Hardware found two of these, both in the scan path
    and both invisible until a real theme sat in the directory: orb_theme_scan() walked
    the card holding the lock and validated entries through card_open(), which took it,
    and the split-off core then still called orb_sd_ensure_mounted(), which takes it
    too. The device's own log said "waiting for the card: held by \"httpd\" for 16002
    ms" — one blocked handler is every route dead, because esp_http_server services
    sessions from a single task, while the dial kept drawing and ping kept replying.

    So this is not a plain call-graph check: an unlocked call to ensure_mounted is fine
    (orb_theme_scan() makes one before it locks) and the same call after the lock is the
    bug. What is examined is the calls made between a take and the give that matches it,
    followed through the local call graph, plus the functions that hold the lock on
    entry by contract (upload_end, upload_cancel) — everything they reach is held.
    """
    lines = _code_only(th_src).split("\n")
    funcs = _defs(lines)
    span = {n: (a, b) for n, a, b in funcs}
    text = {n: "\n".join(lines[a:b + 1]) for n, a, b in funcs}
    # orb_sd.c side of the same mutex. The card-off stubs and the pure helpers are not
    # here because they take nothing: orb_sd_space()'s comment is explicit that it runs
    # outside the lock on purpose, and name_ok/theme_path never touch the volume.
    cross = {"orb_sd_ensure_mounted", "orb_sd_remount", "orb_sd_init"}
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
                # The signature line is part of the span, so a function always "calls"
                # itself once. Self-recursion under a held mutex would be a deadlock, but
                # none of these functions recurse, and reporting the declaration as a call
                # would bury the real ones.
                and m.group(1) != owner]

    def held_spans(n):
        """Lines in n during which this task holds the mutex.

        First take to last give, not a depth walk: orb_theme_scan() releases on its
        opendir-failure early return (line 1160) and again at the end (1215), so a
        depth-tracked span closes at the first give and reports the whole directory
        walk — the exact code that must not re-lock — as unlocked. The conservative
        version costs nothing here, because no holder in this file calls a locker after
        its final give.
        """
        if n in contract:
            return [span[n]]
        a, b = span[n]
        takes = [k for k in range(a, b + 1) if "orb_sd_lock()" in lines[k]]
        if not takes:
            return []
        gives = [k for k in range(a, b + 1) if "orb_sd_unlock()" in lines[k]]
        return [(takes[0], gives[-1] if gives else b)]   # no give: released by the pair

    def walk(seeds, start):
        # Reaching ANY lock taker while holding is the deadlock — its first act is a take
        # — so holders are in the danger set, and only the starting function itself is
        # excluded (no function here recurses, and its own signature line counts as a
        # call to itself).
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
                      "while holding it — the second take never returns, which kills every "
                      "route behind the one httpd worker; call the _locked variant"
                      % (a + 1, n, " -> ".join(bad)))
                rc = 1

    # Rule 2, the one that cost a whole boot cycle to find: a lock holder must already be
    # mounted, because mounting is the one card operation that cannot happen while the
    # mutex is held (orb_sd_ensure_mounted() takes it). card_open() lost its mount check
    # to the core in the refactor, orb_theme_boot() runs before orb_sd_init(), and the
    # staged install quietly burned attempt 1 of 3 over a file the card had all along.
    for n in sorted(holders):
        a, b = span[n]
        first = next((k for k in range(a, b + 1) if "orb_sd_lock()" in lines[k]), None)
        if first is None:
            continue
        if not any("orb_sd_ensure_mounted()" in lines[k] for k in range(a, first)):
            print("FAIL main/art/orb_theme.c:%d: %s() takes orb_sd_lock() at line %d without "
                  "calling orb_sd_ensure_mounted() first — nothing else on this path mounts, "
                  "and a boot install runs before orb_sd_init() ever is called"
                  % (a + 1, n, first + 1))
            rc = 1
    if not rc:
        print("ok    %d card-lock holders: none reaches a second take, all mount before "
              "locking (%d cross-file lockers accounted for)"
              % (len(holders | contract), len(cross)))
    return rc


if "--mutations" in sys.argv:
    sys.exit(run_mutations())

rc = run_harness()
if rc == 0:
    rc = themes_page_js()
if rc == 0:
    rc = err_t_lint()
if rc == 0:
    rc = chunk_lint()
if rc == 0:
    rc = lock_lint()
sys.exit(rc)

