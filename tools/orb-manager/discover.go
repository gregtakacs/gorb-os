package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// Finding an orb without hunting for it.
//
// The hub used to know an address two ways: something the operator typed, or
// something the USB console printed. Both still work. Neither covers an orb already
// joined, powered and on the LAN with no cable in sight, and the old answer to that
// was a /24 sweep — deleted rather than improved, because dialing 254 hosts to find
// one of them is what the ARP table, the neighbour port and the host firewall prompt
// were before it went. The orb has announced itself over mDNS since item 4; this
// file is the listening.
//
// The hub listens only. It used to also ask: a startup burst, then a re-query on a
// timer, on the theory that a device which announces once at its join leaves a
// hub that starts later waiting on a rhythm it does not control. That theory is
// retired — the firmware now re-announces every MDNS_ADV_REFRESH_S (60 s, under
// half the record's 120 s TTL), so the orb owns its own freshness and the hub's
// queries bought nothing but traffic and a class of failure ("this box cannot
// originate multicast") that only existed because the hub was trying to originate
// something. Cold discovery now costs up to one check-in interval. Everything the
// querier was load-bearing for — row sustenance, late-hub startup, half-heard
// instances — the check-in carries, because the firmware bundles PTR, SRV, TXT and
// A in every one.
//
// Two rules hold this file in place.
//
//  1. A browse never dials. internal/orbmdns cannot import an HTTP client — its own
//     suite parses its imports to prove it — so a listing is not a port scan with
//     better wording. It cannot send anything at all: TestBrowserNeverSends fails the
//     package if a write path reappears.
//  2. A browse never grants. Discovery fills a candidate list. The proxy still dials
//     only what ipGrants holds, which is an address the operator named or the USB
//     console read. An orb appearing in the list does not make its address reachable
//     by a page, and TestDiscoveredAddressIsNotDialable is what keeps that true when
//     the next person wires the button up.

// discovery is the browse cache plus the sockets behind it. The Server holds it as
// an interface so a test can hand in a browser fed from memory. Production uses
// *orbmdns.Browser, and its absence is reported rather than fatal: a hub that
// cannot bind 5353 still flashes, still manages, and says why the list is empty.
// There is no Ask on this interface: the only way a row appears is a packet the
// orb sent on its own.
type discovery interface {
	Candidates() []orbmdns.Candidate
	Refusals() []orbmdns.Refusal
	Close() error
}

// upkeepInterval is how often the hub tidies what it overheard: prune rows whose
// TTL has run out, and renew the grant of a board the LAN is still producing. It
// puts nothing on the wire — the orb's 60 s check-in is the only discovery traffic
// this hub generates. The number only has to sit comfortably under the grant TTL
// (ipGrantTTL, 6 h) and the record grace; 45 s keeps a dead row out of the list for
// less than the time it takes to notice it is gone. A var so
// TestUpkeepLoopTicksAndStops can drive a tick in test time; production never writes
// it.
var upkeepInterval = 45 * time.Second

// checkinSeconds mirrors the firmware's MDNS_ADV_REFRESH_S: how often an orb
// re-announces itself, which is the rhythm every row on the card runs on. It is
// published to the UI so the copy can promise a real number, and
// TestCheckinCadenceMatchesFirmware fails the build if the two spellings ever drift
// apart — the hub has no authority to re-derive what the device decides.
const checkinSeconds = 60

// startListening attaches the eligible interfaces and begins the read loop. main calls
// it, never newHub: the suite builds servers by the dozen, and one of them holding
// 5353 makes the next one's bind a coin flip.
func (s *Server) startListening(ctx context.Context) {
	b := s.newBrowser()
	ifaces, err := orbmdns.EligibleInterfaces()
	if err != nil {
		s.setDiscovery(nil, "interfaces: "+err.Error())
		return
	}
	var sources []orbmdns.Source
	var names []string
	for _, ifi := range ifaces {
		c, err := orbmdns.MulticastSource(ifi)
		if err != nil {
			// One interface refusing the join is no reason to have no discovery
			// at all: a hub on a box with a VPN adapter and Wi-Fi should find the
			// orb on whichever one works and log the other rather than dying.
			log.Printf("orb-manager: %v", err)
			continue
		}
		sources = append(sources, c)
		names = append(names, ifi.Name)
	}
	if len(sources) == 0 {
		s.setDiscovery(nil, "no interface could join "+orbmdns.GroupAddr)
		return
	}
	if err := b.Start(ctx, sources...); err != nil {
		s.setDiscovery(nil, err.Error())
		return
	}
	s.setDiscovery(b, "")
	s.setBrowseIfaces(names)
	log.Printf("orb-manager: listening for %s check-ins on %d interface(s): %s",
		orbmdns.ServiceName, len(names), strings.Join(names, ", "))
	go s.upkeepLoop(ctx)
}

