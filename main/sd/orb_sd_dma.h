#pragma once
/* The card's own DMA memory (orb_sd_dma.c): the SD stack's internal-DMA asks are
 * served from a fixed private arena while a task holds the card lock, so the card
 * never borrows the display's tile ring or churns the shared pool. */
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** Fund the arena if it is not yet: true when the card's asks have it. Called by
 *  orb_sd_lock on every take; only the first that succeeds allocates. */
bool orb_sd_dma_ready(void);

/** The task whose DMA asks the arena serves: orb_sd_lock sets the holder,
 *  orb_sd_unlock clears it (NULL). */
void orb_sd_dma_owner(TaskHandle_t t);

/** For /api/v1/status: bytes free now, the low-water mark, and asks that fell
 *  through to the shared pool because the arena was full. */
void orb_sd_dma_stats(uint32_t *free_b, uint32_t *min_b, uint32_t *falls);
