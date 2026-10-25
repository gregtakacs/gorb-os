/**
 * orb_theme.c — see orb_theme.h for the model. An installed theme is a container
 * from the card copied into the single slot at the head of the `assets` partition,
 * and the geometry it carries is what clock_view.c reads instead of the compiled-in
 * one.
 *
 * The wire format is DEFINED in tools/orbtheme.py and mirrored here. That mirror
 * is the risk in this file: a field added on one side only produces a dial whose
 * hand pivots read as overlay offsets, which looks like bad art and not like a
 * bug. Three things hold it together — the static asserts on the header layout,
 * the bounds checks in orb_clock3_parse() that refuse a theme rather than
 * render it wrong, and the golden-pair test in tools/host_sd_test.py that
 * projects the fixture through the Python and reads it back through this C.
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
#include "esp_random.h"           // the staged-commit token
#include "esp_rom_crc.h"
#include "esp_timer.h"              // the listing timing in orb_theme_scan()
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"   /* the §31 verdict lock */
#include "nvs.h"
#include "nvs_flash.h"

#include "orb_theme.h"
#include "orb_bundle.h"
#include "orb_clock3.h"          /* the FORMAT 3 clock section's projection (P6.5) */
#include "orb_sd.h"
#include "orb_jobs.h"      /* the two post_* calls: an install is a worker job, never a poll */
#include "orb_files.h"      /* the OS owns the card walk; this layer interprets what it finds */
#include "art_bank.h"
#include "orb_sched.h"          /* store_park_dial: the tick's park gate is the task-safe half of §6.3 */
#include "orb_app.h"            /* orb_app_home(): the row the store hands the glass to */
#include "input_router.h"       /* input_router_request/current: the task-safe screen switch */
#include "orb_boot_overlay.h"   /* the store's progress frames (§6.3) */
#include "freertos/task.h"      /* vTaskDelay: the park wait rides ticks, not a spin */
#include "mbedtls/sha256.h"  /* theme_id(): sha256 over "slug/name" -> the card filename */

static const char *TAG = "orb_theme";

/* Settings live in their OWN namespace in the `storage` partition, as
 * partitions.csv demands: esp_wifi owns "nvs.net80211" and orb_net owns
 * "orb.net". A chosen theme is device config the owner picked, so it has to
 * survive the same things the timezone does — including main.c's recovery
 * nvs_flash_erase() of the default partition. */
#define THEME_NVS_PARTITION "storage"
#define THEME_NVS_NAMESPACE "orb.settings"
/* Migration 1 retired the `theme_live` choice key: with one slot there is no slot
 * to name, so the key and the A/B designator words it held are gone. A board whose
 * NVS still holds `theme_live` ignores it — boot reads the one slot at offset 0. */
/* D30's §20.4 ledger word, and NOT the key Migration 1 deleted. This one changes nothing
 * about what the glass draws: boot verifies the one slot, commits only
 * on a valid header, and then reports here whether a store that had already swapped
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

/* Slot layout, private to this file. There is ONE slot, at partition offset 0. The
 * two within-slot offsets are the only numbers here. Both are 64 KB aligned because
 * 64 KB is esp_partition_mmap's page, and an unaligned start wastes an MMU entry on
 * rounding at each end. The geometry sits in its own sector at +0x1000 rather than
 * tight against the 128-byte header so that making the slot live — an erase plus a
 * rewrite of its header block — cannot touch anything underneath it. Migration 1
 * retired the A/B pair: the whole 8 MB `assets` partition is one theme slot. */
#define SLOT_HDR        0x000000u
#define SLOT_GEOM_OFF   0x00001000u                 /* within the slot */
#define SLOT_ART_OFF    0x00010000u                 /* within the slot */
#define SLOT_GEOM_CAP   (SLOT_ART_OFF - SLOT_GEOM_OFF)   /* 60 KB of tables */
#define SLOT_CAP        0x00800000u                      /* 8 MB, the whole partition */

/* The exported ceiling is THIS layout's arithmetic, not a copy of it. A tool
 * that gates on ORB_THEME_SLOT_CAP is gating on SLOT_CAP less SLOT_ART_OFF; if
 * these two constants ever stop producing the exported number, every cap check
 * in every reader and tool silently gates on a fiction (§7.4). */
_Static_assert((uint32_t)SLOT_CAP - SLOT_ART_OFF == ORB_THEME_SLOT_CAP,
               "ORB_THEME_SLOT_CAP drifted from the slot layout it names");
/* The stem ceiling and the name ceiling are ONE number by rule: every theme
 * handle the identity layer can build must pass orb_sd_name_ok() and fit the
 * walk's buffer, and a handle the validator refused would be a theme the
 * upload door filed under a name the next scan cannot echo back. */
_Static_assert(ORB_THEME_HANDLE_MAX == ORB_SD_NAME_MAX,
               "the theme handle ceiling and the card name ceiling drifted apart");

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
 * at paint time into a half-drawn frame. The plate bank's own bound is no loader
 * constant: orb_clock3.c refuses a bank past CLK_BG_MAX at the projection (§31),
 * so s_b3_bgf below never sees a row count it cannot hold. */
#define RECTS_MAX       2048
#define GIFS_MAX        32                         /* CLK_GIF_MAX */

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
                                          * filename is theme_stem(slug, name), never this */
    uint32_t fmt;                        /* the format stamp card_check3 reads; only 3
                                          * survives now that FORMAT 2 is retired.
                                          * Unset (0) means unreadable. */
    uint32_t n_sections;                 /* fmt 3 only: the index's app count */
} orb_thdr_t;

typedef struct {
    const uint32_t *w;
    size_t          n, i;
    const char     *why;                 /* first reason this is not a theme */
} rd_t;







/* The FORMAT 3 gate's one sentence, and the accept signal, in one definition.
 * Callers that can install a bundle compare the returned POINTER rather than
 * rephrasing what a caller that cannot should show, so the wording lives once
 * and the rule cannot drift (the method R4 set for the identity refusal).
 * The pointer is the fact; the text only rides it. The old text — "update the
 * dial to load it" — was written when no dial loaded bundles at all, and the
 * hardware found it at the wrong door: orb-A's FORMAT 2 install press on a
 * bundle id put this sentence's older shape on the row through the identity
 * rule, which is a lie about a file whose name matches its id perfectly
 * (§24.8's bench). The sentence names the file's format and the button that
 * answers it, and it fits the try witness's 56-byte buffer whole (§24.8). */
static const char THEME3_REFUSAL[] =
    "that is a FORMAT 3 app bundle — press Store, not Load";
_Static_assert(sizeof THEME3_REFUSAL <= 56,
               "the bundle refusal must fit the try witness (s_try_why)");



/* One canonical card identity, mirrored from tools/orbtheme.py's theme_id(): the
 * card filename stem is <slug>-<token>-<hex(sha256(slug/name))[:12]>, LOWERCASE
 * hex over the slug, a '/', and the name bytes up to the name's NUL — never the
 * padded THEME_NAME_MAX-byte array, so ("clock","Neon") hashes exactly as
 * hashlib.sha256(b"clock/Neon") and a stored name never picks up trailing zero
 * bytes as identity. The theme's one true name is the string inside its
 * container and its owner is the container's single section; the card filename
 * carries BOTH, which is why the same theme name can live under two apps
 * without either stealing the other's row. The middle token is a legible
 * decoration derived from the name; the identity is the pair around it, so a
 * hand rename that keeps slug and hex keeps the theme addressable.
 *
 * The host harness lifts these functions verbatim and pins them against the
 * Python versions on known-answer rows (see tools/host_sd_test.py), so "the two
 * sides agree on the hash input" is a green line and not a hope. */
static void theme_id(const char *slug, const char *name, char out[THEME_ID_HEX + 1])
{
    uint8_t digest[32];
    char buf[ORB_BUNDLE_SLUG_MAX + 1 + THEME_NAME_MAX];
    const size_t sl = strlen(slug), nl = strlen(name);
    memcpy(buf, slug, sl);
    buf[sl] = '/';
    memcpy(buf + sl + 1, name, nl);
    mbedtls_sha256((const unsigned char *)buf, sl + 1 + nl, digest, /*is224=*/0);
    for (int i = 0; i < THEME_ID_HEX / 2; i++) {
        out[2 * i]     = "0123456789abcdef"[digest[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[digest[i] & 0x0F];
    }
    out[THEME_ID_HEX] = '\0';
}

/* The stem's legible middle, sanitized from the theme name: lowercase ASCII
 * letters and digits, every other byte a separator, runs collapsed to one '-',
 * leading and trailing separators dropped, cut at THEME_TOKEN_MAX so the pair
 * around it always fits ORB_SD_NAME_MAX. A name with nothing legible in it
 * ("Ünïcödé 日本語 🔆" sanitizes to a token, "!!!" does not) lands on "theme":
 * the token decorates the row, and an empty token would double the '-'. This
 * is the one sanitizer; orbbundle.py, orbtheme-baker.js and the hub's Go copy
 * mirror these lines, and host_sd_test drives all four on shared vectors. */
#define THEME_TOKEN_MAX 24
static void theme_name_token(const char *name, char *tok, size_t tokcap)
{
    size_t o = 0;
    bool pending = false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        char c = (char)*p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum) { pending = o > 0; continue; }
        if (pending && o + 1 < tokcap) tok[o++] = '-';
        pending = false;
        if (o + 1 >= tokcap) break;
        tok[o++] = c;
    }
    if (o == 0) { memcpy(tok, "theme", 6); return; }
    if (tok[o - 1] == '-') o--;                    /* truncation may leave one */
    if (o == 0) { memcpy(tok, "theme", 6); return; }
    tok[o] = '\0';
}

static void theme_stem(const char *slug, const char *name,
                       char *out, size_t outsz)
{
    char tok[THEME_TOKEN_MAX + 1], id[THEME_ID_HEX + 1];
    theme_name_token(name, tok, sizeof tok);
    theme_id(slug, name, id);
    snprintf(out, outsz, "%s-%s-%s", slug, tok, id);
}

/* Split a stem into its authoritative parts: the slug before the first '-' and
 * the 12-hex after the last. False when either boundary lies (no '-', a slug
 * outside the manifest rule, a tail that is not 12 lowercase hex). The token
 * between them is deliberately NOT checked — the hand-rename rule above. The
 * split reads the stem's own first '-' so a token holding '-' (the sanitizer
 * emits them) cannot shift the slug: the slug charset has no '-', so the first
 * one ends it. */
