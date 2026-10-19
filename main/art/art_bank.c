/**
 * art_bank.c — see art_bank.h for the contract and the rationale.
 */
#include "art_bank.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "soc/soc_caps.h"   /* SOC_MMU_PAGE_SIZE: what a resident bank costs the DROM window */
#include <string.h>         /* memcpy: the arena refill copies through its read buffer */
#include "esp_rom_crc.h"    /* the refill's checksum, and the probes' bank crcs */
#include "esp_timer.h"      /* esp_timer_get_time(): the µs clock every timing here reads */

#if ORB_ART_M2_PROBE || ORB_ART_M3_PROBE
#include "esp_rom_crc.h"    /* crc of a bank through the mapping vs through the read path */
#include "esp_heap_caps.h"  /* free PSRAM at the failing instant: mechanism B's arena cap */
#endif

#if ORB_ART_LAT_PROBE || ORB_ART_M3_PROBE
#include "esp_timer.h"      /* esp_timer_get_time(): the µs clock the reclaim is timed against */
#endif

#include "esp_heap_caps.h"  /* arena allocation + free-PSRAM gate (MECH_B), and
                             * heap_caps_free in the re-grow/unmap dispatch —
                             * reachable textually in every build, so included
                             * always, not under an ORB_ART_MECH_B guard. */

static const char *TAG = "art_bank";

/* Mapped for the process lifetime on purpose; never unmapped. See header. */
static const uint8_t              *s_map;
static size_t                      s_bytes;
static size_t                      s_off;      /* partition offset s_map points at */
static esp_partition_mmap_handle_t s_handle;
static const esp_partition_t      *s_part;

/* The primary bank's second form (D30): a PSRAM arena copy made by
 * art_bank_fill() instead of a flash map. Exactly one of s_map / s_plane is set
 * at a time, so art_bank_ptr() stays the one accessor every reader uses and the
 * §3.3 promise that an app never learns which mechanism feeds its pixels holds
 * for the dial's own bank too, not only for a secondary's.
 *
 * s_fill_ms is the elapsed milliseconds of the last fused refill, and nothing
 * else: §20.5(2) is a boot-log line, and the line reads the number out of the
 * pass rather than out of the prediction that sized it. */
static uint8_t                    *s_plane;
static bool                        s_psram;
static uint32_t                    s_fill_ms;

/* Is the dial's bank resident, by either mechanism? The one liveness test, for
 * the same reason art_extra_live() is the only one the secondary table has: a
 * walk that keyed on .map alone would ignore the arena the moment D30 lit it. */
static bool art_bank_primary_live(void)
{
    return s_map != NULL || s_psram;
}

/* Release the primary, by whichever mechanism holds it. A re-map and a failed
 * refill's cleanup are the same operation, and two copies of it is how an arena
 * leaks 1.17 MB of PSRAM on a path that only runs at boot. */
static void art_bank_primary_release(void)
{
    if (s_psram) {
        heap_caps_free(s_plane);
        s_psram = false;
        s_plane = NULL;
    }
    if (s_map) {
        esp_partition_munmap(s_handle);
        s_map    = NULL;
        s_handle = 0;
    }
    s_bytes = 0;
    s_off   = 0;
}

/* The partition lookup both entry points need, in one place: a second copy of
 * the find is how the boot path and the install path disagree about which
 * partition holds the dial's pixels. art_bank_partition() below is the same
 * lookup, exposed. */
static const esp_partition_t *bank_partition(void)
{
    if (!s_part) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY,
                                          ART_BANK_PARTITION_NAME);
    }
    return s_part;
}

/* Secondary banks (P2, §3.3 mechanism A): one entry per mapped bank, stable
 * slot indices, resident until art_bank_unmap_extra() empties one. Index i
 * keeps its mapping until explicitly emptied so an app's cached slot number
 * outlives its neighbours' churn.
 *
 * An entry is EITHER a flash map (map/handle from esp_partition_mmap — the
 * mechanism-A residency the page budget bills) or a PSRAM arena copy
 * (psram/plane, filled by art_bank_map_extra() under ORB_ART_MECH_B — the
 * mechanism-B residency the page budget never sees). Exactly one side is set,
 * so art_extra_live() is the one liveness test every walk of this table uses:
 * a loop that keyed on .map alone would silently ignore every arena bank the
 * moment MECH_B ships on. */
typedef struct {
    const uint8_t              *map;
    size_t                      bytes;
    size_t                      offset;   /* partition offset map points at */
    esp_partition_mmap_handle_t handle;
    bool                        psram;    /* entry is an arena copy, not a map */
    uint8_t                    *plane;    /* arena base when psram, else NULL */
} art_extra_t;

/* A live secondary slot: a resident map, or (MECH_B only) a resident arena. */
static bool art_extra_live(const art_extra_t *e)
{
    return e->map != NULL || e->psram;
}

static art_extra_t s_extra[ART_BANK_EXTRA_MAX];

/* Booked-but-unmapped banks. Kept as per-bank entries, not a sum, because
 * art_bank_map_extra() converts the exact-size booking into the resident
 * mapping — the only way art_bank_mmu_pages() can bill a reserve-then-map
 * pair once. Cap = one planned bank per installed app (ruling B5). */
#define ORB_ART_RESERVE_MAX 8u
static size_t s_res_bytes[ORB_ART_RESERVE_MAX];

/* Round a raw bank length up to the whole MMU pages the window bills. Kept
 * independent of the ORB_ART_* defines below so art_bank_init() can use it. */
static size_t art_round_pages(size_t bytes)
{
    return (bytes + SOC_MMU_PAGE_SIZE - 1) / SOC_MMU_PAGE_SIZE * SOC_MMU_PAGE_SIZE;
}

/* Consume a booking of exactly `bytes` (already page-rounded): a mapped bank
 * is that planned bank, so its pages are billed once, in the mapping. A map
 * that matches no booking is a bank that was never planned — legal, and the
 * budget check at map time is what keeps the window honest either way. */
static void art_consume_reservation(size_t bytes)
{
    for (unsigned i = 0; i < ORB_ART_RESERVE_MAX; i++)
        if (s_res_bytes[i] == bytes) {
            s_res_bytes[i] = 0;
            return;
        }
}

/* ---- residency: the page window, the arena margin, and what a refusal costs ----
 *
 * These sit here, ahead of both primary entry points, because D30 put an arena
 * gate in art_bank_fill() and an MMU gate in art_bank_map_extra(), and a refusal
 * from either has to land in the same counter for orb_art_refusals() to keep its
 * meaning. The budget is arithmetic, not a reading of the live MMU table. */

#define ORB_ART_PAGE_SIZE   SOC_MMU_PAGE_SIZE      /* 64 KB on the S3 default */
#define ORB_ART_PAGE_ALIGN  ORB_ART_PAGE_SIZE      /* what mmap rounds each end to */

/* The DROM window is 256 pages wide (docs/app-platform-plan.md §3.1), and art does not
 * have it to itself: the image's own flash segments are mapped through it too. Re-read
 * from `idf.py size` on 2026-10-11 — Flash Code .text 1,049,060 B = 17 pages, Flash Data
 * .rodata + .appdesc 305,640 B = 5 pages — so 22 pages are spent before a bank is
 * requested. §3.1 counted only .rodata's 5 and so understated the fixed cost by 17; the
 * conclusion is unchanged because the headroom is enormous either way. Re-derive this
 * whenever the image grows — and believe it only once the M2 probe (bottom of this
 * file) has watched the flash MMU refuse at the total it predicts: it is arithmetic
 * from a size report, not a reading of the live MMU table, until that line matches.
 * A D30 arena bank is not in it at all: the refill takes no pages. */
#define ORB_ART_DROM_PAGES       256u
#define ORB_ART_IMAGE_PAGES      22u   /* .text 17 + .rodata/.appdesc 5, per idf.py size */
#define ORB_ART_PAGE_BUDGET      (ORB_ART_DROM_PAGES - ORB_ART_IMAGE_PAGES)

static unsigned s_reserve_refusals;

