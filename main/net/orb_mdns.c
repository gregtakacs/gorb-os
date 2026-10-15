/* orb_mdns.c — the _orb._tcp advertiser. See orb_mdns.h for why it exists and
 * why it is hand-rolled rather than the espressif/mdns component.
 *
 * Layout, and what is where on purpose:
 *   1. pure codec (encoders, decoder, answer/announce builders) — no sockets, no
 *      lwIP, no esp_netif. tools/host_mdns_test.py lifts THESE and asserts real
 *      wire bytes, because a browser's opinion of us is entirely a function of
 *      these bytes and the bench cannot watch a browse.
 *   2. plumbing (one socket, two IGMP joins, select loop, announce bursts) —
 *      thin, and only provable on the cable.
 */
#include "orb_mdns.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

/* ------------------------------------------------------------------ 1. codec */

#define MDNS_PORT        5353
/* 224.0.0.251. Written as the HOST-order value with an explicit htonl() at
 * each use rather than as a pre-swapped hex literal: a hand-transposed literal
 * here (0xFB000000 reads as 0.0.0.251 on a little-endian S3) binds to nothing,
 * logs no error, and the only symptom is a browser that never sees the orb. */
#define MDNS_GROUP_HOST  0xE00000FBu
#define MDNS_TTL         120u     /* RFC 6762's recommendation for everything
                                   * but the goodbye, which is 0. */
#define MDNS_TYPE_A      1
#define MDNS_TYPE_PTR    12
#define MDNS_TYPE_TXT    16
#define MDNS_TYPE_SRV    33
#define MDNS_CLASS_IN    1
/* The cache-flush bit. Our names are unique-by-MAC, so they must be stated
 * authoritatively: without it a browser is entitled to merge a stranger's
 * record for the same name into its cache alongside ours. */
#define MDNS_CLASS_FLUSH (0x8000u | MDNS_CLASS_IN)
#define MDNS_HDRLEN      12
#define MDNS_BUF         512      /* a DNS packet over UDP is capped at 512 B  */
#define MAX_NAME_HOPS    8        /* a name in this build needs at most 4 pointers;
                                   * anything hungrier is being crafted to spin us */

/* Every helper below returns 0 on "would not fit" and the caller drops the
 * whole packet. Half a response is worse than none: a truncated answer parses,
 * and the browser caches the wrong thing for 120 s. */
static int put16(uint8_t *t, int cap, int *off, uint16_t v)
{
    if (*off + 2 > cap) return 0;
    t[(*off)++] = (uint8_t)(v >> 8);
    t[(*off)++] = (uint8_t)(v & 0xFF);
    return 1;
}

static int put_u8(uint8_t *t, int cap, int *off, uint8_t v)
{
    if (*off + 1 > cap) return 0;
    t[(*off)++] = v;
    return 1;
}

/* A compression pointer: 0xC0 plus the 14-bit offset of a name already in this
 * message. The high-bit test is what tells a pointer from a length octet. */
static int put_ptr(uint8_t *t, int cap, int *off, int target)
{
    if (target < 0 || target > 0x3FFF) return 0;
    return put16(t, cap, off, (uint16_t)(0xC000 | target));
}

/* "orb-782618.local" -> \011orb-782618\005local\0. Refuses an empty label, an
 * over-long label, and a name that would run past the buffer. */
static int put_name(uint8_t *t, int cap, int *off, const char *dotted)
{
    const char *p = dotted;
    while (*p) {
        const char *dot = strchr(p, '.');
        const int len = dot ? (int)(dot - p) : (int)strlen(p);
        if (len <= 0 || len > 63) return 0;
        if (!put_u8(t, cap, off, (uint8_t)len)) return 0;
        if (*off + len > cap) return 0;
        memcpy(t + *off, p, (size_t)len);
        *off += len;
        p += len;
        if (*p == '.') p++;
        else if (*p) return 0;                  /* malformed: '.' inside a label */
    }
    return put_u8(t, cap, off, 0);
}

/* An RR header: NAME, TYPE, CLASS, TTL, RDLENGTH placeholder. The caller fills
 * rdlen once it knows how much rdata it wrote. */
/* The TYPE/CLASS/TTL/RDLENGTH frame. The owner NAME is NOT written here:
 * rr_begin() has already written it, exactly once, as either a pointer or full
 * labels. Writing it in both places shifts every following field two bytes and
 * presents a record whose RDLENGTH is read out of the middle of the TTL — which
 * a browser simply drops, and which the host harness's independent record walk
 * is what caught. */
static int put_rr_frame(uint8_t *t, int cap, int *off,
                        uint16_t type, uint16_t cls, uint32_t ttl, int *rdlen_at)
{
    if (!put16(t, cap, off, type)) return 0;
    if (!put16(t, cap, off, cls)) return 0;
    if (*off + 4 > cap) return 0;
    t[(*off)++] = (uint8_t)(ttl >> 24); t[(*off)++] = (uint8_t)(ttl >> 16);
    t[(*off)++] = (uint8_t)(ttl >> 8);  t[(*off)++] = (uint8_t)(ttl);
    if (rdlen_at) *rdlen_at = *off;
    if (!put16(t, cap, off, 0)) return 0;
    return 1;
}

