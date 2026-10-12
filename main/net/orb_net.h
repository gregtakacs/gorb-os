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
 *
 * The timezone lives in that same namespace under `tz`, for the same reason: it
 * is device config the owner set once, and reopening the case to recover a
 * corrupt partition must not quietly put the dial an hour out with nothing on
 * screen to explain it. Unset means UTC, and /api/v1/status says so rather than
 * letting a wrong-looking dial be the only symptom.
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

/** True once STA has an IP (or the SoftAP is up and reachable). */
bool orb_net_is_up(void);

/** True while serving as the first-boot provisioning SoftAP with no STA link. */
bool orb_net_is_provisioning(void);

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
