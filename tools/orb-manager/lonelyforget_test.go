package main

import (
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// There is no /api/wipe door on this hub — the page ends orb life with the
// lonely write, and the whole-chip erase lives only in the headless CLI. What
// the old wipe door shared with the lonely door is the forgetting, and these
// tests pin the forgetting itself, one call below any handler, so the contract
// survives on whichever door ever calls it.

// instanceOf is the announcement name the fleet store expects a MAC to carry:
// orb-<last three bytes>, which is what the firmware broadcasts (orb_mdns.c).
func instanceOf(mac string) string {
	h := strings.ReplaceAll(mac, ":", "")
	return "orb-" + strings.ToLower(h[len(h)-6:])
}

func TestEndedOrbIsForgottenEverywhere(t *testing.T) {
	s := newHub()
	const mac = "44:bd:8d:78:26:18"
	const ip = "192.0.2.31"
	const port = "/dev/ttyACM9"

	// Load the board up the way a normal onboarding would have left it: a cable
	// sighting for the identity cache, an announcement for the fleet row and its
	// address, and the grant the proxy needs to dial it.
	s.identities.done(port, esprom.OrbInfo{Port: port, IsOrb: true, MAC: mac, Version: "v1.0.0"})
	s.setBoot(mac, "gorb-2618", false, "", nil)
	if _, err := s.fleet.Observe(orbfleet.Observation{MAC: mac, Instance: "orb-782618", IP: ip, FW: "v1.0.0", State: "ready", Port: 80, At: time.Now()}); err != nil {
		t.Fatalf("seed fleet row: %v", err)
	}
	s.grants.learnAs(ip, mac)
	if _, known := s.fleet.Look(mac); !known {
		t.Fatal("setup: the fleet row is not there to be forgotten")
	}
	if !s.grants.allowed(ip) {
		t.Fatal("setup: the address was never granted")
	}

	s.forgetWipedOrb(port, mac)

	if _, ok := s.identities.get(port); ok {
		t.Error("the hub still caches an identity for a port it just ended")
	}
	if v := s.bootFor(mac); v != nil {
		t.Error("an ended orb still carries a boot verdict /api/watch would trust")
	}
	if _, known := s.fleet.Look(mac); known {
		t.Error("the fleet row survived the ending of the board it described")
	}
	if s.grants.allowed(ip) {
		t.Error("the proxy can still dial the address the ended orb held")
	}
	if s.lonely.has(mac) {
		t.Error("forgetWipedOrb marked the chip lonely; the mark belongs to the lonely door, not the shared un-making")
	}
}

// TestEndWithNoMACStillClearsThePort: the write can fail after naming the chip,
// or run on a hub whose fleet store is absent. The port cache always exists and
// always went stale, so it goes either way — and nothing may panic on the way.
func TestEndWithNoMACStillClearsThePort(t *testing.T) {
	s := newHub()
	const port = "/dev/ttyACM7"
	s.identities.done(port, esprom.OrbInfo{Port: port, IsOrb: true, Version: "v1.0.0"})
	s.fleet = nil
	s.forgetWipedOrb(port, "")
	if _, ok := s.identities.get(port); ok {
		t.Error("the port identity survived an ending that reported no MAC")
	}
}

// TestEndLeavesAnotherBoardsGrantAlone: two rows can hold one address while a
// lease is shared or stale. Revoking it over one ended board would cut off an orb
// that did nothing wrong, so the same ClaimedBy check handleFleetForget uses has
// to hold here too.
func TestEndLeavesAnotherBoardsGrantAlone(t *testing.T) {
	s := newHub()
	const erasedMAC = "44:bd:8d:78:26:18"
	const otherMAC = "44:bd:8d:78:26:19"
	const shared = "192.0.2.40"
	for _, m := range []string{erasedMAC, otherMAC} {
		if _, err := s.fleet.Observe(orbfleet.Observation{MAC: m, Instance: instanceOf(m), IP: shared, FW: "v1.0.0", State: "ready", Port: 80, At: time.Now()}); err != nil {
			t.Fatalf("seed %s: %v", m, err)
		}
	}
	s.grants.learnAs(shared, erasedMAC)

	s.forgetWipedOrb("/dev/ttyACM6", erasedMAC)

	if !s.grants.allowed(shared) {
		t.Error("the grant was revoked although a live board still claims the address")
	}
}

// TestPageIDsAreUnique is the guard the grep-based contract layer never had. A
// second element sharing a live id does not fail to render and does not log
// anything: the page binds by id, and a duplicate binds only the first match in
// document order. The newer control then sits there with no handler while the
// older one starts answering clicks meant for it.
func TestPageIDsAreUnique(t *testing.T) {
	page := readFile(t, "web/index.html")
	seen := map[string]int{}
	for _, m := range regexp.MustCompile(`id="([^"]+)"`).FindAllStringSubmatch(page, -1) {
		seen[m[1]]++
	}
	for id, n := range seen {
		if n > 1 {
			t.Errorf("index.html declares id %q %d times: the second control is inert and the first takes its clicks", id, n)
		}
	}
}

// TestEndDidNotStealTheNVSCheckbox pins the neighbouring control that IS still
// offered: the factory-reset checkbox on a re-flash, which clears NVS without
// ending the board. Deleting the chip erase must leave that binding alone, or
// removing one destructive control breaks an ordinary flash instead.
func TestEndDidNotStealTheNVSCheckbox(t *testing.T) {
	page := readFile(t, "web/index.html")
	js := readAppJS(t)
	// The checkbox moved into the flash modal with the rest of the flash choices and
	// kept its meaning: clear NVS as part of a re-flash, board still an orb after.
	if !strings.Contains(page, `type="checkbox" id="ff-wipe"`) {
		t.Error("the NVS factory-reset checkbox lost its #ff-wipe id")
	}
	if strings.Contains(page, `id="ob-wipe"`) {
		t.Error("the retired #ob-wipe id is back, unwired")
	}
	// It must be read at the moment the flash is issued, not remembered from a
	// previous open, and it must reach the wire as the wipe field the handler takes.
	goBlock := blockContaining(t, js, `$("#ff-go").onclick`)
	if !strings.Contains(goBlock, `cableFlash($("#ff-wipe").checked)`) {
		t.Error("app.js no longer reads the NVS factory-reset checkbox when flashing")
	}
	if !strings.Contains(js, `wipe: !!wipe`) {
		t.Error("cableFlash does not pass the wipe flag through to /api/flash")
	}
	// It is a cable-only control: there is no NVS erase on the wireless path.
	modal := blockOf(t, js, "function openFlashModal")
	if !strings.Contains(modal, `$("#ff-wipe-row").hidden = !port`) {
		t.Error("the NVS checkbox is offered on a wireless-only orb, where nothing can honour it")
	}
}

// eventError is the refusal text of one NDJSON event, whether it came from a
// handler's stream or from requireControl's {ok,error} envelope in front of it.
func eventError(ev map[string]any) string {
	s, _ := ev["error"].(string)
	return s
}