esp_err_t art_bank_map(size_t offset, size_t need_bytes)
{
    if (need_bytes == 0)
        return ESP_ERR_INVALID_ARG;

    if (s_map && s_bytes >= need_bytes && s_off == offset)
        return ESP_OK;                    /* already mapped big enough */

    if (art_bank_primary_live()) {
        /* A different theme than the one we made resident: drop the old bank and
         * redo it. Rare (only a load from the card changes it, and that reboots
         * first), and callers must rebuild every cached LVGL descriptor
         * afterwards — which is exactly why orb_theme installs before
         * clock_view_init() rather than while the dial is running. The release is
         * by mechanism, so an arena bank from art_bank_fill() is freed here and
         * not munmap'd as a map it never was. */
        ESP_LOGI(TAG, "re-mapping @0x%x %u B -> %u B",
                 (unsigned)offset, (unsigned)s_bytes, (unsigned)need_bytes);
        art_bank_primary_release();
    }

    /* By LABEL, never by index/subtype: the label is the stable identity, and
     * `assets` is a subtype-less data partition (gen_esp32part.py defaults an
     * empty subtype to `undefined` 0x06, the same value `www` carries). */
    s_part = bank_partition();
    if (!s_part) {
        ESP_LOGE(TAG, "partition \"%s\" not found — is the partition table flashed?",
                 ART_BANK_PARTITION_NAME);
        return ESP_ERR_NOT_FOUND;
    }

    if (offset + need_bytes > s_part->size) {
        ESP_LOGE(TAG, "theme needs %u B at 0x%x but \"%s\" holds only %u B",
                 (unsigned)need_bytes, (unsigned)offset, ART_BANK_PARTITION_NAME,
                 (unsigned)s_part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    const void *map = NULL;
    esp_err_t err = esp_partition_mmap(s_part, offset, need_bytes, ESP_PARTITION_MMAP_DATA,
                                       &map, &s_handle);
    if (err != ESP_OK) {
        /* Almost always the DROM window being exhausted by the app's own
         * .rodata, not a shortage of heap — nothing here allocates. A theme with
         * a bigger bank than the window can hold fails right here, loudly,
         * instead of as a crash on the first paint. */
        ESP_LOGE(TAG, "mmap %u B from \"%s\" @0x%x failed: %s",
                 (unsigned)need_bytes, s_part->label, (unsigned)(s_part->address + offset),
                 esp_err_to_name(err));
        return err;
    }

    s_map   = (const uint8_t *)map;
    s_bytes = need_bytes;
    s_off   = offset;
    art_consume_reservation(art_round_pages(need_bytes));

    ESP_LOGI(TAG, "bank \"%s\" @0x%x, %u B mapped of %u B (theme blob %u B), %u MMU pages",
             s_part->label, (unsigned)(s_part->address + offset), (unsigned)s_bytes,
             (unsigned)s_part->size, (unsigned)need_bytes,
             (unsigned)art_bank_mmu_pages());
    return ESP_OK;
}

esp_err_t art_bank_fill(size_t offset, size_t need_bytes, bool verify,
                        uint32_t expect_crc, uint32_t *out_ms)
{
    if (need_bytes == 0)
        return ESP_ERR_INVALID_ARG;

    /* The gate leads the allocation and the allocation leads the read, so a board
     * that cannot hold this bank refuses with the free figure in its sentence
     * before spending a second and a quarter on pixels it will free. §20.2's Arm A
     * handoff reads this refusal, not a bare NULL. */
    const size_t free_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_p < need_bytes + ORB_ART_ARENA_MARGIN) {
        s_reserve_refusals++;
        ESP_LOGE(TAG, "arena fill %u B @0x%x refused: %u B PSRAM free, %u B bank + %u B "
                      "margin needed — Arm A takes this install.",
                 (unsigned)need_bytes, (unsigned)offset, (unsigned)free_p,
                 (unsigned)need_bytes, (unsigned)ORB_ART_ARENA_MARGIN);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *arena = (uint8_t *)heap_caps_malloc(need_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena) {
        s_reserve_refusals++;
        ESP_LOGE(TAG, "arena fill %u B @0x%x refused: heap_caps_malloc failed with %u B free",
                 (unsigned)need_bytes, (unsigned)offset, (unsigned)free_p);
        return ESP_ERR_NO_MEM;
    }

    s_part = bank_partition();
    if (!s_part) {
        heap_caps_free(arena);
        ESP_LOGE(TAG, "partition \"%s\" not found — is the partition table flashed?",
                 ART_BANK_PARTITION_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    if (offset + need_bytes > s_part->size) {
        heap_caps_free(arena);
        ESP_LOGE(TAG, "theme needs %u B at 0x%x but \"%s\" holds only %u B",
                 (unsigned)need_bytes, (unsigned)offset, ART_BANK_PARTITION_NAME,
                 (unsigned)s_part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* ONE pass, and it is the point of the function: each chunk comes off the
     * partition, lands in the arena, and is CRCed from the arena in the same loop,
     * so a verified refill costs the same bytes over the SPI as an unchecked one.
     * The pre-D30 boot read the slot once to checksum it and mapped it for a second
     * pass to draw from; this replaces that second pass (§20.2's "the same code as
     * the flip's flash sink in read-only shape").
     *
     * 4 KB static, never on the stack: this runs on main_task before the display
     * exists, and the buffer is the destination of an SPI flash read. */
    static uint8_t s_fill_chunk[4096];
    const int64_t  t0 = esp_timer_get_time();
    uint32_t       crc = 0;
    esp_err_t      err = ESP_OK;
    for (size_t done = 0; done < need_bytes; ) {
        size_t chunk = need_bytes - done;
        if (chunk > sizeof s_fill_chunk) chunk = sizeof s_fill_chunk;
        if (esp_partition_read(s_part, offset + done, s_fill_chunk, chunk) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        memcpy(arena + done, s_fill_chunk, chunk);
        if (verify) crc = esp_rom_crc32_le(crc, arena + done, chunk);
        done += chunk;
    }
    if (err != ESP_OK) {
        heap_caps_free(arena);
        ESP_LOGE(TAG, "arena fill %u B from \"%s\" @0x%x failed to read — bank left empty",
                 (unsigned)need_bytes, s_part->label, (unsigned)(s_part->address + offset));
        return err;
    }
    if (verify && crc != expect_crc) {
        heap_caps_free(arena);
        ESP_LOGE(TAG, "arena fill %u B from \"%s\" @0x%x: checksum %08lx is not the header's "
                      "%08lx — the slot is corrupt, and it does not reach the glass",
                 (unsigned)need_bytes, s_part->label, (unsigned)(s_part->address + offset),
                 (unsigned long)crc, (unsigned long)expect_crc);
        return ESP_ERR_INVALID_CRC;
    }

    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    art_bank_primary_release();          /* a default-path map, or an older arena */
    s_plane   = arena;
    s_psram   = true;
    s_bytes   = need_bytes;
    s_off     = offset;
    s_fill_ms = ms;
    art_consume_reservation(art_round_pages(need_bytes));

    ESP_LOGI(TAG, "bank \"%s\" @0x%x, %u B refilled to the PSRAM arena in %u ms%s "
                  "(%u B free after, %u of %u art pages)",
             s_part->label, (unsigned)(s_part->address + offset), (unsigned)need_bytes,
             (unsigned)ms, verify ? ", checksum verified" : "",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)art_bank_mmu_pages(), (unsigned)ORB_ART_PAGE_BUDGET);
    if (out_ms) *out_ms = ms;
    return ESP_OK;
}

esp_err_t art_bank_init(size_t offset, size_t need_bytes)
{
    /* One entry point, two mechanisms, chosen by the define rather than by the
     * caller — §3.3's rule applied to the dial's own bank for the first time by
     * D30. A caller that must have the map whatever the build says (the card-less
     * fallback, or Arm A) calls art_bank_map() directly. */
#if ORB_ART_MECH_B
    return art_bank_fill(offset, need_bytes, false, 0, NULL);
#else
    return art_bank_map(offset, need_bytes);
#endif
}

esp_err_t art_bank_map_at_0(size_t need_bytes) { return art_bank_map(0, need_bytes); }

uint32_t art_bank_fill_ms(void)  { return s_fill_ms; }
bool     art_bank_is_arena(void) { return s_psram; }

const esp_partition_t *art_bank_partition(void)
{
    /* Same label-based lookup art_bank_init() uses, exposed so orb_theme.c does
     * not have a second copy of it to drift: two finds that disagree about how
     * to locate the bank is how an install writes to one partition and the dial
     * reads another. */
    if (!s_part) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY,
                                          ART_BANK_PARTITION_NAME);
    }
    return s_part;
}

const uint8_t *art_bank_ptr(void)
{
    /* One accessor, two sources: the arena base and the flash map are the same
     * readable pixels to whoever reads them. This is §3.3's promise, and D30 made
     * the dial's own bank a second bank rather than the one special case — a
     * reader that behaved differently by mechanism would be two paint paths. */
    return s_psram ? s_plane : s_map;
}

size_t art_bank_bytes(void)
{
    return s_bytes;
}

bool art_bank_ready(void)
{
    return art_bank_primary_live();
}

/* ---- residency: the page budget, secondary banks, and the M2 probe -------
 * (docs/app-platform-plan.md §3.3, mechanism A — map many, the default)
 *
 * Every resident bank costs ceil(bytes / MMU page) pages out of the 256 the S3
 * gives DROM, so "two flagship banks plus the image" is decidable on paper
 * before it is decided on the bench. The table below makes that arithmetic
 * live: the primary plus up to ART_BANK_EXTRA_MAX secondaries, each billed as
 * its own virtual region — a second map of the same flash range is a second
 * set of MMU entries, because the window bills the virtual, not the physical.
 */

/* The page-size, window-budget and refusal-counter declarations these rules need
 * sit above art_bank_map(), where the primary bank's own gates read them (D30). */

/* Liveness goes through art_extra_live(): an arena bank is resident art too,
 * so it counts here even though it bills no MMU pages below. */
unsigned art_bank_extra_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ART_BANK_EXTRA_MAX; i++)
        if (art_extra_live(&s_extra[i])) n++;
    return n;
}

static unsigned art_reservation_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ORB_ART_RESERVE_MAX; i++)
        if (s_res_bytes[i]) n++;
    return n;
}

static size_t art_reserved_total(void)
{
    size_t t = 0;
    for (unsigned i = 0; i < ORB_ART_RESERVE_MAX; i++)
        t += s_res_bytes[i];
    return t;
}

/* Pages every region costs: each resident map and each outstanding booking
 * rounds up on its own, because each is its own virtual region. One rounded
 * sum of their bytes would understate a ragged pair — the same reason the
 * slots are cut on page boundaries. A PSRAM arena bank is DELIBERATELY absent
 * from this sum (MECH_B): it never occupies the window, so it cannot cost it. */
static size_t art_pages_accounted(void)
{
    /* The primary bills pages only when it IS a map: a D30 refill puts the dial's
     * own bank in PSRAM, and pages it never touches are not its cost — the same
     * rule the arena secondaries follow one line below. */
    size_t pages = s_map ? art_bank_pages_for(s_bytes) : 0;
    for (unsigned i = 0; i < ART_BANK_EXTRA_MAX; i++)
        if (art_extra_live(&s_extra[i]) && !s_extra[i].psram)
            pages += art_bank_pages_for(s_extra[i].bytes);
    for (unsigned i = 0; i < ORB_ART_RESERVE_MAX; i++)
        if (s_res_bytes[i])
            pages += art_bank_pages_for(s_res_bytes[i]);
    return pages;
}

esp_err_t orb_art_reserve(size_t bytes)
{
    if (bytes == 0) return ESP_ERR_INVALID_SIZE;
    /* A ragged request costs a whole page at each end of the mapping, so the
     * slot layout is cut on 64 KB boundaries and this refuses anything else
     * instead of quietly eating the slack. */
    if (bytes % ORB_ART_PAGE_ALIGN) return ESP_ERR_INVALID_SIZE;
    /* The cap is on the running total, not on this request: two banks that each
     * fit alone are exactly what overflows the window (two maximum-size 8 MB
     * slots would cost 128+128 pages against a 234-page budget). Refusing here is
     * also the reason the accounting exists at all — an installer that can only
     * report a page fault at boot has no answer for the owner. */
    const size_t want = art_pages_accounted() + art_bank_pages_for(bytes);
    if (want > ORB_ART_PAGE_BUDGET) {
        s_reserve_refusals++;
        ESP_LOGE(TAG, "reserve %u B refused: would cost %u pages, total %u of %u budgeted "
                      "(%u banks accounted). Install fewer themes, or shrink this one.",
                 (unsigned)bytes, (unsigned)art_bank_pages_for(bytes),
                 (unsigned)want, (unsigned)ORB_ART_PAGE_BUDGET,
                 (unsigned)orb_art_resident_count());
        return ESP_ERR_NO_MEM;
    }
    unsigned free_i = ORB_ART_RESERVE_MAX;
    for (unsigned i = 0; i < ORB_ART_RESERVE_MAX; i++)
        if (s_res_bytes[i] == 0) { free_i = i; break; }
    if (free_i == ORB_ART_RESERVE_MAX) {
        /* Pages were available but no table entry holds the plan — more booked
         * apps than B5 allows. An installer bug, not a window shortage, but
         * counted as a refusal so /api/v1/status still shows it. */
        s_reserve_refusals++;
        ESP_LOGE(TAG, "reserve %u B refused: all %u reservation slots are booked",
                 (unsigned)bytes, (unsigned)ORB_ART_RESERVE_MAX);
        return ESP_ERR_NO_MEM;
    }
    s_res_bytes[free_i] = bytes;
    ESP_LOGI(TAG, "reserve %u B (%u pages), %u reservations / %u B accounted (%u of %u pages)",
             (unsigned)bytes, (unsigned)art_bank_pages_for(bytes),
             art_reservation_count(), (unsigned)art_reserved_total(),
             (unsigned)want, (unsigned)ORB_ART_PAGE_BUDGET);
    return ESP_OK;
}

size_t art_bank_pages_for(size_t bytes)
{
    return (bytes + ORB_ART_PAGE_SIZE - 1) / ORB_ART_PAGE_SIZE;
}

unsigned orb_art_refusals(void)
{
    /* How often a requested residency — a booking or a map — was refused for
     * want of MMU pages. Non-zero with art_banks looking healthy is the
     * signature of an install that "did nothing": the app asked, the budget
     * said no, and only this counter knows. */
    return s_reserve_refusals;
}

size_t art_bank_mmu_pages(void)
{
    /* What the flash MMU is carrying for art right now: every resident map plus
     * every outstanding reservation. A reservation and the map that fills it are
     * the same bank — art_bank_map_extra() consumes the booking on success, so
     * the pair never bills its pages twice. */
    return art_pages_accounted();
}

size_t orb_art_resident_count(void)
{
    /* Liveness, not mechanism: an arena primary is a resident bank exactly as a
     * mapped one is, and D30 made the primary's arena a reachable state. */
    return (art_bank_ready() ? 1u : 0u) + art_bank_extra_count() + art_reservation_count();
}

size_t orb_art_reserved_bytes(void)
{
    /* Outstanding bookings only: a consumed reservation is now a mapping, and
     * its bytes show under art_bank_extra_bytes(), not here. */
    return art_reserved_total();
}

/* ---- secondary banks (P2, mechanism A) ------------------------------------
 * The dial boots with its own bank resident and, from here, so can a second
 * app: map its payload once at bring-up, composite from art_bank_extra_ptr()
 * forever, unmap only when its cached descriptors are gone. The page budget
 * is enforced here as hard as in orb_art_reserve(), because a map can arrive
 * with no booking behind it at all. */

esp_err_t art_bank_map_extra(size_t offset, size_t need_bytes, int *out_slot)
{
    if (need_bytes == 0 || offset % ORB_ART_PAGE_ALIGN)
        return ESP_ERR_INVALID_SIZE;

    const esp_partition_t *part = art_bank_partition();
    if (!part) {
        ESP_LOGE(TAG, "extra map: partition \"%s\" not found", ART_BANK_PARTITION_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    if (offset + need_bytes > part->size) {
        ESP_LOGE(TAG, "extra bank needs %u B at 0x%x but \"%s\" holds only %u B",
                 (unsigned)need_bytes, (unsigned)offset, part->label,
                 (unsigned)part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* The same offset twice is the same app asking again, not a second copy of
     * its art: return or grow that entry rather than bill a second region for
     * it. A caller that really wants a second mapping of one range (the M2
     * stress loop does) goes through the probe's raw path and pays for it. */
    int slot = -1;
    for (unsigned i = 0; i < ART_BANK_EXTRA_MAX; i++)
        if (art_extra_live(&s_extra[i]) && s_extra[i].offset == offset) {
            if (s_extra[i].bytes >= need_bytes) {          /* already covered */
                if (out_slot) *out_slot = (int)i;
                return ESP_OK;
            }
            slot = (int)i;                                 /* grow in place */
            break;
        }
    if (slot < 0)
        for (unsigned i = 0; i < ART_BANK_EXTRA_MAX; i++)
            if (!art_extra_live(&s_extra[i])) { slot = (int)i; break; }
    if (slot < 0) {
        ESP_LOGE(TAG, "extra map %u B @0x%x refused: all %u secondary slots hold banks",
                 (unsigned)need_bytes, (unsigned)offset, (unsigned)ART_BANK_EXTRA_MAX);
        return ESP_ERR_NOT_FOUND;
    }

    /* Budget on the running total, minus what this entry already pays for if it
     * is a re-grow — the same rule orb_art_reserve() applies, applied to a map
     * that arrived with no booking behind it. Checked before the munmap below,
     * so a refused request never disturbs what is already resident. Under
     * MECH_B the window check is skipped on purpose: an arena bank takes no
     * pages, and refusing it for want of pages it will never spend is the
     * rigging M3 exists to avoid — its gate is free PSRAM, at the copy below. */
#if !ORB_ART_MECH_B
    size_t want = art_pages_accounted() + art_bank_pages_for(need_bytes);
    if (art_extra_live(&s_extra[slot]) && !s_extra[slot].psram)
        want -= art_bank_pages_for(s_extra[slot].bytes);
    if (want > ORB_ART_PAGE_BUDGET) {
        s_reserve_refusals++;
        ESP_LOGE(TAG, "extra map %u B @0x%x refused: would cost %u pages, total %u of %u "
                      "budgeted (%u banks accounted). Install fewer themes, or shrink this one.",
                 (unsigned)need_bytes, (unsigned)offset,
                 (unsigned)art_bank_pages_for(need_bytes), (unsigned)want,
                 (unsigned)ORB_ART_PAGE_BUDGET, (unsigned)orb_art_resident_count());
        return ESP_ERR_NO_MEM;
    }
#endif

    if (art_extra_live(&s_extra[slot])) {
        /* Re-grow: drop the old bank first, as the primary's re-map does — the
         * caller owns the question of who was still reading it. */
        ESP_LOGI(TAG, "extra bank %d re-mapping @0x%x %u B -> %u B",
                 slot, (unsigned)offset, (unsigned)s_extra[slot].bytes,
                 (unsigned)need_bytes);
        if (s_extra[slot].psram) heap_caps_free(s_extra[slot].plane);
        else                     esp_partition_munmap(s_extra[slot].handle);
        s_extra[slot] = (art_extra_t){ 0 };
    }

#if ORB_ART_MECH_B
    /* ---- mechanism B: fill the slot by copying, never by mapping ---- */
    const size_t free_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_p < need_bytes + ORB_ART_ARENA_MARGIN) {
        s_reserve_refusals++;
        ESP_LOGE(TAG, "arena map %u B @0x%x refused: %u B PSRAM free, %u B bank + %u B "
                      "margin needed — refuse the app, not the boot.",
                 (unsigned)need_bytes, (unsigned)offset, (unsigned)free_p,
                 (unsigned)need_bytes, (unsigned)ORB_ART_ARENA_MARGIN);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *arena = (uint8_t *)heap_caps_malloc(need_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena) {
        s_reserve_refusals++;
        ESP_LOGE(TAG, "arena map %u B @0x%x refused: heap_caps_malloc failed with %u B free",
                 (unsigned)need_bytes, (unsigned)offset, (unsigned)free_p);
        return ESP_ERR_NO_MEM;
    }
    static uint8_t s_arena_chunk[4096];   /* static: main/LVGL stack is not ours */
    esp_err_t rerr = ESP_OK;
    for (size_t done = 0; done < need_bytes; ) {
        size_t chunk = need_bytes - done;
        if (chunk > sizeof s_arena_chunk) chunk = sizeof s_arena_chunk;
        if (esp_partition_read(part, offset + done, arena + done, chunk) != ESP_OK) {
            rerr = ESP_FAIL;
            break;
        }
        done += chunk;
    }
    if (rerr != ESP_OK) {
        heap_caps_free(arena);
        ESP_LOGE(TAG, "arena copy %u B from \"%s\" @0x%x failed — slot left empty",
                 (unsigned)need_bytes, part->label, (unsigned)(part->address + offset));
        return rerr;
    }
    s_extra[slot].plane  = arena;
    s_extra[slot].psram  = true;
    s_extra[slot].bytes  = need_bytes;
    s_extra[slot].offset = offset;
    art_consume_reservation(art_round_pages(need_bytes));

    ESP_LOGI(TAG, "extra bank %d \"%s\" @0x%x, %u B copied to the PSRAM arena (%u B free after), "
                  "%u of %u art pages, %u banks accounted",
             slot, part->label, (unsigned)(part->address + offset), (unsigned)need_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)art_bank_mmu_pages(), (unsigned)ORB_ART_PAGE_BUDGET,
             (unsigned)orb_art_resident_count());
    if (out_slot) *out_slot = slot;
    return ESP_OK;
#else
    const void *map = NULL;
    esp_err_t err = esp_partition_mmap(part, offset, need_bytes, ESP_PARTITION_MMAP_DATA,
                                       &map, &s_extra[slot].handle);
    if (err != ESP_OK) {
        /* The budget said yes and the MMU said no: the live table disagrees with
         * the arithmetic in ORB_ART_PAGE_BUDGET — M2's calibration question
         * arriving as a bug report. Loud, like the primary's failure, because
         * the app whose pixels never arrived cannot say so itself. */
        ESP_LOGE(TAG, "extra mmap %u B from \"%s\" @0x%x failed: %s (%u of %u art pages accounted)",
                 (unsigned)need_bytes, part->label, (unsigned)(part->address + offset),
                 esp_err_to_name(err), (unsigned)art_pages_accounted(),
                 (unsigned)ORB_ART_PAGE_BUDGET);
        return err;
    }

    s_extra[slot].map    = (const uint8_t *)map;
    s_extra[slot].bytes  = need_bytes;
    s_extra[slot].offset = offset;
    art_consume_reservation(art_round_pages(need_bytes));

    ESP_LOGI(TAG, "extra bank %d \"%s\" @0x%x, %u B mapped, %u of %u art pages, %u banks accounted",
             slot, part->label, (unsigned)(part->address + offset), (unsigned)need_bytes,
             (unsigned)art_pages_accounted(), (unsigned)ORB_ART_PAGE_BUDGET,
             (unsigned)orb_art_resident_count());
    if (out_slot) *out_slot = slot;
    return ESP_OK;
#endif /* ORB_ART_MECH_B */
}


const uint8_t *art_bank_extra_ptr(int slot)
{
    if (slot < 0 || slot >= ART_BANK_EXTRA_MAX) return NULL;
    /* One accessor, two sources: the arena base and the flash map are the same
     * readable pixels to a compositor — that equivalence IS the §3.3 ABI. */
    return s_extra[slot].psram ? s_extra[slot].plane : s_extra[slot].map;
}

size_t art_bank_extra_bytes(int slot)
{
    if (slot < 0 || slot >= ART_BANK_EXTRA_MAX) return 0;
    return s_extra[slot].bytes;
}

esp_err_t art_bank_unmap_extra(int slot)
{
    if (slot < 0 || slot >= ART_BANK_EXTRA_MAX) return ESP_ERR_INVALID_ARG;
    if (!art_extra_live(&s_extra[slot])) return ESP_OK;         /* already empty */
    /* The munmap voids every pointer into the region, so "which app just lost
     * its pixels" must be answerable from the boot log alone: silent unmaps are
     * how a garbage paint becomes unexplainable. */
    ESP_LOGI(TAG, "extra bank %d unmapped (%u B @0x%x freed, %u banks accounted)",
             slot, (unsigned)s_extra[slot].bytes, (unsigned)s_extra[slot].offset,
             (unsigned)(orb_art_resident_count() - 1u));
    if (s_extra[slot].psram) heap_caps_free(s_extra[slot].plane);   /* mechanism B */
    else                     esp_partition_munmap(s_extra[slot].handle);
    s_extra[slot] = (art_extra_t){ 0 };
    return ESP_OK;
}

/* ---- M2 bench probe (docs/app-platform-plan.md §6 M2) --------------------
 * One-shot boot measurement, compiled out by default. Answers the arithmetic
 * claims of §3.1/§3.3 with the real MMU: the banks the budget says fit map,
 * stay mapped, and read back byte-exact through the mapping; the flash MMU
 * itself refuses at the page total ORB_ART_IMAGE_PAGES predicts. The two
 * refusal lines are the M2 record, and the free-PSRAM figure at the failing
 * instant is mechanism B's arena cap.
 *
 * The bank list is a fact about what the bench is flashed with, not about the
 * product: the aviator baked in at offset 0 (already the primary), and the
 * slot A / slot B payloads left on flash by their installs. mmap bills address
 * ranges, not contents, so a bank's size is all the probe needs of it; the
 * CRC is what proves a mapped read returns the same bytes the read path
 * returns — the only difference between a sprite read that works and one that
 * paints garbage. */
#if ORB_ART_M2_PROBE || ORB_ART_M3_PROBE

#define M2_BANK_A_OFF   0x00010000u
#define M2_BANK_A_BYTES 2503747u   /* the perpetual-calendar flagship: 39 pages */
#define M2_BANK_B_OFF   0x00410000u
#define M2_BANK_B_BYTES 1978973u   /* the slot B payload: 31 pages */
#define M2_STRESS_BASE   0x00100000u  /* distinct 64 KB-aligned offsets, one per stress bank:
                                       * map_extra keys same-offset as same-app re-request, so
                                       * stress maps sharing an offset would grow in place
                                       * instead of adding regions — 16 iterations reach
                                       * 0x2E0000 + 3,858,874 < the 8 MB partition */
#define M2_STRESS_STRIDE 0x00020000u
#define M2_STRESS_BYTES  3858874u     /* the kit3 flagship size: 59 pages */
#define M2_PUSH_MAX      16u

/* Shared flash-read staging buffer — PSRAM, deliberately. These probes run on
 * a heap whose largest internal block measured 7,168 B this bench week: a 4 KB
 * static .bss chunk here is what put httpd_start's 8 KB task stack over the
 * line (ESP_ERR_HTTPD_TASK at boot, observed 2026-10-12 with the M3 probe's
 * two staging statics resident). Boot-probe scratch earns no internal RAM.
 * One buffer serves every caller (erase gate, CRC loops) — no two reads
 * overlap in a probe pass, so aliasing it is safe and halves the footprint. */
static uint8_t *s_m2_chunk;
static uint8_t *m2_stage_buf(void)
{
    if (!s_m2_chunk)
        s_m2_chunk = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_m2_chunk;
}

static bool m2_crc_verified(const esp_partition_t *part, const uint8_t *map,
                            size_t off, size_t bytes)
{
    uint8_t *chunk = m2_stage_buf();
    if (!chunk) { ESP_LOGE(TAG, "m2: no 4 KB PSRAM stage — cannot CRC-verify"); return false; }
    const uint32_t crc_map = esp_rom_crc32_le(0, map, (uint32_t)bytes);
    uint32_t crc_read = 0;
    for (size_t done = 0; done < bytes; ) {
        size_t want = bytes - done;
        if (want > 4096) want = 4096;
        if (esp_partition_read(part, off + done, chunk, want) != ESP_OK)
            return false;
        crc_read = esp_rom_crc32_le(crc_read, chunk, (uint32_t)want);
        done += want;
    }
    return crc_map == crc_read;
}

#define M2_MB_U(free) (unsigned)((free) >> 20)
#define M2_MB_T(free) (unsigned)((((free) * 10) >> 20) % 10)
#endif /* shared M2/M3 preamble */

#if ORB_ART_M2_PROBE
/* The plan's M2 line: banks resident, pages of 256 claimed (art + the image's
 * own), and whether the bytes through the mapping are the bytes on the flash. */
static void m2_log_resident(const esp_partition_t *part, const char *what,
                            esp_err_t err, int slot, size_t off, size_t bytes)
{
    bool crc_ok = false;
    if (err == ESP_OK)
        crc_ok = m2_crc_verified(part, art_bank_extra_ptr(slot), off, bytes);
    ESP_LOGI(TAG, "m2: %s — %u banks resident = %u pages of %u, %s; crc %s through the mapping",
             what,
             (unsigned)orb_art_resident_count(),
             (unsigned)(art_bank_mmu_pages() + ORB_ART_IMAGE_PAGES),
             (unsigned)ORB_ART_DROM_PAGES,
             err == ESP_OK ? "ok" : esp_err_to_name(err),
             crc_ok ? "verified" : (err == ESP_OK ? "MISMATCH" : "skipped"));
}


void art_bank_m2_probe(void)
{
    const esp_partition_t *part = art_bank_partition();
    if (!part) {
        ESP_LOGE(TAG, "m2: no \"%s\" partition — flash the partition table first",
                 ART_BANK_PARTITION_NAME);
        return;
    }
    ESP_LOGI(TAG, "m2: start — primary %u B (%u pages, %s); budget %u of %u pages, image claims %u",
             (unsigned)art_bank_bytes(), (unsigned)art_bank_pages_for(art_bank_bytes()),
             art_bank_ready() ? "resident" : "MAPPING FAILED, measure degraded",
             (unsigned)ORB_ART_PAGE_BUDGET, (unsigned)ORB_ART_DROM_PAGES,
             (unsigned)ORB_ART_IMAGE_PAGES);

    /* Two flagships on the product path: book both sizes, then map both. The
     * accounting is right only if reserve-then-map bills each bank's pages once
     * — the ok lines below read art_bank_mmu_pages() back to prove it. */
    const size_t a_book = art_round_pages(M2_BANK_A_BYTES);
    const size_t b_book = art_round_pages(M2_BANK_B_BYTES);
    ESP_LOGI(TAG, "m2: booked A %u B: %s", (unsigned)a_book, esp_err_to_name(orb_art_reserve(a_book)));
    ESP_LOGI(TAG, "m2: booked B %u B: %s", (unsigned)b_book, esp_err_to_name(orb_art_reserve(b_book)));

    int slot = -1;
    esp_err_t err = art_bank_map_extra(M2_BANK_A_OFF, M2_BANK_A_BYTES, &slot);
    m2_log_resident(part, "bank A", err, slot, M2_BANK_A_OFF, M2_BANK_A_BYTES);
    slot = -1;
    err = art_bank_map_extra(M2_BANK_B_OFF, M2_BANK_B_BYTES, &slot);
    m2_log_resident(part, "bank B", err, slot, M2_BANK_B_OFF, M2_BANK_B_BYTES);

    /* The carousel's real question: push banks until the budget itself refuses,
     * and keep every accepted one mapped. The clock fps lines later in this
     * boot are the proof of the pass test — resident UNDER load, not just
     * mappable. */
    unsigned stress = 0;
    size_t s_off = M2_STRESS_BASE;
    while (stress < M2_PUSH_MAX) {
        slot = -1;
        err = art_bank_map_extra(s_off, M2_STRESS_BYTES, &slot);
        if (err != ESP_OK) break;
        m2_log_resident(part, "stress bank", err, slot, s_off, M2_STRESS_BYTES);
        stress++;
        s_off += M2_STRESS_STRIDE;
    }
    size_t free_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGE(TAG, "m2: stress bank %u refused (%s): need %u more, art holds %u of %u budgeted; psram %u.%u MB",
             stress + 1u, esp_err_to_name(err),
             (unsigned)art_bank_pages_for(M2_STRESS_BYTES),
             (unsigned)art_bank_mmu_pages(), (unsigned)ORB_ART_PAGE_BUDGET,
             M2_MB_U(free_p), M2_MB_T(free_p));
    if (err != ESP_ERR_NO_MEM)
        ESP_LOGE(TAG, "m2: refused for a reason that is not the page budget — check the table and partition bounds");

    /* Then single pages, unaccounted, until the flash MMU's own allocator
     * refuses: live art pages at that instant is the table's answer, and the
     * distance from the budget's prediction is the error bar on
     * ORB_ART_IMAGE_PAGES. One page at a time because a big contiguous map
     * could refuse earlier for want of contiguity, which is a different
     * question than the budget's. DISTINCT offsets because the flash mmap
     * layer may fold re-requests of the same physical page into one set of
     * entries — the run that hammered offset 0 "succeeded" 256 times without
     * billing a second region, which measures nothing. */
    const unsigned n_pages = (unsigned)(part->size / ORB_ART_PAGE_SIZE);
    static const void *keep_map[ORB_ART_DROM_PAGES];
    static esp_partition_mmap_handle_t keep_h[ORB_ART_DROM_PAGES];
    unsigned pages_raw = 0, dupes = 0;
    esp_err_t rerr = ESP_OK;
    while (pages_raw < n_pages && pages_raw < ORB_ART_DROM_PAGES) {
        const void *m = NULL;
        esp_partition_mmap_handle_t h = 0;
        rerr = esp_partition_mmap(part, (size_t)pages_raw * ORB_ART_PAGE_SIZE,
                                  ORB_ART_PAGE_SIZE, ESP_PARTITION_MMAP_DATA,
                                  &m, &h);
        if (rerr != ESP_OK) break;
        /* Folding detector: page 0 is the primary's own page, and the raw
         * offsets repeat nothing else in this loop — a returned pointer that
         * matches an earlier one or the primary base proves the mmap layer
         * folded the request, and every "kept" count after that is soft. */
        if (m == art_bank_ptr()) dupes++;
        for (unsigned j = 0; j < pages_raw; j++)
            if (keep_map[j] == m) { dupes++; break; }
        keep_map[pages_raw] = m;
        keep_h[pages_raw]   = h;
        pages_raw++;
    }
    free_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* Read one byte through the last kept raw mapping: the kept handles' only
     * job is keeping those pages resident, and this read is the proof the
     * saturated window still serves pixels — which is what the dial is about
     * to do for the next many hours. */
    unsigned sat_byte = 0xff;
    if (pages_raw > 0 && keep_h[pages_raw - 1] != 0)
        sat_byte = ((volatile const uint8_t *)keep_map[pages_raw - 1])[0];
    ESP_LOGE(TAG, "m2: raw distinct-page loop %s at %u of %u (%u returned a folded pointer): %u art pages live, %u pages summed over all regions (overlaps double-count, so this bounds no window), psram %u.%u MB; last kept raw page reads 0x%02x",
             rerr == ESP_OK ? "mapped the whole partition without a refusal" : "was refused",
             pages_raw, n_pages, dupes, (unsigned)art_bank_mmu_pages(),
             (unsigned)(art_bank_mmu_pages() + pages_raw + ORB_ART_IMAGE_PAGES),
             (unsigned)ORB_ART_DROM_PAGES, M2_MB_U(free_p), M2_MB_T(free_p), sat_byte);

    ESP_LOGI(TAG, "m2: leaving %u banks mapped (%u art pages accounted) for the dial — its fps lines that follow, under %u banks plus %u raw pages, are the residency verdict",
             (unsigned)orb_art_resident_count(), (unsigned)art_bank_mmu_pages(),
             (unsigned)orb_art_resident_count(), pages_raw);
}
#endif /* ORB_ART_M2_PROBE */

/* ---- suspend/resume reclaim-latency probe (docs/app-platform-plan.md §6) ---
 * What the release-vs-carry decision actually costs on the glass. A suspend
 * calls art_bank_unmap_extra() (one esp_partition_munmap) and a resume calls
 * art_bank_map_extra() (one esp_partition_mmap); carrying a parked bank skips
 * both and holds its pages against ORB_ART_PAGE_BUDGET instead. The dial never
 * re-reads 2.6 MB here — mmap installs the bank's page count into the SPI-flash
 * cache MMU table and does not copy, so the cost scales with pages, not bytes,
 * and no CRC runs in the shipping path. This measures that op directly.
 *
 * It maps WITHOUT a prior orb_art_reserve(), exactly as app_art_claim() does on a
 * re-claim: art_consume_reservation() is then a documented no-op (no booking
 * matches), so the timed span is the re-claim itself, nothing extra. Each bank is
 * fully unmapped between samples so every map is a cold esp_partition_mmap of the
 * same range — the true steady-state shape of the carousel switched back to.
 * The banks are the real artifacts on this bench, so the us/page figure the
 * ruling cites is measured, not an abstraction. */
#if ORB_ART_LAT_PROBE

#define LAT_ITERS  31u    /* odd, so the median is a real sampled value */
/* 64 KB-aligned offsets clear of the primary at 0. The assets partition is 8 MB. */
#define LAT_BANK_OFF_SMALL  0x00410000u
#define LAT_BANK_OFF_FLAG   0x00100000u
/* The av-kit primary (18 pages), the D10 clock default (41 pages), the kit3
 * flagship (59 pages) — the three bank sizes a shipped clock app can actually be. */
#define LAT_BYTES_PRIMARY   1173563u
#define LAT_BYTES_D10       2650159u
#define LAT_BYTES_KIT3      3858874u

static uint64_t s_lat_map[LAT_ITERS];
static uint64_t s_lat_unm[LAT_ITERS];

/* Full ascending sort of the n samples: only the ordering statistics the log
 * prints (min, median, max) are wanted, and a 31-element sort is cheap here. */
static void lat_sort(uint64_t *a, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            if (a[j] < a[i]) { uint64_t t = a[i]; a[i] = a[j]; a[j] = t; }
}

/* One bank, LAT_ITERS timed map/unmap pairs. The reclaim (map_extra) and the
 * release (unmap_extra) are timed and reported separately, because they are the
 * two distinct operations the release-vs-carry ruling turns on and off. */
static void lat_bench_bank(const esp_partition_t *part, size_t off, size_t bytes,
                           const char *label)
{
    const unsigned pages = (unsigned)art_bank_pages_for(bytes);
    if (off + bytes > part->size) {
        ESP_LOGE(TAG, "lat: %s bank %u B at 0x%x exceeds the %u B partition — skipped",
                 label, (unsigned)bytes, (unsigned)off, (unsigned)part->size);
        return;
    }

    /* Warm the slot once outside the samples so the first cold map — which can
     * miss a freed MMU region — is not mistaken for steady state. */
    int slot = -1;
    esp_err_t err = art_bank_map_extra(off, bytes, &slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lat: %s map refused (%s) at off 0x%x %u B — nothing to time",
                 label, esp_err_to_name(err), (unsigned)off, (unsigned)bytes);
        return;
    }
    art_bank_unmap_extra(slot);

    uint64_t map_min = ~0ULL, map_max = 0, map_sum = 0;
    uint64_t unm_min = ~0ULL, unm_max = 0, unm_sum = 0;
    unsigned n = 0;
    for (unsigned it = 0; it < LAT_ITERS; it++) {
        slot = -1;
        const uint64_t t0 = esp_timer_get_time();
        err = art_bank_map_extra(off, bytes, &slot);
        const uint64_t t1 = esp_timer_get_time();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "lat: %s map refused mid-sweep (%s) at iter %u",
                     label, esp_err_to_name(err), it);
            break;
        }
        art_bank_unmap_extra(slot);
        const uint64_t t2 = esp_timer_get_time();
        const uint64_t map_us = t1 - t0;
        const uint64_t unm_us = t2 - t1;
        s_lat_map[n] = map_us;
        s_lat_unm[n] = unm_us;
        n++;
        if (map_us < map_min) map_min = map_us;
        if (map_us > map_max) map_max = map_us;
        map_sum += map_us;
        if (unm_us < unm_min) unm_min = unm_us;
        if (unm_us > unm_max) unm_max = unm_us;
        unm_sum += unm_us;
    }
    if (n == 0) {
        ESP_LOGE(TAG, "lat: %s produced no samples", label);
        return;
    }
    lat_sort(s_lat_map, n);
    lat_sort(s_lat_unm, n);
    const uint64_t map_med = s_lat_map[n / 2];
    const uint64_t unm_med = s_lat_unm[n / 2];
    ESP_LOGI(TAG, "lat: %-6s %3u pages %5u KB | RECLAIM map_extra   min %6llu median %6llu max %6llu us  (~%llu us/page median)",
             label, pages, (unsigned)(bytes / 1024),
             (unsigned long long)map_min, (unsigned long long)map_med, (unsigned long long)map_max,
             (unsigned long long)(map_med / (pages ? pages : 1)));
    ESP_LOGI(TAG, "lat: %-6s %3u pages %5u KB | RELEASE unmap_extra min %6llu median %6llu max %6llu us  (mean %llu us)",
             label, pages, (unsigned)(bytes / 1024),
             (unsigned long long)unm_min, (unsigned long long)unm_med, (unsigned long long)unm_max,
             (unsigned long long)(unm_sum / n));
    ESP_LOGI(TAG, "lat: %-6s one switch (reclaim+release pair): ~%llu us median",
             label, (unsigned long long)(map_med + unm_med));
}

void art_bank_lat_probe(void)
{
    const esp_partition_t *part = art_bank_partition();
    if (!part) {
        ESP_LOGE(TAG, "lat: no \"%s\" partition — flash the partition table first",
                 ART_BANK_PARTITION_NAME);
        return;
    }
    ESP_LOGI(TAG, "lat: start — primary %u B (%u pages, %s); budget %u of %u pages; %u iters/bank",
             (unsigned)art_bank_bytes(), (unsigned)art_bank_pages_for(art_bank_bytes()),
             art_bank_ready() ? "resident" : "MAPPING FAILED, measure degraded",
             (unsigned)ORB_ART_PAGE_BUDGET, (unsigned)ORB_ART_DROM_PAGES, LAT_ITERS);
    lat_bench_bank(part, LAT_BANK_OFF_FLAG,  LAT_BYTES_KIT3,    "kit3");
    lat_bench_bank(part, LAT_BANK_OFF_FLAG,  LAT_BYTES_D10,     "d10");
    lat_bench_bank(part, LAT_BANK_OFF_SMALL, LAT_BYTES_PRIMARY, "avkit");
    ESP_LOGI(TAG, "lat: done — %u banks accounted (%u art pages; every lat bank unmapped, primary intact)",
             (unsigned)orb_art_resident_count(), (unsigned)art_bank_mmu_pages());
}
#endif /* ORB_ART_LAT_PROBE */

/* ---- M3 bench probe: the overflow bank, mechanism A vs mechanism B (§6 M3)
 * The bank is the third flagship — kit3 bytes, flashed at the bench offset
 * M3_BANK_OFF below, re-flashed before each campaign (a stale or stolen region
 * is caught by the CRC gate before a single number is printed). One build
 * measures one arm: ORB_ART_MECH_B decides at compile time, everything else
 * about the two builds is identical bytes.
 *
 * What is timed, per arm:
 *   A — release (unmap_extra), cold reclaim (map_extra), and the FIRST full-
 *       page commit through the fresh map. The last term is why M3 is not
 *       simply the LAT probe re-run: the LAT numbers priced the MMU table write
 *       (µs, scales with pages), while the compositor's first read after a
 *       remap pays the flash-cache miss storm the plan's "one full-page commit
 *       + fresh mmap" line means. Expect this to dominate the switch.
 *   B — the boot-time arena copy (once, priced as a boot cost with PSRAM free
 *       before/after — the §3.3 gate's real numbers), then the same pass with
 *       the MMU ops reduced to what the ABI actually calls: a same-offset
 *       map_extra that finds the entry already covered (a no-op lookup) and a
 *       suspend that releases nothing. The first-commit pass repeats against
 *       the arena: 3.86 MB against a 32 KB cache means every block reads cold
 *       PSRAM each pass, which is the steady state of an app paging through a
 *       bank that never fits the cache — the same regime as A minus the map.
 *
 * The blend runs through gfx_blend_px() at alpha 128 — the real mix_16_16 —
 * and the block sources are one per 64 KB page band with a per-pass stride, so
 * a pass touches every page of the bank: half-read from cache measures nothing
 * (the same all-pages discipline the header's comment promises). */
#if ORB_ART_M3_PROBE

#include "orb_gfx.h"            /* gfx_blend_px(): the very blend the layers use */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"      /* vTaskDelay between passes: the boot task yields */

#define M3_ITERS      31u               /* odd, like the lat probe: the median is a sample */
/* The bench bank's home: orb_theme's SLOT_B_ART (SLOT_B_HDR 0x400000 +
 * SLOT_ART_OFF 0x10000 = partition-relative; flash 0x830000), 64 KB aligned,
 * inside slot B's 4 MB cap and ending 0x41800 B clear of www. Slot B has never
 * been live on this board (install_target() writes it only on an explicit
 * install), so its art bytes are free for a bench fixture — re-run
 * /tmp/m3_bankwrite.sh to restore the bank before a campaign. The FIRST
 * placement here was 0x100000, BELOW the baked bank's end (0x11E83B): the
 * bench write clobbered the bank's last ~122 KB and the dial rendered corrupt
 * while art_ready stayed true — the ship path checks header geometry only,
 * never a whole-bank CRC (§14's incident). The probe's ground-truth gate below
 * is what makes a stolen or stale bench region refuse to measure. */
#define M3_BANK_OFF   0x00410000u
#define M3_BANK_BYTES 3858874u          /* the kit3 flagship — the flashed bench bank */
#define M3_BLK        60u               /* the DISPLAY_BENCH block, same 3600-px footprint */
#define M3_BLK_PX     (M3_BLK * M3_BLK)
#define M3_BLOCKS     60u               /* ~one full page of blended px per pass */
#define M3_ALPHA      128               /* the real mix; 255 prices the wrong op (.clinerules) */
#define M3_BLK_BYTES  (M3_BLK_PX * 2u)  /* bytes of bank read per block (colour window only) */

static uint64_t s_m3_rls[M3_ITERS], s_m3_rcl[M3_ITERS], s_m3_fc[M3_ITERS];
static const uint8_t *s_m3_plane;
static size_t s_m3_bytes;
static uint16_t *s_m3_dst;          /* the blend's dst page-equivalent (PSRAM) */

const uint8_t *art_bank_m3_plane(void) { return s_m3_plane; }
size_t art_bank_m3_bytes(void)         { return s_m3_bytes; }
bool art_bank_m3_is_psram(void)        { return ORB_ART_MECH_B; }

static void m3_sort(uint64_t *a, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            if (a[j] < a[i]) { uint64_t t = a[i]; a[i] = a[j]; a[j] = t; }
}

/* One op's report, same shape as the lat probe's so §14's table reads evenly. */
static void m3_stats(const char *arm, const char *op, const uint64_t *a, unsigned n)
{
    if (!n) { ESP_LOGE(TAG, "m3: %s %s produced no samples", arm, op); return; }
    uint64_t min = a[0], max = a[0], sum = 0;
    for (unsigned i = 0; i < n; i++) {
        if (a[i] < min) min = a[i];
        if (a[i] > max) max = a[i];
        sum += a[i];
    }
    ESP_LOGI(TAG, "m3: %s %-16s min %8llu median %8llu max %8llu us (mean %llu us)",
             arm, op,
             (unsigned long long)min, (unsigned long long)a[n / 2],
             (unsigned long long)max, (unsigned long long)(sum / n));
}

/* One first-commit pass: M3_BLOCKS 60x60 blocks, each sourced from a distinct
 * 64 KB band of the bank (plus a per-pass rotation so consecutive passes do
 * not re-measure one cache state), blended into the scratch dst through the
 * shipping gfx_blend_px at the real alpha. dst is written AND read (the mix
 * reads its destination), mirroring a commit over a stale canvas. */
static uint64_t m3_first_commit(const uint8_t *plane, size_t bytes, unsigned pass)
{
    const size_t band = bytes / M3_BLOCKS;            /* > M3_BLK_BYTES on any flagship */
    const uint64_t t0 = esp_timer_get_time();
    for (unsigned b = 0; b < M3_BLOCKS; b++) {
        size_t off = (size_t)b * band
                   + (size_t)(((pass * 0x1150u + b * 77u) % (band - M3_BLK_BYTES)) & ~1u);
        const uint16_t *src = (const uint16_t *)(plane + off);
        uint16_t *d = s_m3_dst + (size_t)b * M3_BLK_PX;
        for (unsigned i = 0; i < M3_BLK_PX; i++)
            gfx_blend_px(&d[i], src[i], M3_ALPHA, 0);
    }
    return esp_timer_get_time() - t0;
}

/* ---- the MMU's true refusal point (§11's third finding, handed to M3) ------
 * M2 mapped every distinct 64 KB page of the 8 MB assets partition — all 128,
 * one folded to the primary's own base by the mmap layer's dedup of identical
 * physical requests — and the allocator never refused. That bounds accepted
 * regions at 128+ and nowhere near decides whether ORB_ART_PAGE_BUDGET (234)
 * could rise: the partition ran out before the MMU did. The hunt goes further
 * by going WIDER, not denser — only physically DISTINCT pages are countable
 * (an earlier version walked assets at 8 KB stride and the mmap cache folded
 * every overlapping request onto an existing pointer, measuring nothing), and
 * the board has four 64 KB-aligned partitions: assets (128 pages), www (20),
 * ota_0 (32) and ota_1 (32) — 212 distinct regions on top of the ~99 pages
 * already resident for the image, the primary and the bench bank, past the
 * 256-page window the budget arithmetic claims. First refusal by name (and
 * where) is the number §11 asked for; if the hunt still exhausts, that too is
 * reported as the bound it is. Every mapping is UNMAPPED on the way out:
 * unlike M2's residency leave-behind, this probe must hand the dial back the
 * shipping MMU table, so the fps lines that follow measure the arm under test
 * and nothing else. */
#define M3_HUNT_MAX 300u         /* 212 distinct pages available; refuse-cap beyond that */

/* PSRAM, allocated at hunt entry — see m2_stage_buf's note on why boot-probe
 * scratch earns no internal .bss on this heap. */
static const void **s_hunt_map;
static esp_partition_mmap_handle_t *s_hunt_h;
static unsigned s_hunt_n;

static void m3_hunt_unmap_all(void)
{
    for (unsigned i = 0; i < s_hunt_n; i++)
        if (s_hunt_h[i]) { esp_partition_munmap(s_hunt_h[i]); s_hunt_h[i] = 0; }
    s_hunt_n = 0;
}

/** Returns 0 if a genuine refusal was caught (and logged with its where),
 *  1 if the hunt exhausted without one. */
static int m3_refusal_hunt(const esp_partition_t *assets)
{
    /* Fold detection also counts the boot's own mappings — a page that
     * returns the primary's base is dedup, not a granted region (M2's
     * lesson, in both directions). */
    const void *residents[ART_BANK_EXTRA_MAX + 2];
    unsigned n_res = 0;
    residents[n_res++] = art_bank_ptr();
    residents[n_res++] = s_m3_plane;
    for (unsigned i = 0; i < ART_BANK_EXTRA_MAX; i++)
        residents[n_res++] = art_bank_extra_ptr((int)i);

    s_hunt_n = 0;
    s_hunt_map = (const void **)heap_caps_malloc(M3_HUNT_MAX * sizeof s_hunt_map[0],
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_hunt_h = (esp_partition_mmap_handle_t *)heap_caps_malloc(
        M3_HUNT_MAX * sizeof s_hunt_h[0], MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_hunt_map || !s_hunt_h) {
        ESP_LOGE(TAG, "m3: hunt skipped — no PSRAM for the handle arrays");
        return 1;
    }
    unsigned fresh = 0, folded = 0, attempts = 0;
    esp_err_t rerr = ESP_OK;
    const char *where = "?"; size_t woff = 0;
    /* Every 64 KB-aligned partition the table can give a data map: the two
     * data banks and BOTH app slots (mmap of an app image's bytes as DATA is
     * legal — the first hunt pass searched them as TYPE_DATA and skipped
     * them, which is why its legs logged "no ota_0 partition" on a board that
     * boots from one). */
    static const struct { const char *label; esp_partition_type_t type; } hunt_legs[] = {
        { NULL,       ESP_PARTITION_TYPE_DATA },  /* NULL label = the assets arg */
        { "www",      ESP_PARTITION_TYPE_DATA },
        { "ota_0",    ESP_PARTITION_TYPE_APP },
        { "ota_1",    ESP_PARTITION_TYPE_APP },
    };

    for (unsigned pi = 0; pi < 4 && rerr == ESP_OK; pi++) {
        const esp_partition_t *part = hunt_legs[pi].label
            ? esp_partition_find_first(hunt_legs[pi].type,
                                       ESP_PARTITION_SUBTYPE_ANY, hunt_legs[pi].label)
            : assets;
        if (!part) {
            ESP_LOGW(TAG, "m3: hunt: no \"%s\" partition on this table — leg skipped",
                     hunt_legs[pi].label ? hunt_legs[pi].label : "assets");
            continue;
        }
        where = part->label;
        for (size_t off = 0; off + ORB_ART_PAGE_SIZE <= part->size; off += ORB_ART_PAGE_SIZE) {
            const void *m = NULL;
            esp_partition_mmap_handle_t h = 0;
            woff = off;
            rerr = esp_partition_mmap(part, off, ORB_ART_PAGE_SIZE,
                                      ESP_PARTITION_MMAP_DATA, &m, &h);
            attempts++;
            if (rerr != ESP_OK) break;
            if (m == NULL) { esp_partition_munmap(h); rerr = ESP_FAIL; break; }
            bool dup = false;
            for (unsigned i = 0; i < n_res && !dup; i++) dup = (residents[i] == m);
            for (unsigned i = 0; i < s_hunt_n && !dup; i++) dup = (s_hunt_map[i] == m);
            if (dup) folded++;
            else fresh++;
            if (s_hunt_n < M3_HUNT_MAX) { s_hunt_map[s_hunt_n] = m; s_hunt_h[s_hunt_n] = h; s_hunt_n++; }
            else esp_partition_munmap(h);              /* cap: state held either way */
            if ((attempts % 64u) == 0) vTaskDelay(1);
        }
    }

    const uint32_t internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const unsigned kept = s_hunt_n;
    m3_hunt_unmap_all();
    if (rerr == ESP_OK) {
        ESP_LOGE(TAG, "m3: refusal-hunt EXHAUSTED without a refusal — %u attempts, %u fresh "
                      "(%u folded), never refused; the ceiling is above %u distinct regions "
                      "and budget 234 remains a contract, not a measurement. internal free %u B",
                 attempts, fresh, folded, fresh, (unsigned)internal_free);
        return 1;
    }
    ESP_LOGI(TAG, "m3: refusal-hunt: \"%s\" @0x%x REFUSED with %s — attempt %u, %u fresh regions "
                  "(%u folded) live when it happened (plus %u B internal free). That is the "
                  "allocator's real page ceiling; all %u hunt mappings unmapped again.",
             where, (unsigned)woff, esp_err_to_name(rerr), attempts, fresh, folded,
             (unsigned)internal_free, kept);
    return 0;
}

void art_bank_m3_probe(void)
{
    const esp_partition_t *part = art_bank_partition();
    if (!part) { ESP_LOGE(TAG, "m3: no \"assets\" partition — nothing to measure"); return; }

    const char *arm = ORB_ART_MECH_B ? "B(arena)" : "A(re-map)";
    /* Erased-flash gate FIRST: measuring on 0xFF reads is how a forgotten bench
     * write becomes a fake number that still looks like the other arm. The
     * printed fix is the one-shot esptool write, not a re-flash of the app. */
    uint8_t *head = m2_stage_buf();   /* PSRAM, shared with the CRC loop — never both at once */
    if (!head) { ESP_LOGE(TAG, "m3: no 4 KB PSRAM stage — measure nothing"); return; }
    if (esp_partition_read(part, M3_BANK_OFF, head, 4096) != ESP_OK) {
        ESP_LOGE(TAG, "m3: cannot read bench bank at 0x%x — measure nothing", (unsigned)M3_BANK_OFF);
        return;
    }
    bool ff = true, zero = true;
    for (unsigned i = 0; i < 4096; i++) { if (head[i] != 0xFF) ff = false; if (head[i]) zero = false; }
    if (ff || zero) {
        ESP_LOGE(TAG, "m3: bench bank at 0x%x is ERASED — re-run /tmp/m3_bankwrite.sh "
                      "(write_flash 0x830000 /tmp/m3-kit3.bin, = assets+M3_BANK_OFF). "
                      "Measuring nothing.", (unsigned)M3_BANK_OFF);
        return;
    }

    s_m3_dst = (uint16_t *)heap_caps_malloc((size_t)M3_BLOCKS * M3_BLK_PX * sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_m3_dst) { ESP_LOGE(TAG, "m3: no %u B PSRAM scratch — measure nothing",
                              (unsigned)(M3_BLOCKS * M3_BLK_PX * 2u)); return; }

    ESP_LOGI(TAG, "m3: arm %s, bank %u B @0x%x (%u pages), iters %u, primary %u B %s, "
                  "free PSRAM %u B, margin gate %u B",
             arm, (unsigned)M3_BANK_BYTES, (unsigned)M3_BANK_OFF,
             (unsigned)art_bank_pages_for(M3_BANK_BYTES), (unsigned)M3_ITERS,
             (unsigned)art_bank_bytes(), art_bank_ready() ? "resident" : "NOT RESIDENT",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)ORB_ART_ARENA_MARGIN);

    /* Warm-up acquire (never sampled). For arm B this call IS the boot-time
     * arena copy — timed once and printed as the boot cost §3.3 prices. */
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const uint64_t t_acq0 = esp_timer_get_time();
    int slot = -1;
    esp_err_t err = art_bank_map_extra(M3_BANK_OFF, M3_BANK_BYTES, &slot);
    const uint64_t boot_us = esp_timer_get_time() - t_acq0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "m3: first map refused (%s) — measure nothing", esp_err_to_name(err));
        heap_caps_free(s_m3_dst); s_m3_dst = NULL;
        return;
    }
    if (ORB_ART_MECH_B)
        ESP_LOGI(TAG, "m3: %s BOOT COST: arena fill of %u B in %llu us; free PSRAM "
                      "%u → %u B (margin gate %u B)", arm, (unsigned)M3_BANK_BYTES,
                 (unsigned long long)boot_us, (unsigned)psram_before,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)ORB_ART_ARENA_MARGIN);

    const uint8_t *plane = art_bank_extra_ptr(slot);
    if (!m2_crc_verified(part, plane, M3_BANK_OFF, M3_BANK_BYTES)) {
        ESP_LOGE(TAG, "m3: CRC MISMATCH through the %s at 0x%x — the bytes measured "
                      "would not be the bytes painted. Measure nothing.",
                 arm, (unsigned)M3_BANK_OFF);
        art_bank_unmap_extra(slot);
        heap_caps_free(s_m3_dst); s_m3_dst = NULL;
        return;
    }
    /* Ground truth, not just self-consistency: m2_crc_verified proves map ==
     * read-path over WHATEVER bytes sit there, so a region reclaimed by a real
     * theme install (slot B art is also install_target's destination) between
     * bench runs would measure as if it were kit3. The 2026-10-12 incident had
     * the same shape from the other side: self-consistent bytes, wrong bank.
     * Compare the whole plane against the flashed file's crc32_le — recompute
     * from /tmp/m3-kit3.bin with:
     *   python3 -c "import zlib;print(hex(zlib.crc32(open(...,'rb').read())))" */
    if (esp_rom_crc32_le(0, plane, (uint32_t)M3_BANK_BYTES) != 0x59801b57u) {
        ESP_LOGE(TAG, "m3: bank at 0x%x is NOT the flashed kit3 (plane crc32_le "
                      "0x%08x != 0x59801b57) — re-run /tmp/m3_bankwrite.sh. Measure nothing.",
                 (unsigned)M3_BANK_OFF,
                 (unsigned)esp_rom_crc32_le(0, plane, (uint32_t)M3_BANK_BYTES));
        art_bank_unmap_extra(slot);
        heap_caps_free(s_m3_dst); s_m3_dst = NULL;
        return;
    }
    (void)m3_first_commit(plane, M3_BANK_BYTES, 0);   /* warm the pattern once */

    /* The sweep. A pays a real release + a cold reclaim + the first read
     * through the fresh map, every iteration — the shape of the carousel
     * switched away and back. B must NOT free its arena per iteration: the
     * re-fill would re-bill the 3.86 MB copy inside the reclaim column and
     * corrupt the final residency. B's shipping shape is a carrying host, so
     * its switch is exactly the two ABI calls that remain live under MECH_B:
     * the suspend that releases nothing this probe can see, and the resume's
     * covered-entry lookup. The release-for-free variant of B (a non-carrying
     * arena host) is the single unmap+re-fill pair timed below, OUTSIDE the
     * samples, in the boot-cost line's shape. */
    unsigned n = 0;
    for (unsigned it = 0; it < M3_ITERS; it++) {
        uint64_t rls, rcl;
#if ORB_ART_MECH_B
        int s2 = -1;
        rls = 0;                                     /* carrying host: nothing to release */
        const uint64_t m0 = esp_timer_get_time();
        err = art_bank_map_extra(M3_BANK_OFF, M3_BANK_BYTES, &s2);   /* covered → no-op */
        rcl = esp_timer_get_time() - m0;
        slot = s2;
#else
        const uint64_t u0 = esp_timer_get_time();
        art_bank_unmap_extra(slot);
        const uint64_t u1 = esp_timer_get_time();
        rls = u1 - u0;
        slot = -1;
        const uint64_t m0 = esp_timer_get_time();
        err = art_bank_map_extra(M3_BANK_OFF, M3_BANK_BYTES, &slot); /* cold reclaim */
        rcl = esp_timer_get_time() - m0;
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "m3: re-acquire failed at iter %u (%s) — sweep truncated",
                     it, esp_err_to_name(err));
            break;
        }
        plane = art_bank_extra_ptr(slot);
        s_m3_rls[n] = rls;
        s_m3_rcl[n] = rcl;
        s_m3_fc[n]  = m3_first_commit(plane, M3_BANK_BYTES, it + 1);
        n++;
        vTaskDelay(1);
    }
    if (!n) { ESP_LOGE(TAG, "m3: the sweep produced no samples — nothing to report");
              heap_caps_free(s_m3_dst); s_m3_dst = NULL; return; }

    m3_stats(arm, "release(suspend)", s_m3_rls, n);
    m3_stats(arm, "reclaim(resume)",  s_m3_rcl, n);
    m3_stats(arm, "first-commit",     s_m3_fc, n);
