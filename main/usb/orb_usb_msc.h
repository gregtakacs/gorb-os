/**
 * orb_usb_msc.h — the whole CARD over USB, as a disk (D38).
 *
 * The inverse of orb_usb_mtp.h, and chosen after that one failed on hardware.
 * MTP keeps /sd mounted and answers the host with paths, which is the nicer
 * design and the one that never worked: the first command of any session stalls
 * the bulk pair on both a Windows and a Linux host, and the vendor class has no
 * path that recovers from a stall (docs/usb-sd-msc-plan.md §14). MSC answers BOT
 * with read(2)/write(2) against a block device, so nothing depends on that
 * response path at all, and the host gets a drive letter.
 *
 * The trade is ownership. For the window, the orb does not have a card:
 *   - /sd is not mounted. The dial draws from flash, the clock keeps time, and
 *     the config page answers — the card is an install source, never a runtime
 *     read path, which is what makes giving it away acceptable at all.
 *   - The host owns every sector and may partition, format or erase the volume.
 *     The firmware does not write-protect it and does not care: the owner asked
 *     for a USB stick and the card's contents are the owner's to lose.
 *   - What the host writes is invisible until the card comes back, which is at
 *     reboot. There is no in-window remount: a LUN and a FatFs mount over the
 *     same blocks is two owners of one filesystem.
 *
 * The window is persistent rather than timed: it ends when the cable does, and
 * the reboot that follows ends the mode. A battery on the MX1.25 header keeps
 * the S3 alive through a replug (§4), so the arm stays one read-and-cleared NVS
 * word — a device that loses power mid-copy must come back as a clock, not as a
 * disk nobody asked for and cannot dismiss without opening the shell.
 *
 * Inert unless CONFIG_ORB_USB_MSC is set, and the two USB modes are a Kconfig
 * choice, so an MTP build compiles this file to one stub.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Read-and-clear the "next boot hands its card to the host" one-shot, and claim
 *  the card on success so the storage ladder skips the mount. Unlike the MTP
 *  claim this one is load-bearing for the mount: an MSC boot must come up with
 *  /sd unmounted, and the flag has to be set before the ladder runs to get that.
 *  Call once, early, from the boot ladder. */
bool orb_usb_msc_boot_claim(void);

/** True when the flag is set for the next boot (says nothing about this one). */
bool orb_usb_msc_armed(void);

/** Set or clear the flag for the next boot. Does not reboot and does not touch
 *  this boot: the caller owns the restart. */
esp_err_t orb_usb_msc_set_armed(bool on);

/** Become the disk: unmount /sd if it is up, attach the card raw, expose one
 *  read/write LUN, pulse the line so the host re-reads us. Blocking; worker only.
 *  ESP_ERR_NOT_FOUND — no card in the socket to hand over. */
esp_err_t orb_usb_msc_start(void);

/** Stop: reboot. There is no in-place exit — the mode gave up the console by
 *  hardware and only a boot returns it — and the card must not be re-mounted
 *  underneath a host that may still be writing to it. */
esp_err_t orb_usb_msc_stop(void);

esp_err_t orb_usb_msc_request_start(void);
esp_err_t orb_usb_msc_request_stop(void);
esp_err_t orb_usb_msc_request_disarm(void);

/** True while the mode is up, whether or not a host has enumerated it. */
bool orb_usb_msc_active(void);

/** True for the whole share window — from the press (PENDING) through the
 *  freeze to the reclaim reboot (ACTIVE). The frame the glass carries during
 *  a window is the share card (menu_view), and every knob event anywhere on
 *  the dial is the stop door (input_router) — this one flag is what both ask,
 *  and it covers the pending half-second the row-level getter alone misses. */
bool orb_usb_msc_window_open(void);

/** True only when a host has configured the device. Sampled from the TinyUSB
 *  event callback here, not polled: MSC reports ATTACHED from tud_mount_cb, and
 *  the row's repaint is what reads the flag the event set. */
bool orb_usb_msc_host_up(void);

/** "off" / "starting" / "next boot: disk" / "disk up, no host" / "host connected"
 *  / "stopping" / "failed". "starting" is the press's word: the job is posted, the
 *  card has not moved. It exists so the row repaints honestly before the share
 *  window freezes the glass — the tile ring goes on loan to the card, the flush
 *  path drops every frame after that, and the last painted frame is what the
 *  owner reads until the reclaim reboot. That frame is now the share card
 *  (menu_view's PG_SHARE): the router spends every press or hold anywhere on the
 *  dial as the stop while the window is up, so a door that must be found blind
 *  no longer exists. The USB Drive row still carries its own door for the hub's
 *  menu walk, which reads the row table rather than the glass. */
const char *orb_usb_msc_state(void);

/** The last failure as the owner should read it, "" when there is none. */
const char *orb_usb_msc_reason(void);

/** True from the moment the stop door is pressed: the mode is still the disk's
 *  — the reboot it asked for is on its 750 ms timer — but the honest word the
 *  owner should be reading, on the card and over the hub alike, is "stopping". */
bool orb_usb_msc_stopping(void);

/** The registry's service row for this feature — the same row the MTP build
 *  exports, since it is the same feature with a different class behind it. */
#if CONFIG_ORB_USB_MSC
struct orb_app;
extern struct orb_app orb_app_usb_files_row;
#endif

#ifdef __cplusplus
}
#endif
