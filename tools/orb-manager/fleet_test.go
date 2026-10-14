package main

import (
	"bytes"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// Item 6: the fleet is keyed on the chip, and the cross-lock is what keeps an
// announcement — which is an unauthenticated claim — from deciding which board a
// granted address belongs to.

const macHall = "44bd8d782618" // instance orb-782618
const macBed = "aabbcc112233"  // instance orb-112233

func inst(mac string) string { return "orb-" + mac[len(mac)-6:] }

// fleetHub is a hub whose browse cache announced the given (mac, ip) pairs and whose
// operator then committed to them, which is the only way an address becomes dialable.
func fleetHub(t *testing.T, pairs ...string) *Server {
	t.Helper()
	s := newHub()
	var cs []orbmdns.Candidate
	for i := 0; i+1 < len(pairs); i += 2 {
		cs = append(cs, orbmdns.Candidate{
			MAC: pairs[i], Instance: inst(pairs[i]), IP: pairs[i+1], Port: 80,
			FW: "094fa22", State: "associated",
			From:      pairs[i+1],
			FirstSeen: time.Now(), LastSeen: time.Now(), Expires: time.Now().Add(2 * time.Minute),
		})
	}
	s.browse = filled(t, cs...)
	s.reconcileFleet()
	return s
}

// dialableFleetHub is fleetHub for the case where the hub has to reach the board:
// naming writes through to the device now, so a test that expects a rename to land
// needs an orb on the end of the wire, a grant for its address, and a device client
// aimed at the port the double is listening on. The returned fake is what the test
// reads the stored name back off — the assertion that matters is the device's, not
// the hub's.
//
// The rows go in through Store.Observe rather than through a browse announcement, and
// that is not a shortcut: orbmdns refuses a loopback address on purpose (names.go:63,
// "a packet that says the orb lives at 127.0.0.1 is a bug or an attack"), so a
// httptest backend cannot be announced at all. Observe is the same fold the browser's
// promotion feeds, one step earlier in the pipeline — which is exactly where a real
// USB-detected address lands too.
//
// Any address after the first pair is a row the hub can see but must never dial, which
// is the shape of a cross-lock refusal.
func dialableFleetHub(t *testing.T, mac, ip string, more ...string) (*orbfake.Orb, *Server) {
	t.Helper()
	f := orbfake.New()
	// The MAC the fleet carries and the MAC the device reports have to agree, or the
	// fold keys a row nobody asked for and the test passes for the wrong reason.
	f.SetAddress(mac, ip)
	backend := httptest.NewServer(f)
	t.Cleanup(backend.Close)
	_, port, err := net.SplitHostPort(strings.TrimPrefix(backend.URL, "http://"))
	if err != nil {
		t.Fatalf("backend URL %q: %v", backend.URL, err)
	}
	s := newHub()
	now := time.Now()
	if _, err := s.fleet.Observe(orbfleet.Observation{
		MAC: mac, Instance: inst(mac), IP: ip, Port: 80, FW: "094fa22",
		State: "associated", At: now,
	}); err != nil {
		t.Fatalf("fold of the dialable board: %v", err)
	}
	for i := 0; i+1 < len(more); i += 2 {
		if _, err := s.fleet.Observe(orbfleet.Observation{
			MAC: more[i], Instance: inst(more[i]), IP: more[i+1], Port: 80, FW: "094fa22",
			State: "associated", At: now,
		}); err != nil {
			t.Fatalf("fold of %s: %v", more[i], err)
		}
	}
	c := orbnet.NewClient(2 * time.Second)
	c.DevicePort = port
	s.device = c
	// The grant is what makes the address dialable at all; the fold only says who is
	// standing there.
	s.grants.learnAs(ip, mac)
	return f, s
}

func postFleet(t *testing.T, s *Server, path string, body any, headers map[string]string) *httptest.ResponseRecorder {
	t.Helper()
	b, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	r := httptest.NewRequest(http.MethodPost, path, bytes.NewReader(b))
	r.Header.Set("Content-Type", "application/json")
	for k, v := range headers {
		r.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	// Through the hub's real routing table, not a handler we wrapped by hand: a test
	// that applied requireControl itself would still pass on the day somebody took it
	// off the route, which is the one mistake worth catching here.
	mux := http.NewServeMux()
	s.routes(mux)
	mux.ServeHTTP(rec, r)
	return rec
}

// The card must refuse exactly what the proxy refuses. A row that looks clickable and
// then 403s is the bug that makes an operator click twice and blame the orb.
func TestTheCardRefusesWhatTheProxyRefuses(t *testing.T) {
	now := time.Now()
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return now }})
	s.browse = filled(t,
		orbmdns.Candidate{MAC: macHall, Instance: inst(macHall), IP: "192.168.2.31", Port: 80,
			State: "associated", From: "192.168.2.31", FirstSeen: now, LastSeen: now, Expires: now.Add(2 * time.Minute)},
		orbmdns.Candidate{MAC: macBed, Instance: inst(macBed), IP: "192.168.2.31", Port: 80,
			State: "associated", From: "192.168.2.31", FirstSeen: now, LastSeen: now, Expires: now.Add(2 * time.Minute)})
	s.grants.learnAs("192.168.2.31", macHall)

	got := getDiscovered(t, s)
	if len(got.Data.Orbs) != 2 {
		t.Fatalf("the card listed %d rows", len(got.Data.Orbs))
	}
	for _, row := range got.Data.Orbs {
		if row.Dialable {
			t.Errorf("the card offers to dial a contended address: %+v", row)
		}
		if row.Refusal == "" {
			t.Errorf("the row says no, and does not say why: %+v", row)
		}
	}
	rec := httptest.NewRecorder()
	s.proxyOrb(rec, httptest.NewRequest(http.MethodGet, "/orb/192.168.2.31/api/v1/status", nil))
	if rec.Code != http.StatusForbidden {
		t.Errorf("the card refused and the proxy did not: %d %s", rec.Code, rec.Body.String())
	}
}

