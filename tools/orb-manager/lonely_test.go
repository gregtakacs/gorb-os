package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
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
	// The two the bench found missing: a board revived on another machine, or by
	// the CLI, answered a probe and a status poll as an orb and stayed suppressed
	// here, because every clear site was an act THIS hub had performed.
	if !strings.Contains(readFile(t, "identity.go"), "s.lonely.clear(info.MAC)") {
		t.Error("a console probe that answered as an orb no longer clears the lonely mark")
	}
	if !strings.Contains(readFile(t, "fleet.go"), "s.lonely.clear(probe.MAC)") {
		t.Error("a status poll that answered with its own name no longer clears the lonely mark")
	}
}

// TestLonelyMarkKeepsTheBoardOffTheFleet is the bug the mark exists to kill:
// the sweep attributes the lonely board's cable by descriptor MAC, and a hub
// that re-remembers that sighting puts the forgotten row straight back. The
// mark suppresses the row, fleetKnown reports the chip unknown (New Orb sees a
// blank board to adopt), and clearing it brings the row back.
//
// The two spellings here are the production pair, and the whole point of the
// test: the flash marks the eFuse read the way the console and the USB
// descriptor print it (colon pairs), while the sweep keys a sighting on the
// store's canonical digits. An earlier version of this test stubbed the sighting
// in colon form too, agreed with the bug, and stayed green while every lonely
// board came back nameless on a real bench.
func TestLonelyMarkKeepsTheBoardOffTheFleet(t *testing.T) {
	s := newHub()
	const marked = "44:bd:8d:78:26:18" // how handleLonely formats its eFuse read
	const canonical = "44bd8d782618"   // how the sweep and the store key it
	stubCables(s, cableSighting{port: "/dev/ttyACM0", isOrb: true, mac: canonical})
	now := time.Now()

	s.lonely.mark(marked)
	rows, _ := s.roster(nil, now)
	for _, r := range rows {
		if r.MAC == canonical {
			t.Error("a lonely board rode a cable descriptor back onto the fleet")
		}
	}
	if _, known := s.fleetKnown(canonical); known {
		t.Error("fleetKnown reported a lonely chip as a known orb — the New Orb picker would hide it")
	}
	if _, known := s.fleetKnown(marked); known {
		t.Error("fleetKnown reported a lonely chip as known when it asked in the descriptor's spelling")
	}
	if s.cableOf(canonical, now) != "" {
		t.Error("cableOf keyed a liveness refusal off a lonely board's lead")
	}

	s.lonely.clear(canonical)
	if rows, _ := s.roster(nil, now); len(rows) == 0 {
		t.Error("clearing the mark left the cable sighting invisible — the board is just a port again, not a ban")
	}
}

// TestALonelyMarkAnswersEitherSpelling is the same contract stripped of the
// roster: one mark, reachable from every form a caller arrives in, and cleared
// from any of them too. The announcement path hands over canonical digits and the
// ack paths hand over colon pairs, so a set that only answered one of the two
// would either never suppress or never clear — and the failure would show up as a
// board stuck off the fleet, or a dead row that will not die.
func TestALonelyMarkAnswersEitherSpelling(t *testing.T) {
	l := newLonelyMark()
	l.mark("44:BD:8D:60:80:88")
	for _, form := range []string{
		"44:bd:8d:60:80:88", "44bd8d608088", "44-BD-8D-60-80-88", " 44:bd:8d:60:80:88 ",
	} {
		if !l.has(form) {
			t.Errorf("the mark did not answer a query spelled %q", form)
		}
	}
	if got := l.list(); len(got) != 1 || got[0] != "44bd8d608088" {
		t.Errorf("list() = %v; the state file is written from it, so it must carry one canonical form", got)
	}
	// A malformed MAC keys to nothing, and must not be storable at all: a phantom
	// entry would suppress every board whose descriptor failed to read.
	l.mark("not-a-mac")
	if len(l.list()) != 1 {
		t.Errorf("list() = %v; a malformed MAC must not become a mark", l.list())
	}
	if l.has("not-a-mac") || l.has("") {
		t.Error("a malformed MAC matched a mark it should never reach")
	}
	l.clear("44bd8d608088")
	if l.has("44:bd:8d:60:80:88") {
		t.Error("a canonical clear left the mark standing under the descriptor's spelling")
	}
}

