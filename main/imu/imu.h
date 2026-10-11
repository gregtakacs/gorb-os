#pragma once
/**
 * imu — the QMI8658 6-axis IMU the Waveshare ESP32-S3-Touch-AMOLED-1.75 carries
 * on the shared I2C bus, and the only chip on that bus this firmware talks to.
 *
 * Only the accelerometer comes up. Nothing here wants the gyro (no compass, no
 * attitude) or the 6-axis fusion features, and CTRL7 enables per sensor, so
 * leaving the rest off keeps their current draw out of a battery orb. The one
 * consumer is main/power/idle_dim, which uses motion as a wake source: the knob
 * tells us the dial was turned, the accelerometer tells us it was picked up.
 *
 * Register values, the WHO_AM_I constant and the ±8 g LSB scale are the ones
 * Waveshare's own component for this board writes (waveshare/qmi8658 2.0.1,
 * qmi8658.c:39-89) — the vendor's numbers for this exact part on this exact
 * board, not a guess from the datasheet's register table. The device is probed
 * at both strap addresses and the one that answers is logged, because the SDO
 * strap is not documented for this board and an IMU at the wrong address fails
 * as silently as an absent one.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMU_WHO_AM_I_VALUE   0x05   /* what register 0x00 must read  */
#define IMU_LSB_PER_G_8G     4096    /* ±8 g range: the scale we run  */

/** Create the I2C bus, probe the IMU, enable its accelerometer.
 *
 *  Never fatal: a board without the chip populated (or with it held in reset)
 *  logs why and the orb runs with knob-only wake. Call after display_init(). */
esp_err_t imu_init(void);

/** True when the IMU answered the probe and its accelerometer is running. */
bool imu_present(void);

/** Read the raw accelerometer triple in ±8 g LSB units (4096 == 1 g).
 *  Any task; it is a single 6-byte burst read. ESP_ERR_INVALID_STATE when the
 *  probe never answered, so a caller cannot mistake an absent chip for still. */
esp_err_t imu_read_accel(int16_t out[3]);

#ifdef __cplusplus
}
#endif
