/*
 * main/app/orb_app_scan.c — the /sd/apps listing; see the header for the
 * model, and main/art/orb_theme.c's orb_theme_scan() for the structure this
 * deliberately shadows: walk under the one card lock through orb_files_walk,
 * read only the header block here (the payload checksums are the install's,
 * D9), and keep every refused row with the reader's own sentence in .why.
 *
 * What is this layer's own and what is the mirror's, stated plainly: the
 * parse/check/layout refusals arrive from main/app/orb_app_manifest.c
 * verbatim — not one word re-cut here, because the same sentence is what
 * tools/orbapp.py answers with and the sentinel suites pin that pair. The
 * two refusals BELOW with no reader twin are card facts the reader cannot
 * know: a file that ends inside its own header block, and a file shorter
 * than the geometry it claims (D21 left no art claim to be short of). They are pinned as vectors in
 * tools/host_orbapp_scan_test.py, which lifts row_decide() the way
 * host_sd_test.py lifts theme_state().
 */
#include "orb_app_scan.h"

#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"          // the listing timing, as orb_theme.c:30 does it
#include "sd/orb_files.h"
#include "sd/orb_sd.h"

static const char *TAG = "appscan";

/* The decision over one listed file: the header block bytes (hdr_bytes
 * actually read, so a short file is described and not guessed at) plus
 * FATFS's own size in rec. PURE — no I/O, no locks, no NVS — so the host
 * suite compiles THIS function against the writer's real bytes. */
static void row_decide(const orb_file_rec_t *rec, const uint32_t *hdr,
                       size_t hdr_bytes, orbapp_scan_entry_t *e)
{
    orbapp_hdr_t h;
    char *const why = e->why;
    const size_t wsz = sizeof e->why;

    e->why[0] = '\0';
    e->slug[0] = '\0';
    e->title[0] = '\0';
    e->version[0] = '\0';
    e->app_id = e->caps = e->period_ms = e->serial = 0;
    e->n_settings = e->art_size = 0;
    /* The stem is echoed from the walk, which already passed it through
     * orb_sd_name_ok — it is the handle the page will act on even when
     * nothing else about the file survives validation. */
    strlcpy(e->stem, rec->name, sizeof e->stem);
    e->file_size = rec->size;

    if (hdr_bytes < ORBAPP_HDR_BYTES) {
        /* Before the reader gets a word: there is no trustworthy field to
         * name. This is a volume-level fact, not a format opinion — the
         * writer cannot produce it; a half-copy or a flaky card does. */
        e->err = ESP_ERR_INVALID_SIZE;
        strlcpy(e->why, "the file ends inside its own header block — re-copy"
                       " the file onto the card", wsz);
        return;
    }

    /* Checksum first, then semantics, then the file's own placement — the
     * same ladder orb_app_manifest.c documents, climbed one call at a time.
     * The stem goes to check_header: the identity triple says the file is
     * named for what it is, and here, not in the reader, is where a filename
     * exists to check it against. */
    const char *r = orbapp_parse_header(hdr, &h, why, wsz);
    if (r) { e->err = ESP_ERR_INVALID_ARG; return; }

    /* Past the hdr_crc these fields are trustworthy — the checksum covers
     * all of them — so a row that dies downstream still shows who it claimed
     * to be, exactly as a refused theme row keeps its header name. */
    strlcpy(e->slug, h.slug, sizeof e->slug);
    strlcpy(e->title, h.title, sizeof e->title);
    strlcpy(e->version, h.version, sizeof e->version);   /* D38: stamped or empty */
    e->app_id     = h.app_id;
    e->caps       = h.caps;
    e->period_ms  = h.period_ms;
    e->serial     = h.serial;
    e->n_settings = h.n_settings;
    e->art_size   = h.art_size;

    /* check_stem wants the FILENAME — the writer's filed_as_its_own_slug and
     * the reader's stem refusal are written against "slug.orbapp", and the
     * walk hands over the stem with the extension already stripped
     * (orb_files.c:228). Rebuild it here rather than loosen the reader: the
     * ".orbapp.part is a staging name" refusal stays reachable, and the
     * triple is checked against the exact string the author packed. */
    char fname[64];
    snprintf(fname, sizeof fname, "%s%s", rec->name, ORB_APP_EXT);

    r = orbapp_check_header(&h, 0, false, fname, why, wsz);
    if (r) { e->err = ESP_ERR_INVALID_ARG; return; }

    r = orbapp_check_file_layout(&h, why, wsz);
    if (r) { e->err = ESP_ERR_INVALID_ARG; return; }

    /* The card's facts against the header's claims. Longer than the claims
     * is tolerated: the copied prefix ends at geom_off+geom_size and the rest
     * of the file is the writer's zero pad to the payload sector, so trailing
     * bytes are junk, not damage — refusing them would fail a file an editor
     * had touched for one stray newline, against an author who did nothing
     * wrong. Shorter than the claims is the half-copy D9 prices, caught here
     * so the owner hears it from the listing instead of after a failed install
     * and a reboot lost. Since D21 (§0.9) the prefix is the whole claim: an
     * art-carrying header is refused above us, by the reader's own gate. */
    const uint64_t end = (uint64_t)h.geom_off + h.geom_size;
    if (end > (uint64_t)rec->size) {
        e->err = ESP_ERR_INVALID_SIZE;
        snprintf(why, wsz,
                 "the container claims %llu B of file and the card holds %u B"
                 " — re-copy the file onto the card",
                 (unsigned long long)end, (unsigned)rec->size);
        return;
    }

    e->err = ESP_OK;
}