/* Compare a name in the packet, starting at *off, against a dotted string,
 * following compression pointers. Case-insensitive: DNS names are, and an orb
 * whose TXT record only answers to one capitalisation is a browse that silently
 * finds nothing depending on how the browser happened to spell it.
 *
 * This is the only function here that parses attacker-chosen bytes, so every
 * bound is checked before every dereference and a pointer loop is fatal rather
 * than sticky. Pointers may only point backwards into what has already been
 * consumed: that is what makes the walk terminate without a visited set, and it
 * is also what a hostile packet breaks, which is why the budget is asserted. */
static int name_match(const uint8_t *rx, int n, int off, const char *dotted)
{
    const char *p = dotted;
    int hops = 0;
    int consumed = off;                    /* furthest byte read directly       */

    for (;;) {
        if (off < 0 || off >= n) return 0;
        const uint8_t len = rx[off];

        if (len == 0) return *p == '\0';   /* root label: match iff name spent  */

        if ((len & 0xC0) == 0xC0) {        /* compression pointer               */
            if (off + 2 > n) return 0;
            const int target = ((len & 0x3F) << 8) | rx[off + 1];
            /* Backwards-only, plus a hop budget: a self-referential or
             * mutually-referential pair of pointers must cost us nothing more
             * than the loop it is worth. */
            if (target >= consumed || ++hops > MAX_NAME_HOPS) return 0;
            off = target;
            continue;
        }

        if ((len & 0xC0) != 0) return 0;   /* reserved label form: refuse it    */

        const int next = off + 1 + len;
        if (next > n) return 0;            /* label runs past the datagram      */

        /* Measure the NEXT LABEL of the needle, not the rest of the string:
         * comparing a packet label against strlen of "_orb._tcp.local" asks it
         * to be fifteen bytes long and so refuses every multi-label name on
         * earth, which presents as a healthy orb no browser can ever see. */
        const char *ndot = strchr(p, '.');
        const size_t dlen = ndot ? (size_t)(ndot - p) : strlen(p);
        if (dlen == 0) return 0;           /* packet has a label, needle is out */
        if ((int)dlen != len) return 0;
        for (int i = 0; i < len; i++) {
            uint8_t a = rx[off + 1 + i], b = (uint8_t)p[i];
            if (a >= 'A' && a <= 'Z') a = (uint8_t)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (uint8_t)(b - 'A' + 'a');
            if (a != b) return 0;
        }
        p += len;
        if (*p == '.') p++;
        off = next;
        if (off > consumed) consumed = off;
    }
}

/* Skip a name, returning the offset just past it, or -1. Pointers count as
 * their two bytes for the purpose of finding the next field: a name reached
 * through a pointer ends where the pointer said, and the pointer itself is
 * over with. */
static int name_skip(int off, const uint8_t *rx, int n)
{
    int hops = 0;
    int consumed = off;
    for (;;) {
        if (off < 0 || off >= n) return -1;
        const uint8_t len = rx[off];
        if (len == 0) return off + 1;
        if ((len & 0xC0) == 0xC0) {
            if (off + 2 > n) return -1;
            const int target = ((len & 0x3F) << 8) | rx[off + 1];
            if (target >= consumed || ++hops > MAX_NAME_HOPS) return -1;
            return off + 2;                /* the pointer ends this field       */
        }
        if ((len & 0xC0) != 0) return -1;
        off += 1 + len;
        if (off > n) return -1;
        if (off > consumed) consumed = off;
    }
}

#define SERVICE_NAME   "_orb._tcp.local"

/* The question a browser asks when it browses: PTR for _orb._tcp.local. The
 * A/SRV/TXT lookups that follow it are asked against OUR names, so each has an
 * expected owner. Returns which of those four this query is, or -1 to ignore. */
enum { Q_NONE = -1, Q_PTR = 0, Q_SRV, Q_TXT, Q_A };

