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
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

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
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "orb_net.h"
#include "art_bank.h"

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
#define NET_STA_RETRY_MAX_S 120 /* ceiling — see retry_delay_s() */
#define NET_SSID_MAX     32
#define NET_PASS_MAX     64
#define NET_TZ_MAX       64     /* a POSIX TZ rule is short by construction */
#define NET_TZ_KEY       "tz"   /* NVS: a table name OR a raw POSIX string */
#define NET_CONFIG_BODY_MAX 128 /* POST /api/v1/config is {"tz":"…"} and nothing else */

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
       NET_CMD_AP_EXPIRED };  /* close it, unless the STA still has no lease  */
static QueueHandle_t      s_net_q;
static TaskHandle_t       s_net_task;
static esp_timer_handle_t s_retry_timer;   /* one-shot, re-armed per failure */
static esp_timer_handle_t s_ap_timer;      /* one-shot, AP expiry            */

static esp_netif_t   *s_sta_netif;
static esp_netif_t   *s_ap_netif;
static httpd_handle_t s_server;
static nvs_handle_t   s_nvs;

static volatile bool s_sta_up;         /* has an IP */
static volatile bool s_provisioning;   /* booted with no credentials at all */
static volatile bool s_ap_up;          /* SoftAP is actually broadcasting */
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

/* (The numeric-argument helper lands with PATCH /api/v1/settings in Phase 4;
 * carrying it unused would only trip -Wunused-function.) */

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
typedef struct { const char *name; const char *posix; } tz_entry_t;

/* Names are IANA-style so the same strings still work as keys if esp_timezones
 * ever lands. The rules are the post-2007 POSIX forms — M3.2.0 = second Sunday
 * of March, the /n suffix naming the weekday the transition lands on. US/Arizona
 * is its own row precisely BECAUSE it is MST7 with no DST component: handing it
 * "MST7MDT,…" is the classic way to put a no-DST state an hour off for half the
 * year, and the only thing that ever catches that is someone glancing at the dial. */
static const tz_entry_t s_tz_tbl[] = {
    { "UTC",            "UTC0"                       },
    { "US/Eastern",     "EST5EDT,M3.2.0,M11.1.0"     },
    { "US/Central",     "CST6CDT,M3.2.0,M11.1.0"     },
    { "US/Mountain",    "MST7MDT,M3.2.0,M11.1.0"     },
    { "US/Pacific",     "PST8PDT,M3.2.0,M11.1.0"     },
    { "US/Arizona",     "MST7"                       },
    { "Europe/London",  "GMT0BST,M3.5.0/1,M10.5.0"   },
    { "Europe/Berlin",  "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Asia/Tokyo",     "JST-9"                      },
};
#define NET_TZ_N ((int)(sizeof(s_tz_tbl) / sizeof(s_tz_tbl[0])))

/* The setting exactly as NVS holds it ("" = never set = UTC). Kept in RAM
 * because /api/v1/status is polled at a few Hz by the config page and must not
 * touch flash to say what zone it is in. Written only from orb_net_start() and
 * the serial httpd task, so no lock is needed. */
static char s_tz[NET_TZ_MAX + 1];

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

/* Timer callbacks may only post: the esp_timer task is shared and must not
 * block, and the esp_wifi_* calls do block on the driver's own task. */
static void retry_timer_cb(void *arg) { (void)arg; net_post(NET_CMD_CONNECT, 0); }
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
    wifi_config_t ap;
    ap_fill_config(&ap);

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

