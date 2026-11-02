/* orb_io_window.h — "a large transfer is running", for code that judges itself by
 * the wall clock.
 *
 * A wasm guest's time budget (orb_app_wasm.c) is wall time on the LVGL task, and a
 * multi-megabyte card upload, theme store or OTA takes the CPU, the SPI bus and the
 * flash cache away from that task for long stretches. A guest that did nothing wrong
 * then overruns and takes a C2 strike (orb-A 2026-11-02: flights, during a 3 MB theme
 * upload). The transfers bracket themselves here; the budget asks whether one ran
 * during the call before it calls the overrun the guest's fault.
 *
 * Nesting is allowed, and every function is task-safe and cheap.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void     orb_io_heavy_begin(void);
void     orb_io_heavy_end(void);
/** True while at least one bracket is open. */
bool     orb_io_heavy_now(void);
/** Moves on every begin: a reader that saw one value at the start of a window and
 *  another at its end knows a transfer started inside it, even one already over. */
uint32_t orb_io_heavy_gen(void);
