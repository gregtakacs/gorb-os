#!/usr/bin/env python3
"""Bench check for orb_net.c's config-body parser and timezone offset math.

    python3 tools/host_tz_test.py

Neither of those two pieces of logic can be tested on the device without a radio,
a browser and a stopwatch, and both are the kind of code that fails silently: a
mis-parsed body stores a wrong TZ forever, and a wrong offset moves the dial an
hour with nothing logged. So they run here instead, in about a second.

The harness lifts `json_find`, `json_str` and the whole timezone block VERBATIM
out of main/net/orb_net.c rather than copying it, so the check can never drift
from what ships — if the code moves, the lift fails loudly instead of quietly
testing a stale copy. Only ESP_LOG* and the one NVS read are stubbed.

Why tz_offset_min exists at all, and so why it is worth a test: this toolchain's
newlib has no timegm() (verified with nm on libc.a), so the obvious
"timegm(localtime(x)) - x" cannot even be linked, and tm_gmtoff is only filled in
on the zoneinfo path. The function measures the offset by diffing localtime_r
against gmtime_r of the same instant, with a hand-rolled civil-days conversion —
exactly the kind of arithmetic that is off by one at midnight, at the date line,
and in half-hour zones. Every one of those cases is a row below.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "net", "orb_net.c")
src = open(SRC).read()


def lift(pattern):
    m = re.search(pattern, src, re.S)
    if not m:
        sys.exit(f"could not lift {pattern!r} from {SRC} — did the code move?")
    return m.group(0)

# whole timezone block, from the table typedef to just before net_tz_set's comment
block = src[src.index("typedef struct { const char *name"):src.index("/* Validate, persist, apply")]
jsonf = lift(r"static const char \*json_find\(const char \*body.*?\n\}")
jsons = lift(r"static bool json_str\(const char \*body.*?\n\}")
# The association ladder, lifted with its constants: retry_delay_s() decides how
# long a device sits before retrying a network that rejected it, and also when
# the rescue AP opens (the same counter). Both are invisible from the LAN and
# both are off-by-one-prone, so the ladder is pinned here rather than watched
# for ten minutes at a serial console.
retry_defs = "\n".join(lift(rf"#define {k} +\d+ +/[^\n]*")
                       for k in ("NET_STA_RETRY_S", "NET_STA_RETRY_MAX_S"))
retry_fn = lift(r"static uint32_t retry_delay_s\(unsigned attempt\).*?\n\}")
# Which disconnects the orb caused itself, and which are the network saying no.
# Lifted for the same reason as the ladder: both decide whether the rescue AP
# opens, and neither is observable from the LAN — a miscounted "leave" looks
# exactly like a router refusing the device, and ends with an open AP that
# nobody summoned. The codes are 802.11's, pinned to the values in
# esp_wifi_types_generic.h so a rename on our side cannot silently flip one.
si_fn = lift(r"static bool sta_leave_is_self_inflicted\(uint8_t reason\).*?\n\}")
wifi_reasons = """
enum { WIFI_REASON_AUTH_EXPIRE = 2,           /* esp_wifi_types_generic.h */
       WIFI_REASON_AUTH_LEAVE  = 3,
       WIFI_REASON_ASSOC_LEAVE = 8,
       WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
       WIFI_REASON_NO_AP_FOUND = 201 };
