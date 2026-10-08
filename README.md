# orb-os-esp-idf

ESP-IDF firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.75 board
(ESP32-S3R8, 466x466 CO5300 AMOLED via QSPI, 8 MB PSRAM, 16 MB flash).

## MVP

Displays a centered digital text clock (HH:MM:SS) on the round AMOLED.
Time is free-running from a fixed epoch at boot (no RTC/SNTP yet).

## Prerequisites

- ESP-IDF v5.4 (installed at ~/esp/esp-idf)
- Source the environment before building:

      source ~/esp/esp-idf/export.sh

- USB-C cable connected to the board (flashes via the native USB-Serial-JTAG peripheral)

## Build & Flash

    # one-time per shell
    source ~/esp/esp-idf/export.sh

    idf.py set-target esp32s3   # already configured
    idf.py build

    # flash over USB-C (find your port with: ls /dev/ttyACM* /dev/ttyUSB*)
    idf.py -p /dev/ttyACM0 flash

    # flash + serial monitor in one step
    idf.py -p /dev/ttyACM0 flash monitor

## Project Structure

    main/
      main.c              app_main entry, NVS init, LVGL task
      board_config.h      pin map + display geometry constants
      display/
        display.c/h       SPI2 QSPI bus + LVGL 9 bring-up
        co5300.c/h        CO5300 panel driver (esp_lcd_panel interface)
      clock/
        clock_view.c/h    centered HH:MM:SS label, 1-s LVGL timer
      idf_component.yml   managed dependency: lvgl/lvgl ^9
      CMakeLists.txt
    partitions.csv        NVS + phy + factory app (16 MB flash)
    sdkconfig.defaults    PSRAM OPI, USB console, LVGL font/color options

## Theme assets (offline baking)

All dial art is baked on the bench by `tools/bake_clock.py`; the device never
decodes PNG or JSON. It emits `assets/clock_assets.bin` (packed RGB565 planes,
embedded via `target_add_binary_data`) plus `assets/clock_assets.h` (geometry +
rect tables). Both are committed so `idf.py build` needs no bake step — but
they are generated: regenerate them with the baker, don't hand-edit them.

Animate the background from a **PNG frame sequence**, not a GIF:

    python3 tools/bake_clock.py <theme_dir> \
        --bg-frames ~/Downloads/skeleton-dial-frames.zip --bg-fps 12.5 \
        --dump-dir assets/preview

`--bg-frames` takes a `.zip` of PNGs, a folder, a glob, or one PNG; frames are
natural-sorted (`frame_2` before `frame_10`). The same delta-rect bank (one
keyframe + per-frame changed rects the device memcpy's over a PSRAM canvas) can
also be fed from a GIF with `--bg-gif`, which keeps its native per-frame timing.

Why PNG is the source of record and a `.bin` never is:

- A GIF re-quantises every frame to a 256-colour palette *before* the bake even
  sees it. This dial's art has ~61k unique colours per frame, so a GIF detour
  costs up to 68/255 per channel; going straight from PNG to RGB565 costs at
  most 7.
- `clock_assets.bin` is already the final 5/6/5-bit quantisation. Re-baking it
  re-quantises lossy data, cannot be re-dithered, carries no alpha, and stores
  no size/fps/byte-order metadata — it is an output, not an asset.
- Both loaders share one packer (`bake_bg_bank`), so PNG and GIF bakes are
  byte-identical modulo the source's own pixels; the GIF regression bake
  reproduces `assets/clock_assets.bin` exactly.

Always check `--dump-dir`'s `stale_NN.png` before flashing: it should be solid
black. Any lit pixel is the panel's canvas holding another frame's art.

## Hardware

| Component | Details |
|-----------|----------|
| MCU       | ESP32-S3R8 dual-core @240 MHz |
| Display   | CO5300 AMOLED, 466x466, QSPI (CS=12, RST=39, SCLK=38, D0-D3=4/5/6/7) |
| I2C       | SDA=15, SCL=14 (shared: touch, IMU, RTC, PMIC) |
| Flash     | 16 MB |
| PSRAM     | 8 MB OPI |
| USB       | Native USB-Serial-JTAG (flash + console over USB-C) |

## Notes

- The clock is driven by an LVGL timer (runs in the LVGL task) to avoid
  touching widgets from a non-LVGL context.
- LVGL is pulled in as a managed component (lvgl/lvgl 9.x) via idf_component.yml.
- CO5300 init sequence ported from the Arduino GFX library used by the
  original orb-os fork.

## Roadmap

- [x] Digital clock (free-running from boot)
- [ ] WiFi + SNTP for real time
- [ ] Touch / IMU input
- [ ] Multiple screens / theming
- [ ] USB Mass-Storage (TinyUSB) access to the enclosed SD card, with a
      knob-driven toggle back to USB-Serial-JTAG for flashing.
      **Shelved — full design in `docs/usb-sd-msc-plan.md`.**
