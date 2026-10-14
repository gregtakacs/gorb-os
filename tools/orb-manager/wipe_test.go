package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfleet"
)

// The wipe route is the most destructive thing this hub can be asked to do, so
// these tests are about the door in front of it: the credential, the typed
// confirm, the per-port claim, and what the hub forgets afterwards. The erase
// itself is pinned against an emulated ROM in internal/esprom/wipe_test.go.

// postWipe calls /api/wipe the way the page does and returns the NDJSON events it
// streamed. The handler answers 200 even when it refuses, because the refusal is
// an event in the stream the client is already reading — the same contract
// /api/flash has, and the same one the client's streamFlash parses.
func postWipe(t *testing.T, s *Server, payload map[string]any, token string) ([]map[string]any, int) {
	t.Helper()
	body, err := json.Marshal(payload)
	if err != nil {
		t.Fatal(err)
	}
	mux := http.NewServeMux()
	s.routes(mux)
	req, err := http.NewRequest(http.MethodPost, "/api/wipe", strings.NewReader(string(body)))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/json")
	// No Origin header at all: that is the scripted-agent case security.go admits,
	// and it is admitted only on the strength of the token this call may or may not
	// carry. A hostile Origin would be refused by the guard before the handler saw
	// the request, which is TestEveryMutatingRouteRefusesAnUncredentialedPOST's job.
	if token != "" {
		req.Header.Set(tokenHeader, token)
	}
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	var events []map[string]any
	for _, line := range strings.Split(strings.TrimSpace(rec.Body.String()), "\n") {
		if line == "" {
			continue
		}
		var ev map[string]any
		if err := json.Unmarshal([]byte(line), &ev); err != nil {
			t.Fatalf("handler emitted a non-JSON line %q — the client reads NDJSON", line)
		}
		events = append(events, ev)
	}
	return events, rec.Code
}

// eventError is the refusal text of one event, whether it came from the handler's
// NDJSON stream or from requireControl's {ok,error} envelope in front of it.
func eventError(ev map[string]any) string {
	s, _ := ev["error"].(string)
	return s
}

// instanceOf is the announcement name the fleet store expects a MAC to carry:
// orb-<last three bytes>, which is what the firmware broadcasts (orb_mdns.c).
func instanceOf(mac string) string {
	h := strings.ReplaceAll(mac, ":", "")
	return "orb-" + strings.ToLower(h[len(h)-6:])
}

// TestWipeRouteIsRegisteredAndGated: the route exists, and it is behind the same
// credential as the flash route. A 404 here would reach the client as a page, not
// an error event, which is the failure a button cannot report.
func TestWipeRouteIsRegisteredAndGated(t *testing.T) {
	s := newHub()
	_, code := postWipe(t, s, map[string]any{"port": "/dev/ttyACM9", "confirm": wipeConfirmWord}, "")
	if code != http.StatusForbidden {
		t.Errorf("no token: status = %d, want 403 from requireControl", code)
	}
	if live := s.ops.running(); len(live) != 0 {
		t.Errorf("a refused wipe claimed a port: %+v", live)
	}
	src := readFile(t, "main.go")
	if !strings.Contains(src, `"/api/wipe"`) || !strings.Contains(src, "handleWipe") {
		t.Error("/api/wipe is not registered against handleWipe in main.go")
	}
}

// TestWipeRequiresTheTypedConfirm is the difference between a destructive route
// and an accidental one. Everything else can be right — token, origin, port — and
// it still must carry the word, in caps, alone.
func TestWipeRequiresTheTypedConfirm(t *testing.T) {
	s := newHub()
	for _, c := range []string{"", "wipe", "Wipe", "WIPE everything", "  "} {
		ev, code := postWipe(t, s, map[string]any{"port": "/dev/ttyACM9", "confirm": c}, s.token)
		if code != http.StatusOK {
			t.Fatalf("confirm %q: status %d, want 200 with the refusal in the stream", c, code)
		}
		if len(ev) == 0 || eventError(ev[0]) == "" {
			t.Errorf("confirm %q was accepted — the handler must refuse it", c)
			continue
		}
		if !strings.Contains(eventError(ev[0]), wipeConfirmWord) {
			t.Errorf("confirm %q refused without saying what to send instead: %q", c, eventError(ev[0]))
		}
	}
	if live := s.ops.running(); len(live) != 0 {
		t.Errorf("an unconfirmed wipe reached the port registry: %+v", live)
	}
}

