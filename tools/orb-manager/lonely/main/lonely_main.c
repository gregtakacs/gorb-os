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
 * No burn-in on an AMOLED: the panel is repainted whole every refresh, so no
 * pixel holds a lit dot across a refresh boundary. All drawing is composited one
 * pass per band: the row is rebuilt black-background + message + ball inside an
 * internal DMA band buffer and flushed once, so the panel never shows a mid-frame
 * erase. The erase-then-redraw two-step this file used to run is what the first
 * flicker was; the second came from writing only what moved, mid-scan.
 *
 * That rewrite shipped four panel rules unkept, and each one shows on the glass:
 * pixels reached the CO5300 low-byte-first (the dark of the ball came up green),
 * flush windows started and spanned odd counts on both axes (rows sheared, so
 * the type read as noise and every sprite left its old pixels behind), the one
 * band buffer was resubmitted while its previous DMA still read it (bands landed
 * over each other), and the panel was never painted end to end, so the GRAM a
 * board powers on with stayed on show around the two boxes. Keeping those four
 * made it readable; the glass then reported two more, both from repainting only
 * what moved. The panel scans GRAM on its own clock, so a mid-scan write shows
 * the message moved and the ball not yet — the flicker. And a frame cost 2 to 15
 * bands depending on where the sprites landed, so the hop rate varied with the
 * drawing — the uneven speed. Six rules hold this image now, all six argued in
 * the comments below and all six held by test/run.sh: wire order, even windows,
 * one band in flight, a whole-panel first paint behind a dark display, the flush
 * burst opened on the TE pulse, and the whole panel repainted at a fixed interval.
 */

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_co5300.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#define PIN_LCD_TE   13   /* panel TE out at vertical blanking (Waveshare J301 pin 11) */
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
static uint16_t *ball_sprite;

/* Rule PIPELINED RING: esp_lcd_panel_draw_bitmap returns as soon as the transfer
 * is queued (driver queue depth 10), so a buffer is spoken for until its
 * trans-done fires. One buffer forces take, compose, submit, wait for the wire,
 * every band: measured on the panel at 74 ms for the 22 ms of wire a whole
 * panel costs, and that 3x gap is the window the panel can catch a half-written
 * frame in. A ring of composited bands lets the next band be built while the
 * wire still carries the last, so a refresh costs max(wire, compose) per band
 * instead of their sum. display.c runs the same shape for the same reason
 * (ping-pong tiles, L1); the difference is that every band here is the final
 * scene for its rows, so a shallow ring is enough and no band is ever partial.
 * The queue depth is the ceiling on this: 4 in flight leaves 6 slots of slack. */
#define BAND_RING 4
static uint16_t *ring[BAND_RING];
static int ring_i;
static uint16_t *band;   /* the ring slot being composed; what the render fns write */
/* Counting: one token per buffer whose DMA is off the wire. */
static SemaphoreHandle_t band_slot;

/* The ring and its tokens, in one place so the host check allocates exactly what
 * app_main does. */
