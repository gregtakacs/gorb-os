/* lonely_main.c — the image a lonely orb gets instead of an orb life.
 *
 * "I used to be an Orb, now I'm just lonely. Flash me!" — a bouncing message
 * block and a bouncing 8-ball on the panel the orb used to run its life on.
 * That is the whole program: no WiFi, no NVS use, no mDNS, no serial command
 * door. The point is that the chip BOOTS — enumerates once as an ordinary
 * Espressif CDC device (303a:1001) and holds the port, instead of the post-wipe
 * ROM retry loop re-enumerating the ACM device every few seconds. The hub sees
 * a non-orb Espressif board, opens the New Orb door over it, and a full flash
 * replaces every byte this one occupies.
 *
 * This image is also the settings wipe. It erases the orb's stored name, time
 * zone and credentials at every boot — the serial path erases NVS regions in
 * the same session that wrote this app, but a board that becomes lonely by any
 * other door keeps none of its owner's settings either. The app never writes
 * NVS, so an already-blank partition costs one idempotent pass.
 *
 * No burn-in on an AMOLED: nothing sits still. All drawing is composited one
 * pass per frame: the dirty rows are rebuilt black-background + message + ball
 * inside an internal DMA band buffer and flushed once each, so the panel never
 * shows a mid-frame erase. The erase-then-redraw two-step this file used to run
 * is what the flicker was.
 *
 * That rewrite shipped four panel rules unkept, and each one shows on the glass:
 * pixels reached the CO5300 low-byte-first (the dark of the ball came up green),
 * flush windows started and spanned odd counts on both axes (rows sheared, so
 * the type read as noise and every sprite left its old pixels behind), the one
 * band buffer was resubmitted while its previous DMA still read it (bands landed
 * over each other), and the panel was never painted end to end, so the GRAM a
 * board powers on with stayed on show around the two boxes. The rules kept below
 * are the four that cost: wire order, even windows, one band in flight, and a
 * whole-panel first paint behind a dark display. The flicker fix stands — the
 * bands stay composited. */

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_co5300.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "font5x7.h"

static const char *TAG = "lonely";

/* ---- The board, from main/board_config.h of the firmware this replaced ---- */
#define PIN_LCD_CS   12
#define PIN_LCD_RST  39
#define PIN_LCD_SCLK 38
#define PIN_LCD_D0    4
#define PIN_LCD_D1    5
#define PIN_LCD_D2    6
#define PIN_LCD_D3    7
#define SCREEN_W    466   /* round panel: the visible window is the inscribed square */
#define SCREEN_H    466
#define COL_GAP       6   /* GRAM offset: visible cols map to 6..471 (vendor CASET) */

/* Scale 2 keeps glyph pixels on the panel's even-width flush grid and keeps
 * the longest message line inside the round glass. */
#define GLYPH_SCALE   2
#define CELL_W   (6 * GLYPH_SCALE)   /* 5 cols + 1 space */
#define CELL_H   (8 * GLYPH_SCALE)   /* 7 rows + 1 line gap */
#define MSG_LINES    3
static const char *MSG[MSG_LINES] = {
    "I used to be an Orb,",
    "now I'm just lonely.",
    "Flash me!",
};
#define MSG_W_MAX  (23 * CELL_W)      /* longest line's cell count, measured */
#define MSG_H      (MSG_LINES * CELL_H)
#define BALL_R      26
#define SPRITE_SIDE (2 * BALL_R + 2)

#define BAND_ROWS   8     /* even: a band always starts and spans an even row count */
#define RGB565_BLACK 0x0000
#define RGB565_WHITE 0xFFFF
#define RGB565_GRAY  0x7BEF  /* message body: not quite white, the ball owns white */
#define RGB565_DARK  0x2104

/* Rule WIRE: what goes into a flush buffer is wire order, not host order. The
 * CO5300 reads each pixel big-endian and esp_lcd_panel_draw_bitmap is a verbatim
 * tx_color, so the two tinted colors have to be byte-swapped here or the panel
 * paints them as something else: 0x2104 arrived as 0x0421, a dark green — the
 * ball's body. Black and white are byte palindromes, which is why the ring and
 * the black between sprites still looked right. main/display/display.c does the
 * same swap for the same panel for exactly this reason. */
#define WIRE(v) ((uint16_t)(((uint16_t)(v) >> 8) | ((uint16_t)(v) << 8)))
#define INK_BLACK WIRE(RGB565_BLACK)
#define INK_WHITE WIRE(RGB565_WHITE)
#define INK_GRAY  WIRE(RGB565_GRAY)
#define INK_DARK  WIRE(RGB565_DARK)

