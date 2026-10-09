/**
 * art_bank.c — see art_bank.h for the contract and the rationale.
 */
#include "art_bank.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "art_bank";

/* Mapped for the process lifetime on purpose; never unmapped. See header. */
static const uint8_t              *s_map;
static size_t                      s_bytes;
static esp_partition_mmap_handle_t s_handle;
static const esp_partition_t      *s_part;

esp_err_t art_bank_init(size_t need_bytes)
{
    if (need_bytes == 0)
        return ESP_ERR_INVALID_ARG;

    if (s_map && s_bytes >= need_bytes)
        return ESP_OK;                    /* already mapped big enough */

    if (s_map) {
        /* A bigger theme than the one we mapped: drop the old map and redo it.
         * Rare (only a re-bake between boots grows the bank), and callers must
         * rebuild every cached LVGL descriptor afterwards. */
        ESP_LOGI(TAG, "re-mapping %u B -> %u B", (unsigned)s_bytes, (unsigned)need_bytes);
        esp_partition_munmap(s_handle);
        s_map    = NULL;
        s_bytes  = 0;
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

    if (need_bytes > s_part->size) {
        ESP_LOGE(TAG, "theme needs %u B but \"%s\" holds only %u B — re-bake smaller",
                 (unsigned)need_bytes, ART_BANK_PARTITION_NAME, (unsigned)s_part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    const void *map = NULL;
    esp_err_t err = esp_partition_mmap(s_part, 0, need_bytes, ESP_PARTITION_MMAP_DATA,
                                       &map, &s_handle);
    if (err != ESP_OK) {
        /* Almost always the DROM window being exhausted by the app's own
         * .rodata, not a shortage of heap — nothing here allocates. */
        ESP_LOGE(TAG, "mmap %u B from \"%s\" @0x%x failed: %s",
                 (unsigned)need_bytes, s_part->label, (unsigned)s_part->address,
                 esp_err_to_name(err));
        return err;
    }

    s_map   = (const uint8_t *)map;
    s_bytes = need_bytes;

    ESP_LOGI(TAG, "bank \"%s\" @0x%x, %u B mapped of %u B (theme blob %u B)",
             s_part->label, (unsigned)s_part->address, (unsigned)s_bytes,
             (unsigned)s_part->size, (unsigned)need_bytes);
    return ESP_OK;
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
