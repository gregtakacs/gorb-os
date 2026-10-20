/**
 * orb_theme.c — see orb_theme.h for the model. An installed theme is a container
 * from the card copied into one of two A/B slots in the `assets` partition, and
 * the geometry it carries is what clock_view.c reads instead of the compiled-in
 * one.
 *
 * The wire format is DEFINED in tools/orbtheme.py and mirrored here. That mirror
 * is the risk in this file: a field added on one side only produces a dial whose
 * hand pivots read as overlay offsets, which looks like bad art and not like a
 * bug. Three things hold it together — the static asserts on the header layout,
 * the bounds checks in orb_theme_geom_parse() that refuse a theme rather than
 * render it wrong, and the sentinel test in tools/host_tz_test.py that packs
 * through the Python and reads back through this C lifted verbatim.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>                 // PRIu32 in the complication-loop logs (the
                                      // riscv toolchain's uint32_t is not `int`)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"              // the listing timing in orb_theme_scan()
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "orb_theme.h"
#include "orb_bundle.h"
#include "orb_sd.h"
#include "orb_jobs.h"      /* the two post_* calls: an install is a worker job, never a poll */
#include "orb_files.h"      /* the OS owns the card walk; this layer interprets what it finds */
#include "art_bank.h"
#include "mbedtls/sha256.h"  /* theme_id(): sha256 over the header name -> the card filename */

static const char *TAG = "orb_theme";

/* Settings live in their OWN namespace in the `storage` partition, as
 * partitions.csv demands: esp_wifi owns "nvs.net80211" and orb_net owns
 * "orb.net". A chosen theme is device config the owner picked, so it has to
 * survive the same things the timezone does — including main.c's recovery
 * nvs_flash_erase() of the default partition. */
#define THEME_NVS_PARTITION "storage"
#define THEME_NVS_NAMESPACE "orb.settings"
/* Which slot the dial draws — THEME_LIVE_DEFAULT or THEME_LIVE_CARD. A CHOICE the
 * owner made, not an operation waiting for a reboot: D13 deleted the pending key,
 * because a staged install that survives into a boot is a theme change the owner
 * can no longer cancel and a failure the owner cannot see coming. */
#define THEME_NVS_LIVE      "theme_live"
/* D30's §20.4 ledger word, and NOT the key D13 deleted. This one changes nothing
 * about what the glass draws: boot resolves the choice key as ever, commits only
 * on a valid header, and then reports here whether a flip that had already swapped
 * its pixels never got its header written. A witness, not a pending operation —
 * which is the distinction that lets it exist under D13's own reasoning. The value
 * is the staging theme's 12-hex id, so the word lands on that theme's row and not
 * on the row of whatever the dial is actually showing. */
#define THEME_NVS_STAGED    "theme_staged"

/* Container header words. Mirrors tools/orbtheme.py's W_* map; the sentinel test
 * is what keeps the two lists in step. FORMAT 2 collapses the old slug+title pair
 * into ONE name field at word 14 that occupies exactly the bytes the pair used, so
 * W_HDR_CRC stays 28 and W_WORDS stays 32. */
enum {
    W_MAGIC = 0, W_FORMAT, W_GEOM_ABI, W_HDR_SIZE,
    W_GEOM_OFF, W_GEOM_SIZE, W_GEOM_CRC,
    W_ART_SIZE, W_ART_CRC,
    W_SCR_W, W_SCR_H, W_SERIAL, W_BAKED_MS, W_ART_OFF,
    W_NAME = 14,        /* NAME_MAX bytes -> words 14..27 */
    W_HDR_CRC = 28,     /* crc32 of all 128 B with THIS word zeroed */
    W_WORDS = 32
};
_Static_assert(W_HDR_CRC == W_NAME + (56 + 3) / 4,
               "the header checksum must start where the name field ends");
_Static_assert(W_WORDS == 32, "container header must be 128 bytes");

#define THEME_MAGIC    0x474F5254u   /* 'T','R','O','G' — the same constant twice */
#define THEME_FORMAT   2
#define THEME_GEOM_ABI 1
#define THEME_SECTOR   4096
#define THEME_NAME_MAX 56             /* one name field; 55 usable bytes + NUL */
#define THEME_ID_HEX   12             /* sha256(name)[:12] — the card filename stem */

/* Slot layout, private to this file. The two within-slot offsets are the only
 * numbers here; a slot's absolute addresses are its base plus those. Both are
 * 64 KB aligned because 64 KB is esp_partition_mmap's page, and an unaligned start
 * wastes an MMU entry on rounding at each end. The geometry sits in its own sector
 * at +0x1000 rather than tight against the 128-byte header so that making a slot
 * live — an erase plus a rewrite of its header block — cannot touch anything
 * underneath it. */
#define SLOT_A_HDR      0x000000u
#define SLOT_B_HDR      0x00400000u
#define SLOT_GEOM_OFF   0x00001000u                 /* within a slot */
#define SLOT_ART_OFF    0x00010000u                 /* within a slot */
#define SLOT_A_ART      (SLOT_A_HDR + SLOT_ART_OFF)
#define SLOT_B_ART      (SLOT_B_HDR + SLOT_ART_OFF)
#define SLOT_GEOM_CAP   (SLOT_ART_OFF - SLOT_GEOM_OFF)   /* 60 KB of tables */
#define SLOT_CAP        0x00400000u                      /* 4 MB per slot    */

/* Wire counts. THEME_SCALARS_N must equal len(orbtheme.THEME_FIELDS) and
 * SPRITE_WORDS_N len(orbtheme.SPRITE_FIELDS): a mismatch shifts every field after
 * it, and the sentinel test is what notices. */
#define THEME_SCALARS_N  26
#define SPRITE_WORDS_N   15
#define TOURB_MAX        16                        /* CLK_TOURB_MAX */
#define SPRITES_TOTAL    (5 + 5 + 1 + TOURB_MAX)   /* hands, shadows, overlay, tb */
#define RECT_WORDS       5                         /* x, y, w, h, off */

/* The complication/indicator tables of tools/theme_schema.py. COMPS_MAX etc.
 * are the same caps the bake refuses to exceed, so a theme that would need a
 * bigger firmware array is refused as unloadable instead of installing and
 * silently losing its extra windows. */
#define GEOM_COUNTS_V1   4                          /* rects, first, delays, bgf  */
#define GEOM_COUNTS_V2   7                          /* + comps, aux rows, bank    */
#define COMPS_MAX        16                         /* CLK_COMP_MAX               */
#define AUX_MAX          8                          /* CLK_AUX_MAX                */
#define BANK_MAX         256                        /* CLK_BANK_MAX               */
#define COMP_WORDS       26                         /* len(theme_schema.COMP_FIELDS) */
#define AUX_WORDS        24                         /* 15 sprite + 9 driving words   */
#define BANK_WORDS       SPRITE_WORDS_N

/* Words a blob of each layout must hold, given its own counts — see geom_counts()
 * below, which is the definition and the only caller-facing form of this rule. */

/* Bounds the loader accepts, from clock_view.c's own array sizes and from what
 * the shipped bake measures (128 rects, 8 GIF frames). A theme wanting more than
 * the dial holds is refused as unloadable rather than installed and then clamped
 * at paint time into a half-drawn frame. */
#define RECTS_MAX       2048
#define GIFS_MAX        32                         /* CLK_GIF_MAX */
#define BG_FRAMES_MAX   8                          /* CLK_BG_MAX  */

/* A sprite occupies colour w*h*2 then alpha w*h. Must match sprite_bytes() in
 * clock_view.c, which is the code these offsets are checked against. */
#define SPRITE_BYTES(s) ((uint64_t)(s)->w * (uint64_t)(s)->h * 3u)
#define PLATE_BYTES     ((uint64_t)CLK_SCR_W * (uint64_t)CLK_SCR_H * 2u)


/* ------------------------------------------------------------------------- *
 * Header + geometry blob -> clk_theme_t. Pure: no allocator, no card, no
 * partition. tools/host_tz_test.py calls these with static buffers against a blob
 * packed by tools/orbtheme.py with a distinct sentinel in every field, so "both
 * sides agree on the field order" is a green line and not a hope.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t geom_off, geom_size;
    uint32_t art_off,  art_size;
    uint32_t geom_crc, art_crc;
    uint32_t serial;
    uint32_t n_rects, n_first, n_delays, n_bgf;
    uint32_t n_comp, n_aux, n_bank;      /* the complication tables; 0 for a
                                         * four-count blob, and the length is
                                         * what proves they are really zero */
    char     name[THEME_NAME_MAX + 1];   /* the theme's one true name; the card
                                          * filename is theme_id(name), never this */
    uint32_t fmt;                        /* 2 or 3, stamped by hdr_parse/hdr_at_any;
                                          * FORMAT 2's readers refuse to treat a 3
                                          * as a theme, and P6.4's doors split on
                                          * this word. Unset (0) means unreadable. */
    uint32_t n_sections;                 /* fmt 3 only: the index's app count */
} orb_thdr_t;

typedef struct {
    const uint32_t *w;
    size_t          n, i;
    const char     *why;                 /* first reason this is not a theme */
} rd_t;

static uint32_t rdt(rd_t *r, const char *what)
{
    if (r->why) return 0;                /* already refused; stay in range */
    if (r->i >= r->n) { r->why = what; return 0; }
    return r->w[r->i++];
}

static uint16_t rd_u16(rd_t *r, const char *w) { return (uint16_t)rdt(r, w); }
static int16_t  rd_i16(rd_t *r, const char *w) { return (int16_t)rdt(r, w); }
static uint8_t  rd_u8 (rd_t *r, const char *w) { return (uint8_t)rdt(r, w); }
static int8_t   rd_i8 (rd_t *r, const char *w) { return (int8_t)rdt(r, w); }

/* One clk_sprite_t, in tools/orbtheme.py's SPRITE_FIELDS order. Each read names the
 * field it wanted, so the first out-of-range word says WHICH one: "blob too short"
 * is the useless version of that message, and it is the one somebody reads over
 * USB serial with the case still screwed shut. */
static void rd_sprite(rd_t *r, clk_sprite_t *s, const char *tag)
{
    s->w         = rd_u16(r, tag);
    s->h         = rd_u16(r, tag);
    s->offx      = rd_i16(r, tag);
    s->offy      = rd_i16(r, tag);
    s->pivotx    = rd_i16(r, tag);
    s->pivoty    = rd_i16(r, tag);
    s->centerx   = rd_i16(r, tag);
    s->centery   = rd_i16(r, tag);
    s->blend     = rd_u8 (r, tag);
    s->show      = rd_u8 (r, tag);
    s->color_off = rdt   (r, tag);
    s->diffx     = rd_i16(r, tag);
    s->diffy     = rd_i16(r, tag);
    s->diffw     = rd_u16(r, tag);
    s->diffh     = rd_u16(r, tag);
}

/* One complication record, in tools/theme_schema.py's COMP_FIELDS order, and one
 * aux-hand record in AUX_FIELDS order. Same reason rd_sprite exists: the field
 * named in the refusal is the one somebody can act on, and the order is the
 * whole contract between the bake and the dial. */
static void rd_comp(rd_t *r, clk_comp_t *c, const char *tag)
{
    c->tag         = rd_u16(r, tag);
    c->kind        = rd_u8 (r, tag);
    c->src         = rd_u8 (r, tag);
    c->x           = rd_i16(r, tag);
    c->y           = rd_i16(r, tag);
    c->w           = rd_u16(r, tag);
    c->h           = rd_u16(r, tag);
    c->count       = rd_u16(r, tag);
    c->first       = rd_u16(r, tag);
    c->off         = rdt   (r, tag);
    c->frame_bytes = rdt   (r, tag);
    c->cx          = rd_i16(r, tag);
    c->cy          = rd_i16(r, tag);
    c->radius      = rd_i16(r, tag);
    c->angle0      = rd_i16(r, tag);
    c->angle_step  = rd_i16(r, tag);
    c->value_min   = rd_i16(r, tag);
    c->scale       = rd_u16(r, tag);
    c->glyph_step  = rd_i16(r, tag);
    c->glyph_lead  = rd_i16(r, tag);
    c->edge_left   = rd_u16(r, tag);
    c->edge_right  = rd_u16(r, tag);
    c->flags       = rd_u16(r, tag);
    c->blend       = rd_u8 (r, tag);
    c->show        = rd_u8 (r, tag);
    c->reserve     = rdt   (r, tag);
}

static void rd_aux(rd_t *r, clk_aux_t *a, const char *tag)
{
    rd_sprite(r, &a->spr, tag);
    a->tag       = rd_u16(r, tag);
    a->src       = rd_u8 (r, tag);
    a->kind      = rd_u8 (r, tag);
    a->host      = rd_u16(r, tag);
    a->span      = rd_i16(r, tag);
    a->zero      = rd_i16(r, tag);
    a->value_min = rd_u16(r, tag);
    a->value_max = rd_u16(r, tag);
    a->flags     = rd_u16(r, tag);
}

