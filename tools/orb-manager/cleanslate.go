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

import (
	"strings"
	"sync"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// cleanSlateSet holds the chips this hub has just written its first gorb-os into.
// Keyed by canonical MAC, because the flash speaks MAC and the install dials IP — and
// the two speak it differently: the flash and the lonely write hand in the ROM's
// "44:bd:8d:78:26:18", the install's slateForIP answers the fleet's "44bd8d782618".
// Every method canonicalizes its argument (slateKey), so no caller can miss its own mark.
// Before that, every adoption missed: the mark was filed under the colon form and asked
// for under the bare one, the wipe never ran, a board's card leftovers read as the
// owner's shelf, and a new orb finished on the menu (bench, 2026-11-03).
type cleanSlateSet struct {
	mu   sync.Mutex
	pend map[string]bool
}

func newCleanSlate() *cleanSlateSet { return &cleanSlateSet{pend: map[string]bool{}} }

// slateKey is the one spelling the set files under: the fleet's canonical 12-hex MAC,
// or, for a string that is not a MAC at all, the string itself lowercased (so a garbled
// caller still finds its own mark, and never someone else's).
func slateKey(mac string) string {
	if c, why := orbmdns.CanonicalMAC(mac); why == "" {
		return c
	}
	return strings.ToLower(strings.TrimSpace(mac))
}

// mark says this flash was an adoption: the chip carried no prior proof on this hub.
func (c *cleanSlateSet) mark(mac string) {
	if c == nil || mac == "" {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.pend == nil {
		c.pend = map[string]bool{}
	}
	c.pend[slateKey(mac)] = true
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
	return c.pend[slateKey(mac)]
}

// clear consumes the mark. It runs only after the wipe and the baseline install both
// landed, so a run that died halfway gets another try rather than a silent half-state.
func (c *cleanSlateSet) clear(mac string) {
	if c == nil || mac == "" {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	delete(c.pend, slateKey(mac))
}
