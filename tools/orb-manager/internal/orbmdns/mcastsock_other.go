//go:build !linux && !darwin && !windows

package orbmdns

// The exotic-platform side of the browser's socket (plan9, js, wasip1 — everything
// neither mcastsock_unix.go nor mcastsock_windows.go claims). Windows is deliberately
// excluded: net.ListenMulticastUDP binds to the group address and so drops the unicast
// answers an orb sends back to us, which mcastsock_windows.go exists to avoid. That
// reasoning does not apply to the platforms left here — none of them is a target this
// hub runs discovery on — so the stdlib helper is acceptable as their fallback.
//
// What that costs, stated rather than left to be discovered:
//
//   - Without SO_REUSEADDR before bind, a responder already holding 5353 makes the
//     bind fail. The hub survives that — startBrowse reports the failure on the
//     discovery card rather than dying — but the browse is off until something frees
//     the port.
//   - IP_MULTICAST_LOOP is left at the stdlib's choice, so a browser and an advertiser
//     on the SAME host may not see each other.
//   - The bind is to the group, so a unicast answer to a query is not received here
//     either. If a supported platform is ever added whose users browse real orbs, it
//     needs the wildcard-bind treatment mcastsock_windows.go gives Windows, not this.
//
// The browser itself behaves identically either way: it never dials what it lists, and
// it reports the interfaces it joined, so an empty list on a box that could not bind
// reads as that rather than as a LAN with no orbs.

import (
	"fmt"
	"net"
)

func groupSock(port int, iface *net.Interface, group net.IP) (*net.UDPConn, error) {
	g := &net.UDPAddr{IP: group, Port: port}
	c, err := net.ListenMulticastUDP("udp4", iface, g)
	if err != nil {
		return nil, fmt.Errorf("join via the stdlib: %w", err)
	}
	return c, nil
}
