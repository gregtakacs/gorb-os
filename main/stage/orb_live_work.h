/* orb_live_work.h — the live layers' worker (OS v1.2.38, radar-plan §6.27).
 *
 * An app draws its next picture of a live layer into the layer's back copy without
 * holding the frame: the draws are queued here, in order, and a task under the LVGL task
 * makes them (a rain frame's decode is ~100 ms; the sweep's frame is 50). The app asks
 * stage_live_present when the queue for that layer is empty. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

enum { LW_BEGIN, LW_PNG8, LW_RING, LW_LINE, LW_DIFF };

typedef struct {
    uint8_t  op, layer;
    int8_t   src;            /* LW_BEGIN: the front layer copied in, or -1 for a fill of argb */
    uint32_t argb;
    int32_t  a[8];           /* PNG8: sx sy dx dy w h scale; RING: cx cy r; LINE: x0 y0 x1 y1 */
    uint8_t *png;            /* PNG8: the frame's bytes, the job's own copy (freed after) */
    uint32_t len;
    uint32_t *pal;           /* PNG8: 256 ARGB words, the job's own copy (freed after) */
} lw_job_t;

/* Queue a draw; the job's png and pal pass to the worker. ESP_ERR_NO_MEM: nothing queued
 * (and png and pal are freed). */
esp_err_t orb_live_work_post(lw_job_t *j);
/* Draws queued or being made for layer l. */
int orb_live_work_pending(int l);
/* The back picture against the front, row by row (LW_DIFF, queued by the present): a row's
 * changed pixels lie in [x0[y], x1[y]], none where x0 > x1. 1 with the rows when the last
 * diff of layer l is done and not yet taken (taking it clears it), else 0. The worker
 * compares on its own core, so the compositor rebuilds only what the new picture changed
 * (radar-plan §6.27: a rain frame changes a fraction of the disc; the map under it, none). */
int orb_live_work_diff(int l, const int16_t **x0, const int16_t **x1);
/* The same answer, the diff left in place. */
bool orb_live_work_diff_ready(int l);