#if ORB_ART_MECH_B
    /* The B variant the release-vs-carry ruling also needs: what suspending
     * an ARENA app would cost if the host did NOT carry it — one real unmap
     * (heap_caps_free) plus the re-fill that follows. Once, outside the
     * samples: it is the price of the OTHER carrier choice, not the switch. */
    {
        const uint64_t f0 = esp_timer_get_time();
        art_bank_unmap_extra(slot);
        const uint64_t f1 = esp_timer_get_time();
        slot = -1;
        if (art_bank_map_extra(M3_BANK_OFF, M3_BANK_BYTES, &slot) == ESP_OK) {
            const uint64_t f2 = esp_timer_get_time();
            plane = art_bank_extra_ptr(slot);
            ESP_LOGI(TAG, "m3: %s release-for-free variant: unmap %llu us, arena re-fill %llu us "
                          "(boot-cost shape, one pair — not a switch sample)",
                     arm, (unsigned long long)(f1 - f0), (unsigned long long)(f2 - f1));
        } else {
            ESP_LOGE(TAG, "m3: variant re-fill FAILED — arena could not be re-taken; "
                          "the dial follows without the bank (degraded pass)");
            slot = -1;
        }
    }
#endif
    if (n) {
        /* The plan asks about the SWITCH, so price it from matched samples —
         * summing three medians would quietly invent an independence the
         * iterations do not have. */
        static uint64_t s_tot[M3_ITERS];
        for (unsigned i = 0; i < n; i++) s_tot[i] = s_m3_rls[i] + s_m3_rcl[i] + s_m3_fc[i];
        m3_sort(s_tot, n);
        m3_stats(arm, "SWITCH TOTAL", s_tot, n);
    }

    /* Re-acquire once more if the truncated sweep left the bank down, then
     * LEAVE IT RESIDENT: the dial's fps lines later in this boot log — and
     * the DISPLAY_BENCH blocks, which clock_view points at art_bank_m3_plane()
     * when armed — are then the rotation steady state of an app reading this
     * bank, the M2 residency convention. */
    if (slot < 0 && art_bank_map_extra(M3_BANK_OFF, M3_BANK_BYTES, &slot) != ESP_OK)
        ESP_LOGE(TAG, "m3: FINAL map refused — the dial follows WITHOUT the bank; "
                      "its fps lines are a degraded pass, not a rotation measure");
    s_m3_plane = art_bank_extra_ptr(slot);
    s_m3_bytes = M3_BANK_BYTES;
    (void)m3_refusal_hunt(part);   /* logs its own verdict; unmaps everything it mapped */
    ESP_LOGI(TAG, "m3: done — %u samples, bank LEFT RESIDENT %u B via %s. Plane for "
                  "DISPLAY_BENCH blocks: %s. Read against §12.7's M1 activate 63-87 ms.",
             (unsigned)n, (unsigned)M3_BANK_BYTES, arm, s_m3_plane ? "armed" : "NOT ARMED");
}
#endif /* ORB_ART_M3_PROBE */

