package orbnet

import (
	"bytes"
	"net"
	"strings"
	"sync"
	"testing"
	"time"
)

// TestDevicePostResendsOnceOnDeadSocket is the regression for the
// 2026-10-23 "connection reset by peer" on the FIRST Send after a store: the
// store reboots the orb (or its 3-socket httpd lru-purges a racing request),
// and the POST dies at the socket with no response. The first attempt must
// cost nothing but a re-dial: same path, the WHOLE body again (h_theme_upload
// truncates its scratch at begin, so a re-send always matches Content-Length),
// and the answer comes from the second connection.
func TestDevicePostResendsOnceOnDeadSocket(t *testing.T) {
	var mu sync.Mutex
	var conns int
	var seen []string
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()

	go func() {
		for {
			c, e := ln.Accept()
			if e != nil {
				return
			}
			mu.Lock()
			conns++
			first := conns == 1
			mu.Unlock()
			if first {
				// Behave like a peer that died with bytes in flight: no
				// response at all, hard RST via SO_LINGER 0.
				if tc, ok := c.(*net.TCPConn); ok {
					_ = tc.SetLinger(0)
				}
				_ = c.Close()
				continue
			}
			b := make([]byte, 4096)
			n, _ := c.Read(b)
			mu.Lock()
			seen = append(seen, string(b[:n]))
			mu.Unlock()
			_, _ = c.Write([]byte("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" +
				"Content-Length: 12\r\nConnection: close\r\n\r\n{\"ok\":true}\n"))
			_ = c.Close()
		}
	}()

	host, port, _ := net.SplitHostPort(ln.Addr().String())
	c := NewClient(time.Second)
	body := bytes.Repeat([]byte("x"), 5000)
	var progress []int64
	raw, err := c.devicePostCounted(host+":"+port, "/api/theme/upload", body, "application/octet-stream",
		func(n int64) { progress = append(progress, n) })
	if err != nil {
		t.Fatalf("dead pooled connection surfaced to the caller: %v", err)
	}
	if !bytes.Contains(raw, []byte("ok")) {
		t.Fatalf("answer is not the second connection's: %s", raw)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(seen) != 1 || !strings.Contains(seen[0], "POST /api/theme/upload") {
		t.Fatalf("second connection never carried the POST: %#v", seen)
	}
	// The retry restarted the byte counter, so the meter starts over honestly.
	// (net/http reads a small whole body into its write buffer before the RST
	// surfaces, so the failed attempt can report the full count before the 0.)
	restarted := false
	for i, n := range progress {
		if i > 0 && n == 0 {
			restarted = true
		}
	}
	if !restarted {
		t.Fatalf("progress did not restart for the retry: %v", progress)
	}
}
