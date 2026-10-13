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
 *   write the slot header last. It mirrors orb_theme_install() (orb_theme.c:1456)
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
     * table the reader unpacked, and the geometry the app draws from — art bytes
     * are NOT copied in P4 (no assets writer yet, so the P4 bench installs
     * art-light at art_size 0). geom_off+geom_size is therefore the whole thing
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


    /* Gate 2 — the art bank. P4 installs are art-light (art_size 0, the bench's
     * case) or fit the small assets slot (art2, 0.375 MB). A bank the size of the
     * flagship art0/art1 slots has no home until P6 reconciles the two install
     * paths into the re-sliced slots, so it is refused BY SIZE with the byte count
     * named — the owner hears "too big for now", not a silent skip. */
    if (h->art_size > APPS_ART_SMALL_MAX) {
        snprintf(v->why, sizeof v->why,
                 "its art bank is %u B and the small slot holds %u B — flagship "
                 "art arrives with the next release",
                 (unsigned)h->art_size, (unsigned)APPS_ART_SMALL_MAX);
        return false;
    }

    /* Gate 3 — the config quota, priced at the declared settings' NVS worst case
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

    /* Gate 4 — the cap (B5) and the slot that follows from it. A slug already
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
 * it is the ladder that mirrors orb_theme_install() (orb_theme.c:1456), reached
 * only from the worker task. It keeps that function's three card rules whole:
 * every path that opens a file on /sd takes the one card lock itself, never
 * through a helper that already holds it; the WHOLE payload checksums off the
 * card before the first erase; and the slot header goes last, so a torn copy
 * reads absent rather than corrupt (D9).                                     *
 * =========================================================================== */
#include <errno.h>
#include <fcntl.h>
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
#define APPS_CHUNK_BYTES 4096

