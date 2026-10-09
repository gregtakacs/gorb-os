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
# The captive-portal DNS reply builder, lifted for the same reason as the ladder
# and the self-inflicted guard: it is exactly the code this bench cannot exercise
# (no WiFi radio to send a real query), and every byte is a silent failure mode —
# a wrong offset is a phone that never shows "Sign in to network" and a device
# that logs nothing wrong. So the encoding is pinned here instead.
dns_fn = lift(r"static int dns_build_reply\(const uint8_t \*rx.*?return len;\n\}")
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
{dns_fn}
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
open("/tmp/tzjson_host.c", "w").write(harness)
cc = subprocess.run(["gcc", "-O1", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Wno-unused-function",
                     "-Werror", "-o", "/tmp/tzjson_host", "/tmp/tzjson_host.c"])
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
    bad, in_comment = [], False
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
        if not re.fullmatch(r'"(?:[^"\\]|\\.)*";?', t):
            bad.append((where, t[:64]))
    if bad:
        for where, t in bad[:6]:
            print(f'FAIL {sym}: line {where} is neither a comment nor a complete '
                  f'C string fragment (unbalanced quote?): {t!r}')
        print('       the extractor skips such lines, so they were NOT covered '
              'by the parse/pin checks below.')
        return None, None, sym + ' fragment integrity'
    import ast
    fragments = re.findall(r'"((?:[^"\\\n]|\\.)*)"', region)
    if not fragments:
        return None, None, f'no string fragments inside {sym}'
    page = ''.join(ast.literal_eval('"' + f + '"') for f in fragments)
    js = re.search(r'<script>(.*?)</script>', page, re.S)
    if not js:
        return None, None, f'no <script> block in {sym}'
    return page, js.group(1), None


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
                   'does not follow DST': 'the fallback row admits its limit',
                   'could not reach the orb': 'says so when the fetch dies',
                   'GMT': 'labels lead with the offset',
                   'oninput': 'the search box filters live'}.items():
        if k not in cjs:
            print(f'FAIL clock page: missing {k!r} — {why}'); rc = 1

    # ---- server side of the same decisions ----
    for k, why in {'s_joining': 'the joining state distinct from provisioning',
                   '"joining"': 'the joining state string the page keys on',
                   'wifi_reason_name': 'the reason rendered as a word',
                   'HTML_TZ': 'the clock page bytes',
                   'h_timezone': 'the clock page handler',
                   '"/timezone"': 'the clock page route',
                   'const char *iana': 'the canonical-name column the phone matches',
                   'const char *label': 'the city column a person reads',
                   '\\"iana\\":\\"%s\\"': 'config serves the iana column',
                   '\\"label\\":\\"%s\\"': 'config serves the label column'}.items():
        if k not in src:
            print(f'FAIL orb_net.c: missing {k!r} — {why}'); rc = 1

    # h_config_get's static json[] must survive the table it prints: every
    # column of every row counted at its real length + the two 64-char echo
    # strings, checked against the declared buffer. A truncation here is an
    # empty picker on the phone, which looks like a device fault.
    tb = re.search(r'static const tz_entry_t s_tz_tbl\[\] = \{(.*?)\n\};', src, re.S)
    rows = re.findall(r'\{\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\s*,?\s*\}',
                      tb.group(1)) if tb else []
    if len(rows) < 9:
        print(f'FAIL orb_net.c: expected 9 four-column zone rows, found {len(rows)}'); rc = 1
    else:
        ent = sum(43 + len(n) + len(px) + len(ia) + len(la) for n, px, ia, la in rows)
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

    if rc:
        return rc
    print(f'ok    landing ({len(ljs)} B) + clock ({len(cjs)} B) JS parse; tz UI off the '
          f'network page, {nroutes} routes in cap, config body fits')
    return 0

rc = landing_page_js()
if rc:
    sys.exit(rc)
sys.exit(subprocess.run(["/tmp/tzjson_host"]).returncode)


