#!/usr/bin/env python3
"""Bench check for orb_net.c's config-body parser and timezone offset math.

    python3 tools/host_tz_test.py            # all checks
    python3 tools/host_tz_test.py --mutations # do the checks actually bite?

Three families of check live here, all for the same reason: this is code that
fails silently on a wall clock nobody is watching.

1. The lifted-C harness (parser, offset math, ladder, DNS): neither the device
   nor a stopwatch can substitute for running the real functions here.
2. The IANA oracle: every shipped zone-table row, asserted to reproduce
   zoneinfo under the host C library across 2025-2028 — see the block above
   the harness for why hand-written rules are a liability and how regenerating
   rows (tools notes in orb_net.c's table comment) is the maintenance story.
   This needs the machine's tzdata to be current; that is a feature: when a
   government changes a rule, this test fails before the clock does.
3. The page checks: both HTML pages are rebuilt from their C fragments the way
   the compiler does, parsed with esprima, pinned to the product decisions,
   and linted for calls to functions that do not exist (the fault esprima and
   substring pins both miss). --mutations re-runs the whole file against
   fourteen deliberate regressions; a battery that cannot fail is not a test.

Neither the parser nor a browser was available on the bench when the first
wrong-zone bug shipped: a mis-parsed body stores a wrong TZ forever, and a
wrong offset moves the dial an hour with nothing logged. So they run here
instead, in a few seconds.

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
if "--src" in sys.argv:
    SRC = sys.argv[sys.argv.index("--src") + 1]
src = open(SRC).read()
import hashlib
# Resolved before BIN, not at the D13 block below: the binary's name has to carry
# which theme file went in, or a mutation run reuses the previous run's image.
THEME_SRC = os.path.join(ROOT, "main", "art", "orb_theme.c")
if "--theme-src" in sys.argv:
    THEME_SRC = sys.argv[sys.argv.index("--theme-src") + 1]
# Named for BOTH sources it embeds: a mutation battery that swapped only the theme
# file would otherwise reuse the previous run's binary and grade the wrong code.
BIN = "/tmp/tzjson_host_" + hashlib.md5((SRC + THEME_SRC).encode()).hexdigest()[:10]


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
# The NAME/MODE mirrors ride inside the lifted timezone block, so their sizes and
# their NVS key must be lifted with them rather than hand-copied: NET_NAME_MAX is
# the same 32 the hub enforces (orbfleet.MaxName), and a harness that typed its own
# number would keep passing after the two halves stopped agreeing.
name_mode_defs = "\n".join(lift(rf"#define {k} +(?:\d+|\"name\") +/[^\n]*")
                           for k in ("NET_NAME_MAX", "NET_NAME_KEY", "NET_MODE_MAX"))
retry_fn = lift(r"static uint32_t retry_delay_s\(unsigned attempt\).*?\n\}")
# Which disconnects the orb caused itself, and which are the network saying no.
# Lifted for the same reason as the ladder: both decide whether the rescue AP
# opens, and neither is observable from the LAN — a miscounted "leave" looks
# exactly like a router refusing the device, and ends with an open AP that
# nobody summoned. The codes are 802.11's, pinned to the values in
# esp_wifi_types_generic.h so a rename on our side cannot silently flip one.
si_fn = lift(r"static bool sta_leave_is_self_inflicted\(uint8_t reason\).*?\n\}")
# The two association-window decisions added after the bench wedge of
# 2026-10-12, lifted for the same reason as the ladder above them: both are
# one-line predicates, both are invisible from the LAN, and getting either
# the wrong way round reproduces the failure — a retry fired into a live
# association killed it with an ESP_OK and no further event, and the device
# sat network-dead until someone pulsed EN. The truth tables below pin both
# from every direction so a future edit cannot drift them shut or open.
moot_fn = lift(r"static bool connect_attempt_is_moot\(bool[^;]*?return[^;]*;\n\}")
drop_fn = lift(r"static bool lease_watch_should_drop\(bool[^;]*?return[^;]*;\n\}")
# The join-intent guard added 2026-10-13 after a bench join with a wrong
# passphrase produced auth-fail and then silence: the ladder, the failure counter,
# the rescue and the AP timeout all sit behind a STA_DISCONNECTED event that
# `if (s_provisioning) break;` discarded. The discriminator is now join intent, so
# the composition is pinned here — the two tables below only see its result.
pidle_fn = lift(r"static bool prov_ap_idle\(bool[^;]*?return[^;]*;\n\}")
# The lease-gated credential write (D3, 2026-10-13). Both doors used to
# nvs_set_str() the instant the form was accepted, so one mistyped passphrase
# overwrote a working credential and took the orb off its network. The write now
# waits for a lease, and two pure decisions gate it: whether THIS lease authorises
# a write, and whether the pair is a passphrase or an open network (getting the
# second wrong leaves a blank "pass" in flash that reads back as a credential).
commit_fn = lift(r"static bool lease_verifies_creds\(bool[^;]*?return[^;]*;\n\}")
erase_fn  = lift(r"static bool creds_erase_key\(const char \*pass\)\n\{[^}]*\}")
# D4 (2026-10-13): the bounded sojourn's two decisions. sojourn_arms is the guard
# against the reboot loop — a factory orb has no stored network to fall back to, so
# arming for it would cycle forever; sojourn_reboots decides what a fired deadline
# means, and it must not be confused with the lease-closes-the-AP path.
arms_fn = lift(r"static bool sojourn_arms\(bool[^;]*?return[^;]*;\n\}")
sojreboot_fn = lift(r"static bool sojourn_reboots\(bool[^;]*?return[^;]*;\n\}")
# D2 (2026-10-13): the reboot-to-provisioning intent word lives in RTC NOINIT memory,
# which means it holds GARBAGE at cold power-on. Only the exact magic counts, so these
# rows are the reason it is a uint32_t magic and not a bool. The #define is lifted with
# the predicate on purpose: retarget the magic and the table follows it, and a magic
# that collides with an obvious garbage pattern (all-zero, all-one) still shows up here.
magic_def = lift(r"#define PROV_INTENT_MAGIC [^\n]*")
intent_fn = lift(r"static bool prov_intent_armed\(uint32_t w\)\n\{[^}]*\}")
# The read-and-clear and the rollback are lifted with it, because the interesting D2
# behaviour is not the predicate — it is what the word does across a boot. A word that
# stayed armed after the boot branch read it would force provisioning on every restart,
# and a word that survived a failed 500 would boot the next power cycle into an AP
# nobody asked for. Neither is observable on a bench that cannot cheaply cycle RTC
# memory, so both are pinned here against the shipped functions.
consume_fn = lift(r"static bool prov_intent_consume\(void\)\n\{[^}]*\}")
disarm_fn = lift(r"static void prov_intent_disarm\(void\)\n\{[^}]*\}")
# The captive-portal DNS reply builder, lifted for the same reason as the ladder
# and the self-inflicted guard: it is exactly the code this bench cannot exercise
# (no WiFi radio to send a real query), and every byte is a silent failure mode —
# a wrong offset is a phone that never shows "Sign in to network" and a device
# that logs nothing wrong. So the encoding is pinned here instead.
dns_fn = lift(r"static int dns_build_reply\(const uint8_t \*rx.*?return len;\n\}")

# Ruling D13's two pure predicates, lifted out of the theme layer for the same
# reason everything else in this harness is lifted: they are one-line decisions
# about which 4 MB of flash the dial reads, and the bench has no second slot to
# corrupt and no way to watch a boot draw the wrong bank. Truth-tabled below.
THEME_HDR = os.path.join(ROOT, "main", "art", "orb_theme.h")
theme_src = open(THEME_SRC).read()
theme_hdr = open(THEME_HDR).read()


def lift_theme(text, where, pattern):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit(f"could not lift {pattern!r} from {where} — did the code move? An "
                 f"empty lift here would keep the D13 table green while testing a "
                 f"predicate that no longer ships.")
    return m.group(0)


d13_lift = "\n".join([
    lift_theme(theme_hdr, THEME_HDR, r"#define THEME_LIVE_DEFAULT[^\n]*\n"
                                     r"#define THEME_LIVE_CARD[^\n]*"),
    lift_theme(theme_src, THEME_SRC, r"#define SLOT_A_HDR[^\n]*\n#define SLOT_B_HDR[^\n]*"),
    lift_theme(theme_src, THEME_SRC, r"typedef enum \{ BOOT_ART_DEFAULT = 0,"
                                     r" BOOT_ART_CARD = 1 \} boot_art_t;"),
    lift_theme(theme_src, THEME_SRC,
               r"static boot_art_t boot_resolve\(const char \*live, const bool b_valid\)"
               r"\n\{[\s\S]*?\n\}"),
    lift_theme(theme_src, THEME_SRC, r"static uint32_t install_target\(void\)[^\n]*\}"),
])

wifi_reasons = """
enum { WIFI_REASON_AUTH_EXPIRE = 2,           /* esp_wifi_types_generic.h */
       WIFI_REASON_AUTH_LEAVE  = 3,
       WIFI_REASON_ASSOC_LEAVE = 8,
       WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
       WIFI_REASON_NO_AP_FOUND = 201 };