static void net_task_loop(void *arg)
{
    (void)arg;
    net_msg_t m;
    for (;;) {
        if (xQueueReceive(s_net_q, &m, portMAX_DELAY) != pdTRUE) continue;
        switch (m.cmd) {
        case NET_CMD_CONNECT:
            /* Re-check what the event loop owns: a lease may have landed
             * between the timer firing and this task getting to run. */
            if (s_sta_up || s_provisioning) break;
            s_retry_pending = false;          /* this attempt is now live */
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
             * breaks out on s_sta_up, and a connect() during a live attempt
             * returns ESP_ERR_WIFI_CONN and is ignored. */
            arm_oneshot(s_retry_timer, NET_STA_RETRY_S, "retry");
            break;

        case NET_CMD_AP_OPEN:
            ap_open((uint32_t)m.arg, "requested from the config page");
            break;

        case NET_CMD_AP_RESCUE:
            ap_open((uint32_t)m.arg, "credentials not working");
            break;

        case NET_CMD_AP_HOLD:
            if (s_ap_up) arm_oneshot(s_ap_timer, (uint32_t)m.arg, "ap expiry");
            break;

        case NET_CMD_AP_EXPIRED:
            /* The invariant that decides this whole feature: the AP closes
             * because the device BECAME reachable, not because a timer ran
             * out. With no lease the AP is the only door a phone can walk up
             * to, so it stays open and the check merely re-arms. */
            if (s_sta_up) ap_close("lease held, hold expired");
            else if (!s_provisioning) {
                arm_oneshot(s_ap_timer, NET_AP_RECHECK_S, "ap recheck");
                ESP_LOGI(TAG, "SoftAP stays up: no lease yet, checking again in %d s",
                         NET_AP_RECHECK_S);
            }
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

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ed = data;
        const uint8_t reason = ed ? ed->reason : 0;
        s_sta_up = false;
        s_rssi   = -999;
        if (reason) s_last_reason = reason;
        if (s_provisioning) break;         /* AP-only mode: nothing to retry */
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
        break;

    case WIFI_EVENT_AP_STOP:
        /* The driver's word that the AP is really gone, which is what makes
         * ap_close()'s early return honest. */
        s_ap_up = false;
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

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_sta_up       = true;
    s_provisioning = false;
    s_assoc_fails  = 0;      /* back-off is per-outage, not per-device-life */
    ESP_LOGI(TAG, "associated: config page at http://%s/", s_ip);
    stage_probe("sta-connected");

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
    /* 640, not the 512 this started with: the body has since grown a tz string
     * (up to NET_TZ_MAX) plus the ap/assoc_fails pair, and a truncated body is
     * worse than an error because the page just fails to parse it. The overflow
     * check below is what makes that visible rather than subtle, and the host
     * test pins the worst case. */
    char json[640];

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

    const int n = snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"fw\":\"%s\",\"idf\":\"%s\","
        "\"heap_internal\":%u,\"heap_internal_largest\":%u,\"heap_psram\":%u,"
        "\"art_ready\":%s,\"art_bytes\":%u,\"slot\":\"%s\",\"img\":\"%s\","
        "\"synced\":%s,\"epoch\":%lld,\"tz\":\"%s\",\"tz_offset\":%d,\"ap\":%s,"
        "\"assoc_fails\":%u,\"wifi_reason\":%u}",
        s_provisioning ? "provisioning" : (s_sta_up ? "connected" : "down"),
        s_ip, rssi, app->version, app->idf_ver,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        art_bank_ready() ? "true" : "false", (unsigned)art_bank_bytes(),
        (slot ? slot->label : "?"), img_str,
        s_time_synced ? "true" : "false", (long long)now,
        s_tz[0] ? s_tz : "UTC", tz_offset_min(now),
        /* The AP's real state, from the driver's own events. Without this the
         * whole lifecycle added here is invisible from the LAN — you could not
         * tell a closed AP from a firmware that never closes it, which is
         * exactly the question that motivated the change. assoc_fails rides
         * along because "how long until the rescue AP" is the next question. */
        s_ap_up ? "true" : "false", (unsigned)s_assoc_fails,
        (unsigned)s_last_reason);

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

/* GET /api/v1/config — the zone table is SERVED, not hardcoded in the page. The
 * inline page and the future PWA both render their picker from this reply, which
 * is the only way the list of zones stays in step with the firmware that owns the
 * rules. `posix` is the resolved rule in effect, so the page can show what a name
 * actually means and can tell a table entry from a hand-typed string. */
static esp_err_t h_config_get(httpd_req_t *req)
{
    /* static, not stack (same reasoning as s_scan_json): the nine entries are
     * ~510 B — 9 × (22 B of key/quote/colon + a ≤20 B name + a ≤24 B rule) — and
     * the echo tail can carry a 64-char custom rule, so ~710 B worst case. On
     * httpd's CONFIG_HTTPD_TASK_STACK_SIZE stack that is a real overflow risk;
     * handlers run serially, so one instance is safe. */
    static char json[768];

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
        const int w = snprintf(json + off, sizeof(json) - (size_t)off,
                               "%s{\"name\":\"%s\",\"posix\":\"%s\"}",
                               i ? "," : "", s_tz_tbl[i].name, s_tz_tbl[i].posix);
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

/* Scan is blocking on purpose: the config page shows a spinner. A non-blocking
 * scan would need a result cached across the WIFI_EVENT_SCAN_DONE event, which
 * is more state than a bring-up build should carry. Capped at 12 networks both
 * to bound the reply and because that is all a 466x466 screen can list. */
#define SCAN_MAX_NETWORKS 12
#define SCAN_FETCH_MAX    40   /* records drained looking for distinct SSIDs */

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

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    static wifi_ap_record_t rec;                    /* ~200 B, kept off the stack */
    s_scan_n = 0;
    const int n0 = snprintf(s_scan_json, sizeof(s_scan_json), "{\"networks\":[");
    if (n0 < 0 || (size_t)n0 >= sizeof(s_scan_json)) goto fail;
    size_t off = (size_t)n0;

    const wifi_scan_config_t sc = { .show_hidden = false };
    const esp_err_t se = esp_wifi_scan_start(&sc, true);
    if (se == ESP_ERR_WIFI_STATE) {
        /* A connect() is in flight; the driver aborts scanning in that window.
         * Say so — the page can retry, where "scan failed" is a dead end. */
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"busy\", \"detail\":\"association in progress\"}");
    }
    if (se != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(se));
        goto fail;
    }

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
    return httpd_resp_sendstr(req, s_scan_json);

fail:
    /* Always reach a valid body: a truncated JSON reply is indistinguishable
     * from a crash from the page's point of view. */
    esp_wifi_clear_ap_list();
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, "{\"error\":\"scan failed\"}");
}

