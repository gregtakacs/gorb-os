/**
 * orb_sd.c — see orb_sd.h for the contract and the hardware constraints that
 * forced this shape (1-bit SPI only, no card-detect line, SPI3_HOST only).
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"          // pcTaskGetName, for naming who holds the card lock
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
/* SD_OCR_SDHC_CAP (the HCS bit that separates SDHC from SDSC) is in
 * sd_protocol_defs.h, which sdmmc_cmd.h does NOT pull in — it only reaches
 * sd_protocol_types.h for the struct types. Without this line the SDHC/SDSC
 * split in log_card() does not compile. */
#include "sd_protocol_defs.h"

#include "orb_sd.h"
#include "board_config.h"
#include "display.h"              // tile-ring loan: the SD driver's DMA asks and the display's
                                  // ring come from one internal pool (see orb_sd_lock)

static const char *TAG = "orb_sd";

#if CONFIG_ORB_SD_ENABLE

#define SD_HOST  SPI3_HOST   /* SPI2_HOST belongs to the CO5300 — display.c:702 */

static sdmmc_card_t      *s_card;
static SemaphoreHandle_t  s_lock;
/* Who holds the card mutex and since when, for the waiting log in orb_sd_lock().
 * Only ever written by the task that holds it, so the reader sees either the previous
 * holder or the current one — never a torn value — which is enough to name a suspect. */
static TaskHandle_t       s_lock_holder;
static int64_t            s_lock_taken_us;
static bool               s_mounted;
static esp_err_t          s_last_err = ESP_FAIL;
static int64_t            s_last_try_us;
static char               s_card_id[48];

/* The SPI bus stays initialised across failed mounts: spi_bus_initialize() on a
 * host that is already up answers ESP_ERR_INVALID_STATE, so a retry that tore
 * the bus down and rebuilt it has strictly more ways to fail than one that
 * leaves it alone, and the DMA channel does not need renegotiating per try. */
static bool s_bus_up;

/* Set when the USB host owns the raw volume (MSC mode). Declared up here because
 * mount_now() reads it and the hand-over that sets it is further down. */
static bool s_usb_owns;

static esp_err_t mount_at_khz(uint32_t khz)
{
    if (!s_bus_up) {
        const spi_bus_config_t bus = {
            .mosi_io_num     = PIN_SD_MOSI,
            .miso_io_num     = PIN_SD_MISO,
            .sclk_io_num     = PIN_SD_SCK,
            .quadwp_io_num   = -1,     /* 1-bit SPI: this socket has no D1/D2 */
            .quadhd_io_num   = -1,
            /* One FATFS window is a sector and the driver never asks for more
             * than four at a time, so 4096 covers it with slack. Too small and
             * the failure surfaces from inside the host layer as a rejected
             * transfer, which reads to a bystander as "the card is broken". */
            .max_transfer_sz = 4096,
        };
        const esp_err_t be = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
        if (be != ESP_OK && be != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "spi_bus_initialize(SPI%d): %s",
                     (int)SD_HOST + 1, esp_err_to_name(be));
            return be;
        }
        s_bus_up = true;
    }

    /* SDSPI_HOST_DEFAULT() carries the function pointers; the two fields set
     * below are the ones that decide which GPSPI unit the card lives on and how
     * fast it is driven. host.slot is pinned rather than trusted from the macro
     * because SDSPI_DEFAULT_HOST is an HSPI_HOST alias, and an alias that ever
     * moved under us would put the card on the panel's bus — where it fails as
     * a broken display, not as a broken card. */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SD_HOST;
    host.max_freq_khz = (int)khz;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_HOST;
    slot.gpio_cs = PIN_SD_CS;
    slot.gpio_cd = SDSPI_SLOT_NO_CD;   /* nothing is wired to CD on this board */
    slot.gpio_wp = SDSPI_SLOT_NO_WP;

    const esp_vfs_fat_mount_config_t mc = {
        /* Never true. This is somebody's card — it carries their photos and
         * other people's theme kits — and formatting one that merely holds
         * exFAT or ext4 would be the most destructive line in the firmware. A
         * card we cannot read is reported, not erased. */
        .format_if_mount_failed = false,
        /* Ten, not four. The ceiling is per-volume and every reader shares it,
         * so the HTTP handlers, the theme scanner and the MTP bridge all spend the
         * same four slots (esp_vfs_fat_sdmmc.c:418 opens each file with
         * FF_VOLATOR). Four was enough when only the firmware opened files; MTP
         * fopen()s the same paths the dial reads, so a host paging through a theme
         * list can take the last slot and leave a thumbnail read answering
         * FR_NOT_ENOUGH_CORE. The FatFs objects come from the FATFS ptr window in
         * orb_config.h, not from this number, so the ten slots cost DRAM only when
         * open. */
        .max_files              = 10,
        .allocation_unit_size   = 16 * 1024,
    };

    return esp_vfs_fat_sdspi_mount(ORB_SD_BASE, &host, &slot, &mc, &s_card);
}


/* Takes the card rather than reading s_card, because the MSC hand-over has a card
 * in hand and no mount: s_card means "the mounted volume" everywhere else in this
 * file, and letting a raw handle live there would make the status page and the
 * rescan refusal disagree about what is on the socket. */
