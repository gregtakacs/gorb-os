/**
 * orb_usb_mtp.c — see orb_usb_mtp.h.
 *
 * The card stays MOUNTED for the whole window and the host reaches the files
 * inside it: no hand-over, no raw sectors, no format hazard, no write-protect
 * question. What that leaves is one invariant the rest of the tree does not
 * already have — nobody may unmount /sd while a host holds handles into it — and
 * orb_sd_share_begin() is that refusal, claimed here and released in stop().
 *
 * The window has no timer. It ends when the owner reboots, which is also the only
 * way the console can come back: the port is a console unless the owner explicitly
 * asks otherwise, and asking is not sticky. See the Kconfig help.
 *
 * Two facts organise the rest:
 *
 * 1. The PHY is one switch. GPIO 19/20 feed a single FSLS PHY that USB_WRAP
 *    multiplexes between the USB-Serial-JTAG console and the USB-OTG controller
 *    (RTCCNTL.sw_usb_phy_sel, see usb_wrap_ll_phy_enable_external).
 *    tinyusb_driver_install() moves it to OTG and the console dies by hardware,
 *    with nothing to clean up — and D+ stays pulled up across the move, so the
 *    host sees no gap on the line and keeps the JTAG instance it made at boot.
 *    reconnect_pulse() below is why this mode appears on a host at all.
 * 2. The USB device stack's own task answers MTP. Its handlers call fopen(),
 *    stat() and opendir() directly
 *    (mtp/tinyusb_mtp_write.c:61, tinyusb_mtp_object_store.c:329), so the TinyUSB
 *    task does FATFS work on the SPI bus. Anything in this file that can block
 *    therefore runs on the orb_jobs worker.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"   /* TINYUSB_DEFAULT_CONFIG is variadic and lives here */
#include "tinyusb_mtp.h"

#include "orb_usb_mtp.h"
#include "orb_sd.h"
#include "orb_jobs.h"
#include "orb_app.h"        /* the service-tier row at the bottom of this file */

static const char *TAG = "orb_usb_mtp";

#if CONFIG_ORB_USB_MTP

#define MTP_NVS_PARTITION "storage"     /* the orb's own settings partition; see
                                         * partitions.csv and NET_NVS_PARTITION */
#define MTP_NVS_NAMESPACE "orb.os"
/* One key, one byte. The name is the mode's own, and an orb that carried an older
 * build's flag across the upgrade must not find itself serving files on a boot
 * nobody asked for. */
#define MTP_NVS_KEY       "usb_files"

enum { MTP_OFF, MTP_ACTIVE };

static volatile int                  s_state;
static volatile bool                 s_host_up;      /* sampled from tud_mounted() */
static tinyusb_mtp_storage_handle_t  s_store;
static bool                          s_tinyusb_up;   /* one install per boot, at most */
static char                          s_reason[48] = "";

/* ------------------------------------------------------------------ one-shot
 *
 * Read-and-clear, not a level: a device that loses power mid-copy must come back
 * as a clock rather than as a USB device nobody asked for and cannot dismiss
 * without opening the shell. With no timer, this word is the only thing standing
 * between one accidental press and an orb that lives in device mode, so the boot
 * that consumes it is the boot that serves — and nothing else ever arms it.
 */

