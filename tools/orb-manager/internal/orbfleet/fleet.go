// Package orbfleet is the hub's memory of which orb is which.
//
// The key is the chip's BASE MAC, never the address (§5.3 of
// docs/orb-manager-plan.md). An address is something an orb has at a moment; a MAC
// is something an orb is. Keying on the address would orphan a named orb the moment
// its DHCP lease moved, and would hand the old name to whatever board came next —
// which is how a fleet list ends up pointing a credential at the wrong device.
//
// Three rules hold that together, and each is tested where it bites:
//
//   - One name, one board. Naming a second orb with a name a live row already holds
//     is refused, and the refusal says who has it.
//   - The instance is derived from the MAC, so two boards can derive the SAME
//     instance while differing in the high bytes of their addresses. Those rows are
//     marked ambiguous rather than left to look identical, because a row the operator
//     cannot tell apart is a row they will click wrong.
//   - The announced address wins. A board moves — DHCP rebind, re-flash, a bench that
//     joins the LAN — and updates its own row: the hub follows the MAC, and an address
//     is the row's least durable fact.
//   - The name in the file is a cache, not an authority. The board owns its name (NVS,
//     read back over /api/v1/status); the hub stores the last word it read so a
//     remembered row can show a name instead of a bare MAC, and every device read
//     corrects or clears the copy.
package orbfleet

