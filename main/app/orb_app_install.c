/*
 * main/app/orb_app_install.c — the app installer (plan P4, §2/§3.2/D9).
 *
 * Two halves, split on the same line every sibling layer draws:
 *
 *   install_decide() is PURE — the cap, the quota, the slot assignment and the
 *   serial, decided over a header the reader already accepted and a slot state
 *   the caller read under the lock. tools/host_orbapp_install_test.py lifts it
 *   verbatim (the way host_orbapp_scan_test.py lifts row_decide) and drives it
 *   with the real writer's headers plus a synthetic over-cap / over-quota state,
 *   so the ordering the device depends on is pinned off the board.
 *
 *   orb_app_install() is the ladder — resolve by walk, re-verify identity inside
 *   the card lock, checksum the whole payload OFF the card, then and only then
 *   erase, copy the geometry, read it back and CRC it FROM the partition, and
 *   write the slot header last. It mirrors orb_theme_install_bundle() (orb_theme.c:1456)
 *   and inherits that function's card-lock discipline whole: this runs on the
 *   worker task (posted through orb_jobs), never on the LVGL task or the httpd
 *   task, and it never takes the mutex a lock holder is already holding. The
 *   one thing that makes a slot live is the header write (D9); a copy torn by a
 *   power cut or a pulled card has no header and so reads as an ABSENT app, never
 *   as corrupt art, and the boot ladder falls back rather than drawing it.
 *
 *   Grants are NOT written here (D19): orb_app_install fills orb_app_t.caps from
 *   the manifest and never a grant word, so an absent orb.grants means
 *   as-declared for the new app exactly as the broker already reads it.
 */
#include "orb_app_install.h"

#include <stdio.h>
#include <string.h>

#include "orb_svc.h"   /* ORB_CFG_QUOTA_BYTES, ORB_SET_STRING, orb_setting_desc_t */
#include "orb_app_wasm.h"  /* P7.4: orb_wasm_loaded/_load/_bind_app for the load rung */
#include "input_router.h"  /* D38: input_router_request — the task-safe home switch */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* How long the uninstall door waits for its own home request to land on the
 * glass. The router poll runs every 10 ms; 1500 ms is two orders of margin for
 * a dial mid-frame, and it bounds the one httpd worker rather than hanging it. */
#define UNHOME_WAIT_MS 1500

#define APPS_STR_WORST ((uint32_t)sizeof(((orb_setting_desc_t *)0)->def_str))
#define APPS_NUM_WORST 4u   /* one nvs_set_u32 value: int/enum/toggle */

/* The pure decision. It fills v->why and returns false to refuse; on accept it
 * has chosen the slot and the serial. The gates are ordered so the cheapest and
 * most specific refusal wins: an oversized geometry is said before the cap is
 * counted, so a 9th app that is ALSO too big is refused for its size — the one
 * the owner can act on without uninstalling anything. */
bool install_decide(const orbapp_hdr_t *h, const orbapp_install_slots_t *sl,
                    orbapp_install_verdict_t *v)
{
    v->accept = false;
    v->slot = -1;
    v->reinstall = false;
    v->serial = 0;
    v->cfg_worst = 0;
    v->why[0] = '\0';

    /* Gate 1 — the manifest+geometry+code prefix the slot must hold. The blob
     * copied into a slot is the container's bytes [0, region_end): header, the
     * settings table the reader unpacked, the geometry the app draws from, and —
     * since P7 — the wasm code region that rides the sector-aligned slot art's
     * payload vacated. D27 is that "a module that cannot fit never writes a byte of
     * flash", so the code region enters this arithmetic and an oversized module is
     * refused before the erase, not after a slot is spent. Since D21 (§0.9) no art
     * term enters: the reader refuses any container with art bytes before here. The
     * reader already capped code_size at ORBAPP_CODE_BYTES_MAX, so the max() term
     * fires only if that cap and the slot geometry drift apart. == cap is ACCEPTED:
     * the last word before the header sector. */
    uint64_t blob = (uint64_t)h->geom_off + h->geom_size;
    if (h->code_size) {
        const uint64_t code_end = (uint64_t)h->code_off + h->code_size;
        if (code_end > blob)
            blob = code_end;
    }
    if (blob > (uint64_t)APPS_SLOT_GEOM_CAP) {
        snprintf(v->why, sizeof v->why,
                 "its manifest, geometry and code are %llu B: a slot holds %u B —"
                 " re-bake the app smaller",
                 (unsigned long long)blob, (unsigned)APPS_SLOT_GEOM_CAP);
        return false;
    }

    /* Gate 1b — D27's memory ceiling, enforced off the board's real budget before
     * the erase. The reader already refused a bound past ORBAPP_WASM_MEM_MAX, so
     * this is the defensive twin the host suite drives: a code-bearing app whose
     * declared linear memory this build cannot allocate never gets a slot to try,
     * and the refusal names the ceiling so the owner can act on it. */
    if (h->wasm_mem_max > (uint32_t)ORBAPP_WASM_MEM_MAX) {
        snprintf(v->why, sizeof v->why,
                 "declares a %u B wasm memory bound past the %u B this build gates "
                 "at (D27) — re-package it smaller",
                 (unsigned)h->wasm_mem_max, (unsigned)ORBAPP_WASM_MEM_MAX);
        return false;
    }


    /* Gate 2 — the config quota, priced at the declared settings' NVS worst case
     * (§9.4 item 4). A STRING can be written to its full def_str width at runtime,
     * so it is charged there and not at its default's length; an int/enum/toggle
     * costs the four bytes nvs_set_u32 stores. A legal manifest fits this by
     * construction (16 rows x 32 B = 512 B < the 2048 B quota), so the refusal is
     * the defensive boundary the host suite drives by re-pricing the quota —
     * the installer refuses the PACKAGE here, before the erase, so an app that
     * could never store its own settings is never given a slot to try. */
    uint32_t worst = 0;
    const int n = (sl->n_settings > (int)ORBAPP_SETTINGS_MAX)
                      ? (int)ORBAPP_SETTINGS_MAX : sl->n_settings;
    for (int i = 0; i < n; i++) {
        const orb_setting_desc_t *s = &sl->settings[i];
        worst += (s->type == ORB_SET_STRING) ? APPS_STR_WORST : APPS_NUM_WORST;
    }
    v->cfg_worst = worst;
    if (worst > ORB_CFG_QUOTA_BYTES) {
        snprintf(v->why, sizeof v->why,
                 "its %d setting(s) need %u B against the %d B app quota — "
                 "uninstall one that declares less",
                 n, (unsigned)worst, ORB_CFG_QUOTA_BYTES);
        return false;
    }

    /* Gate 3 — the cap (B5) and the slot that follows from it. A slug already
     * installed is a REUSE, not a new install: it does not bill the cap and it
     * keeps its slot's serial, so re-copying the same app can never be the thing
     * that fills the dial or moves the broker's grant word. A new slug must find
     * room among the 8 slots, and the cap is hard — the answer to the 9th names
     * the number. */
    if (sl->existing_slot >= 0) {
        if (sl->existing_slot >= APPS_N_SLOTS) {
            snprintf(v->why, sizeof v->why, "its slot number is out of range");
            return false;
        }
        v->accept = true;
        v->reinstall = true;
        v->slot = sl->existing_slot;
        v->serial = sl->slot_serial[sl->existing_slot];   /* kept, not bumped */
        return true;
    }

    if (sl->installed_count >= APPS_N_SLOTS) {
        snprintf(v->why, sizeof v->why,
                 "the dial already holds %d apps — uninstall one to make room",
                 (int)sl->installed_count);
        return false;
    }

    int free_slot = -1;
    for (int i = 0; i < APPS_N_SLOTS; i++)
        if (!sl->slot_used[i]) { free_slot = i; break; }
    if (free_slot < 0) {
        snprintf(v->why, sizeof v->why,
                 "the dial already holds %d apps — uninstall one to make room",
                 (int)sl->installed_count);
        return false;
    }

    v->accept = true;
    v->reinstall = false;
    v->slot = free_slot;
    v->serial = sl->slot_serial[free_slot] + 1u;   /* fresh slot: 0+1; a stale
                                                    * slot keeps counting up */
    return true;
}

/* =========================================================================== *
 * The device half. Nothing below install_decide() is lifted by the host suite —
 * it is the ladder that mirrors orb_theme_install_bundle() (orb_theme.c:1456), reached
 * only from the worker task. It keeps that function's three card rules whole:
 * every path that opens a file on /sd takes the one card lock itself, never
 * through a helper that already holds it; the WHOLE payload checksums off the
 * card before the first erase; and the slot header goes last, so a torn copy
 * reads absent rather than corrupt (D9).                                     *
 * =========================================================================== */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>          /* calloc/free for the upload state */
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

#include "app/orb_app_scan.h"   /* the ("orb","apps") source id + dir + ext */
#include "orb_jobs.h"           /* orb_app_install_post's worker posting */
#include "sd/orb_files.h"       /* the OS owns the walk; this interprets it */
#include "sd/orb_sd.h"          /* lock/unlock, mounted, name_ok, path */

static const char *TAG = "appinstall";

/* What the last install attempt was on and what it failed with — the two
 * accessors the /apps refresh reads so the page shows the same words the dial
 * would (orb_theme.c's s_try_id/s_try_why rule). */
static char s_try_stem[ORB_SD_NAME_MAX + 1];
static char s_try_why[128];

const char *orb_app_install_try_stem(void) { return s_try_stem; }
const char *orb_app_install_try_why(void)  { return s_try_why; }

static void try_note(const char *stem, const char *why)
{
    snprintf(s_try_stem, sizeof s_try_stem, "%s", stem ? stem : "");
    snprintf(s_try_why, sizeof s_try_why, "%s", why ? why : "");
}

/* The 4 KB stream buffer, INTERNAL because it is the source/sink of the flash
 * driver's own DMA and (on the card side) the SDMMC one — the exact reason
 * orb_theme.c:742 reserves it internal and not in PSRAM. */
static uint8_t *s_chunk;
#define APPS_CHUNK_BYTES 4096            /* the ask */
#define APPS_CHUNK_MIN   1024            /* below this the copy is mostly command overhead */
static uint32_t  s_chunk_bytes;          /* what the heap granted; every clamp site reads THIS */

