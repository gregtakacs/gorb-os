//go:build windows

package orbmdns

// The Windows browser socket, the twin of mcastsock_unix.go. It exists because
// net.ListenMulticastUDP — the fallback mcastsock_other.go reaches for — is wrong
// for a browser here, in a way that is invisible until you look at how an orb answers.
//
// The helper binds the socket to the GROUP address, not the wildcard: udpsock_posix.go
// (whose build tag includes windows) calls internetSocket with gaddr as the local
// address, so the socket is bound to 224.0.0.251:5353. A UDP socket bound to a
// specific address is delivered only the datagrams destined for that address. And an
// orb does NOT answer a browse to the group: orb_mdns.c's receive loop replies with
// sendto(fd, tx, .., &from, ..), a UNICAST packet to whatever address the query came
// from — the hub's LAN address, say 192.168.2.48. So a group-bound Windows socket
// hears an orb's boot-time announce (that one is multicast, destined for the group)
// and silently discards every answer to a query it sent itself. The operator sees a
// list that only ever refreshes on a reboot. This is exactly the failure a Linux dev
// box never shows, because mcastsock_unix.go already binds the wildcard and joins the
// group separately — the same three setsockopt calls the stdlib helper declines to make.
//
// The options mirror the Unix file for the same reasons, with the two spellings that
// differ on this platform called out where they are used:
//
//   - SO_REUSEADDR before bind, so the browse can stand alongside a responder already
//     holding 5353; on Windows the fd a Control hook receives is a socket HANDLE, not
//     an int, and every Setsockopt* here takes that Handle.
//   - IP_MULTICAST_IF as a bare interface address (SetsockoptInet4Addr), the spelling
//     the stdlib's own Windows multicast path uses, not the ip_mreq the Unix file's
//     first-word trick feeds it — the two platforms read that option's argument
//     through different structs.
//   - IP_MULTICAST_LOOP on, matching Unix, so an orb faked on the same Windows host is
//     still heard. Ingest drops the query the socket sends itself (browse.go's
//     !m.Response guard), so the loop is harmless to real browsing.
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

// groupSock opens one socket on port, joined to group on iface, its sends leaving on
// iface. iface may be nil, which lets the stack pick. The wildcard bind is the point:
// it is what lets the socket receive the unicast answers an orb sends back to us.
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