import (
	"errors"
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// Limits are stated as constants because the card has to enforce the same ones the
// store does, and a name accepted here and truncated there is a name that reads as
// two different orbs depending on which surface you look at.
const (
	MaxName     = 32
	DefaultLive = 6 * time.Minute
)

// Orb is one board as the hub remembers it.
//
// The hub's memory is an ADDRESS BOOK WITH A NAME TAG. Its job is to let a launch dial
// the boards it already knows about instead of waiting up to one announce interval
// (MDNS_ADV_REFRESH_S, 60 s) for a packet to arrive, and to show each remembered board
// by the word its owner gave it rather than by a bare MAC. Four fields cross the restart
// boundary: MAC, IP, Port, and the last Name the hub read off the board. The board still
// owns its name — the hub names a board by writing to the board (fleet.go's
// writeNameToDevice), and the copy here is the read-back: every status fold corrects it,
// and a board answering with "" clears it. Everything else a row displays is a fact
// about this frame only, and a second copy in a file would be a second place for it to
// be wrong.
//
// MAC, IP, Port and Name are the persisted four. The fields below that are json:"-" and
// are a snapshot of the current frame only: presence, the live device facts, and the
// two derived clash flags.
type Orb struct {
	MAC  string `json:"mac"`
	IP   string `json:"ip,omitempty"`
	Port uint16 `json:"port,omitempty"`

	// Remembered marks a row this hub has proof about: it left a fresh flash, or it
	// carries a word. It is NOT a persistence claim — Snapshot keeps an address and
	// nothing else, since an unreachable row is unmanageable (the operator's rule,
	// bench 2026-10-15). What the mark still answers is the narrower question the port
	// list asks: has this chip been an orb as far as this hub personally knows, which
	// is what storeProvesOrb reads to tell a known orb's cable from a blank to adopt.
	// It lives for the run and rides no file.
	Remembered bool `json:"remembered,omitempty"`

	// Name is the last word the DEVICE gave for itself, folded in by NoteDeviceName.
	// It persists so a remembered row reads "Hall" at first paint instead of a MAC the
	// operator cannot read off a label; it is a cache and not an authority. Writing a
	// name goes to the board first, and the next status read settles the copy.
	Name     string `json:"name,omitempty"`
	Instance string `json:"-"`
	FW       string `json:"-"`
	State    string `json:"-"`

	// LastSeen is when this process last heard an announce for the row. It is a
	// presence statement, never a reason to hide a row: a restored address book entry
	// has no LastSeen at all, and dropping it from the card for that is the bug this
	// struct's shape exists to kill.
	LastSeen time.Time `json:"-"`

	// Note is the last thing the store refused to fold into this row, with the
	// reason. A row that quietly stopped matching its announcements looks exactly
	// like a healthy one otherwise.
	Note string `json:"-"`

	// Shared names an instance that more than one live MAC claimed. Derived on read, so
	// it cannot go stale when the other board goes silent.
	Shared bool `json:"-"`

	// Contended marks an address two live boards both announced. The hub will not dial
	// one: it cannot know which of the two the operator meant.
	Contended bool `json:"-"`
}

// Live reports whether the row still describes a board the hub can reach. It is a
// presence statement and nothing else: no route consults it, and the proxy's answer
// comes from ipGrants.
func (o Orb) Live(ttl time.Duration, now time.Time) bool {
	if ttl <= 0 {
		return true
	}
	return now.Sub(o.LastSeen) <= ttl
}

// DisplayName is what the card shows: the operator's word if they gave one, the
// derived instance if they did not. The instance is never shown as though it were a
// name the owner chose, because it is a hex tail.
func (o Orb) DisplayName() string {
	if strings.TrimSpace(o.Name) != "" {
		return o.Name
	}
	if o.Instance != "" {
		return o.Instance
	}
	return o.MAC
}

// Observation is one candidate as the hub saw it on the wire. The caller has already
// run the browser's identity checks; the store repeats the MAC canonicalization and
// the instance derivation, because it must not inherit its safety from whoever happened
// to call it first.
type Observation struct {
	MAC, Instance, IP, FW, State string
	Port                         uint16
	At                           time.Time
}

// Refusal is one rejected fold, kept so the card can say why a board it can see is
// not in the list.
type Refusal struct {
	Why  string
	MAC  string
	From string
	At   time.Time
}

// Store is the fleet. It is safe for concurrent use: the browse feeds it from one
// goroutine while the handlers read it from many.
type Store struct {
	mu      sync.Mutex
	byMAC   map[string]*Orb
	live    time.Duration
	now     func() time.Time
	refused []Refusal
}

// Config wires the two things a test must be able to hold still: the clock, and how
// long silence evicts a row.
type Config struct {
	Now  func() time.Time
	Live time.Duration
}

func New(c Config) *Store {
	if c.Now == nil {
		c.Now = time.Now
	}
	live := c.Live
	if live <= 0 {
		live = DefaultLive
	}
	return &Store{byMAC: map[string]*Orb{}, live: live, now: c.Now}
}

// Observe folds one announcement into the fleet. It returns the row the announcement
// updated and, when the announcement contradicted what the hub already knew, an error
// that explains the contradiction. The row is returned either way: a board whose
// announcement was refused is still a board, and hiding it is how an operator ends up
// with no explanation for an empty card.
func (s *Store) Observe(o Observation) (*Orb, error) {
	mac, why := orbmdns.CanonicalMAC(o.MAC)
	if why != "" {
		s.noteRefusal(why, o.MAC, o.IP)
		return nil, errors.New(why)
	}
	if err := orbmdns.ExpectInstance(o.Instance, mac); err != nil {
		s.noteRefusal(err.Error(), mac, o.IP)
		return nil, err
	}
	now := o.At
	if now.IsZero() {
		now = s.now()
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if s.byMAC == nil {
		s.byMAC = map[string]*Orb{}
	}
	row := s.byMAC[mac]
	if row == nil {
		row = &Orb{MAC: mac}
		s.byMAC[mac] = row
	}

	// The address a board announces now wins, always. A board flashed on the bench and
	// brought onto the network is the ordinary case, and it updates its own entry: the
	// hub follows the MAC. What used to stand here was a guard refusing to move the
	// address of a row the hub had read off a chip, which blocked exactly that case —
	// the chip read says which board a MAC belongs to, not which address it keeps.

	// An accepted observation clears the previous note. A note that only ever got
	// written would keep reporting a contradiction the LAN stopped producing an hour ago.
	//
	// The same board at a new address is a DHCP rebind, the ordinary case §5.3 names:
	// follow the MAC, keep the name, and say what moved.
	row.Note = ""
	if row.IP != "" && o.IP != "" && o.IP != row.IP {
		row.Note = fmt.Sprintf("moved from %s:%d to %s:%d", row.IP, row.Port, o.IP, o.Port)
	}

	// Two live boards claiming one address is a contradiction, and the hub does not
	// resolve it. Clearing the older row would let whoever announced last evict a named
	// orb's address from the fleet, and a DHCP handover is indistinguishable from an
	// impersonation while it is happening. So both claims stand and the contention is
	// derived on read: the proxy refuses to dial a contended address, and the card says
	// two boards claim it rather than the hub silently picking one and being wrong half
	// the time.

	row.Instance = o.Instance
	if o.FW != "" {
		row.FW = o.FW
	}
	if o.State != "" {
		row.State = o.State
	}
	if o.IP != "" {
		row.IP = o.IP
		row.Port = o.Port
	}
	row.LastSeen = now
	copied := *row
	return &copied, nil
}

// Confirm says the hub has proof this board is present right now, and refreshes the
// row's presence clock. Two things call it: the USB sweep attributed a port to this
// MAC (seeCable), and a chip read named the MAC over a lead (markCable). A dial that
// answers deliberately does NOT call it — a dial earns a verdict, not an announce, and
// a presence clock the hub could wind by knocking would make the Forget gate refuse on
// the hub's own fabrication.
//
// Creating the row here is what makes a cable-only board a row at all. It carries a MAC
// and no address, so the address book gains nothing from it: Snapshot writes only rows it
// can dial, and this row dies with the lead.
func (s *Store) Confirm(mac string) (*Orb, error) {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return nil, errors.New(why)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	row, ok := s.byMAC[c]
	if !ok {
		row = &Orb{MAC: c}
		s.byMAC[c] = row
	}
	row.LastSeen = s.now()
	copied := *row
	return &copied, nil
}

// CheckNameFree answers the cross-lock question without writing the answer. It exists
// because a caller that has to put the name on the DEVICE before it can record it
// needs to know the hub would not have accepted it AFTERWARD — dialing first and
// refusing second brands the board with a word the fleet will not carry, which is a
// worse state than the one the refusal was trying to prevent.
//
// It is the cross-lock on its own now: the hub stores no name, so nothing re-checks
// this on write. It remains a check and not a lock — a second rename landing between
// this return and the POST to the board is caught the ordinary way, on the next read.
func (s *Store) CheckNameFree(mac, name string) error {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return errors.New(why)
	}
	clean, err := cleanWord(name, MaxName, "name")
	if err != nil || clean == "" {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.byMAC[c]; !ok {
		return fmt.Errorf("no orb %s to name: it has never announced and is not on a cable", c)
	}
	return s.nameClashLocked(c, clean)
}

// nameClashLocked is the one-name-one-board rule. Callers hold s.mu.
func (s *Store) nameClashLocked(c, clean string) error {
	for other, r := range s.byMAC {
		if other == c || !r.Live(s.live, s.now()) || strings.TrimSpace(r.Name) == "" {
			continue
		}
		if normWord(r.Name) == normWord(clean) {
			return fmt.Errorf("%q is already the name of %s (%s); rename that orb first", clean, r.DisplayName(), other)
		}
	}
	return nil
}

// NoteAnswer folds what the board itself said about its running firmware and
// its own state token into an existing row, without touching the presence
// clock. That last part is the whole design: a dial that answers is not an
// announcement, and a Forget gate fed by a presence clock the hub winds by
// probing its own address book is a gate that never opens (the same reason
// orbmdnsToObservation carries the heard-time and not the fold-time). It
// updates only the fields the caller actually read — an empty fw or state is
// "the board did not say", never "the board has none" — so a PING from older
// firmware, which answers with a state token and no stamp, cannot wipe a
// version the announce already carried. The row must exist: answers fold onto
// boards the hub has met; a board nobody has met yet is the announce's or the
// flash's or Adopt's job, each of which creates the row with its own evidence.
func (s *Store) NoteAnswer(mac, fw, state string) (Orb, bool, error) {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return Orb{}, false, errors.New(why)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	row, ok := s.byMAC[c]
	if !ok {
		return Orb{}, false, fmt.Errorf("no orb %s to fold an answer into", c)
	}
	changed := false
	if fw != "" && row.FW != fw {
		row.FW, changed = fw, true
	}
	if state != "" && row.State != state {
		row.State, changed = state, true
	}
	return *row, changed, nil
}

// NoteDeviceName folds what the DEVICE reported into the row, and it is the reason
// the hub's Name field is a cache rather than an owner: the device's word always
// wins, and the two disagree in exactly one direction.
//
// It does not enforce the one-name-one-board cross-lock, which CheckNameFree applies to
// an operator's keystroke instead. That rule is the right one for a word a human typed
// and the wrong one for a packet the board sent about itself — refusing it would leave
// the card displaying a name no orb answers to, which is the failure the whole
// arrangement exists to prevent. A live collision is therefore ADMITTED and written to
// the refusal list: the operator sees two boards answering to one word and can rename
// one, and the card got that story from a device rather than from a stale file.
//
// An empty name clears the cache. A FAILED status read must not come through here at
// all — clearing on a read that never landed would lose the name over one dropped
// packet, and the caller is what distinguishes those two cases.
func (s *Store) NoteDeviceName(mac, name string) (Orb, bool, error) {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return Orb{}, false, errors.New(why)
	}
	s.mu.Lock()
	row, ok := s.byMAC[c]
	if !ok {
		s.mu.Unlock()
		return Orb{}, false, fmt.Errorf("no orb %s to refresh a name for", c)
	}
	out := *row
	changed := out.Name != name
	if changed {
		out.Name = name
		row.Name = name
		// A board carrying a word is one this hub has a reason to find again, even
		// with nothing to dial: a named board that never joined Wi-Fi is bench setup,
		// not a cable phantom. An empty word does NOT clear the mark — a board that
		// answers "no name yet" is still the board the operator flashed and named
		// until it says otherwise. Only lonely and Forget end a row.
		if name != "" {
			row.Remembered = true
		}
	}
	// Look for a collision only on a change that introduced one; re-reporting the
	// same clash on every read would bury the refusal list in repeats of a fact the
	// operator already acted on.
	collision := ""
	if changed && name != "" {
		for other, r := range s.byMAC {
			if other == c || !r.Live(s.live, s.now()) || r.Name == "" {
				continue
			}
			if normWord(r.Name) == normWord(name) {
				collision = fmt.Sprintf("%s holds %q too", other, name)
				break
			}
		}
	}
	s.mu.Unlock()
	if collision != "" {
		s.noteRefusal("device-reported name collides: "+collision, c, "")
	}
	return out, changed, nil
}

// MarkRemembered records that this hub has proof this chip has been an orb: it just
// wrote firmware into it. The mark buys no line in the state file — Snapshot keeps an
// address and nothing else — and it is deliberately not set by a cable sighting, because
// a hot port is a fact about this launch's bench while a flash is something the hub did
// to a board. What the mark still answers is the port list's question, through
// storeProvesOrb: has this chip been an orb as far as this hub personally knows. An
// unknown MAC gets a row, because the flash that earned the mark is itself proof the
// board exists.
func (s *Store) MarkRemembered(mac string) error {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return errors.New(why)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	row, ok := s.byMAC[c]
	if !ok {
		row = &Orb{MAC: c}
		s.byMAC[c] = row
	}
	row.Remembered = true
	return nil
}

// Forget drops a board from the fleet. It returns the addresses the row held so the
// caller can revoke the matching grants: forgetting an orb while the hub still dials
// its address is half an operation, and the operator meant the whole one.
func (s *Store) Forget(mac string) ([]string, error) {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return nil, errors.New(why)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	row, ok := s.byMAC[c]
	if !ok {
		return nil, nil
	}
	var held []string
	if row.IP != "" {
		held = append(held, row.IP)
	}
	delete(s.byMAC, c)
	return held, nil
}

// ClaimedBy answers the question the proxy must ask before it dials: which board
// currently advertises this address? Two answers is a contradiction and the caller
// must refuse, because it cannot know which device the browser meant.
func (s *Store) ClaimedBy(ip string) []Orb {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []Orb
	now := s.now()
	for _, r := range s.byMAC {
		if r.IP != "" && r.IP == ip && r.Live(s.live, now) {
			copied := *r
			out = append(out, copied)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].MAC < out[j].MAC })
	// The flag is the answer, not a detail: a caller that dialled the first entry of a
	// two-row reply would be dialling a guess.
	for i := range out {
		out[i].Contended = len(out) > 1
	}
	return out
}

