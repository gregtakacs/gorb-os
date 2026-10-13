package orbmdns

import (
	"fmt"
	"net"
	"strings"
)

// CanonicalMAC turns any of the spellings this repo meets — colon or dash
// separated, upper or lower, or the separator-free twelve the device itself
// emits — into the twelve lowercase hex digits the fleet keys on, or refuses.
//
// This is deliberately NOT watch.go's macNorm. That function lowercases and maps
// '-' to ':', which leaves the separator-free form a device actually emits
// uncollapsed and lets two empty MACs match each other. An empty MAC here is a
// refusal, never a key: a fleet row that could be joined by two boards reporting
// nothing is the row that puts the wrong face on the wrong watch.
func CanonicalMAC(s string) (string, string) {
	t := strings.ToLower(strings.TrimSpace(s))
	if t == "" {
		return "", "no mac in the announcement"
	}
	t = strings.NewReplacer(":", "", "-", "", ".", "", " ", "").Replace(t)
	if len(t) != 12 {
		return "", fmt.Sprintf("mac %q is %d hex digits, not 12", s, len(t))
	}
	for i := 0; i < len(t); i++ {
		c := t[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return "", fmt.Sprintf("mac %q is not hex", s)
		}
	}
	// The all-zero and all-one addresses are what a driver that never read the
	// eFuse reports. They are not an identity, and a row keyed on them would
	// merge every such board into one.
	if t == "000000000000" || t == "ffffffffffff" {
		return "", fmt.Sprintf("mac %q is not a chip address", s)
	}
	return t, ""
}

// MulticastSource binds one interface's mDNS socket, joining the group on it.
//
// One socket per interface rather than one socket with N joins — the orb's own budget
// forces its hand and the hub has no such margin to defend. Binding fails loudly here
// rather than silently, because a browser that never joined the group is
// indistinguishable from a network with no orbs on it, which is the exact false pass
// §0.3 forbids reporting.
//
// The socket carries the three options the stdlib's own multicast helper does not give
// a caller — see mcastsock_unix.go for why each one is load-bearing here.
func MulticastSource(iface *net.Interface) (*MulticastConn, error) {
	g := net.ParseIP(GroupAddr)
	if g == nil {
		return nil, fmt.Errorf("orbmdns: %q is not a group address", GroupAddr)
	}
	c, err := groupSock(Port, iface, g)
	if err != nil {
		if iface == nil {
			return nil, fmt.Errorf("orbmdns: join %s on the default interface: %w", GroupAddr, err)
		}
		return nil, fmt.Errorf("orbmdns: join %s on %s: %w", GroupAddr, iface.Name, err)
	}
	name := ""
	if iface != nil {
		name = iface.Name
	}
	return &MulticastConn{c: c, iface: name}, nil
}

// MulticastConn is a production Source: one bound, joined UDP socket.
type MulticastConn struct {
	c     *net.UDPConn
	iface string
}

// Iface names the interface this socket hears on, for a candidate's provenance.
func (m *MulticastConn) Iface() string { return m.iface }

// ReadFrom reads one datagram.
func (m *MulticastConn) ReadFrom(p []byte) (int, net.Addr, error) { return m.c.ReadFrom(p) }

// WriteTo sends one query. The destination is always the group: this socket has no
// way to aim a query at a single host, which is the point.
func (m *MulticastConn) WriteTo(p []byte, addr net.Addr) (int, error) { return m.c.WriteTo(p, addr) }

// Close releases the socket.
func (m *MulticastConn) Close() error { return m.c.Close() }

// EligibleInterfaces returns the up, running, multicast-capable IPv4 interfaces a
// browse should run on. Loopback is included: on this bench, and in every CI run,
// it is the only interface a browser and a fake can meet on, and excluding it
// would make the production path and the tested path differ in the one place that
// matters. Down and point-to-point-only interfaces are skipped, because joining a
// group on them spends a descriptor for traffic that cannot arrive.
func EligibleInterfaces() ([]*net.Interface, error) {
	all, err := net.Interfaces()
	if err != nil {
		return nil, fmt.Errorf("orbmdns: interfaces: %w", err)
	}
	var out []*net.Interface
	for i := range all {
		iface := &all[i]
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagMulticast == 0 {
			continue
		}
		if iface.Flags&net.FlagLoopback == 0 && iface.Flags&net.FlagRunning == 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		v4 := false
		for _, a := range addrs {
			if n, ok := a.(*net.IPNet); ok && n.IP.To4() != nil {
				v4 = true
			}
		}
		if v4 {
			out = append(out, iface)
		}
	}
	return out, nil
}
