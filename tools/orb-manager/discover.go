package main

import (
	"context"
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
// Two rules hold it in place.
//
//  1. A browse never dials. internal/orbmdns cannot import an HTTP client — its own
//     suite parses its imports to prove it — so a listing is not a port scan with
//     better wording.
//  2. A browse never grants. Discovery fills a candidate list. The proxy still dials
//     only what ipGrants holds, which is an address the operator named or the USB
//     console read. An orb appearing in the list does not make its address reachable
//     by a page, and TestDiscoveredAddressIsNotDialable is what keeps that true when
//     the next person wires the button up.

// discovery is the browse cache plus the sockets behind it. The Server holds it as
// an interface so a test can hand in a browser fed from memory. Production uses
// *orbmdns.Browser, and its absence is reported rather than fatal: a hub that
// cannot bind 5353 still flashes, still manages, and says why the list is empty.
type discovery interface {
	Ask() error
	Candidates() []orbmdns.Candidate
	Refusals() []orbmdns.Refusal
	Close() error
}

// browseInterval is how often the hub re-asks. A record's TTL is 120 s and the
// firmware re-announces only when an interface changes, so an interval under half
// the TTL keeps a row from ageing out while the hub is open without turning the LAN
// into a query firehose.
const browseInterval = 45 * time.Second

// startBrowse attaches the eligible interfaces and begins the read loop. main calls
// it, never newHub: the suite builds servers by the dozen, and one of them holding
// 5353 makes the next one's bind a coin flip.
func (s *Server) startBrowse(ctx context.Context) {
	b := orbmdns.NewBrowser(orbmdns.Config{Logf: func(f string, a ...any) {
		log.Printf("orb-manager: "+f, a...)
	}})
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
	log.Printf("orb-manager: browsing %s on %d interface(s): %s",
		orbmdns.ServiceName, len(names), strings.Join(names, ", "))
	if err := b.Ask(); err != nil {
		s.noteBrowseAskErr(err)
	}
	go func() {
		t := time.NewTicker(browseInterval)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				s.reconcileFleet()
				// An unanswered query is normal here: this box may not originate
				// multicast onto the LAN at all, and the answers that matter
				// arrive unasked. Say it once, not every 45 s.
				if err := b.Ask(); err != nil {
					s.noteBrowseAskErr(err)
				}
			}
		}
	}()
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

// noteBrowseAskErr records the first query failure for the card to show. A hub on
// the WSL box in mirrored networking cannot originate multicast at all, which is a
// fact about the host and not a fault in the orb; the honest surface is a visible
// line rather than a list that is quietly empty forever.
func (s *Server) noteBrowseAskErr(err error) {
	s.dmu.Lock()
	if s.browseAskErr == "" {
		s.browseAskErr = err.Error()
	}
	s.dmu.Unlock()
}

// orbRow is one candidate as the UI sees it. Age is reported rather than a
// timestamp so a skewed hub clock cannot make an orb look immortal or vanished, and
// the fields are exactly what a browse is allowed to know: what the orb claimed, not
// what the hub confirmed by dialing it.
type orbRow struct {
	Instance  string `json:"instance"`
	MAC       string `json:"mac"`
	FW        string `json:"fw"`
	State     string `json:"state"`
	IP        string `json:"ip"`
	Port      uint16 `json:"port"`
	TTL       uint32 `json:"ttl"`
	Solicited bool   `json:"solicited"`
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

// handleDiscovered is the read side: the current candidate list. It sends nothing,
// opens nothing and needs no token — what the hub overheard on its own subnet is the
// same class of information as /api/ports, and the UI polls it on a timer.
func (s *Server) handleDiscovered(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	s.dmu.Lock()
	b, listenErr, askErr := s.browse, s.browseErr, s.browseAskErr
	ifaces := append([]string(nil), s.browseIfaces...)
	s.dmu.Unlock()
	if b == nil {
		s.jsonOK(w, map[string]any{
			"orbs": []orbRow{}, "refused": []map[string]any{}, "interfaces": ifaces,
			"listening": false, "error": orElse(listenErr, "discovery is not running"),
			"askError": askErr,
		})
		return
	}
	now := time.Now()
	// Project the cache into the fleet before building rows, or the card would show a
	// contention the proxy is already refusing over. No renewal: that is the timer's.
	s.foldCandidates(false)
	rows := []orbRow{}
	for _, c := range b.Candidates() {
		// One call, one answer: the same decision the proxy will make if this row is
		// clicked, so the card cannot promise a dial the request would refuse.
		may, why := s.mayDial(c.IP)
		r := orbRow{
			Instance: c.Instance, MAC: c.MAC, FW: c.FW, State: c.State,
			IP: c.IP, Port: c.Port, TTL: c.TTL, Solicited: c.Solicited,
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
	s.jsonOK(w, map[string]any{
		"orbs": rows, "refused": refused, "interfaces": ifaces,
		"listening": true, "error": "", "askError": askErr,
	})
}

// handleBrowseNow is the deliberate "ask again". It is guarded like every route that
// puts a packet on the wire: a page the operator never opened should not be able to
// set the rhythm the hub multicasts on.
func (s *Server) handleBrowseNow(w http.ResponseWriter, r *http.Request) {
	s.dmu.Lock()
	b, listenErr := s.browse, s.browseErr
	s.dmu.Unlock()
	if b == nil {
		s.jsonErr(w, orElse(listenErr, "discovery is not running on this hub"))
		return
	}
	if err := b.Ask(); err != nil {
		s.noteBrowseAskErr(err)
		s.jsonErr(w, err.Error())
		return
	}
	s.jsonOK(w, map[string]any{"asked": true})
}