#define JOIN_BODY_MAX 256

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

    ESP_LOGI(TAG, "join request: ssid=\"%s\" pass=%s%s%s", ssid, pass[0] ? "(set)" : "(none)",
             tz_present ? " tz=" : "", tz_present ? tz : "");

    /* Persist FIRST, then apply: if the association fails, the credentials are
     * already on the device so the next boot retries them instead of dropping
     * the user back to an unprovisioned state they have to redo. */
    nvs_set_str(s_nvs, "ssid", ssid);
    if (pass[0]) nvs_set_str(s_nvs, "pass", pass); else nvs_erase_key(s_nvs, "pass");
    nvs_commit(s_nvs);

    /* Zone goes through net_tz_set() rather than a local nvs_set_str(), so the
     * write, the setenv/tzset and the log line are literally the same code the
     * config endpoint runs — one behaviour, two doors. Cannot fail on validation
     * here (checked above, before the credentials were touched); the only way it
     * returns a reason is NVS, and that is worth a log, not a lost join. */
    if (tz_present) {
        const char *why = net_tz_set(tz);
        if (why) ESP_LOGW(TAG, "join: tz \"%s\" not applied (%s)", tz, why);
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

    /* Nothing left can fail in a way the caller needs told about: the
     * credentials are already committed, and the radio outcome is not known yet —
     * it is what /api/v1/status reports as state/assoc_fails/wifi_reason. If the
     * queue never came up (bring-up OOM), do the radio work on this task rather
     * than not do it at all. */
    if (!net_post_join(ssid, pass)) {
        ESP_LOGW(TAG, "join: no radio queue — applying from the httpd task");
        const esp_err_t e = net_apply_sta_config(ssid, pass);
        if (e != ESP_OK) ESP_LOGE(TAG, "join(%s) failed: %s", ssid, esp_err_to_name(e));
    }
    return sent;
}

/* POST /api/v1/wifi/ap — summon the provisioning AP on an orb that is happily
 * on the LAN, so it can be re-provisioned from a phone at the wall without a
 * reboot, a cable, or an NVS erase. Its own endpoint (rather than a flag on
 * /api/v1/config) because it is a radio action with its own lifetime, and the
 * AP state is reported by /api/v1/status so the caller can watch it land.
 *
 * Body is optional: {} opens for CONFIG_ORB_NET_AP_MANUAL_S seconds;
 * {"on":"off"} closes it immediately. The open path goes through the radio task
 * exactly like the rescue path — one code path, one set of log lines — and both
 * arm the AP's expiry, which is why no caller can leave it open by accident. */
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