static void rd_wordstr(const uint32_t *w, int base, char *dst, int cap)
{
    int i = 0;
    for (; i < cap - 1; i++) {
        const char c = (char)((w[base + i / 4] >> (8 * (i % 4))) & 0xFF);
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = '\0';
}

/* The FORMAT 3 gate's one sentence, and the accept signal, in one definition.
 * Callers that can install a bundle compare the returned POINTER rather than
 * rephrasing what a caller that cannot should show, so the wording lives once
 * and the rule cannot drift (the method R4 set for the identity refusal).
 * The pointer is the fact; the text only rides it. */
static const char THEME3_REFUSAL[] =
    "that is a FORMAT 3 app bundle; this firmware installs "
    "single-app themes only — update the dial to load it";

/* NULL on success, otherwise the reason this file is not a theme this device can
 * load — in words, because every one of them is what the page shows next to the
 * file the owner just uploaded. */
static const char *hdr_parse(const uint32_t *w, orb_thdr_t *o)
{
    if (w[W_MAGIC]    != THEME_MAGIC)     return "not an .orbtheme file";
    if (w[W_FORMAT]   != THEME_FORMAT) {
        /* FORMAT 3 is not unreadable-by-accident: it is a bundle this build
         * stores through its own door and cannot draw yet (P6.4), so a
         * well-formed one returns THEME3_REFUSAL above for the pointer
         * comparison. A bundle whose header this build cannot check falls to
         * the generic line WITH ITS FORMAT WORD UNVERIFIED — measured on
         * hardware 2026-10-19 (§24.7), and the FORMAT 3 rows of
         * host_sd_test.py pin the order now: the reader is consulted only
         * after the format word, and its refusal is spoken only after the
         * reader's own checksum settles. No sentence names a file as any
         * format before its 128 bytes have proven themselves. */
        orb_bundle3_hdr_t b3;
        if (w[W_FORMAT] == ORB_BUNDLE3_FORMAT &&
            orb_bundle3_hdr_parse(w, &b3) == NULL)
            return THEME3_REFUSAL;
        return "theme format this firmware cannot read";
    }
    if (w[W_GEOM_ABI] != THEME_GEOM_ABI)  return "baked for a different dial — re-bake it";
    o->fmt = THEME_FORMAT;                /* the only format that reaches here */
    o->n_sections = 0;

    /* The self-checksum first: it covers 128 bytes, so it costs nothing, and a
     * torn header that claims a 4 MB payload would otherwise be streamed, hashed
     * and written to flash before anything noticed. */
    uint32_t zeroed[W_WORDS];
    memcpy(zeroed, w, sizeof(zeroed));
    zeroed[W_HDR_CRC] = 0;
    if (w[W_HDR_CRC] != esp_rom_crc32_le(0, (const uint8_t *)zeroed, sizeof(zeroed)))
        return "container header is damaged — re-copy the file onto the card";

    o->geom_off  = w[W_GEOM_OFF];   o->geom_size = w[W_GEOM_SIZE];
    o->art_off   = w[W_ART_OFF];    o->art_size  = w[W_ART_SIZE];
    o->geom_crc  = w[W_GEOM_CRC];   o->art_crc   = w[W_ART_CRC];
    o->serial    = w[W_SERIAL];
    if (w[W_SCR_W] != CLK_SCR_W || w[W_SCR_H] != CLK_SCR_H)
        return "baked for a different panel size";
    /* Where the two parts sit is the CALLER's rule, not this one: a container FILE
     * puts its blob right after the header, an INSTALLED slot puts it at
     * SLOT_GEOM_OFF. Both check against their own layout. Sizes are checked here
     * because they decide how many bytes get streamed and how much RAM the
     * geometry lands in. */
    if (o->geom_size < (GEOM_COUNTS_V1 + THEME_SCALARS_N
                        + (uint32_t)SPRITES_TOTAL * SPRITE_WORDS_N) * 4u
        || o->geom_size > SLOT_GEOM_CAP)
        return "geometry blob size is not credible for this dial";
    if (o->art_size < PLATE_BYTES || o->art_size > SLOT_CAP)
        return "pixel payload is bigger than a theme slot — re-bake it";
    rd_wordstr(w, W_NAME, o->name, sizeof(o->name));
    /* An empty name is refused here, at parse: the card filename is derived from
     * it, so a nameless theme would hash to one constant and collide with every
     * other nameless theme. This is the one line that stops that, and it agrees
     * with parse_header() in tools/orbtheme.py. */
    if (o->name[0] == '\0')
        return "theme has no name in its header — re-bake it";
    o->n_rects = o->n_first = o->n_delays = o->n_bgf = 0;
    o->n_comp = o->n_aux = o->n_bank = 0;
    return NULL;
}

/* The blob's length is fully determined by its counts, so a header whose numbers
 * do not add up is refused before a single pixel is read. Both the four-count
 * layout every theme baked before the complication tables shipped, and the
 * seven-count layout that carries them, are accepted: which one this is comes
 * from the length, because the two rules cannot both fit one blob. */
static int geom_counts(const uint32_t *w, size_t nwords, uint32_t c[7])
{
    static const int ns[2] = { GEOM_COUNTS_V1, GEOM_COUNTS_V2 };
    for (int k = 0; k < 2; k++) {
        const int n = ns[k];
        if (nwords < (size_t)n) continue;
        uint32_t t[7] = { 0 };
        for (int i = 0; i < n; i++) t[i] = w[i];
        if (t[4] > COMPS_MAX || t[5] > AUX_MAX || t[6] > BANK_MAX) continue;
        const uint64_t rows = (uint64_t)t[4] * COMP_WORDS + (uint64_t)t[5] * AUX_WORDS
                            + (uint64_t)t[6] * BANK_WORDS;
        const uint64_t body = (uint64_t)(t[0] * RECT_WORDS + t[1] + t[2] + t[3])
                            + (uint64_t)(THEME_SCALARS_N + SPRITES_TOTAL * SPRITE_WORDS_N);
        if ((uint64_t)(n + body + rows) * 4u == (uint64_t)nwords * 4u) {
            for (int i = 0; i < 7; i++) c[i] = t[i];
            return n;
        }
    }
    return -1;
}

/* One canonical card identity, mirrored from tools/orbtheme.py's theme_id(): the
 * card filename stem is hex(sha256(name))[:12], LOWERCASE, over the name bytes up
 * to its NUL — never the padded THEME_NAME_MAX-byte array, so "Neon" hashes exactly
 * as hashlib.sha256(b"Neon") and a stored name never picks up trailing zero bytes as
 * identity. The theme's one true name is the string inside its container; the card
 * filename is derived from it and is opaque hex no human types or labels with.
 *
 * The host harness lifts this function verbatim and pins it against the Python
 * version on known-answer rows (see tools/host_sd_test.py), so "the two sides agree
 * on the hash input" is a green line and not a hope. */
static void theme_id(const char *name, char out[THEME_ID_HEX + 1])
{
    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *)name, strlen(name), digest, /*is224=*/0);
    for (int i = 0; i < THEME_ID_HEX / 2; i++) {
        out[2 * i]     = "0123456789abcdef"[digest[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[digest[i] & 0x0F];
    }
    out[THEME_ID_HEX] = '\0';
}

/* Is `id` a card filename stem this device could have issued? Exactly
 * THEME_ID_HEX (12) chars of [0-9a-f] and nothing else: uppercase, 11 or 13 chars,
 * '/' and "../" are all refused before the card is touched. An id arrives back from
 * the browser, but it never reaches a path builder — orb_theme_resolve_id() finds the
 * file by walking the directory and then re-verifies the header name; the id only
 * selects which directory entry to accept. */
static bool theme_id_ok(const char *id)
{
    if (id == NULL) return false;
    size_t n = 0;
    for (const char *p = id; *p; ++p, ++n) {
        if (n >= THEME_ID_HEX) return false;          /* too long */
        const char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return n == THEME_ID_HEX;                          /* and no shorter */
}

/* The public form, so the HTTP handlers can reject a malformed id before calling
 * install/delete/post_install instead of letting each re-implement the 12-hex rule.
 * The card layer re-checks it again on the resolve path — one line of defence at the
 * edge, one at the door, and both hold the same truth from this single definition. */
bool orb_theme_id_ok(const char *id) { return theme_id_ok(id); }

/* Fill `out` from the blob; the caller allocates, which keeps this free of side
 * effects for the host test. Returns NULL or the reason to refuse the theme.
 *
 * `art_size` is the payload length the container declared, and every sprite, rect
 * and full-screen plane is checked against it here. That is the memory-safety half
 * of the feature: the pixels are mmap'd at exactly art_size bytes, so an extent
 * past the end of the mapping is a crash on the first paint after a reboot nobody
 * is watching. An unusable theme beats a wall clock that stops telling the time. */
const char *orb_theme_geom_parse(const uint32_t *words, size_t nwords,
                                 uint32_t art_size, orb_theme_geo_t *out)
{
    rd_t r = { .w = words, .n = nwords };
    clk_theme_t *t = out->theme;

    /* The counts decide everything after them, including how many of them there
     * are: four in a blob baked before the complication tables existed, seven in
     * one that carries them. geom_counts() derives the layout from the blob's own
     * length, so a four-count theme keeps loading with the firmware change and
     * nobody has to re-bake art they already own. */
    uint32_t cnt[7];
    const int n_counts = geom_counts(words, nwords, cnt);
    if (n_counts < 0) return "geometry blob length does not match its own tables";
    r.i = (size_t)n_counts;
    const uint32_t n_rects  = cnt[0], n_first = cnt[1];
    const uint32_t n_delays = cnt[2], n_bgf   = cnt[3];
    const uint32_t n_comp   = cnt[4], n_aux   = cnt[5], n_bank = cnt[6];
    if (n_rects  > RECTS_MAX)      return "too many delta rects for this dial";
    if (n_first  > GIFS_MAX + 1)   return "too many GIF frame indices";
    if (n_delays > GIFS_MAX)       return "too many GIF delays";
    if (n_bgf    > BG_FRAMES_MAX)  return "too many background frames";
    /* The caller allocates these from the same counts, so a NULL alongside a
     * non-zero count is a loader bug rather than a bad theme — say which. */
    if ((n_comp && !out->comps) || (n_aux && !out->aux) || (n_bank && !out->bank))
        return "loader gave no room for the complication tables";
    out->n_comps = (uint16_t)n_comp;
    out->n_aux   = (uint16_t)n_aux;
    out->n_bank  = (uint16_t)n_bank;

    /* 26 scalars, tools/orbtheme.py's THEME_FIELDS order. */
    t->bg              = rdt(&r, "bg");
    t->plate_follow    = rd_u8 (&r, "plate_follow");
    t->shadow_on       = rd_u8 (&r, "shadow_on");
    t->shadow_dx       = rd_i8 (&r, "shadow_dx");
    t->shadow_dy       = rd_i8 (&r, "shadow_dy");
    t->second_sweep    = rd_u8 (&r, "second_sweep");
    t->second_railway  = rd_u8 (&r, "second_railway");
    t->text_over_hands = rd_u8 (&r, "text_over_hands");
    t->tick_rate       = rd_u8 (&r, "tick_rate");
    t->bg_frames       = rd_u16(&r, "bg_frames");
    t->bg_fps          = rd_i16(&r, "bg_fps");
    t->bg_loop         = rd_u8 (&r, "bg_loop");
    t->bg_every        = rd_u16(&r, "bg_every");
    t->plate_off       = rdt(&r, "plate_off");
    for (int i = 0; i < 5; i++) t->order[i] = rd_u8(&r, "order");
    t->order_n         = rd_u8 (&r, "order_n");
    t->matte_mask      = rd_u8 (&r, "matte_mask");
    t->bg_mode         = rd_u8 (&r, "bg_mode");
    t->bg_key_off      = rdt(&r, "bg_key_off");
    t->gif_frames      = rd_u16(&r, "gif_frames");
    t->tourbillon_n    = rd_u16(&r, "tourbillon_n");
    t->tourbillon_fps  = rd_u16(&r, "tourbillon_fps");
    if (r.why) return r.why;

    if (t->order_n > 5)                return "layer order longer than 5";
    if (t->tourbillon_n > TOURB_MAX)   return "more tourbillon frames than the dial holds";
    if (t->gif_frames > GIFS_MAX)      return "more GIF frames than the dial holds";
    if (t->bg_frames  > BG_FRAMES_MAX) return "more background frames than the dial holds";
    /* bg_mode 1 is the delta-rect bank: two frames minimum to be a loop, and the
     * index table is one longer than the count because clock_view walks rects as
     * [first[i], first[i+1]) — see clock_view.c:461. */
    if (t->bg_mode == 1 && t->gif_frames < 2)
        return "delta-rect background with fewer than two frames";
    if (n_first  && n_first  < (uint32_t)t->gif_frames + 1u)
        return "GIF index table too short for gif_frames";
    if (n_delays && n_delays < (uint32_t)t->gif_frames)
        return "GIF delay table too short for gif_frames";

    for (int i = 0; i < 5; i++)         rd_sprite(&r, &t->hand[i],       "hand");
    for (int i = 0; i < 5; i++)         rd_sprite(&r, &t->shadow[i],     "shadow");
    rd_sprite(&r, &t->overlay,                                             "overlay");
    for (int i = 0; i < TOURB_MAX; i++) rd_sprite(&r, &t->tourbillon[i], "tourbillon");
    if (r.why) return r.why;

    /* Extents, layer by layer. A layer the dial never looks at (show == 0, or a
     * zero w/h) is not checked: clock_view.c will not read it, and refusing a bake
     * for the geometry of a layer it does not draw would only block the theme. */
    if (t->overlay.w && t->overlay.h &&
        (uint64_t)t->overlay.color_off + SPRITE_BYTES(&t->overlay) > art_size)
        return "overlay pixels lie outside the declared payload";
    for (int i = 0; i < 5; i++) {
        const clk_sprite_t *h = &t->hand[i];
        if (h->show && h->w && h->h &&
            (uint64_t)h->color_off + SPRITE_BYTES(h) > art_size)
            return "hand pixels lie outside the declared payload";
        const clk_sprite_t *d = &t->shadow[i];
        if (t->shadow_on && d->show && d->w && d->h &&
            (uint64_t)d->color_off + SPRITE_BYTES(d) > art_size)
            return "shadow pixels lie outside the declared payload";
    }
    for (int i = 0; i < (int)t->tourbillon_n; i++) {
        const clk_sprite_t *s = &t->tourbillon[i];
        if (s->w && s->h && (uint64_t)s->color_off + SPRITE_BYTES(s) > art_size)
            return "tourbillon pixels lie outside the declared payload";
    }
    /* The plate and the GIF keyframe are full-screen and read unconditionally in
     * their modes, so they are checked although they are not sprites. */
    if ((uint64_t)t->plate_off + PLATE_BYTES > art_size)
        return "plate pixels lie outside the declared payload";
    if (t->bg_mode == 1 && (uint64_t)t->bg_key_off + PLATE_BYTES > art_size)
        return "GIF keyframe lies outside the declared payload";


    /* The variable tables, sized by the counts validated above. */
    for (uint32_t i = 0; i < n_rects; i++) {
        clk_bg_rect_t *rc = &out->rects[i];
        rc->x   = rd_u16(&r, "rect.x");
        rc->y   = rd_u16(&r, "rect.y");
        rc->w   = rd_u16(&r, "rect.w");
        rc->h   = rd_u16(&r, "rect.h");
        rc->off = rdt(&r, "rect.off");
        if (r.why) return r.why;
        /* clock_view.c:469 paints a rect as the byte run off+x .. off+x+w*2 and
         * mirrors it into the canvas at x,y, so BOTH extents have to hold. This is
         * the check a half-copied or hand-edited file fails, and the only one that
         * catches it before the dial reads off the end of the mapping. */
        if ((uint64_t)rc->off + (uint64_t)rc->x + (uint64_t)rc->w * 2u > art_size)
            return "delta rect lies outside the declared payload";
        if ((int)rc->x + (int)rc->w > CLK_SCR_W || (int)rc->y + (int)rc->h > CLK_SCR_H)
            return "delta rect lies outside the panel";
    }
    for (uint32_t i = 0; i < n_first; i++) {
        out->rect_first[i] = rd_u16(&r, "rect_first");
        if (r.why) return r.why;
        /* first[] indexes the rect table, and clock_view reads first[tgt+1] — the
         * +1 is covered because the baker emits gif_frames+1 entries and the count
         * check above demanded it. */
        if (out->rect_first[i] > n_rects) return "frame index runs past the rect table";
    }
    for (uint32_t i = 0; i < n_delays; i++) {
        out->gif_delays[i] = rd_u16(&r, "gif_delay");
        if (r.why) return r.why;
    }
    for (uint32_t i = 0; i < n_bgf; i++) {
        out->bg_frames[i] = rdt(&r, "bg_frame_off");
        if (r.why) return r.why;
        if ((uint64_t)out->bg_frames[i] + PLATE_BYTES > art_size)
            return "background frame lies outside the declared payload";
    }
    /* The complication tables. A four-count blob reaches here with all three
     * counts zero and allocates nothing, which is what keeps the paint path's
     * `if (n_comps)` test meaningful: a NULL is never merely unallocated. */
    for (uint32_t i = 0; i < n_comp; i++) {
        clk_comp_t *c = &out->comps[i];
        rd_comp(&r, c, "comp");
        if (r.why) return r.why;
        if (c->reserve)             return "comp record uses a reserved word — re-bake it";
        if (!c->count)              return "comp has no frames to index";
        if (c->show && (!c->w || !c->h)) return "comp is shown but has no size";
        if ((int)c->x + (int)c->w > CLK_SCR_W || (int)c->y + (int)c->h > CLK_SCR_H)
            return "comp box lies outside the panel";
        /* A kind or a driving value this dial cannot draw is one skipped row, not a
         * refused theme: the word layout is the contract, so an older dial reads the
         * whole record, hides the window it has no pixels for, and still shows the
         * rest of the watch face. The log line is what tells the owner to update. */
        if (c->kind >= CLK_KIND__MAX || c->src >= CLK_SRC__MAX) {
            ESP_LOGW(TAG, "comp %" PRIu32 ": kind %u source %u is newer than this dial — not drawn",
                     i, c->kind, c->src);
            c->show = 0;
            continue;
        }
        if (c->kind == CLK_KIND_WINDOW) {
            /* frame_bytes is what the paint path strides between tiles, so a
             * record whose own w/h disagree with it is a tile that would read
             * off the end of its row — refuse, and say which comp. */
            const uint64_t need = (uint64_t)c->count * c->frame_bytes;
            if (c->frame_bytes != CLK_WINDOW_BYTES(c))
                return "comp window tile stride disagrees with its own box";
            if ((uint64_t)c->off + need > art_size)
                return "comp window tiles lie outside the declared payload";
        } else {
            /* The record's own shape first: what a strip and a ring may ask for at
             * all, before anything is looked up in the bank they share. For a strip
             * `count` is the size of its glyph SET (ten rows for decimal digits), and
             * how many of them fit in the box is arithmetic on the pitch — the same
             * division clock_view.c does at paint time, done here so a box the dial
             * cannot lay out is a refused bake and not a row of glyphs over the
             * bezel. */
            const uint32_t span = c->count;
            if (c->kind == CLK_KIND_STRIP) {
                if (c->glyph_step <= 0)
                    return "strip has no pitch to lay its glyphs out on";
                if (!c->w || (int)c->w < c->glyph_step)
                    return "strip's box cannot hold even one glyph";
                const uint32_t pos = ((uint32_t)c->w + (uint32_t)c->glyph_step - 1u)
                                   / (uint32_t)c->glyph_step;
                if (pos > CLK_STRIP_GLYPHS_MAX)
                    return "strip's box needs more glyph slots than the dial creates";
                if (c->edge_left  != 0xFFFFu && c->edge_left  >= n_bank)
                    return "strip left edge piece is not in the bank";
                if (c->edge_right != 0xFFFFu && c->edge_right >= n_bank)
                    return "strip right edge piece is not in the bank";
            }
            if (c->kind == CLK_KIND_RING && c->angle_step == 0)
                return "ring has no angle between its slots";
            /* Then the shared bank: strip/ring/flare address it, so the index must
             * exist and every row they can reach must have pixels inside the
             * payload. */
            if ((uint64_t)c->first + span > n_bank)
                return "comp runs past the shared sprite bank";
            for (uint32_t k = c->first; k < c->first + span; k++) {
                const clk_sprite_t *b = &out->bank[k];
                if (b->w && b->h &&
                    (uint64_t)b->color_off + SPRITE_BYTES(b) > art_size)
                    return "comp sprite lies outside the declared payload";
            }
        }
    }
    for (uint32_t i = 0; i < n_aux; i++) {
        clk_aux_t *a = &out->aux[i];
        rd_aux(&r, a, "aux");
        if (r.why) return r.why;
        /* Same rule as an unknown comp kind: a hand this dial cannot drive is
         * hidden and logged, not a reason to refuse the theme it arrived on. The
         * checks below are damage, not the future, and stay refusals. */
        if (a->src >= CLK_SRC__MAX || a->kind > 2) {
            ESP_LOGW(TAG, "aux %" PRIu32 ": src %u kind %u is newer than this dial — not drawn",
                     i, a->src, a->kind);
            a->spr.show = 0;
            continue;
        }
        if (a->spr.show && (!a->spr.w || !a->spr.h))
            return "aux hand is shown but has no size";
        if (a->spr.show &&
            (uint64_t)a->spr.color_off + SPRITE_BYTES(&a->spr) > art_size)
            return "aux hand pixels lie outside the declared payload";
        if (a->value_max < a->value_min) return "aux hand range is inverted";
    }
    for (uint32_t i = 0; i < n_bank; i++) {
        rd_sprite(&r, &out->bank[i], "bank");
        if (r.why) return r.why;
        const clk_sprite_t *b = &out->bank[i];
        if (b->w && b->h &&
            (uint64_t)b->color_off + SPRITE_BYTES(b) > art_size)
            return "shared sprite lies outside the declared payload";
    }
    /* Trailing words mean the baker and this reader disagree on how much to
     * consume. Refusing is the only safe answer: reading on drops the tail of the
     * tables, and accepting them quietly is how a theme installs with a
     * half-populated rect table and shows a frame with a piece missing. */
    if (r.i != r.n) return "geometry blob has words this build does not recognise";
    out->n_comps = (uint16_t)n_comp;
    out->n_aux   = (uint16_t)n_aux;
    out->n_bank  = (uint16_t)n_bank;
    return NULL;
}


/* ------------------------------------------------------------------------- *
 * The resolved geometry the dial draws with.
 * ------------------------------------------------------------------------- */

static clk_theme_t   s_theme;                    /* parsed container geometry */
static clk_bg_rect_t *s_rects;
static uint16_t     *s_first, *s_delays;
static uint32_t     *s_bgframes;
static clk_comp_t   *s_comps;
static clk_aux_t    *s_aux;
static clk_sprite_t *s_bank;
static orb_theme_geo_t s_geo;
/* The installed slot's identity, captured at boot where the slot is adopted. The
 * list marks a row active by matching its header name against s_active_name AND
 * its geom_crc/art_crc against these two — not by name alone. Overwrite leaves the
 * name unchanged on the card while flash still holds the OLD bytes, so a
 * name-only match would call a changed file "active": the exact silent no-op
 * ruling D11 exists to prevent. A row whose name matches but whose CRCs differ
 * reports active=false and changed=true. */
static char          s_active_name[ORB_THEME_NAME_MAX + 1];
static uint32_t      s_active_geom_crc, s_active_art_crc;
static bool          s_active_valid;
static char          s_live[16] = THEME_LIVE_DEFAULT;
/* The slot boot was asked to draw and could not. Id "" means nothing is
 * damaged; the picker puts the sentence on that theme's row rather than in a
 * status line nobody can reach from the dial. */
static char          s_damage_id[ORB_THEME_ID_HEX + 1];
static char          s_damage_why[56];
static const char   *s_source = "baked-in";

/* §20.4's `staged` word: the 12-hex id of the theme whose flip wrote its pixels
 * and never wrote its header, read back out of NVS at boot and kept until a boot
 * finds the commit landed. "" is the clean state. It is a witness and never a
 * request — see THEME_NVS_STAGED for why that distinction survives D13. */
static char          s_staged_id[ORB_THEME_ID_HEX + 1];
/* §24.5(2) measured on hardware: an interrupted flip on this single-slot firmware
 * falls back to the FACTORY clock, not the previous theme — the copy erases the
 * slot it is replacing. The old wording promised the opposite of the boot the
 * bench watched; P6.4 owns this sentence and states the measured fact. */
#define STAGED_WHY "pixels staged, never committed — the dial shows the factory clock"

/* D30's boot refill, as the ledger reports it: the milliseconds of the fused
 * read-and-CRC pass and the bytes it moved. Both stay 0 on a boot that drew the
 * default, so `refill_ms` beside `source:"slot-B"` is the pair that tells the
 * page a refill happened, and a 0 with a slot source is a build that mapped
 * (Arm A, or MECH_B 0) rather than a measurement that failed. */
static uint32_t      s_refill_ms;
static uint32_t      s_refill_bytes;

/* The last streaming bundle store, timed the way the refill is timed: the
 * milliseconds of the fused card-to-slot pass and the bytes of art it moved.
 * Both stay 0 until a store lands, and P6.4's page reads them off /api/v1/sd
 * so the §20.5(1) rate is reachable without a serial cable — the lesson
 * §24.6(5) learned about what gates a bench. */
static uint32_t      s_bundle_ms;
static uint32_t      s_bundle_bytes;

/* The fallback pair, straight out of the generated header. Naming those objects
 * works in THIS file only, and only because orb_theme_geo.h is not included here:
 * its macros shadow exactly these names, and they are what clock_view.c reads. */
static void use_baked_in(void)
{
    /* Geometry from clock_assets.h, pixels from partition offset 0 — the
     * arrangement `idf.py flash` puts there, and the reason a board with no
     * installed theme still boots the dial it always showed. */
    s_geo.theme      = (clk_theme_t *)&clk_theme;
    s_geo.rects      = (clk_bg_rect_t *)clk_bg_rects;
    s_geo.rect_first = (uint16_t *)clk_bg_rect_first;
    s_geo.gif_delays = (uint16_t *)clk_gif_delays;
    s_geo.bg_frames  = (uint32_t *)clk_bg_frames;
    /* The baked-in dial is the four-count blob: it has no complication tables, so
     * clearing these is what stops a theme switch that lands here from leaving the
     * paint path drawing windows out of an arena that has been freed. */
    s_geo.comps      = NULL;
    s_geo.aux        = NULL;
    s_geo.bank       = NULL;
    s_geo.n_comps    = 0;
    s_geo.n_aux      = 0;
    s_geo.n_bank     = 0;
    s_geo.art_size   = (size_t)CLK_BLOB_BYTES;
    s_geo.from_container = false;
    s_geo.name[0] = '\0';
    s_active_name[0] = '\0';
    s_active_valid = false;
    s_source = "baked-in";
}

const orb_theme_geo_t *orb_theme_geo(void) { return &s_geo; }
const char *orb_theme_source(void)         { return s_source; }
const char *orb_theme_active_name(void)    { return s_active_name; }
const char *orb_theme_live(void)           { return s_live; }
const char *orb_theme_damage_id(void)      { return s_damage_id; }
const char *orb_theme_damage_why(void)     { return s_damage_why; }

/* §20.4's word and the two refill figures. All four are read-only answers for the
 * status surfaces: nothing here changes which dial the glass draws. */
const char *orb_theme_staged_id(void)      { return s_staged_id; }
const char *orb_theme_staged_why(void)     { return s_staged_id[0] ? STAGED_WHY : ""; }
uint32_t    orb_theme_refill_ms(void)      { return s_refill_ms; }
uint32_t    orb_theme_refill_bytes(void)   { return s_refill_bytes; }
uint32_t    orb_theme_bundle_ms(void)      { return s_bundle_ms; }
uint32_t    orb_theme_bundle_bytes(void)   { return s_bundle_bytes; }

/* ---- the two decisions D13 pinned, kept free of every syscall ---------------
 *
 * Which slot an install may write, and which slot a boot draws, are the two
 * numbers a wrong answer about costs a device its dial. Both are lifted verbatim
 * into tools/host_tz_test.py and truth-tabled there, because on the bench there is
 * no second slot to corrupt and no way to watch a boot draw the wrong bank. */

typedef enum { BOOT_ART_DEFAULT = 0, BOOT_ART_CARD = 1 } boot_art_t;

/* Which slot the dial draws. Anything that is not the card choice — "default",
 * junk, a key NVS cannot read — is the default, which is the answer that is
 * correct when the owner never chose and the answer that keeps the dial working
 * when the choice itself is damaged. B being valid is boot's evidence, and boot
 * verifies the pixels separately: this predicate decides where to look, not
 * whether it loaded. */
static boot_art_t boot_resolve(const char *live, const bool b_valid)
{
    if (!live || strcmp(live, THEME_LIVE_CARD) != 0) return BOOT_ART_DEFAULT;
    return b_valid ? BOOT_ART_CARD : BOOT_ART_DEFAULT;
}

/* Slot B, always. Slot A is what `idf.py flash` wrote and what the factory dial
 * reads its pixels from, and the bug this replaces (install into whichever slot
 * is not live) aimed the second install at A and erased the default with it. The
 * serial that goes in the new header still comes from whatever B last held. */
static uint32_t install_target(void) { return SLOT_B_HDR; }


/* ------------------------------------------------------------------------- *
 * Reading a container off the card.
 *
 * One 4 KB chunk buffer, shared by every card operation, from internal RAM. Not on
 * the stack: esp_http_server's task is sized at 4096 bytes by
 * HTTPD_DEFAULT_CONFIG() (esp_http_server.h:57 — whatever the note beside the OTA
 * chunk buffer once claimed about 8 KB), and 4 KB on top of FATFS's own frame use
 * is a stack overflow on the first upload rather than a slow day.
 * ------------------------------------------------------------------------- */

static uint8_t *s_chunk;
#define CHUNK_BYTES 4096            /* the ask */
#define CHUNK_MIN   1024            /* below this the copy is mostly command overhead */
static uint32_t  s_chunk_bytes;     /* what the heap granted; every clamp site reads THIS */

/* INTERNAL, not PSRAM: this buffer is the source or sink of the SPI DMA the sdmmc
 * driver runs underneath these reads. The size is a ladder, not a constant, because
 * the board's real budget does not contain 4 KB contiguous: the measured steady
 * state after association is `internal free=8,139 largest=3,328` (orb_net's own heap
 * line), and a flat 4 KB ask answered 507 to every upload and refused the card copy
 * too — docs/app-platform-plan.md §16.1 names the hour this was the whole feature.
 * Granting 2 KB costs four extra round trips per 8 KB of pixels; on the 1.17 MB av-kit
 * bank that is the same 0.5 s of SPI wire time, since the clock rate, not the chunk,
 * is the floor. Below 1 KB the command overhead leads the data and a slow install
 * stops being the right trade against a dead config server, so the ladder stops. */
static esp_err_t ensure_chunk(void)
{
    if (s_chunk) return ESP_OK;
    for (uint32_t n = CHUNK_BYTES; n >= CHUNK_MIN; n /= 2) {
        s_chunk = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_chunk) { s_chunk_bytes = n; return ESP_OK; }
    }
    return ESP_ERR_NO_MEM;
}

/* "/sd/themes/<stem>.orbtheme", or false. The extension is appended HERE, not by
 * the caller: letting the client spell the suffix is how ".orbtheme/../../x" or a
 * doubled extension reaches the path. Every caller passes either a device-issued id
 * (theme_id(name), 12 lowercase hex) — which card_open_verified_id proved against the
 * file's own header before it got here — or nothing at all, so this builder never sees
 * a human-supplied string. The directory and the name rule come from the SD layer, so
 * an endpoint added later cannot reach the card without going through the one
 * validator the host test exercises. */
static bool theme_file(const char *stem, char *out, size_t outsz)
{
    char base[ORB_SD_PATH_MAX];
    if (!orb_sd_theme_path(base, sizeof base, stem)) { out[0] = '\0'; return false; }
    const size_t blen = strlen(base);
    /* sizeof, not strlen: the tail carries the NUL, so the comparison is exact and a
     * stem that only just fits comes back refused instead of as a truncated name --
     * and a truncated name is a different file on the card. */
    if (blen + sizeof(ORB_THEME_EXT) > outsz) { out[0] = '\0'; return false; }
    memcpy(out, base, blen);
    memcpy(out + blen, ORB_THEME_EXT, sizeof(ORB_THEME_EXT));
    return true;
}

/* Read n bytes at *io_pos. pread, not read: verify_container() and the installer
 * walk one fd at two cursors (header, blob, payload), and a shared file offset
 * across those walks is a bug that shows up only on a card whose sector size makes
 * the two cursors meet. */
static ssize_t rd_at(int fd, size_t *io_pos, uint8_t *dst, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t got = pread(fd, dst + done, n - done, (off_t)(*io_pos + done));
        if (got < 0) return -1;
        if (got == 0) break;                      /* short file */
        done += (size_t)got;
    }
    *io_pos += done;
    return (ssize_t)done;
}

