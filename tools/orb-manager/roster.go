package main

import (
	"context"
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// One list, both doors.
//
// The hub learns about a board two ways that do not overlap. mDNS tells it about an
// orb that joined a network and is broadcasting where it lives; the USB enumeration
// tells it about an orb plugged into THIS machine, joined or not, announcing or not.
// Before this file the two had one endpoint each — /api/orbs and /api/ports — and the
// operator was the merge: a board on a cable and on the LAN showed up twice, once
// without its address and once without its port, and a board only on the cable did not
// appear in the fleet list at all.
//
// The roster is the single answer. The key is the canonical base MAC, the same key the
// fleet store uses and for the same reason (§5.3): an address is something an orb has
// at a moment, a MAC is something an orb is. A row says how the board is reachable
// TODAY (via, onCable, ports) as well as what the hub remembers (ip, name, cable).
// Collapsing those is how a fleet UI starts lying: an orb unplugged five minutes ago
// still has an address in the store, and an orb flashed five minutes ago is on a cable
// whether or not it has joined.
//
// Three rules keep the merge honest.
//
//   - A cable never invents an address. The IP half of a row comes from an
//     announcement or from a console read that named its MAC. A board on a lead with
//     no address is listed as such and is not clickable.
//   - A cable outranks a packet, through the fleet store rather than here. This file
//     only reports where a board is; Store.Observe is what refuses to move a confirmed
//     row when an announcement disagrees.
//   - A port the host cannot attribute is a row keyed on its tty path and nothing
//     else. Two unidentified cables never merge, and neither inherits a name.
//
// Persistence is narrower than the view, by owner's order: a board earns a row the
// moment the hub can see it, but only a board with an ADDRESS survives a restart —
// state.json is an address book, and a cable-only row resurrecting without its lead
// would be a lie about a bench. An orb unplugged from USB disappears from the list on
// the next sweep; an orb that loses its lead and has an address gets that address
// re-dialed at the instant of the unplug (liveness.go's kickLostCables). Forget is
// offered the minute a board is provably offline (liveness.go); until then the row is
// there because the hub can still reach it.
//
// Nothing here opens a port, resets a chip, or dials anything. ClassifyPort reads
// sysfs, the registry, or a cached ioreg dump, and the whole sweep sits behind one TTL
// cache because the roster is rebuilt on every stream wake.

// cableSweepTTL bounds how long the roster answers from a cached port sweep instead of
// paying for a fresh one. Ten seconds is under both the mDNS check-in interval and the
// upkeep tick, so a plug-in reaches the card within a tick, and long enough that a
// page reloading twice a second costs one enumeration.
var cableSweepTTL = 10 * time.Second

// cableSighting is what one USB port says about the board behind it with no connection
// made and no reset caused. mac is canonical when the host read the serial descriptor,
// which on the ESP32-S3 USB-Serial-JTAG peripheral is the base MAC. isOrb is the
// VID:PID gate alone, so an orb on a UART bridge is not one and says so.
type cableSighting struct {
	port  string
	isOrb bool
	mac   string
}

// cableCache is the roster's view of the plug set. list is nil until main enables it,
// exactly like the browse: a test hub that never wires a cable view must not pay a
// sysfs read on every card build, and a test that wants one injects the seam rather
// than lying about the host it runs on.
type cableCache struct {
	mu    sync.Mutex
	list  func() ([]cableSighting, error)
	ttl   time.Duration
	seen  []cableSighting
	plug  string
	err   string
	at    time.Time
	swept bool
	// wake is s.wakeOrbs. The stream re-reads the card when the plug set moves even
	// though no mDNS packet caused it. The version digest is what keeps this from
	// costing a frame: a spurious wake sends a keepalive, not a repaint.
	wake func()
}

func newCableCache() *cableCache { return &cableCache{ttl: cableSweepTTL} }

// enableCables turns the cable half of the roster on, with the live host sweep as its
// source and the orb stream as its wake-up. main calls it; newHub leaves it off for the
// same reason it leaves the browse off.
func (s *Server) enableCables(list func() ([]cableSighting, error), wake func()) {
	s.cables.mu.Lock()
	defer s.cables.mu.Unlock()
	s.cables.list = list
	s.cables.wake = wake
}

// refresh re-sweeps when the cache has aged out and reports what it has, along with the
// last enumeration failure. It never returns an error and nothing: a host whose
// enumeration just failed still had a plug set ten seconds ago, and the honest answer
// is the old set with the failure riding along, so the card can say "port list failed"
// rather than implying the cable was pulled.
func (c *cableCache) refresh(now time.Time) ([]cableSighting, string) {
	c.mu.Lock()
	if c.list == nil {
		c.mu.Unlock()
		return nil, ""
	}
	if !c.at.IsZero() && now.Sub(c.at) < c.ttl {
		seen, err := c.seen, c.err
		c.mu.Unlock()
		return seen, err
	}
	seen, err := c.list()
	first := !c.swept
	shouldWake := false
	if err != nil {
		// Keep answering from the last set that enumerated: a host whose sysfs read
		// failed once does not have an empty bench, it has a bench nobody re-read.
		// The failure rides back out to the card rather than being swallowed here.
		c.err = err.Error()
		seen = c.seen
	} else {
		c.seen, c.err, c.at, c.swept = seen, "", now, true
		if d := plugDigest(seen); d != c.plug {
			c.plug = d
			// The first sweep only records: a hub coming up with a cable already in it
			// has nothing to announce to a stream that has not rendered its first frame,
			// and that frame already carries the row. A digest move after that is news.
			shouldWake = !first && c.wake != nil
		}
	}
	perr := c.err
	c.mu.Unlock()
	// Outside the lock: wake fans out to stream subscribers, and a subscriber's rebuild
	// comes back into refresh. Held lock plus rebuild is a dead hub, not a slow one.
	if shouldWake {
		c.wake()
	}
	return seen, perr
}

// cablePortFor answers "is this MAC on a cable right now, and on which one".
// handleFleetSet asks it so a rename refusal can name the plug instead of claiming the
// board has never been seen — which, for a board on a lead, would be a lie.
func (c *cableCache) cablePortFor(mac string, now time.Time) string {
	seen, _ := c.refresh(now)
	for _, s := range seen {
		if s.mac != "" && s.mac == mac {
			return s.port
		}
	}
	return ""
}

// plugDigest is what "the plug set moved" means: port names, their orb-ness, and the
// MAC the host read. Firmware version is NOT in here — a probe pays a reboot for that,
// and a sweep that implied it changed would wake a repaint of something the hub did not
// re-learn.
func plugDigest(seen []cableSighting) string {
	var b strings.Builder
	for _, s := range seen {
		fmt.Fprintf(&b, "%s|%v|%s\n", s.port, s.isOrb, s.mac)
	}
	return b.String()
}

// hostCableSweep is the production source: enumerate, classify each port, read the
// serial the host already has. It returns the first enumeration error rather than
// dropping a port quietly — a half-sweep is indistinguishable from a bench with half
// the orbs on it, which is the exact silence this feature exists to end.
func hostCableSweep() ([]cableSighting, error) {
	ports, err := esprom.ListPorts()
	if err != nil {
		return nil, err
	}
	out := make([]cableSighting, 0, len(ports))
	for _, p := range ports {
		u := esprom.ClassifyPort(p)
		s := cableSighting{port: p, isOrb: u.IsOrbDevice()}
		if s.isOrb && u.Serial != "" {
			// The descriptor spells it "44:BD:8D:78:26:18" and the fleet key is
			// canonical, so the two views cannot disagree about which board a cable
			// belongs to. A descriptor that is not a MAC is an orb-shaped stranger:
			// no row, and the Onboard picker still shows the port.
			if mac, why := orbmdns.CanonicalMAC(u.Serial); why == "" {
				s.mac = mac
			}
		}
		out = append(out, s)
	}
	return out, nil
}

// roster folds the cable view into the rows the browse produced and returns the one
// list the card shows. The argument rather than a browse read keeps the mDNS half in
// discover.go, where the listener state and the refusal tail live; this owns the merge
// and the ordering.
//
// It adds three kinds of row to what the browse produced:
//
//   - a cable match on an announced board: the row gains onCable and its ports;
//   - a board on a cable that no orb announced: a row with its MAC, its derived
//     instance, where it is plugged, and no address;
//   - a port that is an orb the host cannot attribute: a row keyed on the tty path
//     with an empty MAC, offering the probe that would identify it.
//
// A row the store holds that neither the LAN nor the cable currently produces is NOT
// added. It would be a row that can never become clickable without a packet the hub
// cannot require, and /api/fleet is already the answer to "everything I remember".
//
// Every board the hub CAN see is remembered, by MAC, with no operator press: the sweep
// marks each attributable cable it finds, so a plug-in is a fleet row the moment the
// card renders it, and the only deliberate removal left is the card's Forget. MAC is
// the identity; a board that moves between addresses updates its row in place.
func (s *Server) roster(lan []orbRow, now time.Time) ([]orbRow, string) {
	if lan == nil {
		// app.js destructures orbs and reads .length without a guard, so a deaf hub
		// still answers with a list, empty rather than absent.
		lan = []orbRow{}
	}
	seen, cableErr := s.cables.refresh(now)
	// An unplug is worth a dial the same second it happens. The board either moved to
	// the LAN (its row stands, on the answer) or left the network entirely (its minute
	// toward Forget starts now, not when the sweep budget gets around to it). Only a
	// clean sweep may say "gone": a failed enumeration keeps the last good sight, and
	// the hub must not dial a fleet it merely failed to see.
	if cableErr == "" {
		s.kickLostCables(seen)
	}
	// Remember the cable before building any row from it: a board that has only ever
	// been touched over USB still earns its place in the fleet, and its row is what
	// later folds an address onto the same MAC rather than starting a second one.
	if s.fleet != nil {
		fresh := false
		for _, c := range seen {
			if !c.isOrb || c.mac == "" {
				continue
			}
			// A lonely mark outranks the descriptor: the host reads the same
			// MAC this hub just watched stop being an orb, and re-remembering
			// it as one would put the row straight back where the operator
			// just deleted it. The port still shows on the picker; the fleet
			// does not carry a board running no orb.
			if s.lonely.has(c.mac) {
				continue
			}
			if _, ok := s.fleet.ByMAC(c.mac); !ok {
				fresh = true
			}
			// Every sweep re-sees the board, so its presence clock stays current even
			// when it moved sockets. Only a first sighting rewrites the file, and the
			// rewrite only persists an entry that has an address to remember.
			s.seeCable(c.mac, c.port)
		}
		if fresh {
			s.saveState()
		}
	}
	at := make(map[string]int, len(lan))
	for i, r := range lan {
		at[r.MAC] = i
	}
	for _, c := range seen {
		if !c.isOrb {
			// Not an orb, or an orb on a UART bridge the host cannot name. Either way
			// it is the Onboard picker's problem, not this list's.
			continue
		}
		if c.mac == "" {
			lan = append(lan, s.unknownCableRow(c.port))
			continue
		}
		// Same rule as the remember loop above: a lonely mark says the descriptor
		// is describing a board running no orb, and the fleet must not render it.
		if s.lonely.has(c.mac) {
			continue
		}
		if i, ok := at[c.mac]; ok {
			r := &lan[i]
			r.OnCable, r.Ports = true, appendPort(r.Ports, c.port)
			r.Via = []string{"lan", "usb"}
			if r.Identity != "confirmed" {
				r.Identity = "descriptor"
			}
			continue
		}
		at[c.mac] = len(lan)
		lan = append(lan, s.cableRow(c.mac, c.port, now))
	}
	// The address book's own tier: every saved board the wire and the bench are not
	// producing this second. This is the half that makes the file worth having — a hub
	// that shows nothing until a packet arrives spends the first minute of every launch
	// looking empty at a fleet it already knows the addresses of. Addresses are also the
	// whole qualification: a row the hub cannot dial is a row it cannot manage, and an
	// orb the operator unplugged is gone until it plugs in somewhere and self-discovers
	// (the rule, stated by the operator, bench 2026-10-15). The lonely mark suppresses
	// this tier exactly as it suppresses the two cable tiers above — the row a lonely
	// board still describes must not survive the ending on the strength of the file.
	if s.fleet != nil {
		for _, row := range s.fleet.Every() {
			if _, produced := at[row.MAC]; produced {
				continue
			}
			if row.IP == "" || s.lonely.has(row.MAC) {
				continue
			}
			at[row.MAC] = len(lan)
			lan = append(lan, s.rememberedRow(row, now))
		}
	}
	for i := range lan {
		if lan[i].Identity == "" {
			lan[i].Identity = s.identityOf(lan[i].MAC)
		}
	}
	sortRows(lan)
	return lan, cableErr
}

// rememberedRow is an address-book entry on screen: a MAC, the address it was last
// reachable at, and no claim about the present. Reachability is "unknown" until the
// startup dial answers, because the honest answer at first paint is that the hub has not
// asked yet. The row is deliberately not clickable — loading it dials an address the hub
// has not confirmed, and the refusal field says that in the card's own words.
// The fleet tier that calls this admits only rows carrying an address, so every row here
// has something to dial; a board with none is not on the card at all.
func (s *Server) rememberedRow(row orbfleet.Orb, now time.Time) orbRow {
	return orbRow{
		Key: row.MAC, MAC: row.MAC, Instance: instanceForMAC(row.MAC),
		IP: row.IP, Port: row.Port, Name: row.Name, Reachable: "unknown",
		Refusal: "remembered from a previous run and not yet heard from: the hub is asking " +
			"it now, and the row becomes clickable when it answers",
	}
}

// cableRow is a board on a lead that has not announced: real MAC, no address, nothing
// to dial, and the fleet's words for it if the operator ever gave it any.
func (s *Server) cableRow(mac, port string, now time.Time) orbRow {
	r := orbRow{
		Key: mac, MAC: mac, Instance: instanceForMAC(mac), Identity: "descriptor",
		OnCable: true, Via: []string{"usb"}, Ports: []string{port},
		Refusal: "on a cable at " + port + " with no address yet: identify it or bring it onto Wi-Fi",
	}
	if s.fleet == nil {
		return r
	}
	row, ok := s.fleet.ByMAC(mac)
	if !ok {
		return r
	}
	foldStoreName(&r, row)
	r.FW, r.State, r.IP, r.Port = row.FW, row.State, row.IP, row.Port
	r.Cable = s.chipRead(row.MAC)
	r.AgeS = int(now.Sub(row.LastSeen).Seconds())
	if r.Cable {
		r.Identity = "confirmed"
	}
	// The store had an address the browse did not, which is the ordinary shape of an
	// orb that has a lease and stopped announcing. The address stays on the row because
	// it is what mayDial judged, and the refusal carries the hub's own words rather than
	// a bare "no address yet" — otherwise the operator cannot see what was refused.
	if r.IP != "" {
		may, why := s.mayDial(r.IP)
		r.Dialable, r.Refusal = may, why
		if b := s.grants.bound(r.IP); b != "" {
			r.Bound = b == mac
		}
		r.Via = []string{"usb", "lan"}
	}
	return r
}

// unknownCableRow is an orb the host can see and cannot name. It is a row because the
// alternative is silence about a board that is physically in the machine, and a picker
// cannot tell the operator which cable to identify. It carries no MAC, so nothing that
// keys on identity — naming, granting, forgetting — can act on it.
func (s *Server) unknownCableRow(port string) orbRow {
	return orbRow{
		Key: "port:" + port, Instance: port, Identity: "unknown",
		OnCable: true, Via: []string{"usb"}, Ports: []string{port},
		Refusal: "this cable carries no serial descriptor: identify the orb over USB to give the row a mac",
	}
}

// identityOf labels a row the browse produced. confirmed means the fleet store holds a
// chip-read MAC for it — a fact that does not expire, because the hub read the chip.
// claimed means everything the row currently knows came from a packet.
//
// A remembered descriptor sighting is deliberately NOT one of the answers here. The
// store keeps the TTY so the board survives the unplug and so a later rename can find
// the lead to go down, but identity answers "who vouches for this row right now", and
// once the cable is gone the only thing vouching is the announcement. The merge loop
// puts "descriptor" on a row the CURRENT sweep matches to a port; an identity that
// outlived the plug would also move the stream digest on an unplug that changed nothing
// the operator can see.
func (s *Server) identityOf(mac string) string {
	if s.chipRead(mac) {
		return "confirmed"
	}
	return "claimed"
}

// chipRead reports whether THIS RUN of the hub read the MAC off a chip over USB. The
// set is in memory and deliberately never persisted: the strongest fact in the system
// is a record of an act this process performed, and a file written by an earlier one
// cannot make this process remember performing it. A hub restarted beside the same bench
// re-reads the descriptors and earns the answer again inside one sweep.
func (s *Server) chipRead(mac string) bool {
	s.cmu.Lock()
	defer s.cmu.Unlock()
	return s.read[mac]
}

// markChipRead records the read. markCable is the only caller, because a chip's word
// for its own MAC is the only thing that can justify the claim.
func (s *Server) markChipRead(mac string) {
	if mac == "" {
		return
	}
	s.cmu.Lock()
	defer s.cmu.Unlock()
	if s.read == nil {
		s.read = map[string]bool{}
	}
	s.read[mac] = true
}

// clearChipRead withdraws the claim. Only a wipe calls it: the chip's word was about a
// firmware that no longer exists, so leaving the read on the record would keep a board
// the hub just emptied looking like an orb — and the New Orb door hides exactly the
// boards it has evidence about. The boot stamp goes with it, for the same reason.
func (s *Server) clearChipRead(mac string) {
	if mac == "" {
		return
	}
	s.cmu.Lock()
	defer s.cmu.Unlock()
	delete(s.read, mac)
}

// appendPort keeps a row's port list sorted and duplicate-free. Two ttys on one board
// (native USB plus a JTAG link) is one row with two entries, and the order must not
// depend on enumeration order.
func appendPort(ports []string, p string) []string {
	for _, have := range ports {
		if have == p {
			return ports
		}
	}
	ports = append(ports, p)
	sort.Strings(ports)
	return ports
}

// sortRows orders the merged list by what the operator reads, then by key, so a row
// does not trade places with itself between two frames the way Store.All exists to
// keep the fleet list from doing.
func sortRows(rows []orbRow) {
	sort.Slice(rows, func(i, j int) bool {
		a, b := displayOf(rows[i]), displayOf(rows[j])
		if a != b {
			return a < b
		}
		return rows[i].Key < rows[j].Key
	})
}

// displayOf is orbRow's reading of orbfleet.Orb.DisplayName: the operator's word, then
// the instance, then the MAC. A cable the host cannot name has none of those, so its tty
// path stands in — honest, because that is the only thing the hub can say about it.
func displayOf(r orbRow) string {
	switch {
	case r.Name != "":
		return r.Name
	case r.Instance != "":
		return r.Instance
	case r.MAC != "":
		return r.MAC
	default:
		return r.Key
	}
}

// instanceForMAC is the device's own naming rule (orbmdns.ExpectInstance): "orb-" plus
// the last six hex digits of the base MAC. A cable-only board has no announcement to
// carry an instance, and deriving it here is what makes an unnamed board read the same
// on its cable row as it will once it joins.
func instanceForMAC(mac string) string { return "orb-" + mac[len(mac)-6:] }

// foldStoreName is where a cable row learns the name the hub last read off the board.
// A function rather than inline so the fields the store owns — the cached name and the
// note about the last refused fold — cannot drift from the set the LAN fold takes.
func foldStoreName(r *orbRow, row orbfleet.Orb) {
	r.Name, r.Note, r.Shared = row.Name, row.Note, row.Shared
}

// watchCables is the push half of plug detection: nothing in the mDNS path notices a
// board being plugged in, so a stream that woke only on packets would keep reporting a
// cable-less bench until the next check-in. It ticks faster than the sweep TTL so each
// tick pays a fresh enumeration rather than the cache, and refresh is what fires the
// wake when the digest moved. It exits with the context; a hub shutting down is not an
// orb unplugging.
func (s *Server) watchCables(ctx context.Context) {
	t := time.NewTicker(cableSweepTTL / 2)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			s.cables.refresh(time.Now())
		}
	}
}
