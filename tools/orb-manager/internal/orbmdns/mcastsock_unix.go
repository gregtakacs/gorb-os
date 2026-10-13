//go:build linux || darwin

package orbmdns

// The browser's socket, spelled out rather than taken from net.ListenMulticastUDP.
// Two reasons, both learned the hard way on this bench:
//
//   - SO_REUSEADDR before bind. The hub shares 5353 with whatever responder the dev
//     box already runs, and a bind that fails with EADDRINUSE looks exactly like a
//     LAN with no orbs on it — the false pass §0.3 refuses to report.
//   - IP_MULTICAST_LOOP explicit. The stdlib helper turns it OFF
//     (udpsock_posix.go's listenIPv4MulticastUDP), and OFF is correct for a host that
//     wants to ignore its own traffic. This one is not such a host for the bench's
//     sake: tools/bench/browse_mdns.py and cmd/orbfake both run on this machine, and
//     a browser with the loop off hears neither. Setting it ON costs nothing real —
//     Ingest drops a questions-only packet on the floor (browse.go's !m.Response
//     guard), so the query the socket sends comes back and changes nothing.
//
// The rule this package follows and orbfake does not: one interface per socket, so
// IP_MULTICAST_IF pins each socket's Ask to the interface its candidate came from.
// Asking out of the wrong interface is how a browse on a VPN box finds nothing and
// cannot say why.

import (
	"context"
	"fmt"
	"net"
	"syscall"
)

// groupSock opens one socket on port, joined to group on iface, its sends leaving on
// iface. iface may be nil, which lets the kernel pick.
func groupSock(port int, iface *net.Interface, group net.IP) (*net.UDPConn, error) {
	lc := net.ListenConfig{Control: reuseBeforeBind}
	pc, err := lc.ListenPacket(context.Background(), "udp4", fmt.Sprintf("0.0.0.0:%d", port))
	if err != nil {
		return nil, err
	}
	uc, ok := pc.(*net.UDPConn)
	if !ok {
		_ = pc.Close()
		return nil, fmt.Errorf("udp/%d came back as %T", port, pc)
	}
	c, err := uc.SyscallConn()
	if err == nil {
		err = join(c, iface, group)
	}
	if err != nil {
		_ = uc.Close()
		return nil, err
	}
	return uc, nil
}

// reuseBeforeBind runs after socket() and before bind() — the only window in which
// SO_REUSEADDR can make this bind stand alongside another process's.
func reuseBeforeBind(_, _ string, c syscall.RawConn) error {
	var opErr error
	if err := c.Control(func(fd uintptr) {
		f := int(fd)
		if opErr = syscall.SetsockoptInt(f, syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1); opErr != nil {
			return
		}
		// SO_REUSEADDR alone is what the firmware sets (orb_mdns.c) and what Linux
		// requires before several sockets may hold 5353 at once, which is the case
		// here: one socket per interface, each bound to the wildcard. It is also the
		// option avahi and systemd-resolved set, so the browser shares the port with
		// them instead of failing to start.
	}); err != nil {
		return opErr
	}
	return opErr
}

// join names iface in both directions: as the source address of the sends and as the
// interface of the membership. The two share no field, and that is the trap in these
// option names — for IP_MULTICAST_IF the kernel reads the FIRST word of struct ip_mreq
// as the local address, while IP_ADD_MEMBERSHIP reads it as the group and the second
// word as the interface. Two structs, one per option.
//
// The send-side choice is best effort: a box where the address is not claimable (a
// loopback, an address that just left the interface) still gets the membership, which
// is the half that decides whether an answer arrives at all. IP_MULTICAST_IF's integer
// form is deliberately unused — it means an ifindex on Linux and an address on BSD.
func join(c syscall.RawConn, iface *net.Interface, group net.IP) error {
	var opErr error
	err := c.Control(func(fd uintptr) {
		f := int(fd)
		if opErr = syscall.SetsockoptInt(f, syscall.IPPROTO_IP, syscall.IP_MULTICAST_LOOP, 1); opErr != nil {
			return
		}
		if iface != nil {
			if a := ifaceIPv4(iface); a != nil {
				_ = syscall.SetsockoptIPMreq(f, syscall.IPPROTO_IP, syscall.IP_MULTICAST_IF,
					&syscall.IPMreq{Multiaddr: addr4(a)})
			}
		}
		mr := syscall.IPMreq{Multiaddr: addr4(group)}
		if iface != nil {
			mr.Interface = addr4(ifaceIPv4(iface))
		}
		opErr = syscall.SetsockoptIPMreq(f, syscall.IPPROTO_IP, syscall.IP_ADD_MEMBERSHIP, &mr)
	})
	if err != nil {
		return err
	}
	return opErr
}

func addr4(ip net.IP) [4]byte {
	var out [4]byte
	if v4 := ip.To4(); v4 != nil {
		copy(out[:], v4)
	}
	return out
}

func ifaceIPv4(iface *net.Interface) net.IP {
	addrs, err := iface.Addrs()
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