// All returns every live row, with the instance-sharing flag derived. Sorted by
// display name so the card's order is stable across polls and a row does not jump
// under the cursor between two refreshes.
func (s *Store) All() []Orb {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	live := make([]*Orb, 0, len(s.byMAC))
	count := map[string]int{}
	ips := map[string]int{}
	for _, r := range s.byMAC {
		if !r.Live(s.live, now) {
			continue
		}
		live = append(live, r)
		if r.Instance != "" {
			count[r.Instance]++
		}
		if r.IP != "" {
			ips[r.IP]++
		}
	}
	out := make([]Orb, 0, len(live))
	for _, r := range live {
		copied := *r
		copied.Shared = r.Instance != "" && count[r.Instance] > 1
		copied.Contended = r.IP != "" && ips[r.IP] > 1
		out = append(out, copied)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].DisplayName() != out[j].DisplayName() {
			return out[i].DisplayName() < out[j].DisplayName()
		}
		return out[i].MAC < out[j].MAC
	})
	return out
}

// Look returns one row. The store applies no liveness filter to reads: an entry is in
// the address book or it is not, and whether the board is reachable is a question the
// caller answers from the live browser, the cable sweep, or a dial. Look and ByMAC are
// the same read today; Look stays named because the Forget gate asks a different question
// of a row than the card does and should not read like the same call.
func (s *Store) Look(mac string) (Orb, bool) {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return Orb{}, false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.byMAC[c]
	if !ok {
		return Orb{}, false
	}
	return *r, true
}

