#!/usr/bin/env python3
"""host_mdns_test.py — the _orb._tcp wire format, tested on the host.

Why this file exists
--------------------
Item 4 of docs/orb-manager-plan.md §5.3 hands discovery to mDNS, and a browse is
the one thing on this bench that cannot be watched: the answer is judged by a
browser somewhere else, on bytes nobody here can see go wrong. A malformed RDLENGTH
or a mis-set cache-flush bit does not crash the orb — it produces an orb that is
running fine, logging fine, and simply never appears in the hub's list. That is
the failure mode this repo calls a false pass, and it is invisible until someone
tries to add a device.

So the shipped codec is lifted VERBATIM out of main/net/orb_mdns.c between the
"1. codec" and "2. plumbing" banners and compiled here against real query bytes —
not re-typed in Python, which would test my reading of DNS rather than the
firmware's. If the lift ever comes back empty the suite dies loudly, because a
harness quietly testing a stale copy is worse than no harness.

What is under test
------------------
  the four records a browser needs   PTR/SRV/TXT/A present, in order, with the
                                     right owner names, and the walk landing
                                     EXACTLY on the end of the message (an
                                     RDLENGTH that lies is undetectable by
                                     everything except this walk)
  the identity payload               mac=, fw= and state=, byte for byte, and the
                                     instance suffix agreeing with the mac rather
                                     than being maintained beside it
  the port                           the SRV port is CONFIG_ORB_NET_HTTPD_PORT,
                                     read from the firmware's own Kconfig, so the
                                     hub can dial what a browse promised
  one socket, two interfaces         the A record follows the QUERIER's subnet,
                                     which is the substitute for the IP_PKTINFO
                                     this lwIP declares but never implements
  "not ours to answer"               foreign names, foreign classes, opcodes we
                                     do not serve, responses, and multi-question
                                     packets all return zero — silence, not a
                                     wrong answer
  malformed input                    truncated headers, labels running past the
                                     datagram, forward and self-referential
                                     compression pointers: every one refused, and
                                     compiled under ASan so an over-read is fatal
                                     rather than merely unlucky
  the cap rule                       every buffer smaller than the answer returns
                                     0. Half a response parses and gets cached for
                                     the whole TTL, so this is a safety property,
                                     not a tidiness one

Run:  python3 tools/host_mdns_test.py
Mutations:  python3 tools/host_mdns_test.py --mutations   (each must fail the suite)
"""

import os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "net", "orb_mdns.c")
HDR = os.path.join(ROOT, "main", "net", "orb_mdns.h")
NET = os.path.join(ROOT, "main", "net", "orb_net.c")
KCONF = os.path.join(ROOT, "main", "Kconfig")

src = open(SRC).read()
hdr = open(HDR).read()
net = open(NET).read()
kconf = open(KCONF).read()

# ------------------------------------------------------------------ the lift
BEGIN = "/* ------------------------------------------------------------------ 1. codec */"
END   = "/* ---------------------------------------------------------------- 2. plumbing */"
assert BEGIN in src, "the '1. codec' banner is gone from orb_mdns.c: the lift cannot be trusted"
assert END in src,   "the '2. plumbing' banner is gone from orb_mdns.c: the lift cannot be trusted"
codec = src.split(BEGIN, 1)[1].split(END, 1)[0]
assert len(codec) > 4000, f"lifted only {len(codec)} B of codec — too small to be the real thing"
for fn in ("orb_mdns_build_answer", "orb_mdns_build_announce", "name_match",
           "classify_query", "emit_records"):
    assert fn in codec, f"{fn} is not inside the lifted region"

# The firmware's own answers to two questions the harness must not answer itself.
PORT = re.search(r'config ORB_NET_HTTPD_PORT.*?default\s+(\d+)', kconf, re.S)
PORT = int(PORT.group(1)) if PORT else 80
MAC_FMT = re.search(r'snprintf\(mac_str, sizeof\(mac_str\),\s*\n?\s*"([^"]+)"', net)
assert MAC_FMT, "h_status's mac format vanished: the TXT can no longer be pinned to it"
assert "%02x%02x%02x%02x%02x%02x" in MAC_FMT.group(1), \
    "h_status no longer prints the mac as 12 lowercase hex digits; orb_mdns's TXT must follow it"

