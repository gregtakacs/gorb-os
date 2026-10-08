#pragma once
/**
 * orb-os-esp-idf — Board pin definitions
 *
 * Waveshare ESP32-S3-Touch-AMOLED-1.75
 * ESP32-S3R8: 8 MB OPI PSRAM, 16 MB flash
 * CO5300 AMOLED 466×466 over QSPI
 */

// ---------- Display: CO5300 QSPI ----------
#define PIN_LCD_CS      12
#define PIN_LCD_RST     39
#define PIN_LCD_SCLK    38
#define PIN_LCD_D0       4
#define PIN_LCD_D1       5
#define PIN_LCD_D2       6
#define PIN_LCD_D3       7

// ---------- Shared I2C bus (touch / IMU / RTC / PMIC / codec) ----------
#define PIN_I2C_SDA     15
#define PIN_I2C_SCL     14

// ---------- Panel geometry ----------
#define SCREEN_WIDTH    466
#define SCREEN_HEIGHT   466

// CO5300 GRAM offset: on this round panel the visible window maps to GRAM
// columns 6..471 (vendor init sets CASET 0x06..0x1DD), rows start at 0.
// Without this offset the rightmost columns show stale GRAM (green splotch)
// and the image is shifted left. Matches the orb-os fork's LCD_COL/ROW_OFFSET.
#define LCD_COL_OFFSET  6
#define LCD_ROW_OFFSET  0

// ---------- SPI QSPI speed ----------
// Vendor CO5300 QSPI default is 40 MHz. 80 MHz works on some boards but
// causes intermittent shear (rows "go slanted") and green smear on the
// trailing edge / right side from QSPI sampling-timing errors. Stay at the
// vendor-validated 40 MHz for reliable pixels.
#define LCD_SPI_CLOCK_HZ (40 * 1000 * 1000)  // 40 MHz QSPI clock (vendor-validated)