static bool theme_stem_split(const char *stem, char *slug, size_t slugsz,
                             char *id, size_t idsz)
{
    if (stem == NULL || stem[0] == '\0') return false;
    const char *d = strchr(stem, '-');
    if (d == NULL || d == stem) return false;
    const size_t sl = (size_t)(d - stem);
    if (sl > ORB_BUNDLE_SLUG_MAX) return false;
    if (orb_bundle_slug_rule((const uint8_t *)stem, (uint32_t)sl) != 0) return false;
    if (sl + 1 > slugsz) return false;
    memcpy(slug, stem, sl);
    slug[sl] = '\0';
    const size_t n = strlen(stem);
    if (n < 1 + THEME_ID_HEX || n - THEME_ID_HEX - 1 < sl + 1) return false;
    /* The separator ahead of the tail fixes its length: a 13-hex tail has no
     * '-' thirteen bytes from the end, and a shorter tail fails the hex loop. */
    if (stem[n - THEME_ID_HEX - 1] != '-') return false;
    if (idsz < THEME_ID_HEX + 1) return false;
    memcpy(id, stem + n - THEME_ID_HEX, THEME_ID_HEX);
    id[THEME_ID_HEX] = '\0';
    for (int i = 0; i < THEME_ID_HEX; i++) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

/* Is `stem` a card filename this device could have issued? The rule is the
 * split: a legal slug, a '-', a tail of exactly THEME_ID_HEX chars of [0-9a-f],
 * and a token between them that at least exists and carries no dot — the dot
 * was FORMAT 2's whole stem, and one inside a token would read as an extension
 * to every tool that splits on it. '/' and ".." fail the same gate, before the
 * card is touched. An id arrives back from the browser, but it never reaches a
 * path builder — orb_theme_resolve_id() finds the file by walking the directory
 * and re-verifies the header against the stem; the id only selects which
 * directory entry to accept. */
/* The one-section rule's sentence, in the two doors that verify a whole
 * container. A theme container that carries several apps' sections is the
 * FORMAT 2 era's habit in FORMAT 3 clothing: it cannot be named by one stem,
 * installed by one app, or evicted by one owner, so every door refuses it. */
static const char *one_section_why(uint32_t n)
{
    static char why[72];
    snprintf(why, sizeof why,
             "a theme file carries exactly one section %s this one carries %lu",
             "—", (unsigned long)n);
    return why;
}

static bool theme_id_ok(const char *stem)
{
    if (stem == NULL || !orb_sd_name_ok(stem)) return false;
    char slug[ORB_BUNDLE_SLUG_MAX + 1], id[THEME_ID_HEX + 1];
    if (!theme_stem_split(stem, slug, sizeof slug, id, sizeof id)) return false;
    const char *first = strchr(stem, '-'), *last = strrchr(stem, '-');
    const size_t tokn = (size_t)(last - first) - 1;
    if (tokn == 0) return false;
    for (const char *p = first + 1; p < last; p++)
        if (*p == '.') return false;
    return true;
}

/* The public form, so the HTTP handlers can reject a malformed id before calling
 * install/delete/post_install instead of letting each re-implement the 12-hex rule.
 * The card layer re-checks it again on the resolve path — one line of defence at the
 * edge, one at the door, and both hold the same truth from this single definition. */
bool orb_theme_id_ok(const char *id) { return theme_id_ok(id); }



/* ------------------------------------------------------------------------- *
 * The resolved geometry the dial draws with.
 * ------------------------------------------------------------------------- */

/* The adopted bundle kept for the apps to resolve against (Migration 2,
 * commit 3). The projection consumes the element table once, and the region
 * used to be freed at that point; the layer table's first consumer must ask the
 * SAME table a second question — which elements does this theme name, in what
 * order, at what plane — and orb_elems.c answers that question only from a
 * parsed section it can walk. So the region now stays resident: PSRAM, at most
 * ORB_BUNDLE_GEOM_CAP (60 KB), published once per adoption and retired when the
 * next adoption replaces it or when the dial falls to the base face. A flip
 * reboots (D11), so exactly one adoption is live at a time, the clock resolves
 * once per boot at init, and nothing here churns under a running draw.
 * s_live_sec's blob pointers address s_live_region — the two are published and
 * retired together. */
static orb_bundle_t             s_live_b;              /* the index, by value  */
static const orb_bundle_sec_t  *s_live_sec = NULL;     /* its clock section    */
static uint8_t                 *s_live_region = NULL;  /* the blobs' home      */

static clk_theme_t   s_theme;                    /* parsed container geometry */
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
/* The stored bundle's CONTAINER region checksum, read off its slot header at
 * boot: the figure the store page matches a card file against, where
 * s_active_geom_crc holds the rebase's. Both name the same bytes. */
static uint32_t      s_bundle_file_geom;
static bool          s_active_valid;
static const char   *s_source = "none";

/* §20.4's `staged` word: the card stem of a store whose pixels reached the slot
 * and never wrote its header, read back out of NVS at boot and kept until a boot
 * finds the commit landed. "" is the clean state. Under the single slot it is only
 * a witness that a store was armed and torn — never a request and never a choice.
 * The stem, not the 12-hex: per-app theming made the hex ambiguous across apps,
 * and the witness must name the row the page can point at. */
static char          s_staged_id[ORB_THEME_HANDLE_MAX + 1];
/* Migration 1's measured fact for that witness: an interrupted store on this
 * single-slot firmware leaves the one slot headerless, so the dial falls to the
 * base digital face and one re-store recovers it. */
#define STAGED_WHY "pixels staged, never committed — the dial shows the base face"

/* D30's boot refill, as the ledger reports it: the milliseconds of the fused
 * read-and-CRC pass and the bytes it moved. Both stay 0 on a boot that mapped the
 * slot in place, so `refill_ms` beside `source:"slot"` is the pair that tells the
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

/* The no-resident-art fallback (§26 P6.5). The compiled FORMAT 2 pair is retired, so
 * there is no baked-in dial to fall back to: a board whose default bundle and installed
 * slot both fail to draw clears the geometry so clock_view's residency gate fails and it
 * paints the base digital face, which still tells the time. s_geo.theme stays NULL and is
 * only ever dereferenced on the themed leg, which the residency gate has already fenced
 * off by the time anything reads it. */
static void use_no_art(void)
{
    /* The apps resolve against nothing: retire the published index with the
     * geometry, so a base-face dial cannot half-draw a bundle it dropped. */
    heap_caps_free(s_live_region);
    s_live_region = NULL;
    s_live_sec = NULL;
    memset(&s_live_b, 0, sizeof s_live_b);
    s_geo.theme      = NULL;
    s_geo.rects      = NULL;
    s_geo.rect_first = NULL;
    s_geo.gif_delays = NULL;
    s_geo.bg_frames  = NULL;
    s_geo.comps      = NULL;
    s_geo.aux        = NULL;
    s_geo.bank       = NULL;
    s_geo.n_comps    = 0;
    s_geo.n_aux      = 0;
    s_geo.n_bank     = 0;
    s_geo.art_size   = 0;
    s_geo.blob_off   = 0;
    s_geo.from_container = false;
    s_geo.name[0] = '\0';
    s_active_name[0] = '\0';
    s_active_valid = false;
    s_source = "none";
}

const orb_theme_geo_t *orb_theme_geo(void) { return &s_geo; }

/* The adopted bundle's app section, or NULL. This is the door Migration 2's
 * commit 3 opens: an app that draws from a theme asks the bundle directly which
 * elements it names — through orb_elems.c, against the section returned here —
 * instead of reading a projection of it back out of the geometry adapter. The
 * section is the ADOPTED one: the same bytes the dial is drawing, proven at the
 * same door. bank_off answers where that section's own art bank begins inside
 * the payload art_bank_ptr() maps — the figure a catalogue offset must be
 * folded with before it can index the payload (see the measurement in
 * tools/host_orblayers_test.py section 14). The base face has no bundle: the
 * answer is NULL and the caller draws what a dial with no theme draws. */
bool orb_theme_live_sec(const char *slug, const orb_bundle_sec_t **sec,
                        uint32_t *bank_off)
{
    if (!s_live_sec || !s_live_region) return false;
    const orb_bundle_sec_t *s = orb_bundle_find(&s_live_b, slug);
    if (!s) return false;
    *sec = s;
    *bank_off = s->art_off - SLOT_ART_OFF;
    return true;
}

const char *orb_theme_source(void)         { return s_source; }
/* The residency verdict the boot ladder reached: an adopted, drawable theme whose
 * geometry is published. Not the same question as art_bank_ready() — that answers
 * only whether a span is mapped, and a mapped span whose clock projection refused
 * is pixels with no dial. clock_view_init gates its themed leg on the two together;
 * §26's fallback is about losing the theme, not about mapping bytes no parse adopted. */
bool orb_theme_resident(void)              { return s_active_valid; }
const char *orb_theme_active_name(void)    { return s_active_name; }

/* §20.4's word and the two refill figures. All four are read-only answers for the
 * status surfaces: nothing here changes which dial the glass draws. */
const char *orb_theme_staged_id(void)      { return s_staged_id; }
const char *orb_theme_staged_why(void)     { return s_staged_id[0] ? STAGED_WHY : ""; }
uint32_t    orb_theme_refill_ms(void)      { return s_refill_ms; }
uint32_t    orb_theme_refill_bytes(void)   { return s_refill_bytes; }
uint32_t    orb_theme_bundle_ms(void)      { return s_bundle_ms; }
uint32_t    orb_theme_bundle_bytes(void)   { return s_bundle_bytes; }

/* How many files the last scan's walk saw carrying the extension, cap and
 * other apps' prefixes included: the "8 shown of 19 on the card" pair. 0
 * before the first scan, and unchanged by a scan that never reached the card. */
static int s_scan_counted;
int orb_theme_scan_counted(void)           { return s_scan_counted; }

/* ---- single-slot boot (Migration 1) -----------------------------------------
 *
 * With one slot at offset 0 there is no "which slot does a boot draw" and no
 * "which slot must an install never hit" to decide: the boot reads offset 0, and an
 * install always writes offset 0. The two predicates host_tz_test.py used to table
 * (boot_resolve, install_target) and the boot_art_t they returned are retired with the
 * A/B pair, so a torn store leaves the one slot headerless and the base face on. */

/* The slot's name for the log — the one slot, so the sentence a store prints cannot
 * disagree with the slot it wrote. */
static const char *slot_name(uint32_t hdr_base)
{
    (void)hdr_base;
    return "slot";
}


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
 * doubled extension reaches the path. Every caller passes either a device-issued
 * compound stem (theme_stem(slug, name)) — which card_open_verified_id proved
 * against the file's own header before it got here — or nothing at all, so this
 * builder never sees a human-supplied string. The directory and the name rule come from the SD layer, so
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

/* Read n bytes at *io_pos. pread, not read: verify_bundle3() and the installer walk
 * one fd at two cursors (header, region, banks), and a shared file offset across those
 * walks is a bug that shows up only on a card whose sector size makes the cursors meet. */
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





static void card_close(int fd)
{
    if (fd >= 0) close(fd);
    orb_sd_unlock();
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
 * Nothing here chooses what to draw. The install writes the one slot, and the caller
 * (bundle_job, off the store press) asks for the reboot that draws it — there is no
 * choice key to leave alone and no second door to press the glass, because Migration 1
 * merged the store and the flip into one press. D26 is honoured on the way — boot says
 * plainly that a bundle in the slot is not drawable yet. */

static esp_err_t install_failed(esp_err_t e, const char *w, char *why, size_t whysz);

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


/* §31: the projection verdict, asked of a parsed bundle before any door commits.
 * One function for the store, the upload and the list, so a row's word, an
 * upload's refusal and the store's sentence are one fact and not three opinions:
 * the answer is whatever boot's adopt would do with the same region on this panel.
 * A bundle with no clock section is refused too — the slot IS the dial under
 * Migration 1, and storing one would silently replace a working face with the
 * base digital readout; that trade is the owner's to make only if it is said so,
 * and saying it here is the saying. The tables are a dedicated set: a door runs
 * while the live theme is being painted from s_b3_*, and a refusal must not
 * rewrite the dial on the glass. The mutex is the same reason in scheduler form —
 * a scan on the httpd worker and a store on the theme worker may ask at once, and
 * neither may see the other's half-written rows. */
static SemaphoreHandle_t s_verdict_mx;
/* 2026-10-24: one PSRAM block behind all six scratch tables, allocated at
 * orb_theme_boot beside the lock that guards them. As .bss these ~3.5 kB sat
 * on the heap boundary inside the single contiguous block httpd_start needs
 * (ESP_ERR_HTTPD_TASK boot line: hole 14848, task-time 7680, need 8192). The
 * verdict pass is a parse — no DMA, no crash path (it answers a door, not a
 * boot) — so PSRAM costs this file's own precedent, clock3_verdict3's region
 * buffer, the same bus. NULL (alloc refused at boot) reads like the missing
 * lock one line below: named refusal, dial unaffected. */
typedef struct {
    clk_theme_t   theme;
    clk_bg_rect_t rects[ORB_CLOCK3_N_RECTS];
    uint16_t      first[GIFS_MAX + 1], delays[GIFS_MAX];
    uint32_t      bgf[ORB_CLOCK3_N_BGF];
    clk_aux_t     aux[CLK_AUX_MAX];
    clk_comp_t    comps[CLK_COMP_MAX];
} chk_scratch_t;
static chk_scratch_t *s_chk;

static const char *clock3_verdict(const orb_bundle_t *b)
{
    const orb_bundle_sec_t *sec = orb_bundle_find(b, ORB_CLOCK3_SLUG);
    if (!sec)
        return "the bundle carries no clock section to draw";
    if (!s_verdict_mx || !s_chk) return "out of memory for the verdict lock";
    xSemaphoreTake(s_verdict_mx, portMAX_DELAY);
    orb_clock3_io_t pio;
    memset(&pio, 0, sizeof pio);
    pio.theme = &s_chk->theme;
    pio.rects = s_chk->rects; pio.rect_first = s_chk->first;
    pio.gif_delays = s_chk->delays; pio.bg_frames = s_chk->bgf;
    pio.first_cap = GIFS_MAX + 1; pio.delays_cap = GIFS_MAX;
    pio.comps = s_chk->comps; pio.comp_cap = CLK_COMP_MAX;
    pio.aux   = s_chk->aux;   pio.aux_cap   = CLK_AUX_MAX;
    const char *r = orb_clock3_parse(sec, &pio);
    xSemaphoreGive(s_verdict_mx);
    return r;
}

/* The verdict straight off a card file's own bytes: the region read is the whole
 * ask — orb_bundle_parse_region checks the index, every bank record and the region
 * checksum, and the projection needs no pixels. verify_bundle3() streams the art
 * for that file's integrity; this asks only whether the clock inside would draw.
 * The per-app stem needs what the region OWNS, so the parsed index's first slug
 * and its section count come back out too when the caller asks: record 0 is the
 * only section a theme container may carry, and the door that lists or files the
 * file must be able to say which app that section names. */
static const char *clock3_verdict3(int fd, const orb_bundle3_hdr_t *h, uint32_t file_size,
                                   char *slug_out, uint32_t *nsec_out)
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
    if (!r) {
        if (slug_out)
            strlcpy(slug_out, b.sec[0].slug, ORB_BUNDLE_SLUG_MAX + 1);
        if (nsec_out) *nsec_out = b.n_sections;
        r = clock3_verdict(&b);
    }
    heap_caps_free(region);
    return r;
}

/* FORMAT 3's container check at the door, and the only one now that FORMAT 2 is
 * retired: the region alone verified through the shipped reader (bounds,
 * region checksum, index, walk, blob lengths/crcs/slugs, art bounds,
 * sortedness), then EACH BANK streamed and settled against its record's own
 * crc with orb_bundle_bank_rule() — the rule bank_check() applies in the one
 * pass, in the one order. A whole-window art crc is NOT that rule: the writer
 * computes the header's art figure over the banks concatenated WITHOUT the
 * sector-alignment pads the file puts between them (write_bundle's own line),
 * so a multi-bundle door that checksums art_off..art_size hashes padding it
 * should not see and misses banks it should. The pads and the header's sum
 * figure belong to the install's readback, which settles them as the slot's
 * numbers (P6.3's copy path); at the door the bank records are the contract.
 * The region buffer is install_bundle()'s size and cap: geom_size is already
 * clamped to ORB_BUNDLE_GEOM_CAP by the header gate before it reaches this
 * malloc. */
static const char *verify_bundle3(int fd, const orb_bundle3_hdr_t *h, uint32_t file_size,
                                  char *slug_out, uint32_t *nsec_out)
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
    /* §31: the upload door asks the projection while the region is already in
     * hand. A clock this panel refuses never reaches the card, let alone a row. */
    if (!r) {
        if (slug_out)
            strlcpy(slug_out, b.sec[0].slug, ORB_BUNDLE_SLUG_MAX + 1);
        if (nsec_out) *nsec_out = b.n_sections;
        r = clock3_verdict(&b);
    }
    heap_caps_free(region);
    if (r) return r;
    if (ensure_chunk() != ESP_OK) return "out of memory for the card read buffer";
    for (uint32_t i = 0; i < b.n_sections; i++) {
        const orb_bundle_sec_t *s = &b.sec[i];
        if (!s->art_size) continue;                       /* a section with no art */
        uint32_t crc = 0;
        pos = s->art_off;
        uint32_t left = s->art_size;
        while (left) {
            const size_t want = left > s_chunk_bytes ? s_chunk_bytes : left;
            if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want)
                return "pixel data is shorter than its header claims — re-copy it";
            crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
            left -= (uint32_t)want;
        }
        r = orb_bundle_bank_rule(s, s->art_size, crc);
        if (r) return r;
    }
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
    const char *why;          /* resolve_t's twin (P6.4): a FORMAT 2 file at this
                               * door is refused by the reader's format line, and
                               * that is the truth the row must carry — not R4's
                               * identity claim about a file that names itself well */
} resolve3_t;

