package esprom

import (
	"context"
	"encoding/hex"
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// Serial provisioning (Path 2): the wired door for a hub that cannot reach the
// orb's AP at 192.168.4.1. The orb, on a first boot with no credentials, arms a
// scan and logs each result as a PROVSCAN line and also blocks on stdin for one
// PROV line. This file parses the log up and formats the write down. The console
// is one-way by design: results flow up as text, credentials flow down as one
// line — no bidirectional protocol.
//
// Security note: the PROV line carries the passphrase in hex (obfuscating, not
// encrypting) over the user's own USB lead. That is acceptable for a wired,
// physically-local onboarding path and is documented in the README.

// ScanNet is one network as reported on the boot console.
type ScanNet struct {
	Index int    `json:"index"`
	SSID  string `json:"ssid"`
	RSSI  int    `json:"rssi"`
	Ch    int    `json:"ch"`
	Open  bool   `json:"open"`
}

// ParsePROVSCAN parses a firmware console line of the form
//
//	... PROVSCAN <i> <rssi> <ch> <open0|1> <ssid...>
//
// The SSID is the rest of the line (it may contain spaces; the firmware already
// removed control characters, so it cannot break the framing). ok is false for
// any line that is not a network row — the BEGIN/END banners and everything else.
func ParsePROVSCAN(line string) (ScanNet, bool) {
	i := strings.Index(line, "PROVSCAN ")
	if i < 0 {
		return ScanNet{}, false
	}
	rest := line[i+len("PROVSCAN "):]
	f := strings.SplitN(rest, " ", 5)
	if len(f) != 5 {
		return ScanNet{}, false
	}
	idx, e0 := strconv.Atoi(f[0])
	rssi, e1 := strconv.Atoi(f[1])
	ch, e2 := strconv.Atoi(f[2])
	op, e3 := strconv.Atoi(f[3])
	if e0 != nil || e1 != nil || e2 != nil || e3 != nil {
		return ScanNet{}, false
	}
	ssid := f[4]
	if ssid == "" {
		return ScanNet{}, false
	}
	return ScanNet{Index: idx, SSID: ssid, RSSI: rssi, Ch: ch, Open: op == 1}, true
}

// ProvField encodes one PROV wire field: hex of the bytes, or "-" when empty
// (the firmware maps "-" back to the empty string, distinguishing "open
// network" from an absent field without an ambiguous delimiter).
func ProvField(s string) string {
	if s == "" {
		return "-"
	}
	return hex.EncodeToString([]byte(s))
}

// ProvLine formats the single down-line the firmware reader expects:
//
//	PROV <hex-ssid> <hex-pass> <hex-tz>
//
// terminated by CRLF, not a bare LF. The ESP32-S3 USB-Serial-JTAG console hands
// a line to fgets() on its cooked-mode line boundary, which a terminal's Enter
// (\r\n) satisfies and a lone \n does not — the reason the read side looked
// "proven" from a human typing yet an automated \n-only writer never got a
// PROV-ACK. The firmware strips both \r and \n, so the terminator never reaches
// the parser. An empty tz encodes as "-" (leave the stored zone alone), matching
// the firmware's parser.
func ProvLine(ssid, pass, tz string) string {
	return fmt.Sprintf("PROV %s %s %s\r\n", ProvField(ssid), ProvField(pass), ProvField(tz))
}

// ProvResult is the outcome of one wired-provisioning round-trip.
type ProvResult struct {
	APSeen   bool      `json:"ap_seen"`
	Networks []ScanNet `json:"networks"`
	Sent     bool      `json:"sent"`
	Ack      bool      `json:"ack"`
	IP       string    `json:"ip,omitempty"` // DHCP lease read off the console after the join
}

// orbIPRe matches the firmware's post-lease console banner (orb_net.c on_ip):
//
//	I (NNNNN) orb_net: associated: config page at http://192.168.2.32/
//
// It is the only host-readable line that carries the orb's LAN address, so the
// IP comes back over the same USB lead the PROV line went down — no LAN sweep.
var orbIPRe = regexp.MustCompile(`associated: config page at https?://([0-9]{1,3}(\.[0-9]{1,3}){3})/`)

// parseOrbIP returns the dotted IPv4 the orb announced in its "associated:
// config page at" banner, or ("", false) for any other line.
func parseOrbIP(line string) (string, bool) {
	m := orbIPRe.FindStringSubmatch(line)
	if m == nil {
		return "", false
	}
	return m[1], true
}

// ParseOrbIP is the exported form of parseOrbIP, so the manager's flash-time
// boot-console reader can capture the same post-lease banner the provision door
// reads and record the orb's LAN address from USB alone.
func ParseOrbIP(line string) (string, bool) { return parseOrbIP(line) }

// ProvisionOn drives one wired-provisioning exchange on an ALREADY-OPEN port.
// It first forces a genuine fresh boot (EnterDownloadMode + HardReset) so the
// firmware re-arms both the boot scan and the stdin reader — those arm exactly
// once, at app startup while the device is unprovisioned; a board that booted
// long ago prints no PROVSCAN and never starts the reader. It then reads the
// boot console until the scan completes (PROVSCAN-END) or scanBudget elapses,
// writes the PROV line, and reads until PROV-ACK or ackBudget. After the ack it
// keeps reading up to ipBudget for the firmware's "associated: config page at
// http://<IP>/" banner, so the DHCP lease comes back over the same USB lead (no
// LAN sweep); res.IP is empty if the lease did not arrive in that window — a
// join still succeeds, the IP is simply not yet known. onLine, if set,
// sees every console line.
//
// The caller owns opening/closing pt. It must be for the UNPROVISIONED orb; a
// device that already has credentials boots as a station and never starts the
// reader, so the write would be ignored — use the orb's HTTP door to re-configure
// an already-joined orb.
//
// ctx aborts the exchange between console reads. Cancelling after the PROV line
// went down is safe: the firmware's reader is idempotent on the line, and an
// accepted join completes on the orb whether or not the hub is still watching.
func ProvisionOn(ctx context.Context, pt *Port, ssid, pass, tz string, scanBudget, ackBudget, ipBudget time.Duration, onLine func(string)) (*ProvResult, error) {
	res := &ProvResult{Networks: []ScanNet{}}
	if err := ctxErr(ctx); err != nil {
		return res, err
	}

	// Force a genuine fresh boot. The boot scan (PROVSCAN) and the stdin reader
	// are armed exactly once, at app startup, and only while the device came up
	// unprovisioned. Reading a port whose board booted long ago — or one that has
	// already joined as a station — sees no PROVSCAN, no SoftAP banner, and no
	// reader to take the PROV line: the exact "AP seen false / scanned 0 / ack
	// false" failure. Opening the CDC port does NOT reset it — go.bug.st/serial's
	// Open leaves DTR/RTS alone — and USB-Serial-JTAG has no EN wire, so a running
	// app also ignores a bare RTS pulse. The reliable reset is esptool's:
	// EnterDownloadMode pulls the ROM into ownership (its DTR/RTS state machine
	// works from a running app), then HardReset boots the app fresh. DTR is
	// asserted only AFTER the reset so the app boots with IO0 high (a normal boot,
	// not download mode) yet IDF still emits its post-boot provisioning console —
	// the same discipline observeBootOn relies on.
	if err := EnterDownloadMode(pt); err != nil {
		return res, fmt.Errorf("esprom: prov reset (download mode): %w", err)
	}
	if err := HardReset(pt); err != nil {
		return res, fmt.Errorf("esprom: prov reset (boot): %w", err)
	}
	_ = pt.SetDTR(true) // terminal ready: IDF drops console bytes otherwise
	if err := sleepCtx(ctx, 150*time.Millisecond); err != nil {
		return res, err
	}

	// p is the handle both phases read and write. The reset re-enumerates the CDC
	// endpoint and can leave the caller's fd stale, so on the first read error we
	// reopen once — the same fallback observeBootOn uses — and the deferred close
	// reaps only that reopened handle, never the caller's pt.
	p := pt
	reopened := false
	defer func() {
		if reopened && p != pt {
			_ = p.Close()
		}
	}()

	// One read loop serves both phases. scan=true collects the network list and
	// stops on PROVSCAN-END; scan=false feeds lines through onAck and stops when
	// it reports done. A read error is returned so the caller can reopen.
	watch := func(d time.Duration, scan bool, onAck func(string)) error {
		stop := time.Now().Add(d)
		buf := make([]byte, 1024)
		var partial []byte
		for time.Now().Before(stop) {
			if err := ctxErr(ctx); err != nil {
				return err
			}
			n, err := p.Read(buf)
			if err != nil {
				if !reopened {
					if p2, oerr := openForConsole(ctx, pt.name); oerr == nil {
						_ = p2.SetDTR(true)
						p = p2
						reopened = true
						continue
					}
				}
				return fmt.Errorf("esprom: console read: %w", err)
			}
			if n == 0 {
				continue
			}
			partial = append(partial, buf[:n]...)
			for {
				i := strings.IndexByte(string(partial), '\n')
				if i < 0 {
					break
				}
				line := strings.TrimRight(string(partial[:i]), "\r\n")
				partial = append(partial[:0], partial[i+1:]...)
				if line == "" {
					continue
				}
				if onLine != nil {
					onLine(line)
				}
				if strings.Contains(line, "SoftAP up") {
					res.APSeen = true
				}
				if sn, ok := ParsePROVSCAN(line); ok {
					res.Networks = append(res.Networks, sn)
				}
				if scan && strings.Contains(line, "PROVSCAN-END") {
					return nil
				}
				if !scan {
					onAck(line)
					return nil // ack phase reads until the caller's stop() says done
				}
			}
		}
		return nil
	}

	if err := watch(scanBudget, true, nil); err != nil {
		return res, err
	}
	prov := ProvLine(ssid, pass, tz)
	if _, err := p.Write([]byte(prov)); err != nil {
		return res, fmt.Errorf("esprom: prov write: %w", err)
	}
	res.Sent = true

	// Re-push the PROV line on a 2s cadence until PROV-ACK lands or the budget
	// expires. A single write is flaky on this console for two real reasons: the
	// reader task may not have armed when the first bytes arrive, and the
	// USB-Serial-JTAG RX path can swallow the first transfer after a boot (IDF
	// Data Buffering: "more data will get it unstuck"). Re-sending the same
	// CRLF line is idempotent — the firmware parser keys on the PROV token — and
	// we stop at the first PROV-ACK, so extra pushes are harmless.
	ackDeadline := time.Now().Add(ackBudget)
	nextPush := time.Now().Add(2 * time.Second)
	for !res.Ack && time.Now().Before(ackDeadline) {
		if err := ctxErr(ctx); err != nil {
			return res, err
		}
		// Poll for up to the shorter of "until the next push" and "until the
		// ack deadline", with a floor so watch always reads rather than spins.
		wait := time.Until(ackDeadline)
		if d := time.Until(nextPush); d < wait {
			wait = d
		}
		if wait < 100*time.Millisecond {
			wait = 100 * time.Millisecond
		}
		if err := watch(wait, false, func(ln string) {
			if strings.Contains(ln, "PROV-ACK") {
				res.Ack = true
			}
		}); err != nil {
			return res, err
		}
		if res.Ack {
			break
		}
		if !time.Now().Before(nextPush) {
			if _, err := p.Write([]byte(prov)); err != nil {
				return res, fmt.Errorf("esprom: prov write: %w", err)
			}
			nextPush = time.Now().Add(2 * time.Second)
		}
	}

	// The join the ack kicked off is not instant: after PROV-ACK the reader posts
	// the join, the radio associates, and DHCP lands a lease a few seconds later,
	// at which point the firmware prints its "associated: config page at
	// http://<IP>/" banner (orb_net.c on_ip). Keep the console open for that
	// banner so the hub learns the orb's LAN address over the same USB lead — no
	// ARP sweep needed. A missing IP within the budget is NOT a failure: the ack
	// already proved the credentials were accepted and the join posted; the
	// caller can still find the orb on the LAN the usual way.
	if res.Ack && ipBudget > 0 {
		ipDeadline := time.Now().Add(ipBudget)
		for res.IP == "" && time.Now().Before(ipDeadline) {
			if err := ctxErr(ctx); err != nil {
				return res, err
			}
			wait := time.Until(ipDeadline)
			if wait < 100*time.Millisecond {
				wait = 100 * time.Millisecond
			}
			if err := watch(wait, false, func(ln string) {
				if ip, ok := parseOrbIP(ln); ok {
					res.IP = ip
				}
			}); err != nil {
				return res, err
			}
		}
	}
	return res, nil
}
