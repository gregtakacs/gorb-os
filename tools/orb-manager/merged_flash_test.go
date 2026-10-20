package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
)

// The point of accepting a bare .bin is that a person holding a build dir, or a
// CI artifact, can flash an orb without first running this tool to make a bundle.
// These go through the hub's own door with a merged image laid out of the built-in
// bundle's real images, so the assumptions a merged blob has to satisfy — the table
// at 0x8000, the app's descriptor readable, the chip ID at 0x0 — are checked against
// shipped bytes and not against a fixture somebody hand-rolled to agree.

func mergeBinFromBuiltIn(t *testing.T) []byte {
	t.Helper()
	b, err := firmware.BuiltIn()
	if err != nil {
		t.Fatalf("BuiltIn: %v", err)
	}
	// Exactly what merge_bin does: lay the images down in offset order, pad every
	// gap with the 0xFF a blank flash reads as. The built-in plan is already sorted
	// low to high, and its first image is the bootloader at 0x0, so the blob's own
	// header is a real one and the chip gate has something honest to read.
	var merged []byte
	for _, im := range b.Images {
		gap := int(im.Offset) - len(merged)
		if gap < 0 {
			t.Fatalf("image %s at 0x%X overlaps what is already laid down", im.Name, im.Offset)
		}
		merged = append(merged, bytes.Repeat([]byte{0xFF}, gap)...)
		merged = append(merged, im.Data...)
	}
	return merged
}

// The happy path end to end: pick a .bin, and the thing the flasher is handed is
// one verbatim write at the reset vector, with the panel saying so.
func TestMergedBinUploadBecomesAOneImagePlan(t *testing.T) {
	s := bundledServer(t)
	code, resp := uploadBundle(t, s, "orb-merged.bin", mergeBinFromBuiltIn(t))
	if code != http.StatusOK || resp["ok"] != true {
		t.Fatalf("a merged image of the hub's own firmware was rejected: %d %v", code, resp)
	}
	b := s.firmwareBundle()
	if !b.Merged {
		t.Fatal("the hub adopted a merged blob without marking its shape")
	}
	if b.Version == "" {
		t.Error("no version read out of the blob: the panel would show unknown for a build that says what it is")
	}

	_, plan, err := s.planFor("full")
	if err != nil {
		t.Fatalf("planFor: %v", err)
	}
	if len(plan.Images) != 1 || plan.Images[0].Offset != 0x0 {
		t.Fatalf("plan = %+v, want the single 0x0 write a merged image is", plan.Images)
	}
	if !plan.Images[0].Verbatim {
		t.Error("the merged write is not Verbatim: the flasher would digest the whole blob and rewrite the tail partition's last 32 bytes")
	}

	payload, err := s.describeFirmware(nil)
	if err != nil {
		t.Fatalf("describeFirmware: %v", err)
	}
	keys := jsonKeys(t, payload)
	if keys["merged"] != true {
		t.Errorf("/api/build merged = %v, want true so the panel can warn what one write costs", keys["merged"])
	}
}

// While a merged image is loaded the hub has nothing to push at a slot, and the
// Update over Wi-Fi button has to say that rather than fail somewhere out on the
// wire. The refusal happens before the device is contacted, which is what makes it
// testable with no orb on the LAN.
func TestMergedBinClosesTheWiFiDoorWithAReason(t *testing.T) {
	s := bundledServer(t)
	if code, resp := uploadBundle(t, s, "orb-merged.bin", mergeBinFromBuiltIn(t)); code != http.StatusOK {
		t.Fatalf("setup upload: %d %v", code, resp)
	}
	// The dial gate runs first and refuses an address the hub never learned, so
	// grant this one: what the test is after is the firmware-shape refusal behind
	// it, which lands before the hub ever opens a connection to the orb.
	s.grants.learn("203.0.113.7")
	body, _ := json.Marshal(map[string]any{"ip": "203.0.113.7"})
	rec := httptest.NewRecorder()
	s.handleOrbUpdate(rec, httptest.NewRequest(http.MethodPost, "/api/orb/update", strings.NewReader(string(body))))
	if rec.Code == http.StatusOK {
		t.Fatal("a WiFi update went ahead on a firmware with no app slot")
	}
	if !strings.Contains(rec.Body.String(), "whole-flash") {
		t.Errorf("refusal = %s, want it to name the shape and so point at the cable or a .zip", rec.Body.String())
	}
}