static bool resolve3_entry(void *arg, const orb_file_rec_t *rec, int fd)
{
    resolve3_t *r = arg;
    if (strcmp(rec->name, r->want) != 0) return false;
    r->found = true;
    char slug[ORB_BUNDLE_SLUG_MAX + 1], id[THEME_ID_HEX + 1];
    if (!theme_stem_split(r->want, slug, sizeof slug, id, sizeof id)) return false;
    orb_bundle3_hdr_t h;
    uint32_t fsz = 0;
    const char *cr = card_check3(fd, &h, &fsz);
    if (cr) { r->why = cr; return false; }
    char want[THEME_ID_HEX + 1];
    theme_id(slug, h.name, want);
    r->verified = (strcmp(want, id) == 0);
    return r->verified;
}

static int card_open_verified_id3(const char *stem, orb_bundle3_hdr_t *o, uint32_t *file_size,
                                  const char **why)
{
    *why = NULL;
    char slug[ORB_BUNDLE_SLUG_MAX + 1], id[THEME_ID_HEX + 1];
    if (!theme_id_ok(stem) ||
        !theme_stem_split(stem, slug, sizeof slug, id, sizeof id)) {
        *why = "that theme id is not one this device issued";
        return -1;
    }
    const orb_file_src_t *src = orb_files_src("clock", "themes");
    if (!src) { *why = "no \"themes\" file source declared"; return -1; }
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = "no card in the socket"; return -1; }

    resolve3_t r = { .want = stem, .found = false, .verified = false };
    if (orb_files_walk(src, resolve3_entry, &r, NULL) < 0) { *why = "no card in the socket"; return -1; }
    if (!r.found)    { *why = "no such theme on the card";                        return -1; }
    if (!r.verified && r.why) { *why = r.why;                                     return -1; }
    if (!r.verified) { *why = "that file's name does not match its id — refused"; return -1; }

    const int fd = card_open_locked3(stem, o, file_size, why);
    if (fd < 0) return -1;
    char id2[THEME_ID_HEX + 1];
    theme_id(slug, o->name, id2);
    if (strcmp(id2, id) != 0) {
        card_close(fd);
        *why = "that file's name does not match its id — refused";
        return -1;
    }
    return fd;
}

/* ---- the store's ordering, C3 (§5.3 + §6.3) ---------------------------------
 *
 * Two rules come with the theme's move to mechanism A, and both are ordering,
 * not checks: the pages are billed before the erase, and the erase must not
 * happen under a live map. The store door therefore runs: verify, project,
 * bill, park, unmap, erase, write, reboot. Nothing after the unmap can resume
 * the dial — the copy erases the displayed theme's own pixels first thing — so
 * every path past it ends in the reboot, torn store included, and the staged
 * witness names the tear on the next boot.
 *
 * The park is task-safe by construction and touches no LVGL call: the worker
 * may not call lv_timer_pause outside the LVGL task, so it drops the tick's
 * running gate (the plain bool the tick and the commit hook already read
 * verbatim) and hands the glass to the home row, whose activation hides the
 * dial — and a hidden screen is a screen LVGL never re-renders. The wait
 * outlives any tick already in flight. */
