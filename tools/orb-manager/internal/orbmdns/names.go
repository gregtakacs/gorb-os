package orbmdns

import (
	"fmt"
	"net"
	"strings"
)

// The names an orb's records are made of, and the two checks that turn a name into
// a claim about a chip. This file used to be query.go and built the browse
// question, the RFC 6762 §7.1 known-answer list, and the SRV/TXT/A follow-ups. The
// hub does not ask anything anymore: discovery runs entirely on the firmware's
// self-refresh check-ins, so the browser listens and these helpers are all that
// survive of the querier. Nothing here encodes a packet.

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

// OrbHost is HostnameFor inverted: it reports whether an A record's owner name is
// shaped like an orb's host ("orb-<instance>.local"), and returns the instance. The
// hub cannot know a host belongs to an orb until an SRV points at it, but the
// firmware only ever forms orb hostnames as instance+".local" with an "orb-"
// instance, so the shape is the cheap filter that keeps a neighbour's A record out
// of the orb pipeline instead of into a refusal the card would render.
func OrbHost(name string) (string, bool) {
	n := strings.ToLower(strings.TrimSuffix(name, "."))
	host, ok := strings.CutSuffix(n, ".local")
	if !ok || host == "" || strings.Contains(host, ".") {
		return "", false
	}
	// The leftmost label is the instance in full, "orb-782618" — the same string a
	// PTR's ServiceInstance returns and what a refusal row is named by. The "orb-"
	// prefix is only the shape check that proves the host could be an orb's; it is
	// not stripped, so the returned instance and the instance in the FQDN agree.
	if !strings.HasPrefix(host, "orb-") || host == "orb-" {
		return "", false
	}
	return host, true
}

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