// TestLonelyMarksPersistRoundTrip: the mark is a memory of an act, so a hub
// restart must not hand the sweep a blank slate and re-orb a lonely board. And a
// file written by the hub that keyed marks in the descriptor's spelling — every
// state file on disk from before the canonical key — has to come back as the same
// mark, or upgrading the hub silently un-forgets a board.
func TestLonelyMarksPersistRoundTrip(t *testing.T) {
	s := newHub()
	const mac = "44:bd:8d:78:26:18"
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	s.lonely.mark(mac)
	s.saveState()
	if !strings.Contains(readFile(t, s.statePath), `"44bd8d782618"`) {
		t.Fatal("saveState wrote no lonely set in the store's canonical form")
	}
	s2 := newHub()
	s2.statePath = s.statePath
	s2.loadState()
	if !s2.lonely.has(mac) || !s2.lonely.has("44bd8d782618") {
		t.Error("the lonely set did not come back from the state file")
	}

	s3 := newHub()
	s3.statePath = filepath.Join(t.TempDir(), "state.json")
	if err := os.WriteFile(s3.statePath,
		[]byte(`{"version":3,"grants":{},"lonely":["44:BD:8D:60:80:88"]}`), 0o600); err != nil {
		t.Fatal(err)
	}
	s3.loadState()
	if !s3.lonely.has("44bd8d608088") {
		t.Error("a state file written in the old colon spelling restored as no mark at all")
	}
}

// TestFlashNamesTheCardDirectly: the New Orb dialog writes the name to the chip
// over the lead, and the store has to hear about it in the same breath. A row
// with no name, no address and no version proves nothing to storeProvesOrb, so
// the orbSeen gate stays shut and the board the operator just set up and named
// keeps appearing on the New Orb picker as a blank to adopt — the report this
// pin exists for. Pinned at the call site because the fold is invisible to a
// suite that drives no real serial.
func TestFlashNamesTheCardDirectly(t *testing.T) {
	src := readFile(t, "flash.go")
	if !strings.Contains(src, `s.fleet.NoteDeviceName(macNorm(flashedMAC), req.Name)`) {
		t.Error("a flash-time name no longer reaches the fleet store")
	}
	if i, j := strings.Index(src, "NAME-ACK"), strings.Index(src, "NoteDeviceName(macNorm(flashedMAC)"); j < i {
		t.Error("the store write moved ahead of the ack it depends on")
	}
}

// TestAPollAnswerClearsALonelyMark: the suppression has to end the moment the
// board speaks for itself, wherever it was re-flashed. And it ends only for the
// board that spoke — a status body is whatever the far end chose to say, so a
// clear rides on the same precondition as a rename: the store had to hold a row
// for that MAC already.
func TestAPollAnswerClearsALonelyMark(t *testing.T) {
	s := newHub()
	s.lonely.mark("44:BD:8D:60:80:88")
	if _, err := s.fleet.Confirm("44bd8d608088"); err != nil {
		t.Fatal(err)
	}
	s.foldDeviceName("192.168.2.32", []byte(`{"mac":"44:bd:8d:60:80:88","name":"Pantry"}`), true)
	if s.lonely.has("44bd8d608088") {
		t.Error("a board that answered a poll for itself stayed lonely")
	}
	if row, _ := s.fleet.ByMAC("44bd8d608088"); row.Name != "Pantry" {
		t.Errorf("row name = %q, want Pantry", row.Name)
	}

	// A body claiming a MAC the fleet never met gets nothing, including a clear.
	s.lonely.mark("aabbcc112233")
	s.foldDeviceName("192.168.2.32", []byte(`{"mac":"aabbcc112233","name":"Squatter"}`), true)
	if !s.lonely.has("aabbcc112233") {
		t.Error("a status body about an unknown board cleared that board's lonely mark")
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