static esp_lcd_panel_handle_t panel;
static uint16_t *band;              /* SCREEN_W * BAND_ROWS, one composited strip per flush */
static uint16_t *ball_sprite;
/* Rule ONE-IN-FLIGHT: esp_lcd_panel_draw_bitmap returns as soon as the transfer is
 * queued (queue depth 10), so band is still under DMA when the next band is
 * composed. One buffer plus a trans-done token serializes them. The main
 * firmware pays this with a ping-pong tile ring (display.c, L1); this app has
 * two sprites and no frame budget to protect, so it waits. */
static SemaphoreHandle_t band_free;

static bool IRAM_ATTR band_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io; (void)edata; (void)ctx;
    BaseType_t hi = pdFALSE;
    xSemaphoreGiveFromISR(band_free, &hi);
    return hi == pdTRUE;
}

/* Two floaty bodies: the message block and the ball, each bouncing inside its
 * own circular lane of the round panel. */
typedef struct {
    float x, y;      /* center, px */
    float vx, vy;    /* px per tick */
    float lane_r;    /* max center distance from panel center */
    int w, h;        /* bounding box */
} body_t;
static body_t msg, ball;
#define TICK_MS 33

typedef struct { int x, y, w, h; } box_t;

/* ---- Drawing ----------------------------------------------------------------
 * The scene is always black background + message + ball (ball on top), so any
 * rect can be rebuilt to its final state in one pass. Rendering straight into
 * the flush buffer — never a black rect first — is what ends the flicker.
 *
 * Rule EVEN: every flush window is full width and covers a whole number of even
 * rows. The CO5300 clocks two pixels per cycle on each axis and the driver
 * programs CASET/RASET verbatim, so an odd start or an odd span addresses the
 * window one column/row off its data: rows come out sheared (the reason the type
 * read as noise), and the pixels a sprite left on the glass land outside the next
 * window that meant to erase them (the trail). x = 0..465 is even by construction
 * and COL_GAP is 6, so the GRAM window stays even too; the row runs are snapped
 * outward for the same reason display.c's DB_EVEN_BANDS does it. */

/* One panel row of glyphs: the message's pixels inside band rows [y0, y0+rows)
 * of the panel's full width, drawn as they will be on the glass. */
static void render_message(int y0, int rows)
{
    int bx = (int)lroundf(msg.x - msg.w / 2.0f);
    int by = (int)lroundf(msg.y - msg.h / 2.0f);
    for (int line = 0; line < MSG_LINES; line++) {
        int n = (int)strlen(MSG[line]);
        int lw = n * CELL_W - GLYPH_SCALE;      /* trailing inter-cell gap cut */
        int lx = bx + (msg.w - lw) / 2;         /* this line's left edge */
        int ly = by + line * CELL_H;            /* this line's glyph top */
        int xa = (lx > 0) ? lx : 0;             /* never write past a band row */
        int xb = (lx + lw < SCREEN_W) ? lx + lw : SCREEN_W;
        for (int r = ly - y0; r < ly + 7 * GLYPH_SCALE - y0; r++) {
            if (r < 0 || r >= rows) continue;
            int grow = (y0 + r - ly) / GLYPH_SCALE;
            uint16_t *p = band + (size_t)r * SCREEN_W;
            for (int x = xa; x < xb; x++) {
                int px = x - lx;
                int cell = px / CELL_W;
                int gc = (px % CELL_W) / GLYPH_SCALE;
                char c = MSG[line][cell];
                if (cell < n && gc < 5 && c >= ' ' && c <= '~' &&
                    (font5x7[c - ' '][gc] & (1 << grow)))
                    p[x] = INK_GRAY;
            }
        }
    }
}

/* The ball's pixels inside the same band, drawn over anything already there. */
static void render_ball(int y0, int rows)
{
    int bs = (int)lroundf(ball.x - ball.w / 2.0f);
    int by = (int)lroundf(ball.y - ball.h / 2.0f);
    int xa = (bs > 0) ? bs : 0;                 /* never write past a band row */
    int xb = (bs + SPRITE_SIDE < SCREEN_W) ? bs + SPRITE_SIDE : SCREEN_W;
    int sy = (by > y0) ? by : y0;
    int ey = (by + SPRITE_SIDE < y0 + rows) ? by + SPRITE_SIDE : y0 + rows;
    for (int y = sy; y < ey; y++) {
        const uint16_t *src = ball_sprite + (size_t)(y - by) * SPRITE_SIDE;
        uint16_t *dst = band + (size_t)(y - y0) * SCREEN_W;
        for (int x = xa; x < xb; x++) dst[x] = src[x - bs];
    }
}

