//go:build linux || darwin

package orbfake

// Unix socket plumbing for the advertiser. internal/orbmdns carries its own
// equivalent rather than sharing this one: the fake must not borrow the code it is
// here to exercise.
//
// Two options a caller of net.ListenPacket cannot set, one of which is the
// difference between discovery that works on this bench and discovery that cannot:
//
//   - SO_REUSEADDR before bind, so the fake coexists with avahi or systemd-resolved
//     on the dev box instead of dying with EADDRINUSE and reading as a broken
//     advertiser. The device sets the same option (orb_mdns.c's SO_REUSEADDR).
//   - IP_MULTICAST_LOOP on. net.ListenMulticastUDP turns it OFF
//     (udpsock_posix.go's listenIPv4MulticastUDP), so an advertiser built that way can
//     never be heard by a browser on the SAME host — which is exactly this bench, with
//     the fake, the hub and tools/bench/browse_mdns.py all on one box. A real orb has
//     no such problem: one interface, one wire. Leaving the stdlib default in place
//     would have made the item untestable here and reported a LAN with no orbs on it,
//     which §5.1 point 6 forbids.
//
// Membership is joined per interface: the firmware's two joins, STA and SoftAP,
// expressed as one membership per eligible interface.

import (
	"context"
	"fmt"
	"net"
	"syscall"
)

// reuseBeforeBind is the net.ListenConfig Control hook. It runs after socket() and
// before bind(), the only window in which SO_REUSEADDR can make this bind stand
// alongside another process's.
func reuseBeforeBind(_, _ string, c syscall.RawConn) error {
	var opErr error
	if err := c.Control(func(fd uintptr) {
		f := int(fd)
		if opErr = syscall.SetsockoptInt(f, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); opErr != nil {
			return
		}
		// SO_REUSEADDR alone is the option the firmware sets (orb_mdns.c) and the one
		// avahi and systemd-resolved set, so the fake shares 5353 with them instead of
		// failing to start. SO_REUSEPORT is deliberately not asked for: it is absent
		// from syscall on Linux, and with it the kernel hands each incoming datagram
		// to only one of the sockets holding the port — which would split the fake's
		// own answers across the hub's per-interface sockets and lose them.
	}); err != nil {
		return opErr
	}
	return opErr
}

// groupSock opens the advertiser's socket on port, joined to group on every eligible
// interface, with the same-host loopback on.
func groupSock(port int, group net.IP) (PacketSock, error) {
	lc := net.ListenConfig{Control: reuseBeforeBind}
	pc, err := lc.ListenPacket(context.Background(), "udp4", fmt.Sprintf("0.0.0.0:%d", port))
	if err != nil {
		return nil, fmt.Errorf("orbfake: bind udp/%d: %w", port, err)
	}
	uc, ok := pc.(*net.UDPConn)
	if !ok {
		_ = pc.Close()
		return nil, fmt.Errorf("orbfake: udp/%d came back as %T", port, pc)
	}
	ifaces, err := eligibleInterfaces()
	if err != nil {
		_ = uc.Close()
		return nil, err
	}
	joined, lastErr := 0, error(nil)
	for i := range ifaces {
		c, err := uc.SyscallConn()
		if err != nil {
			lastErr = err
			continue
		}
		if err := joinOne(c, &ifaces[i], group); err != nil {
			lastErr = err
			continue
		}
		joined++
	}
	if joined == 0 {
		_ = uc.Close()
		if lastErr == nil {
			lastErr = fmt.Errorf("no interface was eligible")
		}
		return nil, fmt.Errorf("orbfake: join %s on udp/%d: %w", group, port, lastErr)
	}
	return uc, nil
}

// joinOne turns on the same-host loopback and adds the membership on ifi, naming the
// interface by its own IPv4 address in the second word of struct ip_mreq — the first
// word is the group, and mixing the two up is the easy mistake in these option names.
// Sending is left to the kernel's route for the group: IP_MULTICAST_IF's integer form
// means an ifindex on Linux and an address on BSD, and an advertiser that preferred a
// VPN adapter to the route table would be worse than one that did not choose at all.
func joinOne(c syscall.RawConn, ifi *net.Interface, group net.IP) error {
	var opErr error
	err := c.Control(func(fd uintptr) {
		f := int(fd)
		if opErr = syscall.SetsockoptInt(f, syscall.IPPROTO_IP, syscall.IP_MULTICAST_LOOP, 1); opErr != nil {
			return
		}
		mr := syscall.IPMreq{Multiaddr: ip4(group)}
		if a := firstIPv4(ifi); a != nil {
			mr.Interface = ip4(a)
		}
		opErr = syscall.SetsockoptIPMreq(f, syscall.IPPROTO_IP, syscall.IP_ADD_MEMBERSHIP, &mr)
	})
	if err != nil {
		return err
	}
	return opErr
}

func ip4(ip net.IP) [4]byte {
	var out [4]byte
	if v4 := ip.To4(); v4 != nil {
		copy(out[:], v4)
	}
	return out
}

func firstIPv4(ifi *net.Interface) net.IP {
	addrs, err := ifi.Addrs()
	if err != nil {
		return nil
	}
	for _, a := range addrs {
		if n, ok := a.(*net.IPNet); ok {
			if v4 := n.IP.To4(); v4 != nil {
				return v4
			}
		}
	}
	return nil
}

// eligibleInterfaces is the advertiser's own copy of the interface rule, for the
// same reason the socket setup is its own: borrowing the browser's helper would let
// the two agree on an interface policy the firmware does not have.
func eligibleInterfaces() ([]net.Interface, error) {
	all, err := net.Interfaces()
	if err != nil {
		return nil, fmt.Errorf("orbfake: interfaces: %w", err)
	}
	var out []net.Interface
	for _, ifi := range all {
		if ifi.Flags&net.FlagUp == 0 || ifi.Flags&net.FlagMulticast == 0 {
			continue
		}
		if ifi.Flags&net.FlagLoopback == 0 && ifi.Flags&net.FlagRunning == 0 {
			continue
		}
		if firstIPv4(&ifi) == nil {
			continue
		}
		out = append(out, ifi)
	}
	return out, nil
}