static esp_err_t nvs_write(bool on)
{
    nvs_handle_t h;
    const esp_err_t e = nvs_open_from_partition(MTP_NVS_PARTITION, MTP_NVS_NAMESPACE,
                                                NVS_READWRITE, &h);
    if (e != ESP_OK) return e;               /* the namespace appears on first write */
    esp_err_t w = on ? nvs_set_u8(h, MTP_NVS_KEY, 1) : nvs_erase_key(h, MTP_NVS_KEY);
    if (w == ESP_ERR_NVS_NOT_FOUND) w = ESP_OK;      /* clearing an unset key */
    if (w == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return w;
}

bool orb_usb_mtp_boot_claim(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(MTP_NVS_PARTITION, MTP_NVS_NAMESPACE,
                                NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 0;
    const bool set = (nvs_get_u8(h, MTP_NVS_KEY, &v) == ESP_OK) && (v != 0);
    if (set) {
        nvs_erase_key(h, MTP_NVS_KEY);
        nvs_commit(h);
    }
    nvs_close(h);
    /* Says what the mount is doing, because that is the thing an owner reading a
     * boot log needs to know. */
    if (set) ESP_LOGW(TAG, "file-share boot claimed — /sd mounts as normal");
    return set;
}

bool orb_usb_mtp_armed(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(MTP_NVS_PARTITION, MTP_NVS_NAMESPACE,
                                NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    const bool set = (nvs_get_u8(h, MTP_NVS_KEY, &v) == ESP_OK) && (v != 0);
    nvs_close(h);
    return set;
}

esp_err_t orb_usb_mtp_set_armed(bool on)
{
    const esp_err_t e = nvs_write(on);
    if (e != ESP_OK) snprintf(s_reason, sizeof s_reason, "nvs: %s", esp_err_to_name(e));
    ESP_LOGI(TAG, "%s for the next boot (%s)", on ? "armed" : "disarmed",
             esp_err_to_name(e));
    return e;
}

/* ------------------------------------------------------------------ no timer
 *
 * There is no timeout, and the reason is hardware rather than taste: becoming a
 * device gives up the console by multiplexer (§6.1), so a reboot is the only way
 * the log comes back, and the arm is one read-and-cleared NVS word, so a reboot is
 * also the only way out of the mode. The owner flips it back by rebooting, which is
 * the agreement the mode is built on. Nothing here needs to be cleverer about a
 * laptop that went to sleep: the next reboot of an orb in a drawer returns it.
 */

static void job_stop(void *arg)  { (void)arg; orb_usb_mtp_stop(); }

/* ------------------------------------------------------------------ the device
 *
 * One interface: MTP, nothing else. A composite MSC+MTP over one card would hand
 * the host raw sectors of the very volume it is writing through FatFs — two owners
 * of one filesystem — and the MSC class is out of this build entirely, so the
 * composite cannot be assembled by accident. The console is given up rather than
 * moved (§6.1).
 */

/** Force the host to re-read us. The mux handover is silent on the line, so
 *  dcd_disconnect() overrides the pads with D+/D- pulldowns and dcd_connect()
 *  removes the override, and the line goes to J for the delay in between. 150 ms
 *  clears the 2.5 us the spec requires and the ~100 ms Windows debounces a
 *  disconnect for. Runs on the worker, never on the tick or the USB task; called
 *  last, after the class and its storage exist, so the descriptor read on the way
 *  back up is the complete one. */
static void reconnect_pulse(void)
{
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    tud_connect();
}



/* Has a host configured us? tud_mounted() is cfg_num != 0: set by
 * SET_CONFIGURATION, cleared on detach, and independent of class, so it is the only
 * liveness signal MTP gets. esp_tinyusb raises TINYUSB_EVENT_ATTACHED from
 * tud_mount_cb, MSC's mount callback, and the component claims
 * tud_mtp_command_received_cb with a strong definition
 * (mtp/tinyusb_mtp_transport.c:277), so there is no event hook to take and the
 * question has to be asked of the stack instead. The sample rate is the menu's own
 * repaint, because orb_usb_mtp_host_up() is what the row's value line calls and the
 * row repaints at least once a second while the mode is up, so the first sample
 * after a start lands within a second of enumeration. */
bool orb_usb_mtp_host_up(void)
{
    if (s_state != MTP_ACTIVE) return false;
    const bool now = tud_mounted();
    if (now == s_host_up) return now;
    s_host_up = now;
    if (now) {
        ESP_LOGI(TAG, "host has configured the device — files are reachable");
    } else {
        /* Logged, not acted on: there is no timer to re-arm, and the device stays
         * ready for whatever host configures it next. */
        ESP_LOGW(TAG, "host let go of the device");
    }
    return now;
}

/* Unregister before uninstalling the class: the driver refuses to go away with a
 * transfer in flight, and the storage must not outlive the stack that answers for
 * it. /sd is left exactly as found — mounted — which is the whole point. */
static void device_off(void)
{
    if (s_store) { (void)tinyusb_mtp_unregister_storage(s_store); s_store = NULL; }
    if (s_tinyusb_up) { (void)tinyusb_driver_uninstall(); s_tinyusb_up = false; }
    (void)tinyusb_mtp_uninstall_driver();
    s_host_up = false;
}

esp_err_t orb_usb_mtp_start(void)
{
    /* Idempotent: the boot ladder and a knob press can ask for the same device,
     * and the second ask is not an error worth a red line. */
    if (s_state == MTP_ACTIVE) return ESP_OK;
    s_reason[0] = '\0';

    /* First and load-bearing: the filesystem must already be live. MTP answers
     * with paths, so an unmounted card is not a slow answer, it is no answer —
     * tinyusb_mtp_register_storage() stats the base path and returns
     * ESP_ERR_NOT_FOUND on one that is not a mounted FATFS volume
     * (mtp/tinyusb_mtp.c:233). Going through ensure_mounted() rather than
     * trusting boot state is what makes a press on a card that was never found,
     * or one that died mid-window, do the useful thing: try the ladder again. */
    const esp_err_t me = orb_sd_ensure_mounted();
    if (me != ESP_OK) {
        snprintf(s_reason, sizeof s_reason, "%s",
                 me == ESP_ERR_NOT_FOUND ? "no card in the socket" : esp_err_to_name(me));
        ESP_LOGE(TAG, "nothing to serve: /sd did not mount (%s)", esp_err_to_name(me));
        return me;
    }

    /* Before any of it is reachable from the host: /sd must not come back down
     * while the host holds handles into it. Without this, one
     * POST /api/v1/sd/rescan during a copy unmounts the volume under MTP's open
     * file handles, which is FATFS state divergence with a nicer name.
     * orb_sd_share_begin() is that refusal, held here and released in stop(), and
     * it is taken before the driver installs so a failure partway cannot leave the
     * door open. */
    orb_sd_share_begin();

    const tinyusb_mtp_driver_config_t mcfg = {
        .manufacturer      = "orb",
        .model             = "orb",
        /* A literal, because this tree has no build-time version macro to put
         * here: the field is MTP's DeviceInfo Version, the host displays it in a
         * properties sheet, and the firmware's real version is already on the
         * config page and in the boot log, where anybody who needs it looks. */
        .version           = "orb-os",
        /* The eFuse MAC, which is what NULL here already selects — named so the
         * reason is on the record: MTP has no unit name the owner could set, so
         * the only thing that distinguishes one orb's storage from another on a
         * shared machine is the MAC, and guessing it from a serial cable is the
         * one job this device cannot do in this mode. */
        .serial            = NULL,
        .friendly_name     = "orb",
    };
    esp_err_t e = tinyusb_mtp_install_driver(&mcfg);
    if (e != ESP_OK) {
        snprintf(s_reason, sizeof s_reason, "mtp driver: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "tinyusb_mtp_install_driver: %s", esp_err_to_name(e));
        orb_sd_share_end();
        return e;
    }

    /* One storage, the whole volume. Naming /sd rather than /sd/themes and
     * /sd/apps separately is the decision: an owner managing this card means
     * managing the directory tree the orb was built around, and the host seeing
     * one volume that matches what the dial lists is what keeps the two views
     * from disagreeing. The declared-source rules in orb_files.c bound what an
     * APP may reach; they were never a reason to hide the owner's own tree from
     * the owner's own file manager. */
    const tinyusb_mtp_storage_config_t scfg = {
        .base_path    = ORB_SD_BASE,
        .display_name = "orb",
        .removable    = false,
    };
    e = tinyusb_mtp_register_storage(&scfg, &s_store);
    if (e != ESP_OK) {
        snprintf(s_reason, sizeof s_reason, "storage: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "tinyusb_mtp_register_storage(%s): %s", ORB_SD_BASE, esp_err_to_name(e));
        (void)tinyusb_mtp_uninstall_driver();
        orb_sd_share_end();
        return e;
    }

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    e = tinyusb_driver_install(&tusb_cfg);
    if (e != ESP_OK) {
        snprintf(s_reason, sizeof s_reason, "tinyusb: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(e));
        (void)tinyusb_mtp_unregister_storage(s_store); s_store = NULL;
        (void)tinyusb_mtp_uninstall_driver();
        orb_sd_share_end();
        return e;
    }
    s_tinyusb_up = true;

    s_state   = MTP_ACTIVE;
    s_host_up = tud_mounted();
    /* Last, after the class and its storage both exist, so the descriptor a host
     * reads on the way back up is the complete one. */
    reconnect_pulse();
    ESP_LOGW(TAG, "serving /sd over MTP — the card stays mounted, the console does "
                  "not come back until the next boot, and nothing times this out");
    return ESP_OK;
}

esp_err_t orb_usb_mtp_stop(void)
{
    if (s_state != MTP_ACTIVE) return ESP_OK;         /* already back; not an error */
    device_off();
    /* The share lifts after the class is gone and before anything else on this
     * boot can unmount: the order is the whole safety argument, since between
     * these two lines the host no longer has handles and the rescan route is safe
     * to answer again. */
    orb_sd_share_end();
    s_state = MTP_OFF;
    ESP_LOGW(TAG, "sharing stopped — restarting normally so the console comes with it");
    /* §6.1: the console lives on the same USJ peripheral this mode borrowed, so it
     * returns at the next boot and not before. Asked of the worker, which is what
     * this runs on: orb_jobs.c's reboot_task cancels the job that asked. */
    orb_jobs_reboot_soon("USB file sharing stopped");
    return ESP_OK;
}

/* ---- the worker's side of every press ------------------------------------
 *
 * The menu's opt hooks may not block and every action here can: an NVS commit, a
 * mount, a TinyUSB task creation. So the hooks post and the worker does the work.
 */
static void job_start(void *arg)
{
    (void)arg;
    const esp_err_t e = orb_usb_mtp_start();
    ESP_LOGI(TAG, "start requested by the knob: %s", esp_err_to_name(e));
}

static void job_arm(void *arg)
{
    (void)arg;
    const esp_err_t e = orb_usb_mtp_set_armed(true);
    if (e != ESP_OK) return;                 /* set_armed said why, and why is in s_reason */
    orb_jobs_reboot_soon("USB file share armed");
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

esp_err_t orb_usb_mtp_request_start(void)
{
    if (s_state == MTP_ACTIVE) return ESP_OK;                    /* already serving */
    return post(orb_usb_mtp_armed() ? job_arm : job_start);
}

esp_err_t orb_usb_mtp_request_stop(void)  { return post(job_stop); }

static void job_disarm(void *arg) { (void)arg; (void)orb_usb_mtp_set_armed(false); }

esp_err_t orb_usb_mtp_request_disarm(void)
{
    if (s_state == MTP_ACTIVE) return ESP_ERR_INVALID_STATE;  /* armed is a boot's word */
    return post(job_disarm);
}

/* ------------------------------------------------------------------ the state
 *
 * Every reader here is RAM-only, because two of them are called from a render
 * callback on the LVGL task and one from the httpd task. The volatile flags and
 * the string written before the flag that reveals it are the whole
 * synchronisation needed.
 */
bool orb_usb_mtp_active(void) { return s_state == MTP_ACTIVE; }

const char *orb_usb_mtp_state(void)
{
    if (s_state == MTP_ACTIVE)   return orb_usb_mtp_host_up() ? "host connected"
                                                              : "files up, no host";
    if (orb_usb_mtp_armed())     return "next boot: files";
    if (s_reason[0])             return "failed";
    return "off";
}

const char *orb_usb_mtp_reason(void)
{
    return s_reason[0] ? s_reason : "";
}

/* ------------------------------------------------------------------ the row
 *
 * One feature, one row on the menu, in the service tier. No face, no carousel
 * the menu, and the build answers which way it works. No face, no carousel slot,
 * no art, no ledger word.
 *
 * The hooks honour the ABI's one hard rule — opt_label/opt_value run on the LVGL
 * task and may not touch the card, the network or NVS — so every string below is
 * a literal chosen from RAM state, and every press is a post.
 */
#define USB_ROW_ACTION  0        /* press: serve the files, or stop serving them  */
#define USB_ROW_NOTE    1        /* the two facts an owner cannot otherwise see   */
#define USB_ROW_N       2

static const char *const s_act_off   = "Restart to share files";
static const char *const s_act_armed = "Cancel the next boot";
/* The only way out, and the honest label: stopping means a reboot, because the
 * console this mode gave up comes back no other way (§6.1). A plain power cycle
 * does the same thing — the arm is one read-and-cleared word — so the row is a
 * shortcut, not the exit. */
static const char *const s_act_on    = "Stop sharing, restart";
/* The note's whole job is the missing drive letter. A host that plugs in and sees
 * no new disk concludes the card is dead, which is the exact failure this mode
 * would otherwise be remembered for. */
static const char *const s_note_up   = "No drive letter — use an MTP app";
static const char *const s_note_off  = "Shares files, not a disk";
static char s_val[40];

static int usb_opt_count(orb_app_t *app)
{
    (void)app;
    return USB_ROW_N;
}

static const char *usb_opt_label(orb_app_t *app, int idx)
{
    (void)app;
    if (idx == USB_ROW_NOTE) return orb_usb_mtp_active() ? s_note_up : s_note_off;
    if (s_state == MTP_ACTIVE)          return s_act_on;
    if (orb_usb_mtp_armed())            return s_act_armed;
    return s_act_off;
}

/* Calling orb_usb_mtp_host_up() here is the point of the hook, not an accident of
 * reading state: it is the only thing that samples tud_mounted(), so the row's
 * repaint is the poll. It stays RAM-only — one TinyUSB struct field and one static
 * buffer — because this runs on the LVGL task.
 *
 * The width rule bites here: this line shares the glass with the row's label, so a
 * refusal gets truncated widths in the format rather than a wider buffer. The long
 * version stays in the log and on the config page, where it belongs. */
static const char *usb_opt_value(orb_app_t *app, int idx)
{
    (void)app;
    if (idx != USB_ROW_ACTION) return "";
    const char *st = orb_usb_mtp_state();
    if (st[0] == 'f' && orb_usb_mtp_reason()[0]) {
        snprintf(s_val, sizeof s_val, "%.14s: %.20s", st, orb_usb_mtp_reason());
        return s_val;
    }
    return st;
}

/* One press for all three readings, in the same order usb_opt_label() picks its
 * text in, so the label always names the thing the press does. The note row is a
 * note. */
static void usb_opt_sel(orb_app_t *app, int idx)
{
    (void)app;
    if (idx != USB_ROW_ACTION) return;
    if (s_state == MTP_ACTIVE)   (void)orb_usb_mtp_request_stop();
    else if (orb_usb_mtp_armed()) (void)orb_usb_mtp_request_disarm();
    else                         (void)orb_usb_mtp_request_start();
}

/** Armed means the page owns the knob. Nothing here reads the knob except a
 *  press, so the flag is kept only because the shell's gate asks every entry the
 *  same question, and an entry that answered falsely would be steering turns it
 *  never receives. */
static void usb_opt_arm(orb_app_t *app, bool armed) { (void)app; (void)armed; }

/* Nothing to build: there is no face, and every state the hooks read is RAM or an
 * NVS word this file already opened during the boot ladder. The hook exists
 * because the registry's service-tier rule asks every row for init(), and a row
 * that refused would be listed as broken. */
static esp_err_t usb_init(orb_app_t *app) { (void)app; return ESP_OK; }

static const orb_app_iface_t s_usb_iface = {
    .init      = usb_init,
    .opt_count = usb_opt_count,
    .opt_label = usb_opt_label,
    .opt_value = usb_opt_value,
    .opt_sel   = usb_opt_sel,
    .opt_arm   = usb_opt_arm,
};

orb_app_t orb_app_usb_files_row = {
    .slug      = "usb-files",
    .alias     = NULL,
    /* Named for what the host gets. MSC is out of the build, so there is no drive
     * letter to imply, and the note row below is where that gets said out loud. */
    .title     = "USB Files",
    .kind      = ORB_APP_KIND_APP,
    .is_home   = false,
    .behaviour_kind = ORB_BEHAVIOUR_NONE,
    .art_offset = 0,
    .art_bytes  = 0,
    .art_slot   = -1,
    .flash_slot = -1,          /* no card slot: the shell row's answer, and the
                                * same one that keeps this row out of the
                                * slot-keyed ledger words */
    .iface     = &s_usb_iface,
    .installed = true,
    .enabled   = true,
};

#else  /* !CONFIG_ORB_USB_MTP ---------------------------------------------- */

/* The header promises boot_claim answers false when the feature is out of the
 * build, so the boot ladder calls it unguarded and an orb without the option
 * compiled in behaves exactly as it did before the option existed. */
bool orb_usb_mtp_boot_claim(void) { return false; }

#endif /* CONFIG_ORB_USB_MTP */