/* NULL if the container is sound and the dial could draw it, otherwise the reason it
 * is not. Reached before anything is installed and before the partition is touched —
 * by the list, by the uploader, and by the installer alike — so a kit copied onto the
 * card with a flaky reader, or baked with a table the painter refuses, is caught on a
 * page that still says what is on the card, not as a boot that quietly fell back to
 * the default with nothing to blame.
 *
 * Each checksum covers exactly its own bytes, the way tools/orbtheme.py computes
 * them (zlib.crc32 over the blob, then over the payload). Hashing the alignment gap
 * between the two because it is simpler would make the device disagree with every
 * file the baker has ever written. */
/* Defined below with the other geometry readers; declared here because
 * verify_container() is the one place every consumer of a card theme reaches the
 * numbers through, and the readers need the blob's own counts, which only the
 * header path above has established by this point. */
static const char *card_check_geom(int fd, const orb_thdr_t *o);

static const char *verify_container(int fd, const orb_thdr_t *h)
{
    /* The function that touches s_chunk reserves it, the way slot_verify_pixels()
     * already does — idempotent, one branch once the buffer exists. Reserve it HERE
     * and not in each caller: orb_theme_boot() and orb_theme_install() reach this
     * through card_open(), which reserves, but orb_theme_upload_end() opens its .part
     * with a bare open() and never passes through that door. Measured on the bench:
     * the whole 3.87 MB arrived, this function pread() into a NULL s_chunk on a boot
     * that had never staged a theme, FatFs stored through the NULL, and the dial died
     * with a StoreProhibited at the end of a successful upload. */
    if (ensure_chunk() != ESP_OK) return "out of memory for the card read buffer";

    uint32_t crc = 0;
    size_t pos = h->geom_off, left = h->geom_size;
    while (left) {
        const size_t want = left > s_chunk_bytes ? s_chunk_bytes : left;
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want)
            return "file is shorter than its header claims — re-copy it";
        crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
        left -= want;
    }
    if (crc != h->geom_crc)
        return "geometry does not match its checksum — re-copy the file onto the card";

    crc = 0;
    pos = h->art_off; left = h->art_size;
    while (left) {
        const size_t want = left > s_chunk_bytes ? s_chunk_bytes : left;
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want)
            return "pixel data is shorter than its header claims — re-copy it";
        crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
        left -= want;
    }
    if (crc != h->art_crc)
        return "pixels do not match their checksum — re-copy the file onto the card";

    /* And the numbers inside the geometry, not only their checksums. Three intact
     * CRCs and a theme the painter still refuses to draw is a real state — the card's
     * own "neon" is one — and the owner has to hear it from the row that listed the
     * file, not after a press installed it and rebooted into the factory dial. The
     * strings are orb_theme_geom_parse()'s, the same ones the boot path answers with.
     */
    return card_check_geom(fd, h);
}


/* Header, layout and geometry counts from an ALREADY-OPEN file. Returns the
 * reason the file is not a theme, or NULL.
 *
 * The caller owns the fd: refusing a file and closing one are two different
 * acts. orb_theme_scan()'s entries arrive already open from orb_files_walk(),
 * and card_open_locked() opens its own and must close it on every refusal.
 * Duplicating these checks instead of sharing them is how a card theme ends up
 * listed on one path and refused on the other. */
static const char *card_check(int fd, orb_thdr_t *o)
{
    if (ensure_chunk() != ESP_OK) return "out of memory";

    uint32_t hdr[W_WORDS];
    size_t pos = 0;
    if (rd_at(fd, &pos, (uint8_t *)hdr, sizeof hdr) != (ssize_t)sizeof hdr)
        return "file is too short to be a theme";

    const char *r = hdr_parse(hdr, o);
    if (r) return r;

    /* The file's own layout rule, which hdr_parse leaves open deliberately: an
     * installed slot puts its blob somewhere else and is checked separately. */
    if (o->geom_off != W_WORDS * 4u || o->art_off < o->geom_off + o->geom_size ||
        o->art_off % THEME_SECTOR)
        return "container parts are not where this format puts them";

    /* The four counts decide the blob's whole length, so a header whose numbers do
     * not add up is refused here — before 4 MB is streamed off the card. */
    uint32_t c[GEOM_COUNTS_V2];
    pos = o->geom_off;
    if (rd_at(fd, &pos, (uint8_t *)c, sizeof c) != (ssize_t)sizeof c)
        return "geometry blob is missing";

    uint32_t cnt[7];
    if (geom_counts(c, o->geom_size / 4u, cnt) < 0)
        return "geometry blob size does not match its own tables — re-bake it";

    o->n_rects = cnt[0]; o->n_first = cnt[1]; o->n_delays = cnt[2]; o->n_bgf = cnt[3];
    o->n_comp = cnt[4];  o->n_aux = cnt[5];    o->n_bank = cnt[6];
    return NULL;
}

