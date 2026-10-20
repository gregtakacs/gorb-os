package orbfake

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// The double has to be able to refuse the client. If it will answer ?name=, it is back to
// being a copy of the code under test and the suite goes blind the way it was when real
// orbs 400-ed every install. These are the negative controls that make the contract tests
// in internal/orbnet worth anything: they pin the double to the firmware, not to themes.go.

func post(t *testing.T, f *Orb, path string, body []byte) (int, string) {
	t.Helper()
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	req, err := http.NewRequest(http.MethodPost, srv.URL+path, strings.NewReader(string(body)))
	if err != nil {
		t.Fatal(err)
	}
	req.ContentLength = int64(len(body))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(b)
}

func get(t *testing.T, f *Orb, path string) (int, string) {
	t.Helper()
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	resp, err := http.Get(srv.URL + path)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(b)
}

// Addressing a theme by name is what the pre-merge client did, and what no orb answers.
func TestDoubleRefusesEveryNameAddressedVerb(t *testing.T) {
	f := New(Theme{Name: "aurora", Bytes: 100, Serial: 1, OK: true, Active: true})
	for _, route := range []string{
		"/api/v1/sd/bundle?name=aurora",
		"/api/v1/sd/delete?name=aurora",
		"/api/v1/sd/bundle?slug=aurora",
		"/api/v1/sd/bundle?title=Aurora",
		"/api/v1/sd/bundle",
		"/api/v1/sd/delete",
	} {
		code, body := post(t, f, route, nil)
		if code != http.StatusBadRequest || !strings.Contains(body, "not a theme id") {
			t.Errorf("%s answered %d %s, want 400 \"id is missing or not a theme id\" (h_bundle_install)", route, code, body)
		}
	}
}

// h_theme_id_ok is exact: 12 chars of [0-9a-f]. Anything else never touches the card.
func TestDoubleHoldsTheFirmwaresExactIdRule(t *testing.T) {
	f := New(Theme{Name: "aurora", Bytes: 100, Serial: 1, OK: true})
	for _, id := range []string{"9B89025CE7A6", "9b89025ce7a", "9b89025ce7a6f", "gb89025ce7a6",
		"../etc", "aurora", "", "00000000000 "} {
		code, body := post(t, f, "/api/v1/sd/bundle?id="+id, nil)
		if code != http.StatusBadRequest {
			t.Errorf("id %q accepted with %d %s, want 400", id, code, body)
		}
	}
}

// h_sd's rows carry name, id, why, bytes, serial, ok, active, changed. A title field is
// FORMAT 1 and an absent id means the double stopped matching the firmware.
func TestDoubleEmitsTheFormatTwoRowShape(t *testing.T) {
	f := New(Theme{Name: "aurora", Bytes: 123, Serial: 4, OK: true, Active: true})
	code, body := get(t, f, "/api/v1/sd")
	if code != 200 {
		t.Fatalf("status %d", code)
	}
	for _, want := range []string{`"name":"aurora"`, `"id":"9b89025ce7a6"`, `"changed":false`,
		`"try_id":`, `"damage_id":`, `"active":true`} {
		if !strings.Contains(body, want) {
			t.Errorf("h_sd reply missing %s:\n%s", want, body)
		}
	}
	for _, gone := range []string{`"title":`, `try_slug`, `damage_slug`} {
		if strings.Contains(body, gone) {
			t.Errorf("h_sd reply still carries FORMAT 1 field %s:\n%s", gone, body)
		}
	}
}

// An upload is filed by the name inside the container, never by the filename it arrived
// under, and a FORMAT-1 container is the artifact no current orb can read.
func TestDoubleFilesUploadsByTheEmbeddedName(t *testing.T) {
	f := New()
	body := container("Mirage \u2014 Gold")
	code, out := post(t, f, "/api/v1/sd/theme", body)
	if code != 200 || !strings.Contains(out, "stored") {
		t.Fatalf("upload answered %d %s", code, out)
	}
	_, list := get(t, f, "/api/v1/sd")
	if !strings.Contains(list, `"name":"Mirage — Gold"`) {
		t.Errorf("uploaded theme not listed under its embedded name: %s", list)
	}
	if !strings.Contains(list, `"id":"373b51cc021b"`) {
		t.Errorf("uploaded theme not filed under theme_id(name): %s", list)
	}

	// FORMAT 1: rejected, and nothing filed.
	f2 := New()
	bad := container("skeleton")
	bad[4] = 1
	code, out = post(t, f2, "/api/v1/sd/theme", bad)
	if code != http.StatusUnprocessableEntity {
		t.Errorf("FORMAT 1 upload answered %d %s, want 422", code, out)
	}
	_, list = get(t, f2, "/api/v1/sd")
	if strings.Contains(list, "skeleton") {
		t.Errorf("a FORMAT-1 container was filed: %s", list)
	}
}