/* Final scene for panel rows [y0, y0+rows) at full width: black, then the
 * message, then the ball, then one flush into the one band buffer — which is
 * handed back only when its DMA is off the wire. */
static void render_and_flush(int y0, int rows)
{
    xSemaphoreTake(band_free, portMAX_DELAY);
    for (int i = 0; i < SCREEN_W * rows; i++) band[i] = INK_BLACK;
    render_message(y0, rows);
    render_ball(y0, rows);
    if (esp_lcd_panel_draw_bitmap(panel, 0, y0, SCREEN_W, y0 + rows, band) != ESP_OK)
        xSemaphoreGive(band_free);   /* nothing queued, so nothing will give it back */
}

/* Repaint rows [y0, y1) — snapped outward onto the panel's even row grid, then
 * cut into bands of BAND_ROWS. Widening is free: each band is re-composited from
 * the scene, so an extra row holds its real pixels and not a stale copy. */
static void repaint_rows(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    if (y0 >= y1) return;
    y0 &= ~1;
    if ((y1 - y0) & 1) y1++;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    for (int y = y0; y < y1; y += BAND_ROWS) {
        int rows = y1 - y;
        if (rows > BAND_ROWS) rows = BAND_ROWS;
        render_and_flush(y, rows);
    }
}

/* Every row the two sprites owe a repaint to this frame, in as few runs as the
 * glass allows: two bodies whose row runs come within ROW_FUSE of each other
 * are painted as one. Rows outside every run keep what the last paint put there
 * — which, after the first paint covers the whole panel, is the scene. */
#define ROW_FUSE 16     /* bridge a gap this small rather than pay another run */
static void repaint_frame(const box_t *a, const box_t *b)
{
    int r0a = a->y, r1a = a->y + a->h;
    int r0b = b->y, r1b = b->y + b->h;
    if (r0b < r0a) {                      /* order the runs by where they start */
        int t0 = r0a, t1 = r1a;
        r0a = r0b; r1a = r1b;
        r0b = t0; r1b = t1;
    }
    if (r0b - r1a <= ROW_FUSE) {
        repaint_rows(r0a, (r1b > r1a) ? r1b : r1a);
        return;
    }
    repaint_rows(r0a, r1a);
    repaint_rows(r0b, r1b);
}

static box_t body_box(const body_t *b)
{
    return (box_t){ .x = (int)lroundf(b->x - b->w / 2.0f),
                    .y = (int)lroundf(b->y - b->h / 2.0f),
                    .w = b->w, .h = b->h };
}

static box_t box_union(box_t a, box_t b)
{
    int x2 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
    int y2 = (a.y + a.h > b.y + b.h) ? a.y + a.h : b.y + b.h;
    int x1 = (a.x < b.x) ? a.x : b.x;
    int y1 = (a.y < b.y) ? a.y : b.y;
    return (box_t){ .x = x1, .y = y1, .w = x2 - x1, .h = y2 - y1 };
}

/* One 8-ball sprite: white ring, dark body, white number disc, black "8".
 * Stored in wire order, so the band copy below is verbatim. */
static void build_ball(void)
{
    int c = SPRITE_SIDE / 2;
    for (int y = 0; y < SPRITE_SIDE; y++) {
        for (int x = 0; x < SPRITE_SIDE; x++) {
            int dx = x - c, dy = y - c;
            int d2 = dx * dx + dy * dy;
            uint16_t v = INK_BLACK;
            if (d2 <= (BALL_R + 1) * (BALL_R + 1)) {
                v = INK_WHITE;
                if (d2 <= (BALL_R - 2) * (BALL_R - 2)) v = INK_DARK;
                if (d2 <= (BALL_R / 2) * (BALL_R / 2)) v = INK_WHITE;
                int ax = x - (c - 5), ay = y - (c - 7);
                if (ax >= 0 && ax < 10 && ay >= 0 && ay < 14 &&
                    (font5x7['8' - ' '][ax / 2] >> (ay / 2)) & 1)
                    v = INK_DARK;
            }
            ball_sprite[y * SPRITE_SIDE + x] = v;
        }
    }
}

/* Reflect a center around its circular lane wall. */
static void bounce(body_t *b)
{
    b->x += b->vx; b->y += b->vy;
    float cx = b->x - SCREEN_W / 2.0f, cy = b->y - SCREEN_H / 2.0f;
    float d = sqrtf(cx * cx + cy * cy);
    if (d > b->lane_r && d > 0.001f) {
        float nx = cx / d, ny = cy / d;
        float dot = b->vx * nx + b->vy * ny;
        if (dot > 0) {
            b->vx -= 2.0f * dot * nx;
            b->vy -= 2.0f * dot * ny;
        }
        b->x = SCREEN_W / 2.0f + nx * b->lane_r;
        b->y = SCREEN_H / 2.0f + ny * b->lane_r;
    }
}