/* The size is a ladder, not a constant, because the board's real budget does not
 * contain 4 KB contiguous after association: the measured steady state is
 * `internal free=8,139 largest=3,328` (orb_net's own heap line), and this module's
 * flat 4 KB ask refused the install on exactly the wall orb_theme.c records at
 * §16.1. Granting 2 KB costs four extra round trips per 8 KB copied; the SPI clock,
 * not the chunk, is the floor. Below 1 KB the command overhead leads the data, so
 * the ladder stops — the same three rungs and the same reasoning as
 * orb_theme.c's ensure_chunk(). */
static esp_err_t ensure_chunk(void)
{
    if (s_chunk) return ESP_OK;
    for (uint32_t n = APPS_CHUNK_BYTES; n >= APPS_CHUNK_MIN; n /= 2) {
        s_chunk = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_chunk) { s_chunk_bytes = n; return ESP_OK; }
    }
    return ESP_ERR_NO_MEM;
}

/* The apps partition found by label — the sibling of art_bank_partition(), but
 * looked up here because this is the only layer that writes the label in
 * partitions.csv (offset 0xD80000 size 0x280000). NULL means a board whose
 * partition table predates P4: the honest answer is NOT_FOUND, not a crash. */
static const esp_partition_t *apps_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY,
                                    APPS_PARTITION_NAME);
}

/* Read n bytes at *io_pos with pread, never read: the installer walks one fd at
 * two cursors (header, then the prefix copy), and a shared offset across them is
 * the bug orb_theme.c:771 documents. */
static ssize_t rd_at(int fd, size_t *io_pos, uint8_t *dst, size_t n)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t got = pread(fd, dst + done, n - done, (off_t)(*io_pos + done));
        if (got < 0) return -1;
        if (got == 0) break;
        done += (size_t)got;
    }
    *io_pos += done;
    return (ssize_t)done;
}

/* ---- the read source: a card fd, or a container already in memory ----------
 *
 * The ladder's gates are the ladder — resolve by walk, full-payload CRC off the
 * sink, erase, copy, read-back CRC, header last (D9). Only the WHERE differs: a
 * card app arrives at <stem>.orbapp and reads through one locked fd; a memory
 * source hands the ladder bytes the caller already holds, read straight from
 * flash-mapped .rodata. One source struct, one ladder, and the two cannot drift
 * apart — which is why the D36 seed ships no in-image apps: the card path is
 * the only shipped caller. A mem-source install takes no card lock and owns no
 * fd; install_close() only closes what the source opened. */
typedef struct {
    int              fd;        /* card path: the open fd; -1 for a memory source */
    const uint8_t   *mem;       /* memory path: the container's byte 0; NULL for the card */
    size_t           mem_len;   /* valid bytes at mem */
    bool             locked;    /* memory sources never own the card lock */
} appsrc_t;

static ssize_t src_read(const appsrc_t *s, size_t pos, uint8_t *dst, size_t n)
{
    if (s->fd >= 0) {
        size_t p = pos;
        return rd_at(s->fd, &p, dst, n);
    }
    if (pos >= s->mem_len) return 0;
    if (pos + n > s->mem_len) n = s->mem_len - pos;
    memcpy(dst, s->mem + pos, n);
    return (ssize_t)n;
}

/* D36 (§0.11): the in-image factory containers are gone. The firmware ships no
 * apps; the hub seeds the clock and the HSB editor onto the card as ordinary
 * .orbapp files, and they arrive through orb_app_install below exactly like
 * every other app. orb_app_install_image stays as the memory-source front the
 * sentinel pins and any future in-image source would use — with no shipped
 * caller, on purpose. */

/* One place to fail: the page shows why, the log shows the same sentence, and
 * the caller gets the esp_err_t that decides whether a reboot is needed
 * (orb_theme.c:1443's install_failed, same shape). */
static esp_err_t install_failed(esp_err_t e, const char *w, char *why, size_t whysz)
{
    if (why && whysz) snprintf(why, whysz, "%s", w ? w : "install failed");
    ESP_LOGW(TAG, "install: %s", w ? w : "install failed");
    return e;
}

/* Read and decode the header block from an already-open, already-locked fd, then
 * run the reader's full ladder — parse (magic/CRC), the semantic check with the
 * filename stem in the identity triple, and the file's own placement rule the
 * copy depends on for geom_off/geom_size. Returns NULL on success or the reader's
 * own sentence. The caller owns the fd. When `raw` is non-NULL it also copies the
 * ORBAPP_HDR_WORDS little-endian words it validated, so the installer writes the
 * slot header VERBATIM — the same bytes whose hdr_crc just passed — rather than
 * re-encoding the decoded struct, which is the field-by-field drift the whole
 * reader/writer/host mirror exists to prevent. */
static const char *read_and_check_header(const appsrc_t *s, orbapp_hdr_t *h, const char *stem,
                                         uint32_t *raw)
{
    uint32_t hdr[ORBAPP_HDR_WORDS];
    if (src_read(s, 0, (uint8_t *)hdr, sizeof hdr) != (ssize_t)sizeof hdr)
        return "the file ends inside its own header block — re-copy it onto the card";

    /* STATIC, like s_try_why above: the return value is the caller's refusal
     * sentence and outlives this frame — the upload door snprintfs it into the
     * 422 body after close() and the unlock have both reused the stack. The P4b
     * bench watched a live D21 refusal arrive as `#\x04\x06` for exactly that
     * reason. One validator per task (install on the worker, upload on httpd,
     * each serialised by its own slot), so one static per validator is safe. */
    static char why[160];
    const char *r = orbapp_parse_header(hdr, h, why, sizeof why);
    if (r) return r;

    /* A card install cross-checks the filename stem against the header (the C11
     * triple); a memory source HAS no filename, and its identity is the slug it
     * declares — which the writer already insisted the stem match at bake time,
     * and the freshness sentinel re-asserts every run. So a NULL stem means
     * "the header's own slug is the stem", the same equality, checked not skipped. */
    char fname[ORBAPP_SLUG_MAX + 16];
    snprintf(fname, sizeof fname, "%s%s", stem ? stem : h->slug, ORB_APP_EXT);
    r = orbapp_check_header(h, 0, false, fname, why, sizeof why);
    if (r) return r;

    /* The placement rule is the one the copy indexes by: geom_off must sit past
     * the header and the settings table, so a file whose parts are misplaced is
     * refused here, before any byte of it reaches a slot. */
    r = orbapp_check_file_layout(h, why, sizeof why);
    if (r) return r;

    if (raw) memcpy(raw, hdr, sizeof hdr);
    return NULL;
}



/* The geometry CRC, checksummed OFF the card, the way the writer emitted it:
 * crc32 starting at 0 the way zlib.crc32 / esp_rom_crc32_le compose them
 * (orbapp.py's order). The ART CRC is not read — D21 (§0.9): the reader has
 * already refused any container that ships art bytes or carries a non-empty art
 * crc before this runs, so there is no second payload left to check. Mirrors the
 * theme door's verify_bundle3() (orb_theme.c) minus the geometry COUNT check — that belongs
 * to the painter and lands with P5's compositor, not the byte-for-byte checksum the
 * copy needs. */
static const char *verify_payload(const appsrc_t *s, const orbapp_hdr_t *h)
{
    if (ensure_chunk() != ESP_OK) return "out of memory for the card read buffer";

    uint32_t crc = 0;
    size_t pos = h->geom_off, left = h->geom_size;
    while (left) {
        const size_t want = left > s_chunk_bytes ? s_chunk_bytes : left;
        if (src_read(s, pos, s_chunk, want) != (ssize_t)want)
            return "the file is shorter than its geometry claims — re-copy it";
        crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
        pos += want; left -= want;
    }
    if (crc != h->geom_crc)
        return "the geometry does not match its checksum — re-copy the file onto the card";

    /* P7 (D20/D27): the code region is a second payload, copied into the slot by
     * copy_prefix just as the geometry is, so it gets the same OFF-CARD checksum
     * before the erase — a module that would arrive corrupt on the dial must be
     * refused here and never cost a slot. D27's promise is that a module which
     * cannot fit or cannot trust its own bytes "never writes a byte of flash", and
     * the geometry's crc32 is checked exactly this way; the code's is now its twin.
     * A native app has code_size 0 and skips this whole block, so the pre-P7 verify
     * is byte-for-byte what it was. */
    if (h->code_size) {
        crc = 0;
        pos = h->code_off;
        left = h->code_size;
        while (left) {
            const size_t want = left > s_chunk_bytes ? s_chunk_bytes : left;
            if (src_read(s, pos, s_chunk, want) != (ssize_t)want)
                return "the file is shorter than its code claims — re-copy it";
            crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
            pos += want; left -= want;
        }
        if (crc != h->code_crc)
            return "the code does not match its checksum — re-copy the file onto the card";
    }
    /* D37: the editor region is checksummed here, over the card, and then NOT
     * copied — the one place in the install path that reaches past the installed
     * prefix, and deliberately so (§4.2). This is a store-time verify: the card
     * keeps the bytes, no slot ever holds them, and the sentence below therefore
     * refuses the whole upload rather than degrading one app's face. The reason a
     * corrupt editor must stop the install instead of riding along unnoticed is
     * that the container's own claim is that it ships an editor; a door that
     * accepted a file whose editor could not be read would be reporting a
     * capability the hub would then fail to serve, hours later, with no
     * explanation left to give. A native app with no region skips the whole
     * block, exactly as the code check above does, so no pre-D37 container's
     * install path changes by one byte. */
    if (h->editor_size) {
        crc = 0;
        pos = h->editor_off;
        left = h->editor_size;
        while (left) {
            const size_t want = left > s_chunk_bytes ? s_chunk_bytes : left;
            if (src_read(s, pos, s_chunk, want) != (ssize_t)want)
                return "the file is shorter than its editor claims — re-copy it";
            crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
            pos += want; left -= want;
        }
        if (crc != h->editor_crc)
            return "the editor does not match its checksum — re-copy the file onto the card";
    }
    return NULL;
}

