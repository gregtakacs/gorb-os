package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// noCardHub answers the apps ledger the way the firmware does with no card mounted
// (orb_net.c's mounted:false branch, carrying orb_sd_why's sentence).
func noCardHub(t *testing.T, body string) *Server {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if r.URL.Path == "/api/v1/apps" {
			w.Write([]byte(body))
			return
		}
		w.Write([]byte(`{}`))
	}))
	t.Cleanup(srv.Close)
	s := newHub()
	s.device = orbnet.NewClient(deviceTimeout)
	s.device.DevicePort = portOf(srv.URL)
	return s
}

// A new orb with an empty socket finishes its setup: the seed says so and what to do next,
// ends `done` rather than refused, and drops the wipe mark so a later join cannot erase the
// apps the owner installs by hand once a card is in (owner, 2026-11-07).
func TestSeedWithNoCardEndsSetUpAndSaysWhatIsNext(t *testing.T) {
	for _, tc := range []struct{ why, want string }{
		{"no card answered: push it fully into the socket, or try another card", "has no SD card"},
		{"no card in the socket", "has no SD card"},
		{"the card answered, but the orb cannot read its format: it needs FAT32", "cannot use its SD card (the card answered"},
	} {
		s := noCardHub(t, `{"mounted":false,"why":"`+tc.why+`","count":-1,"apps":[],"installed":[]}`)
		s.slate.mark("44bd8d782618")
		var steps []installStep
		if !s.seedEndsWithoutCard("127.0.0.1", "44bd8d782618", func(st installStep) { steps = append(steps, st) }) {
			t.Fatalf("%q: the seed went on to the ladder over a board with no card", tc.why)
		}
		if len(steps) != 2 || steps[0].Action != "nocard" || steps[1].Action != "done" {
			t.Fatalf("%q: steps %+v, want nocard then done", tc.why, steps)
		}
		if !strings.Contains(steps[0].Note, tc.want) || !strings.Contains(steps[0].Note, "Install on Clock on the Apps tab") {
			t.Errorf("%q: the note does not say what happened and what to press: %s", tc.why, steps[0].Note)
		}
		if s.slate.pending("44bd8d782618") {
			t.Errorf("%q: the wipe mark stood, so the next join would erase the apps installed by hand", tc.why)
		}
	}
}

// A card in the socket, or a ledger that does not answer, is the ladder's to decide.
func TestSeedWithACardRunsTheLadder(t *testing.T) {
	s := noCardHub(t, `{"mounted":true,"count":0,"apps":[],"installed":[]}`)
	s.slate.mark("44bd8d782618")
	if s.seedEndsWithoutCard("127.0.0.1", "44bd8d782618", func(installStep) {}) {
		t.Fatal("a mounted card ended the seed before the install")
	}
	if !s.slate.pending("44bd8d782618") {
		t.Error("a mounted card cleared the wipe mark")
	}
	s = noCardHub(t, `not json`)
	if s.seedEndsWithoutCard("127.0.0.1", "", func(installStep) {}) {
		t.Fatal("an unreadable ledger ended the seed")
	}
}
