package main

import (
	"strings"
	"testing"
)

// The serial SCAN verb and the firmware's scan state machine have a contract no Go type
// expresses: scan_collect() frees the driver's AP list and prints nothing unless
// s_scan_state is SCAN_SCANNING, and scan_begin() does NOT set that state — it expects
// its caller to have armed it. So a SCAN that posted NET_CMD_SCAN started a real radio
// scan, threw the results away at the done event, and never printed the PROVSCAN-END the
// hub waits for. The bench evidence was eight SCAN-ARMED lines and no ack (2026-10-15).
//
// This pin reads the C source rather than the binary because the fact under test is which
// command the verb posts — exactly the thing a tidy refactor "helpfully" simplifies back
// into the broken form.
func TestSerialScanVerbPostsTheArmedScan(t *testing.T) {
	src := firmwareFile(t, "main/net/orb_net.c")

	verb := cBlock(t, src, `!memcmp(line, "SCAN", 4)`)
	if !strings.Contains(verb, "NET_CMD_SCAN_BOOT") {
		t.Errorf("the serial SCAN verb posts the bare NET_CMD_SCAN again, which arms nothing and leaves "+
			"scan_collect() to free the results in silence; it must post NET_CMD_SCAN_BOOT:\n%s", verb)
	}
	if strings.Contains(verb, "net_post(NET_CMD_SCAN,") {
		t.Errorf("the serial SCAN verb must not post NET_CMD_SCAN, which nobody armed:\n%s", verb)
	}
	// The one-shot flag is what makes an ordinary scan mirror to the console at all, and
	// scan_collect clears it after one print. Lose either half and Rescan works once, or
	// never.
	if !strings.Contains(verb, "s_serial_scan_wanted = true") {
		t.Errorf("the serial SCAN verb stopped arming the console mirror:\n%s", verb)
	}

	// Anchored on the opening brace, because the same signature appears earlier as a
	// forward declaration and the body is the thing under test.
	collect := cBlock(t, src, "static void scan_collect(void)\n{")
	if !strings.Contains(collect, "s_scan_state != SCAN_SCANNING") {
		t.Errorf("scan_collect() no longer refuses an unarmed scan, and that guard is the whole reason "+
			"the armed post is mandatory:\n%s", collect)
	}
	if !strings.Contains(collect, "s_serial_scan_wanted") {
		t.Error("scan_collect() no longer mirrors PROVSCAN rows for a serial-armed scan")
	}
}

// cBlock returns the brace-matched block that follows the first occurrence of mark. It is
// cFunction in firmware_contract_test.go with the mark as a parameter, because the
// functions this file cares about are not esp_err_t handlers.
func cBlock(t *testing.T, src, mark string) string {
	t.Helper()
	i := strings.Index(src, mark)
	if i < 0 {
		t.Fatalf("the firmware source has no %s — it was renamed, and this pin is now meaningless", mark)
	}
	open := strings.Index(src[i:], "{")
	if open < 0 {
		t.Fatalf("%s has no body", mark)
	}
	depth, j := 0, i+open
	for ; j < len(src); j++ {
		switch src[j] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return src[i : j+1]
			}
		}
	}
	t.Fatalf("unbalanced braces after %s", mark)
	return ""
}
