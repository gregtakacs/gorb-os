package main

import (
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// Two orbs flashed in a row is a normal afternoon. Each onboarding probe must get
// ITS OWN boot verdict back, not whichever one landed last in a single slot.
func TestTwoOrbsKeepBothBootVerdicts(t *testing.T) {
	s := newHub()
	s.setBoot("44:BD:8D:00:00:01", "orb-0001", false, "", []esprom.ScanNet{{Index: 1, SSID: "home"}})
	s.setBoot("44:bd:8d:00:00:02", "", true, "192.168.2.41", nil)

	a := s.bootFor("44:bd:8d:00:00:01")
	if a == nil {
		t.Fatal("the first orb's verdict was lost when the second one flashed")
	}
	if a.apSSID != "orb-0001" || len(a.networks) != 1 {
		t.Errorf("first verdict = %+v, want orb-0001 with its scan", a)
	}
	b := s.bootFor("44:bd:8d-00-00-02") // the UI sends dash-separated MACs too
	if b == nil {
		t.Fatal("the second orb has no verdict either; MAC normalization failed")
	}
	if b.ip != "192.168.2.41" || !b.associated {
		t.Errorf("second verdict = %+v", b)
	}
	if s.bootFor("44:bd:8d:00:00:03") != nil {
		t.Error("a MAC that never flashed was given somebody else's verdict")
	}
}

// A verdict is a claim about a boot that just happened. Past the window it is
// history, and serving it would send the UI down the wrong branch of onboarding.
func TestBootVerdictExpires(t *testing.T) {
	s := newHub()
	s.setBoot("44:bd:8d:00:00:09", "orb-0009", false, "", nil)
	s.bmu.Lock()
	s.boots["44:bd:8d:00:00:09"].at = time.Now().Add(-bootFreshWindow - time.Second)
	s.bmu.Unlock()
	if v := s.bootFor("44:bd:8d:00:00:09"); v != nil {
		t.Errorf("a verdict older than the window was served: %+v", v)
	}
	s.bmu.Lock()
	n := len(s.boots)
	s.bmu.Unlock()
	if n != 0 {
		t.Errorf("the expired verdict was left in the map: %d entries", n)
	}
}

// The map is written by every flash on a hub that may run for a week. Pruning runs
// on write as well as read, or the only unbounded thing in the tool is a map of
// verdicts nobody will ever ask for again.
func TestBootVerdictsArePrunedOnWrite(t *testing.T) {
	s := newHub()
	s.setBoot("44:bd:8d:00:00:0a", "orb-000a", false, "", nil)
	s.bmu.Lock()
	s.boots["44:bd:8d:00:00:0a"].at = time.Now().Add(-bootFreshWindow - time.Second)
	s.bmu.Unlock()

	s.setBoot("44:bd:8d:00:00:0b", "orb-000b", false, "", nil)
	s.bmu.Lock()
	_, stale := s.boots["44:bd:8d:00:00:0a"]
	n := len(s.boots)
	s.bmu.Unlock()
	if stale {
		t.Error("an expired verdict survived the next flash")
	}
	if n != 1 {
		t.Errorf("map holds %d verdicts, want only the fresh one", n)
	}
}

// A verdict recorded without a MAC could never be looked up, and would sit in the
// map taking the place of nothing.
func TestBootWithoutMACIsNotRecorded(t *testing.T) {
	s := newHub()
	s.setBoot("  ", "orb-0000", false, "", nil)
	s.bmu.Lock()
	n := len(s.boots)
	s.bmu.Unlock()
	if n != 0 {
		t.Errorf("recorded %d verdict(s) for an empty MAC", n)
	}
}