/* Opens and validates everything short of the pixels, for callers that do NOT
 * hold the card lock — card_open() below is that wrapper. On success it returns
 * an open fd and the CALLER OWNS BOTH the fd and the card lock: release them
 * together with card_close(), never separately. FATFS keeps one window per
 * volume and this board has one CS line, so two tasks holding files open at
 * once is how a directory entry gets written by one and cached as something
 * else by the other.
 *
 * The mutex must never be taken in here. That is what deadlocked the device for
 * real: orb_theme_scan() held the lock across its whole directory walk and
 * validated each entry through card_open(), which asked for the same
 * non-recursive mutex again. With an empty themes directory the loop body never
 * runs, so the bench saw nothing; the first real theme on a real card left the
 * one httpd worker blocked in xSemaphoreTake forever, and every route stopped
 * answering while ping kept replying. The walk is orb_files_walk()'s now, which
 * takes the lock once for the whole operation and hands the callback an open fd,
 * so no caller of a validation path has to reason about the lock at all. */
static int card_open_locked(const char *id, orb_thdr_t *o, const char **why)
{
    char path[ORB_SD_PATH_MAX];
    *why = NULL;
    if (!theme_file(id, path, sizeof path)) { *why = "not a usable theme name"; return -1; }
    /* orb_sd_mounted(), not orb_sd_ensure_mounted(): the caller checked the card is
     * there before it took the lock, and ensure_mounted takes that same non-recursive
     * mutex itself. Measured here: this call from inside the locked core left the httpd
     * worker waiting on a lock it already owned — "waiting for the card: held by
     * \"httpd\"" — which is every web route dead and the dial still ticking. This
     * version only asks whether the card is still in, and refuses if it is not. */
    if (!orb_sd_mounted())                        { *why = "no card in the socket"; return -1; }

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        *why = errno == ENOENT ? "no such theme on the card" : "cannot open the theme file";
        return -1;
    }

    *why = card_check(fd, o);
    if (*why) { close(fd); return -1; }
    return fd;
}

/* For callers that do not hold the card lock: the mutex is taken here and released by
 * card_close(), which is why the pair is documented as owning both. A failure inside
 * the core never leaves the mutex held.
 *
 * The mount check belongs HERE, before the take, and not in the core. What reaches
 * this path is a worker job — an install, a delete, an upload end — and the SD init
 * ladder can burn a second per failed rung with no card-detect line, so a card job
 * that asked for the mount *after* taking the mutex would either deadlock or find no
 * card at all. Measured on hardware when the boot ladder still installed themes: with
 * the check moved into the core (which cannot mount without taking the mutex its
 * caller already holds), the install at t=712 ms found no card — the mount happened
 * at t=2136 ms — and the dial burned a retry attempt and drew the baked-in bank for a
 * file that was on the card the whole time. */
static int card_open(const char *id, orb_thdr_t *o, const char **why)
{
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = "no card in the socket"; return -1; }
    orb_sd_lock();
    const int fd = card_open_locked(id, o, why);
    if (fd < 0) orb_sd_unlock();
    return fd;
}

static void card_close(int fd)
{
    if (fd >= 0) close(fd);
    orb_sd_unlock();
}

/* Resolve a device-issued id to the one card file that legitimately carries it,
 * by WALKING the themes directory rather than building a path from the string. The
 * card filename is theme_id(header.name) — opaque hex — so an install or a delete
 * arrives with a 12-hex id the browser read off the list, and this is what turns
 * that id back into a file while refusing anything that is not exactly that file.
 *
 * Two passes, both required. First the walk (orb_files_walk, which takes the card
 * lock itself and hands the callback an open fd): the entry whose stem equals the id
 * is parsed and its header name must hash back to the id — this is what proves a file
 * by that name exists AND carries the name it is filed under, so a file swapped into
 * an id's place cannot install. Then, under a fresh lock, the file is opened by its
 * validated-id path (orb_sd_name_ok admits 12 hex, so no traversal is possible) and
 * the header RE-parsed and the hash re-checked once more: the card can change on a
 * laptop between the two passes, and the post-open re-verify is the one that
 * actually guards the bytes the caller goes on to read. Returns the open fd with the
 * lock held, plus the parsed header, or -1 and the reason; caller owns both. */
typedef struct {
    const char *want;         /* the id being resolved */
    bool        found;        /* an entry with that stem appeared */
    bool        verified;     /* its header name hashed back to that id */
} resolve_t;

static bool resolve_entry(void *arg, const orb_file_rec_t *rec, int fd)
{
    resolve_t *r = arg;
    if (strcmp(rec->name, r->want) != 0) return false;   /* not this one; keep looking */
    r->found = true;
    orb_thdr_t h;
    if (card_check(fd, &h)) return false;                 /* damaged: found, not verified */
    char id[THEME_ID_HEX + 1];
    theme_id(h.name, id);
    r->verified = (strcmp(id, r->want) == 0);             /* the walk's identity check */
    return r->verified;
}

static int card_open_verified_id(const char *id, orb_thdr_t *o, const char **why)
{
    *why = NULL;
    if (!theme_id_ok(id)) { *why = "that theme id is not one this device issued"; return -1; }
    const orb_file_src_t *src = orb_files_src("clock", "themes");
    if (!src) { *why = "no \"themes\" file source declared"; return -1; }
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = "no card in the socket"; return -1; }

    /* Pass one: the walk proves the file by that name exists and hashes to it. */
    resolve_t r = { .want = id, .found = false, .verified = false };
    if (orb_files_walk(src, resolve_entry, &r) < 0) { *why = "no card in the socket"; return -1; }
    if (!r.found)    { *why = "no such theme on the card";                          return -1; }
    if (!r.verified) { *why = "that file's name does not match its id — refused";   return -1; }

    /* Pass two: open by the validated-id path and re-verify the header under the
     * lock the copy then runs under. card_open_locked already parses the header
     * (card_check); the extra hash check is the same re-verify as pass one, guarding
     * the card that may have been edited on a laptop in between. */
    const int fd = card_open(id, o, why);
    if (fd < 0) return -1;
    char id2[THEME_ID_HEX + 1];
    theme_id(o->name, id2);
    if (strcmp(id2, id) != 0) { card_close(fd); *why = "that file's name does not match its id — refused"; return -1; }
    return fd;
}

/* ------------------------------------------------------------------------- *
 * The A/B slots.
 * ------------------------------------------------------------------------- */

typedef struct {
    bool       valid;
    uint32_t   hdr_base;                 /* partition offset of its header block */
    uint32_t   art_base;                 /* partition offset of its payload      */
    orb_thdr_t h;
} slot_t;

static void slot_init(slot_t *s, uint32_t hdr_base, uint32_t art_base)
{
    s->valid = false; s->hdr_base = hdr_base; s->art_base = art_base;
    memset(&s->h, 0, sizeof(s->h));
}

static void slot_read(slot_t *s, const esp_partition_t *p)
{
    uint32_t hdr[W_WORDS];
    /* esp_partition_read, not a pointer: the header block is deliberately NOT
     * mmap'd (art_bank.h on the DROM window), and 128 bytes do not justify an MMU
     * entry every boot. */
    if (esp_partition_read(p, s->hdr_base, hdr, sizeof hdr) != ESP_OK) return;
    /* The FORMAT 2 reader reads FORMAT 2 only (P6.4): a bundle's header parses
     * through the sentinel, never into a slot the boot would try to refill. The
     * explicit tag test is belt to hdr_parse's sentinel — boot's bundle row
     * ("an app bundle this dial cannot draw yet") depends on b.valid staying
     * false here, and this is the one read that decides it. */
    if (hdr_parse(hdr, &s->h) || s->h.fmt != THEME_FORMAT) return;
    /* The slot's own layout rule, against the SLOT offsets — not the file's. */
    if (s->h.geom_off != SLOT_GEOM_OFF || s->h.art_off != SLOT_ART_OFF) return;
    /* The blob's own counts, and only the first four or the first seven words are
     * counts depending on which layout its length proves it is: in a four-count
     * blob word 4 is the bg colour, not a table length. geom_counts() returns the
     * triple it actually believes, zeroing the rest. */
    uint32_t c[GEOM_COUNTS_V2];
    if (esp_partition_read(p, s->hdr_base + SLOT_GEOM_OFF, c, sizeof c) != ESP_OK) return;
    uint32_t cnt[7];
    if (geom_counts(c, s->h.geom_size / 4u, cnt) < 0) return;
    s->h.n_rects = cnt[0]; s->h.n_first = cnt[1];
    s->h.n_delays = cnt[2]; s->h.n_bgf = cnt[3];
    s->h.n_comp = cnt[4]; s->h.n_aux = cnt[5]; s->h.n_bank = cnt[6];
    s->valid = true;
}


/* Payload checksum, re-read from the partition in chunks rather than through the
 * map: this runs before clock_view_init() has mapped anything, and a checksum is
 * exactly the place not to depend on a mapping that may not exist. Re-reading
 * 3.68 MB costs a fraction of a second against the seconds an erase-and-write
 * takes, and it is the only thing between a half-written slot and a dial rendering
 * whatever bytes happened to land. */
static esp_err_t slot_verify_pixels(const slot_t *s, const esp_partition_t *p)
{
    if (ensure_chunk() != ESP_OK) return ESP_ERR_NO_MEM;
    uint32_t crc = 0;
    for (uint32_t done = 0; done < s->h.art_size; ) {
        const uint32_t want = (s->h.art_size - done > s_chunk_bytes) ? s_chunk_bytes
                                                                     : (s->h.art_size - done);
        const esp_err_t e = esp_partition_read(p, s->art_base + done, s_chunk, want);
        if (e != ESP_OK) return e;
        crc = esp_rom_crc32_le(crc, s_chunk, want);
        done += want;
    }
    return crc == s->h.art_crc ? ESP_OK : ESP_ERR_INVALID_CRC;
}

/* Parse a geometry blob. adopt=true publishes the tables into this module's
 * geometry, which is what the dial then draws; adopt=false parses the same numbers
 * and throws the tables away, which is what a check of a file or of a copy wants.
 * The refusal strings are the same either way, and that is the point of the
 * parameter: a path that checked less than the boot does commits a theme the owner
 * pressed and the dial refuses to draw, which is the gap the bench found with the
 * card's own "neon" — every checksum intact, one complication record shown with
 * no size. RAM is PSRAM first: the blob is a few KB, and internal RAM is the
 * resource this board is out of. Returns the reason, or NULL. */
static const char *geom_validate_blob(const uint32_t *blob, const orb_thdr_t *h, bool adopt)
{
    /* One arena for every table, sized from the counts the slot declared —
     * geom_counts() already proved those counts and this length agree, and proved
     * the complication counts sit inside the dial's caps. */
    const size_t bytes = sizeof(clk_bg_rect_t) * (size_t)h->n_rects
                       + sizeof(uint16_t) * (size_t)(h->n_first + h->n_delays)
                       + sizeof(uint32_t) * (size_t)h->n_bgf
                       + sizeof(clk_comp_t)   * (size_t)h->n_comp
                       + sizeof(clk_aux_t)    * (size_t)h->n_aux
                       + sizeof(clk_sprite_t) * (size_t)h->n_bank
                       + 16;
    uint8_t *arena = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena) arena = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!arena) return "out of memory for the theme's tables";
    /* Zeroed before the parse fills it: clock_view.c bounds some loops by clk_theme
     * numbers rather than by these lengths (it reads clk_bg_rect_first[tgt+1]), so a
     * byte the validator never saw must never be an uninitialised one. */
    memset(arena, 0, bytes);
    uint8_t *q = arena;
    clk_bg_rect_t *rects  = (clk_bg_rect_t *)q; q += sizeof(clk_bg_rect_t) * (size_t)h->n_rects;
    uint16_t *first       = (uint16_t *)q;      q += sizeof(uint16_t) * (size_t)h->n_first;
    uint16_t *delays      = (uint16_t *)q;      q += sizeof(uint16_t) * (size_t)h->n_delays;
    uint32_t *bgframes    = (uint32_t *)q;      q += sizeof(uint32_t) * (size_t)h->n_bgf;
    clk_comp_t *comps     = (clk_comp_t *)q;    q += sizeof(clk_comp_t) * (size_t)h->n_comp;
    clk_aux_t *aux        = (clk_aux_t *)q;     q += sizeof(clk_aux_t) * (size_t)h->n_aux;
    clk_sprite_t *bank    = (clk_sprite_t *)q;

    /* The pre-flight's own theme struct: the parse writes every field of it, and
     * the live one the dial is drawing must not be touched by a check of a file the
     * owner has only asked about. Static rather than stack because this is a
     * kilobyte-scale struct and the worker that installs through it has a fixed
     * stack sized for the flash copy, not for this; one install runs at a time,
     * which is what orb_jobs' single outstanding job is for. */
    static clk_theme_t preflight;
    orb_theme_geo_t tmp = { .theme = adopt ? &s_theme : &preflight,
                            .rects = rects, .rect_first = first,
                            .gif_delays = delays, .bg_frames = bgframes,
                            .comps = comps, .aux = aux, .bank = bank };
    const char *why = orb_theme_geom_parse(blob, h->geom_size / 4u, h->art_size, &tmp);
    if (why) {
        /* Nothing published either way: on the adopt path the tables are assigned
         * below only once the parse has accepted them, so no static can be left
         * pointing into the arena that is about to go away. */
        free(arena);
        ESP_LOGE(TAG, "%s: %s — refusing to draw it", h->name, why);
        return why;
    }
    if (!adopt) {
        free(arena);          /* pre-flight: it proved the numbers, that is all it was for */
        return NULL;
    }
    /* The arena is never freed: clock_view.c caches pointers into these tables in
     * its LVGL descriptors, the same lifetime rule art_bank.c applies to the map. */
    s_rects = rects; s_first = first; s_delays = delays; s_bgframes = bgframes;
    s_comps = comps; s_aux = aux; s_bank = bank;
    s_geo.theme      = &s_theme;
    s_geo.rects      = s_rects;
    s_geo.rect_first = s_first;
    s_geo.gif_delays = s_delays;
    s_geo.bg_frames  = s_bgframes;
    s_geo.comps      = s_comps;
    s_geo.aux        = s_aux;
    s_geo.bank       = s_bank;
    s_geo.n_comps    = (uint16_t)h->n_comp;
    s_geo.n_aux      = (uint16_t)h->n_aux;
    s_geo.n_bank     = (uint16_t)h->n_bank;
    s_geo.art_size   = h->art_size;
    s_geo.from_container = true;
    strlcpy(s_geo.name, h->name, sizeof(s_geo.name));
    /* Capture the installed slot's identity here, where boot adopts the slot: the
     * name AND the two CRCs. The list reads these to decide active vs changed, so
     * an overwrite of the card file (same name, different bytes) cannot report a
     * file the dial is not drawing as "active". */
    strlcpy(s_active_name, h->name, sizeof(s_active_name));
    s_active_geom_crc = h->geom_crc;
    s_active_art_crc  = h->art_crc;
    s_active_valid    = true;
    return NULL;
}

/* Read a slot's geometry blob off the partition and validate it. */
static const char *slot_parse_geom(const slot_t *s, const esp_partition_t *p, bool adopt)
{
    uint32_t *blob = heap_caps_malloc(s->h.geom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) blob = heap_caps_malloc(s->h.geom_size, MALLOC_CAP_8BIT);
    if (!blob) return "out of memory reading the theme's tables";
    for (uint32_t off = 0; off < s->h.geom_size; off += CHUNK_BYTES) {
        const uint32_t want = (s->h.geom_size - off > CHUNK_BYTES) ? CHUNK_BYTES
                                                                    : (s->h.geom_size - off);
        const esp_err_t e = esp_partition_read(p, s->hdr_base + SLOT_GEOM_OFF + off,
                                               (uint8_t *)blob + off, want);
        if (e != ESP_OK) { free(blob); return "the theme's tables would not read off the flash"; }
    }
    const char *why = geom_validate_blob(blob, &s->h, adopt);
    free(blob);
    return why;
}

/* The same numbers over a container's own blob, off the card. Reached only from
 * verify_container(), so it is the list, the uploader and the install alike that
 * refuse a theme the painter cannot draw — the row says it, at the file, before
 * anybody pays for a copy or a reboot. */
static const char *card_check_geom(int fd, const orb_thdr_t *o)
{
    uint32_t *blob = heap_caps_malloc(o->geom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) blob = heap_caps_malloc(o->geom_size, MALLOC_CAP_8BIT);
    if (!blob) return "out of memory reading the theme's tables";
    size_t pos = o->geom_off, left = o->geom_size;
    while (left) {
        const size_t want = left > CHUNK_BYTES ? CHUNK_BYTES : left;
        uint8_t *at = (uint8_t *)blob + (o->geom_size - left);
        if (rd_at(fd, &pos, at, want) != (ssize_t)want) {
            free(blob);
            return "file is shorter than its header claims — re-copy it";
        }
        left -= want;
    }
    const char *why = geom_validate_blob(blob, o, /*adopt=*/false);
    free(blob);
    return why;
}



