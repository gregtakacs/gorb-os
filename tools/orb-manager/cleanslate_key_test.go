package main

import "testing"

// The flash and the lonely write file a mark under the ROM's colon MAC; the install asks
// under the fleet's bare hex. Both are one chip, and the set must say so — the miss was
// every adoption skipping its wipe and finishing on the menu (bench, 2026-11-03).
func TestCleanSlateFindsItsMarkWhicheverWayTheMACIsSpelled(t *testing.T) {
	c := newCleanSlate()
	c.mark(macNorm("44:BD:8D:78:26:18")) // flash.go and lonely.go
	for _, ask := range []string{"44bd8d782618", "44:bd:8d:78:26:18", "44-BD-8D-78-26-18"} {
		if !c.pending(ask) {
			t.Errorf("the mark filed by the flash is not pending under %q", ask)
		}
	}
	if c.pending("44bd8d608088") {
		t.Error("another chip reads as owed a wipe")
	}
	c.clear("44bd8d782618") // installflow.go, under slateForIP's answer
	if c.pending("44:bd:8d:78:26:18") {
		t.Error("clearing under the fleet's spelling left the flash's spelling standing")
	}
}