static bool bands_alloc(void)
{
    for (int i = 0; i < BAND_RING; i++) {
        ring[i] = heap_caps_malloc(SCREEN_W * BAND_ROWS * sizeof(uint16_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!ring[i]) return false;
    }
    band_slot = xSemaphoreCreateCounting(BAND_RING, BAND_RING);
    return band_slot != NULL;
}

static bool IRAM_ATTR band_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io; (void)edata; (void)ctx;
    BaseType_t hi = pdFALSE;
    xSemaphoreGiveFromISR(band_slot, &hi);
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
#define TICK_MS 33      /* the unit motion is expressed in; see REFRESH_MS */
#define REFRESH_MS 125  /* 8 hops/s; one refresh costs 32 to 48 ms measured, 22 ms of it wire */

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
 * message, then the ball, then one flush into the ring slot this band owns. The
 * take above it is what makes the slot ours: it returns only on a trans-done, so
 * no DMA is reading the buffer we are about to overwrite. */
static void render_and_flush(int y0, int rows)
{
    xSemaphoreTake(band_slot, portMAX_DELAY);
    band = ring[ring_i];
    ring_i = (ring_i + 1) % BAND_RING;
    for (int i = 0; i < SCREEN_W * rows; i++) band[i] = INK_BLACK;
    render_message(y0, rows);
    render_ball(y0, rows);
    if (esp_lcd_panel_draw_bitmap(panel, 0, y0, SCREEN_W, y0 + rows, band) != ESP_OK)
        xSemaphoreGive(band_slot);   /* nothing queued, so nothing will give it back */
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

/* Rule AT ONCE: the whole panel, every refresh, in 59 even bands. Repainting only
 * what moved was the cheaper trick and the reason the two complaints showed up:
 * the message and the ball landed in different passes with the panel scanning
 * between them, so they did not change together, and the price of a frame swung
 * with the sprites from 2 to 15 bands, so motion speed swung with it. One fixed
 * price buys both — 59 flushes of the final scene every refresh, so the frame the
 * eye is allowed to catch is always whole, and every refresh costs the same.
 * A band is 7456 B and the QSPI link carries 20 MB/s, so 0.37 ms of wire each and
 * 22 ms for the panel. Measured end to end with the ring below: 32 to 48 ms per
 * refresh. REFRESH_MS spends that and gives the rest of the interval back idle,
 * which is also what keeps an AMOLED off a long-stationary image. */
#define BANDS_FULL  ((SCREEN_H + BAND_ROWS - 1) / BAND_ROWS)   /* 59 */
static void flush_all(void)
{
    repaint_rows(0, SCREEN_H);
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

/* Reflect a center around its circular lane wall, advanced by dt ticks of
 * velocity. The caller decides dt: the loop always passes the same constant, so
 * a hop is the same distance and the same interval no matter what the wire cost
 * on the way there. A per-tick step made speed track drawing cost — a long run
 * through the middle ate wall-clock time and read as a slow patch. */
static void bounce(body_t *b, float dt)
{
    b->x += b->vx * dt; b->y += b->vy * dt;
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

/* Rule PHASE: the panel scans its GRAM out on its own clock, so a flush that
 * lands mid-scan shows new pixels above the scan line and old ones below it at
 * the same instant. With two sprites updating in one frame that reads exactly as
 * the reported flicker: the message moves before the ball does, every frame, at
 * whatever point of the scan the write happened to fall in. The CO5300 drives
 * LCD_TE at vertical blanking (its default init already sent TEARON, 0x35), so
 * waiting for the pulse and writing from there down holds the whole frame's
 * updates behind the retrace and they appear together. display.c does the same
 * on this panel; this is that wait_for_te, bounded the same way so a dead TE
 * input costs a late frame rather than a hung app. */
static volatile bool te_live;

static void wait_for_te(void)
{
    if (!te_live) return;
    const int64_t deadline = esp_timer_get_time() + 25000;   /* 25 ms ceiling */
    while (gpio_get_level(PIN_LCD_TE) == 1 && esp_timer_get_time() < deadline) {}
    while (gpio_get_level(PIN_LCD_TE) == 0 && esp_timer_get_time() < deadline) {}
}

/* Decide once, by watching the pin, whether TE is actually there. A stuck input
 * would otherwise pay the full 25 ms bound on every single frame. */
static void te_probe(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_TE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    const int first = gpio_get_level(PIN_LCD_TE);
    const int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < 50000) {              /* 50 ms window */
        if (gpio_get_level(PIN_LCD_TE) != first) { te_live = true; break; }
    }
    ESP_LOGI(TAG, "LCD_TE GPIO%d %s: band flushes %s to blanking",
             PIN_LCD_TE, te_live ? "toggling" : "STUCK (no pulse in 50 ms)",
             te_live ? "locked" : "free-running");
}
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
    te_probe();                       /* panel is scanning: TEARON was in its init */
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

    ball_sprite = heap_caps_malloc(SPRITE_SIDE * SPRITE_SIDE * sizeof(uint16_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!ball_sprite || !bands_alloc()) { ESP_LOGE(TAG, "DMA RAM OOM"); return; }

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

    /* One refresh, one price, one interval. Motion advances a fixed number of ticks
     * per refresh (step, from the refresh interval — so the px/s the velocities
     * describe is the same speed the per-tick version ran at), the panel is paid for
     * in full by rule AT ONCE, and the sleep below gives back only what the refresh
     * did not use. Interval, hop distance and cost therefore stop depending on how
     * the sprites landed, which is what the uneven speed was. The lines under this
     * loop are the evidence: cost min/max over 16 refreshes, so a REFRESH_MS tuned
     * below what the wire can do shows up there before it shows up on the glass. */
    const float step = REFRESH_MS / (float)TICK_MS;
    uint32_t cost_min = 0xFFFFFFFFu, cost_max = 0, refreshes = 0;
    for (;;) {
        bounce(&msg, step);
        bounce(&ball, step);
        const int64_t t0 = esp_timer_get_time();
        wait_for_te();                 /* rule PHASE: start behind the retrace */
        flush_all();                   /* rule AT ONCE: the whole panel, 59 bands */
        const uint32_t cost = (uint32_t)(esp_timer_get_time() - t0);
        if (cost < cost_min) cost_min = cost;
        if (cost > cost_max) cost_max = cost;
        if (++refreshes % 16 == 0) {
            ESP_LOGI(TAG, "refresh %lu us (min %lu max %lu of %d ms budget), TE %s",
                     (unsigned long)cost, (unsigned long)cost_min,
                     (unsigned long)cost_max, REFRESH_MS, te_live ? "locked" : "off");
            cost_min = 0xFFFFFFFFu; cost_max = 0;
        }
        int32_t idle_ms = REFRESH_MS - (int32_t)(cost / 1000u);
        vTaskDelay(pdMS_TO_TICKS(idle_ms > 0 ? idle_ms : 1));
    }
}