// The escape hatch still works: the built-in firmware has to come back after a
// merged pick, or a bad browse would leave this hub only able to flash whole.
func TestMergedBinResetsBackToBuiltIn(t *testing.T) {
	s := bundledServer(t)
	if _, resp := uploadBundle(t, s, "orb-merged.bin", mergeBinFromBuiltIn(t)); resp["ok"] != true {
		t.Fatalf("setup upload: %v", resp)
	}
	rec := httptest.NewRecorder()
	s.handleFirmwareReset(rec, httptest.NewRequest(http.MethodPost, "/api/firmware/reset", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("reset: %d %s", rec.Code, rec.Body.String())
	}
	b := s.firmwareBundle()
	if b.Merged || b.Source != "built-in" {
		t.Errorf("after reset the hub carries %q merged=%v, want the built-in bundle", b.Source, b.Merged)
	}
	if got := b.AppImage(); len(got) == 0 {
		t.Error("the built-in app slot did not come back with the built-in bundle")
	}
}

// The offset-0 gate the merged plan relies on: a hub that adopted a merged blob
// must still refuse to write it to anything but the chip it was built for, and
// that check is the same one every plan passes.
func TestMergedPlanStillCarriesTheChipGate(t *testing.T) {
	s := bundledServer(t)
	if _, resp := uploadBundle(t, s, "orb-merged.bin", mergeBinFromBuiltIn(t)); resp["ok"] != true {
		t.Fatalf("setup upload: %v", resp)
	}
	_, plan, err := s.planFor("full")
	if err != nil {
		t.Fatal(err)
	}
	if err := esprom.CheckPlanChipIDs(plan); err != nil {
		t.Errorf("the hub's merged plan fails its own chip gate: %v", err)
	}
}

// ---- flashing somebody else's firmware ---------------------------------------
// The vendor of this panel ships a factory demo, and putting it back is a real
// thing to want. These take the hub's own shipped blob and rewrite its descriptor
// to name the vendor's project, so the only difference between it and the file the
// tests above upload is the answer to "whose is this".

const vendorProject = "esp32-s3-touch-amoled-brookesia"

// appDescOffsets: the esp_app_desc_t magic, and where its project_name sits.
var appDescMagicBytesLE = []byte{0x32, 0x54, 0xcd, 0xab}

// vendorBlob is the shipped merged image with its app descriptor's project field
// overwritten in place — the same 32 bytes the hub reads its claim from. The field is
// zero-filled first: a copy of a short name would leave the previous build's bytes
// behind, and a name the hub reads with a stray tail is not the name the test claims.
func vendorBlob(t *testing.T) []byte {
	t.Helper()
	raw := mergeBinFromBuiltIn(t)
	i := bytes.Index(raw, appDescMagicBytesLE)
	if i < 0 {
		t.Fatal("no app descriptor in the shipped blob to rewrite")
	}
	field := make([]byte, 32)
	copy(field, vendorProject)
	copy(raw[i+48:i+80], field)
	return raw
}

// mustPayload reads the panel payload for the firmware a hub currently carries.
func mustPayload(t *testing.T, s *Server) map[string]any {
	t.Helper()
	payload, err := s.describeFirmware(nil)
	if err != nil {
		t.Fatalf("describeFirmware: %v", err)
	}
	return jsonKeys(t, payload)
}

// The exact exchange the user's screen could not finish: the hub refuses, names
// what it found, and says this refusal is one an operator can answer.
func TestForeignBlobIsRefusedWithANameAndAWayOut(t *testing.T) {
	s := bundledServer(t)
	code, resp := uploadBundle(t, s, "FactoryOnly.bin", vendorBlob(t))
	if code == http.StatusOK || resp["ok"] == true {
		t.Fatal("the hub adopted another project's firmware unprompted")
	}
	msg, _ := resp["error"].(string)
	// The message has to carry the name it read: an operator can only confirm a
	// thing the hub has shown them.
	if !strings.Contains(msg, vendorProject) {
		t.Errorf("refusal = %q, want it to name the project the blob declares", msg)
	}
	// And the fields the card draws its button from, so the button is not the
	// product of pattern-matching English.
	if resp["foreign_project"] != vendorProject {
		t.Errorf("foreign_project = %v, want %q", resp["foreign_project"], vendorProject)
	}
	if resp["overridable"] != true {
		t.Error("a foreign-project refusal must offer the second send")
	}
	// A refusal changes nothing at all.
	if b := s.firmwareBundle(); b.Merged || b.Source != "built-in" {
		t.Errorf("a refused pick left the hub on %q merged=%v", b.Source, b.Merged)
	}
}

// The second send, where the operator's yes actually lands.
func TestForeignBlobIsAdoptedOnTheSecondSend(t *testing.T) {
	s := bundledServer(t)
	code, resp := uploadBundleForm(t, s, "FactoryOnly.bin", vendorBlob(t), map[string]string{"allow_foreign": "1"})
	if code != http.StatusOK || resp["ok"] != true {
		t.Fatalf("the override was not honoured: %d %v", code, resp)
	}
	b := s.firmwareBundle()
	if !b.Merged || !b.Foreign {
		t.Errorf("Merged, Foreign = %v, %v: the hub must remember it was told to do this", b.Merged, b.Foreign)
	}
	// Flashed verbatim as one write: the vendor's own header and layout, unedited.
	_, plan, err := s.planFor("full")
	if err != nil {
		t.Fatalf("planFor: %v", err)
	}
	if len(plan.Images) != 1 || plan.Images[0].Offset != 0x0 || !plan.Images[0].Verbatim {
		t.Errorf("plan = %+v, want one verbatim write at 0x0", plan.Images)
	}
	// The panel gets the true name, so what the operator sees matches what they
	// confirmed instead of being quietly relabelled as ours.
	keys := mustPayload(t, s)
	if keys["project"] != vendorProject {
		t.Errorf("/api/build project = %v, want the vendor's name", keys["project"])
	}
	if keys["foreign"] != true {
		t.Error("/api/build must carry foreign:true while a vendor image is loaded")
	}
	if keys["merged"] != true {
		t.Error("/api/build lost its shape flag on the foreign path")
	}
}

// The doors that reason from "this is gorb-os firmware" close on a foreign image,
// each with the reason it is really closed.
func TestForeignBlobClosesTheDoorsThatAssumeGorbOS(t *testing.T) {
	s := bundledServer(t)
	if _, resp := uploadBundleForm(t, s, "FactoryOnly.bin", vendorBlob(t), map[string]string{"allow_foreign": "1"}); resp["ok"] != true {
		t.Fatalf("setup: %v", resp)
	}
	// The Wi-Fi update: a merged blob has no slot to push, foreign or not.
	s.grants.learn("203.0.113.9")
	body, _ := json.Marshal(map[string]any{"ip": "203.0.113.9"})
	rec := httptest.NewRecorder()
	s.handleOrbUpdate(rec, httptest.NewRequest(http.MethodPost, "/api/orb/update", strings.NewReader(string(body))))
	if rec.Code == http.StatusOK || !strings.Contains(rec.Body.String(), "whole-flash") {
		t.Errorf("WiFi update on a foreign blob = %d %s, want the shape refusal", rec.Code, rec.Body.String())
	}
	// The app-only reflash, which is what a theme push and a bench app flash ride on.
	if _, err := s.firmwareBundle().AppPlan(); err == nil {
		t.Error("an app-only plan came out of a whole-flash image")
	}
	// And the way back is the door it always was.
	reset := httptest.NewRecorder()
	s.handleFirmwareReset(reset, httptest.NewRequest(http.MethodPost, "/api/firmware/reset", nil))
	if reset.Code != http.StatusOK {
		t.Fatalf("reset after a foreign pick: %d %s", reset.Code, reset.Body.String())
	}
	if b := s.firmwareBundle(); b.Foreign || b.Merged || b.Source != "built-in" {
		t.Errorf("after reset the hub carries %q merged=%v foreign=%v", b.Source, b.Merged, b.Foreign)
	}
}

// The flag is exactly the word the card sends. An empty field, a typo or "0" all
// leave the gate shut, so a stray form value cannot be the thing that admitted a
// vendor image to a hub nobody meant to point at one.
func TestForeignOverrideNeedsTheExactYes(t *testing.T) {
	s := bundledServer(t)
	for _, v := range []string{"", "0", "yes", "true", " 1 "} {
		if _, resp := uploadBundleForm(t, s, "FactoryOnly.bin", vendorBlob(t), map[string]string{"allow_foreign": v}); resp["ok"] == true {
			t.Errorf("allow_foreign=%q was taken for a yes", v)
		}
	}
}

// A refusal the operator cannot overrule must not carry a button. A blob for
// another chip is the case: the second send would refuse it identically.
func TestForeignRefusalOnAnotherChipOffersNoButton(t *testing.T) {
	s := bundledServer(t)
	raw := vendorBlob(t)
	// The image header's chip ID at byte 12 of the blob's own header at 0x0: 13 is
	// ESP32-C6, which this board is not.
	raw[12] = 13
	_, resp := uploadBundle(t, s, "c6.bin", raw)
	if resp["ok"] == true {
		t.Fatal("an image for another chip was adopted")
	}
	if resp["overridable"] != nil {
		t.Errorf("refusal %v offers a retry; a chip mismatch is not a decision to overrule", resp)
	}
}
