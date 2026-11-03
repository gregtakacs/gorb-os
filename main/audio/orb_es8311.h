#pragma once
/**
 * orb_es8311 — the board's ES8311 codec, playback only (docs/history/sound-plan.md §2, S4).
 *
 * The register sequence is Espressif's own (esp-adf components/esp_codec_dev
 * device/es8311/es8311.c: es8311_open, es8311_set_fs, es8311_start), cut down to the
 * one configuration this board runs: codec as I2S slave, MCLK on its pin at 256 x fs,
 * 16-bit Philips frames at 22,050 Hz, DAC on and ADC left in its power-on mute. It is
 * written against the I2C master driver this tree already uses (imu.c), not the
 * component's legacy-driver control interface: IDF 5.4 aborts at boot when the two
 * I2C drivers meet on one port.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Find the codec on the shared bus and run the playback bring-up for `rate` Hz with
 *  MCLK = 256 x rate. The I2S clocks must already be running: the codec's clock
 *  manager locks to MCLK as it is configured. Leaves the DAC muted at volume 0 dB. */
esp_err_t orb_es8311_init(uint32_t rate);

/** DAC output level, 0.5 dB steps: 0 is -95.5 dB, 191 is 0 dB (the codec's own
 *  register scale). */
esp_err_t orb_es8311_set_volume_reg(uint8_t reg);

/** Soft mute in the DAC, without touching the volume register. */
esp_err_t orb_es8311_mute(bool mute);

/** True when init found the chip and every write landed. */
bool orb_es8311_present(void);

/** The 7-bit address that answered, 0 when none did. */
uint8_t orb_es8311_addr(void);

#ifdef __cplusplus
}
#endif