static volatile bool s_slot_torn;        /* set once the unmap is behind us */

/* ---- the upload's own park (the copy-to-card frames on the glass) ---------
 *
 * The store parks before the slot copy (store_park_dial), but the upload that
 * precedes a store is the longer wait — a few megabytes over WiFi — and it used
 * to be the silent stretch the owner watched the clock freeze on with no word
 * about why. Same discipline as the store's park, reached through the same
 * task-safe halves: stop the tick gate, hand the glass to the home row so the
 * compositor never repaints over the overlay, black the panel, and name the
 * stage. The httpd task owns the glass for exactly as long as the body crosses.
 * An unpark puts the dial back whole: the slot may stand armed invalid, but the
 * mapped theme is still there, and the owner is owed the face they had. */
static const orb_app_t *s_upload_was;

bool orb_theme_upload_park(uint32_t total)
{
    s_upload_was = input_router_current();
    orb_sched_set_running(false);
    const orb_app_t *home = orb_app_home();
    if (home) input_router_request(home);
    vTaskDelay(pdMS_TO_TICKS(120));       /* one router poll, doubled — as in store_park_dial */
    if (orb_sched_running()) {
        if (s_upload_was) input_router_request(s_upload_was);
        return false;
    }
    orb_boot_overlay_backdrop();
    orb_boot_overlay("copying", 0, total, 0);
    return true;
}

void orb_theme_upload_progress(uint32_t done, uint32_t total)
{
    orb_boot_overlay("copying", done, total, 0);   /* the overlay's 10 Hz rule applies */
}

void orb_theme_upload_unpark(void)
{
    orb_sched_set_running(true);
    if (s_upload_was) input_router_request(s_upload_was);
    s_upload_was = NULL;
}

/* The store parks the glass on the menu for its own reasons (§6.3), and that
 * switch is what a naive resume record would read as "the owner was on the
 * menu". The boot after a store is therefore steered at the clock explicitly
 * (bundle_job); this is where the wrong answer would have been written. */
static bool store_park_dial(const orb_app_t *was, uint32_t total)
{
    orb_sched_set_running(false);
    const orb_app_t *home = orb_app_home();
    if (home) input_router_request(home);
    /* 120 ms: the router's poll period doubled, so the switch to the menu has
     * landed and no tick was mid-frame when the gate dropped. The overlay paints
     * AFTER the dial is hidden, or the switch would paint the menu over it. */
    vTaskDelay(pdMS_TO_TICKS(120));
    if (orb_sched_running()) {
        /* A resume raced the request (the bounce stands on its own gate). The
         * erase cannot be trusted against a compositor that reads the map, so
         * refuse while everything is still whole — and put the owner's screen
         * back exactly as they left it. */
        if (was) input_router_request(was);
        return false;
    }
    orb_boot_overlay_backdrop();
    orb_boot_overlay("storing", 0, total, 0);
    return true;
}

/* One chunk boundary of the copy (§6.3): the glass says what the row and the
 * ledger say, and the overlay's own 10 Hz throttle keeps the panel from
 * rate-limiting the flash write. */
static void store_progress(void *arg, uint32_t done, uint32_t total)
{
    (void)arg;
    orb_boot_overlay("storing", done, total, 0);
}

bool orb_theme_store_torn(void)
{
    return s_slot_torn;
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
                              const char **why,
                              void (*progress)(void *arg, uint32_t done, uint32_t total),
                              void *parg)
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

    /* §6.3: the erases lead the progress frames because they lead the point of
     * no resume — until the first byte goes down the old theme still exists on
     * the card and in nothing else. */
    esp_err_t e = esp_partition_erase_range(p, hdr_base + SLOT_GEOM_OFF, geom_len);
    if (e == ESP_OK) e = esp_partition_erase_range(p, hdr_base + SLOT_ART_OFF, art_len);
    if (e != ESP_OK) { *why = "flash erase failed"; return e; }
    if (progress) progress(parg, 0, art_len);

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
            if (progress) progress(parg, s->art_off - SLOT_ART_OFF + done, art_len);
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
                            uint32_t art_crc, uint32_t serial,
                            uint32_t file_geom_crc)
{
    memset(hdr, 0, W_WORDS * 4);
    hdr[0] = THEME_MAGIC;     hdr[1] = ORB_BUNDLE3_FORMAT;
    /* C4: the slot carries the STORED bundle's own dialect stamp, not the
     * constant this build happens to write into new containers. A v4 theme
     * installs a v4 slot: a reader that cannot read it must refuse it at the
     * same header gate every other route runs, not boot a dial it decoded
     * with defaulted columns. */
    hdr[2] = h->geom_abi;     hdr[3] = W_WORDS * 4;
    hdr[4] = SLOT_GEOM_OFF;   hdr[5] = h->geom_size;  hdr[6] = h->geom_crc;
    hdr[7] = h->art_size;     hdr[8] = art_crc;
    hdr[9] = h->n_sections;   hdr[11] = serial;       hdr[13] = SLOT_ART_OFF;
    /* Word 10 (FORMAT 2's bake duration, always 0 for a bundle) carries the CARD
     * file's region checksum. The slot's own geom_crc is the rebase's figure and
     * cannot be made to equal a file's, and the store page's active/changed question
     * is asked of the file — so the answer has to survive the rebase. */
    hdr[10] = file_geom_crc;
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

/* The store door's verdict-only projection lives in chk_scratch_t above: one
 * PSRAM block per boot, allocated at orb_theme_boot, guarded by s_verdict_mx.
 * A store runs while a live theme is being painted from s_b3_*, and the
 * verdict's rows must never collide with the live theme's — the separate
 * scratch and the mutex carry that rule now (2026-10-24: this comment's
 * six .bss arrays moved with the first set above; the pair that sat here was
 * a duplicate of the 766 block, unreferenced, and the compile settled it). */


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

    /* The container's own region checksum, kept for the header word the row's
     * active/changed comparison reads: the rebase below restamps h.geom_crc to the
     * slot's figure, and the card file keeps the one it shipped with. */
    const uint32_t file_geom_crc = h.geom_crc;

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

    /* §31: the store answers with the projection boot will run, in the same words,
     * before it destroys anything. Bronze's b3 loop was this seam missing: the door
     * verified the container, wrote the one slot, and the next boot's
     * orb_clock3_parse refused the clock section for the panel — themeless dial,
     * and on v1.1.0 a panic loop. A clock that will not draw from this region on
     * this panel will not draw at boot either: the inputs are identical. A refusal
     * here costs the card read and leaves the working slot whole. C3 keeps the
     * verdict where it leads everything: the page bill below, the park, the unmap,
     * and the erase all speak only after it answered yes. */
    {
        const char *cv = clock3_verdict(&b);
        if (cv) {
            heap_caps_free(region); card_close(fd);
            return install_failed(ESP_FAIL, cv, why, whysz);
        }
    }

    /* C3, §5.3 rule 2: pages are billed at install, and the billing leads the
     * erase. The span to book is the span boot will map: the art region rounded
     * to sectors, exactly what copy_bundle3 erases and adopt maps. Refusing here
     * costs the card read and leaves the slot whole — the budget this check
     * guards is shared with every app bank that reserved before it, and a store
     * that ignored it would spend pages another app was promised. The sentence
     * names both figures, because "install failed" without a count is the same
     * dead button §24.8 buried at the identity door. */
    {
        uint32_t art_end = SLOT_ART_OFF;
        for (uint32_t i = 0; i < b.n_sections; i++) {
            const uint32_t end = b.sec[i].art_off + b.sec[i].art_size;
            if (end > art_end) art_end = end;
        }
        const uint32_t art_span = (art_end - SLOT_ART_OFF + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);
        const size_t need = art_bank_pages_for(art_span);
        const size_t free_pages = art_bank_pages_free();
        const esp_err_t re = art_bank_reserve_span(art_span);
        if (re != ESP_OK) {
            char sent[72];
            snprintf(sent, sizeof sent,
                     "this theme needs %u MMU pages and this board has %u free",
                     (unsigned)need, (unsigned)free_pages);
            heap_caps_free(region); card_close(fd);
            return install_failed(re, sent, why, whysz);
        }
    }

    /* C3, §5.3 rule 1: the erase must not happen under a live map. The dial
     * parks first, the map comes down second, and from here on there is no
     * path back to the old theme — its pixels are about to be erased — so every
     * remaining outcome, success or failure, ends in the reboot. If the park
     * itself will not land, refuse while the bank is still resident. */
    s_slot_torn = false;
    const orb_app_t *was = input_router_current();
    if (!store_park_dial(was, h.art_size)) {
        orb_sched_set_running(true);       /* the tick stands; the dial never stopped */
        heap_caps_free(region); card_close(fd);
        return install_failed(ESP_ERR_INVALID_STATE, "the dial would not park", why, whysz);
    }
    art_bank_unmap();
    s_slot_torn = true;                    /* from here the reboot is the only way back */

    /* One target for the whole store: the single slot at offset 0. Migration 1 retired
     * the ping-pong, so the target is a constant and the serial probe, the copy, the
     * header erase and the header write all aim at it.
     *
     * The serial counts up over whatever the slot already held; a header that cannot be
     * read restarts the count at 1. A torn store leaves it headerless, so the next store
     * that lands re-bases from 1 — the count is a staleness guard, not a version line. */
    const uint32_t tgt = SLOT_HDR;
    uint32_t raw[W_WORDS];
    uint32_t serial = 1;
    if (esp_partition_read(p, tgt, raw, sizeof raw) == ESP_OK) {
        orb_bundle3_hdr_t o3;
        if (orb_bundle3_hdr_parse(raw, &o3) == NULL)   serial = o3.serial + 1;
    }

    staged_mark(id);
    uint32_t art_crc = 0;
    esp_err_t e = copy_bundle3(fd, &b, region, file_art_base, p, tgt,
                               &art_crc, &w, store_progress, NULL);
    card_close(fd);
    heap_caps_free(region);
    if (e != ESP_OK) {
        staged_clear();
        /* Past the unmap there is no dial to preserve: the erase already took
         * the old theme's pixels, so a failure here is a reboot into the base
         * face with the sentence on the glass and the row, not a resume. */
        if (s_slot_torn) {
            orb_boot_overlay(w ? w : "store failed", 0, 0, 0);
            if (orb_jobs_reboot_soon("theme: the store failed with the map down") != ESP_OK)
                ESP_LOGE(TAG, "torn store but the reboot could not be armed");
        }
        return install_failed(e, w, why, whysz);
    }

    const esp_err_t ee = esp_partition_erase_range(p, tgt, THEME_SECTOR);
    if (ee == ESP_OK) {
        uint32_t hdr[W_WORDS];
        build_slot_hdr3(hdr, &h, art_crc, serial, file_geom_crc);
        e = esp_partition_write(p, tgt, hdr, sizeof(hdr));
        if (e != ESP_OK) w = "could not make the new slot live";
    } else {
        e = ee;
        w = "could not clear the old slot header";
    }
    if (e != ESP_OK) {
        staged_clear();
        if (s_slot_torn) {
            orb_boot_overlay(w, 0, 0, 0);
            if (orb_jobs_reboot_soon("theme: the store failed with the map down") != ESP_OK)
                ESP_LOGE(TAG, "torn store but the reboot could not be armed");
        }
        return install_failed(e, w, why, whysz);
    }
    staged_clear();
    s_bundle_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_bundle_bytes = h.art_size;
    orb_boot_overlay("stored, rebooting", h.art_size, h.art_size, 0);
    ESP_LOGI(TAG, "bundle \"%s\" (%s) in %s in %u ms — stored, drawn on the next boot",
             h.name, id, slot_name(tgt), (unsigned)s_bundle_ms);
    return ESP_OK;
}

