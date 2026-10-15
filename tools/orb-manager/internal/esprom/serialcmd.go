package esprom

import (
	"context"
	"fmt"
	"regexp"
	"strings"
	"time"
)

// Generic serial commands (Path 3): the wired door for a CONFIGURED orb.
//
// ProvisionOn (serialprov.go) drives a first-boot board whose stdin reader is armed
// only while unprovisioned. orb_net.c now runs that reader for the whole life of the
// app, so the same USB lead that takes a PROV line also takes NAME, SCAN, MODE, RESET
// and PING against a board that has already joined and will never show its boot scan.
// This file sends one such line and waits for its verdict token.
//
// Unlike provisioning there is no boot-scan phase and no forced fresh boot: the door
// is live the moment the app runs, so a plain console open suffices. MODE and RESET
// reboot the orb as their last act, so the ack can be the final line before the CDC
// endpoint disappears — the caller must not treat a post-ack read error as a failure,
// and this driver stops at the first ack token for exactly that reason.
//
// Security matches PROV: operands ride the wire hex-encoded, which obfuscates rather
// than encrypts, and is acceptable because the path is a USB lead the operator owns.

// stateRe matches the firmware's one-line state banner (orb_net.c):
//
//	GorbOS-STATE <b|c-ap|c-sta>
var stateRe = regexp.MustCompile(`GorbOS-STATE\s+(\S+)`)

// ParseState returns the firmware's own state token — "b" (unconfigured), "c-ap"
// (configured, AP-isolated) or "c-sta" (configured, station) — from a console line,
// or ("", false) for any other line. A bare ESP32-S3 with no gorb-os prints no such
// line; that absence is how state (a) is detected.
func ParseState(line string) (string, bool) {
	m := stateRe.FindStringSubmatch(line)
	if m == nil {
		return "", false
	}
	return m[1], true
}

// nameRe matches the firmware's one-line name banner (orb_net.c):
//
//	GorbOS-NAME <name>
//
// A named board prints it once, after the state banner; an unnamed board prints
// nothing, so absence is "no news" and never "cleared".
var nameRe = regexp.MustCompile(`GorbOS-NAME\s+(.+?)\s*$`)

// ParseOrbName returns the name a board announced about itself on its boot
// console, or ("", false) for any other line. It is the read half of the rule
// that the board owns its name: the serial door writes NAME, and this is how a
// hub with only a USB lead finds out what a board is already called. The value
// is whatever the board had in NVS, so it arrives as the firmware stored it —
// this function does not sanitize, and callers must fold it through the same
// gate every other name passes.
func ParseOrbName(line string) (string, bool) {
	m := nameRe.FindStringSubmatch(line)
	if m == nil {
		return "", false
	}
	return m[1], true
}

// pingRe matches the answer to a PING: "ORB-PING <state>".
var pingRe = regexp.MustCompile(`ORB-PING\s+(\S+)`)

// ParsePing returns the state token a PING produced, or ("", false).
func ParsePing(line string) (string, bool) {
	m := pingRe.FindStringSubmatch(line)
	if m == nil {
		return "", false
	}
	return m[1], true
}

// CmdResult is the outcome of one generic serial command.
type CmdResult struct {
	Sent     bool      `json:"sent"`
	Ack      bool      `json:"ack"`                // an ACK token was seen
	Nak      string    `json:"nak,omitempty"`      // the NAK reason, if a NAK came back
	IP       string    `json:"ip,omitempty"`       // post-join lease banner, when a PROV follows
	State    string    `json:"state,omitempty"`    // state token from a PING or state banner
	Networks []ScanNet `json:"networks,omitempty"` // captured PROVSCAN rows, when a SCAN
}

// cmdWriteBudget bounds how long the driver re-pushes a command before giving up. The
// first transfer after a boot can be swallowed by the USB-Serial-JTAG RX path, so a
// re-send cadence makes a single write reliable, exactly as the PROV ack loop does.
const cmdWriteBudget = 15 * time.Second