"""

# ---------------------------------------------------------------------------
# The shipped zone table vs the machine's IANA database — the referee for
# "what if the user is not one of the listed cities". Every row's POSIX rule
# is asserted to reproduce, under the host C library, exactly what zoneinfo
# says for the canonical IANA zone: every noon UTC of 2026 plus the minute
# either side of every 2025-2028 transition. Hand-written rules fail this the
# way hand-written rules always fail — an /n time quoted in the wrong side's
# local clock, a last-Sunday pinned as a fourth-Sunday — and the failure is a
# silent hour of wrongness half the year on somebody's wall, invisible on the
# bench (no screen, and the owner's phone may be in the one wrong zone). The
# same derivation runs in the clock page's JS for zones NOT in this table;
# monthly-window bisection here is the identical algorithm, verified once
# against an exhaustive hourly scan over every zone on the system.
# Requires current tzdata: if a future rule change breaks a row, this test
# fails LOUDLY the day the box's tzdata updates — that is the maintenance
# story, regenerate the row, do not edit rules by hand.
# ---------------------------------------------------------------------------
import datetime as _dt
try:
    from zoneinfo import ZoneInfo
except ImportError:
    ZoneInfo = None

tbm = re.search(r'static const tz_entry_t s_tz_tbl\[\] = \{(.*?)\n\};', src, re.S)
if not tbm:
    sys.exit('could not find s_tz_tbl — did it get renamed?')
rows = re.findall(r'\{\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\s*,?\s*\}',
                  tbm.group(1))
if not rows:
    sys.exit('no four-column zone rows parsed from s_tz_tbl')

def _off_min(z, ts):
    d = _dt.datetime.fromtimestamp(ts, _dt.timezone.utc).astimezone(z)
    return int(d.utcoffset().total_seconds() // 60)

def _trans(z, year):
    out = []
    for m in range(1, 13):
        a0 = int(_dt.datetime(year, m, 1, tzinfo=_dt.timezone.utc).timestamp())
        a1 = int(_dt.datetime(year + (1 if m == 12 else 0),
                              1 if m == 12 else m + 1, 1,
                              tzinfo=_dt.timezone.utc).timestamp())
        o0, o1 = _off_min(z, a0), _off_min(z, a1)
        if o0 == o1:
            continue
        lo, hi = a0 // 60, a1 // 60
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if _off_min(z, mid * 60) == o0:
                lo = mid
            else:
                hi = mid
        out.append((hi * 60, o0, o1))
    return out

probe_data = probe_calls = ''
if ZoneInfo is None:
    print('SKIP  IANA oracle (no zoneinfo; python >= 3.9 required — zone rows go UNREFEREED)')
else:
    datas = []
    missing_zones = []
    for idx, (n, px, ia, la) in enumerate(rows):
        try:
            z = ZoneInfo(ia or n)
        except Exception:
            # A row naming a zone this machine's tzdata does not know is a
            # FAIL line, not a traceback — the battery must report verdicts.
            missing_zones.append('%s -> %s' % (n, ia or n))
            datas.append('static const struct probe pr%d[] = {{0,0}};' % idx)
            probe_calls += ('    off_sweep("%s", "%s", pr%d, 0);\n' % (n, px, idx))
            continue
        pr = []
        for y in (2025, 2026, 2027, 2028):
            for t, o_lo, o_hi in _trans(z, y):
                pr += [(t - 60, o_lo), (t, o_hi), (t + 3540, o_hi)]
        base = int(_dt.datetime(2026, 1, 1, 12, tzinfo=_dt.timezone.utc).timestamp())
        pr += [(base + k * 86400, _off_min(z, base + k * 86400)) for k in range(366)]
        datas.append('static const struct probe pr%d[] = {' % idx +
                     ','.join('{%d,%d}' % p for p in pr) + '};')
        probe_calls += ('    off_sweep("%s", "%s", pr%d, %u);\n' % (n, px, idx, len(pr)))
    probe_data = (
        'struct probe { long long at; int want; };\n'
        'static unsigned off_bad;\n'
        'static void off_at1(const char *posix, long long at, int want)\n'
        '{\n'
        '    setenv("TZ", posix, 1); tzset();\n'
        '    const int got = tz_offset_min((time_t)at);\n'
        '    if (got != want) { off_bad++;\n'
        '        if (off_bad <= 3)\n'
        '            printf("      %s @%lld -> %+d want %+d\\n", posix, at, got, want);\n'
        '    }\n'
        '}\n'
        'static void off_sweep(const char *name, const char *posix,\n'
        '                      const struct probe *p, unsigned n)\n'
        '{\n'
        '    off_bad = 0;\n'
        '    for (unsigned i = 0; i < n; i++) off_at1(posix, p[i].at, p[i].want);\n'
        '    if (off_bad) { fails++;\n'
        '        printf("FAIL zone %-28s some of %u probes wrong vs IANA (posix %s)\\n",\n'
        '               name, n, posix);\n'
        '    } else\n'
        '        printf("ok    zone %-28s %3u probes match IANA\\n", name, n);\n'
        '}\n'
        + chr(10).join(datas) + chr(10))
    probe_calls = ('    puts("== shipped zone rules vs the machine IANA database ==");\n'
                   + probe_calls.replace('\\n', '\n'))

harness = f"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#define NET_TZ_MAX 64
#define NET_TZ_KEY "tz"
{name_mode_defs}
#include <stdint.h>
{retry_defs}
{retry_fn}
{wifi_reasons}
{si_fn}
{moot_fn}
{drop_fn}
{pidle_fn}
{commit_fn}
{erase_fn}
{arms_fn}
{sojreboot_fn}
{magic_def}
{intent_fn}
/* A plain word in the harness where the device has an RTC_NOINIT one: the lifecycle
 * being pinned is the read/clear/rollback logic, and that logic never touches the
 * memory type. The memory type is the link map's problem, checked at build time. */
static uint32_t s_prov_intent;
{consume_fn}
{disarm_fn}
{dns_fn}
{d13_lift}
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
static void moot_case(int assoc, int up, int prov, int want){{
    const bool got = connect_attempt_is_moot(assoc, up, prov);
    const int pass = ((int)got == want);
    if (!pass) fails++;
    printf("%-5s connect(assoc=%d up=%d prov=%d) -> %-4s (want %s)\\n",
           pass ? "ok" : "FAIL", assoc, up, prov,
           got ? "moot" : "try", want ? "moot" : "try");
}}
static void drop_case(int assoc, int up, int prov, int want){{
    const bool got = lease_watch_should_drop(assoc, up, prov);
    const int pass = ((int)got == want);
    if (!pass) fails++;
    printf("%-5s lease_watch(assoc=%d up=%d prov=%d) -> %-5s (want %s)\\n",
           pass ? "ok" : "FAIL", assoc, up, prov,
           got ? "drop" : "hold", want ? "drop" : "hold");
}}
/* The third argument both tables above receive. Four rows, all of them load-
 * bearing: (1,1) is the bug — a submitted join whose failure was discarded. */
static void pidle_case(int prov, int join, int want){{
    const bool got = prov_ap_idle(prov != 0, join != 0);
    const int pass = ((int)got == want);
    if (!pass) fails++;
    printf("%-5s prov_ap_idle(prov=%d joining=%d) -> %-4s (want %s)\\n",
           pass ? "ok" : "FAIL", prov, join,
           got ? "idle" : "runs", want ? "idle" : "runs");
}}
/* D3: does this lease authorise writing the pair to flash? */
static void commit_case(int pend, int join, int want){{
    const bool got = lease_verifies_creds(pend != 0, join != 0);
    const int pass = ((int)got == want);
    if (!pass) fails++;
    printf("%-5s lease_verifies_creds(pend=%d joining=%d) -> %-7s (want %s)\\n",
           pass ? "ok" : "FAIL", pend, join,
           got ? "STORE" : "no-write", want ? "STORE" : "no-write");
}}
/* D3: passphrase -> set the key; open network -> ERASE it. The empty-string and
 * NULL rows are the ones that matter: both mean "no passphrase", and leaving a
 * blank pass in flash instead of removing the key makes the next boot retry an
 * enterprise association the AP never offered. */
static void erase_case(const char *pass, int want){{
    const bool got = creds_erase_key(pass);
    const int ok = ((int)got == want);
    if (!ok) fails++;
    printf("%-5s creds_erase_key(%s) -> %-5s (want %s)\\n",
           ok ? "ok" : "FAIL",
           pass ? (*pass ? "set" : "empty") : "NULL",
           got ? "erase" : "set", want ? "erase" : "set");
}}
/* D4: does this provisioning boot own a deadline? */
static void arms_case(int recovery, int secs, int want){{
    const bool got = sojourn_arms(recovery != 0, secs);
    const int ok = ((int)got == want);
    if (!ok) fails++;
    printf("%-5s sojourn_arms(recovery=%d sojourn_s=%d) -> %-6s (want %s)\\n",
           ok ? "ok" : "FAIL", recovery, secs,
           got ? "ARMED" : "no-timer", want ? "ARMED" : "no-timer");
}}
/* D4: a deadline just fired. Reboot, or leave the AP alone? */
static void sojreboot_case(int sta_up, int recovery, int want){{
    const bool got = sojourn_reboots(sta_up != 0, recovery != 0);
    const int ok = ((int)got == want);
    if (!ok) fails++;
    printf("%-5s sojourn_reboots(sta_up=%d recovery=%d) -> %-6s (want %s)\\n",
           ok ? "ok" : "FAIL", sta_up, recovery,
           got ? "REBOOT" : "no-reboot", want ? "REBOOT" : "no-reboot");
}}
/* D2: does this RTC word read as a provisioning request? */
static void intent_case(uint32_t w, int want, const char *why){{
    const bool got = prov_intent_armed(w);
    const int ok = ((int)got == want);
    if (!ok) fails++;
    printf("%-5s prov_intent_armed(0x%08X, %-12s) -> %-9s (want %s)\\n",
           ok ? "ok" : "FAIL", (unsigned)w, why,
           got ? "PROVISION" : "no-intent", want ? "PROVISION" : "no-intent");
}}
/* D2's lifecycle, not its predicate: set the word the way the setter does, then ask what
 * one consume leaves behind. want_word is what the word must hold afterwards — zero is
 * the whole point of read-and-clear, and the rollback row is the 500 path in h_reprov. */
static void life_case(const char *why, uint32_t start, int want_got, uint32_t want_word){{
    s_prov_intent = start;
    const bool got = prov_intent_consume();
    const uint32_t word = s_prov_intent;
    const int ok = ((int)got == want_got) && (word == want_word);
    if (!ok) fails++;
    printf("%-5s intent life %-22s consume -> %-9s word left 0x%08X (want %s 0x%08X)\\n",
           ok ? "ok" : "FAIL", why, got ? "PROVISION" : "no-intent",
           (unsigned)word, want_got ? "PROVISION" : "no-intent", (unsigned)want_word);
}}
/* A request armed and then refused: h_reprov hits a timer failure after the setter
 * already wrote the word, and an orb that came back armed anyway would cost its next
 * owner a surprise AP. */
static void rollback_case(const char *why){{
    s_prov_intent = PROV_INTENT_MAGIC;
    prov_intent_disarm();
    const bool got = prov_intent_consume();
    const int ok = !got && s_prov_intent == 0;
    if (!ok) fails++;
    printf("%-5s intent rollback %-18s -> %-9s word 0x%08X (want no-intent 0x00000000)\\n",
           ok ? "ok" : "FAIL", why, got ? "PROVISION" : "no-intent",
           (unsigned)s_prov_intent);
}}
/* Ruling D13's two decisions, tabled. Which slot a boot draws and which slot an
 * install may write are the two numbers a wrong answer about costs a device its
 * dial, and neither is observable on a bench with one card and no second slot to
 * corrupt: the shipped predicates are lifted and run instead of watched. */
static void boot_case(const char *what, const char *live, bool b_valid, int want){{
    const boot_art_t got = boot_resolve(live, b_valid);
    const int pass = ((int)got == want);
    if (!pass) fails++;
    printf("%-5s boot_resolve %-34s -> %-7s (want %s)\\n", pass ? "ok" : "FAIL", what,
           (int)got == BOOT_ART_CARD ? "card" : "default",
           want == BOOT_ART_CARD ? "card" : "default");
}}
static void install_target_case(void){{
    /* Slot A is partition offset 0, which is where `idf.py flash` wrote the factory
     * bank and where use_baked_in() reads its pixels from. One install aimed there
     * erases the default and the fallback together — the defect ruling D13 exists
     * to kill, so the target is pinned to B and pinned AWAY from 0. */
    const uint32_t t = install_target();
    const int pass = (t == SLOT_B_HDR) && (t != SLOT_A_HDR) && (t != 0u);
    if (!pass) fails++;
    printf("%-5s install_target() -> 0x%08X (want slot B 0x%08X; A is 0x%08X)\\n",
           pass ? "ok" : "FAIL", (unsigned)t, (unsigned)SLOT_B_HDR, (unsigned)SLOT_A_HDR);
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
/* A single-question DNS query "example", A or AAAA, QDCOUNT forced, so the
 * builder's accept/reject decisions and its exact answer bytes can be checked. */
static int mk_query(uint8_t *q, uint16_t qtype, uint16_t qdcount)
{{
    q[0]=0xAB; q[1]=0xCD;                    /* id, must be echoed back  */
    q[2]=0x01; q[3]=0x00;                     /* RD=1, everything else 0  */
    q[4]=(uint8_t)(qdcount>>8); q[5]=(uint8_t)qdcount;
    q[6]=0;q[7]=0;q[8]=0;q[9]=0;q[10]=0;q[11]=0;
    int p=12;
    q[p++]=7; memcpy(q+p,"example",7); p+=7;  /* one label, no compression */
    q[p++]=0;                                  /* end of QNAME             */
    q[p++]=(uint8_t)(qtype>>8); q[p++]=(uint8_t)qtype;
    q[p++]=0x00; q[p++]=0x01;                  /* CLASS IN                  */
    return p;                                  /* == 25                     */
}}
static void dns_case(const char *what, uint16_t qtype, uint16_t qdcount,
                     int n_override, int cap, int want_len)
{{
    uint8_t q[64], tx[512];
    memset(tx, 0xEE, sizeof tx);
    int n = mk_query(q, qtype, qdcount);
    if (n_override > 0) n = n_override;         /* exercise the truncated path */
    const uint8_t ipb[4] = {{192,168,4,1}};
    uint32_t ip; memcpy(&ip, ipb, 4);           /* network order: bytes = octets */
    const int len = dns_build_reply(q, n, ip, tx, cap);
    int pass = (len == want_len);
    if (pass && want_len > 0) {{
        const int qlen = n - 12;
        if (tx[0]!=0xAB || tx[1]!=0xCD) pass=0;               /* id echoed        */
        if (tx[2]!=0x81 || tx[3]!=0x80) pass=0;               /* QR|RD, RA, noerr */
        if (tx[4]!=0   || tx[5]!=1)     pass=0;               /* QDCOUNT kept = 1 */
        if (tx[8]||tx[9]||tx[10]||tx[11]) pass=0;             /* NSCOUNT, ARCOUNT */
        if (tx[7]==1) {{                                        /* answered A */
            if (qtype != 1) pass=0;
            const uint8_t *a = tx + 12 + qlen;
            if (!(a[0]==0xC0&&a[1]==0x0C&&a[2]==0&&a[3]==1&&a[4]==0&&a[5]==1)) pass=0;
            if (!(a[6]==0&&a[7]==0&&a[8]==0&&a[9]==0x3C&&a[10]==0&&a[11]==4)) pass=0;
            if (memcmp(a+12, ipb, 4)!=0) pass=0;               /* rdata == the AP */
            if (len != 12+qlen+16) pass=0;
        }} else if (tx[7]==0) {{                                /* no answer */
            if (len != 12+qlen) pass=0;
        }} else pass=0;
    }}
    if (!pass) fails++;
    printf("%-5s %-24s qtype=%-2u qd=%u n=%d cap=%d -> %d (want %d)\\n",
           pass?"ok":"FAIL", what, (unsigned)qtype, (unsigned)qdcount, n, cap, len, want_len);
}}
{probe_data}
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
{probe_calls}
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
    /* The association-window guards added after the bench wedge of
     * 2026-10-12: the retry timer firing while the station was associated but
     * still unleased destroyed that association (esp_wifi_connect() returned
     * ESP_OK, no event followed, dead device until EN was pulsed).
     * connect_attempt_is_moot must close that door for every live state, and
     * lease_watch_should_drop must guard exactly the associated-unleased one —
     * wider drops healthy leases or meddles in provisioning, narrower
     * reproduces the wedge. Both are pure, so all eight rows cost nothing. */
    moot_case(0, 0, 0, 0);   /* genuinely quiet: the attempt must proceed */
    moot_case(1, 0, 0, 1);   /* the wedge window: connect must NOT fire */
    moot_case(0, 1, 0, 1);   /* lease held: moot */
    moot_case(0, 0, 1, 1);   /* AP up with nothing submitted: moot */
    moot_case(1, 1, 0, 1); moot_case(1, 0, 1, 1);
    moot_case(0, 1, 1, 1); moot_case(1, 1, 1, 1);
    /* And what feeds that third argument. (1,1) is the provisioning session whose
     * failure must reach the ladder instead of the silence: the row that was
     * previously unreachable because the raw flag stood in for this value. */
    pidle_case(0, 0, 0);   /* ordinary boot, no join pending: not the AP-only case */
    pidle_case(0, 1, 0);   /* ordinary boot, join in flight: ladder owns it */
    pidle_case(1, 0, 1);   /* provisioning, nobody has submitted: leave it alone */
    pidle_case(1, 1, 0);   /* provisioning AFTER a submission: the ladder must run */
    puts("== D3: the lease gates the credential write ==");
    /* The write that used to happen at submit time. Only (1,1) may touch flash:
     * (1,0) is the boot-time auto-retry of what flash already holds, and (0,1) is
     * a submission whose pair never got stored. A build that wrote on either alone
     * is what let one mistyped passphrase take a working orb off its network. */
    commit_case(0, 0, 0);
    commit_case(1, 0, 0);
    commit_case(0, 1, 0);
    commit_case(1, 1, 1);
    /* Set-vs-erase on an empty passphrase, the branch both doors used to spell
     * inline. NULL is what an absent "pass" field arrives as over the serial door. */
    erase_case("correct horse", 0);
    erase_case("", 1);
    erase_case(NULL, 1);
    puts("== D4: the bounded sojourn, and the loop its guard prevents ==");
    /* (0,600) is the row the whole guard exists for: a FACTORY boot — nothing in
     * flash to fall back to — with a perfectly ordinary sojourn configured. Arm
     * there and the orb reboots onto the same provisioning branch, re-arms, and
     * cycles forever unattended. (1,0) is an operator disabling the exit. */
    arms_case(0, 600, 0);
    arms_case(0, 0, 0);
    arms_case(1, 600, 1);
    arms_case(1, 0, 0);
    /* A fired deadline. (1,1) must NOT reboot: the lease arrived, so the session
     * succeeded and the AP closes on the normal path. (0,0) is every ordinary AP
     * expiry — rescue or manual — and stays the re-check it always was. */
    sojreboot_case(0, 1, 1);
    sojreboot_case(1, 1, 0);
    sojreboot_case(0, 0, 0);
    sojreboot_case(1, 0, 0);
    puts("== D2: the RTC intent word, and the garbage it must survive ==");
    /* .rtc_noinit is NOLOAD and nothing zeroes it, so at cold power-on the word is
     * whatever the SRAM came up with. These are not hypothetical rows: a word that
     * happens to come up zero, all-ones, or one bit off must NOT put a sealed
     * wall-powered orb into AP mode. The cold-zero row also proves the magic is not
     * 0, and the all-ones row proves it is not ~0u, without a separate assert. */
    intent_case(0, 0, "cold-zero");
    intent_case(0xFFFFFFFFu, 0, "all-ones");
    intent_case(0xA5A5A5A5u, 0, "pattern-fill");
    intent_case('O', 0, "one-ascii");
    intent_case(PROV_INTENT_MAGIC ^ 1u, 0, "bit-flipped");
    intent_case(PROV_INTENT_MAGIC, 1, "the magic");
    /* One boot's request, no more: consume reports it and leaves the word cleared, so
     * the restart after a forced session is an ordinary boot. The garbage rows go through
     * the same call to prove a miss clears too — that is what stops a cold-boot pattern
     * from surviving to be misread on a later boot. */
    life_case("armed -> consumed once", PROV_INTENT_MAGIC, 1, 0u);
    life_case("consumed again", 0u, 0, 0u);
    life_case("cold-zero miss", 0u, 0, 0u);
    life_case("all-ones miss", 0xFFFFFFFFu, 0, 0u);
    life_case("bit-flipped miss", PROV_INTENT_MAGIC ^ 1u, 0, 0u);
    rollback_case("after a refused POST");
    drop_case(1, 0, 0, 1);   /* the wedge state: drop it; the ladder owns the retry */
    drop_case(0, 0, 0, 0); drop_case(1, 1, 0, 0); drop_case(1, 0, 1, 0);
    drop_case(0, 1, 0, 0); drop_case(0, 0, 1, 0);
    drop_case(1, 1, 1, 0); drop_case(0, 1, 1, 0);
    puts("== ruling D13: the slot the dial draws, and the one an install may write ==");
    /* Every row that can be asked of the resolve predicate: the choice, whether B
     * verified, and what comes out. "card" with a B that will not load is the
     * corruption case, and the answer must be the default with the choice left
     * written — a boot that quietly cleared it would forget what the owner asked
     * for, and a boot that drew the corrupt slot would show half a theme. */
    boot_case("card chosen, B verifies", THEME_LIVE_CARD, true, BOOT_ART_CARD);
    boot_case("card chosen, B corrupt", THEME_LIVE_CARD, false, BOOT_ART_DEFAULT);
    boot_case("default chosen, B verifies", THEME_LIVE_DEFAULT, true, BOOT_ART_DEFAULT);
    boot_case("default chosen, B corrupt", THEME_LIVE_DEFAULT, false, BOOT_ART_DEFAULT);
    boot_case("no key at all, B verifies", "", true, BOOT_ART_DEFAULT);
    boot_case("no key at all, B corrupt", "", false, BOOT_ART_DEFAULT);
    boot_case("junk in the key, B verifies", " Card ", true, BOOT_ART_DEFAULT);
    boot_case("NVS unreadable (NULL), B verifies", NULL, true, BOOT_ART_DEFAULT);
    boot_case("NVS unreadable (NULL), B corrupt", NULL, false, BOOT_ART_DEFAULT);
    install_target_case();
    leave_case(0, 0);      /* driver reported nothing: must still count, or the
                            * ladder stops and the orb sits unreachable forever */
    guard_counter();
    puts("== name/mode mirrors: sized to their own MAX, writable at the boundary ==");
    /* s_name and s_mode arrive with the lifted timezone block. Two rules the shipped
     * code depends on are pinned here rather than trusted: the name mirror must hold
     * a full NET_NAME_MAX name with room for the NUL — the hub enforces the same 32
     * and a mirror one byte short would clip a name the card already accepted, which
     * reads as two different orbs depending on which surface you looked at — and the
     * mode mirror must hold the longest value it can store, "sta". */
    char full_name[NET_NAME_MAX + 1];
    memset(full_name, 'n', NET_NAME_MAX);
    full_name[NET_NAME_MAX] = '\\0';
    snprintf(s_name, sizeof s_name, "%s", full_name);
    snprintf(s_mode, sizeof s_mode, "%s", "sta");
    int mirror_ok = (strlen(s_name) == (size_t)NET_NAME_MAX) && (strcmp(s_mode, "sta") == 0);
    if (!mirror_ok) fails++;
    printf("%-5s s_name holds %d bytes, s_mode holds \\"sta\\"\\n",
           mirror_ok ? "ok" : "FAIL", NET_NAME_MAX);
    puts("== captive-portal DNS reply encoding ==");
    /* The A answer is what actually pops the phone's "Sign in to network" box;
     * AAAA/PTR must come back with the question echoed and NO answer so the
     * client falls through to A instead of mis-parsing a bogus record; and a
     * truncated or second-question query must be dropped, not answered wrong. */
    dns_case("A -> one answer",      1,  1,  0, 512, 41);
    dns_case("AAAA -> echo only",   28,  1,  0, 512, 25);
    dns_case("PTR -> echo only",    12,  1,  0, 512, 25);
    dns_case("truncated <17B",       1,  1, 16, 512,  0);
    dns_case("two questions",        1,  2,  0, 512,  0);
    dns_case("tx too small",         1,  1,  0,  20,  0);
    printf("%s\\n", fails ? "FAILURES" : "all host checks pass");
    return fails != 0;
}}
"""
BC_SRC = BIN + ".c"
open(BC_SRC, "w").write(harness)
cc = subprocess.run(["gcc", "-O1", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Wno-unused-function",
                     "-Werror", "-o", BIN, BC_SRC])
