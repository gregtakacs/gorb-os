//go:build windows

package orbmdns

// The Windows browser socket, the twin of mcastsock_unix.go. The hub is a pure
// listener now — the firmware's self-refresh announce is what keeps a row alive,
// so the browser never sends — and this socket exists to receive that announce
// without the two failure modes the stdlib helper would reintroduce:
//
//   - The helper binds to the GROUP address (udpsock_posix.go's listenIPv4MulticastUDP
//     passes gaddr as the local address), so the socket is bound to 224.0.0.251:5353
//     and is delivered only datagrams destined for that address. A wildcard bind also
//     receives anything sent to the hub's own LAN address — a firmware answer routed
//     unicast, a diagnostic, a probe reply — which is the difference between a list
//     that can see the whole LAN and one that can see the group alone.
//   - The helper sets SO_REUSEADDR after the bind, and on Windows that window is
//     closed by then. Windows' DNS Client service holds 5353 on nearly every desktop,
//     so a bind without the option set first fails outright and the hub ships with no
//     discovery at all. This file exists because that failure is silent: an empty list
//     and a LAN with no orbs look identical. The same three setsockopt calls the
//     stdlib helper declines to make, spelled for this platform: SO_REUSEADDR before
//     bind, IP_MULTICAST_IF per interface, IP_MULTICAST_LOOP on so an orb faked on the
//     same host is still heard. Ingest drops questions and echoes (browse.go's
//     !m.Response guard), so the loop changes nothing.
//
// This needs no dependency outside the standard library: syscall's Windows build
// carries SetsockoptInt, SetsockoptIPMreq and IP_ADD_MEMBERSHIP itself, so go.mod,
// go.sum and the vendor tree are untouched and vendor-check stays green.

import (
	"context"
	"fmt"
	"net"
	"syscall"
)

// groupSock opens one socket on port, joined to group on iface. iface may be nil,
// which lets the stack pick. The wildcard bind is the point: it is what lets the
// socket receive traffic addressed to the hub's own LAN address as well as the group.
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
// SO_REUSEADDR can make this bind stand alongside another process's. On Windows the
// handle a Control hook yields is the socket itself, so it is widened back to a
// syscall.Handle before the option is set.
func reuseBeforeBind(_, _ string, c syscall.RawConn) error {
	var opErr error
	if err := c.Control(func(fd uintptr) {
		opErr = syscall.SetsockoptInt(syscall.Handle(fd), syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
	}); err != nil {
		return opErr
	}
	return opErr
}

// join names iface in both directions: as the source of the sends (IP_MULTICAST_IF)
// and as the interface of the membership (IP_ADD_MEMBERSHIP). Both are done after bind,
// so the socket is already listening when the membership lands and cannot miss an
// announce that arrives during startup. iface may be nil, and the egress pin is best
// effort — an interface address not claimable this instant still gets the membership,
// which is the half that decides whether an answer arrives at all.
func join(c syscall.RawConn, iface *net.Interface, group net.IP) error {
	var opErr error
	err := c.Control(func(fd uintptr) {
		h := syscall.Handle(fd)
		if opErr = syscall.SetsockoptInt(h, syscall.IPPROTO_IP, syscall.IP_MULTICAST_LOOP, 1); opErr != nil {
			return
		}
		if iface != nil {
			if a := ifaceIPv4(iface); a != nil {
				_ = syscall.SetsockoptInet4Addr(h, syscall.IPPROTO_IP, syscall.IP_MULTICAST_IF, addr4(a))
			}
		}
		mr := syscall.IPMreq{Multiaddr: addr4(group)}
		if iface != nil {
			mr.Interface = addr4(ifaceIPv4(iface))
		}
		opErr = syscall.SetsockoptIPMreq(h, syscall.IPPROTO_IP, syscall.IP_ADD_MEMBERSHIP, &mr)
	})
	if err != nil {
		return err
	}
	return opErr
}

// addr4 and ifaceIPv4 duplicate the Unix file's helpers of the same name. The
// duplication is deliberate and bounded to these four lines each: the two files are
// build-tag exclusive, so they never compile together and a shared file would pull a
// linux-only or windows-only symbol into every build of the package. Keep them in step.
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
