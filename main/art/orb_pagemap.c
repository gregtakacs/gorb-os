/* Scattered pages as one window — contract in orb_pagemap.h (theme-cache-plan §4.1). */
#include "orb_pagemap.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_mmu_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/cache_hal.h"
#include "hal/mmu_hal.h"
#include "esp_private/cache_utils.h"

static const char *TAG = "pagemap";

#define SLOTS      16u
#define SLOT_SPAN  0x800000u                 /* one placeholder slot: the whole pool's size */
#define PH_BASE    0x20000000u               /* 512 MB: no flash there, none mapped */

static SemaphoreHandle_t s_mx;
EXT_RAM_BSS_ATTR static StaticSemaphore_t s_mx_buf;   /* PSRAM: internal RAM is the board's cliff */
static uint16_t s_slots;                     /* placeholder slots in use */
static uint32_t s_pages;                     /* pages mapped through windows */

/* With the caches off (nothing here may touch flash or PSRAM): point the window's entries at
 * the real pages, then drop whatever the cache held for those addresses. `paddr` is on the
 * caller's internal stack. */
static void IRAM_ATTR NOINLINE_ATTR remap(uint32_t vaddr, const uint32_t *paddr, uint32_t n)
{
    spi_flash_disable_interrupts_caches_and_other_cpu();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t got = 0;
        mmu_hal_map_region(0, MMU_TARGET_FLASH0, vaddr + i * ORB_PAGEMAP_PAGE, paddr[i],
                           ORB_PAGEMAP_PAGE, &got);
    }
    cache_hal_invalidate_addr(vaddr, n * ORB_PAGEMAP_PAGE);
    spi_flash_enable_interrupts_caches_and_other_cpu();
}

esp_err_t orb_pagemap(const esp_partition_t *p, const uint32_t *page_off, uint32_t n,
                      orb_pagemap_t *out)
{
    out->ptr = NULL; out->pages = 0; out->slot = -1;
    if (!p || !page_off || !n || n > ORB_PAGEMAP_MAX) return ESP_ERR_INVALID_ARG;
    uint32_t paddr[ORB_PAGEMAP_MAX];             /* on this stack: read with the caches off */
    if (!esp_ptr_internal(paddr)) {
        ESP_LOGE(TAG, "refused: the caller's stack is not in internal RAM");
        return ESP_ERR_INVALID_STATE;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (page_off[i] % ORB_PAGEMAP_PAGE || page_off[i] + ORB_PAGEMAP_PAGE > p->size)
            return ESP_ERR_INVALID_ARG;
        paddr[i] = p->address + page_off[i];
        if (paddr[i] % ORB_PAGEMAP_PAGE) return ESP_ERR_INVALID_ARG;   /* the partition is page-aligned */
    }
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    if (!s_mx) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int slot = -1;
    for (uint32_t s = 0; s < SLOTS; s++) if (!(s_slots & (1u << s))) { slot = (int)s; break; }
    esp_err_t e = ESP_ERR_NO_MEM;
    void *v = NULL;
    if (slot >= 0) {
        e = esp_mmu_map(PH_BASE + (uint32_t)slot * SLOT_SPAN, n * ORB_PAGEMAP_PAGE, MMU_TARGET_FLASH0,
                        MMU_MEM_CAP_READ | MMU_MEM_CAP_8BIT, 0, &v);
    }
    if (e == ESP_OK) {
        remap((uint32_t)v, paddr, n);
        s_slots |= 1u << slot;
        s_pages += n;
        out->ptr = v; out->pages = n; out->slot = (int8_t)slot;
    } else {
        ESP_LOGE(TAG, "a %u-page window would not map: %s", (unsigned)n, esp_err_to_name(e));
    }
    xSemaphoreGive(s_mx);
    return e;
}

void orb_pageunmap(orb_pagemap_t *m)
{
    if (!m || !m->ptr) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    const esp_err_t e = esp_mmu_unmap((void *)m->ptr);
    if (e != ESP_OK) ESP_LOGE(TAG, "a window would not unmap: %s", esp_err_to_name(e));
    if (m->slot >= 0) s_slots &= (uint16_t)~(1u << m->slot);
    s_pages -= m->pages;
    xSemaphoreGive(s_mx);
    m->ptr = NULL; m->pages = 0; m->slot = -1;
}

uint32_t orb_pagemap_pages(void) { return s_pages; }
