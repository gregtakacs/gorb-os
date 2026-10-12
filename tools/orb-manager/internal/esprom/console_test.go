package esprom

import (
	"strings"
	"testing"
)

// byteReader feeds readConsole from one continuous buffer, serving at most one
// OS-sized read (len(p)) per call then returning 0 like a serial read-timeout
// tick. Because it hands back <=len(p) bytes at a time it reproduces the line
// being split across reads without the fake losing the remainder of a chunk.
type byteReader struct {
	buf []byte
	i   int
}

func (b *byteReader) Read(p []byte) (int, error) {
	if b.i >= len(b.buf) {
		return 0, nil // read-timeout tick
	}
	n := copy(p, b.buf[b.i:])
	b.i += n
	return n, nil
}

// TestReadConsoleFraming locks the console line splitter that decides the whole
// AP branch: chunks must reassemble across read boundaries, CRLF must be
// stripped, each complete line delivered once, and a newline-less blob past the
// buffer cap must be flushed rather than lost. onLine returns true to stop
// early, exactly as the boot classifier does once it has its verdict.
func TestReadConsoleFraming(t *testing.T) {
	// A provisioning banner that the 1024-byte read window will split mid-line.
	stream := "I (1) orb_net: booting\r\n" +
		`W (1924) orb_net: ` + strings.Repeat("x", 1000) + "\r\n" +
		"W (1924) orb_net: no stored network — provisioning AP \"orb-8089\" (open)\r\n"
	r := &byteReader{buf: []byte(stream)}
	got := []string{}
	stop := false
	err := readConsole(nil, r, 5, func(line string) bool {
		got = append(got, line)
		if strings.Contains(line, "provisioning AP") {
			stop = true
			return true
		}
		return false
	})
	if err != nil {
		t.Fatalf("readConsole: %v", err)
	}
	if len(got) != 3 {
		t.Fatalf("lines = %d, want 3 (each \\r\\n-terminated line delivered whole)", len(got))
	}
	if got[0] != "I (1) orb_net: booting" {
		t.Errorf("line0 = %q, want trimmed first line", got[0])
	}
	want := `W (1924) orb_net: no stored network — provisioning AP "orb-8089" (open)`
	if got[2] != want {
		t.Errorf("final line = %q, want %q", got[2], want)
	}
	if !stop {
		t.Error("classifier did not signal early stop on the provisioning banner")
	}
}

// TestReadConsoleFlushesOversizedNoise proves a device emitting a newline-less
// blob (boot noise) past the buffer cap is reported as one line instead of
// growing the partial buffer forever.
func TestReadConsoleFlushesOversizedNoise(t *testing.T) {
	r := &byteReader{buf: []byte(strings.Repeat("A", 5000))} // > 4*1024 cap, no newline
	flushed := ""
	_ = readConsole(nil, r, 5, func(line string) bool {
		flushed = line
		return true
	})
	if len(flushed) < 4000 || strings.ContainsAny(flushed, "\r\n") {
		t.Errorf("oversized flush = %d bytes, want the whole newline-less blob", len(flushed))
	}
}
