#!/usr/bin/env python3
"""Bench check for the microSD + theme-install feature.

    python3 tools/host_sd_test.py               # all checks
    python3 tools/host_sd_test.py --mutations   # do the checks actually bite?

Why these checks exist on a bench that has no card in the socket and no radio:

1. PATH SAFETY. orb_sd_name_ok()/orb_sd_theme_path() decide whether a string that
   arrived over the radio becomes a path on the card. Under FORMAT 2 the theme
   endpoints hand them only a ?id= — 12 chars the device issued itself, matched
   against sha256() of the container's own header name before any path is built —
   and the card's directory entries pass through them too. Pure functions,
   exhaustively testable here, and the last thing between a crafted string and
   the rest of the volume.

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

4. THE GEOMETRY ARENA. geom_validate_blob() mallocs the one arena every table of
   a theme lives in, and on the adopt path must NEVER free it: clock_view.c caches
   raw pointers into those tables, so a free after the publish is a use-after-free
   the owner reads as a scrambled dial, not an error. The other two exits (parse
   refusal, adopt=false pre-flight) must give their bytes back. Nothing else
   off-target compiles that function, so the harness lifts it with an accounting
   allocator and runs both paths; a lint over its own text pins where the two
   frees are allowed to sit. The vectors and the lint each see what the other is
   blind to — see THE GEOMETRY ARENA block below.

The lifts fail LOUDLY if the code they extract has moved: a lift that silently came
back empty would keep reporting green while testing nothing.

Needs esprima for the page check (pip install esprima); a missing parser is a loud
SKIP on that family, never a pass.
"""
import hashlib, json, os, re, struct, subprocess, sys, zlib

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
       else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, *a)
SD_C, SD_H = P("main", "sd", "orb_sd.c"),   P("main", "sd", "orb_sd.h")
TH_C, TH_H = P("main", "art", "orb_theme.c"), P("main", "art", "orb_theme.h")
B3_C, B3_H = P("main", "art", "orb_bundle.c"), P("main", "art", "orb_bundle.h")
NET_C      = P("main", "net", "orb_net.c")
GEN_H      = P("assets", "clock_assets.h")
for p in (SD_C, SD_H, TH_C, TH_H, B3_C, B3_H, NET_C, GEN_H):
    if not os.path.exists(p):
        sys.exit("missing " + p + " — the lifts below have no fallback")

sd_src, th_src = open(SD_C).read(), open(TH_C).read()
th_hdr, sd_src_h = open(TH_H).read(), open(SD_H).read()
net_src, gen_src = open(NET_C).read(), open(GEN_H).read()
if "--src" in sys.argv:                      # mutation runs re-point the NET lifts
    net_src = open(sys.argv[sys.argv.index("--src") + 1]).read()

sys.path.insert(0, P("tools"))
import orbtheme
import theme_schema as ts

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
# The card read buffer and its granting ladder, lifted verbatim for the same reason as
# the arena: the board refused the flat 4 KB ask (docs 16.1 -- upload 507 at largest
# block 3,328), and what the grant lands at is now a fact the vectors have to pin
# against the shipped text rather than a comment. The whole statics-plus-function block
# comes over, defines included, so CHUNK_MIN cannot be restated wrong here.
f_chunk = lift(th_src, r"static uint8_t \*s_chunk;[\s\S]*?\n\}", TH_C)
f_theme_file = lift(th_src, r"static bool theme_file\(const char \*stem[\s\S]*?\n\}", TH_C)
# The §6 installed-slot comparison, lifted for the same reason: it decides which row
# the page calls "on the dial" and which one it offers to re-install, and getting it
# wrong is a silent no-op Load or a re-install offer on the theme you are wearing.
# theme_id()/theme_id_ok() need no separate lift — they sit between the word map and
# the geometry parser, so @PURE@ already carries the shipped copies, and the harness
# calls them directly.
f_theme_state = lift(th_src, r"static void theme_state\(const orb_thdr_t \*h,[\s\S]*?\n\}", TH_C)
f_theme_state = f_theme_state.replace("strlcpy", "host_strlcpy").replace("strlcat", "host_strlcat")
ext_define = lift(th_hdr, r"#define ORB_THEME_EXT\b[^\n]*", TH_H)

# clk_theme_t / clk_sprite_t / clk_bg_rect_t and CLK_SCR_W/H, from the committed
# GENERATED header — not restated, because a restatement could pass while the device
# compiles against different types.
i0 = gen_src.index("#define CLK_SCR_W")
gen_types = gen_src[i0:gen_src.index("} clk_bg_rect_t;") + len("} clk_bg_rect_t;")]

geo_t = lift(th_hdr, r"typedef struct \{\s*clk_theme_t\s+\*theme;[\s\S]*?\} orb_theme_geo_t;", TH_H)

# The complication/indicator records: the kind/source/flag enums, the two caps the
# reader refuses a strip over, and the two structs. Lifted, not restated, for the
# same reason as the generated types above — a restatement could pass while the
# device compiles against a different field order, and the field order IS the
# contract between the bake and the dial.
rec_types = lift(th_hdr, r"enum \{\s*CLK_KIND_WINDOW[\s\S]*?\} clk_aux_t;", TH_H)

# The caps the loader refuses over, read out of the shipped .c rather than typed in
# here, so the pair below compares the two definitions of the same number instead
# of comparing one of them with itself.
def cdefine(sym):
    """A loader constant, read out of the shipped .c. A define whose value is itself a
    define (BANK_WORDS is SPRITE_WORDS_N) is followed one level, because the number
    the device uses is the one at the end of that chain."""
    m = re.search(r"#define " + sym + r"\s+([A-Za-z_]\w*|\d+)[^\n]*", th_src)
    if not m:
        sys.exit("could not find #define " + sym + " in " + TH_C + " — did the caps move?")
    v = m.group(1)
    if not v.isdigit():
        m2 = re.search(r"#define " + v + r"\s+\(?(\d+)\)?[^\n]*", th_src)
        if not m2:
            sys.exit("#define " + sym + " is " + v + ", and " + v +
                     " is not defined in " + TH_C + " — the chain the caps use is broken")
        v = m2.group(1)
    return int(v)

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
#define ESP_ERR_NO_MEM (-100)   /* esp_err.h; the card buffer ladder answers it */

/* theme_id() asks mbedtls for the SHA-256 of the name. The device uses the ROM
 * implementation; this host build substitutes a HOOK instead of a second SHA-256,
 * and the hook is the whole point of the arrangement:
 *
 *   - it RECORDS the exact bytes and length the shipped theme_id() handed to the
 *     hash, so the vectors assert the identity is sha256(name up to its NUL). A
 *     change of hash input — the padded 56-byte array, the serialized fields, a
 *     lowercased name — changes the recorded bytes or length and goes red, which is
 *     the mutation that silently renames every theme on the card.
 *   - it returns the digest Python computed for those same bytes, so the hex loop
 *     that turns a digest into the 12-char stem is the shipped code under test and
 *     is pinned to hashlib's answer rather than to a host copy of SHA-256 that could
 *     be wrong in exactly the way a real one is not.
 *
 * The digest is supplied per row by icase() below, never computed here. */
