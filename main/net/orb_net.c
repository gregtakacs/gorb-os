/**
 * orb_net.c — WiFi bring-up + config server. See orb_net.h for the model.
 *
 * This file exists partly to answer the one question the plan could not settle
 * on paper: whether esp_wifi + esp_http_server fit in the ESP32-S3's ~105 KB of
 * usable internal RAM alongside LVGL's static internal allocations. Every stage
 * therefore reports the internal / DMA / PSRAM heap, so the budget is decided by
 * measurement rather than arithmetic on datasheets. Set CONFIG_ORB_NET_MEM_DEBUG
 * off for shipping builds.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
/* Item 10 visibility: lwIP's dhcpserver.c contains no logging at all, so the
 * DHCP server's own lease table is the only trustworthy witness of whether a
 * joined client was ever handed an address. IP_EVENT_AP_STAIPASSIGNED is NOT:
 * measured 2026-10-13, it stays silent while the table reports a granted lease,
 * which is what produced a false "no lease" regression report. A netif->input
 * frame counter was tried here first and removed — IDF delivers Wi-Fi frames via
 * esp_netif_receive(), bypassing netif->input, so it read 0 during real joins. */
static void ap_lease_probe(void);   /* defined beside on_ap_lease, used by the net task */
static uint8_t s_ap_last_mac[6];    /* DIAGNOSTIC: the client we are watching */

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_attr.h"   /* D2: RTC_NOINIT_ATTR — the reboot-to-provisioning intent flag */
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "orb_net.h"
#include "input_router.h"
#include "orb_app.h"   /* h_screen: ?to=<slug> resolves through the registry */
#include "knob.h"     /* h_screen injection; h_status reads the raw/detent pair */
#include "art_bank.h"
#include "display.h"        /* h_fb: the one-shot frame capture over the panel mirror */
#include "board_config.h"   /* h_fb: SCREEN_WIDTH/HEIGHT, the row scratch's size */
#include "orb_sd.h"
#include "orb_theme.h"
#if CONFIG_ORB_USB_MTP
#include "orb_usb_mtp.h"   /* §13.3: the share window has no console, so status is its only witness */
#endif
#include "orb_app_scan.h"   /* h_apps: the /sd/apps listing and its disclosures */
#include "orb_app_install.h" /* h_apps_install: the POST that queues a card-to-flash copy */
#include "orb_jobs.h"     /* h_sd: the job-busy witness the themes page polls after a press */
#include "idle_dim.h" /* h_status: the dim's countdown and its motion high-water */
#include "orb_mdns.h"  /* the _orb._tcp advertisement the hub browses for */

static const char *TAG = "orb_net";

/* Whole module body is conditional so that CONFIG_ORB_NET_ENABLE=n really means
 * "compiled but inert": without this guard the file still references
 * CONFIG_ORB_NET_HTTPD_PORT, and because ORB_NET_HTTPD_PORT is declared
 * `depends on ORB_NET_ENABLE` in Kconfig, that symbol ceases to exist the moment
 * networking is switched off — so the clock-only baseline build did not compile
 * at all (found by building exactly that config to A/B the display frame rate).
 * The stubs below keep the public surface present so main.c needs no guards of
 * its own, and keep esp_wifi/httpd code out of the image entirely. */
#if CONFIG_ORB_NET_ENABLE

/* Credentials + settings live here, NOT in the default `nvs` partition that
 * esp_wifi owns (namespace "nvs.net80211"). See orb_net.h. */
#define NET_NVS_PARTITION "storage"
#define NET_NVS_NAMESPACE "orb.net"

#define NET_STA_RETRY_S  5      /* first association back-off; doubles from here */
#define NET_LEASE_WATCHDOG_S 15 /* an association must produce a lease within this,
                                 * or the association is dropped and the ladder
                                 * retries it from a real DISCONNECTED event */
#define NET_STA_RETRY_MAX_S 120 /* ceiling — see retry_delay_s() */
#define NET_SSID_MAX     32
#define NET_PASS_MAX     64
#define NET_TZ_MAX       64     /* a POSIX TZ rule is short by construction */
#define NET_TZ_KEY       "tz"   /* NVS: a table name OR a raw POSIX string */
#define NET_CONFIG_BODY_MAX 128 /* POST /api/v1/config is {"tz":"…"} and nothing else */

/* The orb's own name. The DEVICE is the authority for this string and the hub's
 * fleet row is a cache of it, which is the opposite way round from how the hub
 * first stored names (§ orbfleet). The reason is durability: the hub's store is
 * one JSON file, so a deleted file or a reflashed board used to cost the word an
 * owner typed, while the device went on announcing orb-44bd8d782618. A name that
 * lives in the `storage` partition beside the credentials survives both.
 *
 * 32 is not a free choice — it is orbfleet.MaxName, and the two must agree. The
 * card enforces 32 before it sends, and the device refuses 33 after that; a name
 * accepted by one and clipped by the other reads as two different orbs depending
 * on which surface you looked at last. */
#define NET_NAME_MAX     32     /* == orbfleet.MaxName; keep them equal */
#define NET_NAME_KEY     "name" /* NVS; absent means "unnamed" */
#define NET_NAME_DEFAULT "The Orb" /* the word a factory reset leaves behind; see net_factory_reset */
/* {"name":"…"} for 32 printable bytes is 41 B. Same 128 B frame as config: this
 * endpoint carries one key and nothing else, so the sizing argument from
 * NET_CONFIG_BODY_MAX applies unchanged. */
#define NET_NAME_BODY_MAX 128
/* Radio mode: the owner's choice of how this orb is reachable. Absent ("" ) means
 * the station default — join the stored network, AP only as a transient door.
 * "ap" means the isolated mode: come up as the orb's own open AP and web page, and
 * never associate, so the ONLY way in is that AP. It is persistent, not a session:
 * it survives reboot until a MODE command (serial or the page) changes it back.
 * A third value the station branch shares, "sta", is the explicit default. */
#define NET_MODE_KEY     "mode" /* NVS; absent means station */
#define NET_MODE_MAX     3      /* "ap" or "sta", NUL-terminated */

/* Everything that MUTATES THE RADIO runs on its own task, not on the default
 * event loop and not on an esp_timer callback. esp_wifi_set_mode() is explicitly
 * off-limits from inside a WiFi/IP event handler — the callback is executed in
 * the same thread the driver is waiting on — and the esp_timer task forbids
 * blocking calls, which is exactly what stopping an AP turns into (it tears
 * down beacons and deauths stations). So the event handlers, the timers and the
 * httpd endpoints all just post a command here, and one consumer does the work.
 *
 * It is also what makes a leaked-open AP impossible by construction: the AP is
 * never opened without arming its expiry in the same breath, and the only
 * places that touch the mode are the AP commands below. */
enum { NET_CMD_CONNECT = 1,   /* retry esp_wifi_connect()                   */
       NET_CMD_JOIN,          /* apply credentials posted by /wifi/join      */
       NET_CMD_AP_OPEN,       /* open the AP for a manual request           */
       NET_CMD_AP_RESCUE,     /* open the AP because the creds are not working */
       NET_CMD_AP_HOLD,       /* AP already up: (re)arm its expiry            */
       NET_CMD_AP_EXPIRED,    /* close it, unless the STA still has no lease  */
       NET_CMD_SCAN,          /* start the async scan — driver calls, radio task */
       NET_CMD_SCAN_DONE,     /* drain the results once the driver says done     */
       NET_CMD_SCAN_BOOT,     /* the ARMED console scan: first-boot, or a serial SCAN */
       NET_CMD_LEASE_WATCH,   /* associated but no lease: rearm the watch or drop it */
       NET_CMD_STORE_CREDS,   /* a lease verified the pending pair: write it to NVS now  */
       NET_CMD_AP_LEASE_POLL }; /* DIAGNOSTIC: watch the SoftAP's lease table after a join */
static QueueHandle_t      s_net_q;
static TaskHandle_t       s_net_task;
static esp_timer_handle_t s_retry_timer;   /* one-shot, re-armed per failure */
static esp_timer_handle_t s_ap_timer;      /* one-shot, AP expiry            */
static esp_timer_handle_t s_watch_timer;   /* one-shot, lease watchdog       */

static esp_netif_t   *s_sta_netif;
static esp_netif_t   *s_ap_netif;
static httpd_handle_t s_server;
static nvs_handle_t   s_nvs;

static volatile bool s_sta_up;         /* has an IP */
static volatile bool s_associated;     /* the association is real; the lease may not be */
static volatile bool s_joining;        /* a user-initiated join is in flight */
static volatile bool s_provisioning;   /* booted with no credentials at all */
/* D4: this provisioning boot happened DESPITE credentials being present in
 * flash, so it is a recovery session with something to fall back to and it owns
 * a deadline. A factory boot (nothing stored) leaves this false forever: for that
 * device the AP is the only door there is, and rebooting returns to the same
 * branch. Latched once where the boot branch reads NVS, never re-read after. */
static volatile bool s_prov_recovery_session;

/* ── D2: the reboot-to-provisioning intent flag ────────────────────────────
 * One word of RTC NOINIT memory, and nothing else. Chosen over NVS because its
 * retention is exactly the semantics wanted: it survives esp_restart() (the
 * bootloader only skips the segment, it does not clear it — esp_image_format.c:852
 * "Skipping RTC slow memory segment"; the section is (NOLOAD) in the S3
 * sections.ld.in:93, and no startup code zeroes it), but a power cycle wipes the
 * RTC domain and takes the request with it. A request interrupted by a power loss
 * therefore CANNOT strand the orb in AP mode, which an NVS key would.
 *
 * The noinit caveat is the reason this is a magic and not a bool: at cold
 * power-on the word holds whatever the SRAM came up with. Only the exact magic
 * counts, and the boot branch scrubs the word on every read, so one boot's
 * garbage can never be mistaken for intent and can never linger.
 *
 * Measured retention, bench 2026-10-13 (orb-B, this commit's probe build):
 * survives esp_restart() — rst:0xc RTC_SW_CPU_RST — and FORCES the provisioning
 * branch exactly once. Also survives a chip reset that keeps VDD applied: an
 * RTS-on-EN reset (rst:0x15 USB_UART_CHIP_RESET) came up forced as well, because
 * the RTC power domain never lost power. So "a power cycle clears it" means
 * removing VDD (unplug / brownout), NOT pressing the reset button — do not write
 * a recovery flow that assumes a button press cancels a pending request. The
 * safety argument does not rest on that assumption either way: D4's sojourn
 * bounds any forced session and the boot branch consumes the word on read, so the
 * worst a stale request can do is cost one extra provisioning boot. */
#define PROV_INTENT_MAGIC 0x4F524250u   /* 'O' 'R' 'B' 'P' */
static RTC_NOINIT_ATTR uint32_t s_prov_intent;

/* Pure so tools/host_tz_test.py can pin the garbage rows (0, all-ones, off-by-one)
 * that are the whole reason for a magic rather than a flag. */
static bool prov_intent_armed(uint32_t w)
{
    return w == PROV_INTENT_MAGIC;
}

/* Read-and-clear, called exactly once per boot by the boot branch. Zeroing on
 * every read (not only on a hit) is what keeps a cold-boot garbage word from
 * surviving to be misread later, and makes the request good for exactly one
 * provisioning boot. */
static bool prov_intent_consume(void)
{
    const bool armed = prov_intent_armed(s_prov_intent);
    s_prov_intent = 0;
    return armed;
}

/* Roll a just-armed request back. Only the armed-then-could-not-reboot path needs it:
 * leaving the word set would boot the next power cycle into a provisioning session
 * nobody asked for, and the caller just got a 500 telling it nothing happened. */
static void prov_intent_disarm(void)
{
    s_prov_intent = 0;
}
static volatile bool s_ap_up;          /* SoftAP is actually broadcasting */
/* Set only when a human asked for the AP (the config page / POST /wifi/ap).
 * It exists because every STA lease re-arms the AP timer with the short
 * CONFIG_ORB_NET_AP_HOLD_S backstop, which is right for the boot and rescue APs
 * — their whole purpose is to close once the orb is reachable — but fatal for a
 * manual one: the orb already HAS a lease the instant you open this AP, so the
 * fuse relights to 120 s while you are walking to your phone. Measured on orb-B
 * 2026-10-13: opened at 29.7 s logging "900 s hold", leased at 37.5 s, closed at
 * 157.5 s. The phone saw no SSID at all, which is how "I don't see the AP" was
 * produced twice. s_ap_hold_s is what the opener promised, so a short
 * backstop can never preempt a longer promise. */
static volatile bool     s_ap_manual;
static volatile uint32_t s_ap_hold_s;
static volatile int  s_rssi = -999;
static char          s_ip[16] = "";

/* Consecutive failed associations since the last lease. Drives BOTH the back-off
 * and the rescue-AP threshold, and is cleared only by a real lease — a device
 * that flaps on a marginal signal accumulates attempts and gets the AP, which is
 * the correct outcome: it cannot reach the LAN either. */
static volatile unsigned s_assoc_fails;
/* Set when a retry is armed, cleared when the attempt actually goes out. The
 * driver can report ONE failed association as TWO WIFI_EVENT_STA_DISCONNECTED
 * events (seen on the bench: attempt 1 at t+6.1 s, attempt 2 at t+8.5 s with a
 * 5 s timer in between). Without this latch the counter runs at double speed,
 * the ladder escalates on the duplicate, and the rescue AP opens after roughly
 * half the configured attempts. */
static volatile bool s_retry_pending;
/* The 802.11 reason code from the last disconnect, reported by /api/v1/status.
 * Chosen over "the orb is not connecting": 201 (network not here), 15 (wrong
 * key) and 2 (router let the auth lapse) look identical from the LAN and need
 * different fixes. Sticky through a later lease on purpose — after a flappy
 * night it is still the only evidence of what the router did. */
static volatile uint8_t s_last_reason;

/* Handlers below are registered by pointer, so the apply helper used by the
 * join endpoint needs a prototype ahead of its definition. */
static esp_err_t net_apply_sta_config(const char *ssid, const char *pass);

/* --------------------------------------------------------------------------
 * Heap instrumentation — the reason this build exists.
 * ------------------------------------------------------------------------ */
static void stage_probe(const char *stage)
{
#if CONFIG_ORB_NET_MEM_DEBUG
    const size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t i_lrg  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t d_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t p_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* INTERNAL is the column that decides whether this build works at all:
     * WiFi's RX/management buffers and lwIP's sockets must come from it, and it
     * is the one pool LVGL cannot dodge into PSRAM. Watch for the largest-block
     * collapse — fragmentation kills this build before free-bytes run out. */
    ESP_LOGI(TAG, "[heap] %-16s internal free=%u largest=%u | dma_free=%u | psram_free=%u",
             stage, (unsigned)i_free, (unsigned)i_lrg, (unsigned)d_free, (unsigned)p_free);
#else
    (void)stage;
#endif
}

/* --------------------------------------------------------------------------
 * Minimal JSON — one flat object of known keys.
 *
 * Deliberately not cJSON: these bodies are tiny, closed-schema, and generated by
 * our own page, so a bounded scan costs a few dozen lines and keeps the cJSON
 * component out of a build that budgets internal RAM by the kilobyte.
 * ------------------------------------------------------------------------ */
static const char *json_find(const char *body, size_t len, const char *key)
{
    const size_t klen = strlen(key);
    for (const char *p = body; (size_t)(p - body) + klen + 2 <= len; p++) {
        if (p[0] != '"') continue;
        if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"') continue;
        /* A match only counts at KEY position: the previous non-space character has
         * to be '{' or ',' (or the start of the body). Without that, the body
         * {"x":"tz","tz":"UTC"} matches the VALUE "tz" first, finds a ',' where a
         * colon belongs, and the real key later in the body is never reached — so a
         * perfectly good request gets a 400 it cannot explain. Short keys used as
         * values elsewhere are exactly what a client echoing its own state produces,
         * and the same helper parses ssid/pass, where a shadowed key would send the
         * device joining the wrong network. */
        const char *q = p;
        while (q > body && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\n' || q[-1] == '\r')) q--;
        if (q > body && q[-1] != '{' && q[-1] != ',') continue;
        const char *v = p + klen + 2;                    /* past the closing quote */
        while (v < body + len && (*v == ' ' || *v == ':')) v++;
        return v;
    }
    return NULL;
}

static bool json_str(const char *body, size_t len, const char *key, char *out, size_t out_sz)
{
    const char *v = json_find(body, len, key);
    if (!v || *v != '"') return false;
    v++;                                                  /* past opening quote */
    size_t i = 0;
    bool    too_long = false;
    while (v < body + len && *v != '"') {
        if (*v == '\\') {                                 /* drop escape, keep value */
            v++;
            if (v >= body + len) break;
        }
        if (i + 1 >= out_sz) { too_long = true; break; }
        out[i++] = *v++;
    }
    out[i] = '\0';
    /* An overlong value is REJECTED, never silently truncated. Truncating is the
     * worse failure mode: a 40-char SSID quietly becoming its first 32 chars
     * sends the device hunting for a network that does not exist, and the user
     * gets an unexplained join failure with no hint that the name was too long.
     * (Caught by the host harness before this was ever flashed.) */
    if (too_long) return false;
    return i > 0;
}

/* (The numeric-argument helper lands with the option-registry endpoint
 * `POST /api/v1/menu` (docs/orb-manager-plan.md §5.3 item 8), which drives the
 * same opt_* rows the dial renders; carrying it unused would only trip
 * -Wunused-function. The older note here pointed at "PATCH /api/v1/settings in
 * Phase 4", and that numbering was the onboarding plan's own, not
 * app-platform-plan.md's P4, and no such endpoint is planned.) */

/* --------------------------------------------------------------------------
 * Timezone — device config, never a firmware constant.
 *
 * The zone is per-device state owned by whoever holds the device, so it lives in
 * the same `storage`/orb.net NVS as the credentials (and inherits surviving
 * nvs_flash_erase() for the same reason: a sealed clock on a wall must not lose
 * its settings because a different partition went corrupt).
 *
 * What is stored is what the client sent: one of the names below OR a RAW POSIX
 * string. Both are legal on the wire — there is no zoneinfo in this image
 * (esp_timezones is ~470 KB of it), so the named table is UI sugar over the
 * POSIX rules it resolves to, and accepting the raw form is what keeps every
 * zone reachable without a firmware change. Storing the name rather than
 * resolving-then-storing is deliberate: it lets /api/v1/config echo a value the
 * config page can match against its own <option> list, so a reloaded page
 * pre-selects the zone it was set to instead of showing a rule nobody typed.
 *
 * Nothing here knows about SNTP: TZ only interprets the epoch, it never sets it.
 * ------------------------------------------------------------------------ */
typedef struct { const char *name; const char *posix; const char *iana; const char *label; } tz_entry_t;

/* Keys are what the device STORES and resolves — IANA-style so they still
 * work as keys if esp_timezones ever lands — and they are frozen: NVS holds
 * the served string verbatim and tz_resolve() looks it up by key, so renaming
 * one silently un-sets every device already provisioned to it.
 * `iana` is the canonical spelling the phone world uses (Android reports
 * America/Chicago, never US/Central) and exists purely so the clock page can
 * match the phone's own zone to a table row exactly. `label` is the city a
 * person would recognize; the clock page renders "(GMT-06:00) Chicago", never
 * these keys, and never a POSIX rule — that was the old UX and the owner hated it.
 * The rules are the post-2007 POSIX forms — M3.2.0 = second Sunday
 * of March, the /n suffix naming the weekday the transition lands on. US/Arizona
 * is its own row precisely BECAUSE it is MST7 with no DST component: handing it
 * "MST7MDT,..." is the classic way to put a no-DST state an hour off for half the
 * year, and the only thing that ever catches that is someone glancing at the dial. */
static const tz_entry_t s_tz_tbl[] = {
    { "UTC",            "UTC0",                       "UTC",                 "UTC"                       },
    { "US/Eastern",     "EST5EDT,M3.2.0,M11.1.0",     "America/New_York",    "New York"                  },
    { "US/Central",     "CST6CDT,M3.2.0,M11.1.0",     "America/Chicago",     "Chicago"                   },
    { "US/Mountain",    "MST7MDT,M3.2.0,M11.1.0",     "America/Denver",      "Denver"                    },
    { "US/Pacific",     "PST8PDT,M3.2.0,M11.1.0",     "America/Los_Angeles", "Los Angeles"               },
    { "US/Arizona",     "MST7",                       "America/Phoenix",     "Phoenix (no clock change)"  },
    { "Europe/London",  "GMT0BST,M3.5.0/1,M10.5.0",   "Europe/London",       "London"                    },
    { "Europe/Berlin",  "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Berlin",       "Berlin"                    },
    { "Asia/Tokyo",     "JST-9",                      "Asia/Tokyo",          "Tokyo"                     },
    /* The rows below are NOT hand-typed rules: every one was generated from
     * the IANA zone of the same name by tools/host_tz_test.py's reference
     * derivation and machine-verified against it (host libc applying the
     * shipped string must reproduce zoneinfo minute-for-minute around every
     * transition of 2025-2028 — the test re-runs that proof on every
     * invocation). Re-generate rather than edit by hand: the classic failure
     * of hand-written zone tables is an /n time given in the wrong side's
     * local clock (EU rows ending M10.5.0/2 instead of /3), which is a whole
     * hour of silent wrongness for six months a year. Zones whose clock
     * changes follow a non-solar calendar (Asia/Jerusalem, Africa/Casablanca)
     * cannot be expressed as any static POSIX rule at all and are deliberately
     * absent — a phone there lands on the clock page's honest fixed-offset
     * row instead of a table entry that would drift. */
    { "America/Anchorage",             "ANCHO9ANCHOD8,M3.2.0,M11.1.0",              "America/Anchorage",               "Anchorage"                },
    { "Pacific/Honolulu",              "HONOL10",                                   "Pacific/Honolulu",                "Honolulu"                 },
    { "America/Toronto",               "TORON5TOROND4,M3.2.0,M11.1.0",              "America/Toronto",                 "Toronto"                  },
    { "America/Mexico_City",           "MEXIC6",                                    "America/Mexico_City",             "Mexico City"              },
    { "America/Guatemala",             "GUATE6",                                    "America/Guatemala",               "Guatemala City"           },
    { "America/Caracas",               "CARAC4",                                    "America/Caracas",                 "Caracas"                  },
    { "America/Bogota",                "BOGOT5",                                    "America/Bogota",                  "Bogota"                   },
    { "America/Lima",                  "LIMA5",                                     "America/Lima",                    "Lima"                     },
    { "America/Sao_Paulo",             "SAOPA3",                                    "America/Sao_Paulo",               "Sao Paulo"                },
    { "America/Santiago",              "SANTI4SANTID3,M9.1.0/0,M4.1.0/0",           "America/Santiago",                "Santiago"                 },
    { "America/Argentina/Buenos_Aires", "BUENO3",                                    "America/Argentina/Buenos_Aires",  "Buenos Aires"             },
    { "America/Montevideo",            "MONTE3",                                    "America/Montevideo",              "Montevideo"               },
    { "Europe/Paris",                  "PARIS-1PARISD-2,M3.5.0,M10.5.0/3",          "Europe/Paris",                    "Paris"                    },
    { "Europe/Madrid",                 "MADRI-1MADRID-2,M3.5.0,M10.5.0/3",          "Europe/Madrid",                   "Madrid"                   },
    { "Europe/Rome",                   "ROME-1ROMED-2,M3.5.0,M10.5.0/3",            "Europe/Rome",                     "Rome"                     },
    { "Europe/Athens",                 "ATHEN-2ATHEND-3,M3.5.0/3,M10.5.0/4",        "Europe/Athens",                   "Athens"                   },
    { "Europe/Bucharest",              "BUCHA-2BUCHAD-3,M3.5.0/3,M10.5.0/4",        "Europe/Bucharest",                "Bucharest"                },
    { "Europe/Kyiv",                   "KYIV-2KYIVD-3,M3.5.0/3,M10.5.0/4",          "Europe/Kyiv",                     "Kyiv"                     },
    { "Europe/Moscow",                 "MOSCO-3",                                   "Europe/Moscow",                   "Moscow"                   },
    { "Europe/Dublin",                 "DUBLI0DUBLID-1,M3.5.0/1,M10.5.0",           "Europe/Dublin",                   "Dublin"                   },
    { "Europe/Lisbon",                 "LISBO0LISBOD-1,M3.5.0/1,M10.5.0",           "Europe/Lisbon",                   "Lisbon"                   },
    { "Europe/Istanbul",               "ISTAN-3",                                   "Europe/Istanbul",                 "Istanbul"                 },
    { "Africa/Cairo",                  "CAIRO-2CAIROD-3,M4.5.5/0,M10.5.5/0",        "Africa/Cairo",                    "Cairo"                    },
    { "Africa/Lagos",                  "LAGOS-1",                                   "Africa/Lagos",                    "Lagos"                    },
    { "Africa/Nairobi",                "NAIRO-3",                                   "Africa/Nairobi",                  "Nairobi"                  },
    { "Africa/Johannesburg",           "JOHAN-2",                                   "Africa/Johannesburg",             "Johannesburg"             },
    { "Africa/Algiers",                "ALGIE-1",                                   "Africa/Algiers",                  "Algiers"                  },
    { "Africa/Tripoli",                "TRIPO-2",                                   "Africa/Tripoli",                  "Tripoli"                  },
    { "Asia/Dubai",                    "DUBAI-4",                                   "Asia/Dubai",                      "Dubai"                    },
    { "Asia/Karachi",                  "KARAC-5",                                   "Asia/Karachi",                    "Karachi"                  },
    { "Asia/Kolkata",                  "KOLKA-5:30",                                "Asia/Kolkata",                    "Kolkata"                  },
    { "Asia/Dhaka",                    "DHAKA-6",                                   "Asia/Dhaka",                      "Dhaka"                    },
    { "Asia/Tehran",                   "TEHRA-3:30",                                "Asia/Tehran",                     "Tehran"                   },
    { "Asia/Yangon",                   "YANGO-6:30",                                "Asia/Yangon",                     "Yangon"                   },
    { "Asia/Kathmandu",                "KATHM-5:45",                                "Asia/Kathmandu",                  "Kathmandu"                },
    { "Asia/Tashkent",                 "TASHK-5",                                   "Asia/Tashkent",                   "Tashkent"                 },
    { "Asia/Almaty",                   "ALMAT-5",                                   "Asia/Almaty",                     "Almaty"                   },
    { "Asia/Baku",                     "BAKU-4",                                    "Asia/Baku",                       "Baku"                     },
    { "Asia/Bangkok",                  "BANGK-7",                                   "Asia/Bangkok",                    "Bangkok"                  },
    { "Asia/Singapore",                "SINGA-8",                                   "Asia/Singapore",                  "Singapore"                },
    { "Asia/Hong_Kong",                "HONGK-8",                                   "Asia/Hong_Kong",                  "Hong Kong"                },
    { "Asia/Shanghai",                 "SHANG-8",                                   "Asia/Shanghai",                   "Shanghai"                 },
    { "Asia/Taipei",                   "TAIPE-8",                                   "Asia/Taipei",                     "Taipei"                   },
    { "Asia/Seoul",                    "SEOUL-9",                                   "Asia/Seoul",                      "Seoul"                    },
    { "Australia/Perth",               "PERTH-8",                                   "Australia/Perth",                 "Perth"                    },
    { "Australia/Brisbane",            "BRISB-10",                                  "Australia/Brisbane",              "Brisbane"                 },
    { "Australia/Adelaide",            "ADELA-9:30ADELAD-10:30,M10.1.0,M4.1.0/3",   "Australia/Adelaide",              "Adelaide"                 },
    { "Australia/Darwin",              "DARWI-9:30",                                "Australia/Darwin",                "Darwin"                   },
    { "Australia/Hobart",              "HOBAR-10HOBARD-11,M10.1.0,M4.1.0/3",        "Australia/Hobart",                "Hobart"                   },
    { "Pacific/Auckland",              "AUCKL-12AUCKLD-13,M9.5.0,M4.1.0/3",         "Pacific/Auckland",                "Auckland"                 },
    { "Pacific/Fiji",                  "FIJI-12",                                   "Pacific/Fiji",                    "Fiji"                     },
    { "Pacific/Apia",                  "APIA-13",                                   "Pacific/Apia",                    "Apia"                     },
    { "Pacific/Chatham",               "CHATH-12:45CHATHD-13:45,M9.5.0/2:45,M4.1.0/3:45", "Pacific/Chatham",                 "Chatham Islands"          },
};
#define NET_TZ_N ((int)(sizeof(s_tz_tbl) / sizeof(s_tz_tbl[0])))

/* The setting exactly as NVS holds it ("" = never set = UTC). Kept in RAM
 * because /api/v1/status is polled at a few Hz by the config page and must not
 * touch flash to say what zone it is in. Written only from orb_net_start() and
 * the serial httpd task, so no lock is needed. */
static char s_tz[NET_TZ_MAX + 1];

/* The name exactly as NVS holds it ("" = unnamed), mirrored in RAM for the same
 * reason s_tz is: /api/v1/status is polled at a few Hz and must not touch flash
 * to say what it is called. Same writer rule — orb_net_start() and the serial
 * httpd task — so no lock. */
static char s_name[NET_NAME_MAX + 1];

/* The radio mode as NVS holds it ("" = station, "ap" = isolated AP-only), mirrored
 * in RAM for the same reason as s_name: the boot branch and /api/v1/status both read
 * it, and neither should touch flash to say how the device is configured to behave.
 * Only net_mode_load() at boot and net_mode_set() write it. */
static char s_mode[NET_MODE_MAX + 1];

/* One-shot flag set by a serial SCAN command on a configured orb (state c), telling
 * scan_collect() to also mirror the results to the console as PROVSCAN lines exactly
 * as the provisioning boot does. The page scan stays quiet; a hub that asked over the
 * wire gets the list it could not fetch over a LAN it deliberately has no route to.
 * Cleared after one emission so an ordinary later scan is not echoed too. */
static volatile bool s_serial_scan_wanted;



/* Days since 1970-01-01 for a broken-down date (Howard Hinnant's civil-days
 * algorithm; exact across the whole Gregorian range this device can hold).
 * Hand-rolled because this newlib has NO timegm(): its libc.a exports tzset,
 * localtime_r and gmtime_r but not timegm, so the plan's "two timegm() calls and
 * subtract" cannot even be declared, let alone linked. */
static long days_from_civil(long y, long m, long d)
{
    y -= (m <= 2);
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Minutes EAST of UTC in effect at `at` — measured, never asked for. Asked for
 * is not an option: this newlib only populates tm_gmtoff on the zoneinfo path,
 * and the DST decision cached by the last tzset() belongs to whatever the clock
 * said AT THAT MOMENT, which on the post-power-cut path is 1970. Diffing
 * localtime against gmtime of the SAME instant is correct in both directions,
 * honors DST, and trusts no library state. */
static int tz_offset_min(time_t at)
{
    struct tm lt, gt;
    if (!localtime_r(&at, &lt) || !gmtime_r(&at, &gt)) return 0;
    const long dl = days_from_civil((long)lt.tm_year + 1900, (long)lt.tm_mon + 1, lt.tm_mday);
    const long dg = days_from_civil((long)gt.tm_year + 1900, (long)gt.tm_mon + 1, gt.tm_mday);
    return (int)(((dl - dg) * 86400L + (long)(lt.tm_hour - gt.tm_hour) * 3600L
                  + (long)(lt.tm_min - gt.tm_min) * 60L
                  + (long)(lt.tm_sec - gt.tm_sec)) / 60);
}

/* NULL when acceptable, else the reason to hand back in the 400. Printable ASCII
 * only and no quote/backslash, because this value is echoed straight into JSON by
 * /api/v1/status and /api/v1/config. That check is not belt-and-braces:
 * json_str() DROPS the backslash of an escape and keeps the character after it,
 * so {"tz":"A\"B"} arrives here as the three characters A"B — a stored quote
 * would let a client forge keys inside the device's own replies.
 *
 * A malformed-but-printable rule ("XX3") is deliberately ACCEPTED, per the plan:
 * newlib degrades it to UTC silently rather than failing, and the safety valve is
 * the tz_offset echo — the page can then ask "showing +0:00, right?" instead of
 * the owner staring at a wrong dial wondering what happened. */
static const char *tz_reject_reason(const char *tz)
{
    size_t n = 0;
    for (const char *p = tz; *p; p++, n++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7e)  return "tz must be printable ASCII";
        if (c == '"' || c == '\\') return "tz may not contain quotes";
    }
    if (n == 0) return "tz empty (send \"UTC\")";
    if (n > NET_TZ_MAX) return "tz too long";
    return NULL;
}

/* NULL when acceptable, else the reason to hand back in the 400.
 *
 * Printable ASCII only, no quote and no backslash, for the same reason and by the
 * same argument as tz_reject_reason: the value is echoed straight into JSON with
 * %s by /api/v1/status, and json_str() DROPS the backslash of an escape and keeps
 * the character after it — so an accepted escape sequence becomes a stored quote
 * one hop later, and a stored quote lets the client forge keys inside the
 * device's own replies.
 *
 * Two checks tz does not have, and both exist because a SECOND component now
 * enforces this string. orbfleet.cleanWord collapses runs of whitespace with
 * strings.Fields and rejects < and >. A device that accepted "  Hall  " would
 * store a string the hub would have cleaned before sending, so the operator's
 * own card and the orb's own status line would disagree by invisible bytes —
 * precisely the drift this change sets out to remove. Refusing an unnormalized
 * value is cheaper than normalizing it here and then disagreeing about who did
 * the normalizing: the hub has already collapsed the spaces before it dials, so
 * anything arriving here that still needs collapsing came from a client that did
 * not go through the card.
 *
 * The visible consequence of putting the rules on the device rather than trusting
 * the card: a non-Latin name the hub used to store ("Café") is now refused here,
 * and the card surfaces that refusal rather than storing a word the orb will
 * never answer to. The hub rejects it before dialing — see the ASCII guard in
 * orbfleet.cleanNameASCII — so the operator sees one message, not a 400 from an
 * IP. */
static const char *name_reject_reason(const char *name)
{
    size_t n = 0;
    char prev = ' ';
    for (const char *p = name; *p; p++, n++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7e)  return "name must be printable ASCII";
        if (c == '"' || c == '\\') return "name may not contain quotes";
        if (c == '<' || c == '>')  return "name may not contain angle brackets";
        if (c == ' ' && prev == ' ') return "name may not contain a double space";
        prev = c;
    }
    if (n == 0) return "name empty (send \"\" to clear)";
    if (n > NET_NAME_MAX) return "name too long";
    if (name[0] == ' ' || name[n - 1] == ' ') return "name may not start or end with a space";
    return NULL;
}

/* Pull the "name" out of a request body, same contract and same reasoning as
 * tz_body_value: *present distinguishes an absent key from {"name":""}, and the
 * escape scan runs BEFORE json_str() because that helper would hand back the
 * collapsed form of an escape and the guard above would then wave through what it
 * cannot see. Empty is a legitimate CLEAR here, not an error the way an empty tz
 * would be meaningless. */
static const char *name_body_value(const char *body, size_t len, char *out, size_t out_sz,
                                   bool *present)
{
    out[0] = '\0';
    *present = false;
    const char *v = json_find(body, len, NET_NAME_KEY);
    if (!v) return NULL;
    *present = true;
    if (v[0] == '"' && v[1] == '"') return NULL;                 /* clear it */
    for (const char *p = v + 1; p < body + len && *p != '"'; p++) {
        if (*p == '\\') return "name may not contain an escape";
    }
    if (!json_str(body, len, NET_NAME_KEY, out, out_sz)) return "name not a string, or too long";
    return NULL;
}

/* Pull the optional "tz" out of a request body: *present says whether the key was
 * there at all, out carries the value, and "" means an explicit {"tz":""}, i.e.
 * clear back to never-set. Returns NULL to accept, else the reason to 400.
 *
 * The empty case has to be looked for BEFORE json_str(): that helper reports a
 * missing-quote, an empty value, and an overlong one all as plain false, but here
 * empty and overlong mean opposite things — one is a request to clear the zone,
 * the other is a truncated value that must never reach flash. */
static const char *tz_body_value(const char *body, size_t len, char *out, size_t out_sz,
                                 bool *present)
{
    out[0] = '\0';
    *present = false;
    const char *v = json_find(body, len, NET_TZ_KEY);
    if (!v) return NULL;
    *present = true;
    if (v[0] == '"' && v[1] == '"') return NULL;                 /* clear it */
    /* Refuse an escape outright, for the same reason net_tz_set validates before
     * it writes: json_str() DROPS the backslash and keeps the character after it,
     * so {"tz":"EST5EDT\t"} would otherwise arrive at the guard as the printable
     * 8 characters EST5EDTt — stored, applied as TZ, silently not what anyone
     * typed. A POSIX rule has no use for a backslash, so the escape itself is
     * what is wrong here, not its collapsed result. A RAW control byte is a
     * separate case that this loop must not try to handle: 0x09 in a JSON string
     * is illegal but this parser never checks, and only tz_reject_reason, which
     * sees the decoded value, catches it. */
    for (const char *p = v + 1; p < body + len && *p != '"'; p++) {
        if (*p == '\\') return "tz may not contain an escape";
    }
    if (!json_str(body, len, NET_TZ_KEY, out, out_sz)) return "tz not a string, or too long";
    return NULL;
}

/* Table name -> its POSIX rule. Anything unrecognised IS already the rule. */
static void tz_resolve(const char *stored, char *out, size_t out_sz)
{
    for (int i = 0; i < NET_TZ_N; i++) {
        if (strcmp(s_tz_tbl[i].name, stored) == 0) {
            snprintf(out, out_sz, "%s", s_tz_tbl[i].posix);
            return;
        }
    }
    snprintf(out, out_sz, "%s", stored[0] ? stored : "UTC0");
}

/* setenv + tzset is precisely IDF's own documented runtime pattern — the same
 * pair esp_timezones' esp_timezone_set() uses, and the one in
 * docs/en/api-reference/system/system_time.rst — so changing the zone from the
 * httpd task while the LVGL task renders is a supported call shape, not a novel
 * (newlib takes its own tz lock inside localtime_r for exactly this reason).
 * The dial needs no notification: compose_now() calls localtime_r 13x a second,
 * so the frame after this is already in the new zone — no reboot, and the owner
 * sees the dial move under their hand. */
static void tz_apply(const char *stored)
{
    char posix[NET_TZ_MAX + 1];
    tz_resolve(stored, posix, sizeof(posix));
    setenv("TZ", posix, 1);
    tzset();
    ESP_LOGI(TAG, "tz: \"%s\" -> TZ=\"%s\" (%d min east of UTC)", stored, posix,
             tz_offset_min(time(NULL)));
}

/* Load + apply during bring-up. A stored value that no longer validates (a
 * hand-written NVS, or firmware that has since tightened tz_reject_reason)
 * degrades to UTC and says so in the log: the alternative is feeding setenv()
 * garbage, which newlib would also turn into UTC — just with nobody told. */
static void net_tz_load(void)
{
    s_tz[0] = '\0';
    size_t sz = sizeof(s_tz);
    if (nvs_get_str(s_nvs, NET_TZ_KEY, s_tz, &sz) != ESP_OK) s_tz[0] = '\0';
    const char *why = s_tz[0] ? tz_reject_reason(s_tz) : NULL;
    if (why) {
        ESP_LOGW(TAG, "stored tz rejected (%s); using UTC", why);
        s_tz[0] = '\0';
    }
    tz_apply(s_tz);
}

/* Validate, persist, apply — in that order, so a zone the device will not accept
 * is never in flash and one it does survives the next reboot before the HTTP
 * reply claims it is set (the same persist-before-apply rule POST /wifi/join
 * follows, and the same reason: the user is not watching a serial console).
 *
 * "" is the one value that does not need validating: it is an explicit CLEAR, back
 * to the never-set state the device boots in, so the key is erased rather than
 * written empty — status then reads "UTC" from the same code path as a factory
 * device, not from a special case around a stored empty string. */
static const char *net_tz_set(const char *tz)
{
    if (!tz[0]) {
        nvs_erase_key(s_nvs, NET_TZ_KEY);
        nvs_commit(s_nvs);
        s_tz[0] = '\0';
        tz_apply(s_tz);
        return NULL;
    }
    const char *why = tz_reject_reason(tz);
    if (why) return why;
    if (nvs_set_str(s_nvs, NET_TZ_KEY, tz) != ESP_OK) return "tz not stored (nvs)";
    /* The value is in flash and in s_tz either way, so a failed commit costs
     * durability, not correctness — say which it cost instead of failing a
     * request whose effect the user can already see on the dial. */
    if (nvs_commit(s_nvs) != ESP_OK) ESP_LOGW(TAG, "tz: commit failed (kept until reboot)");
    snprintf(s_tz, sizeof(s_tz), "%s", tz);
    tz_apply(s_tz);
    return NULL;
}

/* Load during bring-up, right beside net_tz_load(). A stored value that no longer
 * validates (a hand-written NVS, or firmware that has since tightened
 * name_reject_reason) degrades to unnamed rather than being echoed: status would
 * otherwise put bytes into a JSON string that the device's own guard says do not
 * belong there. Degrading is safe here in a way it is not for tz — an unnamed orb
 * still announces itself by MAC and still works. */
static void net_name_load(void)
{
    s_name[0] = '\0';
    size_t sz = sizeof(s_name);
    if (nvs_get_str(s_nvs, NET_NAME_KEY, s_name, &sz) != ESP_OK) s_name[0] = '\0';
    const char *why = s_name[0] ? name_reject_reason(s_name) : NULL;
    if (why) {
        ESP_LOGW(TAG, "stored name rejected (%s); leaving unnamed", why);
        s_name[0] = '\0';
    }
}

/* Validate, then persist, then mirror in RAM — the order net_tz_set() uses and for
 * the same reason: a value the device will not echo must never reach flash.
 *
 * "" is an explicit CLEAR and erases the key rather than writing it empty, so
 * status reads the same "" from a cleared orb and from a factory one, with no
 * special case around a stored empty string.
 *
 * Nothing applies. A zone changes what the dial shows the instant it lands; a name
 * changes nothing the device displays today, so the only effect worth reporting is
 * the one the caller already has — the value it just sent. */
static const char *net_name_set(const char *name)
{
    if (!name[0]) {
        nvs_erase_key(s_nvs, NET_NAME_KEY);
        nvs_commit(s_nvs);
        s_name[0] = '\0';
        return NULL;
    }
    const char *why = name_reject_reason(name);
    if (why) return why;
    if (nvs_set_str(s_nvs, NET_NAME_KEY, name) != ESP_OK) return "name not stored (nvs)";
    /* Same rule as the tz commit below: the value is in flash and in s_name
     * either way, so a failed commit costs durability across a reboot, not the
     * correctness of the reply the caller is about to see. */
    if (nvs_commit(s_nvs) != ESP_OK) ESP_LOGW(TAG, "name: commit failed (kept until reboot)");
    snprintf(s_name, sizeof(s_name), "%s", name);
    return NULL;
}

/* Load the mode once at boot, before the branch that reads it. Anything other than
 * the exact "ap" string — absent, corrupt, "AP" — reads as the station default, so a
 * half-written value never strands an orb unreachable from its own network. */
static void net_mode_load(void)
{
    s_mode[0] = '\0';
    size_t sz = sizeof(s_mode);
    if (nvs_get_str(s_nvs, NET_MODE_KEY, s_mode, &sz) != ESP_OK) s_mode[0] = '\0';
    if (strcmp(s_mode, "ap") != 0) s_mode[0] = '\0';   /* only "ap" is a distinct mode */
}

bool orb_net_ap_isolated(void)
{
#if CONFIG_ORB_NET_AP_ISOLATED
    return s_mode[0] == 'a';   /* "ap" */
#else
    return false;              /* mode feature compiled out; never report isolation */
#endif
}

/* Validate, persist, mirror — the net_tz_set()/net_name_set() order, and it changes
 * NOTHING live: the mode is a boot-time choice, so the caller reboots to make it real
 * (see the MODE serial command and POST /api/v1/wifi/mode). Returns NULL on success. */
static const char *net_mode_set(const char *mode)
{
    bool to_ap = false;
    if (!strcmp(mode, "ap")) to_ap = true;
    else if (!strcmp(mode, "sta") || !mode[0]) to_ap = false;
    else return "mode must be 'ap' or 'sta'";
    if (to_ap) {
        if (nvs_set_str(s_nvs, NET_MODE_KEY, "ap") != ESP_OK) return "mode not stored (nvs)";
    } else {
        nvs_erase_key(s_nvs, NET_MODE_KEY);   /* absent is the station default */
    }
    if (nvs_commit(s_nvs) != ESP_OK) ESP_LOGW(TAG, "mode: commit failed (kept until reboot)");
    snprintf(s_mode, sizeof(s_mode), "%s", to_ap ? "ap" : "");
    return NULL;
}

/* ---- the zone table, as the on-dial menu sees it (see orb_net.h) -------------
 *
 * Thin accessors over the one table this file already serves, each reaching the
 * model through the same helpers the HTTP handlers use. Nothing here validates
 * twice: orb_net_tz_select() hands the row's own key to net_tz_set(), which is
 * the same call POST /api/v1/config makes after its body parse. */
int orb_net_tz_count(void)
{
    return NET_TZ_N;
}

/* The broker's D17 door (main/app/orb_svc.c): the OWNER's offset at any
 * instant, minutes east of UTC, DST-aware — the same measured localtime-vs-
 * gmtime diff the tz echo on /api/v1/status shows. Reports the zone, never
 * sets it: net_tz_set() stays the single write door (one behaviour, three
 * doors), and no app ever receives a struct tm or a source name. Sits with
 * the accessors, not with its computation, so the tz host suite's lifted
 * block stays the pure-C slice it was. */
esp_err_t orb_net_tz_offset(time_t when, int32_t *offset_min)
{
    if (!offset_min) return ESP_ERR_INVALID_ARG;
    *offset_min = (int32_t)tz_offset_min(when);
    return ESP_OK;
}

const char *orb_net_tz_key(int i)
{
    if (i < 0 || i >= NET_TZ_N) return "";
    return s_tz_tbl[i].name;
}

const char *orb_net_tz_label(int i)
{
    if (i < 0 || i >= NET_TZ_N) return "";
    return s_tz_tbl[i].label;
}

const char *orb_net_tz_current(void)
{
    return s_tz[0] ? s_tz : "UTC";
}

int orb_net_tz_current_index(void)
{
    /* Match on the key the dial would have stored, and fall back to the row that
     * resolves to the same place: a zone set from the phone by IANA name, or a raw
     * POSIX rule typed into the page, is still one of these rows in effect, and a
     * picker with nothing highlighted reads as "the dial has no zone". */
    const char *cur = orb_net_tz_current();
    char posix[NET_TZ_MAX + 1];
    tz_resolve(s_tz, posix, sizeof(posix));

    for (int i = 0; i < NET_TZ_N; i++) {
        if (strcmp(s_tz_tbl[i].name, cur) == 0) return i;
    }
    for (int i = 0; i < NET_TZ_N; i++) {
        if (strcmp(s_tz_tbl[i].posix, posix) == 0) return i;
    }
    return -1;
}

esp_err_t orb_net_tz_select(int i)
{
    if (i < 0 || i >= NET_TZ_N) return ESP_ERR_INVALID_ARG;
    /* Through net_tz_set() and nothing else: it validates before it writes, so a
     * row this build no longer accepts cannot land in NVS and un-set the dial on
     * the next boot. */
    const char *why = net_tz_set(s_tz_tbl[i].name);
    if (why) {
        ESP_LOGW(TAG, "menu tz: row %d (\"%s\") refused (%s)", i, s_tz_tbl[i].name, why);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "menu tz: set to \"%s\"", s_tz_tbl[i].name);
    return ESP_OK;
}

const char *orb_net_name(void)
{
    return s_name;
}

int orb_net_name_max(void)
{
    return NET_NAME_MAX;
}

esp_err_t orb_net_name_set(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    /* net_name_set() and nothing else, which is the whole point of the accessor:
     * the dial, the page and the hub cannot end up with three different rules
     * about what a name may contain. The reason string is logged rather than
     * returned because the caller gets the useful half of it in ESP_FAIL — the
     * prose is device-side and about a value the caller already holds. */
    const char *why = net_name_set(name);
    if (why) {
        ESP_LOGW(TAG, "name: refused (%s)", why);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "name: set to \"%s\"", s_name);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * WiFi event handling + the radio task
 * ------------------------------------------------------------------------ */

/* Exponential association back-off: 5 s, 10, 20, 40, 80, then 120 s forever.
 *
 * A wrong passphrase is retried indefinitely — there is no "give up" state for
 * a wall clock, since the network may come back after a power cut — so a fixed
 * 5 s cadence means forever-hammering an AP that has already said no. That is
 * not hypothetical here: the bench router's WPA3/SAE implementation rate-limits
 * eager clients, which has been extending every flash cycle this session by a
 * minute or two. Back-off keeps the first ~30 s eager (where a transient
 * dropout actually lives) and goes quiet after that.
 *
 * The same counter decides when the rescue AP opens, so the ladder IS the clock
 * that says "this network is not coming back by itself". attempt is 1-based.
 * Pure, so the host test can check the ladder without a radio. */
static uint32_t retry_delay_s(unsigned attempt)
{
    if (attempt < 2) return NET_STA_RETRY_S;
    uint32_t d = NET_STA_RETRY_S;
    for (unsigned i = 1; i < attempt; i++) {
        if (d >= NET_STA_RETRY_MAX_S) break;
        d *= 2U;
    }
    return d > NET_STA_RETRY_MAX_S ? NET_STA_RETRY_MAX_S : d;
}

/* Re-evaluation period for an AP that is open because there is no lease. The
 * AP is never closed merely for being old — it closes when the station gets a
 * lease (see NET_CMD_AP_EXPIRED), so this only re-arms the check. */
#define NET_AP_RECHECK_S 300

/* arg carries the AP hold seconds; ssid/pass carry a join's credentials. They
 * travel inside the message rather than in a shared static so the queue is the
 * only lock needed: two browsers submitting at once cannot interleave one
 * join's ssid with the other's password. ~104 B x 8 slots of internal RAM. */
typedef struct {
    int  cmd;
    int  arg;
    char ssid[NET_SSID_MAX + 1];
    char pass[NET_PASS_MAX + 1];
} net_msg_t;

static void net_post(int cmd, int arg)
{
    if (!s_net_q) return;               /* before bring-up: nothing to notify */
    const net_msg_t m = { .cmd = cmd, .arg = arg };
    if (xQueueSend(s_net_q, &m, 0) != pdTRUE)
        ESP_LOGW(TAG, "net queue full; dropped cmd %d", cmd);
}

/* Join variant of net_post(): carries the credentials so the radio task, and
 * nothing else, is the task that touches the station. Blocks briefly instead of
 * dropping — a join silently discarded from a full queue would have the device
 * telling the browser "joining" while never having tried. Returns false only
 * when the queue does not exist (bring-up OOM), letting the caller fall back. */
static bool net_post_join(const char *ssid, const char *pass)
{
    if (!s_net_q) return false;
    net_msg_t m = { .cmd = NET_CMD_JOIN };
    snprintf(m.ssid, sizeof(m.ssid), "%s", ssid);
    snprintf(m.pass, sizeof(m.pass), "%s", pass ? pass : "");
    return xQueueSend(s_net_q, &m, pdMS_TO_TICKS(200)) == pdTRUE;
}

/* ---------------- pending (unverified) credentials, D3 ----------------
 * The submitted pair waits here, not in NVS, until a lease proves it works.
 * Both doors used to nvs_set_str() the moment the form was accepted, so one
 * mistyped passphrase overwrote a working credential and took the orb off its
 * network with nothing to fall back to (bench 2026-10-13). A lease is the only
 * evidence a network can give that the pair is right, so the write rides it.
 *
 * Written only by the net task inside NET_CMD_JOIN, and read and cleared only by
 * the net task inside NET_CMD_STORE_CREDS — which on_ip() merely POSTS, carrying
 * nothing, so no buffer here is ever touched by two tasks and the queue stays the
 * only ordering needed, exactly as for net_msg_t's own credential fields. ~100 B
 * static, in internal RAM, unconditionally: this is the code that decides what a
 * device boots with, and it must not depend on an optional feature. */
static char s_pend_ssid[NET_SSID_MAX + 1];
static char s_pend_pass[NET_PASS_MAX + 1];
/* Mirrors pend_any() for the one reader that must not touch the buffers: on_ip()
 * runs on the default event loop, and a single bool is the only cross-task read
 * worth having here. Stale-by-one-tick is safe in either direction — it can only
 * skip or duplicate an idempotent write, never invent a credential. */
static volatile bool s_pend_valid;

/* Does this lease authorise a credential write? Both terms are needed. Pending
 * without join intent is a boot-time auto-retry of what flash already holds, and
 * writing that back is a no-op at best. Join intent without a pending pair cannot
 * happen (NET_CMD_JOIN sets both), and must not clear or write anything either. */
static bool lease_verifies_creds(bool pending, bool joining)
{
    return pending && joining;
}

static void pend_set(const char *ssid, const char *pass)
{
    snprintf(s_pend_ssid, sizeof(s_pend_ssid), "%s", ssid ? ssid : "");
    snprintf(s_pend_pass, sizeof(s_pend_pass), "%s", pass ? pass : "");
    s_pend_valid = s_pend_ssid[0] != '\0';
}

static bool pend_any(void) { return s_pend_ssid[0] != '\0'; }

static void pend_clear(void) { s_pend_ssid[0] = '\0'; s_pend_pass[0] = '\0'; s_pend_valid = false; }

/* Empty passphrase means an OPEN network, and the stored key must be removed
 * rather than set to "": a blank pass left in flash reads back as a configured
 * credential, and the orb then retries an enterprise association the AP never
 * offered. Pure and pinned by tools/host_tz_test.py, because both doors used to
 * spell this branch inline and a typo here silently strands a device. */
static bool creds_erase_key(const char *pass)
{
    return pass == NULL || pass[0] == '\0';
}

/* Write the pair a lease just vouched for. */
static void creds_commit(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return;
    const esp_err_t e1 = nvs_set_str(s_nvs, "ssid", ssid);
    const esp_err_t e2 = creds_erase_key(pass) ? nvs_erase_key(s_nvs, "pass")
                                              : nvs_set_str(s_nvs, "pass", pass);
    nvs_commit(s_nvs);
    if (e1 != ESP_OK || (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND))
        ESP_LOGE(TAG, "creds for \"%s\" NOT stored (%s/%s) — this lease will not "
                      "survive a reboot", ssid, esp_err_to_name(e1), esp_err_to_name(e2));
    else
        ESP_LOGW(TAG, "credentials for \"%s\" accepted by the network and now "
                      "stored (the pair the orb will boot with)", ssid);
}

/* Erase the whole application store, then let the caller reboot into the
 * first-boot branch. This is a FACTORY reset: the literal `ALL` in the verb is the
 * scope, so the theme choice, the grant and enable words, every per-app setting, the
 * fault ledger and the idle-dim timeout die with the credentials. `storage` carries
 * all of those as separate namespaces (orb.net, orb.settings, orb.apps, orb.grants,
 * orb.fault, orb.idle, orb.app<slot>), and erasing the partition rather than a list
 * of names is what keeps this honest when a new namespace lands.
 * One thing is deliberately written back after the erase: the default name
 * "The Orb" (see the tail of this function), so a reset board introduces itself
 * instead of arriving silent.
 *
 * The default `nvs` partition goes too, which is the part an `orb.net`-only erase
 * used to miss: esp_wifi runs with CONFIG_ESP_WIFI_NVS_ENABLED=y and never gets
 * WIFI_STORAGE_RAM, so `nvs.net80211` holds the driver's own copy of the SSID and
 * passphrase. A factory reset that leaves a passphrase behind is not one. The PHY
 * calibration in that namespace is collateral and re-derives on the next boot.
 *
 * Both erases force-close every handle open on the partition and leave it
 * de-initialised (nvs_api.cpp:close_handles_and_deinit), so s_nvs here and the
 * handles orb_theme.c and idle_dim.c hold are all dead on return. Nothing
 * re-initialises them: a reopened `storage` would let the next lease or the next dim
 * change write a credential back into the store the operator just erased. A dead
 * handle fails closed — the lookup is by list membership, so it answers
 * ESP_ERR_NVS_INVALID_HANDLE and cannot land in someone else's namespace. The reboot
 * is the only recovery, and the caller must not skip it.
 *
 * The RAM mirrors go in the same breath, so a status read between here and the reset
 * reports the truth. s_provisioning deliberately does NOT flip: the radio task reads
 * it through prov_ap_idle() and would open the provisioning AP in the ~300 ms left
 * before esp_restart(), for a state the reboot re-derives from flash anyway. RTC
 * never holds a credential, so nothing to scrub there either.
 *
 * The limit of "every namespace": NVS is everything an owner can set, and it is all
 * gone, but installed apps resolve their rows from the `apps` partition headers
 * (orb_app_install_resolve_boot), not from NVS. A reset orb therefore still carries
 * its app blobs and still lists them on the dial — with their grants, enables and
 * per-app settings erased, so each one comes back defaulted and unarmed. Clearing
 * those headers is uninstall, one app at a time, and that asymmetry is worth knowing
 * before this verb is advertised as "erases the device". */
static esp_err_t net_factory_reset(void)
{
    /* Before the erase: a join in flight would otherwise commit its pair into the
     * namespace this call just emptied, on the first lease after it. */
    pend_clear();

    /* Both erases are attempted even after one fails. Stopping at the first error
     * would leave the driver's passphrase on a board whose owner data is already
     * gone — the worst of both, and the one shape of failure a caller could only
     * report as "partly wiped", which is what RESET-PART exists to say. */
    esp_err_t e = nvs_flash_erase_partition(NET_NVS_PARTITION);   /* every orb.* namespace */
    if (e != ESP_OK) ESP_LOGE(TAG, "reset: %s NOT erased (%s)", NET_NVS_PARTITION, esp_err_to_name(e));
    const esp_err_t e_def = nvs_flash_erase();                    /* default nvs: the driver's copy */
    if (e_def != ESP_OK) ESP_LOGE(TAG, "reset: default nvs NOT erased (%s)", esp_err_to_name(e_def));

    /* Scrubbed whether or not the erases worked: if `storage` went, the mirrors are
     * already a lie; if nothing went, the reboot below restores them from flash. */
    s_nvs = 0;                          /* closed by the erase; never used again */
    s_joining = false;
    s_prov_recovery_session = false;    /* there is no fallback config to recover from now */
    s_name[0] = '\0';
    s_tz[0] = '\0';
    s_mode[0] = '\0';
    const esp_err_t wiped = (e != ESP_OK) ? e : e_def;

    /* A board that comes back from a full reset is unnamed no longer: it answers to
     * the default word. Storing it in NVS (not just in the RAM mirror) is the point
     * — every surface that reads the name afterwards (status, the settings page, the
     * hub's next poll) says the same thing without a special case, and the word
     * survives until the next owner's NAME write replaces it or clears it to "".
     * Provisioning is gated on credentials, never on the name, so this cannot keep
     * the AP door shut. Only on a clean wipe: after a partial erase the board is
     * reporting RESET-PART and boots whatever survived — seeding one key into that
     * half-state would claim more cleanliness than the ack admitted. */
    if (wiped == ESP_OK &&
        nvs_open_from_partition(NET_NVS_PARTITION, NET_NVS_NAMESPACE,
                                NVS_READWRITE, &s_nvs) == ESP_OK) {
        if (nvs_set_str(s_nvs, NET_NAME_KEY, NET_NAME_DEFAULT) == ESP_OK &&
            nvs_commit(s_nvs) == ESP_OK) {
            snprintf(s_name, sizeof(s_name), "%s", NET_NAME_DEFAULT);
        } else {
            ESP_LOGW(TAG, "reset: default name not stored (board boots unnamed)");
        }
        nvs_close(s_nvs);
        s_nvs = 0;
    }
    return wiped;
}

/* Timer callbacks may only post: the esp_timer task is shared and must not
 * block, and the esp_wifi_* calls do block on the driver's own task. */
static void retry_timer_cb(void *arg) { (void)arg; net_post(NET_CMD_CONNECT, 0); }
static void lease_watch_timer_cb(void *arg) { (void)arg; net_post(NET_CMD_LEASE_WATCH, 0); }
static void ap_timer_cb(void *arg)    { (void)arg; net_post(NET_CMD_AP_EXPIRED, 0); }
static void arm_oneshot(esp_timer_handle_t t, uint32_t secs, const char *what)
{
    if (!t) { ESP_LOGE(TAG, "%s timer does not exist", what); return; }
    /* Stop first: each of these can be re-armed while already pending, and
     * start_once on a running one-shot simply fails — leaving the OLD deadline
     * in force, which for the AP means closing at the wrong moment. */
    esp_timer_stop(t);
    if (esp_timer_start_once(t, (uint64_t)secs * 1000000ULL) != ESP_OK)
        ESP_LOGE(TAG, "%s timer not armed", what);
}


/* The provisioning AP's identity, in one place so the boot path and the runtime
 * rescue/manual paths cannot drift apart. Open on purpose: the 466x466 panel
 * cannot render a scannable QR, so there is no way to hand out a passphrase —
 * which is precisely why every open here is bounded by a timer. */
static void ap_fill_config(wifi_config_t *ap)
{
    memset(ap, 0, sizeof(*ap));
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)ap->ap.ssid, sizeof(ap->ap.ssid), "orb-%02X%02X", mac[4], mac[5]);
    ap->ap.ssid_len       = (uint8_t)strlen((char *)ap->ap.ssid);
    ap->ap.channel        = 1;      /* 2.4 GHz-only radio; ch 1 is legal everywhere */
    ap->ap.authmode       = WIFI_AUTH_OPEN;
    ap->ap.max_connection = 2;
}

/* RADIO TASK ONLY. */
static void ap_open(uint32_t hold_s, const char *why)
{
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) { ESP_LOGE(TAG, "ap netif: out of memory"); return; }
    }

    /* Declare ourselves a captive portal — RFC 8910 DHCP option 114 — so a joined
     * phone prompts instead of sitting on a network it silently grades "no
     * internet". The probes and their 302 (h_captive) and the DNS responder
     * already exist; this removes the dependence on each vendor's probe
     * heuristic, which is what let a phone report "connected" with no prompt
     * while holding a valid lease. Options are writable only with the server
     * stopped; the default AP glue starts it again on WIFI_EVENT_AP_START. */
    {
        static const char portal_uri[] = "http://192.168.4.1/";
        esp_netif_dhcps_stop(s_ap_netif);   /* already-stopped is expected here */
        const esp_err_t oe = esp_netif_dhcps_option(
            s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
            (void *)portal_uri, sizeof(portal_uri) - 1);
        /* Restart it ourselves. The default AP glue starts the server on netif
         * ATTACH only — esp_netif_lwip_defaults.c has no AP_START handler that
         * does it — so stopping it here to write the option would otherwise leave
         * the AP with no DHCP server at all, which is exactly what the first
         * restart-path test measured. Measured before the fix: option 114 logged
         * OK, no `DHCP server started` line, phone got nothing. */
        const esp_err_t se = esp_netif_dhcps_start(s_ap_netif);
        ESP_LOGI(TAG, "ap open (%s): DHCP option 114 -> %s (server restart %s)", why,
                 oe == ESP_OK ? portal_uri : esp_err_to_name(oe), esp_err_to_name(se));
    }
    wifi_config_t ap;
    ap_fill_config(&ap);

    /* Late promotion (set_mode on a running driver) is the ONE thing here that
     * still needs the buffer trim above. Measured 2026-10-13: calling
     * esp_wifi_disconnect() first does not help -- it frees general heap
     * (free=12791 largest=7168 at the probe) while the static RX EBs stay carved
     * since esp_wifi_init -- and set_mode(APSTA) still dies with
     * `alloc eb len=752 type=4 fail` -> Guru in ieee80211_hostap. That 752 B
     * type=4 EB is the beacon itself (the driver logs `Init max length of beacon:
     * 752/752`), so static-rx=6 rather than 10 is what leaves it a slot.
     *
     * What this is NOT any more: a fix for a phone that would not get an address.
     * That fault was CONFIG_LV_MEM_SIZE starving the config server and the AP's
     * DHCP path (see sdkconfig.defaults); restarting the driver here only cost the
     * LAN ~8 s and changed nothing, so it is out. Whether the trim is still needed
     * now that ~40 KB of internal DRAM is back is OPEN -- to find out, delete the
     * four buffer lines from BOTH sdkconfig.defaults and sdkconfig and repeat the
     * POST /api/v1/wifi/ap that first panicked. Until that is run, keep the trim. */
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
        ESP_LOGE(TAG, "ap open (%s): set_mode(APSTA) failed", why);
        return;
    }

    /* Ask the driver what it is already carrying before writing anything.
     * esp_wifi_set_config() on a RUNNING softAP is a restart, not an update, so
     * writing it here unconditionally is what made every open in the earlier
     * builds log AP_START twice and start the DHCP server twice (30261/30267 in
     * the bench log). Beyond the noise, that restart deauths whoever is already
     * attached — and the whole point of this AP is that a phone is sitting on it
     * while its owner fills in the config page. The profile is deterministic
     * (SSID from the MAC, open, channel 1), so once the driver holds it, it
     * holds it: the write is only needed the first time after boot. */
    wifi_config_t chk;
    memset(&chk, 0, sizeof(chk));
    const bool current =
        esp_wifi_get_config(WIFI_IF_AP, &chk) == ESP_OK &&
        chk.ap.ssid_len == ap.ap.ssid_len &&
        chk.ap.channel  == ap.ap.channel &&
        chk.ap.authmode == ap.ap.authmode &&
        strcmp((const char *)chk.ap.ssid, (const char *)ap.ap.ssid) == 0;

    if (!current) {
        /* Deliberately no early return on failure: the AP IS up either way and
         * must still have its expiry armed below, or a refused write would leak
         * the AP open forever — the one failure this whole design forbids. */
        if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
            ESP_LOGE(TAG, "ap open (%s): set_config failed — AP may advertise the "
                          "driver default SSID", why);
        } else {
            /* Read it back rather than trust the write, for the same reason: a
             * device on the wall advertising "espressif" instead of "orb-xxxxxx"
             * is invisible to every check that runs on the device, and only the
             * person holding the phone would notice it. */
            wifi_config_t rb;
            memset(&rb, 0, sizeof(rb));
            if (esp_wifi_get_config(WIFI_IF_AP, &rb) == ESP_OK &&
                strcmp((const char *)rb.ap.ssid, (const char *)ap.ap.ssid) != 0)
                ESP_LOGE(TAG, "ap open (%s): SSID is \"%s\", not \"%s\"",
                         why, (const char *)rb.ap.ssid, (const char *)ap.ap.ssid);
        }
    }

    s_ap_hold_s = hold_s;     /* the promise NET_CMD_AP_HOLD must not break */
    arm_oneshot(s_ap_timer, hold_s, "ap expiry");
    ESP_LOGW(TAG, "SoftAP \"%s\" OPEN (%s) — join it, then open http://192.168.4.1/"
                  " (%u s hold)", (const char *)ap.ap.ssid, why, (unsigned)hold_s);
}

/* RADIO TASK ONLY. */
static void ap_close(const char *why)
{
    if (!s_ap_up) return;                 /* already down; no wasted flap */
    esp_timer_stop(s_ap_timer);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        ESP_LOGE(TAG, "ap close (%s): set_mode(STA) failed — AP stays up", why);
        return;
    }
    ESP_LOGI(TAG, "SoftAP down (%s)", why);
}

/* --------------------------------------------------------------------------
 * Captive-portal DNS  ("Sign in to network")
 *
 * A phone that joins orb-xxxxxx decides what to tell its user by resolving the
 * hostnames its OS probes for exactly that decision (connectivitycheck.gstatic.com
 * on Android, captive.apple.com on iOS). The default softAP hands out NO resolver
 * at all — esp_netif_lwip's dhcpserver comes up with dns_server.addr = 0
 * (dhcpserver.c:158) — so those lookups go nowhere and the OS declares the
 * network dead rather than a portal. h_captive()'s 302 (which is what raises the
 * notification and lands the browser on the config page) never gets its request,
 * because the phone never finds the orb. Two halves fix it, both scoped to the AP
 * so the station's own DNS — including the SNTP hostname lookup on the LAN — is
 * untouched:
 *   1) advertise the AP's own address as the resolver over DHCP option 6, and
 *   2) answer every A query sent there with that same address.
 * The bind is to the AP address on purpose, NOT INADDR_ANY: a responder on
 * 0.0.0.0:53 would also swallow the orb's own outbound resolution while the
 * station is up, silently breaking time sync the moment anyone opened the AP.
 *
 * Lifecycle is welded to the driver's own AP_START/AP_STOP, so the responder can
 * outlive an AP close only if the AP itself does — the same "leaked open AP"
 * failure the rest of this file is engineered against.
 * ------------------------------------------------------------------------ */
#if CONFIG_ORB_NET_CAPTIVE_DNS

#define NET_DNS_PORT 53
#define NET_DNS_BUF  512      /* a DNS packet over UDP is capped at 512 B */

static int           s_dns_fd = -1;    /* responder socket; -1 when down        */
static TaskHandle_t  s_dns_task;       /* responder task; NULL when down        */
static volatile bool s_dns_wanted;     /* keep the task running                 */
static uint32_t      s_dns_ip;         /* AP address, network byte order        */

/* Build a DNS reply that answers every A query with `ip` (already in network
 * byte order) and answers nothing else, echoing the question verbatim so the
 * client pairs the answer with its request. Deliberately pure — no sockets, no
 * globals, no logging — so the byte encoding is unit-testable in
 * tools/host_tz_test.py. This is precisely the code a bench with no WiFi radio
 * cannot otherwise exercise, and a wrong offset here is a phone that never pops
 * "Sign in to network" with no log line that would say so. Returns the reply
 * length written to tx, or 0 when the query is not ours to answer (malformed,
 * or carrying a second question we decline to serve). */
static int dns_build_reply(const uint8_t *rx, int n, uint32_t ip,
                           uint8_t *tx, int cap)
{
    if (n < 17) return 0;                        /* header + root label + qtype+qclass */
    if ((rx[4] | rx[5]) != 1) return 0;           /* QDCOUNT (bytes 4-5) must be 1 */

    const uint16_t qtype = (uint16_t)((rx[n - 4] << 8) | rx[n - 3]);
    const int qlen = n - 12;                      /* the question, echoed verbatim */
    const int alen = (qtype == 1) ? 16 : 0;        /* one A record, or none for AAAA/PTR */
    const int len  = 12 + qlen + alen;
    if (qlen <= 0 || len > cap) return 0;

    memcpy(tx, rx, n);                             /* id + flags + counts + question */
    tx[2] = 0x81; tx[3] = 0x80;                    /* QR|RD, RA|rcode=NOERROR */
    tx[6] = 0;    tx[7] = (uint8_t)(alen ? 1 : 0);        /* ANCOUNT */
    tx[8] = 0; tx[9] = 0; tx[10] = 0; tx[11] = 0;         /* NSCOUNT, ARCOUNT */
    if (alen) {
        uint8_t *a = tx + 12 + qlen;
        a[0] = 0xC0; a[1] = 0x0C;                  /* name -> the QNAME at offset 12 */
        a[2] = 0x00; a[3] = 0x01;                  /* TYPE A */
        a[4] = 0x00; a[5] = 0x01;                  /* CLASS IN */
        a[6] = 0x00; a[7] = 0x00; a[8] = 0x00; a[9] = 0x3C; /* TTL 60 */
        a[10] = 0x00; a[11] = 0x04;                /* RDLENGTH 4 */
        memcpy(a + 12, &ip, 4);                    /* -> the AP itself */
    }
    return len;
}

static void dns_task_loop(void *arg)
{
    (void)arg;
    uint8_t rx[NET_DNS_BUF];
    uint8_t tx[NET_DNS_BUF];
    for (;;) {
        /* select() with a short timeout, not a bare blocking recvfrom(): the
         * AP_STOP path only clears a flag, and this task must notice within a
         * poll window and close its OWN socket. Closing from another task while
         * recvfrom() blocks is not guaranteed to wake lwIP. */
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s_dns_fd, &rf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        const int sel = select(s_dns_fd + 1, &rf, NULL, NULL, &tv);
        if (!s_dns_wanted) break;               /* AP_STOP asked us down */
        if (sel <= 0) continue;                 /* timeout: re-check the flag */

        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const int n = recvfrom(s_dns_fd, rx, sizeof rx, 0,
                               (struct sockaddr *)&from, &fl);
        /* All the packet-encoding decisions live in dns_build_reply() so they
         * are host-tested; here we only shuttle bytes. A 0 reply means the
         * query was not ours to answer and is silently dropped. */
        const int len = dns_build_reply(rx, n, s_dns_ip, tx, (int)sizeof tx);
        if (len > 0)
            sendto(s_dns_fd, tx, (size_t)len, 0,
                   (const struct sockaddr *)&from, fl);
    }
    close(s_dns_fd);
    s_dns_fd = -1;
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* Called from the AP_START handler (default event task). Everything here is
 * quick and non-blocking, which is what that context requires; and every failure
 * is merely "no popup" — the AP and its config page still work exactly as they
 * did before this feature existed. */
static void dns_start(void)
{
    if (s_dns_wanted) return;                    /* already answering */
    if (!s_ap_netif) return;
    esp_netif_ip_info_t ii;
    if (esp_netif_get_ip_info(s_ap_netif, &ii) != ESP_OK || ii.ip.addr == 0) {
        ESP_LOGW(TAG, "captive DNS: no AP address yet — no portal popup");
        return;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { ESP_LOGW(TAG, "captive DNS: socket: %d", errno); return; }
    struct sockaddr_in sa = { 0 };
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(NET_DNS_PORT);
    sa.sin_addr.s_addr = ii.ip.addr;             /* the AP's own address, net order */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        ESP_LOGW(TAG, "captive DNS: bind: %d", errno);
        close(fd);
        return;
    }
    /* Tell clients the resolver exists, or the query never reaches the socket. */
    esp_netif_dns_info_t d = { 0 };
    d.ip.type       = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4 = ii.ip;
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &d);

    s_dns_ip     = ii.ip.addr;
    s_dns_fd     = fd;
    s_dns_wanted = true;
    if (xTaskCreate(dns_task_loop, "orb_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        ESP_LOGW(TAG, "captive DNS: task out of memory — no portal popup");
        s_dns_wanted = false;
        close(fd);
        s_dns_fd = -1;
        return;
    }
    /* One line on success, and it names the address: whether the AP actually
     * advertises a resolver is exactly the kind of thing that is invisible from
     * the device and shows up only as "my phone never showed the sign-in prompt"
     * in the field. Paired with the AP_STOP teardown so a stuck responder is
     * equally visible. .addr is already network order, so the bytes in memory
     * are the octets in dotted order — no lwIP ip4_addrN() macro needed. */
    const uint8_t *o = (const uint8_t *)&ii.ip.addr;
    ESP_LOGI(TAG, "captive DNS answering for %u.%u.%u.%u",
             o[0], o[1], o[2], o[3]);
}

/* AP_STOP just signals; the responder closes its own socket and self-deletes
 * inside its 200 ms select window, so the socket is never closed from two tasks. */
static void dns_stop(void)
{
    if (s_dns_wanted) ESP_LOGI(TAG, "captive DNS stopped (AP down)");
    s_dns_wanted = false;
}
#endif /* CONFIG_ORB_NET_CAPTIVE_DNS */

/* Defined below with the scan endpoints: every esp_wifi_* call belongs on the
 * radio task, so the scan lifecycle is driven from there, not from the httpd
 * handler — see the h_wifi_scan comment for what that costs a phone. */
static void scan_begin(void);
static void scan_collect(void);
static void scan_boot_begin(void);   /* arms SCAN_SCANNING then starts, radio task */
/* The station interface of a PROVISIONING boot is up but unclaimed: the boot
 * branch opened the AP and never asked for an association. In that state a
 * STA_DISCONNECTED event means nothing and a connect() would be pointless.
 * Once NET_CMD_JOIN has run, though, a human has submitted credentials on the
 * AP - the same event is now the only feedback that submission produced, and
 * swallowing it leaves the device broadcasting an AP with nothing behind it and
 * no pending retry (bench, 2026-10-13: reason 202 auth-fail, then silence; the
 * ladder, the failure counter, the rescue and the AP timeout all sit behind the
 * event that got dropped). So the discriminator for the retry ladder is join
 * intent, not boot mode: `if (s_provisioning) break;` conflated "nothing asked
 * yet" with "the thing you asked for just failed". Pure, and pinned by
 * tools/host_tz_test.py. */
static bool prov_ap_idle(bool provisioning, bool joining)
{
    return provisioning && !joining;
}

/* D4, decision one: does THIS provisioning boot own a deadline? The credentials
 * term is the whole hazard. A factory orb has nothing to fall back to, so a
 * sojourn that rebooted it would land back on the same provisioning branch, open
 * the AP, re-arm and reboot again — forever, on a device nobody is watching. Only
 * a boot that has a stored network to go back to may time out, and the two boot
 * paths therefore stay observably different. Pure so host_tz_test.py can pin it. */
static bool sojourn_arms(bool recovery_session, int sojourn_s)
{
    return recovery_session && sojourn_s > 0;
}

/* D4, decision two: the deadline fired. Nothing to fall back to must never be
 * reached from here (sojourn_arms gated the arming), but the answer is still
 * "reboot into the normal path and let the stored credentials try again" and not
 * ap_close(): ap_close() calls set_mode(STA) and leaves s_provisioning set, which
 * tears the radio down to a half state rather than restoring a join attempt.
 * If a lease DID arrive first, s_sta_up is true and the AP closes normally — the
 * session ended by succeeding, which is the only other way out. */
static bool sojourn_reboots(bool sta_up, bool recovery_session)
{
    return recovery_session && !sta_up;
}

/* Third argument is prov_ap_idle(), never the raw provisioning flag. */
static bool connect_attempt_is_moot(bool associated, bool sta_up, bool ap_idle);
static bool lease_watch_should_drop(bool associated, bool sta_up, bool ap_idle);

static void net_task_loop(void *arg)
{
    (void)arg;
    net_msg_t m;
    for (;;) {
        if (xQueueReceive(s_net_q, &m, portMAX_DELAY) != pdTRUE) continue;
        switch (m.cmd) {
        case NET_CMD_CONNECT:
            /* Re-check what the event loop owns: a lease may have landed
             * between the timer firing and this task getting to run. The
             * s_associated arm is the wedge fix: a retry that arrives while the
             * station is associated but still unleased must not connect over
             * the live attempt — it must hand the stall to the lease watchdog
             * instead. retry_pending is cleared BEFORE any break: a consumed
             * retry that left it set would have the next DISCONNECTED swallowed
             * by the second-event guard, which is the same dead device by
             * another door. */
            s_retry_pending = false;
            if (connect_attempt_is_moot(s_associated, s_sta_up, prov_ap_idle(s_provisioning, s_joining))) {
                if (lease_watch_should_drop(s_associated, s_sta_up, prov_ap_idle(s_provisioning, s_joining)))
                    arm_oneshot(s_watch_timer, NET_LEASE_WATCHDOG_S, "lease watchdog");
                break;
            }
            ESP_LOGI(TAG, "association attempt %u", s_assoc_fails + 1);
            const esp_err_t ce = esp_wifi_connect();
            if (ce == ESP_ERR_WIFI_CONN) break;   /* an attempt is already live */
            if (ce != ESP_OK) {
                /* The attempt never happened, so no DISCONNECTED will ever
                 * arrive to arm the next one. Swallowing that (the previous
                 * behaviour, and what stranded the bench orb with no lease, no
                 * AP and nothing pending) turns one rejected call into a dead
                 * device until someone pulls power. Re-arm eagerly, and without
                 * spending a ladder rung: the network did not say no here. */
                ESP_LOGW(TAG, "connect refused (%s); retrying in %u s",
                         esp_err_to_name(ce), (unsigned)NET_STA_RETRY_S);
                s_retry_pending = true;
                arm_oneshot(s_retry_timer, NET_STA_RETRY_S, "retry");
            }
            break;

        case NET_CMD_JOIN:
            /* Credentials are already in NVS — the handler persisted them before
             * posting, so a failure here costs an association, never the config.
             * The radio half belongs on this task: disconnect()/connect() racing
             * a set_mode() from the AP path is what left the station idle with no
             * timer armed, and serialising every radio mutation here is what
             * keeps that from recurring. */
            s_assoc_fails = 0;
            s_retry_pending = false;
            /* Distinguishes "still waiting for someone to tell me a network" from
             * "trying the one the user just picked". Both were reported as
             * "provisioning", which on a phone mid-setup reads as a page that
             * ignored the Connect button (the first live phone test, where the
             * join actually succeeded and the user never saw it). */
            s_joining = true;
            /* Hold the pair here until a lease vouches for it. Set on this task,
             * before the radio sees it, so a lease that arrives while this handler
             * is still running cannot consume a pair that was never recorded. */
            pend_set(m.ssid, m.pass);
            {
                const esp_err_t je = net_apply_sta_config(m.ssid, m.pass);
                if (je != ESP_OK)
                    ESP_LOGE(TAG, "join(%s) on radio task: %s",
                             m.ssid, esp_err_to_name(je));
            }
            /* Watchdog, not the primary path: normally the driver answers with
             * ASSOCIATED or DISCONNECTED and the ladder takes over from there.
             * If it answers with nothing at all — the wedge on the bench — this
             * is the only thing that brings the station back without a reboot.
             * Harmless when the association is merely slow: NET_CMD_CONNECT
             * breaks out on s_sta_up or a live association and hands the stall
             * to the lease watchdog, and a connect() during a live attempt
             * returns ESP_ERR_WIFI_CONN and is ignored. */
            arm_oneshot(s_retry_timer, NET_STA_RETRY_S, "retry");
            break;

        case NET_CMD_STORE_CREDS:
            /* A lease arrived, so the network accepted this pair: it is now the
             * credential the device will boot with. Nothing writes ssid/pass to
             * NVS anywhere else (see the s_pend_ssid block comment) — that is the
             * point of the whole arrangement, and the reason a wrong passphrase
             * submitted on the config page costs a failed join instead of a
             * device that can no longer reach its own network. */
            if (pend_any()) creds_commit(s_pend_ssid, s_pend_pass);
            pend_clear();
            break;

        case NET_CMD_AP_OPEN:
            /* A human asked for this AP, so it gets the hold the page promised. */
            s_ap_manual = true;
            ap_open((uint32_t)m.arg, "requested from the config page");
            break;

        case NET_CMD_AP_RESCUE:
            s_ap_manual = false;   /* a door, not a request: the lease may close it */
            ap_open((uint32_t)m.arg, "credentials not working");
            break;

        case NET_CMD_AP_HOLD:
            /* The lease backstop must never shorten a hold a user was promised.
             * See the s_ap_manual note: this is the line that ate the manual AP
             * at 120 s, twice, while the phone was still in the owner's pocket. */
            if (s_ap_up && s_ap_manual && (uint32_t)m.arg < s_ap_hold_s) {
                ESP_LOGI(TAG, "SoftAP hold kept at %u s (manual request beats the "
                              "%u s lease backstop)",
                         (unsigned)s_ap_hold_s, (unsigned)m.arg);
            } else if (s_ap_up) {
                arm_oneshot(s_ap_timer, (uint32_t)m.arg, "ap expiry");
            }
            break;

        case NET_CMD_AP_EXPIRED:
            /* The invariant that decides this whole feature: the AP closes
             * because the device BECAME reachable, not because a timer ran
             * out. With no lease the AP is the only door a phone can walk up
             * to, so it stays open and the check merely re-arms. */
            if (s_sta_up) ap_close("lease held, hold expired");
            else if (sojourn_reboots(s_sta_up, s_prov_recovery_session)) {
                /* D4: the recovery session ran out of time and nothing was
                 * achieved. Reboot into the normal path so the stored network is
                 * retried, rather than holding an OPEN AP with an unauthenticated
                 * /api/v1/ota behind it indefinitely. Disarm the fault record
                 * first — the same courtesy POST /api/v1/reboot pays: this is a
                 * planned exit the device decided on, and a next boot that billed
                 * an app for it would report a crash that never happened, which is
                 * exactly the false alarm that makes a fleet UI untrustworthy.
                 * Nothing is written on the way out (D3): a failed session's
                 * credentials never reached flash, so the reboot resumes the pair
                 * that was stored before it.
                 *
                 * DELIBERATELY NOT DONE HERE: do not set D2's intent flag on this
                 * path. This reboot's purpose is to retry the stored network; if it
                 * requested provisioning instead, the next boot would force the AP,
                 * expire, reboot, and request again — an infinite loop between two
                 * unusable states, which is the hazard the guard above exists to
                 * prevent. Only a human requests provisioning: D5's knob, or the
                 * hub on someone's say-so. */
                ESP_LOGW(TAG, "provisioning sojourn expired after %d s with no "
                              "lease; rebooting to retry the stored network",
                         CONFIG_ORB_NET_PROV_SOJOURN_S);
                orb_svc_note_clean_reboot();
                esp_restart();
            }
            else if (!s_provisioning) {
                arm_oneshot(s_ap_timer, NET_AP_RECHECK_S, "ap recheck");
                ESP_LOGI(TAG, "SoftAP stays up: no lease yet, checking again in %d s",
                         NET_AP_RECHECK_S);
            }
            break;

        case NET_CMD_SCAN:
            scan_begin();
            break;

        case NET_CMD_SCAN_DONE:
            scan_collect();
            break;

        case NET_CMD_SCAN_BOOT:
            /* First-boot scan for the wired-provisioning door. Posted only from
             * WIFI_EVENT_AP_START while s_provisioning, so the driver is up;
             * scan_boot_begin() is the radio-task owner of the scan state. */
            scan_boot_begin();
            break;

        case NET_CMD_AP_LEASE_POLL:      /* DIAGNOSTIC (item 10) */
            ap_lease_probe();
            break;

        case NET_CMD_LEASE_WATCH:
            /* The association promised a lease and NET_LEASE_WATCHDOG_S passed
             * with none arriving — the state the bench orb sat in for four
             * minutes. Drop it the honest way: a forced disconnect produces a
             * real DISCONNECTED event, self-inflicted by reason, which the
             * already-tested ladder path re-arms without counting a refusal.
             * A disconnect() that itself fails leaves nothing pending, so the
             * watch re-arms rather than handing the device to the silence. */
            if (!lease_watch_should_drop(s_associated, s_sta_up, prov_ap_idle(s_provisioning, s_joining)))
                break;
            ESP_LOGW(TAG, "no lease %u s after associating; dropping the association",
                     (unsigned)NET_LEASE_WATCHDOG_S);
            if (esp_wifi_disconnect() != ESP_OK)
                arm_oneshot(s_watch_timer, NET_LEASE_WATCHDOG_S, "lease watchdog");
            break;

        default:
            break;
        }
    }
}

/* The handful of codes this device can actually meet at a wall: named ones are
 * worth the screen, everything else prints as its number rather than as a guess.
 * WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT is the one that pays for this function — it
 * is what a wrong passphrase looks like, and the only difference between
 * "retype the password" and "bring me the serial cable". */
static const char *reason_name(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:            return "auth-expired";
    case WIFI_REASON_AUTH_LEAVE:             return "auth-leave";
    case WIFI_REASON_ASSOC_LEAVE:            return "assoc-leave";
    case WIFI_REASON_MIC_FAILURE:            return "mic-failure";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "wrong-key";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:     return "4way-ie-differs";
    case WIFI_REASON_AKMP_INVALID:           return "akmp-invalid";
    case WIFI_REASON_802_1X_AUTH_FAILED:     return "1x-auth-failed";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:  return "cipher-rejected";
    case WIFI_REASON_TIMEOUT:                return "timeout";
    case WIFI_REASON_PEER_INITIATED:         return "peer-initiated";
    case WIFI_REASON_AP_INITIATED:           return "ap-initiated";
    case WIFI_REASON_BEACON_TIMEOUT:         return "beacon-timeout";
    case WIFI_REASON_NO_AP_FOUND:            return "no-ap-found";
    case WIFI_REASON_AUTH_FAIL:              return "auth-fail";
    case WIFI_REASON_ASSOC_FAIL:             return "assoc-fail";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "handshake-timeout";
    case WIFI_REASON_CONNECTION_FAIL:        return "connection-fail";
    case WIFI_REASON_AP_TSF_RESET:           return "ap-tsf-reset";
    case WIFI_REASON_ROAMING:                return "roaming";
    default:                                 return NULL;
    }
}

/* The chip's own account of why this boot happened, in one short word. C2 stores
 * every code-blamed reset under the single string "unexpected reset" (orb_svc.c),
 * so the ledger cannot tell a task-watchdog bite from a panic over the LAN. This
 * is the missing class: esp_reset_reason() reports the reset that ENDED the last
 * boot, read on the boot right after a fault it names the crash. The set mirrors
 * orb_reset_blames_code plus the owner/supply family, so a faulting fleet node
 * reads "task-wdt" or "panic" and nothing else says it. */
static const char *rst_name(int rr)
{
    switch (rr) {
    case ESP_RST_UNKNOWN:    return "unknown";
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_EXT:        return "ext";
    case ESP_RST_SW:         return "sw";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int-wdt";
    case ESP_RST_TASK_WDT:   return "task-wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deepsleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr-glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
    default:                 return "other";
    }
}

/* The two codes that mean the station came down because WE took it down:
 * esp_wifi_disconnect() on a join, or a set_mode() that rebuilt the interface.
 * They must never feed the failure counter — otherwise toggling the AP from the
 * config page walks the device toward its own rescue threshold with nobody
 * having said no to anything, and a user pressing a button watches the orb
 * conclude that its network is dead. Kept as a predicate rather than inlined in
 * the event handler so the host test can pin it: from the LAN, an
 * incorrectly-counted leave is indistinguishable from a router that is refusing
 * the orb, and it ends with an AP open on the wall that nobody summoned. */
static bool sta_leave_is_self_inflicted(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_LEAVE || reason == WIFI_REASON_ASSOC_LEAVE;
}

/* The two decisions the association ladder was missing, both born of the
 * bench wedge of 2026-10-12: a retry armed by a self-inflicted leave at boot
 * fired five seconds later INTO a run-but-unleased association — both old
 * guards (s_sta_up, s_provisioning) were still false there, esp_wifi_connect()
 * returned ESP_OK and destroyed the association in flight, and no further
 * event ever arrived to re-arm the ladder. The device sat network-dead until
 * EN was pulsed. Pure, and shaped as predicates like
 * sta_leave_is_self_inflicted() above, so tools/host_tz_test.py can pin the
 * truth tables without a radio: from the LAN a wrong answer here looks
 * exactly like a router that hates the orb. */
static bool connect_attempt_is_moot(bool associated, bool sta_up, bool ap_idle)
{
    return sta_up || associated || ap_idle;
}

static bool lease_watch_should_drop(bool associated, bool sta_up, bool ap_idle)
{
    return associated && !sta_up && !ap_idle;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case WIFI_EVENT_STA_START:
        /* During provisioning there is no station profile to associate with yet,
         * so do not fire a connect() that can only fail — the user has not
         * submitted credentials. The join endpoint starts it instead. */
        if (!s_provisioning) esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_CONNECTED:
        /* The driver's word that the association is real; the lease is still on
         * its way. Two obligations, both from the bench wedge: a retry timer
         * armed by an EARLIER failure must not survive into this association —
         * cancel on CONNECTED, not on the lease, because the unleased window
         * between the two is exactly where the wedge lived — and a lease that
         * never arrives needs an owner, which is the watchdog. */
        s_associated = true;
        if (s_retry_pending) {
            s_retry_pending = false;
            esp_timer_stop(s_retry_timer);
        }
        arm_oneshot(s_watch_timer, NET_LEASE_WATCHDOG_S, "lease watchdog");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ed = data;
        const uint8_t reason = ed ? ed->reason : 0;
        s_sta_up = false;
        s_associated = false;
        s_rssi   = -999;
        if (reason) s_last_reason = reason;
#if CONFIG_ORB_NET_MDNS
        /* Withdraw the STA advertisement on the disconnect, not on the lease
         * expiry: an orb that has lost the association is unreachable right now,
         * and a hub holding it in cache for another 120 s is a hub dialing a
         * dead address. The re-announce rides the next IP_EVENT_STA_GOT_IP, and
         * a blip that never reaches a lease withdrawal costs one goodbye. */
        orb_mdns_set_sta(false, 0);
#endif
        if (prov_ap_idle(s_provisioning, s_joining)) break;   /* AP up, nothing submitted: no ladder */
        if (s_retry_pending) break;        /* driver's second event for one failure */
        /* See sta_leave_is_self_inflicted(): a leave we caused is not a refusal,
         * so it costs the ladder nothing. The retry is armed regardless, because
         * an unowned disconnect must never end with the device sitting down
         * forever with nothing pending. */
        const bool refused = !sta_leave_is_self_inflicted(reason);
        const unsigned attempt = refused ? ++s_assoc_fails : s_assoc_fails;
        const uint32_t delay   = retry_delay_s(attempt ? attempt : 1);
        char fb[16];
        const char *nm = reason_name(reason);
        if (!nm) { snprintf(fb, sizeof(fb), "code-%u", reason); nm = fb; }
        if (refused) ESP_LOGW(TAG, "disassociated: %s (reason %u, attempt %u); "
                                   "retrying in %u s", nm, reason, attempt,
                             (unsigned)delay);
        else         ESP_LOGI(TAG, "disassociated: %s (reason %u) — self-initiated, "
                                   "not counted; retrying in %u s", nm, reason,
                              (unsigned)delay);
        s_retry_pending = true;
        arm_oneshot(s_retry_timer, delay, "retry");
        /* Credentials exist but are not working. Past the threshold, open the
         * provisioning AP ALONGSIDE the retries: under APSTA the station can
         * still associate (see the boot-path comment on exactly this), so a
         * network that comes back on its own is still taken automatically while
         * the door stays open for a phone. Without this the only exit from a
         * changed router passphrase is serial + an NVS erase. */
        if (refused && !s_ap_up && CONFIG_ORB_NET_AP_RESCUE_FAILS > 0 &&
            attempt >= (unsigned)CONFIG_ORB_NET_AP_RESCUE_FAILS)
            /* NET_CMD_AP_RESCUE, not NET_CMD_AP_OPEN: the two open the radio the
             * same way but the log line differs, and "requested from the config
             * page" next to an AP nobody asked for is exactly the misleading
             * provenance a support call gets stuck on. */
            net_post(NET_CMD_AP_RESCUE, NET_AP_RECHECK_S);
        break;
    }

    case WIFI_EVENT_AP_START:
        s_ap_up = true;
        ESP_LOGI(TAG, "SoftAP up — join it and open the URL in the log above");
#if CONFIG_ORB_NET_CAPTIVE_DNS
        dns_start();                 /* advertise + answer, so phones show "Sign in" */
#endif
#if CONFIG_ORB_NET_MDNS
        /* The AP half of the advertisement, and the reason item 4 is not just a
         * LAN feature: after a factory reset (item 10) the orb has no lease at
         * all, and the SoftAP is the only place it can be found. A wired hub
         * still cannot route to 192.168.4.1 — the same limit that makes the boot
         * scan log PROVSCAN lines — so this serves the phone that joined the AP,
         * which is exactly who is standing there in that scenario. */
        if (s_ap_netif) {
            esp_netif_ip_info_t ai;
            if (esp_netif_get_ip_info(s_ap_netif, &ai) == ESP_OK && ai.ip.addr)
                orb_mdns_set_ap(true, ai.ip.addr);
            else
                ESP_LOGW(TAG, "mdns: no SoftAP address yet — not advertised on the AP");
        }
#endif
        /* First-boot scan for the wired-provisioning door: a wired-only hub has
         * no route to 192.168.4.1, so it reads the network list off the serial
         * log instead of the page. scan_collect() logs each result as a
         * PROVSCAN line while s_provisioning. Posted, not called: the event task
         * must not touch the driver. */
        if (s_provisioning) net_post(NET_CMD_SCAN_BOOT, 0);
        break;

    case WIFI_EVENT_AP_STOP:
        /* The driver's word that the AP is really gone, which is what makes
         * ap_close()'s early return honest. */
        s_ap_up = false;
        s_ap_manual = false;    /* the promise expired with the AP, not before it */
        s_ap_hold_s = 0;
#if CONFIG_ORB_NET_CAPTIVE_DNS
        dns_stop();                  /* the AP is down; take its resolver with it */
#endif
#if CONFIG_ORB_NET_MDNS
        /* Same reasoning as the resolver above, and the reason this rides
         * AP_STOP rather than ap_close()'s own early return: the driver is the
         * only one that knows the AP is genuinely down, and a goodbye for an AP
         * that is still up tells a hub to stop showing an orb it can still
         * reach. If the STA still has its lease the responder keeps running for
         * that interface and sends no goodbye at all. */
        orb_mdns_set_ap(false, 0);
#endif
        break;

    case WIFI_EVENT_AP_STACONNECTED: {
        /* Provisioning evidence, and the half a lease can never stand in for:
         * this says a client associated with the SoftAP. Whether it then got an
         * address is on_ap_lease below. Together the two lines separate a phone
         * that never reached the orb from one that reached it and was refused an
         * address — a distinction that could not be made from the device before,
         * because neither event was observed. */
        const wifi_event_ap_staconnected_t *ac = data;
        if (ac) {
            ESP_LOGI(TAG, "SoftAP: %02x%02x%02x%02x%02x%02x joined (aid %u)",
                     ac->mac[0], ac->mac[1], ac->mac[2],
                     ac->mac[3], ac->mac[4], ac->mac[5], (unsigned)ac->aid);
            /* DIAGNOSTIC (item 10): remember who joined, zero the frame counter,
             * and let the net task watch for their address. Posted, never run
             * inline: the probe blocks, and an event callback must not. */
            memcpy(s_ap_last_mac, ac->mac, 6);
            net_post(NET_CMD_AP_LEASE_POLL, 0);
        }
        break;
    }

    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *ad = data;
        if (ad)
            ESP_LOGI(TAG, "SoftAP: %02x%02x%02x%02x%02x%02x left (aid %u, reason %u)",
                     ad->mac[0], ad->mac[1], ad->mac[2],
                     ad->mac[3], ad->mac[4], ad->mac[5],
                     (unsigned)ad->aid, (unsigned)ad->reason);
        break;
    }

    case WIFI_EVENT_SCAN_DONE:
        /* Post only — the event task must not call into the driver, same rule
         * the timer callbacks live by. Unconditional: even after the page gave
         * up waiting, the driver's record list still has to be freed —
         * scan_collect() handles the not-SCANNING case. */
        net_post(NET_CMD_SCAN_DONE, 0);
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * Time: SNTP
 *
 * Configured at bring-up but NOT started there: lwIP's SNTP client sends its
 * first request from sntp_init(), and doing that before the station has a lease
 * means the DNS lookup for the server hostname runs against the empty resolver
 * set a pre-DHCP interface has. The exchange then fails, the client retreats to
 * the 15 s -> 150 s retry ladder, and the first real time the orb gets can be
 * minutes after the network it needed was already up. Starting on
 * IP_EVENT_STA_GOT_IP (on_ip below) costs nothing on that path and keeps the
 * hourly CONFIG_LWIP_SNTP_UPDATE_DELAY re-sync running from the first answer.
 *
 * Two servers, not one: if pool.ntp.org's hostname does not resolve (or its
 * anycast instance is down) the client fails over to time.google.com instead of
 * losing the clock until the next association. That failover is what
 * CONFIG_LWIP_SNTP_MAX_SERVERS=3 in sdkconfig.defaults pays for — the IDF
 * default of 1 makes the second entry impossible to register at all.
 * ------------------------------------------------------------------------ */
#define NET_SNTP_SERVERS 2

/* lwIP's esp_netif_sntp_init() checks the server count at RUNTIME and its
 * failure path returns ESP_OK anyway (esp_netif_sntp.c:48 -> the ESP_GOTO_ON_FALSE
 * jumps to `err:` without ever assigning the error to `ret`), so a stale
 * sdkconfig that still has CONFIG_LWIP_SNTP_MAX_SERVERS=1 produces a device that
 * quietly never syncs and reports success. Fail the build instead, where it is
 * visible: the pin that matters is the one in sdkconfig.defaults. */
_Static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS >= NET_SNTP_SERVERS,
               "CONFIG_LWIP_SNTP_MAX_SERVERS must be >= the orb_net server list");

/* Set once SNTP has stepped the system clock during this boot; deliberately
 * never cleared. Losing the AP does not make the epoch wrong — it only stops it
 * being corrected — and both readers (the dial's source switch and
 * /api/v1/status) treat it as "time() is trustworthy", not "association up". */
static volatile bool s_time_synced;

/* Runs on the lwIP tcpip task, not the caller's — so it must do nothing but
 * record the fact. The dial reads s_time_synced from the LVGL task; a bool
 * write is atomic on the S3 and the value only ever goes false -> true. */
static void sntp_sync_cb(struct timeval *tv)
{
    s_time_synced = true;
    /* Re-run tzset() now that the real instant is in the clock. This newlib
     * decides DST from the time it sees when tzset() runs, so a TZ loaded at
     * bring-up while the clock still read 1970 (the post-power-cut path, before
     * the RTC epoch survives anything) computed its DST state for Jan-1970 — and
     * later localtime_r() calls do NOT recompute it. One tzset() per sync, i.e.
     * per boot and then hourly, is what makes the dial land on the right side of
     * a DST boundary. Only tzset here, never setenv: the TZ string itself changes
     * only from the httpd task, and setenv() reallocating the environment is not
     * something to be doing on someone else's task for no reason. */
    tzset();
    /* Second granularity out of the callback is enough to log and to prove the
     * exchange landed: the dial's sub-second fraction comes from esp_timer in
     * compose_now() regardless of what the callback saw. */
    ESP_LOGI(TAG, "sntp: time synced, epoch %lld s",
             (long long)(tv ? tv->tv_sec : 0));
}

/* DIAGNOSTIC (item 10) — see the note by s_ap_last_mac for why the lease table
 * and not an IP event is the witness here.
 * RADIO TASK ONLY: blocks ~14 s, so it must never run in an event callback. */
static void ap_lease_probe(void)
{
    if (!s_ap_netif) return;
    esp_netif_pair_mac_ip_t pair = { 0 };
    memcpy(pair.mac, s_ap_last_mac, 6);

    for (int i = 0; i < 14; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        pair.ip.addr = 0;
        if (esp_netif_dhcps_get_clients_by_mac(s_ap_netif, 1, &pair) == ESP_OK &&
            pair.ip.addr != 0) {
            ESP_LOGW(TAG, "SoftAP probe: server ASSIGNED " IPSTR " to "
                          "%02x:%02x:%02x:%02x:%02x:%02x after %d s",
                     IP2STR(&pair.ip), pair.mac[0], pair.mac[1], pair.mac[2],
                     pair.mac[3], pair.mac[4], pair.mac[5], i + 1);
            return;
        }
    }
    ESP_LOGE(TAG, "SoftAP probe: the DHCP server granted NO address to "
                  "%02x:%02x:%02x:%02x:%02x:%02x in 14 s",
             s_ap_last_mac[0], s_ap_last_mac[1], s_ap_last_mac[2],
             s_ap_last_mac[3], s_ap_last_mac[4], s_ap_last_mac[5]);
}

/* The SoftAP's DHCP server reporting a lease. esp_netif posts this from lwIP's
 * dhcps new-lease callback (esp_netif_lwip.c:1133 -> :1064) the moment an
 * address is committed to a joined client, so it is the device's own answer to
 * the one item 10 question the bench could not settle: does a phone that joins
 * the provisioning AP actually get an address? Logged, not acted on: the only
 * interface here running a server is the AP, and a lease handed to someone else
 * must not be mistaken for the station's own — which is why this is not folded
 * into on_ip(), whose whole job is to claim a lease for the STA. */
static void on_ap_lease(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_AP_STAIPASSIGNED) return;

    const ip_event_ap_staipassigned_t *ev = data;
    if (!ev) return;
    ESP_LOGI(TAG, "SoftAP: leased " IPSTR " to %02x%02x%02x%02x%02x%02x",
             IP2STR(&ev->ip),
             ev->mac[0], ev->mac[1], ev->mac[2],
             ev->mac[3], ev->mac[4], ev->mac[5]);
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_sta_up       = true;
    /* The association delivered the lease: the watchdog that guards exactly
     * this hand-off has nothing left to guard. */
    esp_timer_stop(s_watch_timer);
    s_provisioning = false;
#if CONFIG_ORB_NET_MDNS
    /* The lease is the address a browser should be sent to, so the advertisement
     * starts here and nowhere earlier: before the lease the orb has no reachable
     * name on the LAN, and announcing one would hand out 0.0.0.0. Posted from an
     * event handler, which is allowed: orb_mdns_set_sta only sets flags and, on
     * the first call, creates the socket and its own task. */
    orb_mdns_set_provisioning(false);
    orb_mdns_set_sta(true, ev->ip_info.ip.addr);
#endif
    /* The lease is the word that the join the user pressed Connect for actually
     * landed; until then /api/v1/status keeps saying "joining" rather than falling
     * back to "provisioning", which is what made the first phone test look ignored.
     *
     * Captured BEFORE the clear below, because it is also what authorises the
     * credential write: a lease that answers a submission means the submitted pair
     * worked, and only that pair may be committed. A lease from the boot-time
     * auto-retry of what is already in flash must not touch NVS at all. */
    const bool answers_a_join = s_joining;
    const bool store_creds    = lease_verifies_creds(s_pend_valid, answers_a_join);
    s_joining      = false;
    s_assoc_fails  = 0;      /* back-off is per-outage, not per-device-life */
    ESP_LOGI(TAG, "associated: config page at http://%s/", s_ip);
    stage_probe("sta-connected");

    /* Deferred credential write, D3. Posted rather than done here: the pending
     * pair belongs to the radio task, and this handler runs on the default event
     * loop. If the queue is full the pair simply survives to the next lease —
     * losing the write is safe, losing the old credential was not. */
    if (store_creds) net_post(NET_CMD_STORE_CREDS, 0);

    /* A lease is what closes the provisioning AP — see NET_CMD_AP_EXPIRED for
     * why the close is conditioned on this rather than on the timer alone.
     * Posted rather than called directly: this runs on the default event loop,
     * where esp_wifi_set_mode() is off-limits. */
    net_post(NET_CMD_AP_HOLD, CONFIG_ORB_NET_AP_HOLD_S);

    /* Lease + DHCP-supplied resolver exist now, so the NTP exchange can be
     * resolved and answered. esp_netif_sntp_start() is stop-then-init
     * (esp_netif_sntp.c:170-180), so this is idempotent on every reconnect: it
     * re-arms the ladder rather than stacking clients, and a network that drops
     * for a minute gets a fresh sync attempt on the way back instead of waiting
     * out the hourly timer. */
    const esp_err_t se = esp_netif_sntp_start();
    if (se != ESP_OK)
        ESP_LOGW(TAG, "sntp_start: %s (clock keeps free-running)", esp_err_to_name(se));
}

/* --------------------------------------------------------------------------
 * HTTP handlers
 *
 * Routes are registered in httpd's own table; no dispatch function, and the
 * URI-match table is sized at compile time from HTTPD_URI_CONTAINS-style
 * defaults, so the only runtime cost here is the handlers themselves.
 * ------------------------------------------------------------------------ */
static esp_err_t h_status(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    /* 1536, sized by readings taken on orb-B on 2026-10-12, not by estimates.
     * Ten identical requests at e85645e measured 1117 B, with three ledger rows
     * published and no app status line printed. A board that had just booted read
     * 1111 B, and 1115 B and 1116 B appeared between those. The spread comes from
     * the fields themselves, not from noise: wifi_reason_name is a string out of
     * reason_name(), and idle.dimmed prints true or false. The longest name that
     * table offers is handshake-timeout at 17 B, 6 B beyond the assoc-leave seen
     * here, and counter digit widths add a byte apiece. A realistic ceiling sits
     * near 1123 B, which leaves about 410 B in this frame. Quote a range, never
     * one number: the width tracks uptime and recent drops. The frame grew from
     * 512 through 640 and 960 as fields joined it, and every addition was
     * re-measured with `curl -s ORB/api/v1/status | wc -c`, never added up here.
     *
     * The "name" field is the one whose width a measurement cannot pin, because
     * the operator chooses it: the field costs 10 B unnamed and 42 B at the
     * 32-byte ceiling orbfleet and NET_NAME_MAX both enforce. Measured on orb-B
     * on 2026-10-14 at the ceiling, eight identical requests read 1159 B and five
     * after the clear read 1127 B, so the field costs its full 32 B and nothing
     * else moved. Worst case is therefore 1159 B in a 1536 B buffer — 377 B of
     * headroom, and 36 B below the ~1165 B this comment estimated before the bench
     * run. Re-measure the same way when a field joins the frame: set a 32-character
     * name with `curl -X POST -d '{"name":"nnn…"}' ORB/api/v1/name`, then count
     * `curl -s ORB/api/v1/status | wc -c`, and quote a range rather than one number
     * because the width still tracks uptime and recent drops.
     *
     * The fw field grew when the image began carrying a declared version rather than
     * a bare git describe: it now reads v1.0.0+e1a2b3c-dirty at 20 B, and 25 B with
     * an -rc.N in front of the metadata. The fw field fits the frame; the reset-pair
     * paragraph below is the current bound on total width, and the re-measure above
     * is what settles it against hardware.
     *
     * The ledger binds before this frame does. D35 moved the ledger array to a
     * 2048 B PSRAM block — the factory pair names its caps and its slugs longer
     * than the compiled trio did, and six shipped rows measured past the old
     * 1024 B internal static (observed: the seventh row fell off on orb-B
     * 2026-10-21). The frame below still rejects rather than clips: the ledger
     * answers an error array and the body answers a 500, because a truncated
     * body parses as nothing and the page shows a blank card. The merged body
     * measured 1118 B on
     * orb-A on 2026-10-12 over the LAN with those rows' counters at zero, and the
     * fleet build before the ledger measured 668-676 B on orb-B at the 960 frame,
     * the -dirty stamp adding the extra 5-7 B.
     *
     * The mac field costs a fixed 21 B, counted off the wire. Its value is
     * verified against hardware: orb-B reports 44bd8d782618, equal to `esptool
     * read_mac` on that board, and it matches the eFuse read through the native Go
     * flasher and the sysfs USB descriptor for the same port. ESP_MAC_WIFI_SOFTAP
     * would have ended 2619, one off, and every hub join key is BASE.
     * mac_addr.c:366-387 derives the SoftAP address as memcpy(base) then
     * mac[5] += 1, live while CONFIG_ESP_MAC_ADDR_UNIVERSE_WIFI_AP=y.
     *
     * The reset pair and per-app reason joined the frame on 2026-10-21 for the
     * menu-fault investigation, and the frame grew 1536 -> 2048 to hold them.
     * esp_reset_reason() is fixed for a boot's life, so "rst"/"rst_name" cost a
     * flat ~34 B. The per-app "reason" costs ~28 B but only on a faulted row;
     * "unexpected reset" is the widest string orb_svc ever stores, so 11 rows
     * faulted together add ~300 B over the 1159 B the same ledger measured bare
     * (orb-A, 2026-10-21). Worst case lands near 1510 B in a 2048 B frame. Both
     * the ledgers reject rather than clip, so re-measure with `curl -s
     * ORB/api/v1/status | wc -c` before trusting a fuller ledger over this cap.
     *
     * Static, not stack, per the s_scan_json rule above: 1536 B on a 4 KB httpd
     * task was already most of the stack. D19's second name list per row pushed it
     * past the point where a glance at the size is a gamble. Handlers run serially
     * on the single httpd task, so one instance is safe, and .bss is the honest
     * place for a buffer this size. Nothing on the host checks this one. The
     * /api/v1/config buffer has host_tz_test.py recomputing its worst case from the
     * zone table, and this has no equivalent. */
    static char json[2048];

    /* Which A/B slot is running, and whether the bootloader still considers it
     * unproven. This is what makes an OTA observable: /api/v1/ota stages into the
     * other slot and /api/v1/reboot switches to it, so after a reboot this is the
     * only way to tell the two apart from the LAN — and with rollback compiled in,
     * "pending" here means the self-revert safety net is still armed. */
    const esp_partition_t *slot = esp_ota_get_running_partition();
    esp_ota_img_states_t img = ESP_OTA_IMG_UNDEFINED;
    if (slot) esp_ota_get_state_partition(slot, &img);
    const char *img_str =
        img == ESP_OTA_IMG_PENDING_VERIFY ? "pending" :
        img == ESP_OTA_IMG_VALID          ? "valid"   :
        img == ESP_OTA_IMG_INVALID        ? "invalid" :
        img == ESP_OTA_IMG_ABORTED        ? "aborted" :
        img == ESP_OTA_IMG_NEW            ? "new"     : "undefined";

    /* Read the signal straight from the driver rather than trusting s_rssi:
     * that is only refreshed by event handlers, so it went stale and reported
     * -999 (the disconnected sentinel, which is what on_wifi leaves it at) even
     * while the station was happily associated — the first live check of this
     * endpoint showed exactly that nonsense next to state:"connected". Asking
     * the driver per request is cheap and is always the current value, which is
     * what makes the number usable for placement decisions. */
    int rssi = s_rssi;
    if (s_sta_up) {
        wifi_ap_record_t cur;
        if (esp_wifi_sta_get_ap_info(&cur) == ESP_OK) rssi = (int)cur.rssi;
    }

    /* One reading of the clock, used for both the epoch echo and the zone
     * offset — they must describe the same instant or the page shows an offset
     * for a second the dial never displayed. tz_offset is measured, not stored:
     * it is what makes a valid-looking-but-degenerate POSIX rule visible (the
     * device cannot know "XX3" was meant to be a zone, but it can say "+0:00").
     *
     * Pre-sync (epoch < CLOCK_EPOCH, i.e. the first seconds after a power cut)
     * this offset is measured against the 1970 clock, so it is the offset that
     * instant would have had — which can be the wrong side of a DST boundary.
     * "synced":false tells the page not to trust it yet; the sync callback's
     * tzset() fixes the dial itself as soon as the real epoch lands. */
    const time_t now = time(NULL);

    /* The 802.11 reason in words, not as a number: "attempt 1, reason 204" on a
     * phone during setup is unreadable (a user can be expected to act on "wrong
     * password" and on nothing else), and 204 in particular is the code a wrong
     * passphrase returns. The number stays in the reply for tooling. */
    char rbuf[16];
    const char *rname = reason_name(s_last_reason);
    if (!rname) {
        snprintf(rbuf, sizeof(rbuf), "code-%u", (unsigned)s_last_reason);
        rname = rbuf;
    }

    /* The reset that ended the LAST boot, read once: esp_reset_reason() is fixed
     * for a boot's life, so every poll of a booted node reports the same class.
     * This is the field the C2 ledger cannot give — it stores every code reset as
     * "unexpected reset", so a task-watchdog bite and a panic look identical from
     * the hub. Here they read task-wdt and panic. See rst_name(). */
    const int rst = (int)esp_reset_reason();
    const char *rst_nm = rst_name(rst);

#if CONFIG_ORB_IDLE_DIM_ENABLE
    /* Read once, before the format string, so the two booleans and the countdown
     * describe the same instant rather than three races across the task that owns
     * them. on/timeout_s come from the runtime settings the DISPLAY menu writes:
     * reporting the Kconfig values here instead would make a switched-off dial
     * claim it was armed, which is the same lie the block exists to prevent --
     * a build's and an owner's actual values must be readable from the LAN, not
     * inferred from a boot log the next flash destroys. */
    const bool     idle_on        = idle_dim_enabled();
    const char    *idle_dark      = idle_dim_dimmed() ? "true" : "false";
    const unsigned idle_since_s   = (unsigned)(idle_dim_ms_since_touch() / 1000u);
    /* The EFFECTIVE timeout, via the getter: the DISPLAY menu can change it at
     * runtime and the number here must be what the dial is actually counting
     * against, not what Kconfig once said. */
    const unsigned idle_timeout_s = (unsigned)idle_dim_timeout_s();
    const unsigned idle_dim_pct   = (unsigned)CONFIG_ORB_IDLE_DIM_PCT;
#else
    /* ORB_IDLE_DIM_TIMEOUT_S and _PCT do not exist in this build — they are
     * depends-on children of the option that is off — so the only honest values
     * here are "not on" and zeroes. */
    const bool     idle_on        = false;
    const char    *idle_dark      = "false";
    const unsigned idle_since_s   = 0;
    const unsigned idle_timeout_s = 0;
    const unsigned idle_dim_pct   = 0;
#endif
    const unsigned idle_lit_pct   = (unsigned)idle_dim_user_level();
    const unsigned idle_wakes     = (unsigned)idle_dim_motion_wakes();
    const int      idle_worst_mg  = idle_dim_worst_mg();
    const int      idle_worst_dk  = idle_dim_worst_dark_mg();

    /* P3 pass line (plan §8): the broker's ledger, published. Per app: the
     * owner's grants, what it has asked for without holding, its call and
     * denial counts, its D16 beat in force, the bytes of its config quota,
     * and the C2 fault count with the failed flag. The denial count is what
     * makes "no code path lets an app call below its grant" checkable from
     * the LAN — a broker nobody can audit is a broker nobody has to trust.
     * Rows, not walkable entries: the ledger is exactly about the failed
     * and disabled apps the carousel hides. */
    /* D35 grew the shipped ledger past the old 1024 B static's six-row reach
     * (the factory pair names caps and slugs longer than the compiled trio
     * did), and this file's own memory note rules out growing an internal
     * static: one PSRAM block, sized for the whole span, allocated once. A
     * failed allocation answers the honest error array, never a truncated
     * body — the same reject-not-clip rule the fixed static carried. */
    static char *apps_json;                  /* PSRAM; httpd handlers run serially */
    enum { APPS_JSON_BYTES = 2048 };
    static bool apps_json_bad;
    if (!apps_json && !apps_json_bad) {
        apps_json = heap_caps_malloc(APPS_JSON_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!apps_json) apps_json_bad = true;
    }
    {
        int off = apps_json ? snprintf(apps_json, APPS_JSON_BYTES, "[") : -1;
        if (off < 0 || (size_t)off >= APPS_JSON_BYTES) off = -1;
        for (size_t i = 0; off >= 0 && i < orb_app_rows(); i++) {
            const orb_app_t *a = orb_app_row(i);
            if (!a) continue;           /* P4a: the span is positional — a free card
                                         * slot answers NULL and the next slot is a
                                         * different app, not the end of the list */
            char grants[64];
            int goff = 0;
            grants[0] = '\0';
            char declared[64];
            int doff = 0;
            declared[0] = '\0';
            static const orb_cap_t OC[4] = { CAP_POWER, CAP_NET,
                                             CAP_SD_READ, CAP_THEMES };
            const uint32_t gm = orb_svc_grants(a);
            const uint32_t dm = orb_svc_declared(a);
            for (unsigned c = 0; c < 4; c++) {
                if (!(gm & (1u << OC[c]))) continue;
                const int gw = snprintf(grants + goff, sizeof grants - (size_t)goff,
                                        "%s\"%s\"", goff ? "," : "",
                                        orb_svc_cap_name(OC[c]));
                if (gw < 0 || (size_t)gw >= sizeof grants - (size_t)goff) break;
                goff += gw;
            }
            /* The second list is what D19 makes the ledger for: grants alone
             * cannot tell "the app never asked" from "the owner revoked it",
             * and the difference is the whole answer to "what can this thing
             * do on my network". declared is the install-time consent. */
            for (unsigned c = 0; c < 4; c++) {
                if (!(dm & (1u << OC[c]))) continue;
                const int dw = snprintf(declared + doff, sizeof declared - (size_t)doff,
                                        "%s\"%s\"", doff ? "," : "",
                                        orb_svc_cap_name(OC[c]));
                if (dw < 0 || (size_t)dw >= sizeof declared - (size_t)doff) break;
                doff += dw;
            }
            const int w = snprintf(apps_json + off, APPS_JSON_BYTES - (size_t)off,
                "%s{\"slug\":\"%s\",\"grants\":[%s],\"declared\":[%s],\"calls\":%u,"
                "\"denials\":%u,\"denied_mask\":%u,\"beat_ms\":%u,\"cfg_used\":%u,"
                "\"faults\":%u,\"layers\":[%d,%u],\"failed\":%s,\"status\":\"%s\","
                "\"reason\":\"%s\"}",
                i ? "," : "", a->slug, grants, declared,
                (unsigned)orb_svc_calls(a),
                (unsigned)orb_svc_denials(a), (unsigned)orb_svc_denied_mask(a),
                (unsigned)orb_svc_beat_ms(a),
                (unsigned)orb_svc_cfg_used(a), (unsigned)orb_svc_fault_count(a),
                orb_svc_layers_live(a), (unsigned)orb_svc_layers_refused(a),
                a->failed ? "true" : "false", orb_svc_last_status(a),
                orb_svc_fault_reason(a));
            if (w < 0 || (size_t)w >= APPS_JSON_BYTES - (size_t)off) break;
            off += w;
        }
        if (off >= 0 && (size_t)off + 1 < APPS_JSON_BYTES)
            snprintf(apps_json + off, APPS_JSON_BYTES - (size_t)off, "]");
        else if (apps_json)
            snprintf(apps_json, APPS_JSON_BYTES, "[{\"error\":\"ledger too large\"}]");
        /* apps_json NULL (no PSRAM) leaves the "[{"error":"out of memory"}]" the
         * caller already sends for a refused frame buffer — same answer, honest. */
    }

    /* The chip's BASE MAC, not the SoftAP one. esp_read_mac(ESP_MAC_WIFI_SOFTAP)
     * returns base+1 on this target (mac_addr.c:366-387 derives it as memcpy(base)
     * then mac[5] += 1, live because CONFIG_ESP_MAC_ADDR_UNIVERSE_WIFI_AP=y), and
     * both of the hub's join keys are BASE: the USB serial-number descriptor
     * (esprom/usbclass.go:22-25) and the flasher's eFuse read
     * (esprom/commands.go:222-231, ROM targets/esp32s3.py). Reporting
     * SOFTAP here would miss every cable/LAN correlation by one in the last
     * octet, and would look entirely healthy on a bench with no radio. Lowercase
     * hex with no separators, so the wire form is stable and the hub only has to
     * canonicalize case. Empty on the impossible read failure, which the hub
     * treats as "unidentified" rather than matching every other empty MAC. */
    uint8_t base_mac[6] = { 0 };
    char mac_str[13] = { 0 };
    if (esp_read_mac(base_mac, ESP_MAC_BASE) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
                 base_mac[0], base_mac[1], base_mac[2],
                 base_mac[3], base_mac[4], base_mac[5]);
    }

    const int n = snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,\"fw\":\"%s\",\"idf\":\"%s\","
        "\"heap_internal\":%u,\"heap_internal_largest\":%u,\"heap_psram\":%u,"
#if CONFIG_ORB_USB_MTP
        /* The share window has no console and no drive letter beside it, so this is
         * the only place an owner can ask what the mode is doing. reason is the one
         * that matters: without it an orb that failed to start serving and an orb
         * waiting for a host answer the same question with the same silence. */
        "\"usb\":{\"state\":\"%s\",\"host_up\":%s,\"reason\":\"%s\"},"
#endif
        "\"art_ready\":%s,\"art_bytes\":%u,\"art_pages\":%u,\"art_banks\":%u,\"art_refusals\":%u,\"slot\":\"%s\",\"img\":\"%s\","
        "\"synced\":%s,\"epoch\":%lld,\"tz\":\"%s\",\"tz_offset\":%d,\"name\":\"%s\",\"ap\":%s,"
        "\"assoc_fails\":%u,\"wifi_reason\":%u,\"wifi_reason_name\":\"%s\","
        /* What the encoder actually did, not what the driver chose to believe. raw is
         * the quadrature count and det the stream the UI sees, so the pair separates
         * "edges are not arriving" from "the rule is not believing them". spd10 is the
         * measured raw cost of one detent in TENTHS, which is how KNOB_STEPS_PER_DETENT
         * gets set from this unit rather than from a datasheet for a family of them:
         * 40 is a healthy KY-040, 20 means the click emits two and the constant has
         * been asking for two clicks per step. valley/ab are the 2-bit quadrature
         * states, and vhits/shits split the detents by how they were recognised — a
         * dial that is mostly shits is losing edges between the pin and the ISR, which
         * is a wiring or latency fault and no amount of retuning the rule would fix. */
        "\"knob\":{\"raw\":%ld,\"det\":%ld,\"spd10\":%ld,\"span\":%ld,"
        "\"valley\":%ld,\"ab\":%ld,\"vhits\":%ld,\"shits\":%ld},"
        /* The idle dim, in the same shape as the feature's own two questions.
         *
         * since_s counts up to timeout_s and then dimmed goes true, which is the
         * only way to tell "armed and waiting" from "never compiled in" or
         * "something keeps resetting the clock" without a serial cable and a
         * stopwatch — and timeout_s is the compiled-in Kconfig value, so the
         * number a build actually carries is readable from the LAN instead of
         * being inferred from a boot log that a reflash already scrolled away.
         *
         * wakes counts the dims the accelerometer ended, and worst_mg is
         * the widest deviation it has ever measured against the learned rest
         * vector. Settle ORB_IDLE_MOTION_MG from those two: a still orb that
         * reports a worst_mg in the hundreds has a threshold below the desk's own
         * vibration, and one that reports tens but never wakes for a hand lift has
         * it too high. Neither is visible from behaviour alone, and \"is the IMU
         * too sensitive\" cannot be answered at all without them. */
        /* Off is not the same claim as "armed and idle", so it is said with its
         * own flag and the numbers the disabled build cannot know are pinned to
         * zero below, rather than being printed as if they were measurements.
         *
         * writes_failed is the count of brightness writes the panel has refused.
         * A wake that needed a second try is invisible from the glass — the dial
         * simply comes back a tick late, which is what a dial that never comes
         * back looks like until it does — so the retry says so here. Non-zero with
         * dimmed stuck true is the whole diagnosis of "it will not wake". */
        "\"idle\":{\"on\":%s,\"dimmed\":%s,\"since_s\":%u,\"timeout_s\":%u,"
        "\"lit_pct\":%u,\"dim_pct\":%u,\"wakes\":%u,\"worst_mg\":%d,"
        "\"worst_dark_mg\":%d,\"stamps\":%u,\"writes_failed\":%u},"
        "\"rst\":%d,\"rst_name\":\"%s\","
        "\"apps\":%s}",
        s_sta_up      ? "connected"    :
        s_joining     ? "joining"      :
        s_provisioning ? "provisioning" : "down",
        s_ip, mac_str, rssi, app->version, app->idf_ver,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
#if CONFIG_ORB_USB_MTP
        orb_usb_mtp_state(), orb_usb_mtp_host_up() ? "true" : "false", orb_usb_mtp_reason(),
#endif
        art_bank_ready() ? "true" : "false", (unsigned)art_bank_bytes(),
        (unsigned)art_bank_mmu_pages(), (unsigned)orb_art_resident_count(),
        (unsigned)orb_art_refusals(),
        (slot ? slot->label : "?"), img_str,
        s_time_synced ? "true" : "false", (long long)now,
        s_tz[0] ? s_tz : "UTC", tz_offset_min(now),
        /* Unescaped, and safe for exactly the reason s_tz is: name_reject_reason
         * ran over this string before net_name_set() let it into flash, so it
         * holds no quote and no backslash, and net_name_load() re-checks anything
         * read back out of NVS at boot. An unnamed orb reports the empty string
         * rather than a placeholder, so a card can tell "the owner has not named
         * this" from "the device could not say" — which is the distinction that
         * makes the hub's cache rule below implementable at all. Costs 10 B of
         * key plus up to 32 B of value at the widest name either surface allows. */
        s_name,
        /* The AP's real state, from the driver's own events. Without this the
         * whole lifecycle added here is invisible from the LAN — you could not
         * tell a closed AP from a firmware that never closes it, which is
         * exactly the question that motivated this change. assoc_fails rides
         * along because "how long until the rescue AP" is the next question. */
        s_ap_up ? "true" : "false", (unsigned)s_assoc_fails,
        (unsigned)s_last_reason, rname,
        (long)knob_raw_position(), (long)knob_detent_count(),
        (long)knob_raw_steps_per_detent(), (long)knob_raw_span_max(),
        (long)knob_valley(), (long)knob_ab(),
        (long)knob_valley_hits(), (long)knob_settle_hits(),
        idle_on ? "true" : "false", idle_dark, idle_since_s, idle_timeout_s,
        idle_lit_pct, idle_dim_pct, idle_wakes, idle_worst_mg, idle_worst_dk,
        (unsigned)idle_dim_activity_count(),
        (unsigned)idle_dim_level_write_fail_count(),
        rst, rst_nm,
        apps_json ? apps_json : "[{\"error\":\"ledger unavailable\"}]");

    if (n < 0 || (size_t)n >= sizeof(json)) {
        /* A silently truncated body is worse than an error: the page would fail
         * to parse it and render nothing. */
        ESP_LOGE(TAG, "status body overflow (%d B)", n);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"status too large\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* GET /api/v1/config — the zone table is SERVED, not hardcoded in the page: the
 * clock page renders entirely from this reply, which is the only way its list of
 * cities stays in step with the firmware that owns the rules. The reply carries
 * every column a client could want — `label` is what a person reads, `iana` is
 * what a phone's own zone string is matched against, and `posix` (the resolved
 * rule in effect) stays for tooling and for telling a table entry from a
 * hand-set rule; the clock page shows it to nobody. */
static esp_err_t h_config_get(httpd_req_t *req)
{
    /* static, not stack (same reasoning as s_scan_json): 62 rows of
     * name+posix+label (+iana where it differs from the key) is ~5.2 kB, and
     * the echo tail can carry two 64-char strings — the exact worst case is
     * recomputed from the table by tools/host_tz_test.py on every run and
     * asserted against this cap. On httpd's CONFIG_HTTPD_TASK_STACK_SIZE
     * stack that is a real overflow risk; handlers run serially, so one
     * instance is safe. */
    static char json[6144];

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char posix[NET_TZ_MAX + 1];
    tz_resolve(s_tz, posix, sizeof(posix));

    int off = snprintf(json, sizeof(json), "{\"zones\":[");
    if (off < 0 || (size_t)off >= sizeof(json)) goto fail;
    for (int i = 0; i < NET_TZ_N; i++) {
        /* Commit only if the entry fits: snprintf's return is the would-be
         * length, and half an entry left in the buffer would make the whole body
         * unparseable (the exact failure h_wifi_scan documents). */
        /* iana is emitted only where it differs from name: the canonical
         * spelling IS the key for most rows, and at 62 rows the table's body
         * is dominated by that repetition. Clients already fall back
         * (z.iana || z.name). */
        const int w = snprintf(json + off, sizeof(json) - (size_t)off,
                               "%s{\"name\":\"%s\",\"posix\":\"%s\",%s%s%s"
                               "\"label\":\"%s\"}",
                               i ? "," : "", s_tz_tbl[i].name, s_tz_tbl[i].posix,
                               strcmp(s_tz_tbl[i].iana, s_tz_tbl[i].name) == 0 ? "" : "\"iana\":\"",
                               strcmp(s_tz_tbl[i].iana, s_tz_tbl[i].name) == 0 ? "" : s_tz_tbl[i].iana,
                               strcmp(s_tz_tbl[i].iana, s_tz_tbl[i].name) == 0 ? "" : "\",",
                               s_tz_tbl[i].label);
        if (w < 0 || (size_t)w >= sizeof(json) - (size_t)off) goto fail;
        off += w;
    }
    const int t = snprintf(json + off, sizeof(json) - (size_t)off,
                           "],\"tz\":\"%s\",\"posix\":\"%s\",\"tz_offset\":%d}",
                           s_tz[0] ? s_tz : "UTC", posix, tz_offset_min(time(NULL)));
    if (t < 0 || (size_t)t >= sizeof(json) - (size_t)off) goto fail;
    return httpd_resp_sendstr(req, json);

fail:
    /* A truncated body is indistinguishable from a crash to the caller, and an
     * empty picker looks like "no zones configured". Say which it is. */
    ESP_LOGE(TAG, "config body overflow (%d B)", off);
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, "{\"error\":\"config too large\"}");
}

/* POST /api/v1/config {"tz":"US/Eastern"} — or any raw POSIX rule.
 *
 * Response always reports the EFFECT (what the dial is now showing), not an echo
 * of what was sent: a string that newlib silently degrades to UTC still has to
 * come back as "UTC, offset 0", or the page would claim success for a zone that
 * never took. */
static esp_err_t h_config_post(httpd_req_t *req)
{
    static char body[NET_CONFIG_BODY_MAX + 1];

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* Read-until-content_len, exactly as h_wifi_join does and for the same
     * reason: httpd_req_recv() may return a PARTIAL read, and the consequence
     * here is permanent — a body that arrived in two segments would parse as if
     * it ended early and store a truncated TZ, or miss the key entirely and look
     * like a request that asked for nothing. */
    const int clen = (int)req->content_len;
    if (clen < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"content-length required\"}");
    }
    if (clen > NET_CONFIG_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"body too large\"}");
    }
    int got = 0;
    while (got < clen) {
        const int r = httpd_req_recv(req, body + got, (size_t)(clen - got));
        if (r <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"body read failed\"}");
        }
        got += r;
    }
    body[got] = '\0';
    const size_t len = (size_t)got;

    /* {"tz":""} is a real request — it means clear to UTC. A body WITHOUT the key
     * is not: tz is the only setting this endpoint owns today, and answering 200
     * to `{}` would turn a client bug into a silent no-op the page then reports as
     * saved. The reason goes to the log only, never into the reply: it is
     * device-side prose about a value the client controls, and the 400 plus the
     * logged reason is enough to fix the caller. */
    char tz[NET_TZ_MAX + 1];
    bool present = false;
    const char *why = tz_body_value(body, len, tz, sizeof(tz), &present);
    if (!why && !present) why = "no tz in body";
    if (!why) why = net_tz_set(tz);
    if (why) {
        ESP_LOGW(TAG, "config: tz rejected (%s)", why);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"tz rejected\"}");
    }

    /* tz / posix are validated: printable ASCII with no quote or backslash, so
     * they can go into the body unescaped without letting the client close the
     * string early. tz_offset is the number that proves the zone really took. */
    char posix[NET_TZ_MAX + 1];
    tz_resolve(s_tz, posix, sizeof(posix));
    static char resp[256];
    const int n = snprintf(resp, sizeof(resp), "{\"state\":\"zoned\",\"tz\":\"%s\","
                           "\"posix\":\"%s\",\"tz_offset\":%d}",
                           s_tz[0] ? s_tz : "UTC", posix, tz_offset_min(time(NULL)));
    if (n < 0 || (size_t)n >= sizeof(resp))
        return httpd_resp_sendstr(req, "{\"state\":\"zoned\"}");
    return httpd_resp_sendstr(req, resp);
}

/* POST /api/v1/name {"name":"Hall Clock"} — {"name":""} clears it.
 *
 * A separate endpoint rather than a second key on /api/v1/config, and the reason
 * is the contract that endpoint documents: NET_CONFIG_BODY_MAX is 128 sized for
 * {"tz":"…"} and nothing else, and the handler answers 400 "no tz in body" when
 * the key is absent, because silently accepting a tz-less body would turn a
 * client bug into a no-op the page then reports as saved. Adding a second key
 * means reworking that rule — a name-only body is valid, so the handler would
 * have to demand "at least one of the two" and the frame would have to grow for
 * both values at once. That is a larger change to a handler whose sizing is
 * already tight, to buy a request that saves a round trip nobody needs: setting
 * a name and a zone together happens once, during onboarding, and the join body
 * already carries both.
 *
 * Read-until-content_len for the reason h_wifi_join documents in full: a body
 * that arrived in two segments would otherwise parse as truncated, and here the
 * cost of that is storing half a name or clearing one that was meant to be kept. */
static esp_err_t h_name_post(httpd_req_t *req)
{
    static char body[NET_NAME_BODY_MAX + 1];

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const int clen = (int)req->content_len;
    if (clen < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"content-length required\"}");
    }
    if (clen > NET_NAME_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"body too large\"}");
    }
    int got = 0;
    while (got < clen) {
        const int r = httpd_req_recv(req, body + got, (size_t)(clen - got));
        if (r <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"body read failed\"}");
        }
        got += r;
    }
    body[got] = '\0';
    const size_t len = (size_t)got;

    /* Unlike config's tz, a body WITHOUT the key is a real request that means
     * nothing: name is one field of several a client may be setting, and an
     * absent key is "leave the name alone". {} therefore answers 200 with the
     * name it already had. That is the opposite of h_config_post's rule, and the
     * two disagree on purpose — there, tz is the only setting the endpoint owns,
     * so a body missing it can only be a broken client. */
    char name[NET_NAME_MAX + 1];
    bool present = false;
    const char *why = name_body_value(body, len, name, sizeof(name), &present);
    if (!why && present) why = net_name_set(name);
    if (why) {
        /* The reason stays in the log, as config does: it is device-side prose
         * about a value the client controls, and putting it in the reply invites a
         * caller to render attacker-chosen text. The 400 is the contract. */
        ESP_LOGW(TAG, "name: rejected (%s)", why);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"name rejected\"}");
    }

    /* Reports the EFFECT rather than the request, by the same rule as
     * h_config_post: s_name is what net_name_set() left in RAM after validation
     * and after the clear branch, so a request the device silently degraded comes
     * back degraded instead of echoing what was sent. s_name needs no escaping
     * for the reason given at its /api/v1/status echo. */
    static char resp[80];
    const int n = snprintf(resp, sizeof(resp), "{\"state\":\"named\",\"name\":\"%s\"}", s_name);
    if (n < 0 || (size_t)n >= sizeof(resp))
        return httpd_resp_sendstr(req, "{\"state\":\"named\"}");
    return httpd_resp_sendstr(req, resp);
}

/* Scan is ASYNC, and this state machine is why it has to be. The original
 * called esp_wifi_scan_start(..., block=true) inside the handler, and httpd
 * has ONE worker task: a full-channel scan holds that worker 5-10 s, and while
 * it did, the config page's status poll and its /api/v1/config timezone fetch
 * queued behind it and never landed inside the captive-portal webview's
 * patience. Observed on a phone as: shell loads, state stuck at "loading…",
 * no network list, empty timezone dropdown. Sequential curl from a bench
 * cannot reproduce that — a browser fires its first three requests in
 * parallel, one of which takes seconds.
 *
 * Protocol: GET /api/v1/wifi/scan starts a scan when none is live and answers
 * {"status":"scanning"} (HTTP 200, so the page's shared fetch helper treats it
 * as a reply, not an error) until the radio task has drained the driver and
 * rendered s_scan_json. "?fresh=1" — the Rescan button — re-arms from any
 * settled state. The result is cached until the next re-arm: a page reload or
 * a second tab gets the last table instantly instead of paying for a scan.
 * Capped at 12 networks both to bound the reply and because that is all a
 * 466x466 screen can list. */
#define SCAN_MAX_NETWORKS 12
#define SCAN_FETCH_MAX    40   /* records drained looking for distinct SSIDs */
#define SCAN_TIMEOUT_US   ((int64_t)20 * 1000000)
                             /* a scan that never reports done is failed here;
                              * 20 s exceeds a full-channel active scan plus
                              * queue latency, and a late SCAN_DONE is still
                              * drained — see scan_collect's non-SCANNING guard */

enum { SCAN_IDLE = 0, SCAN_SCANNING, SCAN_READY, SCAN_FAILED, SCAN_BUSY };
/* Cross-task: httpd arms SCANNING/IDLE, the radio task settles it to
 * READY/FAILED/BUSY. Every transition is a single store into this volatile
 * int, and s_scan_json is only READ while the state says READY — all buffer
 * writes happen strictly earlier, inside the SCANNING window — so a reader
 * sees one settled state or the next, never a half-built reply. */
static volatile int s_scan_state = SCAN_IDLE;
static int64_t      s_scan_t0_us;   /* when the live scan was armed */

/* A neighbor's SSID is untrusted bytes straight from the air, and an SSID
 * containing a double quote or backslash would corrupt (or be a small injection
 * into) the JSON we hand the browser. Those characters are pathological in real
 * SSIDs, so neutralising them in place is cheaper and safer than an escaping
 * pass over the whole reply. */
static void ssid_sanitize(char *ssid, size_t cap)
{
    for (size_t i = 0; i < cap && ssid[i]; i++) {
        const unsigned char c = (unsigned char)ssid[i];
        if (c == '"' || c == '\\' || c < 0x20 || c == 0x7f) ssid[i] = '.';
    }
}

typedef struct { char ssid[NET_SSID_MAX + 1]; int rssi; uint8_t ch; bool open; } scan_e_t;

/* --------------------------------------------------------------------------
 * Wired provisioning door (Path 2): read one credential line off the USB
 * Serial/JTAG stdin and apply it, for a hub that is wired-only and so has no
 * route to the AP's 192.168.4.1. The console's write side was always used; the
 * read side (host byte -> fgets(stdin) in the running app) was proven on the
 * bench 2026-10-12 before this landed. Runs ONLY while s_provisioning at boot.
 *
 * One line, \n-terminated:   PROV <hex-ssid> <hex-pass> <hex-tz>
 * Each field is hex of the raw bytes; "-" means empty/skip. Hex carries any
 * byte without a delimiter collision, so a credential is never truncated by an
 * unexpected character. Apply is h_wifi_join()'s own sequence (persist, then
 * net_tz_set, then reset back-off, then post the join) so the doors cannot
 * drift; it touches only ssid/pass/tz, never the rest of the shared partition.
 * ------------------------------------------------------------------------ */
static int hexnib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode hex src[0..n) into out, NUL-terminated; "-" decodes to empty. Returns
 * the byte count, or -1 on a bad nibble or if it will not fit (caller rejects
 * the whole line — never truncates). */
static int prov_hex(const char *src, size_t n, char *out, size_t cap)
{
    if (n == 1 && src[0] == '-') { out[0] = '\0'; return 0; }
    if (n % 2 != 0) return -1;
    const size_t len = n / 2;
    if (len + 1 > cap) return -1;
    for (size_t i = 0; i < len; i++) {
        const int hi = hexnib((unsigned char)src[2 * i]);
        const int lo = hexnib((unsigned char)src[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (char)((hi << 4) | lo);
    }
    out[len] = '\0';
    return (int)len;
}

/* Classify this orb's serial state for the hub: (b) unconfigured — the provisioning
 * branch, no usable credential; (c-ap) configured but AP-isolated; (c-sta) configured
 * and a station. (a) — a bare ESP32-S3 running no gorb-os — is not this device's to
 * report: the hub infers it from a chip that answers the ROM and then prints no gorb-os
 * boot banner (see esprom.ParseAppInfo). */
static const char *orb_state_token(void)
{
    if (orb_net_ap_isolated()) return "c-ap";
    if (s_provisioning)         return "b";
    return "c-sta";
}

/* One serial line, dispatched. Returns true when the verb is owned here (handled,
 * caller reads the next line) and false only for PROV, whose operand is a credential
 * pair that belongs with net_post_join in the reader itself. The verb ends at the
 * first space; the operand, if any, is the text after it. Hex operands match PROV's
 * convention exactly (see prov_hex): "-" is empty, any odd length or bad nibble is a
 * rejected line, never a truncated value. */
static bool dispatch_serial_cmd(char *line, size_t vlen)
{
    /* One space-separated verb, then either a hex operand or a literal guard. */
    char *operand = (vlen < strlen(line)) ? line + vlen + 1 : line + vlen;

    if (vlen == 4 && !memcmp(line, "NAME", 4)) {
        char name[NET_NAME_MAX + 1];
        if (prov_hex(operand, strlen(operand), name, sizeof(name)) < 0) {
            ESP_LOGW(TAG, "name: rejected (bad hex)");
            ESP_LOGI(TAG, "NAME-NAK bad-hex");
            return true;
        }
        const char *why = net_name_set(name);   /* net_name_set validates first */
        if (why) {
            ESP_LOGW(TAG, "name: refused (%s)", why);
            ESP_LOGI(TAG, "NAME-NAK %s", why);
        } else {
            ESP_LOGI(TAG, "NAME-ACK %s", s_name[0] ? s_name : "(cleared)");
        }
        return true;
    }

    if (vlen == 4 && !memcmp(line, "SCAN", 4)) {
        s_serial_scan_wanted = true;            /* scan_collect mirrors once, then clears */
        /* Post the ARMED scan, not the bare one. scan_begin() starts the driver
         * scan and leaves s_scan_state alone, because both of its other callers
         * (the page handler and the boot scan) arm SCAN_SCANNING first — and
         * scan_collect() drops the results of a scan nobody armed. So a SCAN on
         * the serial door started a real radio scan, threw the AP list away at
         * the done event, and printed no PROVSCAN-END: the hub re-pushed for its
         * whole budget and reported no ack (bench, 2026-10-15: eight
         * SCAN-ARMED lines, zero PROVSCAN lines, ack:false). scan_boot_begin()
         * is the one starter that arms and starts, and it leaves an in-flight
         * scan alone, so an eager click cannot stack scans. */
        net_post(NET_CMD_SCAN_BOOT, 0);
        ESP_LOGI(TAG, "SCAN-ARMED");
        return true;
    }

    if (vlen == 4 && !memcmp(line, "MODE", 4)) {
        char mode[NET_MODE_MAX + 1];
        if (prov_hex(operand, strlen(operand), mode, sizeof(mode)) < 0) {
            ESP_LOGW(TAG, "mode: rejected (bad hex)");
            ESP_LOGI(TAG, "MODE-NAK bad-hex");
            return true;
        }
        const char *why = net_mode_set(mode);
        if (why) {
            ESP_LOGW(TAG, "mode: refused (%s)", why);
            ESP_LOGI(TAG, "MODE-NAK %s", why);
            return true;
        }
        ESP_LOGI(TAG, "MODE-ACK rebooting into %s", s_mode[0] ? "AP-isolated" : "station");
        orb_svc_note_clean_reboot();            /* a commanded reboot, not a crash */
        vTaskDelay(pdMS_TO_TICKS(300));         /* let the ack reach the host first */
        esp_restart();
        return true;                            /* unreachable */
    }

    /* Factory-reset, the whole device. The literal ALL guard is deliberate: a stray
     * line on an open console must never wipe a working device, so the destructive
     * verb is un-shoutable-by-accident — and ALL is the scope it earns. Everything in
     * both NVS partitions goes (see net_factory_reset): credentials, tz, name, mode,
     * the theme choice, the app grants and enables, every per-app setting, the fault
     * ledger and the idle-dim timeout. The ack is therefore final — no door reopens
     * this state, because the orb comes back as the unconfigured provisioning AP it
     * was the day it was flashed — named only by the default word the reset itself
     * writes back, "The Orb", until a new owner says otherwise. */
    if (vlen == 5 && !memcmp(line, "RESET", 5)) {
        if (strcmp(operand, "ALL") != 0) {
            ESP_LOGW(TAG, "reset: refused (say 'RESET ALL' to erase)");
            ESP_LOGI(TAG, "RESET-NAK need-ALL");
            return true;
        }
        const esp_err_t reset_e = net_factory_reset();
        /* Reboot on a partial failure as well: one partition erased and one intact is
         * neither configured nor clean, and the next boot is the only state that means
         * anything — it re-derives every mirror from whatever survived. The token says
         * which case the operator is in, so a hub that waits for RESET-ACK fails loudly
         * on an incomplete wipe instead of reporting a clean reset. */
        if (reset_e != ESP_OK)
            ESP_LOGE(TAG, "reset: incomplete (%s) — rebooting anyway", esp_err_to_name(reset_e));
        /* D35: the wipe took the factory pair's enable words with it. Rows that
         * still hold a slot get theirs rewritten (shipping state includes a
         * clock); rows the owner had uninstalled queue the ordinary restore job,
         * and a reboot that outruns the job costs only a second press. */
        orb_app_install_factory_rearm();
        ESP_LOGI(TAG, "%s", reset_e == ESP_OK ? "RESET-ACK rebooting unconfigured"
                                              : "RESET-PART rebooting unconfigured");
        orb_svc_note_clean_reboot();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return true;                            /* unreachable */
    }

    if (vlen == 4 && !memcmp(line, "PING", 4)) {
        /* The stamp travels with the ack so a hub with only a lead learns the
         * running version without a reboot-probe: esp_app_get_description is the
         * same RAM read h_status answers from. fw= keys it so an older parser
         * that only grep'd the state token keeps working. */
        ESP_LOGI(TAG, "ORB-PING %s fw=%s", orb_state_token(),
                 esp_app_get_description()->version);
        return true;
    }

    if (vlen == 4 && !memcmp(line, "PROV", 4)) return false;   /* handled in the reader */

    ESP_LOGI(TAG, "serial cmd: unknown verb \"%.*s\"", (int)vlen, line);
    return true;
}

static void prov_reader_task(void *arg)
{
    (void)arg;
    char line[4 * (2 * NET_PASS_MAX + 4)];   /* "PROV " + three hex fields, slack */
    bool use_select = true;                   /* console fd may not support select */

    /* The default USB-Serial-JTAG console reads by polling: the VFS only wires
     * select() once the interrupt driver is installed (usb_serial_jtag_start_
     * select() bails with ESP_ERR_INVALID_STATE otherwise), so a select() on
     * stdin here would return only its 1s timeout forever and fgets() would
     * never run — host->chip bytes sit unread. esp_console installs that driver
     * only when a REPL runs; there is none on this path, so arm it here exactly
     * as esp_console_new_repl_usb_serial_jtag() does. If install fails, drop to
     * the plain blocking fgets() the bench proved. */
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&usj_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "prov reader: USJ driver install failed; blocking fgets only");
        use_select = false;
    } else {
        /* A terminal's Enter sends CR; map it to LF so a human can type a line,
         * and the hub's CRLF collapses to the same line boundary in the VFS. */
        usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
        fcntl(fileno(stdin), F_SETFL, 0);   /* blocking stdin */
        usb_serial_jtag_vfs_use_driver();
    }

    for (;;) {
        if (use_select) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(STDIN_FILENO, &r);
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            const int sel = select(STDIN_FILENO + 1, &r, NULL, NULL, &tv);
            if (sel < 0) {
                if (errno == EINTR) continue;
                /* select unsupported on the console fd: fall back to the plain
                 * blocking fgets() the bench proved, permanently. */
                ESP_LOGI(TAG, "prov reader: select on stdin unsupported (%s); blocking",
                         strerror(errno));
                use_select = false;
                continue;
            }
            if (sel == 0) continue;                 /* timeout tick: door stays armed */
        }
        errno = 0;
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;

        /* The door is open for the whole life of the app now, not only during a
         * first-boot provisioning: the same stdin that takes a PROV credential takes
         * NAME, SCAN, MODE and RESET on a configured orb, so a wire-only hub can rename,
         * rescan, flip AP<->station, or factory-reset a board it cannot reach over the
         * LAN. The task parks in select() until a line arrives, so an untouched console
         * costs nothing. dispatch_serial_cmd returns true for every verb it owns; PROV
         * returns false and is handled inline below, where it can reach the join helpers.
         * A mistyped verb is logged there and the loop stays alive — the console is
         * shared with the human debug log. */
        size_t vlen = strcspn(line, " ");
        if (dispatch_serial_cmd(line, vlen)) continue;
        if (n < 5 || memcmp(line, "PROV ", 5) != 0) continue;   /* not our token */
        if (orb_net_ap_isolated()) {                              /* refuse a silent no-op */
            ESP_LOGW(TAG, "prov: refused while AP-isolated (send MODE sta first)");
            ESP_LOGI(TAG, "PROV-NAK ap-isolated");
            continue;
        }
        char *save = NULL;
        char *f_ssid = strtok_r(line + 5, " ", &save);
        char *f_pass = strtok_r(NULL, " ", &save);
        char *f_tz   = strtok_r(NULL, " ", &save);
        if (!f_ssid) continue;

        char ssid[NET_SSID_MAX + 1];
        char pass[NET_PASS_MAX + 1];
        char tz[NET_TZ_MAX + 1];
        if (prov_hex(f_ssid, strlen(f_ssid), ssid, sizeof(ssid)) <= 0) {
            ESP_LOGW(TAG, "prov: rejected (bad/empty ssid)"); continue;
        }
        const char *p_raw = f_pass ? f_pass : "-";
        const char *z_raw = f_tz   ? f_tz   : "-";
        if (prov_hex(p_raw, strlen(p_raw), pass, sizeof(pass)) < 0 ||
            prov_hex(z_raw, strlen(z_raw), tz,   sizeof(tz))   < 0) {
            ESP_LOGW(TAG, "prov: rejected (bad pass/tz)"); continue;
        }

        ESP_LOGI(TAG, "prov: received ssid=\"%s\" pass=%s%s", ssid,
                 pass[0] ? "(set)" : "(none)", tz[0] ? " +tz" : "");

        /* NOT persisted here. The pair waits in the radio task's pending buffer
         * (see s_pend_ssid) until a lease proves it, and only then reaches NVS —
         * h_wifi_join() does the same. The previous behaviour, writing here, is how
         * one mistyped passphrase on a working device took the device off its own
         * network. The zone is a different case and still applies immediately: it
         * is fully validated above, cannot lock anyone out, and a wrong one is
         * fixable from the LAN. */
        if (tz[0]) {
            const char *why = net_tz_set(tz);
            if (why) ESP_LOGW(TAG, "prov: tz \"%s\" not applied (%s)", tz, why);
        }
        s_assoc_fails = 0;   /* a human just chose this network: reset the ladder */

        ESP_LOGI(TAG, "PROV-ACK");   /* the hub's round-trip confirmation token */
        if (!net_post_join(ssid, pass)) {
            /* No queue means no radio task, so nothing can hold the pair or commit
             * it on a lease. This degenerate path (bring-up OOM) keeps the old
             * write-first behaviour: a lost credential beats a lost join. */
            ESP_LOGW(TAG, "prov: no radio queue — applying from this task");
            creds_commit(ssid, pass);
            const esp_err_t e = net_apply_sta_config(ssid, pass);
            if (e != ESP_OK) ESP_LOGE(TAG, "prov: join(%s) failed: %s", ssid, esp_err_to_name(e));
        }
        /* Stay for a second PROV (a passphrase typo); leave at the next tick once
         * provisioning has ended. */
    }
    ESP_LOGI(TAG, "prov reader: exiting (provisioning over)");
    vTaskDelete(NULL);
}

/* static, not stack: the httpd task stack is only CONFIG_HTTPD_TASK_STACK_SIZE
 * (4 KB default), and 1.2 KB of buffer on top of the handler frame is a real
 * overflow risk. Handlers run serially on the single httpd task, so one
 * instance is safe. .bss does come out of the scarce internal pool (~1.5 KB for
 * the table plus the reply buffer), which is what it costs to dedupe properly. */
static char     s_scan_json[1408];
static scan_e_t s_scan_tbl[SCAN_MAX_NETWORKS];
static int      s_scan_n;

/* Fold one scan record into the distinct-SSID table.
 *
 * Deduping is not cosmetic: the driver reports one record per BSSID, so a mesh /
 * extender estate shows the SAME name many times at different strengths. Measured
 * on the first real network this ran on, 10 results held only 6 distinct names —
 * one name repeated four times, another four times again — leaving a user
 * provisioning to pick between near-identical rows with no way to tell them apart.
 * Strongest signal wins, because that is the access point they would actually
 * roam to. */
static void scan_fold(const wifi_ap_record_t *r)
{
    for (int i = 0; i < s_scan_n; i++) {
        if (strncmp(s_scan_tbl[i].ssid, (const char *)r->ssid,
                    sizeof(s_scan_tbl[i].ssid)) != 0) continue;
        if ((int)r->rssi > s_scan_tbl[i].rssi) {
            s_scan_tbl[i].rssi = (int)r->rssi;
            s_scan_tbl[i].ch   = r->primary;
            s_scan_tbl[i].open = (r->authmode == WIFI_AUTH_OPEN);
        }
        return;
    }
    if (s_scan_n >= SCAN_MAX_NETWORKS) return;
    scan_e_t *e = &s_scan_tbl[s_scan_n++];
    snprintf(e->ssid, sizeof(e->ssid), "%s", (const char *)r->ssid);
    e->rssi = (int)r->rssi;
    e->ch   = r->primary;
    e->open = (r->authmode == WIFI_AUTH_OPEN);
}

/* RADIO TASK ONLY. Start the driver scan; the handler armed SCAN_SCANNING
 * before posting this, so a failure here is the only thing that settles the
 * state early. */
static void scan_begin(void)
{
    s_scan_n = 0;
    const wifi_scan_config_t sc = { .show_hidden = false };
    const esp_err_t se = esp_wifi_scan_start(&sc, /*block=*/false);
    if (se == ESP_ERR_WIFI_STATE) {
        /* A connect() is in flight; the driver aborts scanning in that window.
         * Say so — the page can Rescan, where "scan failed" is a dead end. */
        s_scan_state = SCAN_BUSY;
        return;
    }
    if (se != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(se));
        s_scan_state = SCAN_FAILED;
    }
}

/* RADIO TASK ONLY. Arm and start the first-boot scan the wired-provisioning
 * door reads off the console. scan_begin() expects SCAN_SCANNING already set
 * (the httpd handler does that for a page scan), so this owns that step for the
 * boot case. A scan already in flight is left alone — the page's own scan still
 * covers the phone door. */
static void scan_boot_begin(void)
{
    if (s_scan_state == SCAN_SCANNING || s_scan_state == SCAN_BUSY) return;
    s_scan_t0_us = esp_timer_get_time();
    s_scan_state = SCAN_SCANNING;
    scan_begin();
}

/* RADIO TASK ONLY, posted from WIFI_EVENT_SCAN_DONE. Draining happens here,
 * not in the event callback, for the same reason every other driver call has:
 * one task owns the radio. */
static void scan_collect(void)
{
    if (s_scan_state != SCAN_SCANNING) {
        /* The page already timed out (or nobody started this scan): nothing
         * may read the buffer, but the driver's list still has to be freed or
         * it leaks until the next scan. */
        esp_wifi_clear_ap_list();
        return;
    }

    static wifi_ap_record_t rec;                    /* ~200 B, kept off the stack */

    /* Drain one record at a time into the same static `rec` rather than pulling a
     * batch with esp_wifi_scan_get_ap_records(): the batch form needs
     * count x sizeof(wifi_ap_record_t) of contiguous memory, which is the resource
     * this build is short of. Drain well past SCAN_MAX_NETWORKS because duplicates
     * must be seen before they can be folded away. */
    for (int i = 0; i < SCAN_FETCH_MAX; i++) {
        if (esp_wifi_scan_get_ap_record(&rec) != ESP_OK) break;
        rec.ssid[sizeof(rec.ssid) - 1] = '\0';       /* driver does not guarantee NUL */
        ssid_sanitize((char *)rec.ssid, sizeof(rec.ssid));
        scan_fold(&rec);
    }
    /* Frees the records the cap above left unconsumed; without this the scan
     * list leaks out of the internal pool on every page refresh. */
    esp_wifi_clear_ap_list();

    /* Strongest signal first — the AP worth joining is the one heard best.
     * Insertion sort: at most SCAN_MAX_NETWORKS entries, so no qsort comparator
     * and its indirect-call overhead. */
    for (int i = 1; i < s_scan_n; i++) {
        const scan_e_t k = s_scan_tbl[i];
        int j = i - 1;
        while (j >= 0 && s_scan_tbl[j].rssi < k.rssi) { s_scan_tbl[j + 1] = s_scan_tbl[j]; j--; }
        s_scan_tbl[j + 1] = k;
    }

    const int n0 = snprintf(s_scan_json, sizeof(s_scan_json), "{\"networks\":[");
    if (n0 < 0 || (size_t)n0 >= sizeof(s_scan_json)) goto fail;
    size_t off = (size_t)n0;

    for (int i = 0; i < s_scan_n; i++) {
        /* Format into scratch and commit ONLY if it fits. snprintf's return is
         * the would-be length, so checking it after the write detects overflow
         * but leaves a half-written entry in the buffer; closing that produces
         * unparseable JSON, which renders as an empty network list. (The original
         * version of this handler did exactly that, and 12 networks at the full
         * 32-byte SSID guaranteed it: ~930 B of entries into a 768 B buffer.) */
        char ent[112];
        const int w = snprintf(ent, sizeof(ent),
                               "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u,\"open\":%s}",
                               i ? "," : "", s_scan_tbl[i].ssid, s_scan_tbl[i].rssi,
                               (unsigned)s_scan_tbl[i].ch,
                               s_scan_tbl[i].open ? "true" : "false");
        if (w < 0 || (size_t)w >= sizeof(ent)) continue;      /* malformed entry: skip */
        if ((size_t)w >= sizeof(s_scan_json) - off) break;    /* out of room: stop clean */
        memcpy(s_scan_json + off, ent, (size_t)w + 1);
        off += (size_t)w;
    }

    if ((size_t)snprintf(s_scan_json + off, sizeof(s_scan_json) - off, "]}")
            >= sizeof(s_scan_json) - off) goto fail;
    s_scan_state = SCAN_READY;
    ESP_LOGI(TAG, "scan done: %d distinct network%s", s_scan_n,
             s_scan_n == 1 ? "" : "s");

    /* Wired-provisioning door: while the device is in first-boot provisioning it
     * has no route from a wire-only hub to the AP's 192.168.4.1, so it reads the
     * network list off the serial log. Each line is
     *   PROVSCAN <i> <rssi> <ch> <open0|1> <ssid-to-end-of-line>
     * The SSID is deliberately the rest of the line (it may contain spaces);
     * ssid_sanitize() already removed control chars, so no line can break the
     * framing. PROVSCAN-END closes the list so the hub knows a zero-result scan
     * is complete, not still arriving. Emitted while s_provisioning OR when a serial
     * SCAN armed the one-shot flag — an ordinary page scan stays quiet on the console. */
    if (s_provisioning || s_serial_scan_wanted) {
        s_serial_scan_wanted = false;
        ESP_LOGI(TAG, "PROVSCAN-BEGIN %d", s_scan_n);
        for (int i = 0; i < s_scan_n; i++)
            ESP_LOGI(TAG, "PROVSCAN %d %d %u %d %s", i, s_scan_tbl[i].rssi,
                     (unsigned)s_scan_tbl[i].ch, s_scan_tbl[i].open ? 1 : 0,
                     s_scan_tbl[i].ssid);
        ESP_LOGI(TAG, "PROVSCAN-END %d", s_scan_n);
    }
    return;

fail:
    /* Always reach a settled state: a scan left in SCANNING after a failed
     * render would spin the page until its own timeout, which is
     * indistinguishable from a dead radio from the user's point of view. */
    s_scan_state = SCAN_FAILED;
}

/* The handler never touches the driver — see the SCAN_* comment above for
 * what that cost a phone. It only arms scans (the sole starter, and httpd
 * serves serially, so no double-post) and reports state. */
static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* Rescan re-arms from any settled state; mid-scan it is a no-op, so two
     * eager taps cannot stack scans. */
    const bool fresh = strstr(req->uri, "fresh") != NULL;
    if (fresh && s_scan_state != SCAN_SCANNING) s_scan_state = SCAN_IDLE;

    if (s_scan_state == SCAN_IDLE) {
        s_scan_t0_us = esp_timer_get_time();
        s_scan_state = SCAN_SCANNING;          /* arms the timeout as well */
        net_post(NET_CMD_SCAN, 0);
        return httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
    }

    switch (s_scan_state) {
    case SCAN_SCANNING:
        if (esp_timer_get_time() - s_scan_t0_us > SCAN_TIMEOUT_US) {
            /* The driver never answered with SCAN_DONE. Fail visibly rather
             * than spin: the page offers Rescan, and a late SCAN_DONE still
             * frees the driver's list (see scan_collect's non-SCANNING arm). */
            s_scan_state = SCAN_FAILED;
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"error\":\"scan timed out\"}");
        }
        return httpd_resp_sendstr(req, "{\"status\":\"scanning\"}");
    case SCAN_READY:
        return httpd_resp_sendstr(req, s_scan_json);
    case SCAN_BUSY:
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"busy\", \"detail\":\"association in progress\"}");
    default: /* SCAN_FAILED */
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"scan failed\"}");
    }
}

/* 256 was sized for ssid(32) + pass(64) + tz(64) plus keys and braces. The name
 * field costs a further 42 B at its 32-byte ceiling ("name" key, two quotes, a
 * comma and the braces), so 320 covers the worst case of all four keys present
 * and full without any of them clipping. Refuse rather than clip, as below. */
#define JOIN_BODY_MAX 320

static esp_err_t h_wifi_join(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    static char body[JOIN_BODY_MAX + 1];

    /* httpd_req_recv() may return a PARTIAL read, so a single call is not
     * sufficient: the first attempt used one call plus an `r <= 0` check, which
     * means a body that arrived in two segments was parsed as if it had ended
     * early. Because the credentials are persisted before they are ever
     * verified, that failure mode wrote a truncated password into NVS and the
     * orb then retried a wrong passphrase forever with nothing on screen to
     * explain it. Read until content_len is satisfied. */
    const int clen = (int)req->content_len;
    if (clen < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"content-length required\"}");
    }
    if (clen > JOIN_BODY_MAX) {
        /* Reject rather than clip: see json_str()'s rationale — a truncated SSID
         * that is silently accepted becomes a network that does not exist. */
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"body too large\"}");
    }

    int got = 0;
    while (got < clen) {
        const int r = httpd_req_recv(req, body + got, (size_t)(clen - got));
        if (r <= 0) {                       /* 0 = closed, -1 = recv/timeout error */
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"body read failed\"}");
        }
        got += r;
    }
    body[got] = '\0';
    const size_t len = (size_t)got;

    static char ssid[NET_SSID_MAX + 1];
    static char pass[NET_PASS_MAX + 1];
    if (!json_str(body, len, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"ssid required or too long\"}");
    }
    if (!json_str(body, len, "pass", pass, sizeof(pass)))
        pass[0] = '\0';                              /* open network */

    /* Optional, and parsed + fully validated BEFORE anything is persisted: a join
     * that wrote the credentials and only then noticed the zone was bad would
     * leave the orb on the network with half a request applied and no way for the
     * page to retry the part that failed. Provisioning is the one moment a fresh
     * orb can be zoned before it ever reaches the LAN, which is the whole point of
     * putting it here — a clock that comes up an hour off has to be found on the
     * LAN and re-edited to be fixed. Absent means "leave the stored zone alone",
     * NOT "clear it"; {"tz":""} is what clears. */
    static char tz[NET_TZ_MAX + 1];
    bool tz_present = false;
    const char *tz_why = tz_body_value(body, len, tz, sizeof(tz), &tz_present);
    if (!tz_why && tz_present && tz[0]) tz_why = tz_reject_reason(tz);
    if (tz_why) {
        ESP_LOGW(TAG, "join: tz rejected (%s)", tz_why);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"tz rejected\"}");
    }

    /* Same rule as the zone directly above it, and the reason to name an orb
     * during the join is the reason to zone it there: this is the one moment a
     * fresh board can leave provisioning already called "Hall Clock" instead of
     * orb-44bd8d782618, which is the difference between a card that reads as a
     * room and a card that reads as a hex string. Absent means leave the stored
     * name alone; {"name":""} clears it. Validated here, before the credentials
     * wait for a lease, so a bad name cannot half-apply. */
    static char name[NET_NAME_MAX + 1];
    bool name_present = false;
    const char *name_why = name_body_value(body, len, name, sizeof(name), &name_present);
    if (!name_why && name_present && name[0]) name_why = name_reject_reason(name);
    if (name_why) {
        ESP_LOGW(TAG, "join: name rejected (%s)", name_why);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"name rejected\"}");
    }

    /* One line, so an operator watching the console during onboarding sees the
     * whole request the orb refused or accepted — including the name, which is the
     * field most likely to be typed by hand and the one with nothing on screen to
     * read back at this moment. */
    ESP_LOGI(TAG, "join request: ssid=\"%s\" pass=%s%s%s%s%s", ssid, pass[0] ? "(set)" : "(none)",
             tz_present ? " tz=" : "", tz_present ? tz : "",
             name_present ? " name=" : "", name_present ? name : "");

    /* NOT persisted here: the submitted pair waits in the radio task's pending
     * buffer until this join earns a lease, and only then reaches NVS (see
     * s_pend_ssid). Writing here, which is what this line used to do, let one
     * mistyped passphrase replace a working credential on the first submit - the
     * failure this change exists to remove. The "joining" reply below is now true
     * in a stronger sense: the device has not committed to this network, it is
     * trying it. */

    /* Zone goes through net_tz_set() rather than a local nvs_set_str(), so the
     * write, the setenv/tzset and the log line are literally the same code the
     * config endpoint runs — one behaviour, two doors. Cannot fail on validation
     * here (checked above, before the credentials were touched); the only way it
     * returns a reason is NVS, and that is worth a log, not a lost join. */
    if (tz_present) {
        const char *why = net_tz_set(tz);
        if (why) ESP_LOGW(TAG, "join: tz \"%s\" not applied (%s)", tz, why);
    }

    /* Same door as POST /api/v1/name, same one-behaviour-two-roads argument the
     * tz line above makes: this cannot fail on validation (checked before the
     * credentials were touched), so the only reason it returns is NVS, and a name
     * that did not persist is worth a log line rather than a failed join. A board
     * that joined the LAN unnamed is a naming problem; a board that did not join
     * is the actual failure, and the two must not be conflated in one reply. */
    if (name_present) {
        const char *why = net_name_set(name);
        if (why) ESP_LOGW(TAG, "join: name \"%s\" not applied (%s)", name, why);
    }

    /* A human just said "try these", so start the back-off ladder over — the
     * whole point of the ladder is to stop hammering a network that has already
     * said no, and that reasoning must not apply to a network the user has just
     * corrected. Without this, fixing a passphrase within a minute of the last
     * failure would sit out the current 120 s wait before making the attempt the
     * user can see nothing happening in. */
    s_assoc_fails = 0;

    /* Answer FIRST, then hand the radio over. Applying credentials calls
     * esp_wifi_disconnect(), which drops the very TCP connection this reply
     * travels on, so sending afterwards produced a timeout on a request that had
     * worked — confirmed on the bench, where every join since this feature landed
     * returned nothing at all to the browser.
     *
     * The delay is not enough to make that reliable on its own: sendstr() only
     * hands the segment to lwIP, and a byte still in the retransmit queue when
     * the radio drops never arrives (measured as a 3-8 s curl timeout on an
     * accepted join). So this is a flush window, not a guarantee, and the config
     * page likewise treats a lost reply as "pending" and polls status rather than
     * reporting a rejection. Blocking here costs 250 ms of one httpd worker on a
     * request a human makes once per provisioning; the alternative is an API
     * whose success nobody can observe. */
    const esp_err_t sent = httpd_resp_sendstr(req, "{\"state\":\"joining\"}");
    vTaskDelay(pdMS_TO_TICKS(250));

    /* Nothing left can fail in a way the caller needs told about: the radio
     * outcome is not known yet — it is what /api/v1/status reports as
     * state/assoc_fails/wifi_reason. If the queue never came up (bring-up OOM)
     * there is no radio task to hold the pair or commit it on a lease, so this
     * degenerate path keeps the old write-first behaviour and does the radio work
     * here rather than not do it at all. */
    /* An orb sitting in AP-isolated mode that has just been handed credentials from
     * its own page is being told, by the one person who can, to become a station
     * again. Clear the mode BEFORE the join so a reboot after this lands on the
     * network the page just chose instead of back in isolation, which would read as
     * the join having been ignored. An empty/same mode is a no-op here. */
    if (orb_net_ap_isolated()) net_mode_set("sta");

    if (!net_post_join(ssid, pass)) {
        ESP_LOGW(TAG, "join: no radio queue — applying from the httpd task");
        creds_commit(ssid, pass);
        const esp_err_t e = net_apply_sta_config(ssid, pass);
        if (e != ESP_OK) ESP_LOGE(TAG, "join(%s) failed: %s", ssid, esp_err_to_name(e));
    }
    return sent;
}

/* POST /api/v1/wifi/mode — commit to the station branch or to AP isolation, and reboot.
 *
 * The HTTP twin of the serial MODE verb, and the ONLY page exit from a persistent
 * isolation. That matters more here than anywhere: a board in AP-isolated mode is
 * reachable at exactly one address, its own 192.168.4.1, so if the page could only get
 * INTO the mode the mode would be a trap with one key. Joining a network from the page
 * also clears it (h_wifi_join), but an owner who wants their saved credentials back
 * without retyping them has to be able to say "station" and nothing else.
 *
 * Body: {"mode":"ap"} or {"mode":"sta"}. The mode is a boot-time choice — see
 * net_mode_set, which writes NVS and changes nothing live — so this answers, waits the
 * 750 ms h_reprov measured, and reboots. Refusals are complete: no partial mode, no
 * reboot on a rejected operand. */
static void mode_timer_cb(void *arg)
{
    (void)arg;
    /* A commanded reboot, not a crash — the same courtesy h_reprov and h_reprov pay, so
     * the boot after an owner's MODE does not bill an app for the restart. */
    orb_svc_note_clean_reboot();
    ESP_LOGW(TAG, "rebooting into the %s branch on request (POST /api/v1/wifi/mode)",
             s_mode[0] ? "AP-isolated" : "station");
    esp_restart();
}

static esp_err_t h_wifi_mode(httpd_req_t *req)
{
    char body[64];
    if (req->content_len > (unsigned)sizeof(body) - 1) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"body too large\"}");
    }
    int got = 0;
    while (got < (int)req->content_len) {
        const int r = httpd_req_recv(req, body + got, (size_t)(int)req->content_len - got);
        if (r <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"body read failed\"}");
        }
        got += r;
    }
    body[got] = '\0';

    char mode[NET_MODE_MAX + 1];
    if (!json_str(body, (size_t)got, "mode", mode, sizeof(mode))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"mode required (\\\"ap\\\" or \\\"sta\\\")\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

#if !CONFIG_ORB_NET_AP_ISOLATED
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"error\":\"AP-isolated mode is not in this build\"}");
#else
    const char *why = net_mode_set(mode);
    if (why) {
        /* The reason is one of net_mode_set's own fixed strings — no user text reaches
         * the body, so this needs no escaping and cannot be made to close the object
         * early. */
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"mode must be \\\"ap\\\" or \\\"sta\\\"\"}");
    }
    static esp_timer_handle_t s_mode_timer;
    if (!s_mode_timer) {
        const esp_timer_create_args_t ta = { .callback = mode_timer_cb };
        if (esp_timer_create(&ta, &s_mode_timer) != ESP_OK) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"error\":\"reboot timer\"}");
        }
    }
    if (esp_timer_start_once(s_mode_timer, 750 * 1000) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"reboot schedule\"}");
    }
    ESP_LOGW(TAG, "mode: now %s, rebooting in 750 ms (POST /api/v1/wifi/mode)",
             s_mode[0] ? "AP-isolated" : "station");
    char out[80];
    const int n = snprintf(out, sizeof out, "{\"state\":\"rebooting\",\"mode\":\"%s\"}",
                           s_mode[0] ? "ap" : "sta");
    return httpd_resp_sendstr(req, n > 0 ? out : "{\"state\":\"rebooting\"}");
#endif
}

/* POST /api/v1/wifi/ap — raise or lower the provisioning AP by request.
 *
 * The config page deliberately has NO button on the OPEN half, and the
 * reasoning is worth keeping with the code: whoever can load the page can
 * change the orb's network FROM the page, so an orb reachable enough to show
 * a button never needs a human pressing it to summon an AP; and an orb that
 * ISN'T reachable shows no button to press. (The device's own rescue path
 * posts NET_CMD_AP_RESCUE internally — it does not come through this
 * endpoint.) What remains genuinely useful, and what this endpoint is for:
 *   - the CLOSE half, which the config page posts when the user taps Copy /
 *     Open / Done on the confirmation. Not automatic on render: Android binds
 *     its captive-portal webview to the AP, so closing the AP unasked took the
 *     page -- and the new address on it -- away with it. The hold timer still
 *     covers a page that was never acknowledged, and
 *   - tooling/tests (e.g. /tmp/ap_cp_test.sh) summoning the AP with no page
 *     open at all.
 *
 * Body is optional: {} opens for CONFIG_ORB_NET_AP_MANUAL_S seconds;
 * {"on":"off"} closes it immediately, honoured only while a lease means there
 * is still another way in. The open path goes through the radio task exactly
 * like the rescue path — one code path, one set of log lines — and both arm
 * the AP's expiry, which is why no caller can leave it open by accident. */
static esp_err_t h_wifi_ap(httpd_req_t *req)
{
    char body[NET_CONFIG_BODY_MAX + 1];
    /* content_len + read-until-complete, exactly as h_wifi_join does it and for
     * the same reason: httpd_req_recv() may return a PARTIAL read, and a
     * half-read body here would be interpreted as a different intent — a
     * truncated {"on":"off"} reads as "no on key", which would OPEN the AP when
     * the caller asked to close it. */
    const int clen = (int)req->content_len;
    if (clen > NET_CONFIG_BODY_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"body too large\"}");
    }
    int n = 0;
    while (n < clen) {
        const int r = httpd_req_recv(req, body + n, (size_t)(clen - n));
        if (r <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"body read failed\"}");
        }
        n += r;
    }
    body[n] = '\0';

    bool close_now = false;
    if (n > 0) {
        /* Two spellings of one intent, both cheap to accept: the page sends
         * {"on":"off"} and a person at a curl prompt sends {"on":false}. */
        const char *v = json_find(body, (size_t)n, "on");
        if (v && v[0] == 'f') close_now = true;
        else if (v && v[0] == '"') {
            char on[8] = "";
            json_str(body, (size_t)n, "on", on, sizeof on);
            close_now = strcmp(on, "off") == 0 || strcmp(on, "false") == 0 ||
                        strcmp(on, "0") == 0;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (close_now) {
        /* Only honoured while there IS another way in. Reaching an orb with no
         * lease and no AP is precisely the lockout the rescue path exists for,
         * so saying "ok, closed" there would be actively harmful — 409 and a
         * reason instead. */
        if (!s_sta_up) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req,
                       "{\"error\":\"no lease — the AP is the only way in\"}");
        }
        net_post(NET_CMD_AP_EXPIRED, 0);   /* the one path that closes a leased AP */
        return httpd_resp_sendstr(req, "{\"state\":\"ap_closing\"}");
    }
    net_post(NET_CMD_AP_OPEN, CONFIG_ORB_NET_AP_MANUAL_S);
    char ssid[16];
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, sizeof(ssid), "orb-%02X%02X", mac[4], mac[5]);
    /* Reports the SSID because the phone has to find it in a list of networks,
     * and the seconds because "how long do I have" is the first question a
     * person standing at a wall with a phone asks. */
    char out[128];
    const int m = snprintf(out, sizeof(out),
                           "{\"state\":\"ap_opening\",\"ap_ssid\":\"%s\",\"seconds\":%d}",
                           ssid, CONFIG_ORB_NET_AP_MANUAL_S);
    if (m < 0 || (size_t)m >= sizeof(out)) return httpd_resp_sendstr(req, "{\"state\":\"ap_opening\"}");
    return httpd_resp_sendstr(req, out);
}

/* -------------------------------------------------------------------------
 * OTA: upload an app image over the LAN and stage it in the inactive A/B slot.
 *
 * Deliberately NOT esp_https_ota: that pulls in its own HTTP client plus TLS
 * handling, and TLS is terminated upstream by the reverse proxy (see orb_net.h),
 * so the device-side equivalent is a plain POST over esp_ota_ops. It would also
 * require the download to be reachable FROM the device, which a LAN-side page
 * driving a sealed device on someone's laptop is not.
 *
 * Staged, never auto-applied: /api/v1/ota writes, validates, and sets the boot
 * partition; the caller then decides when to reboot via /api/v1/reboot. A device
 * showing the time to a whole household should not reboot because a curl
 * finished, and staging first means a bad upload costs nothing — the running
 * image is untouched until that second request.
 * ------------------------------------------------------------------------- */

#define OTA_CHUNK 4096   /* static buffer, not stack — see cfg.stack_size in httpd_start() */

/* Smallest credible app image: 24 B image header + one segment header + the
 * appended SHA-256. Below this it is not an image, and rejecting it here saves
 * erasing a 2 MB slot for garbage. */
#define OTA_MIN_BYTES 96

static bool s_ota_busy;   /* one upload at a time; httpd has 3 slots */

static esp_err_t ota_reject(httpd_req_t *req, esp_ota_handle_t *h,
                            const char *status, const char *msg, esp_err_t logged)
{
    if (h && *h) {
        esp_ota_abort(*h);      /* frees the ops entry; slot left uncommitted */
        *h = 0;
    }
    s_ota_busy = false;
    if (logged != ESP_OK) ESP_LOGE(TAG, "ota: %s (%s)", msg, esp_err_to_name(logged));
    else                  ESP_LOGE(TAG, "ota: %s", msg);
    httpd_resp_set_status(req, status);
    /* Echo the reason in the body, not just the status line: every msg here is a
     * string literal (never client data, so no escaping hazard), and an upload
     * that fails while the user is staring at a progress bar has to say why
     * without anyone attaching a serial cable to a mounted device. */
    char body[160];
    const int n = (logged != ESP_OK)
        ? snprintf(body, sizeof(body), "{\"error\":\"%s\",\"detail\":\"%s\"}", msg, esp_err_to_name(logged))
        : snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    if (n < 0 || (size_t)n >= sizeof(body))
        return httpd_resp_sendstr(req, "{\"error\":\"update rejected\"}");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t h_ota(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    static char ota_buf[OTA_CHUNK];

    const int clen = (int)req->content_len;

    /* Content-Length is mandatory, not mere strictness. esp_ota_begin() erases
     * align_up(size, sector) when given a size, but the ENTIRE 2 MB slot when
     * handed OTA_SIZE_UNKNOWN (esp_ota_ops.c:147-153) — and flash erase runs with
     * the cache disabled, which stalls the LVGL task and blanks the clock for
     * seconds. Taking the declared length shrinks that stall to the ~1.3 MB
     * actually being written. A chunked upload gets 411, not a blackout. */
    if (clen <= 0) return ota_reject(req, NULL, "411 Length Required",
                                     "Content-Length required", ESP_OK);
    if (clen < OTA_MIN_BYTES) return ota_reject(req, NULL, "400 Bad Request",
                                                "body too small to be an image", ESP_OK);
    if (s_ota_busy) return ota_reject(req, NULL, "409 Conflict",
                                      "an update is already in progress", ESP_OK);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target  = esp_ota_get_next_update_partition(NULL);
    if (!target) return ota_reject(req, NULL, "507 Insufficient Storage",
                                   "no spare app slot found", ESP_OK);
    if (running && target->subtype == running->subtype) {
        /* The S3 cannot OTA into the partition it is running from. With only
         * ota_0/ota_1, hitting this means the slot pairing is broken. */
        return ota_reject(req, NULL, "507 Insufficient Storage",
                          "next update slot is the running one", ESP_OK);
    }
    if ((size_t)clen > target->size) {
        static char m[96];
        snprintf(m, sizeof(m), "{\"error\":\"too large\",\"max\":%u}", (unsigned)target->size);
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, m);
    }

    s_ota_busy = true;
    ESP_LOGW(TAG, "ota: staging %d B into %s (0x%x)",
             clen, target->label, (unsigned)target->address);

    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(target, (size_t)clen, &h);
    if (e != ESP_OK) {
        /* ESP_ERR_OTA_ROLLBACK_INVALID_STATE lands here: rollback is compiled in
         * and this boot is still PENDING_VERIFY, i.e. esp_ota_mark_app_valid_cancel_rollback()
         * has not run yet (main.c does it once the dial is confirmed up). Refusing
         * the second upgrade is the point of that check — it stops a still-unproven
         * image from overwriting its own fallback before anyone knows whether it
         * works. */
        return ota_reject(req, &h, "500 Internal Server Error", "ota_begin failed", e);
    }

    int got = 0;
    while (got < clen) {
        const int want = clen - got;
        const int r = httpd_req_recv(req, ota_buf,
                                    (size_t)(want < OTA_CHUNK ? want : OTA_CHUNK));
        if (r <= 0) {
            /* Client vanished mid-upload. The slot is left uncommitted and the
             * boot partition still points at the working image — this failure
             * mode is the whole reason staging and rebooting are two requests. */
            return ota_reject(req, &h, "400 Bad Request", "upload aborted by client", ESP_OK);
        }
        e = esp_ota_write(h, ota_buf, (size_t)r);
        if (e != ESP_OK) return ota_reject(req, &h, "500 Internal Server Error", "flash write failed", e);
        got += r;
    }

    /* esp_ota_end() CONSUMES the handle — it LIST_REMOVEs + free()s the ops entry
     * and validates via esp_image_verify() — so h must not be aborted after it.
     * This validation is the reason to be here at all: a truncated or garbage
     * upload fails and the boot partition is left pointing at the working image. */
    e = esp_ota_end(h);
    h = 0;
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
        return ota_reject(req, NULL, "400 Bad Request",
                          "image failed validation (valid app image for this chip?)", e);
    }
    if (e != ESP_OK) return ota_reject(req, NULL, "500 Internal Server Error", "ota_end failed", e);

    /* Commits the slot as next-to-boot (and re-validates independently). */
    e = esp_ota_set_boot_partition(target);
    if (e != ESP_OK) return ota_reject(req, NULL, "500 Internal Server Error", "set_boot_partition failed", e);

    s_ota_busy = false;
    ESP_LOGW(TAG, "ota: staged %d B; boot slot -> %s, reboot to activate", clen, target->label);

    static char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"state\":\"staged\",\"slot\":\"%s\",\"bytes\":%d,"
             "\"reboot\":\"POST /api/v1/reboot\"}", target->label, clen);
    return httpd_resp_sendstr(req, resp);
}

/* Reboot is a separate verb from staging (see h_ota). The response must leave
 * before esp_restart() runs — otherwise the caller sees a dropped connection and
 * cannot tell success from failure — so the restart is deferred to a one-shot
 * timer instead of being done inline. */
static void reboot_timer_cb(void *arg)
{
    (void)arg;
    /* Same C2 courtesy as orb_jobs' reboot_fire: a POST /api/v1/reboot is
     * the owner talking, so the next boot bills no app. */
    orb_svc_note_clean_reboot();
    ESP_LOGW(TAG, "rebooting on request (POST /api/v1/reboot)");
    esp_restart();
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    static esp_timer_handle_t s_reboot_timer;
    if (!s_reboot_timer) {
        const esp_timer_create_args_t ta = { .callback = reboot_timer_cb };
        if (esp_timer_create(&ta, &s_reboot_timer) != ESP_OK) {
            ESP_LOGE(TAG, "ota: could not create reboot timer");
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"error\":\"reboot timer\"}");
        }
    }
    if (esp_timer_start_once(s_reboot_timer, 750 * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "ota: could not arm reboot timer");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"reboot schedule\"}");
    }
    ESP_LOGW(TAG, "ota: rebooting in 750 ms");
    return httpd_resp_sendstr(req, "{\"state\":\"rebooting\"}");
}
/* POST /api/v1/reprov — reboot into the provisioning branch, credentials intact.
 *
 * D2's remote writer and the HTTP sibling of D5's knob hold. The flag write and the
 * reboot stay two separate steps on purpose: orb_net_request_provisioning() only sets
 * the RTC word, and the deferral below is what lets this response reach the caller
 * before the connection dies — the same 750 ms h_reboot measured.
 *
 * Every branch here is a refusal or a deferral, never a partial action. Nothing is
 * written to flash: an intent that loses power before it boots disappears with the
 * power, which is why the cancel path is unplug-the-orb and not a button (plan D2's
 * retention note). And the D4 sojourn needs no mention here — the boot branch arms it
 * whenever it forces the provisioning path, so a hub-triggered reprovision expires on
 * the same clock as a knob-triggered one and no caller can forget to arm it.
 */
static void reprov_timer_cb(void *arg)
{
    (void)arg;
    /* Same C2 courtesy as h_reboot: this reboot is the owner talking, so the next boot
     * bills no app for it. */
    orb_svc_note_clean_reboot();
    ESP_LOGW(TAG, "rebooting into provisioning on request (POST /api/v1/reprov)");
    esp_restart();
}

static esp_err_t h_reprov(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (orb_net_is_provisioning()) {
        /* Already there. Rebooting a device that is mid-provisioning would restart the
         * sojourn clock for no reason and drop the very page the operator is using. */
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"already provisioning\"}");
    }
    if (!orb_net_request_provisioning()) {
        /* CONFIG_ORB_NET_AP_FALLBACK=n leaves no SoftAP behind, so "provisioning" would
         * be a state with no way in and no way out. Refuse rather than take someone's
         * wall clock down to a dead end. */
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"no AP fallback in this build\"}");
    }

    static esp_timer_handle_t s_reprov_timer;
    if (!s_reprov_timer) {
        const esp_timer_create_args_t ta = { .callback = reprov_timer_cb };
        if (esp_timer_create(&ta, &s_reprov_timer) != ESP_OK) {
            prov_intent_disarm();
            ESP_LOGE(TAG, "reprov: could not create reboot timer");
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"error\":\"reboot timer\"}");
        }
    }
    if (esp_timer_start_once(s_reprov_timer, 750 * 1000) != ESP_OK) {
        prov_intent_disarm();
        ESP_LOGE(TAG, "reprov: could not arm reboot timer");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"reboot schedule\"}");
    }
    ESP_LOGW(TAG, "reprov: provisioning boot armed, rebooting in 750 ms");
    return httpd_resp_sendstr(req, "{\"state\":\"rebooting\",\"into\":\"provisioning\"}");
}


/* Landing page for the first-boot SoftAP, when there is no PWA loaded yet (the
 * `www` partition is Phase 4). This is the whole provisioning UX, so it has to
 * stand on its own: a phone that just joined 'orb-xxxxxx' opens 192.168.4.1 and
 * must be able to pick a network, type a passphrase, and see where the orb went
 * afterwards. It stays dependency-free (no framework, no CDN, no bundled JS)
 * because the device may be unreachable from the internet while provisioning and
 * a blank page would be indistinguishable from a broken radio.
 *
 * The final IP is shown deliberately prominently: the orb leaves the SoftAP's
 * subnet on success, so the page you are looking at becomes unreachable and the
 * address to type next has to be on screen before that happens.
 *
 * There is deliberately NO timezone UI on this page — not even a collapsed one.
 * Owner's call after provisioning first worked end to end: this page's job ends
 * at "the orb is on your network, here is where it went", and a zone dropdown
 * asking a first-time user mid-join to read POSIX rules served neither job. The
 * clock is set on its own page (/timezone, HTML_TZ below), which the success
 * confirmation links to. */
/* ===========================================================================
 * SD card + themes.
 *
 * The device cannot notice a card being inserted — the socket's detect line is not
 * wired (main/sd/orb_sd.h) — so every route here asks the SD module to mount on
 * demand and tolerates the answer being "no card". None of them can take the clock
 * down; the worst they do is say so in JSON.
 * ======================================================================== */

/* Read one query parameter into out. httpd_query_key_value() is two-pass (length,
 * then copy); doing it by hand instead keeps the URL-decode in one place and lets the
 * refusals below happen BEFORE any string reaches a validator or the path builder. A
 * '%' that is not a valid %XX escape is rejected outright rather than passed on,
 * because the classic way "?id=%2e%2e%2f" turns into a traversal is a decoder that is
 * generous and a check that runs afterwards on a different string than the one it
 * inspected — the id is compared against sha256() of a header name, so a value that
 * decoded differently than it was checked would defeat that comparison outright. */
static bool q_param(httpd_req_t *req, const char *key, char *out, size_t outsz)
{
    char qbuf[192], raw[ORB_SD_NAME_MAX * 3 + 8];
    /* The two-call API, not req->query: the query string is only parsed on demand
     * and httpd_req_get_url_query_str() is the documented accessor (and the only one
     * that exists — httpd_req_t has no query member in IDF v5.4). */
    if (httpd_req_get_url_query_str(req, qbuf, sizeof qbuf) != ESP_OK) return false;
    if (httpd_query_key_value(qbuf, key, raw, sizeof raw) != ESP_OK) return false;

    size_t o = 0;
    for (const char *p = raw; *p && o + 1 < outsz; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        if (!p[1] || !p[2]) return false;                        /* truncated escape */
        int v = 0;
        for (int i = 1; i <= 2; i++) {
            const char c = p[i];
            int d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;                                   /* not a hex pair */
            v = v * 16 + d;
        }
        /* NUL and '/' can never be part of a theme id or any of the short control
         * values read here; refusing them at decode time is what keeps the value the
         * path builder receives identical to the value orb_sd_name_ok() looked at. */
        if (v == 0 || v == '/') return false;
        out[o++] = (char)v;
        p += 2;
    }
    out[o] = '\0';
    return o > 0;
}


/* GET /api/v1/screen?to=<slug> - put a registered app on the glass from the LAN.
 *
 * Gated by CONFIG_ORB_DEBUG_HTTP (plan §7 item 7): it is the only route whose
 * whole purpose is measurement, and a build that leaves the bench should not
 * carry a LAN-reachable knob injector. With the symbol off, both this handler and
 * its entry in the uri table are compiled out, so the route 404s rather than
 * answering. host_tz_test.py counts routes textually over this table, so an
 * off-build count is one higher than what registers — the safe direction: the
 * lint refuses cap < count, and an over-count can only ever ask for more room.
 *
 * This exists so the menu and the picker can be verified without a hand on the
 * knob. The encoder is three GPIOs driven only by a human finger, so every other
 * route into lv_screen_load() is unreachable from the bench, and a screen that
 * has never been painted has never actually been looked at. It is a debug door,
 * not a feature: it changes what is displayed and nothing else, cannot reach the
 * card, the radio or the bootloader, and is strictly less capable than the
 * /api/v1/reboot and /api/v1/ota endpoints registered beside it.
 *
 * The switch is requested, not performed: lv_screen_load() touches widgets and
 * belongs on the LVGL task, so the router applies it on its next poll tick. */
#if CONFIG_ORB_DEBUG_HTTP
static esp_err_t h_screen(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char to[24] = "";
    char turn[8] = "";
    char push[4] = "";
    char hold[4] = "";
    const bool have_to   = q_param(req, "to",   to,   sizeof to);
    const bool have_turn = q_param(req, "turn", turn, sizeof turn);
    q_param(req, "push", push, sizeof push);   /* both tested by content below */
    q_param(req, "hold", hold, sizeof hold);

    /* Screen switch and synthetic knob events are the same endpoint because they
     * are one test: go to the menu, arm it, scroll, choose. Keeping them apart
     * would mean a caller had to trust ordering between two URLs. */
    if (have_to) {
        /* A registry slug, resolved by orb_app_find_slug() - no longer a
         * three-way strcmp against input_screen_t values that had to be kept in
         * step with an enum by hand. D35 moved the picker from a compiled alias
         * to an ordinary card row, and card rows have no alias field — so the
         * PWA's historical "?to=hue" is translated here, in the layer that owns
         * routes, to the row's real slug. Every other name is the registry's
         * business: clock and menu are the rows' own slugs, and an app that
         * installs is reachable here the moment it is in the table. */
        if (strcmp(to, "hue") == 0) snprintf(to, sizeof to, "color_picker");
        const orb_app_t *app = orb_app_find_slug(to);
        if (!app) {
            /* The bad value goes to the log, never into the body: it is
             * caller-controlled text, and an error page that repeats it is how a
             * quoting bug becomes someone else's problem. The body names only
             * what the device accepts — and since the registry grew card slugs,
             * it streams them from the table instead of restating a hand-kept
             * list that is wrong the day an app installs. Slugs and aliases are
             * writer-validated to [a-z0-9_] and compiled literals: no escaping. */
            ESP_LOGW(TAG, "screen: unknown to=\"%s\"", to);
            httpd_resp_set_status(req, "400 Bad Request");
            char body[512];
            size_t kn = (size_t)snprintf(body, sizeof body,
                                         "{\"error\":\"unknown app\",\"known\":[");
            bool first = true;
            for (size_t i = 0; i < orb_app_rows(); i++) {
                const orb_app_t *r = orb_app_row(i);
                if (!r) continue;   /* positional span: a free slot is a hole */
                const int w = snprintf(body + kn, sizeof body - kn, "%s\"%s\"",
                                       first ? "" : ",", r->slug);
                if (w < 0 || (size_t)w >= sizeof body - kn - 4) break;
                kn += (size_t)w;
                first = false;
                if (r->alias && r->alias[0]) {
                    const int v = snprintf(body + kn, sizeof body - kn,
                                           ",\"%s\"", r->alias);
                    if (v < 0 || (size_t)v >= sizeof body - kn - 4) break;
                    kn += (size_t)v;
                }
            }
            snprintf(body + kn, sizeof body - kn, "]}");
            return httpd_resp_sendstr(req, body);
        }
        ESP_LOGI(TAG, "screen request -> %s", app->slug);
        input_router_request(app);
    }

    if (have_turn) {
        /* atoi and not strtol: the saturating add in input_router_inject() makes
         * the range irrelevant here, and a rejected 400 for "turn=abc" would just
         * be a slower way of landing on zero. */
        const int32_t d = (int32_t)atoi(turn);
        ESP_LOGI(TAG, "synthetic turn %+ld detent(s)", (long)d);
        input_router_inject(d, false, false);
    }

    if (push[0] == '1') {
        ESP_LOGI(TAG, "synthetic push");
        input_router_inject(0, true, false);
    }

    if (hold[0] == '1') {
        /* The escape hatch, exercised over HTTP: an armed screen with a broken
         * long-press is a dial you cannot leave. */
        ESP_LOGI(TAG, "synthetic long-press");
        input_router_inject(0, false, true);
    }

    if (!have_to && !have_turn && !push[0] && !hold[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
               "{\"error\":\"to, turn, push or hold required\"}");
    }
    return httpd_resp_sendstr(req, "{\"state\":\"accepted\"}");
}
#endif /* CONFIG_ORB_DEBUG_HTTP */

#if CONFIG_ORB_DEBUG_HTTP
/* GET /api/v1/fb?fmt=bmp|raw565&wait=ms&direct=1 — the glass, as bytes.
 *
 * Every claim about a screen so far rested on a log line saying the draw path
 * ran plus a person holding the dial. "It runs" is not "it looks right", and
 * §26's slice 1 status is stuck on exactly that difference. The CO5300 GRAM is
 * write-only, so the panel cannot be read back; the PSRAM mirror IS the frame
 * the panel received, and display_capture_arm() copies it on the LVGL task
 * inside the commit window, so this is one whole displayed frame rather than a
 * screenshot race.
 *
 * Same gate as /api/v1/screen and the same reason (§7 item 7): measurement, not
 * a feature. It reads pixels and changes nothing, cannot reach the card, the
 * radio or the bootloader, and is less capable than the reboot route beside it.
 * At 652 KB it is also the largest response this device makes — a bench-only
 * expense by design. bmp is a plain 24-bit BMP so any viewer opens it with
 * nothing installed; raw565 is little-endian top-down for tools that want
 * pixels rather than a picture of them. One capture at a time: the staging
 * buffer and row scratch are singletons, so a second caller gets 503 rather
 * than interleaved pixels.
 *
 * Two behaviours a bench operator will meet, both measured here rather than
 * predicted. Aborting mid-transfer is NOT free: esp_http_server is one task, so
 * a half-read 652 KB response sits in send() until the socket times out and
 * every other route — /api/v1/status included — answers nothing meanwhile
 * (observed: ~1-2 min of dark API after two Ctrl-C'd grabs, recovering on its
 * own with nothing wedged). Let a grab finish. And ?direct=1 on a screen that
 * repaints can return the frame from before a just-requested switch: it copies
 * what the mirror holds at that instant, which is honest but not what was
 * asked for, so prefer the commit path when a switch is in flight.
 *
 * BMP rows pad to 4 bytes (466*3 = 1398 -> 1400), 932 B less than the row
 * scratch's size. Sending the scratch instead of the padded row shears the
 * frame by a third of its height, and a thumbnail of a round dial does not
 * visibly fail — which is how that one got past review once already.
 */
static esp_err_t h_fb(httpd_req_t *req)
{
    static uint8_t s_row[SCREEN_WIDTH * 3 + 4];   /* one BMP row + pad */
    static bool    s_busy;
    char v[8];
    const bool want_raw = q_param(req, "fmt", v, sizeof v) && !strcmp(v, "raw565");
    const bool go_direct = q_param(req, "direct", v, sizeof v) && v[0] == '1';
    char wbuf[8] = "";
    int wait_ms = 1500;
    if (q_param(req, "wait", wbuf, sizeof wbuf)) {
        wait_ms = atoi(wbuf);
        if (wait_ms < 0) wait_ms = 0;
        if (wait_ms > 8000) wait_ms = 8000;
    }

    if (!display_capture_present()) {
        ESP_LOGW(TAG, "fb: no panel mirror in this build, nothing to capture");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"no panel mirror\"}");
    }
    if (s_busy) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"capture already running\"}");
    }
    s_busy = true;

    esp_err_t r = ESP_OK;
    uint32_t seq = 0;
    bool torn = true;
    bool have = false;

    /* Armed only for the commit-synchronised path. display_capture_direct()
     * refuses while ARMED — that refusal is what keeps a commit's copy and a
     * fallback copy from filling the same buffer at once — so arming first
     * would make ?direct=1 structurally unable to ever run. It did exactly
     * that until the first bench capture caught it. */
    if (!go_direct) {
        if (display_capture_arm() != ESP_OK) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "{\"error\":\"cannot stage capture (psram)\"}");
            s_busy = false;
            return ESP_OK;
        }
        /* The copy happens at the next commit. On the clock that is a frame or
         * two away; a screen that never repaints never commits, and that is
         * what ?direct=1 and the 504 below are for. */
        for (int waited = 0; waited < wait_ms; waited += 10) {
            if (display_capture_ready(&seq, &torn)) { have = true; break; }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (!have && go_direct) {
        r = display_capture_direct(&seq);
        if (r == ESP_OK) { torn = true; have = true; }
    }
    if (!have) {
        if (go_direct) {
            /* Only one thing makes a direct copy refuse: a commit is mid-copy
             * into the same buffer, so another caller armed first. */
            ESP_LOGW(TAG, "fb: direct copy refused, a commit holds the buffer");
            httpd_resp_set_status(req, "503 Service Unavailable");
            s_busy = false;
            return httpd_resp_sendstr(req,
                   "{\"error\":\"capture buffer held by a commit\",\"retry\":\"later\"}");
        }
        ESP_LOGW(TAG, "fb: no commit within %d ms; retry with ?direct=1", wait_ms);
        httpd_resp_set_status(req, "504 Gateway Timeout");
        httpd_resp_sendstr(req, "{\"error\":\"no frame committed\",\"retry\":\"direct=1\"}");
        s_busy = false;
        return ESP_OK;
    }
    r = ESP_OK;

    const uint16_t *src = display_capture_data();
    const size_t total = display_capture_bytes();
    /* Padded row, always: the tool on the other end checks its received length
     * against this number, and a BMP row is 466*3 rounded UP to 4 bytes. */
    const uint32_t img_bytes = want_raw ? (uint32_t)total
        : (uint32_t)(54 + ((SCREEN_WIDTH * 3 + 3) & ~3) * SCREEN_HEIGHT);

    char meta[128];
    snprintf(meta, sizeof meta,
             "seq=%u consistency=%s fmt=%s w=%u h=%u bytes=%u",
             (unsigned)seq, torn ? "torn" : "commit", want_raw ? "raw565" : "bmp",
             (unsigned)SCREEN_WIDTH, (unsigned)SCREEN_HEIGHT, (unsigned)img_bytes);
    httpd_resp_set_type(req, want_raw ? "application/octet-stream" : "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Orb-FB", meta);

    /* Chunked, never a Content-Length: the whole point is that we do not hold
     * a second 652 KB buffer to size one, and the host tool checks the length
     * against this header instead. */
    /* BMP insists each row start on a 4-byte boundary: 466*3 = 1398 pads to
     * 1400. The first version of this sent 1402 (sizeof the row scratch rather
     * than the padded row), which put every row 2 bytes out of true — a
     * progressive horizontal shear of one third of the frame height that a
     * thumbnail of a round dial does not visibly fail. The pad bytes are zeroed
     * per row, because s_row is static and would otherwise leak the previous
     * row's tail into the next one's padding. */
    const int row_bytes = (SCREEN_WIDTH * 3 + 3) & ~3;
    if (!want_raw) {
        uint8_t hdr[54] = {
            'B', 'M',                        /* 0  signature            */
            0, 0, 0, 0,                      /* 2  file size, patched   */
            0, 0, 0, 0,                      /* 6  reserved             */
            54, 0, 0, 0,                     /* 10 pixel data offset    */
            40, 0, 0, 0,                     /* 14 DIB header size      */
            SCREEN_WIDTH & 255, (SCREEN_WIDTH >> 8) & 255, 0, 0,   /* 18 width  */
            SCREEN_HEIGHT & 255, (SCREEN_HEIGHT >> 8) & 255, 0, 0, /* 22 height */
            1, 0,                            /* 26 planes               */
            24, 0,                           /* 28 bits per pixel       */
            0, 0, 0, 0,                      /* 30 BI_RGB               */
            0, 0, 0, 0,                      /* 34 image size           */
            0, 0, 0, 0,                      /* 38 x pixels per metre   */
            0, 0, 0, 0,                      /* 42 y pixels per metre   */
            0, 0, 0, 0,                      /* 46 colours used         */
            0, 0, 0, 0 };                    /* 50 important colours    */
        const uint32_t sz = img_bytes;
        hdr[2] = (uint8_t)sz;  hdr[3] = (uint8_t)(sz >> 8);
        hdr[4] = (uint8_t)(sz >> 16); hdr[5] = (uint8_t)(sz >> 24);
        if (httpd_resp_send_chunk(req, (const char *)hdr, 54) != ESP_OK) r = ESP_FAIL;
        /* Bottom-up, as BMP requires: row 0 of the file is the panel's last. */
        for (int y = SCREEN_HEIGHT - 1; y >= 0 && r == ESP_OK; y--) {
            uint8_t *o = s_row;
            const uint16_t *in = src + (size_t)y * SCREEN_WIDTH;
            for (int x = 0; x < SCREEN_WIDTH; x++, in++, o += 3) {
                const uint16_t p = __builtin_bswap16(*in);   /* wire -> native */
                const uint32_t r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F,
                               b5 = p & 0x1F;
                o[0] = (uint8_t)((b5 << 3) | (b5 >> 2));
                o[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
                o[2] = (uint8_t)((r5 << 3) | (r5 >> 2));
            }
            memset(o, 0, (size_t)row_bytes - (size_t)SCREEN_WIDTH * 3);
            if (httpd_resp_send_chunk(req, (const char *)s_row,
                                      row_bytes) != ESP_OK) r = ESP_FAIL;
        }
    } else {
        for (int y = 0; y < SCREEN_HEIGHT && r == ESP_OK; y++) {
            const uint16_t *in = src + (size_t)y * SCREEN_WIDTH;
            uint16_t *o = (uint16_t *)(void *)s_row;
            for (int x = 0; x < SCREEN_WIDTH; x++) o[x] = __builtin_bswap16(in[x]);
            if (httpd_resp_send_chunk(req, (const char *)s_row,
                                      SCREEN_WIDTH * 2) != ESP_OK) r = ESP_FAIL;
        }
    }
    if (r == ESP_OK && httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) r = ESP_FAIL;

    ESP_LOGI(TAG, "fb: served %u B %s seq=%u %s%s", (unsigned)img_bytes,
             want_raw ? "raw565" : "bmp", (unsigned)seq, torn ? "torn" : "commit",
             r == ESP_OK ? "" : " (client aborted)");
    s_busy = false;
    return r;
}
#endif /* CONFIG_ORB_DEBUG_HTTP */

/* Copy src into dst as JSON string contents. Every string on this page came from a
 * filename on somebody's card or from a browser, and a title containing a quote would
 * otherwise break the page's JSON.parse with nothing on the device to explain why. */
static void json_put(char *dst, size_t dsz, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < dsz; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\')                              { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c == '\n')                                     { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c == '\r')                                     { dst[o++] = '\\'; dst[o++] = 'r'; }
        else if (c == '\t')                                     { dst[o++] = '\\'; dst[o++] = 't'; }
        else if (c < 0x20)                                      { dst[o++] = '?'; }
        else                                                    { dst[o++] = (char)c; }
    }
    dst[o] = '\0';
}

#define THEME_LIST_MAX 12

/* GET /api/v1/sd — the card, the installed theme, and what is on the card. */
static esp_err_t h_sd(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    static orb_theme_entry_t ent[THEME_LIST_MAX];
    const int n = orb_theme_scan(ent, THEME_LIST_MAX);
    uint64_t total = 0, freeb = 0;
    orb_sd_space(&total, &freeb);

    /* The bound is the point, not the number: 12 rows whose owner name can each run
     * to ORB_THEME_NAME_MAX characters (JSON-escaped to twice that) plus a 115-char
     * why and the refused-install, damage, and staged sentences. Worst case
     * arithmetic, from the format strings below: ~102 B of fixed row text and digits
     * + 113 (name) + 12 (id) + 115 (why) + ~22 B of the P6.4 fmt/sects pair
     * + ~12 B of §26 slice 3's slot letter = ~374 B a row, ~4.4 kB for 12 of them, plus ~700 B of the original base fields, ~190 B
     * more for the staged pair at its full-width why and two 10-digit refill figures,
     * and ~50 B for the bundle pair at two 10-digit figures and ~15 B for the
     * job-busy witness = ~955 B. Measured against
     * the live device at 192.168.2.34: the same route answers 653 B today with four
     * short-named themes and every new field empty, and a truncated body would look
     * exactly like the empty list "no card" prints — which is why an overflow is an
     * error string, not a partial body. Same reason h_status carries its own bound. */
    static char json[6144];
    /* The last install refused, if any, travels with the list: the dial's row and
     * this page read the same two strings out of orb_theme, so a pick the firmware
     * said no to cannot come back as a silent no-op on the glass that pressed it
     * (ruling D11's "failure stays honest", and the C8 lesson that a swallowed press
     * is a press the owner has to be told about). Both name the theme by its id —
     * 12 hex, so the buffer only has to hold that. */
    char t_id[ORB_THEME_ID_HEX * 2 + 3], t_why[128];
    json_put(t_id, sizeof t_id, orb_theme_try_id());
    json_put(t_why, sizeof t_why, orb_theme_try_why());
    /* And the `staged` witness (Migration 1): a store whose pixels reached the slot and
     * whose header never did. Under the single slot that word is only a witness that a
     * store was armed and torn — boot draws the base face and one re-store clears it.
     * The damage-reporting pair is retired with the A/B choice: with one slot there is
     * no chosen-but-uncorruptable slot to explain. */
    char stg_id[ORB_THEME_ID_HEX * 2 + 3], stg_why[128];
    json_put(stg_id, sizeof stg_id, orb_theme_staged_id());
    json_put(stg_why, sizeof stg_why, orb_theme_staged_why());
    /* The installed slot's owner name, escaped for the base "active" field the page
     * shows beside the source. */
    char active_name[ORB_THEME_NAME_MAX * 2 + 3];
    json_put(active_name, sizeof active_name, orb_theme_active_name());
    int off = snprintf(json, sizeof json,
        "{\"mounted\":%s,\"card\":\"%.40s\",\"uploading\":%s,\"total_mb\":%u,\"free_mb\":%u,"
        "\"source\":\"%s\",\"active\":\"%s\",\"card_count\":%d,"
        "\"try_id\":\"%s\",\"try_why\":\"%s\","
        "\"staged_id\":\"%s\",\"staged_why\":\"%s\",\"refill_ms\":%u,\"refill_bytes\":%u,"
        "\"bundle_ms\":%u,\"bundle_bytes\":%u,\"job_busy\":%s,"
        "\"themes\":[",
        orb_sd_mounted() ? "true" : "false",
        orb_sd_mounted() ? orb_sd_card_id() : "",
        orb_theme_upload_busy() ? "true" : "false",
        (unsigned)(total / (1024 * 1024)), (unsigned)(freeb / (1024 * 1024)),
        orb_theme_source(), active_name, n > 0 ? n : 0,
        t_id, t_why, stg_id, stg_why,
        (unsigned)orb_theme_refill_ms(), (unsigned)orb_theme_refill_bytes(),
        (unsigned)orb_theme_bundle_ms(), (unsigned)orb_theme_bundle_bytes(),
        /* The press the owner just made is a queued job, not a done one: without this
         * witness a store in flight and a store that never started look the same from
         * the page, which is the silent button §24.5(2) bans. ~15 B. */
        orb_jobs_busy() ? "true" : "false");

    bool too_big = false;
    for (int i = 0; n > 0 && i < n; i++) {
        /* The bound is checked HERE, before the row is written. snprintf returns
         * the length it WOULD have written, so once off passes the buffer the next
         * `sizeof json - (size_t)off` wraps to a huge size_t and that row writes
         * past the end of the array — a check after the loop (the shape this code
         * shipped with, docs/theme-naming-todo.md §5) closes the truncation bug
         * and leaves the overrun open. An error body is the only honest answer,
         * because a partial list reads as "no card". */
        if (off < 0 || (size_t)off >= sizeof json) { too_big = true; break; }
        /* name is the owner's free-text label (escaped, up to NAME_MAX chars); id is
         * the 12-hex handle every later request comes back with. A damaged row keeps
         * its id (the filename the OS normalised) so it stays addressable, and puts
         * the reason in why. */
        char e_name[ORB_THEME_NAME_MAX * 2 + 3], e_id[ORB_THEME_ID_HEX * 2 + 3], e_why[116];
        json_put(e_name, sizeof e_name, ent[i].name);
        json_put(e_id,   sizeof e_id,   ent[i].id);
        json_put(e_why,  sizeof e_why,  ent[i].why);
        /* stored is whether the one slot holds this file's stored copy. Under the
         * single slot it is the same test as the row's id matching the slot's, and the
         * page marks the row "stored" and offers a re-store; the door takes the id,
         * never this flag. */
        off += snprintf(json + off, sizeof json - (size_t)off,
            "%s{\"name\":\"%s\",\"id\":\"%s\",\"why\":\"%s\",\"bytes\":%u,\"serial\":%u,"
            "\"ok\":%s,\"active\":%s,\"changed\":%s,\"fmt\":%u,\"sects\":%u,\"stored\":%s}",
            i ? "," : "", e_name, e_id, e_why,
            (unsigned)ent[i].art_size, (unsigned)ent[i].serial,
            ent[i].err == ESP_OK ? "true" : "false",
            ent[i].active ? "true" : "false",
            ent[i].changed ? "true" : "false",
            (unsigned)ent[i].fmt, (unsigned)ent[i].n_sections,
            ent[i].stored ? "true" : "false");
    }
    if (!too_big)
        off += snprintf(json + off, sizeof json - (size_t)off, "]}");
    if (too_big || off < 0 || (size_t)off >= sizeof json) {
        ESP_LOGE(TAG, "sd status overflow (%d B) — raise the json[] bound", off);
        return httpd_resp_sendstr(req, "{\"error\":\"sd status too large\"}");
    }
    return httpd_resp_sendstr(req, json);
}

/* POST /api/v1/sd/bundle?id=X — the theme store door. The same press shape the FORMAT 2
 * Load door had, through the same queue, and it is the ONLY route to
 * orb_theme_install_bundle(): the FORMAT 2 door above refuses a bundle by identity
 * check and the picker never sees one. One field differs from the FORMAT 2 door on
 * purpose — reboot:automatic. Under the single slot a store IS the theme change: it
 * writes the one slot and bundle_job asks for the reboot that draws it, exactly as a
 * FORMAT 2 install did. The outcome lands where every other press outcome lands: the
 * try witness on the row, and /api/v1/sd's bundle_ms/bundle_bytes pair beside the
 * refill figures. The confirm() on the page is the store's other duty: it says plainly
 * that a cut during this copy comes back on the base face, because the store erases the
 * slot that held the previous theme and only a re-store recovers it. */
static esp_err_t h_bundle_install(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char id[ORB_THEME_ID_HEX + 1] = "";
    if (!q_param(req, "id", id, sizeof id) || !orb_theme_id_ok(id)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"id is missing or not a theme id\"}");
    }
    const esp_err_t e = orb_theme_post_bundle(id);
    if (e == ESP_ERR_INVALID_STATE) {
        /* Same 409 discipline as the install route: a double press is a wait,
         * not a defect in the file. */
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"a theme job is already running\"}");
    }
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char b[96];
        snprintf(b, sizeof b, "{\"error\":\"job refused: %s\"}", esp_err_to_name(e));
        return httpd_resp_sendstr(req, b);
    }
    char b[192];
    const int n = snprintf(b, sizeof b,
        "{\"state\":\"storing\",\"id\":\"%s\",\"reboot\":\"automatic\"}", id);
    if (n < 0 || (size_t)n >= sizeof b) return httpd_resp_sendstr(req, "{\"error\":\"id too long\"}");
    return httpd_resp_sendstr(req, b);
}

/* POST /api/v1/sd/delete?id=X */
static esp_err_t h_theme_del(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char id[ORB_THEME_ID_HEX + 1] = "";
    if (!q_param(req, "id", id, sizeof id) || !orb_theme_id_ok(id)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"id is missing or not a theme id\"}");
    }
    const esp_err_t e = orb_theme_delete(id);
    if (e == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"error\":\"no such theme on the card\"}");
    }
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"delete failed\"}");
    }
    char b[160];
    const int n = snprintf(b, sizeof b, "{\"state\":\"deleted\",\"id\":\"%s\"}", id);
    if (n < 0 || (size_t)n >= sizeof b) return httpd_resp_sendstr(req, "{\"error\":\"id too long\"}");
    return httpd_resp_sendstr(req, b);
}

/* POST /api/v1/sd/rescan — the card-detect replacement. With no detect line wired,
 * this is the only way a card inserted after boot becomes visible without opening the
 * case or rebooting. */
static esp_err_t h_sd_rescan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t e = orb_sd_remount();
    char b[176];
    snprintf(b, sizeof b, "{\"mounted\":%s,\"card\":\"%.40s\"}",
             e == ESP_OK ? "true" : "false", e == ESP_OK ? orb_sd_card_id() : "");
    return httpd_resp_sendstr(req, b);
}

/* GET /api/v1/apps — the /sd/apps drop zone, listed with its reasons (P4).
 *
 * The shown-with-reason rule is h_sd's, inherited whole: every file the walk
 * surfaces gets a row, refused ones with the reader's own sentence in "why"
 * and not one word re-cut here. The owner's problem is different for "copied
 * nothing" and "copied four, all refused", and only the row can say which.
 *
 * The rows STREAM instead of assembling into one big buffer — a deliberate
 * departure from h_sd's single static json[6144]. The drop zone lists up to
 * ORB_APP_SCAN_MAX (16) candidates where themes list 12, and each row's
 * "why" escapes to 510 B at worst, so the single-buffer version would cost
 * ~9 KB of static internal DRAM on a device whose post-boot internal free
 * the plan measures in tens of KB (§15). Here each row builds in its own
 * scratch and a per-row overflow is arithmetic, not luck: every field's
 * escape bound is pinned by tools/host_orbapp_scan_test.py, which
 * recomputes row_worst() from the shipped headers and goes red when the
 * row outgrows its scratch. A format that cannot overflow needs no
 * all-or-nothing error body, because there is no partial to protect. */
#define APPS_ROW_MAX 1024

/* P4a's installed tier — streamed by h_apps_list on every response, card or
 * no card: what is ON the dial does not depend on what is in the socket. One
 * row per used table position (the span is positional — NULL answers are
 * holes, and a hole is a free slot, not the end of the list). The row carries
 * everything the page needs to draw its two buttons: identity for the POST,
 * state for the labels, and on_glass for the one refusal the page should warn
 * about before the device does. */
static void apps_send_installed(httpd_req_t *req, char *row, size_t rowsz)
{
    char e_slug[2 * ORB_APP_SLUG_MAX], e_title[2 * ORB_APP_TITLE_MAX], caps[40];
    int seen = 0;
    /* The compiled tier first (indices 0..ORB_APP_BASE-1). P4a streamed only
     * cards; a built-in that C2 quarantined at boot is exactly the row an owner
     * must see and clear, and it answers installed=false (init_all skipped it),
     * so the card loop below would hide it. A built-in is always on and never
     * togglable, so drawInst shows only a Clear button for a failed one, and the
     * reason rides along so the fault is named rather than a bare FAILED chip. */
    for (size_t i = 0; i < ORB_APP_BASE; i++) {
        const orb_app_t *a = orb_app_row(i);
        if (!a) continue;
        json_put(e_slug, sizeof e_slug, a->slug);
        json_put(e_title, sizeof e_title, a->title ? a->title : "");
        if (orbapp_caps_names(orb_svc_grants(a), caps, sizeof caps) < 0)
            caps[0] = '\0';
        char e_reason[2 * 40];
        json_put(e_reason, sizeof e_reason, orb_svc_fault_reason(a));
        const int w = snprintf(row, rowsz,
            "%s{\"slug\":\"%s\",\"title\":\"%s\",\"slot\":%d,\"caps\":\"%s\","
            "\"period_ms\":%" PRIu32 ",\"n_settings\":%d,"
            "\"enabled\":%s,\"failed\":%s,\"walkable\":%s,\"on_glass\":%s,"
            "\"compiled\":true,\"reason\":\"%s\"}",
            seen++ ? "," : "", e_slug, e_title, (int)i, caps,
            a->period_ms, a->n_settings,
            a->enabled ? "true" : "false", a->failed ? "true" : "false",
            a->iface ? "true" : "false",
            (a == orb_app_current() || a->active) ? "true" : "false", e_reason);
        if (w < 0 || (size_t)w >= rowsz) {
            ESP_LOGE(TAG, "compiled app row exceeded %d B", (int)rowsz);
            httpd_resp_send_chunk(req, "{}", HTTPD_RESP_USE_STRLEN);
            continue;
        }
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    }
    for (size_t i = ORB_APP_BASE; i < orb_app_rows(); i++) {
        const orb_app_t *a = orb_app_row(i);
        if (!a || !a->installed) continue;
        json_put(e_slug, sizeof e_slug, a->slug);
        json_put(e_title, sizeof e_title, a->title ? a->title : "");
        if (orbapp_caps_names(a->caps, caps, sizeof caps) < 0)
            caps[0] = '\0';                       /* same rule as the card tier */
        /* D35: a factory row is labeled, not privileged — the buttons below it
         * are the same two every installed row gets, and the label only tells
         * the owner where the bytes shipped from. */
        const bool fct = orb_app_install_is_factory(a->slug);
        const int w = snprintf(row, rowsz,
            "%s{\"slug\":\"%s\",\"title\":\"%s\",\"slot\":%d,\"caps\":\"%s\","
            "\"period_ms\":%" PRIu32 ",\"n_settings\":%d,"
            "\"enabled\":%s,\"failed\":%s,\"walkable\":%s,\"on_glass\":%s%s}",
            seen++ ? "," : "", e_slug, e_title, (int)(i - ORB_APP_BASE), caps,
            a->period_ms, a->n_settings,
            a->enabled ? "true" : "false", a->failed ? "true" : "false",
            a->iface ? "true" : "false",
            (a == orb_app_current() || a->active) ? "true" : "false",
            fct ? ",\"factory\":true" : "");
        if (w < 0 || (size_t)w >= rowsz) {        /* cannot: bounds are arithmetic;
                                                  * a placeholder keeps the body valid */
            ESP_LOGE(TAG, "installed app row exceeded %d B", (int)rowsz);
            httpd_resp_send_chunk(req, "{}", HTTPD_RESP_USE_STRLEN);
            continue;
        }
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    }
}

static esp_err_t h_apps_list(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* The rows array AND the row scratch live in PSRAM, not in .bss. This
     * file's own heap ladders log internal free in double digits by the time
     * httpd starts, and the task's 8 kB stack (cfg.stack_size, sized for the
     * FatFs chain) plus the card's AES scratch both draw from what is left:
     * the internal-static version of these buffers starved one or the other
     * on orb-A the day it shipped (ESP_ERR_HTTPD_TASK 2026-10-12 — the boot
     * window here is a two-way squeeze, and every static this page adds
     * bids against both). One PSRAM block carries the 16 rows and, appended
     * past them, the row scratch; the escapes are stack locals, the
     * h_sd convention for buffers under ~1 kB (its d_id/d_why sit on the
     * same 8 kB task this handler runs on). */
    static orbapp_scan_entry_t *ent;
    static char *row;                      /* tail bytes of the same block */
    if (!ent) {
        ent = heap_caps_malloc(ORB_APP_SCAN_MAX * sizeof *ent + APPS_ROW_MAX,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ent)
            row = (char *)(ent + ORB_APP_SCAN_MAX);
        if (!ent) {
            ESP_LOGE(TAG, "apps listing: no PSRAM for the row block");
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
        }
    }
    const int n = orb_app_scan(ent, ORB_APP_SCAN_MAX);
    /* Read AFTER the scan: the walk asks the volume to mount itself
     * (orb_files.c:185), so a card inserted after boot answers true here and
     * the listing needs no rescan press to know about it. */
    const bool mounted = orb_sd_mounted();
    /* D35: the Restore button's condition, read once per listing — the page
     * shows the door only when a factory row is actually absent from the
     * ledger, and the field says nothing else about the dial's behaviour. */
    const bool fmissing = orb_app_install_factory_missing();
    char closer[40];
    snprintf(closer, sizeof closer, "],\"factory_missing\":%s}", fmissing ? "true" : "false");
    char card[83], head[160];
    json_put(card, sizeof card, mounted ? orb_sd_card_id() : "");
    if (n < 0 && !mounted) {
        httpd_resp_send_chunk(req, "{\"mounted\":false,\"count\":-1,\"apps\":[],\"installed\":[",
                              HTTPD_RESP_USE_STRLEN);
        apps_send_installed(req, row, APPS_ROW_MAX);
        httpd_resp_send_chunk(req, closer, HTTPD_RESP_USE_STRLEN);
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    if (n < 0) {
        /* The card answered and the walk still refused: what it refused is
         * the apps DIRECTORY. An absent directory is not the same answer as
         * an empty one (orb_files.c:196), and this distinction is the whole
         * difference for the owner between "the folder is misnamed" and "the
         * files are there but all refused" — the second lists rows. */
        snprintf(head, sizeof head,
                 "{\"mounted\":true,\"card\":\"%s\",\"count\":-1,\"apps\":[],\"installed\":[",
                 card);
        httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
        apps_send_installed(req, row, APPS_ROW_MAX);
        httpd_resp_send_chunk(req, closer, HTTPD_RESP_USE_STRLEN);
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    snprintf(head, sizeof head,
             "{\"mounted\":true,\"card\":\"%s\",\"count\":%d,\"apps\":[",
             card, n);
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);

    char e_stem[2 * sizeof ent[0].stem], e_slug[2 * sizeof ent[0].slug],
         e_title[2 * sizeof ent[0].title], e_why[2 * sizeof ent[0].why];
    char caps[40];
    for (int i = 0; i < n; i++) {
        /* stem is the handle every later call comes back with — the same
         * device-issued-name rule as a theme id. The three escape buffers
         * stay filled but empty on a pre-CRC refusal (row_decide's blanking
         * rule), and the claimed app_id/caps ride along post-CRC so the
         * owner can name the file that lied about itself. */
        const orbapp_scan_entry_t *const e = &ent[i];
        json_put(e_stem,  sizeof e_stem,  e->stem);
        json_put(e_slug,  sizeof e_slug,  e->slug);
        json_put(e_title, sizeof e_title, e->title);
        json_put(e_why,   sizeof e_why,   e->why);
        if (orbapp_caps_names(e->caps, caps, sizeof caps) < 0)
            caps[0] = '\0';        /* unreachable post-check_header: it bounds
                                    * caps to the four owner bits, and their
                                    * names total 27 B. Never show a torn-off
                                    * disclosure line. */
        const int w = snprintf(row, APPS_ROW_MAX,
            "%s{\"stem\":\"%s\",\"slug\":\"%s\",\"title\":\"%s\",\"app_id\":\"0x%08" PRIx32 "\","
            "\"caps\":\"%s\",\"period_ms\":%" PRIu32 ",\"serial\":%" PRIu32 ","
            "\"n_settings\":%" PRIu32 ",\"art_size\":%" PRIu32 ",\"file_size\":%" PRIu32 ","
            "\"ok\":%s,\"why\":\"%s\"}",
            i ? "," : "", e_stem, e_slug, e_title, e->app_id, caps,
            e->period_ms, e->serial, e->n_settings, e->art_size, e->file_size,
            e->err == ESP_OK ? "true" : "false", e_why);
        if (w < 0 || (size_t)w >= APPS_ROW_MAX) {
            /* The bound check says it cannot; a placeholder keeps the body
             * valid JSON if someone disproves the arithmetic anyway, and the
             * log names the row so the sentinel's omission shows up red. */
            ESP_LOGE(TAG, "apps row %d exceeded %d B — host_orbapp_scan_test "
                          "row bound is stale", i, APPS_ROW_MAX);
            httpd_resp_send_chunk(req, "{}", HTTPD_RESP_USE_STRLEN);
            continue;
        }
        httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
    }
    /* Close the card tier, then the installed tier (P4a), then the object. */
    httpd_resp_send_chunk(req, "],\"installed\":[", HTTPD_RESP_USE_STRLEN);
    apps_send_installed(req, row, APPS_ROW_MAX);
    /* the chunked-encoding contract (esp_http_server.h:1129): finish by calling
     * the chunk API with length 0 — that is the terminating chunk. */
    httpd_resp_send_chunk(req, closer, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* POST /api/v1/apps/install?id=X — queue a card .orbapp onto the worker (P4).
 *
 * The browser names a container by the card stem the /api/v1/apps listing gave
 * it, never by a path it typed: orb_app_install resolves that stem by walking the
 * registered ("orb","apps") source and re-verifying the header under the card lock,
 * so a crafted id can only ever select a real .orbapp in the drop zone. The route
 * itself only validates the stem is a legal FATFS name (orb_sd_name_ok) and queues
 * the job; the multi-second card-to-flash copy is the worker's, exactly as a theme
 * install's is. A 202-style "installing" reply means the job was accepted, not that
 * it finished — the owner reloads /apps to see the row and any refusal it earned. */
static esp_err_t h_apps_install(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char stem[ORB_SD_NAME_MAX + 1] = "";
    if (!q_param(req, "id", stem, sizeof stem) || !orb_sd_name_ok(stem)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"id is missing or not a usable app name\"}");
    }
    const esp_err_t e = orb_app_install_post(stem);
    if (e == ESP_ERR_INVALID_STATE) {
        /* One job at a time (orb_jobs' gate): a second press while a copy runs is
         * 409, not 422 — the app may be perfectly installable and the answer is to
         * wait for the first to finish and reload. */
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"a card job is already running\"}");
    }
    if (e == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"the install queue is full\"}");
    }
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char b[96];
        snprintf(b, sizeof b, "{\"error\":\"job refused: %s\"}", esp_err_to_name(e));
        return httpd_resp_sendstr(req, b);
    }
    char b[192];
    const int n = snprintf(b, sizeof b, "{\"state\":\"installing\",\"stem\":\"%s\"}", stem);
    if (n < 0 || (size_t)n >= sizeof b) return httpd_resp_sendstr(req, "{\"error\":\"stem too long\"}");
    return httpd_resp_sendstr(req, b);
}

/* ---- P4a's two owner doors: /api/v1/apps/enable and /apps/uninstall ----------
 * Both are synchronous — the opposite of the install POST, and for the same
 * reason inverted: install is a multi-second card copy that must not sit on
 * this task, while these touch one NVS key or one 4 KB header sector, so the
 * handler can answer with the TRUTH of the attempt instead of a 202 the page
 * has to reload to discover. The id is the app's SLUG (the identity the
 * reader validated as FATFS-safe), never a path; orb_app_enable/uninstall
 * re-validate it and own every refusal sentence the body echoes. The slugs
 * and the reason texts here are device-authored (no quotes, no backslashes),
 * so the bodies need no json_put — same rule h_apps_install cites. */
static const char *door_http_status(esp_err_t e)
{
    switch (e) {
        case ESP_ERR_NOT_FOUND:       return "404 Not Found";
        case ESP_ERR_INVALID_ARG:     return "400 Bad Request";
        case ESP_ERR_NOT_SUPPORTED:   return "400 Bad Request";
        case ESP_ERR_INVALID_STATE:   return "409 Conflict";
        default:                      return "500 Internal Server Error";
    }
}

/* POST /api/v1/apps/enable?id=<slug>&on=0|1 — the owner's toggle (P4a). The
 * install confirm's "stays off until you turn it on" ends here: ON persists
 * the word before the RAM flip, so the choice survives whatever the dial
 * does next, and a failed app's ON doubles as the fault-record clear (C2's
 * owner door, its first caller). */
static esp_err_t h_app_enable(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char id[ORB_APP_SLUG_MAX + 1] = "", onv[4] = "";
    if (!q_param(req, "id", id, sizeof id)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"id is missing\"}");
    }
    if (!q_param(req, "on", onv, sizeof onv) ||
        (strcmp(onv, "0") != 0 && strcmp(onv, "1") != 0)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"on must be 0 or 1\"}");
    }
    char why[144] = "";
    const esp_err_t e = orb_app_enable(id, strcmp(onv, "1") == 0, why, sizeof why);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, door_http_status(e));
        char b[208];
        snprintf(b, sizeof b, "{\"error\":\"%s\"}", why);
        return httpd_resp_sendstr(req, b);
    }
    char b[224];
    snprintf(b, sizeof b, "{\"state\":\"%s\",\"slug\":\"%s\"}",
             strcmp(onv, "1") == 0 ? "enabled" : "disabled", id);
    return httpd_resp_sendstr(req, b);
}

/* POST /api/v1/apps/factory — D35's Restore door. Re-seeds the two containers
 * the image ships, through the same install ladder and the same enable ledger
 * as a card install; rows already on the dial keep their state, and an owner's
 * newer install of one of the two slugs is never walked back (serial rule).
 * Like the card install this queues the same worker job and answers 202: the
 * flash work is the worker's, and the /apps reload shows the rows. */
static esp_err_t h_app_factory(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t e = orb_app_install_restore_factory_post();
    if (e == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"a card job is running\"}");
    }
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"the restore could not be queued\"}");
    }
    return httpd_resp_sendstr(req, "{\"state\":\"restoring\"}");
}

/* POST /api/v1/apps/clear?id=<slug> — the fault-record door, the third owner
 * door (P4a's set). A built-in app that failed twice at boot cannot use the
 * enable door above to revive (it refuses non-card rows, and a quarantine also
 * leaves installed=false), and the only other way back was RESET ALL — which
 * wipes the owner's WiFi with it. This clears just the fault record; a reboot
 * rebuilds the app into the carousel. Card rows are accepted too, so an owner
 * can clear a failed card app without uninstalling it. */
static esp_err_t h_app_clear(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char id[ORB_APP_SLUG_MAX + 1] = "";
    if (!q_param(req, "id", id, sizeof id)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"id is missing\"}");
    }
    char why[144] = "";
    const esp_err_t e = orb_app_clear_faults(id, why, sizeof why);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, door_http_status(e));
        char b[208];
        snprintf(b, sizeof b, "{\"error\":\"%s\"}", why);
        return httpd_resp_sendstr(req, b);
    }
    char b[224];
    snprintf(b, sizeof b, "{\"state\":\"cleared\",\"slug\":\"%s\",\"reboot\":true}", id);
    return httpd_resp_sendstr(req, b);
}

/* POST /api/v1/apps/uninstall?id=<slug> — the B4 door (P4a). Erases the slot
 * header (D9: no header, no app — no reboot, the card file untouched), forgets
 * every owner word the row owned, and frees the table entry that keeps the
 * slot's place in the quota. The page confirms first; the device does not
 * trust the confirm — it just refuses anything on the glass. */
static esp_err_t h_app_uninstall(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char id[ORB_APP_SLUG_MAX + 1] = "";
    if (!q_param(req, "id", id, sizeof id)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"id is missing\"}");
    }
    char why[144] = "";
    const esp_err_t e = orb_app_uninstall(id, why, sizeof why);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, door_http_status(e));
        char b[208];
        snprintf(b, sizeof b, "{\"error\":\"%s\"}", why);
        return httpd_resp_sendstr(req, b);
    }
    char b[192];
    snprintf(b, sizeof b, "{\"state\":\"uninstalled\",\"slug\":\"%s\"}", id);
    return httpd_resp_sendstr(req, b);
}

/* POST /api/v1/sd/app[?overwrite=1], body = the .orbapp bytes.

 * The apps twin of h_theme_upload, and the missing first step of the card ladder:
 * the owner's card socket is unreachable from the laptop that baked the file, and
 * the /apps install button can only act on a container already in /sd/apps. This
 * streams the bytes in so they CAN get there without a physical card read.
 *
 * Raw body, mandatory Content-Length, one upload at a time, and NO name from the
 * client — the file lands under <slug>.orbapp, the slug its OWN header carries,
 * decided in orb_app_upload_end() once the bytes arrive (h_theme_upload's rules,
 * inherited whole). The device cannot tell an .orbapp from a random blob until the
 * last byte is in and the reader's ladder plus the geometry CRC have run off the
 * card, so nothing is named, filed, or listed until that has passed; a half-upload
 * is cancelled to .part and never reaches a loadable name. ?overwrite=1 is the only
 * client decision, and only governs replacing a DIFFERENT app already filed under
 * the same slug. */
static esp_err_t h_app_upload(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* Only the overwrite flag is read; the destination is the header's own slug. */
    char ow[4] = "";
    const bool overwrite = q_param(req, "overwrite", ow, sizeof ow) && strcmp(ow, "1") == 0;

    const int clen = (int)req->content_len;
    if (clen <= 0) {
        httpd_resp_set_status(req, "411 Length Required");
        return httpd_resp_sendstr(req, "{\"error\":\"Content-Length required\"}");
    }
    if (clen > (int)ORB_APP_UPLOAD_MAX) {
        /* Refused on the declared length before a byte reaches the card. */
        httpd_resp_set_status(req, "413 Payload Too Large");
        char b[112];
        snprintf(b, sizeof b, "{\"error\":\"too large\",\"max\":%u}", (unsigned)ORB_APP_UPLOAD_MAX);
        return httpd_resp_sendstr(req, b);
    }
    if (orb_app_upload_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"an upload is already in progress\"}");
    }

    orb_app_up_t *up = NULL;
    const esp_err_t be = orb_app_upload_begin(&up);
    if (be != ESP_OK) {
        /* no card (503), no room for the card read buffer (507), a card that would
         * not open the file (500) — the page has to say which one it hit. */
        httpd_resp_set_status(req, be == ESP_ERR_NOT_FOUND ? "503 Service Unavailable"
                                       : be == ESP_ERR_NO_MEM ? "507 Insufficient Storage"
                                                              : "500 Internal Server Error");
        return httpd_resp_sendstr(req, be == ESP_ERR_NOT_FOUND
            ? "{\"error\":\"no card in the socket\"}"
            : be == ESP_ERR_NO_MEM
                ? "{\"error\":\"out of memory for the card read buffer\"}"
                : "{\"error\":\"could not open the app file on the card\"}");
    }

    /* The receive buffer lives in PSRAM, not internal .bss. This file already
     * carries the theme upload's static rx[4096] and the OTA chunk; a SECOND
     * internal-static rx starved the httpd task's own stack on orb-A the day it
     * shipped (ESP_ERR_HTTPD_TASK — the h_apps_list comment records it, and it bit
     * again the moment this route added one). PSRAM has megabytes to spare, and the
     * worker copies straight from here to the card, so it needs no DMA-capable
     * memory. Allocated once, kept for the process's life (only one upload runs at a
     * time, so this one buffer is never contended). */
    static char *rx;
    if (!rx) {
        rx = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!rx) {
            orb_app_upload_cancel(&up);
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "{\"error\":\"out of memory for the upload buffer\"}");
        }
    }
    int got = 0;
    while (got < clen) {
        const int room = clen - got;
        const int r = httpd_req_recv(req, rx,
                                    (size_t)(room < (int)sizeof rx ? room : (int)sizeof rx));
        if (r <= 0) {
            /* Client vanished: cancel() deletes the .part, so nothing survives on the
             * card to be listed as an app and fail to load. */
            orb_app_upload_cancel(&up);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"upload aborted by client\"}");
        }
        const esp_err_t de = orb_app_upload_data(up, rx, (size_t)r);
        if (de != ESP_OK) {
            orb_app_upload_cancel(&up);
            if (de == ESP_ERR_INVALID_SIZE) {
                httpd_resp_set_status(req, "413 Payload Too Large");
                return httpd_resp_sendstr(req, "{\"error\":\"upload too large\"}");
            }
            if (de == ESP_ERR_INVALID_STATE) {
                httpd_resp_set_status(req, "409 Conflict");
                return httpd_resp_sendstr(req, "{\"error\":\"this upload is no longer current\"}");
            }
            /* The write FAILED — the card driver refused bytes, not the size gate.
             * §17.6 measured the exact shape: esp_dma_capable_malloc(125) refused on
             * a fragmented pool while the upload answered 413 with a live dial. A
             * size verdict was a lie that sent the next session down the wrong path
             * (plan §17.4 owed this mapping). 413 now means the body outgrew the
             * declared cap, and nothing else. */
            httpd_resp_set_status(req, de == ESP_ERR_NO_MEM ? "507 Insufficient Storage"
                                                            : "500 Internal Server Error");
            return httpd_resp_sendstr(req, de == ESP_ERR_NO_MEM
                ? "{\"error\":\"out of memory for the card write buffer\"}"
                : "{\"error\":\"the card refused the write\"}");
        }
        got += r;
    }

    const char *why = NULL;
    const esp_err_t ee = orb_app_upload_end(&up, overwrite, &why);
    if (ee == ORB_APP_UPLOAD_E_DUP) {
        /* A different app already lives under that slug. 409, not 422: nothing is
         * wrong with the upload — the owner decides, and the page re-sends with
         * ?overwrite=1 to keep the one they are pushing. */
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req,
            "{\"error\":\"a different app with that name is already on the card\","
            "\"overwrite\":false}");
    }
    if (ee != ESP_OK) {
        /* The reason goes in the body, not only the status line: whoever is at this
         * page has no serial cable and no reason to go and get one. */
        char b[256];
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        snprintf(b, sizeof b, "{\"error\":\"rejected\",\"detail\":\"%s\"}",
                 why ? why : "not a valid app file");
        return httpd_resp_sendstr(req, b);
    }
    char b[192];
    snprintf(b, sizeof b, "{\"state\":\"stored\",\"bytes\":%d}", got);
    return httpd_resp_sendstr(req, b);
}

/* POST /api/v1/sd/theme[?overwrite=1|?commit=TOK], body = the .orbtheme bytes.
 *
 * Raw body, not multipart: a boundary-following parser is ~200 lines of state machine
 * in a device that also renders a clock, and the page can set the request body to the
 * File object directly. Content-Length is mandatory for the reason h_ota gives for the
 * same rule — without it, a finished upload and a client that vanished are the same
 * request, and a partial file must never be reachable under a loadable name. The one
 * empty-body request is the ?commit= branch below, which is not an upload at all.
 *
 * No name comes from the client: the file lands under the id its OWN header name
 * hashes to, decided in orb_theme_upload_end() once the bytes have arrived. ?overwrite=1
 * is the only client-controlled body decision, and it only governs whether a verified
 * file replaces a different theme already filed under the same name. A duplicate
 * refusal now KEEPS the verified bytes staged and answers with a one-shot commit
 * token: the confirmed replace may then arrive as ?commit=<token> with no body at
 * all, and the staged copy is re-verified from the card and filed. The old re-upload
 * with ?overwrite=1 remains honored — every road ends with the same file on the card,
 * and no road re-sends bytes the card already holds. */
static esp_err_t h_theme_upload(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* The destination is not a client argument: an upload is filed by the name inside
     * its container, so there is no ?name= to validate here. Only the overwrite flag is
     * read, and it changes nothing unless a same-name/different-bytes file already sits
     * on the card. */
    char ow[4] = "";
    const bool overwrite = q_param(req, "overwrite", ow, sizeof ow) && strcmp(ow, "1") == 0;

    /* The confirmed-duplicate answer: an empty-body POST carrying ?commit=<token>
     * files the copy the last refusal left staged, instead of making the owner
     * watch a second full upload cross the room for bytes the card already holds.
     * The token came from this device's own refusal and recommit re-verifies the
     * staged file from the card before anything is renamed — the query parameter
     * is an identifier, never a path. */
    char commit[20] = "";
    if (q_param(req, "commit", commit, sizeof commit)) {
        const char *why = NULL;
        char committed[ORB_THEME_ID_HEX + 1] = "";
        uint32_t sz = 0;
        const esp_err_t ce = orb_theme_upload_recommit(commit, &why, committed, &sz);
        if (ce != ESP_OK) {
            /* The staged copy is gone, stale, or corrupt — an honest re-upload is
             * the only way forward, and the body says so. 410: what was offered
             * is no longer available, distinct from a fresh refusal's 409. */
            httpd_resp_set_status(req, "410 Gone");
            char b[224];
            snprintf(b, sizeof b, "{\"error\":\"commit refused\",\"detail\":\"%s\"}",
                     why ? why : "the staged copy is gone");
            return httpd_resp_sendstr(req, b);
        }
        char b[192];
        const esp_err_t pe = orb_theme_post_bundle(committed);
        if (pe != ESP_OK) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            snprintf(b, sizeof b, "{\"error\":\"stored on the card, store job refused: %s\","
                     "\"id\":\"%s\",\"reboot\":\"none\"}", esp_err_to_name(pe), committed);
            return httpd_resp_sendstr(req, b);
        }
        snprintf(b, sizeof b, "{\"state\":\"stored\",\"id\":\"%s\",\"bytes\":%lu,"
                 "\"reboot\":\"automatic\",\"committed\":\"staged\"}", committed, (unsigned long)sz);
        return httpd_resp_sendstr(req, b);
    }

    const int clen = (int)req->content_len;
    if (clen <= 0) {
        httpd_resp_set_status(req, "411 Length Required");
        return httpd_resp_sendstr(req, "{\"error\":\"Content-Length required\"}");
    }
    if (clen > (int)ORB_THEME_UPLOAD_MAX) {
        /* Refused on the declared length before a byte is written. An unbounded upload
         * route is a way to fill somebody's card with junk from a browser tab. */
        httpd_resp_set_status(req, "413 Payload Too Large");
        char b[112];
        snprintf(b, sizeof b, "{\"error\":\"too large\",\"max\":%u}", (unsigned)ORB_THEME_UPLOAD_MAX);
        return httpd_resp_sendstr(req, b);
    }
    if (orb_theme_upload_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"an upload is already in progress\"}");
    }

    orb_theme_up_t *up = NULL;
    const esp_err_t be = orb_theme_upload_begin(&up);
    if (be != ESP_OK) {
        /* Three distinct refusals, because the page has to say which one it hit: no
         * card, no 4 KB for the card read buffer (orb_theme_upload_begin() checks that
         * before the radio carries anything, so the owner can free the memory by
         * closing the other tab rather than by retrying blindly), and a card that
         * would not open the file. */
        httpd_resp_set_status(req, be == ESP_ERR_NOT_FOUND ? "503 Service Unavailable"
                                       : be == ESP_ERR_NO_MEM ? "507 Insufficient Storage"
                                                              : "500 Internal Server Error");
        return httpd_resp_sendstr(req, be == ESP_ERR_NOT_FOUND
            ? "{\"error\":\"no card in the socket\"}"
            : be == ESP_ERR_NO_MEM
                ? "{\"error\":\"out of memory for the card read buffer\"}"
                : "{\"error\":\"could not open the theme file on the card\"}");
    }

    /* The file is open and the slot is armed: from here to a queued store the dial
     * is promised to the overlay, so the copy names itself on the glass. A park
     * that loses to a raced resume says so once and the upload proceeds anyway —
     * the frames are courtesy, the bytes are the job. */
    const bool parked = orb_theme_upload_park((uint32_t)clen);

    /* Static, never a local: HTTPD_DEFAULT_CONFIG() sizes this task's stack at 4096
     * (esp_http_server.h:57), which is also why the OTA chunk buffer is static. */
    static char rx[4096];
    int got = 0;
    while (got < clen) {
        const int room = clen - got;
        const int r = httpd_req_recv(req, rx,
                                    (size_t)(room < (int)sizeof rx ? room : (int)sizeof rx));
        if (r <= 0) {
            /* Client vanished: cancel() deletes the .part, so nothing is left on the
             * card that could later be listed as a theme and fail to load. */
            orb_theme_upload_cancel(&up);
            if (parked) orb_theme_upload_unpark();
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"upload aborted by client\"}");
        }
        const esp_err_t de = orb_theme_upload_data(up, rx, (size_t)r);
        if (de != ESP_OK) {
            orb_theme_upload_cancel(&up);
            if (parked) orb_theme_upload_unpark();
            if (de == ESP_ERR_INVALID_SIZE) {
                httpd_resp_set_status(req, "413 Payload Too Large");
                return httpd_resp_sendstr(req, "{\"error\":\"upload too large\"}");
            }
            if (de == ESP_ERR_INVALID_STATE) {
                httpd_resp_set_status(req, "409 Conflict");
                return httpd_resp_sendstr(req, "{\"error\":\"this upload is no longer current\"}");
            }
            /* Same rule as the app upload above: a refused write is not a size
             * verdict. §17.6 reproduced this exact mis-map live — the dma_utils
             * refusal and a 413 body in the same second. */
            httpd_resp_set_status(req, de == ESP_ERR_NO_MEM ? "507 Insufficient Storage"
                                                            : "500 Internal Server Error");
            return httpd_resp_sendstr(req, de == ESP_ERR_NO_MEM
                ? "{\"error\":\"out of memory for the card write buffer\"}"
                : "{\"error\":\"the card refused the write\"}");
        }
        got += r;
        if (parked) orb_theme_upload_progress((uint32_t)got, (uint32_t)clen);
    }

    const char *why = NULL;
    char committed[ORB_THEME_ID_HEX + 1] = "";
    const esp_err_t ee = orb_theme_upload_end(&up, overwrite, &why, committed);
    if (ee == ORB_THEME_UPLOAD_E_DUP) {
        /* A different theme already lives under that name. 409, not 422: nothing is
         * wrong with the upload — the owner has to decide. The verified bytes STAY
         * staged on the card (upload_end's DUP branch), and the body hands back the
         * one-shot commit token that names them: the confirmed answer is an empty
         * POST to this door with ?commit=<token>, not a second full crossing. The
         * old contract — re-send the whole file with ?overwrite=1 — still works for
         * any client too old or too new to know about the token. The upload armed
         * the slot invalid before it ran, so this refusal leaves the dial base-faced
         * until a store lands — the commit, the re-send, or a plain re-store. */
        if (parked) orb_theme_upload_unpark();
        httpd_resp_set_status(req, "409 Conflict");
        char db[256];
        snprintf(db, sizeof db,
            "{\"error\":\"a different theme with that name is already on the card\","
            "\"overwrite\":false,\"reboot\":\"none\",\"commit\":\"%s\"}",
            orb_theme_staged_commit());
        return httpd_resp_sendstr(req, db);
    }
    if (ee != ESP_OK) {
        /* The reason goes in the body and not only the status line: whoever is at this
         * page has no serial cable and no reason to go and get one. The upload armed the
         * slot invalid before it ran, so a rejected upload leaves the base face — recover
         * by re-uploading or re-storing a good file. */
        if (parked) orb_theme_upload_unpark();
        char b[256];
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        snprintf(b, sizeof b, "{\"error\":\"rejected\",\"detail\":\"%s\",\"reboot\":\"none\"}",
                 why ? why : "not a valid theme file");
        return httpd_resp_sendstr(req, b);
    }
    /* The bytes are verified and on the card under `committed`. The upload armed the
     * single slot invalid, so the theme is not live until it is stored. Queue the store
     * door for that id: bundle_job copies card-to-slot, writes the header last, and
     * reboots onto it. This is the same press the Store button and the dial make, so the
     * upload door cannot hold a second opinion about what storing a theme does. */
    char b[192];
    const esp_err_t pe = orb_theme_post_bundle(committed);
    if (pe != ESP_OK) {
        /* On the card but not queued. The slot stays armed invalid; the page says so and
         * the owner re-presses Store. No store is coming, so the glass comes back too —
         * a parked dial with no job ahead of it is the frozen clock this frame set
         * exists to end, not a state to leave anybody in. */
        if (parked) orb_theme_upload_unpark();
        httpd_resp_set_status(req, "500 Internal Server Error");
        snprintf(b, sizeof b, "{\"error\":\"stored on the card, store job refused: %s\","
                 "\"id\":\"%s\",\"reboot\":\"none\"}", esp_err_to_name(pe), committed);
        return httpd_resp_sendstr(req, b);
    }
    snprintf(b, sizeof b, "{\"state\":\"stored\",\"id\":\"%s\",\"bytes\":%d,"
             "\"reboot\":\"automatic\"}", committed, got);
    return httpd_resp_sendstr(req, b);
}

static const char HTML_LANDING[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>orb setup</title><style>"
"body{font:16px system-ui;margin:0;padding:1.2rem;background:#111;color:#eee;max-width:32rem}"
"h1{font-size:1.2rem}#state{padding:.4rem .6rem;border-radius:.4rem;background:#222;margin:.5rem 0}"
"#state.ok{background:#14421f}#state.bad{background:#4a1414}"
"li{list-style:none;padding:.55rem .2rem;border-bottom:1px solid #2a2a2a}"
"button{background:#1d3f6e;color:#fff;border:0;border-radius:.35rem;padding:.5rem .7rem;font:inherit}"
"input{font:inherit;padding:.5rem;width:100%;box-sizing:border-box;background:#1b1b1b;color:#eee;border:1px solid #333;border-radius:.35rem}"
".row{display:flex;gap:.5rem;align-items:center;margin:.5rem 0}"
"select{font:inherit;padding:.5rem;background:#1b1b1b;color:#eee;border:1px solid #333;border-radius:.35rem;max-width:100%}"
"</style></head><body>"
"<h1>orb setup</h1>"
"<div id=state>loading&hellip;</div>"
"<div id=done hidden></div>"
"<div id=pick hidden><label>Network password</label>"
"<div class=row><input id=pass type=password placeholder='leave empty for open' autocomplete=off autocapitalize=none style='flex:1;min-width:0'><button id=eye>show</button></div>"
"<div class=row><button id=go>Connect</button><button id=rs>Rescan</button></div></div>"
"<ul id=ns></ul>"
"<script>"
"var sel=null,timer=0,mainTmr=0,done=false,apSent=false,fails=0,scanning=false;"
"function say(t,c){var e=document.getElementById('state');e.textContent=t;e.className=c||''}"
"async function j(u,o){var r=await fetch(u,o);if(!r.ok)throw new Error((await r.json()).error||r.status);return r.json()}"
"async function status(){if(done)return;try{var d=await j('/api/v1/status');fails=0;"
"if(d.state=='connected'){success(d);return}"
"if(scanning)return;say(d.state+(sel?' \\u2014 connecting to '+sel:'')"
"+(d.assoc_fails?' (attempt '+d.assoc_fails+(d.wifi_reason_name?', '+d.wifi_reason_name:'')+')':'')"
"+(d.wifi_reason&&!d.assoc_fails?' (last reason '+d.wifi_reason+')':''));}"
"catch(e){if(++fails<3){say('waiting for the orb \\u2026');return}say('device unreachable','bad')}}"
"var scanTmr=0;"
"function scanStop(){clearTimeout(scanTmr);scanTmr=0}"
"async function scan1(force){var ns=document.getElementById('ns');scanning=1;"
"say('scanning\\u2026');ns.innerHTML='';"
"try{var d=await j('/api/v1/wifi/scan'+(force?'?fresh=1':''));"
"if(d.status=='scanning'){scanStop();scanTmr=setTimeout(function(){scan1(false)},1200);return}"
"scanStop();scanning=0;var list=d.networks||[];"
"if(!list.length){say('no networks found \\u2014 try Rescan','bad');return}"
"list.forEach(function(n){var li=document.createElement('li');var b=document.createElement('button');"
"b.textContent=n.ssid+'  ('+n.rssi+' dBm, ch '+n.ch+(n.open?', open)':' )');"
"b.onclick=function(){sel=n.ssid;document.getElementById('pick').hidden=false;say('selected '+n.ssid);document.getElementById('pass').focus()};"
"li.appendChild(b);ns.appendChild(li)});say('pick your network from the list');}catch(e){scanStop();scanning=0;say('scan failed: '+e.message,'bad')}}"
"function scan(force){scanStop();scan1(!!force)}"
"async function join(){if(!sel){say('pick a network first','bad');return}"
"var p=document.getElementById('pass').value;"
"say('joining '+sel+'\\u2026');"
"try{await j('/api/v1/wifi/join',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:sel,pass:p})});}"
"catch(e){var m=''+((e&&e.message)||'');"
"/* Losing the reply is the normal outcome, not a failure: the orb drops"
"   this very connection to try the new credentials, so the answer is often"
"   lost on the way back. Only call it a rejection when an answer came"
"   back with a reason in it. */"
"if(/tz|ssid|body|content|large/i.test(m)){say('join rejected: '+m,'bad');return}"
"say('sent \\u2014 orb is reconnecting\\u2026')}"
"clearInterval(timer);timer=setInterval(status,1500);status()}"
/* The provisioning handoff. Deliberately NOT automatic: Android binds its
captive-portal webview to the AP it just joined, so the instant the SoftAP goes
away the webview is torn down with it -- closing the AP as soon as this page
rendered the confirmation yanked the page away before anyone could read the
orb's new address (first live phone test). The AP comes down only on a
deliberate tap here; CONFIG_ORB_NET_AP_HOLD_S from the lease stays the backstop
for a page nobody acknowledged. no-cors keeps this a simple request, because
the phone may already be back on the home network when the retry fires and the
absolute-IP fallback has to survive being cross-origin without a preflight the
orb does not answer. */
"var lanip='',tip=null,addr=null;"
"function apkill(){var b=JSON.stringify({on:'off'});"
"fetch('/api/v1/wifi/ap',{method:'POST',mode:'no-cors',body:b}).catch(function(){"
"if(lanip)fetch('http://'+lanip+'/api/v1/wifi/ap',{method:'POST',mode:'no-cors',body:b})});}"
"function tiptxt(t){if(tip)tip.textContent=t}"
"function ack(t){if(apSent)return;apSent=true;clearInterval(timer);clearInterval(mainTmr);"
"document.getElementById('pick').hidden=true;document.getElementById('ns').innerHTML='';"
"tiptxt(t);apkill()}"
/* Two copy paths, both needed: navigator.clipboard is undefined on an insecure
   origin, and the provisioning page IS http://192.168.4.1 -- the API a modern
   example would use cannot exist here. execCommand is the one that answers on
   a phone, and its boolean is what lets the AP close only on a copy we know
   landed, instead of taking the page down having done nothing. */
"function cp(u){var ok=false;"
"try{var ta=document.createElement('textarea');ta.value=u;ta.style.position='fixed';ta.style.left='-2000px';"
"document.body.appendChild(ta);ta.focus();ta.select();ta.setSelectionRange(0,u.length);"
"ok=document.execCommand('copy');document.body.removeChild(ta)}catch(e){ok=false}"
"if(!ok&&navigator.clipboard){try{navigator.clipboard.writeText(u);ok=true}catch(e){}}"
"if(ok){ack('copied - switch back to your wifi and paste it anywhere');return}"
"tiptxt('browser blocked the copy - select the address above and copy it yourself');"
"if(addr)addr.select()}"
"function success(d){if(done)return;done=true;lanip=d.ip;clearInterval(timer);clearInterval(mainTmr);"
"var u='http://'+d.ip+'/';say('orb is on your network','ok');"
"var e=document.getElementById('done');e.innerHTML='';e.hidden=false;"
"var p=document.createElement('div');"
"p.textContent='Setup is done. Switch your phone back to your own wifi, then use this address. '"
"+'The setup network closes when you tap one of these, or by itself shortly after.';"
"e.appendChild(p);"
"addr=document.createElement('input');addr.value=u;addr.readOnly=true;"
"addr.onclick=function(){addr.select()};e.appendChild(addr);addr.select();"
"var r=document.createElement('div');r.className='row';"
"var c=document.createElement('button');c.textContent='Copy address';"
"c.onclick=function(){cp(u)};"
"var f=document.createElement('button');f.textContent='Done - close setup network';"
"f.onclick=function(){ack('closing the setup network - you can close this page')};"
"r.appendChild(c);r.appendChild(f);e.appendChild(r);"
"var a=document.createElement('a');a.href=u;a.target='_blank';a.rel='noopener';a.style.color='#9cf';"
"a.textContent='Open the orb page in a new tab';"
"a.onclick=function(){ack('opening the orb page - you can close this one')};e.appendChild(a);"
"var k=document.createElement('div');k.style.marginTop='.35rem';"
"var k2=document.createElement('a');k2.href='http://'+d.ip+'/timezone';k2.target='_blank';k2.rel='noopener';"
"k2.style.color='#9cf';k2.textContent='orb clock: set your city';"
"k2.onclick=function(){ack('opening the clock page - you can close this one')};k.appendChild(k2);e.appendChild(k);"
"tip=document.createElement('div');tip.style.color='#9cf';tip.style.marginTop='.4rem';"
"e.appendChild(tip)}"
"document.getElementById('go').onclick=join;"
"document.getElementById('rs').onclick=function(){scan(true)};"
"document.getElementById('pass').onkeydown=function(e){if(e.key=='Enter')join()};"
"document.getElementById('eye').onclick=function(){var p=document.getElementById('pass');"
"var s=p.type=='password';p.type=s?'text':'password';this.textContent=s?'hide':'show';"
"p.focus();};"
"status();scan();mainTmr=setInterval(function(){if(!sel&&!done)status()},3000);"
"</script></body></html>";

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, HTML_LANDING, HTTPD_RESP_USE_STRLEN);
}

/* The clock page: /timezone, deliberately its own page and not a section of
 * provisioning (owner's call after the first successful phone test, with two
 * concrete complaints behind it: the dropdown listed IANA keys beside POSIX
 * rules no person reads, and the free-text "other" box expected someone to
 * type EST5EDT,M3.2.0,M11.1.0 correctly on a phone keyboard). Nothing here is
 * a technical decision any more: no <select> of keys, no POSIX ever, no
 * free-text at all. Cities are served by /api/v1/config (one source of truth
 * with the firmware that owns the rules) and rendered as
 * "GMT-06:00 - Chicago", the offset measured live by the browser — the one
 * place on this network with a full zoneinfo database. Tapping a city sets
 * it, one tap, no confirm button.
 *
 * On load the page reads the phone's own zone — Intl resolvedOptions, which
 * unlike the clipboard API is available on this insecure origin — and applies
 * it without being asked, then asks the only question that matters: is the
 * time on the orb's dial right? That question is the safety valve the config
 * endpoint's tz_offset echo exists for: a rule newlib silently degrades shows
 * up as a wrong GMT offset on screen, in words, immediately. If the phone's
 * zone is not in the (deliberately small) table, one honest extra row keeps
 * the phone's exact time as a fixed-offset rule and says out loud that it
 * will not follow DST — so the escape hatch is a tap, never typing rules. */
static const char HTML_TZ[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>orb clock</title><style>"
"body{font:16px system-ui;margin:0;padding:1.2rem;background:#111;color:#eee;max-width:32rem}"
"h1{font-size:1.2rem}"
"#msg{padding:.4rem .6rem;border-radius:.4rem;background:#222;margin:.5rem 0;position:sticky;top:0;z-index:3}"
"#msg.ok{background:#14421f}#msg.bad{background:#4a1414}"
"input{font:inherit;padding:.5rem;width:100%;box-sizing:border-box;background:#1b1b1b;color:#eee;border:1px solid #333;border-radius:.35rem;margin:.5rem 0}"
"button{background:#1d3f6e;color:#fff;border:0;border-radius:.35rem;padding:.55rem .7rem;font:inherit;width:100%;text-align:left;margin:.15rem 0;display:block}"
"button.cur{background:#14421f}"
"</style></head><body>"
"<h1>orb clock</h1>"
"<div style='margin-bottom:.6rem'><a href=/themes style='color:#7ad'>themes on the card</a> &nbsp; <a href=/apps style='color:#7ad'>apps on the card</a></div>"
"<div id=msg>loading&hellip;</div>"
"<input id=q placeholder='Type your city' autocomplete=off>"
"<div id=zs></div>"
"<script>"
"var zones=[],cur='',phone='',want='';"
"function msg(t,c){var e=document.getElementById('msg');e.textContent=t;e.className=c||''}"
"async function j(u,o){var r=await fetch(u,o);if(!r.ok)throw new Error((await r.json()).error||r.status);return r.json()}"
/* Minutes east of UTC that the browser's own tz database says `zn` is in at a
   given instant (DST included). The orb deliberately does not compute these:
   its newlib has no zoneinfo, only the rules it ships, while every phone that
   can open this page has the full IANA table. Measured via formatToParts
   because there is no other honest way to ask a browser for a zone's offset.
   Cached per zone+utc-day: synth() bisects hundreds of probes and building a
   fresh DateTimeFormat per probe is the slow part of this page. */
"var _oc={};"
"function offAt(zn,ms){var k=zn+'|'+Math.floor(ms/864e5),c=_oc[k];if(c!==undefined)return c;var o=null;"
"try{var a=new Intl.DateTimeFormat('en-US',{timeZone:zn,hourCycle:'h23',year:'numeric',month:'2-digit',"
"day:'2-digit',hour:'2-digit',minute:'2-digit',second:'2-digit'}).formatToParts(new Date(ms));"
"var p={};for(var i=0;i<a.length;i++)p[a[i].type]=a[i].value;"
"var h=parseInt(p.hour,10);if(h==24)h=0;"
"o=Math.round((Date.UTC(+p.year,p.month-1,+p.day,h,+p.minute,+p.second)-ms)/60000);}"
"catch(e){o=null}_oc[k]=o;return o}"
"function offMin(zn){return offAt(zn,Date.now())}"
"function hm(m){if(m==null||isNaN(m))return 'GMT?';var s=m<0?'-':'+';m=Math.abs(m);"
"return 'GMT'+s+('0'+Math.floor(m/60)).slice(-2)+':'+('0'+(m%60)).slice(-2)}"
"function zname(z){return z.iana||z.name}"
"function lbl(z){return hm(offMin(zname(z)))+' - '+(z.label||z.name)}"
"function phoneTz(){try{var t=Intl.DateTimeFormat().resolvedOptions().timeZone;return t||''}catch(e){return''}}"
/* Name first (exact against iana — Android says America/Chicago, never
   US/Central); current offset second, but only when it names exactly one
   zone: guessing between Denver and Phoenix is how clocks get quietly an
   hour wrong in November, and a wrong guess here is auto-APPLIED. */
"function matchPhone(){if(!phone)return null;var i;"
"for(i=0;i<zones.length;i++)if(zones[i].iana==phone||zones[i].name==phone)return zones[i];"
"var po=-(new Date()).getTimezoneOffset(),hit=null,n=0;"
"for(i=0;i<zones.length;i++){var o=offMin(zname(zones[i]));if(o!=null&&o==po){n++;hit=zones[i]}}"
"return n==1?hit:null}"
/* Rule synthesis for a zone the table does not carry — the answer to "what if
   the user is not one of the listed cities". The phone knows this zone's real
   clock changes; we ask it (bisecting each monthly window to the minute by
   offset — verified against an exhaustive scan over all 486 system IANA zones
   by tools/host_tz_test.py) and assemble the POSIX rule the orb will run:
   offsets counted west, rule1 into DST with /time in the pre-change local
   clock, last-weekday-of-month encoded as the .5 C libraries read as "last".
   The rule is cross-checked against the next two years too — a zone whose
   clock does not follow the sun (Jerusalem's Hebrew-calendar DST, Ramadan-
   suspended Casablanca) shifts week to week and fails, so it gets the honest
   fixed-offset row rather than a rule that would drift silently. Everything
   the phone can name, it can set; no one types a rule. Strings carry only
   letters/digits/:,.-+ — nothing a localized city name could smuggle trips
   the endpoint's printable-ASCII guard, and X/D are the last resort. */
"function foff(e){var w=-e,x=w<0?'-':'';w=Math.abs(w);"
"var h=Math.floor(w/60),r=w%60;return x+h+(r?':'+('0'+r).slice(-2):'')}"
"function transY(zn,y){var out=[];"
"for(var m=0;m<12;m++){var a0=Date.UTC(y,m,1)/6e4,a1=(m==11?Date.UTC(y+1,0,1):Date.UTC(y,m+1,1))/6e4;"
"var o0=offAt(zn,a0*6e4),o1=offAt(zn,a1*6e4);if(o0==null||o1==null)return null;if(o0==o1)continue;"
"var lo=a0,hi=a1;while(hi-lo>1){var mid=lo+((hi-lo)>>1);"
"if(offAt(zn,mid*6e4)==o0)lo=mid;else hi=mid}out.push([hi*6e4,o0,o1])}return out}"
"function mrule(t,pre){var d=new Date(t+pre*6e4);"
"var day=d.getUTCDate(),dim=(new Date(Date.UTC(d.getUTCFullYear(),d.getUTCMonth()+1,0))).getUTCDate();"
"var wd=d.getUTCDay(),wk=(day+7>dim)?5:Math.floor((day-1)/7)+1;"
"var hh=d.getUTCHours(),mm=d.getUTCMinutes(),ss=d.getUTCSeconds();"
"var tt=(hh==2&&mm==0&&ss==0)?'':(hh==0&&mm==0&&ss==0)?'/0':"
"'/'+hh+(mm||ss?':'+('0'+mm).slice(-2):'')+(ss?':'+('0'+ss).slice(-2):'');"
"return 'M'+(d.getUTCMonth()+1)+'.'+wk+'.'+wd+tt}"
"function sym(zn){var c=(String(zn).split('/').pop()||'').replace(/[^A-Za-z0-9]/g,'').toUpperCase();"
"return (c||'X').slice(0,5)}"
"/* The no-DST shape, kept for zones synthesis refuses: their clocks move more"
"   often than twice a year, so no static rule honestly describes them. */"
"function gmtRule(m){var s=m>=0?'-':'+';m=Math.abs(m);var h=Math.floor(m/60),r=m%60;"
"return 'GMT'+s+h+(r?':'+('0'+r).slice(-2):'')}"
/* null = not honestly expressable (changes more or fewer than twice a year,
   or the pattern moves between years). ok = the rule plus the phone's CURRENT
   offset, which becomes the echo-verify target at the device. */
"function synth(zn){var y=new Date().getUTCFullYear(),ts=transY(zn,y);"
"if(!ts||ts.length!==2)return null;"
"var a=ts[0],b=ts[1];"
"var std=Math.min(a[1],a[2],b[1],b[2]),dst=Math.max(a[1],a[2],b[1],b[2]);"
"if(std==dst)return null;"
"var td=(a[2]==dst)?a:b,tsd=(a[2]==dst)?b:a,nm=sym(zn);"
"var parts=[nm+foff(std)+nm+'D'+foff(dst),mrule(td[0],td[1]),mrule(tsd[0],tsd[1])];"
"for(var k=1;k<=2;k++){var o=transY(zn,y+k);if(!o||o.length!==2)return null;"
"var p0=o[0][2]>o[0][1]?[mrule(o[0][0],o[0][1]),mrule(o[1][0],o[1][1])]:[mrule(o[1][0],o[1][1]),mrule(o[0][0],o[0][1])];"
"if(p0[0]!=parts[1]||p0[1]!=parts[2])return null}"
"var r=parts[0]+','+parts[1]+','+parts[2];"
"if(r.length>64)return null;return r}"
"function mkrow(txt,on,cls){var b=document.createElement('button');b.textContent=txt;"
"if(cls)b.className=cls;b.onclick=on;return b}"
/* The response's tz_offset is read, never the echo of what was sent: the
   question this page answers is what the dial now shows, not what was asked.
   When `want` is given (anything chosen because it IS the phone's zone), the
   device's measured offset must agree with the phone's: a rule the orb's
   libc silently degraded reports itself as UTC, and lands here as 'did not
   stick' in words — instead of a wrong dial the owner notices hours later. */
"function setTz(v,txt,want){msg('setting '+txt+'...');"
"j('/api/v1/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tz:v})})"
".then(function(d){cur=d.tz;render();"
"if(want!=null&&d.tz_offset==want)msg('orb now shows '+hm(want)+' - matches your phone. If the dial is wrong, tap your city','ok');"
"else if(want!=null)msg('orb shows '+hm(d.tz_offset)+', not your '+hm(want)+' - that zone did not stick; tap your city','bad');"
"else msg('orb now shows '+hm(d.tz_offset)+' - is that the right time? If not, tap your city','ok')})"
".catch(function(e){msg('the orb refused it: '+e.message,'bad')})}"
"function render(){var box=document.getElementById('zs');box.innerHTML='';"
"var f=want.toLowerCase();"
"var list=zones.filter(function(z){return !f||(z.label||'').toLowerCase().indexOf(f)>=0"
"||z.name.toLowerCase().indexOf(f)>=0||(z.iana||'').toLowerCase().indexOf(f)>=0});"
"list.sort(function(a,b){var x=offMin(zname(a)),y=offMin(zname(b));if(x==null)x=-9999;if(y==null)y=-9999;"
"return x-y||String(a.label||a.name).localeCompare(String(b.label||b.name))});"
"list.forEach(function(z){box.appendChild(mkrow(lbl(z),function(){setTz(z.name,z.label||z.name)},z.name==cur?'cur':''))});"
"if(phone&&!matchPhone()){var po=-(new Date()).getTimezoneOffset(),city=sym(phone);"
"var r=synth(phone);"
"if(r)box.appendChild(mkrow(hm(po)+' - '+city+' (your phone, follows its clock changes)',"
"function(){setTz(r,city+' from your phone',po)},'phone'));"
"else box.appendChild(mkrow(hm(po)+' - Same as your phone (does not follow DST automatically)',"
"function(){setTz(gmtRule(po),'your phone time',po)},'phone'))}}"
"async function boot(){var d;try{d=await j('/api/v1/config')}catch(e){msg('could not reach the orb','bad');return}"
"zones=d.zones||[];cur=d.tz||'UTC';phone=phoneTz();render();"
"var m=matchPhone();if(!m){if(phone)msg('the orb does not know '+phone+' - tap the '+sym(phone)+' row at the end of the list, or search for a city');"
"else msg('tap your city');return}"
"if(m.name==cur){msg('the orb is already on your phone time ('+hm(offMin(zname(m)))+') - if the dial is wrong, tap your city','ok');return}"
"msg('your phone says '+(m.label||m.name)+' - setting the orb...');"
"setTz(m.name,m.label||m.name,-(new Date()).getTimezoneOffset())}"
"document.getElementById('q').oninput=function(){want=this.value;render()};"
"boot();"
"</script></body></html>";

static esp_err_t h_timezone(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, HTML_TZ, HTTPD_RESP_USE_STRLEN);
}

/* The themes page: what is on the card, and the three things a person does with
 * that list — put a file on it, load one, delete one.
 *
 * Three rules, all learned from these pages breaking on a real phone:
 *   - Every statement in the script ends with a semicolon. The C fragments below
 *     concatenate with NO newline between them (that is how the compiler builds the
 *     page), so anything relying on automatic semicolon insertion is a syntax error
 *     that only appears on a phone — and one error kills the whole <script>, which
 *     presents as a page stuck at "loading" with an empty list.
 *   - Nothing destructive happens without saying what it costs. Delete removes the
 *     FILE and states that the dial keeps drawing that theme until another is
 *     loaded, because otherwise delete looks like it did nothing.
 *   - The upload is the only long operation (3.7 MB at the ~1.5 MB/s this card can
 *     do), so it runs in an XHR with a real percentage: a fetch gives no upload
 *     progress, and a bar parked at 0% for four seconds reads as a hung device.
 */
static const char HTML_THEMES[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>orb themes</title><style>"
"body{font:16px system-ui;margin:0;padding:1.2rem;background:#111;color:#eee;max-width:32rem}"
"h1{font-size:1.2rem}"
"#msg{padding:.4rem .6rem;border-radius:.4rem;background:#222;margin:.5rem 0;position:sticky;top:0;z-index:3}"
"#msg.ok{background:#14421f}#msg.bad{background:#4a1414}"
"#card{color:#8a8;font-size:.9rem;margin:.2rem 0 1rem}"
"input{font:inherit;padding:.5rem;width:100%;box-sizing:border-box;background:#1b1b1b;color:#eee;border:1px solid #333;border-radius:.35rem;margin:.4rem 0}"
"button{background:#1d3f6e;color:#fff;border:0;border-radius:.35rem;padding:.55rem .7rem;font:inherit;margin:.15rem 0}"
"button.cur{background:#14421f}button.warn{background:#5a1d1d}"
".row{border-bottom:1px solid #262626;padding:.5rem 0}"
".nm{font-weight:600}.sub{color:#889;font-size:.85rem}"
"a{color:#7ad}"
"</style></head><body>"
"<h1>orb themes</h1>"
"<div id=msg>loading&hellip;</div>"
"<div id=card></div>"
"<div id=list></div>"
"<div class=row>"
"<input id=file type=file accept=.orbtheme>"
"<button onclick=doUp()>Upload to card</button>"
"</div>"
"<div class=row>"
"<button onclick=scan()>Rescan card</button> "
"<a href=/timezone>clock setup</a> &nbsp; <a href=/>network</a>"
"</div>"
"<script>"
"var data=null;"
/* Only the id travels into onclick markup: a name may legally hold a quote, and the id is [0-9a-f]{12} by construction. */
"var ROWS={};"
"function say(t,c){var m=document.getElementById('msg');m.textContent=t;m.className=c||'';}"
"function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}"
"function bytes(n){return n>1048576?(n/1048576).toFixed(2)+' MB':(n/1024).toFixed(0)+' KB';}"
"function j(u,o){return fetch(u,o).then(function(r){return r.json().then(function(d){return{ok:r.ok,st:r.status,d:d};});});}"
"function qa(s){return document.querySelectorAll(s);}"
"function draw(){"
"var L=document.getElementById('list'),C=document.getElementById('card');"
"if(!data){return;}"
"if(data.uploading){say('upload in progress','');return;}"
"if(!data.mounted){C.textContent='no card in the socket';L.innerHTML='';return;}"
"C.textContent=data.card+' \\u2014 '+data.free_mb+' MB free of '+data.total_mb+' MB; dial is on '+data.source+' ('+(data.active||'baked-in')+')';"
"var h='';"
"h=h+'<div class=row><b>dial: '+(data.source=='slot'?'the stored theme':'the base face')+(data.active?' ('+esc(data.active)+')':'')+'</b>';"
"h=h+'</div>';"
"if(!data.themes.length){h=h+'<div class=row>No themes on the card yet. Upload a .orbtheme below.</div>';}"
"if(data.bundle_ms){h=h+'<div class=row><span class=sub>last bundle store: '+bytes(data.bundle_bytes)+' in '+data.bundle_ms+' ms</span></div>';}"
"if(data.job_busy){h=h+'<div class=row><b>a theme job is running</b> <span class=sub>the copy is on the worker; the rows update when it lands</span></div>';}"
"for(var i=0;i<data.themes.length;i++){var t=data.themes[i];var nm=t.name||t.id;ROWS[t.id]=nm;"
"h=h+'<div class=row><div class=nm>'+esc(nm)"
"+' <span class=sub>app bundle of '+(t.sects||'?')+' apps</span>'+(t.active?' <span class=sub>on the dial</span>':(t.changed?' <span class=sub>card copy changed since store</span>':''))+'</div>';"
"h=h+'<div class=sub>'+esc(t.id)+(t.bytes?' \\u00b7 '+bytes(t.bytes):'')+(t.serial?' \\u00b7 #'+t.serial:'')+'</div>';"
"if(data.try_id==t.id&&data.try_why){h=h+'<div class=sub>LAST TRY FAILED: '+esc(data.try_why)+'</div>';}"
"if(data.staged_id==t.id&&data.staged_why){h=h+'<div class=sub>STORE NEVER COMMITTED: '+esc(data.staged_why)+'</div>';}"
"if(t.ok){h=h+'<button onclick=\"store(\\''+t.id+'\\')\">Store on the dial</button>';}"
"if(t.ok&&t.stored&&!t.active){h=h+' <span class=sub>stored, re-store to draw it</span>';}"
"if(!t.ok){h=h+'<div class=sub>CANNOT LOAD: '+esc(t.why||'the card copy did not verify')+'</div>';}"
"h=h+'<button class=warn onclick=\"del(\\''+t.id+'\\')\">Delete file</button></div>';}"
"L.innerHTML=h;}"


"function refresh(){j('/api/v1/sd').then(function(r){data=r.d;draw();}).catch(function(){say('orb not reachable','bad');});}"
"function store(id){var nm=ROWS[id]||id;"
"if(!confirm('Store '+nm+' on the dial? It overwrites the current theme and the orb reboots onto it. A power cut during the copy leaves the base face, and storing again recovers it.')){return;}"
"say('storing '+nm+' - the orb copies it into the slot and reboots onto it in a few seconds','');"
"j('/api/v1/sd/bundle?id='+encodeURIComponent(id),{method:'POST'}).then(function(r){"
"if(!r.ok){say('could not store '+nm+': '+((r.d&&r.d.error)?r.d.error:r.st),'bad');return;}"
"watch('store',id,nm,0,0);}).catch(function(){say('the store request did not reach the orb - nothing was copied','bad');});}"
/* The store door answers the moment the job is queued; the copy runs seconds later on
 * the worker, then the worker reboots the orb onto the theme it just wrote. So the press
 * is not over when the door answers: read the card until the job is quiet and the orb is
 * back on the new theme. A single slot leaves no second copy to nudge: storing once is the
 * whole action, and a torn copy leaves the base face to be re-stored. D11's failure-stays-
 * honest rule, applied to the button instead of the glass. */
"function watch(kind,id,nm,n,down){"
"j('/api/v1/sd').then(function(r){var d=r.d;"
"if(d&&d.job_busy){if(n>120){say('still storing '+nm+' after two minutes - reload this page for the outcome','bad');return;}"
"say('storing '+nm+' - copying, '+n+' s so far','');"
"setTimeout(function(){watch(kind,id,nm,n+1,down);},1000);return;}"
"data=d;draw();"
"if(d&&d.try_id==id&&d.try_why){say('could not store '+nm+': '+d.try_why,'bad');return;}"
"if(d&&d.staged_id==id&&d.staged_why){say('store of '+nm+' never committed: '+d.staged_why,'bad');return;}"
"if(d&&d.active==nm){say('the orb is back - dial on '+nm,'ok');return;}"
"if(down||n>6){say('the orb answered but is not on '+nm+' - reload this page and check the row','bad');return;}"
"setTimeout(function(){watch(kind,id,nm,n+1,down);},1500);},"
"function(){if(n>120){say('the orb stopped answering - reload this page','bad');return;}"
"setTimeout(function(){watch(kind,id,nm,n+1,1);},1500);});}"
"function del(id){var nm=ROWS[id]||id;if(!confirm('Delete '+nm+' from the card? The dial keeps drawing it until another theme is loaded.')){return;}"
"j('/api/v1/sd/delete?id='+encodeURIComponent(id),{method:'POST'}).then(function(r){"
"if(!r.ok){say('delete failed: '+((r.d&&r.d.error)?r.d.error:r.st),'bad');return;}refresh();});}"
"function scan(){say('rescanning card\\u2026','');j('/api/v1/sd/rescan',{method:'POST'}).then(function(){refresh();});}"
"function doUp(ow){var f=document.getElementById('file').files[0];"
"if(!f){say('choose a .orbtheme file first','bad');return;}"

"var x=new XMLHttpRequest();x.open('POST','/api/v1/sd/theme'+(ow?'?overwrite=1':''),true);x.timeout=900000;"
"x.upload.onprogress=function(e){if(e.lengthComputable){say('copying to the card '+Math.floor(e.loaded*100/e.total)+'% of '+bytes(e.total),'');}};"
"x.onload=function(){var d={};"
"try{d=JSON.parse(x.responseText);}catch(e){}"
"if(x.status==200){say('uploaded - it is on the card','ok');refresh();}"

"else if(x.status==409&&d.overwrite===false&&!ow){if(confirm('A different theme with that name is already on the card. Replace it?')){if(d.commit){doCommit(d.commit);}else{doUp(true);}return;}say('not replaced: the existing theme is kept','bad');}"
"else{say('rejected: '+(d.detail||d.error||x.status),'bad');}};"
"x.onerror=function(){say('upload failed \\u2014 is the card locked or full?','bad');};"
"x.ontimeout=function(){say('upload timed out','bad');};"
"say('copying to the card\\u2026','');x.send(f);}"
"function doCommit(tok){var x=new XMLHttpRequest();x.open('POST','/api/v1/sd/theme?commit='+encodeURIComponent(tok),true);x.timeout=300000;"
"x.onload=function(){var d={};try{d=JSON.parse(x.responseText);}catch(e){}"
"if(x.status==200){say('replaced from the copy already on the card - storing','ok');refresh();}"
"else if(x.status==410){say('the staged copy is gone - uploading the file again','');doUp(true);return;}"
"else{say('replace refused: '+(d.detail||d.error||x.status),'bad');}};"
"x.onerror=function(){say('replace failed \\u2014 is the card locked?','bad');};"
"x.ontimeout=function(){say('replace timed out','bad');};"
"say('replacing from the copy already on the card\\u2026','');x.send();}"
"refresh();"
"</script></body></html>";

static esp_err_t h_themes(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, HTML_THEMES, HTTPD_RESP_USE_STRLEN);
}

/* --------------------------------------------------------------------------
 * /apps — the drop-zone card listing (plan P4, first surface).
 *
 * This is the owner's view of /sd/apps: one card per container the scan
 * surfaces, refused ones kept and named, exactly as the themes page lists a
 * damaged .orbtheme with its reason. The disclosure line is the page's whole
 * point under ruling D19 (§0.8): the declared capability list printed here
 * is the consent the install carries, from the same bits the gate will
 * consult — orbapp_caps_names() is that one source, and this page never
 * re-rolls a capability list of its own.
 *
 * The Install button queues the verify-then-header ladder (D9) against the apps
 * partition through the worker — it is the same orb_app_install() the boot ladder
 * mirrors from the slot side, so the page and the dial cannot disagree about what
 * installing means. The button is behind a confirm(): an install writes flash and
 * the plan's rule is that nothing destructive runs without the owner costing it
 * first. An accepted card says what it is and offers the copy; a refused one says
 * what to fix on the card and offers nothing, and the row's refusal never gains a
 * button.
 *
 * The script follows the themes page's rules — every statement ends in a
 * semicolon, nothing destructive skips its costing sentence — even though
 * this slice has nothing destructive to cost. (host_tz_test.py's linters
 * parse the landing and clock pages; this one is reviewed against the same
 * two rules by hand, and its data shape is pinned by the scan sentinel.)
 * ------------------------------------------------------------------------ */
static const char HTML_APPS[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>orb apps</title><style>"
"body{font:16px system-ui;margin:0;padding:1.2rem;background:#111;color:#eee;max-width:32rem}"
"h1{font-size:1.2rem}"
"#msg{padding:.4rem .6rem;border-radius:.4rem;background:#222;margin:.5rem 0;position:sticky;top:0;z-index:3}"
"#msg.bad{background:#4a1414}"
"#card{color:#8a8;font-size:.9rem;margin:.2rem 0 1rem}"
"button{background:#1d3f6e;color:#fff;border:0;border-radius:.35rem;padding:.55rem .7rem;font:inherit;margin:.15rem 0}"
".row{border-bottom:1px solid #262626;padding:.5rem 0}"
".nm{font-weight:600}.sub{color:#889;font-size:.85rem}"
".no{color:#c66;font-size:.85rem}"
"a{color:#7ad}"
"</style></head><body>"
"<h1>apps on the card</h1>"
"<div id=msg>loading&hellip;</div>"
"<div id=card></div>"
"<div id=inst></div>"
"<div id=list></div>"
"<div class=row>Pick a <b>.orbapp</b> to copy onto the card, or drop files into an <b>apps</b> folder and reload. Nothing installs until you confirm it.</div>"
"<div class=row><input id=file type=file accept=.orbapp> <button onclick=doAppUp()>Upload to card</button></div>"
"<div class=row><a href=/themes>themes on the card</a> &nbsp; <a href=/timezone>clock setup</a> &nbsp; <a href=/>network</a></div>"
"<script>"
"var data=null;"
"function say(t,c){var m=document.getElementById('msg');m.textContent=t;m.className=c||'';}"
"function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}"
"function bytes(n){return n>1048576?(n/1048576).toFixed(2)+' MB':(n/1024).toFixed(0)+' KB';}"
"function j(u,o){return fetch(u,o).then(function(r){return r.json().then(function(d){return{ok:r.ok,st:r.status,d:d};});});}"
"function draw(){"
"var L=document.getElementById('list'),C=document.getElementById('card');"
"if(!data){return;}"
"drawInst();"
"if(!data.mounted){C.textContent='no card in the socket';L.innerHTML='';return;}"
"if(data.count<0){C.textContent=(data.card||'the card')+' \\u2014 the card has no apps folder';L.innerHTML='<div class=row>Copy .orbapp files into an <b>apps</b> folder in the card\\'s top-level directory, then reload.</div>';return;}"
"C.textContent=data.card+' \\u2014 '+data.count+' file'+(data.count==1?'':'s')+' in apps/';"
"if(!data.apps.length){L.innerHTML='<div class=row>No .orbapp files in apps/ yet.</div>';return;}"
"var h='';"
"for(var i=0;i<data.apps.length;i++){var a=data.apps[i];"
"h=h+'<div class=row><div class=nm>'+(a.title?esc(a.title):'(no title claimed)')+'</div>';"
"h=h+'<div class=sub>'+esc(a.slug)+(onDial(a.slug)?' \\u00b7 on the dial':'')+' \\u00b7 '+esc(a.app_id)+(a.serial?' \\u00b7 #'+a.serial:'')+(a.period_ms?' \\u00b7 every '+(a.period_ms/1000)+' s':' \\u00b7 no beat')+'</div>';"
"h=h+'<div class=sub>may use: '+(a.caps?esc(a.caps):'nothing beyond its own settings')"
"+(a.n_settings?' \\u00b7 '+a.n_settings+' setting'+(a.n_settings==1?'':'s'):'')"
"+(a.art_size?' \\u00b7 art '+bytes(a.art_size):'')+' \\u00b7 file '+bytes(a.file_size)+'</div>';"
"if(!a.ok){h=h+'<div class=no>CANNOT INSTALL: '+esc(a.why)+'</div>';}"
"else{h=h+'<div style=\"margin-top:.3rem\"><button onclick=\"install(\\''+esc(a.stem)+'\\',this)\">Install on the dial</button></div>';}"
"h=h+'</div>';}L.innerHTML=h;}"
"function install(stem,btn){if(!confirm('Install '+stem+' from the card onto the dial? The copy takes a few seconds and the app stays off until you turn it on.')){return;}if(btn){btn.disabled=true;}say('installing '+stem+' from the card...','');j('/api/v1/apps/install?id='+encodeURIComponent(stem),{method:'POST'}).then(function(r){if(!r.ok){say('orb refused the install: '+(r.d&&r.d.error?r.d.error:r.st),'bad');refresh();return;}say('install queued for '+stem+' — reload to see whether it landed','');refresh();}).catch(function(){say('orb not reachable','bad');if(btn){btn.disabled=false;}});}"
"function doAppUp(ow){var f=document.getElementById('file').files[0];if(!f){say('choose a .orbapp file first','bad');return;}var x=new XMLHttpRequest();x.open('POST','/api/v1/sd/app'+(ow?'?overwrite=1':''),true);x.timeout=900000;x.upload.onprogress=function(e){if(e.lengthComputable){say('copying to the card '+Math.floor(e.loaded*100/e.total)+'% of '+bytes(e.total),'');}};x.onload=function(){var d={};try{d=JSON.parse(x.responseText);}catch(e){}if(x.status==200){say('uploaded - the app is on the card','ok');refresh();}else if(x.status==409&&d.overwrite===false&&!ow){if(confirm('A different app with that name is already on the card. Replace it?')){doAppUp(true);return;}say('not replaced: the existing file is kept','bad');}else{say('rejected: '+(d.detail||d.error||x.status),'bad');}};x.onerror=function(){say('upload failed \\u2014 is the card locked or full?','bad');};x.ontimeout=function(){say('upload timed out','bad');};say('copying to the card\\u2026','');x.send(f);}"
"function onDial(s){var x=data.installed||[];for(var i=0;i<x.length;i++){if(x[i].slug==s)return true;}return false;}"
"function drawInst(){"
"var I=document.getElementById('inst'),x=data.installed||[];"
"if(!x.length){I.innerHTML='<div class=row><b>On the dial</b> \\u2014 nothing installed yet.</div>';return;}"
"var h='<div class=row><b>On the dial</b>'+(data.factory_missing?' <button onclick=\"facRes(this)\">Restore the factory apps</button>':'')+'</div>';"
"for(var i=0;i<x.length;i++){var a=x[i];"
"h=h+'<div class=row><div class=nm>'+(a.title?esc(a.title):esc(a.slug))+'</div>';"
"h=h+'<div class=sub>'+esc(a.slug)+' \\u00b7 '+(a.compiled?'shell':(a.factory?'factory, slot '+a.slot:'slot '+a.slot))+' \\u00b7 may use: '+(a.caps?esc(a.caps):'nothing')+(a.n_settings?' \\u00b7 '+a.n_settings+' settings':'')+(a.failed?' \\u00b7 FAILED':'')+(a.on_glass?' \\u00b7 on the glass':'')+'</div>';"
"h=h+'<div class=sub>'+(a.enabled?'on the carousel':'turned off')+(a.period_ms?' \\u00b7 every '+(a.period_ms/1000)+' s':'')+(a.walkable?'':' \\u00b7 no behaviour module for it')+'</div>';"
"h=h+'<div style=\"margin-top:.3rem\">';"
"h=h+(a.compiled?'':(a.enabled?'<button onclick=\"setOn(\\''+esc(a.slug)+'\\',0)\">Turn off</button> ':'<button onclick=\"setOn(\\''+esc(a.slug)+'\\',1)\">Turn on</button> '));"
"h=h+(a.failed?'<button onclick=\"clearFault(\\''+esc(a.slug)+'\\')\">Clear fault</button>':'')+(a.compiled?'':'<button onclick=\"unin(\\''+esc(a.slug)+'\\')\">Uninstall</button>')+(a.compiled&&!a.failed?'built into the firmware - always on':'')+'</div></div>';}"
"I.innerHTML=h;"
"}"
"function setOn(s,on){say((on?'turning on ':'turning off ')+s+'...','');j('/api/v1/apps/enable?id='+encodeURIComponent(s)+'&on='+on,{method:'POST'}).then(function(r){if(!r.ok){say('orb refused: '+(r.d&&r.d.error?r.d.error:r.st),'bad');refresh();return;}say(on?'turned on - it walks the dial now':'turned off - the slot keeps its place','ok');refresh();}).catch(function(){say('orb not reachable','bad');});}"
"function unin(s){var f=onDial(s)&&(data.installed||[]).some(function(a){return a.slug==s&&a.factory;});if(!confirm('Uninstall '+s+'? The dial erases its slot and forgets its permissions.'+(f?' This one ships inside the firmware: it stays gone until you press Restore the factory apps.':' The file stays on your card.'))){return;}say('uninstalling '+s+'...','');j('/api/v1/apps/uninstall?id='+encodeURIComponent(s),{method:'POST'}).then(function(r){if(!r.ok){say('orb refused: '+(r.d&&r.d.error?r.d.error:r.st),'bad');refresh();return;}say('uninstalled '+s+' - the slot is free again','ok');refresh();}).catch(function(){say('orb not reachable','bad');});}"
"function facRes(btn){if(!confirm('Restore the clock and HSB? Anything already installed keeps its state; anything missing comes back enabled.')){return;}if(btn){btn.disabled=true;}j('/api/v1/apps/factory',{method:'POST'}).then(function(r){if(!r.ok){say('orb refused: '+(r.d&&r.d.error?r.d.error:r.st),'bad');if(btn){btn.disabled=false;}return;}say('restore queued - reload to see the rows','');refresh();}).catch(function(){say('orb not reachable','bad');if(btn){btn.disabled=false;}});}"
"function clearFault(s){if(!confirm('Clear the fault record for '+s+'? A reboot brings it back; WiFi and the card are untouched.')){return;}say('clearing the fault for '+s+'...','');j('/api/v1/apps/clear?id='+encodeURIComponent(s),{method:'POST'}).then(function(r){if(!r.ok){say('orb refused: '+(r.d&&r.d.error?r.d.error:r.st),'bad');refresh();return;}say('fault cleared - reboot the dial to rebuild it','ok');refresh();}).catch(function(){say('orb not reachable','bad');});}"
"function refresh(){j('/api/v1/apps').then(function(r){if(!r.ok){say('orb refused the listing: '+(r.d&&r.d.error?r.d.error:r.st),'bad');return;}data=r.d;draw();}).catch(function(){say('orb not reachable','bad');});}"
"refresh();"
"</script></body></html>";

static esp_err_t h_apps(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, HTML_APPS, HTTPD_RESP_USE_STRLEN);
}


/* Captive-portal probes. A phone that joins an open AP with no internet
 * immediately fires these to decide whether the network is usable; if we do not
 * answer, it declares the AP dead and drops it, and the user's browser never
 * gets to the page. That failure looks identical to a broken config server, so
 * it is worth handling even though the PWA (Phase 4) will supersede the flow.
 * Redirect rather than return 204: the point is to land them on the page. */
static esp_err_t h_captive(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

/* --------------------------------------------------------------------------
 * Bring-up
 * ------------------------------------------------------------------------ */
/* Push a station profile and associate. Called on boot with the stored
 * credentials and again from POST /api/v1/wifi/join.
 *
 * This is the ONLY way WiFi config is written. esp_wifi with
 * CONFIG_ESP_WIFI_NVS_ENABLED=y caches the last profile it is given in the
 * `nvs.net80211` namespace, but that copy is treated as write-only and is never
 * read or written by us — the source of truth is `storage`:/orb.net. */
static esp_err_t net_apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid,     sizeof(wc.sta.ssid),     "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass ? pass : "");
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set_config(%s): %s", ssid, esp_err_to_name(e));
        return e;
    }
    /* "If station interface is connected to an AP, call esp_wifi_disconnect to
     * disconnect" (esp_wifi.h, esp_wifi_connect attention 2). Re-joining from the
     * config page while already associated is the common case — user fixes a
     * password without rebooting — so drop the old association first. Called
     * unconditionally because the driver rejects connect() while an association
     * is live, and disconnect() on a never-connected station is a harmless
     * ESP_ERR_WIFI_NOT_CONNECT whose return there is not worth branching on. */
    esp_wifi_disconnect();
    s_sta_up = false;
    e = esp_wifi_connect();
    if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "connect(%s): %s", ssid, esp_err_to_name(e));
        return e;
    }
    return ESP_OK;
}

static esp_err_t net_open_storage(void)
{
    /* Its own partition, so the default-NVS recovery erase in main.c cannot
     * reach these keys. See orb_net.h. */
    esp_err_t e = nvs_flash_init_partition(NET_NVS_PARTITION);
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "re-formatting the `%s` partition", NET_NVS_PARTITION);
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(NET_NVS_PARTITION), TAG, "erase");
        e = nvs_flash_init_partition(NET_NVS_PARTITION);
    }
    if (e != ESP_OK) return e;
    return nvs_open_from_partition(NET_NVS_PARTITION, NET_NVS_NAMESPACE,
                                   NVS_READWRITE, &s_nvs);
}

static esp_err_t net_start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = CONFIG_ORB_NET_HTTPD_PORT;
    /* 3, not the default 7: every open socket costs a lwIP pcb plus receive
     * buffers out of internal RAM, and a config page is driven by one browser
     * tab. lru_purge_enable reclaims a slot when a tab closes without FIN.
     * One slot is for the page, one for its fetches; the third exists because a
     * phone fires captive-portal probes on its own schedule, and at 2 those
     * probes could hold both slots and starve the page the user is waiting on.
     * CONFIG_LWIP_MAX_SOCKETS must stay >= max_open_sockets + 3 — httpd_main.c
     * asserts that and refuses to start otherwise. It is 10 here, so 3 is the
     * ceiling without touching lwIP. */
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    /* HTTPD_DEFAULT_CONFIG() ships max_uri_handlers at 8 (httpd_config.h:61), and
     * httpd_register_uri_handler() fails SILENTLY past that — this table now has
     * more than 8 entries, so the tail of it would 404 with no log line at all.
     * Measured before the fix: the 9th handler (/favicon.ico, registered today)
     * already answers 404 on the running device while the 8 before it 302. Raise
     * it with room for the endpoints this feature needs, and check the return
     * below rather than trusting the count. The /timezone page brings the table
     * to 15 registered routes; the /apps pair brought it to 25, the /apps install
     * POST is the 26th, the /api/v1/sd/app upload POST is the 27th, and P4a's
     * enable and uninstall doors make 29 registered, and the /api/v1/name door
     * makes 30 — one over the old ceiling of 29 that line records, still inside
     * the 32 that keeps the headroom the plan's route budget (P5's APPS-page mirror
     * and the theme pair) was priced against touching this line again. The
     * /api/v1/wifi/mode door is the 31st and the /api/v1/reprov door the 32nd, so
     * the ceiling moves to 36: still one quiet 404 away from a mystery, and a route
     * that fails to register logs the line above rather than vanishing. P6.4's
     * /api/v1/sd/bundle store door is the 33rd; the ceiling stays 36 and the count
     * is re-checked by host_tz_test.py's textual table count, not by this sentence.
     * Migration 1 then deleted the /api/v1/sd/flip and /api/v1/sd/default doors: the
     * single slot makes the bundle store the only theme-changing press and it reboots,
     * so the flip and factory-default routes no longer exist and the count drops by two.
     *
     * The merge with the app-platform line makes one of those 32 conditional:
     * /api/v1/screen registers only under CONFIG_ORB_DEBUG_HTTP, default y on the
     * bench and off for a build that leaves for a shelf. Count is therefore 34 with
     * the door on and 32 with it off, and host_tz_test.py counts the table textually,
     * so it sees 32 either way. Both fit 36; the guard never pushes the count up. */
    cfg.max_uri_handlers  = 36;
    /* 8192, not the 4096 HTTPD_DEFAULT_CONFIG() ships (esp_http_server.h:57). The
     * theme routes put a FATFS file operation on this task: f_open/f_write walk the
     * FAT with their own buffers, and the OTA chunk and upload receive buffers are
     * static precisely so they do not stack-allocate — but the FatFs call chain itself
     * still burns stack, and the old comment beside the OTA buffer claiming this task
     * was 8 KB was simply wrong about the default. Doubling it costs 4 KB of internal
     * RAM once, against a stack overflow on the first theme upload — which presents as
     * a device that reboots when someone presses Upload. */
    cfg.stack_size         = 8192;
    /* Below the LVGL task's priority 5: a slow or chatty client must never
     * preempt a paint and show up as a dropped frame on the dial. */
    cfg.task_priority    = 4;
    /* 30 s, not the 5 s HTTPD_DEFAULT_CONFIG() ships (esp_http_server.h:65).
     * Measured on the bench at -76 dBm with a 3.87 MB theme: the radio stalled
     * past 5 s twice, httpd_req_recv() returned EAGAIN, and h_theme_upload()
     * deleted a .part whose client was still sending and said "upload aborted
     * by client". A stalled link is not a vanished client, and the difference
     * is the whole upload path: the delete-then-refuse branch is correct for
     * the real thing and wrong for this. Bounded because both POST-body routes
     * refuse a declared length over their cap before reading a byte, only the
     * two big-body routes block in recv at all, and lru_purge_enable reclaims
     * the slot the moment a new connection needs one. */
    cfg.recv_wait_timeout  = 30;

    const esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s (check LWIP_MAX_SOCKETS >= max_open_sockets+3)",
                 esp_err_to_name(e));
        return e;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = h_index },
        { .uri = "/timezone",          .method = HTTP_GET,  .handler = h_timezone },
        { .uri = "/api/v1/status",   .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/v1/wifi/scan",.method = HTTP_GET,  .handler = h_wifi_scan },
        { .uri = "/api/v1/wifi/join",.method = HTTP_POST, .handler = h_wifi_join },
        { .uri = "/api/v1/wifi/ap",  .method = HTTP_POST, .handler = h_wifi_ap },
        /* The persistent reachability mode, and the page's only exit from an isolation
         * it can also be entered from. Reboots the orb, like the serial MODE verb. */
        { .uri = "/api/v1/wifi/mode", .method = HTTP_POST, .handler = h_wifi_mode },
        { .uri = "/api/v1/config",   .method = HTTP_GET,  .handler = h_config_get },
        { .uri = "/api/v1/config",   .method = HTTP_POST, .handler = h_config_post },
        { .uri = "/api/v1/name",     .method = HTTP_POST, .handler = h_name_post },
        { .uri = "/api/v1/ota",      .method = HTTP_POST, .handler = h_ota },
        { .uri = "/api/v1/reboot",   .method = HTTP_POST, .handler = h_reboot },
        /* One-shot: reboot into the provisioning branch with the stored credential
         * intact. Deliberately not folded into /api/v1/reboot — a caller has to be able
         * to say "I want this orb back in its AP" without wiping anything, and a route
         * that reboots a wall clock already needs the arm/disarm care plan 5.1 decision
         * 1 pins for /api/v1/menu. */
        { .uri = "/api/v1/reprov",  .method = HTTP_POST, .handler = h_reprov },
#if CONFIG_ORB_DEBUG_HTTP
        { .uri = "/api/v1/screen",     .method = HTTP_GET,  .handler = h_screen },
        { .uri = "/api/v1/fb",         .method = HTTP_GET,  .handler = h_fb },
#endif
        /* microSD + themes. /themes is the page; the routes under /api/v1/sd are its
         * backing calls. Migration 1 deleted the flip and default doors: under the
         * single slot the bundle store is the only theme-changing press, and it reboots. */
        { .uri = "/themes",            .method = HTTP_GET,  .handler = h_themes },
        /* The apps drop zone: /apps is the page, /api/v1/apps its backing
         * call. Neither can touch anything but a read of the card. */
        { .uri = "/apps",              .method = HTTP_GET,  .handler = h_apps },
        { .uri = "/api/v1/apps",       .method = HTTP_GET,  .handler = h_apps_list },
        { .uri = "/api/v1/apps/install",.method = HTTP_POST, .handler = h_apps_install },
        { .uri = "/api/v1/apps/enable", .method = HTTP_POST, .handler = h_app_enable },
        { .uri = "/api/v1/apps/uninstall", .method = HTTP_POST, .handler = h_app_uninstall },
        { .uri = "/api/v1/apps/clear", .method = HTTP_POST, .handler = h_app_clear },
        { .uri = "/api/v1/apps/factory", .method = HTTP_POST, .handler = h_app_factory },
        { .uri = "/api/v1/sd/app",      .method = HTTP_POST, .handler = h_app_upload },
        { .uri = "/api/v1/sd",          .method = HTTP_GET,  .handler = h_sd },
        { .uri = "/api/v1/sd/theme",    .method = HTTP_POST, .handler = h_theme_upload },
        { .uri = "/api/v1/sd/bundle",   .method = HTTP_POST, .handler = h_bundle_install },
        { .uri = "/api/v1/sd/delete",   .method = HTTP_POST, .handler = h_theme_del },
        { .uri = "/api/v1/sd/rescan",   .method = HTTP_POST, .handler = h_sd_rescan },
        /* Android + iOS/macOS captive-portal probes (see h_captive). The last two
         * are Firefox's network-check and the old-Chromium/Kindle endpoint: a phone
         * that finds them covered pops its own "Sign in to network" notification,
         * which is the whole point of the DNS responder above. */
        { .uri = "/generate_204",      .method = HTTP_GET, .handler = h_captive },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = h_captive },
        { .uri = "/favicon.ico",       .method = HTTP_GET, .handler = h_captive },
        { .uri = "/gen_204",           .method = HTTP_GET, .handler = h_captive },
        { .uri = "/connect/test.php",  .method = HTTP_GET, .handler = h_captive },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        /* Checked, not ignored: past max_uri_handlers this returns
         * ESP_ERR_NO_MEM and the endpoint simply 404s forever with nothing in the
         * log — which is how a nine-route server silently loses its ninth route. */
        const esp_err_t re = httpd_register_uri_handler(s_server, &uris[i]);
        if (re != ESP_OK)
            ESP_LOGE(TAG, "uri %s: %s (raise max_uri_handlers)", uris[i].uri, esp_err_to_name(re));
    }

    return ESP_OK;
}

esp_err_t orb_net_start(void)
{
    stage_probe("pre-netif");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif_init");
    /* The default loop may already exist if another module made it; that is not
     * an error for us. */
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create_default: %s", esp_err_to_name(loop_err));
        return loop_err;
    }

    ESP_RETURN_ON_ERROR(net_open_storage(), TAG, "open storage");

    /* Zone before radio: the dial's first frames compose while the association is
     * still in flight, so loading + applying TZ here (rather than in on_ip()) is
     * what makes the very first render local instead of UTC-for-two-seconds. It
     * only reads NVS — nothing here touches the network. */
    net_tz_load();
    /* Same moment and same reason as the zone: the card reads a name off
     * /api/v1/status the first time it folds this board, and a name that only
     * appeared after the association completed would show as blank during the
     * one window an operator is watching most closely — a board that just joined.
     * NVS read only, no radio. */
    net_name_load();
    /* Mode after tz and name and before the branch that reads it: like those two it
     * is an NVS-only read that decides a boot behaviour, and reading it here is what
     * lets the AP-isolated choice survive a reboot without a radio ever having run. */
    net_mode_load();

    stage_probe("pre-sntp_init");

    /* SNTP is configured here and started from on_ip() — see the block comment
     * above the callback. start=false keeps the client off the wire until there
     * is a lease; wait_for_sync=false because nothing here may block on the
     * network: orb_net_start() runs on the app path in front of the display, and
     * a dead NTP server must not be able to hold up the clock. smooth_sync=false
     * deliberately: this is a wall clock, so stepping to the right time at boot
     * is more honest than slewing, and the sweep smoothness comes from the
     * esp_timer fraction in compose_now(), not from the clock discipline. */
    const esp_sntp_config_t sntp_cfg = {
        .smooth_sync   = false,
        .server_from_dhcp = false,
        .wait_for_sync = false,
        .start         = false,
        .sync_cb       = sntp_sync_cb,
        .renew_servers_after_new_IP = false,
        .ip_event_to_renew = IP_EVENT_STA_GOT_IP,
        .index_of_first_server = 0,
        .num_of_servers = NET_SNTP_SERVERS,
        .servers = ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"),
    };
    const esp_err_t sne = esp_netif_sntp_init(&sntp_cfg);
    if (sne != ESP_OK) {
        /* Not fatal by design: the dial falls back to CLOCK_EPOCH + esp_timer
         * and stays a clock with the wrong time rather than no clock. */
        ESP_LOGE(TAG, "sntp_init: %s (clock will free-run)", esp_err_to_name(sne));
    }
    stage_probe("post-sntp_init");

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    const esp_err_t e = esp_wifi_init(&wic);
    if (e != ESP_OK) {
        /* The likeliest cause here is internal RAM, not a bad argument — this is
         * the first thing in the build that needs a few tens of KB of DMA-capable
         * memory. stage_probe above tells you what was actually available. */
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(e));
        return e;
    }
    stage_probe("post-wifi_init");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL),
                        TAG, "ip handler");
    /* Registered beside the STA's own lease handler rather than inside the AP
     * bring-up: the SoftAP opens from three places (provisioning boot, rescue,
     * manual), and the lease must be visible from whichever one a person at a
     * wall actually used. */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                   on_ap_lease, NULL),
                        TAG, "ap lease handler");

    char ssid[NET_SSID_MAX + 1] = "";
    char pass[NET_PASS_MAX + 1] = "";
    size_t sz;
    sz = sizeof(ssid); nvs_get_str(s_nvs, "ssid", ssid, &sz);
    sz = sizeof(pass); nvs_get_str(s_nvs, "pass", pass, &sz);

    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* D4's guard reads exactly like the branch it guards: "credentials exist"
     * means nothing more than "this boot would have taken the STA path". D2's RTC
     * intent flag forces the provisioning branch anyway, and is by definition a
     * recovery session with a network to go back to, so it gets the deadline. */
    const bool creds_in_flash = (ssid[0] != '\0');
    const bool ap_isolated = orb_net_ap_isolated();
#if CONFIG_ORB_NET_AP_FALLBACK
    const bool prov_forced = prov_intent_consume();
#else
    /* No SoftAP in this build, so there is nothing to force the branch onto. The
     * word is still scrubbed, and the request is reported as dropped rather than
     * silently ignored: an operator who pressed the knob should see why nothing
     * happened. */
    const bool prov_forced = false;
    if (prov_intent_consume())
        ESP_LOGE(TAG, "provisioning requested but ORB_NET_AP_FALLBACK is disabled; "
                      "booting into the stored network instead");
#endif
    /* An AP-isolated boot needs the SoftAP, which this build may have compiled out.
     * Refuse the mode loudly rather than boot a station-only orb that believes it is
     * reachable by its own AP: a mode the firmware cannot honour is worse than one it
     * rejects. The key stays set, so re-enabling the fallback brings the device back. */
#if !CONFIG_ORB_NET_AP_FALLBACK
    if (ap_isolated) {
        ESP_LOGE(TAG, "AP-isolated mode set but ORB_NET_AP_FALLBACK is disabled; "
                      "ignoring the mode and booting the stored network");
    }
    const bool take_ap = (!creds_in_flash || prov_forced);
#else
    const bool take_ap = (!creds_in_flash || prov_forced || ap_isolated);
#endif
    if (creds_in_flash && !take_ap) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    } else {
#if CONFIG_ORB_NET_AP_FALLBACK
        /* First boot / no credentials: come up as the provisioning AP. The
         * 466x466 panel is too small and glossy to render a scannable QR, so
         * pairing is by joining this AP from a phone instead. */
        s_provisioning = true;
        /* Latch the session kind here, once, where the decision is actually
         * derivable. The deadline itself is armed later in this function — the
         * timer handle does not exist yet at this point. A factory boot takes this
         * branch precisely because creds_in_flash is false, so it stays as it has
         * always been: AP open with no deadline, because there is nothing to fall
         * back to and rebooting would return here. */
        s_prov_recovery_session = creds_in_flash && !ap_isolated;
#if CONFIG_ORB_NET_MDNS
        /* Set before the AP comes up, so the very first announcement this boot
         * can make already says "provisioning" rather than making item 5's
         * browser show a factory-fresh orb as an ordinary one until the next
         * re-announce. */
        orb_mdns_set_provisioning(true);
#endif
        s_ap_netif     = esp_netif_create_default_wifi_ap();

        wifi_config_t ap;
        ap_fill_config(&ap);   /* same builder the runtime rescue/manual paths use,
                                * so the SSID scheme cannot drift between them */

        /* APSTA, not AP: a single radio cannot scan or associate while the STA
         * interface is disabled, and esp_wifi_connect() "only impact
         * WIFI_MODE_STA or WIFI_MODE_APSTA mode" (esp_wifi.h). Under AP-only the
         * provisioning page would have listed nothing AND silently accepted the
         * user's credentials without ever attempting to join them: set_config()
         * succeeds, then connect() is a no-op, so the orb looks configured and
         * never comes back. The AP still runs so the browser that just submitted
         * the credentials stays attached while the association proceeds. */
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
        if (ap_isolated)
            /* The owner asked for isolation: this AP has no deadline and no stored
             * network behind it, and the device will not associate this boot. */
            ESP_LOGW(TAG, "AP-isolated mode — orb reachable ONLY at \"http://192.168.4.1/\" "
                          "on its own AP \"%s\"; send MODE sta to rejoin Wi-Fi",
                     (const char *)ap.ap.ssid);
        else if (prov_forced)
            /* Deliberately not the factory wording: credentials ARE stored, and
             * saying otherwise would send an operator hunting for a pairing they
             * never lost. The request is good for this boot only. */
            ESP_LOGW(TAG, "provisioning requested — coming up as \"%s\" (open) for ONE "
                          "boot; the stored network is left untouched as the fallback",
                     (const char *)ap.ap.ssid);
        else
            ESP_LOGW(TAG, "no stored network — provisioning AP \"%s\" (open), "
                          "then open http://192.168.4.1/", (const char *)ap.ap.ssid);
#else
        ESP_LOGE(TAG, "no stored network and SoftAP fallback is disabled");
        return ESP_ERR_NOT_FOUND;
#endif
    }

    /* The radio-mutation task + its timers exist before esp_wifi_start(),
     * because the START event can arm the retry timer immediately. Priority 4
     * like httpd — under LVGL's 5, so neither a reconnect burst nor an AP
     * teardown can preempt a paint. 3 KB is enough for esp_wifi_set_mode(): it
     * waits on the driver, it does not compute. */
    const esp_timer_create_args_t retry_ta = { .callback = retry_timer_cb,
                                               .name = "netretry" };
    if (esp_timer_create(&retry_ta, &s_retry_timer) != ESP_OK)
        ESP_LOGE(TAG, "retry timer: creation failed — associations will not retry");
    const esp_timer_create_args_t ap_ta = { .callback = ap_timer_cb,
                                            .name = "apexpire" };
    if (esp_timer_create(&ap_ta, &s_ap_timer) != ESP_OK)
        ESP_LOGE(TAG, "ap expiry timer: creation failed — the SoftAP will stay up");
    const esp_timer_create_args_t watch_ta = { .callback = lease_watch_timer_cb,
                                               .name = "netlease" };
    if (esp_timer_create(&watch_ta, &s_watch_timer) != ESP_OK)
        ESP_LOGE(TAG, "lease watchdog timer: creation failed — a stalled association will not recover");
    s_net_q = xQueueCreate(8, sizeof(net_msg_t));
    if (!s_net_q || xTaskCreate(net_task_loop, "orb_net", 3072, NULL, 4,
                                &s_net_task) != pdPASS) {
        /* Not fatal: without the queue the association retry simply stops
         * firing, which the log says plainly. The boot-path join below is
         * unaffected, so a correctly-configured orb still comes up. */
        ESP_LOGE(TAG, "radio task/queue: out of memory — retries and AP teardown disabled");
        s_net_q = NULL;
    }
    stage_probe("post-nettask");

    /* D4: the bounded sojourn, armed HERE and not in the provisioning branch
     * above. That branch runs before esp_timer_create() gives s_ap_timer a handle
     * and before the queue the expiry posts to exists, so arming there produced
     * "prov sojourn timer does not exist" and the session ran with no deadline at
     * all — invisible to every truth table in tools/host_tz_test.py, which pin the
     * predicate and cannot see the call site. Bench 2026-10-13. */
    if (sojourn_arms(s_prov_recovery_session, CONFIG_ORB_NET_PROV_SOJOURN_S)) {
        arm_oneshot(s_ap_timer, CONFIG_ORB_NET_PROV_SOJOURN_S, "prov sojourn");
        ESP_LOGW(TAG, "RECOVERY provisioning: credentials are stored but this boot "
                      "was forced into provisioning, so the AP gives up in %d s and "
                      "the stored network is retried",
                 CONFIG_ORB_NET_PROV_SOJOURN_S);
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");
    stage_probe("post-wifi_start");

    /* Radio stays awake: no modem sleep. The default WIFI_PS_MIN_MODEM parks the
     * receiver between beacon intervals, which measured ~200 ms round-trip on
     * this device (curl 0.171 s, ping 192-239 ms) — a config page that stutters
     * on every tap, and a scan that looks hung. The orb is wall-powered, so the
     * power that buys back is not a currency we are spending; the only cost is a
     * little heat inside the enclosure. Set once for the STA interface after
     * start so it covers both the connected and the APSTA provisioning paths.
     * Revisit (WIFI_PS_MIN_MODEM) if this ever ships on a battery. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps");

    if (!s_provisioning && !net_post_join(ssid, pass)) {
        /* Queue missing (bring-up OOM): apply here so a correctly-configured
         * orb still reaches its network on first boot. */
        const esp_err_t je = net_apply_sta_config(ssid, pass);
        if (je != ESP_OK) ESP_LOGW(TAG, "initial join failed: %s (will retry)",
                                   esp_err_to_name(je));
    }

    ESP_RETURN_ON_ERROR(net_start_httpd(), TAG, "httpd");
    stage_probe("post-httpd_start");

    ESP_LOGI(TAG, "config server listening on port %d", CONFIG_ORB_NET_HTTPD_PORT);

    /* The serial command door is open for the whole life of the app, on every boot and
     * in every state — not only a first-boot provisioning. A configured orb is exactly
     * the case that needs it: a hub with only a USB lead can still NAME it, SCAN the
     * air around it, switch it AP<->station, or factory-reset it, none of which reach it
     * over a LAN it may not be on. The reader blocks in select() until a line arrives, so
     * an orb whose console nobody touches pays nothing. Low priority, modest stack. */
    if (xTaskCreate(prov_reader_task, "orbserial", 4096, NULL, 3, NULL) != pdPASS)
        ESP_LOGE(TAG, "serial reader: out of memory — the serial command door is disabled");
    else
        ESP_LOGI(TAG, "serial door armed: PROV|NAME|SCAN|MODE|RESET|PING on the console");

    /* One line the hub greps to classify this board over the wire without a
     * reboot-probe. It is the same token PING returns, so a hub that missed boot can ask
     * for it later. State (a), a bare ESP32-S3, prints no such line because no gorb-os
     * app is running to print it — the hub infers (a) from a ROM that answers and a boot
     * banner that never comes. */
    ESP_LOGI(TAG, "GorbOS-STATE %s", orb_state_token());

    /* The same argument for the name. A board on a lead with no lease has no door the
     * hub can dial, and the serial command door only writes — so the name it holds was
     * unreadable from the cable, and a hub that never saw it online kept a blank card
     * for a board somebody had already named. The boot console is the one channel that
     * always runs. Printed only when named: an unnamed board prints nothing, which the
     * hub reads as no news rather than as a cleared name. */
    if (s_name[0] != '\0') {
        ESP_LOGI(TAG, "GorbOS-NAME %s", s_name);
    }
    return ESP_OK;
}

bool orb_net_is_up(void)
{
    return s_sta_up || s_provisioning;
}

bool orb_net_is_provisioning(void)
{
    return s_provisioning;
}

/* D2: request a provisioning boot. Sets the RTC intent word and returns true;
 * returns false and changes nothing when this build has no SoftAP to fall back
 * on, so a caller can report the refusal instead of rebooting into a dead end.
 *
 * It deliberately does NOT reboot. The caller owns the timing: a knob press must
 * let the panel show something first, and an HTTP caller must get its response
 * out before the connection dies — see reboot_timer_cb(), which defers for the
 * same reason. Nothing is written to flash: the whole point of D2 is that a
 * request which loses power before it boots disappears with the power. */
bool orb_net_request_provisioning(void)
{
#if CONFIG_ORB_NET_AP_FALLBACK
    s_prov_intent = PROV_INTENT_MAGIC;
    return true;
#else
    return false;
#endif
}

const char *orb_net_ip_str(void)
{
    return s_ip;
}

bool orb_net_time_synced(void)
{
    return s_time_synced;
}

#else /* !CONFIG_ORB_NET_ENABLE */

esp_err_t orb_net_start(void)
{
    ESP_LOGI(TAG, "networking disabled (CONFIG_ORB_NET_ENABLE=n); clock only");
    return ESP_OK;
}

bool orb_net_is_up(void) { return false; }

bool orb_net_is_provisioning(void) { return false; }

bool orb_net_ap_isolated(void) { return false; }   /* no SoftAP in a disabled build */

/* D2's setter, disabled build: there is no SoftAP to force, so this refuses the way
 * the enabled build refuses when ORB_NET_AP_FALLBACK=n. Without the stub any caller
 * fails to link the moment ORB_NET_ENABLE flips off, which is the one configuration
 * this file's own guards are written to survive. */
bool orb_net_request_provisioning(void) { return false; }

const char *orb_net_ip_str(void) { return ""; }

bool orb_net_time_synced(void) { return false; }

/* Zone accessors, disabled build: the menu must still compile and must not offer
 * a picker over an empty table, so count 0 is the honest answer rather than a
 * fake UTC row the dial would let you "select" into nowhere. */
int orb_net_tz_count(void) { return 0; }
const char *orb_net_tz_key(int i) { (void)i; return ""; }
const char *orb_net_tz_label(int i) { (void)i; return ""; }
const char *orb_net_tz_current(void) { return ""; }
int orb_net_tz_current_index(void) { return -1; }
esp_err_t orb_net_tz_select(int i) { (void)i; return ESP_ERR_NOT_SUPPORTED; }
/* No zone machinery in a disabled build: UTC is the only honest answer, and
 * ESP_OK keeps the broker's tz_offset entry a plain pass-through. */
esp_err_t orb_net_tz_offset(time_t when, int32_t *offset_min)
{
    (void)when;
    if (!offset_min) return ESP_ERR_INVALID_ARG;
    *offset_min = 0;
    return ESP_OK;
}

/* Name accessors, disabled build: "" is the honest answer. The limit is a literal
 * 32 because NET_NAME_MAX is defined inside the enabled half of this file, so it
 * is not visible here — the one place in this feature where the two builds state
 * the same number twice. device_contract_test.go pins it against orbfleet.MaxName
 * and against the string in the enabled half, which is what keeps the duplication
 * from drifting. */
const char *orb_net_name(void) { return ""; }
int orb_net_name_max(void) { return 32; }
esp_err_t orb_net_name_set(const char *name) { return name ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_ARG; }

#endif /* CONFIG_ORB_NET_ENABLE */