// upkeepLoop is the hub's own housekeeping tick, and it sends nothing. Candidates()
// prunes what the clock has outlived, and reconcileFleet renews the grant of every
// board still in the cache — a cache that only ever fills from a packet the orb sent
// first, so renewal is the LAN continuing to produce the board, not the hub vouching
// for it. The loop takes no browser argument and exists on the Server's own state so
// a test can drive it with a fake in place of the socket set.
func (s *Server) upkeepLoop(ctx context.Context) {
	t := time.NewTicker(upkeepInterval)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			s.reconcileFleet()
		}
	}
}

func (s *Server) setDiscovery(d discovery, why string) {
	s.dmu.Lock()
	s.browse, s.browseErr = d, why
	s.dmu.Unlock()
}

func (s *Server) setBrowseIfaces(names []string) {
	s.dmu.Lock()
	s.browseIfaces = append([]string(nil), names...)
	s.dmu.Unlock()
}

// orbRow is one candidate as the UI sees it. Age is reported rather than a
// timestamp so a skewed hub clock cannot make an orb look immortal or vanished, and
// the fields are exactly what a browse is allowed to know: what the orb claimed, not
// what the hub confirmed by dialing it. AgeS doubles as the check-in clock: a row at
// age 200 s is an orb whose last announce is wearing out, not a row the hub forgot
// to refresh.
type orbRow struct {
	Instance  string `json:"instance"`
	MAC       string `json:"mac"`
	FW        string `json:"fw"`
	State     string `json:"state"`
	IP        string `json:"ip"`
	Port      uint16 `json:"port"`
	TTL       uint32 `json:"ttl"`
	AgeS      int    `json:"ageS"`
	Provision bool   `json:"provisioning"`
	Dialable  bool   `json:"dialable"`

	// What the fleet adds to what the wire claimed. Name and Room are the operator's
	// words, so a row that has been named reads as the orb they already own instead of
	// a hex tail they have to recognize twice.
	Name string `json:"name,omitempty"`
	Room string `json:"room,omitempty"`
	// Cable says the hub has read this MAC off the chip over USB, which is the only
	// proof in the system; without it the identity is what a packet asserted.
	Cable bool `json:"cable"`
	// Contended: another live board announces this same address, so the hub will not
	// dial it for anybody. Shared: another board derives this instance name from a
	// different MAC, so the label alone cannot identify the row.
	Contended bool `json:"contended"`
	Shared    bool `json:"shared"`
	// Bound: the grant for this address names the same board, so loading it is the
	// hub reaching the orb the operator actually committed to rather than whatever
	// holds the lease today.
	Bound bool   `json:"bound"`
	Note  string `json:"note,omitempty"`
	// Refusal is why the hub would NOT dial this address, in the hub's own words: the
	// reason the proxy will answer with, not a paraphrase of it. A row that is simply
	// not clickable with no explanation is a row the operator has to guess about.
	Refusal string `json:"refusal,omitempty"`
}

// handleDiscovered is the read side: the current candidate list in one shot. It sends
// nothing, opens nothing and needs no token — what the hub overheard on its own subnet
// is the same class of information as /api/ports. The card no longer polls it: the page
// holds /api/orbs/stream open and lets a check-in move it, and this stays the endpoint a
// stranger, a probe or a curl hits. Both answers come from orbCard, which is the only
// thing that decides what a row means.
func (s *Server) handleDiscovered(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	body, _ := s.orbCard(time.Now())
	s.jsonOK(w, body)
}