static esp_err_t slot_load_geom(const slot_t *s, const esp_partition_t *p)
{
    return slot_parse_geom(s, p, /*adopt=*/true) ? ESP_FAIL : ESP_OK;
}


/* ------------------------------------------------------------------------- *
 * NVS: the one key this module owns, and the handle it is read and written with.
 * ------------------------------------------------------------------------- */

static nvs_handle_t s_h;              /* one open handle, for the boot's life */

static nvs_handle_t theme_nvs(void)
{
    if (!s_h) {
        /* The dedicated `storage` partition first, per partitions.csv's retention
         * rule. An orb still running an older table has no partition by that name,
         * and a theme feature that cannot start there is worse than settings sharing
         * esp_wifi's namespace the way they used to. */
        if (nvs_open_from_partition(THEME_NVS_PARTITION, THEME_NVS_NAMESPACE,
                                    NVS_READWRITE, &s_h) != ESP_OK) {
            if (nvs_open(THEME_NVS_NAMESPACE, NVS_READWRITE, &s_h) != ESP_OK) {
                s_h = 0;
                ESP_LOGE(TAG, "no NVS for theme settings — loads will not survive a reboot");
            }
        }
    }
    return s_h;
}

static esp_err_t nvs_put(const char *key, const char *val)
{
    const nvs_handle_t h = theme_nvs();
    if (!h) return ESP_ERR_INVALID_STATE;
    esp_err_t e = val ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    /* Erasing a key that was never written is the normal case for "back to the
     * factory default", not a failure to report. */
    if (e == ESP_ERR_NVS_NOT_FOUND && !val) e = ESP_OK;
    if (e != ESP_OK) return e;
    /* The commit is what makes the choice survive the reboot that follows. Skip it
     * and the dial says "using <theme>" and comes back with the theme it already
     * had — the worst failure this feature can have: quiet, and indistinguishable
     * from a theme that simply looked wrong. */
    return nvs_commit(h);
}

static bool nvs_get(const char *key, char *out, size_t outsz)
{
    const nvs_handle_t h = theme_nvs();
    if (!h) return false;
    out[0] = '\0';
    size_t sz = outsz;
    return nvs_get_str(h, key, out, &sz) == ESP_OK;
}

/* ---- the `staged` witness (§20.2 step 4, §20.4's ledger word) --------------
 *
 * Marked BEFORE the copy starts and cleared AFTER the slot header lands, which is
 * the same window the header-write-last rule already protects. A cut anywhere in
 * that window leaves the marker set and the header unwritten, so the next boot
 * draws the theme that was committed and the row says why — the failure story
 * §20.2 asks for, told by the ledger instead of by a log line nobody is watching.
 *
 * A failed write clears it, because a refused install has nothing left in flight:
 * the row then carries try_why, and a stale witness would name two things at once.
 * An NVS write that fails is not worth failing an install over — the witness is
 * diagnostics, and the safety property is the header ordering, not this key. So
 * these return void and log. */
static void staged_mark(const char *id)
{
    if (nvs_put(THEME_NVS_STAGED, id) != ESP_OK)
        ESP_LOGW(TAG, "could not record the staged word for %s", id);
    strlcpy(s_staged_id, id, sizeof s_staged_id);
}

static void staged_clear(void)
{
    if (nvs_put(THEME_NVS_STAGED, NULL) != ESP_OK)
        ESP_LOGW(TAG, "could not clear the staged word");
    s_staged_id[0] = '\0';
}

/* Build the header that makes a slot live. Built here rather than copied from the
 * file: same magic and the same two checksums (the bytes they cover are copied
 * verbatim), but the offsets describe the SLOT layout and the serial is a boot-time
 * counter. Rebuilding it is also what stops a container that lied about its own
 * offsets from surviving an install — those fields get overwritten with the values
 * this function actually wrote to. */
static void build_slot_hdr(uint32_t hdr[W_WORDS], const orb_thdr_t *h, uint32_t serial)
{
    memset(hdr, 0, W_WORDS * 4);
    hdr[W_MAGIC] = THEME_MAGIC;   hdr[W_FORMAT] = THEME_FORMAT;
    hdr[W_GEOM_ABI] = THEME_GEOM_ABI;
    hdr[W_HDR_SIZE] = W_WORDS * 4;
    hdr[W_GEOM_OFF] = SLOT_GEOM_OFF;  hdr[W_GEOM_SIZE] = h->geom_size;
    hdr[W_GEOM_CRC] = h->geom_crc;
    hdr[W_ART_SIZE] = h->art_size;    hdr[W_ART_CRC] = h->art_crc;
    hdr[W_SCR_W] = CLK_SCR_W;     hdr[W_SCR_H] = CLK_SCR_H;
    hdr[W_SERIAL] = serial;       hdr[W_ART_OFF] = SLOT_ART_OFF;
    /* The name byte array is bounded by the struct's own field size, so a field
     * widened in orb_thdr_t cannot overflow the header's word range here. */
    for (int i = 0; h->name[i] && i < (int)sizeof(h->name) - 1; i++)
        hdr[W_NAME  + i / 4] |= (uint8_t)h->name[i]  << (8 * (i % 4));
    /* Computed last, over the whole block with the checksum field still zero. */
    hdr[W_HDR_CRC] = esp_rom_crc32_le(0, (const uint8_t *)hdr, W_WORDS * 4);
}


/* Copy the container's two parts into one slot. `fd` is open on the file and the
 * caller holds the card lock.
 *
 * ORDER IS THE WHOLE POINT. Erase, write geometry, write pixels, verify — and let
 * the CALLER write the header last, because the header is what makes a slot live. An
 * install interrupted at any point in here (power cut, card pulled, watchdog) leaves
 * the target slot headerless and therefore invalid, and the device boots the theme it
 * already had. The other slot is never touched at all.
 *
 * Erase is bounded to what is written, aligned up to the sector, rather than the
 * whole 4 MB slot: esp_partition_erase_range() runs with the cache disabled, and
 * although CONFIG_SPI_FLASH_YIELD_DURING_ERASE=y (which this build has) makes the
 * driver yield between sectors, bounding the erase is still what keeps the stall
 * proportionate to the theme being loaded instead of to the partition size. */
static esp_err_t copy_into(int fd, const orb_thdr_t *h, const esp_partition_t *p,
                           uint32_t hdr_base, const char **why)
{
    /* Same rule as verify_container(): the function that reads through s_chunk
     * reserves it, so a future caller cannot reach this loop with a NULL buffer the
     * way the upload path reached verify_container(). */
    if (ensure_chunk() != ESP_OK) { *why = "out of memory for the card read buffer"; return ESP_ERR_NO_MEM; }

    const uint32_t geom_len = (h->geom_size + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);
    const uint32_t art_len  = (h->art_size  + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);
    if (SLOT_GEOM_OFF + geom_len > SLOT_ART_OFF) { *why = "geometry would overrun the pixels"; return ESP_ERR_INVALID_SIZE; }
    if (SLOT_ART_OFF + art_len > SLOT_CAP)       { *why = "theme is larger than a slot";      return ESP_ERR_INVALID_SIZE; }

    esp_err_t e = esp_partition_erase_range(p, hdr_base + SLOT_GEOM_OFF, geom_len);
    if (e == ESP_OK) e = esp_partition_erase_range(p, hdr_base + SLOT_ART_OFF, art_len);
    if (e != ESP_OK) { *why = "flash erase failed"; return e; }

    size_t pos = h->geom_off;
    for (uint32_t done = 0; done < h->geom_size; ) {
        const uint32_t want = (h->geom_size - done > s_chunk_bytes) ? s_chunk_bytes
                                                                    : (h->geom_size - done);
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want) {
            *why = "theme file ended before its declared geometry"; return ESP_ERR_INVALID_SIZE;
        }
        e = esp_partition_write(p, hdr_base + SLOT_GEOM_OFF + done, s_chunk, want);
        if (e != ESP_OK) { *why = "flash write failed on the geometry"; return e; }
        done += want;
    }
    pos = h->art_off;
    for (uint32_t done = 0; done < h->art_size; ) {
        const uint32_t want = (h->art_size - done > s_chunk_bytes) ? s_chunk_bytes
                                                                   : (h->art_size - done);
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want) {
            *why = "theme file ended before its declared pixels"; return ESP_ERR_INVALID_SIZE;
        }
        e = esp_partition_write(p, hdr_base + SLOT_ART_OFF + done, s_chunk, want);
        if (e != ESP_OK) { *why = "flash write failed on the pixels"; return e; }
        done += want;
    }
    return ESP_OK;
}



/* ---- FORMAT 3 ingest (P6.3, §22.3) ----------------------------------------
 *
 * A bundle is not a bigger FORMAT 2 file. It carries N per-app sections whose
 * records name banks by address, and it can be 4 MB while this board keeps a 2 MB
 * arena margin in reserve. So the ingest never holds the container: it reads the
 * 128-byte header, then the geometry region (at most ORB_BUNDLE_GEOM_CAP, 60 KB),
 * validates both, and streams each bank chunk by chunk — internal-RAM chunk in,
 * partition write out, crc32 accumulated as it crosses — settling each section
 * against orb_bundle_bank_rule() before it touches the next. §22.3's "banks CRC'd
 * as they arrive" is literally that loop.
 *
 * THE SLOT IS NOT THE FILE. FORMAT 2 already moved its blob from file offset 128
 * to SLOT_GEOM_OFF and rewrote the header's geom_off to say so; a bundle's banks
 * move the same way, to SLOT_ART_OFF, and because a record's art_off is an address
 * rather than a length the records are edited and the region re-checksummed
 * (orb_bundle_rebase). After that the slot is self-describing: every offset in it
 * reads slot-relative, which is what §22.1's third rule needs in order to answer
 * --inspect with installed bytes.
 *
 * Nothing here selects the bundle. The install leaves the choice key alone and the
 * dial keeps drawing what it drew; the flip page (P6.4, landed) is the door that
 * presses it, and only it. D26 is honoured on the way — boot says plainly that a
 * bundle in the slot is not drawable yet. */

static esp_err_t install_failed(esp_err_t e, const char *w, char *why, size_t whysz);
static const char *hdr_at(int fd, orb_thdr_t *h);   /* defined with the upload door below */

/* The v3 twin of card_check(). Same fd discipline, same sentences, and the file's
 * own layout rule: a bundle puts its region where a container puts its blob, and
 * also reports the container's length, which the region-only parse needs to bound
 * an art span it is not going to read. */
static const char *card_check3(int fd, orb_bundle3_hdr_t *o, uint32_t *file_size)
{
    if (ensure_chunk() != ESP_OK) return "out of memory";
    uint32_t hdr[W_WORDS];
    size_t pos = 0;
    if (rd_at(fd, &pos, (uint8_t *)hdr, sizeof hdr) != (ssize_t)sizeof hdr)
        return "file is too short to be a theme";
    const long len = lseek(fd, 0, SEEK_END);
    if (len < (long)sizeof hdr) return "file is too short to be a theme";
    if (lseek(fd, 0, SEEK_SET) < 0) return "the card could not be re-read";
    *file_size = (uint32_t)len;
    const char *r = orb_bundle3_hdr_parse(hdr, o);
    if (r) return r;
    if (o->geom_off != W_WORDS * 4u || o->art_off < o->geom_off + o->geom_size ||
        o->art_off % THEME_SECTOR)
        return "container parts are not where this format puts them";
    if (o->art_off > *file_size || o->art_size > *file_size - o->art_off)
        return "art region runs past the end of the file";
    return NULL;
}

/* The door's reader: an open container read by whichever format its own header
 * proves, with FORMAT 3's shared tuple words (name, both checksums, serial) and
 * its format tag filled into the FORMAT 2 struct so the duplicate check below
 * and every caller of it is one rule, not two. Returns NULL, the bundle sentence
 * for a caller holding no bundle door, or the reason the header is nothing this
 * device reads. card_check3() is re-read rather than duplicated: it is the same
 * door the install path uses, and a container checked here and re-checked there
 * is the identical pair of sentences, byte for byte. */
static const char *hdr_at_any(int fd, orb_thdr_t *h, orb_bundle3_hdr_t *b3, uint32_t *fsz)
{
    const char *r = hdr_at(fd, h);           /* FORMAT 2's full header check */
    if (r != THEME3_REFUSAL) return r;        /* FORMAT 2, or nothing */
    if (!b3) return r;                        /* no bundle door here */
    r = card_check3(fd, b3, fsz);
    if (r) return r;
    h->fmt = 3;
    h->n_sections = b3->n_sections;
    h->geom_crc = b3->geom_crc;
    h->art_crc  = b3->art_crc;
    h->serial   = b3->serial;
    strlcpy(h->name, b3->name, sizeof h->name);
    return NULL;
}

/* FORMAT 3's container check at the door, the twin of verify_container() and
 * nothing else: the region alone verified through the shipped reader (bounds,
 * region checksum, index, walk, blob lengths/crcs/slugs, art bounds,
 * sortedness), then the art region streamed and checksummed against the
 * header's own. Per-bank rules are NOT checked here — the install settles them
 * on the wire (orb_bundle_bank_rule), and the streamed whole-art checksum here
 * is the same figure read_bundle()'s art walk settles against at the far end,
 * so a bank tampered between this door and the install still dies at the
 * install, not on the wall. The region buffer is install_bundle()'s size and
 * cap: geom_size is already clamped to ORB_BUNDLE_GEOM_CAP by the header gate
 * before it reaches this malloc. */
static const char *verify_bundle3(int fd, const orb_bundle3_hdr_t *h, uint32_t file_size)
{
    uint8_t *region = heap_caps_malloc(h->geom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) return "out of memory for the bundle's tables";
    size_t pos = h->geom_off;
    if (rd_at(fd, &pos, region, h->geom_size) != (ssize_t)h->geom_size) {
        heap_caps_free(region);
        return "geometry region runs past the end of the file";
    }
    orb_bundle_t b;
    const char *r = orb_bundle_parse_region(h, region, h->geom_size, file_size, &b);
    heap_caps_free(region);
    if (r) return r;
    if (ensure_chunk() != ESP_OK) return "out of memory for the card read buffer";
    uint32_t crc = 0;
    pos = h->art_off;
    uint32_t left = h->art_size;
    while (left) {
        const size_t want = left > s_chunk_bytes ? s_chunk_bytes : left;
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want)
            return "pixel data is shorter than its header claims — re-copy it";
        crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
        left -= (uint32_t)want;
    }
    if (crc != h->art_crc)
        return "pixels do not match their checksum — re-copy the file onto the card";
    return NULL;
}

/* card_open_locked()'s v3 twin. Same mutex rule (never taken in here) and the same
 * orb_sd_mounted() rather than ensure_mounted() — calling the mounting version from
 * inside the locked core is §11's deadlock, dead worker, dial still ticking. */
static int card_open_locked3(const char *id, orb_bundle3_hdr_t *o, uint32_t *file_size,
                             const char **why)
{
    char path[ORB_SD_PATH_MAX];
    *why = NULL;
    if (!theme_file(id, path, sizeof path)) { *why = "not a usable theme name"; return -1; }
    if (!orb_sd_mounted())                  { *why = "no card in the socket"; return -1; }
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        *why = errno == ENOENT ? "no such theme on the card" : "cannot open the theme file";
        return -1;
    }
    *why = card_check3(fd, o, file_size);
    if (*why) { close(fd); return -1; }
    return fd;
}

/* The v3 twin of card_open_verified_id(). Twin rather than shared because the name
 * this one hashes lives in a different header shape; the refusal words are the same
 * strings, so one damaged file cannot get two different reasons depending on which
 * door it rang (R4's identity rule is format-independent by ruling). */
typedef struct {
    const char *want;
    bool        found;
    bool        verified;
} resolve3_t;

static bool resolve3_entry(void *arg, const orb_file_rec_t *rec, int fd)
{
    resolve3_t *r = arg;
    if (strcmp(rec->name, r->want) != 0) return false;
    r->found = true;
    orb_bundle3_hdr_t h;
    uint32_t fsz = 0;
    if (card_check3(fd, &h, &fsz)) return false;
    char id[THEME_ID_HEX + 1];
    theme_id(h.name, id);
    r->verified = (strcmp(id, r->want) == 0);
    return r->verified;
}

