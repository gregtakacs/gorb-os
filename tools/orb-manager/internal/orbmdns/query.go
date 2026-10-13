package orbmdns

import (
	"encoding/binary"
	"fmt"
	"net"
	"strings"
)

// BuildQuery encodes the one question a browse starts with: PTR for
// _orb._tcp.local, asked with the RFC 6762 unicast-request bit set so a responder
// answers to us directly instead of flooding the group back at every host on the
// LAN. Names are written as full labels and never compressed: a question section
// has nothing earlier in the message to point at.
func BuildQuery(txid uint16, name string, qtype uint16) []byte {
	b := make([]byte, headerLen, 512)
	binary.BigEndian.PutUint16(b[0:], txid)
	binary.BigEndian.PutUint16(b[4:], 1) // QDCOUNT
	for _, part := range strings.Split(strings.TrimSuffix(name, "."), ".") {
		if part == "" {
			continue
		}
		b = append(b, byte(len(part)))
		b = append(b, part...)
	}
	b = append(b, 0)
	b = binary.BigEndian.AppendUint16(b, qtype)
	return binary.BigEndian.AppendUint16(b, classIN|classQU)
}

// BrowseQuery is the question that finds every orb on the LAN. The A/SRV/TXT
// lookups a browse also needs ride along in the answer's own additional section —
// the firmware sends all four in one reply — so the hub asks this and, only when
// an instance arrived without its records, those.
func BrowseQuery(txid uint16) []byte { return BuildQuery(txid, ServiceName, typePTR) }

// InstanceQuery asks one instance for its SRV and TXT; HostQuery asks a hostname
// for its A record. Both exist because a browser that only ever asks the group
// question has to answer "which address" from a record it may not have heard yet.
func InstanceQuery(txid uint16, instance string, qtype uint16) []byte {
	return BuildQuery(txid, instance+"."+ServiceName, qtype)
}

// HostQuery asks for a hostname's address.
func HostQuery(txid uint16, hostname string) []byte { return BuildQuery(txid, hostname, typeA) }

// ServiceInstance splits "<instance>._orb._tcp.local" into its leftmost label.
// Anything that is not shaped like that is refused rather than guessed at, since
// the leftmost label is what the hub displays and what the MAC beside it has to
// agree with.
func ServiceInstance(name string) (string, bool) {
	n := strings.ToLower(strings.TrimSuffix(name, "."))
	suffix := "." + ServiceName
	if !strings.HasSuffix(n, suffix) {
		return "", false
	}
	inst := strings.TrimSuffix(n, suffix)
	if inst == "" || strings.Contains(inst, ".") {
		return "", false
	}
	return inst, true
}

// InstanceName is the reverse: the FQDN an instance's records are owned by.
func InstanceName(instance string) string { return instance + "." + ServiceName }

// HostnameFor is the A-record name the firmware forms for an instance.
func HostnameFor(instance string) string { return instance + ".local" }

// ExpectInstance checks the announcement against the rule the device builds it
// from: instance is "orb-" plus the last six hex digits of the base MAC
// (orb_mdns.c mdns_update's six %c arguments off mac[6..11]). This is checked
// here, at the hub, rather than assumed, because it is the one relation that makes
// an announcement a claim about a chip instead of a claim about a name: an orb
// that says orb-782618 while its TXT names a MAC ending in something else is a
// stranger, and the row that mixes the two is the row a fleet list gets wrong.
func ExpectInstance(instance, mac string) error {
	want := "orb-" + strings.ToLower(mac)[len(mac)-6:]
	if instance != want {
		return fmt.Errorf("instance %q does not name the mac it advertises (expected %q)", instance, want)
	}
	return nil
}

// ParseOrbAddress reads a candidate's IPv4 out of an A record. It refuses anything
// that is not a plain unicast IPv4: no 0.0.0.0, no multicast group, no loopback,
// no v6-in-v4 wrapper. A candidate address is the seed of a later dial, and the
// cheaper place to refuse a nonsense one is here rather than in the proxy.
func ParseOrbAddress(ip net.IP) (string, error) {
	v4 := ip.To4()
	if v4 == nil {
		return "", fmt.Errorf("orbmdns: %v is not an IPv4 A record", ip)
	}
	if v4.IsUnspecified() || v4.IsLoopback() || v4.IsMulticast() {
		return "", fmt.Errorf("orbmdns: A record %v is not an orb address", v4)
	}
	return v4.String(), nil
}
