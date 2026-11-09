/* The card's own DMA memory: a small private heap the SD path allocates from, so a
 * card read never has to go to the shared internal pool (owner, 2026-11-09).
 *
 * The SD stack asks for internal DMA memory all the time, in small pieces: a 512 B
 * bounce per sdmmc_read_sectors/write_sectors call into a PSRAM buffer
 * (sdmmc_cmd.c:477, :611 — every FatFs window here is in PSRAM), the SPI master's
 * bounce for each command block that sits on a PSRAM task stack (spi_master.c:1145,
 * :1165 — most OS tasks have one), the SDSPI slot's block buffer (sdspi_host.c:162)
 * and a handful of tiny response buffers at mount (sdmmc_sd.c, sdmmc_common.c:401).
 * Each is freed again at once. With the radio associated that pool is fragmented
 * below even those asks (measured: 125 B refused with 4,315 B free), so the card lock
 * used to free the display's whole tile ring (2 x 18,640 B) into the pool and take it
 * back on release. The take-back is a fresh allocation, and anything that took a
 * piece of the hole meanwhile (the radio, lwIP) leaves no 18,640 B block for it: the
 * ring comes back short (seen on Bronze under radar, 1 of 2) or not at all, and the
 * glass stops refreshing.
 *
 * So the card never touches the shared pool after this: every DMA-capable ask of
 * ORB_SD_DMA_MAX_ASK or less made by the task holding the card lock is served from
 * the arena below, an IDF multi_heap of its own over a fixed block of internal DRAM.
 * The ring stays where it is, and the churn the card makes stays inside its own
 * block, where nothing else can fragment it or take it.
 *
 * The hook is the linker: main/CMakeLists.txt wraps the heap_caps entry points the
 * SD stack reaches (directly, through esp_dma_capable_malloc, and through free()).
 * Everything else passes straight through to the real heap. heap_caps_free is IRAM
 * and is called from ISRs and with the flash cache off, so every wrapper here is
 * IRAM_ATTR, its data is in DRAM, and it only calls multi_heap (IRAM unless
 * CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH) and a spinlock.
 *
 * A full arena is not a failure: the ask falls through to the real heap, which is
 * exactly the old behaviour minus the ring loan, and the fall is counted for
 * /api/v1/status ("sd".dma_falls). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "multi_heap.h"

#include "orb_sd_dma.h"

static const char *TAG = "orb_sd_dma";

/* Allocated from the shared pool once, at the first card lock, and kept for the life of
 * the boot: a fixed 3 KB that never moves fragments nothing. Not .bss: an unprovisioned
 * boot ends with that pool near zero (orb_net.c, "a 533 B .bss line ... tipped"), and a
 * block taken at run time can be refused, where a .bss one would refuse something else.
 * Refused, the lock lends the tile ring the old way (orb_sd_lock) and asks again next
 * time.
 *
 * Sized from the asks above at their worst together: the slot's block buffer
 * (516 B, kept while mounted), one sector bounce (512 B), two SPI command bounces
 * (~32 B each) and a response, plus multi_heap's own control block and per-block
 * heads. The bench reads dma_min after a mount and a theme store to check it. */
#define ORB_SD_DMA_ARENA    3072
/* An ask bigger than this is not one of the card's (the largest is a sector plus its
 * token), so it goes to the real heap even from the lock holder. */
#define ORB_SD_DMA_MAX_ASK  1024

static uint8_t *volatile s_arena;          /* NULL until the first lock funds it */
static multi_heap_handle_t s_heap;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t volatile s_owner;      /* the card lock's holder, or NULL */
static volatile uint32_t s_falls;          /* asks the arena could not hold */

void *__real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void __real_heap_caps_free(void *ptr);

bool orb_sd_dma_ready(void)
{
    if (s_heap) return true;
    uint8_t *a = __real_heap_caps_aligned_alloc(16, ORB_SD_DMA_ARENA,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!a) return false;
    const multi_heap_handle_t h = multi_heap_register(a, ORB_SD_DMA_ARENA);
    if (!h) {
        __real_heap_caps_free(a);
        ESP_LOGE(TAG, "the card's DMA arena would not register");
        return false;
    }
    s_arena = a;          /* the range first: a free() of an arena block checks it */
    s_heap = h;
    ESP_LOGI(TAG, "card DMA arena: %u B, %u B usable", (unsigned)ORB_SD_DMA_ARENA,
             (unsigned)multi_heap_free_size(h));
    return true;
}

void orb_sd_dma_owner(TaskHandle_t t) { s_owner = t; }