static int card_open_verified_id3(const char *id, orb_bundle3_hdr_t *o, uint32_t *file_size,
                                  const char **why)
{
    *why = NULL;
    if (!theme_id_ok(id)) { *why = "that theme id is not one this device issued"; return -1; }
    const orb_file_src_t *src = orb_files_src("clock", "themes");
    if (!src) { *why = "no \"themes\" file source declared"; return -1; }
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = "no card in the socket"; return -1; }

    resolve3_t r = { .want = id, .found = false, .verified = false };
    if (orb_files_walk(src, resolve3_entry, &r) < 0) { *why = "no card in the socket"; return -1; }
    if (!r.found)    { *why = "no such theme on the card";                        return -1; }
    if (!r.verified) { *why = "that file's name does not match its id — refused"; return -1; }

    const int fd = card_open_locked3(id, o, file_size, why);
    if (fd < 0) return -1;
    char id2[THEME_ID_HEX + 1];
    theme_id(o->name, id2);
    if (strcmp(id2, id) != 0) {
        card_close(fd);
        *why = "that file's name does not match its id — refused";
        return -1;
    }
    return fd;
}

/* Erase, write, and read back — the FORMAT 2 discipline in copy_into(), applied to
 * a region and N banks. The header is written LAST by the caller, so an interrupt
 * anywhere in here leaves the target slot headerless and the boot drawing what it
 * drew before. Banks are checksummed twice on purpose: once accumulated as the
 * bytes cross off the card, which is what lets a damaged section be refused before
 * the next one is touched, and once from the partition afterwards, which proves the
 * flash wrote what it was given and produces the installed art region's own
 * checksum for the header. */
static esp_err_t copy_bundle3(int fd, const orb_bundle_t *b, const uint8_t *region,
                              uint32_t file_art_base, const esp_partition_t *p,
                              uint32_t hdr_base, uint32_t *out_art_crc,
                              const char **why)
{
    if (ensure_chunk() != ESP_OK) { *why = "out of memory for the card read buffer"; return ESP_ERR_NO_MEM; }

    uint32_t art_end = SLOT_ART_OFF;
    for (uint32_t i = 0; i < b->n_sections; i++) {
        const uint32_t end = b->sec[i].art_off + b->sec[i].art_size;
        if (end > art_end) art_end = end;
    }
    const uint32_t geom_len = (b->region_size + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);
    const uint32_t art_len  = (art_end - SLOT_ART_OFF + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);
    if (SLOT_GEOM_OFF + geom_len > SLOT_ART_OFF) { *why = "geometry would overrun the pixels"; return ESP_ERR_INVALID_SIZE; }
    if (art_len > SLOT_CAP - SLOT_ART_OFF)       { *why = "bundle is larger than a slot — re-bake it"; return ESP_ERR_INVALID_SIZE; }

    esp_err_t e = esp_partition_erase_range(p, hdr_base + SLOT_GEOM_OFF, geom_len);
    if (e == ESP_OK) e = esp_partition_erase_range(p, hdr_base + SLOT_ART_OFF, art_len);
    if (e != ESP_OK) { *why = "flash erase failed"; return e; }

    for (uint32_t done = 0; done < b->region_size; ) {
        const uint32_t want = (b->region_size - done > s_chunk_bytes) ? s_chunk_bytes
                                                                      : (b->region_size - done);
        e = esp_partition_write(p, hdr_base + SLOT_GEOM_OFF + done, region + done, want);
        if (e != ESP_OK) { *why = "flash write failed on the geometry"; return e; }
        done += want;
    }

    for (uint32_t i = 0; i < b->n_sections; i++) {
        const orb_bundle_sec_t *s = &b->sec[i];
        size_t   pos = file_art_base + (s->art_off - SLOT_ART_OFF);
        uint32_t crc = 0, done = 0;
        while (done < s->art_size) {
            const uint32_t want = (s->art_size - done > s_chunk_bytes) ? s_chunk_bytes
                                                                       : (s->art_size - done);
            if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want) {
                *why = "bundle ended before its declared pixels"; return ESP_ERR_INVALID_SIZE;
            }
            crc = esp_rom_crc32_le(crc, s_chunk, want);
            e = esp_partition_write(p, hdr_base + s->art_off + done, s_chunk, want);
            if (e != ESP_OK) { *why = "flash write failed on the pixels"; return e; }
            done += want;
        }
        const char *rw = orb_bundle_bank_rule(s, done, crc);
        if (rw) { *why = rw; return ESP_ERR_INVALID_CRC; }
    }

    uint32_t acrc = 0;
    for (uint32_t done = 0; done < art_len; ) {
        const uint32_t want = (art_len - done > s_chunk_bytes) ? s_chunk_bytes : (art_len - done);
        if (esp_partition_read(p, hdr_base + SLOT_ART_OFF + done, s_chunk, want) != ESP_OK) {
            *why = "could not read the installed pixels back"; return ESP_FAIL;
        }
        acrc = esp_rom_crc32_le(acrc, s_chunk, want);
        done += want;
    }
    *out_art_crc = acrc;
    return ESP_OK;
}

/* The slot header for an installed bundle: the same 128 bytes and the same torn-
 * header self-check as a container, with the slot's offsets, this install's serial,
 * and the checksums of the bytes as they now sit in the partition. Word 9 carries
 * the section count where FORMAT 2 carried a panel width, and word 10 is zero: an
 * installed bundle does not claim a panel size it never had. */
static void build_slot_hdr3(uint32_t hdr[W_WORDS], const orb_bundle3_hdr_t *h,
                            uint32_t art_crc, uint32_t serial)
{
    memset(hdr, 0, W_WORDS * 4);
    hdr[0] = THEME_MAGIC;     hdr[1] = ORB_BUNDLE3_FORMAT;
    hdr[2] = ORB_BUNDLE3_ABI; hdr[3] = W_WORDS * 4;
    hdr[4] = SLOT_GEOM_OFF;   hdr[5] = h->geom_size;  hdr[6] = h->geom_crc;
    hdr[7] = h->art_size;     hdr[8] = art_crc;
    hdr[9] = h->n_sections;   hdr[11] = serial;       hdr[13] = SLOT_ART_OFF;
    for (int i = 0; h->name[i] && i < (int)sizeof(h->name) - 1; i++)
        hdr[W_NAME + i / 4] |= (uint8_t)h->name[i] << (8 * (i % 4));
    hdr[W_HDR_CRC] = esp_rom_crc32_le(0, (const uint8_t *)hdr, W_WORDS * 4);
}

/* The one implementation of "put this bundle on the wall". Worker task, like every
 * other theme operation, reached only through orb_theme_post_bundle() — the page
 * and the queue cannot disagree about whether the copy may hold the card lock. It
 * does NOT change what the dial draws: the bundle is stored, not chosen, until the
 * FORMAT 3 reader (P6.5) gives a compositor something to draw. D26 still applies —
 * the page reports the install as stored-and-waiting, because "it installed and
 * nothing happened" is a sentence an owner reads as a broken button. */
esp_err_t orb_theme_install_bundle(const char *id, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    const esp_partition_t *p = art_bank_partition();
    if (!p)
        return install_failed(ESP_ERR_NOT_FOUND, "this board has no theme storage", why, whysz);

    const char *w = NULL;
    uint32_t fsz = 0;
    orb_bundle3_hdr_t h;
    const int64_t t0 = esp_timer_get_time();
    const int fd = card_open_verified_id3(id, &h, &fsz, &w);
    if (fd < 0) return install_failed(ESP_FAIL, w, why, whysz);

    /* The region alone, in PSRAM. Internal RAM cannot hold 60 KB, and the header
     * parse already bounded the claim at ORB_BUNDLE_GEOM_CAP before this size was
     * trusted with a malloc. */
    uint8_t *region = heap_caps_malloc(h.geom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) {
        card_close(fd);
        return install_failed(ESP_ERR_NO_MEM, "not enough memory for the bundle's tables",
                              why, whysz);
    }
    size_t pos = h.geom_off;
    if (rd_at(fd, &pos, region, h.geom_size) != (ssize_t)h.geom_size) {
        heap_caps_free(region); card_close(fd);
        return install_failed(ESP_ERR_INVALID_SIZE,
                              "bundle ended before its declared geometry", why, whysz);
    }

    orb_bundle_t b;
    const char *pv = orb_bundle_parse_region(&h, region, h.geom_size, fsz, &b);
    if (pv) {
        heap_caps_free(region); card_close(fd);
        return install_failed(ESP_ERR_INVALID_CRC, pv, why, whysz);
    }

    /* Banks to SLOT_ART_OFF, records and region checksum following. The file's base
     * comes back for the stream's seeks: after this the records describe the slot. */
    uint32_t file_art_base = 0;
    const char *rv = orb_bundle_rebase(&b, &h, region, SLOT_ART_OFF, SLOT_CAP,
                                       &file_art_base);
    if (rv) {
        heap_caps_free(region); card_close(fd);
        return install_failed(ESP_ERR_INVALID_SIZE, rv, why, whysz);
    }
    h.geom_off = SLOT_GEOM_OFF;

    /* The serial counts up over whatever the slot held, in either format; a header
     * that cannot be read restarts the count, safe for FORMAT 2's stated reason —
     * one slot is in play and nothing compares two of them. */
    uint32_t raw[W_WORDS];
    uint32_t serial = 1;
    if (esp_partition_read(p, install_target(), raw, sizeof raw) == ESP_OK) {
        orb_thdr_t o2;
        orb_bundle3_hdr_t o3;
        if (hdr_parse(raw, &o2) == NULL)                    serial = o2.serial + 1;
        else if (orb_bundle3_hdr_parse(raw, &o3) == NULL)   serial = o3.serial + 1;
    }

    staged_mark(id);
    uint32_t art_crc = 0;
    esp_err_t e = copy_bundle3(fd, &b, region, file_art_base, p, install_target(),
                               &art_crc, &w);
    card_close(fd);
    heap_caps_free(region);
    if (e != ESP_OK) {
        staged_clear();
        return install_failed(e, w, why, whysz);
    }

    const esp_err_t ee = esp_partition_erase_range(p, install_target(), THEME_SECTOR);
    if (ee == ESP_OK) {
        uint32_t hdr[W_WORDS];
        build_slot_hdr3(hdr, &h, art_crc, serial);
        e = esp_partition_write(p, install_target(), hdr, sizeof(hdr));
        if (e != ESP_OK) w = "could not make the new slot live";
    } else {
        e = ee;
        w = "could not clear the old slot header";
    }
    if (e != ESP_OK) {
        staged_clear();
        return install_failed(e, w, why, whysz);
    }
    staged_clear();
    s_bundle_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_bundle_bytes = h.art_size;
    ESP_LOGI(TAG, "bundle \"%s\" (%s) in slot-B in %u ms — stored, and not yet chosen",
             h.name, id, (unsigned)s_bundle_ms);
    return ESP_OK;
}

/* Make the resolved theme the one the dial draws: geometry into s_geo, pixels


 * refilled into the PSRAM arena by one pass that checksums them as it reads them
 * (D30, §20.2's boot rule). On any failure the caller gets the baked-in pair,
 * which is the only state in which the clock is guaranteed to come up.
 *
 * The checksum is fused, not sequential. The pre-D30 shape here was
 * slot_verify_pixels() and then a map — one pass over the slot to prove it and a
 * second, on the paint path, to read it. art_bank_fill() proves each chunk in the
 * same loop that lands it, so the boot that verifies and the boot that draws are
 * one pass, and `*out_ms` is the number §20.5(2) owed: printed on the line below
 * it rather than predicted at 150 ms.
 *
 * An arena refusal (ESP_ERR_NO_MEM: PSRAM under art_size + ORB_ART_ARENA_MARGIN)
 * takes the same fall-through as a corrupt slot — the default draws, the row
 * carries the damage sentence — and §20.2's Arm A retry of the *map* is P6.4's
 * flip-time question, because a boot that fell back has no arena to argue with. */
static esp_err_t adopt(const esp_partition_t *p, slot_t *s, const char *src_name)
{
    uint32_t ms = 0;
    const esp_err_t fe = art_bank_fill(s->art_base, s->h.art_size, true, s->h.art_crc, &ms);
    if (fe != ESP_OK) return fe;
    if (slot_load_geom(s, p) != ESP_OK) return ESP_FAIL;
    s_source = src_name;
    s_refill_ms = ms;
    s_refill_bytes = (uint32_t)s->h.art_size;
    return ESP_OK;
}

/* Resolve the owner's choice and verify the slot it names. Called from app_main
 * BEFORE display_init(), so nothing has mapped the bank yet — the pixels boot
 * verifies are the ones the paint path is about to read.
 *
 * This function no longer installs anything (D13). It used to open the card, copy
 * a container and keep a retry counter, which made every boot depend on a card
 * that may have been pulled and made a half-finished copy a state a device could
 * be left in overnight. An install now happens in orb_theme_install(), on the
 * worker, at the moment the owner picks. Boot draws what is already verified or
 * draws the default; those are the only two outcomes. */
esp_err_t orb_theme_boot(void)
{
    const esp_partition_t *p = art_bank_partition();
    if (!p) {
        ESP_LOGE(TAG, "no \"%s\" partition — theme storage is off", ART_BANK_PARTITION_NAME);
        use_baked_in();
        return ESP_ERR_NOT_FOUND;
    }

    s_live[0] = '\0';
    if (!nvs_get(THEME_NVS_LIVE, s_live, sizeof s_live) || !s_live[0])
        strlcpy(s_live, THEME_LIVE_DEFAULT, sizeof s_live);
    s_damage_id[0] = '\0';
    s_damage_why[0] = '\0';

    /* Slot B is the only thing worth reading: A is the flashed bank, carries no
     * header at all, and is what the fallback path below draws anyway. */
    slot_t b;
    slot_init(&b, SLOT_B_HDR, SLOT_B_ART);
    slot_read(&b, p);

    /* The `staged` witness, and the one rule that lets it expire. It testifies to a
     * flip whose pixels landed and whose header never did, so it clears on the first
     * boot that finds the header it was waiting for — identified by hashing the
     * committed slot's own name back to the id the marker carries. A valid slot
     * naming a DIFFERENT theme is not that commit: the staged install died before
     * its header, and the slot belongs to an older pick. The convergence is one NVS
     * write at boot, and the word it removes is diagnostics rather than a choice,
     * so D13's no-NVS-write-in-the-fallback-path rule is not what this touches. */
    s_staged_id[0] = '\0';
    char staged[ORB_THEME_ID_HEX + 1];
    if (nvs_get(THEME_NVS_STAGED, staged, sizeof staged) && theme_id_ok(staged)) {
        bool landed = false;
        if (b.valid) {
            char cid[ORB_THEME_ID_HEX + 1];
            theme_id(b.h.name, cid);
            landed = (strcmp(cid, staged) == 0);
        }
        if (landed) staged_clear();
        else        strlcpy(s_staged_id, staged, sizeof s_staged_id);
    }

    /* A FORMAT 3 bundle in the slot is installed bytes this dial cannot draw yet.
     * slot_read() above parses FORMAT 2, so it calls a bundle's header invalid — and
     * "no installed theme" would be a lie about bytes that are plainly there. Read
     * the header once through the bundle door and let the row say what is true
     * (D26's rule that a refusal names the reason, on the path that has no press to
     * answer). P6.4's flip page and P6.5's FORMAT 3 reader are what change this. */
    orb_bundle3_hdr_t b3;
    bool slot_is_bundle = false;
    uint32_t raw3[W_WORDS];
    if (!b.valid && esp_partition_read(p, SLOT_B_HDR, raw3, sizeof raw3) == ESP_OK &&
        raw3[W_MAGIC] == THEME_MAGIC && raw3[W_FORMAT] == ORB_BUNDLE3_FORMAT &&
        orb_bundle3_hdr_parse(raw3, &b3) == NULL)
        slot_is_bundle = true;

    if (boot_resolve(s_live, b.valid) == BOOT_ART_CARD) {
        const esp_err_t ae = adopt(p, &b, "slot-B");
        if (ae == ESP_OK) {
            /* The §20.5(2) line: the measured milliseconds of the fused refill, on
             * the boot log, beside the byte count that produced it. §20.2 predicted
             * 150 ms at av-kit size from D14's flash-read arithmetic; this is the
             * number that replaces the prediction, and the ledger cites it. */
            ESP_LOGI(TAG, "theme \"%s\" from slot-B (chosen): %u B refilled to the arena in %u ms",
                     s_active_name, (unsigned)b.h.art_size, (unsigned)s_refill_ms);
            return ESP_OK;
        }
        /* The choice named a slot that will not load. Its bytes were read back and
         * checksummed when the install wrote them, so anything that fails now is
         * corruption and not a job still running: say so on that theme's row, draw
         * the default, and leave the choice WRITTEN. Clearing it here would erase
         * what the owner asked for, would be an NVS write during boot, and would
         * make the dial quietly forget a theme they can still re-install. The picker
         * matches a row by id, so the damaged slot is named by the id its header name
         * hashes to (a headerless slot hashes to nothing → empty, no row). */
        if (b.h.name[0]) {
            char did[THEME_ID_HEX + 1];
            theme_id(b.h.name, did);
            strlcpy(s_damage_id, did, sizeof s_damage_id);
        } else {
            s_damage_id[0] = '\0';
        }
        /* Two refusals, two sentences: a slot that checksums wrong is damage the
         * owner can only fix by re-installing, and an arena that is too small is a
         * capacity answer that names itself (§20.2's Arm A question, D26's rule that
         * a refusal says why). */
        strlcpy(s_damage_why, ae == ESP_ERR_NO_MEM
                    ? "not enough memory for its pixels — the default is drawn"
                    : "the installed copy did not verify — re-install it",
                sizeof s_damage_why);
        ESP_LOGE(TAG, "the chosen theme \"%s\" would not load (%s) — drawing the default",
                 b.h.name, esp_err_to_name(ae));
    } else if (slot_is_bundle) {
        char did[THEME_ID_HEX + 1];
        theme_id(b3.name, did);
        strlcpy(s_damage_id, did, sizeof s_damage_id);
        strlcpy(s_damage_why, "an app bundle this dial cannot draw yet",
                sizeof s_damage_why);
        ESP_LOGI(TAG, "slot-B holds bundle \"%s\" — stored, and not drawable by this dial yet",
                 b3.name);
    } else if (b.valid) {
        /* B holds an install the owner walked away from. It is not drawn and not
         * erased: it is theirs, and the default costs nothing to keep. */
        ESP_LOGI(TAG, "slot-B holds \"%s\" but the dial is on the factory default",
                 b.h.name);
    } else {
        ESP_LOGI(TAG, "no installed theme — drawing the baked-in bank");
    }
    use_baked_in();
    /* The default ALWAYS maps, whatever ORB_ART_MECH_B says (D13 as ruled option A
     * stands: the flashed bytes at partition offset 0 are the only copy of the
     * factory pixels on the board, and an arena copy of them would be made from
     * the very region an install erases). They were checksummed by tools at build
     * time, so there is nothing left to verify here. */
    return art_bank_map_at_0(s_geo.art_size);
}

