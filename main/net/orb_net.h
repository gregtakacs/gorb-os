/**
 * orb_net.h — WiFi station + first-boot SoftAP + the plain-HTTP config server.
 *
 * Security model, deliberately: this device speaks PLAIN HTTP only. TLS is
 * terminated upstream by a reverse proxy (dev: DEV-LLM-STACK's Traefik with a
 * real LE cert; production NAS-STACK: its own Traefik through the existing
 * passthrough rules). That is what keeps the ~185 KB of mbedTLS server-side SSL
 * and its per-session TLS buffers out of the budget, and it is why the
 * 8 KB-header / per-connection scratch sizing that once looked impossible is
 * affordable. Consequence: the device is open on the LAN with no auth — an
 * accepted trade-off for a personal dev device with no accounts on it, and the
 * thing to revisit before this is anything public-facing.
 *
 * Credentials live in their OWN NVS partition (`storage`, see partitions.csv),
 * never in esp_wifi's private `nvs.net80211`. esp_wifi with
 * CONFIG_ESP_WIFI_NVS_ENABLED=y caches whatever config it is last given in that
 * namespace, but that copy is treated as disposable: every boot re-pushes the
 * SSID/passphrase from `storage` via esp_wifi_set_config(). This is what makes
 * the credentials survive `nvs_flash_erase()`, which main.c still calls to
 * recover from a corrupt default NVS — with the driver's namespace and ours in
 * different partitions, that recovery can no longer cost the user their WiFi.
 * The one thing that DOES erase both partitions on purpose is the serial
 * `RESET ALL` (orb_net.c:net_factory_reset): a factory reset has to reach the
 * driver's copy, or it leaves a passphrase on a board that is about to change
 * hands. Surviving an accident is not the same as surviving a factory reset.
 *
 * The timezone lives in that same namespace under `tz`, for the same reason: it
 * is device config the owner set once, and reopening the case to recover a
 * corrupt partition must not quietly put the dial an hour out with nothing on
 * screen to explain it. Unset means UTC, and /api/v1/status says so rather than
 * letting a wrong-looking dial be the only symptom.
 *
 * The orb's NAME lives beside it under `name`, on the same argument and with the
 * same durability requirement — and the DEVICE, not the hub, is the authority for
 * it. The hub's fleet row keeps a copy so a card can render a fleet without
 * dialing thirty boards, but that copy is a cache: a deleted hub store or a
 * reflashed board used to cost the owner the word they typed while the device
 * went on announcing its MAC. Set it with POST /api/v1/name, or in the join body
 * during onboarding, and read it off /api/v1/status. Unset means unnamed, and the
 * status field is the empty string rather than a placeholder, so a caller can
 * still tell "never named" from "could not be asked".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

/**
 * Bring up networking: netif + event loop + esp_wifi, then either associate as
 * STA from stored credentials or open the provisioning SoftAP, then start the
 * config server.
 *
 * NEVER fatal. If any stage fails this logs it, leaves the clock running, and
 * returns the error — the display is the product, and a broken radio must not
 * take the clock down with it on a device in a sealed case.
 */
esp_err_t orb_net_start(void);

/**
 * Pin the httpd task's worst-case stack block out of the heap BEFORE the theme
 * maps, and net_start_httpd() releases it the moment the ladder runs. Guards the
 * fragmentation flavor of the headless boot — one board was seen with a 3,456 B
 * largest internal block, under the ladder's smallest tier. The other flavor
 * (refused task create with ample heap, serial capture 2026-10-26) is answered
 * by the ladder's retry timer, not by this. Call once, before orb_theme_boot();
 * never fatal — a board that cannot reserve still starts without the shield.
 */
esp_err_t orb_net_reserve_httpd_stack(void);

/** True once STA has an IP (or the SoftAP is up and reachable). */
bool orb_net_is_up(void);

/** True while serving the provisioning SoftAP with no STA link: a first boot
 *  with no credentials, or a one-boot session requested by
 *  orb_net_request_provisioning(). */
bool orb_net_is_provisioning(void);

/** True when the orb is in the persistent AP-isolated mode: it serves its own open
 *  SoftAP and web page and never associates with any Wi-Fi network, so the only way in
 *  is that AP. The mode lives in NVS (orb.net "mode" = "ap") and survives reboot until
 *  a MODE command (serial or page) changes it. */
bool orb_net_ap_isolated(void);

