/* The system's edge arc — contract in orb_edge_arc.h.
 *
 * The arc lives only in the tiles: the mirror never holds it, so a row the dial re-sends
 * gets it stamped again on its way out, and a row it does not keeps what the panel already
 * shows. A change re-sends the rows it touched (the segment between the old figure and the
 * new, the whole ring for a show or a hide), and the hide re-sends them from the mirror
 * clean. A tile carries it only on the tiled path; the direct-from-mirror fallback, which
 * runs with no ring funded, shows none. */
#include "orb_edge_arc.h"

#include <math.h>
#include <stddef.h>

#include "esp_timer.h"
#include "lvgl.h"
#include "display.h"
#include "board_config.h"

#define CX      (SCREEN_WIDTH * 0.5f)
#define CY      (SCREEN_HEIGHT * 0.5f)
#define R_OUT   231.0f          /* a pixel in from the glass's rim */
#define R_IN    224.0f          /* 7 px: theme_strip's ring at the glass's size */
#define MOVE_US 100000          /* a moving arc re-sends at 10 Hz */

static volatile int16_t s_want = -1;    /* any task: what is asked */
static int16_t s_shown = -1;            /* LVGL task: what the tiles stamp */
static int64_t s_moved_us;

static inline uint16_t wire(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

void orb_edge_arc_set(int permille)
{
    s_want = (int16_t)(permille < 0 ? -1 : permille > 1000 ? 1000 : permille);
}

/* The glass rows the ring covers between two figures, both ends included. */
static void mark_segment(int p0, int p1)
{
    if (p0 > p1) { const int t = p0; p0 = p1; p1 = t; }
    const float a0 = (float)p0 * (2.0f * (float)M_PI) / 1000.0f;
    const float a1 = (float)p1 * (2.0f * (float)M_PI) / 1000.0f;
    float y_lo = (float)SCREEN_HEIGHT, y_hi = 0.0f;
    for (float a = a0;; a += 0.02f) {
        if (a > a1) a = a1;
        const float c = cosf(a);
        const float ya = CY - R_OUT * c, yb = CY - R_IN * c;
        y_lo = fminf(y_lo, fminf(ya, yb));
        y_hi = fmaxf(y_hi, fmaxf(ya, yb));
        if (a >= a1) break;
    }
    display_mark_rows((int32_t)y_lo - 1, (int32_t)y_hi + 1);
}

void orb_edge_arc_service(void)
{
    const int want = s_want;
    if (want == s_shown) return;
    const int64_t now = esp_timer_get_time();
    if (want >= 0 && s_shown >= 0 && now - s_moved_us < MOVE_US) return;
    if (want < 0 || s_shown < 0) display_mark_rows(0, SCREEN_HEIGHT - 1);
    else mark_segment(s_shown, want);
    s_shown = (int16_t)want;
    s_moved_us = now;
    /* A frame for the marks to ride: one pixel in the corner, outside the glass, which no
     * compositor paints and LVGL renders as it already stands. */
    lv_obj_t *scr = lv_screen_active();
    if (scr) {
        const lv_area_t corner = { 0, 0, 1, 1 };
        lv_obj_invalidate_area(scr, &corner);
    }
}

void orb_edge_arc_stamp(int32_t y0, int rows, uint16_t *tile, int32_t x0, int32_t w)
{
    if (s_shown < 0 || !tile) return;
    const uint16_t track = wire(0x3186), arc = wire(0x66DF);
    const float lim = (float)s_shown * (2.0f * (float)M_PI) / 1000.0f;
    for (int r = 0; r < rows; r++) {
        const float dy = (float)(y0 + r) + 0.5f - CY;
        if (dy <= -R_OUT || dy >= R_OUT) continue;
        /* the band alone: from the outer edge in to the inner one, either side (one span
         * through the rows that miss the inner circle), as orb_strip's ring_row does */
        const float ho = sqrtf(R_OUT * R_OUT - dy * dy);
        const float hi = fabsf(dy) < R_IN ? sqrtf(R_IN * R_IN - dy * dy) : 0.0f;
        int32_t spans[2][2] = { { (int32_t)(CX - ho), (int32_t)(CX - hi) },
                                { (int32_t)(CX + hi), (int32_t)(CX + ho) } };
        const int ns = hi == 0.0f ? 1 : 2;
        if (ns == 1) spans[0][1] = spans[1][1];
        uint16_t *d = tile + (size_t)r * (size_t)w - x0;
        for (int k = 0; k < ns; k++) {
            int32_t xa = spans[k][0], xb = spans[k][1];
            if (xa < x0) xa = x0;
            if (xb > x0 + w - 1) xb = x0 + w - 1;
            for (int32_t x = xa; x <= xb; x++) {
                const float dx = (float)x + 0.5f - CX, d2 = dx * dx + dy * dy;
                if (d2 < R_IN * R_IN || d2 > R_OUT * R_OUT) continue;
                float a = atan2f(dx, -dy);
                if (a < 0) a += 2.0f * (float)M_PI;
                d[x] = a <= lim ? arc : track;
            }
        }
    }
}