/* The FORMAT 3 twin of adopt() (P6.5 §25.5(2)): the same two publications — pixels
 * resident and checked, geometry the dial can draw — reached through the bundle's
 * own doors. Three differences, and each is the format's, not a lapse:
 *
 *   The region is read before the pixels are filled, because a bundle's art lives at
 *   addresses only its records name. FORMAT 2's payload starts at the region's first
 *   byte and its length is in the header; here both come out of the table, so the
 *   table has to be parsed first. The region is at most ORB_BUNDLE_GEOM_CAP, and the
 *   header gate already refused a bigger claim before this malloc.
 *
 *   The refill verifies against the INSTALLED figure in the slot header, which is the
 *   checksum of the padded art span as it now sits in flash (copy_bundle3's readback),
 *   not the container's bank sum. That is deliberate and it is the same rule FORMAT 2
 *   follows: the boot that verifies and the boot that draws share one pass over the
 *   bytes that are actually resident, and the pads between banks are part of what the
 *   slot holds. §24.9's door-side bug was hashing pads at a door that had no span;
 *   this is a span, hashed whole, once.
 *
 *   The row's identity keeps the CONTAINER's region checksum, in the header word
 *   FORMAT 2 spent on a bake duration. orb_bundle_rebase() restamps the region's own
 *   figure when it moves banks, so the slot's geom_crc can never equal the card
 *   file's, and a comparison built on it would call every stored bundle "changed"
 *   forever. Word 10 carries what the file said instead; the damaged-vs-active
 *   question is then the same one FORMAT 2 answers.
 *
 * Returns ESP_OK with the bundle drawn, or the reason it is not. A refusal leaves the
 * caller's fallback exactly where adopt()'s does: the default draws, and the row on
 * the store page carries the sentence. */
static uint16_t  s_b3_first[GIFS_MAX + 1], s_b3_delays[GIFS_MAX];
static clk_bg_rect_t s_b3_rects[ORB_CLOCK3_N_RECTS];
static uint32_t  s_b3_bgf[ORB_CLOCK3_N_BGF];
static clk_aux_t  s_b3_aux[CLK_AUX_MAX];      /* D34: the FORMAT 3 dial's pointer rows,
                                               * the table a bundle's AUX elements project
                                               * into. Static for the same reason as the
                                               * comp table beside it: the projection
                                               * allocates nothing, and clock_view's aux
                                               * pass reads the rows for the life of the
                                               * theme. Sized to CLK_AUX_MAX because that
                                               * is the cap the projection refuses past. */
static clk_comp_t s_b3_comps[CLK_COMP_MAX];   /* D32 slice 2: the FORMAT 3 dial's
                                               * complications, filled by the
                                               * projection and read by clock_view
                                               * through s_geo below. Static by the
                                               * same rule as the four gif tables:
                                               * the projection allocates nothing,
                                               * and a dial's tables outlive its
                                               * themes. */

static esp_err_t adopt_bundle3(const esp_partition_t *p, const orb_bundle3_hdr_t *h,
                               const uint32_t *raw, const char *src_name,
                               uint32_t slot_hdr_base)
{
    uint8_t *region = heap_caps_malloc(h->geom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) return ESP_ERR_NO_MEM;
    if (esp_partition_read(p, slot_hdr_base + SLOT_GEOM_OFF, region, h->geom_size) != ESP_OK) {
        heap_caps_free(region);
        return ESP_FAIL;
    }
    orb_bundle_t b;
    /* SLOT_CAP as the file size: inside a slot the banks are bounded by the slot, and
     * the reader's art checks want the span they live in rather than a card's length.
     * Every bank was proved against its own record's crc at the install door and
     * written back-from-flash in the same pass; boot proves them again here as the
     * refill hashes the span they sit in. */
    const char *why = orb_bundle_parse_region(h, region, h->geom_size, SLOT_CAP, &b);
    if (why) {
        ESP_LOGE(TAG, "bundle in %s would not parse: %s", src_name, why);
        heap_caps_free(region);
        return ESP_ERR_INVALID_CRC;
    }
    const orb_bundle_sec_t *sec = orb_bundle_find(&b, ORB_CLOCK3_SLUG);
    if (!sec) {
        /* Not damage — a bundle that carries other apps and no clock. The row's
         * sentence says which, because "the file I stored is not on the wall" without
         * a reason is what an owner reads as a broken button. */
        ESP_LOGI(TAG, "bundle in %s carries no %s section", src_name, ORB_CLOCK3_SLUG);
        heap_caps_free(region);
        return ESP_ERR_NOT_FOUND;
    }

    /* The projection runs BEFORE a byte of pixels is brought resident. The order is
     * the lesson of Bronze's b3 panic loop (2026-10-22): when map preceded parse, a
     * clock section this panel refuses left the pixels resident with the geo zeroed,
     * art_bank_ready() answered true over bytes that are not a bank, and the boot
     * log's honest "base digital face" sentence and the themed leg's residency test
     * disagreed about what the glass was about to read. The store door projects the
     * same way now, so a refusal never maps at either door and the two answers can
     * not diverge. Pure by contract (orb_clock3.h), so the region is all it needs. */
    orb_clock3_io_t io;
    memset(&io, 0, sizeof io);
    io.theme = &s_theme;
    io.rects = s_b3_rects; io.rect_first = s_b3_first;
    io.gif_delays = s_b3_delays; io.bg_frames = s_b3_bgf;
    io.first_cap = GIFS_MAX + 1; io.delays_cap = GIFS_MAX;
    io.comps = s_b3_comps; io.comp_cap = CLK_COMP_MAX;
    io.aux   = s_b3_aux;   io.aux_cap   = CLK_AUX_MAX;   /* D34 */
    why = orb_clock3_parse(sec, &io);
    if (why) {
        heap_caps_free(region);
        ESP_LOGE(TAG, "bundle \"%s\" has no drawable clock: %s", h->name, why);
        return ESP_FAIL;
    }

    /* The span to make resident: the installed art region runs to the end of the LAST
     * bank, sector-rounded, which is what copy_bundle3 erased, wrote and read back. */
    uint32_t end = SLOT_ART_OFF;
    for (uint32_t i = 0; i < b.n_sections; i++) {
        const uint32_t e2 = b.sec[i].art_off + b.sec[i].art_size;
        if (e2 > end) end = e2;
    }
    const uint32_t span = (end - SLOT_ART_OFF + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);

    /* C3 (§5.3): the theme's residency is mechanism A at the call, whatever
     * ORB_ART_MECH_B says. mmap is not content-verified here — the bytes were
     * written whole at the last store's readback-verified copy, the header's
     * geom checksum pins the tables, and a bank that no longer matches its
     * record's provenance is a torn store, which the staged witness already
     * names on this same boot. The refill figures stay 0 beside a slot source:
     * that pair is the page's proof a boot mapped rather than copied (§20.5(2)
     * reads through them, and they now read 0 by design, not by failure). */
    const esp_err_t fe = art_bank_map(slot_hdr_base + SLOT_ART_OFF, span);
    if (fe != ESP_OK) {
        heap_caps_free(region);              /* the parse's copy dies with the map */
        return fe;
    }
    /* Publish the parsed index for the apps to resolve against (see the block
     * above s_live_b): the projection consumed the element table once, and
     * Migration 2's commit 3 makes the first consumer that asks it again, so the
     * region now survives the parse instead of dying at it. It goes up only once
     * the pixels are resident, so no door can answer a section whose bank the
     * dial cannot draw. Retire the previous adoption AFTER the new one is proved
     * -- a failed load leaves the dial drawing what it was drawing, pointers and
     * all. */
    heap_caps_free(s_live_region);
    s_live_region = region;
    s_live_b = b;
    s_live_sec = orb_bundle_find(&s_live_b, ORB_CLOCK3_SLUG);   /* re-found in the copy */

    s_geo.theme = &s_theme;
    s_geo.rects = s_b3_rects; s_geo.rect_first = s_b3_first;
    s_geo.gif_delays = s_b3_delays; s_geo.bg_frames = s_b3_bgf;
    /* The projection refused anything it could not draw whole before this line
     * was reachable (D32 R-F), so a count of zero here means the table carries
     * no bank row, not that a row was dropped. */
    s_geo.comps = io.out_n_comps ? s_b3_comps : NULL;
    /* D34: the pointers a bundle carried ride the same static table the projection
     * filled. A count of zero names a dial with no pointer rows, not a row that was
     * dropped — the projection refuses the whole draw past its own guards (D32 R-F),
     * so nothing can be partly drawn here. */
    s_geo.aux = io.out_n_aux ? s_b3_aux : NULL;
    s_geo.bank = NULL;
    s_geo.n_comps = (uint16_t)io.out_n_comps;
    s_geo.n_aux   = (uint16_t)io.out_n_aux;
    s_geo.n_bank = 0;
    s_geo.art_size = io.out_art_size;
    s_geo.blob_off = io.out_art_off - SLOT_ART_OFF;
    s_geo.from_container = true;
    /* The §16 line the owner reads after every install: what the projection
     * actually put on the dial, from the table the bundle carried. A carriage
     * reads as its frame count, a bank row as a comp; both zero is the plain
     * dial, which is every bundle before D32 and every refusal's alternative. */
    ESP_LOGI(TAG, "bundle \"%s\" projected into a dial: %u complication row%s, "
             "%u pointer row%s, tourbillon_n %u @ %u fps start %u",
             h->name, (unsigned)io.out_n_comps, io.out_n_comps == 1 ? "" : "s",
             (unsigned)io.out_n_aux, io.out_n_aux == 1 ? "" : "s",
             (unsigned)s_theme.tourbillon_n, (unsigned)s_theme.tourbillon_fps,
             (unsigned)s_theme.tourbillon_start);
    if (s_theme.bg_frames)                      /* §31: an animated plate says so */
        ESP_LOGI(TAG, "plate bank: %u keyframes @ %u fps, %s",
                 (unsigned)s_theme.bg_frames + 1u, (unsigned)s_theme.bg_fps,
                 s_theme.bg_loop ? "loop" : "one-shot");

    strlcpy(s_geo.name, h->name, sizeof(s_geo.name));
    strlcpy(s_active_name, h->name, sizeof(s_active_name));
    s_active_geom_crc = s_bundle_file_geom = raw[W_BAKED_MS];
    /* FORMAT 3's identity is that ONE figure. The region holds every index record and
     * and every record carries its bank's checksum, so the container's region checksum
     * already pins the pixels. Hashing art again would add no coverage and cost a
     * figure the slot cannot state honestly: word 8 is the INSTALLED span's checksum,
     * pads included, and no card file has ever carried that number. Both halves get
     * the same word, so the row's art term is identically false and the geom term
     * alone answers "is this the bundle on the wall". */
    s_active_art_crc  = s_active_geom_crc;
    s_active_valid    = true;
    s_source = src_name;
    s_refill_ms = 0;                      /* a boot that mapped refilled nothing (§20.5(2)) */
    s_refill_bytes = span;
    return ESP_OK;
}


