#pragma once
#include <stdint.h>
/* Install the per-task allocator into mbedTLS. Call first thing in app_main,
 * before the radio's supplicant or anything else allocates through it. */
void orb_tls_mem_init(void);
/* Route the calling task's mbedTLS allocations to PSRAM (orb_tls_mem.c). Call
 * once from the task that will run TLS sessions, before its first handshake. */
void orb_tls_mem_psram_for_me(void);
/* Internal-pool bytes mbedTLS holds right now (all tasks): the bench's check
 * that the per-task policy is what is actually happening. */
int32_t orb_tls_mem_internal_live(void);
