package main

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// This file holds the ladder's shape rather than its steps: the waits a provisioning run owes a
// rebooting dial, and the sentences a seeding run may not close on. installflow.go reads its own
// text because the bench's worst failures of 2026-10-29 were shape failures — code that worked,
// wired to the wrong patience, and reporting a healthy board as a dead one.

// installFn cuts one function out of installflow.go for the text gates.
func installFn(t *testing.T, src, decl string) string {
	t.Helper()
	at := strings.Index(src, decl)
	if at < 0 {
		t.Fatalf("%s is gone from installflow.go", decl)
	}
	fn := src[at:]
	if n := strings.Index(fn, "\n}\n"); n > 0 {
		fn = fn[:n]
	}
	return fn
}

// TestTheCausedRebootsWaitOnTheCycle pins the patience of the waits that follow a reboot the hub
// caused. A dial behind its boot loop answers HTTP for a beat while it is dying, so the first
// reply a probe gets is the previous instance's corpse; waitOrbCycle refuses any answer until it
// has watched the board go quiet (firmware's own claim, 2026-10-29). parkGlass broke this rule the
// day the door shipped and refused a seed its own park had just landed (bench, Bronze,
// 2026-10-29), so the rule is a gate now, not a comment.
func TestTheCausedRebootsWaitOnTheCycle(t *testing.T) {
	raw, err := os.ReadFile("installflow.go")
	if err != nil {
		t.Fatalf("installflow.go: %v", err)
	}
	src := string(raw)
	for _, want := range []struct{ what, needle string }{
		{"the theme step's post-store wait", "s.waitOrbCycle(context.Background(), ip, installRebootWait, beats...)"},
		{"the park's post-reboot wait", "s.waitOrbCycle(ctx, ip, installRebootWait,"},
		{"the silence the cycle demands", "var cycleQuietGrace = 20 * time.Second"},
		{"the budget behind every caused reboot", "installRebootWait = 3 * time.Minute"},
	} {
		if !strings.Contains(src, want.needle) {
			t.Errorf("%s lost its patience: installflow.go no longer says %q", want.what, want.needle)
		}
	}
	if park := installFn(t, src, "func (s *Server) parkGlass"); strings.Contains(park, "s.waitOrbBack(") {
		t.Error("parkGlass waits with the bare probe again: a door that answers while the board reboots is a false refuse waiting to happen")
	}
	if back := installFn(t, src, "func (s *Server) installWaitBack"); !strings.Contains(back, "s.waitOrbCycle(") {
		t.Error("installWaitBack no longer routes through the cycle")
	}
}

// The refusal a seeding arm writes must be a lever, not a diagnosis. The owner's bench report of
// 2026-10-29 read "the dial is not showing it — open the orb to retry" on an orb whose only
// missing step was a knob flick, and a sentence an operator cannot act on reads as the hub failing
// rather than as the orb waiting.
func TestTheSeedingRefusalNamesTheLever(t *testing.T) {
	s, _ := deviceHub(t, goodApps, goodSD)
	note := ""
	err := s.verifySeededGlass(context.Background(), "127.0.0.1",
		func(st installStep) { note = st.Note })
	if err == nil {
		t.Fatal("a clock the dial is not showing verified green")
	}
	for _, want := range []string{"flick the knob", "Seed"} {
		if !strings.Contains(note, want) {
			t.Errorf("the refusal never named %q, so the operator has nothing to do: %s", want, note)
		}
	}
	for _, ban := range []string{"open the orb to retry", "try again"} {
		if strings.Contains(note, ban) {
			t.Errorf("the refusal is back to %q, which is an alarm and not an instruction: %s", ban, note)
		}
	}
	if !strings.Contains(err.Error(), "flick the knob") {
		t.Errorf("the stream offered the lever and the result withheld it: %s", err)
	}
	if !isSeedFailure(err) {
		t.Error("the refusal is not a seed failure, so the run would clear its slate and leave the board half set up")
	}
}

// The tolerated branch — the owner's own shelf, or a dial behind the door's floor — may report the
// glass as it found it and may not promise anything about it. An operator reads this sentence as a
// green run, so it states what the hub read and stops there.
func TestTheToleratedSkipDescribesAndPromisesNothing(t *testing.T) {
	got := seedNote(seedFacts{OnGlass: false})
	if !strings.Contains(got, "only the knob") {
		t.Errorf("the tolerated skip hides the one lever: %s", got)
	}
	for _, ban := range []string{"no card", "retry", "provision again"} {
		if strings.Contains(got, ban) {
			t.Errorf("the tolerated skip claims %q it never read: %s", ban, got)
		}
	}
}

// installTheme stands down the moment the card carries themes: they are the owner's shelf, and
// overwriting an owner's face with the library's first file is not a seed. It must stand down
// without touching the upload door, because that door reboots the board and a skipped step has no
// business spending a boot.
func TestThemeStepStandsDownWithoutDialingTheDoor(t *testing.T) {
	var paths []string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		paths = append(paths, r.URL.Path)
		w.Header().Set("Content-Type", "application/json")
		if r.URL.Path == "/api/v1/sd" {
			w.Write([]byte(`{"mounted":true,"card_count":3,"active":"Aviator","themes":[{"name":"Aviator"}]}`))
			return
		}
		w.Write([]byte(`{}`))
	}))
	t.Cleanup(srv.Close)
	s := newHub()
	s.device = orbnet.NewClient(deviceTimeout)
	s.device.DevicePort = portOf(srv.URL)
	s.ota = orbnet.NewClient(otaTimeout)
	s.ota.DevicePort = portOf(srv.URL)

	step, note := "", ""
	if err := s.installTheme(context.Background(), "127.0.0.1",
		func(st installStep) { step, note = st.Action, st.Note }); err != nil {
		t.Fatalf("the theme step refused a card holding its owner's themes: %v", err)
	}
	if step != "skipped" || !strings.Contains(note, "the card already carries themes") {
		t.Errorf("the theme step answered %s / %q instead of standing down", step, note)
	}
	for _, p := range paths {
		if strings.Contains(p, "/api/v1/apps/theme") {
			t.Errorf("a skipped theme step dialed the upload door (%s) and rebooted a board it had no reason to touch", p)
		}
	}
}
