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

// fakeContainer writes a FORMAT-2 .orbtheme header holding name, enough of a container for
// the identity gates. The header's own crc is not checked by the identity path, so it is
// left as written; tools/host_sd_test.py pins the real packer against the C reader.
func fakeContainer(t *testing.T, name string) []byte {
	t.Helper()
	if len(name) > ThemeNameMax {
		t.Fatalf("fakeContainer name is %d bytes, over the %d the header holds", len(name), ThemeNameMax)
	}
	b := make([]byte, 1024)
	binary.LittleEndian.PutUint32(b[0:], 0x474F5254) // 'TROR'
	binary.LittleEndian.PutUint32(b[4:], 2)          // FORMAT 2
	binary.LittleEndian.PutUint32(b[8:], 1)          // GEOM_ABI
	copy(b[14*4:], name)
	return b
}

// fakeContainerFormat1 writes the pre-FORMAT-2 container: a slug where the name now
// lives, stamped FORMAT 1. No current orb can read it (orb_theme.c:262).
func fakeContainerFormat1(t *testing.T, slug string) []byte {
	b := fakeContainer(t, slug)
	binary.LittleEndian.PutUint32(b[4:], 1)
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
	if byName["aurora"].ID != "9b89025ce7a6" {
		t.Errorf("aurora id = %q, want the stem theme_id(name) issues", byName["aurora"].ID)
	}
	if !byName["aurora"].Active || byName["damaged"].OK {
		t.Errorf("load state lost: %+v", l.Themes)
	}

	// Store the listed id. The FORMAT 3 store door copies the bundle and answers
	// reboot:none without flipping the dial — there is no compositor to draw it until
	// P6.5 — so the ack says a job was queued, and a fresh read shows the SAME active
	// face as before. An install that silently flipped here would be the old FORMAT 2
	// door, which P6 deleted.
	ack, err := c.ThemeStore(ip, byName["carousel"].ID)
	if err != nil {
		t.Fatalf("store carousel: %v", err)
	}
	if !strings.Contains(string(ack), `"state":"storing"`) || !strings.Contains(string(ack), `"reboot":"none"`) {
		t.Errorf("store door ack = %s, want state:storing reboot:none", ack)
	}
	l2, err := c.ThemeList(ip)
	if err != nil {
		t.Fatalf("re-list: %v", err)
	}
	if l2.Active != "aurora" {
		t.Errorf("active = %q after storing carousel, want the store door to leave the dial alone (no flip until P6.5)", l2.Active)
	}
	for _, e := range l2.Themes {
		if e.Name == "carousel" && e.Active {
			t.Error("storing a theme flipped the dial; the store door must not until P6.5")
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

// Uploading a FORMAT-2 container lands it under the id its own name hashes to, and the
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
	want, _ := ThemeIDOf("Spring Boulevard")
	if row.ID != want {
		t.Errorf("filed under %q, want theme_id(name) = %q", row.ID, want)
	}
	// ...and the id the hub computed is the id the orb accepts, which is the loop that
	// has to close for a push-then-store to work without a re-list.
	if _, err := c.ThemeStore(ip, row.ID); err != nil {
		t.Errorf("storing the id the hub derived for its own upload: %v", err)
	}
}

// A FORMAT-1 container is the fragmented-identity artifact itself. The hub must refuse it
// before the bytes cross the room, with the reason, because the device would answer 422
// after the whole payload arrived.
func TestThemeUploadRefusesFormat1BeforeDialing(t *testing.T) {
	c, ip, f := liveOrb(t)

	_, err := c.ThemeUpload(ip, fakeContainerFormat1(t, "skeleton"), false)
	if err == nil {
		t.Fatal("a FORMAT-1 container was accepted")
	}
	if !strings.Contains(err.Error(), "FORMAT") {
		t.Errorf("err = %v, want it to name the format it rejected", err)
	}
	for _, call := range f.Calls() {
		if strings.Contains(call, "/api/v1/sd/theme") {
			t.Errorf("a FORMAT-1 file reached the orb anyway: %q", call)
		}
	}
}

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
	_, err := c.ThemeStore(ip, "000000000000")
	if err == nil {
		t.Fatal("store of an id the card does not hold succeeded")
	}
	if !strings.Contains(err.Error(), "no such theme") {
		t.Errorf("err = %v, want the firmware's 404 sentence", err)
	}
}

// The refused-install and boot-damage strings key on id, and the card can only point them
// at a row if the id that arrives beside them is the one the row carries.
func TestThemeTryAndDamageKeyOnTheListedID(t *testing.T) {
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
	f.SetDamage(carouselID, "art plane crc disagrees with the header")

	after, err := c.ThemeList(ip)
	if err != nil {
		t.Fatal(err)
	}
	if after.TryID != carouselID || after.TryWhy == "" {
		t.Errorf("try_id/try_why = %q/%q, want them to name the row that failed", after.TryID, after.TryWhy)
	}
	if after.DamageID != carouselID || after.DamageWhy == "" {
		t.Errorf("damage_id/damage_why = %q/%q", after.DamageID, after.DamageWhy)
	}
}

// The default face is not a card file, so it never took an id and must not start.
func TestThemeDefaultNeedsNoTarget(t *testing.T) {
	c, ip, f := liveOrb(t)
	if _, err := c.ThemeDefault(ip); err != nil {
		t.Fatalf("default: %v", err)
	}
	for _, call := range f.Calls() {
		if strings.HasPrefix(call, "POST /api/v1/sd/default?") {
			t.Errorf("default carried a query, and the firmware reads none: %q", call)
		}
	}
}