void orb_sd_dma_stats(uint32_t *free_b, uint32_t *min_b, uint32_t *falls)
{
    *free_b = s_heap ? (uint32_t)multi_heap_free_size(s_heap) : 0;
    *min_b  = s_heap ? (uint32_t)multi_heap_minimum_free_size(s_heap) : 0;
    *falls  = s_falls;
}

static inline IRAM_ATTR bool in_arena(const void *p)
{
    const uint8_t *a = s_arena;
    return a && (const uint8_t *)p >= a && (const uint8_t *)p < a + ORB_SD_DMA_ARENA;
}

static inline IRAM_ATTR bool for_arena(size_t size, uint32_t caps)
{
    return s_heap && s_owner && (caps & MALLOC_CAP_DMA) && !(caps & MALLOC_CAP_SPIRAM) &&
           size && size <= ORB_SD_DMA_MAX_ASK && !xPortInIsrContext() &&
           xTaskGetCurrentTaskHandle() == s_owner;
}

static IRAM_ATTR void *arena_alloc(size_t alignment, size_t size)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    void *p = alignment > 4 ? multi_heap_aligned_alloc(s_heap, size, alignment)
                            : multi_heap_malloc(s_heap, size);
    portEXIT_CRITICAL_SAFE(&s_mux);
    if (!p) s_falls++;
    return p;
}

static IRAM_ATTR void arena_free(void *p)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    multi_heap_free(s_heap, p);
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void *__real_heap_caps_malloc(size_t size, uint32_t caps);
void *__real_heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *__real_heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps);
void *__real_heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void *__real_heap_caps_realloc_default(void *ptr, size_t size);
size_t __real_heap_caps_get_allocated_size(void *ptr);

IRAM_ATTR void *__wrap_heap_caps_malloc(size_t size, uint32_t caps)
{
    if (for_arena(size, caps)) {
        void *p = arena_alloc(4, size);
        if (p) return p;
    }
    return __real_heap_caps_malloc(size, caps);
}

IRAM_ATTR void *__wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    if (for_arena(size, caps)) {
        void *p = arena_alloc(alignment, size);
        if (p) return p;
    }
    return __real_heap_caps_aligned_alloc(alignment, size, caps);
}

IRAM_ATTR void *__wrap_heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    if (n && size <= ORB_SD_DMA_MAX_ASK / n && for_arena(n * size, caps)) {
        void *p = arena_alloc(4, n * size);
        if (p) { memset(p, 0, n * size); return p; }
    }
    return __real_heap_caps_calloc(n, size, caps);
}

IRAM_ATTR void *__wrap_heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size,
                                                uint32_t caps)
{
    if (n && size <= ORB_SD_DMA_MAX_ASK / n && for_arena(n * size, caps)) {
        void *p = arena_alloc(alignment, n * size);
        if (p) { memset(p, 0, n * size); return p; }
    }
    return __real_heap_caps_aligned_calloc(alignment, n, size, caps);
}

IRAM_ATTR void __wrap_heap_caps_free(void *ptr)
{
    if (in_arena(ptr)) { arena_free(ptr); return; }
    __real_heap_caps_free(ptr);
}

IRAM_ATTR size_t __wrap_heap_caps_get_allocated_size(void *ptr)
{
    if (in_arena(ptr)) return multi_heap_get_allocated_size(s_heap, ptr);
    return __real_heap_caps_get_allocated_size(ptr);
}

/* Nothing in the SD stack reallocates its DMA buffers, but a block from the arena
 * must never reach the real heap's realloc, which would look for it in a heap it is
 * not in. Moved out to the real heap instead: a realloc is somebody keeping it. */
static IRAM_ATTR void *arena_realloc(void *ptr, size_t size, uint32_t caps)
{
    if (!size) { arena_free(ptr); return NULL; }
    void *n = __real_heap_caps_malloc(size, caps);
    if (!n) return NULL;
    const size_t had = multi_heap_get_allocated_size(s_heap, ptr);
    memcpy(n, ptr, had < size ? had : size);
    arena_free(ptr);
    return n;
}

IRAM_ATTR void *__wrap_heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    if (in_arena(ptr)) return arena_realloc(ptr, size, caps);
    return __real_heap_caps_realloc(ptr, size, caps);
}

IRAM_ATTR void *__wrap_heap_caps_realloc_default(void *ptr, size_t size)
{
    if (in_arena(ptr)) return arena_realloc(ptr, size, MALLOC_CAP_DEFAULT);
    return __real_heap_caps_realloc_default(ptr, size);
}