static const unsigned char *s_hash_in;      /* what theme_id() asked to hash */
static size_t               s_hash_len;
static const unsigned char *s_hash_want;    /* the digest the row installed */
static int                  s_hash_is224;   /* must stay 0: 224 would truncate */
static int host_sha256_hook(const unsigned char *in, size_t ilen,
                            unsigned char out[32], int is224)
{
    s_hash_in = in; s_hash_len = ilen;
    s_hash_is224 = is224;
    if (!s_hash_want) return -1;            /* a row that forgot to supply one */
    memcpy(out, s_hash_want, 32);
    return 0;
}
#define mbedtls_sha256 host_sha256_hook
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
ART_BYTES = 4 * PLATE           # plate + keyframe + the window tiles + the sprite bank

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
# FORMAT 2 carries ONE name field, and it is the theme's whole identity: the card
# filename is theme_id(NAME)[:12], never this string. The sentinel header puts a
# distinct value in every field, so a reader off by one field cannot accidentally
# produce the number the test expected.
NAME = "Kit Three (test)"
HDR = orbtheme.pack_header(NAME, len(GEOM),
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

def mutate_pair(ws, i0, v0, i1, v1):
    """Two fields damaged in one blob. The ink-extent cases need it: the payload is
    big enough that neither width nor offset alone passes the bounds, and blowing
    both up is also what proves the multiply is 64-bit — a 32-bit one would wrap
    65535*65535*3 down into range and read as a sprite that fits."""
    ws2 = mutate_words(ws, i0, v0); return mutate_words(ws2, i1, v1)

# Offsets into the blob's fixed part: 4 counts, the 26 scalars, then the sprite
# table in the order pack_geom writes it -- 5 hands, 5 shadows, the overlay and the
# 16-frame tourbillon bank -- then the variable tables, sized by those counts. A
# refusal case that aims at the wrong word aims at nothing, so the layout the writer
# uses is spelled out here once rather than guessed per case.
OFF_SCALAR  = 4
OFF_SPRITE  = OFF_SCALAR + len(orbtheme.THEME_FIELDS)
SPR_BYTES_N = len(orbtheme.SPRITE_FIELDS)
FIELD_W     = orbtheme.SPRITE_FIELDS.index("w")
FIELD_H     = orbtheme.SPRITE_FIELDS.index("h")
SPRITE_N    = 5 + 5 + 1 + 16                       # hands, shadows, overlay, tourb
RECT_WORDS  = 5                                    # x, y, w, h, off
OFF_RECTS   = OFF_SPRITE + SPRITE_N * SPR_BYTES_N
OFF_FIRST   = OFF_RECTS + N_RECTS * RECT_WORDS
OFF_DELAY   = OFF_FIRST + N_FIRST

# ---------------------------------------------------------------------------
# The complication/indicator tables, one record per kind so every branch the
# shipped reader takes is a branch this suite has walked: a window drawn from the
# tiles composited under the dial, a strip of bank glyphs, a ring of bank slots,
# and a flare. The offsets are chosen against the same declared payload the reader
# bounds-checks, so a row that would read past the end of the mapping is refused
# here instead of on a dial.
# ---------------------------------------------------------------------------
def rec(table, base=0, **over):
    """One record of `table` with every field present: the schema's blank, then the
    values the case cares about. A sprite-shaped row gets small distinct ink so the
    reader's extent checks pass on it, which is what makes the row prove anything."""
    d = ts.blank(table)
    d.update(over)
    if table != "comp":
        for j, f in enumerate(orbtheme.SPRITE_FIELDS):
            if f not in over:
                d[f] = {"w": 12, "h": 16, "show": 1, "blend": j % 3,
                        "color_off": base + j * 64,
                        "diffw": 4 + j, "diffh": 3 + j,
                        "diffx": -4 + j, "diffy": -2 + j}.get(f, j - 3)
    return d

N_BANK  = 13         # ten digits the strip indexes, its two window pieces, three
                     # ring slots (shared with the strip's first three digits), and
                     # one row nothing points at — which is what makes the reader's
                     # own bank pass testable rather than merely reachable
BANK    = [rec("bank", 3 * PLATE + 512 * i) for i in range(N_BANK)]
COMPS   = [
    rec("comp", tag=2, kind=ts.KINDS.index("window"), src=ts.SOURCES.index("weekday"),
        x=40, y=60, w=60, h=30, count=7, off=2 * PLATE,
        frame_bytes=60 * 30 * 2, value_min=1, scale=16,
        flags=ts.FLAG_UNDER_DIAL, blend=0, show=1),
    rec("comp", tag=3, kind=ts.KINDS.index("strip"), src=ts.SOURCES.index("date"),
        x=100, y=300, w=26, h=16, count=10, first=0, glyph_step=13, glyph_lead=4,
        edge_left=10, edge_right=11, value_min=1, scale=16,
        flags=ts.FLAG_ZERO_PAD, show=1),
    rec("comp", tag=7, kind=ts.KINDS.index("ring"), src=ts.SOURCES.index("battery"),
        x=0, y=0, w=12, h=16, count=3, first=3, cx=233, cy=233, radius=120,
        angle0=0, angle_step=1200, value_min=0, scale=16, show=1),
    rec("comp", tag=9, kind=ts.KINDS.index("flare"), src=ts.SOURCES.index("ampm"),
        x=200, y=200, w=12, h=16, count=2, first=1, blend=2, show=1),
]
AUX = [
    rec("aux", 3 * PLATE + 512 * N_BANK, tag=1, src=ts.SOURCES.index("second"),
        kind=0, host=0, span=3600, zero=0, value_min=0, value_max=59, flags=0),
    rec("aux", 3 * PLATE + 512 * (N_BANK + 1), tag=2,
        src=ts.SOURCES.index("power_reserve"), kind=1, host=2, span=2400, zero=-1200,
        value_min=0, value_max=72, flags=ts.FLAG_JUMP),
]
GEOM2  = orbtheme.pack_geom(SCALARS, HANDS, SHADOWS, OVERLAY, TOURB,
                            RECTS, FIRSTS, DELAYS, BGF,
                            comps=COMPS, aux=AUX, bank=BANK)
WORDS2 = list(struct.unpack("<" + "I" * (len(GEOM2) // 4), GEOM2))
# Where the three tables sit in that blob, computed from the same packer rather
# than counted by eye: every refusal below aims at a word in one of them, and an
# offset guessed here would aim at nothing and still print ok.
FIXED2   = 7 + len(orbtheme.THEME_FIELDS) + SPRITE_N * SPR_BYTES_N
OFF_COMP = FIXED2 + N_RECTS * RECT_WORDS + N_FIRST + N_DELAY + N_BGF
OFF_AUX  = OFF_COMP + len(COMPS) * ts.words_of("comp")
OFF_BANK = OFF_AUX  + len(AUX)   * ts.words_of("aux")

def _idx(fields, name):
    return [n for n, _t in fields].index(name)

def comp_i(i, name): return OFF_COMP + i * ts.words_of("comp") + _idx(ts.COMP_FIELDS, name)
def aux_i(i, name):  return OFF_AUX  + i * ts.words_of("aux")  + _idx(ts.AUX_FIELDS, name)
def bank_i(i, name): return OFF_BANK + i * ts.words_of("bank") + _idx(ts.BANK_FIELDS, name)

# Field-by-field expectations, generated from the records the packer consumed, so a
# field added to theme_schema.py adds its own assertion instead of shipping unread.
rows2 = ['    CKV(G2.n_comps, %d);' % len(COMPS),
         '    CKV(G2.n_aux, %d);' % len(AUX),
         '    CKV(G2.n_bank, %d);' % len(BANK)]
for i, c in enumerate(COMPS):
    for n, _t in ts.COMP_FIELDS:
        rows2.append('    CK(G2.comps[%d], %s, %d);' % (i, n, int(c[n])))
for i, a in enumerate(AUX):
    for n, _t in ts.BANK_FIELDS:
        rows2.append('    CK(G2.aux[%d].spr, %s, %d);' % (i, n, int(a[n])))
    for n, _t in ts.AUX_EXTRA_FIELDS:
        rows2.append('    CK(G2.aux[%d], %s, %d);' % (i, n, int(a[n])))
for i, b in enumerate(BANK):
    for n, _t in ts.BANK_FIELDS:
        rows2.append('    CK(G2.bank[%d], %s, %d);' % (i, n, int(b[n])))
COMPCHECKS = "\n".join(rows2)
NROWS2 = len(rows2)

# The rows a dial newer than the bake must survive: an unknown kind or driver hides
# that one record, and the theme still loads. Each case mutates one word of the good
# seven-count blob and asserts both that the parse succeeded and that the record
# that provoked it came back with show cleared.
HIDE_CALLS, HIDE_IDX = [], []
for i, c in enumerate(COMPS):
    HIDE_IDX.append(comp_i(i, "kind"))
    HIDE_IDX.append(comp_i(i, "src"))
for i, a in enumerate(AUX):
    HIDE_IDX.append(aux_i(i, "src"))
    HIDE_IDX.append(aux_i(i, "kind"))
for k, wi in enumerate(HIDE_IDX):
    label = "word %d of the complication tables names a feature this dial lacks" % wi
    HIDE_CALLS.append(arr("hide%d" % k, mutate_words(WORDS2, wi, 99)))
    if k < 2 * len(COMPS):
        which = "G3.comps[%d]" % (k // 2)
    else:
        which = "G3.aux[%d].spr" % ((k - 2 * len(COMPS)) // 2)
    HIDE_CALLS.append('    if (ablob("%s", hide%d, %d, %uu)) CKV(%s.show, 0);'
                      % (label, k, len(WORDS2), ART_BYTES, which))
HIDES = "\n".join(HIDE_CALLS)

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
     "geometry blob length does not match its own tables",
     lambda: (list(WORDS) + [0], ART_BYTES)),
    ("hand ink outside the payload (its extent blown up past the mapping)",
     "hand pixels lie outside the declared payload",
     lambda: (mutate_pair(WORDS, OFF_SPRITE + 0 * SPR_BYTES_N + FIELD_W, 65535,
                         OFF_SPRITE + 0 * SPR_BYTES_N + FIELD_H, 65535), ART_BYTES)),
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
    # The complication tables, same rule: one damaged word of the seven-count blob,
    # and the EXACT reason the shipped reader has to answer with. Each of these is a
    # way a half-written theme would otherwise have the paint path read a window tile
    # or a bank glyph out of the mapping its pixels did not fit in — the failure that
    # shows up as a dial with a piece missing and no log line.
    ("comp record leans on the reserved word",
     "comp record uses a reserved word — re-bake it",
     lambda: (mutate_words(WORDS2, comp_i(0, "reserve"), 1), ART_BYTES)),
    ("window tile stride disagrees with the comp's own box",
     "comp window tile stride disagrees with its own box",
     lambda: (mutate_words(WORDS2, comp_i(0, "frame_bytes"), 60 * 30 * 2 + 4), ART_BYTES)),
    ("window tiles run past the end of the payload",
     "comp window tiles lie outside the declared payload",
     lambda: (mutate_words(WORDS2, comp_i(0, "off"), ART_BYTES - 1000), ART_BYTES)),
    ("comp box off the edge of the panel",
     "comp box lies outside the panel",
     lambda: (mutate_words(WORDS2, comp_i(2, "x"), 500), ART_BYTES)),
    ("comp with no frames to index",
     "comp has no frames to index",
     lambda: (mutate_words(WORDS2, comp_i(3, "count"), 0), ART_BYTES)),
    ("shown comp with no size",
     "comp is shown but has no size",
     lambda: (mutate_words(WORDS2, comp_i(3, "w"), 0), ART_BYTES)),
    ("strip wider than the dial can lay glyphs out for",
     "strip's box needs more glyph slots than the dial creates",
     lambda: (mutate_words(WORDS2, comp_i(1, "w"), 200), ART_BYTES)),
    ("strip with no pitch to space its glyphs on",
     "strip has no pitch to lay its glyphs out on",
     lambda: (mutate_words(WORDS2, comp_i(1, "glyph_step"), 0), ART_BYTES)),
    ("strip whose box cannot hold even one glyph",
     "strip's box cannot hold even one glyph",
     lambda: (mutate_words(WORDS2, comp_i(1, "w"), 5), ART_BYTES)),
    ("strip's left window piece not in the bank",
     "strip left edge piece is not in the bank",
     lambda: (mutate_words(WORDS2, comp_i(1, "edge_left"), 99), ART_BYTES)),
    ("strip's glyph run past the shared bank",
     "comp runs past the shared sprite bank",
     lambda: (mutate_words(WORDS2, comp_i(1, "first"), 200), ART_BYTES)),
    ("ring with no angle between its slots",
     "ring has no angle between its slots",
     lambda: (mutate_words(WORDS2, comp_i(2, "angle_step"), 0), ART_BYTES)),
    ("aux hand whose range is inverted",
     "aux hand range is inverted",
     lambda: (mutate_words(WORDS2, aux_i(1, "value_min"), 100), ART_BYTES)),
    ("aux hand ink past the end of the payload",
     "aux hand pixels lie outside the declared payload",
     lambda: (mutate_words(WORDS2, aux_i(0, "color_off"), 0x7FFF0000), ART_BYTES)),
    ("shared sprite no comp points at, past the end of the payload",
     "shared sprite lies outside the declared payload",
     lambda: (mutate_words(WORDS2, bank_i(N_BANK - 1, "color_off"), 0x7FFF0000), ART_BYTES)),
    # The length rule that keeps every theme baked before the calendar loading: a
    # seven-count blob whose counts contradict its length is refused, because the
    # only way to read such a blob would be to guess which table is truncated.
    ("seven-count blob whose counts contradict its length",
     "geometry blob length does not match its own tables",
     lambda: (mutate_words(WORDS2, 4, len(COMPS) + 1), ART_BYTES)),

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
# A torn header: one byte flipped inside the name field, which the self-checksum must
# catch before hdr_parse() ever looks at the string.
_t = list(HDR_WORDS); _t[orbtheme.W_NAME + 1] ^= 0x20
HDR_REFUSALS.append(arr("hbad_torn", _t))
HDR_REFUSALS.append('    rhdr("torn header, stale checksum", '
                    '"container header is damaged — re-copy the file onto the card", hbad_torn);')
HDR_REFUSALS = "\n".join(HDR_REFUSALS)

# ---- P6.4: the FORMAT 3 door vectors ------------------------------------------
# §24.7's hardware truth, pinned as vectors instead of prose: a well-formed
# bundle header returns the sentinel (by POINTER, the row after), a torn one
# returns the generic line with its format word UNVERIFIED, and every other
# refusal a verified FORMAT 3 header earns is the READER's own sentence, spoken
# by the direct rows because the FORMAT 2 gate suppresses it on purpose. Built
# from the writer's pack_header3 so no word here is transcribed by eye.
import orbbundle as ob3
_m3 = re.search(r"static const char THEME3_REFUSAL\[\] =\s*(((?:\"[^\"]*\"\s*)+));", th_src)
if not _m3:
    sys.exit("THEME3_REFUSAL is gone from orb_theme.c — the accept signal has no "
             "single definition to compare against any more")
T3_SENT = "".join(re.findall(r'"([^"]*)"', _m3.group(1)))
if len(T3_SENT.encode("utf-8")) > 56:
    sys.exit("THEME3_REFUSAL is %d bytes; the try witness holds 56 and the sentence "
             "would arrive on the row truncated" % len(T3_SENT.encode("utf-8")))
if th_src.count('"that is a FORMAT 3 app bundle') != 1:
    sys.exit("the FORMAT 3 sentence is defined more than once in orb_theme.c — the "
             "one-definition rule this whole gate rests on is gone")

_h3 = list(struct.unpack("<32I", ob3.pack_header3(
    "b3 probe", 2, 40, 0xAABBCCDD, 0x10000, 4096, 0x11223344, serial=7)))
_h3_torn = list(_h3); _h3_torn[orbtheme.W_HDR_CRC] ^= 0x55AA
_h3_abi  = _recrc([w if i != 2 else 99 for i, w in enumerate(_h3)])
_h3_f4   = _recrc([w if i != orbtheme.W_FORMAT else 4 for i, w in enumerate(_h3)])
_h3_big  = _recrc([w if i != orbtheme.W_ART_SIZE else 0x3F0001 for i, w in enumerate(_h3)])
_h3_noname = _recrc([0 if 14 <= i < 28 else w for i, w in enumerate(_h3)])
B3_ROWS = "\n".join([
    arr("h3ok", _h3),
    arr("h3torn", _h3_torn),
    arr("h3abi", _h3_abi),
    arr("h3f4", _h3_f4),
    arr("h3big", _h3_big),
    arr("h3noname", _h3_noname),
    '    rhdr("FORMAT 3, writer\'s own header", "%s", h3ok);' % T3_SENT,
    '    rhdr("FORMAT 3, torn checksum: format word unverified", '
    '"theme format this firmware cannot read", h3torn);',
    '    rhdr("FORMAT 3, unknown abi: reader reason suppressed at the gate", '
    '"theme format this firmware cannot read", h3abi);',
    '    rhdr("FORMAT 4: unreadable, not a bundle", '
    '"theme format this firmware cannot read", h3f4);',
    '    rhdr("FORMAT 3, payload over the slot: budget reason suppressed", '
    '"theme format this firmware cannot read", h3big);',
    '    r3ptr(h3ok);',
    '    r3fields("writer\'s header parses whole", h3ok, "b3 probe", 2, 7, 0x11223344u, 0xAABBCCDDu, 0x10000u, 40u);',
    '    r3why("reader refuses a lying abi", h3abi, '
    '"bundle geometry v99; this build reads v3 — re-bake it");',
    '    r3why("reader refuses payload over the slot", h3big, '
    '"pixel payload is 4128769 B; a theme slot holds 4128768 B — theme is larger than a slot");',
    '    r3why("reader refuses a nameless bundle", h3noname, '
    '"theme has no name in its header — re-bake it");',
])


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

# P6.4's doors hand the lifted reader real FORMAT 3 headers (the vector set
# below), so the P6.1 stub retires on its own instruction: the reader is LIFTED,
# not stubbed — b32, reject, the real orb_bundle3_hdr_t, the writer's own header
# words. The word-index enum is deliberately NOT lifted with it: orb_bundle.c
# declares the identical anonymous enum (its comment names orb_theme.c's as the
# layout), and C forbids one enumerator twice in a translation unit.
# bundle_wordmap_lint() compares the two enum blocks as text every run, which is
# what makes sharing them a checked fact in this harness and not a hope.
b3_src = open(P("main/art/orb_bundle.c")).read()
b3_hdr_src = open(P("main/art/orb_bundle.h")).read()
b3_typedef = lift(b3_hdr_src, r"typedef struct \{\s*char\s+name\[57\];[\s\S]*?\} orb_bundle3_hdr_t;", "orb_bundle.h")
b3_b32 = lift(b3_src, r"static uint32_t b32\(const uint8_t \*p\)[\s\S]*?\n\}", "orb_bundle.c")
b3_rej = lift(b3_src, r"static char s_msg\[160\];[\s\S]*?\n\}", "orb_bundle.c")
b3_parse = lift(b3_src, r"const char \*orb_bundle3_hdr_parse\(const uint32_t \*w, orb_bundle3_hdr_t \*o\)\n\{[\s\S]*?\n\}", "orb_bundle.c")
BUNDLE_STUB = ("#include <stdarg.h>\n"
               + lift(b3_hdr_src, r"#define ORB_BUNDLE3_FORMAT[\s\S]*?#define ORB_BUNDLE_ART_CAP[\s\S]*?\*/", "orb_bundle.h")
               + "\n#define B3_MAGIC  0x474F5254u\n"
               + b3_typedef + "\n" + b3_b32 + "\n" + b3_rej + "\n" + b3_parse + "\n")
# Insert before hdr_parse, not at the top: the lifted reader reads the word map,
# and the word map is the head of pure_theme. One anchor, one insertion.
_B3_AT = "static const char *hdr_parse(const uint32_t *w, orb_thdr_t *o)"
if _B3_AT not in pure_theme:
    sys.exit("hdr_parse's definition line moved in orb_theme.c — the bundle reader "
             "lift no longer knows where to stand")
pure_theme = pure_theme.replace(_B3_AT, BUNDLE_STUB + _B3_AT, 1)

# ---------------------------------------------------------------------------
# THE GEOMETRY ARENA — the one function in the theme layer whose mistake draws
# garbage on the glass instead of refusing a file. geom_validate_blob() mallocs
# the single arena every table of a theme lives in; on the adopt path that arena
# must NEVER be freed, because clock_view.c caches raw pointers into those
# tables — a free after the publish is a use-after-free the owner reads as a
# scrambled dial, and nothing logs it. The other two exits (parse refusal, the
# adopt=false pre-flight) must give their bytes back. Nothing else off-target
# compiles this function: host_tz_test lifts only the pure D13 predicates, so
# before this lift a text mutation of this body changed bytes NO host binary
# ever compiled — every battery green, the dial drawing from freed heap. The
# counters below are what turn "freed / not freed" into a verdict.
_arena_m = re.search(r"static const char \*geom_validate_blob\(const uint32_t \*blob"
                     r".*?\n\}", th_src, re.S)
if not _arena_m:
    sys.exit("could not find geom_validate_blob in orb_theme.c — the arena rows "
             "no longer know what to copy (renamed? reshaped?)")
arena_fn = (_arena_m.group(0)
            .replace("free(arena)", "host_arena_free(arena)")
            .replace("strlcpy(", "host_strlcpy("))
# The arena is sized from the HEADER's counts while the parser reads the BLOB's,
# so the vectors aim them at each other by lifting the blob's own leading count
# words — the same words geom_counts() reads — rather than transcribing numbers
# by eye: a count guessed here would size the arena against a layout nobody
# checks, and every row below would still print ok.
_N = [str(int(v)) for v in WORDS2[:7]]
_M = [str(int(v)) for v in WORDS[:4]]
if any(not v.isdigit() for v in _N + _M):
    sys.exit("non-numeric count words lifted off the writer's own blobs — the "
             "leading-counts layout moved and the arena vectors aim at nothing")

ARENA_DECLS = r'''
/* ---- the arena: orb_theme.c's own module state and allocator, stood up here
 * because the function under test is file-static and nothing else off-target
 * compiles it. The counters ARE the test. On the device the adopt arena is
 * deliberately never freed; "adopt: freed nothing" is the green line, and a
 * red one there means the dial draws from reclaimed heap. */
#include <stdlib.h>
static clk_theme_t     s_theme;
static clk_bg_rect_t  *s_rects;
static uint16_t       *s_first, *s_delays;
static uint32_t       *s_bgframes;
static clk_comp_t     *s_comps;
static clk_aux_t      *s_aux;
static clk_sprite_t   *s_bank;
static orb_theme_geo_t s_geo;
static char            s_active_name[ORB_THEME_NAME_MAX + 1];
/* The identity the installed slot captured at adopt, and that scan_entry()/theme_state()
 * compare a card file against. Both are module statics in orb_theme.c, so the harness
 * has to stand them up for the lifted code to compile — and the two adopt rows below
 * only mean anything because these start empty at every arena_reset(). */
static uint32_t        s_active_geom_crc, s_active_art_crc;
static bool            s_active_valid;
static unsigned long ar_allocs, ar_frees;
static int ar_oom;                     /* the no-room row arms this */
#define MALLOC_CAP_SPIRAM 0x1000
#define MALLOC_CAP_8BIT   0x0008
#define MALLOC_CAP_INTERNAL 0x0001
static size_t ar_int_max;                  /* the no-room-for-N-contiguous arm */
static void *heap_caps_malloc(size_t n, int caps)
{ if (ar_oom) return NULL;
  if (ar_int_max && (caps & MALLOC_CAP_INTERNAL) && n > ar_int_max) return NULL;
  ar_allocs++; return malloc(n); }
static void host_arena_free(void *p) { if (p) { ar_frees++; free(p); } }
__ENSURE_FN__
__ARENA_FN__
static void arena_reset(void)
{
    memset(&s_geo, 0, sizeof s_geo);
    s_active_name[0] = '\0';
    s_active_geom_crc = s_active_art_crc = 0;
    s_active_valid = false;
    s_rects = NULL; s_first = NULL; s_delays = NULL; s_bgframes = NULL;
    s_comps = NULL; s_aux = NULL; s_bank = NULL;
    ar_allocs = 0; ar_frees = 0; ar_oom = 0; ar_int_max = 0;
    if (s_chunk) { free(s_chunk); s_chunk = NULL; }
    s_chunk_bytes = 0;
}
static void arow(const char *what, int cond)
{
    if (!cond) fails++;
    printf("%-5s arena: %s\n", cond ? "ok" : "FAIL", what);
    fflush(stdout);                /* a mutant that aborts the harness must not
                                    * also erase the FAIL line that killed it */
}
/* A header aimed at one of the two blobs, sized from that blob's OWN leading
 * count words. hdr_parse() zeroes these fields by design — the real loaders
 * derive them from the blob — so the vectors supply the consistent pair the
 * arena is sized from, and the truncation row breaks it exactly the way
 * geom_counts() exists to catch. */
static void arena_hdr(orb_thdr_t *h, int two)
{
    memset(h, 0, sizeof *h);
    h->geom_size = (uint32_t)(two ? sizeof good2 : sizeof good);
    h->art_size  = __ART__u;
    /* Distinct non-zero checksums, because the adopt path is expected to CAPTURE
     * them alongside the name and the page's active/changed verdict is built from
     * all three. Left at zero, the row "the wall's identity was captured" would
     * pass against a loader that stored nothing at all. */
    h->geom_crc  = two ? 0xA1A1A1A1u : 0x1111u;
    h->art_crc   = two ? 0xB2B2B2B2u : 0x2222u;
    h->n_rects   = (uint32_t)(two ? __N0__ : __M0__);
    h->n_first   = (uint32_t)(two ? __N1__ : __M1__);
    h->n_delays  = (uint32_t)(two ? __N2__ : __M2__);
    h->n_bgf     = (uint32_t)(two ? __N3__ : __M3__);
    h->n_comp    = two ? (uint32_t)__N4__ : 0u;
    h->n_aux     = two ? (uint32_t)__N5__ : 0u;
    h->n_bank    = two ? (uint32_t)__N6__ : 0u;
    host_strlcpy(h->name, __NAME__, sizeof h->name);
}
'''

ARENA_CASES = r'''
    puts("== the card read buffer: a granting ladder, not a 4 KB ultimatum ==");
    {
        /* The bug the board found: a flat heap_caps_malloc(4096, INTERNAL) at a steady
         * largest block of 3,328 answered 507 to every upload AND refused the card
         * copy, because both reserve the same buffer. The rows below are the grant,
         * the cache, each rung, and the refusal floor -- the four facts the shipped
         * ladder has to keep. */
        arena_reset();
        esp_err_t ce = ensure_chunk();
        arow("a healthy internal heap still gets the whole 4 KB",
             ce == ESP_OK && s_chunk != NULL && s_chunk_bytes == CHUNK_BYTES);
        unsigned long c_before = ar_allocs;
        ce = ensure_chunk();
        arow("the second call reserves nothing new -- the grant is cached",
             ce == ESP_OK && ar_allocs == c_before);

        arena_reset(); ar_int_max = 3328;          /* the measured largest block */
        ce = ensure_chunk();
        arow("the measured starved board gets 2 KB, not a 507",
             ce == ESP_OK && s_chunk_bytes == 2048);

        arena_reset(); ar_int_max = 1500;
        ce = ensure_chunk();
        arow("a smaller hole falls one more rung to 1 KB", ce == ESP_OK && s_chunk_bytes == 1024);

        arena_reset(); ar_int_max = 512;
        ce = ensure_chunk();
        arow("the ladder stops at 1 KB and says NO_MEM rather than crawl",
             ce == ESP_ERR_NO_MEM && s_chunk == NULL && s_chunk_bytes == 0);
        arena_reset();
    }

    puts("== the geometry arena: who owns the tables, per adopt ==");
    {
        orb_thdr_t h1, h2, hb;
        const char *why;
        arena_hdr(&h1, 0);
        arena_hdr(&h2, 1);
        arow("the four-count blob leads with its own counts",
             h1.n_rects > 0 && h1.n_delays > 0);
        arow("the complication blob declares its tables",
             h2.n_comp > 0 && h2.n_aux > 0 && h2.n_bank > 0);

        arena_reset();
        why = geom_validate_blob(good, &h1, false);
        arow("pre-flight accepts the shipped four-count blob", why == NULL);
        arow("pre-flight gives back every byte it took",
             ar_allocs > 0 && ar_frees == ar_allocs);
        arow("pre-flight publishes nothing: the owner keeps the dial they had",
             !s_geo.from_container && s_rects == NULL && s_active_name[0] == '\0');

        arena_reset();
        why = geom_validate_blob(good2, &h2, true);
        arow("adopt accepts the complication blob", why == NULL);
        arow("adopt leaves its arena alive on purpose (clock_view caches into it)",
             ar_allocs > 0 && ar_frees == 0);
        arow("adopt publishes exactly the tables it verified",
             s_geo.from_container && s_rects != NULL && s_geo.rects == s_rects
             && s_geo.theme == &s_theme
             && s_geo.n_comps == (uint16_t)h2.n_comp
             && s_geo.n_bank == (uint16_t)h2.n_bank
             && strcmp(s_geo.name, @NAME@) == 0);
        /* The identity the page later compares against (plan §6). Captured here, at
         * the one place the slot is adopted — and all three values together. A name
         * without the checksums says "active" for a card file that was overwritten
         * after install, and the owner presses Load on something the dial is not
         * drawing while the page insists it already is. */
        arow("adopt captures the name and BOTH checksums of what it published",
             s_active_valid && strcmp(s_active_name, @NAME@) == 0
             && s_active_geom_crc == h2.geom_crc && s_active_art_crc == h2.art_crc);

        arena_reset();
        hb = h2; hb.geom_size -= 4;              /* shorter than its own tables */
        why = geom_validate_blob(good2, &hb, true);
        arow("a refusal publishes nothing even on the adopt path",
             why != NULL && !s_geo.from_container && s_comps == NULL
             && ar_allocs > 0 && ar_frees == ar_allocs);

        arena_reset();
        why = geom_validate_blob(good2, &hb, false);
        arow("a blob shorter than its tables is refused, and frees its arena",
             why != NULL && ar_allocs > 0 && ar_frees == ar_allocs
             && !s_geo.from_container && s_rects == NULL);

        arena_reset();
        ar_oom = 1;
        why = geom_validate_blob(good2, &h2, true);
        arow("no room answers with the reason, not a crash or a fake success",
             why != NULL && ar_allocs == 0 && ar_frees == 0
             && !s_geo.from_container);

        arena_reset();
        (void)geom_validate_blob(good2, &hb, false);   /* refused first */
        why = geom_validate_blob(good2, &h2, true);    /* then the retry lands */
        arow("a refused check leaves no state the retry inherits",
             why == NULL && s_geo.from_container && ar_allocs > ar_frees);
    }
'''
for _k, _v in (("__ARENA_FN__", arena_fn), ("__ENSURE_FN__", f_chunk), ("__ART__", str(ART_BYTES)),
               ("__NAME__", json.dumps(NAME)),
               ("__N0__", _N[0]), ("__N1__", _N[1]), ("__N2__", _N[2]),
               ("__N3__", _N[3]), ("__N4__", _N[4]), ("__N5__", _N[5]),
               ("__N6__", _N[6]), ("__M0__", _M[0]), ("__M1__", _M[1]),
               ("__M2__", _M[2]), ("__M3__", _M[3])):
    ARENA_DECLS = ARENA_DECLS.replace(_k, _v)
if re.search(r"__[A-Z0-9]+__", ARENA_DECLS):
    sys.exit("unfilled token in the arena declarations block")

def geom_arena_lint():
    """The free/publish contract of geom_validate_blob, from its own text.

    The vectors catch the arena bugs that run; this pins the SHAPE that lets
    them back in. Only two frees are legal — the parse-refusal one and the
    (!adopt)-guarded pre-flight exit — and both must sit BEFORE the publish
    block, because clock_view.c caches raw pointers into the adopt arena and
    an adopt-path free hands the dial a use-after-free. Neither half is
    redundant: a mutant that moves a free while still balancing the counters
    is visible only in the rows, and a reshaped guard that happens to pass the
    run is visible only here. The pair together is the pin.
    """
    m = re.search(r"static const char \*geom_validate_blob\(const uint32_t \*blob"
                  r".*?\n\}", th_src, re.S)
    if not m:
        print("FAIL geom arena lint: geom_validate_blob is not in orb_theme.c "
              "anymore — this lint no longer sees what it was written for")
        return 1
    body = m.group(0)
    rc = 0
    frees = [mm.start() for mm in re.finditer(r"free\(arena\);", body)]
    pub = body.find("s_rects = rects;")
    if len(frees) != 2:
        print("FAIL geom arena lint: %d free(arena) site(s), want exactly 2 "
              "(refusal + pre-flight) — a free on the adopt path is a "
              "use-after-free on the dial" % len(frees)); rc = 1
    if pub == -1:
        print("FAIL geom arena lint: the publish block (s_rects = rects;) moved "
              "— the ordering checks below can no longer mean anything"); rc = 1
    elif frees and max(frees) > pub:
        print("FAIL geom arena lint: a free(arena) sits AFTER the publish block "
              "— clock_view.c caches pointers into that arena"); rc = 1
    if not re.search(r"if \(why\) \{[^}]*free\(arena\);", body):
        print("FAIL geom arena lint: the parse-refusal path no longer frees its "
              "arena before returning the reason"); rc = 1
    if not re.search(r"if \(!adopt\) \{\s*free\(arena\);[^}]*return NULL;", body):
        print("FAIL geom arena lint: the pre-flight free is not the (!adopt)-"
              "guarded exit — the adopt path is freeing, or falling into "
              "publishing, what it only came to check"); rc = 1
    if rc == 0:
        print("ok    geom arena lint: two frees, both before the publish block; "
              "the adopt path owns an arena it never frees")
    return rc

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
_long_stem  = "0" * _NAME_CAP
_long_path  = _theme_dir + "/" + _long_stem + _theme_ext

NAME_ROWS = (NAME_ROWS.replace("@CAP31@", "0" * _NAME_CAP)
                       .replace("@CAP32@", "0" * (_NAME_CAP + 1)))

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
# Under FORMAT 2 the stem is always sha256(name)[:12] or a directory entry the page is
# showing, so the doubled-extension row guards the listing rather than the install: a
# 31-char entry FATFS hands back must come back as one path, not a cut-off one. Stripping
# a trailing ".orbtheme" would make "x" and "x.orbtheme" land on one file, and one
# upload could quietly overwrite another theme.
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
# The card identity: theme_id() and theme_id_ok(), run rather than read.
#
# These two functions decide every filename on the card and every ?id= the device
# will honour, and a mistake in either is invisible until a theme lists, offers a
# Load button, and then cannot be installed. The harness lifts both — theme_id()
# reaches mbedtls through the recording hook in CRC_STUB — so the rows below check
# three things at once: WHICH BYTES were handed to the hash (the name to its NUL,
# never the padded NAME_MAX array), what the 12 hex chars that came back are
# (hashlib's answer for exactly those bytes), and that the id this function issues
# is one theme_id_ok() will later admit.
# ---------------------------------------------------------------------------
_ID_NAMES = [
    NAME,                                   # the sentinel header's own name
    "Neon",                               # short ASCII: the case that reads innocent
    "Ünïcödé 日本語 🔆",            # UTF-8: multi-byte, so bytes != chars
    "a" * (orbtheme.NAME_MAX - 1),          # 55 chars, the longest name allowed
    "0",                                    # one char: proves the length is the string's
]
_ID_ROWS = []
for _i, _nm in enumerate(_ID_NAMES):
    _b = _nm.encode("utf-8")
    _ID_ROWS.append('    static const unsigned char sdig%d[32] = {%s};'
                    % (_i, ",".join(str(x) for x in hashlib.sha256(_b).digest())))
    _ID_ROWS.append('    icase("%s", sdig%d, %d, "%s");   /* %d UTF-8 bytes hashed */'
                    % (_nm.replace("\\", "\\\\").replace('"', '\\"'), _i, len(_b),
                       orbtheme.theme_id(_nm), len(_b)))
# The one row that pins the hash INPUT and not just the answer: the name is copied
# into a NAME_MAX+1 array whose tail is filled with 0xA5, exactly the state a header
# that came off a card is in. If theme_id() hashed the array instead of the string,
# the recorded length would be 56 and the id would be a different 12 chars — while a
# padded tail of zeros would have hidden the bug entirely.
_PAD = "Padded Tail"
_pad_bytes = _PAD.encode("utf-8")
_ID_ROWS.append('    static const unsigned char sdigpad[32] = {%s};'
                % ",".join(str(x) for x in hashlib.sha256(_pad_bytes).digest()))
_ID_ROWS.append('    icase_pad("%s", sdigpad, "%s");' % (_PAD, orbtheme.theme_id(_PAD)))
# Every id the shipped hasher issues must be one the shipped validator admits: the
# two functions are the whole contract between the list and the install button.
_ID_ROWS.append('    idcase("a1b2c3d4e5f6", 1);            /* a real issued id */')
_ID_ROWS.append('    idcase("", 0);                        /* empty */')
_ID_ROWS.append('    idcase("a1b2c3d4e5f", 0);             /* eleven: one too short */')
_ID_ROWS.append('    idcase("a1b2c3d4e5f60", 0);           /* thirteen: one too long */')
_ID_ROWS.append('    idcase("A1B2C3D4E5F6", 0);            /* uppercase is not the id */')
_ID_ROWS.append('    idcase("a1b2c3d4e5g6", 0);            /* not hex */')
_ID_ROWS.append('    idcase("../../../../etc/passwd", 0);  /* no traversal reaches the id */')
_ID_ROWS.append('    idcase("a1b2c3d4e5f ", 0);            /* trailing space */')
_ID_ROWS.append('    idcase("a1b2c3d4e5f6/../", 0);        /* slash inside */')
_ID_ROWS.append('    idcase(NULL, 0);')
ID_ROWS = "\n".join(r for r in _ID_ROWS if r)

# §6 — active vs changed, the pair the page renders as "On the dial" versus
# "Re-install". Same rule, four inputs: the installed slot's name and two CRCs, and
# the card file's. Each row is the mistake the other way round: an unchanged file
# that is not the wall's (Load is a no-op), or the wall's own file marked changed.
_STATE_ROWS = []
for _nm, _g, _a, _wn, _wg, _wa, _act, _chg, _why in [
        ("Wall",    111, 222, "Wall",  111, 222, 1, 0, "the file the dial draws is the one on the card"),
        ("Wall",    111, 999, "Wall",  111, 222, 0, 1, "same name, pixels changed on the card since install"),
        ("Wall",    333, 222, "Wall",  111, 222, 0, 1, "same name, geometry changed on the card since install"),
        ("Wall",    333, 999, "Wall",  111, 222, 0, 1, "both changed since it was installed"),
        ("Another", 111, 222, "Wall",  111, 222, 0, 0, "a byte-identical file for a DIFFERENT name is neither"),
        ("Another", 333, 999, "Wall",  111, 222, 0, 0, "another theme's file is neither, whatever its bytes"),
        ("Wall",    111, 222, "Neon",  111, 222, 0, 0, "nothing is the wall's while the wall holds another name"),
]:
    _STATE_ROWS.append('    scase("%s", %du, %du, "%s", %du, %du, %d, %d, "%s");'
                       % (_nm, _g, _a, _wn, _wg, _wa, _act, _chg, _why))
# The never-installed device: whatever the card holds, nothing is active and nothing
# is changed, because there is no captured identity to compare against at all.
_STATE_ROWS.append('    scase_none("a board that has never installed a theme");')
STATE_ROWS = "\n".join(_STATE_ROWS)


HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* The name cap comes from the shipped header, not from a copy here: the harness sizes
 * the same arrays the device does, so a raised cap is a length row that changes on its
 * own instead of a test still green against a header that overflowed. */
#define ORB_THEME_NAME_MAX @NAMEMAX@
@HELPERS@
@CRC@
@TYPES@
@RECTYPES@
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
@NAMEDIG@

static orb_theme_geo_t G;
static clk_theme_t     T1, T2, T3;
static clk_bg_rect_t   r1[64], r2[64], r3[64];
static uint16_t        f1[64], d1[64], f2[64], d2[64], f3[64], d3[64];
static uint32_t        b1[64], b2[64], b3[64];
/* The complication tables for the accepted cases and for the refusals, so a blob
 * that declares rows reaches the checks under test instead of the loader's own
 * "no room" guard. Sized by the caps the shipped reader refuses over. */
static clk_comp_t      cp1[@COMPS_MAX@], cp2[@COMPS_MAX@], cp3[@COMPS_MAX@];
static clk_aux_t       ax1[@AUX_MAX@],   ax2[@AUX_MAX@],   ax3[@AUX_MAX@];
static clk_sprite_t    bk1[@BANK_MAX@],  bk2[@BANK_MAX@],  bk3[@BANK_MAX@];
static orb_theme_geo_t G2, G3;
static clk_theme_t     T4;
static clk_bg_rect_t   r4[64];
static uint16_t        f4[64], d4[64];
static uint32_t        b4[64];
static int fails = 0;
@ARENA@
/* Lifted after the arena block because it reads the s_active_* module state that the
 * adopt path above owns: one definition of the §6 rule, compiled here and there. */
@THEMESTATE@

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
 * stops a maximum-length stem coming back as a cut-off filename, and deleting it has
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

/* ---- the card identity, run not read ------------------------------------------------
 * theme_id() is the single function that names every file on the card, and it is pure
 * except for the hash: this is the only code in the tree that could compare an id the
 * host has no independent answer for. So the expected hash is handed to the hook below
 * and the code under test is the part that decides WHICH BYTES reach it and how the
 * first six of thirty-two become the twelve characters on the card. */
static void icase(const char *name, const unsigned char *dig, size_t n, const char *want)
{
    char got[16];
    s_hash_want = dig;
    s_hash_is224 = -1;                         /* the hook records what was asked for */
    theme_id(name, got);
    const bool hex_ok = strcmp(got, want) == 0;
    const bool pass = hex_ok && s_hash_len == n && s_hash_is224 == 0
                      && memcmp(s_hash_in, name, n) == 0 && s_hash_in[n] == '\0';
    if (!pass) fails++;
    printf("%-5s id(\"%s\") -> %s (want %s)%s%s%s\n", pass ? "ok" : "FAIL", name, got, want,
           s_hash_len != n ? "   <== HASHED THE WRONG BYTE COUNT" : "",
           (hex_ok || s_hash_len != n) ? "" : "   <== HEX CONVERSION OR THE DIGEST IS WRONG",
           s_hash_is224 ? "   <== ASKED FOR SHA-224, WHICH TRUNCATES" : "");
    /* Every id the device issues must be one its own validator admits: the list and
     * the install button agree, or a row on the page leads to a refusal. */
    if (theme_id_ok(got) != hex_ok) {
        fails++;
        printf("FAIL  id \"%s\" issued but theme_id_ok() disagrees about it\n", got);
    }
}
/* The same row with the name living in a NAME_MAX+1 array whose tail is 0xA5, which is
 * the exact shape the header's name field has after a read: strlcpy left the pad bytes
 * from the source untouched there. A theme_id() that hashed the array instead of the
 * string produces a stable, plausible, WRONG id that lists, matches nothing, and gives
 * back "That card file is gone." on every press. */
static void icase_pad(const char *name, const unsigned char *dig, const char *want)
{
    char padded[ORB_THEME_NAME_MAX + 1];
    char got[16];
    /* The exact shape a header's name field has after a read from a card: the string,
     * its NUL, and then bytes that are whatever was in the file before it — deliberately
     * not zeros, because a tail of zeros is the one shape that hides a hasher that ran
     * past the terminator. The shipped code must ask for strlen(name) bytes and no more,
     * which is what the recorded length and bytes below assert. */
    memset(padded, 0xA5, sizeof padded);
    padded[0] = '\0';
    host_strlcpy(padded, name, sizeof padded);
    s_hash_want = dig;
    s_hash_is224 = -1;
    theme_id(padded, got);
    const bool pass = strcmp(got, want) == 0 && s_hash_len == strlen(name)
                      && s_hash_is224 == 0 && memcmp(s_hash_in, name, strlen(name)) == 0;
    if (!pass) fails++;
    printf("%-5s padded id(\"%s\") -> %s (want %s, hashed %u bytes)%s\n", pass ? "ok" : "FAIL",
           name, got, want, (unsigned)s_hash_len,
           pass ? "" : "   <== HASHED THE PAD, NOT THE NAME");
}
static void idcase(const char *id, int want)
{
    const int got = theme_id_ok(id);
    if (got != want) fails++;
    printf("%-5s theme_id_ok(%-24s) -> %d (want %d)\n", got == want ? "ok" : "FAIL",
           id ? (id[0] ? id : "(empty)") : "(null)", got, want);
}
/* The §6 pair, straight at the lifted rule. Each row names the mistake on the other
 * side: active-when-it-is-not shows the page offering to re-install what is already up;
 * changed-when-it-is-not hides the re-install after an overwrite, so the owner presses
 * Load on a file that is not what the dial is drawing and nothing happens. */
static void scase(const char *name, uint32_t g, uint32_t a, const char *act_name,
                  uint32_t ag, uint32_t aa, int want_active, int want_changed,
                  const char *why)
{
    orb_thdr_t h;
    bool active = true, changed = true;
    memset(&h, 0, sizeof h);
    host_strlcpy(h.name, name, sizeof h.name);
    h.geom_crc = g; h.art_crc = a;
    host_strlcpy(s_active_name, act_name, sizeof s_active_name);
    s_active_geom_crc = ag; s_active_art_crc = aa; s_active_valid = true;
    theme_state(&h, &active, &changed);
    const bool pass = (active == (bool)want_active) && (changed == (bool)want_changed);
    if (!pass) fails++;
    printf("%-5s state %-8s geom=%-3u art=%-3u vs wall %-8s geom=%-3u art=%-3u -> "
           "active=%d changed=%d (want %d/%d)  %s\n", pass ? "ok" : "FAIL",
           name, g, a, act_name, ag, aa, active, changed, want_active, want_changed, why);
}
static void scase_none(const char *why)
{
    orb_thdr_t h;
    bool active = true, changed = true;
    memset(&h, 0, sizeof h);
    strlcpy(h.name, "anything", sizeof h.name);
    h.geom_crc = 1; h.art_crc = 2;
    s_active_valid = false;
    theme_state(&h, &active, &changed);
    const bool pass = !active && !changed;
    if (!pass) fails++;
    printf("%-5s state with no installed slot: active=%d changed=%d  %s\n",
           pass ? "ok" : "FAIL", active, changed, why);
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
    R.comps = cp2; R.aux = ax2; R.bank = bk2;
    const char *got = orb_theme_geom_parse(w, n, art, &R);
    if (!got)     { fails++; printf("FAIL blob '%s' was ACCEPTED; wanted '%s'\n", what, want); }
    else if (strcmp(got, want))
                  { fails++; printf("FAIL blob '%s' refused as '%s'; wanted '%s'\n", what, got, want); }
    else          { printf("ok    blob refused as designed: %s\n", got); }
}

/* A blob that must be ACCEPTED, for the rows the reader is designed to hide rather
 * than refuse — a kind or a driver newer than this build. Refusing those is the
 * regression this catches: it would turn one feature the dial has never heard of
 * into a blank screen. */
static int ablob(const char *what, const uint32_t *w, uint32_t n, uint32_t art)
{
    memset(&G3, 0, sizeof G3);
    G3.theme = &T3; G3.rects = r3; G3.rect_first = f3; G3.gif_delays = d3;
    G3.bg_frames = b3; G3.comps = cp3; G3.aux = ax3; G3.bank = bk3;
    const char *why = orb_theme_geom_parse(w, n, art, &G3);
    if (why) { fails++; printf("FAIL blob '%s' was refused: %s\n", what, why); return 0; }
    printf("ok    blob accepted as designed: %s\n", what);
    return 1;
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

/* P6.4: the accept signal is the sentinel's ADDRESS, not its text (a rewording
 * that keeps the pointer is the change the doors survive; a copied literal is
 * the drift they don't). r3ptr is the one row no string compare can fake. */
static void r3ptr(const uint32_t *w)
{
    orb_thdr_t o;
    const char *got = hdr_parse(w, &o);
    if (got != THEME3_REFUSAL) { fails++; printf("FAIL FORMAT 3 accept not the sentinel pointer: '%s'\n", got ? got : "accepted"); }
    else            { printf("ok    FORMAT 3 accept is the sentinel's own pointer, not a copy\n"); }
}
/* r3fields: the reader fills every shared field from the writer's header. A
 * dropped or swapped field is the §22.8 lesson one level down — the tuple the
 * duplicate check compares is exactly these words. */
static void r3fields(const char *what, const uint32_t *w, const char *name,
                     unsigned ns, unsigned ser, unsigned acrc, unsigned gcrc,
                     unsigned aoff, unsigned gsz)
{
    orb_bundle3_hdr_t b;
    const char *r = orb_bundle3_hdr_parse(w, &b);
    int pass = !r && strcmp(b.name, name) == 0 && b.n_sections == ns && b.serial == ser &&
               b.art_crc == acrc && b.geom_crc == gcrc && b.art_off == aoff && b.geom_size == gsz;
    if (!pass) fails++;
    printf("%-5s %-32s reader fills every shared field: %s\n", pass ? "ok" : "FAIL",
           what, r ? r : (pass ? name : "FIELDS WRONG"));
}
/* r3why: the reader's OWN sentence for a verified-format header, which the
 * FORMAT 2 gate suppresses. The door (hdr_at_any) speaks this text; the gate
 * row above pins that it does not. */
static void r3why(const char *what, const uint32_t *w, const char *want)
{
    orb_bundle3_hdr_t b;
    const char *got = orb_bundle3_hdr_parse(w, &b);
    if (!got)         { fails++; printf("FAIL %s: reader ACCEPTED; wanted '%s'\n", what, want); }
    else if (strcmp(got, want)) { fails++; printf("FAIL %s: reader said '%s'; wanted '%s'\n", what, got, want); }
    else              { printf("ok    %s: %s\n", what, got); }
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
            CKV(strcmp(o.name, @NAME@), 0);
            /* The id the card file MUST be called, pinned to Python's own answer for
             * the same name, produced by the C hex loop from the name as it came out of
             * the shipped reader — the padded array and all. If the two implementations
             * ever part ways, every baked file stops matching what the device looks for
             * and the whole page is a list of buttons that say "gone". */
            char idwant[16];
            s_hash_want = name_dig;
            theme_id(o.name, idwant);
            CKV(strcmp(idwant, @IDHEX@), 0);
            CKV((int)s_hash_len, (int)sizeof(@NAME@) - 1);
        }
    }
    puts("== the name on the card is a hash of the name in the header ==");
@IDCASES@
    puts("== which row the page calls the one on the dial (§6) ==");
@STATECASES@
    puts("== the geometry blob, field by field, through the shipped reader ==");
    {
        G.theme = &T1; G.rects = r1; G.rect_first = f1; G.gif_delays = d1; G.bg_frames = b1;
        const char *why = orb_theme_geom_parse(good, @NW@, @ART@u, &G);
        if (why) { fails++; printf("FAIL the writer's own blob was refused: %s\n", why); }
        else printf("ok    @NROWS@ fields agreed between the Python writer and the C reader\n");
        /* The four-count blob is the layout every theme baked before the
         * complication tables existed. It has to keep loading, with nothing drawn:
         * this is the row that fails if the new tables ever make the reader reject a
         * theme that was already on somebody's card. */
        CKV(G.n_comps, 0); CKV(G.n_aux, 0); CKV(G.n_bank, 0);
@CHECKS@
    }
    puts("== the complication tables, field by field, through the shipped reader ==");
    {
        G2.theme = &T4; G2.rects = r4; G2.rect_first = f4; G2.gif_delays = d4;
        G2.bg_frames = b4; G2.comps = cp1; G2.aux = ax1; G2.bank = bk1;
        const char *why = orb_theme_geom_parse(good2, @NW2@, @ART@u, &G2);
        if (why) { fails++; printf("FAIL the writer's own complication blob was refused: %s\n", why); }
        else printf("ok    @NROWS2@ complication fields agreed between the two sides\n");
@COMPCHECKS@
    }
    puts("== a feature this dial has never heard of costs one row, not the dial ==");
@HIDES@
    puts("== the same reader's refusals ==");
@REFUSES@
    puts("== the header's refusals ==");
@HDRREFUSES@
    puts("== the FORMAT 3 door (P6.4): the accept signal and the reader's own refusals ==");
@B3ROWS@
@ARENA_CASES@
    return fails != 0;
}
'''

for k, v in (("@ARENA@", ARENA_DECLS), ("@ARENA_CASES@", ARENA_CASES),
             ("@HELPERS@", HELPERS), ("@CRC@", CRC_STUB), ("@TYPES@", gen_types),
             ("@GEOT@", geo_t), ("@CONSTS@", sd_consts), ("@NAMEOK@", f_name_ok),
             ("@THEMEPATH@", f_theme_pt), ("@PURE@", pure_theme), ("@CKV@", CKV),
             ("@EXT@", ext_define), ("@THEMEFILE@", f_theme_file),
             ("@FILECASES@", FILE_ROWS),
             ("@IDCASES@", ID_ROWS), ("@STATECASES@", STATE_ROWS),
             ("@THEMESTATE@", f_theme_state),
             ("@NAMEMAX@", _must(r"#define ORB_THEME_NAME_MAX\s+(\d+)", th_hdr,
                                 "ORB_THEME_NAME_MAX")),
             ("@NAME@", json.dumps(NAME)), ("@IDHEX@", json.dumps(orbtheme.theme_id(NAME))),
             ("@NAMEDIG@", "\n/* the digest Python computes for the sentinel header's own name */\n"
                           "static const unsigned char name_dig[32] = {%s};\n"
                           % ",".join(str(x) for x in
                                      hashlib.sha256(NAME.encode("utf-8")).digest())),
             ("@CK@", CK), ("@ARRAYS@", arr("good", WORDS) + arr("good2", WORDS2)
                                             + arr("hdr_ok", HDR_WORDS)),
             ("@ARENA@", ARENA_DECLS), ("@ARENA_CASES@", ARENA_CASES),
             ("@NAMES@", NAME_ROWS), ("@PATHS@", PATH_ROWS), ("@CHECKS@", CHECKS),
             ("@RECTYPES@", rec_types), ("@COMPS_MAX@", str(ts.CLK_COMP_MAX)),
             ("@AUX_MAX@", str(ts.CLK_AUX_MAX)), ("@BANK_MAX@", str(ts.CLK_BANK_MAX)),
             ("@COMPCHECKS@", COMPCHECKS), ("@NROWS2@", str(NROWS2)),
             ("@HIDES@", HIDES), ("@NW2@", str(len(WORDS2))),
             ("@REFUSES@", REFUSE_CALLS), ("@HDRREFUSES@", HDR_REFUSALS),
             ("@B3ROWS@", B3_ROWS),
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
    # button deletes a file from somebody's card. The install confirm and the two
    # endpoints beside it are ruling D11's and D13's side of the bargain: the page
    # posts an install, the firmware reboots, and there is no staged state left to
    # cancel — a page that still offered an undo would be offering a state the
    # firmware no longer has.
    pins = [("onclick=doUp()", "an upload path exists"),
            ("function del(id){var nm=ROWS[id]||id;",
             "delete is offered, and names what it destroys"),
            ("if(!confirm('Delete ", "delete asks before destroying a file"),
            ("function load(id){var nm=ROWS[id]||id;", "a pick names the theme it is about to install"),
            ("ROWS[t.id]=nm;", "the row's name is read back from a map, never pasted into an onclick"),
            ("var ROWS={};", "and that map exists before the rows are drawn"),
            ("if(!confirm('Install ", "a pick names the reboot it is about to cause"),
            ("if(!confirm('Go back to the factory dial", "the way back asks too"),
            ("x.upload.onprogress", "the upload reports real progress (fetch cannot)"),
            # FORMAT 2: a row addresses its theme by the 12-hex id the device issued,
            # never by a name the browser could spell however it liked. Pinned as the
            # whole call expression: the bare endpoint text survives a handler that
            # posts anything after `?id=`, and `encodeURIComponent(id)` appears twice
            # in the page, so a generic pin hides whichever one went missing.
            ("/api/v1/sd/install?id='+encodeURIComponent(id)", "Load posts the install job by the url-encoded id, not by name"),
            ("/api/v1/sd/delete?id='+encodeURIComponent(id)", "delete addresses the row by the url-encoded id, the only handle that resolves"),
            ("/api/v1/sd/default", "the factory dial is one press away without the card"),
            ("data.live", "the banner states the choice, not a pending operation"),
            ("LAST TRY FAILED", "the install the worker refused explains itself on its row"),
            ("THE DIAL WOULD NOT LOAD IT", "what boot found damaged in the slot goes on its row"),
            ("CANNOT LOAD", "an unloadable theme says why instead of vanishing")]
    rc = 0 if js_ok else 1
    for want, why in pins:
        # the page, not just the script: the upload button lives in the markup, and
        # searching only the JS would report a present feature as missing
        if want not in page:
            print("FAIL /themes: missing %r — %s" % (want, why)); rc = 1
    # No row label may reach a handler as markup. Names cover the full printable
    # ASCII range including apostrophes, so a name interpolated into
    # onclick="del('...')" is a broken button at best and an attribute injection at
    # worst; the id is the only value the row is allowed to hand to a function. `page`
    # is the unescaped text the browser receives, so the quotes here are plain.
    for fn, args in re.findall(r'onclick="(load|del)\((.*?)\)"', page):
        if "nm" in args or "name" in args:
            print("FAIL /themes: the %s handler interpolates a name into markup (%s) — "
                  "pass the id alone and look the name up in ROWS inside the function"
                  % (fn, args[:60]))
            rc = 1
    # And the refusals: the pending concept is gone (ruling D13), so a page that still
    # names it is a page that will offer a button for a state nothing can be in.
    # D30's `staged` word is NOT this concept and does not live here: it is a field on
    # /api/v1/sd that reports a flip that rolled back, it selects nothing, and no
    # button on this page acts on it. The two rules are one sentence apart and must not
    # be merged by a reader who stops at the word.
    for gone, why in (("/api/v1/sd/unstage", "the staged load has no undo any more"),
                      ("staged", "no row may promise an install that waits for a reboot")):
        if gone in page:
            print("FAIL /themes: still carries %r — %s" % (gone, why)); rc = 1
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
    # the list growing a '/' -- which is what hands an id or a listed entry back as a
    # path to somewhere the theme directory never was.
    ("traversal: '/' allowed in a name",            "sd",  "(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'",
                                                     "(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/'"),
    ("name length cap raised away",                  "sd",  "if (n >= ORB_SD_NAME_MAX) return false;", "/* no cap */"),
    ("path builder stops validating the name",       "sd",  "if (!orb_sd_name_ok(name)) { out[0] = '\\0'; return false; }", ""),
    ("leading dot allowed (hides the entry)",        "sd",  "if (name[0] == '.') return false;", ""),
    # The blob's length is the evidence for which layout it is, so the two rows below
    # are the pair that keeps an old theme loading and a half-written one refused:
    # relax the length equation and a trailing word slips in, and the reader's own
    # trailing-word check is what answers it with a different message than the case
    # expects. (That trailing check is unreachable while the length rule is exact, and
    # exists for the day a table is added to one rule and not the other.)
    ("reader accepts a blob longer than its own tables say", "th",
     "if ((uint64_t)(n + body + rows) * 4u == (uint64_t)nwords * 4u) {",
     "if ((uint64_t)(n + body + rows) * 4u <= (uint64_t)nwords * 4u) {"),
    ("reader stops checking a comp's window tiles against the payload", "th",
     'return "comp window tiles lie outside the declared payload";', "return NULL;"),
    ("reader stops checking a comp's window tile stride", "th",
     'return "comp window tile stride disagrees with its own box";', "return NULL;"),
    ("reader stops checking a comp's run against the shared bank", "th",
     'return "comp runs past the shared sprite bank";', "return NULL;"),
    ("reader stops checking a strip's glyph cap", "th",
     'return "strip\'s box needs more glyph slots than the dial creates";', "return NULL;"),
    ("reader stops checking a strip's glyph pitch", "th",
     'return "strip has no pitch to lay its glyphs out on";', "return NULL;"),
    ("reader stops checking that a strip holds a glyph", "th",
     'return "strip\'s box cannot hold even one glyph";', "return NULL;"),
    ("reader stops checking a strip's window pieces", "th",
     'return "strip left edge piece is not in the bank";', "return NULL;"),
    ("reader stops checking the reserved word a comp must leave alone", "th",
     'return "comp record uses a reserved word — re-bake it";', "return NULL;"),
    ("reader stops checking a shared sprite against the payload", "th",
     'return "shared sprite lies outside the declared payload";', "return NULL;"),
    ("reader stops checking an aux hand's ink", "th",
     'return "aux hand pixels lie outside the declared payload";', "return NULL;"),
    ("reader stops checking an aux hand's value range", "th",
     'return "aux hand range is inverted";', "return NULL;"),
    # Forward compatibility is the design decision, so its regression is a mutation:
    # refuse what you do not know and one window the dial has never seen turns into a
    # theme that will not install at all. The accept-cases in the harness are what
    # this bites.
    ("reader refuses a comp kind it has never seen", "th",
     "            c->show = 0;\n            continue;",
     '            return "comp kind this dial cannot draw";'),
    ("reader refuses an aux hand it has never seen", "th",
     "            a->spr.show = 0;\n            continue;",
     '            return "aux hand kind this dial cannot drive";'),
    ("reader stops checking rect vs panel",          "th",  "return \"delta rect lies outside the panel\";", "return NULL;"),
    ("reader stops checking rect vs payload",        "th",  "return \"delta rect lies outside the declared payload\";", "return NULL;"),
    ("reader stops checking the frame index table",  "th",  "if (out->rect_first[i] > n_rects) return \"frame index runs past the rect table\";", ""),
    ("reader stops checking the plate extent",       "th",  "return \"plate pixels lie outside the declared payload\";", "return NULL;"),
    ("reader stops checking shadows",                "th",  "return \"shadow pixels lie outside the declared payload\";", "return NULL;"),
    ("reader stops checking the GIF frame count",    "th",  "if (t->gif_frames > GIFS_MAX)      return \"more GIF frames than the dial holds\";", ""),
    ("theme file stops checking the extension fits", "th",  "if (blen + sizeof(ORB_THEME_EXT) > outsz) { out[0] = '\\0'; return false; }",
                                                     "if (0 && blen + sizeof(ORB_THEME_EXT) > outsz) { out[0] = '\\0'; return false; }"),
    ("theme file trusts the caller over the validator", "th", "if (!orb_sd_theme_path(base, sizeof base, stem)) { out[0] = '\\0'; return false; }",
                                                     "if (!orb_sd_theme_path(base, sizeof base, stem)) { return false; }"),
    ("reader stops checking tourbillon count",       "th",  "if (t->tourbillon_n > TOURB_MAX)   return \"more tourbillon frames than the dial holds\";", ""),
    ("header stops checking its own checksum",       "th",  "return \"container header is damaged — re-copy the file onto the card\";", "return NULL;"),
    # FORMAT 2's identity chain in the page: the id the device issued is the only thing
    # a row may send. Hand-written here rather than generated from the C theme-side
    # table, because this half of the chain lives in the JS and has no C twin. The pins
    # are checked as complete call expressions, not as the bare endpoint: a pin on
    # "/api/v1/sd/install?id=" alone survives a handler that posts anything after the
    # equals sign, which is exactly the regression these two rows exist to catch.
    ("/themes' install posts a name instead of the id", "net",
     "j('/api/v1/sd/install?id='+encodeURIComponent(id)",
     "j('/api/v1/sd/install?id='+nm"),
    ("/themes' install stops url-encoding the id",       "net",
     "j('/api/v1/sd/install?id='+encodeURIComponent(id)",
     "j('/api/v1/sd/install?id='+id"),
    ("/themes loses the delete confirmation",        "net", "function del(id){var nm=ROWS[id]||id;if(!confirm(",
     "function del(id){var nm=ROWS[id]||id;if(true && ("),
    # The row's name must never re-enter markup. Names may legally hold an apostrophe,
    # so a handler that interpolates one produces a button that silently does nothing —
    # exactly the failure a browser never reports.
    ("/themes' delete handler takes the name back into onclick", "net",
     r'''onclick=\"del(\\''+t.id+'\\')\"''',
     r'''onclick=\"del(\\''+t.id+'\\',\\''+esc(nm)+'\\')\"'''),
    ("/themes' install handler takes the name back into onclick", "net",
     r'''onclick=\"load(\\''+t.id+'\\')\"''',
     r'''onclick=\"load(\\''+t.id+'\\',\\''+esc(nm)+'\\')\"'''),
    # Ruling D13's install promise: the geometry pre-flight is the same validator
    # boot runs, so deleting it must red a check. The mutation keeps the file
    # compiling — only the lint can see the call is gone.
    ("install drops the geometry pre-flight",        "th",
     "const char *gw = (ve == ESP_OK) ? slot_parse_geom(&probe, p, /*adopt=*/false) : NULL;",
     "const char *gw = NULL; (void)ve;"),
    ("/themes loses the install confirmation",       "net", "function load(id){var nm=ROWS[id]||id;if(!confirm(",
     "function load(id){var nm=ROWS[id]||id;if(true && ("),
    ("/themes' factory switch stops asking",         "net",
     "function tofactory(){if(!confirm(", "function tofactory(){if(true && ("),
    ("/themes loses the upload progress handler",    "net", "x.upload.onprogress", "x.noupload_onprogress"),
    ("/themes calls a function that does not exist", "net", "function refresh()", "function refreshX()"),
    # The bug this file found for real: an esp_err_t read as a truth value. ESP_OK is
    # 0, so this inverted form refuses every card that is actually in the socket.
    ("install tests the mount result as a boolean",  "th",  "if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;",
                                                     "if (!orb_sd_ensure_mounted()) return ESP_ERR_NOT_FOUND;"),
    # The bug the hardware found for real: verify_container() reached s_chunk while it
    # was still NULL, because only the OTHER two users of that buffer reserved it. The
    # device rebooted at the end of a completed 3.87 MB upload.
    # The bug of 2026-10-14 evening: the flat 4 KB ask answered 507 at largest block
    # 3,328 and killed the card feature whole. A re-flattened ladder is the regression
    # this row exists to catch; the grant-ladder vectors red on it.
    ("the card buffer ladder re-flattens to a single 4 KB ask", "th",
     "    for (uint32_t n = CHUNK_BYTES; n >= CHUNK_MIN; n /= 2) {",
     "    for (uint32_t n = CHUNK_BYTES; n >= CHUNK_BYTES; n /= 2) {"),
    ("upload verify reads through an unreserved buffer", "th",
     'if (ensure_chunk() != ESP_OK) return "out of memory for the card read buffer";', ""),
    ("installer copies through an unreserved buffer",   "th",
     'if (ensure_chunk() != ESP_OK) { *why = "out of memory for the card read buffer"; return ESP_ERR_NO_MEM; }', ""),
    # The bug the card found for real: a card-lock holder reaching something that takes
    # the mutex again. The scan's walk now lives in orb_files_walk(), so the shape to
    # test is a holder calling a locked orb_files entry point — which is only caught
    # because orb_files_walk/orb_files_delete are in the lint's cross set. Delete is a
    # real holder, so asking it for a second walk is the same deadlock.
    ("scan re-asks for the card lock inside its walk", "th",
     "    orb_sd_lock();\n    const int r = unlink(path);",
     "    orb_sd_lock();\n    orb_files_walk(orb_files_src(\"clock\", \"themes\"), NULL, NULL);\n"
     "    const int r = unlink(path);"),
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
    # FORMAT 2's identity. The theme's one true name is the string inside its
    # container; the card filename is theme_id(name) — hex(sha256(name))[:12]. Every
    # mutation below breaks one link in that chain, and every link has to stay joined
    # or the page lists a theme whose buttons all answer "That card file is gone":
    # the bytes hashed, the hex loop, the charset the id is validated against, and the
    # two places a browser-supplied id is checked against the file's own header.
    ("identity: hashes the padded name array, not the name to its NUL", "th",
     "    mbedtls_sha256((const unsigned char *)name, strlen(name), digest, /*is224=*/0);",
     "    mbedtls_sha256((const unsigned char *)name, THEME_NAME_MAX, digest, /*is224=*/0);"),
    ("identity: asks for SHA-224, whose digest is shorter", "th",
     "    mbedtls_sha256((const unsigned char *)name, strlen(name), digest, /*is224=*/0);",
     "    mbedtls_sha256((const unsigned char *)name, strlen(name), digest, /*is224=*/1);"),
    ("identity: the high nibble comes out uppercase", "th",
     '        out[2 * i]     = "0123456789abcdef"[digest[i] >> 4];',
     '        out[2 * i]     = "0123456789ABCDEF"[digest[i] >> 4];'),
    ("identity: the low nibble takes the high one", "th",
     '        out[2 * i + 1] = "0123456789abcdef"[digest[i] & 0x0F];',
     '        out[2 * i + 1] = "0123456789abcdef"[digest[i] >> 4];'),
    ("identity: emits half the id it should", "th",
     "    for (int i = 0; i < THEME_ID_HEX / 2; i++) {",
     "    for (int i = 0; i < THEME_ID_HEX / 4; i++) {"),
    ("identity validator admits uppercase", "th",
     "        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;",
     "        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;"),
    ("identity validator has no upper length bound", "th",
     "        if (n >= THEME_ID_HEX) return false;          /* too long */",
     "        if (n > THEME_ID_HEX) return false;           /* too long */"),
    ("identity validator stops checking the lower bound", "th",
     "    return n == THEME_ID_HEX;                          /* and no shorter */",
     "    return n > 0;                                      /* and no shorter */"),
    # The resolve path's second look: pass one walked the directory and hashed the
    # header, pass two opens the file it chose and must hash AGAIN, because the card
    # can be edited on a laptop in between. Deleting the re-check leaves a path that
    # reads bytes no one re-verified, and only the lint below sees it — no host process
    # can mount a card.
    ("resolve stops re-verifying the header after opening it", "th",
     '    if (strcmp(id2, id) != 0) { card_close(fd); *why = "that file\'s name does not match its id — refused"; return -1; }',
     "    if (false) { card_close(fd); *why = \"refused\"; return -1; }"),
    ("resolve accepts a file whose header hashes to another id", "th",
     '    r->verified = (strcmp(id, r->want) == 0);             /* the walk\'s identity check */',
     "    r->verified = true;"),
    ("upload's duplicate check stops comparing the name it re-baked", "th",
     "                oh.serial == h.serial && strcmp(oh.name, h.name) == 0) {",
     "                oh.serial == h.serial) {"),
    ("the list stops refusing a file not named for its own name", "th",
     '    if (strcmp(id, rec->name) != 0) {',
     "    if (false) {"),
    # The §6 pair. Each mutation is one of the two ways the page lies about the dial.
    ("§6 calls any same-named file active, changed bytes included", "th",
     "    *active  = same_name && !geom_diff && !art_diff;",
     "    *active  = same_name;"),
    ("§6 marks another theme's file changed just because bytes differ", "th",
     "    *changed = same_name && (geom_diff || art_diff);",
     "    *changed = (geom_diff || art_diff);"),
    ("§6 never captures the geometry checksum at adopt", "th",
     "    s_active_geom_crc = h->geom_crc;",
     "    s_active_geom_crc = 0;"),
    # The geometry arena. Every one of these changed only bytes no host binary
    # ever compiled until geom_validate_blob was lifted above: green batteries,
    # and a dial drawing from reclaimed heap. Together they pin the contract
    # from both sides — the vectors see a free that moves while the counters
    # stay honest, the lint sees a guard reshaped while the run stays green.
    ("the adopt path frees the arena the dial is drawing", "th",
     "    if (!adopt) {\n        free(arena);          /* pre-flight: it proved the numbers, that is all it was for */\n        return NULL;\n    }",
     "    free(arena);\n    if (!adopt) {\n        return NULL;\n    }"),
    ("the (!adopt) guard is inverted: the pre-flight publishes", "th",
     "    if (!adopt) {\n        free(arena);",
     "    if (adopt) {\n        free(arena);"),
    ("a refused theme leaks its arena", "th",
     "        free(arena);\n        ESP_LOGE(TAG, \"%s: %s — refusing to draw it\", h->name, why);",
     "        ESP_LOGE(TAG, \"%s: %s — refusing to draw it\", h->name, why);"),
    ("the refusal falls through and publishes anyway", "th",
     "        ESP_LOGE(TAG, \"%s: %s — refusing to draw it\", h->name, why);\n        return why;",
     "        ESP_LOGE(TAG, \"%s: %s — refusing to draw it\", h->name, why);"),
    ("the arena is freed out from under the published tables", "th",
     "    s_geo.from_container = true;",
     "    s_geo.from_container = true;\n    free(arena);"),
    # ---- P6.4: the flip door ----
    # A pin you can delete without a failure is not a pin; each line below is one
    # way the store could lie, and each must redden a check.
    ("the FORMAT 3 gate re-refuses the well-formed bundle (door dead again)", "th",
     "            orb_bundle3_hdr_parse(w, &b3) == NULL)\n            return THEME3_REFUSAL;",
     "            orb_bundle3_hdr_parse(w, &b3) == NULL)\n            return \"theme format this firmware cannot read\";"),
    ("the torn bundle header now NAMES a format it cannot prove", "th",
     "        if (w[W_FORMAT] == ORB_BUNDLE3_FORMAT &&\n"
     "            orb_bundle3_hdr_parse(w, &b3) == NULL)\n"
     "            return THEME3_REFUSAL;\n"
     "        return \"theme format this firmware cannot read\";",
     "        if (w[W_FORMAT] == ORB_BUNDLE3_FORMAT) {\n"
     "            const char *r3m = orb_bundle3_hdr_parse(w, &b3);\n"
     "            return r3m ? r3m : THEME3_REFUSAL;\n"
     "        }\n"
     "        return \"theme format this firmware cannot read\";"),
    ("the scan stops distinguishing the bundle row", "th",
     "    if (why == THEME3_REFUSAL) {", "    if (0 && why == THEME3_REFUSAL) {"),
    ("the upload door accepts a bundle without the region check", "th",
     "r = (h.fmt == 3) ? verify_bundle3(fd, &b3, fsz) : verify_container(fd, &h);",
     "r = (h.fmt == 3) ? NULL : verify_container(fd, &h);"),
    ("the duplicate tuple drops the format word", "th",
     "if (!or_ && oh.fmt == h.fmt && oh.geom_crc == h.geom_crc &&",
     "if (!or_ && oh.geom_crc == h.geom_crc &&"),
    ("bundle_job reboots to show a store nobody can draw", "th",
     "    try_note(NULL, NULL);\n}\n\nesp_err_t orb_theme_post_bundle",
     "    try_note(NULL, NULL);\n    orb_jobs_reboot_soon(\"bundle store\");\n}\n\nesp_err_t orb_theme_post_bundle"),
    ("post_bundle calls the ingest off the worker (no queue, no busy gate)", "th",
     "    return orb_jobs_post(bundle_job, &r, sizeof r);",
     "    orb_theme_install_bundle(r.id, NULL, 0); return ESP_OK;"),
    ("slot_read trusts the FORMAT 2 reader alone with a v3 slot", "th",
     "if (hdr_parse(hdr, &s->h) || s->h.fmt != THEME_FORMAT) return;",
     "if (hdr_parse(hdr, &s->h)) return;"),
    ("the store confirm loses the power-cut sentence", "net",
     "A power cut during the copy brings back the factory clock, and storing again recovers it.",
     "Stored bundles are not drawn yet."),
    ("the flip route vanishes from the table", "net",
     "        { .uri = \"/api/v1/sd/bundle\",   .method = HTTP_POST, .handler = h_bundle_install },\n", ""),
    ("the bundle timing never reaches the page", "net",
     "\\\"bundle_ms\\\":%u,\\\"bundle_bytes\\\":%u,",
     "/* P6.4's timing pair removed */"),
    ("the resolver swallows the header's own refusal", "th",
     "if (cr) { r->why = cr; return false; }                /* damaged or foreign format */",
     "if (cr) return false;"),
    ("the door speaks R4's identity line over the real reason", "th",
     "    if (!r.verified && r.why) { *why = r.why;                                       return -1; }\n",
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
        for real in (SD_C, SD_H, TH_C, TH_H, B3_C, B3_H, GEN_H, NET_C, P("tools", "orbtheme.py"), P("tools", "orbbundle.py")):
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
    # orb_files.c holds the same mutex: walk() and delete() take it for their whole
    # operation, so a card theme path that reaches either while holding the lock is the
    # same deadlock the scan used to have. path() and src() are deliberately NOT here —
    # they are pure, which is what lets a lock holder build a filename, and adding a
    # take to path() is caught here rather than on a wall.
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


def naming_lint():
    """FORMAT 2's identity rule, checked over the shipped source.

    A theme's one true name is the string inside its container, and the card filename
    is theme_id(name) — hex(sha256(name))[:12]. Nothing else is allowed to name a card
    file: not the upload's original filename, not the string in ?id=, not the string a
    row on the page displayed. The failure this pins is the one FORMAT 1 had and the
    one the owner found from the wall with no serial cable: an entry listed as loadable
    because one consumer applied the naming rule while the consumer that acted did not,
    so the button answered with a refusal that named nothing.

    Executing that is impossible off-target — the paths need FatFs — so this is a check
    on the text: the two identity functions must be the single definition of the rule,
    and every function that lists, resolves or files a card theme must reach for them.
    """
    lines = _code_only(th_src).split("\n")
    raw = th_src.split("\n")          # same line count: spans index both
    spans = {}
    for n, a, b in _defs(lines):
        spans.setdefault(n, (a, b))
    rc = 0

    def body(name):
        """The definition as it was WRITTEN. These checks read string and char literals
        — the hex table, the [0-9a-f] bounds — and _code_only() blanks exactly those, so
        the comment-stripped copy is used for call sites and this for content."""
        a, b = spans.get(name, (0, -1))
        return "\n".join(raw[a:b + 1])

    def calls(name):
        a, b = spans.get(name, (0, -1))
        return "\n".join(lines[a:b + 1])

    # The hasher: one definition, hashing the NAME TO ITS NUL through the lower-case
    # table. The strlen() argument is the whole rule — the header's name field is a
    # padded array, so hashing the array instead is a stable, plausible id that matches
    # no file on the card and no byte the bake wrote.
    for need, why, sym in (
            (r"mbedtls_sha256\s*\(\s*\(const unsigned char \*\)\s*name\s*,\s*strlen\s*\(\s*name\s*\)",
             "hashes the name to its NUL — not the padded header array, which would "
             "issue a different id for the same theme depending on what was in the file "
             "before it", "theme_id"),
            (r'"0123456789abcdef"\s*\[\s*digest\[i\]\s*>>\s*4\s*\]',
             "builds the id's hex from the digest's high nibble first, lower-case", "theme_id"),
            (r"out\[THEME_ID_HEX\]\s*=\s*'",
             "NUL-terminates the id it hands back", "theme_id"),
            # The §6 pair: the rule the page's two badges come from. Its own vectors
            # run it, and these two lines keep the vectors aimed at the same rule.
            (r"\*active\s*=\s*same_name\s*&&\s*!geom_diff\s*&&\s*!art_diff\s*;",
             "calls a card file the dial's own only when the name and BOTH checksums agree",
             "theme_state"),
            (r"\*changed\s*=\s*same_name\s*&&\s*\(geom_diff\s*\|\|\s*art_diff\)\s*;",
             "offers a re-install only for the theme the dial is already wearing",
             "theme_state")):
        if sym not in spans:
            print("FAIL main/art/orb_theme.c: %s() is not a column-0 definition any more; this "
                  "lint can no longer prove it holds the identity rule" % sym)
            rc = 1
        elif not re.search(need, body(sym)):
            print("FAIL main/art/orb_theme.c: %s() no longer %s — the one rule this lint "
                  "exists to keep" % (sym, why))
            rc = 1

    # The validator: bounds the length BOTH ways and admits only [0-9a-f]. An id
    # arrives from a browser; this is the only thing between that string and a
    # directory walk that has to match it.
    for need, why in (
            (r"if\s*\(\s*id\s*==\s*NULL\s*\)\s*return false;", "refuses a missing id"),
            (r"if\s*\(\s*n\s*>=\s*THEME_ID_HEX\s*\)\s*return false;", "refuses an id that is too long"),
            (r"return\s+n\s*==\s*THEME_ID_HEX\s*;", "refuses an id that is too short"),
            (r"\(c\s*>=\s*'0'\s*&&\s*c\s*<=\s*'9'\)\s*\|\|\s*\(c\s*>=\s*'a'\s*&&\s*c\s*<=\s*'f'\)",
             "admits nothing outside [0-9a-f] — no '/', no uppercase, no traversal")):
        if not re.search(need, body("theme_id_ok")):
            print("FAIL main/art/orb_theme.c: theme_id_ok() no longer %s — a browser-supplied "
                  "id reaches the card walk unchecked" % why)
            rc = 1

    # Every consumer of a card theme applies the rule itself, through the one
    # definition. scan_entry lists, resolve_entry turns an id back into a file,
    # card_open_verified_id guards the bytes an install reads, orb_theme_upload_end
    # decides the filename an upload lands at, and orb_theme_boot names the slot it
    # found damaged. Drop the call in any one of them and the page offers a button
    # on a theme that cannot be used — the exact bug, one consumer at a time.
    CONSUMERS = ("scan_entry", "resolve_entry", "card_open_verified_id",
                 "orb_theme_upload_end", "orb_theme_boot")
    for n in CONSUMERS:
        if n not in spans:
            print("FAIL main/art/orb_theme.c: %s() is not a column-0 definition any more; the "
                  "lint can no longer prove it applies the naming rule" % n)
            rc = 1
            continue
        a, b = spans[n]
        seg = "\n".join(lines[a:b + 1])                     # comment-free: no prose counts
        if "theme_id(" not in seg and "theme_id_ok(" not in seg:
            print("FAIL main/art/orb_theme.c:%d: %s() touches a card theme without asking "
                  "theme_id()/theme_id_ok() what its name is — a file filed under a name it "
                  "does not call itself gets through, and the page can only offer a Load "
                  "button that fails" % (a + 1, n))
            rc = 1
    # The resolve path is TWO checks in TWO functions, and both are load-bearing:
    # resolve_entry hashes the header of the directory entry it matched (so a file
    # swapped into an id's place is refused), and card_open_verified_id then opens the
    # file it chose and hashes THAT header again, because the card can be edited on a
    # laptop in between. Delete either and a path opens whose bytes nobody re-checked.
    if "resolve_entry" not in spans:
        print("FAIL main/art/orb_theme.c: resolve_entry() is gone — nothing checks the card "
              "file's own header against the id it is filed under")
        rc = 1
    elif "theme_id(" not in calls("resolve_entry"):
        print("FAIL main/art/orb_theme.c:%d: resolve_entry() matches a directory entry without "
              "hashing its header name — any file renamed to an id's stem installs"
              % (spans["resolve_entry"][0] + 1))
        rc = 1
    if ("card_open_verified_id" in spans):
        seg = calls("card_open_verified_id")
        opened = seg.find("card_open(")
        if "theme_id_ok(" not in seg:
            print("FAIL main/art/orb_theme.c:%d: card_open_verified_id() stops validating the "
                  "id before it walks for it" % (spans["card_open_verified_id"][0] + 1))
            rc = 1
        if opened < 0 or seg.find("theme_id(", opened) < 0:
            print("FAIL main/art/orb_theme.c:%d: card_open_verified_id() no longer re-hashes the "
                  "header AFTER opening the file — the walk's check is a different card read, "
                  "and the bytes the install goes on to copy are the ones nobody re-verified"
                  % (spans["card_open_verified_id"][0] + 1))
            rc = 1
        # The re-hash has to be COMPARED. The mutation that survives a call-presence check
        # is the one that keeps the hash and drops the test: `if (false)` around the refusal
        # reads as verified code, compiles, and installs a file whose bytes do not belong to
        # the id that was asked for.
        if not re.search(r"strcmp\s*\(\s*id2\s*,\s*id\s*\)\s*!=\s*0", seg):
            print("FAIL main/art/orb_theme.c:%d: card_open_verified_id() re-hashes the opened "
                  "header without comparing the result to the id it was asked for — the "
                  "post-open check is a call that decides nothing"
                  % (spans["card_open_verified_id"][0] + 1))
            rc = 1
    # Pass one, the walk: an entry whose stem matched is accepted only when its own
    # header name hashes to that stem. Same shape as above — the assignment must be the
    # comparison, not a constant, or any file renamed to an id's stem installs under it.
    if "resolve_entry" in spans:
        if not re.search(r"verified\s*=\s*\(\s*strcmp\s*\(\s*id\s*,\s*r->want\s*\)\s*==\s*0\s*\)",
                         calls("resolve_entry")):
            print("FAIL main/art/orb_theme.c:%d: resolve_entry() no longer derives r->verified "
                  "from the header's hash matching the id asked for — a card file copied to an "
                  "id's filename installs as that theme"
                  % (spans["resolve_entry"][0] + 1))
            rc = 1
    # The list applies the same rule to what it renders, so a row can only ever exist for
    # a file that is named for its own name. (It is the row's `ok` flag the page's Load
    # button tests, which is why the comparison may not be short-circuited here either.)
    if "scan_entry" in spans:
        if not re.search(r"strcmp\s*\(\s*id\s*,\s*rec->name\s*\)", calls("scan_entry")):
            print("FAIL main/art/orb_theme.c:%d: scan_entry() lists a theme without checking the "
                  "filename against the hash of its header name — the row's button leads to a "
                  "file the resolve path will refuse"
                  % (spans["scan_entry"][0] + 1))
            rc = 1
    # The upload's duplicate policy: byte-identical re-send is idempotent, and "identical"
    # is the whole §6 tuple — name, both checksums, serial. Drop the name and a re-send
    # that collides on crc (two themes whose payloads happen to match) is quietly discarded
    # as "already on the card" for a theme the owner never uploaded.
    if "orb_theme_upload_end" in spans:
        seg = calls("orb_theme_upload_end")
        for pat, what in ((r"strcmp\s*\(\s*oh\.name\s*,\s*h\.name\s*\)", "the owner name"),
                          (r"oh\.geom_crc\s*==\s*h\.geom_crc", "the geometry checksum"),
                          (r"oh\.art_crc\s*==\s*h\.art_crc", "the pixel checksum"),
                          (r"oh\.serial\s*==\s*h\.serial", "the install serial")):
            if not re.search(pat, seg):
                print("FAIL main/art/orb_theme.c:%d: the upload's duplicate check no longer "
                      "compares %s — two different themes can be called the same file, and the "
                      "second one is discarded as a re-send"
                      % (spans["orb_theme_upload_end"][0] + 1, what))
                rc = 1
    if not rc:
        print("ok    card identity is one rule in two functions, applied by all %d consumers, "
              "verified on the walk and again on the open" % len(CONSUMERS))
    return rc


def install_lint():
    """An install may not make a slot live that it did not verify the way boot reads it.

    Four things, in the order they have to happen inside orb_theme_install(): the
    container is checksummed off the card (verify_container) before the copy, and the
    copy is read back out of the partition (slot_verify_pixels) and run through the
    same geometry validator boot runs (slot_parse_geom, adopt=false) before
    esp_partition_erase_range() — the one call that makes the previous slot gone. The
    bench found the hole this pins: the card's own "neon" carries intact checksums and
    a complication record the painter refuses to draw, so an install that verified
    only the pixels made it live and left the dial on the factory default with the
    reason in a log line nobody could read from the wall.

    It is a source check rather than a call of the function because the paths it
    orders touch FatFs and the flash driver: on the host there is no card to copy
    from and no slot to erase.
    """
    lines = _code_only(th_src).split("\n")
    raw_lines = th_src.split("\n")
    spans = {}
    for n, a, b in _defs(lines):
        spans.setdefault(n, (a, b))
    if "orb_theme_install" not in spans:
        sys.exit("no orb_theme_install() in main/art/orb_theme.c — this lint has nothing "
                 "to check, which means it silently stopped working")
    a, b = spans["orb_theme_install"]
    body = "\n".join(lines[a:b + 1])
    rc = 0
    erase = body.find("esp_partition_erase_range")
    if erase < 0:
        print("FAIL orb_theme_install(): no header erase — the commit that makes a slot "
              "live has gone missing along with the discipline around it"); rc = 1
    for need in ("verify_container(", "slot_verify_pixels(", "slot_parse_geom("):
        at = body.find(need)
        if at < 0:
            print("FAIL orb_theme_install(): never calls %s — it can commit a slot that "
                  "boot will refuse to draw, which costs the owner a reboot and a dial "
                  "they did not choose" % need)
            rc = 1
        elif erase >= 0 and at > erase:
            print("FAIL orb_theme_install(): %s runs AFTER the header erase — a failure "
                  "after that point has already destroyed the slot that worked" % need)
            rc = 1
    # Ruling D13's own half: slot A is never an install target. The predicate is
    # pinned in tools/host_tz_test.py's table; this pins the callers from naming it.
    if "SLOT_A_HDR" in body or "SLOT_A_ART" in body:
        print("FAIL orb_theme_install(): names slot A — the factory default's pixels "
              "live there, and an install aimed at it erases the fallback with it")
        rc = 1

    # ---- P6.3: the `staged` witness, and the bundle ingest's equivalent ordering ----
    # Same rule as FORMAT 2's, one function over: nothing may make a slot live before
    # the check that proves it drawable, and the ledger word must bracket the window
    # the erase opens (§20.2 step 4). A staged marker that outlives a refused install
    # would put two stories on one row, so both exits clear it.
    for fn, why_call in (("orb_theme_install", "staged_mark("),
                         ("orb_theme_install_bundle", "staged_mark(")):
        if fn not in spans:
            if fn == "orb_theme_install_bundle":
                print("FAIL main/art/orb_theme.c has no orb_theme_install_bundle() — the "
                      "P6.3 streaming ingest (§22.3) is gone, and the ledger still says it "
                      "shipped"); rc = 1
            continue
        fb = "\n".join(lines[spans[fn][0]:spans[fn][1] + 1])
        mk, cl, er = fb.find(why_call), fb.find("staged_clear("), fb.find("esp_partition_erase_range")
        if mk < 0 or cl < 0:
            print("FAIL %s(): does not bracket the copy with staged_mark()/staged_clear() "
                  "— a power cut inside it would leave the row silent (§20.4's word)" % fn)
            rc = 1
        elif er >= 0 and mk > er:
            print("FAIL %s(): marks staged AFTER the first erase — the window the word "
                  "testifies to was already open when it was written" % fn)
            rc = 1

    if "orb_theme_install_bundle" in spans:
        ba, bb_end = spans["orb_theme_install_bundle"]
        bb = "\n".join(lines[ba:bb_end + 1])
        # The bundle path verifies by a different door, and the door has to be the
        # same rules: region parse before the copy, bank rule settled inside the
        # streaming loop, and rebase (which edits offsets) before the erase.
        for need in ("orb_bundle_parse_region(", "orb_bundle_rebase(", "copy_bundle3("):
            at = bb.find(need)
            if at < 0:
                print("FAIL orb_theme_install_bundle(): never calls %s — it would commit "
                      "bytes nothing proved, which is the hole D13 and C8 both named" % need)
                rc = 1
        # P6.3 under option A: the ingest stores, and does not choose. A set_live here
        # would flip the dial to a bundle no compositor can draw yet, and the owner's
        # dial would come up default with an installed theme they never pressed.
        if "orb_theme_set_live" in bb:
            print("FAIL orb_theme_install_bundle(): calls orb_theme_set_live — a FORMAT 3 "
                  "slot is stored, not chosen, until the flip page (P6.4) can draw it")
            rc = 1
        # Slot A stays out of the bundle path too, for the D13 reason above.
        if "SLOT_A_HDR" in bb or "SLOT_A_ART" in bb:
            print("FAIL orb_theme_install_bundle(): names slot A — the D13 rule is not "
                  "format-specific")
            rc = 1

    if "staged_mark" not in th_src or "THEME_NVS_STAGED" not in th_src:
        print("FAIL main/art/orb_theme.c lost the staged witness"); rc = 1
    # The wire pair, and the shape it has to keep: an id and a sentence, like the
    # damage pair above it, so the page can put the word on the right row. Searched
    # bare, because in the handler's format string both are inside an escaped quote.
    for need in ("staged_id", "staged_why", "orb_theme_staged_id()", "orb_theme_staged_why()"):
        if need not in net_src:
            print("FAIL /api/v1/sd does not carry %s — the word exists in NVS and reaches "
                  "nobody, which is the log-line failure §20.4 exists to end" % need)
            rc = 1

    # ---- P6.4: the flip door — a bundle has exactly one caller and no reboot ----
    # The streaming ingest (P6.3) shipped unrouted; P6.4's sin would be routing it
    # wrong. Four things, all checkable without a card: the ingest has exactly one
    # caller and it does not reboot; the FORMAT 2 reader still refuses a FORMAT 3
    # slot so boot can name it; the scan lists a bundle with active never true; and
    # the page's store button posts to the bundle route and not the install one.
    if "bundle_job" in spans:
        ja, jb = spans["bundle_job"]
        jbody = "\n".join(lines[ja:jb + 1])
        if "orb_jobs_reboot_soon" in jbody:
            print("FAIL bundle_job() reboots — a stored bundle is not drawn, so the boot "
                  "shows the owner nothing they asked for; the store does not choose (P6.4)")
            rc = 1
        if "orb_theme_install_bundle(" not in jbody:
            print("FAIL bundle_job() does not call the streaming ingest — the flip page "
                  "would store through the FORMAT 2 door or not at all"); rc = 1
    else:
        print("FAIL main/art/orb_theme.c has no bundle_job() — orb_theme_post_bundle is "
              "the only thing that reaches the streaming ingest, and it has no worker"); rc = 1
    if "orb_theme_post_bundle" in spans:
        pa, pb = spans["orb_theme_post_bundle"]
        pbody = "\n".join(lines[pa:pb + 1])
        if "orb_jobs_post(bundle_job" not in pbody:
            print("FAIL orb_theme_post_bundle() does not queue bundle_job — the ingest "
                  "ran off the worker loses the busy gate and the card-lock rule"); rc = 1
    else:
        print("FAIL main/art/orb_theme.c has no orb_theme_post_bundle() — P6.4's route "
              "has nothing to post"); rc = 1
    # The ingest has exactly one caller (the job); a second one would be a second
    # opinion about card-lock discipline.
    if th_src.count("orb_theme_install_bundle(r->id") != 1:
        print("FAIL orb_theme_install_bundle() is not called from exactly one place — the "
              "one-worker rule that keeps the card lock singular is only held if the "
              "bundle_job is its sole caller"); rc = 1
    # The FORMAT 2 reader must refuse the tag, or boot's bundle row is a lie.
    if "slot_read" in spans:
        ra, rb = spans["slot_read"]
        rbody = "\n".join(lines[ra:rb + 1])
        if "s->h.fmt != THEME_FORMAT" not in rbody:
            print("FAIL slot_read() does not refuse a FORMAT 3 tag — the bundle would "
                  "read as a valid v2 slot and boot would refill garbage it never verified")
            rc = 1
    # scan's bundle branch, by its exact condition: a dead-coded `0 &&` keeps every
    # string below alive and kills the behaviour, so the strings are not enough.
    if "scan_entry" in spans:
        ea, eb = spans["scan_entry"]
        ebody = "\n".join(lines[ea:eb + 1])
        if ("if (why == THEME3_REFUSAL) {" not in ebody
                or "card_check3(" not in ebody
                or "e->active = false" not in ebody or "e->fmt = 3" not in ebody):
            print("FAIL scan_entry() lost its live bundle branch (exact condition, the "
                  "v3 door, the fmt stamp, active held false) — bundles would list as "
                  "broken themes and the flip page could never offer one")
            rc = 1
    # The upload door: FORMAT 3 header through the v3 door, region+art verified,
    # BEFORE the rename, and the duplicate tuple includes the format word.
    if "orb_theme_upload_end" in spans:
        ua, ub = spans["orb_theme_upload_end"]
        ubody = "\n".join(lines[ua:ub + 1])
        for need in ("hdr_at_any(", "verify_bundle3(", "oh.fmt == h.fmt"):
            if need not in ubody:
                print("FAIL orb_theme_upload_end() lost %s — an unverified FORMAT 3 "
                      "container would reach the card name it is filed under" % need)
                rc = 1
        rpos, upos = ubody.find("rename("), min((ubody.find(x) for x in ("hdr_at_any(", "verify_bundle3(") if ubody.find(x) >= 0), default=-1)
        if rpos >= 0 and upos >= 0 and upos > rpos:
            print("FAIL orb_theme_upload_end() verifies after the rename — the window "
                  "where a half-checked file carries a loadable name is the C8 bug")
            rc = 1
    # P6.4's second hardware lesson: a resolver that swallows the header's own
    # refusal makes R4's identity sentence the door's only words, and those words
    # are a lie about a well-named file of another format. Both ends pinned.
    if "resolve_entry" in spans:
        ra, rb = spans["resolve_entry"]
        rbody = chr(10).join(lines[ra:rb + 1])
        if "r->why = cr" not in rbody:
            print("FAIL resolve_entry() drops the header's refusal — the door can only "
                  "answer R4's identity line about a file whose real problem is format")
            rc = 1
    else:
        print("FAIL no resolve_entry() — the walk's refusal propagation has nothing to "
              "propagate from"); rc = 1
    if "card_open_verified_id" in spans:
        # _code_only blanks string contents, so the identity line is found by its
        # guard alone; the strings themselves must be read from the raw lines.
        va, vb = spans["card_open_verified_id"]
        prop = ident = -1
        for k in range(va, vb + 1):
            if prop < 0 and "if (!r.verified && r.why)" in raw_lines[k]:
                prop = k
            if ident < 0 and "does not match its id" in raw_lines[k]:
                ident = k
        if prop < 0 or ident < 0 or prop > ident:
            print("FAIL card_open_verified_id() does not speak the propagated reason "
                  "BEFORE the identity line — order is the whole fix")
            rc = 1
    c3 = spans.get("card_open_verified_id3")
    if not c3 or "*why = r.why" not in chr(10).join(lines[c3[0]:c3[1] + 1]):
        print("FAIL the FORMAT 3 door lost its twin propagation — the same D26 lie "
              "would speak at the bundle door about a FORMAT 2 file"); rc = 1
    # The staged sentence is the measured one, in the firmware and on the page.
    if "the dial shows the factory clock" not in th_src:
        print("FAIL STAGED_WHY lost the measured fallback sentence — §24.5(2) proved the "
              "old wording false on hardware; the correction must not rot back"); rc = 1
    # The page's store button hits /api/v1/sd/bundle through the bundle post. The
    # ROUTE TABLE line and the JSON format string are pinned, not their bare words:
    # the words survive in comments and argument lists when the wiring is gone,
    # which is exactly how two mutants here first survived.
    for need in ('{ .uri = "/api/v1/sd/bundle",   .method = HTTP_POST, .handler = h_bundle_install }',
                 "orb_theme_post_bundle(id)", "h_bundle_install(httpd_req_t",
                 "\\\"bundle_ms\\\":%u,\\\"bundle_bytes\\\":%u"):
        if need not in net_src:
            print("FAIL the flip door is missing %s from orb_net.c — P6.4's whole claim "
                  "is a route to the streaming ingest" % need); rc = 1
    if net_src.count("brings back the factory clock") != 2:
        print("FAIL the page's power-cut warning is not on BOTH confirm doors — the store "
              "and the install cost the same interruption and must say so alike"); rc = 1
    # The install route must NOT be able to store a bundle: it posts FORMAT 2 only.
    ka = net_src.find("static esp_err_t h_theme_install(")
    kb = net_src.find("\n}\n", ka)
    if ka > 0 and ("post_bundle" in net_src[ka:kb]):
        print("FAIL h_theme_install() posts a bundle job — the FORMAT 2 door must not "
              "reach the ingest; that is the same refusal-not-trust split as the scan")
        rc = 1

    if not rc:
        print("ok    install verifies off-card, on-partition and geometrically before "
              "the header erase, and never names slot A")
        print("ok    both install paths bracket the copy with the staged word, the bundle "
              "path parses-rebases-copies in that order, stores without choosing, and the "
              "sd body carries the word")
        print("ok    the flip door stores without rebooting, the FORMAT 2 reader refuses a "
              "bundle slot, the scan holds a bundle inactive, and the page posts /sd/bundle")
    return rc


def caps_lint():
    """The numbers the two sides share by name rather than by value.

    theme_schema.py is the single authority: the bake, the packer, this reader and
    the dial's arrays all take their widths and caps from it. Each of these lines is
    one place a rename or an appended enum value on one side would otherwise ship as
    a dial that skips a window the theme paid for, with nothing logged.
    """
    print("== the caps, word counts and enums the two sides share ==")
    rc = 0
    for sym, want in (("COMPS_MAX", ts.CLK_COMP_MAX),
                      ("AUX_MAX", ts.CLK_AUX_MAX),
                      ("BANK_MAX", ts.CLK_BANK_MAX),
                      ("COMP_WORDS", ts.words_of("comp")),
                      ("AUX_WORDS", ts.words_of("aux")),
                      ("BANK_WORDS", ts.words_of("bank")),
                      ("GEOM_COUNTS_V1", 4),
                      ("GEOM_COUNTS_V2", 7)):
        got = cdefine(sym)
        ok = got == want
        print("%-5s %-16s reader %4d  schema %4d" % ("ok" if ok else "FAIL", sym, got, want))
        if not ok: rc = 1
    m = re.search(r"#define CLK_STRIP_GLYPHS_MAX\s+(\d+)", th_hdr)
    if not m:
        sys.exit("could not find #define CLK_STRIP_GLYPHS_MAX in " + TH_H
                 + " — the strip cap has no definition left to compare")
    ok = int(m.group(1)) == ts.CLK_STRIP_GLYPHS
    print("%-5s %-16s reader %4d  schema %4d"
          % ("ok" if ok else "FAIL", "CLK_STRIP_GLYPHS", int(m.group(1)), ts.CLK_STRIP_GLYPHS))
    if not ok: rc = 1
    # The header's own guard, read rather than trusted: it pins the C enum's length
    # to a literal, and that literal has to be the length of the Python tuple.
    m = re.search(r"_Static_assert\(CLK_SRC__MAX == (\d+)", th_hdr)
    if not m:
        sys.exit("orb_theme.h no longer asserts the source enum's length — the "
                 "17 driving values of theme_schema.py are unpinned")
    ok = int(m.group(1)) == len(ts.SOURCES)
    print("%-5s %-16s reader %4d  schema %4d"
          % ("ok" if ok else "FAIL", "CLK_SRC__MAX", int(m.group(1)), len(ts.SOURCES)))
    if not ok: rc = 1
    # Names, not just lengths: a reordered enum keeps the count and silently swaps
    # a day window for a month one, which is the bad-art-shaped bug every other
    # check here is written to catch.
    for stem, want in (("CLK_KIND_", ts.KINDS), ("CLK_SRC_", ts.SOURCES)):
        names = re.findall(r"\b(" + stem + r"[A-Z0-9_]+)\b", rec_types)
        names = [n for n in dict.fromkeys(names) if not n.endswith("__MAX")]
        got = tuple(n[len(stem):].lower() for n in names)
        ok = got == tuple(want)
        print("%-5s %-16s order matches the schema tuple" % ("ok" if ok else "FAIL", stem + "*"))
        if not ok:
            print("      reader:", got, "\n      schema:", tuple(want))
            rc = 1

    # P6.4's harness runs one translation unit: the lifted bundle reader shares
    # orb_theme.c's word-index enum instead of carrying a second copy C would reject
    # as duplicate enumerators. That sharing is only sound while the two enums are
    # the same map — orb_bundle.c's comment claims it; this line makes it a checked
    # fact. Drift here silently changes which word the reader calls FORMAT, so the
    # whole P6.4 gate would read the wrong byte and still print ok.
    _e_th = re.search(r"enum \{[^}]*W_MAGIC[^}]*\}", th_src)
    _e_b3 = re.search(r"enum \{[^}]*W_MAGIC[^}]*\}", b3_src)
    def _emap(m):
        return re.sub(r"\s+", "", re.sub(r"/\*[\s\S]*?\*/", "", m.group(0)))
    if not _e_th or not _e_b3:
        print("FAIL a word-index enum is unfindable — caps_lint no longer knows the map"); rc = 1
    elif _emap(_e_th) != _emap(_e_b3):
        print("FAIL orb_bundle.c and orb_theme.c word maps differ — the shared-enum lift "
              "is reading FORMAT from the wrong word, so every P6.4 gate claim is void")
        rc = 1
    else:
        print("ok    orb_bundle.c's word map is orb_theme.c's, byte for byte")
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
if rc == 0:
    rc = geom_arena_lint()
sys.exit(rc)

