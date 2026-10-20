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

    /* Gate 1 — the manifest+geometry prefix the slot must hold. The blob copied
     * into a slot is the container's bytes [0, geom_end): header, the settings
     * table the reader unpacked, and the geometry the app draws from. Since D21
     * (§0.9) that prefix is the WHOLE payload — the reader refuses any container
     * with art bytes before this gate is ever consulted, so no art term enters
     * the slot arithmetic. geom_off+geom_size is therefore the whole thing
     * that has to fit APPS_SLOT_GEOM_CAP; the reader already capped geom_size at
     * ORBAPP_GEOM_BYTES_MAX, so this fires only if that cap and the slot geometry
     * drift apart. == cap is ACCEPTED: the last word before the header sector. */
    const uint64_t blob = (uint64_t)h->geom_off + h->geom_size;
    if (blob > (uint64_t)APPS_SLOT_GEOM_CAP) {
        snprintf(v->why, sizeof v->why,
                 "its manifest and geometry are %llu B: a slot holds %u B —"
                 " re-bake the app smaller",
                 (unsigned long long)blob, (unsigned)APPS_SLOT_GEOM_CAP);
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
static char s_try_stem[32];
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
static const char *read_and_check_header(int fd, orbapp_hdr_t *h, const char *stem,
                                         uint32_t *raw)
{
    uint32_t hdr[ORBAPP_HDR_WORDS];
    size_t pos = 0;
    if (rd_at(fd, &pos, (uint8_t *)hdr, sizeof hdr) != (ssize_t)sizeof hdr)
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

    char fname[ORBAPP_SLUG_MAX + 16];
    snprintf(fname, sizeof fname, "%s%s", stem, ORB_APP_EXT);
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
static const char *verify_payload(int fd, const orbapp_hdr_t *h)
{
    if (ensure_chunk() != ESP_OK) return "out of memory for the card read buffer";

    uint32_t crc = 0;
    size_t pos = h->geom_off, left = h->geom_size;
    while (left) {
        const size_t want = left > s_chunk_bytes ? s_chunk_bytes : left;
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want)
            return "the file is shorter than its geometry claims — re-copy it";
        crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
        left -= want;
    }
    if (crc != h->geom_crc)
        return "the geometry does not match its checksum — re-copy the file onto the card";
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
    orbapp_hdr_t h;
    const char *bad = read_and_check_header(fd, &h, r->want, NULL);
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
    if (orb_files_walk(src, resolve_entry, &r) < 0) { *why = "no card in the socket"; return -1; }
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
    const char *bad = read_and_check_header(fd, h_storage, stem, raw_out);
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
static esp_err_t copy_prefix(const esp_partition_t *p, int fd, const orbapp_hdr_t *h,
                             int slot, const char **why)
{
    if (ensure_chunk() != ESP_OK) { *why = "out of memory for the card read buffer"; return ESP_ERR_NO_MEM; }

    const uint32_t base = slot_base(slot);
    const uint32_t blob = h->geom_off + h->geom_size;
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
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want) {
            *why = "the app file ended before its declared geometry — re-copy it";
            return ESP_ERR_INVALID_SIZE;
        }
        e = esp_partition_write(p, base + APPS_SLOT_HDR_BYTES + done, s_chunk, want);
        if (e != ESP_OK) { *why = "flash write failed on the geometry"; return e; }
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
esp_err_t orb_app_install(const char *stem, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    const esp_partition_t *p = apps_partition();
    if (!p)
        return install_failed(ESP_ERR_NOT_FOUND, "this board has no app storage",
                              why, whysz);
    if (!stem || !stem[0])
        return install_failed(ESP_ERR_INVALID_ARG, "no app was named", why, whysz);

    const int64_t t0 = esp_timer_get_time();

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

    /* The unpacked settings and the wire words that feed them are ~1 KB each and
     * must NOT sit on this worker's stack: orb_jobs is a hard-measured 4096 B
     * (orb_jobs.c:65 — "needs more stack is a crash rather than a log line"), and
     * settings[] + sw[] + h_store + raw + the verdict's why[128] + the deep
     * read/decide/copy chain overflowed it on orb-A the day this path was first
     * benched (*** stack overflow in task orb_jobs). This is the fix boot_resolve()
     * already documents for the same arrays on the 3584 B main task (its comment
     * above); the install path had never run on hardware to find it. One PSRAM
     * block carries both, freed at the single out: label at the tail. */
    uint8_t *scratch = heap_caps_malloc(ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS * 4u +
                                        ORBAPP_SETTINGS_MAX * sizeof(orb_setting_desc_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch) { install_close(fd);
        return install_failed(ESP_ERR_NO_MEM, "out of memory for the settings table",
                              why, whysz); }
    uint32_t *sw = (uint32_t *)scratch;
    orb_setting_desc_t *settings = (orb_setting_desc_t *)(scratch +
                                        ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS * 4u);
    esp_err_t ret = ESP_OK;
    bool fd_open = true;

    /* The WHOLE payload checksums off the card before any erase. Both CRCs, over
     * the full geometry and the full art range, exactly as orb_theme.c verifies a
     * container — the half-copy D9 prices is caught here, on a page that still
     * lists the card, not after a reboot into a slot that will not draw. */
    const char *vv = verify_payload(fd, h);
    if (vv) { ret = install_failed(ESP_ERR_INVALID_CRC, vv, why, whysz); goto out; }

    /* Unpack the declared settings — needed twice over: the quota prices them and
     * the resident record carries them (only these ids may reach NVS, §4.2). The
     * tables live in the PSRAM scratch above, never this frame. */
    int n_settings = 0;
    if (h->n_settings > 0) {
        size_t spos = h->setting_off;
        const size_t need = (size_t)h->n_settings * ORBAPP_SETTINGS_WORDS * 4u;
        if (rd_at(fd, &spos, (uint8_t *)sw, need) != (ssize_t)need) {
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
        install_close(fd);
        return install_failed(ESP_FAIL, v.why[0] ? v.why : "the app was refused",
                              why, whysz);
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
    esp_err_t e = copy_prefix(p, fd, h, v.slot, &fw);
    if (e == ESP_OK) e = verify_slot_geom(p, h, v.slot, &fw);
    if (e == ESP_OK) e = write_slot_header(p, v.slot, raw, &fw);
    install_close(fd); fd_open = false;
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
    if (orb_app_card_upsert(&row) < 0) {
        ret = install_failed(ESP_ERR_NO_MEM,
                             "it reached flash but the app registry is full — uninstall one",
                             why, whysz);
        goto out;
    }

    ESP_LOGI(TAG, "installed \"%s\" (%s) into app slot %d in %lld ms",
             h->title, stem, v.slot, (long long)((esp_timer_get_time() - t0) / 1000));

out:
    if (fd_open) install_close(fd);
    heap_caps_free(scratch);
    return ret;
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
 * and its contract is partition-reads-only — so the enable pass is its own rung,
 * and the default for "no word" is the same answer card_fill gave: off, exactly
 * what the install confirm promises. */
void orb_app_install_apply_enable(void)
{
    if (!orb_app_card_count()) return;                /* nothing installed: nothing to ask */
    for (size_t i = ORB_APP_BASE; i < orb_app_rows(); i++) {
        const orb_app_t *row = orb_app_row(i);
        if (!row || !row->installed) continue;        /* positional span: holes are free slots */
        if (!orb_svc_enabled(ORB_APP_BASE + row->flash_slot)) continue;
        if (orb_app_set_enabled(row, true) == ESP_OK)
            ESP_LOGI(TAG, "enabled \"%s\" from its stored word", row->slug);
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
    if (orb_jobs_busy() || orb_app_upload_busy()) {
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
                           "that app ships inside the firmware — it is always on",
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

esp_err_t orb_app_uninstall(const char *slug, char *why, size_t whysz)
{
    if (why && whysz) why[0] = '\0';
    esp_err_t err = ESP_OK;
    const orb_app_t *row = door_open(slug, why, whysz, &err);
    if (!row) return err;
    if (orb_app_current() == row || row->active)
        return door_refuse(ESP_ERR_INVALID_STATE,
                           "that app is on the dial — carousel away from it first",
                           why, whysz);

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
            if (!or_ && oh.geom_crc == h.geom_crc &&
                oh.serial == h.serial && strcmp(oh.slug, h.slug) == 0) {
                /* geom_crc + serial + slug is the whole identity tuple: the art
                 * words on both sides are zero because app_hdr_validate just
                 * refused any file that carried art (D21). */
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


