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
#include "esp_rom_crc.h"
#include "esp_timer.h"              // the listing timing in orb_theme_scan()
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "orb_theme.h"
#include "orb_bundle.h"
#include "orb_clock3.h"          /* the FORMAT 3 clock section's projection (P6.5) */
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
/* The stored bundle's CONTAINER region checksum, read off its slot header at
 * boot: the figure the flip page matches a card file against, where
 * s_active_geom_crc holds the rebase's. Both name the same bytes. */
static uint32_t      s_bundle_file_geom;
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

/* The no-resident-art fallback (§26 P6.5). The compiled FORMAT 2 pair is retired, so
 * there is no baked-in dial to fall back to: a board whose default bundle and installed
 * slot both fail to draw clears the geometry so clock_view's residency gate fails and it
 * paints the base digital face, which still tells the time. s_geo.theme stays NULL and is
 * only ever dereferenced on the themed leg, which the residency gate has already fenced
 * off by the time anything reads it. */
static void use_no_art(void)
{
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

/* Which slot the dial is actually drawing, resolved once by orb_theme_boot(). This is
 * the only honest answer to "which slot must an install never erase", so it is what
 * install_target() alternates against. It starts on the default because a board that
 * has not resolved yet is drawing nothing, and the target that follows from that —
 * slot B — is the one that can never damage what is on the glass. */
static boot_art_t s_art = BOOT_ART_DEFAULT;

/* Which slot the dial draws. Anything that is not the card choice — "default",
 * junk, a key NVS cannot read — is the default, which is the answer that is
 * correct when the owner never chose and the answer that keeps the dial working
 * when the choice itself is damaged. B being valid is boot's evidence, and boot
 * verifies the pixels separately: this predicate decides where to look, not
 * whether it loaded. */
/* What the owner's choice and the slot's contents agree on. Pure, so the host suite
 * drives every case with vectors, and so the dial's answer cannot drift from the page's.
 * §26 (P6.5) retired FORMAT 2, so a bundle is the only drawable thing a slot can hold:
 * `b_is_bundle` is boot's evidence. A choice of "card" that names no drawable bundle is
 * still the default — D13's rule that the fallback never installs, unchanged; only the
 * FORMAT 2 verdict it also consulted is gone. */
static boot_art_t boot_resolve(const char *live, const bool b_is_bundle)
{
    if (!live || strcmp(live, THEME_LIVE_CARD) != 0) return BOOT_ART_DEFAULT;
    return b_is_bundle ? BOOT_ART_CARD : BOOT_ART_DEFAULT;
}

/* The ping-pong (§26 slice 3, owner ruling 2026-10-20): install into whichever slot
 * the dial is NOT drawing. This is the rule ruling D13 deleted, correct again now that
 * D13 is retired — the flashed default is no longer sacred, so slot A is a legal target
 * and the second install destroys it, with reflash as the sanctioned recovery.
 *
 * What survives D13 is the invariant underneath it, which was always the real point:
 * a store never touches the slot being drawn, so a copy interrupted at any moment leaves
 * the live header intact and the dial drawing what it drew before. Slot A's protection
 * was one consequence of that; it is not the rule itself.
 *
 * Callers latch this once and pass the same value to the serial read, the erase, the
 * copy and the header write. Recomputing it per call would let a flip between two of
 * those steps aim the write at a slot the erase emptied. */
static uint32_t install_target(boot_art_t live)
{
    return (live == BOOT_ART_CARD) ? SLOT_A_HDR : SLOT_B_HDR;
}

/* The slot's name for the log, derived from the same base the copy used — so the
 * sentence a store prints cannot disagree with the slot it wrote. */
static const char *slot_name(uint32_t hdr_base)
{
    return (hdr_base == SLOT_A_HDR) ? "slot-A" : "slot-B";
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
 * Nothing here selects the bundle. The install leaves the choice key alone and the
 * dial keeps drawing what it drew; the flip page (P6.4, landed) is the door that
 * presses it, and only it. D26 is honoured on the way — boot says plainly that a
 * bundle in the slot is not drawable yet. */

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
    orb_bundle3_hdr_t h;
    uint32_t fsz = 0;
    const char *cr = card_check3(fd, &h, &fsz);
    if (cr) { r->why = cr; return false; }
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
    if (!r.verified && r.why) { *why = r.why;                                     return -1; }
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
                            uint32_t art_crc, uint32_t serial,
                            uint32_t file_geom_crc)
{
    memset(hdr, 0, W_WORDS * 4);
    hdr[0] = THEME_MAGIC;     hdr[1] = ORB_BUNDLE3_FORMAT;
    hdr[2] = ORB_BUNDLE3_ABI; hdr[3] = W_WORDS * 4;
    hdr[4] = SLOT_GEOM_OFF;   hdr[5] = h->geom_size;  hdr[6] = h->geom_crc;
    hdr[7] = h->art_size;     hdr[8] = art_crc;
    hdr[9] = h->n_sections;   hdr[11] = serial;       hdr[13] = SLOT_ART_OFF;
    /* Word 10 (FORMAT 2's bake duration, always 0 for a bundle) carries the CARD
     * file's region checksum. The slot's own geom_crc is the rebase's figure and
     * cannot be made to equal a file's, and the flip page's active/changed question
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

    /* One target for the whole store. Latched here, before the first read, so the
     * serial probe, the copy, the header erase and the header write cannot be driven by
     * four separate answers even if the live choice were to change underneath us.
     *
     * The serial counts up over whatever THIS slot held; a header that cannot be read
     * restarts the count. Under the ping-pong the slot may hold the flashed default
     * rather than a previous install, which is exactly the case D13 protected and this
     * slice spends: the count is per-slot and nothing compares the two of them. */
    const uint32_t tgt = install_target(s_art);
    uint32_t raw[W_WORDS];
    uint32_t serial = 1;
    if (esp_partition_read(p, tgt, raw, sizeof raw) == ESP_OK) {
        orb_bundle3_hdr_t o3;
        if (orb_bundle3_hdr_parse(raw, &o3) == NULL)   serial = o3.serial + 1;
    }

    staged_mark(id);
    uint32_t art_crc = 0;
    esp_err_t e = copy_bundle3(fd, &b, region, file_art_base, p, tgt,
                               &art_crc, &w);
    card_close(fd);
    heap_caps_free(region);
    if (e != ESP_OK) {
        staged_clear();
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
        return install_failed(e, w, why, whysz);
    }
    staged_clear();
    s_bundle_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_bundle_bytes = h.art_size;
    ESP_LOGI(TAG, "bundle \"%s\" (%s) in %s in %u ms — stored, and not yet chosen",
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
 * the flip page carries the sentence. */
static uint16_t  s_b3_first[GIFS_MAX + 1], s_b3_delays[GIFS_MAX];
static clk_bg_rect_t s_b3_rects[ORB_CLOCK3_N_RECTS];
static uint32_t  s_b3_bgf[ORB_CLOCK3_N_BGF];

static esp_err_t adopt_bundle3(const esp_partition_t *p, const orb_bundle3_hdr_t *h,
                               const uint32_t *raw, const char *src_name,
                               uint32_t slot_hdr_base, bool resident_map)
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

    /* The span to make resident: the installed art region runs to the end of the LAST
     * bank, sector-rounded, which is what copy_bundle3 erased, wrote and read back. */
    uint32_t end = SLOT_ART_OFF;
    for (uint32_t i = 0; i < b.n_sections; i++) {
        const uint32_t e2 = b.sec[i].art_off + b.sec[i].art_size;
        if (e2 > end) end = e2;
    }
    const uint32_t span = (end - SLOT_ART_OFF + THEME_SECTOR - 1) & ~(THEME_SECTOR - 1);

    uint32_t ms = 0;
    esp_err_t fe;
    if (resident_map) {
        /* The default maps its own pixels rather than copying them to the arena
         * (D13's "the default ALWAYS maps"): the flashed slot-A bank is the only copy
         * of these pixels on the board, and it is version-matched to this firmware.
         * mmap is not content-verified here — the bytes were written whole by
         * esptool, and clock_view proves residency on the mapped span. */
        fe = art_bank_map(slot_hdr_base + SLOT_ART_OFF, span);
    } else {
        fe = art_bank_fill(slot_hdr_base + SLOT_ART_OFF, span, true, raw[W_ART_CRC], &ms);
    }
    if (fe != ESP_OK) {
        heap_caps_free(region);
        return fe;
    }

    orb_clock3_io_t io;
    memset(&io, 0, sizeof io);
    io.theme = &s_theme;
    io.rects = s_b3_rects; io.rect_first = s_b3_first;
    io.gif_delays = s_b3_delays; io.bg_frames = s_b3_bgf;
    io.first_cap = GIFS_MAX + 1; io.delays_cap = GIFS_MAX;
    why = orb_clock3_parse(sec, &io);
    heap_caps_free(region);
    if (why) {
        ESP_LOGE(TAG, "bundle \"%s\" has no drawable clock: %s", h->name, why);
        return ESP_FAIL;
    }

    s_geo.theme = &s_theme;
    s_geo.rects = s_b3_rects; s_geo.rect_first = s_b3_first;
    s_geo.gif_delays = s_b3_delays; s_geo.bg_frames = s_b3_bgf;
    s_geo.comps = NULL; s_geo.aux = NULL; s_geo.bank = NULL;
    s_geo.n_comps = 0; s_geo.n_aux = 0; s_geo.n_bank = 0;
    s_geo.art_size = io.out_art_size;
    s_geo.blob_off = io.out_art_off - SLOT_ART_OFF;
    s_geo.from_container = true;

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
    s_refill_ms = ms;
    s_refill_bytes = span;
    return ESP_OK;
}


/* The flashed default, FORMAT 3 (§26 P6.5): slot A's bank now ships as a bundle, so
 * boot reads its header through the bundle door and, if it is one with a clock
 * section, adopts it MAPPED (not arena-filled) exactly as the FORMAT 2 default was
 * mapped. A slot-A header that is not a FORMAT 3 bundle returns non-OK and the caller
 * falls back. This is the only read that turns the flashed pixels into a dial; it
 * does not install or erase anything (D13's no-NVS-write-in-the-fallback rule holds).
 */
static esp_err_t adopt_default_bundle(const esp_partition_t *p)
{
    uint32_t raw[W_WORDS];
    if (esp_partition_read(p, SLOT_A_HDR, raw, sizeof raw) != ESP_OK)
        return ESP_FAIL;
    if (raw[W_MAGIC] != THEME_MAGIC || raw[W_FORMAT] != ORB_BUNDLE3_FORMAT)
        return ESP_ERR_NOT_FOUND;           /* not a bundle: erased, or a FORMAT 2 blob */
    orb_bundle3_hdr_t h;
    if (orb_bundle3_hdr_parse(raw, &h) != NULL)
        return ESP_ERR_INVALID_CRC;         /* claims 3, header will not check */
    return adopt_bundle3(p, &h, raw, "slot-A", SLOT_A_HDR, true);
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

    s_live[0] = '\0';
    if (!nvs_get(THEME_NVS_LIVE, s_live, sizeof s_live) || !s_live[0])
        strlcpy(s_live, THEME_LIVE_DEFAULT, sizeof s_live);
    s_damage_id[0] = '\0';
    s_damage_why[0] = '\0';

    /* Slot B is the only installed theme there is to read TODAY, and the reason is the
     * missing flip door rather than the layout: nothing can yet set the choice to "card",
     * so nothing can ever draw an A-installed bundle, so alternation stays inert and the
     * store's target remains B. §26 (P6.5): a slot holds a FORMAT 3 bundle or nothing —
     * the FORMAT 2 reader is retired — so the header is sniffed once and parsed through
     * the bundle door. A slot that claims FORMAT 3 but whose header will not check is
     * neither drawable nor empty; it gets its own damage sentence (§24.7's lesson that a
     * FORMAT 3 file is never described by a generic "no theme").
     *
     * WHEN THE FLIP DOOR LANDS this sniff and the choice beside it have to become
     * slot-general together: "card" names a slot, boot reads the header of the slot the
     * choice points at, and the other slot becomes the default leg. Shipping a choose
     * door on top of this B-only read is the mistake to avoid — it would let an owner
     * pick a bundle boot then refuses to look for. */
    orb_bundle3_hdr_t b3;
    bool slot_is_bundle = false;
    bool slot_claims3 = false;
    uint32_t raw3[W_WORDS];
    if (esp_partition_read(p, SLOT_B_HDR, raw3, sizeof raw3) == ESP_OK &&
        raw3[W_MAGIC] == THEME_MAGIC && raw3[W_FORMAT] == ORB_BUNDLE3_FORMAT) {
        slot_claims3 = true;
        if (orb_bundle3_hdr_parse(raw3, &b3) == NULL) slot_is_bundle = true;
    }

    /* The `staged` witness expires on the boot that finds the header it waited for,
     * identified by hashing the committed slot's own name back to the marker's id. The
     * committed slot holds a bundle now, so the name is b3.name. A slot naming a
     * DIFFERENT theme is not that commit. One NVS write at boot, and the word it removes
     * is diagnostics, so D13's no-NVS-write-in-the-fallback rule is untouched. */
    s_staged_id[0] = '\0';
    char staged[ORB_THEME_ID_HEX + 1];
    if (nvs_get(THEME_NVS_STAGED, staged, sizeof staged) && theme_id_ok(staged)) {
        bool landed = false;
        if (slot_is_bundle) {
            char cid[ORB_THEME_ID_HEX + 1];
            theme_id(b3.name, cid);
            landed = (strcmp(cid, staged) == 0);
        }
        if (landed) staged_clear();
        else        strlcpy(s_staged_id, staged, sizeof s_staged_id);
    }

    s_art = boot_resolve(s_live, slot_is_bundle);
    if (s_art == BOOT_ART_CARD) {
        /* The chosen slot holds a bundle: its clock section is the dial, and a refusal
         * names itself on the row rather than silently landing on the default. */
        const esp_err_t be = adopt_bundle3(p, &b3, raw3, "slot-B", SLOT_B_HDR, false);
        if (be == ESP_OK) {
            ESP_LOGI(TAG, "bundle \"%s\" from slot-B (chosen): %u B refilled to the "
                          "arena in %u ms", s_active_name,
                     (unsigned)s_refill_bytes, (unsigned)s_refill_ms);
            return ESP_OK;
        }
        char did[THEME_ID_HEX + 1];
        theme_id(b3.name, did);
        strlcpy(s_damage_id, did, sizeof s_damage_id);
        strlcpy(s_damage_why,
                be == ESP_ERR_NOT_FOUND ? "it carries no clock section to draw"
                    : be == ESP_ERR_NO_MEM
                        ? "not enough memory for its pixels — the default is drawn"
                        : "the installed copy did not verify — re-store it",
                sizeof s_damage_why);
        ESP_LOGE(TAG, "the chosen bundle \"%s\" would not load (%s) — drawing the default",
                 b3.name, esp_err_to_name(be));
    } else if (slot_is_bundle) {
        /* Stored, not chosen: nothing is damaged and no row needs a reason. */
        ESP_LOGI(TAG, "slot-B holds bundle \"%s\" — stored, and not chosen", b3.name);
    } else if (slot_claims3) {
        /* Chosen as "card", the slot claims FORMAT 3, and its header will not parse:
         * damage the owner can act on (re-store), with no id to hang it on because the
         * id hashes a name this header cannot be trusted to carry. */
        s_damage_id[0] = '\0';
        strlcpy(s_damage_why, "the stored bundle's header is damaged — re-store it",
                sizeof s_damage_why);
        ESP_LOGE(TAG, "slot-B claims FORMAT 3 and its header would not parse — "
                      "drawing the default");
    } else {
        ESP_LOGI(TAG, "no installed theme — reading the flashed default");
    }
    /* Every path from here draws the default, so the card answer above is void for
     * targeting purposes even when it was the owner's choice: a card that would not
     * load is not on the glass, and the ping-pong keys on what IS. Leaving s_art on
     * CARD here would aim the next store's erase at the slot being drawn — the one
     * failure the invariant exists to prevent. It is corrected at the source rather
     * than at each install, so the two can never disagree. */
    s_art = BOOT_ART_DEFAULT;

    /* The default leg (D13 as ruled option A stands): slot A is the flashed bank, the
     * only copy of the factory pixels on the board, and it is mapped — never arena-
     * filled. §26 P6.5: those pixels now ship as a FORMAT 3 bundle, so boot reads slot
     * A through the bundle door first. A bundle with a clock section is drawn; a
     * board whose slot A is not a drawable bundle falls through to the base render. */
    const esp_err_t de = adopt_default_bundle(p);
    if (de == ESP_OK) {
        ESP_LOGI(TAG, "default bundle \"%s\" from slot-A: mapped, %u B resident",
                 s_active_name, (unsigned)s_refill_bytes);
        return ESP_OK;
    }
    /* No drawable default and no chosen card that loads: the compiled FORMAT 2 pair is
     * retired, so there is no second copy of the factory pixels to fall back to. The
     * board still boots — clock_view's residency gate fails and it paints the base
     * digital face — and the reason is on the serial log for whoever reflashes it. */
    use_no_art();
    ESP_LOGE(TAG, "no drawable theme (default slot-A refused: %s) — base digital face",
             esp_err_to_name(de));
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
    theme_id(b3.name, bid);
    strlcpy(e->name, b3.name, sizeof e->name);
    e->fmt = 3;
    e->n_sections = b3.n_sections;
    e->art_size = b3.art_size;
    e->serial = b3.serial;
    if (strcmp(bid, rec->name) != 0) {
        strlcpy(e->why, "the file is not named for its own theme name — re-upload it",
                sizeof(e->why));
        e->err = ESP_ERR_INVALID_ARG;
        c->n++;
        return true;
    }
    strlcpy(e->id, bid, sizeof e->id);
    e->err = ESP_OK;
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
    const char *r = card_check3(fd, &b3, &fsz);
    if (!r) r = verify_bundle3(fd, &b3, fsz);
    close(fd);

    /* The card filename is the id, and the id is the hash of the header's owner name.
     * Nothing the client named this upload — the destination is whatever this decides,
     * from bytes that had to arrive intact. card_check3() already refused a container
     * with no name in it, so the name here is never empty and theme_id() cannot be
     * asked for the hash of nothing. */
    char id[THEME_ID_HEX + 1];
    char final[ORB_SD_PATH_MAX];
    if (!r) theme_id(b3.name, id);
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
                /* Idempotent: identical bundle, discard the scratch, nothing changed. */
                unlink(up->tmp); orb_sd_unlock(); free(up);
                ESP_LOGI(TAG, "upload of \"%s\" (%s) is unchanged — kept as is", b3.name, id);
                return ESP_OK;
            }
            if (!overwrite) {
                /* Same name, different bytes. The existing file is left intact; the
                 * scratch is deleted so no rejected upload survives to confuse the list. */
                unlink(up->tmp); orb_sd_unlock();
                *why = "a different theme with that name is already on the card";
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
    ESP_LOGI(TAG, "bundle \"%s\" uploaded to the card as %s%s", b3.name, id,
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
