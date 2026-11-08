//go:build darwin

package orbfake

import "syscall"

// reusePort is the second half of sharing 5353 on macOS. mDNSResponder holds the port
// with SO_REUSEPORT, and on BSD a second wildcard bind to the same port succeeds only
// when both sockets set it; SO_REUSEADDR alone fails with EADDRINUSE on every
// interface, which the hub reports as "no interface could join". The stdlib's
// multicast helper sets both on BSD for the same reason. Multicast datagrams still go
// to every socket holding the port, which is all the browser listens for.
func reusePort(fd int) error {
	return syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_REUSEPORT, 1)
}