// Every returns every remembered row, live or not, sorted by MAC. It is the address
// book read whole, and it wears two hats that come from the same fact: it is the source
// of the card's remembered tier, and the dial schedule for the hub's liveness sweep. The
// addresses worth asking about are the ones a row still points at, including the rows a
// Live filter would drop — a board that stopped announcing is exactly the case where "does
// it still answer?" matters, and it is the case the file exists to keep visible at launch.
func (s *Store) Every() []Orb {
	s.mu.Lock()
	out := make([]Orb, 0, len(s.byMAC))
	for _, r := range s.byMAC {
		out = append(out, *r)
	}
	s.mu.Unlock()
	sort.Slice(out, func(i, j int) bool { return out[i].MAC < out[j].MAC })
	return out
}

// ByMAC returns one live row. A caller that wants the row regardless of presence —
// the Forget gate, the roster's remembered tier — reads Look or Every instead.
func (s *Store) ByMAC(mac string) (Orb, bool) {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return Orb{}, false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.byMAC[c]
	if !ok || !r.Live(s.live, s.now()) {
		return Orb{}, false
	}
	copied := *r
	return copied, true
}

// Refusals returns the folds the store would not make, oldest first, capped: the card
// shows the recent ones and the log has the rest.
func (s *Store) Refusals() []Refusal {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]Refusal(nil), s.refused...)
}