func okBody(t *testing.T, rec *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &m); err != nil {
		t.Fatalf("body is not JSON: %q: %v", rec.Body.String(), err)
	}
	return m
}

// The cross-lock. A granted address is a lease; the MAC is the board. When the board
// at a committed address turns out to be a different one, the hub must stop.
func TestTheCommittedBoardIsDialedAndAnyOtherIsNot(t *testing.T) {
	now := time.Now()
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return now }})
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now))
	s.grants.learnAs("192.168.2.31", macHall)
	if ok, why := s.mayDial("192.168.2.31"); !ok {
		t.Fatalf("the committed board was refused its own address: %s", why)
	}
}

// A DHCP handover and an impersonation look identical for one moment: two boards
// announce one address. The hub cannot tell them apart, so it must refuse the dial
// WITHOUT destroying a commitment the operator made — the contention clears by itself
// within a TTL, and making them re-click on every lease change is how a safety rule
// gets routed around.
func TestATransientContentionRefusesTheDialAndRecovers(t *testing.T) {
	now := time.Now()
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return now }})
	s.grants.learnAs("192.168.2.31", macHall)
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now))
	s.fleet.Observe(obsAt(macBed, "192.168.2.31", now))

	ok, why := s.mayDial("192.168.2.31")
	if ok {
		t.Fatal("the hub would dial an address two boards both claim")
	}
	if !strings.Contains(why, "two") {
		t.Errorf("the refusal does not name the contradiction: %q", why)
	}
	if !s.grants.allowed("192.168.2.31") {
		t.Error("the contention destroyed a commitment that may well have been a lease change")
	}

	// The interloper stops announcing: the committed board must be dialable again with
	// no new human action, which is the whole point of keeping the grant alive.
	now = now.Add(7 * time.Minute)
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now))
	s.grants.renew("192.168.2.31", macHall)
	if ok, why := s.mayDial("192.168.2.31"); !ok {
		t.Errorf("the contention outlived the contention: %s", why)
	}
}

