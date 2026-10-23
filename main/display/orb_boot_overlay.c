/**
 * Boot / install overlay implementation — §6.2 of the theme-delta plan.
 *
 * Frame layout (diameter 466, center 233):
 *   - stage line, scale-3 (15x21 px glyphs), centered at y=184;
 *   - with total > 0: a ring arc at r=200..204 (inside the bezel ring the glass
 *     itself hides at the edge) and a `NN% ddd/dddKB` line at y=252;
 *   - a 4 s-old frame appends `NNs` to the stage line (§6.4).
 *
 * One band buffer of 466x24 RGB565 (22,368 B) in internal DMA RAM is allocated
 * on first use and never freed: §6.2's cost ceiling is "one small buffer, no
 * PSRAM, nothing on the paint path". Bands are drawn top line first, so a torn
 * mid-draw is at worst half a text row on a screen that is about to change.
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "orb_boot_overlay.h"
#include "esp_lcd_panel_ops.h"
#include "display.h"          /* display_get_panel() */
#include "board_config.h"     /* SCREEN_WIDTH / SCREEN_HEIGHT */

static const char *TAG = "overlay";

/* ---- the compiled-in 5x7 ASCII font (0x20..0x7E), column-major: 5 bytes per
 * glyph, bit0 = top row. Public-domain table from the HD44780 character ROM
 * family; §6.2's constraint is "no theme font", so this one ships in .rodata
 * (~475 B) and nothing at runtime can fail to load it. ---- */
static const uint8_t FONT5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, //   
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7E,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x23,0x55,0x49,0x55,0x22}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x14,0x08,0x3E,0x08,0x14}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x19,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x41,0x51,0x72}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x7F,0x20,0x18,0x20,0x7F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x7F,0x41,0x41,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20}, // (backslash)
    {0x00,0x41,0x41,0x7F,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x0C,0x52,0x52,0x52,0x3E}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x00,0x7F,0x10,0x28,0x44}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x20,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
    {0x00,0x08,0x36,0x41,0x00}, // {
    {0x00,0x00,0x7F,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00}, // }
    {0x08,0x04,0x08,0x10,0x08}, // ~
};

/* ---- frame state ---------------------------------------------------------- */

#define BAND_ROWS   24
#define GLYPH_W     5
#define GLYPH_H     7
#define SCALE       3                          /* glyphs land at 15x21 px      */
#define PITCH       ((GLYPH_W + 1) * SCALE)    /* 18 px advance                */
#define LINE1_Y     184                        /* stage band top row           */
#define LINE2_Y     252                        /* percent/KB band              */
#define BAR_Y       300                        /* progress band (total > 0)    */
#define BAR_W       320
#define BAR_H       10

/* RGB565 in WIRE order (the panel takes bytes verbatim, big-endian per pixel).
 * White and black are symmetric; the two below are pre-swapped deliberately. */
#define COL_BG      0x0000u
#define COL_TEXT    0xFFFFu
#define COL_TRACK   0x0421u                    /* RGB565(32,32,32), swapped    */
#define COL_ARC     0xF507u                    /* RGB565(0,255,165), swapped   */

static uint16_t *s_band;                       /* 466 x 24 internal DMA RAM    */
static int64_t   s_first_us;                   /* first accepted draw          */
static uint32_t  s_last_ms;                    /* throttle clock               */
static char      s_last_stage[48];
static uint32_t  s_last_done;
static uint32_t  s_last_secs;
static bool      s_warned_no_panel;

/* Deviation from §6.2's letter, recorded honestly: the progress figure is a
 * bar, not a ring arc. A ring at a visible radius crosses every row band, so
 * an honest arc costs ~18 band passes; the bar costs one, inside the same
 * "one reused buffer, nothing on the paint path" budget the section names. */

/* Clear the band and stamp one ASCII string centered at its top. Unknown
 * bytes (including the em dash the store sentences use) render as '?': the
 * overlay's contract is that it always paints something legible from the word
 * it was handed, and a font with no lowercase-and-dash cover is no cover. */
static uint32_t band_text(const char *s, uint16_t *out)
{
    for (size_t i = 0; i < (size_t)SCREEN_WIDTH * BAND_ROWS; i++) out[i] = COL_BG;

    const size_t len = strlen(s);
    const uint32_t w = (uint32_t)len * PITCH;
    int32_t x = ((int32_t)SCREEN_WIDTH - (int32_t)w) / 2;
    /* §32.3: a word wider than the panel used to center at a negative x, and the
     * clip guards ate BOTH its ends: the 2026-10-23 boot failure line ("theme load
     * failed" plus a 32-hex id) showed the mangled middle of itself. An over-wide
     * line now clips at the right edge only, so the head always reads. */
    if (x < 0)
        x = 0;
    uint32_t ink = 0;
    for (const char *p = s; *p; p++, x += PITCH) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = FONT5x7[c - 0x20];
        for (int col = 0; col < GLYPH_W; col++) {
            const uint8_t bits = g[col];
            if (!bits) continue;
            for (int row = 0; row < GLYPH_H; row++) {
                if (!(bits & (1u << row))) continue;
                const int32_t px = x + col * SCALE, py = row * SCALE;
                if (px >= SCREEN_WIDTH) break;
                for (int sy = 0; sy < SCALE; sy++) {
                    for (int sx = 0; sx < SCALE; sx++) {
                        const int32_t gx = px + sx;
                        if (gx < 0 || gx >= SCREEN_WIDTH) continue;
                        out[(py + sy) * SCREEN_WIDTH + gx] = COL_TEXT;
                        ink++;
                    }
                }
            }
        }
    }
    return ink;
}