/** One place to fail: the row shows `why`, the log shows the same sentence, the
 *  caller gets the esp_err_t that decides whether it asks for a reboot. */
static esp_err_t install_failed(esp_err_t e, const char *w, char *why, size_t whysz)
{
    if (why && whysz) strlcpy(why, w ? w : "install failed", whysz);
    ESP_LOGW(TAG, "install: %s", w ? w : "install failed");
    return e;
}

/* The one implementation of "put this container on the wall", reached the same way
 * from the dial's picker and the page's Load button — worker task both times, so the
 * two entry points cannot disagree about whether the copy may take the card lock.
 * Everything before the first flash write can fail as often as it likes; the copy
 * starts only once the whole container has checksummed off the card, which is why a
 * damaged file costs a sentence on the row and not a slot. */
esp_err_t orb_theme_install(const char *id, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    const esp_partition_t *p = art_bank_partition();
    if (!p)
        return install_failed(ESP_ERR_NOT_FOUND, "this board has no theme storage",
                              why, whysz);

    const char *w = NULL;
    orb_thdr_t h;
    const int64_t t0 = esp_timer_get_time();
    /* Resolve-by-walk, then open + re-verify the header hash inside the lock: this is
     * the identity rule the old card-name==slug check enforced, now applied to the id
     * so a file swapped under an id the picker is holding cannot install. It lives in
     * card_open_verified_id so install and delete resolve an id the same way. Checked
     * before the checksums — one header read, not four megabytes of pixels. */
    const int fd = card_open_verified_id(id, &h, &w);
    if (fd < 0) return install_failed(ESP_FAIL, w, why, whysz);

    /* Geometry bytes and pixel bytes, both checksummed, over the whole file, off the
     * card — the same ladder the old boot path climbed, in the same order, one call
     * earlier in the owner's attention. */
    const char *vv = verify_container(fd, &h);
    if (vv) {
        card_close(fd);
        return install_failed(ESP_ERR_INVALID_CRC, vv, why, whysz);
    }

    /* The serial keeps counting up across re-installs: whatever B last held says what
     * number comes next, and a header that cannot be read restarts the count at 1 —
     * safe now that B is the only slot in play and nothing compares two slots. */
    slot_t old;
    slot_init(&old, SLOT_B_HDR, SLOT_B_ART);
    slot_read(&old, p);
    const uint32_t serial = old.valid ? old.h.serial + 1 : 1;

    /* §20.2 step 4, in the shape this path can actually honour: the marker goes up
     * before the first erase and comes down after the header lands, so every window
     * between "the owner pressed" and "the slot is committed" is witnessed. */
    staged_mark(id);
    esp_err_t e = copy_into(fd, &h, p, install_target(), &w);
    if (e == ESP_OK) {
        /* Verify from the PARTITION, then make the slot live. Reversing those two
         * would let a slot go live on bytes nobody read back. */
        slot_t probe;
        slot_init(&probe, install_target(), install_target() + SLOT_ART_OFF);
        memcpy(&probe.h, &h, sizeof(h));
        probe.h.geom_off = SLOT_GEOM_OFF;
        probe.h.art_off  = SLOT_ART_OFF;
        const esp_err_t ve = slot_verify_pixels(&probe, p);
        const char *gw = (ve == ESP_OK) ? slot_parse_geom(&probe, p, /*adopt=*/false) : NULL;
        if (ve != ESP_OK) {
            w = "pixels did not read back intact — try another card";
            e = ve;
        } else if (gw) {
            /* The parse boot is about to run, run here instead, on the bytes as
             * they now sit in the slot. Without it the install checks less than the
             * boot does: every checksum can match and the theme still be one the
             * painter refuses to draw — a complication shown with no size, a hand
             * whose pixels lie outside the payload — and the owner's press would buy
             * a reboot into the factory dial with the reason only in a log line.
             * Bench-found with the card's own "neon". The slot keeps its old header
             * either way, because the commit below has not happened yet. */
            w = gw;
            e = ESP_ERR_INVALID_CRC;
        } else {
            /* The header block is erased HERE, not in copy_into(): erasing it
             * earlier would invalidate B for the whole multi-second copy, and erasing
             * it later is impossible — a NOR write over already-programmed bytes ANDs
             * the bits and yields a header whose checksum can never match. Between
             * these two facts sits the safety property, and after D13 it has a second
             * half: B's old header stays valid until the new one is ready to replace
             * it, AND the slot the factory default draws from is never an install
             * target at all. So an install that dies anywhere from here to the header
             * write — in the erase, in the write, at a power cut — leaves B headerless
             * and the next boot draws the default. */
            const esp_err_t ee = esp_partition_erase_range(p, install_target(),
                                                           THEME_SECTOR);
            if (ee != ESP_OK) {
                w = "could not clear the old slot header";
                e = ee;
            } else {
                uint32_t hdr[W_WORDS];
                build_slot_hdr(hdr, &h, serial);
                e = esp_partition_write(p, install_target(), hdr, sizeof(hdr));
                if (e != ESP_OK) w = "could not make the new slot live";
            }
        }
    }
    card_close(fd);
    if (e != ESP_OK) {
        /* Refused, so nothing is in flight: the row carries try_why, and a witness
         * left standing would name two events on one theme at once. */
        staged_clear();
        return install_failed(e, w, why, whysz);
    }
    /* The header landed, so the flip is committed and the witness has nothing left to
     * testify to. Cleared here rather than after set_live below: a choice key that
     * fails to write does not un-commit the slot, and the sentence right after this
     * one is already the honest report of that. */
    staged_clear();

    /* The commit is last and is the ONLY thing that makes the new bytes the dial's:
     * B is verified by here, so a set that fails leaves a correct-but-unused install
     * behind rather than a dial drawing bytes the owner never saw work. */
    e = orb_theme_set_live(THEME_LIVE_CARD);
    if (e != ESP_OK)
        return install_failed(e, "it installed, but the dial could not be told to use it",
                              why, whysz);
    s_damage_id[0] = '\0';
    s_damage_why[0] = '\0';
    /* Timed, and logged after the card lock came back: a 3.87 MB container takes
     * seconds to read at SPI 20 MHz, so this is the line that says how long the
     * worker held the card away from the photo viewer, and the line only appearing
     * after the give is what proves the mutex came back. */
    ESP_LOGI(TAG, "installed \"%s\" (%s) into slot-B in %lld ms — the dial reboots to draw it",
             h.name, id, (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}




/* ------------------------------------------------------------------------- *
 * The install, and the choice. Worker-task calls: the page reaches them through
 * the same job queue the dial does, so the two entry points cannot disagree about
 * whether the copy happens on a task that is allowed to take the card lock.
 * ------------------------------------------------------------------------- */

/** What the last pick could not do, and which theme it was on. Kept here rather
 *  than in the picker or the page so both show one sentence about one failure: an
 *  install attempted from the browser must put the same words on the dial's row,
 *  or the two entry points are two opinions about the same event. */
static char s_try_id[ORB_THEME_ID_HEX + 1];
static char s_try_why[56];

const char *orb_theme_try_id(void)  { return s_try_id; }
const char *orb_theme_try_why(void) { return s_try_why; }

/** Record an attempt, and the sentence a failure earns. Called by the two post
 *  functions below, which are the only ways anything reaches install(). */
static void try_note(const char *id, const char *why)
{
    strlcpy(s_try_id, id ? id : "", sizeof s_try_id);
    strlcpy(s_try_why, why ? why : "", sizeof s_try_why);
}

/** The worker's copy of a request: the id, nothing else. Small enough that the
 *  queue owns it outright and the caller's stack may vanish on return. */
typedef struct { char id[ORB_THEME_ID_HEX + 1]; } install_req_t;

static void install_job(void *arg)
{
    const install_req_t *r = arg;
    char why[56] = "";
    if (orb_theme_install(r->id, why, sizeof why) != ESP_OK) {
        try_note(r->id, why[0] ? why : "the install failed");
        return;
    }
    /* The install committed the choice, so the last complaint about this theme is
     * retired with it: leaving it set would keep the row warning about a file that
     * is now the dial. */
    try_note(NULL, NULL);
    /* Asked, never called: the reboot belongs to the OS, and the delay it owns is
     * what lets the last frame of the page that said it worked reach the panel. */
    if (orb_jobs_reboot_soon("theme install") != ESP_OK)
        ESP_LOGE(TAG, "\"%s\" installed but the reboot could not be armed", r->id);
}

static void default_job(void *arg)
{
    (void)arg;
    /* No card, no container, no copy: the factory dial is the bank `idf.py flash`
     * wrote at partition offset 0, which is the one set of bytes on this device
     * that has never needed verifying at runtime. */
    if (orb_theme_set_live(THEME_LIVE_DEFAULT) != ESP_OK) {
        try_note("", "the dial could not be told to use the default");
        return;
    }
    try_note(NULL, NULL);
    if (orb_jobs_reboot_soon("theme: the factory default") != ESP_OK)
        ESP_LOGE(TAG, "default chosen but the reboot could not be armed");
}

esp_err_t orb_theme_post_install(const char *id)
{
    if (!theme_id_ok(id)) return ESP_ERR_INVALID_ARG;
    /* Named before the queue is asked, so the row the failure lands on is the row
     * that was pressed even when the reason is that the card never answered. */
    try_note(id, "");
    install_req_t r;
    strlcpy(r.id, id, sizeof r.id);
    return orb_jobs_post(install_job, &r, sizeof r);
}

esp_err_t orb_theme_post_default(void)
{
    try_note("", "");
    const install_req_t r = { { 0 } };
    return orb_jobs_post(default_job, (void *)&r, sizeof r);
}

/* P6.4's flip worker: the bundle's half of install_job, and its one deliberate
 * difference is the reboot. A FORMAT 2 install reboots because loading is
 * choosing, and the new dial cannot draw itself until the boot refills the
 * arena; a bundle store changes nothing the dial draws (the slot is stored,
 * not live, until P6.5), so rebooting would cost the owner a boot to show them
 * the same clock they already had. The try witness is identical: the row the
 * press named carries the refusal sentence, exactly as the FORMAT 2 door's
 * does. */
static void bundle_job(void *arg)
{
    const install_req_t *r = arg;
    char why[56] = "";
    if (orb_theme_install_bundle(r->id, why, sizeof why) != ESP_OK) {
        try_note(r->id, why[0] ? why : "the install failed");
        return;
    }
    try_note(NULL, NULL);
}

esp_err_t orb_theme_post_bundle(const char *id)
{
    if (!theme_id_ok(id)) return ESP_ERR_INVALID_ARG;
    /* Named before the queue is asked, exactly as post_install does it: the row
     * a refusal lands on is the row that was pressed, even when the reason is
     * that the card never answered. */
    try_note(id, "");
    install_req_t r;
    strlcpy(r.id, id, sizeof r.id);
    return orb_jobs_post(bundle_job, &r, sizeof r);
}

esp_err_t orb_theme_set_live(const char *which)
{
    if (!which) return ESP_ERR_INVALID_ARG;
    /* Only the two real choices reach NVS: a typo written down is a boot that
     * silently draws the default, and a silent default is the failure mode this
     * whole rewrite exists to remove. */
    if (strcmp(which, THEME_LIVE_DEFAULT) != 0 && strcmp(which, THEME_LIVE_CARD) != 0)
        return ESP_ERR_INVALID_ARG;
    const esp_err_t e = nvs_put(THEME_NVS_LIVE, which);
    if (e != ESP_OK) return e;
    strlcpy(s_live, which, sizeof(s_live));
    return ESP_OK;
}

esp_err_t orb_theme_delete(const char *id)
{
    /* Resolve-by-walk first — the same card_open_verified_id install uses — so the
     * id is proven to name a real, self-consistent file before anything is unlinked.
     * A card file whose header name does not hash to its id is refused here exactly as
     * at install; unlinking never trusts a client string to reach a path builder.
     * Deleting the theme that is on the wall is allowed and must be: the installed
     * copy is already in flash and does not need the card. But it is the case where
     * "delete" looks like it changed nothing, because the dial keeps drawing that
     * theme until another is loaded — so it is logged, and the page says so too. */
    const char *w = NULL;
    orb_thdr_t h;
    /* Ask for the card before the lock below, not after: this runs on a worker that
     * a boot can start before orb_sd_init() ever called, and FatFs calls against an
     * unmounted volume answer with the wrong error, not a crash. card_open_verified_id
     * mounts too; this is the one the unlink's own lock hold depends on. */
    if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;
    int fd = card_open_verified_id(id, &h, &w);
    if (fd < 0) {
        /* The FORMAT 2 resolver refuses a bundle at the identity check (its name
         * hashes fine, but card_check calls it damaged), so it cannot tell a
         * bundle from a corrupt theme here. Try the bundle door, and keep the
         * FORMAT 2 reason if that too refuses — the caller's error is then the
         * one about the id it was issued, never a card edit's wording. */
        orb_bundle3_hdr_t b3;
        uint32_t fsz = 0;
        const char *bw = NULL;
        const int bfd = card_open_verified_id3(id, &b3, &fsz, &bw);
        if (bfd >= 0) {
            fd = bfd;
            strlcpy(h.name, b3.name, sizeof h.name);
            h.fmt = 3;
        }
    }
    if (fd < 0) return ESP_ERR_NOT_FOUND;
    card_close(fd);

    char path[ORB_SD_PATH_MAX];
    if (!theme_file(id, path, sizeof path)) return ESP_ERR_INVALID_ARG;

    orb_sd_lock();
    const int r = unlink(path);
    const int err = errno;
    orb_sd_unlock();
    if (r != 0) {
        ESP_LOGW(TAG, "delete \"%s\": %s", id, strerror(err));
        return err == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted \"%s\" (%s) from the card%s", h.name, id,
             s_active_valid && strcmp(h.name, s_active_name) == 0
                 ? " (still installed on the dial)" : "");
    return ESP_OK;
}

/* The §6 installed-slot comparison, as a pure function of the slot's captured
 * identity and one container's header, so tools/host_sd_test.py lifts it and drives
 * every case with vectors. Two rules, and the difference between them is the whole
 * point: a card file is ACTIVE only when the name and both checksums still agree
 * with what boot adopted, because Load would otherwise be a silent no-op the owner
 * never asked for; it is CHANGED when the name agrees and a checksum does not, which
 * means the bytes on the card are not the bytes on the wall and the page has to offer
 * a re-install. A device that has never installed anything has no captured identity
 * and marks nothing either way. */
static void theme_state(const orb_thdr_t *h, bool *active, bool *changed)
{
    if (!s_active_valid) { *active = false; *changed = false; return; }
    const bool same_name  = strcmp(h->name, s_active_name) == 0;
    const bool geom_diff  = h->geom_crc != s_active_geom_crc;
    const bool art_diff   = h->art_crc  != s_active_art_crc;
    *active  = same_name && !geom_diff && !art_diff;
    *changed = same_name && (geom_diff || art_diff);
}

/* One walk's accumulation. The OS hands the callback one entry at a time so the
 * list is built under a single hold of the card lock, which is the only way an
 * entry cannot be edited into something else between being listed and being
 * read. */
typedef struct {
    orb_theme_entry_t *out;
    int                cap;
    int                n;
} scan_ctx_t;

/* The app's half of the walk. The OS found a <id>.orbtheme, checked the filename
 * is one path segment, and has it open; what the file MEANS — whether it is a theme
 * at all, whether its header name hashes to the id its file is named after, whether
 * it is the one on the dial — is this layer's business and not the card's. */
static bool scan_entry(void *arg, const orb_file_rec_t *rec, int fd)
{
    scan_ctx_t *const c = arg;
    if (c->n >= c->cap) return false;

    orb_theme_entry_t *e = &c->out[c->n];
    ESP_LOGI(TAG, "checking \"%s\" on the card", rec->name);
    e->name[0] = '\0';
    /* The id is the filename the OS already normalised; echo it so a damaged row
     * still has an addressable handle, then overwrite with the header's own id once
     * the header is read. */
    strlcpy(e->id, rec->name, sizeof(e->id));
    e->why[0] = '\0';
    e->active = false; e->changed = false;
    e->art_size = 0; e->serial = 0;
    e->fmt = 0; e->n_sections = 0;

    orb_thdr_t h;
    const char *why = card_check(fd, &h);
    if (why == THEME3_REFUSAL) {
        /* A bundle, listed as one (P6.4): the id is the same R4 hash of the same
         * owner name, the layout and art bounds are the install door's own rule
         * (card_check3, re-run here so a listed row means a press has a chance),
         * and the row is never active: nothing draws a bundle yet. The reason a
         * torn bundle header carries is the generic one it always carried —
         * §24.7 measured that and the order above is what keeps it true. */
        orb_bundle3_hdr_t b3;
        uint32_t fsz3 = 0;
        why = card_check3(fd, &b3, &fsz3);
        if (!why) {
            char bid[THEME_ID_HEX + 1];
            theme_id(b3.name, bid);
            strlcpy(e->name, b3.name, sizeof e->name);
            e->fmt = 3;
            e->n_sections = b3.n_sections;
            e->art_size = b3.art_size;
            e->serial = b3.serial;
            if (strcmp(bid, rec->name) != 0) {
                /* FORMAT 2's own sentence, one rule for both formats: a bundle
                 * copied onto the card under a name that is not its hash is
                 * addressable and refused, exactly like a theme — and the id the
                 * row carries stays the filename stem, which is what a damaged
                 * row needs to remain addressable. */
                strlcpy(e->why, "the file is not named for its own theme name — re-upload it",
                        sizeof(e->why));
                e->err = ESP_ERR_INVALID_ARG;
            } else {
                strlcpy(e->id, bid, sizeof e->id);
                e->err = ESP_OK;
            }
            c->n++;
            return true;
        }
    }
    if (why) {
        /* Listed with its reason rather than skipped. "no themes on the card" and
         * "three themes, all damaged" are different problems — the card's or the
         * bake's — and hiding the entries is what makes that undiagnosable. The
         * reason goes in .why, not as a name: the page shows the name as a label and
         * the reason as a warning, and overloading one field with the other means a
         * damaged theme is listed under the name "geometry checksum". */
        strlcpy(e->why, why, sizeof(e->why));
        e->err = ESP_ERR_INVALID_ARG;
        c->n++;
        return true;
    }

    /* The id this file truly IS: the header's owner name hashed, not the filename.
     * They must agree — the file is NAMED by the hash of exactly this name — so a
     * file copied onto the card by hand (correct bytes, wrong filename) or a card
     * edited on a laptop between listing and reading is caught here and refused
     * rather than shown as installable. The install/delete paths re-check the same
     * rule in card_open_verified_id; here it is the reason the row carries. */
    char id[THEME_ID_HEX + 1];
    theme_id(h.name, id);
    strlcpy(e->name, h.name, sizeof(e->name));
    strlcpy(e->id, id, sizeof(e->id));
    e->art_size = h.art_size;
    e->serial   = h.serial;

    if (strcmp(id, rec->name) != 0) {
        ESP_LOGW(TAG, "\"%s\" on the card calls itself \"%s\"", rec->name, h.name);
        strlcpy(e->why, "the file is not named for its own theme name — re-upload it",
                sizeof(e->why));
        e->err = ESP_ERR_INVALID_ARG;
        c->n++;
        return true;
    }

    /* Active vs changed (plan §6) — the one rule, in theme_state(), which the host
     * harness drives with vectors: same name and both CRCs is the wall; same name and
     * a different CRC is a card file that is not what the dial draws. */
    theme_state(&h, &e->active, &e->changed);
    e->fmt = h.fmt;
    e->err = ESP_OK;
    c->n++;
    return true;
}

int orb_theme_scan(orb_theme_entry_t *out, int cap)
{
    if (!out || cap <= 0) return -1;
    /* While an upload owns the card, do not open a second file on it: FatFs is
     * reentrant per volume, but this walk plus a 3.7 MB write on the same
     * directory would serialise the httpd worker behind the upload for seconds, and
     * a page that cannot poll its own list looks exactly like a device that has
     * stopped answering. The caller reports "uploading" instead. */
    if (orb_theme_upload_busy()) return -1;

    /* The directory and the extension are the clock app's declaration, not a
     * constant here: the OS owns the walk, so this layer asks where its themes
     * live rather than assuming. A missing declaration is a build order bug —
     * nothing on the card has been read, so -1 is the honest answer and the line
     * says which invariant broke. */
    const orb_file_src_t *src = orb_files_src("clock", "themes");
    if (!src) {
        ESP_LOGE(TAG, "no \"themes\" file source declared — the clock app did not register");
        return -1;
    }

    scan_ctx_t c = { .out = out, .cap = cap, .n = 0 };
    /* Both ends logged, with the wait: a scan that never comes back is the one card
     * failure that looks like a dead web server from outside (the dial keeps drawing
     * and ping keeps replying), and which of the two lines is missing says whether the
     * worker is waiting on the mutex or inside FatFs. */
    const int64_t t_scan = esp_timer_get_time();
    ESP_LOGI(TAG, "listing %s", src->dir);
    if (orb_files_walk(src, scan_entry, &c) < 0) return -1;
    ESP_LOGI(TAG, "%d entr%slisted in %lld ms", c.n, c.n == 1 ? "y " : "ies ",
             (long long)((esp_timer_get_time() - t_scan) / 1000));
    return c.n;
}

/* ------------------------------------------------------------------------- *
 * Uploading a theme from the browser.
 * ------------------------------------------------------------------------- */

struct orb_theme_up {
    int    fd;
    char   tmp[ORB_SD_PATH_MAX];
    uint32_t got;
};

/* The one upload in progress. orb_theme_upload_busy() is the gate every caller
 * checks first, so there is at most one of these alive at a time — which is what
 * lets the scratch file keep a single fixed name and the card lock stay held across
 * the whole transfer without a second upload queueing behind it. */
static orb_theme_up_t *s_up;

/* The scratch file an upload lands in before its header is read. It deliberately
 * does NOT end in .orbtheme, so orb_files_walk never lists it, and it is a single
 * fixed name so the destination is never half-built from a client string — the card
 * filename is the hash of the name INSIDE the file, which is only knowable after the
 * whole upload has arrived and its header parsed. orb_theme_upload_busy() admits one
 * upload at a time, so this name cannot collide with a second upload's scratch. */
#define THEME_SCRATCH "upload.part"

/* Read and validate just the header block of an open container, filling the counts
 * the geom blob declares. Returns the reason it is not a usable container, or NULL.
 * Shared by the uploaded scratch file and an existing card file the duplicate check
 * opens beside it, so both are held to the identical header rule. */
static const char *hdr_at(int fd, orb_thdr_t *h)
{
    size_t pos = 0;
    uint32_t hdr[W_WORDS];
    if (rd_at(fd, &pos, (uint8_t *)hdr, sizeof hdr) != (ssize_t)sizeof hdr)
        return "too short to be a theme";
    const char *r = hdr_parse(hdr, h);
    if (r) return r;
    if (h->geom_off != W_WORDS * 4u || h->art_off < h->geom_off + h->geom_size ||
        h->art_off % THEME_SECTOR)
        return "container parts are not where this format puts them";
    uint32_t c[GEOM_COUNTS_V2], cnt[7];
    pos = h->geom_off;
    if (rd_at(fd, &pos, (uint8_t *)c, sizeof c) != (ssize_t)sizeof c)
        return "geometry blob is missing";
    if (geom_counts(c, h->geom_size / 4u, cnt) < 0)
        return "geometry blob size does not match its own tables";
    h->n_rects = cnt[0]; h->n_first = cnt[1]; h->n_delays = cnt[2]; h->n_bgf = cnt[3];
    h->n_comp  = cnt[4]; h->n_aux   = cnt[5]; h->n_bank   = cnt[6];
    return NULL;
}

bool orb_theme_upload_busy(void) { return s_up != NULL; }

esp_err_t orb_theme_upload_begin(orb_theme_up_t **out)
{
    *out = NULL;
    if (s_up) return ESP_ERR_INVALID_STATE;                /* one at a time */
    const orb_file_src_t *src = orb_files_src("clock", "themes");
    if (!src) return ESP_ERR_NOT_FOUND;
    if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;
    /* Reserve the card read buffer NOW, before the radio carries 3.87 MB for a
     * transfer this device cannot finish. verify_container() reserves it again
     * defensively; this is the early, cheap no. */
    if (ensure_chunk() != ESP_OK) return ESP_ERR_NO_MEM;

    orb_theme_up_t *u = calloc(1, sizeof *u);
    if (!u) return ESP_ERR_NO_MEM;
    /* The scratch path is built from the OS's own declared themes directory, never
     * from anything the client sends — the destination filename is not even known
     * yet, because it is the hash of a name that is still inside the bytes still
     * arriving. */
    const int n = snprintf(u->tmp, sizeof u->tmp, "%s/%s", src->dir, THEME_SCRATCH);
    if (n < 0 || (size_t)n >= sizeof u->tmp) { free(u); return ESP_ERR_INVALID_SIZE; }

    orb_sd_lock();
    u->fd = open(u->tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (u->fd < 0) {
        const int e = errno;
        orb_sd_unlock(); free(u);
        ESP_LOGW(TAG, "upload: create failed (%s)", strerror(e));
        return ESP_FAIL;
    }
    s_up = u;
    *out = u;
    ESP_LOGI(TAG, "uploading a theme to the card");
    return ESP_OK;
}

esp_err_t orb_theme_upload_data(orb_theme_up_t *u, const void *buf, size_t n)
{
    if (!u || u != s_up) return ESP_ERR_INVALID_STATE;
    if (u->got + n > ORB_THEME_UPLOAD_MAX) {
        /* Refused before the write, not after: a card has finite free space and an
         * attacker with a browser has an infinite body, and the one that costs
         * nothing to stop is the one stopped on a length. */
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *p = buf;
    while (n) {
        const ssize_t w = write(u->fd, p, n);
        if (w <= 0) {
            const int e = errno;
            ESP_LOGW(TAG, "upload: card write failed (%s)", strerror(e));
            /* ENOMEM is FatFS's FR_NOT_ENOUGH_CORE: the sdmmc driver could not get
             * its own internal scratch. A capacity verdict, never a size one —
             * §17.6's evidence is a 125 B ask refused by a fragmented pool. */
            if (e == ENOMEM) return ESP_ERR_NO_MEM;
            return ESP_FAIL;              /* ENOSPC and friends: caller reports it */
        }
        p += w; n -= (size_t)w; u->got += (uint32_t)w;
    }
    return ESP_OK;
}

esp_err_t orb_theme_upload_end(orb_theme_up_t **u, bool overwrite, const char **why)
{
    orb_theme_up_t *const up = *u;
    *why = NULL;
    if (!up || up != s_up) { *why = "no upload is in progress"; return ESP_ERR_INVALID_STATE; }

    close(up->fd);
    s_up = NULL;                     /* the card is still locked; released below */

    /* Verify the file that is NOW on the card — not the bytes that passed through
     * this handler. A card that dropped a write, or a client that lied about the
     * length, is caught here rather than by the install three boots later.
     *
     * Both formats are received (P6.4): FORMAT 3 is stored through this door and
     * installed by the bundle press, which re-verifies everything it is about to
     * put in a slot. A torn FORMAT 3 header still answers the generic line — its
     * format word is unverified, and the refusal says only what is proven. */
    const int fd = open(up->tmp, O_RDONLY);
    if (fd < 0) {
        *why = "the uploaded file is not on the card";
        unlink(up->tmp); orb_sd_unlock(); free(up);
        return ESP_FAIL;
    }
    orb_thdr_t h;
    orb_bundle3_hdr_t b3;
    uint32_t fsz = 0;
    const char *r = hdr_at_any(fd, &h, &b3, &fsz);
    if (!r) r = (h.fmt == 3) ? verify_bundle3(fd, &b3, fsz) : verify_container(fd, &h);
    close(fd);

    /* The card filename is the id, and the id is the hash of the header's owner name.
     * Nothing the client named this upload — the destination is whatever this decides,
     * from bytes that had to arrive intact. hdr_at() already refused a container with
     * no name in it, so the name here is never empty and theme_id() cannot be asked for
     * the hash of nothing. */
    char id[THEME_ID_HEX + 1];
    char final[ORB_SD_PATH_MAX];
    if (!r) theme_id(h.name, id);
    if (!r && !theme_file(id, final, sizeof final)) r = "the theme name is not one the card can hold";

    /* Duplicate policy (plan §5). If a file already sits at this id's path, compare
     * the §6 identity tuple — geometry, pixels, and the install serial. Byte-identical
     * is the same theme re-sent: succeed quietly and leave the file that is there
     * (which the dial may already be drawing from flash), so a double-submit or a
     * retried request is not an error. Different bytes under the same name are a real
     * collision: refuse without ?overwrite=1, because silently replacing a theme the
     * owner liked is the surprise this whole rule exists to prevent. */
    bool replace = false;
    if (!r) {
        const int efd = open(final, O_RDONLY);
        if (efd >= 0) {
            orb_thdr_t oh;
            orb_bundle3_hdr_t oh3;
            uint32_t ohfsz = 0;
            const char *or_ = hdr_at_any(efd, &oh, &oh3, &ohfsz);
            close(efd);
            if (!or_ && oh.fmt == h.fmt && oh.geom_crc == h.geom_crc &&
                oh.art_crc == h.art_crc &&
                oh.serial == h.serial && strcmp(oh.name, h.name) == 0) {
                /* Idempotent: identical theme, discard the scratch, nothing changed. */
                unlink(up->tmp); orb_sd_unlock(); free(up);
                ESP_LOGI(TAG, "upload of \"%s\" (%s) is unchanged — kept as is", h.name, id);
                return ESP_OK;
            }
            if (!overwrite) {
                /* Same name, different bytes. The existing file is left intact; the
                 * scratch is deleted so no rejected upload survives to confuse the list. */
                unlink(up->tmp); orb_sd_unlock();
                *why = "a different theme with that name is already on the card";
                ESP_LOGW(TAG, "upload \"%s\" (%s) collides with a different installed theme", h.name, id);
                return ORB_THEME_UPLOAD_E_DUP;
            }
            replace = true;    /* overwrite=1 and it verified: replace the old bytes */
        }
    }

    if (r) {
        /* Deleted, not left: a rejected upload that survives on the card shows up in
         * the list as an unloadable entry, and the owner has to guess whether they
         * uploaded it or somebody else did. */
        unlink(up->tmp);
        orb_sd_unlock();
        *why = r;
        ESP_LOGW(TAG, "upload rejected: %s", r);
        free(up);
        return ESP_ERR_INVALID_CRC;
    }

    /* f_rename() fails on an existing destination (FR_EXIST), so an overwrite is an
     * unlink plus a rename rather than a replace. Not atomic together: the window
     * between them is the one moment this theme does not exist on the card, and it is
     * a fraction of a millisecond on a device nobody is pulling the card from
     * mid-upload — the alternative, renaming over a live theme and discovering the
     * checksum mismatch afterwards, destroys the theme that was working. Nothing is
     * renamed until this point has verified the whole container, so the window never
     * exposes a half-written file. */
    if (replace) unlink(final);
    const int rr = rename(up->tmp, final);
    const int re = errno;
    orb_sd_unlock();
    free(up);
    if (rr != 0) {
        *why = "could not put the uploaded theme in place";
        ESP_LOGE(TAG, "rename: %s", strerror(re));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "theme \"%s\" uploaded to the card as %s%s", h.name, id,
             replace ? " (overwrote the previous one)" : "");
    return ESP_OK;
}

void orb_theme_upload_cancel(orb_theme_up_t **u)
{
    orb_theme_up_t *const up = *u;
    if (!up) return;
    *u = NULL;
    if (up == s_up) {
        close(up->fd);
        s_up = NULL;
        /* The partial file is always removed: the next attempt truncates it anyway,
         * but a .part left here is a file with the theme's name and none of its
         * bytes, which is the kind of thing that gets found months later and
         * investigated as corruption. */
        unlink(up->tmp);
        orb_sd_unlock();
    }
    free(up);
}
