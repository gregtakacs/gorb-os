/**
 * art_bank.c — see art_bank.h for the contract and the rationale.
 */
#include "art_bank.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "soc/soc_caps.h"   /* SOC_MMU_PAGE_SIZE: what a resident bank costs the DROM window */

#if ORB_ART_M2_PROBE
#include "esp_rom_crc.h"    /* crc of a bank through the mapping vs through the read path */
#include "esp_heap_caps.h"  /* free PSRAM at the failing instant: mechanism B's arena cap */
#endif

static const char *TAG = "art_bank";

/* Mapped for the process lifetime on purpose; never unmapped. See header. */
static const uint8_t              *s_map;
static size_t                      s_bytes;
static size_t                      s_off;      /* partition offset s_map points at */
static esp_partition_mmap_handle_t s_handle;
static const esp_partition_t      *s_part;

/* Secondary banks (P2, §3.3 mechanism A): one entry per mapped bank, stable
 * slot indices, resident until art_bank_unmap_extra() empties one. Index i
 * keeps its mapping until explicitly emptied so an app's cached slot number
 * outlives its neighbours' churn. */
typedef struct {
    const uint8_t              *map;
    size_t                      bytes;
    size_t                      offset;   /* partition offset map points at */
    esp_partition_mmap_handle_t handle;
} art_extra_t;

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

esp_err_t art_bank_init(size_t offset, size_t need_bytes)
{
    if (need_bytes == 0)
        return ESP_ERR_INVALID_ARG;

    if (s_map && s_bytes >= need_bytes && s_off == offset)
        return ESP_OK;                    /* already mapped big enough */

    if (s_map) {
        /* A different theme than the one we mapped: drop the old map and redo
         * it. Rare (only a load from the card changes it, and that reboots
         * first), and callers must rebuild every cached LVGL descriptor
         * afterwards — which is exactly why orb_theme installs before
         * clock_view_init() rather than while the dial is running. */
        ESP_LOGI(TAG, "re-mapping @0x%x %u B -> %u B",
                 (unsigned)offset, (unsigned)s_bytes, (unsigned)need_bytes);
        esp_partition_munmap(s_handle);
        s_map   = NULL;
        s_bytes = 0;
        s_off   = 0;
        s_handle = 0;
    }

    /* By LABEL, never by index/subtype: the label is the stable identity, and
     * `assets` is a subtype-less data partition (gen_esp32part.py defaults an
     * empty subtype to `undefined` 0x06, the same value `www` carries). */
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY,
                                      ART_BANK_PARTITION_NAME);
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
    return s_map;
}

size_t art_bank_bytes(void)
{
    return s_bytes;
}

bool art_bank_ready(void)
{
    return s_map != NULL;
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
 * from a size report, not a reading of the live MMU table, until that line matches. */
#define ORB_ART_DROM_PAGES       256u
#define ORB_ART_IMAGE_PAGES      22u   /* .text 17 + .rodata/.appdesc 5, per idf.py size */
#define ORB_ART_PAGE_BUDGET      (ORB_ART_DROM_PAGES - ORB_ART_IMAGE_PAGES)

static unsigned s_reserve_refusals;

unsigned art_bank_extra_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ART_BANK_EXTRA_MAX; i++)
        if (s_extra[i].map) n++;
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
 * slots are cut on page boundaries. */
static size_t art_pages_accounted(void)
{
    size_t pages = art_bank_pages_for(s_bytes);
    for (unsigned i = 0; i < ART_BANK_EXTRA_MAX; i++)
        if (s_extra[i].map)
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
    return (s_map != NULL ? 1u : 0u) + art_bank_extra_count() + art_reservation_count();
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
        if (s_extra[i].map && s_extra[i].offset == offset) {
            if (s_extra[i].bytes >= need_bytes) {          /* already covered */
                if (out_slot) *out_slot = (int)i;
                return ESP_OK;
            }
            slot = (int)i;                                 /* grow in place */
            break;
        }
    if (slot < 0)
        for (unsigned i = 0; i < ART_BANK_EXTRA_MAX; i++)
            if (!s_extra[i].map) { slot = (int)i; break; }
    if (slot < 0) {
        ESP_LOGE(TAG, "extra map %u B @0x%x refused: all %u secondary slots hold banks",
                 (unsigned)need_bytes, (unsigned)offset, (unsigned)ART_BANK_EXTRA_MAX);
        return ESP_ERR_NOT_FOUND;
    }

    /* Budget on the running total, minus what this entry already pays for if it
     * is a re-grow — the same rule orb_art_reserve() applies, applied to a map
     * that arrived with no booking behind it. Checked before the munmap below,
     * so a refused request never disturbs what is already resident. */
    size_t want = art_pages_accounted() + art_bank_pages_for(need_bytes);
    if (s_extra[slot].map)
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

    if (s_extra[slot].map) {
        /* Re-grow: drop the old map first, as the primary's re-map does — the
         * caller owns the question of who was still reading it. */
        ESP_LOGI(TAG, "extra bank %d re-mapping @0x%x %u B -> %u B",
                 slot, (unsigned)offset, (unsigned)s_extra[slot].bytes,
                 (unsigned)need_bytes);
        esp_partition_munmap(s_extra[slot].handle);
        s_extra[slot] = (art_extra_t){ 0 };
    }

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
}


const uint8_t *art_bank_extra_ptr(int slot)
{
    if (slot < 0 || slot >= ART_BANK_EXTRA_MAX) return NULL;
    return s_extra[slot].map;
}

size_t art_bank_extra_bytes(int slot)
{
    if (slot < 0 || slot >= ART_BANK_EXTRA_MAX) return 0;
    return s_extra[slot].bytes;
}

esp_err_t art_bank_unmap_extra(int slot)
{
    if (slot < 0 || slot >= ART_BANK_EXTRA_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_extra[slot].map) return ESP_OK;                 /* already empty */
    /* The munmap voids every pointer into the region, so "which app just lost
     * its pixels" must be answerable from the boot log alone: silent unmaps are
     * how a garbage paint becomes unexplainable. */
    ESP_LOGI(TAG, "extra bank %d unmapped (%u B @0x%x freed, %u banks accounted)",
             slot, (unsigned)s_extra[slot].bytes, (unsigned)s_extra[slot].offset,
             (unsigned)(orb_art_resident_count() - 1u));
    esp_partition_munmap(s_extra[slot].handle);
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
#if ORB_ART_M2_PROBE

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

/* Static: the main task's stack would not thank us for 4 KB of scratch. */
static uint8_t s_m2_chunk[4096];

static bool m2_crc_verified(const esp_partition_t *part, const uint8_t *map,
                            size_t off, size_t bytes)
{
    const uint32_t crc_map = esp_rom_crc32_le(0, map, (uint32_t)bytes);
    uint32_t crc_read = 0;
    for (size_t done = 0; done < bytes; ) {
        size_t want = bytes - done;
        if (want > sizeof s_m2_chunk) want = sizeof s_m2_chunk;
        if (esp_partition_read(part, off + done, s_m2_chunk, want) != ESP_OK)
            return false;
        crc_read = esp_rom_crc32_le(crc_read, s_m2_chunk, (uint32_t)want);
        done += want;
    }
    return crc_map == crc_read;
}

#define M2_MB_U(free) (unsigned)((free) >> 20)
#define M2_MB_T(free) (unsigned)((((free) * 10) >> 20) % 10)

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