#define OTA_CHUNK 4096   /* static buffer, not stack: httpd's task stack is 8 KB */

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
 * address to type next has to be on screen before that happens. */
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
"#tzs{color:#9cf;font-size:.9rem;margin:.3rem 0 1rem}"
"</style></head><body>"
"<h1>orb setup</h1>"
"<div id=state>loading&hellip;</div>"
"<div id=pick hidden><label>Network password <input id=pass type=password placeholder='leave empty for open'></label>"
"<div class=row><button id=go>Connect</button><button id=rs>Rescan</button></div></div>"
"<ul id=ns></ul>"
"<h1>Timezone</h1>"
"<div class=row><select id=tz></select></div>"
"<div class=row><input id=tzx placeholder='POSIX rule, e.g. EST5EDT,M3.2.0,M11.1.0' hidden></div>"
"<div class=row><button id=tzgo hidden>Set zone</button></div>"
"<div class=row><button id=apbtn>Show setup AP</button><span id=apinfo></span></div>"
"<div id=tzs></div>"
"<script>"
"var sel=null,timer=0,zones=[],tzcur='',appin=0;"
"function say(t,c){var e=document.getElementById('state');e.textContent=t;e.className=c||''}"
"async function j(u,o){var r=await fetch(u,o);if(!r.ok)throw new Error((await r.json()).error||r.status);return r.json()}"
"async function status(){try{var d=await j('/api/v1/status');aprep(d);"
"if(d.state=='connected'){say('connected \\u2014 orb is now at http://'+d.ip+'/','ok');return}"
"say(d.state+(sel?' \\u2014 connecting to '+sel:'')"
"+(d.assoc_fails?' (attempt '+d.assoc_fails+(d.wifi_reason?', reason '+d.wifi_reason:'')+')':''))"
"+(d.wifi_reason&&!d.assoc_fails?' (last reason '+d.wifi_reason+')':''));}"
"catch(e){say('device unreachable','bad')}}"
"async function scan(){say('scanning\\u2026');var ns=document.getElementById('ns');ns.innerHTML='';"
"try{var d=await j('/api/v1/wifi/scan');var list=d.networks||[];"
"if(!list.length){say('no networks found \\u2014 try Rescan','bad');return}"
"list.forEach(function(n){var li=document.createElement('li');var b=document.createElement('button');"
"b.textContent=n.ssid+'  ('+n.rssi+' dBm, ch '+n.ch+(n.open?', open)':' )');"
"b.onclick=function(){sel=n.ssid;document.getElementById('pick').hidden=false;say('selected '+n.ssid);document.getElementById('pass').focus()};"
"li.appendChild(b);ns.appendChild(li)});say('pick a network');}catch(e){say('scan failed: '+e.message,'bad')}}"
"async function join(){if(!sel){say('pick a network first','bad');return}"
"var p=document.getElementById('pass').value;"
"say('joining '+sel+'\\u2026');"
"try{await j('/api/v1/wifi/join',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(tzbody(sel,p))});}"
"catch(e){var m=''+((e&&e.message)||'');"
"/* Losing the reply is the normal outcome, not a failure: the orb drops"
"   this very connection to try the new credentials, so the answer is often"
"   lost on the way back. Only call it a rejection when an answer came"
"   back with a reason in it. */"
"if(/tz|ssid|body|content|large/i.test(m)){say('join rejected: '+m,'bad');return}"
"say('sent \\u2014 orb is reconnecting\\u2026')}"
"clearInterval(timer);timer=setInterval(status,1500);status()}"
"function hm(m){var s=m<0?'-':'+',a=Math.abs(m);return s+Math.floor(a/60)+':'+('0'+(a%60)).slice(-2)}"
"function tzmsg(t){document.getElementById('tzs').textContent=t}"
"function tzpicked(){var s=document.getElementById('tz');return s.value=='__c__'?document.getElementById('tzx').value:s.value}"
"/* Only send tz when it differs from what the device reported: a join that resent the"
"   zone it already has costs an NVS write and an erase for nothing. */"
"function tzbody(ssid,p){var o={ssid:ssid,pass:p};var t=tzpicked();if(t&&t!=tzcur)o.tz=t;return o}"
"function tssel(v){var s=document.getElementById('tz'),x=document.getElementById('tzx'),g=document.getElementById('tzgo');"
" var k=false;for(var i=0;i<zones.length;i++)if(zones[i].name==v)k=true;"
" if(k){s.value=v;x.hidden=true;g.hidden=true}else{s.value='__c__';x.hidden=false;g.hidden=false;if(v&&v!='UTC')x.value=v}}"
"function aprep(d){var e=document.getElementById('apinfo');if(!e)return;"
"if(Date.now()<appin)return;                     /* do not clobber a fresh instruction */"
"e.textContent=d.ap?'setup AP is OPEN right now \u2014 join it from your phone':'';}"
"function apgo(){var e=document.getElementById('apinfo');if(e)e.textContent='opening\u2026';"
"appin=Date.now()+20000;                        /* aprep must not overwrite before this */"
"j('/api/v1/wifi/ap',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'})"
".then(function(d){if(!e)return;"
"e.textContent='join '+(d.ap_ssid||'the orb AP')+' from your phone, then open http://192.168.4.1/ ('+(d.seconds||'?')+'s)';})"
".catch(function(x){appin=0;if(e)e.textContent='ap: '+x.message;})}"
"async function tzload(){var v='UTC';"
" try{var d=await j('/api/v1/config');zones=d.zones||[];v=d.tz||'UTC';tzcur=v;"
" tzmsg('showing '+hm(d.tz_offset)+' from UTC ('+d.posix+')');}catch(e){tzmsg('zone list unavailable')}"
" var s=document.getElementById('tz');s.innerHTML='';"
" zones.forEach(function(z){var o=document.createElement('option');o.value=z.name;o.textContent=z.name+'  '+z.posix;s.appendChild(o)});"
" var c=document.createElement('option');c.value='__c__';c.textContent='other (POSIX rule)...';s.appendChild(c);"
" tssel(v)}"
"async function tzpush(){var v=tzpicked();if(!v){tzmsg('pick or type a zone');return}"
" tzmsg('setting '+v+'...');"
" try{var d=await j('/api/v1/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tz:v})});"
" tzcur=d.tz;tssel(d.tz);tzmsg('showing '+hm(d.tz_offset)+' from UTC ('+d.posix+') - is the dial right?');"
" }catch(e){tzmsg('zone rejected: '+e.message)}}"
"document.getElementById('tz').onchange=function(){var x=document.getElementById('tzx'),g=document.getElementById('tzgo');"
" if(this.value=='__c__'){x.hidden=false;g.hidden=false;x.focus();return}x.hidden=true;g.hidden=true;tzpush()};"
"document.getElementById('tzgo').onclick=tzpush;"
"document.getElementById('tzx').onkeydown=function(e){if(e.key=='Enter')tzpush()};"
"tzload();"
"document.getElementById('go').onclick=join;"
"document.getElementById('rs').onclick=scan;"
"document.getElementById('apbtn').onclick=apgo;"
"document.getElementById('pass').onkeydown=function(e){if(e.key=='Enter')join()};"
"status();scan();setInterval(function(){if(!sel)status()},3000);"
"</script></body></html>";

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, HTML_LANDING, HTTPD_RESP_USE_STRLEN);
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
     * below rather than trusting the count. */
    cfg.max_uri_handlers  = 16;
    /* Below the LVGL task's priority 5: a slow or chatty client must never
     * preempt a paint and show up as a dropped frame on the dial. */
    cfg.task_priority    = 4;

    const esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s (check LWIP_MAX_SOCKETS >= max_open_sockets+3)",
                 esp_err_to_name(e));
        return e;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = h_index },
        { .uri = "/api/v1/status",   .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/v1/wifi/scan",.method = HTTP_GET,  .handler = h_wifi_scan },
        { .uri = "/api/v1/wifi/join",.method = HTTP_POST, .handler = h_wifi_join },
        { .uri = "/api/v1/wifi/ap",  .method = HTTP_POST, .handler = h_wifi_ap },
        { .uri = "/api/v1/config",   .method = HTTP_GET,  .handler = h_config_get },
        { .uri = "/api/v1/config",   .method = HTTP_POST, .handler = h_config_post },
        { .uri = "/api/v1/ota",      .method = HTTP_POST, .handler = h_ota },
        { .uri = "/api/v1/reboot",   .method = HTTP_POST, .handler = h_reboot },
        /* Android + iOS/macOS captive-portal probes (see h_captive). */
        { .uri = "/generate_204",      .method = HTTP_GET, .handler = h_captive },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = h_captive },
        { .uri = "/favicon.ico",       .method = HTTP_GET, .handler = h_captive },
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

    char ssid[NET_SSID_MAX + 1] = "";
    char pass[NET_PASS_MAX + 1] = "";
    size_t sz;
    sz = sizeof(ssid); nvs_get_str(s_nvs, "ssid", ssid, &sz);
    sz = sizeof(pass); nvs_get_str(s_nvs, "pass", pass, &sz);

    s_sta_netif = esp_netif_create_default_wifi_sta();

    if (ssid[0] != '\0') {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    } else {
#if CONFIG_ORB_NET_AP_FALLBACK
        /* First boot / no credentials: come up as the provisioning AP. The
         * 466x466 panel is too small and glossy to render a scannable QR, so
         * pairing is by joining this AP from a phone instead. */
        s_provisioning = true;
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

const char *orb_net_ip_str(void) { return ""; }

bool orb_net_time_synced(void) { return false; }

#endif /* CONFIG_ORB_NET_ENABLE */
