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
//   - A cable outranks a packet. Once the hub has read a MAC off the chip itself, an
//     announcement cannot move, rename, or re-prove that row: the console said it
//     down a lead, and a UDP reply cannot outvote that.
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
	MaxRoom     = 32
	DefaultLive = 6 * time.Minute
)

// Orb is one board as the hub remembers it. IP and Port are a snapshot of the last
// announcement, not identity: they are what the operator clicks, and the proxy still
// decides separately whether the hub may dial them.
type Orb struct {
	MAC       string    `json:"mac"`
	Name      string    `json:"name,omitempty"`
	Room      string    `json:"room,omitempty"`
	Instance  string    `json:"instance,omitempty"`
	FW        string    `json:"fw,omitempty"`
	IP        string    `json:"ip,omitempty"`
	Port      uint16    `json:"port,omitempty"`
	State     string    `json:"state,omitempty"`
	Cable     bool      `json:"cable,omitempty"`
	FirstSeen time.Time `json:"firstSeen"`
	LastSeen  time.Time `json:"lastSeen"`
	Announces int       `json:"announces"`

	// Note is the last thing the store refused to fold into this row, with the
	// reason. A row that quietly stopped matching its announcements looks exactly
	// like a healthy one otherwise.
	Note string `json:"note,omitempty"`

	// Shared names an instance that more than one live MAC claimed. Deriving it on
	// read means it cannot go stale when the other board goes silent.
	Shared bool `json:"shared,omitempty"`

	// Contended marks an address two live boards both announced. The hub will not dial
	// one: it cannot know which of the two the operator meant. Derived on read, for the
	// same reason Shared is.
	Contended bool `json:"contended,omitempty"`
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
		row = &Orb{MAC: mac, FirstSeen: now}
		s.byMAC[mac] = row
	}

	// A cable-verified row is not moved by a packet. The address may still genuinely
	// have changed — in which case the next console read updates it, and until then the
	// card shows the address the hub proved, with the note saying what it refused.
	if row.Cable && row.IP != "" && o.IP != "" && o.IP != row.IP {
		row.Note = fmt.Sprintf("ignored %s from %s: this board was read over USB at %s", o.IP, o.Instance, row.IP)
		return row, errors.New(row.Note)
	}

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
	row.Announces++
	copied := *row
	return &copied, nil
}

// MarkCable says the hub read this MAC off the chip itself. It is the strongest thing
// the hub can know about a board, and it is what makes Observe stop trusting packets
// about it.
func (s *Store) MarkCable(mac string) (*Orb, error) {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return nil, errors.New(why)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	row, ok := s.byMAC[c]
	if !ok {
		row = &Orb{MAC: c, FirstSeen: s.now()}
		s.byMAC[c] = row
	}
	row.Cable = true
	row.LastSeen = s.now()
	copied := *row
	return &copied, nil
}

// SetName gives a board the operator's word. Naming is only possible for a board the
// hub has actually met — over the cable or on the air — because a name attached to a
// MAC nobody has ever confirmed is a phantom that will outlive the typo that made it.
func (s *Store) SetName(mac, name string) error {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return errors.New(why)
	}
	clean, err := cleanWord(name, MaxName, "name")
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	row, ok := s.byMAC[c]
	if !ok {
		return fmt.Errorf("no orb %s to name: it has never announced and is not on a cable", c)
	}
	if clean == "" {
		row.Name = ""
		return nil
	}
	// The cross-lock: two live rows may not answer to one word. Compare the normalized
	// forms, so "Kitchen " and "kitchen" cannot both exist, and report who holds it so
	// the operator can fix the real problem instead of retrying the typo.
	if err := s.nameClashLocked(c, clean); err != nil {
		return err
	}
	row.Name = clean
	return nil
}

// CheckNameFree answers the cross-lock question without writing the answer. It exists
// because a caller that has to put the name on the DEVICE before it can record it
// needs to know the hub would not have accepted it AFTERWARD — dialing first and
// refusing second brands the board with a word the fleet will not carry, which is a
// worse state than the one the refusal was trying to prevent.
//
// SetName calls the same helper, so the pre-check cannot drift from the rule it
// predicts. It is still a check and not a lock: a second rename landing between this
// return and the write is caught by SetName the ordinary way.
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

// NoteDeviceName folds what the DEVICE reported into the row, and it is the reason
// the hub's Name field is a cache rather than an owner: the device's word always
// wins, and the two disagree in exactly one direction.
//
// It deliberately does not call SetName. SetName enforces the one-name-one-board
// cross-lock, which is the right rule for an operator typing a word and the wrong
// rule for a packet the board sent about itself — refusing it would leave the card
// displaying a name no orb answers to, which is the failure the whole arrangement
// exists to prevent. A live collision is therefore ADMITTED and written to the
// refusal list: the operator sees two boards answering to one word and can rename
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

// SetRoom places a board in a room. Rooms are not unique on purpose: a hallway holds
// four clocks, and refusing the second would teach the operator to type nonsense.
func (s *Store) SetRoom(mac, room string) error {
	c, why := orbmdns.CanonicalMAC(mac)
	if why != "" {
		return errors.New(why)
	}
	clean, err := cleanWord(room, MaxRoom, "room")
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	row, ok := s.byMAC[c]
	if !ok {
		return fmt.Errorf("no orb %s to place in a room", c)
	}
	row.Room = clean
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

// ByMAC returns one live row.
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

// Snapshot is the persisted form: everything the hub knows about a board that a
// restart cannot relearn by itself. It is a copy, so a caller may write it to disk
// while the browse keeps updating the live rows.
func (s *Store) Snapshot() []Orb {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]Orb, 0, len(s.byMAC))
	for _, r := range s.byMAC {
		copied := *r
		out = append(out, copied)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].MAC < out[j].MAC })
	return out
}

// Restore refills the store after a launch. Rows are revalidated rather than trusted:
// a hand-edited or future-version file that named a non-hex MAC or an address that is
// not an IPv4 literal is dropped, not dialed.
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
		if r.LastSeen.IsZero() {
			r.LastSeen = s.now()
		}
		if r.FirstSeen.IsZero() {
			r.FirstSeen = r.LastSeen
		}
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
