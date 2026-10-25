package orbnet

import (
	"encoding/binary"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// The whole theme contract, end to end, against a double that was written from
// main/net/orb_net.c rather than from this client.
//
// This file exists because of a specific miss. The suite that should have caught a client
// dialing ?name= asserted the ?name= the client already sent, and the live fake orb answered
// it; both sides of the test were copies of the same wrong idea, and every real orb 400s an
// install. A mock that mirrors the client proves the client agrees with itself. So the double
// here keeps its own copy of the 12-hex rule and its own read of the container header, and
// these tests take the ids off the list the device handed back instead of typing them, so a
// client that invented an addressing scheme fails here rather than on glass.

// fakeContainer hands a test the bytes a FORMAT 3 bake produces. It deliberately borrows
// the double's writer rather than writing its own header: the claim under test is that the
// hub's reader parses what a device's door accepts, so the input has to come from the far
// side and not from a copy of this package's own parse. tools/host_orbbundle_test.py pins
// that writer against the C reader byte for byte, which is what makes borrowing it a real
// test rather than an agreement by construction.
func fakeContainer(t *testing.T, name string) []byte {
	t.Helper()
	if len(name) > ThemeNameMax {
		t.Fatalf("fakeContainer name is %d bytes, over the %d the header holds", len(name), ThemeNameMax)
	}
	return orbfake.Container(name)
}

// fakeContainerFormat writes the container stamped f — 2 for the FORMAT 2 file no orb on
// this network can install (the reader island went in 7c0fd4b), 1 for the pre-FORMAT-2
// artifact. The FORMAT word is the first field the firmware checks after the magic, so a
// wrong stamp refuses there and never reaches the header's checksum.
func fakeContainerFormat(t *testing.T, name string, f uint32) []byte {
	b := fakeContainer(t, name)
	binary.LittleEndian.PutUint32(b[4:], f)
	return b
}

func liveOrb(t *testing.T) (*Client, string, *orbfake.Orb) {
	t.Helper()
	f := orbfake.New(
		orbfake.Theme{Name: "aurora", Bytes: 412032, Serial: 7, OK: true, Active: true},
		orbfake.Theme{Name: "carousel", Bytes: 388123, Serial: 9, OK: true},
		orbfake.Theme{Name: "damaged", Why: "art short", Bytes: 12, Serial: 2, OK: false},
	)
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	return NewClient(2 * time.Second), strings.TrimPrefix(srv.URL, "http://"), f
}

// The list is the only place a caller may get an id from, and the verbs have to accept
// exactly what it gave.
func TestThemeContractAgainstTheLiveDouble(t *testing.T) {
	c, ip, f := liveOrb(t)

	l, err := c.ThemeList(ip)
	if err != nil {
		t.Fatalf("ThemeList: %v", err)
	}
	if len(l.Themes) != 3 {
		t.Fatalf("themes = %d, want the three the card holds", len(l.Themes))
	}
	byName := map[string]ThemeEntry{}
	for _, e := range l.Themes {
		byName[e.Name] = e
	}
	if byName["aurora"].ID != "clock-aurora-958c30f62484" {
		t.Errorf("aurora id = %q, want the compound stem theme_stem(clock, name) issues", byName["aurora"].ID)
	}
	if !byName["aurora"].Active || byName["damaged"].OK {
		t.Errorf("load state lost: %+v", l.Themes)
	}

	// Store the listed id. Migration 1's one store door copies the bundle, verifies
	// it, writes the header last and reboots onto the new face — the store IS the
	// theme change — so the ack says reboot:automatic and a fresh read shows the
	// dial moved to carousel with the stored witness landed. A store that left the
	// face alone here would be the old two-press form Migration 1 retired.
	ack, err := c.ThemeStore(ip, byName["carousel"].ID)
	if err != nil {
		t.Fatalf("store carousel: %v", err)
	}
	if !strings.Contains(string(ack), `"state":"storing"`) || !strings.Contains(string(ack), `"reboot":"automatic"`) {
		t.Errorf("store door ack = %s, want state:storing reboot:automatic", ack)
	}
	l2, err := c.ThemeList(ip)
	if err != nil {
		t.Fatalf("re-list: %v", err)
	}
	if l2.Active != "carousel" {
		t.Errorf("active = %q after storing carousel, want the one press to have moved the dial", l2.Active)
	}
	for _, e := range l2.Themes {
		if e.Name == "carousel" && !e.Stored {
			t.Error("the stored witness did not land with the store")
		}
	}

	// Delete by id, and watch the row go.
	if _, err := c.ThemeDelete(ip, byName["damaged"].ID); err != nil {
		t.Fatalf("delete damaged: %v", err)
	}
	l3, err := c.ThemeList(ip)
	if err != nil {
		t.Fatalf("re-list after delete: %v", err)
	}
	if len(l3.Themes) != 2 {
		t.Errorf("themes = %d after delete, want 2", len(l3.Themes))
	}

	// Nothing on this path ever dialed a name.
	for _, call := range f.Calls() {
		if strings.Contains(call, "name=") || strings.Contains(call, "slug=") {
			t.Errorf("the orb was addressed by name: %q", call)
		}
	}
}

// Uploading a FORMAT 3 container lands it under the id its own name hashes to, and the
// name the operator picked for the file on disk is irrelevant. The old client asked for a
// name and the firmware threw it away; this pins the two agreeing that the header decides.
func TestThemeUploadFilesByTheEmbeddedName(t *testing.T) {
	c, ip, _ := liveOrb(t)

	// "2026-spring-final.png" is what a file is called on disk. It is not a theme name.
	body := fakeContainer(t, "Spring Boulevard")
	if _, err := c.ThemeUpload(ip, body, false); err != nil {
		t.Fatalf("upload: %v", err)
	}
	l, err := c.ThemeList(ip)
	if err != nil {
		t.Fatalf("re-list: %v", err)
	}
	var row *ThemeEntry
	for i := range l.Themes {
		if l.Themes[i].Name == "Spring Boulevard" {
			row = &l.Themes[i]
		}
	}
	if row == nil {
		t.Fatalf("the uploaded theme is not on the card: %+v", l.Themes)
	}
	want, _ := ThemeIDOf("clock", "Spring Boulevard")
	if row.ID != want {
		t.Errorf("filed under %q, want theme_id(name) = %q", row.ID, want)
	}
	// ...and the id the hub computed is the id the orb accepts, which is the loop that
	// has to close for a push-then-store to work without a re-list.
	if _, err := c.ThemeStore(ip, row.ID); err != nil {
		t.Errorf("storing the id the hub derived for its own upload: %v", err)
	}
}

// A container the firmware cannot read is the fragmented-identity artifact itself. The hub
// must refuse it before the bytes cross the room, with the reason, because the device would
// answer 422 after the whole payload arrived. FORMAT 2 is the case that matters today: the
// Theme Editor exports it, every current orb refuses it, and until this file moved the hub
// refused FORMAT 3 — so the pair rejected each other's good files and passed through each
// other's dead ones.
func TestThemeUploadRefusesADeadFormatBeforeDialing(t *testing.T) {
	for _, f := range []uint32{1, 2} {
		c, ip, orb := liveOrb(t)
		_, err := c.ThemeUpload(ip, fakeContainerFormat(t, "skeleton", f), false)
		if err == nil {
			t.Fatalf("a FORMAT-%d container was accepted", f)
		}
		if !strings.Contains(err.Error(), "FORMAT") {
			t.Errorf("FORMAT %d: err = %v, want it to name the format it rejected", f, err)
		}
		if !strings.Contains(err.Error(), "FORMAT 3 only") {
			t.Errorf("FORMAT %d: err = %v, want it to say what the orb reads", f, err)
		}
		for _, call := range orb.Calls() {
			if strings.Contains(call, "/api/v1/sd/theme") {
				t.Errorf("a FORMAT-%d file reached the orb anyway: %q", f, call)
			}
		}
	}
}

// The FORMAT 3 accept side of that same loop is TestThemeUploadFilesByTheEmbeddedName
// above: the fixture it uploads is a real FORMAT 3 bundle now, so that test refuses to pass
// unless the hub's reader and the device's door agree on the same bytes.

// Two different themes cannot share a name, and the second one has to be a decision.
// Without the 409 surfacing as an overwrite prompt, a re-bake either silently replaces a
// theme or fails with a sentence that offers no way forward.
func TestThemeUploadDuplicateNameIsADecision(t *testing.T) {
	c, ip, _ := liveOrb(t)

	first := fakeContainer(t, "Tourbillon")
	if _, err := c.ThemeUpload(ip, first, false); err != nil {
		t.Fatalf("first upload: %v", err)
	}
	// Same name, different bytes: the firmware's exact duplicate case. The byte has to
	// live past the 128 B header — touching the name field would make it a different
	// theme, which is the distinction the whole format turns on.
	second := fakeContainer(t, "Tourbillon")
	second[200] ^= 0xFF
	_, err := c.ThemeUpload(ip, second, false)
	de, ok := err.(*DeviceError)
	if !ok || !de.OverwriteConflict() {
		t.Fatalf("err = %v, want the 409 overwrite prompt", err)
	}
	// Re-sending with the flag replaces it, and the row keeps one id and one name.
	if _, err := c.ThemeUpload(ip, second, true); err != nil {
		t.Fatalf("overwrite upload: %v", err)
	}
	l, err := c.ThemeList(ip)
	if err != nil {
		t.Fatalf("re-list: %v", err)
	}
	n := 0
	for _, e := range l.Themes {
		if e.Name == "Tourbillon" {
			n++
		}
	}
	if n != 1 {
		t.Errorf("%d rows named Tourbillon after an overwrite, want one", n)
	}
}

// An empty name is the one container the firmware cannot file at all — theme_id("") would
// give every nameless theme the same card name — so the hub has to refuse it locally.
func TestThemeUploadRefusesAnUnnamedContainer(t *testing.T) {
	c, ip, _ := liveOrb(t)
	if _, err := c.ThemeUpload(ip, fakeContainer(t, ""), false); err == nil {
		t.Fatal("a container with no name in it was accepted")
	}
}

// The store refusal a real orb gives when the handle is not a theme id: the door wants an
// id, and everything else is a 400 with a reason.
func TestThemeStoreRejectsANameWhereItWantsAnID(t *testing.T) {
	c, ip, _ := liveOrb(t)

	for _, want := range []string{"aurora", "../etc", "9B89025CE7A6"} {
		if _, err := c.ThemeStore(ip, want); err == nil {
			t.Errorf("store(%q) should never have left the hub", want)
		}
	}
	// Passing the gate locally and reaching a device that knows the id is the happy path;
	// this proves the double is really the one answering.
	_, err := c.ThemeStore(ip, "clock-x-000000000000")
	if err == nil {
		t.Fatal("store of an id the card does not hold succeeded")
	}
	if !strings.Contains(err.Error(), "no such theme") {
		t.Errorf("err = %v, want the firmware's 404 sentence", err)
	}
}

// The refused-install and torn-store strings key on id, and the card can only point them
// at a row if the id that arrives beside them is the one the row carries.
func TestThemeTryAndStagedKeyOnTheListedID(t *testing.T) {
	c, ip, f := liveOrb(t)

	l, err := c.ThemeList(ip)
	if err != nil {
		t.Fatal(err)
	}
	carouselID := ""
	for _, e := range l.Themes {
		if e.Name == "carousel" {
			carouselID = e.ID
		}
	}
	f.SetTry(carouselID, "the geometry blob was shorter than the counts claim")
	f.SetStaged(carouselID, "the store tore before its header landed")

	after, err := c.ThemeList(ip)
	if err != nil {
		t.Fatal(err)
	}
	if after.TryID != carouselID || after.TryWhy == "" {
		t.Errorf("try_id/try_why = %q/%q, want them to name the row that failed", after.TryID, after.TryWhy)
	}
	if after.StagedID != carouselID || after.StagedWhy == "" {
		t.Errorf("staged_id/staged_why = %q/%q", after.StagedID, after.StagedWhy)
	}
}