// TestWipeRefusesAPortMidFlash proves the erase joins the same per-port registry
// as every other operation: two writes on one cable corrupt each other, and a
// 16 MB erase is the longest write there is.
func TestWipeRefusesAPortMidFlash(t *testing.T) {
	s := newHub()
	const port = "/dev/ttyACM9"
	job, _, err := s.ops.start(nil, port, opFlash, 0)
	if err != nil {
		t.Fatalf("claim port: %v", err)
	}
	defer s.ops.finish(job)
	ev, _ := postWipe(t, s, map[string]any{"port": port, "confirm": wipeConfirmWord}, s.token)
	if len(ev) == 0 || eventError(ev[0]) == "" {
		t.Fatal("a wipe started on a port mid-flash; the registry is not consulted")
	}
	if !strings.Contains(eventError(ev[0]), "busy") {
		t.Errorf("the refusal does not name the clash: %q", eventError(ev[0]))
	}
}

// TestWipeNeedsAPort names the mistake rather than erasing a guess.
func TestWipeNeedsAPort(t *testing.T) {
	s := newHub()
	ev, code := postWipe(t, s, map[string]any{"confirm": wipeConfirmWord}, s.token)
	if code != http.StatusOK {
		t.Fatalf("status = %d, want 200 with the failure in the stream", code)
	}
	if len(ev) != 1 || !strings.Contains(eventError(ev[0]), "port") {
		t.Errorf("port-less wipe answered %v, want one error naming the port", ev)
	}
}

// TestWipedOrbIsForgottenEverywhere covers the hub's half of "as if the firmware
// was never on it". Everything the hub cached about that board described bytes
// that no longer exist, and a card that outlives the firmware on the chip is a
// lie with a name on it: the fleet row, the name earned by that firmware, the
// address the proxy was willing to dial, and the boot verdict /api/watch reads.
func TestWipedOrbIsForgottenEverywhere(t *testing.T) {
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
		t.Error("the hub still caches an identity for a port it just erased")
	}
	if v := s.bootFor(mac); v != nil {
		t.Error("a wiped orb still carries a boot verdict /api/watch would trust")
	}
	if _, known := s.fleet.Look(mac); known {
		t.Error("the fleet row survived the wipe of the board it described")
	}
	if s.grants.allowed(ip) {
		t.Error("the proxy can still dial the address the erased orb held")
	}
}

// TestWipeWithNoMACStillClearsThePort: the erase can fail after naming the chip,
// or run on a hub whose fleet store is absent. The port cache always exists and
// always went stale, so it goes either way — and nothing may panic on the way.
func TestWipeWithNoMACStillClearsThePort(t *testing.T) {
	s := newHub()
	const port = "/dev/ttyACM7"
	s.identities.done(port, esprom.OrbInfo{Port: port, IsOrb: true, Version: "v1.0.0"})
	s.fleet = nil
	s.forgetWipedOrb(port, "")
	if _, ok := s.identities.get(port); ok {
		t.Error("the port identity survived a wipe that reported no MAC")
	}
}

