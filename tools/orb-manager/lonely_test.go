package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// The lonely door is a destructive write with a joke inside, so these tests
// pin the door and the contract, and the ONE claim of the payload: the
// embedded image carries its sentence. Everything visual is the firmware's
// bench problem; everything wire-level is not.

// postLonely is postWipe aimed at /api/lonely — same NDJSON answer, same
// token conventions, same scripted-agent (no Origin) case.
func postLonely(t *testing.T, s *Server, payload map[string]any, token string) ([]map[string]any, int) {
	t.Helper()
	body, err := json.Marshal(payload)
	if err != nil {
		t.Fatal(err)
	}
	mux := http.NewServeMux()
	s.routes(mux)
	req, err := http.NewRequest(http.MethodPost, "/api/lonely", strings.NewReader(string(body)))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/json")
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

// TestLonelyRouteIsRegistered pins the wiring a silent 404 would otherwise
// hide forever: the route exists, it runs the handler, and the guard owns it.
func TestLonelyRouteIsRegistered(t *testing.T) {
	s := newHub()
	_, code := postLonely(t, s, map[string]any{"port": "/dev/ttyACM9", "confirm": lonelyConfirmWord}, "")
	if code != http.StatusForbidden {
		t.Errorf("no token: status = %d, want 403 from requireControl", code)
	}
	if live := s.ops.running(); len(live) != 0 {
		t.Errorf("a refused lonely write claimed a port: %+v", live)
	}
	src := readFile(t, "main.go")
	if !strings.Contains(src, `"/api/lonely"`) || !strings.Contains(src, "handleLonely") {
		t.Error("/api/lonely is not registered against handleLonely in main.go")
	}
}

// TestLonelyRequiresTheTypedConfirm is the wipe test again, because the act
// deserves the same discipline: any other part of the request may be perfect
// and the word still has to arrive, alone, in caps.
func TestLonelyRequiresTheTypedConfirm(t *testing.T) {
	s := newHub()
	for _, c := range []string{"", "lonely", "Lonely", "LONELY forever", "WIPE", "  "} {
		ev, code := postLonely(t, s, map[string]any{"port": "/dev/ttyACM9", "confirm": c}, s.token)
		if code != http.StatusOK {
			t.Fatalf("confirm %q: status %d, want 200 with the refusal in the stream", c, code)
		}
		if len(ev) == 0 || eventError(ev[0]) == "" {
			t.Errorf("confirm %q was accepted — the handler must refuse it", c)
			continue
		}
		if !strings.Contains(eventError(ev[0]), lonelyConfirmWord) {
			t.Errorf("confirm %q refused without saying what to send instead: %q", c, eventError(ev[0]))
		}
	}
	if live := s.ops.running(); len(live) != 0 {
		t.Errorf("an unconfirmed lonely write reached the port registry: %+v", live)
	}
}

// TestLonelyWordTrimsButPortDoesNot is the accept half of the word gate: with
// "LONELY " (paste-trailing space) the handler must look past the word and
// fail on the hardware it cannot see, proving the word itself passed.
func TestLonelyWordTrimsButPortDoesNot(t *testing.T) {
	s := newHub()
	ev, _ := postLonely(t, s, map[string]any{"port": "/dev/ttyACM9", "confirm": lonelyConfirmWord + " "}, s.token)
	if len(ev) == 0 || eventError(ev[0]) == "" {
		t.Fatal("a nonexistent port accepted the write — the open must fail loudly")
	}
	if strings.Contains(eventError(ev[0]), lonelyConfirmWord) {
		t.Errorf("the word gate refused a valid word: %q", eventError(ev[0]))
	}
}

// TestLonelyRefusesAPortMidFlash: the lonely write is a full ROM conversation
// and joins the registry exactly like flash and wipe.
func TestLonelyRefusesAPortMidFlash(t *testing.T) {
	s := newHub()
	const port = "/dev/ttyACM9"
	job, _, err := s.ops.start(nil, port, opFlash, 0)
	if err != nil {
		t.Fatalf("claim port: %v", err)
	}
	defer s.ops.finish(job)
	ev, _ := postLonely(t, s, map[string]any{"port": port, "confirm": lonelyConfirmWord}, s.token)
	if len(ev) == 0 || eventError(ev[0]) == "" {
		t.Fatal("a lonely write started on a port mid-flash; the registry is not consulted")
	}
	if !strings.Contains(eventError(ev[0]), "busy") {
		t.Errorf("the refusal does not name the clash: %q", eventError(ev[0]))
	}
}

// TestLonelyPageHasTheDoorAndTheWord pins the UI against a rename that would
// orphan the handler: button, word input, confirm on the face, and the stream
// call the button makes.
func TestLonelyPageHasTheDoorAndTheWord(t *testing.T) {
	page := hubIndexHTML(t)
	for _, want := range []string{`id="ob-lonely"`, `id="ob-lonely-word"`, `id="ob-lonely-go"`, "<b>LONELY</b>"} {
		if !strings.Contains(page, want) {
			t.Errorf("index.html has no %s — the lonely door lost a part of itself", want)
		}
	}
	app := readAppJS(t)
	if !strings.Contains(app, `"/api/lonely"`) || !strings.Contains(app, `confirm: "LONELY"`) {
		t.Error(`app.js never posts /api/lonely with confirm: "LONELY"`)
	}
	if !strings.Contains(app, `ob-lonelyrow`) {
		t.Error("app.js never shows or hides #ob-lonelyrow, so the door sits on unwired orbs")
	}
}

// TestLonelyImageCarriesItsMessage reads the embedded bytes, not the source:
// one sentence is the whole product, and the file must start with the ESP
// image magic — a real IDF app, not an empty placeholder or a stray zip.
func TestLonelyImageCarriesItsMessage(t *testing.T) {
	if len(lonelyImage) < 4096 {
		t.Fatalf("lonely.bin is %d bytes — too small to be a built app image", len(lonelyImage))
	}
	if lonelyImage[0] != 0xE9 {
		t.Errorf("lonely.bin starts %#02x, want 0xE9 (ESP image magic)", lonelyImage[0])
	}
	for _, want := range []string{"I used to be an Orb,", "now I'm just lonely.", "Flash me!"} {
		if !strings.Contains(string(lonelyImage), want) {
			t.Errorf("lonely.bin does not contain %q — the message is the product", want)
		}
	}
}

// TestLonelyErasesWhatTheOwnerStored pins the wiring the door exists for since
// the settings complaint: the lonely session must carry the NVS-region erase,
// resolved from the very partition table the plan writes — timezone, name and
// every credential go with the orb life, the same rule factory-reset flashes
// obey. A test hub cannot reach hardware, so the wiring itself is the contract.
func TestLonelyErasesWhatTheOwnerStored(t *testing.T) {
	src := readFile(t, "lonely.go")
	if !strings.Contains(src, "esprom.NvsRegions(bundle.PartitionTable())") {
		t.Error("the lonely write no longer resolves its erase targets from the bundle's partition table")
	}
	if !strings.Contains(src, "Wipe: nvs") {
		t.Error("the lonely session no longer carries the NVS erase")
	}
}

// TestLonelyMarksTheBoardAndEvidenceClearsIt: the forget alone loses to the next
// cable sweep — the lonely board still enumerates the same descriptor MAC — so
// the handler must mark the chip, and the doors that prove orb life again (an
// orb flash, a serial ack, an announcement) must clear it. Pinned at each call
// site because each is a one-line wiring a refactor can drop silently.
func TestLonelyMarksTheBoardAndEvidenceClearsIt(t *testing.T) {
	if !strings.Contains(readFile(t, "lonely.go"), "s.lonely.mark(lonelyMAC)") {
		t.Error("handleLonely no longer marks the board it left lonely")
	}
	if !strings.Contains(readFile(t, "flash.go"), "s.lonely.clear(macNorm(flashedMAC))") {
		t.Error("a successful orb flash no longer clears the lonely mark")
	}
	if !strings.Contains(readFile(t, "orbcmd.go"), "s.lonely.clear(macNorm(req.MAC))") {
		t.Error("a serial ack no longer clears the lonely mark")
	}
	if !strings.Contains(readFile(t, "fleet.go"), "s.lonely.clear(c.MAC)") {
		t.Error("an announcement no longer clears the lonely mark")
	}
}

// TestLonelyMarkKeepsTheBoardOffTheFleet is the bug the mark exists to kill:
// the sweep attributes the lonely board's cable by descriptor MAC, and a hub
// that re-remembers that sighting puts the forgotten row straight back. The
// mark suppresses the row, fleetKnown reports the chip unknown (New Orb sees a
// blank board to adopt), and clearing it brings the row back.
func TestLonelyMarkKeepsTheBoardOffTheFleet(t *testing.T) {
	s := newHub()
	const mac = "44:bd:8d:78:26:18"
	stubCables(s, cableSighting{port: "/dev/ttyACM0", isOrb: true, mac: mac})
	now := time.Now()

	s.lonely.mark(mac)
	rows, _ := s.roster(nil, now)
	for _, r := range rows {
		if r.MAC == mac {
			t.Error("a lonely board rode a cable descriptor back onto the fleet")
		}
	}
	if _, known := s.fleetKnown(mac); known {
		t.Error("fleetKnown reported a lonely chip as a known orb — the New Orb picker would hide it")
	}
	if s.cableOf(mac, now) != "" {
		t.Error("cableOf keyed a liveness refusal off a lonely board's lead")
	}

	s.lonely.clear(mac)
	if rows, _ := s.roster(nil, now); len(rows) == 0 {
		t.Error("clearing the mark left the cable sighting invisible — the board is just a port again, not a ban")
	}
}

// TestLonelyMarksPersistRoundTrip: the mark is a memory of an act, so a hub
// restart must not hand the sweep a blank slate and re-orb a lonely board.
func TestLonelyMarksPersistRoundTrip(t *testing.T) {
	s := newHub()
	const mac = "44:bd:8d:78:26:18"
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	s.lonely.mark(mac)
	s.saveState()
	if !strings.Contains(readFile(t, s.statePath), `"`+mac+`"`) {
		t.Fatal("saveState wrote no lonely set")
	}
	s2 := newHub()
	s2.statePath = s.statePath
	s2.loadState()
	if !s2.lonely.has(mac) {
		t.Error("the lonely set did not come back from the state file")
	}
}

// TestFactoryResetNamesTheCardTheOrb: the board a hub-commanded RESET ALL
// leaves behind answers to the default word — the firmware seeds "The Orb"
// into the fresh NVS on a clean erase — and the hub writes the same word to
// the card at the ack, so the row reads it even before the board answers a
// poll again. Pinned at the ack site because a refactor of the write-back
// block drops it silently.
func TestFactoryResetNamesTheCardTheOrb(t *testing.T) {
	if defaultOrbName != "The Orb" {
		t.Errorf("defaultOrbName = %q; the sentence the operator asked for is The Orb", defaultOrbName)
	}
	src := readFile(t, "orbcmd.go")
	if !strings.Contains(src, "NoteDeviceName(macNorm(req.MAC), defaultOrbName)") {
		t.Error("the reset ack no longer renames the card to the default")
	}
}
