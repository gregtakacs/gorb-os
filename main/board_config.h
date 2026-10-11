#pragma once
/**
 * gorb-os — Board pin definitions
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

// Tear-effect output from the CO5300. Confirmed from the Waveshare
// ESP32-S3-Touch-AMOLED-1.75 schematic: "GPIO13 LCD_TE" on pin 11 of the
// display FPC J301 through a populated 0R link, so the signal is genuinely
// present. The panel's GRAM is scanned out independently of our writes, so a
// write that lands mid-scan shows the top of the new frame and the bottom of
// the old one at once (tearing). TE pulses at vertical blanking and lets us
// "write between scans". The official driver already sends TEARON (0x35) in
// its default init, so the panel emits TE — we only read the pin here.
// Set to -1 to disable the TE wait entirely.
#define PIN_LCD_TE       13

// ---------- Shared I2C bus (touch / IMU / RTC / PMIC / codec) ----------
#define PIN_I2C_SDA     15
#define PIN_I2C_SCL     14

// ---------- microSD, 1-bit SPI mode ----------
// The Waveshare schematic's SD-CARD block labels the socket CMD/CLK/D0/D3
// (generic microSD naming) but wires only the four SPI-mode lines to the S3:
// CMD->MOSI, CLK->SCK, D0->MISO, D3->CS. D1/D2/card-detect are NOT connected,
// so there is no 4-bit SDIO and no hot-plug sense — the card cannot be noticed
// being swapped, which is why main/sd re-attempts the mount on demand instead
// of relying on a detect line. Same pins the Arduino upstream uses
// (orb-os/src/sdcard.cpp), where the card measured ~1.5 MB/s.
//
// These four GPIOs are claimed by nothing else here: the display is 12/39/38/
// 4-7 (+13 TE), I2C is 15/14, the knob is 16/17/18.
#define PIN_SD_MOSI      1
#define PIN_SD_SCK       2
#define PIN_SD_MISO      3
#define PIN_SD_CS       41   // the socket's D3 line

// ---------- Rotary knob (KY-040 on the 8-pin header H2) ----------
// Same wiring as the Arduino reference orb-os/src/knob.cpp: A/B are the
// quadrature phases, SW is the active-low push switch. These three GPIOs are
// free on this board (not claimed by the display above or the I2C peripherals).
#define PIN_KNOB_A      18   // encoder CLK / phase A
#define PIN_KNOB_B      17   // encoder DT  / phase B
#define PIN_KNOB_SW     16   // encoder push (active low)

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