static int classify_query(const orb_mdns_id_t *id, const uint8_t *rx, int n,
                          int *qtype_out, int *qend_out)
{
    if (n < MDNS_HDRLEN) return Q_NONE;
    if ((rx[2] & 0x80) != 0) return Q_NONE;           /* QR: a response, not ours */
    if ((rx[2] & 0x78) != 0) return Q_NONE;           /* opcode must be QUERY     */
    if (rx[4] != 0 || rx[5] != 1) return Q_NONE;      /* exactly one question     */
    if (rx[6] || rx[7] || rx[8] || rx[9] || rx[10] || rx[11]) return Q_NONE;

    int off = MDNS_HDRLEN;
    const int qtype_at = name_skip(off, rx, n);
    if (qtype_at < 0 || qtype_at + 4 > n) return Q_NONE;
    const uint16_t qtype = (uint16_t)((rx[qtype_at] << 8) | rx[qtype_at + 1]);
    const uint16_t qclass = (uint16_t)((rx[qtype_at + 2] << 8) | rx[qtype_at + 3]);
    /* The top bit of the question class is RFC 6762's UNICAST-REQUEST flag, not
     * part of the class: avahi and zeroconf both ask with 0x8001, so comparing
     * against a bare 1 here refuses every real browse on the floor while the
     * hand-built test queries sail through. The answer goes back unicast either
     * way, which is what makes the absent IP_PKTINFO harmless. */
    if ((qclass & 0x7FFF) != MDNS_CLASS_IN) return Q_NONE;
    *qend_out = qtype_at + 4;

    /* The instance's own FQDN, assembled here so the comparison below cannot be
     * handed a name the orb did not itself form. */
    char inst_fqdn[64];
    if (snprintf(inst_fqdn, sizeof(inst_fqdn), "%s." SERVICE_NAME, id->instance)
        >= (int)sizeof(inst_fqdn)) return Q_NONE;

    *qtype_out = qtype;
    if (qtype == MDNS_TYPE_PTR && name_match(rx, n, off, SERVICE_NAME)) return Q_PTR;
    if (qtype == MDNS_TYPE_SRV && name_match(rx, n, off, inst_fqdn))     return Q_SRV;
    if (qtype == MDNS_TYPE_TXT && name_match(rx, n, off, inst_fqdn))     return Q_TXT;
    if (qtype == MDNS_TYPE_A   && name_match(rx, n, off, id->hostname))  return Q_A;
    return Q_NONE;
}

/* Name offsets already written into this message, so a later record can point
 * at an earlier one instead of repeating it. -1 means not yet written. mDNS
 * browsers do not care either way, but four uncompressed copies of three names
 * pushes a PTR answer plus its three additional records toward the 512 B cap
 * that the answer must not exceed. */
typedef struct {
    uint8_t *t;
    int      cap;
    int      off;
    int      at_service;
    int      at_instance;
    int      at_host;
} rr_writer_t;

/* Write NAME, then the TYPE/CLASS/TTL/RDLENGTH frame. The caller fills the
 * RDLENGTH octets it was handed once the rdata is complete. */
static int rr_begin(rr_writer_t *w, int name_at, const char *name,
                    uint16_t type, uint16_t cls, uint32_t ttl, int *rdlen_at)
{
    if (name_at >= 0) {
        if (!put_ptr(w->t, w->cap, &w->off, name_at)) return 0;
    } else {
        if (name == NULL) return 0;   /* no cached offset and nothing to write */
        name_at = w->off;
        if (!put_name(w->t, w->cap, &w->off, name)) return 0;
    }
    return put_rr_frame(w->t, w->cap, &w->off, type, cls, ttl, rdlen_at);
}

/* A single TXT string: one length octet then the bytes, never compressed —
 * RFC 6762 §6.1 forbids compressing inside TXT rdata, and a browser that finds
 * a pointer there reads two garbage bytes instead of the key. */
static int txt_string(rr_writer_t *w, const char *kv)
{
    const int len = (int)strlen(kv);
    if (len <= 0 || len > 255) return 0;
    if (!put_u8(w->t, w->cap, &w->off, (uint8_t)len)) return 0;
    if (w->off + len > w->cap) return 0;
    memcpy(w->t + w->off, kv, (size_t)len);
    w->off += len;
    return 1;
}

/* The four records an orb owns, in the order that lets each one point at the
 * last: PTR, SRV, TXT, A. The `want_*` flags say which belong in the ANSWER
 * section; the rest follow as ADDITIONAL, so one browse reply carries every-
 * thing a browser needs to dial without asking three more questions.
 *
 * RDLENGTH is written as a placeholder and patched afterwards, which is why
 * every record remembers `rl`, the index of those two octets: the length is
 * only known once the rdata has been emitted. A response whose rdlength lies
 * is not "slightly wrong" — the browser resyncs at the wrong offset and drops
 * every record after it, so this arithmetic is the whole ballgame. */
