/**
 * orb_theme.c — see orb_theme.h for the model. An installed theme is a container
 * from the card copied into the single slot at the head of the `assets` partition,
 * and the geometry it carries is what orb_stage.c reads instead of the compiled-in
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
#include "orb_disc.h"   /* a preview stored as its disc */
#include "orb_inflate.h" /* §60: and deflated */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "esp_flash.h"      /* esp_flash_read_id: the C0 bench names the chip */
#include <inttypes.h>                 // PRIu32 in the complication-loop logs (the
                                      // riscv toolchain's uint32_t is not `int`)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"     /* rd_at: is the destination one the card can DMA into */
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
#include "orb_pool.h"
#include "orb_pagemap.h"
#include "orb_strip.h"       /* a chosen theme's preview, read again */     /* §4.1: an entry's pages as one window */
#include "orb_elems.h"      /* orb_elems_at(), and the answers read_one_plane maps onto the registry's */
#include "orb_clock3.h"          /* the FORMAT 3 clock section's projection (P6.5) */
#include "orb_sound.h"           /* D39: silence the old bank's voices before a remap */
#include "orb_sd.h"
#include "orb_jobs.h"
#include "orb_voice.h"
#include "orb_adpcm.h"      /* a sound's bytes from its samples */      /* theme-cache-plan §2.6: an app's voice bank */      /* the two post_* calls: an install is a worker job, never a poll */
#include "orb_files.h"      /* the OS owns the card walk; this layer interprets what it finds */
#include "art_bank.h"
#include "orb_sched.h"          /* store_park_dial: the tick's park gate is the task-safe half of §6.3 */
#include "orb_app.h"            /* orb_app_home(): the row the store hands the glass to */
#include "orb_app_wasm.h"       /* theme-cache-plan §2.4: orb_wasm_theme_landed */
#include "lvgl.h"               /* theme-cache-plan §2.4: lv_async_call, a landed fill rebinds on the LVGL task */
#include "orb_io_window.h"      /* theme-cache-plan §2.4: the fill is heavy I/O to a guest's budget */
#include "input_router.h"       /* input_router_request/current: the task-safe screen switch */
#include "orb_boot_overlay.h"   /* the store's progress frames (§6.3) */
#include "display.h"            /* display_lvgl_passes: the store waits out a render before it unmaps */
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
    W_DESIGN_OFF = 29, W_DESIGN_SIZE = 30, W_DESIGN_CRC = 31,   /* D43 P7: an app's design */
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

/* Bounds the loader accepts, from orb_stage.c's own array sizes and from what
 * the shipped bake measures (128 rects, 8 GIF frames). A theme wanting more than
 * the dial holds is refused as unloadable rather than installed and then clamped
 * at paint time into a half-drawn frame. The plate bank's own bound is no loader
 * constant: orb_clock3.c refuses a bank past CLK_BG_MAX at the projection (§31),
 * so s_b3_bgf below never sees a row count it cannot hold. */
#define RECTS_MAX       2048
#define GIFS_MAX        32                         /* CLK_GIF_MAX */

/* A sprite occupies colour w*h*2 then alpha w*h. Must match sprite_bytes() in
 * orb_stage.c, which is the code these offsets are checked against. */
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
    /* The separator ahead of the tail fixes the tail's length. The tail is
     * THEME_ID_HEX hex chars — 12, not 13; the 13th byte from the end is the
     * separator itself. A stem with no '-' at that offset, or a shorter run of
     * hex than that, fails here: the '-' would be read as part of the token and
     * the hex loop below would refuse a non-hex char inside the id. */
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

/* D39: the adopted dial's sounds. PSRAM behind one pointer rather than a static
 * table beside the projection's other tables: thirteen rows of it would be 208 B of the internal
 * RAM this board has none of (orb_sound.c's header has the measurement). */
typedef struct {
    clk_sound_t s[CLK_SND_SLOTS];
    uint32_t    n;
    uint8_t     pattern;
} theme_sounds_t;
static theme_sounds_t *s_snd;
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
 * base digital face and one re-store recovers it. Worded for the owner who
 * reads it on the dial's own page and on the hub's row — what happened, what
 * the dial shows, and the one press that fixes it. No internals. */
#define STAGED_WHY "last store did not finish - press again to complete it"

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
 * slot both fail to draw clears the geometry so the stage's residency gate fails and it
 * paints the base digital face, which still tells the time. s_geo.theme stays NULL and is
 * only ever dereferenced on the themed leg, which the residency gate has already fenced
 * off by the time anything reads it. */
/* No art is a zeroed theme, never a NULL one: clk_theme dereferences the geometry's theme
 * unguarded on every flush (orb_sched_l4_active -> orb_bg_gif_mode), and a bind that let its
 * art go while the stage still flushed read address 0x34 in a boot loop (Bronze, 2026-11-08,
 * theme-cache-plan ruling 8's refusal). Zero reads as no gif, no sweep, no rows. */
EXT_RAM_BSS_ATTR static clk_theme_t s_no_theme;

static void use_no_art(void)
{
    /* The apps resolve against nothing: retire the published index with the
     * geometry, so a base-face dial cannot half-draw a bundle it dropped. */
    heap_caps_free(s_live_region);
    s_live_region = NULL;
    s_live_sec = NULL;
    memset(&s_live_b, 0, sizeof s_live_b);
    memset(&s_no_theme, 0, sizeof s_no_theme);
    s_geo.theme      = &s_no_theme;
    s_geo.rects      = NULL;
    s_geo.rect_first = NULL;
    s_geo.gif_delays = NULL;
    s_geo.bg_frames  = NULL;
    s_geo.comps      = NULL;
    s_geo.rows       = NULL;
    s_geo.bank       = NULL;
    s_geo.n_comps    = 0;
    s_geo.n_rows     = 0;
    s_geo.n_bank     = 0;
    s_geo.art_size   = 0;
    s_geo.blob_off   = 0;
    s_geo.from_container = false;
    s_geo.name[0] = '\0';
    if (s_snd) s_snd->n = 0;
    s_active_name[0] = '\0';
    s_active_valid = false;
    s_source = "none";
}

const orb_theme_geo_t *orb_theme_geo(void) { return &s_geo; }

const clk_sound_t *orb_theme_sounds(uint32_t *n, uint8_t *pattern)
{
    *n = s_snd ? s_snd->n : 0;
    *pattern = s_snd ? s_snd->pattern : CLK_PAT_NONE;
    return *n ? s_snd->s : NULL;
}

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

/* D50: the app the live theme belongs to (its one section's slug), "" when none. */
const char *orb_theme_live_slug(void)      { return s_live_sec ? s_live_sec->slug : ""; }
/* The residency verdict the boot ladder reached: an adopted, drawable theme whose
 * geometry is published. Not the same question as art_bank_ready() — that answers
 * only whether a span is mapped, and a mapped span whose clock projection refused
 * is pixels with no dial. stage_init gates its themed leg on the two together;
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

/* ---- the theme pool (docs/theme-pool-plan.md, theme-cache-plan.md §4.1) ----------------
 *
 * Several apps' themes, each on whole 64 KB pages wherever they were free, with orb_pool.c's
 * table in the area's last two sectors. Each entry is laid out as the one slot was, so every
 * SLOT_* offset in this file is the entry's own, reached through its pages (pg_read and the
 * rest): its header at SLOT_HDR, its index at SLOT_GEOM_OFF, its banks at SLOT_ART_OFF. The
 * one slot of Migration 1 sits on the first pages, which is why boot can carry it into the
 * pool without moving a byte. s_live_key names the entry the stage is bound to: its pages
 * and its CRCs, since a refilled theme can land on the same first page. */
static orb_pool_t    *s_pool;          /* PSRAM: the table as last read or written */
static int            s_pool_which;    /* the live copy, orb_pool_load's answer */
static uint8_t       *s_pool_buf;      /* PSRAM: one sector, the pool's scratch */
static orb_pool_io_t  s_pool_io;
static uint32_t       s_live_key;      /* the bound entry's pages and CRCs, 0 = none */

static int pool_read(void *c, uint32_t off, void *d, uint32_t n)
{
    return esp_partition_read((const esp_partition_t *)c, off, d, n) == ESP_OK ? 0 : -1;
}

static int pool_erase(void *c, uint32_t off, uint32_t n)
{
    return esp_partition_erase_range((const esp_partition_t *)c, off, n) == ESP_OK ? 0 : -1;
}

static int pool_write(void *c, uint32_t off, const void *s, uint32_t n)
{
    return esp_partition_write((const esp_partition_t *)c, off, s, n) == ESP_OK ? 0 : -1;
}

