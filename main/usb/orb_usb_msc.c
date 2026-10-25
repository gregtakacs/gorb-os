/**
 * orb_usb_msc.c — see orb_usb_msc.h.
 *
 * Three facts organise this file.
 *
 * 1. One class, one owner. The LUN is the raw card, so FatFs must be down for the
 *    whole window; orb_sd_hand_to_usb() takes it down and refuses to put it back
 *    while the host holds it. The write-protect question does not arise: the
 *    point of the mode is that the host can write.
 * 2. The PHY is one switch. GPIO 19/20 feed a single FSLS PHY that USB_WRAP
 *    multiplexes, so installing TinyUSB ends the console by hardware and leaves
 *    D+ pulled up across the move — the host keeps the JTAG instance it made at
 *    boot and never reads the MSC descriptor unless reconnect_pulse() shows it a
 *    gap (§6.1, §11).
 * 3. Block work runs on the TinyUSB task, which calls sdmmc_read_sectors() on
 *    SPI3 directly. Anything here that can block therefore runs on the orb_jobs
 *    worker, and the dial's own card readers are all shut out by the mount
 *    refusal rather than by the card mutex, since they have no business reading
 *    a volume the host owns.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"   /* TINYUSB_DEFAULT_CONFIG is variadic and lives here */
#include "tinyusb_msc.h"

#include "orb_usb_msc.h"
#include "display.h"            /* display_dma_borrow — see orb_usb_msc_start */
#include "orb_sd.h"
#include "orb_jobs.h"
#include "orb_app.h"        /* the service-tier row at the bottom of this file */

static const char *TAG = "orb_usb_msc";

#if CONFIG_ORB_USB_MSC

#define MSC_NVS_PARTITION "storage"     /* as the MTP mode: see partitions.csv */
#define MSC_NVS_NAMESPACE "orb.os"
/* The MTP mode's key is usb_files and is deliberately not reused: an orb
 * upgraded from a build that armed MTP must not find itself exporting a raw LUN
 * on the strength of a press that meant something else. */
#define MSC_NVS_KEY       "usb_disk"

/* PENDING is the press's word, not the mode's: the press landed and the worker
 * owns the job, but the card has not moved yet. It exists so the row repaints
 * honestly BEFORE the tile ring is borrowed, because the frame painted at that
 * moment is the last frame the glass will get until the reboot — a share window
 * freezes the dial on whatever was funded when the loan opened (display.c, the
 * tile-ring loan). */
enum { MSC_OFF, MSC_PENDING, MSC_ACTIVE };

static volatile int                 s_state;
static volatile bool                s_host_up;     /* set by the ATTACHED event */
static volatile bool                s_stopping;    /* the door was pressed; the reboot timer is running */
static tinyusb_msc_storage_handle_t s_store;
static bool                         s_tinyusb_up;  /* one install per boot, at most */
static char                         s_reason[48] = "";

/* ------------------------------------------------------------------ full read/write
 *
 * The MSC class asks this on every MODE_SENSE, and the component gives it a weak
 * definition that answers read-only (§10). The mode is a USB stick or it is
 * nothing, so the answer is yes for every LUN there is — which is exactly one. */
bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

/* ------------------------------------------------------------------ one-shot
 *
 * Read-and-clear, not a level: a device that loses power mid-copy must come back
 * as a clock rather than as a disk nobody asked for and cannot dismiss without
 * opening the shell. */
