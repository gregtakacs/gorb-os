/**
 * imu.c — QMI8658 accelerometer bring-up over the shared I2C bus.
 *
 * The register addresses, the WHO_AM_I constant, the CTRL1 value and the CTRL2
 * range/ODR composition are taken from Waveshare's own component for this board
 * (waveshare/qmi8658 2.0.1, qmi8658.c lines 39-89 and include/qmi8658.h), which
 * is the vendor's statement of how this part is driven on this exact board.
 * Nothing here is reconstructed from the datasheet's register table.
 */
#include "imu.h"
#include "board_config.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "imu";

#define REG_WHO_AM_I      0x00
#define REG_CTRL1         0x02
#define REG_CTRL2         0x03
#define REG_CTRL7         0x08
#define REG_AX_L          0x35      /* 6 bytes: Xl Xh Yl Yh Zl Zh, little-endian */

#define CTRL1_RUN         0x60      /* the value the vendor driver writes: address
                                     * auto-increment on, which the 6-byte burst
                                     * read below depends on                     */
#define CTRL2_8G_125HZ    0x26      /* (ACCEL_RANGE_8G << 4) | ACCEL_ODR_125HZ.
                                     * 125 Hz is the lowest ODR that still puts a
                                     * fresh sample in the data registers between
                                     * two 20 Hz polls, and ±8 g is the narrowest
                                     * range whose 4096 LSB/g still resolves the
                                     * tens-of-mg deviation the wake test needs.  */
#define CTRL7_ACC_ON      0x01      /* per-sensor enable: accel only, no gyro    */

/* The two strap addresses. Which one this board uses is not documented, so both
 * are probed and the winner is logged — a chip addressed wrong is indistinguish-
 * able from no chip, and that is exactly the failure worth naming on the serial
 * line once instead of debugging from behaviour. */
#define ADDR_LOW          0x6A
#define ADDR_HIGH         0x6B

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool                  s_present;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t d[2] = { reg, val };
    return i2c_master_transmit(s_dev, d, sizeof(d), 100);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

esp_err_t imu_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* The board's own pull-ups serve touch/RTC/PMU on this bus already;
         * asking for the weak internal ones costs nothing and covers a rail
         * that never got an external pair. */
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c master bus: %s", esp_err_to_name(err));
        return err;
    }

    for (uint8_t try_addr = ADDR_LOW; try_addr <= ADDR_HIGH; try_addr++) {
        /* Probe the address BEFORE creating a device. i2c_master_transmit on an
         * absent chip makes the i2c.master driver log three E-level lines, and an
         * expected miss on the unprobed strap would then look like a real fault
         * in every boot log; the probe returns NOT_FOUND silently. */
        if (i2c_master_probe(s_bus, try_addr, 20) != ESP_OK) continue;

        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = try_addr,
            .scl_speed_hz    = 400000,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(s_bus, &dev_cfg, &dev) != ESP_OK) continue;

        uint8_t id = 0;
        s_dev = dev;                                  /* reg_read/write use s_dev */
        if (reg_read(REG_WHO_AM_I, &id, 1) == ESP_OK && id == IMU_WHO_AM_I_VALUE) {
            ESP_LOGI(TAG, "QMI8658 at 0x%02X (WHO_AM_I 0x%02X)", try_addr, id);
            break;
        }
        /* ACKs but is not the IMU: whatever it is, leave nothing attached to it. */
        i2c_master_bus_rm_device(dev);
        s_dev = NULL;
    }

    if (!s_dev) {
        /* Deliberately not an error: the dial's other 100 % of features do not
         * involve the IMU, and an orb built without the chip populated must not
         * boot-loop over it. The wake source simply becomes the knob alone. */
        ESP_LOGW(TAG, "no QMI8658 answered at 0x%02X/0x%02X: motion wake is off",
                 ADDR_LOW, ADDR_HIGH);
        return ESP_OK;
    }

    /* Bring-up order the vendor driver uses: control registers first, the sensor
     * enable last, so no data is requested from a block still in standby. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(reg_write(REG_CTRL1, CTRL1_RUN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(reg_write(REG_CTRL2, CTRL2_8G_125HZ));
    err = reg_write(REG_CTRL7, CTRL7_ACC_ON);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "accelerator enable failed: %s — motion wake is off",
                 esp_err_to_name(err));
        return ESP_OK;
    }
    s_present = true;
    return ESP_OK;
}

bool imu_present(void)
{
    return s_present;
}

esp_err_t imu_read_accel(int16_t out[3])
{
    if (!s_present || !out) return ESP_ERR_INVALID_STATE;
    uint8_t b[6];
    const esp_err_t err = reg_read(REG_AX_L, b, sizeof(b));
    if (err != ESP_OK) return err;
    out[0] = (int16_t)((uint16_t)b[1] << 8 | b[0]);
    out[1] = (int16_t)((uint16_t)b[3] << 8 | b[2]);
    out[2] = (int16_t)((uint16_t)b[5] << 8 | b[4]);
    return ESP_OK;
}
