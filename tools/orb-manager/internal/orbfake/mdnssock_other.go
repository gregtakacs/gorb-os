//go:build !linux && !darwin

package orbfake

// The Windows side of the advertiser's socket setup. The fake still advertises and is
// still browsable by a third-party browser on another host: it binds the group through
// the one portable call the stdlib offers.
//
// What these platforms do not get is the two options the Unix file sets, and both cost
// something real. Without SO_REUSEADDR before bind, a responder already holding 5353
// makes the bind fail loudly instead of the fake quietly sharing the port. Without
// IP_MULTICAST_LOOP turned on, a browser on the SAME host may not hear the
// announcements, so the same-host loop that the bench relies on is a Linux and macOS
// arrangement. The item is judged on real packets off the wire (PLAN §5.1 point 6), and
// that judgement is made where the hub and the fake actually run.

import (
	"fmt"
	"net"
)

func groupSock(port int, group net.IP) (PacketSock, error) {
	uc, err := net.ListenMulticastUDP("udp4", nil, &net.UDPAddr{IP: group, Port: port})
	if err != nil {
		return nil, fmt.Errorf("orbfake: join %s on udp/%d: %w", group, port, err)
	}
	return uc, nil
}
