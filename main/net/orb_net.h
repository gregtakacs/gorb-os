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
 */
#pragma once

#include <stdbool.h>

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
