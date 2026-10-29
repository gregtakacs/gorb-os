package main

// The first-flash clean slate.
//
// The owner's rule (bench, 2026-10-29): a firmware UPGRADE keeps the registry it
// finds, because the installed rows, the enable words and the owner's card files are
// the board's history and a reflash is not a reason to lose it. A NEW orb is the
// opposite case — a chip this hub is adopting has no history worth keeping, and any
// rows it does carry are leftovers from being somebody's dev board. Adopting one
// therefore wipes what it found and seeds exactly the shipped baseline: the clock app
// and the skeleton face, on the card and in the slot.
//
// Why a mark and not a live question. Nothing the board can answer distinguishes the
// two cases. An `apps` partition header says a row exists and says nothing about who
// put it there, and a reflash writes only 0x0, 0x8000, 0x11000 and 0x20000
// (the bundle's own flasher_args.json), so `apps` at 0xD80000 and `storage` NVS at
// 0xD60000 survive it by design — that retention is what lets credentials live
// through an upgrade. The only witness that separates an adoption from an upgrade is
// the one the hub holds at the moment it writes the image: had this chip already been
// an orb on this hub's books. That question is asked once, in flash.go, and its answer
// is kept here.
//
// The mark is deliberately in-memory. A hub restarted between the flash and the
// provisioning loses it, and the loss is survivable in both directions: nothing gets
// wiped that should have been kept, and the hollow-row check in installOneApp repairs a
// leftover row that did need refilling without a wipe. A destructive step gated on
// memory that outlived a restart by an accident of timing is the worse failure.

import "sync"

// cleanSlateSet holds the chips this hub has just written its first gorb-os into.
// Keyed by canonical MAC, because the flash speaks MAC and the install dials IP.
type cleanSlateSet struct {
	mu   sync.Mutex
	pend map[string]bool
}

func newCleanSlate() *cleanSlateSet { return &cleanSlateSet{pend: map[string]bool{}} }

// mark says this flash was an adoption: the chip carried no prior proof on this hub.
func (c *cleanSlateSet) mark(mac string) {
	if mac == "" {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.pend == nil {
		c.pend = map[string]bool{}
	}
	c.pend[mac] = true
}

// pending asks whether a chip is owed a clean slate, without consuming the mark: a
// run that refuses for a reason unrelated to the wipe must leave the board's turn
// standing for the next press.
func (c *cleanSlateSet) pending(mac string) bool {
	if c == nil || mac == "" {
		return false
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.pend[mac]
}

// clear consumes the mark. It runs only after the wipe and the baseline install both
// landed, so a run that died halfway gets another try rather than a silent half-state.
func (c *cleanSlateSet) clear(mac string) {
	if c == nil || mac == "" {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	delete(c.pend, mac)
}
