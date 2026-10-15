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
 * is what the flicker was. */

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

#define BAND_ROWS   8
#define RGB565_BLACK 0x0000
#define RGB565_WHITE 0xFFFF
#define RGB565_GRAY  0x7BEF  /* message body: not quite white, the ball owns white */
#define RGB565_DARK  0x2104

static esp_lcd_panel_handle_t panel;
static uint16_t *band;              /* SCREEN_W * BAND_ROWS, one composited strip per flush */
static uint16_t *ball_sprite;

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
 * the flush buffer — never a black rect first — is what ends the flicker. */

/* One panel row of glyphs: the message's pixels inside band rows [y0, y0+rows)
 * of the band's columns [x0, x0+bw), drawn as they will be on the glass. */
static void render_message(int x0, int bw, int y0, int rows)
{
    int bx = (int)lroundf(msg.x - msg.w / 2.0f);
    int by = (int)lroundf(msg.y - msg.h / 2.0f);
    for (int line = 0; line < MSG_LINES; line++) {
        int n = (int)strlen(MSG[line]);
        int lw = n * CELL_W - GLYPH_SCALE;      /* trailing inter-cell gap cut */
        int lx = bx + (msg.w - lw) / 2;         /* this line's left edge */
        int ly = by + line * CELL_H;            /* this line's glyph top */
        int sx = (lx > x0) ? lx : x0;
        int ex = (lx + lw < x0 + bw) ? lx + lw : x0 + bw;
        for (int r = ly - y0; r < ly + 7 * GLYPH_SCALE - y0; r++) {
            if (r < 0 || r >= rows) continue;
            int grow = (y0 + r - ly) / GLYPH_SCALE;
            uint16_t *p = band + (size_t)r * bw;
            for (int x = sx; x < ex; x++) {
                int px = x - lx;
                int cell = px / CELL_W;
                int gc = (px % CELL_W) / GLYPH_SCALE;
                char c = MSG[line][cell];
                if (cell < n && gc < 5 && c >= ' ' && c <= '~' &&
                    (font5x7[c - ' '][gc] & (1 << grow)))
                    p[x - x0] = RGB565_GRAY;
            }
        }
    }
}

/* The ball's pixels inside the same band, drawn over anything already there. */
static void render_ball(int x0, int bw, int y0, int rows)
{
    int bs = (int)lroundf(ball.x - ball.w / 2.0f);
    int by = (int)lroundf(ball.y - ball.h / 2.0f);
    int sx = (bs > x0) ? bs : x0;
    int ex = (bs + SPRITE_SIDE < x0 + bw) ? bs + SPRITE_SIDE : x0 + bw;
    int sy = (by > y0) ? by : y0;
    int ey = (by + SPRITE_SIDE < y0 + rows) ? by + SPRITE_SIDE : y0 + rows;
    for (int y = sy; y < ey; y++) {
        const uint16_t *src = ball_sprite + (size_t)(y - by) * SPRITE_SIDE;
        uint16_t *dst = band + (size_t)(y - y0) * bw;
        for (int x = sx; x < ex; x++) dst[x - x0] = src[x - bs];
    }
}

/* Final scene for rows [y0, y0+rows) of columns [x0, x0+bw): black, then the
 * message, then the ball, then one flush. */
static void render_and_flush(int x0, int y0, int w, int rows)
{
    for (int i = 0; i < w * rows; i++) band[i] = RGB565_BLACK;
    render_message(x0, w, y0, rows);
    render_ball(x0, w, y0, rows);
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x0 + w, y0 + rows, band);
}

/* Repaint a rect band by band, clipped to the panel and the band buffer. */
static void repaint(box_t r)
{
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > SCREEN_W) r.w = SCREEN_W - r.x;
    if (r.y + r.h > SCREEN_H) r.h = SCREEN_H - r.y;
    if (r.w <= 0 || r.h <= 0) return;
    for (int y = r.y; y < r.y + r.h; y += BAND_ROWS) {
        int rows = r.y + r.h - y;
        if (rows > BAND_ROWS) rows = BAND_ROWS;
        render_and_flush(r.x, y, r.w, rows);
    }
}

static box_t body_box(const body_t *b)
{
    return (box_t){ .x = (int)lroundf(b->x - b->w / 2.0f),
                    .y = (int)lroundf(b->y - b->h / 2.0f),
                    .w = b->w, .h = b->h };
}

static bool box_overlaps(box_t a, box_t b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static box_t box_union(box_t a, box_t b)
{
    int x2 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
    int y2 = (a.y + a.h > b.y + b.h) ? a.y + a.h : b.y + b.h;
    int x1 = (a.x < b.x) ? a.x : b.x;
    int y1 = (a.y < b.y) ? a.y : b.y;
    return (box_t){ .x = x1, .y = y1, .w = x2 - x1, .h = y2 - y1 };
}

/* One 8-ball sprite: white ring, dark body, white number disc, black "8". */
static void build_ball(void)
{
    int c = SPRITE_SIDE / 2;
    for (int y = 0; y < SPRITE_SIDE; y++) {
        for (int x = 0; x < SPRITE_SIDE; x++) {
            int dx = x - c, dy = y - c;
            int d2 = dx * dx + dy * dy;
            uint16_t v = RGB565_BLACK;
            if (d2 <= (BALL_R + 1) * (BALL_R + 1)) {
                v = RGB565_WHITE;
                if (d2 <= (BALL_R - 2) * (BALL_R - 2)) v = RGB565_DARK;
                if (d2 <= (BALL_R / 2) * (BALL_R / 2)) v = RGB565_WHITE;
                int ax = x - (c - 5), ay = y - (c - 7);
                if (ax >= 0 && ax < 10 && ay >= 0 && ay < 14 &&
                    (font5x7['8' - ' '][ax / 2] >> (ay / 2)) & 1)
                    v = RGB565_DARK;
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

    esp_lcd_panel_io_spi_config_t io = CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
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
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
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

    box_t msg_now = body_box(&msg), ball_now = body_box(&ball);
    if (box_overlaps(msg_now, ball_now))
        repaint(box_union(msg_now, ball_now));   /* one composited pass, ball on top */
    else {
        repaint(msg_now);
        repaint(ball_now);
    }

    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(TICK_MS);
    for (;;) {
        bounce(&msg);
        bounce(&ball);
        box_t msg_prev = msg_now, ball_prev = ball_now;
        msg_now = body_box(&msg);
        ball_now = body_box(&ball);
        if (msg_now.x == msg_prev.x && msg_now.y == msg_prev.y &&
            ball_now.x == ball_prev.x && ball_now.y == ball_prev.y)
            continue;
        /* Each body repaints the union of where it was and where it is now —
         * every flush carries the final scene, so nothing ever shows blacked
         * out text or a ball mid-move. Where the two unions touch, one pass
         * covers both, with the ball on top in its final place. */
        box_t m = box_union(msg_prev, msg_now);
        box_t b = box_union(ball_prev, ball_now);
        if (box_overlaps(m, b))
            repaint(box_union(m, b));
        else {
            repaint(m);
            repaint(b);
        }
        vTaskDelayUntil(&last, period);
    }
}