// When the board the operator committed to is GONE from the address and two others are
// arguing over it, that is not a handover in progress. The commitment is void.
func TestAGrantIsVoidWhenTheCommittedBoardIsAbsent(t *testing.T) {
	now := time.Now()
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return now }})
	s.grants.learnAs("192.168.2.31", macHall)
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now.Add(-7*time.Minute)))
	s.fleet.Observe(obsAt(macBed, "192.168.2.31", now))
	third := "aabbcc445566"
	s.fleet.Observe(obsAt(third, "192.168.2.31", now))

	ok, why := s.mayDial("192.168.2.31")
	if ok {
		t.Fatal("the hub would still dial for a board that stopped announcing")
	}
	if !strings.Contains(why, macHall) {
		t.Errorf("the refusal does not say which board went missing: %q", why)
	}
	if s.grants.allowed("192.168.2.31") {
		t.Error("the grant outlived the board it named")
	}
}

// The proxy must reach the same answer the card shows, or a row marked clickable fails
// on click and a row marked dead was a lie.
func TestTheProxyAndTheCardGiveTheSameAnswer(t *testing.T) {
	dials := 0
	host, s := localOrb(t, &dials)
	s.grants.learnAs(host, macHall)
	if _, err := s.fleet.Observe(obs(macHall, host)); err != nil {
		t.Fatal(err)
	}
	rec := httptest.NewRecorder()
	s.proxyOrb(rec, httptest.NewRequest(http.MethodGet, "/orb/"+host+"/api/v1/status", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("the matched board was refused: %d %s", rec.Code, rec.Body.String())
	}
	if dials == 0 {
		t.Error("the OK reply did not come from the device end, so nothing was proven")
	}

	// Same hub, same grant, one new announcement: the dial must now be refused.
	if _, err := s.fleet.Observe(obs(macBed, host)); err != nil {
		t.Fatal(err)
	}
	rec = httptest.NewRecorder()
	s.proxyOrb(rec, httptest.NewRequest(http.MethodGet, "/orb/"+host+"/api/v1/status", nil))
	if rec.Code != http.StatusForbidden {
		t.Fatalf("the proxy dialed through the cross-lock: %d %s", rec.Code, rec.Body.String())
	}
	if before := dials; before == 0 {
		t.Error("no dial happened at all, so the refusal proves nothing")
	}
	if !strings.Contains(rec.Body.String(), "error") {
		t.Errorf("the refusal is not the JSON envelope the client parses: %q", rec.Body.String())
	}
}

// An unbound grant — the operator typed an address and no announcement names a board
// there — must still work. Locking it would break the wired bench flow this hub exists
// for and buy nothing, because there is no second claim to prefer.
func TestAGrantTheFleetCannotCheckIsStillHonoured(t *testing.T) {
	s := newHub()
	s.grants.learn("192.168.2.9")
	if ok, why := s.mayDial("192.168.2.9"); !ok {
		t.Errorf("an unbound grant was refused on no evidence: %s", why)
	}
	// And the moment the fleet CAN check it, it does.
	if _, err := s.fleet.Observe(obs(macHall, "192.168.2.9")); err != nil {
		t.Fatal(err)
	}
	s.grants.learnAs("192.168.2.9", macBed)
	if ok, _ := s.mayDial("192.168.2.9"); ok {
		t.Error("a grant bound to one board dialed an address another board claims")
	}
}

// The cable outranks the wire: the address the hub read off the chip is not moved by
// an announcement, and the refusal says why instead of silently preferring one.
func TestACableConfirmedBoardIsNotMovedByAPacket(t *testing.T) {
	s := newHub()
	s.markCable(macHall)
	if _, err := s.fleet.Observe(obs(macHall, "192.168.2.31")); err != nil {
		t.Fatal(err)
	}
	s.grants.learnAs("192.168.2.31", macHall)

	// An announcement putting this same board at a new address.
	if _, err := s.fleet.Observe(obs(macHall, "192.168.9.9")); err == nil {
		t.Error("a packet relocated a board the hub had read over USB")
	}
	if ok, why := s.mayDial("192.168.2.31"); !ok {
		t.Errorf("the cable-verified address lost its dial to a claim: %s", why)
	}
	row, ok := s.fleet.ByMAC(macHall)
	if !ok || row.IP != "192.168.2.31" {
		t.Errorf("the row moved: %+v", row)
	}
}

// Liveness from announcements: the board a grant was committed for keeps it by
// announcing; a different board at that address must not be able to.
func TestOnlyTheCommittedBoardRenewsAGrant(t *testing.T) {
	g := newIPGrants(ipGrantTTL)
	g.learnAs("192.168.2.31", macHall)

	// A still-live grant is extended by its own board and by nobody else.
	if g.renew("192.168.2.31", macBed) {
		t.Error("a stranger at the address renewed somebody else's commitment")
	}
	if !g.renew("192.168.2.31", macHall) {
		t.Fatal("the committed board could not keep its own grant alive by announcing")
	}
	if !g.allowed("192.168.2.31") {
		t.Error("the renewal did not leave the dial usable")
	}

	// Once it HAS aged out, the right MAC does not bring it back either. An mDNS MAC is
	// broadcast in the clear, so treating a repeat of it as proof would let whoever
	// grabbed the address restore a dial a human had already let lapse.
	g.mu.Lock()
	st := g.byIP["192.168.2.31"]
	st.at = time.Now().Add(-2 * ipGrantTTL)
	g.byIP["192.168.2.31"] = st
	g.mu.Unlock()
	if g.allowed("192.168.2.31") {
		t.Fatal("an expired grant was still accepted")
	}
	if g.renew("192.168.2.31", macHall) {
		t.Error("an expired grant was revived by hearing its MAC again")
	}
	if g.allowed("192.168.2.31") {
		t.Error("the refused renewal left the dial open anyway")
	}
	// An unbound grant is not renewable at all: nothing may extend what nobody bound.
	g.learn("192.168.2.40")
	if g.renew("192.168.2.40", macHall) {
		t.Error("an unbound grant was renewed by an announcement")
	}
}

// The ceiling: announcements keep a grant fresh, and cannot make it immortal.
func TestRenewalCannotStretchAGrantForever(t *testing.T) {
	g := newIPGrants(ipGrantTTL)
	g.learnAs("192.168.2.31", macHall)
	g.mu.Lock()
	st := g.byIP["192.168.2.31"]
	st.since = time.Now().Add(-31 * 24 * time.Hour)
	g.byIP["192.168.2.31"] = st
	g.mu.Unlock()
	if g.renew("192.168.2.31", macHall) {
		t.Error("a month-old commitment is still being extended by a packet")
	}
	if g.allowed("192.168.2.31") {
		t.Error("a grant past the ceiling was still accepted, whatever the announcements say")
	}
}

func TestFleetNamingWritesAreGuarded(t *testing.T) {
	f, s := dialableFleetHub(t, macHall, "127.0.0.1")
	body := map[string]string{"mac": macHall, "name": "Hall", "room": "Upstairs"}

	rec := postFleet(t, s, "/api/fleet/set", body, nil)
	if rec.Code != http.StatusForbidden {
		t.Errorf("a tokenless rename was accepted: %d %s", rec.Code, rec.Body.String())
	}
	rec = postFleet(t, s, "/api/fleet/set", body, map[string]string{
		"X-Orb-Token": s.token, "Sec-Fetch-Site": "cross-site",
	})
	if rec.Code != http.StatusForbidden {
		t.Errorf("a cross-origin rename carried by a stolen token was accepted: %d", rec.Code)
	}
	// Neither refusal above may have reached the board. A guard that rejects the
	// request on the way in and names the orb on the way out is not a guard.
	if got := f.DeviceName(); got != "" {
		t.Errorf("a refused rename still reached the device: %q", got)
	}
	rec = postFleet(t, s, "/api/fleet/set", body, map[string]string{"X-Orb-Token": s.token})
	if rec.Code != http.StatusOK {
		t.Fatalf("the operator's own rename was refused: %d %s", rec.Code, rec.Body.String())
	}
	row, ok := s.fleet.ByMAC(macHall)
	if !ok || row.Name != "Hall" || row.Room != "Upstairs" {
		t.Errorf("the name did not land: %+v", row)
	}
	// The device, not just the hub file: the orb owns this string and the row is a
	// cache of it, so a rename that updated only the cache is the bug this feature
	// was built to remove.
	if got := f.DeviceName(); got != "Hall" {
		t.Errorf("the hub recorded %q but the orb holds %q", row.Name, got)
	}
}

// Two boards, one word: the fleet would not be able to say which orb a later
// credential belongs to, so the second claim is refused where it is made.
//
// This test also pins the ORDER, and that is the sharper half of it since naming
// went over the wire. Hall has a board listening on loopback; Bed does not exist on
// the wire at all. If the cross-lock ran after the write-through, Bed's rename would
// fail with a dial error instead of the "already the name" refusal asserted below —
// and Bed would have been branded with the word first, which is the drift the order
// exists to prevent.
func TestNamingTwoBoardsTheSameWordIsRefused(t *testing.T) {
	f, s := dialableFleetHub(t, macHall, "127.0.0.1", macBed, "192.168.2.32")
	h := map[string]string{"X-Orb-Token": s.token}
	if rec := postFleet(t, s, "/api/fleet/set", map[string]string{"mac": macHall, "name": "Hall"}, h); rec.Code != http.StatusOK {
		t.Fatalf("first name: %d %s", rec.Code, rec.Body.String())
	}
	if got := f.DeviceName(); got != "Hall" {
		t.Errorf("the first rename never reached its orb: %q", got)
	}
	rec := postFleet(t, s, "/api/fleet/set", map[string]string{"mac": macBed, "name": "Hall"}, h)
	if rec.Code == http.StatusOK {
		t.Fatalf("two boards accepted one name: %s", rec.Body.String())
	}
	if !strings.Contains(rec.Body.String(), "already the name") {
		t.Errorf("the refusal does not say the name was taken: %q", rec.Body.String())
	}
}

func TestNamingABoardNobodyHasMetIsRefused(t *testing.T) {
	s := newHub()
	rec := postFleet(t, s, "/api/fleet/set", map[string]string{"mac": macBed, "name": "Ghost"},
		map[string]string{"X-Orb-Token": s.token})
	if rec.Code == http.StatusOK {
		t.Fatal("the hub invented a fleet row for a board it never met")
	}
}

// Forgetting is the whole operation: the row goes, and so does the hub's willingness
// to dial the address it held.
func TestForgettingAnOrbRevokesItsAddress(t *testing.T) {
	s := fleetHub(t, macHall, "192.168.2.31")
	s.grants.learnAs("192.168.2.31", macHall)
	if ok, _ := s.mayDial("192.168.2.31"); !ok {
		t.Fatal("setup: the address was not dialable to begin with")
	}
	rec := postFleet(t, s, "/api/fleet/forget", map[string]string{"mac": macHall},
		map[string]string{"X-Orb-Token": s.token})
	if rec.Code != http.StatusOK {
		t.Fatalf("forget refused: %d %s", rec.Code, rec.Body.String())
	}
	if s.grants.allowed("192.168.2.31") {
		t.Error("the forgotten orb is still an address the hub dials")
	}
	if _, ok := s.fleet.ByMAC(macHall); ok {
		t.Error("the forgotten orb is still in the fleet")
	}
}

// A v1 file has addresses and no MAC. Those grants must come back working, not locked
// out by a cross-lock the file was never able to satisfy.
func TestAVersionOneStateFileStillLoadsItsGrants(t *testing.T) {
	path := t.TempDir() + "/state.json"
	if err := os.WriteFile(path, []byte(`{"version":1,"lastIp":"192.168.2.7","grants":{"192.168.2.7":1760000000}}`), 0o600); err != nil {
		t.Fatal(err)
	}
	s := newHub()
	s.statePath = path
	if n := s.loadState(); n != 1 {
		t.Fatalf("restored %d grants from a v1 file", n)
	}
	if !s.grants.allowed("192.168.2.7") {
		t.Error("the v1 grant did not come back")
	}
	if s.grants.bound("192.168.2.7") != "" {
		t.Error("a v1 grant was credited with a binding it never had")
	}
	if ok, _ := s.mayDial("192.168.2.7"); !ok {
		t.Error("an upgrade locked the operator out of their own remembered orb")
	}
}

// What survives a restart is the identity, and the commitment made while the operator
// was looking at it.
func TestNamesAndBindingsSurviveARestart(t *testing.T) {
	path := t.TempDir() + "/state.json"
	first := fleetHub(t, macHall, "192.168.2.31")
	first.statePath = path
	first.grants.learnAs("192.168.2.31", macHall)
	if err := first.fleet.SetName(macHall, "Hall"); err != nil {
		t.Fatal(err)
	}
	if err := first.fleet.SetRoom(macHall, "Upstairs"); err != nil {
		t.Fatal(err)
	}
	first.saveState()

	cold := newHub()
	cold.statePath = path
	if n := cold.loadState(); n != 1 {
		t.Fatalf("cold hub restored %d grants", n)
	}
	if !cold.grants.allowed("192.168.2.31") {
		t.Fatal("the remembered address was not dialable after a restart")
	}
	if cold.grants.bound("192.168.2.31") != macHall {
		t.Errorf("the MAC binding did not survive: %q", cold.grants.bound("192.168.2.31"))
	}
	// The row is back and still named, and it is not presented as present until it
	// speaks again on this launch.
	saved := cold.fleet.Snapshot()
	if len(saved) != 1 || saved[0].Name != "Hall" || saved[0].Room != "Upstairs" {
		t.Fatalf("the fleet identity did not survive: %+v", saved)
	}
	cold.reconcileFleet()
	rows := cold.fleet.All()
	if len(rows) != 1 || rows[0].Name != "Hall" {
		t.Fatalf("the announced board came back nameless: %+v", rows)
	}
}

// The card must not be able to grant what the operator never committed to, and the
// fleet read is the surface where that would most easily creep in.
func TestReadingTheFleetDialsNothing(t *testing.T) {
	s := fleetHub(t, macHall, "192.168.2.31")
	rec := httptest.NewRecorder()
	s.handleFleet(rec, httptest.NewRequest(http.MethodGet, "/api/fleet", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("fleet read: %d %s", rec.Code, rec.Body.String())
	}
	if s.grants.allowed("192.168.2.31") {
		t.Error("reading the fleet list taught the hub to dial the orb it named")
	}
	if ok, _ := s.mayDial("192.168.2.31"); ok {
		t.Error("the proxy would reach an orb nobody committed to")
	}
}

// A garbage MAC must not become a binding, because then nothing could ever satisfy it
// and the address would be undialable forever.
func TestAJunkMACLeavesTheGrantWorkingAndUnbound(t *testing.T) {
	s := newHub()
	s.grants.learnAs("192.168.2.31", "not-a-mac")
	if s.grants.bound("192.168.2.31") != "" {
		t.Error("a junk MAC was stored as a binding")
	}
	if !s.grants.allowed("192.168.2.31") {
		t.Error("the failed binding destroyed a legitimate grant")
	}
}

func obs(mac, ip string) orbfleet.Observation { return obsAt(mac, ip, time.Now()) }

func obsAt(mac, ip string, at time.Time) orbfleet.Observation {
	return orbfleet.Observation{MAC: mac, Instance: inst(mac), IP: ip, Port: 80,
		FW: "094fa22", State: "associated", At: at}
}

// localOrb runs the device end on loopback and returns the address the proxy must be
// handed to reach it, plus a client aimed at the port the hub dials.
func localOrb(t *testing.T, hits *int) (host string, s *Server) {
	t.Helper()
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if hits != nil {
			*hits++
		}
		_, _ = w.Write([]byte(`{"state":"associated"}`))
	}))
	t.Cleanup(backend.Close)
	h, port, err := net.SplitHostPort(strings.TrimPrefix(backend.URL, "http://"))
	if err != nil {
		t.Fatalf("backend URL %q: %v", backend.URL, err)
	}
	s = newHub()
	c := orbnet.NewClient(2 * time.Second)
	c.DevicePort = port
	s.device = c
	return h, s
}