# ---------------------------------------------------------------- the harness
# A query-builder and a strict record-walker, both written here rather than
# borrowed: the walker is what catches an RDLENGTH that lies, and it must be
# independent of the encoder's own bookkeeping to count as evidence.
HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct { char instance[24]; char hostname[32]; char mac[13]; char fw[32];
                 _Bool provisioning; } orb_mdns_id_t;
#define CONFIG_ORB_NET_HTTPD_PORT __PORT__

__CODEC__

/* ------------------------------------------------------------------ helpers */
static int fails = 0, checks = 0;
static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("  FAIL: %s\n", what); }
}

/* A DNS name, decompressed, lowercased, dots. Independent of the encoder. */
static int get_name(const uint8_t *r, int n, int off, char *out, int cap)
{
    int hops = 0, consumed = off;
    out[0] = 0;
    int used = 0;
    for (;;) {
        if (off < 0 || off >= n) return 0;
        uint8_t len = r[off];
        if (len == 0) return 1;
        if ((len & 0xC0) == 0xC0) {
            if (off + 2 > n) return 0;
            int t = ((len & 0x3F) << 8) | r[off + 1];
            if (t >= consumed || ++hops > 8) return 0;
            off = t;
            continue;
        }
        if ((len & 0xC0) != 0) return 0;
        if (off + 1 + len > n) return 0;
        if (used + len + 2 > cap) return 0;
        if (used) out[used++] = '.';
        for (int i = 0; i < len; i++) {
            char c = (char)r[off + 1 + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out[used++] = c;
        }
        out[used] = 0;
        off += 1 + len;
        if (off > consumed) consumed = off;
    }
}

/* Walk the answer + additional records from `off`, filling each owner name and
 * its type. Returns the count, or -1 on any malformed step — including the end
 * not landing exactly on the message length, which is how a lying RDLENGTH is
 * caught. */
typedef struct { char owner[96]; int type, cls; uint32_t ttl; int rd_off, rd_len; } rr_t;
static int walk(const uint8_t *r, int n, int off, rr_t *out, int max)
{
    int cnt = 0;
    for (; cnt < max; cnt++) {
        rr_t *e = &out[cnt];
        e->owner[0] = 0;
        int after = off;
        if (!get_name(r, n, off, e->owner, (int)sizeof e->owner)) return -1;
        /* advance the raw offset the same way a parser must */
        int o = off, hops = 0, consumed = off;
        for (;;) {
            if (o >= n) return -1;
            uint8_t l = r[o];
            if (l == 0) { o++; break; }
            if ((l & 0xC0) == 0xC0) {
                if (o + 2 > n) return -1;
                int t = ((l & 0x3F) << 8) | r[o + 1];
                if (t >= consumed || ++hops > 8) return -1;
                o += 2;
                break;
            }
            if ((l & 0xC0) != 0) return -1;
            o += 1 + l; if (o > n) return -1;
            if (o > consumed) consumed = o;
        }
        after = o;
        if (after + 10 > n) return -1;
        /* TYPE, CLASS, TTL, RDLENGTH — CLASS at +2, not folded away: the
         * cache-flush bit lives in CLASS, and which records may carry it is a
         * rule a responder gets wrong in silence. */
        e->type = (r[after] << 8) | r[after + 1];
        e->cls  = (r[after+2] << 8) | r[after + 3];
        e->ttl  = ((uint32_t)r[after+4] << 24) | ((uint32_t)r[after+5] << 16)
                | ((uint32_t)r[after+6] << 8) | r[after+7];
        int rd = (r[after + 8] << 8) | r[after + 9];
        e->rd_off = after + 10;
        e->rd_len = rd;
        off = e->rd_off + rd;
        if (off > n) return -1;
        if (off == n) { return cnt + 1; }
    }
    return (off == n) ? cnt : -1;
}

static int mk_query(const char *name, int qtype, uint8_t *q, int cap)
{
    if (cap < 12) return 0;
    memset(q, 0, 12);
    q[0] = 0xAB; q[1] = 0xCD;                /* id, so the echo can be checked   */
    q[4] = 0; q[5] = 1;                      /* QDCOUNT = 1: a query with no
                                              * question is a packet no responder
                                              * should answer, and the firmware
                                              * rightly refuses it. */
    int off = 12;
    const char *p = name;
    while (*p) {
        const char *d = strchr(p, '.');
        int l = d ? (int)(d - p) : (int)strlen(p);
        if (l <= 0 || l > 63 || off + 1 + l > cap) return 0;
        q[off++] = (uint8_t)l;
        memcpy(q + off, p, (size_t)l); off += l;
        p += l; if (*p == '.') p++; else break;
    }
    if (off + 5 > cap) return 0;
    q[off++] = 0;
    q[off++] = (uint8_t)(qtype >> 8); q[off++] = (uint8_t)(qtype & 0xFF);
    q[off++] = 0;     q[off++] = 1;                      /* CLASS IN             */
    return off;
}

static orb_mdns_id_t test_id(void)
{
    orb_mdns_id_t id;
    strcpy(id.instance, "orb-782618");
    strcpy(id.hostname, "orb-782618.local");
    strcpy(id.mac,      "44bd8d782618");
    strcpy(id.fw,       "f716ffa");
    id.provisioning = 0;
    return id;
}

#define STA_IP 0x2202A8C0u      /* 192.168.2.34, network order on disk here     */
#define AP_IP  0x0104A8C0u      /* 192.168.4.1                                  */
#define LAN_SRC 0x6402A8C0u     /* a hub asking from 192.168.2.100              */
#define AP_SRC  0x2304A8C0u     /* a phone asking from 192.168.4.35             */

static uint32_t ip_of(const char *d)
{
    int a,b,c,e; sscanf(d, "%d.%d.%d.%d", &a,&b,&c,&e);
    return (uint32_t)(a | (b<<8) | (c<<16) | (e<<24));
}

/* ------------------------------------------------------------------ the tests */
static void t_browse_ptr(void)
{
    orb_mdns_id_t id = test_id();
    uint8_t q[256], tx[512];
    const int n = mk_query("_orb._tcp.local", 12 /*PTR*/, q, (int)sizeof q);
    ck(n > 0, "query builder produced a PTR query");
    const int len = orb_mdns_build_answer(&id, STA_IP, AP_IP, LAN_SRC, q, n, tx, (int)sizeof tx);
    ck(len > 0, "a browse for _orb._tcp.local is answered");
    if (len <= 0) return;
    ck(tx[0] == 0xAB && tx[1] == 0xCD, "the query id is echoed");
    ck(tx[2] == 0x84 && tx[3] == 0x00, "QR|AA set, recursion bits clear");
    ck(tx[4] == 0 && tx[5] == 1, "the one question is echoed");
    ck(tx[6] == 0 && tx[7] == 1, "ANCOUNT is exactly the one PTR");
    ck(tx[8] == 0 && tx[9] == 3, "SRV, TXT and A follow as ADDITIONAL");
    ck(tx[10] == 0 && tx[11] == 0, "no nameservers offered");

    rr_t rr[8];
    const int cnt = walk(tx, len, 12 + (n - 12), rr, 8);
    ck(cnt == 4, "all four records walk clean to the exact end of the message");
    if (cnt != 4) { printf("  (walk returned %d)\n", cnt); return; }
    ck(rr[0].type == 12 && !strcmp(rr[0].owner, "_orb._tcp.local"), "PTR owned by the service");
    ck((rr[0].cls & 0x8000) == 0, "PTR is NOT cache-flush: several orbs share one service name");
    char tgt[96];
    ck(get_name(tx, len, rr[0].rd_off, tgt, (int)sizeof tgt) &&
       !strcmp(tgt, "orb-782618._orb._tcp.local"), "PTR points at this orb's instance");
    ck(rr[1].type == 33 && !strcmp(rr[1].owner, "orb-782618._orb._tcp.local"), "SRV owned by the instance");
    ck((rr[1].cls & 0x8000) != 0, "SRV is cache-flush: the name is unique to this orb");
    ck(rr[2].type == 16, "TXT present");
    ck((rr[2].cls & 0x8000) != 0, "TXT is cache-flush");
    ck(rr[3].type == 1 && !strcmp(rr[3].owner, "orb-782618.local"), "A owned by the hostname");
    ck((rr[3].cls & 0x8000) != 0, "A is cache-flush");
    for (int i = 0; i < 4; i++) ck(rr[i].ttl == 120, "TTL is RFC 6762's 120, not an invented value");
    /* The SRV must name the port the hub will dial, and the target it must dial. */
    ck(((tx[rr[1].rd_off] << 8) | tx[rr[1].rd_off + 1]) == 0 &&
       ((tx[rr[1].rd_off + 2] << 8) | tx[rr[1].rd_off + 3]) == 0,
       "SRV priority and weight are zero: one instance, no load balancing to invent");
    ck(((tx[rr[1].rd_off + 4] << 8) | tx[rr[1].rd_off + 5]) == __PORT__,
       "SRV port is the firmware's own CONFIG_ORB_NET_HTTPD_PORT");
    char host[96];
    ck(get_name(tx, len, rr[1].rd_off + 6, host, (int)sizeof host) &&
       !strcmp(host, "orb-782618.local"), "SRV target is the hostname the A record answers for");
    /* The A record's four octets, in wire order. */
    ck(rr[3].rd_len == 4 && tx[rr[3].rd_off] == 192 && tx[rr[3].rd_off+1] == 168 &&
       tx[rr[3].rd_off+2] == 2   && tx[rr[3].rd_off+3] == 34,
       "a hub on the LAN is given the STA lease, not the SoftAP address");
}

static void t_txt_payload(void)
{
    orb_mdns_id_t id = test_id();
    uint8_t q[256], tx[512];
    const int n = mk_query("_orb._tcp.local", 12, q, (int)sizeof q);
    const int len = orb_mdns_build_answer(&id, STA_IP, AP_IP, LAN_SRC, q, n, tx, (int)sizeof tx);
    if (len <= 0) { ck(0, "TXT test: no answer to read"); return; }
    rr_t rr[8];
    if (walk(tx, len, 12 + (n - 12), rr, 8) != 4) { ck(0, "TXT test: walk failed"); return; }
    char joined[160] = {0};
    int p = 0;
    for (int i = 0; i < rr[2].rd_len; ) {
        const int l = tx[rr[2].rd_off + i];
        if (l <= 0 || i + 1 + l > rr[2].rd_len) { ck(0, "TXT is not a well-formed series of counted strings"); return; }
        if (p) joined[p++] = ' ';
        memcpy(joined + p, tx + rr[2].rd_off + i + 1, (size_t)l);
        p += l; i += 1 + l;
    }
    ck(!strcmp(joined, "mac=44bd8d782618 fw=f716ffa state=normal"),
       "TXT is exactly mac=, fw= and state=, spelled as h_status spells them");
    /* The whole reason the payload exists: the MAC here must be the MAC the hub
     * reads back from /api/v1/status, or item 6's cross-lock compares two
     * different spellings of one chip and locks nothing. */
    ck(strstr(joined, "mac=") != NULL && !strncmp(joined, "mac=44bd8d782618", 16),
       "the announced mac is the BASE mac, lowercase, unseparated");
    /* instance suffix vs mac tail, so the two cannot be maintained separately. */
    ck(!strcmp(id.instance + 4, id.mac + 6), "the instance name's tail IS the mac's tail");

    id.provisioning = 1;
    const int l2 = orb_mdns_build_answer(&id, 0, AP_IP, AP_SRC, q, n, tx, (int)sizeof tx);
    rr_t r2[8];
    ck(l2 > 0 && walk(tx, l2, 12 + (n - 12), r2, 8) == 4, "a provisioning orb still answers");
    char j2[160] = {0}; int p2 = 0;
    for (int i = 0; i < r2[2].rd_len; ) {
        const int l = tx[r2[2].rd_off + i];
        if (p2) j2[p2++] = ' ';
        memcpy(j2 + p2, tx + r2[2].rd_off + i + 1, (size_t)l);
        p2 += l; i += 1 + l;
    }
    ck(strstr(j2, "state=provisioning") != NULL,
       "after a factory reset the announcement says provisioning, which is how item 10 is findable");
    ck(r2[3].rd_len == 4 && tx[r2[3].rd_off+2] == 4,
       "with no lease the A record offers the SoftAP address rather than 0.0.0.0");
}

static int a_octets(const orb_mdns_id_t *id, uint32_t sta, uint32_t ap, uint32_t src,
                    uint8_t *out4)
{
    uint8_t q[256], tx[512];
    const int n = mk_query("orb-782618.local", 1 /*A*/, q, (int)sizeof q);
    const int len = orb_mdns_build_answer(id, sta, ap, src, q, n, tx, (int)sizeof tx);
    if (len <= 0) return 0;
    rr_t rr[4];
    if (walk(tx, len, 12 + (n - 12), rr, 4) < 1) return 0;
    if (rr[0].type != 1 || rr[0].rd_len != 4) return 0;
    memcpy(out4, tx + rr[0].rd_off, 4);
    return 1;
}

static void t_per_interface_a(void)
{
    orb_mdns_id_t id = test_id();
    uint8_t o[4];
    ck(a_octets(&id, STA_IP, AP_IP, LAN_SRC, o) && o[0]==192 && o[1]==168 && o[2]==2 && o[3]==34,
       "a querier on the LAN is sent to the STA lease");
    ck(a_octets(&id, STA_IP, AP_IP, AP_SRC, o) && o[0]==192 && o[1]==168 && o[2]==4 && o[3]==1,
       "a querier on the SoftAP is sent to the AP address: it has no route to the lease");
    ck(a_octets(&id, 0, AP_IP, LAN_SRC, o) && o[2]==4,
       "with no lease at all even a LAN-side querier gets the AP, never 0.0.0.0");
    /* Both interfaces up, and the answer must still follow the asker rather
     * than whichever interface happened to come up first. This is the whole
     * compensation for the missing IP_PKTINFO, so it is asserted three ways. */
    ck(a_octets(&id, STA_IP, AP_IP, AP_SRC, o) && o[3]==1,
       "AP-up + STA-up + asking from the AP: the AP wins on source, not on order");
}

static int asks(const orb_mdns_id_t *id, const uint8_t *q, int n)
{
    uint8_t tx[512];
    return orb_mdns_build_answer(id, STA_IP, AP_IP, LAN_SRC, q, n, tx, (int)sizeof tx);
}

static void t_not_ours(void)
{
    orb_mdns_id_t id = test_id();
    uint8_t q[256];
    int n;
    n = mk_query("_http._tcp.local", 12, q, (int)sizeof q);
    ck(asks(&id, q, n) == 0, "a browse for someone else's service is not answered");
    n = mk_query("orb-deadbe._orb._tcp.local", 33, q, (int)sizeof q);
    ck(asks(&id, q, n) == 0, "another orb's instance is not answered: we are not its authority");
    n = mk_query("printer.local", 1, q, (int)sizeof q);
    ck(asks(&id, q, n) == 0, "another host's A record is not answered");

    /* Header-level refusals, each built by hand off a good query. */
    n = mk_query("_orb._tcp.local", 12, q, (int)sizeof q);
    uint8_t bad[256]; memcpy(bad, q, (size_t)n);
    bad[2] |= 0x80;                                   /* QR: this is a response */
    ck(asks(&id, bad, n) == 0, "a response is never answered: that is the echo loop");
    memcpy(bad, q, (size_t)n); bad[2] = (uint8_t)(bad[2] | 0x10);   /* opcode 1 */
    ck(asks(&id, bad, n) == 0, "an inverse query is refused");
    memcpy(bad, q, (size_t)n); bad[5] = 0;                          /* QDCOUNT 0*/
    ck(asks(&id, bad, n) == 0, "a questionless packet is refused");
    memcpy(bad, q, (size_t)n); bad[5] = 2;                          /* QDCOUNT 2*/
    ck(asks(&id, bad, n) == 0, "a multi-question packet is refused rather than half answered");
    memcpy(bad, q, (size_t)n); bad[3 + 0] ^= 0; bad[n - 1] = 3;    /* CLASS CHAOS */
    ck(asks(&id, bad, n) == 0, "a non-IN class question is refused");
    memcpy(bad, q, (size_t)n); bad[9] = 1;                          /* ANCOUNT != 0 */
    ck(asks(&id, bad, n) == 0, "a query that already carries answers is refused");
    for (int cut = 0; cut < n; cut++) {
        if (asks(&id, q, cut) != 0) { ck(0, "a truncated query produced an answer"); break; }
    }
    ck(1, "every truncation of a good query, 0 up to just short, is refused");
}

static void t_malformed_names(void)
{
    orb_mdns_id_t id = test_id();
    uint8_t q[256];
    /* label length that runs past the datagram */
    q[0]=0xAB; q[1]=0xCD; q[2]=0; q[3]=0; q[4]=0; q[5]=1;
    for (int i=6;i<12;i++) q[i]=0;
    q[12] = 200;
    ck(asks(&id, q, 14) == 0, "a 200-byte label in a 14-byte packet is refused");
    /* compression pointer pointing FORWARD (a loop waiting to happen) */
    q[12] = 0xC0; q[13] = 0x40;                       /* -> offset 64, past end  */
    q[14] = 0; q[15] = 0; q[16] = 12; q[17] = 0; q[18] = 1;
    ck(asks(&id, q, 19) == 0, "a forward compression pointer is refused");
    /* self-referential pointer: the classic spin */
    q[12] = 0xC0; q[13] = 12;
    q[14] = 0; q[15] = 0; q[16] = 12; q[17] = 0; q[18] = 1;
    ck(asks(&id, q, 19) == 0, "a self-referential pointer is refused, not followed");
    /* reserved label form 0x80..0xBF */
    q[12] = 0x80; q[13] = 0x00; q[14] = 0; q[15] = 0; q[16] = 12; q[17] = 0; q[18] = 1;
    ck(asks(&id, q, 19) == 0, "a reserved label form is refused");
    /* case-insensitivity must hold: avahi and dns-sd do not agree on case */
    uint8_t qq[256];
    const int n = mk_query("_ORB._TCP.LOCAL", 12, qq, (int)sizeof qq);
    ck(asks(&id, qq, n) > 0, "an upper-case browse is still answered");
}

static void t_unicast_query_bit(void)
{
    /* RFC 6762 2.4 puts the UNICAST-REQUEST flag in the TOP HALF of the
     * question class, so a real browser asks with 0x8001. Comparing the class
     * against a bare 1 answers the hand-built test queries and refuses every
     * browser on the floor, which is what happened on the bench: the log said
     * the orb was advertising and no tool could find it. */
    orb_mdns_id_t id = test_id();
    uint8_t q[256], tx[512];
    const int n = mk_query("_orb._tcp.local", 12, q, (int)sizeof q);
    ck(n > 13, "query built");
    q[n - 1] = 1; q[n - 2] = 0x80;                    /* CLASS = IN | QU */
    const int len = orb_mdns_build_answer(&id, STA_IP, AP_IP, LAN_SRC, q, n, tx, (int)sizeof tx);
    ck(len > 0, "a browse asking for a unicast reply (class 0x8001) is ANSWERED");
    q[n - 2] = 0; q[n - 1] = 3;                       /* CLASS = CHAOS */
    ck(orb_mdns_build_answer(&id, STA_IP, AP_IP, LAN_SRC, q, n, tx, (int)sizeof tx) == 0,
       "and a genuinely foreign class still is not");
}
static void t_cap_and_goodbye(void)
{
    orb_mdns_id_t id = test_id();
    uint8_t q[256], tx[512], ref[512];
    const int n = mk_query("_orb._tcp.local", 12, q, (int)sizeof q);
    const int full = orb_mdns_build_answer(&id, STA_IP, AP_IP, LAN_SRC, q, n, ref, (int)sizeof ref);
    ck(full > 0 && full <= 512, "the whole browse answer fits DNS's 512-byte UDP cap");
    int short_fails = 0;
    for (int cap = 0; cap < full; cap++) {
        memset(tx, 0xEE, sizeof tx);
        if (orb_mdns_build_answer(&id, STA_IP, AP_IP, LAN_SRC, q, n, tx, cap) != 0) short_fails++;
    }
    ck(short_fails == 0, "no buffer too small ever yields a partial packet (half an answer parses, then caches)");
    memset(tx, 0xEE, sizeof tx);
    ck(orb_mdns_build_answer(&id, STA_IP, AP_IP, LAN_SRC, q, n, tx, full) == full,
       "the exact-size buffer yields the whole packet, byte for byte");
    ck(memcmp(tx, ref, (size_t)full) == 0, "and it is the same packet");

    uint8_t g[512];
    const int gl = orb_mdns_build_announce(&id, STA_IP, true, g, (int)sizeof g);
    ck(gl > 0, "a goodbye encodes");
    if (gl > 0) {
        rr_t rr[4];
        const int c = walk(g, gl, 12, rr, 4);       /* announce has no question */
        ck(c >= 1 && rr[0].ttl == 0, "goodbye records carry TTL 0, which is the whole point");
        ck(g[2] == 0x84, "goodbye is still an authoritative answer");
    }
    uint8_t a[512];
    const int al = orb_mdns_build_announce(&id, STA_IP, false, a, (int)sizeof a);
    ck(al > 0, "an unsolicited announce encodes");
    if (al > 0) {
        rr_t rr[4];
        /* 1 ANSWER (the PTR) + 3 ADDITIONAL (SRV/TXT/A) = four records to walk.
         * Asserting 1 here would only have asserted that the walk stopped early. */
        ck(walk(a, al, 12, rr, 4) == 4 && a[6] == 0 && a[7] == 1 && a[8] == 0 && a[9] == 3,
           "the announce answers with the PTR alone; SRV/TXT/A ride as additional");
        ck(rr[0].ttl == 120, "announce TTL is 120");
    }
}

int main(void)
{
    printf("-- browse reply\n");            t_browse_ptr();
    printf("-- TXT payload\n");             t_txt_payload();
    printf("-- per-interface A\n");         t_per_interface_a();
    printf("-- not ours to answer\n");      t_not_ours();
    printf("-- malformed names\n");         t_malformed_names();
    printf("-- unicast-query bit\n");     t_unicast_query_bit();
    printf("-- cap and goodbye\n");         t_cap_and_goodbye();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
"""


def lint_plumbing():
    """The codec is lifted and compiled; the plumbing is neither. These greps
     are the cheap half of its review, and the sizeof rules are a bug this
     project actually shipped: rx/tx became heap pointers, sizeof rx quietly
     became 8, every query was truncated to its header, and the orb answered
     nothing while the log said it was advertising. A suite that cannot see a
     whole file is not coverage of that file."""
    body = src.split("2. plumbing", 1)[1]
    problems = []
    for pat, why in [
        ("sizeof[ ]+rx" + chr(92) + "b", "sizeof on the heap pointer rx is 8, not MDNS_BUF"),
        ("sizeof[ ]+tx" + chr(92) + "b", "sizeof on the heap pointer tx is 8, not MDNS_BUF"),
        ("recvfrom[^)]*sizeof", "recvfrom must be handed MDNS_BUF explicitly, never a sizeof"),
    ]:
        if re.search(pat, body):
            problems.append(why)
    if re.search("LWIP_MAX_SOCKETS[ ]*=[^=]", body):
        problems.append("the responder set LWIP_MAX_SOCKETS itself: the budget is the project-wide 10")
    return problems

CC = os.environ.get("CC", "cc")

def build_and_run(sfx):
    c = (HARNESS
         .replace("__CODEC__", codec)
         .replace("__PORT__", str(PORT)))
    d = os.path.join(tempfile.gettempdir(), f"mdns-host-{sfx}.c")
    exe = os.path.join(tempfile.gettempdir(), f"mdns-host-{sfx}")
    open(d, "w").write(c)
    base = [CC, "-std=gnu11", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-Wno-unused-function", "-o", exe, d]
    try:
        subprocess.run(base + ["-fsanitize=address,undefined", "-Werror"],
                       check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError:
        # No sanitizer in this compiler: run the same binary plain. The bounds
        # assertions still hold; only the over-read fatality is lost, and the
        # output says so rather than pretending.
        try:
            subprocess.run(base, check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as e:
            print(e.stdout + e.stderr)
            sys.exit(f"host harness for {sfx} does not compile")
        print(f"  [{sfx}: built WITHOUT sanitizers]")
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    print(r.stdout, end="")
    if r.stderr.strip():
        print(r.stderr)
    return r.returncode

def mutations():
    """Each mutation must turn the suite red. A pin that survives its own
    removal is not a pin, and this suite was written by the same hand that
    wrote the encoder, so that is the risk worth paying for."""
    cases = [
        ("drop the TXT record entirely",           lambda s: s.replace("if (want_txt || want_ptr) {", "if (0) {")),
        ("drop the A record",                      lambda s: s.replace("if (want_a || want_srv || want_txt || want_ptr) {", "if (0) {")),
        ("invert the per-interface A choice",      lambda s: s.replace("if (ap_ip != 0 && (src_ip & 0x00FFFFFFu) == (ap_ip & 0x00FFFFFFu)) ip = ap_ip;", "ip = ap_ip;")),
        ("announce with a 0 TTL always",           lambda s: s.replace("if (!emit_records(&w, id, ip, 1, 0, 0, 0,\n                      goodbye ? 0u : MDNS_TTL, &an, &ar)) return 0;", "if (!emit_records(&w, id, ip, 1, 0, 0, 0, 0u, &an, &ar)) return 0;")),
        ("forget the cache-flush bit",             lambda s: s.replace("return put_rr_frame(w->t, w->cap, &w->off, type, cls, ttl, rdlen_at);", "return put_rr_frame(w->t, w->cap, &w->off, type, (uint16_t)(cls & 0x7FFFu), ttl, rdlen_at);")),
        ("lie in one RDLENGTH",                    lambda s: s.replace("const int rd = w->off - (rl + 2);\n        if (rd < 0 || rd > 0xFFFF) return 0;\n        w->t[rl] = (uint8_t)(rd >> 8); w->t[rl + 1] = (uint8_t)(rd & 0xFF);\n        an++;", "const int rd = (w->off - (rl + 2)) + 1;\n        if (rd < 0 || rd > 0xFFFF) return 0;\n        w->t[rl] = (uint8_t)(rd >> 8); w->t[rl + 1] = (uint8_t)(rd & 0xFF);\n        an++;")),
        ("emit a partial packet when the buffer is small", lambda s: s.replace("if (qlen <= 0 || MDNS_HDRLEN + qlen > cap) return 0;", "if (qlen <= 0) return 0;")),
        ("answer a foreign name anyway",           lambda s: s.replace("if (qtype == MDNS_TYPE_PTR && name_match(rx, n, off, SERVICE_NAME)) return Q_PTR;", "if (qtype == MDNS_TYPE_PTR) return Q_PTR;")),
        ("follow forward compression pointers",    lambda s: s.replace("if (target >= consumed || ++hops > MAX_NAME_HOPS) return 0;\n            off = target;", "if (++hops > MAX_NAME_HOPS) return 0;\n            off = target;")),
        ("spell the TXT state wrong",              lambda s: s.replace('"state=%s"', '"station=%s"')),
        ("announce the SOFTAP mac (base+1) instead", lambda s: s.replace('snprintf(kv, sizeof(kv), "mac=%s", id->mac)', 'snprintf(kv, sizeof(kv), "mac=%s", "44bd8d782619")')),
        ("announce a port the hub cannot dial",    lambda s: s.replace('(uint16_t)CONFIG_ORB_NET_HTTPD_PORT', '(uint16_t)8080')),
    ]
    bad = 0
    for what, fn in cases:
        mutated = fn(codec)
        if mutated == codec:
            print(f"  MUTATION DID NOT APPLY: {what}")
            bad += 1
            continue
        globals()["codec"] = mutated
        globals()["HARNESS_CACHE"] = None
        rc = build_and_run("mut")
        globals()["codec"] = codec
        print(f"  {'caught' if rc else 'SURVIVED  '} <- {what}")
        if rc == 0:
            bad += 1
    return bad

if __name__ == "__main__":
    if "--mutations" in sys.argv:
        sys.exit(1 if mutations() else 0)
    print(f"  [codec lifted: {len(codec)} B; SRV port from Kconfig: {PORT}]")
    bad = 0
    for why in lint_plumbing():
        print("  PLUMBING LINT: " + why)
        bad += 1
    if bad:
        sys.exit(1)
    sys.exit(1 if build_and_run("main") else 0)
