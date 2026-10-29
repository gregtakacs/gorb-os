package main

import (
	"sort"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// The lonely mark: the hub's memory that it personally ended a board's orb
// life.
//
// A lonely board enumerates exactly as the orb it was: the same native-USB
// peripheral, the same eFuse MAC in the serial descriptor. The cable sweep
// therefore re-attributes the port on the next tick, and without this mark the
// forgotten fleet row would rebuild itself from the sighting — the board the
// operator just ended, still listed as an orb. The mark makes the hub's own
// act a fact it remembers: the chip is lonely until something OTHER than a
// cable descriptor says otherwise.
//
// The mark is cleared by evidence of orb life, never by time: an orb flash
// over the lead, an _orb._tcp announcement, or a serial door that answered —
// each proves the chip now runs something that claims to be an orb. A mark
// with no evidence after it costs the operator nothing: the board still shows
// on the port picker as a blank Espressif chip, which is exactly what New Orb
// needs to take it back.
type lonelyMark struct {
	mu  sync.Mutex
	at  map[string]time.Time
	now func() time.Time
}

func newLonelyMark() *lonelyMark {
	return &lonelyMark{at: map[string]time.Time{}, now: time.Now}
}

// lonelyKey is the one form every entry point funnels through, and the reason it
// exists is a MAC spelled two ways in this codebase: the cable sweep and the fleet
// store key on the canonical bare twelve digits (orbmdns.CanonicalMAC), while the
// lonely flash formats the eFuse read it just took as colon-separated pairs for the
// console, which is also what the USB descriptor itself prints.
//
// A set keyed on the caller's spelling got the worst of both: the mark written at
// the flash was invisible to every fleet-side query, so the row the operator had
// just ended was rebuilt nameless by the next sweep while the port list still
// labelled the chip lonely. Two surfaces, two answers, one board. Keying on
// canonical digits inside the set — not at each call site, where the next new
// caller gets it wrong again — makes the mark answer whoever asks, in whichever
// form they arrived.
//
// Anything that is not a chip address keys to "" and is dropped: a phantom entry
// under a malformed MAC would suppress every board that failed to read one.
func lonelyKey(mac string) string {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return ""
	}
	return c
}

// defaultOrbName is the card's word for a board whose own name is gone: a
// factory reset erased the NVS the device kept the word in. The firmware seeds
// this same default into the fresh NVS on a clean erase (main/net/orb_net.c,
// net_factory_reset), so the hub's ack write-back only bridges the gap until the
// board answers a poll again — device and card converge on the one word.
// Whoever claims the board next gives it a word of their own, and the store's
// copy is corrected the same way any device-named fact is — by the device.
const defaultOrbName = "The Orb"

func (l *lonelyMark) mark(mac string) {
	mac = lonelyKey(mac)
	if mac == "" {
		return
	}
	l.mu.Lock()
	l.at[mac] = l.now().UTC()
	l.mu.Unlock()
}

func (l *lonelyMark) clear(mac string) {
	mac = lonelyKey(mac)
	if mac == "" {
		return
	}
	l.mu.Lock()
	delete(l.at, mac)
	l.mu.Unlock()
}

func (l *lonelyMark) has(mac string) bool {
	mac = lonelyKey(mac)
	if mac == "" {
		return false
	}
	l.mu.Lock()
	_, ok := l.at[mac]
	l.mu.Unlock()
	return ok
}

// outranked answers the only question that decides whether an announcement may end a
// lonely mark: was this packet HEARD AFTER the hub blanked the board? A mark is the
// hub's memory of an act it performed at a known instant, and evidence older than that
// act cannot contradict it.
//
// The check exists because the browse cache outlives the board. A record is held for
// TTL plus grace — 120 s by the device's own MDNS_TTL (main/net/orb_mdns.c) — so for
// two minutes after an erase the cache still returns an announcement the board sent
// while it WAS an orb. Folding that ghost re-created the fleet row the erase had just
// deleted and cleared the mark that was supposed to keep it deleted, so an operator who
// blanked a board on the cable watched it come back named orb-<mac> and could never
// forget it again (bench, 2026-10-28). liveness.go states this rule for the presence
// clock — "a presence clock the hub winds from its own cache is a gate that never
// opens" — and orbmdnsToObservation carries the packet time for exactly that reason;
// the lonely mark was the one consumer that read the cache as if every record were new.
//
// heard is the packet time, never the fold time. A zero heard is no evidence at all and
// cannot end a mark.
func (l *lonelyMark) outranked(mac string, heard time.Time) bool {
	mac = lonelyKey(mac)
	if mac == "" || heard.IsZero() {
		return false
	}
	l.mu.Lock()
	at, marked := l.at[mac]
	l.mu.Unlock()
	return marked && !heard.After(at)
}

// list returns the marked MACs sorted, for the state file.
func (l *lonelyMark) list() []string {
	l.mu.Lock()
	out := make([]string, 0, len(l.at))
	for m := range l.at {
		out = append(out, m)
	}
	l.mu.Unlock()
	sort.Strings(out)
	return out
}

// restore fills the set from the state file. It runs once at launch, before
// any sweep can read it. The file has carried this MAC in both spellings over
// the life of the feature, so every entry is re-keyed here rather than trusted:
// a v3 file written by the colon-keyed hub still suppresses the board it marked.
func (l *lonelyMark) restore(macs []string) {
	l.mu.Lock()
	for _, m := range macs {
		if k := lonelyKey(m); k != "" {
			l.at[k] = l.now().UTC()
		}
	}
	l.mu.Unlock()
}