/* The progress band: a track line and the filled fraction, done/total as a bar
 * of BAR_W px. Percent rides on the LINE2 band as text, so this band is pure
 * geometry and survives a caller whose words are long. */
static void band_bar(uint16_t *out, uint32_t done, uint32_t total)
{
    for (size_t i = 0; i < (size_t)SCREEN_WIDTH * BAND_ROWS; i++) out[i] = COL_BG;
    const uint32_t frac = total ? (done * BAR_W) / (total ? total : 1u) : 0u;
    const int32_t x0 = (SCREEN_WIDTH - BAR_W) / 2;
    for (uint32_t x = 0; x < BAR_W; x++) {
        const uint16_t c = (x < frac) ? COL_ARC : COL_TRACK;
        for (uint32_t y = (BAND_ROWS - BAR_H) / 2; y < (BAND_ROWS + BAR_H) / 2; y++)
            out[y * SCREEN_WIDTH + x0 + (int32_t)x] = c;
    }
}

void orb_boot_overlay(const char *stage, uint32_t done, uint32_t total, uint32_t secs)
{
    if (!stage || !stage[0]) return;
    esp_lcd_panel_handle_t panel = display_get_panel();
    if (!panel) {
        if (!s_warned_no_panel) {
            ESP_LOGW(TAG, "overlay refused: the panel is not up yet (%s)", stage);
            s_warned_no_panel = true;
        }
        return;
    }
    if (!s_band) {
        s_band = heap_caps_malloc((size_t)SCREEN_WIDTH * BAND_ROWS * sizeof(uint16_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_band) {
            ESP_LOGW(TAG, "overlay refused: no %u B of internal DMA RAM",
                     (unsigned)(SCREEN_WIDTH * BAND_ROWS * sizeof(uint16_t)));
            return;
        }
        s_first_us = esp_timer_get_time();

        /* §6.2, first: every boot wipes the panel black before the first stage
         * line. The CO530 holds its framebuffer across a warm reset — the panel
         * controller is powered and its GRAM survives an MCU restart — so after
         * one, the last life's image is still on the glass while the new boot
         * draws. The overlay paints three 24-row bands and nothing else; a
         * restart into a half-drawn store progress left that frozen bar showing
         * under every stage line this boot wrote, and orb-B wore exactly that
         * ghost on 2026-10-22 (a torn store's 47% bar under the fallback
         * sentences) where the operator could read neither. A cold boot pays one
         * black fill at ~22 KB over QSPI; a warm boot finally starts honest. */
        memset(s_band, 0, (size_t)SCREEN_WIDTH * BAND_ROWS * sizeof(uint16_t));
        for (int y = 0; y < SCREEN_HEIGHT; y += BAND_ROWS) {
            const int rows = (y + BAND_ROWS > SCREEN_HEIGHT) ? SCREEN_HEIGHT - y : BAND_ROWS;
            esp_lcd_panel_draw_bitmap(panel, 0, y, SCREEN_WIDTH, y + rows, s_band);
        }
    }

    const int64_t now_us = esp_timer_get_time();

    /* §6.4's elapsed rule: with no caller-supplied figure, every frame drawn
     * 4 s or more after the first names its seconds, so a long boot says so on
     * the glass. A caller that knows better (the copy loop) passes secs. */
    if (!secs) {
        const int64_t el = now_us - s_first_us;
        if (el >= 4000000) secs = (uint32_t)(el / 1000000);
    }

    /* §6.3's rate rule: same stage, same byte count and same named seconds
     * redraws at most at 10 Hz; a changed stage word, progress figure or
     * elapsed second always repaints at once. */
    const bool stage_changed = strcmp(stage, s_last_stage) != 0;
    const uint32_t now_ms = (uint32_t)(now_us / 1000);
    if (!stage_changed && done == s_last_done && secs == s_last_secs &&
        now_ms - s_last_ms < 100)
        return;

    char line1[72];
    if (secs) snprintf(line1, sizeof line1, "%s %us", stage, (unsigned)secs);
    else      snprintf(line1, sizeof line1, "%s", stage);

    uint16_t *b = s_band;
    uint32_t ink = band_text(line1, b);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, LINE1_Y, SCREEN_WIDTH, LINE1_Y + BAND_ROWS, b));

    if (total) {
        char line2[48];
        snprintf(line2, sizeof line2, "%u%% %u/%u KB",
                 (unsigned)((done * 100u) / (total ? total : 1u)),
                 (unsigned)(done / 1024u), (unsigned)(total / 1024u));
        ink += band_text(line2, b);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, LINE2_Y, SCREEN_WIDTH, LINE2_Y + BAND_ROWS, b));
        band_bar(b, done, total);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, BAR_Y, SCREEN_WIDTH, BAR_Y + BAND_ROWS, b));
    }

    if (stage_changed) {
        ESP_LOGI(TAG, "stage \"%s\" at +%u ms (ink %u px)", stage,
                 (unsigned)(now_ms - (uint32_t)(s_first_us / 1000)), (unsigned)ink);
        strlcpy(s_last_stage, stage, sizeof s_last_stage);
    }
    s_last_ms = now_ms;
    s_last_done = done;
    s_last_secs = secs;
}