/* Host check for the lonely image's drawing code. Run it with test/run.sh.
 *
 * It compiles the SHIPPED drawing functions out of ../main/lonely_main.c (run.sh
 * extracts them; this file holds no copy of them) against a fake CO5300 panel
 * with a fake GRAM, runs the animation for thousands of frames, and holds it to
 * the four rules that panel punishes silently. Those four are exactly what the
 * composited-dirty-band rewrite shipped without, and each one showed on the
 * glass: the green ball, the unreadable type, the sprite trails.
 *
 *   1 EVEN   every flush window starts and spans an even count on both axes,
 *   2 WIDTH  every flush is full width (x 0..465), so x is even by construction,
 *   3 ONE    no band is submitted while the one band buffer's previous DMA is
 *            still reading it, and every submit follows its take,
 *   4 CLEAN  the GRAM after every frame equals a fresh full repaint of the same
 *            scene: a stale pixel is a trail, and trails are what an odd window
 *            leaves behind.
 *
 * It also decodes the GRAM the way the wire does and counts the palette, so a
 * color stored in host order (0x2104 reaching the panel as 0x0421, dark green)
 * fails the run instead of shipping as a joke about the ball.
 *
 * Exits non-zero on the first violation. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

typedef int esp_err_t;
#define ESP_OK 0
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef struct { int dummy; } esp_lcd_panel_io_event_data_t;
#define IRAM_ATTR
typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
typedef void *SemaphoreHandle_t;

#define GRAM_W 466
#define GRAM_H 466
static uint16_t gram[2][GRAM_H][GRAM_W];
static int cur_fb;
static int flushes, bad_align, bad_width, submits_while_inflight, takes;
static int in_flight, order_bad, last_event, done_events;

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0,
                                   int x1, int y1, const void *color)
{
    const uint16_t *buf = (const uint16_t *)color;
    (void)p;
    flushes++;
    if ((x0 & 1) || ((x1 - x0) & 1) || (y0 & 1) || ((y1 - y0) & 1)) bad_align++;
    if (x0 != 0 || x1 != GRAM_W) bad_width++;
    if (in_flight) submits_while_inflight++;
    if (last_event != 1) order_bad++;
    last_event = 2;
    for (int r = 0; r < y1 - y0; r++)
        for (int x = 0; x < GRAM_W; x++)
            gram[cur_fb][y0 + r][x] = buf[r * GRAM_W + x];
    in_flight = 1;
    return ESP_OK;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
int xSemaphoreTake(SemaphoreHandle_t s, int t)
{
    (void)s; (void)t;
    takes++;
    last_event = 1;
    if (in_flight) { in_flight = 0; done_events++; }
    return 1;
}
int xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *w) { (void)s; (void)w; return 1; }
void *heap_caps_malloc(size_t n, int f) { (void)f; return calloc(n, 1); }
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_DMA 0
#define portMAX_DELAY 0xFFFFFFFFu

/* The extraction above drops the device includes, so the font table the scene
 * draws with is declared here and compiled from the real main/font5x7.c. */
extern const uint8_t font5x7[95][5];
#include "scene.inc"

static void reference_paint(void)
{
    cur_fb = 1;
    repaint_rows(0, SCREEN_H);
    cur_fb = 0;
}

int main(void)
{
    band = heap_caps_malloc(SCREEN_W * BAND_ROWS * sizeof(uint16_t), 0);
    ball_sprite = heap_caps_malloc(SPRITE_SIDE * SPRITE_SIDE * sizeof(uint16_t), 0);
    band_free = xSemaphoreCreateBinary();
    build_ball();
    int half_diag = (int)sqrtf((MSG_W_MAX / 2.0f) * (MSG_W_MAX / 2.0f) +
                               (MSG_H / 2.0f) * (MSG_H / 2.0f));
    msg = (body_t){ .x = SCREEN_W / 2.0f, .y = SCREEN_H / 2.0f - 60,
                    .vx = -2.0f, .vy = 1.6f,
                    .lane_r = SCREEN_W / 2.0f - half_diag - 8,
                    .w = MSG_W_MAX, .h = MSG_H };
    ball = (body_t){ .x = SCREEN_W / 2.0f + 90, .y = SCREEN_H / 2.0f + 80,
                     .vx = 4.0f, .vy = 3.0f,
                     .lane_r = SCREEN_W / 2.0f - BALL_R - 3,
                     .w = SPRITE_SIDE, .h = SPRITE_SIDE };

    repaint_rows(0, SCREEN_H);            /* the dark-glass first paint */
    box_t msg_now = body_box(&msg), ball_now = body_box(&ball);
    const int FRAMES = 2000;
    int stale = 0, stale_frame = -1, first_r = -1, first_c = -1;

    for (int frame = 0; frame < FRAMES; frame++) {
        bounce(&msg);
        bounce(&ball);
        box_t mp = msg_now, bp = ball_now;
        msg_now = body_box(&msg);
        ball_now = body_box(&ball);
        box_t m = box_union(mp, msg_now);
        box_t b = box_union(bp, ball_now);
        repaint_frame(&m, &b);
        /* Every frame must leave the glass holding exactly the scene. A frame
         * that does not is a trail, and trails are what an odd window writes. */
        reference_paint();
        for (int y = 0; y < GRAM_H && !stale; y++)
            for (int x = 0; x < GRAM_W; x++)
                if (gram[0][y][x] != gram[1][y][x]) {
                    stale++; stale_frame = frame;
                    if (first_r < 0) { first_r = y; first_c = x; }
                }
        if (stale) break;
    }

    /* What the panel lights up. It reads each stored uint16 big-endian, so
     * decode by swapping back — exactly what the wire does to it. A tint stored
     * in host order lands here as a different color: 0x2104 as 0x0421, green. */
    int bx = (int)lroundf(ball.x - ball.w / 2.0f), by = (int)lroundf(ball.y - ball.h / 2.0f);
    uint16_t shown = (uint16_t)__builtin_bswap16(gram[0][by + SPRITE_SIDE / 2][bx + SPRITE_SIDE / 2]);
    int gray = 0, dark = 0, white = 0, black = 0, other = 0;
    for (int y = 0; y < GRAM_H; y++) for (int x = 0; x < GRAM_W; x++) {
        uint16_t v = (uint16_t)__builtin_bswap16(gram[0][y][x]);
        if (v == 0x7BEF) gray++; else if (v == 0x2104) dark++;
        else if (v == 0xFFFF) white++; else if (v == 0x0000) black++; else other++;
    }
    printf("ball centre lights as 0x%04X (wants 0x2104, the dark of the ball)\n", shown);
    printf("pixel census: gray %d dark %d white %d black %d other %d\n", gray, dark, white, black, other);
    printf("frames %d flushes %d takes %d\n", FRAMES, flushes, takes);
    printf("odd windows          %d\n", bad_align);
    printf("non-full-width       %d\n", bad_width);
    printf("submit on open DMA   %d\n", submits_while_inflight);
    printf("take/submit order    %d\n", order_bad);
    printf("stale pixels (trail) %d\n", stale);
    if (stale) printf("  first at frame %d row %d col %d\n", stale_frame, first_r, first_c);
    if (shown != 0x2104 || gray == 0 || dark == 0 || white == 0 || other != 0) {
        printf("FAIL: the palette did not reach the glass as drawn\n");
        return 1;
    }
    return (bad_align || bad_width || submits_while_inflight || order_bad || stale) ? 1 : 0;
}