static int emit_records(rr_writer_t *w, const orb_mdns_id_t *id, uint32_t ip,
                        int want_ptr, int want_srv, int want_txt, int want_a,
                        uint32_t ttl, int *an_out, int *ar_out)
{
    int an = 0, ar = 0;

    /* Formed once, here: the SRV/TXT owner name and the PTR's rdata are the same
     * string, and if each built its own the packet could carry two spellings of
     * the instance and the browser would see two services. */
    char inst_fqdn[64];
    if (snprintf(inst_fqdn, sizeof(inst_fqdn), "%s." SERVICE_NAME, id->instance)
        >= (int)sizeof(inst_fqdn)) return 0;

    if (want_ptr) {                                   /* PTR service -> instance */
        int rl = 0;
        if (!rr_begin(w, w->at_service, SERVICE_NAME, MDNS_TYPE_PTR,
                      MDNS_CLASS_IN, ttl, &rl)) return 0;   /* PTR is shared, no flush */
        if (w->at_instance < 0) {
            w->at_instance = w->off;
            if (!put_name(w->t, w->cap, &w->off, inst_fqdn)) return 0;
        } else if (!put_ptr(w->t, w->cap, &w->off, w->at_instance)) return 0;
        const int rd = w->off - (rl + 2);
        if (rd < 0 || rd > 0xFFFF) return 0;
        w->t[rl] = (uint8_t)(rd >> 8); w->t[rl + 1] = (uint8_t)(rd & 0xFF);
        an++;
    }

    if (want_srv || want_ptr) {                       /* SRV instance -> host    */
        int rl = 0;
        if (!rr_begin(w, w->at_instance, inst_fqdn, MDNS_TYPE_SRV, MDNS_CLASS_FLUSH,
                      ttl, &rl)) return 0;
        if (!put16(w->t, w->cap, &w->off, 0)) return 0;   /* priority */
        if (!put16(w->t, w->cap, &w->off, 0)) return 0;   /* weight   */
        if (!put16(w->t, w->cap, &w->off, (uint16_t)CONFIG_ORB_NET_HTTPD_PORT)) return 0;
        if (w->at_host < 0) {
            w->at_host = w->off;
            if (!put_name(w->t, w->cap, &w->off, id->hostname)) return 0;
        } else if (!put_ptr(w->t, w->cap, &w->off, w->at_host)) return 0;
        const int rd = w->off - (rl + 2);
        if (rd < 0 || rd > 0xFFFF) return 0;
        w->t[rl] = (uint8_t)(rd >> 8); w->t[rl + 1] = (uint8_t)(rd & 0xFF);
        if (want_srv) an++; else ar++;
    }

    if (want_txt || want_ptr) {                       /* TXT instance -> fields  */
        int rl = 0;
        if (!rr_begin(w, w->at_instance, inst_fqdn, MDNS_TYPE_TXT, MDNS_CLASS_FLUSH,
                      ttl, &rl)) return 0;
        char kv[64];
        if (snprintf(kv, sizeof(kv), "mac=%s", id->mac) >= (int)sizeof(kv)) return 0;
        if (!txt_string(w, kv)) return 0;
        if (snprintf(kv, sizeof(kv), "fw=%s", id->fw) >= (int)sizeof(kv)) return 0;
        if (!txt_string(w, kv)) return 0;
        if (snprintf(kv, sizeof(kv), "state=%s",
                     id->provisioning ? "provisioning" : "normal")
            >= (int)sizeof(kv)) return 0;
        if (!txt_string(w, kv)) return 0;
        const int rd = w->off - (rl + 2);
        if (rd < 0 || rd > 0xFFFF) return 0;
        w->t[rl] = (uint8_t)(rd >> 8); w->t[rl + 1] = (uint8_t)(rd & 0xFF);
        if (want_txt) an++; else ar++;
    }

    if (want_a || want_srv || want_txt || want_ptr) { /* A host -> address       */
        if (ip == 0) {
            /* No address to give on this interface. Emitting 0.0.0.0 would be
             * worse than saying nothing: the browser would cache a host with no
             * address for the whole TTL and never fall back to the record it
             * could have used. */
            if (want_a) return 0;
        } else {
            int rl = 0;
            if (!rr_begin(w, w->at_host, id->hostname, MDNS_TYPE_A, MDNS_CLASS_FLUSH,
                          ttl, &rl)) return 0;
            if (w->off + 4 > w->cap) return 0;
            /* ip arrived in network order (esp_netif's ip_info), so the
             * LSB is the FIRST octet of the address on the wire. */
            w->t[w->off++] = (uint8_t)(ip & 0xFF);        w->t[w->off++] = (uint8_t)((ip >> 8) & 0xFF);
            w->t[w->off++] = (uint8_t)((ip >> 16) & 0xFF); w->t[w->off++] = (uint8_t)((ip >> 24) & 0xFF);
            const int rd = w->off - (rl + 2);
            w->t[rl] = (uint8_t)(rd >> 8); w->t[rl + 1] = (uint8_t)(rd & 0xFF);
            if (want_a) an++; else ar++;
        }
    }

    *an_out = an; *ar_out = ar;
    return 1;
}

/* The header of a response: id echoed, QR|AA set, recursion bits clear (an
 * mDNS responder is authoritative for its own names and never recurses), and
 * the four counts written last because they are only known at the end. */
static int resp_begin(rr_writer_t *w, uint16_t id, int qd)
{
    if (w->off != 0) return 0;
    if (!put16(w->t, w->cap, &w->off, id)) return 0;
    if (!put16(w->t, w->cap, &w->off, 0x8400)) return 0;   /* QR | AA           */
    if (!put16(w->t, w->cap, &w->off, (uint16_t)qd)) return 0;
    if (!put16(w->t, w->cap, &w->off, 0)) return 0;        /* ANCOUNT, patched  */
    if (!put16(w->t, w->cap, &w->off, 0)) return 0;        /* ARCOUNT, patched  */
    if (!put16(w->t, w->cap, &w->off, 0)) return 0;
    return 1;
}