static esp_err_t nvs_write(bool on)
{
    nvs_handle_t h;
    const esp_err_t e = nvs_open_from_partition(MSC_NVS_PARTITION, MSC_NVS_NAMESPACE,
                                                NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    esp_err_t w = on ? nvs_set_u8(h, MSC_NVS_KEY, 1) : nvs_erase_key(h, MSC_NVS_KEY);
    if (w == ESP_ERR_NVS_NOT_FOUND) w = ESP_OK;      /* clearing an unset key */
    if (w == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return w;
}

static bool nvs_read(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(MSC_NVS_PARTITION, MSC_NVS_NAMESPACE,
                                NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    const bool set = (nvs_get_u8(h, MSC_NVS_KEY, &v) == ESP_OK) && (v != 0);
    nvs_close(h);
    return set;
}

bool orb_usb_msc_boot_claim(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(MSC_NVS_PARTITION, MSC_NVS_NAMESPACE,
                                NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 0;
    const bool set = (nvs_get_u8(h, MSC_NVS_KEY, &v) == ESP_OK) && (v != 0);
    if (set) {
        nvs_erase_key(h, MSC_NVS_KEY);
        nvs_commit(h);
    }
    nvs_close(h);
    /* The difference from the MTP claim, and the whole reason the call sits ahead
     * of the storage ladder: this boot must NOT mount /sd, and by the time the
     * ladder asks it has to already know not to. */
    if (set) {
        orb_sd_usb_claim();
        ESP_LOGW(TAG, "disk boot claimed — /sd will not mount");
    }
    return set;
}

bool orb_usb_msc_armed(void) { return nvs_read(); }

esp_err_t orb_usb_msc_set_armed(bool on)
{
    const esp_err_t e = nvs_write(on);
    if (e != ESP_OK) snprintf(s_reason, sizeof s_reason, "nvs: %s", esp_err_to_name(e));
    ESP_LOGI(TAG, "%s for the next boot (%s)", on ? "armed" : "disarmed",
             esp_err_to_name(e));
    return e;
}
/* ------------------------------------------------------------------ the device
 *
 * Force the host to re-read us — see the header note on the PHY, and §11 for why
 * this is load-bearing rather than polite: without the gap Windows keeps the
 * JTAG device instance it made at boot and never asks for the MSC descriptor. */
static void reconnect_pulse(void)
{
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    tud_connect();
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_host_up = true;
        ESP_LOGI(TAG, "host has configured the device — the card is its disk");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        /* Reported and not acted on. The mode persists by design; the card is not
         * taken back while a host might still be writing to it. */
        s_host_up = false;
        ESP_LOGW(TAG, "host let go of the card");
    }
}

/* The component's own storage bookkeeping. Two of these lines earn their place:
 * MOUNT_COMPLETE fires for an unmount as well as a mount, so nothing here tries to
 * read a direction out of the id (§10), and FORMAT_REQUIRED is the mode refusing
 * to destroy a card, which a bystander would otherwise read as a failure. */
static void msc_storage_cb(tinyusb_msc_storage_handle_t handle,
                           tinyusb_msc_event_t *event, void *arg)
{
    (void)handle; (void)arg;
    if (event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED) {
        ESP_LOGW(TAG, "no filesystem the orb can read — the host may format it, we will not");
    } else if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED) {
        ESP_LOGE(TAG, "MSC storage operation failed");
    }
}

esp_err_t orb_usb_msc_start(void)
{
    if (s_state == MSC_ACTIVE) return ESP_OK;
    s_reason[0] = '\0';

    /* First and load-bearing: the card must be ours in the raw. This one call
     * unmounts /sd if the boot ladder got there first, refuses every later mount,
     * and runs the same speed ladder the mount does — a card that will not
     * enumerate at 20 MHz still deserves to show up at 4. */
    orb_sd_raw_card_t card = NULL;
    const esp_err_t he = orb_sd_hand_to_usb(&card);
    if (he != ESP_OK) {
        s_state = MSC_OFF;
        snprintf(s_reason, sizeof s_reason, "%s",
                 he == ESP_ERR_NOT_FOUND ? "no card in the socket" : esp_err_to_name(he));
        ESP_LOGE(TAG, "nothing to hand over: the card did not attach (%s)",
                 esp_err_to_name(he));
        return he;
    }

    /* auto_mount_off, on purpose. Left to itself the component re-mounts the
     * volume for the application when USB lets go, which would put FatFs back on
     * top of a LUN a host may still be writing to — and it is the one place the
     * mode could acquire two owners without anybody asking. */
    const tinyusb_msc_driver_config_t dcfg = {
        .callback = msc_storage_cb,
        .user_flags.auto_mount_off = 1,
    };
    /* 2026-10-24: the ring loan must cover the allocations, not just the card
     * hand-over. orb_sd_lock lends and orb_sd_unlock returns, so the ring was
     * already funded back when the component went looking for its storage
     * object — which embeds the 8 KB write buffer inline (MSC_BUFSIZE=8192)
     * and asks for it as one MALLOC_CAP_DMA calloc. Measured on the bench:
     * that calloc met a largest DMA block of 7,168 B and answered NO_MEM
     * ("lun: ESP_ERR_NO_MEM"); the same instant inside a borrow window shows
     * 19,456 B free. Refcounted, so hand_to_usb's own inner pair nests here
     * harmlessly. Held through the rest of the window on purpose: the flush
     * path drops, the panel holds its GRAM, and the only exit from the mode
     * is a reboot anyway (§6.1) — a funded ring buys a share window nothing. */
    display_dma_borrow();

    esp_err_t e = tinyusb_msc_install_driver(&dcfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        s_state = MSC_OFF;
        snprintf(s_reason, sizeof s_reason, "msc: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "tinyusb_msc_install_driver: %s", esp_err_to_name(e));
        display_dma_return();
        orb_sd_hand_back();
        return e;
    }

    /* One LUN, the whole volume, mounted to USB and never to the app.
     * do_not_format is not a preference: the card carries somebody's photos, and a
     * card we cannot read is reported rather than erased — the same rule
     * mount_at_khz() follows from the other side. */
    const tinyusb_msc_storage_config_t scfg = {
        .medium.card          = card,
        .mount_point          = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs.base_path     = NULL,
        .fat_fs.do_not_format = true,
    };
    e = tinyusb_msc_new_storage_sdmmc(&scfg, &s_store);
    if (e != ESP_OK) {
        s_state = MSC_OFF;
        snprintf(s_reason, sizeof s_reason, "lun: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc: %s", esp_err_to_name(e));
        display_dma_return();
        orb_sd_hand_back();
        return e;
    }
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    e = tinyusb_driver_install(&tusb_cfg);
    if (e != ESP_OK) {
        s_state = MSC_OFF;
        snprintf(s_reason, sizeof s_reason, "tinyusb: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(e));
        (void)tinyusb_msc_delete_storage(s_store); s_store = NULL;
        display_dma_return();
        orb_sd_hand_back();
        return e;
    }
    s_tinyusb_up = true;

    s_state   = MSC_ACTIVE;
    s_host_up = tud_mounted();
    /* Last, after the class and its LUN both exist, so the descriptor the host
     * reads on the way back up is the complete one. */
    reconnect_pulse();
    ESP_LOGW(TAG, "exporting the whole card as one disk — /sd is unmounted, "
                  "and a reboot is the only thing that gives it back");
    return ESP_OK;
}

/* No unmount and no hand-back of the CARD here either, and that remains the
 * design: the host may be mid-write, and the only moment it is safe to take
 * the volume back is a boot. What does come back is the tile ring — 2026-10-24,
 * after owners read the frozen window as a lock-up. The door press starts a
 * 750 ms reboot timer; inside that window the USB side can be laid down
 * honestly first (a clean host-facing detach, then the class and the driver —
 * which frees the DMA memory the ring was lent to), so the flush path's own
 * refund retry can paint the card's closing line before the panel goes dark.
 * If the memory does not come back, nothing regresses: the ring stays parked
 * and the frame the owner already reads is the one they keep until the boot. */
esp_err_t orb_usb_msc_stop(void)
{
    if (s_state != MSC_ACTIVE || s_stopping) return ESP_OK;
    s_stopping = true;
    ESP_LOGW(TAG, "rebooting out of disk mode — eject from the host first if it is copying");
    tud_disconnect();                              /* the host sees go before we vanish */
    if (s_store) { (void)tinyusb_msc_delete_storage(s_store); s_store = NULL; }
    (void)tinyusb_msc_uninstall_driver();
    if (s_tinyusb_up) { (void)tinyusb_driver_uninstall(); s_tinyusb_up = false; }
    (void)display_dma_return();
    orb_jobs_reboot_soon("USB disk mode stopped");
    return ESP_OK;
}

static void job_start(void *arg)
{
    (void)arg;
    /* The press set PENDING on the LVGL task and the menu repainted under it;
     * give that frame time to reach the glass before the loan parks the ring.
     * One poll period is 10 ms and the paint is submitted in the press's own
     * handler pass, so this waits on a commit already in flight, not on a hope:
     * the frame the freeze preserves is the honest one. */
    vTaskDelay(pdMS_TO_TICKS(150));
    if (s_state != MSC_PENDING) return;   /* withdrawn while the job queued */
    (void)orb_usb_msc_start();
}
static void job_stop(void *arg)  { (void)arg; orb_usb_msc_stop(); }
static void job_arm(void *arg)
{
    (void)arg;
    const esp_err_t e = orb_usb_msc_set_armed(true);
    if (e != ESP_OK) return;
    orb_jobs_reboot_soon("USB disk mode armed");
}

static esp_err_t post(orb_job_fn job)
{
    const esp_err_t e = orb_jobs_post(job, NULL, 0);
    if (e == ESP_OK) return ESP_OK;
    snprintf(s_reason, sizeof s_reason, "worker busy");
    ESP_LOGW(TAG, "%s: the worker refused the job (%s) — nothing changed",
             __func__, esp_err_to_name(e));
    return e;
}

/* A press means "become the disk now" unless the next boot is already armed, in
 * which case the same press cancels that instead — one row, one gesture, and the
 * label always names the thing it does. */
esp_err_t orb_usb_msc_request_start(void)
{
    if (s_state == MSC_ACTIVE) return ESP_OK;
    /* Armed means the next boot is already promised to the disk; a press on the
     * row cancels that instead (usb_opt_sel routes armed to disarm), so this
     * arm path only runs from the boot claim, where NVS was already consumed. */
    if (orb_usb_msc_armed()) return post(job_arm);
    /* The state flips here, synchronously, on the press's own task — before the
     * repaint act_app takes one call later can read it. That ordering is the
     * whole point: it puts the true label on the last frame the glass gets. */
    if (s_state == MSC_OFF) s_state = MSC_PENDING;
    const esp_err_t e = post(job_start);
    if (e != ESP_OK) s_state = MSC_OFF;   /* the worker refused: nothing pending */
    return e;
}

esp_err_t orb_usb_msc_request_stop(void) { return post(job_stop); }

static void job_disarm(void *arg) { (void)arg; (void)orb_usb_msc_set_armed(false); }

esp_err_t orb_usb_msc_request_disarm(void)
{
    if (s_state == MSC_ACTIVE) return ESP_ERR_INVALID_STATE;  /* armed is a boot's word */
    return post(job_disarm);
}

/* ------------------------------------------------------------------ the state

Readers here touch RAM and one NVS word, and nothing else — the same budget the
MTP row ran on, which is what makes them safe to call from a render callback on
the LVGL task and from httpd alike. No card I/O, no network, no allocation. */
bool orb_usb_msc_active(void)  { return s_state == MSC_ACTIVE; }
bool orb_usb_msc_host_up(void) { return s_state == MSC_ACTIVE && s_host_up; }
bool orb_usb_msc_window_open(void) { return s_state == MSC_PENDING || s_state == MSC_ACTIVE; }
bool orb_usb_msc_stopping(void) { return s_stopping; }

const char *orb_usb_msc_state(void)
{
    if (s_stopping)            return "stopping";
    if (s_state == MSC_ACTIVE)   return s_host_up ? "host connected" : "disk up, no host";
    if (s_state == MSC_PENDING)  return "starting";
    if (orb_usb_msc_armed())     return "next boot: disk";
    if (s_reason[0])             return "failed";
    return "off";
}

const char *orb_usb_msc_reason(void) { return s_reason[0] ? s_reason : ""; }
/* ------------------------------------------------------------------ the row

One feature, one row, in the service tier — the same row orb_usb_mtp.c exports,
because it is the same feature with a different class behind it and the owner has
no reason to learn two addresses. The hooks honour the ABI's hard rule:
opt_label/opt_value run on the LVGL task and may not touch the card, the network
or NVS, so every string is a literal chosen from RAM state and every press is a
post. */
#define USB_ROW_ACTION  0        /* press: hand the card over, or take it back    */
#define USB_ROW_NOTE    1        /* the one fact an owner cannot otherwise see    */
#define USB_ROW_N       2

/* A press shares the card NOW; the mode has never restarted anything to get
 * there. The armed path is the boot promise, not this press. */
static const char *const s_act_off   = "Share the disk now";
static const char *const s_act_armed = "Cancel the next boot";
/* The only way out, and the honest label: stopping means a reboot, because the
 * console this mode gave up comes back no other way and because the card must
 * not be remounted under a host that may still be writing. PENDING and the note
 * row carry it too — a share window paints no more frames (the tile ring is on
 * loan to the card), so this row's own press is the door, found blind or not. */
static const char *const s_act_on    = "Press to restart, stop sharing";
/* The note's whole job is saying what the press makes true, before it is true.
 * During a window the note row carries the door instead (usb_opt_label): the
 * glass paints no frames then, and a fact nobody can press is worth less than a
 * press that gets the dial back. The card is blank to the dial until the reboot
 * that follows a share — the orb puts the volume down completely before handing
 * it over, which is why this mode cannot corrupt a volume it also mounts. */
static const char *const s_note_off  = "Whole card, one disk, writable";
static char s_val[40];

static int usb_opt_count(orb_app_t *app) { (void)app; return USB_ROW_N; }

static const char *usb_opt_label(orb_app_t *app, int idx)
{
    (void)app;
    /* While the mode holds — or is committed to holding — the card, every row
     * on this page carries the door. The glass cannot repaint during the
     * window, so the row the owner sees selected when the freeze lands is the
     * row they will press blind, whatever it was before: it must read as the
     * exit on the note row too. */
    if (s_state == MSC_ACTIVE || s_state == MSC_PENDING) return s_act_on;
    if (idx == USB_ROW_NOTE) return s_note_off;
    if (orb_usb_msc_armed())              return s_act_armed;
    return s_act_off;
}

/* Reads state only. The host-up flag arrives from the ATTACHED event rather than
 * a poll, so nothing here touches the USB stack on the LVGL task. */
static const char *usb_opt_value(orb_app_t *app, int idx)
{
    (void)app;
    if (idx != USB_ROW_ACTION) return "";
    const char *st = orb_usb_msc_state();
    if (st[0] == 'f' && orb_usb_msc_reason()[0]) {
        snprintf(s_val, sizeof s_val, "%.14s: %.20s", st, orb_usb_msc_reason());
        return s_val;
    }
    return st;
}

static void usb_opt_sel(orb_app_t *app, int idx)
{
    (void)app;
    /* A press during a window means the door, whichever row the selection
     * drifted to while the glass was frozen: opt_label put the exit on every
     * row for exactly this, and opt_sel honours that promise. The request is a
     * post and a reboot, so nothing here touches the card or the USB stack. */
    if (s_state == MSC_ACTIVE || s_state == MSC_PENDING) {
        (void)orb_usb_msc_request_stop();
        return;
    }
    if (idx != USB_ROW_ACTION) return;
    if (orb_usb_msc_armed())             (void)orb_usb_msc_request_disarm();
    else                                 (void)orb_usb_msc_request_start();
}

/* Hold is the drift-proof door. The router offers it before the hold-to-home
 * rule spends it (orb_app.h, opt_long), and while the mode owns the card there
 * is nothing else a hold could usefully do on this page: home would switch a
 * screen the glass cannot repaint. True means the hold was the exit; false
 * leaves it the ordinary hold-to-home. */
static bool usb_opt_long(orb_app_t *app)
{
    (void)app;
    if (s_state != MSC_ACTIVE && s_state != MSC_PENDING) return false;
    (void)orb_usb_msc_request_stop();
    return true;
}

/* Nothing to build: there is no face, and every state the hooks read is RAM or an
 * NVS word this file already opened during the boot ladder. */
static esp_err_t usb_init(orb_app_t *app) { (void)app; return ESP_OK; }

static void usb_opt_arm(orb_app_t *app, bool armed) { (void)app; (void)armed; }

static const orb_app_iface_t s_usb_iface = {
    .init      = usb_init,
    .opt_count = usb_opt_count,
    .opt_label = usb_opt_label,
    .opt_value = usb_opt_value,
    .opt_sel   = usb_opt_sel,
    .opt_arm   = usb_opt_arm,
    .opt_long  = usb_opt_long,
};

orb_app_t orb_app_usb_files_row = {
    .slug      = "usb-files",
    .alias     = NULL,
    /* Named for what the host gets, which in this build is a disk. The slug stays
     * usb-files so the one address the owner has ever had for this row does not
     * move when the class behind it changes. */
    .title     = "USB Drive",
    .kind      = ORB_APP_KIND_APP,
    .is_home   = false,
    .behaviour_kind = ORB_BEHAVIOUR_NONE,
    .art_offset = 0,
    .art_bytes  = 0,
    .art_slot   = -1,
    .flash_slot = -1,          /* no card slot, and no card to put one on */
    .iface     = &s_usb_iface,
    .installed = true,
    .enabled   = true,
};

#else  /* !CONFIG_ORB_USB_MSC ---------------------------------------------- */

/* The header promises boot_claim answers false when the feature is out of the
 * build, so the boot ladder calls it unguarded. Nothing here claims the card, so
 * an MTP build mounts /sd exactly as it always did. */
bool orb_usb_msc_boot_claim(void) { return false; }
bool orb_usb_msc_window_open(void) { return false; }
bool orb_usb_msc_stopping(void) { return false; }

#endif /* CONFIG_ORB_USB_MSC */
