package main

// The serial verb table is the wire contract between this hub and every orb's console,
// and its failures are silent in the worst way: an unknown verb is a 400 the UI shows as
// a shrug, and a wrong ack token is a command that ran on the board and timed out here.
// These rows pin both halves, including the two-token reset ack the firmware earned when
// RESET ALL became a whole-device wipe across two NVS partitions.

import (
	"strings"
	"testing"
)

func TestSerialCmdVerbTable(t *testing.T) {
	for _, tc := range []struct {
		name string
		req  orbCmdRequest
		want string // verb + operand, or "" for a rejected request
		ack  string
	}{
		{"name", orbCmdRequest{Command: "name", Operand: "Kitchen"}, "NAME 4b69746368656e", "NAME-ACK"},
		{"scan", orbCmdRequest{Command: "scan"}, "SCAN", "PROVSCAN-END"},
		{"mode ap", orbCmdRequest{Command: "mode", Operand: "ap"}, "MODE 6170", "MODE-ACK"},
		{"mode upper", orbCmdRequest{Command: "MODE", Operand: " STA "}, "MODE 737461", "MODE-ACK"},
		{"ping", orbCmdRequest{Command: "ping"}, "PING", "ORB-PING"},
		// The guard is the literal ALL, not hex: the firmware compares it with strcmp
		// before anything is decoded (orb_net.c dispatch_serial_cmd).
		{"reset", orbCmdRequest{Command: "reset", Operand: "ALL"}, "RESET ALL", "RESET-ACK,RESET-PART"},
	} {
		line, ack, err := serialCmd(tc.req)
		if err != nil {
			t.Errorf("%s: serialCmd returned %v", tc.name, err)
			continue
		}
		if !strings.HasPrefix(line, tc.want) {
			t.Errorf("%s: line %q does not start with %q", tc.name, line, tc.want)
		}
		if ack != tc.ack {
			t.Errorf("%s: ack %q, want %q", tc.name, ack, tc.ack)
		}
	}
}

// The destructive verb's guard is the whole safety story, so both refusals are pinned:
// a bare RESET and a lowercase "all" must never build a line at all.
func TestResetNeedsTheLiteralGuard(t *testing.T) {
	for _, operand := range []string{"", "all", "All", "EVERYTHING"} {
		if _, _, err := serialCmd(orbCmdRequest{Command: "reset", Operand: operand}); err == nil {
			t.Errorf("reset with operand %q built a line; the ALL guard is the only thing between "+
				"a stray write and a factory reset", operand)
		}
	}
	for _, verb := range []string{"teleport", ""} {
		if _, _, err := serialCmd(orbCmdRequest{Command: verb, Operand: "ALL"}); err == nil {
			t.Errorf("unknown command %q built a line", verb)
		}
	}
}

// A comma-packed ack list must split into the tokens RunCommand matches, not one string
// no console line can contain. A join bug here is a reset that always times out.
func TestResetAckListSplits(t *testing.T) {
	_, ack, err := serialCmd(orbCmdRequest{Command: "reset", Operand: "ALL"})
	if err != nil {
		t.Fatal(err)
	}
	got := strings.Split(ack, ",")
	if len(got) != 2 || got[0] != "RESET-ACK" || got[1] != "RESET-PART" {
		t.Errorf("ack %q splits to %q, want both reset verdicts", ack, got)
	}
	for _, tok := range got {
		if !strings.HasSuffix(tok, "-ACK") && !strings.HasSuffix(tok, "-PART") {
			t.Errorf("ack token %q is not a verdict token", tok)
		}
	}
}