/* The single slot's bundle (Migration 1): the header at offset 0, read through the
 * bundle door. If it is a FORMAT 3 bundle with a clock section it is adopted MAPPED
 * — mechanism A at the call, C3's rule, the only mechanism the theme has any more.
 * A header that is not a FORMAT 3 bundle — erased, a torn store, or a FORMAT 2 blob
 * — returns non-OK and the caller falls to the base face. This is the only read that
 * turns the slot's pixels into a dial; it installs and erases nothing, so the
 * no-write-in-the-fallback rule that outlived the A/B pair holds. */
static esp_err_t adopt_default_bundle(const esp_partition_t *p)
{
    uint32_t raw[W_WORDS];
    if (esp_partition_read(p, SLOT_HDR, raw, sizeof raw) != ESP_OK)
        return ESP_FAIL;
    if (raw[W_MAGIC] != THEME_MAGIC || raw[W_FORMAT] != ORB_BUNDLE3_FORMAT)
        return ESP_ERR_NOT_FOUND;           /* not a bundle: erased, or a FORMAT 2 blob */
    orb_bundle3_hdr_t h;
    if (orb_bundle3_hdr_parse(raw, &h) != NULL)
        return ESP_ERR_INVALID_CRC;         /* claims 3, header will not check */
    return adopt_bundle3(p, &h, raw, "slot", SLOT_HDR);
}

/* One slot's header, read and classified. Returns whether the slot holds a parseable
 * FORMAT 3 bundle, and sets *claims3 for the in-between state: magic and format say
 * bundle, the header checksum says torn. Boot and the scan probe the ONE slot at
 * offset 0 with it; the `staged` witness expires only against this header, the header
 * the store actually wrote. */
static bool slot_probe3(const esp_partition_t *p, uint32_t base, orb_bundle3_hdr_t *b3,
                        uint32_t raw[W_WORDS], bool *claims3)
{
    *claims3 = false;
    if (esp_partition_read(p, base, raw, W_WORDS * 4) != ESP_OK) return false;
    if (raw[W_MAGIC] != THEME_MAGIC || raw[W_FORMAT] != ORB_BUNDLE3_FORMAT) return false;
    *claims3 = true;
    return orb_bundle3_hdr_parse(raw, b3) == NULL;
}

/* Resolve the owner's choice and verify the slot it names. Called from app_main
 * BEFORE display_init(), so nothing has mapped the bank yet — the pixels boot
 * verifies are the ones the paint path is about to read.
 *
 * This function no longer installs anything (D13). It used to open the card, copy
 * a container and keep a retry counter, which made every boot depend on a card
 * that may have been pulled and made a half-finished copy a state a device could
 * be left in overnight. A store now happens in orb_theme_install_bundle(), on the
 * worker, at the moment the owner picks. Boot draws what is already verified or
 * draws the default; those are the only two outcomes. */
esp_err_t orb_theme_boot(void)
{
    const esp_partition_t *p = art_bank_partition();
    if (!p) {
        ESP_LOGE(TAG, "no \"%s\" partition — theme storage is off", ART_BANK_PARTITION_NAME);
        use_no_art();
        return ESP_ERR_NOT_FOUND;
    }

    /* Migration 1: one slot, one theme, no choice key and no A/B probe. The `staged`
     * witness expires on the boot that finds the header the torn store waited for — the
     * one header at offset 0. At most one NVS write here, and the word it removes is
     * diagnostics, so the no-write-in-the-fallback rule holds. */
    s_staged_id[0] = '\0';
    /* The §31 verdict lock exists before any door can post: both door tasks are
     * created after app_main calls this, so this one creation needs no guard. */
    s_verdict_mx = xSemaphoreCreateMutex();
    if (!s_verdict_mx)
        ESP_LOGE(TAG, "the projection verdict lock refused — stores and listings "
                      "will name that and refuse");
    if (!s_chk)
        s_chk = heap_caps_malloc(sizeof *s_chk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_chk)
        ESP_LOGE(TAG, "the projection verdict scratch refused — stores and "
                      "listings will name that and refuse");

    orb_bundle3_hdr_t b3;
    uint32_t raw3[W_WORDS];
    bool claims3 = false;
    const bool slot_is_bundle = slot_probe3(p, SLOT_HDR, &b3, raw3, &claims3);

    /* The witness is the card STEM the store was asked for. Its slug is the one
     * the install hashed with, so the landing test re-derives the id from the
     * slot's own name under that slug: the header that arrives says the store
     * committed, and a stale witness dies here either way. */
    char staged[ORB_THEME_HANDLE_MAX + 1];
    if (nvs_get(THEME_NVS_STAGED, staged, sizeof staged) && theme_id_ok(staged)) {
        char wslug[ORB_BUNDLE_SLUG_MAX + 1], wid[ORB_THEME_ID_HEX + 1];
        bool landed = false;
        if (slot_is_bundle &&
            theme_stem_split(staged, wslug, sizeof wslug, wid, sizeof wid)) {
            char cid[ORB_THEME_ID_HEX + 1];
            theme_id(wslug, b3.name, cid);
            landed = (strcmp(cid, wid) == 0);
        }
        if (landed) staged_clear();
        else        strlcpy(s_staged_id, staged, sizeof s_staged_id);
    }

    /* The one slot IS the theme. A valid, drawable bundle is adopted and drawn;
     * anything else — a torn header, a claim of FORMAT 3 that will not parse, or a
     * bundle with no clock section — is the base digital face, with the reason on the
     * serial log for whoever reflashes. There is no second slot to reach across to and
     * no factory copy to fall back on: a torn store answers one re-store. */
    const esp_err_t de = adopt_default_bundle(p);
    if (de == ESP_OK) {
        /* The C3 pass-line line: a stored theme boots MAPPED, with the page count
         * the §9 budget speaks of, and art_bank's refill line is absent. */
        ESP_LOGI(TAG, "slot holds bundle \"%s\": mapped, %u B / %u MMU pages resident",
                 s_active_name, (unsigned)s_refill_bytes,
                 (unsigned)art_bank_mmu_pages());
        return ESP_OK;
    }
    if (claims3 && de == ESP_ERR_INVALID_CRC)
        ESP_LOGE(TAG, "the slot claims FORMAT 3 and its header would not parse — "
                      "re-store the theme");
    else if (de == ESP_ERR_NOT_FOUND)
        ESP_LOGI(TAG, "the slot is empty — base digital face");
    else
        ESP_LOGE(TAG, "the slot's theme would not load (%s) — base digital face",
                 esp_err_to_name(de));
    use_no_art();
    return ESP_OK;
}

/** One place to fail: the row shows `why`, the log shows the same sentence, the
 *  caller gets the esp_err_t that decides whether it asks for a reboot. */
