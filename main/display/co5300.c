/**
 * CO5300 AMOLED QSPI panel driver — ESP-IDF implementation.
 */

#include "co5300.h"
#include "board_config.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_interface.h"

static const char *TAG = "co5300";

#define CO5300_SLPOUT           0x11
#define CO5300_DISPON           0x29
#define CO5300_CASET            0x2A
#define CO5300_PASET            0x2B
#define CO5300_RAMWR            0x2C
#define CO5300_MADCTL           0x36
#define CO5300_PIXFMT           0x3A
#define CO5300_WDBRIGHTNESS     0x51
#define CO5300_WCTRLD1          0x53
#define CO5300_WDBRIGHTNESS_HBM 0x63
#define CO5300_WCE              0x58
#define CO5300_SPIMODECTL       0xC4
#define CO5300_VENDOR_PAGE      0xFE

#define CO5300_RST_DELAY_MS     200
#define CO5300_SLPOUT_DELAY_MS  120

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
} co5300_panel_t;

static esp_err_t co5300_send_cmd(co5300_panel_t *panel, uint8_t cmd,
                                const uint8_t *params, size_t param_len)
{
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(panel->io, cmd, params, param_len),
        TAG, "tx_param cmd=0x%02X", cmd);
    return ESP_OK;
}

static esp_err_t co5300_panel_del(esp_lcd_panel_t *panel)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    free(p);
    return ESP_OK;
}

static esp_err_t co5300_panel_reset(esp_lcd_panel_t *panel)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    ESP_LOGI(TAG, "Hard-resetting CO5300 on GPIO %d", p->reset_gpio);
    gpio_set_level(p->reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(p->reset_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(CO5300_RST_DELAY_MS));
    gpio_set_level(p->reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(CO5300_RST_DELAY_MS));
    return ESP_OK;
}

static esp_err_t co5300_panel_init(esp_lcd_panel_t *panel)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    ESP_LOGI(TAG, "Initialising CO5300 controller");

    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_SLPOUT, NULL, 0), TAG, "SLPOUT");
    vTaskDelay(pdMS_TO_TICKS(CO5300_SLPOUT_DELAY_MS));

    uint8_t param;
    param = 0x00;
    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_VENDOR_PAGE, &param, 1), TAG, "page");

    param = 0x80;
    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_SPIMODECTL, &param, 1), TAG, "spimode");

    param = 0x55;  // RGB565
    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_PIXFMT, &param, 1), TAG, "pixfmt");

    param = 0x20;
    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_WCTRLD1, &param, 1), TAG, "ctrld1");

    param = 0xFF;
    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_WDBRIGHTNESS_HBM, &param, 1), TAG, "hbm");

    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_DISPON, NULL, 0), TAG, "dispon");

    param = 0xD0;
    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_WDBRIGHTNESS, &param, 1), TAG, "brightness");

    param = 0x00;
    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_WCE, &param, 1), TAG, "wce");

    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "CO5300 init complete");
    return ESP_OK;
}

static esp_err_t co5300_panel_draw_bitmap(esp_lcd_panel_t *panel,
                                          int x_start, int y_start,
                                          int x_end, int y_end,
                                          const void *color_data)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    assert(color_data);

    x_start += p->x_gap;
    x_end   += p->x_gap;
    y_start += p->y_gap;
    y_end   += p->y_gap;

    uint8_t buf[4];
    buf[0] = (x_start >> 8) & 0xFF;
    buf[1] = x_start & 0xFF;
    buf[2] = ((x_end - 1) >> 8) & 0xFF;
    buf[3] = (x_end - 1) & 0xFF;
    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_CASET, buf, 4), TAG, "caset");

    buf[0] = (y_start >> 8) & 0xFF;
    buf[1] = y_start & 0xFF;
    buf[2] = ((y_end - 1) >> 8) & 0xFF;
    buf[3] = (y_end - 1) & 0xFF;
    ESP_RETURN_ON_ERROR(co5300_send_cmd(p, CO5300_PASET, buf, 4), TAG, "paset");

    size_t total_px = (size_t)(x_end - x_start) * (y_end - y_start);
    size_t bytes = total_px * (p->fb_bits_per_pixel / 8);

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_color(p->io, CO5300_RAMWR, color_data, bytes),
        TAG, "tx_color");
    return ESP_OK;
}

static esp_err_t co5300_panel_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    return co5300_send_cmd(p, on_off ? CO5300_DISPON : 0x28, NULL, 0);
}

static esp_err_t co5300_panel_sleep(esp_lcd_panel_t *panel, bool mode)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    return co5300_send_cmd(p, mode ? 0x10 : CO5300_SLPOUT, NULL, 0);
}

static esp_err_t co5300_panel_mirror(esp_lcd_panel_t *panel, bool x_mirror, bool y_mirror)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    p->madctl_val = 0;
    if (x_mirror) p->madctl_val |= 0x02;
    if (y_mirror) p->madctl_val |= 0x05;
    return co5300_send_cmd(p, CO5300_MADCTL, &p->madctl_val, 1);
}

static esp_err_t co5300_panel_swap_xy(esp_lcd_panel_t *panel, bool mode)
{
    return ESP_OK;
}

static esp_err_t co5300_panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t co5300_panel_invert_color(esp_lcd_panel_t *panel, bool mode)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    return co5300_send_cmd(p, mode ? 0x21 : 0x20, NULL, 0);
}

esp_err_t esp_lcd_new_panel_co5300(esp_lcd_panel_io_handle_t io,
                                   int reset_gpio,
                                   esp_lcd_panel_handle_t *out_panel)
{
    ESP_RETURN_ON_FALSE(io && out_panel, ESP_ERR_INVALID_ARG, TAG, "null arg");

    co5300_panel_t *panel = calloc(1, sizeof(co5300_panel_t));
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_NO_MEM, TAG, "oom");

    panel->io = io;
    panel->reset_gpio = reset_gpio;
    panel->fb_bits_per_pixel = 16;
    panel->madctl_val = 0;

    if (reset_gpio != -1) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << reset_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio");
    }

    panel->base.del          = co5300_panel_del;
    panel->base.reset        = co5300_panel_reset;
    panel->base.init         = co5300_panel_init;
    panel->base.draw_bitmap  = co5300_panel_draw_bitmap;
    panel->base.disp_on_off  = co5300_panel_disp_on_off;
    panel->base.disp_sleep   = co5300_panel_sleep;
    panel->base.mirror       = co5300_panel_mirror;
    panel->base.swap_xy      = co5300_panel_swap_xy;
    panel->base.set_gap      = co5300_panel_set_gap;
    panel->base.invert_color = co5300_panel_invert_color;

    *out_panel = &panel->base;
    ESP_LOGI(TAG, "CO5300 panel created (%dx%d)", SCREEN_WIDTH, SCREEN_HEIGHT);
    return ESP_OK;
}

esp_err_t co5300_set_brightness(esp_lcd_panel_handle_t panel, uint8_t brightness)
{
    co5300_panel_t *p = __containerof(panel, co5300_panel_t, base);
    return co5300_send_cmd(p, CO5300_WDBRIGHTNESS, &brightness, 1);
}
