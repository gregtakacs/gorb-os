/*
 * orb_log: a RAM ring over the log stream, readable over HTTP.
 *
 * Why this exists at all is D38 §6.1 and §13.3: the MTP file-share mode hands the
 * USB Serial/JTAG console to TinyUSB, so for the whole window in which the device
 * is most likely to misbehave there is no way to see it misbehave. Every
 * diagnosis of that mode so far has been reasoning from a status line because the
 * evidence that would have settled it went to a port nobody was holding.
 *
 * esp_log_set_vprintf() is the only seam that survives that window: it sits below
 * the driver choice entirely, so a build with no console sink still fills this
 * ring. It costs ORB_LOG_BYTES of internal DRAM, permanently, in every build.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ORB_LOG_BYTES 2048   /* ~13 MTP trace lines; see the sizing note in orb_log.c */

/* Install the hook. Call before anything worth reading is logged, and exactly
 * once: the previous vprintf is chained, so the console keeps what it had. */
esp_err_t orb_log_init(void);

/* Copy the retained log text, oldest line first, into out and NUL-terminate it.
 * Incomplete lines at the head of a wrapped ring are dropped rather than
 * half-reported. Returns the number of bytes written, excluding the NUL. */
size_t orb_log_tail(char *out, size_t outsz);

/* Bytes of log text overwritten and therefore gone. Non-zero simply means the
 * window being asked about is older than the ring; it is not a fault. */
uint32_t orb_log_dropped(void);