// noteRefusal keeps the last hundred, which is a lot of refusals and still bounded.
func (s *Store) noteRefusal(why, mac, ip string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.refused = append(s.refused, Refusal{Why: why, MAC: mac, From: ip, At: s.now()})
	if len(s.refused) > 100 {
		s.refused = s.refused[len(s.refused)-100:]
	}
}

// Snapshot is the persisted form: the address book and the name tags, and nothing a
// restart could only get wrong. A row is kept ONLY when it carries an address. That is
// the whole rule, and it comes from the operator's own words on the bench (2026-10-15):
// the sole reason a board the hub is not currently connected to belongs on the list is
// that the hub can reach out and manage it, which means dialling its address. A row
// with no address can never be managed; the moment it sits on a lead again it is
// self-discovering anyway, so persisting it bought a phantom row and nothing else.
// This replaces the Remembered exception, which every orb flash earned at write time
// and which kept exactly those unreachable rows alive across restarts — the board the
// operator had unplugged, and the lonely board whose mark had not been saved yet, both
// came back listed.
func (s *Store) Snapshot() []Orb {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]Orb, 0, len(s.byMAC))
	for _, r := range s.byMAC {
		if r.IP == "" {
			continue
		}
		copied := *r
		out = append(out, copied)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].MAC < out[j].MAC })
	return out
}

// Restore refills the store after a launch. Rows are revalidated rather than trusted:
// a hand-edited or future-version file that named a non-hex MAC or an address that is
// not an IPv4 literal is dropped rather than loaded. A row with no address is dropped,
// the same test Snapshot applies — and dropping it HERE is what cleans up a file an
// older hub wrote: entries that carry the retired Remembered mark and no address (the
// unplugged board, the lonely board whose mark lost the save race) are refused at load
// and disappear from the file at the next save, with no migration step and no operator
// action.
func (s *Store) Restore(rows []Orb) int {
	n := 0
	for _, r := range rows {
		c, why := orbmdns.CanonicalMAC(r.MAC)
		if why != "" {
			continue
		}
		if r.IP != "" && !isV4(r.IP) {
			r.IP, r.Port = "", 0
		}
		if r.IP == "" {
			continue
		}
		if len(r.Name) > MaxName {
			r.Name = ""
		}
		// Presence is never restored. A row that came out of the file has not been
		// heard from, and claiming otherwise would make a board that has been in a
		// drawer for a month look like one that announced a second ago: the Forget gate
		// would refuse on its own fake clock, and the card would show it as alive.
		copied := r
		copied.MAC = c
		s.mu.Lock()
		s.byMAC[c] = &copied
		s.mu.Unlock()
		n++
	}
	return n
}

