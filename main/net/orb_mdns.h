/* orb_mdns.h — advertise this orb over mDNS so a hub can find it without
 * sweeping the subnet and without being told its address.
 *
 * Why it exists
 * -------------
 * The hub's whole discovery story (docs/orb-manager-plan.md §5, ladder items 4
 * and 5) is "browse _orb._tcp, list candidates, dial only the one you picked".
 * That promise has a precondition this file supplies: somebody has to answer
 * the browse. Nothing here is authoritative — an mDNS reply proves only that
 * something on that address claims to be an orb, so the hub still dials
 * /api/v1/status and compares the `mac` it is told against the `mac` in this
 * TXT before it trusts an address. Item 6's cross-lock is that comparison;
 * without the MAC in the announcement there is nothing to cross-lock, and
 * discovery degenerates into "send a request to any address a stranger
 * mentioned", which is why the /24 sweep was deleted rather than improved.
 *
 * Why hand-rolled instead of the espressif/mdns component
 * ------------------------------------------------------
 * IDF v5.4 ships no in-tree mdns component, and the managed one allocates a
 * socket pair PER INTERFACE. sdkconfig.defaults:68 holds LWIP_MAX_SOCKETS at
 * 10: six belong to httpd's floor (max_open_sockets 3 + esp_http_server's own
 * 3), leaving four for everything else — the captive-DNS responder takes one
 * while the SoftAP is up. A per-interface advertiser spends the entire margin
 * to answer twelve bytes of question, so this is one socket with two
 * IP_ADD_MEMBERSHIP joins, exactly as the plan's §5.2 socket note specifies.
 *
 * The one deviation from RFC 6762, stated rather than hidden: there is no
 * probing phase before the announce, because every name here is derived from
 * the chip's BASE MAC and so cannot collide with another orb's. Probing exists
 * to resolve contested names; this build has no contest to resolve.
 *
 * The packet codec below is deliberately pure (no lwIP, no esp_netif, no
 * sockets) and is lifted VERBATIM by tools/host_mdns_test.py, which asserts
 * real wire bytes and feeds back malformed queries. The socket plumbing is the
 * thin part; the encoder and decoder are what a browser actually judges.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The three names one orb owns, all derived from the same BASE MAC so the
 * instance, the service and the host name cannot drift apart. Filled by the
 * caller (orb_net.c) because it already reads the MAC for /api/v1/status, and
 * the announcement MUST carry the identical string: two readers of one field
 * is how a fake browser and a fake device start agreeing with each other. */
typedef struct {
    char instance[24];   /* "orb-782618"            — the leftmost label        */
    char hostname[32];   /* "orb-782618.local"      — the SRV target + A name   */
    char mac[13];        /* "44bd8d782618"          — same as h_status.mac      */
    char fw[32];         /* "v1.0.0+e1a2b3c"        — same as h_status.fw;
                        * the width of esp_app_desc_t.version, which is what
                        * is copied in, so the copy can never truncate      */
    bool provisioning;   /* true => SoftAP-only, no lease yet: item 10's case   */
} orb_mdns_id_t;

/* Lifecycle. Both are safe to call from the default event task: they only set
 * flags and post, never touch the driver, and a failure here costs discovery
 * and nothing else — the config page, the API and the radio behave exactly as
 * they did before this file existed. */
void orb_mdns_set_sta(bool up, uint32_t ip_net_order);
void orb_mdns_set_ap(bool up, uint32_t ip_net_order);

/* The word that goes in TXT as state=. True until a lease lands, after which the
 * announcement says "normal": this is how item 10's factory reset is tellable from
 * a SoftAP announcement without dialing an address no wired hub can route to. */
void orb_mdns_set_provisioning(bool provisioning);

/* Answer a query and (re)announce. The two up/down setters arm this; the task
 * calls it. Exposed for the host harness, which drives the codec directly.
 *
 * src_ip is the QUERIER's address, not ours: this lwIP has no working
 * IP_PKTINFO (sockets.h:282 declares the option and sock.c never implements
 * it), so a single socket carrying two IGMP joins cannot report which
 * interface a datagram arrived on. The querier's subnet is the substitute, and
 * it is the honest one — a host can only ask from a network it is attached to.
 * The answer itself goes back UNICAST to the querier, which is why the missing
 * option never forces a wrong egress route. */
int orb_mdns_build_answer(const orb_mdns_id_t *id, uint32_t sta_ip,
                          uint32_t ap_ip, uint32_t src_ip,
                          const uint8_t *rx, int n, uint8_t *tx, int cap);

/* One unsolicited announce for one interface, carrying THAT interface's A
 * record. goodbye=true emits TTL 0 so caches drop the orb immediately rather
 * than holding a ghost until the 80 s expiry. Returns the encoded length. */
int orb_mdns_build_announce(const orb_mdns_id_t *id, uint32_t ip,
                            bool goodbye, uint8_t *tx, int cap);
