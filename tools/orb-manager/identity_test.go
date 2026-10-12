package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// A tty name is a lease the OS hands out, not an identity. These pin the way that
// fact bites on a bench: the same socket carrying a different orb.

// hubWithCables builds a hub whose view of which orb sits behind which port is
// scripted, so a re-plug can be replayed without a hand on the cable.
func hubWithCables(serial map[string]string) *Server {
	s := newHub()
	s.identities.usbMAC = func(port string) string { return serial[port] }
	return s
}

// Orb A is identified on /dev/ttyACM0. It is unplugged and orb B takes the same
// socket. The hub must say nothing about B, not repeat A's version: the picker label
// is a claim about the firmware an operator is about to flash over.
func TestIdentityCacheIsKeyedByCableNotPortName(t *testing.T) {
	const port = "/dev/ttyACM0"
	serials := map[string]string{port: "44:bd:8d:60:80:88"}
	s := hubWithCables(serials)

	s.identities.done(port, esprom.OrbInfo{Port: port, IsOrb: true, MAC: serials[port],
		Project: "gorb-os", Version: "aaa1111", ProbedAt: "earlier"})
	if _, ok := s.identities.get(port); !ok {
		t.Fatal("the answer was refused while the same orb is still in the socket")
	}

	serials[port] = "44:bd:8d:60:80:99" // the cable was swapped; the tty name did not change

	if _, ok := s.identities.get(port); ok {
		t.Error("a different orb was reported as the one that was identified")
	}
	if got := s.identities.all(port); len(got) != 0 {
		t.Errorf("/api/ports would still paint the stale version for %v: %+v", port, got)
	}
}

// The cached answer must still be surfaced when it IS the same cable, complete with
// the fields the card renders — otherwise the guard above is achieved by simply
// never caching anything.
func TestPortsEndpointStillCarriesAFreshIdentity(t *testing.T) {
	s := newHub()
	s.identities.usbMAC = func(string) string { return "" } // unattributable host
	s.identities.done("/dev/ttyACM1", esprom.OrbInfo{Port: "/dev/ttyACM1", IsOrb: true,
		MAC: "44:bd:8d:60:80:aa", Project: "gorb-os", Version: "bbb2222", ElfSHA256: "abcd"})
	if _, ok := s.identities.get("/dev/ttyACM1"); !ok {
		t.Fatal("an answer from a host that cannot attribute the cable was refused; that is the cache doing no work")
	}
	rec := httptest.NewRecorder()
	s.handlePorts(rec, httptest.NewRequest(http.MethodGet, "/api/ports", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "\"ports\"") {
		t.Errorf("payload lost its object wrapper: %s", rec.Body)
	}
}

// An identity is a reboot behind the operator, and an orb can change underneath a
// cached answer (an OTA, a flash from orbflash on the bench). The TTL is what stops
// the card describing last hour's orb as this one.
func TestIdentityExpires(t *testing.T) {
	const port = "/dev/ttyACM3"
	s := hubWithCables(map[string]string{port: "44:bd:8d:00:00:01"})
	s.identities.done(port, esprom.OrbInfo{Port: port, IsOrb: true, MAC: "44:bd:8d:00:00:01", Version: "old"})

	s.identities.mu.Lock()
	e := s.identities.byPort[port]
	e.at = time.Now().Add(-identityTTL - time.Minute)
	s.identities.byPort[port] = e
	s.identities.mu.Unlock()

	if _, ok := s.identities.get(port); ok {
		t.Error("an answer older than the TTL was still served")
	}
	if len(s.identities.all(port)) != 0 {
		t.Error("the expired answer was still listed to the picker")
	}
}

// Unplugging a cable must clear its answer, so the next device handed that tty name
// starts with no claim attached even before the serial comparison runs.
func TestIdentityDropsPortsThatVanished(t *testing.T) {
	s := hubWithCables(map[string]string{"/dev/ttyACM7": "44:bd:8d:00:00:07"})
	s.identities.done("/dev/ttyACM7", esprom.OrbInfo{Port: "/dev/ttyACM7", IsOrb: true, MAC: "44:bd:8d:00:00:07"})
	if len(s.identities.all()) != 1 {
		t.Error("an empty enumeration erased state; a listing that saw no ports must not be read as proof they are gone")
	}
	if got := s.identities.all("/dev/ttyACM0"); len(got) != 0 {
		t.Errorf("an answer for an unplugged port was still listed: %+v", got)
	}
	if _, ok := s.identities.get("/dev/ttyACM7"); ok {
		t.Error("the vanished port kept its answer after being evicted from the listing")
	}
}

// The eviction rule lives in the cache, but only the handler's call makes it real:
// /api/ports must pass the enumeration in, or an unplugged cable's answer outlives
// the cable.
func TestPortsEndpointArmsTheEviction(t *testing.T) {
	src := readFile(t, "identity.go")
	if !strings.Contains(src, "s.identities.all(ports...)") {
		t.Error("handlePorts must pass the live port list to identities.all, or vanished ports never get evicted")
	}
	if !strings.Contains(src, "who: c.serialOf(port)") {
		t.Error("identity answers must be tagged with the cable that produced them")
	}
}