static void resp_counts(uint8_t *t, int an, int ar)
{
    t[6] = (uint8_t)(an >> 8); t[7] = (uint8_t)(an & 0xFF);
    t[8] = (uint8_t)(ar >> 8); t[9] = (uint8_t)(ar & 0xFF);
    t[10] = 0; t[11] = 0;                                  /* no nameservers    */
}

int orb_mdns_build_answer(const orb_mdns_id_t *id, uint32_t sta_ip,
                          uint32_t ap_ip, uint32_t src_ip,
                          const uint8_t *rx, int n, uint8_t *tx, int cap)
{
    int qtype = 0, qend = 0;
    const int kind = classify_query(id, rx, n, &qtype, &qend);
    if (kind == Q_NONE) return 0;                 /* not ours to answer         */

    /* Which address this answer may give. There is no IP_PKTINFO in this lwIP
     * (sockets.h:282 declares it; sock.c never implements it), so the socket
     * cannot report which of its two joins a datagram arrived on. The query's
     * SOURCE address is the next best evidence and it is the right evidence: a
     * querier can only reach the interface it is already attached to, so a
     * phone on the SoftAP asks from 192.168.4.x and a hub on the LAN asks from
     * the STA's netmask. A querier off both subnets gets the STA address,
     * which is the only one that is ever reachable from elsewhere. */
    uint32_t ip = sta_ip;
    if (ap_ip != 0 && (src_ip & 0x00FFFFFFu) == (ap_ip & 0x00FFFFFFu)) ip = ap_ip;
    if (sta_ip == 0) ip = ap_ip;                  /* SoftAP-only boot: no lease */

    rr_writer_t w = { .t = tx, .cap = cap, .off = 0,
                      .at_service = -1, .at_instance = -1, .at_host = -1 };
    if (!resp_begin(&w, (uint16_t)((rx[0] << 8) | rx[1]), 1)) return 0;

    /* Echo the question verbatim. Beyond 12 the first name we write can then be
     * a pointer to offset 12, which is where the QNAME now lives. */
    const int qlen = qend - MDNS_HDRLEN;
    if (qlen <= 0 || MDNS_HDRLEN + qlen > cap) return 0;
    memcpy(tx + MDNS_HDRLEN, rx + MDNS_HDRLEN, (size_t)qlen);
    w.off = MDNS_HDRLEN + qlen;
    /* The QNAME at offset 12 is the very name one of these records is about, so
     * plant the matching offset and that record costs two bytes instead of
     * repeating a 20-odd byte name. Planting the WRONG one would answer a
     * question about the service with a record about the instance, so each kind
     * plants exactly its own. */
    if (kind == Q_PTR) w.at_service  = MDNS_HDRLEN;
    if (kind == Q_SRV || kind == Q_TXT) w.at_instance = MDNS_HDRLEN;
    if (kind == Q_A)   w.at_host     = MDNS_HDRLEN;

    int an = 0, ar = 0;
    const int want_ptr = (kind == Q_PTR);
    const int want_srv = (kind == Q_SRV);
    const int want_txt = (kind == Q_TXT);
    const int want_a   = (kind == Q_A);
    if (!emit_records(&w, id, ip, want_ptr, want_srv, want_txt, want_a,
                      MDNS_TTL, &an, &ar)) return 0;
    if (an == 0) return 0;
    resp_counts(tx, an, ar);
    return w.off;
}

int orb_mdns_build_announce(const orb_mdns_id_t *id, uint32_t ip,
                            bool goodbye, uint8_t *tx, int cap)
{
    rr_writer_t w = { .t = tx, .cap = cap, .off = 0,
                      .at_service = -1, .at_instance = -1, .at_host = -1 };
    if (!resp_begin(&w, 0, 0)) return 0;
    int an = 0, ar = 0;
    if (!emit_records(&w, id, ip, 1, 0, 0, 0,
                      goodbye ? 0u : MDNS_TTL, &an, &ar)) return 0;
    if (an == 0) return 0;
    resp_counts(tx, an, ar);
    return w.off;
}

/* ---------------------------------------------------------------- 2. plumbing */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

static const char *M_TAG = "orb_mdns";

static TaskHandle_t   s_mdns_task;
static int            s_mdns_fd = -1;           /* owned by the task, closed there */
static volatile bool  s_mdns_wanted;            /* keep the responder running        */
static volatile bool  s_mdns_dirty;             /* an address changed: re-join      */
static volatile uint32_t s_sta_ip;              /* network order, 0 = no lease      */
static volatile uint32_t s_ap_ip;               /* network order, 0 = AP down       */
static orb_mdns_id_t  s_id;

