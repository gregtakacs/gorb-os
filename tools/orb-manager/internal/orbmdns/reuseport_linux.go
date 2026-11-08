//go:build linux

package orbmdns

// reusePort is a no-op on Linux: SO_REUSEADDR alone shares 5353 with avahi and
// systemd-resolved, and SO_REUSEPORT would split unicast datagrams across sockets.
// macOS needs it; see reuseport_darwin.go.
func reusePort(int) error { return nil }
