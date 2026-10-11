/**
 * art_bank.c — see art_bank.h for the contract and the rationale.
 */
#include "art_bank.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "soc/soc_caps.h"   /* SOC_MMU_PAGE_SIZE: what a resident bank costs the DROM window */

static const char *TAG = "art_bank";

/* Mapped for the process lifetime on purpose; never unmapped. See header. */
static const uint8_t              *s_map;
static size_t                      s_bytes;
static size_t                      s_off;      /* partition offset s_map points at */
static esp_partition_mmap_handle_t s_handle;
static const esp_partition_t      *s_part;

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

/* ---- residency accounting (P1) --------------------------------------------
 * The whole app platform rests on one arithmetic fact: every resident bank costs
 * ceil(bytes / MMU page) pages out of the 256 the S3 gives DROM, so "two flagship
 * banks plus the app's own .rodata" is decidable on paper before it is decided on
 * the bench (docs/app-platform-plan.md §3). Until the loader maps secondary banks,
 * orb_art_reserve() accounts and never maps — that keeps the ABI compiling and
 * keeps /api/v1/status reporting a real number rather than an aspiration. */

#define ORB_ART_PAGE_SIZE   SOC_MMU_PAGE_SIZE      /* 64 KB on the S3 default */
#define ORB_ART_PAGE_ALIGN  ORB_ART_PAGE_SIZE      /* what mmap rounds each end to */

static size_t s_reserved_bytes;
static unsigned s_reserved_banks;

esp_err_t orb_art_reserve(size_t bytes)
{
    if (bytes == 0) return ESP_ERR_INVALID_SIZE;
    /* A ragged request costs a whole page at each end of the mapping, so the
     * slot layout is cut on 64 KB boundaries and this refuses anything else
     * instead of quietly eating the slack. */
    if (bytes % ORB_ART_PAGE_ALIGN) return ESP_ERR_INVALID_SIZE;
    s_reserved_bytes += bytes;
    s_reserved_banks++;
    ESP_LOGI(TAG, "reserve %u B (%u pages), %u reservations / %u B accounted",
             (unsigned)bytes, (unsigned)art_bank_pages_for(bytes),
             s_reserved_banks, (unsigned)s_reserved_bytes);
    return ESP_OK;
}

size_t art_bank_pages_for(size_t bytes)
{
    return (bytes + ORB_ART_PAGE_SIZE - 1) / ORB_ART_PAGE_SIZE;
}

size_t art_bank_mmu_pages(void)
{
    /* What the flash MMU is actually carrying for art right now: the live
     * mapping plus every accepted reservation, which are the same thing once
     * the loader starts honouring them. */
    return art_bank_pages_for(s_bytes) + art_bank_pages_for(s_reserved_bytes);
}

size_t orb_art_resident_count(void)
{
    return (s_map != NULL ? 1u : 0u) + s_reserved_banks;
}

size_t orb_art_reserved_bytes(void)
{
    return s_reserved_bytes;
}
