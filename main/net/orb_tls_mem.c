/* mbedTLS's allocator, chosen per task (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC).
 *
 * Every mbedTLS allocation on this board used to land in the internal pool,
 * which is right for the one user that existed (the radio's WPA supplicant:
 * small contexts the AES/SHA engines DMA straight out of) and impossible for an
 * outbound TLS session, which wants a 16 KB record buffer from a pool that
 * idles at ~11 KB free.
 *
 * The obvious switch, CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC, moves BOTH. Measured on
 * orb-A 2026-10-31: the supplicant's handshake then logged "esp-aes: Failed to
 * allocate memory" at boot — its contexts in PSRAM sent the AES engine down the
 * bounce path, which wants 1.6 KB of internal DMA RAM at the one instant the
 * pool sits under 100 B (post-wifi_start) — and the same squeeze cost the
 * serial door's mutex. So the policy is by task, not global: the task that
 * registers here (the flight feed's) gets PSRAM, everyone else keeps exactly
 * the internal behaviour the shipped image had. docs/flight-tracker-design.md §4.
 *
 * CUSTOM does not wire anything in by itself: esp_config.h only binds
 * MBEDTLS_PLATFORM_STD_CALLOC when CUSTOM is OFF, so under CUSTOM mbedTLS uses
 * libc calloc until orb_tls_mem_init() hands it these functions at runtime.
 * Measured before that call existed: a kept connection left 63 blocks / 15.5 KB
 * of mbedTLS state in the internal pool (anything <= SPIRAM_MALLOC_ALWAYSINTERNAL
 * lands there first), with this file's functions never called. */
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/platform.h"
#include "orb_tls_mem.h"

#define ORB_TLS_PSRAM_TASKS 2
static TaskHandle_t volatile s_psram_task[ORB_TLS_PSRAM_TASKS];

void orb_tls_mem_psram_for_me(void)
{
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < ORB_TLS_PSRAM_TASKS; i++)
        if (s_psram_task[i] == me) return;
    for (int i = 0; i < ORB_TLS_PSRAM_TASKS; i++)
        if (!s_psram_task[i]) { s_psram_task[i] = me; return; }
}

/* Live internal bytes mbedTLS holds, for the bench (orb_tls_mem_internal_live). */
static volatile int32_t s_int_live;

int32_t orb_tls_mem_internal_live(void) { return s_int_live; }

static void *book(void *p)
{
    if (p && esp_ptr_internal(p)) s_int_live += (int32_t)heap_caps_get_allocated_size(p);
    return p;
}

static void *tls_calloc(size_t n, size_t size)
{
    if (s_psram_task[0]) {
        const TaskHandle_t me = xTaskGetCurrentTaskHandle();
        for (int i = 0; i < ORB_TLS_PSRAM_TASKS; i++) {
            if (s_psram_task[i] == me) {
                void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (p) return p;
                break;
            }
        }
    }
    return book(heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void tls_free(void *ptr)
{
    if (ptr && esp_ptr_internal(ptr)) s_int_live -= (int32_t)heap_caps_get_allocated_size(ptr);
    heap_caps_free(ptr);
}

void orb_tls_mem_init(void)
{
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
}