/* ---- resolve-by-stem, then open-and-re-verify under the lock ----------------
 *
 * Two passes, both required, mirroring card_open_verified_id (orb_theme.c:997):
 * pass one is orb_files_walk's (it owns the card lock and hands the callback an
 * open fd), which proves a file named <stem>.orbapp exists AND survives the
 * header ladder, so a file swapped into its place between listing and install
 * cannot install. Pass two opens the stem's own validated path under a fresh
 * lock and re-runs the header ladder — the bytes this call then reads are the
 * ones that passed, not the ones the laptop may have edited in between. */
typedef struct {
    const char *want;      /* the stem being resolved */
    bool        found;     /* an entry with that stem appeared */
    bool        verified;  /* it survived the header ladder */
} resolve_t;

static bool resolve_entry(void *arg, const orb_file_rec_t *rec, int fd)
{
    resolve_t *r = arg;
    if (strcmp(rec->name, r->want) != 0) return false;   /* keep looking */
    r->found = true;
    const appsrc_t s = { .fd = fd, .locked = false };    /* the walk owns the lock */
    orbapp_hdr_t h;
    const char *bad = read_and_check_header(&s, &h, r->want, NULL);
    r->verified = (bad == NULL);
    return false;   /* one match is enough; stop the walk */
}

/* Returns an open fd with the card lock HELD, plus the decoded header and its raw
 * words, or -1 and the reason (the caller owns the fd and the lock together,
 * released by install_close()). Mirrors the pair orb_theme.c's card_open/card_close
 * run. */
static int open_verified_stem(const orbapp_hdr_t **h_out, orbapp_hdr_t *h_storage,
                              uint32_t *raw_out, const char *stem, const char **why)
{
    *why = NULL;
    *h_out = NULL;
    const orb_file_src_t *src = orb_files_src(ORB_APP_SCAN_SLUG, ORB_APP_SCAN_ID);
    if (!src) { *why = "no \"apps\" file source declared"; return -1; }
    /* The mount check goes HERE, before the lock, exactly as orb_theme.c:947
     * learns it the hard way: orb_sd_ensure_mounted() takes the same non-recursive
     * mutex the walk and this open then need, so calling it inside the lock dead-
     * locks the one httpd worker with the dial still ticking. */
    if (orb_sd_ensure_mounted() != ESP_OK) { *why = "no card in the socket"; return -1; }
    if (!orb_sd_name_ok(stem))            { *why = "that is not a usable app name"; return -1; }

    /* Pass one: the walk proves the file exists and passes the header ladder. It
     * takes the lock itself, so it runs OUTSIDE ours. */
    resolve_t r = { .want = stem, .found = false, .verified = false };
    if (orb_files_walk(src, resolve_entry, &r, NULL) < 0) { *why = "no card in the socket"; return -1; }
    if (!r.found)    { *why = "no such app on the card"; return -1; }
    if (!r.verified) { *why = "that file failed the app reader — see the /apps row"; return -1; }

    /* Pass two: open by the validated path under our own lock and re-run the
     * header ladder, so the bytes the copy reads are the bytes that passed. */
    char path[ORB_SD_PATH_MAX];
    orb_sd_lock();
    if (!orb_files_path(src, stem, path, sizeof path)) {
        orb_sd_unlock(); *why = "that is not a usable app name"; return -1;
    }
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        orb_sd_unlock();
        *why = errno == ENOENT ? "no such app on the card" : "cannot open the app file";
        return -1;
    }
    const appsrc_t hsrc = { .fd = fd, .locked = true };   /* the fd carries the lock */
    const char *bad = read_and_check_header(&hsrc, h_storage, stem, raw_out);
    if (bad) { close(fd); orb_sd_unlock(); *why = bad; return -1; }
    *h_out = h_storage;
    return fd;
}

/* Release both together — the fd and the one card lock — never separately, the
 * way card_close() (orb_theme.c:956) owns both. */
static void install_close(int fd)
{
    if (fd >= 0) close(fd);
    orb_sd_unlock();
}

/* ---- the slot ledger read + the flash copy --------------------------------- *
 * Every function here runs with the card lock already held (the caller opened
 * through open_verified_stem), and touches flash — never the card mutex again.
 * esp_partition_read on the header block mirrors card_open_verified_id3 (orb_theme.c):
 * read, not mmap; 148 bytes do not justify an MMU entry and the header is not
 * mapped. The apps slots hold no pixels in P4, so there is no pixels read-back —
 * only the geometry region, CRCed FROM the partition before the header goes live,
 * exactly as card_open_verified_id3 does for its bundle (orb_theme.c).           */

static uint32_t slot_base(int slot) { return (uint32_t)slot * APPS_SLOT_STRIDE; }

/* Read every slot's header under the current lock and build the state install_-
 * decide() prices: which slots are live, their serials, whether this slug already
 * owns one, and the installed tally the B5 cap reads. A slot whose header fails to
 * parse (blank after erase, or a torn copy that never got a header — D9) is simply
 * unused, so the ladder never counts a dead slot as an app. */
static void read_slots(const esp_partition_t *p, const char *slug,
                       orbapp_install_slots_t *sl)
{
    memset(sl, 0, sizeof *sl);
    sl->existing_slot = -1;
    for (int i = 0; i < APPS_N_SLOTS; i++) {
        uint32_t hdr[ORBAPP_HDR_WORDS];
        if (esp_partition_read(p, slot_base(i), hdr, sizeof hdr) != ESP_OK) continue;
        orbapp_hdr_t h;
        char why[64];
        if (orbapp_parse_header(hdr, &h, why, sizeof why)) continue;   /* absent slot */
        sl->slot_used[i] = true;
        sl->slot_serial[i] = h.serial;
        sl->installed_count++;
        if (slug && strcmp(h.slug, slug) == 0) sl->existing_slot = i;
    }
}

/* Copy the container's manifest+geometry prefix — bytes [0, geom_off+geom_size) —
 * into one slot's blob region, then leave the header sector erased. The header is
 * the caller's to write last. ORDER IS THE WHOLE POINT (orb_theme.c:1304): erase,
 * copy, and only the CALLER makes it live; a power cut anywhere in here leaves the
 * slot headerless and so reading as no app at all, never as a half-installed app. */
static esp_err_t copy_prefix(const esp_partition_t *p, const appsrc_t *s,
                             const orbapp_hdr_t *h, int slot, const char **why)
{
    if (ensure_chunk() != ESP_OK) { *why = "out of memory for the card read buffer"; return ESP_ERR_NO_MEM; }

    const uint32_t base = slot_base(slot);
    /* P7: the copied prefix is the geometry's end OR the code region's end, the
     * same max install_decide priced the slot against. A native app has code_size 0
     * and this stays geom_end — the pre-P7 copy is byte-identical. A module-bearing
     * app gets its code bytes into the slot too, so the header's code triple points
     * at bytes that are actually there (the integrity P7.2's loader will trust). */
    uint32_t blob = h->geom_off + h->geom_size;
    if (h->code_size && (h->code_off + h->code_size) > blob)
        blob = h->code_off + h->code_size;
    const uint32_t blob_align = (blob + APPS_SECTOR - 1) & ~(uint32_t)(APPS_SECTOR - 1);
    if (blob > (uint32_t)APPS_SLOT_GEOM_CAP) { *why = "the app is larger than a slot"; return ESP_ERR_INVALID_SIZE; }

    /* Erase the header sector (clear any prior install, D9's "live" bit) and the
     * blob area, each bounded to what is actually written and sector-aligned so
     * the cache-suspend never spans a partial sector (D6). */
    esp_err_t e = esp_partition_erase_range(p, base, APPS_SLOT_HDR_BYTES);
    if (e != ESP_OK) { *why = "could not clear the slot header"; return e; }
    e = esp_partition_erase_range(p, base + APPS_SLOT_HDR_BYTES, blob_align);
    if (e != ESP_OK) { *why = "flash erase failed"; return e; }

    size_t pos = 0;
    for (uint32_t done = 0; done < blob; ) {
        const uint32_t want = (blob - done > s_chunk_bytes) ? s_chunk_bytes : (blob - done);
        if (src_read(s, pos, s_chunk, want) != (ssize_t)want) {
            *why = "the app file ended before its declared geometry — re-copy it";
            return ESP_ERR_INVALID_SIZE;
        }
        e = esp_partition_write(p, base + APPS_SLOT_HDR_BYTES + done, s_chunk, want);
        if (e != ESP_OK) { *why = "flash write failed on the geometry"; return e; }
        pos += want;
        done += want;
    }
    return ESP_OK;
}

/* CRC the copied geometry back FROM the partition, before the header goes live —
 * the read-back the theme install runs (orb_theme.c:1501). Reading through the
 * chunk buffer, not a mapping, because nothing here maps the apps partition. */