/* ---- Panel bring-up: the same QSPI conversation display.c has, minus LVGL ---- */
static void panel_init(void)
{
    spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_SCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        SCREEN_W * BAND_ROWS * (int)sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io = CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, band_done, NULL);
    io.pclk_hz = 40 * 1000 * 1000;   /* vendor default; the firmware's 80 MHz is a
                                      * per-panel gamble, and this app has no frame
                                      * budget to chase */
    esp_lcd_panel_io_handle_t pio;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &pio));

    co5300_vendor_config_t vendor = { .flags.use_qspi_interface = 1 };
    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(pio, &pc, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, COL_GAP, 0));
    /* The display stays dark until the whole panel has been painted once: the
     * CO5300 init sequence ends with DISPON and the GRAM it turns on with is
     * whatever the previous image left — a board flashed lonely over an orb
     * boots straight into that garbage around the two boxes. */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, false));
}

/* ---- The settings wipe -------------------------------------------------------
 * The orb's owner data lives in the `storage` NVS partition (orb.* namespaces —
 * credentials, tz, name, mode, grants) and the driver's copy in the default
 * `nvs`. net_factory_reset in orb_net.c is the authority on what "wiped" means;
 * this erases exactly those two, from the other side of the boot, so a lonely
 * board is ownerless no matter which door wrote it. Both are attempted even
 * after one fails — one partition left and one gone is neither configured nor
 * clean. A missing partition is not a failure: this app also runs on hardware
 * that never was an orb. */
static void wipe_orb_settings(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_OK || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_deinit();
        e = nvs_flash_erase();
    }
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_INITIALIZED)
        ESP_LOGW(TAG, "settings wipe: default nvs %s", esp_err_to_name(e));
    nvs_flash_deinit_partition("storage");   /* fine if it was never mounted */
    esp_err_t e2 = nvs_flash_erase_partition("storage");
    if (e2 == ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGI(TAG, "settings wipe: no storage partition — nothing to erase here");
    else if (e2 != ESP_OK)
        ESP_LOGW(TAG, "settings wipe: storage %s", esp_err_to_name(e2));
}

/* ---- main ------------------------------------------------------------------- */
void app_main(void)
{
    /* Nothing here stores or joins, and nothing the last owner stored stays. */
    ESP_LOGI(TAG, "lonely: I used to be an Orb. Flash me when you're ready.");
    wipe_orb_settings();

    band = heap_caps_malloc(SCREEN_W * BAND_ROWS * sizeof(uint16_t),
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ball_sprite = heap_caps_malloc(SPRITE_SIDE * SPRITE_SIDE * sizeof(uint16_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!band || !ball_sprite) { ESP_LOGE(TAG, "DMA RAM OOM"); return; }
    band_free = xSemaphoreCreateBinary();
    xSemaphoreGive(band_free);          /* the first band may be composed at once */

    panel_init();
    build_ball();

    /* Half-diagonal of the message box keeps its corners on the glass. */
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

    /* Rule DARK-FIRST: one paint end to end while the glass is still dark. This is
     * the clear the erase-then-redraw version used to get for free, and the
     * first frame the eye is allowed to see is already the scene. */
    repaint_rows(0, SCREEN_H);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    box_t msg_now = body_box(&msg), ball_now = body_box(&ball);

    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(TICK_MS);
    for (;;) {
        bounce(&msg);
        bounce(&ball);
        box_t msg_prev = msg_now, ball_prev = ball_now;
        msg_now = body_box(&msg);
        ball_now = body_box(&ball);
        if (!(msg_now.x == msg_prev.x && msg_now.y == msg_prev.y &&
              ball_now.x == ball_prev.x && ball_now.y == ball_prev.y)) {
            /* Each body repaints the rows between where it was and where it is
             * now — every band carries the final scene, so nothing ever shows
             * blacked out text or a ball mid-move. */
            box_t m = box_union(msg_prev, msg_now);
            box_t b = box_union(ball_prev, ball_now);
            repaint_frame(&m, &b);
        }
        /* The tick waits every time. The skip that used to sit here
         * jumped over the delay whenever a bounce clamped a body still, and the
         * loop ran the rest of the core at full tilt: that is the stall, and
         * vTaskDelayUntil's own clock went stale with it. */
        vTaskDelayUntil(&last, period);
    }
}
