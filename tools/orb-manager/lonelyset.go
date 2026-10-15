package main

import (
	"sort"
	"sync"
	"time"
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

// defaultOrbName is the card's word for a board whose own name is gone: a
// factory reset erased the NVS the device kept the word in. The firmware seeds
// this same default into the fresh NVS on a clean erase (main/net/orb_net.c,
// net_factory_reset), so the hub's ack write-back only bridges the gap until the
// board answers a poll again — device and card converge on the one word.
// Whoever claims the board next gives it a word of their own, and the store's
// copy is corrected the same way any device-named fact is — by the device.
const defaultOrbName = "The Orb"

func (l *lonelyMark) mark(mac string) {
	if mac == "" {
		return
	}
	l.mu.Lock()
	l.at[mac] = l.now().UTC()
	l.mu.Unlock()
}

func (l *lonelyMark) clear(mac string) {
	if mac == "" {
		return
	}
	l.mu.Lock()
	delete(l.at, mac)
	l.mu.Unlock()
}

func (l *lonelyMark) has(mac string) bool {
	if mac == "" {
		return false
	}
	l.mu.Lock()
	_, ok := l.at[mac]
	l.mu.Unlock()
	return ok
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
// any sweep can read it.
func (l *lonelyMark) restore(macs []string) {
	l.mu.Lock()
	for _, m := range macs {
		if m != "" {
			l.at[m] = l.now().UTC()
		}
	}
	l.mu.Unlock()
}