static esp_err_t verify_slot_geom(const esp_partition_t *p, const orbapp_hdr_t *h,
                                  int slot, const char **why)
{
    if (ensure_chunk() != ESP_OK) { *why = "out of memory for the read buffer"; return ESP_ERR_NO_MEM; }
    const uint32_t base = slot_base(slot) + APPS_SLOT_HDR_BYTES;
    uint32_t crc = 0;
    for (uint32_t off = 0; off < h->geom_size; ) {
        const uint32_t want = (h->geom_size - off > s_chunk_bytes) ? s_chunk_bytes
                                                                       : (h->geom_size - off);
        if (esp_partition_read(p, base + h->geom_off + off, s_chunk, want) != ESP_OK) {
            *why = "could not read the geometry back"; return ESP_FAIL;
        }
        crc = esp_rom_crc32_le(crc, s_chunk, want);
        off += want;
    }
    if (crc != h->geom_crc) {
        *why = "the geometry did not read back intact — re-copy the file or try another card";
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

/* Write the slot header last, verbatim — the same ORBAPP_HDR_WORDS whose hdr_crc
 * the reader validated. This single write is what makes the slot live (D9): until
 * it lands the slot reads as no app, and after it the boot ladder finds the app. */
static esp_err_t write_slot_header(const esp_partition_t *p, int slot,
                                   const uint32_t *raw, const char **why)
{
    const esp_err_t e = esp_partition_write(p, slot_base(slot), raw, ORBAPP_HDR_BYTES);
    if (e != ESP_OK) *why = "could not make the new app live";
    return e;
}


/* =========================================================================== *
 * orb_app_install — the ladder, and orb_app_install_post — its worker gate.
 * =========================================================================== */

/* Install /sd/apps/<stem>.orbapp into one apps-partition slot and make it a
 * resident card app. Worker task only (posted, never called on the LVGL or httpd
 * task): the copy holds the one card lock for seconds. Everything before the first
 * erase can fail as often as it likes; the flash is touched only after the whole
 * payload has checksummed off the card, which is why a damaged file costs a
 * sentence and not a slot (card_open_verified_id3's rule, orb_theme.c). */
/* The two sources' shared ladder. Everything from the settings scratch down is
 * identical for a card fd and a memory image - unpack the declared settings,
 * price the pure decision, wipe a dirty slot's stale owner words, erase, copy,
 * read back, write the header LAST (D9), commit the row. `src` is already
 * header-verified by the caller's front half; `stem` labels the log only (a
 * memory install passes NULL and the header's slug carries its identity).
 *
 * The PSRAM scratch note below belongs to orb_jobs's 4096 B worker and the main
 * task's 3584 B boot stack alike: it is why the two tables never sit in a frame
 * of either caller, and the ladder keeps that rule for both sources. */
static esp_err_t install_ladder(const esp_partition_t *p, const appsrc_t *src,
                                const orbapp_hdr_t *h, const uint32_t *raw,
                                const char *stem, char *why, size_t whysz)
{
    const int64_t t0 = esp_timer_get_time();
    uint8_t *scratch = heap_caps_malloc(ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS * 4u +
                                        ORBAPP_SETTINGS_MAX * sizeof(orb_setting_desc_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch)
        return install_failed(ESP_ERR_NO_MEM, "out of memory for the settings table",
                              why, whysz);
    uint32_t *sw = (uint32_t *)scratch;
    orb_setting_desc_t *settings = (orb_setting_desc_t *)(scratch +
                                        ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS * 4u);
    esp_err_t ret = ESP_OK;

    /* The WHOLE payload checksums off the card before any erase. Both CRCs, over
     * the full geometry and the full art range, exactly as orb_theme.c verifies a
     * container — the half-copy D9 prices is caught here, on a page that still
     * lists the card, not after a reboot into a slot that will not draw. */
    const char *vv = verify_payload(src, h);
    if (vv) { ret = install_failed(ESP_ERR_INVALID_CRC, vv, why, whysz); goto out; }

    /* Unpack the declared settings — needed twice over: the quota prices them and
     * the resident record carries them (only these ids may reach NVS, §4.2). The
     * tables live in the PSRAM scratch above, never this frame. */
    int n_settings = 0;
    if (h->n_settings > 0) {
        const size_t need = (size_t)h->n_settings * ORBAPP_SETTINGS_WORDS * 4u;
        if (src_read(src, h->setting_off, (uint8_t *)sw, need) != (ssize_t)need) {
            ret = install_failed(ESP_ERR_INVALID_SIZE,
                                 "the app file ended inside its settings table — re-copy it",
                                 why, whysz);
            goto out;
        }
        char rwhy[128];
        const char *r = orbapp_unpack_settings(sw,
                                               (size_t)h->n_settings * ORBAPP_SETTINGS_WORDS,
                                               h->n_settings, settings, rwhy, sizeof rwhy);
        if (r) { ret = install_failed(ESP_ERR_INVALID_ARG, r, why, whysz); goto out; }
        n_settings = (int)h->n_settings;
    }

    /* The slot state, read under the same lock, then the pure decision — cap,
     * quota, slot, serial. A refusal costs a sentence and leaves every slot
     * untouched: nothing here has erased a byte yet. */
    orbapp_install_slots_t sl;
    read_slots(p, h->slug, &sl);
    sl.settings   = (n_settings > 0) ? settings : NULL;
    sl.n_settings = n_settings;

    orbapp_install_verdict_t v;
    if (!install_decide(h, &sl, &v)) {
        ret = install_failed(ESP_FAIL, v.why[0] ? v.why : "the app was refused",
                             why, whysz);
        goto out;
    }

    /* P4a fresh-install wipe: when the chosen slot is not the slug's own, any
     * grant revoke, fault count or enable word it carries belongs to a former
     * occupant — an inherited revoke denies an innocent app, an inherited
     * fault count fails one that never crashed, an inherited enable makes the
     * confirm's promise lie. An upgrade (same slug, same slot) keeps them all:
     * re-flashing an app does not un-ask its owner's decisions (D19). Nothing
     * has erased yet, so a refusal after this point still costs no slot — it
     * cost a dirty slot its stale words, which it was owed. */
    if (v.slot != sl.existing_slot) {
        if (orb_svc_forget_row(ORB_APP_BASE + v.slot) != ESP_OK)
            ESP_LOGW(TAG, "slot %d's stored owner words could not be cleared — "
                          "check this app's PERMISSIONS ladder after the install", v.slot);
    }

    /* Erase + copy + read-back CRC; the header goes live only once the geometry
     * came back intact (orb_theme.c:1494's ordering — a slot never goes live on
     * bytes nobody read back). The card lock comes off with install_close. */
    const char *fw = NULL;
    esp_err_t e = copy_prefix(p, src, h, v.slot, &fw);
    if (e == ESP_OK) e = verify_slot_geom(p, h, v.slot, &fw);
    if (e == ESP_OK) e = write_slot_header(p, v.slot, raw, &fw);
    if (e != ESP_OK) { ret = install_failed(e, fw, why, whysz); goto out; }


    /* Commit to the registry LAST, from the manifest — D19: caps come from the
     * header, never a stored grant word; D21 (§0.9): the container carries no
     * art, so the row lands art-free (art_bytes 0, art_slot -1) and the boot
     * ladder re-derives the same record from the slot header next power.
     * card_upsert copies slug/title/settings into the
     * table's own buffers, so this stack row, h_store and the scratch may all die
     * on return. */
    orb_app_t row;
    memset(&row, 0, sizeof row);
    row.slug       = h->slug;
    row.title      = h->title;
    row.kind       = ORB_APP_KIND_APP;
    row.behaviour_kind = h->behaviour_kind;   /* D1/C6: selects the compiled module */
    row.flash_slot     = v.slot;              /* P4a: the row IS its slot — key stability */
    row.caps       = h->caps;
    row.period_ms  = h->period_ms;
    row.settings   = (n_settings > 0) ? settings : NULL;
    row.n_settings = n_settings;
    row.art_offset = 0;
    row.art_bytes  = 0;               /* D21: the container carries no art */
    row.art_slot   = -1;
    /* Owner ruling, bench 2026-10-29: installed IS enabled. The row lands switched on
     * here rather than waiting for apply_enable, which early-returns as soon as the card
     * holds no readable file (a slot whose card copy was removed has nobody to ask), and
     * only an explicit stored 0 from the recovery door walks it back off. */
    row.enabled    = true;
    /* D37: the same six words the duplicate policy above compares, off the same header
     * it was parsed from — so a row's identity is what its flash says, and an install
     * cannot be made to look current by a caller that believed it was. */
    row.geom_crc   = h->geom_crc;
    row.code_crc   = h->code_crc;
    row.code_size  = h->code_size;
    row.serial     = h->serial;
    row.editor_crc = h->editor_crc;
    /* S6 (§4.1): the release stamp rides in as the tuple's seventh word, read from the
     * same header. It is the only member with an order, and the only one an operator
     * reads as words: the hub prints it on the Apps face. */
    row.version    = h->version;
    if (orb_app_card_upsert(&row) < 0) {
        ret = install_failed(ESP_ERR_NO_MEM,
                             "it reached flash but the app registry is full — uninstall one",
                             why, whysz);
        goto out;
    }

    ESP_LOGI(TAG, "installed \"%s\" (%s) into app slot %d in %lld ms",
             h->title, stem ? stem : h->slug, v.slot, (long long)((esp_timer_get_time() - t0) / 1000));


out:
    heap_caps_free(scratch);
    return ret;
}

esp_err_t orb_app_install(const char *stem, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    const esp_partition_t *p = apps_partition();
    if (!p)
        return install_failed(ESP_ERR_NOT_FOUND, "this board has no app storage",
                              why, whysz);
    if (!stem || !stem[0])
        return install_failed(ESP_ERR_INVALID_ARG, "no app was named", why, whysz);

    /* Resolve-by-walk, then open and RE-verify the header inside the lock: this
     * returns the fd with the card lock held and the header already through the
     * reader's full ladder (identity triple, layout). The copy that follows reads
     * those same bytes, so a file edited on a laptop between listing and install
     * cannot be the thing that lands. */
    orbapp_hdr_t h_store;
    uint32_t raw[ORBAPP_HDR_WORDS];
    const orbapp_hdr_t *h = NULL;
    const char *cw = NULL;
    const int fd = open_verified_stem(&h, &h_store, raw, stem, &cw);
    if (fd < 0) return install_failed(ESP_FAIL, cw, why, whysz);

    const appsrc_t src = { .fd = fd, .locked = true };
    const esp_err_t ret = install_ladder(p, &src, h, raw, stem, why, whysz);
    install_close(fd);
    return ret;
}

/* The memory-source front (kept generic since D36 retired its only shipped
 * caller): install a container the caller already holds through the SAME header
 * ladder, decision, and copy-verify-header-last pipeline a card file pays - one
 * install path, two sources. The source needs no card lock, no mount, and no
 * stem (the header's slug is its own filename). A torn install reads as an
 * ABSENT app exactly as a torn card copy would (D9). */
esp_err_t orb_app_install_image(const uint8_t *img, size_t len, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    const esp_partition_t *p = apps_partition();
    if (!p)
        return install_failed(ESP_ERR_NOT_FOUND, "this board has no app storage",
                              why, whysz);

    const appsrc_t src = { .fd = -1, .mem = img, .mem_len = len, .locked = false };
    orbapp_hdr_t h;
    uint32_t raw[ORBAPP_HDR_WORDS];
    const char *bad = read_and_check_header(&src, &h, NULL, raw);
    if (bad) return install_failed(ESP_FAIL, bad, why, whysz);
    return install_ladder(p, &src, &h, raw, NULL, why, whysz);
}


/* The worker's request: the stem, nothing else — small enough the queue owns it
 * outright and the httpd caller's stack may vanish on return (orb_theme.c's
 * install_req_t, same shape). */
typedef struct { char stem[ORB_SD_NAME_MAX + 1]; } install_req_t;

static void install_job(void *arg)
{
    const install_req_t *r = arg;
    char why[128] = "";
    const esp_err_t e = orb_app_install(r->stem, why, sizeof why);
    try_note(r->stem, (e == ESP_OK) ? NULL : (why[0] ? why : "the install failed"));
}

/* Queue an install onto the worker. The name is validated here, on the caller's
 * task, so a bad stem is refused before it reaches the queue and the /apps page
 * can answer it directly; the worker re-validates against the card anyway.
 * ESP_ERR_INVALID_STATE when one install is already outstanding — orb_jobs keeps
 * the single-job gate — so a double-tap on Load cannot queue two copies. */
esp_err_t orb_app_install_post(const char *stem)
{
    if (!stem || !orb_sd_name_ok(stem)) return ESP_ERR_INVALID_ARG;
    install_req_t r;
    memset(&r, 0, sizeof r);
    snprintf(r.stem, sizeof r.stem, "%s", stem);
    return orb_jobs_post(install_job, &r, sizeof r);
}

/* Rebuild the card table from the apps partition at boot — the read-half of D9.
 * Each slot's header sector (written last at install) is the sole thing that makes
 * it an app: a slot that never got its header (a copy torn by a power cut) parses
 * as nothing and is skipped, so a half-installed app is an ABSENT app, never a
 * corrupt one. A slot whose header parses but whose geometry no longer CRCs is the
 * flash itself having gone bad under a completed install; it is skipped with its
 * number logged rather than handed to the dial as an app that would fault when
 * drawn. Runs on the boot task before the display is up, so no LVGL and no card
 * lock — only esp_partition_read. Granting is not here: card_upsert fills caps
 * from the header (D19) and orb_svc_init, which reads the table after this, masks
 * the owner's stored word against those caps. */
int orb_app_install_resolve_boot(void)
{
    const esp_partition_t *p = apps_partition();
    if (!p) {
        ESP_LOGW(TAG, "no \"" APPS_PARTITION_NAME "\" partition — no installed apps this boot");
        return 0;
    }

    orb_app_card_reset();
    /* The two table buffers this loop needs — the settings wire words and the
     * unpacked descriptors — are ~1 KB each and must NOT sit on the main task's
     * stack: app_main() calls resolve_boot() directly, and CONFIG_ESP_MAIN_TASK_STACK_SIZE
     * is only 3584 B. With sw[]+settings[] on top of hdr[], why[] and the call
     * frames, the resolver overflowed that stack and the guard page (main.c's
     * 0xA5A5A5A5 fill) then tripped the next internal allocant — gpio_install_isr_service
     * at knob-init — into a reboot loop (2026-10-12, orb-A: "***ERROR*** A stack
     * overflow in task main has been detected", caught only with comprehensive heap
     * poisoning). One PSRAM block carries both; hdr[] (~0.5 KB) stays on the stack.
     * No ensure_chunk() here either: the slot CRC reaches verify_slot_geom(), which
     * reserves its own 4 KB internal buffer guarded, mirroring orb_theme_boot. */
    uint8_t *scratch = heap_caps_malloc(ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS * 4u +
                                        ORBAPP_SETTINGS_MAX * sizeof(orb_setting_desc_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *sw = (uint32_t *)scratch;
    orb_setting_desc_t *settings = scratch
        ? (orb_setting_desc_t *)(scratch + ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS * 4u)
        : NULL;

    int resolved = 0;
    for (int slot = 0; slot < APPS_N_SLOTS; slot++) {
        const uint32_t base = slot_base(slot);
        uint32_t hdr[ORBAPP_HDR_WORDS];
        if (esp_partition_read(p, base, hdr, sizeof hdr) != ESP_OK) continue;
        orbapp_hdr_t h;
        char why[128];
        if (orbapp_parse_header(hdr, &h, why, sizeof why)) continue;   /* headerless slot */

        /* Header intact but is the geometry it points at still there? Re-CRC it
         * from the blob region; a mismatch means the install completed and the
         * flash has since rotted — worse than an absent app, so refuse it too. */
        const char *vw = NULL;
        if (verify_slot_geom(p, &h, slot, &vw) != ESP_OK) {
            ESP_LOGW(TAG, "app slot %d (\"%s\") failed read-back: %s — skipped",
                     slot, h.slug, vw ? vw : "geometry unreadable");
            continue;
        }

        /* Unpack the declared settings from the blob — the same table the install
         * stored there, and the only ids the app may ever write to NVS. Read the
         * wire words straight from the partition at their file-relative offset
         * (the blob starts at the file's byte 0), then hand them to the reader.
         * sw/settings are the PSRAM scratch reserved above, never this frame. */
        int n_settings = 0;
        if (h.n_settings > 0 && h.n_settings <= ORBAPP_SETTINGS_MAX) {
            if (!scratch) {                              /* PSRAM gone: no settings read */
                ESP_LOGW(TAG, "app slot %d (\"%s\"): no PSRAM for the settings scratch — skipped",
                         slot, h.slug);
                continue;
            }
            const size_t need = (size_t)h.n_settings * ORBAPP_SETTINGS_WORDS * 4u;
            if (esp_partition_read(p, base + APPS_SLOT_HDR_BYTES + h.setting_off,
                                   sw, need) != ESP_OK) {
                ESP_LOGW(TAG, "app slot %d (\"%s\"): could not read its settings — skipped",
                         slot, h.slug);
                continue;
            }
            char rwhy[128];
            if (orbapp_unpack_settings(sw, (size_t)h.n_settings * ORBAPP_SETTINGS_WORDS,
                                       h.n_settings, settings, rwhy, sizeof rwhy)) {
                ESP_LOGW(TAG, "app slot %d (\"%s\"): %s — skipped", slot, h.slug, rwhy);
                continue;
            }
            n_settings = (int)h.n_settings;
        }

        orb_app_t row;
        memset(&row, 0, sizeof row);
        row.slug       = h.slug;
        row.title      = h.title;
        row.kind       = ORB_APP_KIND_APP;
        row.behaviour_kind = h.behaviour_kind;  /* D1/C6: selects the compiled module */
        row.flash_slot     = slot;              /* P4a: the row IS its slot — key stability */
        row.caps       = h.caps;                 /* D19: from the slot header only */
        row.period_ms  = h.period_ms;
        row.settings   = (n_settings > 0) ? settings : NULL;
        row.n_settings = n_settings;
        row.art_offset = 0;
        row.art_bytes  = 0;        /* D21: the container carries no art */
        row.art_slot   = -1;
        /* Owner ruling, bench 2026-10-29: installed IS enabled, and a board that boots
         * straight off its card is the case that has to honour it. apply_enable runs one
         * rung later and is the only thing that could have carried this bit, but it
         * early-returns when the card reports nothing — and a card-restored row on a
         * board whose enable words were just wiped lands there with no word to read.
         * Before this line the row's own default came from memset(0), so a fresh flash
         * restored the clock INSTALLED AND OFF and the dial showed no face. */
        row.enabled    = true;
        /* D37: read back off the slot header this rung already checksummed, so the
         * identity the listing reports is the one the flash carries even after a
         * half-completed install or a card swapped between boots — the case where a
         * hub's own idea of what it shipped is worth least. */
        row.geom_crc   = h.geom_crc;
        row.code_crc   = h.code_crc;
        row.code_size  = h.code_size;
        row.serial     = h.serial;
        row.editor_crc = h.editor_crc;
        /* S6 (§4.1): the stamp the card itself carries, read off the header this rung
         * just checksummed — so a swapped card reports its own versions and not the
         * ones the hub remembers shipping. */
        row.version    = h.version;
        if (orb_app_card_upsert(&row) < 0) {
            ESP_LOGW(TAG, "app slot %d: registry refused it", slot);
            continue;
        }
        resolved++;
    }

    /* Release the transient buffers this resolver reserved, so it leaves no
     * internal or PSRAM block behind for the display or the knob. A later install
     * reserves the read buffer again through ensure_chunk(). */
    if (s_chunk) { heap_caps_free(s_chunk); s_chunk = NULL; }
    if (scratch) heap_caps_free(scratch);

    ESP_LOGI(TAG, "resolved %d installed app%s from the \"" APPS_PARTITION_NAME "\" partition",
             resolved, resolved == 1 ? "" : "s");
    return resolved;
}

/* ============================================================================
 *  P4a — the owner's two doors: enable and uninstall (plan §8 P4, B4).
 * =========================================================================== */

/* The boot ladder's second step (after orb_svc_init, before orb_app_init_all):
 * an installed app walks only what its stored word says it may. resolve_boot
 * could not read these words — it runs before the NVS partition is initialised
 * and its contract is partition-reads-only — so the enable pass is its own rung.
 * Under the installed-IS-enabled ruling the rung carries BOTH arms: an absent
 * word leaves card_fill's enabled standing, and only an explicit stored 0 walks
 * the row off — the recovery door's durable off-record, nothing else. */
void orb_app_install_apply_enable(void)
{
    if (!orb_app_card_count()) return;                /* nothing installed: nothing to ask */
    for (size_t i = ORB_APP_BASE; i < orb_app_rows(); i++) {
        const orb_app_t *row = orb_app_row(i);
        if (!row || !row->installed) continue;        /* positional span: holes are free slots */
        const bool on = orb_svc_enabled(ORB_APP_BASE + row->flash_slot);
        if (row->enabled == on) continue;             /* the default already agrees with the word */
        if (orb_app_set_enabled(row, on) == ESP_OK)
            ESP_LOGI(TAG, "%s \"%s\" from its stored word",
                     on ? "enabled" : "held off", row->slug);
    }
}

/* ---- P7.4: the code tier's load rung (plan §8 P7, D20/D27) -----------------
 *
 * The third boot rung, after the enable words land and before init_all builds
 * screens: every enabled WASM-kind row gets its module read back from its slot
 * and handed to the engine. It runs HERE, not inside the wasm module, because
 * only this file knows the slot layout (D9's geometry) — the shim takes bytes,
 * never partition offsets, exactly as it takes settings the caller unpacked.
 *
 * Read-back CRC before the engine sees a byte, mirroring verify_slot_geom: a
 * slot whose module rotted since install is refused at this rung, and init_all
 * then drops the row the same way it drops any app whose init refuses — the
 * dial boots fine without it, which is the whole D27 promise applied to flash
 * rather than to the card. A disabled row is never read at all: an app the
 * owner turned off must not cost 128 KB of PSRAM or a runtime. */
static esp_err_t wasm_row_load(const esp_partition_t *p, orb_app_t *row,
                               char *why, size_t whysz)
{
    const int slot = row->flash_slot;
    if (orb_wasm_loaded(slot)) return ESP_OK;          /* already resident */
    uint32_t hdr[ORBAPP_HDR_WORDS];
    if (esp_partition_read(p, slot_base(slot), hdr, sizeof hdr) != ESP_OK) {
        snprintf(why, whysz, "the slot header could not be read");
        return ESP_FAIL;
    }
    orbapp_hdr_t h;
    char rwhy[128];
    if (orbapp_parse_header(hdr, &h, rwhy, sizeof rwhy)) {
        snprintf(why, whysz, "its header no longer parses: %.64s", rwhy);
        return ESP_ERR_INVALID_CRC;
    }
    /* Defensive bound on a header whose CRC passes: the region must still fit
     * the slot blob, or no read below is trustworthy. */
    if (h.code_size == 0 || h.code_size > APPS_SLOT_GEOM_CAP ||
        h.code_off > APPS_SLOT_GEOM_CAP - h.code_size) {
        snprintf(why, whysz, "its code region is larger than the slot");
        return ESP_ERR_INVALID_SIZE;
    }
    /* The image lands in PSRAM whole: m3 parses lazily and keeps the pointer
     * only until orb_wasm_load makes its OWN copy (orb_app_wasm.c), so this
     * buffer is freed by us right after — unlike the shim's, which is the
     * runtime's for the module's life. */
    uint8_t *buf = heap_caps_malloc(h.code_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { snprintf(why, whysz, "no PSRAM for the module"); return ESP_ERR_NO_MEM; }
    const uint32_t base = slot_base(slot) + APPS_SLOT_HDR_BYTES + h.code_off;
    esp_err_t e = ESP_OK;
    for (uint32_t off = 0; off < h.code_size; ) {
        const uint32_t want = (h.code_size - off > 4096u) ? 4096u : h.code_size - off;
        if (esp_partition_read(p, base + off, buf + off, want) != ESP_OK) {
            e = ESP_FAIL;
            break;
        }
        off += want;
    }
    if (e == ESP_OK && esp_rom_crc32_le(0, buf, h.code_size) != h.code_crc) {
        snprintf(why, whysz, "the module did not read back intact");
        e = ESP_ERR_INVALID_CRC;
    }
    if (e == ESP_OK) {
        e = orb_wasm_load(slot, &h, buf, h.code_size);
        if (e == ESP_OK) orb_wasm_bind_app(slot, row);
        else snprintf(why, whysz, "the engine refused the module");
    }
    heap_caps_free(buf);
    return e;
}

void orb_app_install_apply_code(void)
{
    const esp_partition_t *p = apps_partition();
    if (!p) return;                                    /* no partition: no code apps */
    for (size_t i = ORB_APP_BASE; i < orb_app_rows(); i++) {
        const orb_app_t *row = orb_app_row(i);
        if (!row || !row->installed || !row->enabled || row->failed) continue;
        if (row->behaviour_kind != ORB_BEHAVIOUR_WASM) continue;
        /* C2 rows stay cold: init_all runs next and quarantines them, and a
         * quarantined app must not cost its memory while it is quarantined. */
        if (orb_svc_faulted(row)) continue;
        char why[128] = "";
        if (wasm_row_load(p, (orb_app_t *)row, why, sizeof why) != ESP_OK)
            ESP_LOGW(TAG, "code app \"%s\" refused at load: %s", row->slug, why);
            /* No enable rollback: init_all calls the module's init, gets FAIL
             * for the row that never loaded, and drops it from the carousel —
             * the same refusal path every other app's broken init already walks,
             * so one broken module cannot cost the dial anything but its slot. */
    }
}

/* One refusal sentence for both doors. */
static esp_err_t door_refuse(esp_err_t e, const char *w, char *why, size_t whysz)
{
    if (why && whysz) snprintf(why, whysz, "%s", w);
    ESP_LOGW(TAG, "owner door: %s", w);
    return e;
}

/* Shared front of both doors: a usable name, no card job in the way, and the
 * slug's resident row. Returns NULL and fills why otherwise. Compiled/shell
 * rows answer NOT_SUPPORTED with the same sentence on both doors. */
static const orb_app_t *door_open(const char *slug, char *why, size_t whysz,
                                  esp_err_t *err)
{
    if (!slug || !orb_sd_name_ok(slug)) {
        *err = door_refuse(ESP_ERR_INVALID_ARG, "not a usable app name", why, whysz);
        return NULL;
    }
    /* The gate refuses a door asked for while the card is committed elsewhere.
     * A door already running on the worker — the glass's own enable/uninstall
     * press, posted as this job — meets only its own busy flag, so the gate
     * must not refuse it: that deadlock refused every switch press on the
     * dial. The upload window is a separate, non-worker thing and still refuses. */
    if ((orb_jobs_busy() && !orb_jobs_on_worker()) || orb_app_upload_busy()) {
        *err = door_refuse(ESP_ERR_INVALID_STATE,
                           "a card job is running — try again when it finishes",
                           why, whysz);
        return NULL;
    }
    const orb_app_t *row = orb_app_find_slug(slug);
    if (!row || !row->installed) {
        *err = door_refuse(ESP_ERR_NOT_FOUND,
                           "no app by that name is installed on this dial", why, whysz);
        return NULL;
    }
    if (!orb_app_is_card(row)) {
        *err = door_refuse(ESP_ERR_NOT_SUPPORTED,
                           "the shell menu is part of the dial — it cannot be turned off",
                           why, whysz);
        return NULL;
    }
    return row;
}


esp_err_t orb_app_enable(const char *slug, bool on, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    esp_err_t err = ESP_OK;
    const orb_app_t *row = door_open(slug, why, whysz, &err);
    if (!row) return err;

    if (!on && (orb_app_current() == row || row->active))
        return door_refuse(ESP_ERR_INVALID_STATE,
                           "that app is on the dial — carousel away from it first",
                           why, whysz);

    if (on) {
        if (!row->iface)
            return door_refuse(ESP_ERR_INVALID_STATE,
                               "no firmware module implements this app's behaviour — "
                               "it cannot be shown", why, whysz);
        if (row->failed) {
            /* The owner pressing ON IS the revive gesture (C2): clear the
             * record first, and say so if NVS refuses — a failed app turned
             * on with its counts still standing would fail the boot it cannot
             * win. This is clear_faults' first device caller. */
            if (orb_svc_clear_faults((orb_app_t *)row) != ESP_OK)
                return door_refuse(ESP_FAIL, "the fault record would not clear — "
                                             "the app stays failed", why, whysz);
            ((orb_app_t *)row)->failed = false;
            ((orb_app_t *)row)->fault_count = 0;
        }
        /* P7.4: a code app turns on only if its module reads back and the engine
         * takes it NOW — the door promises a working app, not one that waits for
         * a reboot to find out it rotted. The guard is !enabled, not !loaded, on
         * purpose: a row already on means its own module is already resident (or
         * this boot refused it at apply_code and failed the row), while a row off
         * may face engine state left by a PREVIOUS occupant of the slot — an app
         * uninstalled since the last reboot. Released before the load, and safe
         * to release from here: an off row is not walkable, so no LVGL-task hook
         * can be inside the slot it names. Loading itself is also race-free off
         * the LVGL task for the same reason — the row joins the walkable set only
         * when the word below lands. */
        if (row->behaviour_kind == ORB_BEHAVIOUR_WASM && !row->enabled) {
            orb_wasm_unload(row->flash_slot);
            const esp_partition_t *cp = apps_partition();
            char lw[128] = "";
            if (!cp || wasm_row_load(cp, (orb_app_t *)row, lw, sizeof lw) != ESP_OK)
                return door_refuse(ESP_FAIL, lw[0] ? lw
                                 : "no apps partition to read the module from",
                                   why, whysz);
        }
    }

    /* Word first, RAM second: a crash between the two leaves the intent on
     * disk and next boot applies it (apply_enable). The reverse order loses
     * the owner's decision with nothing to show for it. */
    const int slot = orb_app_index_of(row);
    const esp_err_t e = orb_svc_set_enabled(slot, on);
    if (e != ESP_OK)
        return door_refuse(e, "the owner ledger refused it — nothing changed",
                           why, whysz);
    orb_app_set_enabled(row, on);
    ESP_LOGI(TAG, "%s \"%s\"", on ? "enabled" : "disabled", row->slug);
    return ESP_OK;
}

/* Third owner door: clear one app's fault record — compiled tier included.
 *
 * This exists because the ON gesture above cannot reach a built-in app.
 * orb_app_enable revives a failed app through clear_faults only on the CARD
 * path: door_open refuses a compiled row at the is_card gate before the revive
 * branch runs, and a two-strike C2 quarantine also leaves that row installed=
 * false (init_all skips it), so the enable door's NOT_FOUND gate would refuse it
 * too. A dial whose menu panicked at boot — the failure this hardening closes —
 * therefore had one way back: RESET ALL, which also wipes the owner's WiFi.
 *
 * This door clears only the fault record. It skips both door_open gates on
 * purpose: the app is a built-in (not a card), and a quarantine is exactly the
 * state where installed reads false, so requiring an install would defeat the
 * door's whole point. It validates the name and the no-card-job front the same
 * way, then clears the broker's word and mirrors it into RAM. The screen is NOT
 * rebuilt here: re-running a built-in app's iface->init off the httpd task is
 * the widget race every rule in input_router.c exists to avoid, so the app
 * returns on the next reboot, when init_all finds its fault word gone. */
esp_err_t orb_app_clear_faults(const char *slug, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    if (!slug || !orb_sd_name_ok(slug))
        return door_refuse(ESP_ERR_INVALID_ARG, "not a usable app name", why, whysz);
    if (orb_jobs_busy() || orb_app_upload_busy())
        return door_refuse(ESP_ERR_INVALID_STATE,
                           "a card job is running — try again when it finishes",
                           why, whysz);
    const orb_app_t *row = orb_app_find_slug(slug);
    if (!row)
        return door_refuse(ESP_ERR_NOT_FOUND,
                           "no app by that name is known to this dial", why, whysz);
    if (orb_app_index_of(row) < 0)
        return door_refuse(ESP_ERR_NOT_FOUND,
                           "that app has no fault record to clear", why, whysz);

    orb_app_t *a = (orb_app_t *)row;
    const esp_err_t e = orb_svc_clear_faults(a);
    if (e != ESP_OK)
        return door_refuse(e, "the fault record would not clear — the app stays failed",
                           why, whysz);
    a->failed = false;
    a->fault_count = 0;
    ESP_LOGI(TAG, "fault record cleared for \"%s\"%s", a->slug,
             orb_app_is_card(a) ? "" : " — a reboot rebuilds it into the carousel");
    return ESP_OK;
}

esp_err_t orb_app_uninstall(const char *slug, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    esp_err_t err = ESP_OK;
    const orb_app_t *row = door_open(slug, why, whysz, &err);
    if (!row) return err;
    if (orb_app_current() == row || row->active) {
        /* D38: an app on the glass is uninstallable — the door homes the dial
         * itself instead of teaching the operator to carousel away. The rule
         * the refusal protected stands: the registry may not lose the row the
         * knob is holding (orb_app.c:563). What changed is who moves the knob.
         * input_router_request is task-safe — it records a pointer the LVGL
         * poll applies within one 10 ms tick, and leaving the glass clears the
         * row's active bit (orb_app_activate's old->active=false). So this
         * task asks for home and waits for the switch to land, bounded: a dial
         * whose poll cannot run is a dial that will not take the erase either,
         * and the honest answer stays a refusal. */
        input_router_request(orb_app_home());
        int waited_ms = 0;
        while ((orb_app_current() == row || row->active) &&
               waited_ms < UNHOME_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(20));
            waited_ms += 20;
        }
        if (orb_app_current() == row || row->active)
            return door_refuse(ESP_ERR_INVALID_STATE,
                               "the dial would not leave the app — try again",
                               why, whysz);
    }

    const int slot = orb_app_index_of(row);
    const esp_partition_t *p = apps_partition();

    /* Which flash slot carries it? The headers are the truth (read_slots is
     * the installer's own census); a row whose slot cannot be found is the
     * registry disagreeing with flash — finish the removal anyway: the ledger
     * row and the owner words are real state to clear, and the next boot's
     * resolve would drop the row regardless. */
    int fslot = -1;
    if (p) {
        orbapp_install_slots_t sl;
        read_slots(p, slug, &sl);
        fslot = sl.existing_slot;
    }
    if (fslot >= 0) {
        /* Header sector only: from this byte the D9 rule makes the app no app
         * at all — no reboot needed, nothing aliases a headerless slot, and
         * the blob bytes below cost nobody until the slot's next copy erases
         * them (an uninstalled slot reads to every reader exactly as a slot a
         * torn install never completed: absent). */
        const esp_err_t e = esp_partition_erase_range(p, slot_base(fslot),
                                                      APPS_SLOT_HDR_BYTES);
        if (e != ESP_OK)
            return door_refuse(e, "the flash would not erase — the app is still installed",
                               why, whysz);
    } else {
        ESP_LOGW(TAG, "uninstall \"%s\": no flash slot found for it — clearing the "
                      "registry and the owner words only", slug);
    }

    /* Erase first, then the words, then the row: every power cut between the
     * three leaves the app already dead (D9) and at worst stale words on a
     * dirty slot, which the next fresh install wipes. */
    /* A code app's engine state is deliberately NOT released here: this door
     * runs on the httpd task while the LVGL task may be inside the module's
     * service hook, and a slot_release under it is the race every rule here
     * exists to avoid. The row is gone from the registry, so nothing reaches
     * the slot again; the state goes at the next reboot, or at the enable door
     * of whatever app takes this slot next, which releases before loading. */
    if (orb_svc_forget_row(slot) != ESP_OK)
        ESP_LOGW(TAG, "uninstall \"%s\": some owner words could not be erased — "
                      "reinstalling will wipe them again", slug);
    if (!orb_app_card_remove(slug))
        return door_refuse(ESP_ERR_INVALID_STATE,
                           "it came off the flash but the registry still holds it",
                           why, whysz);
    ESP_LOGI(TAG, "uninstalled \"%s\" from app slot %d", slug, fslot);
    return ESP_OK;
}

/* ============================================================================ *
 *  The browser→card upload. Mirror of orb_theme_upload_* (orb_theme.c:1835),
 *  for an .orbapp into the OS-owned /sd/apps drop zone. Bytes land in
 *  upload.part under the one card lock; the container is verified IN FULL —
 *  reader ladder plus both payload CRCs — off the card, and only then rename()d
 *  to the <slug>.orbapp its OWN header names. Half-arrived or damaged bytes are
 *  deleted, never published, so a torn upload can never be listed as an app the
 *  install then selects. The scratch carries no ORB_APP_EXT, so orb_files_walk
 *  (and therefore the install's resolve-by-stem) cannot select it, exactly as the
 *  theme scratch is invisible to orb_theme_scan.                                        *
 * =========================================================================== */

/* The upload in progress. orb_app_upload_busy() is the gate every scan checks
 * first, so at most one exists — which is what lets the scratch keep one fixed
 * name and the card lock stay held across the whole transfer. */
struct orb_app_up {
    int      fd;
    char     tmp[ORB_SD_PATH_MAX];
    uint32_t got;
};
static orb_app_up_t *s_up;

/* Not .orbapp, so the walk never lists it and the install never resolves it. One
 * fixed name (in /sd/apps, where the theme's upload.part lives in /sd/themes), so
 * the two uploads cannot collide and a single scratch cannot race a second. */
#define APPS_SCRATCH "upload.part"

/* Validate an open container's header block the way the reader does — parse, then
 * the identity triple against the FILENAME the file will be filed under (derived
 * from the header's own slug, since an upload has no name on disk yet), then the
 * placement rule. Returns the refusal sentence, or NULL. A caller-supplied stem
 * would be the only difference from read_and_check_header, and there is none here:
 * the upload's destination IS whatever this header says it is. */
static const char *app_hdr_validate(int fd, orbapp_hdr_t *h)
{
    uint32_t hdr[ORBAPP_HDR_WORDS];
    size_t pos = 0;
    if (rd_at(fd, &pos, (uint8_t *)hdr, sizeof hdr) != (ssize_t)sizeof hdr)
        return "the file ends inside its own header block — re-send it";

    /* Static for the same reason read_and_check_header's is: the door prints
     * this sentence after the frame that held it is gone. Runs only on the
     * httpd task, one upload at a time (orb_app_upload_begin's gate). */
    static char why[160];
    const char *r = orbapp_parse_header(hdr, h, why, sizeof why);
    if (r) return r;

    char fname[ORBAPP_SLUG_MAX + 16];
    snprintf(fname, sizeof fname, "%s%s", h->slug, ORB_APP_EXT);
    r = orbapp_check_header(h, 0, false, fname, why, sizeof why);
    if (r) return r;
    return orbapp_check_file_layout(h, why, sizeof why);
}

bool orb_app_upload_busy(void) { return s_up != NULL; }

esp_err_t orb_app_upload_begin(orb_app_up_t **out)
{
    *out = NULL;
    if (s_up) return ESP_ERR_INVALID_STATE;                 /* one at a time */
    const orb_file_src_t *src = orb_files_src(ORB_APP_SCAN_SLUG, ORB_APP_SCAN_ID);
    if (!src) return ESP_ERR_NOT_FOUND;
    if (orb_sd_ensure_mounted() != ESP_OK) return ESP_ERR_NOT_FOUND;
    /* /sd/apps may have been deleted from the file manager; the scratch path
     * below cannot be opened without its parent (FR_NO_PATH). */
    orb_sd_ensure_store_dirs();
    /* No card read buffer reserved HERE, and none needed: the upload verifies only
     * the header block (a stack copy), not the payload. The payload CRCs are the
     * INSTALL's job — orb_app_install() runs verify_payload() off the card before
     * the first erase (D9), and orb_app_scan.c:17 states the division outright:
     * only the header is read at the card-facing tiers. Checking the whole payload
     * here would need install.c's internal DMA buffer from the httpd task, where
     * the WiFi/SDMMC scratch already contends for contiguous internal heap — the
     * exact squeeze h_apps_list documents as ESP_ERR_HTTPD_TASK. The install is the
     * right place to pay it: it runs on the worker, off the dial's hot path. */

    orb_app_up_t *u = calloc(1, sizeof *u);
    if (!u) return ESP_ERR_NO_MEM;
    /* The scratch path is built from the OS's own declared apps directory, never
     * from a client string — the destination filename is not even known yet,
     * because it is the slug still inside the bytes still arriving. */
    const int n = snprintf(u->tmp, sizeof u->tmp, "%s/%s", src->dir, APPS_SCRATCH);
    if (n < 0 || (size_t)n >= sizeof u->tmp) { free(u); return ESP_ERR_INVALID_SIZE; }

    orb_sd_lock();
    u->fd = open(u->tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (u->fd < 0) {
        const int e = errno;
        orb_sd_unlock(); free(u);
        ESP_LOGW(TAG, "app upload: create failed (%s)", strerror(e));
        return ESP_FAIL;
    }
    s_up = u;
    *out = u;
    ESP_LOGI(TAG, "uploading an app to the card");
    return ESP_OK;
}

esp_err_t orb_app_upload_data(orb_app_up_t *u, const void *buf, size_t n)
{
    if (!u || u != s_up) return ESP_ERR_INVALID_STATE;
    if (u->got + n > ORB_APP_UPLOAD_MAX) {
        /* Refused before the write, not after (orb_theme.c:1915). */
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *p = buf;
    while (n) {
        const ssize_t w = write(u->fd, p, n);
        if (w <= 0) {
            const int e = errno;
            ESP_LOGW(TAG, "app upload: card write failed (%s)", strerror(e));
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

esp_err_t orb_app_upload_end(orb_app_up_t **u, bool overwrite, const char **why)
{
    orb_app_up_t *const up = *u;
    *why = NULL;
    if (!up || up != s_up) { *why = "no upload is in progress"; return ESP_ERR_INVALID_STATE; }

    close(up->fd);
    s_up = NULL;                     /* the card is still locked; released below */

    /* Verify the file that is NOW on the card — not the bytes that passed through
     * this handler. A card that dropped a write, or a client that lied about the
     * length, is caught here rather than by the install three boots later. */
    const int fd = open(up->tmp, O_RDONLY);
    if (fd < 0) {
        *why = "the uploaded file is not on the card";
        unlink(up->tmp); orb_sd_unlock(); free(up);
        return ESP_FAIL;
    }
    orbapp_hdr_t h;
    /* Header-only verify, matching the card-facing tier's contract (scan.c:17) and
     * the install's re-verify: app_hdr_validate reads only the header block (a stack
     * copy, ~150 B), so the httpd task never reaches for install.c's internal DMA
     * buffer. The whole payload CRCs are run off the card by orb_app_install()
     * before the first erase (D9), so a container whose art/geometry disagree with
     * its header never reaches a live slot even if it is filed and listed here. */
    const char *r = app_hdr_validate(fd, &h);
    close(fd);

    /* The card filename is the header's own slug, filed by orb_files_path (which
     * appends .orbapp and re-runs orb_sd_name_ok). Nothing the client named this
     * upload — the destination is whatever this decides, from bytes that had to
     * arrive intact. app_hdr_validate already refused a header with no usable slug. */
    char final[ORB_SD_PATH_MAX];
    const orb_file_src_t *src = orb_files_src(ORB_APP_SCAN_SLUG, ORB_APP_SCAN_ID);
    if (!r && (!src || !orb_files_path(src, h.slug, final, sizeof final)))
        r = "the app name is not one the card can hold";

    /* Duplicate policy, the theme's rule verbatim: a file already sits at this
     * path. Byte-identical on the identity tuple is the same app re-sent — accept
     * quietly and leave the file that is there. Different bytes are a real
     * collision: refuse without ?overwrite=1, because silently replacing an app
     * the owner installed is the surprise this whole rule exists to prevent. */
    bool replace = false;
    if (!r) {
        const int efd = open(final, O_RDONLY);
        if (efd >= 0) {
            orbapp_hdr_t oh;
            const char *or_ = app_hdr_validate(efd, &oh);
            close(efd);
            if (!or_ && oh.geom_crc == h.geom_crc && oh.code_crc == h.code_crc &&
                oh.code_size == h.code_size && oh.editor_crc == h.editor_crc &&
                oh.serial == h.serial && strcmp(oh.version, h.version) == 0 &&
                strcmp(oh.slug, h.slug) == 0) {
                /* geom_crc + the code pair + the editor triple's checksum + serial +
                 * version + slug is the whole identity tuple. D20 made the code region
                 * the app's payload, so two files whose modules differ are two apps
                 * whatever else they share. D37 added the editor region for the
                 * mirror-image reason: a re-build changes it and the dial never
                 * installs it, so a tuple without it called an edited editor an
                 * unchanged upload, kept the old bytes, and named no escape. S6's stamp
                 * joins for the same shape of bug. A release that bumped the version
                 * and nothing else is a mislabelled build, so this word never carries a
                 * decision alone — but a tuple without it would answer such a rebuild
                 * "unchanged" while the hub's library and the card's header disagreed
                 * about which release sat in the slot. The art words on both sides are
                 * zero because
                 * app_hdr_validate just refused any file that carried art (D21). */
                unlink(up->tmp); orb_sd_unlock(); free(up);
                ESP_LOGI(TAG, "app upload of \"%s\" is unchanged — kept as is", h.slug);
                return ESP_OK;
            }
            if (!overwrite) {
                unlink(up->tmp); orb_sd_unlock(); free(up);
                *why = "a different app with that name is already on the card";
                ESP_LOGW(TAG, "app upload \"%s\" collides with a different file of that name", h.slug);
                return ORB_APP_UPLOAD_E_DUP;
            }
            replace = true;    /* overwrite=1 and it verified: replace the old bytes */
        }
    }

    if (r) {
        /* Deleted, not left: a rejected upload that survives on the card shows up
         * in the list as an unloadable entry (orb_theme.c:1996). */
        unlink(up->tmp);
        orb_sd_unlock();
        static char whybuf[128];
        strlcpy(whybuf, r, sizeof whybuf);
        free(up);
        *why = whybuf;
        ESP_LOGW(TAG, "app upload rejected: %s", r);
        return ESP_ERR_INVALID_ARG;
    }

    /* f_rename fails on an existing destination, so an overwrite is an unlink plus
     * a rename, not a replace — the theme's reasoning verbatim (orb_theme.c:2008).
     * Nothing is renamed until this point has verified the whole container, so the
     * window between them never exposes a half-written app. */
    if (replace) unlink(final);
    const int rr = rename(up->tmp, final);
    const int re = errno;
    orb_sd_unlock();
    free(up);
    if (rr != 0) {
        static char whybuf2[64];
        strlcpy(whybuf2, "could not put the uploaded app in place", sizeof whybuf2);
        *why = whybuf2;
        ESP_LOGE(TAG, "app upload rename: %s", strerror(re));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "app \"%s\" uploaded to the card as %s.orbapp%s", h.slug, h.slug,
             replace ? " (overwrote the previous one)" : "");
    return ESP_OK;
}

void orb_app_upload_cancel(orb_app_up_t **u)
{
    orb_app_up_t *const up = *u;
    if (!up) return;
    *u = NULL;
    if (up == s_up) {
        close(up->fd);
        s_up = NULL;
        /* The partial file is always removed: a .part left here is a file with an
         * app's name and none of its bytes, the kind of thing investigated as
         * corruption months later (orb_theme.c:2039). */
        unlink(up->tmp);
        orb_sd_unlock();
    }
    free(up);
}


/* ---- D37: the editor region, read off the card and never installed ---------- *
 * open_verified_stem already does the hard half of this job for the install path:
 * it mounts, validates the name, walks to prove the container passes every header
 * rung, then re-opens it under the one card lock and re-checks. This door reuses
 * it rather than growing a second path to a card file, because a second path is a
 * second set of rules about what a valid container is — the exact shape of bug
 * D26's pre-erase verification exists to prevent.
 *
 * What it does NOT do is install anything. It reads the region's bytes, checks
 * them against the header's own crc32 as they go, and hands them to the caller.
 * No partition, no slot, no registry row: §4.2's card-resident posture, enforced
 * by this function never mentioning esp_partition_t. */
esp_err_t orb_app_editor_read(const char *stem, orb_app_editor_chunk_t chunk, void *ctx,
                              uint32_t *total, const char **why_out)
{
    const char *why = NULL;
    if (total) *total = 0;
    if (why_out) *why_out = NULL;
    if (!stem || !*stem || !chunk) {
        if (why_out) *why_out = "no app named";
        return ESP_ERR_INVALID_ARG;
    }
    /* A store in flight holds the card lock for its whole duration and this door
     * would block the httpd task behind it, so it refuses now instead. Same
     * posture as h_sd_preview's upload_busy gate, and the same `retry:later`. */
    if (orb_app_upload_busy()) {
        if (why_out) *why_out = "an upload holds the card";
        return ESP_ERR_INVALID_STATE;
    }

    orbapp_hdr_t h;
    uint32_t raw[ORBAPP_HDR_WORDS];
    const orbapp_hdr_t *hp = NULL;
    const int fd = open_verified_stem(&hp, &h, raw, stem, &why);
    if (fd < 0) {
        if (why_out) *why_out = why ? why : "no such app on the card";
        return ESP_ERR_NOT_FOUND;
    }
    /* The region's own triple, read off the header that just passed every rung.
     * A container that ships no editor is NOT damage and NOT an error worth a
     * 500: it is the ordinary state of every app written before D37, and the page
     * that asked gets a plain 404 naming it. */
    if (hp->editor_size == 0 || hp->editor_crc == 0) {
        install_close(fd);
        if (why_out) *why_out = "this app ships no editor";
        return ESP_ERR_NOT_FOUND;
    }
    /* The file must actually hold the region. open_verified_stem proved the header
     * and nothing past it, and a container copied to the card by an interrupted
     * transfer is exactly the file that reads a full header and stops early. */
    size_t pos = hp->editor_off;
    uint8_t *buf = heap_caps_malloc(ORB_APP_EDITOR_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        install_close(fd);
        if (why_out) *why_out = "no memory for an editor read";
        return ESP_ERR_NO_MEM;
    }
    if (why_out) *why_out = NULL;
    if (total) *total = hp->editor_size;
    const esp_err_t framed = chunk(NULL, hp->editor_size, ctx);
    if (framed != ESP_OK) {                     /* caller hung up before byte one */
        heap_caps_free(buf);
        install_close(fd);
        return framed;
    }

    uint32_t crc = 0;
    uint32_t done = 0;
    esp_err_t r = ESP_OK;
    while (done < hp->editor_size) {
        const size_t want = ((size_t)hp->editor_size - done) > ORB_APP_EDITOR_CHUNK
                          ? ORB_APP_EDITOR_CHUNK : (size_t)hp->editor_size - done;
        const ssize_t got = rd_at(fd, &pos, buf, want);
        if (got != (ssize_t)want) {
            why = "the file is shorter than its editor claims — re-copy it";
            r = ESP_FAIL;
            break;
        }
        crc = esp_rom_crc32_le(crc, buf, (uint32_t)want);
        if (chunk(buf, want, ctx) != ESP_OK) {  /* peer gone; a torn read is not damage */
            r = ESP_FAIL;
            why = "the reader stopped mid-region";
            break;
        }
        done += (uint32_t)want;
    }
    /* The checksum is checked only on a COMPLETE read. Half a region streamed to
     * a browser that closed its tab has no checksum to match, and reporting a CRC
     * failure there would blame the card for the client's behavior. */
    if (r == ESP_OK && crc != hp->editor_crc) {
        why = "the editor does not match its checksum — re-copy the file onto the card";
        r = ESP_FAIL;
    }
    heap_caps_free(buf);
    install_close(fd);
    if (why_out) *why_out = why;
    return r;
}