static void log_card(const sdmmc_card_t *card, const char *verb)
{
    /* Identity in one line, in the order a person at the wall needs it: what the
     * card calls itself, how big, and what clock it negotiated. The kHz is there
     * because a card that fell back to 4 MHz is slow by agreement rather than by
     * fault, and that is the difference between "try another card" and the
     * "socket is broken" reading. */
    const int mb = (int)((int64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    const char *kind = card->is_mmc ? "MMC"
                     : (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC" : "SDSC";
    snprintf(s_card_id, sizeof(s_card_id), "%.8s %s %.1fGB @%lukHz",
             card->cid.name[0] ? card->cid.name : "card", kind,
             mb / 1024.0, (unsigned long)card->max_freq_khz);
    s_card_id[sizeof(s_card_id) - 1] = '\0';
    ESP_LOGI(TAG, "%s %s", verb, s_card_id);
}

static esp_err_t mount_now(void)
{
    s_last_try_us = esp_timer_get_time();

    /* The USB host owns the raw volume. Mounting FatFs over it would put two
     * writers on one filesystem, which is the divergence this mode exists to
     * prevent, so the answer is a flat refusal and not a retry — and the retry
     * throttle above is left untouched, because "the host has it" does not go
     * away on a timer and must not cost a bus attempt every time the theme page
     * asks. */
    if (s_usb_owns) return ESP_ERR_INVALID_STATE;

    /* The speed ladder is the Arduino original's (orb-os/src/sdcard.cpp), which
     * lost a day on the bench to exactly the card it saves: 20 MHz is what a
     * modern card manages, but an SDSC card will power up, answer the init
     * commands at the 400 kHz probe clock, and then fail the data phase with no
     * hint as to why. Falling through 10 to 4 MHz turns "no card detected" into
     * a card that works slowly. */
    const uint32_t ladder[3] = { (uint32_t)CONFIG_ORB_SD_SPI_MAX_KHZ, 10000, 4000 };

    esp_err_t e = ESP_FAIL;
    for (unsigned i = 0; i < 3; i++) {
        e = mount_at_khz(ladder[i]);
        if (e == ESP_OK) break;
        /* s_card is only written by the success path, so a failed attempt has
         * already run its own cleanup (host deinit, diskio unregister, VFS
         * unregister) inside esp_vfs_fat_sdspi_mount. Nothing to undo here —
         * and undoing it wrong is how the next rung fails for a reason that has
         * nothing to do with the card. */
        ESP_LOGD(TAG, "mount at %" PRIu32 " kHz: %s", ladder[i], esp_err_to_name(e));
    }
    if (e != ESP_OK) {
        s_last_err = e;
        ESP_LOGW(TAG, "no usable card on %s (tried %d/10/4 kHz): %s",
                 ORB_SD_BASE, CONFIG_ORB_SD_SPI_MAX_KHZ, esp_err_to_name(e));
        return e;
    }

    s_mounted  = true;
    s_last_err = ESP_OK;
    log_card(s_card, "mounted at /sd");

    /* The one directory the device owns, created at mount rather than at first
     * upload: a card in a locked adapter, or one whose controller has worn out,
     * says so here at boot instead of under a progress bar. */
    /* The two directories the device owns, created at mount rather than at first
     * upload: a card in a locked adapter, or one whose controller has worn out,
     * says so here at boot instead of under a progress bar. The apps drop zone
     * needs the same create as the themes dir — the browser upload opens
     * /sd/apps/upload.part and FATFS refuses to create a file whose PARENT dir is
     * absent (FR_NO_PATH). It is ORB_APP_DIR, the same path orb_app_scan registers,
     * so the walk, the install's resolve-by-stem, and the upload all agree on one
     * directory. */
    if (mkdir(ORB_SD_THEME_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: %s — theme uploads will fail",
                 ORB_SD_THEME_DIR, strerror(errno));
    }
    if (mkdir(ORB_SD_APPS_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: %s — app uploads will fail",
                 ORB_SD_APPS_DIR, strerror(errno));
    }
    return ESP_OK;
}

esp_err_t orb_sd_ensure_mounted(void)
{
    orb_sd_lock();

    if (s_mounted) {
        orb_sd_unlock();
        return ESP_OK;
    }

    /* Bounded retry, not a ladder per poll. The theme page lists /sd while a
     * person browses, and one attempt costs the command timeout at three clock
     * rates — unbounded, a browser tab slowly starves the SPI bus and the httpd
     * task with it. */
    const int64_t now = esp_timer_get_time();
    if (s_last_try_us != 0 &&
        now - s_last_try_us < (int64_t)CONFIG_ORB_SD_RETRY_S * 1000000) {
        orb_sd_unlock();
        return s_last_err;
    }

    const esp_err_t e = mount_now();
    orb_sd_unlock();
    return e;
}

esp_err_t orb_sd_init(void)
{
    const esp_err_t e = orb_sd_ensure_mounted();
    if (e == ESP_OK) {
        /* Internal heap after the mount is the number that decides whether
         * FATFS is affordable next to esp_wifi on a board that is mostly LVGL
         * buffers — the same reason orb_net.c logs a heap line per stage. */
        ESP_LOGI(TAG, "internal heap %" PRIu32 " kB free, largest block %" PRIu32 " kB",
                 (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (uint32_t)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    } else {
        /* Loud once, then quiet. Most of these devices spend their life on a
         * wall with no card in the socket, and the clock is the product. */
        ESP_LOGW(TAG, "SD unavailable (%s) — install from card is off", esp_err_to_name(e));
    }
    return e;
}

/* ---- holding the volume against unmount while a host shares it -----------
 *
 * Set for the whole of an MTP window by orb_usb_mtp_start() and cleared by
 * orb_usb_mtp_stop(). The card keeps exactly one owner in this mode — the FATFS
 * mount everything else already uses — and the host reaches files through it, so
 * there is no hand-over to guard and no mount refusal to make. What this blocks
 * is the unmount below, which would pull the volume out from under file handles
 * the host is still holding.
 *
 * Deliberately not refcounted: one session per boot is the design, and a second
 * claim would be a bug worth losing the release over. Neither setter takes the
 * card lock — that guards the mount table, not this flag — and locking here would
 * deadlock the release path, which runs while the worker may still be inside a
 * file operation.
 */
static bool s_share_held;

void orb_sd_share_begin(void) { s_share_held = true; }
void orb_sd_share_end(void)   { s_share_held = false; }
bool orb_sd_share_held(void)  { return s_share_held; }

/* ------------------------------------------------------------------ the hand-over
 *
 * The other USB mode's seam, and it points the opposite way from the share claim
 * above. MTP needs /sd mounted and refuses only an unmount; MSC needs /sd GONE,
 * because a LUN is raw sectors of the same blocks FatFs keeps its FAT in, and two
 * owners of one filesystem is the failure this mode exists to avoid
 * (docs/usb-sd-msc-plan.md §8). So while the host owns the card, mount_now()
 * refuses — on a flag set BEFORE the ladder runs, because a boot that consumed the
 * one-shot must not mount the volume it is about to hand away.
 *
 * Exit is a reboot, so nothing hands the card back and keeps running: the console
 * returns no other way (§6.1). orb_sd_hand_back() serves only the failure path, so
 * a start that cannot attach the card leaves the volume to the dial.
 */
static sdmmc_card_t      *s_raw_card;
static sdspi_dev_handle_t s_raw_dev = -1;
static bool               s_raw_host_inited;

void orb_sd_usb_claim(void) { s_usb_owns = true; }
bool orb_sd_usb_owned(void) { return s_usb_owns; }

/* The bus set-up is the mount path's, unchanged: same four wires, same SPI3, same
 * 4 KiB window — the card does not change shape when a host is reading it. What
 * differs is everything above the host layer: no FatFs context and no VFS
 * registration, only a card structure the MSC class can read sectors through. */
static esp_err_t raw_attach_bus(void)
{
    if (!s_bus_up) {
        const spi_bus_config_t bus = {
            .mosi_io_num     = PIN_SD_MOSI,
            .miso_io_num     = PIN_SD_MISO,
            .sclk_io_num     = PIN_SD_SCK,
            .quadwp_io_num   = -1,
            .quadhd_io_num   = -1,
            .max_transfer_sz = 4096,
        };
        const esp_err_t be = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
        if (be != ESP_OK && be != ESP_ERR_INVALID_STATE) return be;
        s_bus_up = true;
    }
    return ESP_OK;
}

/* Mirrors IDF's own esp_vfs_fat_sdspi_mount down to sdmmc_card_init and stops
 * there (vfs_fat_sdmmc.c:356-378). Two steps there are easy to get wrong: the host
 * driver is inited once, and the handle sdspi_host_init_device() returns REPLACES
 * host.slot before the card is asked anything — pass the bare SPI unit and
 * sdmmc_card_init fails exactly as an empty socket does. */
static esp_err_t raw_attach_at_khz(uint32_t khz, sdmmc_card_t **out)
{
    const esp_err_t be = raw_attach_bus();
    if (be != ESP_OK) return be;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SD_HOST;
    host.max_freq_khz = (int)khz;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_HOST;
    slot.gpio_cs = PIN_SD_CS;
    slot.gpio_cd = SDSPI_SLOT_NO_CD;
    slot.gpio_wp = SDSPI_SLOT_NO_WP;

    if (!s_raw_host_inited) {
        const esp_err_t ie = host.init();
        if (ie != ESP_OK && ie != ESP_ERR_INVALID_STATE) return ie;
        s_raw_host_inited = true;
    }

    sdspi_dev_handle_t dev = -1;
    esp_err_t e = sdspi_host_init_device(&slot, &dev);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "sdspi_host_init_device: %s", esp_err_to_name(e));
        return e;
    }

    sdmmc_host_t attached = host;
    attached.slot = dev;

    sdmmc_card_t *card = calloc(1, sizeof(*card));
    if (card == NULL) {
        (void)sdspi_host_remove_device(dev);
        return ESP_ERR_NO_MEM;
    }
    e = sdmmc_card_init(&attached, card);
    if (e != ESP_OK) {
        free(card);
        (void)sdspi_host_remove_device(dev);
        return e;
    }
    s_raw_dev = dev;
    *out = card;
    return ESP_OK;
}

esp_err_t orb_sd_hand_to_usb(orb_sd_raw_card_t *out_card)
{
    orb_sd_lock();

    /* The volume may be mounted when the press comes at runtime rather than at
     * boot. Coming down is the same call the rescan route makes and is safe for
     * the same reason: nothing else holds the card open, because every reader
     * goes through this mutex. */
    if (s_mounted) {
        const esp_err_t ue = esp_vfs_fat_sdcard_unmount(ORB_SD_BASE, s_card);
        if (ue != ESP_OK) {
            orb_sd_unlock();
            ESP_LOGE(TAG, "cannot hand the card to USB: unmount said %s",
                     esp_err_to_name(ue));
            return ue;
        }
        s_card = NULL;
        s_mounted = false;
    }
    s_usb_owns = true;

    /* The mount's own speed ladder, for its own reason: an SDSC card that answers
     * the probe at 400 kHz and fails the data phase at 20 MHz is not a broken
     * card, and it should not cost the host a missing disk. */
    const uint32_t ladder[3] = { (uint32_t)CONFIG_ORB_SD_SPI_MAX_KHZ, 10000, 4000 };
    sdmmc_card_t  *card = NULL;
    esp_err_t e = ESP_FAIL;
    for (unsigned i = 0; i < 3; i++) {
        e = raw_attach_at_khz(ladder[i], &card);
        if (e == ESP_OK) break;
    }
    if (e != ESP_OK) {
        s_usb_owns = false;
        s_last_err = e;
        orb_sd_unlock();
        ESP_LOGE(TAG, "no card to hand over (%s)", esp_err_to_name(e));
        return e;
    }

    /* Deliberately NOT s_card: that pointer means "the mounted volume" to the
     * status page and to the unmount in orb_sd_remount(), and neither is true
     * here. The MSC LUN lives in s_raw_card, which only hand_back() touches. */
    s_raw_card = card;
    s_last_err = ESP_OK;
    log_card(card, "attached raw for the USB host");
    orb_sd_unlock();
    ESP_LOGW(TAG, "card handed to the USB host — /sd stays unmounted while it holds it");
    *out_card = card;
    return ESP_OK;
}

void orb_sd_hand_back(void)
{
    orb_sd_lock();
    if (s_raw_card != NULL) { free(s_raw_card); s_raw_card = NULL; }
    if (s_raw_dev >= 0) { (void)sdspi_host_remove_device(s_raw_dev); s_raw_dev = -1; }
    if (s_raw_host_inited) { (void)sdspi_host_deinit(); s_raw_host_inited = false; }
    s_usb_owns = false;
    orb_sd_unlock();
}



esp_err_t orb_sd_remount(void)
{
    /* Refused before the lock, never queued behind it: a route that blocked here
     * would look like a hung card rather than a shared one. The refusal is worth
     * its own line because the volume is mounted and healthy, which is exactly why
     * an unmount looks safe to the code asking for it. A rescan issued over WiFi
     * mid-copy would take the FATFS mount out from under open MTP file handles,
     * and the host's next read lands on a card re-enumerated underneath it. */
    if (s_share_held || s_usb_owns) return ESP_ERR_INVALID_STATE;
    orb_sd_lock();

    if (s_mounted) {
        const esp_err_t ue = esp_vfs_fat_sdcard_unmount(ORB_SD_BASE, s_card);
        if (ue != ESP_OK) {
            /* Left mounted on purpose: a volume that failed to come down is
             * still the volume the page should report as present. Saying it is
             * gone would make /sd vanish under someone reading its files. */
            ESP_LOGE(TAG, "unmount failed (%s) — left mounted", esp_err_to_name(ue));
            orb_sd_unlock();
            return ue;
        }
        s_card = NULL;
        s_mounted = false;
        s_card_id[0] = '\0';
    }

    s_last_try_us = 0;              /* a deliberate rescan ignores the back-off */
    const esp_err_t e = mount_now();
    orb_sd_unlock();
    return e;
}

bool orb_sd_mounted(void)        { return s_mounted; }
const char *orb_sd_card_id(void) { return s_mounted ? s_card_id : ""; }

void orb_sd_space(uint64_t *out_total, uint64_t *out_free)
{
    if (out_total) *out_total = 0;
    if (out_free)  *out_free  = 0;
    if (!s_mounted) return;
    /* Deliberately NOT under orb_sd_lock(). An upload holds that mutex for the
     * seconds a 3.7 MB theme takes to land, and the theme page asks for free space
     * while an upload is in flight — locking here turns a progress bar into a request
     * that hangs until the upload finishes. Safe without it: FatFs is built with
     * FF_FS_REENTRANT=1 (ffconf.h:326), so the volume's own mutex covers a concurrent
     * f_statfs. The orb_sd_lock contract is for whole-file operations that must not
     * interleave, not for a metadata read. */
    esp_vfs_fat_info(ORB_SD_BASE, out_total, out_free);
}

void orb_sd_lock(void)
{
    /* Creation retried, not trusted: the IPC block this mutex wants is ~100 B of
     * internal DMA memory, and an unprovisioned boot has passed that pool through
     * zero on the way here (bench 2026-10-27: dma_free=19 after esp_wifi_start,
     * creation returned NULL, and the take below asserted on the NULL — a reboot
     * in the card module for a card that was never in the socket). The heap
     * churns; the loop below already waits a second at a time on a held lock, so
     * the same patience applies to the lock's own creation. */
    while (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            ESP_LOGE(TAG, "card lock: out of memory for the mutex — retrying in 1 s");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    /* A take that blocks is the one card failure the owner can do nothing about: the
     * theme page simply stops answering, the dial keeps drawing, and ping keeps replying,
     * so nothing points at the card. Name the task that holds it and for how long, then
     * keep waiting — the holder finishes, and a reboot is a worse outcome than a wait. */
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (s_lock_holder == me) {
        /* Not a hypothetical: orb_theme_scan() used to walk the directory holding this
         * mutex and validate each entry through card_open(), which took it again. The
         * second take on a non-recursive mutex never returns and kills every route
         * behind the one httpd worker. Report it, then keep waiting rather than
         * continuing without the lock — a second task on the volume is the corruption
         * this mutex exists to prevent. tools/host_sd_test.py:lock_lint() is what keeps
         * it from shipping. */
        ESP_LOGE(TAG, "\"%s\" took the card lock twice — it is not recursive, so this "
                     "wait never ends", pcTaskGetName(me));
    }
    const int64_t t0 = esp_timer_get_time();
    int64_t next_report = t0 + 5000000;
    while (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        const int64_t now = esp_timer_get_time();
        if (now >= next_report) {
            ESP_LOGW(TAG, "waiting for the card: held by \"%s\" for %lld ms",
                     s_lock_holder ? pcTaskGetName(s_lock_holder) : "nobody",
                     (long long)(now - s_lock_taken_us) / 1000);
            next_report = now + 5000000;
        }
        if ((now - t0) / 1000000 >= 60) {
            ESP_LOGE(TAG, "still waiting on the card lock after %lld ms (holder \"%s\")",
                     (long long)(now - t0) / 1000,
                     s_lock_holder ? pcTaskGetName(s_lock_holder) : "nobody");
            next_report = now + 30000000;      // say it again a minute later, not per second
        }
    }
    s_lock_holder = me;
    s_lock_taken_us = esp_timer_get_time();

    /* The display's tile ring travels with the card lock.
     *
     * Why here, and not at each writer: the SD driver asks the internal DMA
     * pool for small blocks at mount (sdmmc_common.c:401) and on every unaligned
     * write (sdmmc_cmd.c:477), and with the radio associated that pool is
     * fragmented below the ask -- measured 125 B refused with 4,315 B free. The
     * ring is the largest freeable block on the board (2 x 18,640 B of
     * MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA), and the lock's own lifetime is
     * exactly the window that needs it: held for one mount, one upload, one
     * install. Putting the loan here means a card path cannot forget it.
     *
     * What it costs: the flush path drops frames while the ring is out (see
     * display_dma_borrow), so the glass is a still image for as long as the
     * card is held. That is already the deal this mutex makes -- the clock's
     * tick skips while the card is locked precisely because a fatfs read is a
     * 4-second business -- and the CO5300 holds its own GRAM, so "still" is not
     * "blank". The whole page is re-sent from the mirror on the return.
     *
     * Bounded and safe against the LVGL task: the loan's in-flight drain is
     * capped, and a drain that does not settle REFUSES the loan rather than
     * waiting, so a card path that somehow runs ahead of a flush cannot hang. */
    display_dma_borrow();
}

void orb_sd_unlock(void)
{
    /* Give the ring back BEFORE releasing the mutex, so the next holder never
     * sees the card without its DMA. If the memory did not come back (the card
     * kept it), display_dma_return clears the loan anyway and the flush path
     * re-funds the ring on its own next frame. */
    display_dma_return();
    if (s_lock) {
        s_lock_holder = NULL;
        s_lock_taken_us = 0;
        xSemaphoreGive(s_lock);
    }
}

/* The two pure helpers below are lifted verbatim by tools/host_tz_test.py, so
 * they must stay free of the card, the lock, and the CONFIG_ORB_SD_ENABLE body
 * above. That is not cosmetic: FORMAT 2 leaves the theme endpoints handing this
 * layer only a ?id= the browser typed, and the card's own directory entries come
 * through it too, so this guard is the last thing between a crafted string and
 * the rest of the volume — the one part of the SD feature a radio-less,
 * card-less bench CAN prove, so it is proved here instead of at a wall. */
bool orb_sd_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    if (name[0] == '.') return false;        /* ".", "..", and hidden entries */
    size_t n = 0;
    for (const char *p = name; *p; ++p, ++n) {
        if (n >= ORB_SD_NAME_MAX) return false;
        const char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;                    /* '/', '\', space, UTF-8, control */
        }
    }
    return true;
}

bool orb_sd_theme_path(char *out, size_t outsz, const char *name)
{
    if (out == NULL || outsz == 0) return false;
    if (!orb_sd_name_ok(name)) { out[0] = '\0'; return false; }
    const int n = snprintf(out, outsz, ORB_SD_THEME_DIR "/%s", name);
    if (n < 0 || (size_t)n >= outsz) { out[0] = '\0'; return false; }
    return true;
}

/* ---- the file manager surface (v1.1.18) --------------------------------------
 *
 * VFS stdio, not ff.h directly: the volume is already esp_vfs_fat-registered at
 * /sd, and orb_sd.h's one-lock-per-operation rule applies here per chunk the
 * way the app-upload door does it — a lock held across a 100 MB radio transfer
 * would starve the theme store and the dial's own card reads, while a lock
 * around each fwrite serializes exactly what FatFs is not reentrant about.
 */
#include <dirent.h>
#include <unistd.h>

/* Read-side segment rule: an MTP host may have written spaces and odd but
 * legal FAT names, and the listing must show them. Printable ASCII, no leading
 * dot (the same hidden-entry rule the mutations and orb_files.c already keep —
 * a file the device will not name cannot be half-reachable by browsing it), and
 * none of the characters FAT itself forbids, which readdir can still hand back
 * from a corrupted directory. Length capped at the read name, 64. */
static bool fm_seg_ok_read(const char *seg, size_t sl)
{
    if (sl == 0 || sl > 64 || seg[0] == '.') return false;
    for (size_t i = 0; i < sl; i++) {
        const char c = seg[i];
        if (c < 0x20 || c > 0x7E) return false;
        if (c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
    return true;
}

bool orb_sd_fm_path(char *out, size_t outsz, const char *rel, bool read_only)
{
    if (out == NULL || outsz == 0 || rel == NULL) return false;
    const size_t len = strlen(rel);
    if (len > ORB_SD_FM_PATH_MAX) return false;
    if (len && (rel[0] == '/' || rel[len - 1] == '/' || strstr(rel, "//")))
        return false;                       /* traversal escapes live here */

    char tmp[ORB_SD_FM_PATH_MAX + 1];
    memcpy(tmp, rel, len + 1);
    int segs = 0;
    for (char *s = tmp; s && *s; ) {
        char *e = strchr(s, '/');
        const size_t sl = e ? (size_t)(e - s) : strlen(s);
        const char save = s[sl];
        s[sl] = '\0';
        if (!(read_only ? fm_seg_ok_read(s, sl) : orb_sd_name_ok(s)))
            return false;                   /* tmp is dead either way */
        if (++segs > ORB_SD_FM_SEGS_MAX) return false;
        if (segs == 1 && !read_only &&
            ((sl == 4 && memcmp(s, "apps", 4) == 0) ||
             (sl == 6 && memcmp(s, "themes", 6) == 0)))
            return false;                   /* the stores keep their own doors */
        if (!e) break;
        s[sl] = save;
        s = e + 1;
    }
    const int n = rel[0] ? snprintf(out, outsz, ORB_SD_BASE "/%s", rel)
                         : snprintf(out, outsz, ORB_SD_BASE);
    if (n < 0 || (size_t)n >= outsz) { out[0] = '\0'; return false; }
    return true;
}

/* stat one child of an absolute dir, for list and rm-walk alike. */
static bool fm_child_stat(const char *abs_parent, const char *name, struct stat *st)
{
    char child[ORB_SD_FM_PATH_MAX + 96];
    if (snprintf(child, sizeof child, "%s/%s", abs_parent, name) >= (int)sizeof child)
        return false;
    return stat(child, st) == 0;
}

esp_err_t orb_sd_fm_list(const char *rel, orb_sd_fm_ent_t *ents, int max,
                        int *n, bool *truncated)
{
    if (n) *n = 0;
    if (truncated) *truncated = false;
    if (ents == NULL || max <= 0) return ESP_ERR_INVALID_ARG;
    char abs[ORB_SD_FM_PATH_MAX + 32];
    if (!orb_sd_fm_path(abs, sizeof abs, rel ? rel : "", true))
        return ESP_ERR_INVALID_ARG;
    const esp_err_t me = orb_sd_ensure_mounted();
    if (me != ESP_OK) return me;            /* ORB_SD_FM_NO_CARD until a card answers */
    if (orb_sd_usb_owned()) return ESP_ERR_INVALID_STATE;

    orb_sd_lock();
    DIR *d = opendir(abs);
    if (!d) {
        orb_sd_unlock();
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    int cnt = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (cnt == max) { if (truncated) *truncated = true; break; }
        struct stat st;
        if (!fm_child_stat(abs, de->d_name, &st)) continue;   /* vanished mid-list */
        orb_sd_fm_ent_t *en = &ents[cnt++];
        size_t nl = strlen(de->d_name);
        if (nl >= sizeof en->name) nl = sizeof en->name - 1;  /* header says so */
        memcpy(en->name, de->d_name, nl);
        en->name[nl] = '\0';
        en->dir  = S_ISDIR(st.st_mode);
        en->size = en->dir ? 0 : (uint32_t)st.st_size;
    }
    closedir(d);
    orb_sd_unlock();
    if (n) *n = cnt;
    return ESP_OK;
}

esp_err_t orb_sd_fm_mkdir(const char *rel)
{
    if (rel == NULL || rel[0] == '\0') return ESP_ERR_INVALID_ARG;  /* root exists */
    char abs[ORB_SD_FM_PATH_MAX + 32];
    if (!orb_sd_fm_path(abs, sizeof abs, rel, false)) return ESP_ERR_INVALID_ARG;
    const esp_err_t me = orb_sd_ensure_mounted();
    if (me != ESP_OK) return me;
    if (orb_sd_usb_owned()) return ESP_ERR_INVALID_STATE;

    orb_sd_lock();
    esp_err_t e = ESP_OK;
    char walk[ORB_SD_FM_PATH_MAX + 32];
    int wlen = snprintf(walk, sizeof walk, ORB_SD_BASE);
    char tmp[ORB_SD_FM_PATH_MAX + 1];
    memcpy(tmp, rel, strlen(rel) + 1);
    for (char *s = tmp, *nx; s && *s; ) {   /* the body owns the advance */
        nx = strchr(s, '/');
        const size_t sl = nx ? (size_t)(nx - s) : strlen(s);
        const char save = s[sl]; s[sl] = '\0';
        wlen += snprintf(walk + wlen, sizeof walk - (size_t)wlen, "/%s", s);
        s[sl] = save;
        if (wlen >= (int)sizeof walk) { e = ESP_ERR_INVALID_SIZE; break; }
        if (mkdir(walk, 0755) == 0) { if (nx) { s = nx + 1; continue; } break; }
        if (errno == EEXIST) {
            struct stat st;
            if (stat(walk, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (nx) { s = nx + 1; continue; }
                break;                      /* the intention already holds */
            }
            e = ESP_ERR_INVALID_STATE;      /* a file stands on the way */
            break;
        }
        e = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        break;
    }
    orb_sd_unlock();
    return e;
}

/* One pass over a tree. The dry run counts and proves every directory opens:
 * a tree that cannot be fully WALKED cannot be fully DELETED, and the dry run
 * is what makes the refusal come before the first unlink instead of mid-way.
 * The real run unlinks bottom-up and the caller rmdirs the top node last.
 * *count accumulates every entry taken, files and directories alike. */
static esp_err_t fm_rm_tree(const char *abs, int *count, bool dry)
{
    DIR *d = opendir(abs);
    if (!d) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    struct dirent *de;
    char child[ORB_SD_FM_PATH_MAX + 96];
    esp_err_t e = ESP_OK;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (snprintf(child, sizeof child, "%s/%s", abs, de->d_name) >= (int)sizeof child) {
            e = ESP_ERR_INVALID_SIZE; break;
        }
        struct stat st;
        if (stat(child, &st) != 0) continue;            /* vanished: nothing to do */
        if (S_ISDIR(st.st_mode)) {
            e = fm_rm_tree(child, count, dry);
            if (e == ESP_OK && !dry)
                e = (rmdir(child) == 0) ? ESP_OK
                  : (errno == ENOTEMPTY ? ESP_ERR_INVALID_STATE : ESP_FAIL);
            if (e == ESP_OK && ++*count > ORB_SD_FM_RM_MAX)   /* the emptied dir too */
                e = ESP_ERR_INVALID_SIZE;
        } else {
            if (++*count > ORB_SD_FM_RM_MAX) e = ESP_ERR_INVALID_SIZE;
            else if (!dry && remove(child) != 0)
                e = (errno == EACCES || errno == EPERM) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
        }
        if (e != ESP_OK) break;
    }
    closedir(d);
    return e;
}

esp_err_t orb_sd_fm_rm(const char *rel, bool recursive, int *removed_out)
{
    if (removed_out) *removed_out = 0;
    if (rel == NULL || rel[0] == '\0') return ESP_ERR_INVALID_ARG;  /* never the root */
    char abs[ORB_SD_FM_PATH_MAX + 32];
    if (!orb_sd_fm_path(abs, sizeof abs, rel, false)) return ESP_ERR_INVALID_ARG;
    const esp_err_t me = orb_sd_ensure_mounted();
    if (me != ESP_OK) return me;
    if (orb_sd_usb_owned()) return ESP_ERR_INVALID_STATE;

    orb_sd_lock();
    struct stat st;
    esp_err_t e = ESP_OK;
    if (stat(abs, &st) != 0) {
        e = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    } else if (S_ISDIR(st.st_mode) && !recursive) {
        /* Empty is deletable, and "pass recursive to be safe" is a lie the first
         * time an MTP guest leaves a dotfile behind: probe instead. */
        DIR *d = opendir(abs);
        int seen = 0;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL)
                if (de->d_name[0] != '.') { seen++; break; }
            closedir(d);
        }
        e = seen ? ESP_ERR_INVALID_STATE : (rmdir(abs) == 0 ? ESP_OK : ESP_FAIL);
        if (e == ESP_OK && removed_out) *removed_out = 1;
    } else if (S_ISDIR(st.st_mode)) {
        int count = 1;                                  /* the top node counts */
        e = fm_rm_tree(abs, &count, true);              /* dry: whole tree first */
        if (e == ESP_OK) {
            int real = 1;
            e = fm_rm_tree(abs, &real, false);
            if (e == ESP_OK && rmdir(abs) == 0) {
                if (removed_out) *removed_out = real;
            } else if (e == ESP_OK) {
                e = errno == ENOTEMPTY ? ESP_ERR_INVALID_STATE : ESP_FAIL;
            }
        }
    } else {
        e = (remove(abs) == 0) ? ESP_OK
          : (errno == EACCES || errno == EPERM) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
        if (e == ESP_OK && removed_out) *removed_out = 1;
    }
    orb_sd_unlock();
    return e;
}

/* --- streaming write ----------------------------------------------------------
 * The session holds no lock between chunks (the section comment above says why);
 * overwrite=false is a polite-not-airtight rule on a volume an MTP host can
 * write too, exactly as it is on every other door this device has.
 */
typedef struct {
    FILE *f;
    char path[ORB_SD_FM_PATH_MAX + 32];
    size_t written, cap;
} fm_write_t;

esp_err_t orb_sd_fm_write_open(const char *rel, bool overwrite, size_t declared, void **h)
{
    if (h) *h = NULL;
    if (rel == NULL || rel[0] == '\0' || declared == 0) return ESP_ERR_INVALID_ARG;
    if (declared > (size_t)ORB_SD_FM_WRITE_MAX) return ESP_ERR_INVALID_SIZE;
    char abs[ORB_SD_FM_PATH_MAX + 32];
    if (!orb_sd_fm_path(abs, sizeof abs, rel, false)) return ESP_ERR_INVALID_ARG;
    const esp_err_t me = orb_sd_ensure_mounted();
    if (me != ESP_OK) return me;
    if (orb_sd_usb_owned()) return ESP_ERR_INVALID_STATE;

    orb_sd_lock();
    struct stat st;
    esp_err_t e = ESP_OK;
    if (!overwrite && stat(abs, &st) == 0) e = ESP_ERR_INVALID_STATE;
    uint64_t total = 0, freeb = 0;
    orb_sd_space(&total, &freeb);
    if (e == ESP_OK && declared + (1u << 20) > freeb) e = ESP_ERR_NO_MEM;
    fm_write_t *w = NULL;
    FILE *f = NULL;
    if (e == ESP_OK) {
        w = malloc(sizeof *w);
        f = w ? fopen(abs, "wb") : NULL;
        if (!f) {
            e = (errno == EACCES || errno == EPERM) ? ESP_ERR_INVALID_STATE
              : errno == ENOENT ? ESP_ERR_NOT_FOUND     /* a parent is missing */
              : errno == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
            free(w); w = NULL;
        }
    }
    orb_sd_unlock();
    if (e != ESP_OK) return e;
    snprintf(w->path, sizeof w->path, "%s", abs);
    w->f = f; w->written = 0; w->cap = declared;
    *h = w;
    return ESP_OK;
}

esp_err_t orb_sd_fm_write_data(void *h, const char *buf, size_t len)
{
    fm_write_t *w = h;
    if (!w || !w->f || !buf) return ESP_ERR_INVALID_ARG;
    if (w->written + len > w->cap) return ESP_ERR_INVALID_SIZE;
    orb_sd_lock();
    const size_t put = fwrite(buf, 1, len, w->f);
    orb_sd_unlock();
    w->written += put;                      /* honest partial for the report */
    if (put != len) return ferror(w->f) ? ESP_FAIL : ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t orb_sd_fm_write_close(void *h, bool commit)
{
    fm_write_t *w = h;
    if (!w) return ESP_ERR_INVALID_ARG;
    esp_err_t e = ESP_OK;
    if (w->f) {
        orb_sd_lock();
        const int r = fclose(w->f);
        if (!commit) remove(w->path);       /* cancel deletes what it was writing */
        orb_sd_unlock();
        if (r != 0 && commit) e = ESP_FAIL;
    }
    free(w);
    return e;
}




#else  /* !CONFIG_ORB_SD_ENABLE */

/* Stubs, so main.c and orb_net.c carry no #ifs of their own and so that
 * switching the socket off really does take esp_vfs_fat, sdmmc and
 * esp_driver_sdspi out of the image — that removal is the point of the switch
 * when frame time is being A/B-ed. */
esp_err_t orb_sd_ensure_mounted(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t orb_sd_init(void)           { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t orb_sd_remount(void)        { return ESP_ERR_NOT_SUPPORTED; }
/* The share claim has nothing to hold with the socket off, and true would be the
 * wrong lie: an orb with no card never reaches an MTP window at all. */
void      orb_sd_share_begin(void)  { }
void      orb_sd_share_end(void)    { }
bool      orb_sd_share_held(void)   { return false; }
/* Nothing to claim or hand over with no socket compiled in, and false is the
 * honest answer to "does the USB host have the card" on hardware that has none. */
void      orb_sd_usb_claim(void)    { }
bool      orb_sd_usb_owned(void)    { return false; }
esp_err_t orb_sd_hand_to_usb(orb_sd_raw_card_t *out_card)
{
    if (out_card) *out_card = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
void      orb_sd_hand_back(void)    { }
const char *orb_sd_card_id(void)      { return ""; }
void      orb_sd_space(uint64_t *t, uint64_t *f) { if (t) *t = 0; if (f) *f = 0; }
void      orb_sd_lock(void)   { }
void      orb_sd_unlock(void) { }

bool orb_sd_fm_path(char *out, size_t outsz, const char *rel, bool read_only)
{
    (void)rel; (void)read_only;
    if (out && outsz) out[0] = '\0';
    return false;
}
esp_err_t orb_sd_fm_list(const char *rel, orb_sd_fm_ent_t *ents, int max,
                        int *n, bool *truncated)
{
    (void)rel; (void)ents; (void)max; (void)n; (void)truncated;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t orb_sd_fm_mkdir(const char *rel) { (void)rel; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t orb_sd_fm_rm(const char *rel, bool recursive, int *removed_out)
{
    (void)rel; (void)recursive;
    if (removed_out) *removed_out = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t orb_sd_fm_write_open(const char *rel, bool overwrite, size_t declared, void **h)
{
    (void)rel; (void)overwrite; (void)declared;
    if (h) *h = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t orb_sd_fm_write_data(void *h, const char *buf, size_t len)
{
    (void)h; (void)buf; (void)len; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t orb_sd_fm_write_close(void *h, bool commit)
{
    (void)h; (void)commit; return ESP_ERR_NOT_SUPPORTED;
}

bool orb_sd_name_ok(const char *name)
bool orb_sd_name_ok(const char *name)
{
    /* Alive even with the card off, and deliberately not `return true`: the
     * HTTP layer validates with this either way, and a permissive stub is
     * precisely how a path-traversal hole appears the moment someone flips the
     * switch to compare two builds. */
    if (name == NULL || name[0] == '\0' || name[0] == '.') return false;
    size_t n = 0;
    for (const char *p = name; *p; ++p, ++n) {
        if (n >= ORB_SD_NAME_MAX) return false;
        const char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool orb_sd_theme_path(char *out, size_t outsz, const char *name)
{
    if (out == NULL || outsz == 0) return false;
    if (!orb_sd_name_ok(name)) { out[0] = '\0'; return false; }
    const int n = snprintf(out, outsz, ORB_SD_THEME_DIR "/%s", name);
    if (n < 0 || (size_t)n >= outsz) { out[0] = '\0'; return false; }
    return true;
}

#endif /* CONFIG_ORB_SD_ENABLE */