static esp_err_t install_failed(esp_err_t e, const char *w, char *why, size_t whysz)
{
    if (why && whysz) strlcpy(why, w ? w : "install failed", whysz);
    ESP_LOGW(TAG, "install: %s", w ? w : "install failed");
    return e;
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
static char s_try_id[ORB_THEME_HANDLE_MAX + 1];
/* C3 widened the witness from 56 to 72 bytes: §5.3's refusal sentence carries
 * two page counts ("this theme needs N MMU pages and this board has M free")
 * and must not lose the count that is its whole point to a truncation. The
 * static assert on THEME3_REFUSAL still bounds the older sentence; this buffer
 * is now the wider of the two rules. */
static char s_try_why[72];

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
typedef struct { char id[ORB_THEME_HANDLE_MAX + 1]; } install_req_t;


/* Migration 1's one worker: the press that stores a bundle into the single slot and
 * then asks for the reboot that draws it. A store IS the theme change now — there is
 * no second slot to store into without choosing and no flip to move the glass after —
 * so a successful store reboots, exactly as a FORMAT 2 install did. The try witness is
 * unchanged: the row the press named carries the refusal sentence. */
static void bundle_job(void *arg)
{
    const install_req_t *r = arg;
    char why[72] = "";        /* as wide as the try witness: §5.3's page-count sentence must land whole */
    if (orb_theme_install_bundle(r->id, why, sizeof why) != ESP_OK) {
        try_note(r->id, why[0] ? why : "the install failed");
        return;
    }
    try_note(NULL, NULL);
    /* The store parked the glass on the menu — the erase needs the map down — and
     * the park's own switch wrote `menu` into the resume record. Overwrite it with
     * the face the store just replaced: the reboot this job arms should answer the
     * owner's upload with their new dial, not the menu. If the clock row refuses or
     * vanished, resume_last falls back home exactly as an unremembering boot does. */
    input_router_remember_slug("clock");
    if (orb_jobs_reboot_soon("theme: the store") != ESP_OK)
        ESP_LOGE(TAG, "bundle stored but the reboot could not be armed");
}

esp_err_t orb_theme_post_bundle(const char *id)
{
    if (!theme_id_ok(id)) return ESP_ERR_INVALID_ARG;
    /* Named before the queue is asked, exactly as the store door does it: the row
     * a refusal lands on is the row that was pressed, even when the reason is
     * that the card never answered. */
    try_note(id, "");
    install_req_t r;
    strlcpy(r.id, id, sizeof r.id);
    return orb_jobs_post(bundle_job, &r, sizeof r);
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
    /* Ask for the card before the lock below, not after: this runs on a worker that
     * a boot can start before orb_sd_init() ever called, and FatFs calls against an
     * unmounted volume answer with the wrong error, not a crash. card_open_verified_id3
     * mounts too; this is the one the unlink's own lock hold depends on. */
    if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;
    orb_bundle3_hdr_t b3;
    uint32_t fsz = 0;
    const int fd = card_open_verified_id3(id, &b3, &fsz, &w);
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
    ESP_LOGI(TAG, "deleted \"%s\" (%s) from the card%s", b3.name, id,
             s_active_valid && strcmp(b3.name, s_active_name) == 0
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
    int                counted;         /* the walk's extension-total, cap and
                                         * prefix included — the page's "8 of 19" */
    const char        *app;             /* which app's rows this listing carries;
                                         * another app's file is not a refusal,
                                         * it is not this source's row at all */
    /* Migration 1: the OWNER NAME the ONE slot header carried when this walk
     * started, probed LIVE rather than remembered from boot — a store lands and
     * reboots, and the row must show as stored for the theme that is in the slot
     * right now. The slot stores the theme, not the filename, so a row's stored
     * test re-derives the stem id from this name under the row's own slug.
     * Empty string = nothing drawable in the slot. */
    char               slot_name[THEME_NAME_MAX + 1];
} scan_ctx_t;

/* The app's half of the walk. The OS found a <slug>-<token>-<id>.orbtheme, checked
 * the filename is one path segment, and has it open; what the file MEANS — whether
 * it is a theme at all, whether its header name hashes to the id its file is named
 * under, whether its section belongs to the app its name claims, whether it is the
 * one on the dial — is this layer's business and not the card's. */
static bool scan_entry(void *arg, const orb_file_rec_t *rec, int fd)
{
    scan_ctx_t *const c = arg;
    if (c->n >= c->cap) return false;

    /* Split first, before any row is spent: a name that is not the device's own
     * compound form was never filed here by an orb, and one carrying ANOTHER
     * app's slug is not this source's row — the walk counted it, this listing
     * does not show it, and no header of it is read on this walk. An
     * unparseable name keeps a row with its reason: the owner has to be able to
     * see the file the page then declines to act on. */
    char slug[ORB_BUNDLE_SLUG_MAX + 1], id[THEME_ID_HEX + 1];
    const bool mine = theme_stem_split(rec->name, slug, sizeof slug,
                                       id, sizeof id) != false;
    if (mine && strcmp(slug, c->app) != 0) return false;

    orb_theme_entry_t *e = &c->out[c->n];
    ESP_LOGI(TAG, "checking \"%s\" on the card", rec->name);
    e->name[0] = '\0';
    /* The id is the filename the OS already normalised; echo it so a damaged row
     * still has an addressable handle, and fill the owner from the name the
     * split proved. An unparseable name keeps its stem echoed all the same: the
     * page lists it, refuses it, and names why in .why. */
    strlcpy(e->id, rec->name, sizeof(e->id));
    strlcpy(e->app, mine ? slug : "", sizeof e->app);
    e->why[0] = '\0';
    e->active = false; e->changed = false; e->stored = false;
    e->art_size = 0; e->serial = 0;
    e->fmt = 0; e->n_sections = 0;

    if (!mine) {
        strlcpy(e->why, "not a name the orb gives themes — re-export the file",
                sizeof e->why);
        e->err = ESP_ERR_INVALID_ARG;
        c->n++;
        return true;
    }

    orb_bundle3_hdr_t b3;
    uint32_t fsz3 = 0;
    const char *why = card_check3(fd, &b3, &fsz3);
    if (why) {
        /* Listed with its reason rather than skipped. "no bundles on the card" and
         * "three bundles, all damaged" are different problems — the card's or the
         * bake's — and hiding the entries is what makes that undiagnosable. The
         * reason goes in .why, not as a name: the page shows the name as a label and
         * the reason as a warning. */
        strlcpy(e->why, why, sizeof(e->why));
        e->err = ESP_ERR_INVALID_ARG;
        c->n++;
        return true;
    }

    /* A verified FORMAT 3 bundle, listed as one: the id is the R4 hash of the owner
     * name, and the layout/art bounds are the install door's own rule (card_check3,
     * re-run here so a listed row means a Store press has a chance). active/changed
     * are asked of the identity pair the STORE door compares — the container's region
     * checksum, kept in the slot header's word 10 — because P6.5 §25.5(2) made a
     * stored bundle able to BE the dial, so a row that always said active=false lied
     * by omission. FORMAT 2 is retired; a bundle whose name is not its id is refused
     * exactly as the Store door refuses it, keeping the row addressable. */
    char bid[THEME_ID_HEX + 1];
    theme_id(slug, b3.name, bid);
    strlcpy(e->name, b3.name, sizeof e->name);
    e->fmt = 3;
    e->n_sections = b3.n_sections;
    e->art_size = b3.art_size;
    e->serial = b3.serial;
    if (strcmp(bid, id) != 0) {
        strlcpy(e->why, "the file is not named for its own theme name — re-upload it",
                sizeof(e->why));
        e->err = ESP_ERR_INVALID_ARG;
        c->n++;
        return true;
    }
    /* §31: "valid on this orb" is the projection's answer, not the header's. The
     * region read is bounded by the header-checked geom_size, and the answer is
     * the store door's own sentence — the row can mark nothing the press would
     * then do. This is the check the hub's Store button gates on. The same read
     * names the section this container carries: the stem's slug is the row's
     * owner, and a file named for one app that themes another is a file the
     * listing must refuse by name, not one it files under a stranger. */
    char sec0[ORB_BUNDLE_SLUG_MAX + 1] = "";
    uint32_t nsec = 0;
    const char *dr = clock3_verdict3(fd, &b3, fsz3, sec0, &nsec);
    if (!dr && nsec != 1) {
        snprintf(e->why, sizeof e->why,
                 "a theme file carries exactly one section — this one carries %lu",
                 (unsigned long)nsec);
        dr = e->why;
    }
    if (!dr && strcmp(sec0, slug) != 0) {
        snprintf(e->why, sizeof e->why,
                 "this file themes %s — its name says %s", sec0, slug);
        dr = e->why;
    }
    if (dr) {
        if (dr != e->why) strlcpy(e->why, dr, sizeof(e->why));   /* set-in-place rows copy once */
        e->err = ESP_ERR_INVALID_ARG;
        c->n++;
        return true;
    }
    e->err = ESP_OK;
    /* Which slot holds this file's stored copy — the one slot. The store reboots, so a
     * row's stored marker tells the page this file is what the dial now draws; the
     * store door re-verifies the same header before it writes, so a card that changed
     * under the answer costs nothing but one honest refusal on the row. The slot
     * carries the theme, not the filename: the test re-derives this row's id from
     * the slot's name under this row's own slug — the identity the store hashed on
     * its way in. */
    if (c->slot_name[0]) {
        char sid[THEME_ID_HEX + 1];
        theme_id(slug, c->slot_name, sid);
        if (strcmp(sid, id) == 0) e->stored = true;
    }
    orb_thdr_t as;      /* the identity theme_state() reads: name + the two figures */
    memset(&as, 0, sizeof as);
    strlcpy(as.name, b3.name, sizeof as.name);
    as.geom_crc = s_bundle_file_geom;
    as.art_crc = s_bundle_file_geom;   /* adopt_bundle3's one-figure rule */
    as.fmt = 3;
    theme_state(&as, &e->active, &e->changed);
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

    scan_ctx_t c = { .out = out, .cap = cap, .n = 0, .app = "clock" };
    /* Probe the one slot for this walk's answer. The partition outliving the card is
     * the point: a stored bundle stays listed as stored with no card in the socket at
     * all, and this probe only decides what the page MARKS — the store door re-reads
     * the same header on the worker before it writes anything. The probe answers with
     * the slot's OWNER NAME; the stem a row is stored under is re-derived per row,
     * because the slot holds a theme and the name is a filing decision. */
    const esp_partition_t *sp = art_bank_partition();
    if (sp) {
        orb_bundle3_hdr_t sh;
        uint32_t sraw[W_WORDS];
        bool claims;
        if (slot_probe3(sp, SLOT_HDR, &sh, sraw, &claims))
            strlcpy(c.slot_name, sh.name, sizeof c.slot_name);
    }
    /* Both ends logged, with the wait: a scan that never comes back is the one card
     * failure that looks like a dead web server from outside (the dial keeps drawing
     * and ping keeps replying), and which of the two lines is missing says whether the
     * worker is waiting on the mutex or inside FatFs. */
    const int64_t t_scan = esp_timer_get_time();
    ESP_LOGI(TAG, "listing %s", src->dir);
    if (orb_files_walk(src, scan_entry, &c, &c.counted) < 0) return -1;
    s_scan_counted = c.counted;
    ESP_LOGI(TAG, "%d of %d entr%slisted in %lld ms", c.n, c.counted,
             c.n == 1 ? "y " : "ies ",
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

/* The one-shot token naming a scratch that a duplicate refusal left standing. A
 * refused-by-duplication upload keeps its bytes on the card (see upload_end's DUP
 * branch) so the owner's "replace it?" answer costs an empty POST, not a second
 * multi-megabyte crossing. The token is 8 random bytes in hex, lives only in RAM,
 * and every path that consumes, replaces, rejects or cancels the scratch clears it
 * — so a commit either names exactly the copy a refusal just tokened or is refused,
 * and a months-old scratch can never be filed under a stranger's overwrite. */
static char s_stage_tok[17];

static void stage_forget(void) { s_stage_tok[0] = 0; }

const char *orb_theme_staged_commit(void) { return s_stage_tok; }


bool orb_theme_upload_busy(void) { return s_up != NULL; }

esp_err_t orb_theme_upload_begin(orb_theme_up_t **out)
{
    *out = NULL;
    if (s_up) return ESP_ERR_INVALID_STATE;                /* one at a time */
    const orb_file_src_t *src = orb_files_src("clock", "themes");
    if (!src) return ESP_ERR_NOT_FOUND;
    if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;
    /* Reserve the card read buffer NOW, before the radio carries 3.87 MB for a
     * transfer this device cannot finish. verify_bundle3() reserves it again
     * defensively; this is the early, cheap no. */
    if (ensure_chunk() != ESP_OK) return ESP_ERR_NO_MEM;

    /* Migration 1 / D9: an upload to this door is the start of a STORE, and under a
     * single slot a store arms its invalid state BEFORE any payload crosses. Erase the
     * slot's header sector now. A complete upload re-stores and reboots; a killed,
     * cancelled, or refused one leaves the slot headerless, so the next boot draws the
     * base face and one re-press recovers. Erasing just the 4 KB header is enough to
     * make the slot unreadable — copy_bundle3 erases the payload region and rewrites the
     * header last when the store runs. */
    const esp_partition_t *ap = art_bank_partition();
    if (!ap) return ESP_ERR_NOT_FOUND;
    if (esp_partition_erase_range(ap, SLOT_HDR, THEME_SECTOR) != ESP_OK) {
        ESP_LOGE(TAG, "upload: could not arm the slot — refusing before any bytes cross");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "upload started: the theme slot is armed invalid until a re-store lands");
    /* A fresh upload owns the scratch's fixed name from here: whatever token named
     * the previous one refers to bytes this begin() is about to truncate. */
    stage_forget();

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

esp_err_t orb_theme_upload_end(orb_theme_up_t **u, bool overwrite, const char **why,
                               char *committed_id)
{
    orb_theme_up_t *const up = *u;
    *why = NULL;
    if (committed_id) committed_id[0] = '\0';
    if (!up || up != s_up) { *why = "no upload is in progress"; return ESP_ERR_INVALID_STATE; }

    close(up->fd);
    s_up = NULL;                     /* the card is still locked; released below */

    /* Verify the file that is NOW on the card — not the bytes that passed through
     * this handler. A card that dropped a write, or a client that lied about the
     * length, is caught here rather than by the install three boots later.
     *
     * FORMAT 3 only: this door stores a bundle, and the Store press re-verifies
     * everything it is about to put in a slot. A torn FORMAT 3 header answers the
     * generic line — its format word is unverified, and the refusal says only what
     * is proven. FORMAT 2 is retired and refused here like everywhere else. */
    const int fd = open(up->tmp, O_RDONLY);
    if (fd < 0) {
        *why = "the uploaded file is not on the card";
        unlink(up->tmp); orb_sd_unlock(); free(up);
        return ESP_FAIL;
    }
    orb_bundle3_hdr_t b3;
    uint32_t fsz = 0;
    char sec0[ORB_BUNDLE_SLUG_MAX + 1] = "";
    uint32_t nsec = 0;
    const char *r = card_check3(fd, &b3, &fsz);
    if (!r) r = verify_bundle3(fd, &b3, fsz, sec0, &nsec);
    if (!r && nsec != 1)
        r = one_section_why(nsec);

    /* The card filename is the compound stem: the owning app's slug from the
     * header's single section, a token sanitized from the owner name, and the
     * id that hashes the pair. Nothing the client named this upload — the
     * destination is whatever this decides, from bytes that had to arrive
     * intact. card_check3() already refused a container with no name in it, so
     * the name here is never empty and theme_stem() cannot be asked to file a
     * theme of nothing. */
    close(fd);
    char id[ORB_THEME_HANDLE_MAX + 1];
    char final[ORB_SD_PATH_MAX];
    if (!r) theme_stem(sec0, b3.name, id, sizeof id);
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
            orb_bundle3_hdr_t oh3;
            uint32_t ohfsz = 0;
            const char *or_ = card_check3(efd, &oh3, &ohfsz);
            close(efd);
            if (!or_ && oh3.geom_crc == b3.geom_crc &&
                oh3.art_crc == b3.art_crc &&
                oh3.serial == b3.serial && strcmp(oh3.name, b3.name) == 0) {
                /* Idempotent: identical bundle, discard the scratch, nothing changed.
                 * The id is still reported so the caller re-stores the one it already
                 * has — an upload armed the slot, so the only way out is a store. */
                unlink(up->tmp); orb_sd_unlock(); free(up);
                if (committed_id) strlcpy(committed_id, id, ORB_THEME_HANDLE_MAX + 1);
                stage_forget();
                ESP_LOGI(TAG, "upload of \"%s\" (%s) is unchanged — kept as is", b3.name, id);
                return ESP_OK;
            }
            if (!overwrite) {
                /* Same name, different bytes: the existing file stays, and the
                 * scratch STAYS too — verified bytes already sit on the card, and the
                 * owner's next word may be "replace". Deleting it here is what made
                 * every duplicate cost a second full upload after the confirm. The
                 * refusal now issues a one-shot commit token naming the scratch; the
                 * hub's confirmed replace commits it with an empty body. The scratch
                 * keeps its fixed non-.orbtheme name so the list never shows it, and
                 * the next upload_begin() truncates whatever a refusal left behind. */
                orb_sd_unlock();
                const uint32_t rnd[2] = { esp_random(), esp_random() };
                static const char hx[] = "0123456789abcdef";
                for (int i = 0; i < 8; i++) {
                    const uint8_t b = (uint8_t)(rnd[i / 4] >> ((i % 4) * 8));
                    s_stage_tok[i * 2]     = hx[b >> 4];
                    s_stage_tok[i * 2 + 1] = hx[b & 0xF];
                }
                s_stage_tok[16] = 0;
                *why = "a different theme with that name is already on the card";
                free(up);
                ESP_LOGW(TAG, "upload \"%s\" (%s) collides with a different installed theme", b3.name, id);
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
        stage_forget();
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
    stage_forget();   /* the rename consumed the scratch; its token names nothing now */
    ESP_LOGI(TAG, "bundle \"%s\" uploaded to the card as %s%s", b3.name, id,
             replace ? " (overwrote the previous one)" : "");
    if (committed_id) strlcpy(committed_id, id, ORB_THEME_HANDLE_MAX + 1);
    return ESP_OK;
}

/* The confirmed answer to a duplicate refusal — the empty-body twin of a completed
 * upload. The caller proves it holds the token the refusal issued (the refusal's own
 * JSON echoed it), and the scratch it names is re-verified from the card, not from
 * the token: card_check3 + verify_bundle3 + the identity hash, the same three gates
 * upload_end ran when the bytes first landed. Only then does it take the same last
 * step a completed upload takes — rename over the existing file. Everything that can
 * go wrong returns a code and leaves the scratch exactly as found, so a failed
 * commit still costs a real re-upload rather than a lost theme. */
esp_err_t orb_theme_upload_recommit(const char *token, const char **why,
                                    char *committed_id, uint32_t *bytes)
{
    *why = NULL;
    if (committed_id) committed_id[0] = '\0';
    if (bytes) *bytes = 0;
    if (!token || !s_stage_tok[0] || strcmp(token, s_stage_tok) != 0) {
        *why = "no staged upload answers to that token";
        return ESP_ERR_NOT_FOUND;
    }
    if (s_up) {                       /* an upload owns the scratch name right now */
        *why = "an upload is in progress";
        return ESP_ERR_INVALID_STATE;
    }
    const orb_file_src_t *src = orb_files_src("clock", "themes");
    if (!src) { *why = "no card in the socket"; return ESP_ERR_NOT_FOUND; }
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = "no card in the socket"; return ESP_ERR_NOT_FOUND; }
    if (ensure_chunk() != ESP_OK) { *why = "out of memory for the card read buffer"; return ESP_ERR_NO_MEM; }

    char tmp[ORB_SD_PATH_MAX];
    const int tn = snprintf(tmp, sizeof tmp, "%s/%s", src->dir, THEME_SCRATCH);
    if (tn < 0 || (size_t)tn >= sizeof tmp) { *why = "staged path overflow"; return ESP_ERR_INVALID_SIZE; }

    orb_sd_lock();
    const int fd = open(tmp, O_RDONLY);
    if (fd < 0) {
        orb_sd_unlock(); stage_forget();
        *why = "the staged copy is no longer on the card";
        return ESP_ERR_NOT_FOUND;
    }
    const long scratch_sz = lseek(fd, 0, SEEK_END);
    (void)lseek(fd, 0, SEEK_SET);
    orb_bundle3_hdr_t b3;
    uint32_t fsz = 0;
    char sec0[ORB_BUNDLE_SLUG_MAX + 1] = "";
    uint32_t nsec = 0;
    const char *r = (scratch_sz > 0) ? card_check3(fd, &b3, &fsz) : "the staged copy is no longer on the card";
    if (!r) r = verify_bundle3(fd, &b3, fsz, sec0, &nsec);
    if (!r && nsec != 1) r = one_section_why(nsec);
    close(fd);
    if (r) {
        /* Corrupt or gone: exactly what a refused upload's deletion used to do —
         * remove it, forget the token, and make the caller send the file for real. */
        unlink(tmp);
        orb_sd_unlock(); stage_forget();
        *why = r;
        return ESP_ERR_INVALID_CRC;
    }

    char id[ORB_THEME_HANDLE_MAX + 1];
    char final[ORB_SD_PATH_MAX];
    theme_stem(sec0, b3.name, id, sizeof id);
    if (!theme_file(id, final, sizeof final)) {
        orb_sd_unlock();
        *why = "the theme name is not one the card can hold";
        return ESP_FAIL;
    }
    const int efd = open(final, O_RDONLY);   /* the old file, if it still stands */
    if (efd >= 0) close(efd);
    const uint32_t sz = (uint32_t)scratch_sz;
    /* The overwrite window, same reasoning as upload_end's: verified bytes in hand,
     * unlink the old, rename the staged copy in. */
    if (efd >= 0) unlink(final);
    const int rr = rename(tmp, final);
    const int re = errno;
    orb_sd_unlock();
    if (rr != 0) {
        *why = "could not put the staged theme in place";
        ESP_LOGE(TAG, "recommit rename: %s", strerror(re));
        return ESP_FAIL;
    }
    stage_forget();
    if (bytes) *bytes = sz;
    if (committed_id) strlcpy(committed_id, id, ORB_THEME_HANDLE_MAX + 1);
    ESP_LOGI(TAG, "duplicate confirmed from the staged copy: bundle \"%s\" filed as %s (one upload, not two)", b3.name, id);
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
         * investigated as corruption. A cancelled upload takes any staged-commit
         * token with it: the scratch name is fixed, so a token left standing could
         * only ever name the file this cancel just deleted. */
        unlink(up->tmp);
        stage_forget();
        orb_sd_unlock();
    }
    free(up);
}