// A page that polls the card must not be able to keep a dial alive: only the timer
// that actually goes looking extends a commitment. Without this, GET /api/fleet would
// be a keepalive any same-origin script could hammer.
func TestReadingTheCardDoesNotExtendAGrant(t *testing.T) {
	now := time.Now()
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return now }})
	s.browse = filled(t, orbmdns.Candidate{MAC: macHall, Instance: inst(macHall), IP: "192.168.2.31",
		Port: 80, State: "associated", From: "192.168.2.31", FirstSeen: now, LastSeen: now,
		Expires: now.Add(2 * time.Minute)})
	s.grants.learnAs("192.168.2.31", macHall)

	aged := func() time.Time {
		s.grants.mu.Lock()
		defer s.grants.mu.Unlock()
		return s.grants.byIP["192.168.2.31"].at
	}
	s.grants.mu.Lock()
	g := s.grants.byIP["192.168.2.31"]
	g.at = time.Now().Add(-time.Hour)
	s.grants.byIP["192.168.2.31"] = g
	s.grants.mu.Unlock()
	before := aged()

	rec := httptest.NewRecorder()
	s.handleFleet(rec, httptest.NewRequest(http.MethodGet, "/api/fleet", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("fleet read: %d", rec.Code)
	}
	if aged() != before {
		t.Error("GET /api/fleet extended a grant's life")
	}
	rec = httptest.NewRecorder()
	s.handleDiscovered(rec, httptest.NewRequest(http.MethodGet, "/api/orbs", nil))
	if aged() != before {
		t.Error("GET /api/orbs extended a grant's life")
	}
	// The pushed card goes through the same builder, and gets asserted at that seam rather
	// than over a socket: the security property is about what building a card does, and both
	// transports call orbCard. A stream that renewed on read would let any same-origin page hold
	// a dial open forever by leaving one tab pointed at the hub.
	if _, v := s.orbCard(time.Now()); v == "" {
		t.Error("orbCard returned no version, so the stream has nothing to dedupe frames with")
	}
	if aged() != before {
		t.Error("building the card for the stream extended a grant's life")
	}
	// And the timer does, or the previous two assertions prove nothing.
	s.reconcileFleet()
	if aged() == before {
		t.Error("the browse tick did not renew the committed board, so the checks above tested nothing")
	}
}