/* The announce rhythm: RFC 6762 §8.3 wants several copies a second apart so a
 * browser that missed one still hears the orb, and one counter PER INTERFACE so
 * a provisioning orb that raises the SoftAP while the STA keeps its lease does
 * not lose either burst behind the other. Counted in poll ticks: no timer, no
 * queue, no second task for a rhythm this coarse. */
#define ANNOUNCE_REPEAT  4
#define POLL_US          250000                  /* 4 Hz                             */
#define POLLS_PER_SEC    (1000000 / POLL_US)

/* Steady self-refresh cadence, in seconds. The join burst announces once per
 * boot, so a hub's cache of this orb expires at MDNS_TTL and the orb drops off
 * a fleet list until re-queried or rebooted. A querier-hostile network (a host
 * firewall that drops the unicast answer to the hub's query while still
 * accepting our unsolicited multicast) makes that gap permanent. Re-advertise
 * one announce per up interface every MDNS_ADV_REFRESH_S, half of MDNS_TTL and
 * no faster than RFC 6762 §8.3's one-per-60s steady floor, so the row is self-
 * sustaining over the multicast path a hub always hears. This is what the
 * manager's README calls mdns_adv_s; the symbol had no code behind it. */
#define MDNS_ADV_REFRESH_S  60

static void mdns_join(int fd, uint32_t iface_ip, const char *whence)
{
    if (iface_ip == 0) return;
    struct ip_mreq m;
    m.imr_multiaddr.s_addr = htonl(MDNS_GROUP_HOST);
    m.imr_interface.s_addr = iface_ip;
    /* The second join rides the SAME socket, which is what keeps discovery inside
     * the four-socket margin §5.2 allows: LWIP_MAX_SOCKETS is 10 and six belong
     * to httpd's floor. A failed join is logged and survivable — the orb simply
     * cannot be browsed on that interface — because the config page, the API and
     * the radio are all unaffected by anything that happens here. */
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) != 0)
        ESP_LOGE(M_TAG, "IGMP join on %s failed (errno %d): this orb will not "
                        "appear in a browse — check LWIP_MAX_SOCKETS and LWIP_IGMP",
                 whence, errno);
    else
        ESP_LOGI(M_TAG, "advertising _orb._tcp on %s", whence);
}

/* One multicast packet out one interface. Called once per second from the loop
 * rather than looping itself, so answering a live browser is never made to wait
 * behind an announce schedule. */
static void mdns_announce(int fd, uint32_t ip, bool goodbye, uint8_t *tx)
{
    /* The buffer is the task's, not a local: this is called from inside the
     * receive loop, which already carries two of these, and a third on the
     * stack is what overflowed the 3 KB this task was first given. */
    const int len = orb_mdns_build_announce(&s_id, ip, goodbye, tx, MDNS_BUF);
    if (len <= 0) {
        ESP_LOGE(M_TAG, "announcement would not encode: the orb cannot be browsed");
        return;
    }
    struct sockaddr_in to = { 0 };
    to.sin_family      = AF_INET;
    to.sin_port        = htons(MDNS_PORT);
    to.sin_addr.s_addr = htonl(MDNS_GROUP_HOST);
    /* Source AND egress interface, both this interface. Without it lwIP picks one
     * route for the packet and the A record inside it can name the other one,
     * which sends a browser to an address it has no path to. */
    uint32_t out = ip;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof out) != 0)
        ESP_LOGW(M_TAG, "IP_MULTICAST_IF (errno %d): announce may leave the wrong way",
                 errno);
    if (sendto(fd, tx, (size_t)len, 0, (struct sockaddr *)&to, sizeof to) < 0)
        ESP_LOGW(M_TAG, "%s sendto: errno %d", goodbye ? "goodbye" : "announce", errno);
}