/* The io over the partition, and the table's two PSRAM buffers, made once. */
static bool pool_io(const esp_partition_t *p)
{
    if (!s_pool) s_pool = heap_caps_calloc(1, sizeof *s_pool, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pool_buf) s_pool_buf = heap_caps_malloc(ORB_POOL_SECTOR, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pool || !s_pool_buf || !p) return false;
    s_pool_io = (orb_pool_io_t){ (void *)p, pool_read, pool_erase, pool_write,
                                 s_pool_buf, ORB_POOL_SECTOR, NULL };
    return true;
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
/* §60.3: a read into PSRAM ran at ~600 KB/s. The card driver cannot DMA there, so it bounced
 * every 512 B sector through its own buffer as a command of its own (Gold, 2026-11-07: a
 * 160-190 KB preview held the card, and so the glass, for ~300 ms). A big read into memory
 * the card cannot reach goes through an internal buffer instead, whole sectors from a sector
 * boundary, so FatFs hands the driver multi-block reads. The buffer is taken and given back
 * inside the one call: the caller holds the card, so the display's ring is lent and this is
 * the memory the loan freed — nothing outlives the loan to leave the ring short. A refused
 * buffer is the old way, slower and still correct. */
#define RD_BOUNCE_MAX  16384u
#define RD_BOUNCE_MIN  4096u
static ssize_t rd_at_bounced(int fd, size_t *io_pos, uint8_t *dst, size_t n)
{
    uint8_t *b = NULL;
    size_t bsz = RD_BOUNCE_MAX;
    for (; bsz >= RD_BOUNCE_MIN && !b; bsz /= 2)
        b = heap_caps_malloc(bsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!b) return -2;                            /* the caller reads the plain way */
    bsz *= 2;                                     /* undo the loop's last halving */
    size_t done = 0;
    while (done < n) {
        const size_t at = *io_pos + done;
        const size_t lead = at % 512u;            /* re-read the sector's head: aligned */
        size_t ask = lead + (n - done);
        if (ask > bsz) ask = bsz;
        const ssize_t got = pread(fd, b, ask, (off_t)(at - lead));
        if (got < 0) { heap_caps_free(b); return -1; }
        if ((size_t)got <= lead) break;           /* short file */
        const size_t use = (size_t)got - lead;
        memcpy(dst + done, b + lead, use);
        done += use;
        if ((size_t)got < ask) break;
    }
    heap_caps_free(b);
    *io_pos += done;
    return (ssize_t)done;
}

static ssize_t rd_at(int fd, size_t *io_pos, uint8_t *dst, size_t n)
{
    if (n >= RD_BOUNCE_MIN && !esp_ptr_internal(dst)) {
        const ssize_t r = rd_at_bounced(fd, io_pos, dst, n);
        if (r != -2) return r;
    }
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

/* ---- theme-cache-plan §2.1: each app's choice, and the order of the glass ----------------
 * One NVS blob: what each app wears (its card stem and the file's geometry CRC, the identity
 * its flash copies carry, ruling 7), kept when the pool lets the copies go, and the order the
 * apps were last on the glass, least recent first, which decides what a store or a fill
 * evicts. The order is RAM's between saves: it is written with an eviction, a choice, or
 * at most every ORDER_SAVE_US otherwise, so an app switch costs no flash write. */
#define THEME_NVS_CACHE   "theme_cache"
#define CACHE_VER         1u
#define CACHE_APPS        8u
#define ORDER_SAVE_US     (5LL * 60 * 1000000)
typedef struct {
    uint32_t ver, n_choice, n_order, pad;
    struct { char slug[ORB_POOL_SLUG_MAX]; char stem[ORB_THEME_HANDLE_MAX + 1]; uint32_t file_crc; } c[CACHE_APPS];
    char order[CACHE_APPS][ORB_POOL_SLUG_MAX];
} cache_rec_t;
EXT_RAM_BSS_ATTR static cache_rec_t s_cache;
static bool    s_cache_dirty;
static int64_t s_cache_saved_us;

static void cache_save(void)
{
    const nvs_handle_t h = theme_nvs();
    if (!h) return;
    s_cache.ver = CACHE_VER;
    if (nvs_set_blob(h, THEME_NVS_CACHE, &s_cache, sizeof s_cache) == ESP_OK && nvs_commit(h) == ESP_OK) {
        s_cache_dirty = false;
        s_cache_saved_us = esp_timer_get_time();
    } else {
        ESP_LOGW(TAG, "the theme choices would not save — the order since the last save is lost at a cut");
    }
}

static void cache_load(void)
{
    memset(&s_cache, 0, sizeof s_cache);
    const nvs_handle_t h = theme_nvs();
    size_t sz = sizeof s_cache;
    if (!h || nvs_get_blob(h, THEME_NVS_CACHE, &s_cache, &sz) != ESP_OK || sz != sizeof s_cache ||
        s_cache.ver != CACHE_VER || s_cache.n_choice > CACHE_APPS || s_cache.n_order > CACHE_APPS) {
        memset(&s_cache, 0, sizeof s_cache);         /* none yet: the pool's order stands in */
        return;
    }
    for (uint32_t i = 0; i < CACHE_APPS; i++) {      /* a torn string is no string */
        s_cache.c[i].slug[sizeof s_cache.c[i].slug - 1] = '\0';
        s_cache.c[i].stem[sizeof s_cache.c[i].stem - 1] = '\0';
        s_cache.order[i][sizeof s_cache.order[i] - 1] = '\0';
    }
}

static void gone_set(const char *slug, const char *stem);
static void note_set(const char *slug, uint8_t kind, const char *why);

/* `slug` wears card file `stem`, whose geometry CRC is file_crc; NULL stem lets the choice go. */
static void choice_set(const char *slug, const char *stem, uint32_t file_crc)
{
    uint32_t k = 0;
    while (k < s_cache.n_choice && strcmp(s_cache.c[k].slug, slug)) k++;
    if (!stem) {
        if (k == s_cache.n_choice) return;
        memmove(&s_cache.c[k], &s_cache.c[k + 1], (s_cache.n_choice - k - 1) * sizeof s_cache.c[0]);
        memset(&s_cache.c[--s_cache.n_choice], 0, sizeof s_cache.c[0]);
    } else {
        if (k == s_cache.n_choice) {
            if (k >= CACHE_APPS) { ESP_LOGW(TAG, "theme choices full: %s's is not kept", slug); return; }
            s_cache.n_choice++;
        }
        strlcpy(s_cache.c[k].slug, slug, sizeof s_cache.c[k].slug);
        strlcpy(s_cache.c[k].stem, stem, sizeof s_cache.c[k].stem);
        s_cache.c[k].file_crc = file_crc;
    }
    cache_save();
    if (stem) { note_set(slug, 0, NULL); gone_set(slug, NULL); }   /* it has a theme again */
}

/* `slug` is on the glass now: the end of the order (the most recent). */
static void order_touch(const char *slug)
{
    if (!slug || !slug[0]) return;
    uint32_t k = 0;
    while (k < s_cache.n_order && strcmp(s_cache.order[k], slug)) k++;
    if (k + 1 == s_cache.n_order) return;            /* already the most recent */
    if (k < s_cache.n_order) {
        memmove(&s_cache.order[k], &s_cache.order[k + 1], (s_cache.n_order - k - 1) * sizeof s_cache.order[0]);
        s_cache.n_order--;
    } else if (s_cache.n_order == CACHE_APPS) {      /* the least recent drops off the front */
        memmove(&s_cache.order[0], &s_cache.order[1], (CACHE_APPS - 1) * sizeof s_cache.order[0]);
        s_cache.n_order--;
    }
    strlcpy(s_cache.order[s_cache.n_order++], slug, sizeof s_cache.order[0]);
    s_cache_dirty = true;
    if (esp_timer_get_time() - s_cache_saved_us >= ORDER_SAVE_US) cache_save();
}

/* The order as orb_pool_evictions takes it: least recent first. */
static uint32_t order_list(const char *out[CACHE_APPS])
{
    for (uint32_t i = 0; i < s_cache.n_order; i++) out[i] = s_cache.order[i];
    return s_cache.n_order;
}

/* ---- why an app's theme is missing (owner, 2026-11-09: its theme page says why) ----------
 * A file the reconcile found gone is kept in NVS (its removal reboots, and the page must
 * still say so after); a copy that failed and a theme refused by ruling 8 are this boot's,
 * in RAM; a card that is out is read live. Each clears when the app is stored a theme. */
#define THEME_NVS_GONE "theme_gone"
typedef struct {
    uint32_t n;
    struct { char slug[ORB_POOL_SLUG_MAX]; char stem[ORB_THEME_HANDLE_MAX + 1]; } g[CACHE_APPS];
} gone_rec_t;
EXT_RAM_BSS_ATTR static gone_rec_t s_gone;
enum { NOTE_FAILED = 1, NOTE_RESEND = 2 };
EXT_RAM_BSS_ATTR static struct { char slug[ORB_POOL_SLUG_MAX]; uint8_t kind; char why[72]; } s_note[CACHE_APPS];

static void gone_save(void)
{
    const nvs_handle_t h = theme_nvs();
    if (h && (nvs_set_blob(h, THEME_NVS_GONE, &s_gone, sizeof s_gone) != ESP_OK || nvs_commit(h) != ESP_OK))
        ESP_LOGW(TAG, "the deleted-theme notes would not save");
}

static void gone_load(void)
{
    memset(&s_gone, 0, sizeof s_gone);
    const nvs_handle_t h = theme_nvs();
    size_t sz = sizeof s_gone;
    if (!h || nvs_get_blob(h, THEME_NVS_GONE, &s_gone, &sz) != ESP_OK || sz != sizeof s_gone ||
        s_gone.n > CACHE_APPS) {
        memset(&s_gone, 0, sizeof s_gone);
        return;
    }
    for (uint32_t i = 0; i < CACHE_APPS; i++) {
        s_gone.g[i].slug[sizeof s_gone.g[i].slug - 1] = '\0';
        s_gone.g[i].stem[sizeof s_gone.g[i].stem - 1] = '\0';
    }
}

/* `slug`'s file `stem` was deleted from the card; NULL stem: it has a theme again. */
static void gone_set(const char *slug, const char *stem)
{
    uint32_t k = 0;
    while (k < s_gone.n && strcmp(s_gone.g[k].slug, slug)) k++;
    if (!stem) {
        if (k == s_gone.n) return;
        memmove(&s_gone.g[k], &s_gone.g[k + 1], (s_gone.n - k - 1) * sizeof s_gone.g[0]);
        memset(&s_gone.g[--s_gone.n], 0, sizeof s_gone.g[0]);
    } else {
        if (k == s_gone.n) {
            if (k >= CACHE_APPS) return;
            s_gone.n++;
        }
        strlcpy(s_gone.g[k].slug, slug, sizeof s_gone.g[k].slug);
        strlcpy(s_gone.g[k].stem, stem, sizeof s_gone.g[k].stem);
    }
    gone_save();
}

/* This boot's note on `slug`: kind 0 clears it. */
static void note_set(const char *slug, uint8_t kind, const char *why)
{
    int k = -1, free_k = -1;
    for (int i = 0; i < (int)CACHE_APPS; i++) {
        if (s_note[i].slug[0] && !strcmp(s_note[i].slug, slug)) k = i;
        if (!s_note[i].slug[0] && free_k < 0) free_k = i;
    }
    if (!kind) { if (k >= 0) memset(&s_note[k], 0, sizeof s_note[k]); return; }
    if (k < 0) k = free_k;
    if (k < 0) return;
    strlcpy(s_note[k].slug, slug, sizeof s_note[k].slug);
    s_note[k].kind = kind;
    strlcpy(s_note[k].why, why ? why : "", sizeof s_note[k].why);
}

/* theme-cache-plan §2.4: a fill is the store's own body, copying an evicted app's display
 * entry back from the card, without the park, the unmap's silence or the reboot. */
static volatile bool s_install_fill;          /* the install running is a fill            */
static volatile bool s_fill_changed;          /* a fill found its file changed: reconcile  */
static uint32_t s_reconciled_gen;             /* the card mount the reconcile last ran on  */
static volatile bool s_pool_busy;             /* a fill is moving entries: binds wait     */
static volatile uint32_t s_fill_done, s_fill_total;   /* the fill's progress, for its face */
EXT_RAM_BSS_ATTR static char s_fill_slug[ORB_POOL_SLUG_MAX];   /* the app being filled, "" for none        */
EXT_RAM_BSS_ATTR static char s_hide_slug[ORB_POOL_SLUG_MAX];   /* a store is choosing for it: binds see none */
EXT_RAM_BSS_ATTR static char s_refill_slug[ORB_POOL_SLUG_MAX]; /* chosen while on the glass: fill it there    */

/* The cache as the hub reads it (GET /api/v1/theme_cache): each app's choice, the order of
 * the glass, and the pool's entries. Answers the bytes written. */
size_t orb_theme_cache_json(char *b, size_t cap)
{
    size_t o = 0;
#define PUT(...) do { const int w_ = snprintf(b + o, o < cap ? cap - o : 0, __VA_ARGS__); if (w_ > 0) o += (size_t)w_; } while (0)
    PUT("{\"choices\":[");
    for (uint32_t i = 0; i < s_cache.n_choice; i++)
        PUT("%s{\"app\":\"%s\",\"stem\":\"%s\",\"file_crc\":%lu}", i ? "," : "", s_cache.c[i].slug,
            s_cache.c[i].stem, (unsigned long)s_cache.c[i].file_crc);
    PUT("],\"order\":[");
    for (uint32_t i = 0; i < s_cache.n_order; i++) PUT("%s\"%s\"", i ? "," : "", s_cache.order[i]);
    PUT("],\"pool\":[");
    for (uint32_t k = 0; s_pool && k < s_pool->n; k++) {
        PUT("%s{\"app\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\",\"off\":%lu,\"size\":%lu,\"file_crc\":%lu,\"pages\":[",
            k ? "," : "", s_pool->e[k].slug, s_pool->e[k].name, s_pool->e[k].kind == ORB_POOL_VOICE ? "voice" :
            s_pool->e[k].kind == ORB_POOL_PREVIEW ? "preview" : "display",
            (unsigned long)s_pool->e[k].off, (unsigned long)s_pool->e[k].size, (unsigned long)s_pool->e[k].file_crc);
        /* §4.1: its pages, as runs "first-last" (a scattered entry has several) */
        int n = 0;
        for (uint32_t pg = 0; pg < ORB_POOL_PAGES; pg++) {
            if (s_pool->owner[pg] != s_pool->e[k].pid || (pg && s_pool->owner[pg - 1] == s_pool->e[k].pid)) continue;
            uint32_t end = pg;
            while (end + 1 < ORB_POOL_PAGES && s_pool->owner[end + 1] == s_pool->e[k].pid) end++;
            PUT("%s\"%lu-%lu\"", n++ ? "," : "", (unsigned long)pg, (unsigned long)end);
        }
        PUT("]}");
    }
    PUT("],\"capacity\":%lu,\"free\":%lu", (unsigned long)ORB_POOL_CAP,
        (unsigned long)(s_pool ? orb_pool_free_pages(s_pool) * ORB_POOL_PAGE : 0));
    if (s_fill_slug[0])                        /* §2.4: the fill running, as its face shows it */
        PUT(",\"fill\":{\"app\":\"%s\",\"done\":%lu,\"total\":%lu}", s_fill_slug,
            (unsigned long)s_fill_done, (unsigned long)s_fill_total);
    PUT("}");
#undef PUT
    return o < cap ? o : cap;
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
    clk_row_t     rows[CLK_ROW_MAX];
    clk_comp_t    comps[CLK_COMP_MAX];
    clk_sound_t   sounds[CLK_SND_SLOTS];   /* D39 */
} chk_scratch_t;
static chk_scratch_t *s_chk;

/* D48: a bundle is one app's theme. Its one section is the theme, whatever app it is
 * addressed to; the section's own slug says which app draws it. (The reader still
 * parses the N-section shape until the format reversion lands with its writer and
 * suite; until then the first section is the theme.) */
static const orb_bundle_sec_t *sole_sec(const orb_bundle_t *b)
{
    return (b && b->n_sections >= 1) ? &b->sec[0] : NULL;
}


/* theme-cache-plan §2.6: the store's sounds, taken from its verdict's projection. */
typedef struct { clk_sound_t s[CLK_SND_SLOTS]; uint32_t n; uint8_t pattern; } store_snd_t;
EXT_RAM_BSS_ATTR static store_snd_t s_store_snd;

static const char *clock3_verdict_ex(const orb_bundle_t *b, store_snd_t *snd);
static const char *clock3_verdict(const orb_bundle_t *b) { return clock3_verdict_ex(b, NULL); }
static const char *clock3_verdict_ex(const orb_bundle_t *b, store_snd_t *snd)
{
    const orb_bundle_sec_t *sec = sole_sec(b);
    if (!sec)
        return "the bundle carries no theme section to draw";
    if (!s_verdict_mx || !s_chk) return "out of memory for the verdict lock";
    xSemaphoreTake(s_verdict_mx, portMAX_DELAY);
    orb_clock3_io_t pio;
    memset(&pio, 0, sizeof pio);
    pio.theme = &s_chk->theme;
    pio.rects = s_chk->rects; pio.rect_first = s_chk->first;
    pio.gif_delays = s_chk->delays; pio.bg_frames = s_chk->bgf;
    pio.first_cap = GIFS_MAX + 1; pio.delays_cap = GIFS_MAX;
    pio.comps = s_chk->comps; pio.comp_cap = CLK_COMP_MAX;
    pio.rows  = s_chk->rows;  pio.row_cap   = CLK_ROW_MAX;
    pio.sounds = s_chk->sounds; pio.sound_cap = CLK_SND_SLOTS;   /* D39 */
    const char *r = orb_clock3_parse(sec, &pio);
    if (snd) {
        snd->n = r ? 0 : pio.out_n_sounds;
        snd->pattern = pio.out_pattern;
        memcpy(snd->s, s_chk->sounds, snd->n * sizeof snd->s[0]);
    }
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

static const orb_file_src_t s_any_themes_src;   /* every app's themes: below */

/* The slug-aware twin. `card_open_verified_id3` below is this body with "clock"
 * bound, and every picker-side read (the preview first, and any per-app service
 * that follows it) calls this with whatever slug the handle names — so a weather
 * theme resolves against `orb_files_src("weather", "themes")` under `weather/`
 * without the clock having to register first. The stem's slug and the caller's
 * must agree: an app asking about another app's theme is refused here, before the
 * card is touched, and the refusal words match the ones the id3 door uses for the
 * same defect on the clock's path. */
static int card_open_verified_slug3(const char *app_slug, const char *stem,
                                    orb_bundle3_hdr_t *o, uint32_t *file_size,
                                    const char **why)
{
    *why = NULL;
    char slug[ORB_BUNDLE_SLUG_MAX + 1], id[THEME_ID_HEX + 1];
    if (!theme_id_ok(stem) ||
        !theme_stem_split(stem, slug, sizeof slug, id, sizeof id)) {
        *why = "that theme id is not one this device issued";
        return -1;
    }
    if (strcmp(slug, app_slug) != 0) {
        *why = "that theme belongs to a different app";
        return -1;
    }
    /* theme-pool-plan P5: any installed app's theme, through the app's own source if
     * it declared one, else the OS's shared one; an app that is not installed has
     * nowhere to wear a theme. */
    if (!orb_app_find_slug(app_slug)) { *why = "no installed app wears that theme"; return -1; }
    const orb_file_src_t *src = orb_files_src(app_slug, "themes");
    if (!src) src = &s_any_themes_src;
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = orb_sd_why(); return -1; }

    resolve3_t r = { .want = stem, .found = false, .verified = false };
    if (orb_files_walk(src, resolve3_entry, &r, NULL) < 0) { *why = "no card in the socket"; return -1; }
    if (!r.found)    { *why = "no such theme on the card";                        return -1; }
    if (!r.verified && r.why) { *why = r.why;                                     return -1; }
    if (!r.verified) { *why = "that file's name does not match its id — refused"; return -1; }

    /* card_open_locked3 means what it says: the CALLER holds the card. The walk
     * above took and released the lock itself (it is not recursive), so it is taken
     * here, and card_close() gives it back. Without this every install and preview
     * read ran with no lock and, worse, no tile-ring loan: a listing on another task
     * borrowed and re-funded the ring mid-store, the SD driver's 125 B DMA bounce
     * found nothing, sdmmc_read_blocks failed (0x101), and the store left the slot
     * empty — "bundle ended before its declared pixels" (bench, orb-A, 2026-10-31,
     * a store under preview traffic, the shape of a hub send and its read-ahead). */
    orb_sd_lock();
    const int fd = card_open_locked3(stem, o, file_size, why);
    if (fd < 0) { orb_sd_unlock(); return -1; }
    char id2[THEME_ID_HEX + 1];
    theme_id(slug, o->name, id2);
    if (strcmp(id2, id) != 0) {
        card_close(fd);
        *why = "that file's name does not match its id — refused";
        return -1;
    }
    return fd;
}

/* §60.3: the preview readers' door. card_open_verified_slug3 walks the app's theme folder to
 * find the file by name, and the walk cost ~120 ms of card time — twice per preview, the
 * probe's and the read's (Gold, 2026-11-07). The path is the stem's (theme_file), so this
 * opens it directly; the identity checks are the same ones, on the opened header. */
static int card_open_preview3(const char *app_slug, const char *stem,
                              orb_bundle3_hdr_t *o, uint32_t *file_size, const char **why)
{
    *why = NULL;
    char slug[ORB_BUNDLE_SLUG_MAX + 1], id[THEME_ID_HEX + 1];
    if (!theme_id_ok(stem) ||
        !theme_stem_split(stem, slug, sizeof slug, id, sizeof id)) {
        *why = "that theme id is not one this device issued";
        return -1;
    }
    if (strcmp(slug, app_slug) != 0) { *why = "that theme belongs to a different app"; return -1; }
    if (!orb_app_find_slug(app_slug)) { *why = "no installed app wears that theme"; return -1; }
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = orb_sd_why(); return -1; }
    orb_sd_lock();
    const int fd = card_open_locked3(stem, o, file_size, why);
    if (fd < 0) { orb_sd_unlock(); return -1; }
    char id2[THEME_ID_HEX + 1];
    theme_id(slug, o->name, id2);
    if (strcmp(id2, id) != 0) {
        card_close(fd);
        *why = "that file's name does not match its id — refused";
        return -1;
    }
    return fd;
}

/* A store's and a delete's door: the app is the one the stem names (theme-pool-plan
 * P5; it was the clock alone while the device held one theme). */
static int card_open_verified_id3(const char *stem, orb_bundle3_hdr_t *o, uint32_t *file_size,
                                  const char **why)
{
    char slug[ORB_BUNDLE_SLUG_MAX + 1], id[THEME_ID_HEX + 1];
    if (!theme_id_ok(stem) || !theme_stem_split(stem, slug, sizeof slug, id, sizeof id)) {
        *why = "that theme id is not one this device issued";
        return -1;
    }
    return card_open_verified_slug3(slug, stem, o, file_size, why);
}

/* Transient read of one bundle's geometry region from an OPEN card file. The store
 * path keeps its region resident because a redraw reads it again; a preview read is
 * not on the redraw path, and a resident copy per theme would be 60 KB times the
 * card's contents. This helper allocates a PSRAM scratch at the header's own
 * claim (bounded at ORB_BUNDLE_GEOM_CAP by card_check3 before it reaches here),
 * reads the region from the still-open fd, parses through the shipped reader, and
 * hands back both the parsed index and the scratch that owns the blobs the index
 * points into. The caller frees the scratch when done. */
static const char *region_from_card3(int fd, const orb_bundle3_hdr_t *h,
                                     uint32_t file_size, orb_bundle_t *b,
                                     uint8_t **region_out)
{
    uint8_t *region = heap_caps_malloc(h->geom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) return "out of memory for the bundle's tables";
    size_t pos = h->geom_off;
    if (rd_at(fd, &pos, region, h->geom_size) != (ssize_t)h->geom_size) {
        heap_caps_free(region);
        return "geometry region runs past the end of the file";
    }
    const char *r = orb_bundle_parse_region(h, region, h->geom_size, file_size, b);
    if (r) { heap_caps_free(region); return r; }
    *region_out = region;
    return NULL;
}

/* Transient read of one plane from one section of one open card file. Resolves
 * (cls, slot) at frame `frame` through orb_elems_at — the same resolver the resident
 * layer's apps use, so a plane the theme's own dial could draw is a plane this one
 * can too, and the two answers to a torn row cannot drift. `*w`/`*h` come back on
 * OK and on a room refusal (a caller needs the extent to size a real buffer after
 * a shape probe); they stay zero on every other refusal. A NULL `buf` is the shape
 * probe: verify, report the extent, copy nothing, return OK. Answers map to the
 * registry's vocabulary so a caller branches on the enum, not a sentence: NO_SLOT
 * and EMPTY both mean the theme chose not to carry the row, and NO_CONTENT is
 * exactly what the preview's absence rule (docs/history/theme-preview-plan.md §1) names. */
/* zsize: non-NULL asks for a deflated preview's STORED bytes (§60, the hub's wire form):
 * they are copied as they lie and *zsize says how many; a plane that is not deflated
 * answers ORB_ASSET_CLASS and copies nothing, so the caller reads it the plain way. */
static orb_asset_ans_t read_one_plane_ex(int fd, const orb_bundle_sec_t *sec,
                                         uint16_t cls, uint16_t slot, uint32_t frame,
                                         uint8_t *buf, size_t cap,
                                         uint16_t *w, uint16_t *h,
                                         char *why, size_t whysz, uint32_t *zsize);
static orb_asset_ans_t read_one_plane(int fd, const orb_bundle_sec_t *sec,
                                       uint16_t cls, uint16_t slot, uint32_t frame,
                                       uint8_t *buf, size_t cap,
                                       uint16_t *w, uint16_t *h,
                                       char *why, size_t whysz)
{
    return read_one_plane_ex(fd, sec, cls, slot, frame, buf, cap, w, h, why, whysz, NULL);
}

static orb_asset_ans_t read_one_plane_ex(int fd, const orb_bundle_sec_t *sec,
                                         uint16_t cls, uint16_t slot, uint32_t frame,
                                         uint8_t *buf, size_t cap,
                                         uint16_t *w, uint16_t *h,
                                         char *why, size_t whysz, uint32_t *zsize)
{
    orb_elem_t e;
    char seat_why[192];
    seat_why[0] = '\0';
    const orb_elems_ans_t ea = orb_elems_at(sec, cls, slot, frame, &e,
                                            seat_why, sizeof seat_why);
    orb_asset_ans_t ans;
    if (ea == ORB_ELEMS_NO_SLOT || ea == ORB_ELEMS_EMPTY) ans = ORB_ASSET_NO_CONTENT;
    else if (ea == ORB_ELEMS_TORN || ea == ORB_ELEMS_NO_FRAME || ea == ORB_ELEMS_ARG)
        ans = ORB_ASSET_NO_SLOT;
    else ans = ORB_ASSET_OK;

    if (w) *w = (ans == ORB_ASSET_OK || ans == ORB_ASSET_ROOM) ? e.plane_w : 0;
    if (h) *h = (ans == ORB_ASSET_OK || ans == ORB_ASSET_ROOM) ? e.plane_h : 0;
    if (ans != ORB_ASSET_OK) {
        if (why && whysz) strlcpy(why, seat_why[0] ? seat_why : orb_elems_why(ea), whysz);
        return ans;
    }
    /* A masked row's third stride is not preview bytes. The write door refuses one
     * (theme_schema's slot_bytes check on the class); a reader meets one only off a
     * hand-edited bundle, and refusing beats painting alpha as colour. The row's
     * own extent stays zero on this answer: it is not a preview shape. */
    if (e.masked) {
        if (w) *w = 0;
        if (h) *h = 0;
        if (why && whysz) strlcpy(why,
            "that row carries an alpha stride, which the class never bakes", whysz);
        return ORB_ASSET_CLASS;
    }
    /* A shape probe asks only whether the row is there and how big it is, so it
     * must not be refused for the size it is probing: NULL buf means "verify,
     * report the extent, copy nothing" (the header's contract), and the extent
     * stays reported on this answer. The cap check below is for real reads only. */
    if (!buf) return ORB_ASSET_OK;
    if (e.size > cap) {
        if (why && whysz) snprintf(why, whysz,
            "the plane is %lux%lu (%lu B of RGB565) and the buffer holds %lu B",
            (unsigned long)e.plane_w, (unsigned long)e.plane_h,
            (unsigned long)e.size, (unsigned long)cap);
        return ORB_ASSET_ROOM;
    }

    /* OS v1.2.12: a preview stored as its disc is read into the head of the square's
     * buffer and expanded where it lies, so every caller still gets the square (corners
     * black) it sized by the probe. */
    const bool disc = cls == ORB_ASSET_PREVIEW && e.plane_w == e.plane_h &&
                      e.size == orb_disc_px(e.plane_w) * 2u;
    /* §60: or its disc deflated (orb_elems accepts it by its size), inflated by the ROM's
     * tinfl into the head of the buffer and expanded like a raw disc. */
    const bool zpv = cls == ORB_ASSET_PREVIEW && e.plane_w == e.plane_h &&
                     e.size >= 6u && e.size < orb_disc_px(e.plane_w) * 2u;
    if (zsize) {                                   /* the stored stream, or the plain way */
        if (!zpv) return ORB_ASSET_CLASS;
        size_t zpos = sec->art_off + e.off;
        if (rd_at(fd, &zpos, buf, e.size) != (ssize_t)e.size || !orb_inflate_head_ok(buf, e.size)) {
            if (why && whysz) strlcpy(why, "the preview's compressed plane could not be read", whysz);
            return ORB_ASSET_NO_SLOT;
        }
        *zsize = e.size;
        return ORB_ASSET_OK;
    }
    if ((disc || zpv) && (uint64_t)e.plane_w * e.plane_h * 2u > cap) {
        if (why && whysz) snprintf(why, whysz,
            "the plane is %lux%lu (%lu B of RGB565) and the buffer holds %lu B",
            (unsigned long)e.plane_w, (unsigned long)e.plane_h,
            (unsigned long)(e.plane_w * e.plane_h * 2u), (unsigned long)cap);
        return ORB_ASSET_ROOM;
    }
    size_t pos = sec->art_off + e.off;
    if (zpv) {
        uint8_t *z = heap_caps_malloc(e.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!z) {
            if (why && whysz) strlcpy(why, "no room to read the compressed preview", whysz);
            return ORB_ASSET_NO_SLOT;
        }
        const bool ok = rd_at(fd, &pos, z, e.size) == (ssize_t)e.size &&
                        orb_inflate_head_ok(z, e.size) &&
                        orb_inflate(z, e.size, buf, orb_disc_px(e.plane_w) * 2u);
        heap_caps_free(z);
        if (!ok) {
            if (w) *w = 0;
            if (h) *h = 0;
            if (why && whysz) strlcpy(why,
                "the preview's compressed plane does not inflate to its disc", whysz);
            return ORB_ASSET_NO_SLOT;
        }
        orb_disc_expand_inplace(buf, e.plane_w);
        return ORB_ASSET_OK;
    }
    const ssize_t got = rd_at(fd, &pos, buf, e.size);
    if (got != (ssize_t)e.size) {
        if (w) *w = 0;
        if (h) *h = 0;
        if (why && whysz) strlcpy(why,
            "the plane runs past the end of the file", whysz);
        return ORB_ASSET_NO_SLOT;
    }
    if (disc) orb_disc_expand_inplace(buf, e.plane_w);
    return ORB_ASSET_OK;
}

orb_asset_ans_t orb_theme_preview_read(const char *handle,
                                       uint8_t *buf, size_t cap,
                                       uint16_t *w, uint16_t *h,
                                       char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    if (w) *w = 0;
    if (h) *h = 0;

    char slug[ORB_BUNDLE_SLUG_MAX + 1], hex[THEME_ID_HEX + 1];
    if (!theme_stem_split(handle, slug, sizeof slug, hex, sizeof hex)) {
        if (why && whysz) strlcpy(why,
            "that theme id is not one this device issued", whysz);
        return ORB_ASSET_NO_SLOT;
    }

    orb_bundle3_hdr_t h3;
    uint32_t fsz = 0;
    const char *card_why = NULL;
    const int fd = card_open_preview3(slug, handle, &h3, &fsz, &card_why);
    if (fd < 0) {
        if (why && whysz && card_why) strlcpy(why, card_why, whysz);
        return ORB_ASSET_NO_SLOT;
    }

    orb_bundle_t b;
    uint8_t *region = NULL;
    const char *rv = region_from_card3(fd, &h3, fsz, &b, &region);
    if (rv) {
        card_close(fd);
        if (why && whysz) strlcpy(why, rv, whysz);
        return ORB_ASSET_NO_SLOT;
    }

    const orb_bundle_sec_t *sec = orb_bundle_find(&b, slug);
    if (!sec) {
        heap_caps_free(region);
        card_close(fd);
        if (why && whysz) strlcpy(why,
            "the theme's bundle carries no section for its own stem's app", whysz);
        return ORB_ASSET_NO_SLOT;
    }

    const orb_asset_ans_t ans = read_one_plane(fd, sec,
                                                (uint16_t)ORB_ASSET_PREVIEW, 0, 0,
                                                buf, cap, w, h, why, whysz);
    heap_caps_free(region);
    card_close(fd);
    return ans;
}

/* §60: a deflated preview as it lies on the card, for the hub to inflate (orb_theme.h). */
orb_asset_ans_t orb_theme_preview_z(const char *handle, uint8_t *buf, size_t cap,
                                    uint16_t *w, uint16_t *h, uint32_t *zsize,
                                    char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    if (w) *w = 0;
    if (h) *h = 0;

    char slug[ORB_BUNDLE_SLUG_MAX + 1], hex[THEME_ID_HEX + 1];
    if (!theme_stem_split(handle, slug, sizeof slug, hex, sizeof hex)) {
        if (why && whysz) strlcpy(why,
            "that theme id is not one this device issued", whysz);
        return ORB_ASSET_NO_SLOT;
    }

    orb_bundle3_hdr_t h3;
    uint32_t fsz = 0;
    const char *card_why = NULL;
    const int fd = card_open_preview3(slug, handle, &h3, &fsz, &card_why);
    if (fd < 0) {
        if (why && whysz && card_why) strlcpy(why, card_why, whysz);
        return ORB_ASSET_NO_SLOT;
    }

    orb_bundle_t b;
    uint8_t *region = NULL;
    const char *rv = region_from_card3(fd, &h3, fsz, &b, &region);
    if (rv) {
        card_close(fd);
        if (why && whysz) strlcpy(why, rv, whysz);
        return ORB_ASSET_NO_SLOT;
    }

    const orb_bundle_sec_t *sec = orb_bundle_find(&b, slug);
    if (!sec) {
        heap_caps_free(region);
        card_close(fd);
        if (why && whysz) strlcpy(why,
            "the theme's bundle carries no section for its own stem's app", whysz);
        return ORB_ASSET_NO_SLOT;
    }

    *zsize = 0;
    const orb_asset_ans_t ans = read_one_plane_ex(fd, sec,
                                                  (uint16_t)ORB_ASSET_PREVIEW, 0, 0,
                                                  buf, cap, w, h, why, whysz, zsize);
    heap_caps_free(region);
    card_close(fd);
    return ans;
}

/* §60.2: the two halves the preview caches use (orb_theme.h). The card half is the only one
 * that holds the card; the inflate half is CPU on whichever task calls it. */
uint8_t *orb_theme_preview_zcopy(const char *handle, uint32_t *n, uint16_t *side,
                                 char *why, size_t whysz)
{
    return orb_theme_preview_zcopy_ex(handle, n, side, why, whysz, NULL, NULL);
}

/* §60.3: one segment's card hold. ~35 ms at what the card reads into the bounce, so a turn
 * that lands mid-read waits at most that long for its frame (a whole 190 KB preview held
 * the glass ~200 ms). */
#define ZSEG_BYTES  (32u * 1024u)

uint8_t *orb_theme_preview_zcopy_ex(const char *handle, uint32_t *n, uint16_t *side,
                                    char *why, size_t whysz,
                                    bool (*keep)(void *arg, uint32_t done, uint32_t total),
                                    void *arg)
{
    /* §60.3: one open, one hold. The probe and the read used to be two calls, each its own
     * open, region read and card loan; here the probe sizes the buffer on the same fd. */
    *n = 0;
    *side = 0;
    if (why && whysz) why[0] = '\0';
    char slug[ORB_BUNDLE_SLUG_MAX + 1], hex[THEME_ID_HEX + 1];
    if (!theme_stem_split(handle, slug, sizeof slug, hex, sizeof hex)) {
        if (why && whysz) strlcpy(why, "that theme id is not one this device issued", whysz);
        return NULL;
    }
    orb_bundle3_hdr_t h3;
    uint32_t fsz = 0;
    const char *card_why = NULL;
    const int fd = card_open_preview3(slug, handle, &h3, &fsz, &card_why);
    if (fd < 0) {
        if (why && whysz && card_why) strlcpy(why, card_why, whysz);
        return NULL;
    }
    orb_bundle_t b;
    uint8_t *region = NULL;
    const char *rv = region_from_card3(fd, &h3, fsz, &b, &region);
    if (rv) {
        card_close(fd);
        if (why && whysz) strlcpy(why, rv, whysz);
        return NULL;
    }
    uint8_t *buf = NULL;
    const orb_bundle_sec_t *sec = orb_bundle_find(&b, slug);
    orb_elem_t e;
    char seat_why[96];
    /* The same seat read_one_plane_ex finds, and the same deflated-preview test: anything
     * else (no preview, a raw disc from before v1.2.20) answers NULL for the plain read. */
    if (sec && orb_elems_at(sec, (uint16_t)ORB_ASSET_PREVIEW, 0, 0, &e, seat_why, sizeof seat_why)
                   == ORB_ELEMS_OK &&
        !e.masked && e.plane_w && e.plane_w == e.plane_h &&
        e.size >= 6u && e.size < orb_disc_px(e.plane_w) * 2u)
        buf = heap_caps_malloc(e.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool lost = false;                       /* the card was given back and not retaken */
    if (buf) {
        size_t pos = sec->art_off + e.off;
        uint32_t done = 0;
        while (done < e.size) {
            /* No keep is the boot's carousel load: nothing is on the glass to wait for. */
            const uint32_t most = keep ? ZSEG_BYTES : e.size;
            const uint32_t seg = e.size - done < most ? e.size - done : most;
            if (rd_at(fd, &pos, buf + done, seg) != (ssize_t)seg) break;
            done += seg;
            if (done == e.size) break;
            /* Between segments the card goes back, and with it the ring: the glass gets a
             * frame, and a knob turn its answer. The fd stays open; nothing else holds it. */
            orb_sd_unlock();
            vTaskDelay(pdMS_TO_TICKS(20));
            /* The card was given back, so it may have gone: a pulled card ends the read. */
            const bool go = (!keep || keep(arg, done, e.size)) && !orb_theme_upload_busy() &&
                            orb_sd_ensure_mounted() == ESP_OK;
            orb_sd_lock();
            if (!go) { lost = true; break; }
        }
        if (done != e.size || !orb_inflate_head_ok(buf, e.size)) {
            if (!lost && why && whysz) strlcpy(why, "the preview's compressed plane could not be read", whysz);
            heap_caps_free(buf);
            buf = NULL;
        } else {
            *n = e.size;
            *side = e.plane_w;
        }
    }
    heap_caps_free(region);
    card_close(fd);
    return buf;
}

uint8_t *orb_preview_inflate(const uint8_t *z, uint32_t n, uint16_t side)
{
    if (!z || !n || !side) return NULL;
    uint8_t *sq = heap_caps_malloc((size_t)side * side * 2u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sq) return NULL;
    if (!orb_preview_inflate_into(z, n, side, sq, (size_t)side * side * 2u)) {
        heap_caps_free(sq);
        return NULL;
    }
    return sq;
}

bool orb_preview_inflate_into(const uint8_t *z, uint32_t n, uint16_t side, uint8_t *sq, size_t cap)
{
    if (!z || !n || !side || !sq || cap < (size_t)side * side * 2u) return false;
    if (!orb_inflate_head_ok(z, n) || !orb_inflate(z, n, sq, orb_disc_px(side) * 2u)) return false;
    orb_disc_expand_inplace(sq, side);
    return true;
}

/* D43 P7 (§8): a card theme's app design, streamed back for its editor. The OS never reads
 * the bytes: it checks the pointer against the file (after the banks, inside the file) and
 * hands the header's size and CRC to `begin` before the first byte, so the receiver can
 * verify what arrives. NO_CONTENT when the theme carries none. Holds the card for the
 * stream, as the preview read does. */
orb_asset_ans_t orb_theme_design_stream(const char *handle,
                                        esp_err_t (*begin)(void *arg, uint32_t size, uint32_t crc),
                                        esp_err_t (*chunk)(void *arg, const uint8_t *p, size_t n),
                                        void *arg, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    char slug[ORB_BUNDLE_SLUG_MAX + 1], hex[THEME_ID_HEX + 1];
    if (!theme_stem_split(handle, slug, sizeof slug, hex, sizeof hex)) {
        if (why && whysz) strlcpy(why, "that theme id is not one this device issued", whysz);
        return ORB_ASSET_NO_SLOT;
    }
    orb_bundle3_hdr_t h3;
    uint32_t fsz = 0;
    const char *card_why = NULL;
    const int fd = card_open_verified_slug3(slug, handle, &h3, &fsz, &card_why);
    if (fd < 0) {
        if (why && whysz && card_why) strlcpy(why, card_why, whysz);
        return ORB_ASSET_NO_SLOT;
    }
    if (!h3.design_size) {
        card_close(fd);
        if (why && whysz) strlcpy(why, "this theme carries no design, so it cannot be reopened", whysz);
        return ORB_ASSET_NO_CONTENT;
    }
    if ((uint64_t)h3.design_off < (uint64_t)h3.art_off + h3.art_size ||
        (uint64_t)h3.design_off + h3.design_size > fsz) {
        card_close(fd);
        if (why && whysz) strlcpy(why, "this theme's design runs past the end of the file - it cannot be reopened", whysz);
        return ORB_ASSET_NO_CONTENT;
    }
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { card_close(fd); if (why && whysz) strlcpy(why, "no room for the read buffer", whysz); return ORB_ASSET_NO_SLOT; }
    orb_asset_ans_t ans = ORB_ASSET_OK;
    if (begin(arg, h3.design_size, h3.design_crc) != ESP_OK) ans = ORB_ASSET_NO_SLOT;
    size_t pos = h3.design_off, left = h3.design_size;
    while (ans == ORB_ASSET_OK && left) {
        const size_t n = left < 4096 ? left : 4096;
        if (rd_at(fd, &pos, buf, n) != (ssize_t)n) { ans = ORB_ASSET_NO_SLOT; if (why && whysz) strlcpy(why, "the card could not be read", whysz); break; }
        if (chunk(arg, buf, n) != ESP_OK) { ans = ORB_ASSET_NO_SLOT; break; }
        left -= n;
    }
    heap_caps_free(buf);
    card_close(fd);
    return ans;
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
/* Set when the store's park lands, and never cleared: the unmap follows at once and
 * every path past it ends in the reboot. From here the router may hand the glass to
 * nothing but home (orb_theme_glass_held), because loading any other screen renders
 * from a bank that is about to be, or already is, unmapped. A /api/v1/screen request
 * or a knob turn during a store did exactly that on 2026-11-01 (bench, orb-B). */
static volatile bool s_glass_held;

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
static char s_upload_stage[12];   /* the stage word park() drew; progress() repeats it */

/* The old dial must be off the glass AND out of the renderer before the bank it
 * draws from is unmapped. A fixed wait is not that: on "Kite tourbillon" one LVGL
 * pass runs ~270 ms, the 120 ms below covers less than half of one, and on
 * 2026-10-31 a store unmapped the bank under a render still turning the carriage
 * (transform_rgb565a8, "Cache disabled but cached memory region accessed"). Wait
 * for the switch to the home screen to have landed, then for two whole passes:
 * the first may have begun before this looked, the second began after, on the
 * home screen. Bounded: a renderer that never answers refuses the park. Only the
 * store waits on it: an upload, an OTA or a reboot parks for its overlay and never
 * unmaps, and waiting there would let the park's own switch to the menu land and
 * rewrite the resume record the reboot comes back to. */
static bool glass_settled(const orb_app_t *home)
{
    const int64_t t0 = esp_timer_get_time(), limit = 3000000;
    while (home && input_router_current() != home) {
        if (esp_timer_get_time() - t0 > limit) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const uint32_t p0 = display_lvgl_passes();
    while (display_lvgl_passes() - p0 < 2u) {
        if (esp_timer_get_time() - t0 > limit) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool orb_theme_upload_park(uint32_t total, const char *stage)
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
    strlcpy(s_upload_stage, stage && stage[0] ? stage : "copying", sizeof s_upload_stage);
    orb_boot_overlay_backdrop();
    orb_boot_overlay(s_upload_stage, 0, total, 0);
    return true;
}

void orb_theme_upload_progress(uint32_t done, uint32_t total)
{
    orb_boot_overlay(s_upload_stage, done, total, 0);   /* the overlay's 10 Hz rule applies */
}

void orb_theme_upload_unpark(void)
{
    orb_boot_overlay_release();           /* the dial is back: the band's RAM is too */
    orb_sched_set_running(true);
    if (s_upload_was) input_router_request(s_upload_was);
    s_upload_was = NULL;
}

/* The store parks the glass on the menu for its own reasons (§6.3), and that
 * switch is what a naive resume record would read as "the owner was on the
 * menu". The boot after a store is therefore steered at the clock explicitly
 * (bundle_job); this is where the wrong answer would have been written. */
static bool store_park_dial(const orb_app_t *was, uint32_t total, const char *stage)
{
    orb_sched_set_running(false);
    const orb_app_t *home = orb_app_home();
    if (home) input_router_request(home);
    /* 120 ms: the router's poll period doubled, so the switch to the menu has
     * landed and no tick was mid-frame when the gate dropped. The overlay paints
     * AFTER the dial is hidden, or the switch would paint the menu over it. */
    vTaskDelay(pdMS_TO_TICKS(120));
    if (orb_sched_running() || !glass_settled(home)) {
        /* A resume raced the request (the bounce stands on its own gate), or the
         * renderer never let go of the old dial. The erase cannot be trusted
         * against a compositor that reads the map, so refuse while everything is
         * still whole — and put the owner's screen back exactly as they left it. */
        if (was) input_router_request(was);
        return false;
    }
    s_glass_held = true;
    orb_boot_overlay_backdrop();
    orb_boot_overlay(stage, 0, total, 0);
    return true;
}

bool orb_theme_glass_held(void) { return s_glass_held || s_slot_torn; }

/* One chunk boundary of the copy (§6.3): the glass says what the row and the
 * ledger say, and the overlay's own 10 Hz throttle keeps the panel from
 * rate-limiting the flash write. */
/* The store's stage as the hub reads it (orb_theme_job_stage). The stage pointer goes
 * last so a reader never pairs a new stage with the last stage's counts for long. */
static const char *volatile s_job_stage = "";
static volatile uint32_t    s_job_done, s_job_total;
static void job_stage(const char *stage, uint32_t done, uint32_t total)
{
    s_job_done = done;
    s_job_total = total;
    s_job_stage = stage;
}

const char *orb_theme_job_stage(uint32_t *done, uint32_t *total)
{
    const char *st = s_job_stage;
    if (done) *done = s_job_done;
    if (total) *total = s_job_total;
    return st;
}

static void store_progress(void *arg, uint32_t done, uint32_t total)
{
    (void)arg;
    job_stage("storing", done, total);
    orb_boot_overlay("storing", done, total, 0);
}

/* The pool's moves on the glass: a compaction carries megabytes (Bronze, 2026-11-04:
 * 2.8 MB in about 20 s), so a store making room and a boot repairing a cut say so with
 * the overlay's bar rather than standing on a word that has stopped meaning anything. */
static void pool_progress(const char *stage, uint32_t done, uint32_t total)
{
    /* The overlay repaints at once on any new byte count, and a move reports every
     * sector; a paint per sector cost a 2.8 MB move about ten seconds (Bronze,
     * 2026-11-04: a store 42.8 s without the bar, 52.1 s with it). One paint per
     * percent, and always the last. */
    static uint32_t last_pct = 101;
    const uint32_t pct = total ? (uint32_t)((uint64_t)done * 100u / total) : 100u;
    if (pct == last_pct && done != total) return;
    last_pct = done == total ? 101 : pct;
    orb_boot_overlay(stage, done, total, 0);
}

static void room_progress(void *ctx, uint32_t done, uint32_t total)
{
    (void)ctx;
    job_stage("making room", done, total);
    pool_progress("making room", done, total);
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
/* theme-cache-plan §4.1 (C7): an entry's bytes go through its pages, the pool's io under
 * them; esp_err_t answers, as the partition calls they replace gave. */
static esp_err_t pg_read(const orb_pool_pages_t *pg, uint32_t off, void *dst, uint32_t n)
{ return orb_pool_pread(&s_pool_io, pg, off, dst, n) ? ESP_FAIL : ESP_OK; }
static esp_err_t pg_write(const orb_pool_pages_t *pg, uint32_t off, const void *src, uint32_t n)
{ return orb_pool_pwrite(&s_pool_io, pg, off, src, n) ? ESP_FAIL : ESP_OK; }
static esp_err_t pg_erase(const orb_pool_pages_t *pg, uint32_t off, uint32_t n)
{ return orb_pool_perase(&s_pool_io, pg, off, n) ? ESP_FAIL : ESP_OK; }

/* theme-cache-plan §2.4: how far a fill got, so one paused when the owner left its app goes
 * on from there when they come back (ruling 5). The worker is never held for a pause — a
 * store or a settings write would queue behind it — so the job ends and this stays. Good
 * only while the pool's table is the one it was made under (`table_crc`) and the slot is
 * where it was. */
typedef struct {
    char     slug[ORB_POOL_SLUG_MAX];
    uint32_t file_crc, at, table_crc;   /* at: the slot it writes */
    uint32_t erased;            /* art bytes erased from SLOT_ART_OFF */
    uint32_t sec, sec_done;     /* the section being copied, and its bytes written */
    bool     valid;
} fill_mark_t;
EXT_RAM_BSS_ATTR static fill_mark_t s_fill_mark;
static volatile bool s_fill_stop;              /* the owner left the app being filled */

#define FILL_ERASE_STEP (64u * 1024u)          /* a fill erases this far ahead of its writes */

static esp_err_t copy_bundle3(int fd, const orb_bundle_t *b, const uint8_t *region,
                              uint32_t file_art_base, const orb_pool_pages_t *pg,
                              uint32_t *out_art_crc,
                              const char **why,
                              void (*progress)(void *arg, uint32_t done, uint32_t total),
                              void *parg, fill_mark_t *mark)
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
     * the card and in nothing else.
     *
     * D9's arm lives on the FIRST of those erases (moved out of
     * orb_theme_upload_begin() on 2026-10-27, where every killed upload cost
     * the owner the live face). The slot's own header goes down ahead of the
     * payload region, so a store torn at any later point — power, panic, a
     * flash that will not write — boots the base face rather than mapping half
     * an image under a header that described the theme before it. The header is
     * rewritten last, by the adopt stamp at the end of this store. */
    /* theme-cache-plan §2.4: a fill (mark) erases its pixels a block ahead of its writes, so
     * its face's ring moves the whole way rather than sitting at nought through one long
     * erase; it may also go on from where a pause left it (mark->valid). */
    const bool resume = mark && mark->valid;
    esp_err_t e = ESP_OK;
    if (!resume) {
        e = pg_erase(pg, SLOT_HDR, THEME_SECTOR);
        if (e == ESP_OK) e = pg_erase(pg, SLOT_GEOM_OFF, geom_len);
        if (e == ESP_OK && !mark) e = pg_erase(pg, SLOT_ART_OFF, art_len);
        if (e != ESP_OK) { *why = "flash erase failed"; return e; }
        if (mark) { mark->erased = 0; mark->sec = 0; mark->sec_done = 0; }
    }
    if (progress) progress(parg, 0, art_len);

    for (uint32_t done = 0; !resume && done < b->region_size; ) {
        const uint32_t want = (b->region_size - done > s_chunk_bytes) ? s_chunk_bytes
                                                                      : (b->region_size - done);
        e = pg_write(pg, SLOT_GEOM_OFF + done, region + done, want);
        if (e != ESP_OK) { *why = "flash write failed on the geometry"; return e; }
        done += want;
    }

    for (uint32_t i = 0; i < b->n_sections; i++) {
        const orb_bundle_sec_t *s = &b->sec[i];
        size_t   pos = file_art_base + (s->art_off - SLOT_ART_OFF);
        uint32_t crc = 0, done = 0;
        /* a resumed fill reads what it already wrote back off the flash for the section's
         * checksum, never the card again */
        const uint32_t kept = !resume || i > mark->sec ? 0 : (i < mark->sec ? s->art_size : mark->sec_done);
        while (done < kept) {
            const uint32_t want = (kept - done > s_chunk_bytes) ? s_chunk_bytes : (kept - done);
            if (pg_read(pg, s->art_off + done, s_chunk, want) != ESP_OK) {
                *why = "could not read the paused fill back"; return ESP_FAIL;
            }
            crc = esp_rom_crc32_le(crc, s_chunk, want);
            done += want;
        }
        pos += done;
        if (progress && kept) progress(parg, s->art_off - SLOT_ART_OFF + done, art_len);
        while (done < s->art_size) {
            const uint32_t want = (s->art_size - done > s_chunk_bytes) ? s_chunk_bytes
                                                                       : (s->art_size - done);
            if (mark) {
                if (s_fill_stop) {               /* ruling 5: the owner left; go on later */
                    mark->sec = i; mark->sec_done = done; mark->valid = true;
                    *why = "paused: the owner left the app";
                    return ESP_ERR_TIMEOUT;
                }
                const uint32_t need = s->art_off - SLOT_ART_OFF + done + want;
                while (mark->erased < need && mark->erased < art_len) {
                    const uint32_t n = art_len - mark->erased > FILL_ERASE_STEP ? FILL_ERASE_STEP
                                                                                : art_len - mark->erased;
                    e = pg_erase(pg, SLOT_ART_OFF + mark->erased, n);
                    if (e != ESP_OK) { *why = "flash erase failed"; return e; }
                    mark->erased += n;
                }
            }
            if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want) {
                *why = "bundle ended before its declared pixels"; return ESP_ERR_INVALID_SIZE;
            }
            crc = esp_rom_crc32_le(crc, s_chunk, want);
            e = pg_write(pg, s->art_off + done, s_chunk, want);
            if (e != ESP_OK) { *why = "flash write failed on the pixels"; return e; }
            done += want;
            if (progress) progress(parg, s->art_off - SLOT_ART_OFF + done, art_len);
        }
        const char *rw = orb_bundle_bank_rule(s, done, crc);
        if (rw) { *why = rw; return ESP_ERR_INVALID_CRC; }
    }
    /* the padding past the last section reads back erased, as a store's does */
    while (mark && mark->erased < art_len) {
        const uint32_t n = art_len - mark->erased > FILL_ERASE_STEP ? FILL_ERASE_STEP : art_len - mark->erased;
        e = pg_erase(pg, SLOT_ART_OFF + mark->erased, n);
        if (e != ESP_OK) { *why = "flash erase failed"; return e; }
        mark->erased += n;
    }
    if (mark) mark->valid = false;

    uint32_t acrc = 0;
    for (uint32_t done = 0; done < art_len; ) {
        const uint32_t want = (art_len - done > s_chunk_bytes) ? s_chunk_bytes : (art_len - done);
        if (pg_read(pg, SLOT_ART_OFF + done, s_chunk, want) != ESP_OK) {
            *why = "could not read the installed pixels back"; return ESP_FAIL;
        }
        acrc = esp_rom_crc32_le(acrc, s_chunk, want);
        done += want;
    }
    *out_art_crc = acrc;
    return ESP_OK;
}

#if CONFIG_ORB_DEBUG_HTTP
/* ---- theme-cache-plan C0: a fill's cost under a running dial ------------------------- */
static char s_bench_fill_last[400];        /* the last run's line, for the door */
const char *orb_theme_bench_fill_last(void) { return s_bench_fill_last; }

/* theme-cache-plan C5 bench: drop `slug`'s display entry as an eviction would (its voice bank
 * and its choice stay), then reboot — the entries after it move, and some may be mapped. */
static void bench_evict_job(void *arg)
{
    const char *slug = arg;
    const esp_partition_t *p = art_bank_partition();
    int r = ORB_POOL_ERR_ARG;
    /* the dial parks first, as a removal's does: an unmap under a drawing dial panicked
     * (Bronze 2026-11-08, "Cache disabled but cached memory region accessed") */
    if (p && s_pool && pool_io(p) && store_park_dial(input_router_current(), 0, "removing")) {
        orb_sound_stop_all();
        art_bank_unmap();
        s_slot_torn = true;
        r = orb_pool_remove_kind(&s_pool_io, s_pool, &s_pool_which, slug, ORB_POOL_DISPLAY);
    }
    ESP_LOGW(TAG, "bench: %s's display entry %s; rebooting", slug, r ? "would not go" : "left the pool");
    (void)orb_jobs_reboot_soon("bench: an eviction");
}

/* C5c bench: make `slug`'s choice disagree with its file, as an edit on the card behind the
 * orb's back would, and have the reconcile look again. */
esp_err_t orb_theme_bench_stale(const char *slug)
{
    for (uint32_t c = 0; slug && c < s_cache.n_choice; c++)
        if (!strcmp(s_cache.c[c].slug, slug)) {
            s_cache.c[c].file_crc ^= 1u;
            s_reconciled_gen--;
            return ESP_OK;
        }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t orb_theme_bench_evict(const char *slug)
{
    char a[ORB_POOL_SLUG_MAX];
    if (!slug || !s_pool || orb_pool_find_kind(s_pool, slug, ORB_POOL_DISPLAY) < 0) return ESP_ERR_NOT_FOUND;
    strlcpy(a, slug, sizeof a);
    return orb_jobs_post(bench_evict_job, a, sizeof a);
}

esp_err_t orb_theme_bench_fill(const char *id, uint32_t chunk_kb, uint32_t pace_ms, bool urgent)
{
    /* C0's copy bench wrote into the packed pool's free run by offset; its figures are in
     * theme-cache-plan §6, and under format 3 (§4.1) a fill is the thing to time. */
    (void)id; (void)chunk_kb; (void)pace_ms; (void)urgent;
    snprintf(s_bench_fill_last, sizeof s_bench_fill_last, "retired with format 3: time a fill");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

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


/* ---- theme-cache-plan §2.6: the voice bank -------------------------------------------- */

/* The bank a store's sounds make, in bytes; 0 for none, or for more than a bank holds. */
static uint32_t voice_bank_size(const store_snd_t *snd)
{
    if (!snd->n) return 0;
    orb_voice_in_t in[CLK_SND_SLOTS];
    for (uint32_t k = 0; k < snd->n; k++) {
        in[k].s = snd->s[k];
        in[k].bytes = snd->s[k].enc == CLK_SND_ENC_ADPCM ? orb_adpcm_bytes(snd->s[k].n) : 2u * snd->s[k].n;
    }
    EXT_RAM_BSS_ATTR static uint8_t head[ORB_VOICE_HEAD_BYTES + ORB_VOICE_MAX * ORB_VOICE_REC_BYTES];
    return orb_voice_build(in, snd->n, snd->pattern, 0, head, sizeof head);
}

/* The app's voice bank, made from its theme's sounds — read from the display entry just
 * written (its pages `src`), or straight from the card file (`fd` >= 0, its banks at
 * `file_art_base`, as copy_bundle3 reads them) when a store writes no display entry (owner,
 * 2026-11-09: a store does not reboot) — or its old one gone for a theme with no sounds.
 * Format 3 (§4.1): the bank's pages are free pages, so nothing a reader holds moves. */
EXT_RAM_BSS_ATTR static orb_pool_pages_t s_voice_pg;
EXT_RAM_BSS_ATTR static orb_pool_pages_t s_job_pg;      /* the install's entry, old then new */
static void voice_add(const char *slug);
static esp_err_t store_voice(const char *slug, const char *name, const orb_pool_pages_t *src,
                             int fd, uint32_t file_art_base,
                             uint32_t sec_art_off, uint32_t src_crc, uint32_t serial, const char **w)
{
    const store_snd_t *snd = &s_store_snd;
    if (!snd->n) {
        if (orb_pool_find_kind(s_pool, slug, ORB_POOL_VOICE) >= 0 &&
            orb_pool_remove_kind(&s_pool_io, s_pool, &s_pool_which, slug, ORB_POOL_VOICE)) {
            *w = "its old voice bank would not go";
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    orb_voice_in_t in[CLK_SND_SLOTS];
    for (uint32_t k = 0; k < snd->n; k++) {
        in[k].s = snd->s[k];
        in[k].bytes = snd->s[k].enc == CLK_SND_ENC_ADPCM ? orb_adpcm_bytes(snd->s[k].n) : 2u * snd->s[k].n;
    }
    EXT_RAM_BSS_ATTR static uint8_t head[ORB_VOICE_HEAD_BYTES + ORB_VOICE_MAX * ORB_VOICE_REC_BYTES];
    EXT_RAM_BSS_ATTR static orb_voice_t v;
    const uint32_t total = orb_voice_build(in, snd->n, snd->pattern, src_crc, head, sizeof head);
    if (!total || orb_voice_parse(head, total, &v)) { *w = "its sounds would not make a bank"; return ESP_ERR_INVALID_SIZE; }
    const char *ord[CACHE_APPS];
    const uint32_t nord = order_list(ord);
    char gone[ORB_POOL_ENTRIES][ORB_POOL_SLUG_MAX];
    uint32_t ngone = 0;
    orb_pool_pages_t *pg = &s_voice_pg;
    const int r = orb_pool_reserve(&s_pool_io, s_pool, &s_pool_which, slug, ORB_POOL_VOICE, total,
                                   ord, nord, slug, pg, gone, &ngone);
    for (uint32_t i = 0; i < ngone; i++)
        ESP_LOGI(TAG, "making room for %s's sounds: %s's theme left the pool", slug, gone[i]);
    if (r) { *w = r == ORB_POOL_ERR_FULL ? "no room for its sounds" : "the theme area would not make room for its sounds"; return ESP_FAIL; }
    const uint32_t size = orb_pool_round(total), hb = orb_voice_head_bytes(snd->n);
    if (ensure_chunk() != ESP_OK) { *w = "out of memory for the copy"; return ESP_ERR_NO_MEM; }
    esp_err_t e = pg_erase(pg, 0, size);
    if (e == ESP_OK) e = pg_write(pg, 0, head, hb);
    for (uint32_t i = 0; e == ESP_OK && i < v.n; i++) {
        for (uint32_t done = 0; e == ESP_OK && done < v.bytes[i]; ) {
            const uint32_t want = v.bytes[i] - done > s_chunk_bytes ? s_chunk_bytes : v.bytes[i] - done;
            if (fd >= 0) {
                size_t pos = file_art_base + (sec_art_off - SLOT_ART_OFF) + snd->s[i].pcm_off + done;
                e = rd_at(fd, &pos, s_chunk, want) == (ssize_t)want ? ESP_OK : ESP_FAIL;
            } else {
                e = pg_read(src, sec_art_off + snd->s[i].pcm_off + done, s_chunk, want);
            }
            if (e == ESP_OK) e = pg_write(pg, v.s[i].pcm_off + done, s_chunk, want);
            done += want;
        }
    }
    if (e != ESP_OK) { *w = "its sounds would not copy"; return e; }
    orb_pool_entry_t ve;
    memset(&ve, 0, sizeof ve);
    strlcpy(ve.slug, slug, sizeof ve.slug);
    strlcpy(ve.name, name, sizeof ve.name);
    ve.size = size;
    ve.geom_size = hb; ve.art_off = v.data_off; ve.art_size = v.data_bytes;
    ve.file_crc = src_crc; ve.serial = serial; ve.kind = ORB_POOL_VOICE;
    if (orb_pool_pcrc(&s_pool_io, pg, 0, ve.geom_size, &ve.geom_crc) ||
        orb_pool_pcrc(&s_pool_io, pg, ve.art_off, ve.art_size, &ve.art_crc) ||
        orb_pool_commit(&s_pool_io, s_pool, &s_pool_which, &ve, pg)) {
        *w = "its voice bank would not be named";
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s's voice bank: %u sounds, %u KB on %u pages from 0x%x", slug, (unsigned)v.n,
             (unsigned)(total / 1024), (unsigned)pg->n, (unsigned)pg->off[0]);
    return ESP_OK;
}

/* theme-cache-plan ruling 2: while the carousel or the fill's face has the glass (orb_strip)
 * and owes a frame — a moving carousel, or a frame the held card refused — the fill lets the
 * card go at its next chunk until the frame is out (250 ms at most: an 80 ms cap let the
 * copy's flash writes back in mid-frame, and their cache stalls doubled the compose). The card lock carries the
 * display's DMA ring (orb_sd_lock), and held from open to close it starved the glass (Bronze
 * 2026-11-08: 9 face frames in a 33 s fill); let go after every 4 KB chunk it took the fill
 * from 14 s to 24. */
static volatile bool s_glass_up, s_glass_owed;
void orb_theme_fill_glass(bool up, bool owed) { s_glass_up = up; s_glass_owed = owed; }

static void fill_show(uint32_t done, uint32_t total);
static void fill_progress(void *arg, uint32_t done, uint32_t total)
{
    (void)arg;
    fill_show(done, total);     /* §4.1: nothing moves, so the copy is the whole of a fill */
}

static void fill_show(uint32_t done, uint32_t total)
{
    s_fill_done = done;
    s_fill_total = total;
    /* the file stays open; a card pulled meanwhile fails the next read, and the fill with it */
    if (!s_glass_up || !s_glass_owed || s_fill_stop) return;
    orb_sd_unlock();
    const int64_t t0 = esp_timer_get_time();
    while (s_glass_up && s_glass_owed && !s_fill_stop && esp_timer_get_time() - t0 < 250000)
        vTaskDelay(1);
    (void)orb_sd_ensure_mounted();      /* a card pulled meanwhile: the next read says so */
    orb_sd_lock();
}

/* ---- owner, 2026-11-09: each chosen app's preview, resident in flash ----------------------
 * The theme's full-glass preview, deflated as its bundle carries it, in an entry of its own
 * that is never evicted (ORB_POOL_PREVIEW): the carousel reads it from flash — fast, and
 * without the card lock that lends the display's DMA ring — and builds its thumbnails when it
 * opens, keeping nothing while it is shut. Written by a store and, for a choice that has none
 * (or one from another file), by the reconcile. The entry: a 16-byte head (magic, side,
 * bytes), then the bytes. */
#define PREVIEW_MAGIC 0x31565250u                   /* "PRV1" */
static void preview_unmap(const char *slug);
static void preview_map(const char *slug);
EXT_RAM_BSS_ATTR static orb_pool_pages_t s_prev_pg;

/* Worker only, the card not held by the caller (the read takes it). */
static esp_err_t preview_store(const char *slug, const char *stem, uint32_t file_crc)
{
    uint32_t zn = 0;
    uint16_t side = 0;
    char why[96] = "";
    uint8_t *z = orb_theme_preview_zcopy(stem, &zn, &side, why, sizeof why);
    if (!z) {
        ESP_LOGW(TAG, "%s's preview: none to keep from \"%s\" (%s)", slug, stem, why[0] ? why : "no deflated preview");
        return ESP_ERR_NOT_FOUND;
    }
    const char *ord[CACHE_APPS];
    const uint32_t nord = order_list(ord);
    orb_pool_pages_t *pg = &s_prev_pg;
    const uint32_t total = 16u + zn;
    preview_unmap(slug);                             /* its old entry's pages are about to go */
    /* room never taken from the theme bound now: its banks are mapped and drawing */
    const char *keep = (s_active_valid && s_live_sec) ? s_live_sec->slug : slug;
    int r = orb_pool_reserve(&s_pool_io, s_pool, &s_pool_which, slug, ORB_POOL_PREVIEW, total,
                             ord, nord, keep, pg, NULL, NULL);
    esp_err_t e = r ? ESP_FAIL : ESP_OK;
    const uint32_t head[4] = { PREVIEW_MAGIC, side, zn, 0 };
    if (e == ESP_OK) e = pg_erase(pg, 0, orb_pool_round(total));
    if (e == ESP_OK) e = pg_write(pg, 0, head, sizeof head);
    if (e == ESP_OK) e = pg_write(pg, 16, z, zn);
    heap_caps_free(z);
    orb_pool_entry_t pe;
    memset(&pe, 0, sizeof pe);
    strlcpy(pe.slug, slug, sizeof pe.slug);
    strlcpy(pe.name, stem, sizeof pe.name);
    pe.size = orb_pool_round(total);
    pe.geom_size = 16; pe.art_off = 16; pe.art_size = zn;
    pe.file_crc = file_crc; pe.kind = ORB_POOL_PREVIEW;
    if (e != ESP_OK || orb_pool_pcrc(&s_pool_io, pg, 0, 16, &pe.geom_crc) ||
        orb_pool_pcrc(&s_pool_io, pg, 16, zn, &pe.art_crc) ||
        orb_pool_commit(&s_pool_io, s_pool, &s_pool_which, &pe, pg)) {
        ESP_LOGE(TAG, "%s's preview would not store (%d)", slug, r);
        return ESP_FAIL;
    }
    preview_map(slug);
    ESP_LOGI(TAG, "%s's preview kept in flash: %u B on %u page%s", slug, (unsigned)zn,
             (unsigned)pg->n, pg->n == 1 ? "" : "s");
    return ESP_OK;
}

/* Each preview entry has a window of its own, mapped from a task on an internal stack (boot,
 * the worker), so the carousel's preview task — whose stack is PSRAM, where a flash read
 * (it turns the caches off) is not allowed — reads it as plain memory. The lock keeps a window
 * from coming down while that task reads it. */
typedef struct { char slug[ORB_POOL_SLUG_MAX]; uint32_t file_crc; orb_pagemap_t pm; } prev_reg_t;
EXT_RAM_BSS_ATTR static prev_reg_t s_prevs[CACHE_APPS];
EXT_RAM_BSS_ATTR static StaticSemaphore_t s_prev_mx_buf;
static SemaphoreHandle_t s_prev_mx;

static void prev_lock(void)
{
    if (!s_prev_mx) s_prev_mx = xSemaphoreCreateMutexStatic(&s_prev_mx_buf);
    xSemaphoreTake(s_prev_mx, portMAX_DELAY);
}
static void prev_unlock(void) { xSemaphoreGive(s_prev_mx); }

/* `slug`'s window down (its entry is about to go). Internal-stack tasks only. */
static void preview_unmap(const char *slug)
{
    prev_lock();
    for (uint32_t i = 0; i < CACHE_APPS; i++)
        if (s_prevs[i].slug[0] && !strcmp(s_prevs[i].slug, slug)) {
            orb_pageunmap(&s_prevs[i].pm);
            memset(&s_prevs[i], 0, sizeof s_prevs[i]);
        }
    prev_unlock();
}

/* `slug`'s entry mapped and listed. Internal-stack tasks only. */
static void preview_map(const char *slug)
{
    const esp_partition_t *p = art_bank_partition();
    const int k = (p && s_pool) ? orb_pool_find_kind(s_pool, slug, ORB_POOL_PREVIEW) : -1;
    if (k < 0) return;
    preview_unmap(slug);
    EXT_RAM_BSS_ATTR static orb_pool_pages_t mpg;
    orb_pool_pages(s_pool, (uint32_t)k, &mpg);
    prev_lock();
    for (uint32_t i = 0; i < CACHE_APPS; i++) {
        if (s_prevs[i].slug[0]) continue;
        if (orb_pagemap(p, mpg.off, mpg.n, &s_prevs[i].pm) == ESP_OK) {
            s_prevs[i].file_crc = s_pool->e[k].file_crc;
            strlcpy(s_prevs[i].slug, slug, sizeof s_prevs[i].slug);
        }
        break;
    }
    prev_unlock();
}

static void previews_map(void)
{
    for (uint32_t k = 0; k < s_pool->n; k++)
        if (s_pool->e[k].kind == ORB_POOL_PREVIEW) preview_map(s_pool->e[k].slug);
}

const uint8_t *orb_theme_preview_hold(const char *slug, uint32_t *n, uint16_t *side)
{
    *n = 0; *side = 0;
    if (!slug) return NULL;
    uint32_t want = 0;                               /* only the preview of what it wears */
    bool chosen = false;
    for (uint32_t c = 0; c < s_cache.n_choice; c++)
        if (!strcmp(s_cache.c[c].slug, slug)) { want = s_cache.c[c].file_crc; chosen = true; }
    if (!chosen) return NULL;
    prev_lock();
    for (uint32_t i = 0; i < CACHE_APPS; i++) {
        const prev_reg_t *r = &s_prevs[i];
        if (!r->slug[0] || strcmp(r->slug, slug) || r->file_crc != want || !r->pm.ptr) continue;
        const uint32_t *head = r->pm.ptr;
        if (head[0] != PREVIEW_MAGIC || !head[2] || !head[1] || head[1] > 466u ||
            16u + head[2] > r->pm.pages * ORB_POOL_PAGE)
            break;
        *n = head[2];
        *side = (uint16_t)head[1];
        return (const uint8_t *)r->pm.ptr + 16;      /* held: orb_theme_preview_release */
    }
    prev_unlock();
    return NULL;
}

void orb_theme_preview_release(void) { prev_unlock(); }

/* ---- owner, 2026-11-09: a store chooses, and does not reboot ------------------------------
 *
 * Under format 3 (§4.1) nothing a reader holds ever moves, so a store need not park the dial
 * and reboot. It lets the app's old theme go on the LVGL task (its stage rebinds to the bare
 * face, its voices stop, its voice bank's window comes down), then on the worker drops the
 * old copies, records the choice, and makes the new voice bank from the card file (sounds
 * play off the glass, ruling 6, so the bank is made now). The display entry is the cache's:
 * it fills when the app is next on the glass, now if it is. */
static SemaphoreHandle_t s_choose_sem;
static void voice_drop(const char *slug);

static void choose_release(void *arg)                 /* LVGL task */
{
    const char *slug = arg;
    orb_app_t *app = (orb_app_t *)orb_app_find_slug(slug);
    if (app) orb_sound_stop_owner(app);               /* its voices and its schedule */
    voice_drop(slug);
    if (s_active_valid && s_live_sec && !strcmp(s_live_sec->slug, slug)) {
        if (app) orb_wasm_theme_release(app);        /* the stage rebinds: bare, unmapped */
        else { use_no_art(); art_bank_unmap(); s_live_key = 0; }
    }
    xSemaphoreGive(s_choose_sem);
}

static esp_err_t store_choose(const char *slug, const char *id, const char *name, int fd,
                              uint32_t file_art_base, uint32_t sec_art_off, uint32_t file_crc,
                              char *why, size_t whysz)
{
    if (!s_choose_sem) s_choose_sem = xSemaphoreCreateBinary();
    if (!s_choose_sem) return install_failed(ESP_ERR_NO_MEM, "out of memory for the store", why, whysz);
    job_stage("letting the old theme go", 0, 0);
    strlcpy(s_hide_slug, slug, sizeof s_hide_slug);
    (void)xSemaphoreTake(s_choose_sem, 0);
    lv_async_call(choose_release, s_hide_slug);
    if (xSemaphoreTake(s_choose_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        s_hide_slug[0] = '\0';
        return install_failed(ESP_ERR_TIMEOUT, "the dial would not let the old theme go", why, whysz);
    }
    job_stage("storing its sounds", 0, 0);
    const char *w = NULL;
    if (orb_pool_remove_kind(&s_pool_io, s_pool, &s_pool_which, slug, ORB_POOL_DISPLAY) ||
        orb_pool_remove_kind(&s_pool_io, s_pool, &s_pool_which, slug, ORB_POOL_VOICE)) {
        s_hide_slug[0] = '\0';
        return install_failed(ESP_FAIL, "the theme area's table would not let the old theme go", why, whysz);
    }
    choice_set(slug, id, file_crc);                   /* theme-cache-plan §2.1: what it wears */
    if (store_voice(slug, name, NULL, fd, file_art_base, sec_art_off, file_crc, 1, &w) != ESP_OK)
        ESP_LOGE(TAG, "\"%s\": its voice bank: %s — its sounds are missing until it is sent again",
                 name, w ? w : "failed");
    else
        voice_add(slug);
    s_hide_slug[0] = '\0';
    strlcpy(s_refill_slug, slug, sizeof s_refill_slug);   /* on the glass: it fills there */
    staged_clear();
    ESP_LOGI(TAG, "stored \"%s\" (%s) for %s: chosen, no reboot; it fills from the card when "
                  "%s is on the glass", name, id, slug, slug);
    return ESP_OK;
}

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
    if (s_install_fill) {
        /* a fill copies the file the app chose; a file changed on the card is a store's
         * (its voice bank is rebuilt too) — theme-cache-plan ruling 7, owed in C5c */
        uint32_t c = 0;
        while (c < s_cache.n_choice && strcmp(s_cache.c[c].stem, id)) c++;
        if (c == s_cache.n_choice || s_cache.c[c].file_crc != file_geom_crc) {
            heap_caps_free(region); card_close(fd);
            s_fill_changed = true;             /* the reconcile stores it again (ruling 7) */
            return install_failed(ESP_ERR_INVALID_STATE, "the theme changed on the card since it "
                                  "was chosen: it is stored again", why, whysz);
        }
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
        const char *cv = clock3_verdict_ex(&b, &s_store_snd);
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
    uint32_t art_span = 0;
    {
        uint32_t art_end = SLOT_ART_OFF;
        for (uint32_t i = 0; i < b.n_sections; i++) {
            const uint32_t end = b.sec[i].art_off + b.sec[i].art_size;
            if (end > art_end) art_end = end;
        }
        art_span = (art_end - SLOT_ART_OFF + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);
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

    /* The file is filed under one app and must theme that app: the card's id hashes
     * the stem's slug, and the pool files the theme under its section's. */
    {
        char stem_slug[ORB_BUNDLE_SLUG_MAX + 1], stem_id[THEME_ID_HEX + 1];
        if (!theme_stem_split(id, stem_slug, sizeof stem_slug, stem_id, sizeof stem_id) ||
            strcmp(stem_slug, b.sec[0].slug) != 0) {
            heap_caps_free(region); card_close(fd);
            return install_failed(ESP_ERR_INVALID_ARG, "this file themes another app than "
                                  "its name says", why, whysz);
        }
    }

    /* The pool's answer, before anything is parked: whether this app's theme fits
     * once its old one is gone (docs/theme-pool-plan.md §2.2). The entry is laid out as
     * the slot was, so its size is the slot's head and its banks. */
    const char *slug = b.sec[0].slug;
    const uint32_t entry_size = SLOT_ART_OFF + art_span;
    const uint32_t sec_art_off = b.sec[0].art_off;      /* the sounds' bank, inside the entry */
    /* theme-cache-plan §2.6: its sounds become the app's voice bank, which must fit too */
    const uint32_t voice_size = voice_bank_size(&s_store_snd);
    if (s_store_snd.n && !voice_size) {
        heap_caps_free(region); card_close(fd);
        return install_failed(ESP_ERR_INVALID_SIZE, "this theme's sounds are more than an app's "
                              "voice bank holds (1 MB)", why, whysz);
    }
    {
        const int old_v = s_pool ? orb_pool_find_kind(s_pool, slug, ORB_POOL_VOICE) : -1;
        /* a fill makes a voice bank only for an app with none (§4.1); a store makes it anew */
        const uint32_t v_need = (s_install_fill && old_v >= 0) ? 0 : orb_pool_round(voice_size);
        /* theme-cache-plan C4: a full pool is not a refusal. The least recently worn
         * apps' display entries give way (their files stay on the card); only a theme
         * bigger than the pool with every voice bank in it is refused. */
        const char *ord[CACHE_APPS];
        const uint32_t nord = order_list(ord);
        uint32_t mask = 0;
        int f = (s_pool && pool_io(p))
                ? orb_pool_evictions_kind(s_pool, slug, ORB_POOL_DISPLAY,
                                          orb_pool_round(entry_size) + v_need -
                                          (old_v >= 0 && v_need ? s_pool->e[old_v].size : 0),
                                          ord, nord, NULL, &mask)
                : ORB_POOL_ERR_ARG;
        if (f) {
            char sent[96];
            if (f == ORB_POOL_ERR_FULL)
                snprintf(sent, sizeof sent, "this theme needs %u KB and the theme area has "
                         "%u KB free", (unsigned)(entry_size / 1024),
                         (unsigned)((ORB_POOL_CAP - orb_pool_used(s_pool) +
                                     (orb_pool_find(s_pool, slug) >= 0
                                      ? s_pool->e[orb_pool_find(s_pool, slug)].size : 0)) / 1024));
            else
                snprintf(sent, sizeof sent, f == ORB_POOL_ERR_TABLE
                         ? "the theme area holds eight apps' themes already"
                         : "the theme area's table is not loaded");
            ESP_LOGW(TAG, "store refused: %s (its sounds %u KB)", sent, (unsigned)(voice_size / 1024));
            heap_caps_free(region); card_close(fd);
            return install_failed(ESP_ERR_NO_MEM, sent, why, whysz);
        }
    }

    /* Owner, 2026-11-09: a store does not reboot. It chooses: the app's old copies go, its
     * new voice bank is made from the card file, and its display entry fills when it is on
     * the glass, now if it is (store_choose). Only a fill goes on into the copy below. */
    if (!s_install_fill) {
        heap_caps_free(region);
        const esp_err_t ce = store_choose(slug, id, h.name, fd, file_art_base, sec_art_off,
                                          file_geom_crc, why, whysz);
        card_close(fd);
        if (ce == ESP_OK) {
            (void)preview_store(slug, id, file_geom_crc);   /* the carousel's, from flash */
            orb_strip_preview_changed(slug);
        }
        return ce;
    }

    /* C3, §5.3 rule 1: the erase must not happen under a live map. The dial
     * parks first, the map comes down second, and from here on there is no
     * path back to the old theme — its pixels are about to be erased — so every
     * remaining outcome, success or failure, ends in the reboot. If the park
     * itself will not land, refuse while the bank is still resident. */
    /* theme-cache-plan §2.4: the app being filled is on the glass on its bare face (its bind
     * let the art go, on the LVGL task); binds wait while the table changes; the voice banks
     * keep playing. Nothing is parked: a fill writes free pages alone (§4.1). */
    s_pool_busy = true;

    /* The serial counts up over this app's last theme; none, or a header that cannot be
     * read, restarts the count at 1 — the count is a staleness guard, not a version
     * line. Read before the pool lets that theme go. */
    uint32_t raw[W_WORDS];
    uint32_t serial = 1;
    {
        const int old = orb_pool_find(s_pool, slug);
        if (old >= 0) orb_pool_pages(s_pool, (uint32_t)old, &s_job_pg);
        if (old >= 0 && pg_read(&s_job_pg, SLOT_HDR, raw, sizeof raw) == ESP_OK) {
            orb_bundle3_hdr_t o3;
            if (orb_bundle3_hdr_parse(raw, &o3) == NULL)   serial = o3.serial + 1;
        }
    }

    /* The target (theme-cache-plan §4.1): the pool lets this app's old theme go, evicts the
     * least recently worn if it must, and answers free pages for the new bytes. Nothing
     * moves; the pages are free space until the commit names them. Every SLOT_* offset below
     * is the entry's own, through its pages. */
    if (!s_install_fill) staged_mark(id);
    orb_pool_pages_t *pg = &s_job_pg;
    const char *ord[CACHE_APPS];
    const uint32_t nord = order_list(ord);
    char gone[ORB_POOL_ENTRIES][ORB_POOL_SLUG_MAX];
    uint32_t ngone = 0;
    const int pr = orb_pool_reserve(&s_pool_io, s_pool, &s_pool_which, slug, ORB_POOL_DISPLAY,
                                    entry_size, ord, nord, NULL, pg, gone, &ngone);
    for (uint32_t i = 0; i < ngone; i++)
        ESP_LOGI(TAG, "making room: %s's theme left the pool (least recently on the glass; its file "
                      "stays on the card)", gone[i]);
    uint32_t art_crc = 0;
    esp_err_t e = pr ? ESP_FAIL : ESP_OK;
    if (pr) w = "the theme area would not make room";
    fill_mark_t *mark = NULL;
    if (s_install_fill && e == ESP_OK) {
        /* theme-cache-plan §2.4: the copy writes free pages alone, so the other apps bind
         * again now; and a fill paused here before goes on from its mark, if the table is the
         * one it left (so the free pages it answered are the same) */
        s_pool_busy = false;
        const uint32_t tcrc = esp_rom_crc32_le(0, (const uint8_t *)s_pool, sizeof *s_pool);
        mark = &s_fill_mark;
        if (mark->valid && (strcmp(mark->slug, slug) || mark->file_crc != file_geom_crc ||
                            mark->at != pg->off[0] || mark->table_crc != tcrc || ngone))
            mark->valid = false;
        if (mark->valid)
            ESP_LOGI(TAG, "fill: %s goes on from %u KB (paused there)", slug, (unsigned)(mark->erased / 1024));
        strlcpy(mark->slug, slug, sizeof mark->slug);
        mark->file_crc = file_geom_crc; mark->at = pg->off[0]; mark->table_crc = tcrc;
    }
    if (e == ESP_OK)
        e = copy_bundle3(fd, &b, region, file_art_base, pg,
                         &art_crc, &w, s_install_fill ? fill_progress : store_progress, NULL, mark);
    if (mark && e != ESP_OK && e != ESP_ERR_TIMEOUT) mark->valid = false;   /* only a pause goes on */
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

    const esp_err_t ee = pg_erase(pg, SLOT_HDR, THEME_SECTOR);
    if (ee == ESP_OK) {
        uint32_t hdr[W_WORDS];
        build_slot_hdr3(hdr, &h, art_crc, serial, file_geom_crc);
        e = pg_write(pg, SLOT_HDR, hdr, sizeof(hdr));
        if (e != ESP_OK) w = "could not make the new slot live";
    } else {
        e = ee;
        w = "could not clear the old slot header";
    }
    /* The table last: until it names the entry, the bytes just written are free space
     * and a cut leaves this app on its base face, every other app's theme whole. */
    if (e == ESP_OK) {
        orb_pool_entry_t pe;
        memset(&pe, 0, sizeof pe);
        strlcpy(pe.slug, slug, sizeof pe.slug);
        strlcpy(pe.name, h.name, sizeof pe.name);
        pe.size = orb_pool_round(entry_size);
        pe.geom_size = SLOT_GEOM_OFF + h.geom_size;
        pe.art_off = SLOT_ART_OFF;
        pe.art_size = art_span;
        pe.art_crc = art_crc;
        pe.file_crc = file_geom_crc;
        pe.serial = serial;
        pe.abi = h.geom_abi;
        pe.kind = ORB_POOL_DISPLAY;                /* theme-cache-plan §2.6: the theme as it draws */
        if (orb_pool_pcrc(&s_pool_io, pg, 0, pe.geom_size, &pe.geom_crc) ||
            orb_pool_commit(&s_pool_io, s_pool, &s_pool_which, &pe, pg)) {
            e = ESP_FAIL;
            w = "the theme area's table would not take the new theme";
        }
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
    /* theme-cache-plan §2.6: the app's voice bank, from the entry just written (or the old
     * one gone, for a theme with no sounds). A failure leaves the theme stored without it,
     * which binds no more (ruling 8): the app runs bare and its log says to send it again. */
    if (s_install_fill) {
        s_pool_busy = false;
        ESP_LOGI(TAG, "fill: %s's theme \"%s\" is back in the pool (%u KB on %u pages in %u ms)",
                 slug, h.name, (unsigned)(entry_size / 1024), (unsigned)pg->n,
                 (unsigned)((esp_timer_get_time() - t0) / 1000));
        /* §4.1: an app with sounds and no voice bank at all gets one from the entry just
         * written, mapped now; one made from another file is the reconcile's (a store) */
        if (s_store_snd.n && orb_pool_find_kind(s_pool, slug, ORB_POOL_VOICE) < 0) {
            const char *vw = NULL;
            if (store_voice(slug, h.name, pg, -1, 0, sec_art_off, file_geom_crc, serial, &vw) != ESP_OK)
                ESP_LOGE(TAG, "fill: %s's voice bank: %s", slug, vw ? vw : "failed");
            else
                voice_add(slug);
        }
        return ESP_OK;
    }
    return ESP_OK;                                 /* unreached: only a fill copies */
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
/* D42: the dial's moving rows — every hand, pointer, ring, still layer, twin and the
 * carriage — the table a bundle's AUX elements project into, read by clock_view for the
 * life of the theme. PSRAM, allocated once at the first adoption: 32 rows would add
 * ~2.1 kB to .bss on a board whose internal heap boundary is what httpd_start needs
 * (the 2026-10-24 lesson orb_stage.c's layer keys record), and the dial reads each row
 * a handful of times a frame. */
static clk_row_t *s_b3_rows;
static clk_comp_t s_b3_comps[CLK_COMP_MAX];   /* D32 slice 2: the FORMAT 3 dial's
                                               * complications, filled by the
                                               * projection and read by clock_view
                                               * through s_geo below. Static by the
                                               * same rule as the four gif tables:
                                               * the projection allocates nothing,
                                               * and a dial's tables outlive its
                                               * themes. */

static esp_err_t adopt_bundle3(const orb_bundle3_hdr_t *h, const uint32_t *raw,
                               const char *src_name, const orb_pool_pages_t *pg)
{
    uint8_t *region = heap_caps_malloc(h->geom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) return ESP_ERR_NO_MEM;
    if (pg_read(pg, SLOT_GEOM_OFF, region, h->geom_size) != ESP_OK) {
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
    const orb_bundle_sec_t *sec = sole_sec(&b);
    if (!sec) {
        /* Not damage — an empty bundle. The row's sentence says so, because "the
         * file I stored is not on the wall" without a reason is what an owner reads
         * as a broken button. */
        ESP_LOGI(TAG, "bundle in %s carries no theme section", src_name);
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
    if (!s_b3_rows)
        s_b3_rows = heap_caps_calloc(CLK_ROW_MAX, sizeof *s_b3_rows,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    io.rows = s_b3_rows;   io.row_cap = s_b3_rows ? CLK_ROW_MAX : 0;   /* D42 */
    if (!s_snd) s_snd = heap_caps_calloc(1, sizeof *s_snd, MALLOC_CAP_SPIRAM);
    /* The parse fills a scratch table, not the live one: a refused bundle must
     * leave the playing dial's sounds as they were, and the live rows are copied
     * over only after the voices are stopped and the new bank is mapped. */
    static clk_sound_t *snd_tmp;
    if (!snd_tmp) snd_tmp = heap_caps_calloc(CLK_SND_SLOTS, sizeof *snd_tmp, MALLOC_CAP_SPIRAM);
    io.sounds = snd_tmp; io.sound_cap = (s_snd && snd_tmp) ? CLK_SND_SLOTS : 0;
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
    /* D39 stopped every voice here, because voices read PCM out of the bank this map
     * replaces. theme-cache-plan §2.6: they read the voice banks now, mapped apart from
     * boot and never remapped, so a bind no longer silences a chime. */
    /* §4.1: the banks' pages, from the entry's second page on, as one window */
    const uint32_t first = SLOT_ART_OFF / ORB_POOL_PAGE, npg = (span + ORB_POOL_PAGE - 1) / ORB_POOL_PAGE;
    const esp_err_t fe = first + npg <= pg->n ? art_bank_map_pages(&pg->off[first], npg, span)
                                              : ESP_ERR_INVALID_SIZE;
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
    s_live_sec = sole_sec(&s_live_b);                            /* re-found in the copy */

    s_geo.theme = &s_theme;
    s_geo.rects = s_b3_rects; s_geo.rect_first = s_b3_first;
    s_geo.gif_delays = s_b3_delays; s_geo.bg_frames = s_b3_bgf;
    /* The projection refused anything it could not draw whole before this line
     * was reachable (D32 R-F), so a count of zero here means the table carries
     * no bank row, not that a row was dropped. */
    s_geo.comps = io.out_n_comps ? s_b3_comps : NULL;
    /* D42: the rows a bundle carried ride the table the projection filled. A count of
     * zero names a dial with no moving rows, not a row that was dropped — the
     * projection refuses the whole draw past its own guards (D32 R-F). */
    s_geo.rows = io.out_n_rows ? s_b3_rows : NULL;
    s_geo.bank = NULL;
    s_geo.n_comps = (uint16_t)io.out_n_comps;
    s_geo.n_rows  = (uint16_t)io.out_n_rows;
    s_geo.n_bank = 0;
    if (s_snd) {
        memcpy(s_snd->s, snd_tmp, io.out_n_sounds * sizeof *snd_tmp);
        s_snd->n = io.out_n_sounds;
        s_snd->pattern = io.out_pattern;
    }
    s_geo.art_size = io.out_art_size;
    s_geo.blob_off = io.out_art_off - SLOT_ART_OFF;
    s_geo.values_off = io.out_values_off;     /* radar-plan §2.11 */
    s_geo.values_len = io.out_values_len;
    s_geo.values_ver = io.out_values_ver;
    s_geo.from_container = true;
    /* The §16 line the owner reads after every install: what the projection
     * actually put on the dial, from the table the bundle carried. A carriage
     * reads as its frame count, a bank row as a comp; both zero is the plain
     * dial, which is every bundle before D32 and every refusal's alternative. */
    ESP_LOGI(TAG, "bundle \"%s\" projected into a dial: %u complication row%s, "
             "%u moving row%s, carriage %u frames @ %u fps, beat %s %u/s",
             h->name, (unsigned)io.out_n_comps, io.out_n_comps == 1 ? "" : "s",
             (unsigned)io.out_n_rows, io.out_n_rows == 1 ? "" : "s",
             (unsigned)s_theme.tourbillon_n, (unsigned)s_theme.tourbillon_fps,
             s_theme.second_sweep ? "sweep" : "step", (unsigned)s_theme.tick_rate);
    if (s_theme.bg_frames)                      /* §31: an animated plate says so */
        ESP_LOGI(TAG, "plate bank: %u keyframes @ %u fps, %s",
                 (unsigned)s_theme.bg_frames + 1u, (unsigned)s_theme.bg_fps,
                 s_theme.bg_loop ? "loop" : "one-shot");

    strlcpy(s_geo.name, h->name, sizeof(s_geo.name));
    strlcpy(s_active_name, h->name, sizeof(s_active_name));
    /* The CARD file's region checksum, which build_slot_hdr3() keeps in word 10: the
     * slot's own geom_crc is the rebase's and never equals a file's. Word 12 is zero
     * in every slot, so reading it matched nothing to anything (2026-11-05). */
    s_active_geom_crc = raw[W_SCR_H];
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
/* ---- theme-cache-plan §2.6: every app's voice bank, mapped from boot ----------------------
 * Each voice bank has a window of its own (§4.1: its pages are wherever they were free), made
 * at boot or when a fill makes the bank, and checked against its entry's CRCs through it
 * before it is listed. A voice reads its PCM through it whatever the stage is bound to. A
 * store reboots, so a bank is never replaced under a voice. */
typedef struct {
    char slug[ORB_POOL_SLUG_MAX]; uint32_t src_crc; const uint8_t *bank; orb_voice_t v;
    orb_pagemap_t pm;
} voice_reg_t;
static voice_reg_t *s_voices;
static volatile uint32_t s_nvoices;

static void voice_add(const char *slug)
{
    const esp_partition_t *p = art_bank_partition();
    const int k = s_pool ? orb_pool_find_kind(s_pool, slug, ORB_POOL_VOICE) : -1;
    if (!p || k < 0) return;
    if (!s_voices) s_voices = heap_caps_calloc(ORB_POOL_ENTRIES, sizeof *s_voices, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_voices) return;
    uint32_t at = s_nvoices;                       /* a dropped bank's place first */
    for (uint32_t i = 0; i < s_nvoices; i++) {
        if (!strcmp(s_voices[i].slug, slug)) return;
        if (!s_voices[i].slug[0] && at == s_nvoices) at = i;
    }
    if (at >= ORB_POOL_ENTRIES) return;
    const orb_pool_entry_t *e = &s_pool->e[k];
    EXT_RAM_BSS_ATTR static orb_pool_pages_t vpg;
    orb_pool_pages(s_pool, (uint32_t)k, &vpg);
    voice_reg_t *r = &s_voices[at];
    memset(r, 0, sizeof *r);
    if (orb_pagemap(p, vpg.off, vpg.n, &r->pm) != ESP_OK) {
        ESP_LOGE(TAG, "%s's voice bank: %u pages would not map — no sounds for it this boot", e->slug, (unsigned)vpg.n);
        return;
    }
    const uint8_t *b = r->pm.ptr;
    const char *why = esp_rom_crc32_le(0, b, e->geom_size) != e->geom_crc ||
                      esp_rom_crc32_le(0, b + e->art_off, e->art_size) != e->art_crc
                      ? "its bytes are not its CRCs" : orb_voice_parse(b, e->size, &r->v);
    if (why) {
        ESP_LOGE(TAG, "%s's voice bank refused: %s", e->slug, why);
        orb_pageunmap(&r->pm);
        return;
    }
    r->src_crc = e->file_crc;
    r->bank = b;
    strlcpy(r->slug, e->slug, sizeof r->slug);     /* named last: a reader sees it whole */
    if (at == s_nvoices) s_nvoices++;
    ESP_LOGI(TAG, "%s's voice bank: %u sounds, %u KB on %u pages", e->slug, (unsigned)r->v.n,
             (unsigned)(e->size / 1024), (unsigned)vpg.n);
}

/* `slug`'s voice bank leaves the registry and its window comes down (a store chose another
 * theme). LVGL task, after the app's voices were stopped: every reader is on it. */
static void voice_drop(const char *slug)
{
    for (uint32_t i = 0; s_voices && i < s_nvoices; i++) {
        voice_reg_t *r = &s_voices[i];
        if (!r->slug[0] || strcmp(r->slug, slug)) continue;
        r->slug[0] = '\0';
        r->bank = NULL;
        orb_pageunmap(&r->pm);
    }
}

static void voices_map(void)
{
    for (uint32_t k = 0; k < s_pool->n; k++)
        if (s_pool->e[k].kind == ORB_POOL_VOICE) voice_add(s_pool->e[k].slug);
}

const orb_voice_t *orb_theme_voice(const char *slug, const uint8_t **bank)
{
    for (uint32_t i = 0; slug && i < s_nvoices; i++)
        if (!strcmp(s_voices[i].slug, slug)) { if (bank) *bank = s_voices[i].bank; return &s_voices[i].v; }
    return NULL;
}

/* Ruling 8: a theme with sounds binds only beside a voice bank made from the same file. */
static bool voice_missing(const char *slug, uint32_t file_crc)
{
    if (!s_snd || !s_snd->n) return false;
    for (uint32_t i = 0; i < s_nvoices; i++)
        if (!strcmp(s_voices[i].slug, slug)) return s_voices[i].src_crc != file_crc;
    return true;
}

/* Entry k's identity for "already bound": its pages and its CRCs (never 0). */
EXT_RAM_BSS_ATTR static orb_pool_pages_t s_bind_pg;
static uint32_t entry_key(uint32_t k)
{
    orb_pool_pages_t *pg = &s_bind_pg;
    orb_pool_pages(s_pool, k, pg);
    const uint32_t c = esp_rom_crc32_le(s_pool->e[k].geom_crc, (const uint8_t *)pg->off, pg->n * 4u);
    return (c ^ s_pool->e[k].art_crc) | 1u;
}

static esp_err_t adopt_default_bundle(uint32_t k)
{
    uint32_t raw[W_WORDS];
    orb_pool_pages(s_pool, k, &s_bind_pg);
    if (pg_read(&s_bind_pg, SLOT_HDR, raw, sizeof raw) != ESP_OK)
        return ESP_FAIL;
    if (raw[W_MAGIC] != THEME_MAGIC || raw[W_FORMAT] != ORB_BUNDLE3_FORMAT)
        return ESP_ERR_NOT_FOUND;           /* not a bundle: erased, or a FORMAT 2 blob */
    orb_bundle3_hdr_t h;
    if (orb_bundle3_hdr_parse(raw, &h) != NULL)
        return ESP_ERR_INVALID_CRC;         /* claims 3, header will not check */
    return adopt_bundle3(&h, raw, "slot", &s_bind_pg);
}

/* One slot's header, read and classified. Returns whether the slot holds a parseable
 * FORMAT 3 bundle, and sets *claims3 for the in-between state: magic and format say
 * bundle, the header checksum says torn. Boot and the scan probe the ONE slot at
 * offset 0 with it; the `staged` witness expires only against this header, the header
 * the store actually wrote. */
static bool slot_probe3(const orb_pool_pages_t *pg, orb_bundle3_hdr_t *b3,
                        uint32_t raw[W_WORDS], bool *claims3)
{
    *claims3 = false;
    if (pg_read(pg, SLOT_HDR, raw, W_WORDS * 4) != ESP_OK) return false;
    if (raw[W_MAGIC] != THEME_MAGIC || raw[W_FORMAT] != ORB_BUNDLE3_FORMAT) return false;
    *claims3 = true;
    return orb_bundle3_hdr_parse(raw, b3) == NULL;
}

/* The one slot of Migration 1, carried into the pool on the first boot that finds no
 * table (docs/theme-pool-plan.md §2.3). The slot at offset 0 already IS an entry, laid
 * out as every entry is, so the carry reads its header and index for the slug and the
 * extents, takes the CRCs off flash, and writes a table naming it. Nothing moves and
 * nothing is erased: a cut before the table lands leaves the slot to carry again. */
static void pool_carry(const esp_partition_t *p)
{
    (void)p;
    orb_bundle3_hdr_t h;
    uint32_t raw[W_WORDS];
    bool claims;
    EXT_RAM_BSS_ATTR static orb_pool_pages_t cpg;    /* the slot's own pages: from page 0 on */
    cpg.n = 1; cpg.off[0] = 0;
    if (!slot_probe3(&cpg, &h, raw, &claims)) return;   /* an empty pool */
    uint8_t *region = heap_caps_malloc(h.geom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) { ESP_LOGE(TAG, "pool carry: no memory for the slot's index"); return; }
    orb_bundle_t b;
    const char *why = pg_read(&cpg, SLOT_GEOM_OFF, region, h.geom_size) != ESP_OK
                    ? "the slot's index would not read"
                    : orb_bundle_parse_region(&h, region, h.geom_size, SLOT_CAP, &b);
    if (!why && b.n_sections < 1) why = "the slot carries no section";
    orb_pool_entry_t e;
    memset(&e, 0, sizeof e);
    if (!why) {
        strlcpy(e.slug, b.sec[0].slug, sizeof e.slug);
        strlcpy(e.name, h.name, sizeof e.name);
        uint32_t end = SLOT_ART_OFF;
        for (uint32_t i = 0; i < b.n_sections; i++)
            if (b.sec[i].art_off + b.sec[i].art_size > end) end = b.sec[i].art_off + b.sec[i].art_size;
        e.geom_size = SLOT_GEOM_OFF + h.geom_size;
        e.art_off   = SLOT_ART_OFF;
        e.art_size  = (end - SLOT_ART_OFF + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);
        e.size      = orb_pool_round(SLOT_ART_OFF + e.art_size);
        e.file_crc  = raw[W_SCR_H];
        e.serial    = h.serial;
        e.abi       = h.geom_abi;
        e.kind      = ORB_POOL_DISPLAY;
        cpg.n = e.size / ORB_POOL_PAGE;
        for (uint32_t i = 0; i < cpg.n && i < ORB_POOL_PAGES; i++) cpg.off[i] = i * ORB_POOL_PAGE;
        if (cpg.n > ORB_POOL_PAGES) why = "the slot is bigger than the pool";
        else if (orb_pool_pcrc(&s_pool_io, &cpg, 0, e.geom_size, &e.geom_crc) ||
                 orb_pool_pcrc(&s_pool_io, &cpg, e.art_off, e.art_size, &e.art_crc))
            why = "the slot would not read back";
    }
    heap_caps_free(region);
    if (!why && orb_pool_commit(&s_pool_io, s_pool, &s_pool_which, &e, &cpg) != 0)
        why = "the pool's table would not write";
    if (why) { ESP_LOGE(TAG, "pool carry: %s — the slot stays as it is", why); return; }
    ESP_LOGI(TAG, "pool: carried the one slot into the pool as %s's theme \"%s\" (%u B)",
             e.slug, e.name, (unsigned)e.size);
}

/* The entry boot adopts before any app runs: the clock's, else the first. Each app's
 * own is bound when it builds and when it takes the glass (orb_theme_bind, P4). */
static int pool_live_entry(void)
{
    const int k = orb_pool_find(s_pool, "clock");
    if (k >= 0) return k;
    for (uint32_t i = 0; i < s_pool->n; i++)          /* a voice bank draws nothing (theme-cache-plan §2.6) */
        if (s_pool->e[i].kind == ORB_POOL_DISPLAY) return (int)i;
    return -1;
}

bool orb_theme_pool_stem(const char *slug, char *out, size_t outsz)
{
    if (!slug || !out || !outsz || !s_pool) return false;
    const int k = orb_pool_find(s_pool, slug);
    if (k >= 0) { theme_stem(slug, s_pool->e[k].name, out, outsz); return true; }
    for (uint32_t c = 0; c < s_cache.n_choice; c++)      /* theme-cache-plan §2.4: on the card */
        if (!strcmp(s_cache.c[c].slug, slug)) {          /* only, its preview is still its own */
            strlcpy(out, s_cache.c[c].stem, outsz);
            return true;
        }
    return false;
}

bool orb_theme_has(const char *slug)
{
    if (!slug || !s_pool) return false;
    if (orb_pool_find(s_pool, slug) >= 0) return true;
    for (uint32_t c = 0; c < s_cache.n_choice; c++)      /* theme-cache-plan §2.4: chosen, */
        if (!strcmp(s_cache.c[c].slug, slug)) return true;   /* its copy a fill away       */
    return false;
}

/* theme-pool-plan P4: make `slug`'s theme the one the paint path reads — its entry
 * projected into the dial's tables and its banks mapped — for the stage that is about to
 * build on that app's page. The stage releases the previous app's layers first, so no
 * widget, row or voice is reading the map this replaces. Already bound: nothing to do. */
esp_err_t orb_theme_bind(const char *slug)
{
    if (s_pool_busy) return ESP_ERR_INVALID_STATE;     /* a fill is moving entries (§2.4) */
    const esp_partition_t *p = art_bank_partition();
    /* a store choosing another theme for it lets this one go first (store_choose) */
    const int k = (p && s_pool && strcmp(slug, s_hide_slug)) ? orb_pool_find(s_pool, slug) : -1;
    if (k < 0) {
        /* theme-cache-plan §2.4: no theme of its own in the pool. The last app's adoption
         * goes with it, here on the LVGL task: the stage then builds this app's bare face
         * (it had built the clock's on weather's page), and nothing reads the bank a fill
         * is about to move (Bronze, 2026-11-08: the clock's gear bank read an unmapped page
         * mid-fill, "Cache disabled but cached memory region accessed"). */
        if (s_active_valid) use_no_art();
        art_bank_unmap();
        s_live_key = 0;
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t key = entry_key((uint32_t)k);
    if (s_active_valid && s_live_key == key && s_live_sec && !strcmp(s_live_sec->slug, slug))
        return ESP_OK;
    const esp_err_t e = adopt_default_bundle((uint32_t)k);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bind: %s's theme would not load (%s)", slug, esp_err_to_name(e));
        use_no_art();
        return e;
    }
    if (voice_missing(slug, s_pool->e[k].file_crc)) {
        ESP_LOGE(TAG, "bind: %s's theme \"%s\" has sounds and was stored before voice banks "
                      "(theme-cache-plan ruling 8) — send it again", slug, s_active_name);
        note_set(slug, NOTE_RESEND, s_active_name);
        use_no_art();
        return ESP_ERR_INVALID_STATE;
    }
    s_live_key = key;
    order_touch(slug);                               /* theme-cache-plan §2.1 */
    ESP_LOGI(TAG, "bound %s's theme \"%s\"", slug, s_active_name);
    return ESP_OK;
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
    if (!s_geo.theme) s_geo.theme = &s_no_theme;        /* never NULL (use_no_art) */
    /* The clock's themes source enters the OS's file table HERE, on the boot
     * rung and before any door task exists — not on the clock app's init. The
     * join-time theme death this replaces (bench, 2026-10-29) had the upload door
     * asking for a declaration only stage_init() writes, on the boot where
     * the hub had just installed and enabled the clock and no reboot had rebuilt
     * the table; the door's ESP_ERR_NOT_FOUND came back as "no card in the
     * socket" over a mounted card. A no-I/O table write belongs with the module
     * that owns the directory. See the declaration above orb_theme_scan(). */
    const esp_err_t src_err = orb_theme_src_register();
    if (src_err != ESP_OK)
        ESP_LOGE(TAG, "themes file source refused (%s) — the theme doors fall back "
                      "to the shipped declaration", esp_err_to_name(src_err));

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

    /* The pool (docs/theme-pool-plan.md): its table, the one slot carried into it on
     * the first boot that finds none, a cut store's or compaction's repair, then the
     * entry this boot draws. No pool, no theme: the base face, as an empty slot was. */
    s_live_key = 0;
    int pr = pool_io(p) ? orb_pool_load(&s_pool_io, s_pool, &s_pool_which) : ORB_POOL_ERR_ARG;
    cache_load();                                      /* theme-cache-plan §2.1 */
    gone_load();                                       /* why a theme is missing */
    if (!pr && s_pool_which && s_pool->format != ORB_POOL_FORMAT) {
        /* §4.1's carry: a packed pool on the 4 KB grid is not on pages, so it is not carried.
         * What it says each app wears is kept (the choices, seeded below from its entries if
         * missing), and the pool starts empty: each app's theme, voice bank and all, fills
         * from the card the first time it opens. */
        ESP_LOGW(TAG, "pool: a format %u table with %u entries — not on pages; every theme fills "
                      "again from the card as its app opens", (unsigned)s_pool->format, (unsigned)s_pool->n);
        bool seeded = false;
        for (uint32_t k = 0; k < s_pool->n; k++) {
            const orb_pool_entry_t *e = &s_pool->e[k];
            if (e->kind != ORB_POOL_DISPLAY) continue;
            uint32_t c = 0;
            while (c < s_cache.n_choice && strcmp(s_cache.c[c].slug, e->slug)) c++;
            if (c < s_cache.n_choice || s_cache.n_choice >= CACHE_APPS) continue;
            strlcpy(s_cache.c[c].slug, e->slug, sizeof s_cache.c[c].slug);
            theme_stem(e->slug, e->name, s_cache.c[c].stem, sizeof s_cache.c[c].stem);
            s_cache.c[c].file_crc = e->file_crc;
            s_cache.n_choice++;
            seeded = true;
        }
        if (seeded) cache_save();
        pr = orb_pool_reset(&s_pool_io, s_pool, &s_pool_which);
    } else if (!pr && !s_pool_which) {
        pool_carry(p);
    }
    if (!pr) {
        /* a theme stored before the choices were kept: its entry says what it wears, so
         * the choice is seeded from it while the entry is still here to say it */
        bool seeded = false;
        for (uint32_t k = 0; k < s_pool->n; k++) {
            const orb_pool_entry_t *e = &s_pool->e[k];
            if (e->kind != ORB_POOL_DISPLAY) continue;
            uint32_t c = 0;
            while (c < s_cache.n_choice && strcmp(s_cache.c[c].slug, e->slug)) c++;
            if (c < s_cache.n_choice || s_cache.n_choice >= CACHE_APPS) continue;
            strlcpy(s_cache.c[c].slug, e->slug, sizeof s_cache.c[c].slug);
            theme_stem(e->slug, e->name, s_cache.c[c].stem, sizeof s_cache.c[c].stem);
            s_cache.c[c].file_crc = e->file_crc;
            s_cache.n_choice++;
            seeded = true;
            ESP_LOGI(TAG, "%s's choice seeded from its stored theme: %s", e->slug, s_cache.c[c].stem);
        }
        if (seeded) cache_save();
    }
    if (!pr) voices_map();                             /* theme-cache-plan §2.6 */
    if (!pr) previews_map();                           /* the carousel's, from flash */
    const int live = pr ? -1 : pool_live_entry();
    if (pr)
        ESP_LOGE(TAG, "the theme pool would not load (%d) — base digital face", pr);
    else
        ESP_LOGI(TAG, "pool: %u theme%s, %u of %u B used", (unsigned)s_pool->n,
                 s_pool->n == 1 ? "" : "s", (unsigned)orb_pool_used(s_pool),
                 (unsigned)ORB_POOL_CAP);
    if (live < 0) {
        /* Nothing to draw. A store's witness that outlived it says which store did
         * not land: no entry can answer it, so it stays up for the row to read. */
        char staged[ORB_THEME_HANDLE_MAX + 1];
        if (nvs_get(THEME_NVS_STAGED, staged, sizeof staged) && theme_id_ok(staged))
            strlcpy(s_staged_id, staged, sizeof s_staged_id);
        use_no_art();
        return ESP_OK;
    }
    s_live_key = entry_key((uint32_t)live);          /* and s_bind_pg is its pages */

    orb_bundle3_hdr_t b3;
    uint32_t raw3[W_WORDS];
    bool claims3 = false;
    const bool slot_is_bundle = slot_probe3(&s_bind_pg, &b3, raw3, &claims3);

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
    esp_err_t de = adopt_default_bundle((uint32_t)live);
    if (de == ESP_OK && voice_missing(s_pool->e[live].slug, s_pool->e[live].file_crc)) {
        ESP_LOGE(TAG, "%s's theme \"%s\" has sounds and was stored before voice banks "
                      "(theme-cache-plan ruling 8) — send it again", s_pool->e[live].slug, s_active_name);
        use_no_art();
        de = ESP_ERR_INVALID_STATE;
    }
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
    job_stage("checking", 0, 0);
    if (orb_theme_install_bundle(r->id, why, sizeof why) != ESP_OK) {
        try_note(r->id, why[0] ? why : "the install failed");
        /* A store torn past the unmap reboots on its own; either way no stage runs now. */
        job_stage("", 0, 0);
        return;
    }
    /* Owner, 2026-11-09: no reboot. The store chose the theme (store_choose); the app wears
     * it the next time it is on the glass, now if it is. */
    job_stage("", 0, 0);
    try_note(NULL, NULL);
}

/* ---- theme-cache-plan §2.4: the fill ------------------------------------------------- */
typedef struct { char slug[ORB_POOL_SLUG_MAX]; } fill_req_t;      /* the stem is its choice's */

static void fill_landed(void *arg)                 /* LVGL task */
{
    const orb_app_t *app = orb_app_find_slug((const char *)arg);
    if (app && input_router_current() == app) orb_wasm_theme_landed((orb_app_t *)app);
}

static void fill_job(void *arg)
{
    const fill_req_t *r = arg;
    char why[72] = "", stem[ORB_THEME_HANDLE_MAX + 1] = "";
    for (uint32_t c = 0; c < s_cache.n_choice; c++)
        if (!strcmp(s_cache.c[c].slug, r->slug)) strlcpy(stem, s_cache.c[c].stem, sizeof stem);
    s_fill_done = 0; s_fill_total = 0;
    s_fill_stop = false;
    orb_io_heavy_begin();                           /* a guest the flash stalls is forgiven */
    s_install_fill = true;
    const esp_err_t e = stem[0] ? orb_theme_install_bundle(stem, why, sizeof why) : ESP_ERR_NOT_FOUND;
    s_install_fill = false;
    s_pool_busy = false;
    orb_io_heavy_end();
    if (e == ESP_ERR_TIMEOUT) {
        ESP_LOGI(TAG, "fill: %s paused at %u of %u KB (the owner left it)", r->slug,
                 (unsigned)(s_fill_done / 1024), (unsigned)(s_fill_total / 1024));
        s_fill_slug[0] = '\0';
        return;
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "fill: %s's theme stays on the card: %s", r->slug, why[0] ? why : "the copy failed");
        if (!s_fill_changed) note_set(r->slug, NOTE_FAILED, why[0] ? why : "the copy failed");
        s_fill_slug[0] = '\0';
        return;
    }
    note_set(r->slug, 0, NULL);
    static char landed[ORB_POOL_SLUG_MAX];
    strlcpy(landed, r->slug, sizeof landed);
    s_fill_slug[0] = '\0';
    lv_async_call(fill_landed, landed);
}

static int fill_choice(const char *slug)            /* its choice's index, -1: no fill */
{
    if (!slug || !s_pool || orb_pool_find(s_pool, slug) >= 0 || s_fill_slug[0]) return -1;
    uint32_t c = 0;
    while (c < s_cache.n_choice && strcmp(s_cache.c[c].slug, slug)) c++;
    return c < s_cache.n_choice ? (int)c : -1;        /* nothing chosen: the bare form stands */
}

bool orb_theme_fill_wanted(const char *slug) { return fill_choice(slug) >= 0; }

bool orb_theme_fill_post(const char *slug)
{
    const int c = fill_choice(slug);
    if (c < 0) return false;
    fill_req_t r;
    strlcpy(r.slug, slug, sizeof r.slug);
    strlcpy(s_fill_slug, slug, sizeof s_fill_slug);
    if (orb_jobs_post(fill_job, &r, sizeof r) != ESP_OK) { s_fill_slug[0] = '\0'; return false; }
    ESP_LOGI(TAG, "fill: %s's theme \"%s\" comes back from the card", slug, s_cache.c[c].stem);
    return true;
}

void orb_theme_fill_pause(const char *slug)
{
    if (slug && s_fill_slug[0] && !strcmp(s_fill_slug, slug)) s_fill_stop = true;
}

bool orb_theme_fill_progress(const char *slug, uint32_t *done, uint32_t *total)
{
    if (!slug || strcmp(s_fill_slug, slug)) return false;
    *done = s_fill_done; *total = s_fill_total;
    return true;
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

/* ---- D38: removing the active theme takes it off the wall too ----------------
 *
 * The one slot is the theme: erasing its header sector is the whole of
 * deactivation, and the next boot draws the base face exactly as a torn store
 * does (adopt_default_bundle refuses a headerless slot and logs the base-face
 * reason). The ladder is the store's, stripped to its safety-critical spine:
 * park the dial (menu, overlay, tick parked), bring the art map down, erase the
 * header, ask for the reboot that comes back without a theme. Nothing about
 * the word order here can leave the dial half-dressed: pre-erase the theme is
 * on the wall whole; post-erase it is gone, and the reboot is the erase's own
 * tail, armed the same way a failed store arms itself. */
static void clear_job(void *arg)
{
    const char *app = arg && ((const char *)arg)[0] ? (const char *)arg : orb_theme_live_slug();
    const orb_app_t *was = input_router_current();
    if (!store_park_dial(was, 0, "removing")) {
        orb_boot_overlay_release();        /* the overlay's band goes back with the glass */
        orb_sched_set_running(true);       /* the tick stands; the dial never stopped */
        ESP_LOGW(TAG, "deactivate refused: the dial would not park");
        return;
    }
    orb_sound_stop_all();                  /* theme-cache-plan §2.6: voices read the voice banks it moves */
    art_bank_unmap();
    s_slot_torn = true;                    /* from here the reboot is the only way back */
    /* The pool lets this app's theme go and packs every other app's down over it
     * (docs/theme-pool-plan.md §2.2); the map is down, so nothing reads what moves. */
    char gone[ORB_POOL_SLUG_MAX];
    strlcpy(gone, app, sizeof gone);
    const esp_partition_t *p = art_bank_partition();
    int r = ORB_POOL_ERR_ARG;
    if (p && s_pool && pool_io(p)) {
        s_pool_io.progress = room_progress;     /* the apps after it move down */
        r = orb_pool_remove(&s_pool_io, s_pool, &s_pool_which, gone);
        /* theme-cache-plan ruling 7: the file it wore is gone, so both of its copies go,
         * and the choice */
        if (!r) r = orb_pool_remove_kind(&s_pool_io, s_pool, &s_pool_which, gone, ORB_POOL_VOICE);
        if (!r) r = orb_pool_remove_kind(&s_pool_io, s_pool, &s_pool_which, gone, ORB_POOL_PREVIEW);
        s_pool_io.progress = NULL;
        if (!r) choice_set(gone, NULL, 0);
    }
    if (r) {
        orb_boot_overlay("could not clear the theme slot", 0, 0, 0);
        ESP_LOGE(TAG, "deactivate: the pool would not let %s's theme go (%d) — "
                      "the dial stands on the parked glass", gone, r);
        return;                                /* s_slot_torn stands: reboot recovers */
    }
    if (!strcmp(gone, orb_theme_live_slug()))
        s_active_valid = false;              /* nothing is on the wall now */
    orb_boot_overlay("theme removed, rebooting", 0, 0, 0);
    if (!strcmp(gone, orb_theme_live_slug()))
        ESP_LOGI(TAG, "deactivated: the theme's card file was deleted and %s's entry left "
                      "the pool — the base face draws on the next boot", gone);
    else
        ESP_LOGI(TAG, "%s's stored theme left the pool with its card file — %s draws its "
                      "base face, and the dial keeps its own", gone, gone);
    if (orb_jobs_reboot_soon("theme: the active theme was deleted") != ESP_OK)
        ESP_LOGE(TAG, "deactivated but the reboot could not be armed — "
                      "a manual reboot lands the base face");
}

/* ---- theme-cache-plan ruling 7: the flash copies follow the card file ------------------
 *
 * Once a card has mounted (at boot, or back after a swap or a USB session), each choice's
 * file is read against the CRC its copies were made from. A file that changed is stored
 * again — the store rebuilds both copies, without a reboot; a file that is gone takes both
 * copies and the choice with it (the removal's own ladder, which reboots, and the reboot
 * brings the rest). A card that did not
 * answer, or a header that would not read, changes nothing. */
static void reconcile_job(void *arg)
{
    (void)arg;
    if (!s_pool || orb_sd_ensure_mounted() != ESP_OK) return;
    for (uint32_t c = 0; c < s_cache.n_choice; c++) {
        const char *slug = s_cache.c[c].slug, *stem = s_cache.c[c].stem;
        char path[ORB_SD_PATH_MAX + 16];
        if (!theme_file(stem, path, sizeof path)) continue;
        struct stat st;
        orb_sd_lock();
        const int rc = stat(path, &st), en = errno;
        const bool dir_there = stat(ORB_SD_THEME_DIR, &st) == 0;   /* a card that answers */
        orb_sd_unlock();
        if (rc != 0 && en == ENOENT && dir_there) {
            const bool held = orb_pool_find_kind(s_pool, slug, ORB_POOL_DISPLAY) >= 0 ||
                              orb_pool_find_kind(s_pool, slug, ORB_POOL_VOICE) >= 0;
            ESP_LOGW(TAG, "reconcile: %s's theme file \"%s\" is gone from the card: %s", slug, stem,
                     held ? "its copies go with it" : "it had no copies; the choice goes");
            gone_set(slug, stem);        /* its page says so, after the reboot too */
            if (held) { clear_job((void *)slug); return; }
            choice_set(slug, NULL, 0);
            continue;
        }
        if (rc != 0) continue;
        orb_bundle3_hdr_t h;
        uint32_t fsz = 0;
        const char *w = NULL;
        const int fd = card_open_verified_id3(stem, &h, &fsz, &w);
        if (fd < 0) continue;
        card_close(fd);
        if (h.geom_crc == s_cache.c[c].file_crc) {
            /* its preview in flash (owner, 2026-11-09): made for a choice that has none */
            const int pk = orb_pool_find_kind(s_pool, slug, ORB_POOL_PREVIEW);
            if (pk < 0 || s_pool->e[pk].file_crc != h.geom_crc) {
                char st[ORB_THEME_HANDLE_MAX + 1], sl[ORB_POOL_SLUG_MAX];
                strlcpy(st, stem, sizeof st);
                strlcpy(sl, slug, sizeof sl);
                if (preview_store(sl, st, h.geom_crc) == ESP_OK) orb_strip_preview_changed(sl);
            }
            continue;
        }
        ESP_LOGW(TAG, "reconcile: %s's theme file \"%s\" changed on the card since it was stored: "
                      "stored again", slug, stem);
        char why[72] = "";
        if (orb_theme_install_bundle(stem, why, sizeof why) != ESP_OK)
            ESP_LOGW(TAG, "reconcile: %s's store refused: %s", slug, why);
        /* no reboot (owner, 2026-11-09): on to the next choice */
    }
}

bool orb_theme_absent(const char *slug, const char **brief, char *why, size_t whysz)
{
    if (brief) *brief = NULL;
    if (why && whysz) why[0] = '\0';
    if (!slug) return false;
    bool chosen = false;
    for (uint32_t c = 0; c < s_cache.n_choice; c++)
        if (!strcmp(s_cache.c[c].slug, slug)) chosen = true;
    const char *b = NULL;
    for (uint32_t k = 0; !chosen && k < s_gone.n; k++)
        if (!strcmp(s_gone.g[k].slug, slug)) {
            b = "file deleted";
            if (why) snprintf(why, whysz, "Its theme file \"%s\" was deleted from the card, so the "
                              "orb let its copy go. Send it a theme again.", s_gone.g[k].stem);
        }
    const bool off_pool = s_pool && orb_pool_find_kind(s_pool, slug, ORB_POOL_DISPLAY) < 0;
    if (chosen && !b && off_pool && !orb_sd_mounted()) {   /* a card out outranks a failed copy */
        b = "no card";
        if (why) snprintf(why, whysz, "Its theme is on the SD card, and no card is in. Put the "
                          "card back and open the app.");
    }
    for (int i = 0; chosen && !b && i < (int)CACHE_APPS; i++) {
        if (!s_note[i].slug[0] || strcmp(s_note[i].slug, slug)) continue;
        if (s_note[i].kind == NOTE_RESEND) {
            b = "send it again";
            if (why) snprintf(why, whysz, "Its theme \"%s\" was stored by older firmware without "
                              "its sounds. Send it again from the hub.", s_note[i].why);
        } else if (off_pool) {
            b = "copy failed";
            if (why) snprintf(why, whysz, "Its theme could not be copied back from the card: %s.",
                              s_note[i].why);
        }
    }
    if (brief) *brief = b;
    return b != NULL;
}

int orb_theme_hot(const char *slug)
{
    if (!slug || !s_pool) return -1;
    if (orb_pool_find(s_pool, slug) >= 0) return 1;      /* its display entry is in flash */
    for (uint32_t c = 0; c < s_cache.n_choice; c++)
        if (!strcmp(s_cache.c[c].slug, slug)) return 0;  /* chosen, on the card only */
    return -1;
}

bool orb_theme_chosen(const char *stem)
{
    for (uint32_t c = 0; stem && c < s_cache.n_choice; c++)
        if (!strcmp(s_cache.c[c].stem, stem)) return true;
    return false;
}

void orb_theme_service(void)
{
    if (s_fill_changed) { s_fill_changed = false; s_reconciled_gen--; }   /* due again */
    /* a theme chosen for the app on the glass fills there, once the store's job is done */
    if (s_refill_slug[0] && !orb_jobs_busy()) {
        orb_app_t *app = (orb_app_t *)orb_app_find_slug(s_refill_slug);
        s_refill_slug[0] = '\0';
        if (app && input_router_current() == app) orb_wasm_theme_changed(app);
    }
    /* after the boot has settled (the first app bound, the carousel's previews read), with
     * the worker free and no upload in flight; a refused post tries again next poll */
    const uint32_t g = orb_sd_mount_gen();
    if (g == s_reconciled_gen || !orb_sd_mounted() || esp_timer_get_time() < 20000000 ||
        orb_jobs_busy() || orb_theme_upload_busy() || s_fill_slug[0])
        return;
    if (orb_jobs_post(reconcile_job, NULL, 0) == ESP_OK) s_reconciled_gen = g;
}

esp_err_t orb_theme_post_clear(void)
{
    return orb_jobs_post(clear_job, NULL, 0);
}

/* theme-pool-plan P5: let one app's stored theme go, whichever app is on the glass. */
static esp_err_t post_clear_app(const char *slug)
{
    char a[ORB_POOL_SLUG_MAX];
    strlcpy(a, slug, sizeof a);
    return orb_jobs_post(clear_job, a, sizeof a);
}

esp_err_t orb_theme_deactivate(void)
{
    /* Nothing worn: say so rather than queue an erase of an empty slot and a
     * reboot for no visible change. The one caller today is the store door's
     * delete tail (orb_theme_delete), which already proved the slot worn. */
    if (!s_active_valid) return ESP_ERR_INVALID_STATE;
    /* Queue-only, exactly like post_bundle: the caller's door must return before
     * the job parks the dial and reboots the board, so the ack survives. The
     * board answers again on the base face when it clears the slot. */
    return orb_jobs_post(clear_job, NULL, 0);
}

esp_err_t orb_theme_delete(const char *id, bool *deactivating)
{
    if (deactivating) *deactivating = false;
    /* Resolve-by-walk first — the same card_open_verified_id install uses — so the
     * id is proven to name a real, self-consistent file before anything is unlinked.
     * A card file whose header name does not hash to its id is refused here exactly as
     * at install; unlinking never trusts a client string to reach a path builder.
     * D38 changed what deleting the ACTIVE theme means. It is no longer "the dial
     * keeps drawing it": deleting the file the wall wears also empties the slot,
     * and the reboot that draws the base face rides clear_job's own store ladder
     * (park, unmap, erase, arm). The card file is unlinked first either way, so the
     * owner's delete always lands; when the slot clear cannot be queued the face
     * stands until the next store, and the answer says so. */
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
    /* Worn means stored for its app: the pool's entry for the stem's app carries this
     * file's name (theme-pool-plan P5). */
    char dslug[ORB_BUNDLE_SLUG_MAX + 1], dhex[THEME_ID_HEX + 1];
    const int k = (s_pool && theme_stem_split(id, dslug, sizeof dslug, dhex, sizeof dhex))
                ? orb_pool_find(s_pool, dslug) : -1;
    const bool was_active = k >= 0 && strcmp(b3.name, s_pool->e[k].name) == 0;
    ESP_LOGI(TAG, "deleted \"%s\" (%s) from the card%s%s", b3.name, id,
             was_active ? " — it was " : "",
             !was_active ? "" : !strcmp(dslug, orb_theme_live_slug())
                              ? "the face on the wall, so the slot goes too"
                              : "its app's stored theme, so it leaves the pool");
    if (was_active && deactivating)
        *deactivating = (post_clear_app(dslug) == ESP_OK);
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
    const char        *app;             /* which app's rows this listing carries,
                                         * NULL for every app's; another app's file
                                         * is not a refusal, it is not this
                                         * listing's row at all */
    /* theme-pool-plan P5: what each app has stored, as the pool's table said when
     * this walk started — a store on the worker cannot rewrite it under the walk. The
     * pool stores the theme, not the filename, so a row's stored test re-derives the
     * stem id from its app's entry name under the row's own slug. */
    uint32_t           n_stored;
    struct { char slug[ORB_POOL_SLUG_MAX]; char name[ORB_POOL_NAME_MAX]; uint32_t file_crc; }
                       stored[ORB_POOL_ENTRIES];
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
    if (mine && c->app && strcmp(slug, c->app) != 0) return false;

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
    e->art_size = 0; e->serial = 0; e->design_size = 0; e->baked_s = 0;
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
    e->design_size = b3.design_size;
    e->baked_s = b3.baked_s;
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
                 "one app per theme file (D48); this one has %lu sections",
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
    for (uint32_t k = 0; k < c->n_stored; k++) {
        if (strcmp(c->stored[k].slug, slug) != 0) continue;
        char sid[THEME_ID_HEX + 1];
        theme_id(slug, c->stored[k].name, sid);
        if (strcmp(sid, id) == 0) {
            e->stored = true;
            /* The theme the dial drew at boot answers through theme_state(), as it
             * always has; another app's stored theme against the card file's region
             * checksum, which its pool entry keeps (theme-pool-plan P5). */
            if (s_active_valid && !strcmp(slug, orb_theme_live_slug())) {
                orb_thdr_t as;      /* the identity theme_state() reads */
                memset(&as, 0, sizeof as);
                strlcpy(as.name, b3.name, sizeof as.name);
                /* The card file's own figure against the one boot adopted. This
                 * read boot's figure here too, so the row compared it with itself
                 * and the live theme could never read changed (2026-11-05). */
                as.geom_crc = b3.geom_crc;
                as.art_crc = b3.geom_crc;   /* adopt_bundle3's one-figure rule */
                as.fmt = 3;
                theme_state(&as, &e->active, &e->changed);
            } else {
                e->active  = c->stored[k].file_crc == b3.geom_crc;
                e->changed = !e->active;
            }
        }
    }
    c->n++;
    return true;
}

/* =========================================================================== *
 * The clock's themes source, owned here.
 *
 * orb_files is the OS's walk and apps declare their directories into it, so the
 * declaration below is the one stage_init() hands up at boot — lifted here
 * so the theme doors can answer from it themselves. The reason is the join-time
 * theme death (bench, Bronze, 2026-10-29): the hub installs the clock over the
 * wire on a board that booted without one, enables it in the same run, and asks
 * the upload door for the theme seconds later. stage_init() has never run
 * on that boot, orb_files_src() answers NULL, upload_begin() returns
 * ESP_ERR_NOT_FOUND, and the door calls the whole thing "no card in the socket"
 * over a card that /api/v1/sd swears is mounted. The hub's mitigation spent a
 * full reboot and a second 3.7 MB upload to work around a door that did not
 * know where its own themes live.
 *
 * The values are orb_stage.c's declaration field for field, and this file is
 * now the one copy of them: orb_theme_src_register() puts them in the OS table
 * on the boot ladder (from orb_theme_boot, before any task that can ask exists),
 * and stage_init() re-declares the same source through the same call when
 * the clock builds — orb_files_register() takes a re-declaration as an app's
 * init running twice and overwrites in place.
 *
 * The doors below read through theme_src(), which falls back to the constant
 * when the table holds nothing. The fallback is a READ of a static const and
 * never a table write, so the httpd task can take it without racing a worker's
 * walk — the law orb_files.h writes the whole registration API under.
 * =========================================================================== */
_Static_assert(ORB_THEME_LIST_MAX <= 255, "orb_file_src_t.max_entries is a uint8_t");
/* theme-pool-plan P5: every app's themes, the OS's own read of the shared directory.
 * No prefix: a row's app is its stem's slug, which the doors split and check. A static
 * const, never registered, so any task may read it (orb_files.h's threading rule). */
static const orb_file_src_t s_any_themes_src = {
    .id          = "themes",
    .dir         = ORB_SD_THEME_DIR,
    .ext         = ORB_THEME_EXT,
    .max_entries = ORB_THEME_LIST_MAX,
    .name_prefix = "",
};
static const orb_file_src_t s_clock_themes_src = {
    .id          = "themes",
    .dir         = ORB_SD_THEME_DIR,
    .ext         = ORB_THEME_EXT,
    .max_entries = ORB_THEME_LIST_MAX,
    /* Every app's themes share this directory; the compound stem names its
     * owner, and the prefix keeps this source's cap and rows the clock's. */
    .name_prefix = "clock-",
};

esp_err_t orb_theme_src_register(void)
{
    return orb_files_register("clock", &s_clock_themes_src);
}

/* The clock's themes source: the registered row when the table has one, the
 * shipped declaration when it does not. Never NULL — the doors' own no-directory
 * branches stay as plain null guards, the shape every orb_files caller carries. */
static const orb_file_src_t *theme_src(void)
{
    const orb_file_src_t *src = orb_files_src("clock", "themes");
    if (src) return src;
    /* Loud once per refusal: the boot rung registered this source and should not
     * have needed this line. A full table is the only realistic cause — twelve
     * sources, and a build that reaches it has apps the shelf cannot hold. */
    ESP_LOGW(TAG, "\"themes\" unregistered at a theme door — using the shipped "
                  "declaration (orb_theme_src_register() did not land on this boot)");
    return &s_clock_themes_src;
}

int orb_theme_scan(orb_theme_entry_t *out, int cap)
{
    return orb_theme_scan_app(NULL, out, cap);
}

int orb_theme_scan_app(const char *app, orb_theme_entry_t *out, int cap)
{
    if (!out || cap <= 0) return -1;
    /* While an upload owns the card, do not open a second file on it: FatFs is
     * reentrant per volume, but this walk plus a 3.7 MB write on the same
     * directory would serialise the httpd worker behind the upload for seconds, and
     * a page that cannot poll its own list looks exactly like a device that has
     * stopped answering. The caller reports "uploading" instead. */
    if (orb_theme_upload_busy()) return -1;
    /* The same for a store: a theme job holds the card (and the display's DMA ring)
     * for its whole copy, and a walk here would wait it out on the httpd worker. The
     * job's own worker is let through, since a job that scans must not refuse itself. */
    if (orb_jobs_busy() && !orb_jobs_on_worker()) return -1;

    /* The directory and the extension stay the clock's declaration, not a
     * constant here: the OS owns the walk, so this layer asks where its themes
     * live rather than assuming — through theme_src(), which can ask that
     * question on a boot whose clock app never built. */
    const orb_file_src_t *src = &s_any_themes_src;

    /* Most of a kilobyte with the snapshot: PSRAM per walk, not the door task's 4 KB
     * stack, and not a static that two door tasks listing at once would share. */
    scan_ctx_t *c = heap_caps_calloc(1, sizeof *c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) return -1;
    c->out = out; c->cap = cap; c->app = app;
    /* What each app has stored, from the pool's table. The partition outliving the card
     * is the point: a stored theme stays listed as stored with no card in the socket at
     * all, and this only decides what the page MARKS — the store door re-reads the
     * card's header on the worker before it writes anything. */
    for (uint32_t k = 0; s_pool && k < s_pool->n && k < ORB_POOL_ENTRIES; k++) {
        if (s_pool->e[k].kind != ORB_POOL_DISPLAY) continue;   /* a voice bank is not a theme row */
        strlcpy(c->stored[c->n_stored].slug, s_pool->e[k].slug, sizeof c->stored[0].slug);
        strlcpy(c->stored[c->n_stored].name, s_pool->e[k].name, sizeof c->stored[0].name);
        c->stored[c->n_stored].file_crc = s_pool->e[k].file_crc;
        c->n_stored++;
    }
    /* Both ends logged, with the wait: a scan that never comes back is the one card
     * failure that looks like a dead web server from outside (the dial keeps drawing
     * and ping keeps replying), and which of the two lines is missing says whether the
     * worker is waiting on the mutex or inside FatFs. */
    const int64_t t_scan = esp_timer_get_time();
    ESP_LOGI(TAG, "listing %s", src->dir);
    const int w = orb_files_walk(src, scan_entry, c, &c->counted);
    const int n = c->n;
    if (w >= 0) {
        s_scan_counted = c->counted;
        ESP_LOGI(TAG, "%d of %d entr%slisted in %lld ms", n, c->counted,
                 n == 1 ? "y " : "ies ",
                 (long long)((esp_timer_get_time() - t_scan) / 1000));
    }
    heap_caps_free(c);
    return w < 0 ? -1 : n;
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
    const orb_file_src_t *src = theme_src();
    if (!src) return ESP_ERR_NOT_FOUND;                    /* unreachable by build */
    if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;
    /* The file manager may delete /sd/themes, and FATFS refuses a file whose
     * parent is absent, so the scratch the upload is about to open needs its
     * directory to exist. Re-created here rather than only at mount: the owner
     * should be able to clear the directory and upload again without a reboot. */
    orb_sd_ensure_store_dirs();
    /* Reserve the card read buffer NOW, before the radio carries 3.87 MB for a
     * transfer this device cannot finish. verify_bundle3() reserves it again
     * defensively; this is the early, cheap no. */
    if (ensure_chunk() != ESP_OK) return ESP_ERR_NO_MEM;

    /* D9's arm was moved OFF this path (2026-10-27). It used to erase the slot
     * header here, before a payload byte crossed, so EVERY killed, stalled or
     * refused upload cost the owner their face — bench-proven the same day: one
     * aborted 1.2 MB curl left art_ready false, and the hub's card rows then had
     * nothing to fall back to. The erase's own invariant — a torn store boots the
     * base face because the header goes before the payload — is now held inside
     * copy_bundle3(), ahead of the payload erase it already performs: local
     * flash with no radio in it, past every check the store has to pass.
     * A refused upload now leaves the working slot and the live face exactly as
     * they were. */
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
    orb_sd_note_write();
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
    const orb_file_src_t *src = theme_src();
    if (!src) { *why = "no \"themes\" file source declared"; return ESP_ERR_NOT_FOUND; }
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = orb_sd_why(); return ESP_ERR_NOT_FOUND; }
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