// orbCard builds the discovery card and its fingerprint. The payload is what GET
// /api/orbs answers and what every /api/orbs/stream frame carries, byte for byte, so a
// row cannot mean one thing on a poll and another on a push; the version is what the
// stream compares to decide whether a frame is worth the wire. now is a parameter so
// the tests can freeze the clock and age a row on purpose.
func (s *Server) orbCard(now time.Time) (map[string]any, string) {
	s.dmu.Lock()
	b, listenErr := s.browse, s.browseErr
	ifaces := append([]string(nil), s.browseIfaces...)
	s.dmu.Unlock()
	if b == nil {
		body := map[string]any{
			"orbs": []orbRow{}, "refused": []map[string]any{}, "interfaces": ifaces,
			"listening": false, "error": orElse(listenErr, "discovery is not running"),
			"checkinS": checkinSeconds,
		}
		// The version carries the reason, so a hub that fails to bind and then
		// recovers is a version change, and the stream says "listening" the moment it
		// is true rather than leaving the failure up forever.
		return body, "not-listening|" + listenErr
	}
	// Project the cache into the fleet before building rows, or the card would show a
	// contention the proxy is already refusing over. No renewal: that is the upkeep tick's.
	s.foldCandidates(false)
	rows := []orbRow{}
	for _, c := range b.Candidates() {
		// One call, one answer: the same decision the proxy will make if this row is
		// clicked, so the card cannot promise a dial the request would refuse.
		may, why := s.mayDial(c.IP)
		r := orbRow{
			Instance: c.Instance, MAC: c.MAC, FW: c.FW, State: c.State,
			IP: c.IP, Port: c.Port, TTL: c.TTL,
			AgeS: int(now.Sub(c.LastSeen).Seconds()), Provision: c.Provisioning(),
			// Dialable is a readout of the allowlist, not a request. The hub will
			// not reach an address the operator has not committed to, and the card
			// had better say so than let a click discover it.
			Dialable: may,
			Refusal:  why,
			Bound:    s.grants.bound(c.IP) != "" && s.grants.bound(c.IP) == c.MAC,
		}
		if s.fleet != nil {
			if row0, ok := s.fleet.ByMAC(c.MAC); ok {
				r.Name, r.Room, r.Cable, r.Shared = row0.Name, row0.Room, row0.Cable, row0.Shared
				r.Note = row0.Note
				r.Contended = row0.Contended || len(s.fleet.ClaimedBy(c.IP)) > 1
			}
		}
		rows = append(rows, r)
	}
	refused := []map[string]any{}
	for _, f := range b.Refusals() {
		refused = append(refused, map[string]any{
			"name": f.Name, "from": f.From, "reason": f.Reason,
			"ageS": int(now.Sub(f.At).Seconds()),
		})
	}
	body := map[string]any{
		"orbs": rows, "refused": refused, "interfaces": ifaces,
		"listening": true, "error": "", "checkinS": checkinSeconds,
	}
	return body, orbVersion(rows, refused, ifaces)
}

// orbVersion fingerprints the card minus the clock. Age is the field that moves on its
// own: a row the orb refreshed on schedule says nothing new about the orb, so a stream
// that counted age as content would repaint the page once a minute to change a number
// nobody reads. Everything an operator would call a change is in the digest — address,
// port, firmware, state, name, room, the cable/contended/shared/bound flags, the refusal
// text, and which interfaces the hub joined. A row's ttl and first-seen are in too, so a
// record about to expire and a row that just arrived are different answers even when every
// word on them matches.
func orbVersion(rows []orbRow, refused []map[string]any, ifaces []string) string {
	h := sha256.New()
	fmt.Fprintf(h, "ifaces %s\n", strings.Join(ifaces, ","))
	for _, r := range rows {
		a := r
		a.AgeS = 0
		fmt.Fprintf(h, "row %v\n", a)
	}
	for _, f := range refused {
		fmt.Fprintf(h, "refused %v|%v|%v\n", f["name"], f["from"], f["reason"])
	}
	return hex.EncodeToString(h.Sum(nil))
}

// ---- the live card: one wake-up, many listeners ---------------------------------

// newBrowser builds the browser the hub runs. It is a method rather than a call site
// because the one thing that must not be forgotten on it is the wake-up: a browser built
// without OnChange works, ages its rows correctly, and leaves the page frozen on whatever
// it loaded — a bug no test would catch if the wiring lived inline in startListening,
// where the sockets live and so no test of the whole path goes.
func (s *Server) newBrowser() *orbmdns.Browser {
	return orbmdns.NewBrowser(orbmdns.Config{
		Logf:     func(f string, a ...any) { log.Printf("orb-manager: "+f, a...) },
		OnChange: s.wakeOrbs,
	})
}

// subscribeOrbs hands one open /api/orbs/stream its own wake-up slot. The channel is
// buffered to one and is closed by nobody but its owner, which is what lets wakeOrbs fire
// without waiting on a page that has stopped reading.
func (s *Server) subscribeOrbs() chan struct{} {
	ch := make(chan struct{}, 1)
	s.omu.Lock()
	if s.orbSubs == nil {
		s.orbSubs = map[chan struct{}]struct{}{}
	}
	s.orbSubs[ch] = struct{}{}
	s.omu.Unlock()
	return ch
}

