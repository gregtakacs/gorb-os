/* The glass heap — contract in orb_glass.h. */
#include "orb_glass.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "multi_heap.h"

static const char *TAG = "glass";

static uint8_t *s_base;
static size_t s_size;
static multi_heap_handle_t s_heap;
static SemaphoreHandle_t s_mx;
static StaticSemaphore_t s_mx_buf;
static volatile uint32_t s_fallbacks;
static void (*s_reclaim)(void);
static TaskHandle_t s_reclaim_task;
static bool s_tried;

static void lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

/* The carve, at the first ask: as much as the board can spare, once. */
static void carve(void)
{
    if (s_tried) return;
    s_tried = true;
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t freeb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t want = ORB_GLASS_MAX;
    if (freeb < ORB_GLASS_RESERVE + 512u * 1024u) want = 0;
    else if (want > freeb - ORB_GLASS_RESERVE) want = freeb - ORB_GLASS_RESERVE;
    if (want > largest) want = largest & ~(size_t)15;
    uint8_t *b = want ? heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    multi_heap_handle_t h = b ? multi_heap_register(b, want) : NULL;
    if (!h) {
        heap_caps_free(b);
        ESP_LOGW(TAG, "no glass region (%u B free, largest %u B): glass buffers use the general heap",
                 (unsigned)freeb, (unsigned)largest);
        return;
    }
    s_base = b;
    s_size = want;
    s_heap = h;
    ESP_LOGI(TAG, "glass region %u KB (%u KB was free, largest %u KB)", (unsigned)(want / 1024),
             (unsigned)(freeb / 1024), (unsigned)(largest / 1024));
}

bool orb_glass_owns(const void *p)
{
    return s_base && (const uint8_t *)p >= s_base && (const uint8_t *)p < s_base + s_size;
}

void *orb_glass_alloc_only(size_t n)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    lock();
    carve();
    void *p = s_heap ? multi_heap_malloc(s_heap, n) : NULL;
    unlock();
    if (!p && s_heap && s_reclaim && xTaskGetCurrentTaskHandle() == s_reclaim_task) {
        s_reclaim();                       /* the lingering caches off the glass go */
        lock();
        p = multi_heap_malloc(s_heap, n);
        unlock();
    }
    return p;
}

void *orb_glass_alloc(size_t n)
{
    void *p = orb_glass_alloc_only(n);
    if (p) return p;
    p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        s_fallbacks++;
        ESP_LOGW(TAG, "%u B from the general heap: the glass region is short", (unsigned)n);
    }
    return p;
}

void orb_glass_free(void *p)
{
    if (!p) return;
    if (orb_glass_owns(p)) {
        lock();
        multi_heap_free(s_heap, p);
        unlock();
    } else {
        heap_caps_free(p);
    }
}

void orb_glass_set_reclaim(void (*fn)(void))
{
    s_reclaim = fn;
    s_reclaim_task = xTaskGetCurrentTaskHandle();
}

void orb_glass_info(orb_glass_info_t *o)
{
    *o = (orb_glass_info_t){ .size = (uint32_t)s_size, .fallbacks = s_fallbacks };
    if (!s_heap) return;
    multi_heap_info_t mi;
    lock();
    multi_heap_get_info(s_heap, &mi);
    unlock();
    o->free = (uint32_t)mi.total_free_bytes;
    o->largest = (uint32_t)mi.largest_free_block;
}