// CleanName applies the store's own operator-input rules and returns the form it
// would persist. A caller that must act on the cleaned value BEFORE storing it — the
// card dialing the device to write the name through — needs this, because reading it
// back out of SetName means storing it first, and a device refusal after that leaves a
// hub file claiming a name no orb carries.
func CleanName(s string) (string, error) { return cleanWord(s, MaxName, "name") }

// CheckNameASCII reports whether a cleaned name can live on the DEVICE, which is a
// narrower set than what the hub itself will store. The firmware echoes the value
// into JSON with %s and no escaper (orb_net.c name_reject_reason), so it refuses
// every byte above 0x7e — and an accented name is the one thing this rules out that
// cleanWord above happily accepted for as long as the hub owned the string.
//
// Callers run this BEFORE dialing. Refusing at the hub turns "Café" into one
// sentence the operator can act on; skipping it gets a bare 400 from an IP with the
// reason parked in a serial log nobody has attached.
//
// The rule is narrower than "is this ASCII": it is the firmware's rule restated, and
// device_contract_test.go pins the two against each other so a loosening on one side
// fails a test rather than surfacing as a name that sticks on the card and never
// lands on the orb.
func CheckNameASCII(name string) error {
	if name == "" {
		return nil // a clear, which the device accepts
	}
	if len(name) > MaxName {
		return fmt.Errorf("name %q is %d bytes, over the %d the orb stores", name, len(name), MaxName)
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c < 0x20 || c > 0x7e {
			return fmt.Errorf("name %q needs plain ASCII: the orb stores what it displays and has no room for %s",
				name, hexByte(c))
		}
		if c == '"' || c == '\\' || c == '<' || c == '>' {
			return fmt.Errorf("name %q carries a character the orb refuses (%q)", name, c)
		}
	}
	if strings.HasPrefix(name, " ") || strings.HasSuffix(name, " ") || strings.Contains(name, "  ") {
		return fmt.Errorf("name %q has spaces the orb will not store", name)
	}
	return nil
}

// hexByte names an invisible byte in a refusal without printing it. A message that
// echoed "Café" back with the raw byte in it tells the operator nothing they could
// type; 0xC3 is at least searchable.
func hexByte(c byte) string {
	const dig = "0123456789ABCDEF"
	return "0x" + string([]byte{dig[c>>4], dig[c&0xf]})
}

// cleanWord trims and bounds an operator-typed word, refusing control bytes and
// angle brackets. The angle-bracket refusal is not the card's escaping doing its job
// twice: the value is also written to a file and read back, and a name that has to be
// escaped everywhere is a name that will eventually be unescaped somewhere.
func cleanWord(s string, max int, what string) (string, error) {
	t := strings.Join(strings.Fields(s), " ")
	if t == "" {
		return "", nil
	}
	if len(t) > max {
		return "", fmt.Errorf("%s %q is %d bytes, over the %d the hub stores", what, t, len(t), max)
	}
	for _, r := range t {
		if r < 0x20 || r == 0x7f || r == '<' || r == '>' {
			return "", fmt.Errorf("%s %q carries a character the hub will not store", what, t)
		}
	}
	return t, nil
}

// normWord is the comparison form for the one-name-one-board rule.
func normWord(s string) string {
	return strings.ToLower(strings.Join(strings.Fields(s), " "))
}

func isV4(s string) bool {
	if s == "" || strings.Contains(s, ":") || strings.Count(s, ".") != 3 {
		return false
	}
	for _, part := range strings.Split(s, ".") {
		if part == "" || len(part) > 3 {
			return false
		}
		for i := 0; i < len(part); i++ {
			if part[i] < '0' || part[i] > '9' {
				return false
			}
		}
	}
	return true
}

var _ = fmt.Sprintf