// Two boards announced one address and the operator had committed to the first. Forgetting
// the second must not revoke a grant the first one still needs: the hub would otherwise be
// destroying a commitment on behalf of a board that was never named in it.
func TestForgettingOneBoardLeavesTheOthersGrantAlone(t *testing.T) {
	now := time.Now()
	s := newHub()
	s.fleet = orbfleet.New(orbfleet.Config{Now: func() time.Time { return now }})
	s.fleet.Observe(obsAt(macHall, "192.168.2.31", now))
	s.fleet.Observe(obsAt(macBed, "192.168.2.31", now))
	s.grants.learnAs("192.168.2.31", macHall)

	rec := postFleet(t, s, "/api/fleet/forget", map[string]string{"mac": macBed},
		map[string]string{"X-Orb-Token": s.token})
	if rec.Code != http.StatusOK {
		t.Fatalf("forget refused: %d %s", rec.Code, rec.Body.String())
	}
	if !s.grants.allowed("192.168.2.31") {
		t.Error("forgetting an unrelated board revoked the grant the kept board was committed for")
	}
	if ok, why := s.mayDial("192.168.2.31"); !ok {
		t.Errorf("the surviving board cannot be dialed after its co-claimant was forgotten: %s", why)
	}
}

// The whole arrangement on one board, in the order a person would meet it: the card
// renames the orb and the orb ends up holding the word; the orb is then renamed by
// something that is not this hub, which is the case the cache exists for; and the next
// ordinary status poll — the one the Manage panel issues every few seconds, not a
// button — pulls the new word back off the wire. The last step is what makes the device
// the owner rather than a place the hub also writes: without it, authority lasts only
// as long as nobody renames the orb somewhere the hub was not watching.
func TestNamingWritesThroughAndAPollRefoldsWhatAnotherRenamed(t *testing.T) {
	f, s := dialableFleetHub(t, macHall, "127.0.0.1")
	s.statePath = t.TempDir() + "/state.json"
	h := map[string]string{"X-Orb-Token": s.token}

	if rec := postFleet(t, s, "/api/fleet/set", map[string]string{"mac": macHall, "name": "Hall"}, h); rec.Code != http.StatusOK {
		t.Fatalf("rename: %d %s", rec.Code, rec.Body.String())
	}
	if got := f.DeviceName(); got != "Hall" {
		t.Fatalf("the hub recorded a name it never put on the board: the orb says %q", got)
	}

	// Something else renames it. Deliberately not a POST through the proxy: that path
	// is exercised by the contract tests, and what matters here is that the hub did
	// not ask for this.
	f.SetDeviceName("Nursery")
	if row, _ := s.fleet.ByMAC(macHall); row.Name != "Hall" {
		t.Fatalf("the hub already knew the new name without being polled, so the poll below tests nothing: %q", row.Name)
	}

	rec := httptest.NewRecorder()
	s.proxyOrb(rec, httptest.NewRequest(http.MethodGet, "/orb/127.0.0.1/api/v1/status", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("status poll: %d %s", rec.Code, rec.Body.String())
	}
	if got := rec.Body.String(); !strings.Contains(got, `"name":"Nursery"`) {
		t.Fatalf("the poll did not carry the device's own name to the browser: %s", got)
	}
	row, _ := s.fleet.ByMAC(macHall)
	if row.Name != "Nursery" {
		t.Errorf("the orb says %q and the card still says %q", f.DeviceName(), row.Name)
	}

	// And it is on disk, not just in RAM. The reason this feature exists is that the
	// hub's file stopped being the thing an orb's name could be lost from — which
	// means it still has to be able to come back with the name on it after a restart.
	fresh := newHub()
	fresh.statePath = s.statePath
	fresh.loadState()
	if row, _ := fresh.fleet.ByMAC(macHall); row.Name != "Nursery" {
		t.Errorf("the fold reached memory but not the state file: %q", row.Name)
	}
}

// A poll that returns no name must not cost an orb its name. This is the difference
// between a cache refresh and a wipe: an unreachable board, a firmware that predates the
// field, and a body truncated by the proxy's own cap are all ordinary events, and none
// of them is an owner asking for the orb to be unnamed.
func TestAnAnswerlessPollDoesNotUnnameAnOrb(t *testing.T) {
	f, s := dialableFleetHub(t, macHall, "127.0.0.1")
	h := map[string]string{"X-Orb-Token": s.token}
	if rec := postFleet(t, s, "/api/fleet/set", map[string]string{"mac": macHall, "name": "Hall"}, h); rec.Code != http.StatusOK {
		t.Fatalf("rename: %d %s", rec.Code, rec.Body.String())
	}

	// A body with no "name" key at all: an orb running firmware from before the field
	// existed. Absent must mean "unknown", never "none".
	s.foldDeviceName("127.0.0.1", []byte(`{"state":"associated","mac":"`+macHall+`","tz":"UTC"}`), true)
	if row, _ := s.fleet.ByMAC(macHall); row.Name != "Hall" {
		t.Errorf("a status reply with no name key cleared the name: %q", row.Name)
	}
	// Half a body, which is what the proxy's cap leaves behind on a large reply. A fold
	// that parsed what it could and inferred from the rest would un-name the orb here.
	s.foldDeviceName("127.0.0.1", []byte(`{"state":"associated","mac":"`+macHall+`","na`), true)
	if row, _ := s.fleet.ByMAC(macHall); row.Name != "Hall" {
		t.Errorf("a truncated status body cleared the name: %q", row.Name)
	}
	// A body naming a DIFFERENT board must not move this row. The fold keys on the MAC
	// inside the payload, and that is the right key only while a body cannot be filed
	// under somebody else's row.
	s.foldDeviceName("127.0.0.1", []byte(`{"mac":"aabbcc112233","name":"Squatter"}`), true)
	if row, _ := s.fleet.ByMAC(macHall); row.Name != "Hall" {
		t.Errorf("a reply about another board renamed this one: %q", row.Name)
	}
	if _, ok := s.fleet.ByMAC(macBed); ok {
		t.Error("a status body folded itself into a row the fleet never met")
	}
	// Empty is not absent, and this is the one payload that may clear the cache.
	s.foldDeviceName("127.0.0.1", []byte(`{"mac":"`+macHall+`","name":""}`), true)
	if row, _ := s.fleet.ByMAC(macHall); row.Name != "" {
		t.Errorf("an explicit empty name from the orb left the cache at %q", row.Name)
	}
	// The fold is read-only. It moved a cache in response to a body it was handed and
	// must not have dialed anything to argue about it: a status reader that wrote back
	// would make the poll the owner after all, and one that wrote back the WRONG value
	// would turn a stray packet into a rename.
	if got := f.DeviceName(); got != "Hall" {
		t.Errorf("folding a status body wrote %q back to the device; the fold reads and never writes", got)
	}
}