static void mdns_task_loop(void *arg)
{
    (void)arg;
    /* PSRAM, not the stack and not internal DRAM: 7.4 MB is free there against
     * 11,791 B free internally on the bench, and none of this is DMA-visible.
     * Out of memory here costs discovery and says so; it never takes the
     * network down with it. */
    uint8_t *rx = heap_caps_malloc(MDNS_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *tx = heap_caps_malloc(MDNS_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx || !tx) {
        free(rx); free(tx);
        ESP_LOGE(M_TAG, "packet buffers: out of memory — discovery disabled");
        close(s_mdns_fd); s_mdns_fd = -1; s_mdns_task = NULL; s_mdns_wanted = false;
        vTaskDelete(NULL);
        return;
    }
    uint32_t joined_sta = 0, joined_ap = 0;
    int burst_sta = 0, burst_ap = 0;
    int ticks = 0;
    bool need_watermark = true;     /* log the headroom once, after the deep path */

    for (;;) {
        /* select() with a short timeout, never a bare blocking recvfrom(): the
         * down paths only clear a flag, and this task has to notice within a poll
         * window and close its OWN socket. Closing from another task while
         * recvfrom() blocks is not guaranteed to wake lwIP. */
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s_mdns_fd, &rf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = POLL_US };
        const int sel = select(s_mdns_fd + 1, &rf, NULL, NULL, &tv);
        if (!s_mdns_wanted) break;

        if (s_mdns_dirty) {
            s_mdns_dirty = false;
            const uint32_t sta = s_sta_ip, ap = s_ap_ip;
            if (sta != joined_sta) {
                joined_sta = sta;
                if (sta) { mdns_join(s_mdns_fd, sta, "STA"); burst_sta = ANNOUNCE_REPEAT; }
            }
            if (ap != joined_ap) {
                joined_ap = ap;
                if (ap) { mdns_join(s_mdns_fd, ap, "SoftAP"); burst_ap = ANNOUNCE_REPEAT; }
            }
        }

        /* Steady self-refresh. Without this the orb announces once per boot and
         * a hub's cache of it expires at MDNS_TTL, so the row blinks out of a
         * fleet list until re-queried — and on a network that drops the unicast
         * answer to a hub's query, re-querying never revives it. Re-arm ONE
         * announce per up interface every MDNS_ADV_REFRESH_S so the row stays
         * alive over the multicast path a hub always hears. One copy, not the
         * ANNOUNCE_REPEAT burst: the boot burst covers the lost-packet case;
         * this only has to outrun the TTL, and one per interval is gentle on a
         * shared 5353. The burst drain just below sends it on this same tick
         * (240 % POLLS_PER_SEC == 0) and skips any interface mid-burst. */
        if (ticks > 0 && (ticks % (MDNS_ADV_REFRESH_S * POLLS_PER_SEC)) == 0) {
            if (joined_sta && burst_sta == 0) burst_sta = 1;
            if (joined_ap  && burst_ap  == 0) burst_ap  = 1;
            /* Proven on the wire, not assumed: the announce itself stays quiet
             * (sendto only logs on failure), so without this tick line an
             * absent hub row cannot be split into "never armed" and "armed but
             * dropped downstream". Prints only at the refresh boundary. */
            ESP_LOGI(M_TAG, "self-refresh re-armed at tick %d (sta=%d ap=%d)",
                     ticks, burst_sta, burst_ap);
        }

        if ((burst_sta > 0 || burst_ap > 0) && (ticks % POLLS_PER_SEC) == 0) {
            if (burst_sta > 0) { mdns_announce(s_mdns_fd, joined_sta, false, tx); burst_sta--; }
            if (burst_ap  > 0) { mdns_announce(s_mdns_fd, joined_ap,  false, tx); burst_ap--;  }
            if (burst_sta == 0 && burst_ap == 0 && need_watermark) {
                /* Measured rather than guessed. The first cut of this task died
                 * here — a 3 KB stack with the packet buffers on it — and the
                 * deepest path in the file is exactly this one, so the number
                 * that matters is printed once per boot instead of assumed. */
                need_watermark = false;
                ESP_LOGI(M_TAG, "stack headroom after the first announce burst: %u bytes",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
        ticks++;

        if (sel <= 0) continue;

        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const int n = recvfrom(s_mdns_fd, rx, MDNS_BUF, 0,
                               (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        /* Every encoding decision lives in orb_mdns_build_answer(), where the
         * host harness can drive it; here we only shuttle bytes. A 0 answer
         * means the query was not ours to answer, and is dropped silently — a
         * browser asks plenty of questions about other hosts on the LAN.
         *
         * MDNS_BUF, never sizeof: rx/tx are heap pointers now, and sizeof on
         * one is 8, which truncated every query to a header and silently
         * answered nothing. The host suite lints for this. */
        const int len = orb_mdns_build_answer(&s_id, s_sta_ip, s_ap_ip,
                                              from.sin_addr.s_addr, rx, n,
                                              tx, MDNS_BUF);
        /* Query ingress, proven on the console rather than inferred from a hub row.
         * A hub row that stays `unasked` has two opposite causes — the query died
         * on the way in, or the unicast answer died on the way back — and only
         * this line tells them apart. Capped at 24 answers: a browsed LAN is not
         * quiet, and a flood here would bury the boot log this exists to be read
         * alongside. QU is the RFC 6762 unicast-request bit as the query arrived. */
        if (len > 0) {
            static int s_answer_logged = 0;
            if (s_answer_logged < 24) {
                /* The question's class high byte sits right after its labels and
                 * TYPE, found the same way classify_query found it: an end-of-packet
                 * offset would be wrong for a hub query, which carries known
                 * answers after the question. */
                const int qa = name_skip(MDNS_HDRLEN, rx, n);
                const int qu = (qa >= 0 && qa + 4 <= n) ? ((rx[qa + 2] & 0x80) ? 1 : 0) : -1;
                const unsigned src = (unsigned)from.sin_addr.s_addr;
                s_answer_logged++;
                ESP_LOGI(M_TAG, "query answered from %u.%u.%u.%u QU=%d (%d B out, %d/%d)",
                         src & 255u, (src >> 8) & 255u, (src >> 16) & 255u,
                         (src >> 24) & 255u, qu, len, s_answer_logged, 24);
            }
        }
        if (len > 0)
            sendto(s_mdns_fd, tx, (size_t)len, 0, (struct sockaddr *)&from, fl);
    }

    /* Goodbye before the socket goes: without it a hub that had this orb in
     * cache keeps offering it until the 120 s TTL expires, which is the ghost
     * §5.1 point 2 refuses to leave in the fleet list. */
    if (joined_sta) mdns_announce(s_mdns_fd, joined_sta, true, tx);
    if (joined_ap)  mdns_announce(s_mdns_fd, joined_ap,  true, tx);
    free(rx); free(tx);
    close(s_mdns_fd);
    s_mdns_fd = -1;
    s_mdns_task = NULL;
    vTaskDelete(NULL);
}

/* Start the responder if anything is up, and ask it to stop when nothing is.
 * Called from the default event task, so it only sets flags: the socket, the
 * joins and the closes all belong to the task, one owner, no races. */
static void mdns_update(void)
{
    const bool want = (s_sta_ip != 0) || (s_ap_ip != 0);
    s_mdns_wanted = want;
    s_mdns_dirty  = true;
    if (!want) return;                        /* the task sees the flag and exits */
    if (s_mdns_fd >= 0) return;               /* already running                  */

    s_id.mac[0] = s_id.fw[0] = s_id.instance[0] = s_id.hostname[0] = '\0';
    uint8_t mac[6] = { 0 };
    /* BASE, not SOFTAP: the same reason h_status reports BASE (mac_addr.c:366-387
     * derives the SoftAP address as base+1), and the reason the TXT and the JSON
     * must be read from one place — a hub that cross-locks two spellings of the
     * chip's address against each other locks nothing at all. */
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        ESP_LOGE(M_TAG, "no base MAC: refusing to advertise an orb that cannot "
                        "be identified, because an unnamed row is a wrong row");
        s_mdns_wanted = false;
        return;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(s_id.mac, sizeof(s_id.mac), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_id.fw, sizeof(s_id.fw), "%s", app ? app->version : "?");
    snprintf(s_id.instance, sizeof(s_id.instance), "orb-%c%c%c%c%c%c",
             s_id.mac[6], s_id.mac[7], s_id.mac[8],
             s_id.mac[9], s_id.mac[10], s_id.mac[11]);
    snprintf(s_id.hostname, sizeof(s_id.hostname), "%s.local", s_id.instance);
    /* The two spellings of this board on one line, once: the SoftAP SSID is
     * base+1 by the same derivation ("orb-2619" beside "orb-...2618"), and an
     * owner comparing them without this note reads a correct pair as a bug. */
    ESP_LOGI(M_TAG, "advertising %s._orb._tcp (mac=%s fw=%s) while the SoftAP is "
                    "named orb-%02X%02X", s_id.instance, s_id.mac, s_id.fw,
             mac[4], (unsigned)(mac[5] + 1));

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { ESP_LOGE(M_TAG, "socket: errno %d — discovery disabled", errno); s_mdns_wanted = false; return; }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in sa = { 0 };
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(MDNS_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);   /* BOTH joins, one socket — see above */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        ESP_LOGE(M_TAG, "bind :5353: errno %d — discovery disabled (socket budget?)", errno);
        close(fd); s_mdns_wanted = false; return;
    }
    s_mdns_fd = fd;
    if (xTaskCreate(mdns_task_loop, "orb_mdns", 4096, NULL, 3, &s_mdns_task) != pdPASS) {
        /* This stack is internal DRAM, which is the scarce pool here: ~11.6 KB
         * free at post-httpd_start on the bench, and every byte this task takes
         * is a byte the SoftAP driver cannot have later. So it is sized to a
         * measurement, not a guess: with the packet buffers in PSRAM the deepest
         * path (announce -> build_announce -> emit_records, with its snprintf
         * locals and its logging) peaked at 2,076 B, reported by the headroom
         * line above. 4,096 leaves that doubled. It is not in PSRAM because a
         * PSRAM stack is unreadable while the flash cache is disabled for an
         * OTA write, and this device does OTA writes. */
        ESP_LOGE(M_TAG, "task out of memory — discovery disabled");
        close(fd); s_mdns_fd = -1; s_mdns_wanted = false; return;
    }
}

void orb_mdns_set_sta(bool up, uint32_t ip_net_order)
{
    s_sta_ip = up ? ip_net_order : 0;
    mdns_update();
}

void orb_mdns_set_ap(bool up, uint32_t ip_net_order)
{
    s_ap_ip = up ? ip_net_order : 0;
    mdns_update();
}

void orb_mdns_set_provisioning(bool provisioning)
{
    const bool was = s_id.provisioning;
    s_id.provisioning = provisioning;
    /* The TXT changed, so the caches are stale: re-announce rather than let a
     * hub keep "provisioning" for a joined orb for the rest of the TTL. */
    if (was != provisioning && s_mdns_fd >= 0) s_mdns_dirty = true;
}
