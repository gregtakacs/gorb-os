/**
 * orb_usb_mtp.h — the card's FILES over USB (D37).
 *
 * Everything on this card is a file the owner put there — themes, apps, photo
 * kits — and the work is pushing new ones on and deleting old ones, interactively,
 * while the orb keeps running. MTP does that because esp_tinyusb's MTP class talks
 * POSIX to an already-mounted VFS path and never takes the block device:
 *   - /sd stays mounted for the whole window. Themes resolve, apps scan, the config
 *     page answers, and a file the host deleted is gone from the dial on the next
 *     paint, because both sides read the same directory.
 *   - The host writes through FatFs: SendObject lands in a temp file and is rename()d
 *     into place, so an interrupted copy leaves the old file intact instead of a
 *     half-written theme kit — the same guarantee orb_theme's staged header gives
 *     from the other side.
 *   - There is nothing to write-protect. A host that cannot write is a broken
 *     feature here, not a safe default.
 *
 * Costs, before anyone reaches for it:
 *   - No drive letter. The host sees a camera-shaped device and needs an MTP client
 *     (Windows Explorer lists it under Portable Devices, never This PC; gMTP; Android
 *     Documents UI). Nothing opens a file in place, which is deliberate: an in-place
 *     open on a volume the dial is using is what this mode refuses to be.
 *   - The console. GPIO 19/20 feed one FSLS PHY that USB_WRAP multiplexes, so
 *     becoming a device ends the console by hardware and only a boot gives it back
 *     (docs/usb-sd-msc-plan.md §6.1). The mode therefore costs a reboot to enter.
 *   - File operations run on the TinyUSB task, so a walk of a big directory is slow
 *     SPI work on the task that answers the host.
 *
 * There is no timeout on the window, and none is wanted: the port comes up as a
 * console on every boot unless the owner explicitly asked otherwise, and the ask is
 * one read-and-cleared NVS word. A reboot always ends it. See the Kconfig help.
 *
 * Inert unless CONFIG_ORB_USB_MTP is set; then inert until the one-shot says this
 * boot asked for it. MTP is the only USB device class in this build — MSC was
 * removed rather than kept beside it, so a composite binary that exports raw sectors
 * of the volume MTP is writing through FatFs cannot be assembled by accident.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"        /* the row extern at the bottom guards itself */

#ifdef __cplusplus
extern "C" {
#endif

/** Read-and-clear the "next boot serves its files over USB" one-shot. Call once,
 *  early, from the boot ladder. Says NOTHING about the mount: an MTP boot must
 *  mount /sd normally, because a live filesystem is the whole mechanism. True when
 *  the flag was set. */
bool orb_usb_mtp_boot_claim(void);

/** True when the flag is set for the next boot (says nothing about this one). */
bool orb_usb_mtp_armed(void);

/** Set or clear the flag for the next boot. Does not reboot and does not touch
 *  this boot: the caller owns the restart (orb_jobs_reboot_soon). */
esp_err_t orb_usb_mtp_set_armed(bool on);

/** Serve /sd over USB as an MTP device. Blocking: a card mount, an MTP driver
 *  install, a storage registration that stats the mount, and a TinyUSB install.
 *  Safe to call any time after orb_jobs_start().
 *  ESP_ERR_NOT_FOUND — no card mounted (nothing to serve). Anything else — named
 *  in the log and in orb_usb_mtp_reason(). */
esp_err_t orb_usb_mtp_start(void);

/** Stop serving: storage unregistered, class uninstalled, /sd left mounted exactly
 *  as it was, then a reboot so the console comes back with the PHY. Idempotent,
 *  worker-only. */
esp_err_t orb_usb_mtp_stop(void);

/** Post the stop through the job worker (the dial's row presses here). */
esp_err_t orb_usb_mtp_request_stop(void);

/** Become the device now, or cancel the armed boot — both posted, because every
 *  action here can block and a menu press runs on the LVGL task. */
esp_err_t orb_usb_mtp_request_start(void);
esp_err_t orb_usb_mtp_request_disarm(void);

/** True while the mode is up, whether or not a host has enumerated it. */
bool orb_usb_mtp_active(void);

/** True only when a host has configured the device — the distinction between "this
 *  firmware opened an interface" and "somebody can use it". Polled from
 *  tud_mounted(), not signalled: esp_tinyusb raises TINYUSB_EVENT_ATTACHED from
 *  tud_mount_cb, and the MTP class claims its own command callback, so no event
 *  arrives for file traffic. The dial's row calls this once per repaint. */
bool orb_usb_mtp_host_up(void);

/** "off" / "next boot: files" / "files up, no host" / "host connected" / "failed".
 *  RAM-only, safe from a render callback. */
const char *orb_usb_mtp_state(void);

/** The last failure as the owner should read it, "" when there is none. */
const char *orb_usb_mtp_reason(void);

/** The registry's service row for this feature: a menu entry with options and no
 *  face. */
#if CONFIG_ORB_USB_MTP
struct orb_app;
extern struct orb_app orb_app_usb_files_row;
#endif

#ifdef __cplusplus
}
#endif
