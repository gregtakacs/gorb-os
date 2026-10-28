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

/* Forward, not included. IDF defines sdmmc_card_t from an anonymous struct
 * (sd_protocol_types.h:274), so there is no tag to name here, and including
 * sdmmc_cmd.h would put the SD driver's whole type set in front of every file
 * that only asks whether /sd is mounted. The hand-over is therefore opaque: the
 * MSC mode passes it straight to tinyusb_msc_new_storage_sdmmc() and never reads
 * through it. */
typedef void *orb_sd_raw_card_t;

#define ORB_SD_BASE        "/sd"
#define ORB_SD_THEME_DIR   ORB_SD_BASE "/themes"
#define ORB_SD_APPS_DIR    ORB_SD_BASE "/apps"   /* the .orbapp drop zone; must equal ORB_APP_DIR */
/* ORB_SD_NAME_MAX was 31 until per-app theming: a theme stem carries a slug,
 * a legible token, and a 12-hex id, and the volume admits 64
 * (CONFIG_FATFS_MAX_LFN). The host suites lift these #define lines with a
 * one-line regex, so the value line stays single-line and the prose lives
 * above it. */
#define ORB_SD_NAME_MAX    55    /* one FATFS name, sans directory */
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

/** ms the card lock has been held right now, or 0 when nobody holds it. */
int64_t orb_sd_lock_held_ms(void);

/** ms since any task last held the card lock (0 while held; huge if never).
 *  A decider that must not blink at a chunked writer's between-chunk gap
 *  reads this, not lock_held_ms. */
int64_t orb_sd_lock_idle_ms(void);

/* ---- the card's second reader: USB file sharing (D37) ---------------------
 *
 * MTP answers the host with paths inside the mounted volume, so the card has ONE
 * owner here — the FATFS mount the dial and the config page already use — and the
 * host holds file handles through it. Nothing switches hands. What has to be
 * declared is the one thing that ownership does NOT cover: while a host holds
 * handles, the volume must not come down.
 */

/** Hold the volume against unmount for an MTP session (orb_usb_mtp). The card
 *  stays MOUNTED for the whole window — that is the mode's entire point — so this
 *  refuses orb_sd_remount() and nothing else. Held by orb_usb_mtp_start(),
 *  released by orb_usb_mtp_stop(). */
void orb_sd_share_begin(void);
void orb_sd_share_end(void);
bool orb_sd_share_held(void);

/** MSC mode's seam, the inverse of the share claim: the host gets the raw volume
 *  and /sd must NOT be mounted. Claim sets the refusal flag early (the boot ladder
 *  consults it before mounting); hand_to_usb unmounts if needed, attaches the card
 *  raw and hands back the sdmmc_card_t the MSC LUN is built on; hand_back is the
 *  failure path only — the exit gesture is a reboot. */
void orb_sd_usb_claim(void);
bool orb_sd_usb_owned(void);
esp_err_t orb_sd_hand_to_usb(orb_sd_raw_card_t *out_card);
void      orb_sd_hand_back(void);

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

/* ---- the file manager surface (v1.1.18) --------------------------------------
 *
 * The card has always been writable through doors that understand one artifact
 * each — a theme container, an app blob. The file manager is the owner's own
 * view of the volume: browse, upload, mkdir, delete, over ANY file, mirroring
 * what the MTP share already gives a USB host. The safety model is unchanged,
 * only widened to whole paths: every mutation is validated before it resolves,
 * the device-owned directories stay device-owned, and the one card lock covers
 * an operation the way orb_sd.h has always demanded.
 *
 * A "rel" path is card-root-relative, '/'-separated, with no leading or trailing
 * '/'. The empty rel is the root itself — listable, never mutable.
 */

#define ORB_SD_FM_PATH_MAX   127   /* the rel string, NUL excluded */
#define ORB_SD_FM_SEGS_MAX   8     /* nesting the manager may walk */
#define ORB_SD_FM_LIST_MAX   128   /* entries one list call returns */
#define ORB_SD_FM_RM_MAX     1000  /* entries one recursive rm may take */
#define ORB_SD_FM_WRITE_MAX  (128u * 1024u * 1024u) /* declared-size gate */
/* "The socket is mounted on nothing." esp_err.h has no no-media code, and
 * inventing one in the app range is honest: it never crosses this boundary
 * as anything but a value the door handler maps to its own sentence. */
#define ORB_SD_FM_NO_CARD    ((esp_err_t)0x4101)

typedef struct {
    char name[65];   /* FATFS_MAX_LFN is 64 here; longer MTP-made names are
                        truncated in the LISTING only — they still exist on the
                        card, and deleting them from the dial was never offered
                        anyway: a mutation name must pass orb_sd_name_ok */
    uint32_t size;   /* bytes; 0 for directories, which FATFS does not size */
    bool dir;
} orb_sd_fm_ent_t;

/** Validate `rel` and build the absolute "/sd/..." path into out.
 *  For MUTATIONS (read_only=false) every segment must pass orb_sd_name_ok and
 *  the first segment may not be a device-owned directory ("apps", "themes").
 *  For READS (read_only=true) segments only need to be printable, dot-safe and
 *  length-safe — the listing shows whatever a USB host put on the card,
 *  including names this device itself would refuse to write.
 *  Returns false and writes nothing when the path is refused. */
bool orb_sd_fm_path(char *out, size_t outsz, const char *rel, bool read_only);

/** List one directory. Fills ents up to max, sets *n and *truncated.
 *  ORB_SD_FM_NO_CARD unmounted; ESP_ERR_NOT_FOUND no such directory;
 *  ESP_ERR_INVALID_STATE the USB host owns the card right now. */
esp_err_t orb_sd_fm_list(const char *rel, orb_sd_fm_ent_t *ents, int max,
                        int *n, bool *truncated);

/** Create the directory (and any missing parents). Already existing as a
 *  directory is ESP_OK — mkdir is an intention, not a diff. A file standing on
 *  the way is ESP_ERR_INVALID_STATE. */
esp_err_t orb_sd_fm_mkdir(const char *rel);

/** Delete one file, or one directory tree when recursive. Directories that are
 *  not empty refuse without `recursive` (ESP_ERR_DIR_NOT_EMPTY is not an IDF
 *  code; the answer is ESP_ERR_INVALID_STATE). Caps at ORB_SD_FM_RM_MAX entries
 *  taken, refusing the whole operation, not part of it, when the tree is bigger. */
esp_err_t orb_sd_fm_rm(const char *rel, bool recursive, int *removed_out);

/** Write `rel` from a stream of chunks (the httpd task feeds bytes as they
 *  arrive). open checks the mount, the path, the free space and the overwrite
 *  word; data is a pure append under the lock open took; close(COMMIT=false)
 *  unlinks a partial file — but only if commit was never attempted, because
 *  after close(true) unlinks the target, there is nothing to cancel.
 *  A truncated upload leaves a short file the owner can see and delete; the
 *  card has no journal, and a manager that pretended otherwise would be the
 *  second thing on this device to lie about FATFS. */
esp_err_t orb_sd_fm_write_open(const char *rel, bool overwrite, size_t declared, void **h);
esp_err_t orb_sd_fm_write_data(void *h, const char *buf, size_t len);
esp_err_t orb_sd_fm_write_close(void *h, bool commit);
