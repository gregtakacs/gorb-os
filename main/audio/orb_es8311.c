/* orb_es8311.c — ES8311 playback bring-up on the shared I2C bus. See orb_es8311.h. */
#include "orb_es8311.h"

#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "es8311";

/* The CE strap picks 0x18 or 0x19 (the BSP's ES8311_CODEC_DEFAULT_ADDR is 0x30, the
 * 8-bit form of 0x18). Both are probed, the same way imu.c probes its two straps. */
#define ADDR_LOW   0x18
#define ADDR_HIGH  0x19

static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static bool    s_ok;

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    const uint8_t d[2] = { reg, val };
    return i2c_master_transmit(s_dev, d, sizeof d, 100);
}

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

/* Read-modify-write: keep `keep` bits of the register, OR in `set`. */
static esp_err_t rmw(uint8_t reg, uint8_t keep, uint8_t set)
{
    uint8_t v = 0;
    esp_err_t e = rd(reg, &v);
    if (e != ESP_OK) return e;
    return wr(reg, (uint8_t)((v & keep) | set));
}

/* esp_codec_dev's coeff_div row {mclk, rate, pre_div, pre_multi, adc_div, dac_div,
 * fs_mode, lrck_h, lrck_l, bclk_div, adc_osr, dac_osr}, for the one clock pair this
 * board runs: {5644800, 22050, 1, 1, 1, 1, 0, 0x00, 0xff, 4, 0x10, 0x10}. Another
 * rate means another row, so anything else is refused rather than guessed. */
static esp_err_t clock_22050(void)
{
    esp_err_t e = ESP_OK;
    e |= rmw(0x02, 0x07, (0 << 5) | (0 << 3));   /* pre_div 1, pre_multi x1 */
    e |= wr(0x05, 0x00);                          /* adc_div 1, dac_div 1 */
    e |= rmw(0x03, 0x80, (0 << 6) | 0x10);        /* single speed, adc_osr */
    e |= rmw(0x04, 0x80, 0x10);                   /* dac_osr */
    e |= rmw(0x07, 0xC0, 0x00);                   /* lrck divider high */
    e |= wr(0x08, 0xFF);                          /* lrck divider low: 256 */
    e |= rmw(0x06, 0xE0, 4 - 1);                  /* bclk = mclk / 4 */
    return e;
}

esp_err_t orb_es8311_init(uint32_t rate)
{
    if (rate != 22050) {
        ESP_LOGE(TAG, "no clock row for %lu Hz (only 22050 is wired)", (unsigned long)rate);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* imu_init() creates the shared bus when motion wake is built in, and it runs
     * first in app_main; without it the bus is this file's to create, on the same
     * terms imu.c creates it. */
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_master_get_bus_handle(I2C_NUM_0, &bus);
    if (err != ESP_OK || !bus) {
        const i2c_master_bus_config_t bc = {
            .i2c_port          = I2C_NUM_0,
            .sda_io_num        = PIN_I2C_SDA,
            .scl_io_num        = PIN_I2C_SCL,
            .clk_source        = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = { .enable_internal_pullup = true },
        };
        err = i2c_new_master_bus(&bc, &bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "shared I2C bus: %s — sound is off", esp_err_to_name(err));
            return err;
        }
    }
    for (uint8_t a = ADDR_LOW; a <= ADDR_HIGH && !s_dev; a++) {
        if (i2c_master_probe(bus, a, 20) != ESP_OK) continue;
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = a,
            .scl_speed_hz    = 400000,
        };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) == ESP_OK) s_addr = a;
    }
    if (!s_dev) {
        ESP_LOGW(TAG, "no ES8311 answered at 0x%02X/0x%02X: sound is off", ADDR_LOW, ADDR_HIGH);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t e = ESP_OK;
    /* es8311_open */
    e |= wr(0x0D, 0xFA);
    e |= wr(0x44, 0x08);          /* I2C noise immunity; written twice, as the */
    e |= wr(0x44, 0x08);          /* vendor does, because the first can miss   */
    e |= wr(0x01, 0x30);
    e |= wr(0x02, 0x00);
    e |= wr(0x03, 0x10);
    e |= wr(0x16, 0x24);
    e |= wr(0x04, 0x10);
    e |= wr(0x05, 0x00);
    e |= wr(0x0B, 0x00);
    e |= wr(0x0C, 0x00);
    e |= wr(0x10, 0x1F);
    e |= wr(0x11, 0x7F);
    e |= wr(0x00, 0x80);          /* slave: the S3 drives BCLK/LRCK */
    e |= wr(0x01, 0x3F);          /* MCLK from its pin, not inverted */
    e |= rmw(0x06, (uint8_t)~0x20, 0x00);   /* SCLK not inverted */
    e |= wr(0x13, 0x10);
    e |= wr(0x1B, 0x0A);
    e |= wr(0x1C, 0x6A);
    e |= wr(0x44, 0x08);          /* no ADC reference loop: nothing records */
    /* es8311_set_fs: 16-bit, Philips I2S, then the clock row */
    e |= rmw(0x09, 0xFF, 0x0C);
    e |= rmw(0x0A, 0xFF, 0x0C);
    e |= rmw(0x09, 0xFC, 0x00);
    e |= rmw(0x0A, 0xFC, 0x00);
    e |= clock_22050();
    /* es8311_start, DAC mode */
    e |= wr(0x00, 0x80);
    e |= wr(0x01, 0x3F);
    e |= rmw(0x09, 0xBF, 0x00);   /* DAC serial input on */
    e |= wr(0x17, 0xBF);
    e |= wr(0x0E, 0x02);
    e |= wr(0x12, 0x00);          /* DAC powered */
    e |= wr(0x14, 0x1A);
    e |= rmw(0x14, (uint8_t)~0x40, 0x00);   /* no PDM mic */
    e |= wr(0x0D, 0x01);
    e |= wr(0x15, 0x40);
    e |= wr(0x37, 0x08);
    e |= wr(0x45, 0x00);
    e |= wr(0x32, 0xBF);          /* 0 dB; the engine sets the owner's level */
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 at 0x%02X: a bring-up write failed", s_addr);
        return ESP_FAIL;
    }
    s_ok = true;
    uint8_t id1 = 0, id2 = 0;
    rd(0xFD, &id1);
    rd(0xFE, &id2);
    ESP_LOGI(TAG, "ES8311 at 0x%02X (chip id %02X%02X), DAC on at %lu Hz",
             s_addr, id1, id2, (unsigned long)rate);
    return ESP_OK;
}

esp_err_t orb_es8311_set_volume_reg(uint8_t reg)
{
    if (!s_ok) return ESP_ERR_INVALID_STATE;
    return wr(0x32, reg);
}

esp_err_t orb_es8311_mute(bool mute)
{
    if (!s_ok) return ESP_ERR_INVALID_STATE;
    return rmw(0x31, 0x9F, mute ? 0x60 : 0x00);
}

bool orb_es8311_present(void) { return s_ok; }
uint8_t orb_es8311_addr(void) { return s_addr; }