"""

harness = f"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#define NET_TZ_MAX 64
#define NET_TZ_KEY "tz"
#include <stdint.h>
{retry_defs}
{retry_fn}
{wifi_reasons}
{si_fn}
#define TAG "host"
#define ESP_LOGI(a, ...) do {{ }} while (0)
#define ESP_LOGW(a, ...) do {{ }} while (0)
#define ESP_LOGE(a, ...) do {{ }} while (0)
/* the only NVS touch inside the lifted block is net_tz_load's read; the host test
 * never exercises it, it just has to link */
#define ESP_OK 0
typedef int nvs_handle_t;
static nvs_handle_t s_nvs;
static int nvs_get_str(nvs_handle_t h, const char *k, char *o, size_t *sz)
{{ (void)h; (void)k; (void)sz; o[0] = '\\0'; return 1; }}
{jsonf}
{jsons}
{block}
static int fails = 0;
/* --- the plan's negative table, one row per case -------------------------- */
static void post(const char *body, int want_ok, const char *why, int want_clear)
{{
    (void)why;                    /* names the intent of the row; the verdict is printed */
    char v[NET_TZ_MAX + 1];
    bool present = false;
    const char *rej = tz_body_value(body, strlen(body), v, sizeof v, &present);
    /* net_tz_set's decision, minus the flash: accepted-and-would-be-applied */
    int ok = (rej == NULL) && (present ? (tz_reject_reason(v[0] ? v : "UTC") == NULL) : 1);
    int cleared = present && v[0] == '\\0';
    /* when the body is rejected out right, out stays empty — that is NOT a clear */
    int pass = (ok == want_ok) && (rej ? 1 : (present ? cleared == want_clear : want_clear == 0));
    if (!pass) fails++;
    printf("%-5s %-46s present=%d clear=%d value='%s' %s%s\\n", pass ? "ok" : "FAIL",
           body, present, cleared, v, rej ? "reject: " : "", rej ? rej : "");
}}
static void bk(unsigned attempt, uint32_t want)
{{
    const uint32_t got = retry_delay_s(attempt);
    if (got != want) fails++;
    printf("%-5s attempt %-12u -> %4u s (want %u)\\n",
           got == want ? "ok" : "FAIL", attempt, (unsigned)got, (unsigned)want);
}}
static void leave_case(unsigned reason, int want_self){{
    const bool got = sta_leave_is_self_inflicted((uint8_t)reason);
    const int pass = ((int)got == want_self);
    if (!pass) fails++;
    printf("%-5s reason %-4u -> %-14s (want %s)\\n", pass ? "ok" : "FAIL", reason,
           got ? "self-inflicted" : "counted",
           want_self ? "self-inflicted" : "counted");
}}
/* The consequence that matters: self-inflicted leaves must be able to go on
 * forever without reaching the rescue threshold, because the config page's AP
 * button generates one per press. Modelled against the same counter the handler
 * drives, so "the button eventually summons a rescue AP nobody asked for" is a
 * test failure rather than a field report. */
static void guard_counter(void){{
    unsigned counted = 0;
    for (int i = 0; i < 40; i++) {{
        const unsigned r = (i & 1) ? WIFI_REASON_AUTH_LEAVE : WIFI_REASON_ASSOC_LEAVE;
        if (!sta_leave_is_self_inflicted((uint8_t)r)) counted++;
    }}
    unsigned refused = 0;
    for (int i = 0; i < 40; i++) {{
        const unsigned r = (i & 1) ? WIFI_REASON_NO_AP_FOUND : WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT;
        if (!sta_leave_is_self_inflicted((uint8_t)r)) refused++;
    }}
    if (counted != 0 || refused != 40) fails++;
    printf("%-5s 40 leaves -> counter %u (want 0); 40 refusals -> %u (want 40)\\n",
           (counted == 0 && refused == 40) ? "ok" : "FAIL", counted, refused);
}}
static void off(const char *posix, int hh, int mm, int ss, int want){{
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 8;
    t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
    setenv("TZ", posix, 1); tzset();
    const time_t at = timegm(&t);
    const int got = tz_offset_min(at);
    if (got != want) fails++;
    printf("%-5s %-34s %02d:%02d:%02d -> %+d min (want %+d)\\n",
           got == want ? "ok" : "FAIL", posix, hh, mm, ss, got, want);
}}
int main(void)
{{
    puts("== POST /api/v1/config bodies ==");
    post("{{\\"tz\\":\\"US/Eastern\\"}}", 1, NULL, 0);          /* named zone  */
    post("{{\\"tz\\":\\"EST5EDT,M3.2.0,M11.1.0\\"}}", 1, NULL, 0); /* raw POSIX   */
    post("{{\\"tz\\":\\"\\"}}", 1, NULL, 1);                 /* explicit clear */
    post("{{\\"ssid\\":\\"x\\"}}", 1, NULL, 0);               /* absent = no-op */
    post("{{\\"tz\\" : \\"UTC\\" }}", 1, NULL, 0);            /* whitespace tolerated */
    post("{{\\"tz\\":\\"A\\\\\\"B\\"}}", 0, "quote collapse", 0);  /* arrives as A"B */
    post("{{\\"tz\\":\\"bad\\\\t\\"}}", 0, "escape", 0);         /* 0x5c 0x74, two chars */
    post("{{\\"tz\\":\\"a\\tb\\"}}", 0, "raw control", 0);        /* a REAL 0x09 */
    post("{{\\"tz\\":\\"a\\n\\"}}", 0, "raw newline", 0);         /* would break the log line */
    post("{{\\"tz\\":\\"XX3\\"}}", 1, NULL, 0);                /* degenerate rule: accepted */
    char long_body[256];
    sprintf(long_body, "{{\\"tz\\":\\"%.65s\\"}}",
            "0123456789012345678901234567890123456789012345678901234567890123456789");
    post(long_body, 0, "too long", 0);                        /* > NET_TZ_MAX */
    post("{{\\"tz\\":\\"UTC0\\"}}", 1, NULL, 0);
    post("{{\\"tz\\":\\"UTC0\\",\\"x\\":\\"tz\\"}}", 1, NULL, 0);   /* decoy key must not win */
    post("{{\\"x\\":\\"tz\\",\\"tz\\":\\"UTC\\"}}", 1, NULL, 0);   /* decoy value must not win */
    post("{{\\"ssid\\":\\"tz\\",\\"tz\\":\\"MST7\\"}}", 1, NULL, 0); /* join body, value=="tz" */
    post("{{\\"pass\\":\\"tz\\",\\"tz\\":\\"UTC\\"}}", 1, NULL, 0);   /* same, other key first */
    post("  {{\\"tz\\" : \\"UTC\\" }}  ", 1, NULL, 0);            /* leading whitespace */
    post("{{\\"a\\":{{\\"tz\\":\\"UTC\\"}}}}", 1, NULL, 0);      /* nested: still key position */
    post("{{\\"tz\\":\\"  \\"}}", 1, NULL, 0);                /* spaces: printable, accepted */
    puts("== offset measured, not asked for ==");
    off("EST5EDT,M3.2.0,M11.1.0", 12, 0, 0, -240);            /* October: EDT */
    off("EST5EDT,M3.2.0,M11.1.0", 12, 0, 0, -240);
    off("MST7", 12, 0, 0, -420);                              /* Arizona, no DST */
    off("CET-1CEST,M3.5.0,M10.5.0/3", 12, 0, 0, 120);
    off("GMT0BST,M3.5.0/1,M10.5.0", 12, 0, 0, 60);
    off("JST-9", 12, 0, 0, 540);
    off("XX3", 12, 0, 0, 0);                                  /* garbage rule -> UTC */
    off("UTC0", 12, 0, 0, 0);
    off("IST-5:30", 12, 0, 0, 330);                           /* half-hour zone */
    off("NPT-5:45", 12, 0, 0, 345);                           /* 45-minute zone */
    /* the day-rollover cases hand-rolled minute math gets wrong */
    off("EST5EDT,M3.2.0,M11.1.0", 0, 0, 0, -240);             /* local midnight */
    off("UTC0", 23, 59, 59, 0);
    off("JST-9", 23, 59, 59, 540);                            /* +1 day across the date line */
    off("EST5EDT,M3.2.0,M11.1.0", 12, 0, 30, -240);           /* non-zero seconds */
    puts("== association back-off ladder ==");
    /* The ladder is what a wall clock uses instead of giving up, so its shape is
     * the whole design: eager while a dropout might still be transient, quiet
     * once the network has clearly said no, and never past the ceiling. The
     * ceiling matters most — a `d *= 2` without the break overflows at attempt
     * ~27 into a huge unsigned, and esp_timer_start_once() on that is a device
     * that never retries and never opens the rescue AP, i.e. bricked until
     * someone finds the serial port. */
    bk(0, NET_STA_RETRY_S);                                   /* defensive clamp */
    bk(1, 5); bk(2, 10); bk(3, 20); bk(4, 40); bk(5, 80);
    bk(6, 120); bk(7, 120); bk(8, 120); bk(40, 120); bk(4000000000u, 120);
    /* Sum of the waits before the 8th failure — the time the rescue AP takes to
     * appear on a device with unusable credentials. Pinned so the Kconfig help's
     * "roughly N minutes" cannot silently become a lie. */
    {{
        unsigned sum = 0;
        for (unsigned a = 1; a < 8; a++) sum += retry_delay_s(a);
        if (sum != 395u) fails++;
        printf("%-5s first 8 attempts span %u s (want 395)\\n",
               sum == 395u ? "ok" : "FAIL", sum);
    }}
    puts("== which disconnects may spend a ladder rung ==");
    /* The same counter decides when the rescue AP opens, so a code misclassified
     * here either summons an AP nobody asked for (a self-inflicted leave counted
     * as a refusal) or leaves the device retrying a dead network forever (a real
     * refusal not counted). Both are invisible from the LAN. */
    leave_case(WIFI_REASON_AUTH_LEAVE, 1);                  /* 3: our disconnect() */
    leave_case(WIFI_REASON_ASSOC_LEAVE, 1);                  /* 8: our set_mode() */
    leave_case(WIFI_REASON_AUTH_EXPIRE, 0);                  /* 2: router let it lapse */
    leave_case(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, 0);       /* 15: wrong passphrase */
    leave_case(WIFI_REASON_NO_AP_FOUND, 0);                  /* 201: SSID gone */
    leave_case(0, 0);      /* driver reported nothing: must still count, or the
                            * ladder stops and the orb sits unreachable forever */
    guard_counter();
    printf("%s\\n", fails ? "FAILURES" : "all host checks pass");
    return fails != 0;
}}
"""
open("/tmp/tzjson_host.c", "w").write(harness)
cc = subprocess.run(["gcc", "-O1", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Wno-unused-function",
                     "-Werror", "-o", "/tmp/tzjson_host", "/tmp/tzjson_host.c"])
if cc.returncode:
    sys.exit(cc.returncode)
# -Werror on the lifted code is deliberate: it is also the cheapest compiler-diagnostics
# check on the device source, since anything that warns here warns in the IDF build too.
sys.exit(subprocess.run(["/tmp/tzjson_host"]).returncode)


