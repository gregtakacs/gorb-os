/* The storage carry — contract in orb_layout_carry.h. */
#include "orb_layout_carry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "carry";

#define LAYOUT_B_STORAGE  0xD60000u   /* Layout B's `storage` row, now inside `assets` */
#define STORAGE_BYTES     0x6000u
#define NVS_PAGE          0x1000u

/* An NVS page header (nvs_page.hpp): state, sequence number, version, reserved, then a
 * CRC over the sequence number through the reserved bytes. */
#define NVS_STATE_ACTIVE  0xFFFFFFFEu
#define NVS_STATE_FULL    0xFFFFFFFCu
#define NVS_STATE_FREEING 0xFFFFFFF8u
#define NVS_STATE_BLANK   0xFFFFFFFFu
#define NVS_STATE_CORRUPT 0xFFFFFFF0u   /* dead pages IDF leaves in a live store */
#define NVS_STATE_INVALID 0x00000000u

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* True when the 24 KB at `off` in `p` is an NVS store: at least one page that holds
 * data with a header whose CRC checks, and every other page blank or dead. Old
 * app-slot bytes or theme pixels fail the second test long before the first could
 * pass by accident. */
static bool holds_nvs(const esp_partition_t *p, uint32_t off)
{
    int live = 0;
    for (uint32_t pg = 0; pg < STORAGE_BYTES; pg += NVS_PAGE) {
        uint8_t h[32];
        if (esp_partition_read(p, off + pg, h, sizeof h) != ESP_OK) return false;
        const uint32_t state = rd32(h);
        if (state == NVS_STATE_BLANK || state == NVS_STATE_CORRUPT || state == NVS_STATE_INVALID)
            continue;
        if (state != NVS_STATE_ACTIVE && state != NVS_STATE_FULL && state != NVS_STATE_FREEING)
            return false;
        if (esp_rom_crc32_le(0xFFFFFFFFu, h + 4, 24) != rd32(h + 28)) return false;
        live++;
    }
    return live > 0;
}

void orb_layout_carry_storage(void)
{
    const esp_partition_t *dst = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                          ESP_PARTITION_SUBTYPE_DATA_NVS, "storage");
    const esp_partition_t *src = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                          ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (!dst || !src || dst->address == LAYOUT_B_STORAGE || dst->size < STORAGE_BYTES)
        return;                                       /* not a Layout C table */
    if (LAYOUT_B_STORAGE < src->address ||
        LAYOUT_B_STORAGE + STORAGE_BYTES > src->address + src->size)
        return;
    const uint32_t from = LAYOUT_B_STORAGE - src->address;
    if (!holds_nvs(src, from))
        return;                                       /* nothing to carry, or carried */
    if (holds_nvs(dst, 0)) {
        /* Carried already, by hand (orb-A crossed before this file existed): the old
         * copy is stale, and left standing it would come back after a RESET ALL. */
        const esp_err_t e = esp_partition_erase_range(src, from, STORAGE_BYTES);
        ESP_LOGW(TAG, "erased the stale Layout B storage copy: %s", esp_err_to_name(e));
        return;
    }

    uint8_t *buf = heap_caps_malloc(STORAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc(STORAGE_BYTES, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "no memory to carry storage across the table change");
        return;
    }
    esp_err_t e = esp_partition_read(src, from, buf, STORAGE_BYTES);
    if (e == ESP_OK) e = esp_partition_erase_range(dst, 0, STORAGE_BYTES);
    if (e == ESP_OK) e = esp_partition_write(dst, 0, buf, STORAGE_BYTES);
    if (e == ESP_OK && !holds_nvs(dst, 0)) e = ESP_ERR_INVALID_CRC;
    /* The old copy goes only once the new one reads back as NVS: a cut between the two
     * leaves the source standing and the next boot carries again. Erasing it is what
     * keeps a later RESET ALL from resurrecting the credentials it wiped. */
    if (e == ESP_OK) e = esp_partition_erase_range(src, from, STORAGE_BYTES);
    heap_caps_free(buf);
    if (e == ESP_OK)
        ESP_LOGW(TAG, "carried storage from 0x%06x to 0x%06x (Layout B -> C)",
                 (unsigned)LAYOUT_B_STORAGE, (unsigned)dst->address);
    else
        ESP_LOGE(TAG, "storage carry failed: %s", esp_err_to_name(e));
}