// Content-Length is mandatory: without it a vanished client and a finished upload are the
// same request, and a partial file must never be reachable under a loadable name.
func TestDoubleDemandsContentLength(t *testing.T) {
	f := New()
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	req, _ := http.NewRequest(http.MethodPost, srv.URL+"/api/v1/sd/theme", strings.NewReader(string(container("x"))))
	req.ContentLength = -1
	delete(req.Header, "Content-Length")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusLengthRequired || !strings.Contains(string(b), "Content-Length required") {
		t.Errorf("no-Content-Length upload answered %d %s, want 411", resp.StatusCode, b)
	}
}

// container writes a minimal FORMAT-2 header carrying name, matching tools/orbtheme.py's
// pack_header for the fields the identity gates read.
func container(name string) []byte {
	b := make([]byte, 512)
	b[0], b[1], b[2], b[3] = 0x54, 0x52, 0x4f, 0x47
	b[4] = 2
	copy(b[14*4:], name)
	return b
}

// The id rule and the firmware's must agree on the same answer, or the double is addressing
// files its own list is not naming.
func TestDoubleIdsAreTheFirmwaresHash(t *testing.T) {
	for _, tc := range []struct{ name, id string }{
		{"aurora", "9b89025ce7a6"},
		{"Aurora", "e9de098c7733"},
		{"Neon", "618484e2c549"},
		{"Mirage — Gold", "373b51cc021b"},
	} {
		if got := themeID(tc.name); got != tc.id {
			t.Errorf("themeID(%q) = %s, want %s from tools/orbtheme.py", tc.name, got, tc.id)
		}
	}
}

// The mode door is the only exit from a state the page can put an orb into, so its accept
// set has to be the firmware's and not merely plausible: net_mode_set takes "ap", and takes
// "sta" and "" as the way back to the station default, and rejects everything else — the
// case-variant "AP" and an over-long value included. A double that accepted "AP" would let
// a hub test pass by storing a mode no orb can ever boot.
func TestModeDoorMatchesNetModeSet(t *testing.T) {
	for _, tc := range []struct {
		name, body string
		code       int
		stored     string // "" means the absent key; -1 = do not assert
	}{
		{"ap", `{"mode":"ap"}`, 200, "ap"},
		{"sta", `{"mode":"sta"}`, 200, ""},
		{"empty is the default", `{"mode":""}`, 200, ""},
		{"wrong case", `{"mode":"AP"}`, 400, "-"},
		{"too long", `{"mode":"station"}`, 400, "-"},
		{"no key", `{}`, 400, "-"},
	} {
		f := New()
		code, body := post(t, f, "/api/v1/wifi/mode", []byte(tc.body))
		if code != tc.code {
			t.Errorf("%s: POST /wifi/mode %s -> %d, want %d (%s)", tc.name, tc.body, code, tc.code, body)
		}
		if tc.code != 200 {
			continue
		}
		// The reply names the mode the orb will boot in, which is the hub's only
		// confirmation that "sta" meant the default rather than a literal string.
		if !strings.Contains(body, `"mode":"`+tc.stored+`"`) &&
			!strings.Contains(body, `"mode":"sta"`) {
			t.Errorf("%s: reply %s does not name the accepted mode", tc.name, body)
		}
		if got, n := f.ModeSet(); got != tc.stored || n != 1 {
			t.Errorf("%s: stored %q after %d accepted writes, want %q after 1", tc.name, got, n, tc.stored)
		}
	}
}

// AP-isolation is a boot-time choice, so the door has to leave the radio looking the way the
// next boot will find it: the SoftAP is the only address and there is no lease. A double
// that kept a lease would let a hub test drive an orb out of an isolation it never entered.
func TestModeApClearsTheLease(t *testing.T) {
	f := New()
	f.SetSTA(true)
	if code, body := post(t, f, "/api/v1/wifi/mode", []byte(`{"mode":"ap"}`)); code != 200 {
		t.Fatalf("mode ap -> %d (%s)", code, body)
	}
	code, body := get(t, f, "/api/v1/status")
	if code != 200 {
		t.Fatalf("status -> %d", code)
	}
	if strings.Contains(body, `"sta_up":true`) {
		t.Errorf("an AP-isolated orb still reports a lease: %s", body)
	}
}

// The mode refusals quote their own two values, so the firmware's C text carries three
// backslashes where the wire carries one and the shared sentence scrape would call a real
// message missing. Compared with the escapes stripped from both sides instead: word for
// word still honest, and it fails on a reworded sentence rather than on its punctuation.
func TestModeRefusalTextIsTheFirmwares(t *testing.T) {
	net := glueC.ReplaceAllString(firmwareSrc(t, "net", "orb_net.c"), "")
	strip := strings.NewReplacer(`\`, "", `"`, "")
	hay := strip.Replace(net)
	for _, body := range []string{noModeBody, badModeBody, modeOffBody} {
		sentence := strings.TrimSuffix(strings.TrimPrefix(body, `{"error":"`), `"}`)
		if sentence == body {
			t.Fatalf("%s is not in the shape the doors send", body)
		}
		if !strings.Contains(hay, strip.Replace(sentence)) {
			t.Errorf("double refuses mode with %q, which is not a sentence in orb_net.c", sentence)
		}
	}
}
