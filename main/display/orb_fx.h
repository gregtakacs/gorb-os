#pragma once
/**
 * orb_fx — the shell-transition bench (CONFIG_ORB_DEBUG_HTTP only).
 *
 * The question it answers before any design is written: what does an app-to-app
 * transition cost when it is painted outside LVGL, from the glass as it stands
 * (the panel mirror) into a theme's 232 px preview plane? Each effect is a
 * per-slab painter handed to display_fx_frame(), so a frame is compose + wire
 * and nothing else, and the door reports both per frame.
 *
 * One run, end to end, on the LVGL task: animate mirror -> preview over `ms`,
 * hold the last frame `hold` ms, then push the mirror back unchanged. The mirror
 * is never written, so the restore is exact and the view underneath never learns
 * the glass was borrowed. The restore frame doubles as the copy baseline: it is
 * the cheapest whole frame this path can send.
 *
 * Threading: orb_fx_run() blocks the calling httpd task on a semaphore while the
 * LVGL task, from input_router's poll, runs the frames. The plane is handed over
 * with the request and freed by whichever side finishes last.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_ORB_DEBUG_HTTP

typedef enum {
    ORB_FX_SLIDE = 0,   /* push: the glass leaves left, the preview enters right   */
    ORB_FX_IRIS,        /* a circle of preview grows from the centre               */
    ORB_FX_FADE,        /* per-pixel crossfade, 32 alpha steps                     */
    ORB_FX_DISC,        /* shrink to a disc, slide discs, grow (scaled gathers)    */
    ORB_FX_DRUM,        /* vertical drum: rows squash up, the preview rolls in     */
    ORB_FX_COUNT
} orb_fx_kind_t;

/** Name <-> kind, for the door. NULL / -1 when unknown. */
const char *orb_fx_name(orb_fx_kind_t k);
int         orb_fx_find(const char *name);

typedef struct {
    orb_fx_kind_t kind;
    uint32_t ms;          /* animation length (wall clock, frames are time-placed) */
    uint32_t hold_ms;     /* last frame held before the restore                    */
    bool     te;          /* wait one blanking edge per frame                      */
    int      dir;         /* +1 / -1: which way the slide/disc travel              */
} orb_fx_req_t;

typedef struct {
    esp_err_t err;
    int       frames;
    uint32_t  anim_us;        /* first frame start to last frame retire   */
    uint32_t  frame_avg_us, frame_max_us;
    uint32_t  compose_avg_us, compose_max_us;
    uint32_t  te_avg_us;
    uint32_t  restore_us;     /* the copy baseline: one mirror frame       */
    uint32_t  restore_compose_us;
    int       tiles;          /* ring depth the run had                    */
} orb_fx_report_t;

/** Run one transition into `plane` (w x h RGB565 little-endian, PSRAM, owned by
 *  the callee from here on: it is byte-swapped in place and freed). Blocks the
 *  caller up to `wait_ms` for the LVGL task to finish. ESP_ERR_TIMEOUT means the
 *  run is still going and will free the plane itself. */
esp_err_t orb_fx_run(const orb_fx_req_t *req, uint16_t *plane, uint16_t w,
                     uint16_t h, uint32_t wait_ms, orb_fx_report_t *out);

/** Called by input_router's poll on the LVGL task: runs a posted request. */
void orb_fx_service(void);

#endif /* CONFIG_ORB_DEBUG_HTTP */

#ifdef __cplusplus
}
#endif
