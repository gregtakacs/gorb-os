/**
 * orb_sd.h — the microSD socket: 1-bit SPI, FATFS, mounted at /sd.
 *
 * The card is the theme IMPORT path. Theme art is baked on the bench by
 * tools/bake_clock.py and dropped onto the card (or uploaded to it by the
 * config page at /themes); the device installs one into the `assets` partition
 * — see main/art/orb_theme.h. Art is NEVER streamed from here at render time:
 * the bus is 1-bit SPI at ~1.5 MB/s, which caps a 434 KB frame at ~3.4 fps, and
 * docs/usb-sd-msc-plan.md §9 already settled that flash is the only place a
 * full frame bank lives. Do not add a per-frame read here.
 *
 * Hardware constraints that shape this API (verified against the Waveshare
 * schematic and orb-os/src/sdcard.cpp; see the pin block in board_config.h):
 *   - Only MOSI/SCK/MISO/CS are wired (GPIO 1/2/3/41). D1, D2 and card-detect
 *     are NOT connected, so there is no 4-bit mode and — more importantly —
 *     no way to notice a card being swapped. Presence is discovered by trying
 *     to talk to the socket, which is why mounting is lazy and retryable
 *     rather than a boot-time assert.
 *   - The LCD owns SPI2_HOST (display.c), so the card must be SDSPI on
 *     SPI3_HOST.
 *   - SPI-mode init is slow to fail: a socket with no card costs a full
 *     command-timeout ladder per attempt. Everything here is therefore
 *     either off the render path or bounded — nothing in this file may be
 *     called from the LVGL task.
 *
 * Nothing in here is fatal. A board shipped without a card must keep showing
 * the time exactly as it did before the socket existed, which is the same rule
 * main/net/orb_net.c follows for a dead radio.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define ORB_SD_BASE        "/sd"
#define ORB_SD_THEME_DIR   ORB_SD_BASE "/themes"
#define ORB_SD_APPS_DIR    ORB_SD_BASE "/apps"   /* the .orbapp drop zone; must equal ORB_APP_DIR */
#define ORB_SD_NAME_MAX    31    /* one FATFS name, sans directory */
#define ORB_SD_PATH_MAX    128   /* ORB_SD_THEME_DIR + name + spare */

/**
 * Mount the card if it is not already mounted. Idempotent, cheap when
 * mounted, and rate-limited after a failure (see ORB_SD_RETRY_S) so a page
 * polling for the theme list cannot spin the SPI bus on an empty socket.
 *
 * Returns ESP_OK when the card is mounted, or the first mount failure that is
 * still being retried. NEVER fatal: every caller must tolerate ESP_FAIL here.
 */
esp_err_t orb_sd_ensure_mounted(void);

/** Boot-time convenience: ensure_mounted(), logging the card identity once. */
esp_err_t orb_sd_init(void);

/** True while the FATFS volume is mounted and /sd resolves. */
bool orb_sd_mounted(void);

/**
 * Unmount + remount — the only recovery from a card being swapped while the
 * device runs, since the socket has no card-detect line. Safe to call from the
 * httpd task; refuses (ESP_ERR_INVALID_STATE) while an install is in progress.
 */
esp_err_t orb_sd_remount(void);

/** "SDHC 3.7GB @20000kHz" style identity, or "" when unmounted. */
const char *orb_sd_card_id(void);

/** Volume size + free bytes, 0 when unmounted. Both args may be NULL. */
void orb_sd_space(uint64_t *out_total, uint64_t *out_free);

/**
 * Serialises the card. The bus has one CS line and FATFS has one volume
 * context: an upload and an install touching it at once corrupts the FAT, and
 * nothing below the driver will notice. Every path that opens a file or a
 * directory on /sd takes this for the whole operation, not per read.
 */
void orb_sd_lock(void);
void orb_sd_unlock(void);

/**
 * Is `name` safe to use as a single FATFS path segment?
 *
 * Pure, no card needed. FORMAT 2 leaves exactly one browser-supplied string
 * reaching a path — the ?id= of the install and delete endpoints, which is a
 * 12-char lowercase hex theme id — and this is the last thing that looks at it
 * before it becomes a path on /sd. The card's own directory entries are checked
 * with it too (main/sd/orb_files.c), because a file FATFS lists is not a file
 * this device necessarily wrote.
 * Accepted: 1..ORB_SD_NAME_MAX bytes of [A-Za-z0-9._-] with no leading '.'
 * (a leading dot hides the entry from the page and '.'/'..' are the escapes
 * that matter). Everything else — '/', '\', whitespace, control bytes, UTF-8,
 * an embedded ".." that is not the whole name — is refused.
 */
bool orb_sd_name_ok(const char *name);

/**
 * Build "/sd/themes/<name>" into out. Returns false (and writes nothing) if
 * the name fails orb_sd_name_ok() or does not fit. Always NUL-terminates.
 *
 * The theme layer calls this with a theme id, never with a card name: its one
 * caller is theme_file() in main/art/orb_theme.c, which runs after that file
 * matched the id against sha256() of the name read out of the container header.
 * The check stays anyway — a path builder that validates nothing is the exact
 * bug this layer exists to prevent, and the pin costs one strcmp-free loop.
 */
bool orb_sd_theme_path(char *out, size_t outsz, const char *name);