/**
 * D2: ask the NEXT boot to come up in provisioning even though credentials are
 * stored, so an orb on a network that no longer works can be recovered from a
 * phone. The request lives in RTC slow memory (noinit): it survives esp_restart()
 * and is consumed by that one boot. It is never written to flash, so it cannot
 * outlive a reboot loop or a botched OTA.
 *
 * "Dies with the power" means VDD removal — an unplug or a brownout. Measured on
 * the bench: a chip reset that keeps power applied (rst:0x15, RTS on EN) does NOT
 * clear it. Do not build a cancel path on the reset button.
 *
 * Returns false (having changed nothing) when this build has no SoftAP fallback.
 * Does NOT reboot — the caller decides when, and how to be polite about it.
 *
 * Writers today: none yet. D5's knob hold and any hub-initiated recovery call
 * this; D4's sojourn expiry must NOT, or the reboot it performs re-enters
 * provisioning and the session never ends.
 */
bool orb_net_request_provisioning(void);

/** The STA IPv4 as dotted-quad text, or "" when not associated. */
const char *orb_net_ip_str(void);

/**
 * True once SNTP has landed and the system clock is real (i.e. the dial may
 * read time()). False before the first sync and with CONFIG_ORB_NET_ENABLE=n,
 * which is the caller's cue to keep free-running from its own boot epoch —
 * a stale dial beats a 1970 dial.
 */
bool orb_net_time_synced(void);

/* ---- the zone table, for the on-dial menu (see main/menu/menu_view.c) --------
 *
 * These exist so the dial can offer the same zone choice the config page does
 * without a browser. They are deliberately accessors over the ONE table
 * orb_net.c already serves rather than a second copy: s_tz_tbl is the device's
 * truth about which zones exist, and the NVS write, the validation and the
 * setenv/tzset all stay inside net_tz_set() — one behaviour, three doors (the
 * page's POST, the join's optional tz field, and now the knob).
 *
 * All safe from any task: the table is const, and the only mutation is the one
 * orb_net_tz_select() makes through the same path the HTTP handler uses.
 */

/** Rows in the zone table; 0 when networking is compiled out. */
int orb_net_tz_count(void);

/** The stored key for row i (what NVS holds), or "" out of range. */
const char *orb_net_tz_key(int i);

/** The city-style label for row i, or "" out of range. */
const char *orb_net_tz_label(int i);

/** The zone currently in effect ("UTC" when none is set), or "" if unavailable. */
const char *orb_net_tz_current(void);

/** Row index of the zone currently in effect, or -1 if it is not in the table. */
int orb_net_tz_current_index(void);

/**
 * Validate, persist and apply row i — the identical sequence POST /api/v1/config
 * runs. ESP_ERR_INVALID_ARG out of range; ESP_FAIL if the device refused the
 * stored rule (a row that no longer validates), which is reported rather than
 * silently left on the old zone.
 */
esp_err_t orb_net_tz_select(int i);

/**
 * The OWNER's tz offset in effect at `when`, minutes east of UTC, DST-aware
 * (D17, plan §0.7) — the measured localtime-minus-gmtime diff, the same
 * computation the tz echo on /api/v1/status uses. Reports the zone; never
 * sets it: net_tz_set() stays the single write door. ESP_ERR_INVALID_ARG for
 * a NULL out; ESP_ERR_NOT_SUPPORTED with networking compiled out is NOT used
 * — a disabled build answers UTC (0) so the broker's entry stays a plain
 * pass-through.
 */
esp_err_t orb_net_tz_offset(time_t when, int32_t *offset_min);

/* ---- the orb's own name -----------------------------------------------------
 *
 * Accessors over the ONE copy orb_net.c keeps in s_name, so a caller cannot get a
 * stale second copy. The name is the device's, not the hub's: read it here, and
 * POST /api/v1/status mirrors it.
 */

/** The stored name, or "" when the orb is unnamed. Never NULL, always NUL-terminated,
 *  and safe from any task: the string is only ever replaced whole by net_name_set(). */
const char *orb_net_name(void);

/** The longest name this device accepts (orbfleet.MaxName, kept equal by test). */
int orb_net_name_max(void);

/**
 * Validate and persist — the identical sequence POST /api/v1/name runs, and the
 * only way to name the device from on-device code. ESP_ERR_INVALID_ARG for a NULL
 * `name`; ESP_FAIL when the device refused the string or could not write it (the
 * reason is logged, not returned); ESP_OK otherwise. `name` may be "" to clear,
 * which is not an error. Nothing on the dial displays this today; it is the record
 * the hub and any later UI read from.
 *
 * Call it after orb_net_start(). There is no not-yet-open state to report, because
 * the tz setter beside it has none either and an unopened handle simply fails the
 * write, which lands as ESP_FAIL — the same answer a refused string gives.
 */
esp_err_t orb_net_name_set(const char *name);
