#pragma once
/**
 * orb_pagemap — scattered flash pages as one contiguous read-only window
 * (docs/theme-cache-plan.md §4.1, C7).
 *
 * A theme in the pool is a set of 64 KB pages wherever they were free. To draw it the
 * dial needs its bytes at consecutive addresses, so a window is made the size of the
 * pages: esp_mmu_map maps a placeholder run (an owned, contiguous block of virtual
 * addresses), and its MMU entries are then pointed, page by page, at the real pages, with
 * the caches off as ESP-IDF's own mapper does. Unmapping the window frees the block as any
 * other.
 *
 * The placeholder is far past the flash (the MMU takes physical addresses to 1 GB), one
 * 8 MB slot per window, so no placeholder is ever inside another mapped block (esp_mmu_map
 * would hand that block back instead of a new one) and none overlaps another.
 * spi_flash_mmap_pages is not used: it maps each run of pages separately and assumes the
 * addresses come out adjacent, which nothing guarantees.
 *
 * The caller's stack must be in internal RAM (the remap runs with the caches off); a
 * caller on a PSRAM stack is refused. One window operation at a time (a mutex).
 */
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"

#define ORB_PAGEMAP_PAGE    0x10000u
#define ORB_PAGEMAP_MAX     128u          /* pages a window holds: the whole pool */

typedef struct {
    const void *ptr;                      /* the window, NULL when unmapped */
    uint32_t    pages;
    int8_t      slot;                     /* its placeholder slot, -1 when unmapped */
} orb_pagemap_t;

/** Map `n` pages of partition `p`, given as partition-relative offsets of 64 KB pages, in
 *  the window's byte order. */
esp_err_t orb_pagemap(const esp_partition_t *p, const uint32_t *page_off, uint32_t n,
                      orb_pagemap_t *out);

/** Unmap a window (a no-op on one that is not mapped). */
void orb_pageunmap(orb_pagemap_t *m);

/** Windows mapped now, and their pages: the DROM budget's share. */
uint32_t orb_pagemap_pages(void);