// unsubscribeOrbs drops the slot. It does not close the channel: a wake-up landing in a
// channel whose owner just went is a value nobody reads, while a send racing a close is a
// panic in a goroutine the page cannot see.
func (s *Server) unsubscribeOrbs(ch chan struct{}) {
	s.omu.Lock()
	delete(s.orbSubs, ch)
	s.omu.Unlock()
}

// wakeOrbs is orbmdns.Config.OnChange: the browser's cache moved, so every open stream
// should re-answer. It runs on the goroutine that just read a UDP datagram, so the one
// rule is that it never blocks — a subscriber whose slot is already full has a re-read
// coming and needs no second nudge. Ten pages and an orb flooding the LAN cannot make this
// loop cost more than ten non-blocking sends.
func (s *Server) wakeOrbs() {
	s.omu.Lock()
	defer s.omu.Unlock()
	for ch := range s.orbSubs {
		select {
		case ch <- struct{}{}:
		default:
		}
	}
}

// orbStreamKeepalive is the heartbeat the page watches to tell a quiet LAN from a dead
// pipe. It is not the freshness bound: that is orbStreamResync.
var orbStreamKeepalive = 25 * time.Second

// orbStreamResync re-answers the card on the timer even when no wake-up fired and the
// version has not moved, because two things the stream gets no event about can still
// change what the card says: upkeep pruning a row whose grace ran out while nobody was
// looking, and a grant or a fleet edit arriving over USB. Two minutes means the worst case
// is a stale row left on screen for the length of a coffee break, on a card whose row age
// already moves every keepalive. It is the old poll, demoted to a backstop.
var orbStreamResync = 2 * time.Minute

// handleOrbsStream keeps the discovery card open as a stream: one frame the instant it is
// asked for, then one per check-in that changes what the card says. The body of every frame
// is the same envelope GET /api/orbs answers, so the page has one renderer and its first
// paint is not a second code path.
//
// A plain GET, not EventSource and not a WebSocket: the page has one consumer and it is a
// WebView the hub owns, so an abortable fetch beats a reconnect loop, and the hub's own
// streaming precedent — the console and the flasher — is line-delimited JSON over a live
// response body the client already knows how to read. What a GET gives up is the token, and
// the trade is deliberate rather than forced: the frames carry exactly the bytes the
// unguarded GET answers, so guarding the stream would protect no information the poll left
// open. The bind stays loopback by default, and the remote bind is opt-in behind a warning.
//
// Nothing here dials, grants, or sends a query. The stream is the read endpoint wearing a
// longer hat, and TestHubHasNoRouteThatAsks holds over it.
func (s *Server) handleOrbsStream(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	flusher, ok := w.(http.Flusher)
	if !ok {
		s.jsonErr(w, "streaming unsupported on this listener")
		return
	}
	w.Header().Set("Content-Type", "application/x-ndjson")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Accel-Buffering", "no")

	send := func(body map[string]any) bool {
		if err := json.NewEncoder(w).Encode(map[string]any{"ok": true, "data": body}); err != nil {
			return false
		}
		flusher.Flush()
		return true
	}
	keep := func() bool {
		if _, err := fmt.Fprint(w, ":keep\n"); err != nil {
			return false
		}
		flusher.Flush()
		return true
	}

	wake := s.subscribeOrbs()
	defer s.unsubscribeOrbs(wake)

	// Seed before parking. A page opened seconds after a check-in would otherwise sit on
	// "Loading…" until the next one, and removing the Refresh button is only honest if the
	// stream answers the moment it opens.
	body, version := s.orbCard(time.Now())
	if !send(body) {
		return
	}
	lastFrame := time.Now()
	t := time.NewTicker(orbStreamKeepalive)
	defer t.Stop()

	ctx := r.Context()
	for {
		select {
		case <-ctx.Done():
			return

		case <-wake:
			// Re-read the whole card rather than trusting what the browser thought it
			// changed: the cache moving and the card moving are not the same question once
			// a fleet rename, a cable MAC or an expiring grant is involved, and the browser
			// can see none of those three.
			body, v := s.orbCard(time.Now())
			if v == version {
				if !keep() {
					return
				}
				continue
			}
			if !send(body) {
				return
			}
			version, lastFrame = v, time.Now()

		case <-t.C:
			// Heartbeat and backstop in one tick: the comment stops anything between the
			// page and the loopback port deciding an idle connection is dead, and every
			// orbStreamResync the card gets re-answered whatever woke it or did not.
			if !keep() {
				return
			}
			if time.Since(lastFrame) < orbStreamResync {
				continue
			}
			body, v := s.orbCard(time.Now())
			if !send(body) {
				return
			}
			version, lastFrame = v, time.Now()
		}
	}
}
