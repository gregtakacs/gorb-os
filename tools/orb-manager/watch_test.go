package main

import (
	"encoding/json"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

func TestProvisioningSSID(t *testing.T) {
	cases := []struct{ line, want string }{
		{`I (2601) orb_net: no stored network — provisioning AP "orb-8088" (open), 900 s`, "orb-8088"},
		// The firmware's own SoftAP-up banner carries no SSID (orb_net.c
		// "SoftAP up — join it and open the URL in the log above"), so it must
		// not produce a verdict on its own — only the "provisioning AP" line,
		// which does name it, ends the console phase early.
		{`W (9001) orb_net: SoftAP up — join it and open the URL in the log above`, ""},
		// Must NOT fire on ordinary lines, least of all the disassociation
		// banner, whose "associated:" substring also threatens stationJoinedLine.
		{`I (1101) orb_net: associated: config page at http://192.168.2.32/`, ""},
		{`W (1234) orb_net: disassociated: beacon-timeout (reason 200, attempt 3)`, ""},
		{`I (300) boot: provisioning of images complete`, ""},
	}
	for _, c := range cases {
		if got := provisioningSSID(c.line); got != c.want {
			t.Errorf("provisioningSSID(%q) = %q, want %q", c.line, got, c.want)
		}
	}
}

func TestStationJoinedLine(t *testing.T) {
	yes := []string{
		`I (2503) orb_net: associated: config page at http://192.168.2.32/`,
	}
	no := []string{
		`W (2600) orb_net: disassociated: beacon-timeout (reason 200, attempt 1)`,
		`I (100) cpu_start: Pro CPU started`,
	}
	for _, l := range yes {
		if !stationJoinedLine(l) {
			t.Errorf("stationJoinedLine(%q) = false, want true", l)
		}
	}
	for _, l := range no {
		if stationJoinedLine(l) {
			t.Errorf("stationJoinedLine(%q) = true, want false", l)
		}
	}
}

func TestMacNorm(t *testing.T) {
	for _, in := range []string{"44:BD:8D:60:80:88", "44-bd-8d-60-80-88", " 44:bd:8d:60:80:88 "} {
		if got := macNorm(in); got != "44:bd:8d:60:80:88" {
			t.Errorf("macNorm(%q) = %q", in, got)
		}
	}
}

// ---- Phase B's second voice (owner ruling, 2026-10-29) ----------------------
//
// The board that joined after the console window closed is still the board the
// cable held, and it says so itself — the mDNS TXT mac is the eFuse address the
// flash read over USB minutes earlier. These tests pin the ruling: dial what
// the orb announced about this chip, refuse everything else, sweep nothing.

// watchResults drives handleWatch over a request body and returns every event
// it streamed. httptest.ResponseRecorder implements Flusher, so the handler
// runs its whole path in one call.
func watchResults(t *testing.T, h *Server, body string) []map[string]any {
	t.Helper()
	rec := httptest.NewRecorder()
	h.handleWatch(rec, httptest.NewRequest("POST", "/api/watch", strings.NewReader(body)))
	var out []map[string]any
	for _, line := range strings.Split(rec.Body.String(), "\n") {
		if strings.TrimSpace(line) == "" {
			continue
		}
		var ev map[string]any
		if err := json.Unmarshal([]byte(line), &ev); err != nil {
			t.Fatalf("watch emitted a non-event line: %.120s", line)
		}
		out = append(out, ev)
	}
	return out
}

func lastResult(evs []map[string]any) map[string]any {
	for i := len(evs) - 1; i >= 0; i-- {
		if r, ok := evs[i]["result"].(map[string]any); ok {
			return r
		}
	}
	return nil
}

func shrinkWatchBudget(t *testing.T) {
	t.Helper()
	b, p := watchLanBudget, watchPollInterval
	watchLanBudget, watchPollInterval = 1200*time.Millisecond, 80*time.Millisecond
	t.Cleanup(func() { watchLanBudget, watchPollInterval = b, p })
}

// The failure the owner reported: the join landed a breath after the console
// window closed, the banner was never read, and the flow dead-ended at "enter
// its IP" while the board's own announcement sat in the hub's cache. The watch
// must adopt the announced address as its dial target. The adoption's grant
// and status legs ride the same code adopt_test already proves from the
// console side, so this pins the decision the ruling is about: the watch
// dials the announced chip's address and reports THAT failure when the dial
// dies — never the old "enter its IP" dead end. (The fake device answers only
// on loopback, and the browser refuses loopback announcements by design, so
// the address here is an unroutable one and the dial is meant to die.)
func TestWatchDialsWhatTheChipAnnouncedWhenTheConsoleMissedTheLease(t *testing.T) {
	shrinkWatchBudget(t)
	s := newHub()
	s.browse = filled(t, orbmdns.Candidate{MAC: "44bd8d782618", IP: "198.18.0.43",
		From: "198.18.0.43", State: "normal", FW: "094fa22"})

	evs := watchResults(t, s, `{"mac":"44:bd:8d:78:26:18","skip_ap":true,"ip":""}`)
	r := lastResult(evs)
	if r == nil || r["mode"] == "no_ip" {
		t.Fatalf("the watch ignored the board's own announcement: %v", r)
	}
	if r["mode"] != "unknown" || r["ip"] != "198.18.0.43" {
		t.Fatalf("the watch did not adopt the announced address: %v", r)
	}
	var heard bool
	for _, ev := range evs {
		if l, _ := ev["log"].(string); strings.Contains(l, "announced itself at 198.18.0.43") {
			heard = true
		}
	}
	if !heard {
		t.Error("the watch never said the announcement is where the address came from")
	}
}

// The same chip on a moved number: the console DID read a lease, DHCP handed a
// new one before the dial, and the old rule (poll the one IP for the whole
// budget) reads a joined board as a dead one. The announcement is the only
// thing allowed to move the dial — no sweep is consulted, and the move speaks.
func TestWatchFollowsTheChipWhenTheLeaseMoves(t *testing.T) {
	shrinkWatchBudget(t)
	s := newHub()
	s.browse = filled(t, orbmdns.Candidate{MAC: "44bd8d782618", IP: "198.18.0.43",
		From: "198.18.0.43", State: "normal", FW: "094fa22"})

	evs := watchResults(t, s, `{"mac":"44:bd:8d:78:26:18","skip_ap":true,"ip":"198.18.0.42"}`)
	r := lastResult(evs)
	if r == nil || r["mode"] == "no_ip" {
		t.Fatalf("the watch died on the old number instead of following the chip: %v", r)
	}
	if at, _ := r["ip"].(string); at != "198.18.0.43" {
		t.Fatalf("the dial did not move to the announced address: %v", r)
	}
	var moved bool
	for _, ev := range evs {
		if l, _ := ev["log"].(string); strings.Contains(l, "lease moved") {
			moved = true
		}
	}
	if !moved {
		t.Error("the dial moved silently — an operator must see the lease change hands")
	}
}

// A PROV-ACK means received, not joined: a wrong passphrase looks identical
// until the board reopens its setup AP. The announcement says which board this
// is, and a provisioning state after an ack is the join failing — reported as
// a failed join, not as a hub that lost the address, and not by dialling
// 192.168.4.1 across the room.
func TestWatchCallsAProvisioningAnnouncementAJoinThatDidNotLand(t *testing.T) {
	shrinkWatchBudget(t)
	s := newHub()
	s.browse = filled(t, orbmdns.Candidate{MAC: "44bd8d782618", IP: "192.168.4.1",
		From: "192.168.4.1", State: "provisioning", FW: "094fa22"})

	evs := watchResults(t, s, `{"mac":"44:bd:8d:78:26:18","skip_ap":true,"ip":""}`)
	r := lastResult(evs)
	if r == nil || r["mode"] != "no_ip" {
		t.Fatalf("a board announcing setup mode was adopted: %v", r)
	}
	if why, _ := r["reason"].(string); !strings.Contains(why, "passphrase") {
		t.Errorf("the join failure was reported without naming its suspect: %q", why)
	}
	if s.grants.allowed("192.168.4.1") {
		t.Error("the watch dialled a provisioning AP it cannot reach")
	}
}

// The announcement is the orb's word, and the eFuse is the cable's. When the
// two disagree the watch must refuse, not bind the stranger's MAC into the
// cross-lock — which is the exact failure the old cable-only rule feared,
// and the reason the MAC condition is this path's whole gate.
func TestWatchRefusesAnAddressAnsweringAsAnotherChip(t *testing.T) {
	shrinkWatchBudget(t)
	s := newHub()
	o := orbfake.New()
	o.SetAddress("a4cf120000aa", "127.0.0.1")
	fakeOrbServer(t, s, o)

	evs := watchResults(t, s, `{"mac":"44:bd:8d:78:26:18","skip_ap":true,"ip":"127.0.0.1"}`)
	r := lastResult(evs)
	if r == nil || r["mode"] != "unknown" {
		t.Fatalf("a stranger at the banner address was adopted: %v", r)
	}
	if s.grants.allowed("127.0.0.1") {
		t.Error("the contradiction was granted anyway")
	}
}

// With no MAC in the request there is nothing to cross-check an announcement
// against, and an unbound row is the stranger the old rule correctly refused.
// The cable exit stands, named honestly: the board announced NOTHING.
func TestWatchStillRefusesToGuessForAnUnknownChip(t *testing.T) {
	shrinkWatchBudget(t)
	s := newHub()
	s.browse = filled(t, orbmdns.Candidate{MAC: "a4cf120000aa", IP: "127.0.0.1",
		From: "127.0.0.1", State: "normal", FW: "094fa22"})

	evs := watchResults(t, s, `{"skip_ap":true,"ip":""}`)
	r := lastResult(evs)
	if r == nil || r["mode"] != "no_ip" {
		t.Fatalf("an announcement naming a different chip fed the watch: %v", r)
	}
}

// The budget itself must still speak: the announce wait is 45 live seconds of
// a dialog behind a spinner, and a silent budget is the hang report of
// 2026-10-15 again.
func TestTheAnnounceWaitSpeaksWhileItWaits(t *testing.T) {
	shrinkWatchBudget(t)
	s := newHub()
	s.browse = filled(t) // a live browser with nothing in it yet

	logs := 0
	evs := watchResults(t, s, `{"mac":"44:bd:8d:78:26:18","skip_ap":true,"ip":""}`)
	for _, ev := range evs {
		if l, _ := ev["log"].(string); strings.Contains(l, "announce") {
			logs++
		}
	}
	if logs == 0 {
		t.Error("the announce wait ran its budget without one progress line")
	}
	r := lastResult(evs)
	if r == nil || r["mode"] != "no_ip" {
		t.Fatalf("a silent budget ended as anything but no_ip: %v", r)
	}
}