static esp_err_t ensure_chunk(void)
{
    if (s_chunk) return ESP_OK;
    s_chunk = heap_caps_malloc(APPS_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return s_chunk ? ESP_OK : ESP_ERR_NO_MEM;
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

    char why[128];
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



/* The whole payload, checksummed OFF the card, both CRCs the writer emitted: geom
 * over the geometry, art over the pixels, each running crc32 starting at 0 the way
 * zlib.crc32 / esp_rom_crc32_le compose them (orbapp.py's order). An art-light app
 * has art_size 0 and an empty-bank crc of 0, which is exactly what orbapp.py writes,
 * so the loop runs zero times and the equality still holds. Mirrors
 * verify_container() (orb_theme.c:805) minus the geometry COUNT check — that belongs
 * to the painter and lands with P5's compositor, not the byte-for-byte checksum the
 * copy needs. */
static const char *verify_payload(int fd, const orbapp_hdr_t *h)
{
    if (ensure_chunk() != ESP_OK) return "out of memory for the card read buffer";

    uint32_t crc = 0;
    size_t pos = h->geom_off, left = h->geom_size;
    while (left) {
        const size_t want = left > APPS_CHUNK_BYTES ? APPS_CHUNK_BYTES : left;
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want)
            return "the file is shorter than its geometry claims — re-copy it";
        crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
        left -= want;
    }
    if (crc != h->geom_crc)
        return "the geometry does not match its checksum — re-copy the file onto the card";

    crc = 0;
    pos = h->art_off; left = h->art_size;
    while (left) {
        const size_t want = left > APPS_CHUNK_BYTES ? APPS_CHUNK_BYTES : left;
        if (rd_at(fd, &pos, s_chunk, want) != (ssize_t)want)
            return "the art is shorter than its header claims — re-copy it";
        crc = esp_rom_crc32_le(crc, s_chunk, (uint32_t)want);
        left -= want;
    }
    if (crc != h->art_crc)
        return "the art does not match its checksum — re-copy the file onto the card";
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
 * esp_partition_read on the header block mirrors slot_read (orb_theme.c:1040):
 * read, not mmap; 148 bytes do not justify an MMU entry and the header is not
 * mapped. The apps slots hold no pixels in P4, so there is no pixels read-back —
 * only the geometry region, CRCed FROM the partition before the header goes live,
 * exactly as orb_theme_install does for its blob (orb_theme.c:1501).            */

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
        const uint32_t want = (blob - done > APPS_CHUNK_BYTES) ? APPS_CHUNK_BYTES : (blob - done);
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
        const uint32_t want = (h->geom_size - off > APPS_CHUNK_BYTES) ? APPS_CHUNK_BYTES
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
 * sentence and not a slot (orb_theme_install's rule, orb_theme.c:1450). */
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

    /* The WHOLE payload checksums off the card before any erase. Both CRCs, over
     * the full geometry and the full art range, exactly as orb_theme.c verifies a
     * container — the half-copy D9 prices is caught here, on a page that still
     * lists the card, not after a reboot into a slot that will not draw. */
    const char *vv = verify_payload(fd, h);
    if (vv) { install_close(fd); return install_failed(ESP_ERR_INVALID_CRC, vv, why, whysz); }

    /* Unpack the declared settings — needed twice over: the quota prices them and
     * the resident record carries them (only these ids may reach NVS, §4.2). */
    orb_setting_desc_t settings[ORBAPP_SETTINGS_MAX];
    int n_settings = 0;
    if (h->n_settings > 0) {
        uint32_t sw[ORBAPP_SETTINGS_MAX * ORBAPP_SETTINGS_WORDS];
        size_t spos = h->setting_off;
        const size_t need = (size_t)h->n_settings * ORBAPP_SETTINGS_WORDS * 4u;
        if (rd_at(fd, &spos, (uint8_t *)sw, need) != (ssize_t)need) {
            install_close(fd);
            return install_failed(ESP_ERR_INVALID_SIZE,
                                  "the app file ended inside its settings table — re-copy it",
                                  why, whysz);
        }
        char rwhy[128];
        const char *r = orbapp_unpack_settings(sw,
                                               (size_t)h->n_settings * ORBAPP_SETTINGS_WORDS,
                                               h->n_settings, settings, rwhy, sizeof rwhy);
        if (r) { install_close(fd); return install_failed(ESP_ERR_INVALID_ARG, r, why, whysz); }
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

    /* Erase + copy + read-back CRC; the header goes live only once the geometry
     * came back intact (orb_theme.c:1494's ordering — a slot never goes live on
     * bytes nobody read back). The card lock comes off with install_close. */
    const char *fw = NULL;
    esp_err_t e = copy_prefix(p, fd, h, v.slot, &fw);
    if (e == ESP_OK) e = verify_slot_geom(p, h, v.slot, &fw);
    if (e == ESP_OK) e = write_slot_header(p, v.slot, raw, &fw);
    install_close(fd);
    if (e != ESP_OK) return install_failed(e, fw, why, whysz);


    /* Commit to the registry LAST, from the manifest — D19: caps come from the
     * header, never a stored grant word; art is art-light in P4 so no bank is
     * claimed (art_slot -1), and the boot ladder re-derives the same record from
     * the slot header next power. card_upsert copies slug/title/settings into the
     * table's own buffers, so this stack row and h_store may die on return. */
    orb_app_t row;
    memset(&row, 0, sizeof row);
    row.slug       = h->slug;
    row.title      = h->title;
    row.kind       = ORB_APP_KIND_APP;
    row.behaviour_kind = h->behaviour_kind;   /* D1/C6: selects the compiled module */
    row.caps       = h->caps;
    row.period_ms  = h->period_ms;
    row.settings   = (n_settings > 0) ? settings : NULL;
    row.n_settings = n_settings;
    row.art_offset = 0;
    row.art_bytes  = h->art_size;
    row.art_slot   = -1;
    if (orb_app_card_upsert(&row) < 0)
        return install_failed(ESP_ERR_NO_MEM,
                              "it reached flash but the app registry is full — uninstall one",
                              why, whysz);

    ESP_LOGI(TAG, "installed \"%s\" (%s) into app slot %d in %lld ms",
             h->title, stem, v.slot, (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
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
        row.caps       = h.caps;                 /* D19: from the slot header only */
        row.period_ms  = h.period_ms;
        row.settings   = (n_settings > 0) ? settings : NULL;
        row.n_settings = n_settings;
        row.art_offset = 0;
        row.art_bytes  = h.art_size;              /* art-light in P4: none claimed */
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