// TestWipeLeavesAnotherBoardsGrantAlone: two rows can hold one address while a
// lease is shared or stale. Revoking it over one erased board would cut off an orb
// that did nothing wrong, so the same ClaimedBy check handleFleetForget uses has
// to hold here too.
func TestWipeLeavesAnotherBoardsGrantAlone(t *testing.T) {
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

// TestWipeClaimsItsPortUnderItsOwnKind: the UI's live-operation line and the
// cancel door both address an operation by kind and port, so an erase that
// registered itself as a flash would be reported as something it is not.
func TestWipeClaimsItsPortUnderItsOwnKind(t *testing.T) {
	if string(opWipe) != "wipe" {
		t.Errorf("opWipe = %q, want %q — /api/ops and the UI show this string", opWipe, "wipe")
	}
	s := newHub()
	job, _, err := s.ops.start(nil, "/dev/ttyACM8", opWipe, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer s.ops.finish(job)
	if kind, busy := s.ops.busy(orbbind.CanonicalPortPath("/dev/ttyACM8")); !busy || kind != opWipe {
		t.Errorf("registry says %q/%v, want wipe/true", kind, busy)
	}
	if _, ok := s.ops.cancel("/dev/ttyACM8"); !ok {
		t.Error("an in-flight wipe cannot be cancelled")
	}
}

// TestWipeConfirmWordIsOneConstantOnBothSides: the page will not arm the button
// until the typed text matches, and the hub will not act until it matches too. Two
// spellings of that word is the failure where the button arms and every click then
// comes back refused, so both sides are pinned to the handler's own value.
func TestWipeConfirmWordIsOneConstantOnBothSides(t *testing.T) {
	if wipeConfirmWord != "WIPE" {
		t.Errorf("wipeConfirmWord = %q; the UI wording and this test assume WIPE", wipeConfirmWord)
	}
	src := readAppJS(t)
	if !strings.Contains(src, `!== "WIPE"`) {
		t.Error("app.js no longer gates the erase button on the exact word WIPE")
	}
	if !strings.Contains(src, `confirm: "WIPE"`) {
		t.Error("app.js no longer sends confirm:WIPE with the erase request")
	}
}

// TestWipeControlsExistInThePage is the blank-card failure mode this repo already
// guards elsewhere: app.js binds handlers by id, and an id that is not in the
// served page throws on the first line of the wiring and silently kills every
// other onclick that follows it in the same block.
func TestWipeControlsExistInThePage(t *testing.T) {
	page := readFile(t, "web/index.html")
	for _, id := range []string{
		"ob-chipwipe",         // the button that arms the confirmation
		"ob-chipwipe-confirm", // the confirmation row itself
		"ob-chipwipe-word",    // the text input
		"ob-chipwipe-go",      // the button that actually erases
		"ob-chipwipe-no",      // backing out
		"ob-chipwipe-port",    // names the cable about to be emptied
		"ob-chipwipe-hint",    // says what is about to happen
	} {
		if !strings.Contains(page, `id="`+id+`"`) {
			t.Errorf("app.js wires #%s, but index.html has no such id", id)
		}
	}
	// The destructive control must not be reachable without the typed word, so it
	// ships disabled rather than enabled-and-checked-later.
	if !strings.Contains(page, `id="ob-chipwipe-go" class="danger" disabled`) {
		t.Error("the erase button must start disabled")
	}
}

// TestPageIDsAreUnique is the guard the grep-based contract layer never had. A
// second element sharing a live id does not fail to render and does not log
// anything: the page binds by id, and a duplicate binds only the first match in
// document order. The newer control then sits there with no handler while the
// older one starts answering clicks meant for it. That is how the chip-erase
// button shipped for one full round of bench testing, and every existing pin
// passed, because each one only asked whether the id was present.
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

// TestChipWipeDidNotStealTheNVSCheckbox pins the other half of the same accident.
// The page carried a factory-reset checkbox at #ob-wipe long before the chip
// erase existed, and a normal flash still reads it to decide whether to clear
// NVS. Renaming the erase must leave that binding alone, or fixing the wipe
// breaks an ordinary flash instead.
func TestChipWipeDidNotStealTheNVSCheckbox(t *testing.T) {
	page := readFile(t, "web/index.html")
	js := readAppJS(t)
	if !strings.Contains(page, `type="checkbox" id="ob-wipe"`) {
		t.Error("the NVS factory-reset checkbox lost its #ob-wipe id")
	}
	if strings.Contains(page, `id="ob-wipe" class="danger"`) {
		t.Error("the chip-erase button reused the NVS checkbox id #ob-wipe")
	}
	if !strings.Contains(js, `$("#ob-wipe").checked`) {
		t.Error("app.js no longer reads the NVS factory-reset checkbox when flashing")
	}
}