typedef struct {
    orbapp_scan_entry_t *out;
    int                  cap;
    int                  n;
} scan_ctx_t;

/* The walk's callback. orb_files_walk holds the card lock and hands over an
 * open fd; this may read that fd and nothing else — no open(), no second
 * take of the mutex, no allocation (orb_files.h:70's contract, which
 * orb_theme.c:901's deadlock is the reason the contract exists). */
static bool scan_entry(void *arg, const orb_file_rec_t *rec, int fd)
{
    scan_ctx_t *const c = arg;
    if (c->n >= c->cap) return false;

    /* The header block on the stack: ORBAPP_HDR_BYTES is ~150 B, not the 4 KB
     * stream buffer orb_theme.c needs for payload hashing, and this callback
     * runs on whoever asked for the list — the httpd worker with its 4096-
     * byte stack is exactly why that number is small. pread, not read: the
     * fd the walk hands over is one the volume may have been using, and this
     * never assumes where its offset sits (orb_theme.c:771's rule). */
    uint32_t hdr[ORBAPP_HDR_WORDS];
    size_t got = 0;
    while (got < sizeof hdr) {
        const ssize_t g = pread(fd, (uint8_t *)hdr + got, sizeof hdr - got,
                                (off_t)got);
        if (g <= 0) break;
        got += (size_t)g;
    }

    row_decide(rec, hdr, got, &c->out[c->n]);
    /* Kept either way — the shown-with-reason rule. A card with four refused
     * files must say so with four rows, not with an empty page that reads as
     * an empty card. */
    c->n++;
    return true;
}

esp_err_t orb_app_scan_init(void)
{
    orb_file_src_t src;
    memset(&src, 0, sizeof src);
    snprintf(src.id,  sizeof src.id,  "%s", ORB_APP_SCAN_ID);
    snprintf(src.dir, sizeof src.dir, "%s", ORB_APP_DIR);
    snprintf(src.ext, sizeof src.ext, "%s", ORB_APP_EXT);
    src.max_entries = ORB_APP_SCAN_MAX;
    /* dir_under_sd() is orb_files' door and it stays the judge: this call
     * fails loudly at boot, on the ladder, if the declared directory ever
     * stops being a subdirectory of the volume. */
    const esp_err_t e = orb_files_register(ORB_APP_SCAN_SLUG, &src);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "drop zone %s/*%s registered (max %u listed per scan)",
                 src.dir, src.ext, (unsigned)src.max_entries);
    return e;
}

int orb_app_scan(orbapp_scan_entry_t *out, int cap)
{
    if (!out || cap <= 0) return -1;

    /* A missing declaration is a boot-order bug, and -1 is the honest answer
     * for it — same shape as orb_theme_scan() at orb_theme.c:1806: nothing
     * has been read off the card, and "no apps" would be a lie. */
    const orb_file_src_t *src = orb_files_src(ORB_APP_SCAN_SLUG, ORB_APP_SCAN_ID);
    if (!src) {
        ESP_LOGE(TAG, "no \"" ORB_APP_SCAN_ID "\" file source registered —"
                      " orb_app_scan_init() did not run");
        return -1;
    }

    /* Both ends timed and logged: a scan that never returns is the card
     * failure that looks like a dead web server from outside, and which line
     * is missing says whether the task waits on the lock or sits in FatFs
     * (orb_theme.c:1813's reasoning, inherited whole). An app upload to the
     * same directory lands as .orbapp.part — invisible to the walk's
     * extension filter by design, and check_stem refuses it by name if a
     * caller ever conjures one — so no upload-busy gate is needed until an
     * upload path for apps exists. */
    scan_ctx_t c = { .out = out, .cap = cap, .n = 0 };
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "listing %s", src->dir);
    if (orb_files_walk(src, scan_entry, &c, NULL) < 0) return -1;
    ESP_LOGI(TAG, "%d entr%slisted in %lld ms", c.n, c.n == 1 ? "y " : "ies ",
             (long long)((esp_timer_get_time() - t0) / 1000));
    return c.n;
}

