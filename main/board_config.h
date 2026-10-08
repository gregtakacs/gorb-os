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

// ---------- SPI QSPI speed ----------
#define LCD_SPI_CLOCK_HZ (80 * 1000 * 1000)  // 80 MHz QSPI clock
