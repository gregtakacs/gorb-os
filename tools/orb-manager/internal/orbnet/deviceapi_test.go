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
// declaredBody returns the request's declared Content-Length and how many body
// bytes have arrived, and ok only once the header block is terminated — the
// fake server's one job is to know when it has heard all of the upload.
func declaredBody(b []byte) (want, got int, ok bool) {
	sep := bytes.Index(b, []byte("\r\n\r\n"))
	if sep < 0 {
		return 0, 0, false
	}
	lower := bytes.ToLower(b[:sep])
	i := bytes.Index(lower, []byte("content-length: "))
	if i < 0 {
		return 0, 0, false
	}
	j := i + len("content-length: ")
	for ; j < sep && b[j] >= '0' && b[j] <= '9'; j++ {
		want = want*10 + int(b[j]-'0')
	}
	return want, len(b) - (sep + 4), true
}

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
			// Drain the request like the firmware does. h_theme_upload reads
			// the WHOLE declared body before it answers — it refuses a chunked
			// body outright for the same reason — and an early answer here
			// closes the socket under the transport's still-streaming write:
			// the client then surfaces "use of closed network connection"
			// instead of the honest resend, and which of the two won was a
			// scheduler coin-flip under -race. Read until the declared length
			// has arrived, then answer.
			b := make([]byte, 0, 8192)
			chunk := make([]byte, 4096)
			for {
				n, rerr := c.Read(chunk)
				b = append(b, chunk[:n]...)
				if want, body, ok := declaredBody(b); ok && body >= want {
					break
				}
				if rerr != nil {
					break
				}
			}
			mu.Lock()
			seen = append(seen, string(b[:min(len(b), 4096)]))
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
	// The restart is the FINAL zero: net/http reads a small whole body into its
	// write buffer before the RST surfaces when scheduling lets the write drain,
	// and reports nothing at all when the poison lands first — so the failed
	// attempt may show a full count before the 0, or no count at all. Either
	// way the re-send's own shape is the claim: a 0 immediately followed by the
	// whole body recounted. (A "restart after index 0" assertion silently
	// rejected the second ordering, and this test failed under -race for it.)
	if len(progress) < 2 || progress[len(progress)-2] != 0 {
		t.Fatalf("progress never restarted for the retry: %v", progress)
	}
}