// SerialLine formats a generic command line: verb, then a hex operand when one is given
// (or a literal for RESET's guard, passed already-formed as operand), CRLF-terminated
// for the same cooked-mode line-boundary reason ProvLine is.
func SerialLine(verb, operand string) string {
	if operand == "" {
		return verb + "\r\n"
	}
	return verb + " " + operand + "\r\n"
}

// RunCommand writes one command line to an ALREADY-OPEN port and reads the console
// until a verdict token, the write budget expires, or ctx is cancelled. It re-pushes
// the line on a 2 s cadence because the firmware parser keys on the verb and re-sending
// is idempotent. ackPrefixes are the tokens that end the read successfully (for example
// "NAME-ACK"); a line containing the matching "-NAK" is captured as Nak and also ends
// the read. onLine, if set, sees every console line so the caller can stream it.
//
// A post-ack read error is NOT returned as an error once the ack arrived, because MODE
// and RESET reboot the orb and the CDC endpoint vanishes mid-read. The caller owns pt.
func RunCommand(ctx context.Context, pt *Port, line string, ackPrefixes []string,
	onLine func(string)) (*CmdResult, error) {

	res := &CmdResult{Networks: []ScanNet{}}
	if err := ctxErr(ctx); err != nil {
		return res, err
	}
	_ = pt.SetDTR(true) // IDF drops console bytes unless a host holds the port ready

	p := pt
	reopened := false
	defer func() {
		if reopened && p != pt {
			_ = p.Close()
		}
	}()

	buf := make([]byte, 1024)
	var partial []byte
	deadline := time.Now().Add(cmdWriteBudget)
	nextPush := time.Now() // push immediately, then every 2 s until a verdict

	handle := func(ln string) bool {
		trimmed := strings.TrimRight(ln, "\r\n \t")
		if trimmed == "" {
			return false
		}
		if onLine != nil {
			onLine(trimmed)
		}
		if st, ok := ParsePing(trimmed); ok {
			res.State = st
		}
		if st, ok := ParseState(trimmed); ok {
			res.State = st
		}
		if ip, ok := parseOrbIP(trimmed); ok {
			res.IP = ip
		}
		if sn, ok := ParsePROVSCAN(trimmed); ok {
			res.Networks = append(res.Networks, sn)
		}
		for _, pre := range ackPrefixes {
			if strings.Contains(trimmed, pre) {
				res.Ack = true
				return true
			}
			nak := strings.Replace(pre, "-ACK", "-NAK", 1)
			if nak != pre && strings.Contains(trimmed, nak) {
				if i := strings.Index(trimmed, nak); i >= 0 {
					res.Nak = strings.TrimSpace(trimmed[i+len(nak):])
				}
				return true
			}
		}
		return false
	}

	for !res.Ack && res.Nak == "" && time.Now().Before(deadline) {
		if err := ctxErr(ctx); err != nil {
			return res, err
		}
		if !time.Now().Before(nextPush) {
			if _, err := p.Write([]byte(line)); err != nil {
				return res, fmt.Errorf("esprom: cmd write: %w", err)
			}
			res.Sent = true
			nextPush = time.Now().Add(2 * time.Second)
		}
		n, rerr := p.Read(buf)
		if rerr != nil {
			if res.Ack {
				return res, nil // a MODE/RESET reboot dropped the fd right after the ack
			}
			if !reopened {
				if p2, oerr := openForConsole(ctx, pt.name); oerr == nil {
					_ = p2.SetDTR(true)
					p = p2
					reopened = true
					continue
				}
			}
			return res, fmt.Errorf("esprom: console read: %w", rerr)
		}
		if n == 0 {
			continue
		}
		partial = append(partial, buf[:n]...)
		for {
			i := indexAny(partial, '\n')
			if i < 0 {
				break
			}
			if done := handle(string(partial[:i])); done {
				return res, nil
			}
			partial = append(partial[:0], partial[i+1:]...)
		}
	}
	return res, nil
}
