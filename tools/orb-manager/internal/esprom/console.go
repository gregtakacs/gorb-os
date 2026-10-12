package esprom

import (
	"fmt"
	"strings"
	"time"
)

// observeBootOn reads the boot console on an ALREADY-OPEN port held across a
// reset the ROM honoured, for the post-HardReset tail of a flash session. This
// is the only reliable place to catch the firmware's one-shot provisioning
// banner: IDF's USB-Serial-JTAG console DROPS output while no host holds the
// port with DTR asserted, and USB-Serial-JTAG has no EN wire (reset.go), so a
// running app ignores an RTS pulse — a reset and read must happen while the
// ROM still owns the peripheral, which is exactly here. It adds a settle delay
// for app_main to start printing and, if the ROM-to-app transition drops the
// fd across CDC re-enumeration, reopens once and continues.
func observeBootOn(pt *Port, secs int, onLine func(string) bool) {
	_ = pt.SetDTR(true) // IDF: terminal ready — otherwise console bytes are dropped
	time.Sleep(150 * time.Millisecond)
	if err := readConsole(pt, secs, onLine); err == nil {
		return
	}
	// The fd went stale across re-enumeration; reopen once and keep reading.
	if pt2, err := openForConsole(pt.name); err == nil {
		_ = pt2.SetDTR(true)
		_ = readConsole(pt2, secs, onLine)
		_ = pt2.Close()
	}
}

// openForConsole opens name, retrying for openBudget to survive the CDC
// re-enumeration a reset causes, and arms the short read timeout.
func openForConsole(name string) (*Port, error) {
	const openBudget = 8 * time.Second
	deadline := time.Now().Add(openBudget)
	for {
		pt, err := OpenPort(name)
		if err == nil {
			return pt, nil
		}
		if time.Now().After(deadline) {
			return nil, fmt.Errorf("esprom: console open %s: %w", name, err)
		}
		time.Sleep(250 * time.Millisecond)
	}
}

// lineReader is the read side readConsole needs, so the console framing can be
// exercised against a fake without a real serial device.
type lineReader interface {
	Read([]byte) (int, error)
}

// readConsole forwards each complete line from an already-open port for up to
// secs, stopping early when onLine returns true.
func readConsole(pt lineReader, secs int, onLine func(string) bool) error {
	stop := time.Now().Add(time.Duration(secs) * time.Second)
	buf := make([]byte, 1024)
	var partial []byte
	send := func(line string) bool {
		line = strings.TrimRight(line, "\r\n \t")
		if line == "" {
			return false
		}
		return onLine(line)
	}
	for time.Now().Before(stop) {
		n, rerr := pt.Read(buf)
		if rerr != nil {
			return fmt.Errorf("esprom: console read: %w", rerr)
		}
		if n == 0 { // read-timeout tick, not an error (SetReadTimeout in OpenPort)
			continue
		}
		partial = append(partial, buf[:n]...)
		for {
			i := indexAny(partial, '\n')
			if i < 0 {
				break
			}
			if send(string(partial[:i])) {
				return nil
			}
			partial = append(partial[:0], partial[i+1:]...)
		}
		// A device that never ends a line (binary noise) must not grow the
		// buffer forever: flush it as one oversized "line" past the cap.
		if len(partial) > 4*len(buf) {
			if send(string(partial)) {
				return nil
			}
			partial = partial[:0]
		}
	}
	if len(partial) > 0 {
		send(string(partial))
	}
	return nil
}

func indexAny(b []byte, c byte) int {
	for i, x := range b {
		if x == c {
			return i
		}
	}
	return -1
}