if cc.returncode:
    sys.exit(cc.returncode)
# -Werror on the lifted code is deliberate: it is also the cheapest compiler-diagnostics
# check on the device source, since anything that warns here warns in the IDF build too.
# ---------------------------------------------------------------------------
# Landing-page JS. The provisioning UX is one giant C string literal, and a
# syntax error in it was invisible to every other check in this file — and
# invisible on the bench, which has no radio and has therefore never once
# loaded the page in a real browser. It is NOT invisible to a phone: the first
# parse error kills the whole <script> block, so status(), scan() and tzload()
# all silently never run and the page sits at "loading…" with an empty network
# list and an empty timezone dropdown. Found exactly that way, from a user
# report: an unbalanced ')' in status()'s say() call — each C fragment balances
# locally, only the compiler's concatenation breaks it, so reviewing source
# lines one by one shows nothing.
#
# So: rebuild the page bytes the way the compiler does (concatenate the
# adjacent literals inside HTML_LANDING, C-unescaping each) and parse the
# script block. esprima is not stdlib — a missing parser is a loud SKIP (run
# this file with a python that has it: pip install esprima); a missing page or
# a broken script is a FAIL.
def c_page(sym):
    """Rebuild the page bytes the way the compiler does (concatenate the adjacent
    literals, C-unescape each) after first refusing any line that is neither a
    comment nor a whole C string fragment — the extractor below silently skips
    lines it cannot match, so without this gate an unbalanced quote deletes
    source from the bytes under test instead of failing (two real edits did
    exactly that, and the parse checks stayed green on a page that had quietly
    lost lines)."""
    m = re.search(r'static const char ' + sym + r'\[\] =(.*?);\s*\n', src, re.S)
    if not m:
        return None, None, (sym + ' not found in orb_net.c — did it get renamed? '
                            'This check dies loudly rather than silently stop covering.')
    region = m.group(1)
    base = src[:m.start()].count(chr(10)) + 1
    bad, in_comment, kept = [], False, []
    for off, line in enumerate(region.split(chr(10))):
        t, where = line.strip(), base + off
        if not t:
            continue
        if in_comment:
            if '*/' in t:
                in_comment = False
            continue
        if t.startswith('/*') or t.startswith('//'):
            if t.startswith('/*') and '*/' not in t:
                in_comment = True
            continue
        if re.fullmatch(r'"(?:[^"\\]|\\.)*";?', t):
            kept.append(t)
        else:
            bad.append((where, t[:64]))
    if bad:
        for where, t in bad[:6]:
            print(f'FAIL {sym}: line {where} is neither a comment nor a complete '
                  f'C string fragment (unbalanced quote?): {t!r}')
        print('       the extractor skips such lines, so they were NOT covered '
              'by the parse/pin checks below.')
        return None, None, sym + ' fragment integrity'
    import ast
    # fragments come ONLY from gate-classified string lines: a regex over the
    # raw region also harvests "quoted words" from C comments and injects them
    # into the bytes under test (one comment that wrote read as "last" shipped
    # that word into the parsed page and past the forbidden-token pins).
    fragments = [mm.group(1) for t in kept
                 for mm in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', t)]
    if not fragments:
        return None, None, f'no string fragments inside {sym}'
    page = ''.join(ast.literal_eval('"' + f + '"') for f in fragments)
    js = re.search(r'<script>(.*?)</script>', page, re.S)
    if not js:
        return None, None, f'no <script> block in {sym}'
    return page, js.group(1), None


def _calls(js_):
    # User-facing copy contains shapes that only LOOK like call sites
    # (Same as your phone (does not follow DST)); blank strings first.
    out, instr, esc = [], None, False
    for ch in js_:
        if instr:
            if esc:
                esc = False
            elif ch == chr(92):
                esc = True
            elif ch == instr:
                instr = None
            out.append(chr(32))
        else:
            if ch in (chr(39), chr(34)):
                instr = ch
                out.append(chr(32))
            else:
                out.append(ch)
    t = str().join(out)
    # callback/catch parameter lists are not calls: function(d){, catch(e){
    t = re.sub(r"(function\s*\()[^)]*(\))", r"\1\2", t)
    t = re.sub(r"(catch\s*\()[^)]*(\))", r"\1\2", t)
    return set(re.findall(r"(?<![.\w$])([a-z][A-Za-z0-9_$]*)\s*\(", t))

def landing_page_js():
    # Two pages ship in firmware now: the provisioning landing page and the
    # clock page (/timezone, HTML_TZ). The timezone UI was MOVED, not restyled
    # — owner's call after provisioning worked: the network page must not show
    # zone settings at all, and the old dropdown of IANA keys + POSIX free
    # text was unusable for a person (and the phone's own zone was never
    # auto-applied, so it usually was not touched at all). So the pins below
    # enforce BOTH halves of that decision: tz UI is FORBIDDEN on the landing
    # page, and the clock page is pinned to the human-readable shape.
    try:
        import esprima
    except ImportError:
        print('SKIP  landing-page JS parse (no esprima: pip install esprima)')
        return 0
    rc = 0
    pages = {}
    for sym in ('HTML_LANDING', 'HTML_TZ'):
        page, js, err = c_page(sym)
        if err:
            print(f'FAIL landing page: {err}'); return 1
        try:
            esprima.parseScript(js)
        except Exception as e:
            print(f'FAIL {sym} JS: {e}')
            idx = getattr(e, 'index', None)
            if isinstance(idx, int):
                print('      near: …' + js[max(0, idx - 70):idx + 70] + '…')
            return 1
        pages[sym] = (page, js)
    lpage, ljs = pages['HTML_LANDING']
    cpage, cjs = pages['HTML_TZ']

    # ---- landing: the shipped product decisions (see the h_wifi_ap and
    # HTML_LANDING comments for the reasoning behind each) ----
    for k, why in {'apbtn': 'AP-summon button (removed deliberately)',
                   'apgo': 'AP-summon JS (removed deliberately)',
                   'apoff': 'the old automatic AP close on render',
                   # the whole point of the split: nothing timezone-shaped
                   # belongs on the network page, not even as a leftover.
                   'Timezone': 'timezone section on the network page',
                   'id=tz': 'timezone dropdown on the network page',
                   'tzs': 'old timezone status line',
                   'tzgo': 'old set-zone button',
                   'tzx': 'old POSIX free-text box',
                   'tzbody': 'old join-piggybacked timezone',
                   'POSIX': 'POSIX rules shown to a person',
                   '__c__': 'old custom-rule dropdown value',
                   '<select': 'dropdown UI on the network page',
                   'resolvedOptions': 'phone timezone detection (belongs on the clock page only)'}.items():
        if k in lpage:
            print(f'FAIL landing page: contains {k!r} — {why}'); rc = 1
    for k, why in {"if(d.state=='connected'){success(d);return}":
                   'the success render does not ack the AP'}.items():
        if k not in ljs:
            print(f'FAIL landing page: missing {k!r} — {why}'); rc = 1
    for k, why in {'id=eye': 'password show/hide toggle button',
                   'id=done': 'confirmation block polling cannot clobber'}.items():
        if k not in lpage:
            print(f'FAIL landing page: missing {k!r} — {why}'); rc = 1
    for k, why in {"createElement('a')": 'the open-in-new-tab link',
                   "a.target='_blank'": 'that link opens a new tab, not over this page',
                   'orb clock: set your city': 'the link to the clock page',
                   'Copy address': 'the copy button',
                   'close setup network': 'the explicit user ack that closes the AP',
                   'Switch your phone back to your own wifi': 'instructions to leave the AP',
                   'addr.readOnly=true': 'a selectable address box for manual copy',
                   "JSON.stringify({ssid:sel,pass:p})": 'join sends only the network choice',
                   "document.execCommand('copy')": 'the copy path that works on http',
                   'navigator.clipboard': 'the modern copy path, tried when present',
                   'clearInterval(mainTmr)': 'status polling stops on success',
                   "getElementById('eye')": "the toggle's click wiring",
                   "'hide':'show'": "the toggle's own label flips",
                   'if(scanning)return;': 'status() cannot overwrite the scan UX',
                   'fails<3': 'transient fetch loss is tolerated, not unreachable',
                   'wifi_reason_name': 'the 802.11 reason shown as a word'}.items():
        if k not in ljs:
            print(f'FAIL landing page: missing {k!r} — {why}'); rc = 1
    for k, why in {"?', reason '+d.wifi_reason": 'raw reason number shown to the user'}.items():
        if k in ljs:
            print(f'FAIL landing page: contains {k!r} — {why}'); rc = 1
    # Count-exact: apkill() posts twice (AP address + absolute-LAN retry for a
    # phone that already roamed home); EVERY one must be a simple request.
    # cors on the fallback means a preflight the orb never answers, i.e. the
    # ack silently never lands. A substring pin passed this mutation before.
    for k, want, why in [("mode:'no-cors'", 2,
                         'every ack post is a simple (no-preflight) request'),
                        ("/api/v1/wifi/ap", 2,
                         'both ack posts exist (AP address + LAN-address fallback)')]:
        got = ljs.count(k)
        if got != want:
            print(f'FAIL landing page: {k!r} appears {got}x, want {want}x — {why}'); rc = 1

    # ---- clock page: human-readable, phone-driven, zero typing ----
    for k, why in {'posix': 'POSIX shown on the clock page',
                   'POSIX': 'POSIX named on the clock page',
                   '<select': 'dropdown (the list is tap-one-row buttons now)',
                   '__c__': 'custom-rule case',
                   'tzx': 'free-text rule box',
                   'EST5EDT': 'a POSIX rule example on the page',
                   'Set zone': 'the old confirm button'}.items():
        if k in cpage:
            print(f'FAIL clock page: contains {k!r} — {why}'); rc = 1
    for k, why in {'Type your city': 'search box in the markup',
                   'id=msg': 'one plain status line',
                   'id=zs': 'the city list container'}.items():
        if k not in cpage:
            print(f'FAIL clock page: missing {k!r} — {why}'); rc = 1
    for k, why in {'resolvedOptions().timeZone': 'reads the phone\'s own zone',
                   'formatToParts': 'measures zone offsets in the browser',
                   'function matchPhone': 'phone-to-table matching before asking',
                   'your phone says ': 'announces the auto-set it is about to do',
                   'is that the right time': 'asks the one question that catches a bad rule',
                   'does not follow DST': 'the refused-synth row admits its limit',
                   'function synth': 'phone-side POSIX rule derivation for unlisted zones',
                   'function transY': "finds the phone zone's clock changes by bisect",
                   'function mrule': 'encodes a transition as an M rule',
                   'function offAt': 'offset probes at arbitrary instants',
                   'function gmtRule': 'the fixed-offset shape synthesis refuses to fake',
                   'function sym': 'a phone zone name becomes a safe rule token',
                   'for(var k=1;k<=2;k++)': 'the rule must hold for the NEXT TWO YEARS too',
                   'follows its clock changes': 'the synthesized row promises DST tracking it makes good on',
                   'matches your phone': 'the device echo agreed with the phone',
                   'did not stick': 'the device echo disagreed — said out loud',
                   'd.tz_offset==want': 'set-time compares the dial, not the echo of the request',
                   'return n==1?hit:null': 'offset matching only ever applies a UNANIMOUS answer',
                   'could not reach the orb': 'says so when the fetch dies',
                   'GMT': 'labels lead with the offset',
                   'oninput': 'the search box filters live'}.items():
        if k not in cjs:
            print(f'FAIL clock page: missing {k!r} — {why}'); rc = 1

    # A call to a function that does not exist is a RUNTIME fault: esprima
    # parses it happily and every string pin still passes while the phone just
    # shows a dead button. One real edit here dropped gmtRule's definition
    # while leaving its call site; only this check catches that class.
    for tag, js_ in (('landing', ljs), ('clock', cjs)):
        defs = set(re.findall(r'function ([A-Za-z_$][\w$]*)\s*\(', js_))
        used = _calls(js_)
        undef = sorted(used - defs - {'fetch', 'if', 'for', 'while', 'switch',
                                      'return', 'typeof', 'function', 'async',
                                      'await', 'new', 'catch', 'parseInt',
                                      'isNaN', 'setTimeout', 'setInterval',
                                      'clearTimeout', 'clearInterval'})
        if undef:
            print(f'FAIL {tag} page JS calls undefined functions: {undef} — '
                  'esprima cannot see this, the phone can'); rc = 1

    # ---- server side of the same decisions ----
    for k, why in {'s_joining': 'the joining state distinct from provisioning',
                   '"joining"': 'the joining state string the page keys on',
                   'wifi_reason_name': 'the reason rendered as a word',
                   'HTML_TZ': 'the clock page bytes',
                   'h_timezone': 'the clock page handler',
                   '"/timezone"': 'the clock page route',
                   'const char *iana': 'the canonical-name column the phone matches',
                   'const char *label': 'the city column a person reads',
                   '\\"iana\\":\\"': 'config serves the iana column (when it differs from the key)',
                   'strcmp(s_tz_tbl[i].iana, s_tz_tbl[i].name)': 'iana omitted where it repeats the key',
                   '\\"label\\":\\"%s\\"': 'config serves the label column'}.items():
        if k not in src:
            print(f'FAIL orb_net.c: missing {k!r} — {why}'); rc = 1

    # h_config_get's static json[] must survive the table it prints: every
    # column of every row counted at its real length + the two 64-char echo
    # strings, checked against the declared buffer. A truncation here is an
    # empty picker on the phone, which looks like a device fault.
    if len(rows) < 50:
        print(f'FAIL orb_net.c: the tap-the-city table is supposed to be GLOBAL '
              f'(>=50 rows), found {len(rows)} — new rows are machine-generated, '
              f'see the s_tz_tbl comment'); rc = 1
    for city in ('Asia/Kathmandu', 'Pacific/Chatham', 'America/Santiago',
                 'Africa/Cairo', 'Europe/Dublin', 'America/Anchorage'):
        if ('"' + city + '"') not in tbm.group(1):
            print(f'FAIL orb_net.c: curated row {city} is gone — a user there '
                  f'would silently lose the auto-match; rows are regenerated, not trimmed'); rc = 1
    FROZEN_KEYS = {'US/Eastern', 'US/Central', 'US/Mountain', 'US/Pacific',
                   'US/Arizona'}          # NVS-stored legacy keys: frozen contract
    for n, px, ia, la in rows:
        if n != ia and n not in FROZEN_KEYS:
            print(f'FAIL orb_net.c: row {n!r} is neither canonical (name==iana) nor a '
                  f'frozen legacy key — new rows key on the IANA name verbatim'); rc = 1
    if len({n for n, _, _, _ in rows}) != len(rows) or \
       len({ia for _, _, ia, _ in rows}) != len(rows):
        print('FAIL orb_net.c: duplicate zone key or iana name in the table'); rc = 1
    if any(len(px) > 64 or len(n) > 64 or not px.isprintable()
           for n, px, _, _ in rows):
        print('FAIL orb_net.c: a rule/key exceeds NET_TZ_MAX(64) or is not printable'); rc = 1
    else:
        ent = sum(31 + len(n) + len(px) + len(la) +
                  (9 + len(ia) if ia != n else 0) for n, px, ia, la in rows)
        worst = (len('{"zones":[') + ent + len(rows) - 1
                 + len('],"tz":"') + 64 + len('","posix":"') + 64
                 + len('","tz_offset":') + 6 + 1 + 32)   # tail + slack
        mb = re.search(r'static esp_err_t h_config_get.*?static char json\[(\d+)\]', src, re.S)
        cap = int(mb.group(1)) if mb else -1
        if cap < worst:
            print(f'FAIL orb_net.c: config json buffer {cap} B < computed worst case '
                  f'{worst} B — enlarge it or trim a column'); rc = 1

    # Every registered route must fit the httpd table cap: registration past
    # max_uri_handlers 404s with no log line at all.
    nroutes = src.count('{ .uri = "')
    mcap = re.search(r'max_uri_handlers\s*=\s*(\d+)', src)
    cap = int(mcap.group(1)) if mcap else -1
    if not (0 < nroutes <= cap):
        print(f'FAIL orb_net.c: {nroutes} routes vs max_uri_handlers {cap}'); rc = 1

    # P5.1 (§7 item 7): /api/v1/screen is the one route whose whole purpose is
    # measurement, so it must be gated in BOTH places — an ungated handler inside
    # a #if'd table entry still registers nothing (harmless), and a table entry
    # inside the #if over an ungated handler is the bug that actually ships: the
    # knob injector stays in the image and simply becomes unreachable by URL, so
    # nobody auditing the build notices it is still there.
    mscr = re.search(r'#if CONFIG_ORB_DEBUG_HTTP\s*\n\s*\{ \.uri = "/api/v1/screen"[^\n]*\n\s*#endif', src)
    if not mscr:
        print('FAIL orb_net.c: /api/v1/screen is not registered inside its own '
              'CONFIG_ORB_DEBUG_HTTP guard (plan §7 item 7)'); rc = 1
    hsc = re.search(r'(?:/\*[^/]*?\*/\s*)*#if CONFIG_ORB_DEBUG_HTTP\s*\n\s*static esp_err_t h_screen', src)
    if not hsc:
        print('FAIL orb_net.c: h_screen() is not wrapped in #if CONFIG_ORB_DEBUG_HTTP, '
              'so a build that drops the route still carries the knob injector'); rc = 1

    # P5.1 (§7 item 7): h_sd()'s row loop must test the bound BEFORE writing each
    # row. snprintf returns the length it WOULD have written, so with the check
    # only after the loop the next iteration's `sizeof json - (size_t)off` wraps
    # to a huge size_t and the write runs off the end of a 6 KB static.
    if 'static esp_err_t h_sd' in src:
        isd = src.index('static esp_err_t h_sd')
        body = src[isd:src.index('\nstatic ', isd + 10)]
        iro = body.index('for (int i = 0; n > 0 && i < n; i++)')
        itail = body.find('off += snprintf(json + off, sizeof json - (size_t)off, "]}")', iro)
        loop = body[iro:itail if itail > 0 else len(body)]
        if 'if (off < 0 || (size_t)off >= sizeof json)' not in loop:
            print('FAIL orb_net.c: h_sd() no longer checks the row bound inside the '
                  'loop — the post-loop check catches the truncation and misses the '
                  'overrun (theme-naming-todo §5, plan §7 item 7)'); rc = 1
        if 'too_big' not in loop:
            print('FAIL orb_net.c: h_sd() breaks out of its row loop without recording '
                  'that it overflowed, so the error body is unreachable'); rc = 1

    if rc:
        return rc
    print(f'ok    landing ({len(ljs)} B) + clock ({len(cjs)} B) JS parse + call linter; '
          f'tz UI off the network page, {len(rows)} zone rows, {nroutes} routes in cap, '
          f'config body fits')
    return 0

MUTATIONS = [
    # name, old text in orb_net.c, new text — each must make SOME check fail.
    # The battery's whole purpose: a pin you can delete without any red line
    # is not a pin. Renames target the linter, deletions target copies.
    ('synth engine disappears', 'function synth(zn){var y=', 'function synthX(zn){var y='),
    ('transition finder gone', 'function transY(zn,y){', 'function transYx(zn,y){'),
    ('gmtRule def dropped but call kept', 'function gmtRule(m){', 'function gmtRuleQ(m){'),
    ('lbl renamed (call site survives)', 'function lbl(z){', 'function lblQ(z){'),
    ('rule trusted for one year only', 'for(var k=1;k<=2;k++)', 'for(var k=1;k<=0;k++)'),
    ('echo check rubber-stamped', 'd.tz_offset==want', 'd.tz_offset==d.tz_offset'),
    ('refused zone hides its limit', ' (does not follow DST automatically)', ' (probably fine)'),
    ('ambiguous offset auto-applied', 'return n==1?hit:null', 'return hit?hit:null'),
    ('posix text leaks to the page', " (your phone, follows its clock changes)",
     " (your phone posix)"),
    ('curated city hand-deleted', 'Asia/Kathmandu', 'XX/Deleted', 2),
    ('rule hand-edited into wrongness', 'CET-1CEST,M3.5.0,M10.5.0/3',
     'CET-1CEST,M3.5.0,M10.4.0/3'),
    ('iana never emitted', '? "" : "\\"iana\\":\\"",', '? "" : "",'),
    ('config buffer shrunk', 'static char json[6144];\n\n    httpd_resp_set_type(req, "application/json");', 'static char json[2048];\n\n    httpd_resp_set_type(req, "application/json");'),
    # P5.1's two debt fixes, mutated at the shipped site. Each is the regression
    # the fix exists to close, and each must turn a lint above red: the bound
    # check back outside the row loop (the overrun theme-naming-todo §5 named),
    # and the debug door registered without its guard.
    ('h_sd overflow check back after the loop',
     '        if (off < 0 || (size_t)off >= sizeof json) { too_big = true; break; }\n', ''),
    ('the screen door loses its guard',
     '#if CONFIG_ORB_DEBUG_HTTP\n        { .uri = "/api/v1/screen",     .method = HTTP_GET,  .handler = h_screen },\n#endif',
     '        { .uri = "/api/v1/screen",     .method = HTTP_GET,  .handler = h_screen },'),
    ('the screen handler stays compiled in',
     '#if CONFIG_ORB_DEBUG_HTTP\nstatic esp_err_t h_screen(httpd_req_t *req)',
     'static esp_err_t h_screen(httpd_req_t *req)'),
    # Every registered route must fit the httpd table cap. The anchor is read from
    # the source so the battery cannot go stale when a phase raises the number
    # (29 routes registered as of P4a). Lowering the cap past the count is the bug:
    # httpd_register_uri_handler fails and the route 404s forever with nothing in
    # the log — and the live lint above already refuses cap < count.
    ('httpd route cap lowered',
     'cfg.max_uri_handlers  = %s;' % re.search(r'max_uri_handlers\s*=\s*(\d+)',
                                               src).group(1),
     'cfg.max_uri_handlers  = 12;'),
    # Ruling D13's two predicates, mutated in the file that ships them. Each is the
    # failure the ruling was written to remove, and each must turn a row of the
    # table above red: an install aimed at the factory bank, a boot that draws a
    # slot it never verified, and a boot that ignores the choice it was given.
    ('an install aims at the factory bank',
     'static uint32_t install_target(void) { return SLOT_B_HDR; }',
     'static uint32_t install_target(void) { return SLOT_A_HDR; }', 1, 'theme'),
    ('boot draws a slot it never verified',
     'return b_valid ? BOOT_ART_CARD : BOOT_ART_DEFAULT;',
     '(void)b_valid;\n    return BOOT_ART_CARD;', 1, 'theme'),
    ('the owner\'s choice stops mattering',
     'if (!live || strcmp(live, THEME_LIVE_CARD) != 0) return BOOT_ART_DEFAULT;',
     'if (!live) return BOOT_ART_DEFAULT;', 1, 'theme'),
]

def run_mutations():
    rc = 0
    for i, entry in enumerate(MUTATIONS):
        name, old, new = entry[0], entry[1], entry[2]
        want = entry[3] if len(entry) > 3 else 1
        target = entry[4] if len(entry) > 4 else 'net'
        base = src if target == 'net' else theme_src
        if base.count(old) != want:
            print(f'FAIL mutation {name!r}: anchor found {base.count(old)}x in the '
                  f'{"orb_net.c" if target == "net" else "orb_theme.c"} source, want {want}x '
                  f'— the mutation is stale, fix the battery'); rc = 1; continue
        mut = '/tmp/orb_%s_mut_%d.c' % (target, i)
        open(mut, 'w').write(base.replace(old, new))
        argv = [sys.executable, __file__]
        argv += ['--src', mut] if target == 'net' else ['--theme-src', mut]
        r = subprocess.run(argv, capture_output=True, text=True)
        if r.returncode == 0:
            print(f'FAIL  mutation {name!r} SURVIVED — a real regression would pass '
                  f'the suite too'); print(r.stdout[-1200:]); rc = 1
        else:
            first = next((l for l in r.stdout.splitlines() if l.startswith('FAIL')), '?')
            print(f'ok    mutation {name!r} caught: {first[:96]}')
    return rc

if '--mutations' in sys.argv:
    sys.exit(run_mutations())
if ZoneInfo is not None and missing_zones:
    for mz in missing_zones:
        print('FAIL orb_net.c: zone table names a zone the IANA database does not know:', mz)
    sys.exit(1)
rc = landing_page_js()
if rc:
    sys.exit(rc)
sys.exit(subprocess.run([BIN]).returncode)


