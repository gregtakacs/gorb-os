//go:build !linux && !darwin

package orbmdns

// The Windows side of the browser's socket, and the reason it is separate: the two
// options the Unix file sets are the ones net.ListenMulticastUDP does not offer a
// caller, and this platform's stdlib gives no RawConn hook with the same guarantees.
//
// What that costs, stated rather than left to be discovered:
//
//   - Without SO_REUSEADDR before bind, a responder already holding 5353 makes the
//     bind fail. The hub survives that — startBrowse reports the failure on the
//     discovery card rather than dying — but the browse is off until something frees
//     the port.
//   - IP_MULTICAST_LOOP is left at the stdlib's choice, so a browser and an advertiser
//     on the SAME Windows host may not see each other. The same-host browse loop is a
//     Linux and macOS arrangement; on Windows, browse a real orb over the LAN.
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
