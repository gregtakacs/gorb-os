package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The theme endpoints are the Manage card's theme surface. What is under test is not
// the JSON shape but the gates in front of a dial that WRITES to an SD card: the address
// must be one this hub learned, and the theme must be named by the id the device issued.
//
// These tests dial internal/orbfake, the double written from main/net/orb_net.c. They
// used to dial a stub in this file that answered whatever themes.go asked it to, which
// is how the hub kept sending ?name= through a green suite: the orb end of the test was
// generated from the client, so the client could not be wrong. The double now refuses a
// name-addressed call the way an orb does, and a regression here fails here.

// orbCall records what the fake orb was actually asked, so a claim about a route is
// checked on the wire rather than in a comment.
type orbCall struct{ method, path, query, body string }

// fakeOrbHub returns a hub whose device client dials a live fake orb, the address that
// orb answers at (already LEARNED, as onboarding would leave it), a getter for the last
// request the orb saw, and the orb itself so a test can read its card afterwards.
// Nothing here reaches a real network.
func fakeOrbHub(t *testing.T) (*Server, string, func() orbCall, *orbfake.Orb) {
	t.Helper()
	var last orbCall
	f := orbfake.New(
		orbfake.Theme{Name: "aurora", Bytes: 99, Serial: 1, OK: true, Active: true},
	)
	recording := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		last = orbCall{method: r.Method, path: r.URL.Path, query: r.URL.RawQuery, body: string(b)}
		// Hand the bytes back: reading the body here to record it would otherwise leave
		// the double looking at an empty upload and refusing a valid file.
		r.Body = io.NopCloser(bytes.NewReader(b))
		r.ContentLength = int64(len(b))
		f.ServeHTTP(w, r)
	})
	orb := httptest.NewServer(recording)
	t.Cleanup(orb.Close)

	hp := strings.TrimPrefix(orb.URL, "http://")
	i := strings.LastIndex(hp, ":")
	if i < 0 {
		t.Fatalf("test orb URL %q has no port", orb.URL)
	}
	host, port := hp[:i], hp[i+1:]
	s := newHub()
	s.device = orbnet.NewClient(2 * time.Second)
	s.device.DevicePort = port
	// Theme uploads ride the upload-tuned client (theme.go): the hub's real
	// wiring sets this too, and a test that left it nil would pass a handler
	// the shipped binary would crash. Mirror the port here.
	s.ota = orbnet.NewClient(2 * time.Second)
	s.ota.DevicePort = port
	s.grants.learn(host)
	return s, host, func() orbCall { return last }, f
}

// The id the seeded aurora carries: theme_id("aurora"). Written out so a test that
// drifts from the firmware's hash fails rather than following the client.
const auroraID = "9b89025ce7a6"

// themeBody builds a call's JSON so tests do not hand-write strings with quotes in
// them, which is how an id like ../etc would have been mistyped rather than tested.
func themeBody(ip, id string) string {
	m := map[string]string{"ip": ip}
	if id != "" {
		m["id"] = id
	}
	b, _ := json.Marshal(m)
	return string(b)
}

// postTheme goes through the real mux, so the token guard is exercised next to the
// handler: calling the handler directly would pass even if the route were never
// registered behind requireControl.
func postTheme(t *testing.T, s *Server, path, body string) *httptest.ResponseRecorder {
	t.Helper()
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	return rec
}

func decodeEnv(t *testing.T, rec *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var env map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("body is not the {ok,...} envelope: %v (%s)", err, rec.Body)
	}
	return env
}

func asString(v any) string {
	s, _ := v.(string)
	return s
}

func TestThemeListThroughTheHub(t *testing.T) {
	s, ip, called, _ := fakeOrbHub(t)
	rec := postTheme(t, s, "/api/theme/list", themeBody(ip, ""))
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d body %s", rec.Code, rec.Body)
	}
	env := decodeEnv(t, rec)
	data, _ := env["data"].(map[string]any)
	if env["ok"] != true || data == nil {
		t.Fatalf("body = %s, want an ok payload holding the card state", rec.Body)
	}
	if data["mounted"] != true || data["active"] != "aurora" {
		t.Errorf("card state lost in translation: %v", data)
	}
	rows, _ := data["themes"].([]any)
	if len(rows) != 1 {
		t.Fatalf("themes = %v, want the orb's one row", data["themes"])
	}
	// The card builds its buttons off this row, so the id has to survive the hub on its
	// way to the page: without it the UI can display a theme it cannot install.
	row, _ := rows[0].(map[string]any)
	if row["id"] != auroraID || row["name"] != "aurora" {
		t.Errorf("row = %v, want the name beside the id the orb issued", row)
	}
	// The stored witness is what turns a store press from a hope into a fact: the door
	// acks the queue, and the row's stored flag is the copy arriving in the single
	// slot. If it stopped surviving the hub, the Store button could never tell
	// stored from refused.
	if _, hasStored := row["stored"]; !hasStored {
		t.Error("the row lost the stored witness; the card cannot see a store land")
	}
	if _, hasBusy := data["job_busy"]; !hasBusy {
		t.Error("the list lost the job_busy witness; a queued press would read as a dead button")
	}
	if _, hasTitle := row["title"]; hasTitle {
		t.Error("the row carries a title field; FORMAT 2 has one name")
	}
	if c := called(); c.method != http.MethodGet || c.path != "/api/v1/sd" {
		t.Errorf("orb saw %s %s, want GET /api/v1/sd", c.method, c.path)
	}
}

// The learned-address gate, on every theme route: an address this hub never learned
// over a cable and was never handed must not be dialled, and the refusal must explain
// itself. These are the calls that write to a card.
func TestThemeCallsRefuseAnUnlearnedAddress(t *testing.T) {
	for _, path := range []string{
		"/api/theme/list", "/api/theme/install",
		"/api/theme/delete", "/api/theme/rescan",
	} {
		s, _, called, _ := fakeOrbHub(t)
		env := decodeEnv(t, postTheme(t, s, path, themeBody("192.168.9.99", auroraID)))
		if env["ok"] == true {
			t.Errorf("%s succeeded against an unlearned address", path)
		}
		if !strings.Contains(asString(env["error"]), "not an orb address") {
			t.Errorf("%s refused for the wrong reason: %v", path, env["error"])
		}
		if c := called(); c.path != "" {
			t.Errorf("%s dialled the orb anyway: %+v", path, c)
		}
	}
}

// Hostnames are refused rather than resolved, so the theme surface cannot be aimed at
// a name a page invented, nor made to dial a port the hub did not choose.
func TestThemeCallsRefuseHostnamesAndPorts(t *testing.T) {
	s, _, called, _ := fakeOrbHub(t)
	for _, ip := range []string{"orb.local", "router", "http://10.0.0.1", "10.0.0.1:80", "[::1]", ""} {
		env := decodeEnv(t, postTheme(t, s, "/api/theme/list", themeBody(ip, "")))
		if env["ok"] == true {
			t.Errorf("%q was accepted as an orb address", ip)
		}
	}
	if c := called(); c.path != "" {
		t.Errorf("a refused address still reached the orb: %+v", c)
	}
}

// The id gate at the hub as well as in the client. The card file is selected by an
// opaque 12-hex stem, so a spelling that could have been a path, a title or a FORMAT-1
// slug is refused before the dial — and the refusal must not reach the orb.
func TestThemeCallsRefuseBadIDs(t *testing.T) {
	s, ip, called, _ := fakeOrbHub(t)
	for _, path := range []string{"/api/theme/install", "/api/theme/delete"} {
		for _, id := range []string{"", "aurora", "Aurora", "..", "../etc", "a/b", ".hidden",
			"9B89025CE7A6", "9b89025ce7a", "9b89025ce7a6f", "has space"} {
			env := decodeEnv(t, postTheme(t, s, path, themeBody(ip, id)))
			if env["ok"] == true {
				t.Errorf("%s id=%q was accepted by the hub", path, id)
			}
		}
		if c := called(); c.path != "" {
			t.Errorf("a bad id reached the orb: %+v", c)
		}
	}
}

// The verbs and query spelling the card depends on, checked on the wire the orb sees.
// This is what catches a hub that starts calling a route the firmware never registers,
// or addressing a theme by anything but the id: a 400 that only shows up on glass.
func TestThemeRoutesReachTheOrbAsTheFirmwareRegistersThem(t *testing.T) {
	cases := []struct{ path, id, wantMethod, wantPath, wantQuery string }{
		{"/api/theme/list", "", http.MethodGet, "/api/v1/sd", ""},
		{"/api/theme/install", auroraID, http.MethodPost, "/api/v1/sd/bundle", "id=" + auroraID},
		{"/api/theme/delete", "7565e4480e56", http.MethodPost, "/api/v1/sd/delete", "id=7565e4480e56"},
		{"/api/theme/rescan", "", http.MethodPost, "/api/v1/sd/rescan", ""},
	}
	for _, tc := range cases {
		s, ip, called, _ := fakeOrbHub(t)
		postTheme(t, s, tc.path, themeBody(ip, tc.id))
		c := called()
		if c.method != tc.wantMethod || c.path != tc.wantPath || c.query != tc.wantQuery {
			t.Errorf("%s: orb saw %s %s?%s, want %s %s?%s",
				tc.path, c.method, c.path, c.query, tc.wantMethod, tc.wantPath, tc.wantQuery)
		}
	}
}

// The device's reason must survive two hops. Paraphrasing "a theme job is already
// running" into "request failed" is the difference between an operator who waits and
// one who presses the button again.
func TestThemeFailureKeepsTheFirmwaresReason(t *testing.T) {
	s, ip, _, f := fakeOrbHub(t)
	f.Busy = true
	env := decodeEnv(t, postTheme(t, s, "/api/theme/install", themeBody(ip, auroraID)))
	if env["ok"] == true || !strings.Contains(asString(env["error"]), "a theme job is already running") {
		t.Errorf("env = %v, want the firmware's own sentence", env)
	}
}

// The upload carries the file as its body and the orb as its only target: the theme's
// name arrives inside the container, and ?overwrite is the sole other argument the
// firmware reads.
func TestThemeUploadForwardsTheFileBytes(t *testing.T) {
	s, ip, called, f := fakeOrbHub(t)
	file := orbfake.Container("Perpetual Calendar")
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/theme/upload?ip="+ip, strings.NewReader(string(file)))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d body %s", rec.Code, rec.Body)
	}
	c := called()
	if c.method != http.MethodPost || c.path != "/api/v1/sd/theme" || c.query != "" {
		t.Errorf("orb saw %s %s?%s, want a bare POST /api/v1/sd/theme", c.method, c.path, c.query)
	}
	if c.body != string(file) {
		t.Errorf("forwarded body = %d B, want the %d B verbatim", len(c.body), len(file))
	}
	// The hub read the identity off the bytes rather than being handed it, and reported
	// it, so the card can name what it just pushed.
	env := decodeEnv(t, rec)
	data, _ := env["data"].(map[string]any)
	want, _ := orbnet.ThemeIDOf("Perpetual Calendar")
	if data["name"] != "Perpetual Calendar" || data["id"] != want {
		t.Errorf("upload reply = %v, want the embedded name and its id %s", env["data"], want)
	}
	found := false
	for _, th := range f.Themes() {
		if th.Name == "Perpetual Calendar" && th.ID == want {
			found = true
		}
	}
	if !found {
		t.Errorf("the orb did not file the upload under its embedded name: %+v", f.Themes())
	}
}

// A duplicate upload is the firmware's one client decision. The hub must pass the
// operator's answer through as ?overwrite=1 rather than either failing or replacing
// silently.
func TestThemeUploadOverwriteIsTheOperatorsCall(t *testing.T) {
	s, ip, called, f := fakeOrbHub(t)
	first := orbfake.Container("Tourbillon")
	second := orbfake.Container("Tourbillon")
	second[200] ^= 0xFF // past the header: same name, different bytes — the duplicate case

	if _, err := s.device.ThemeUpload(ip, first, false); err != nil {
		t.Fatalf("seed upload: %v", err)
	}

	mux := http.NewServeMux()
	s.routes(mux)

	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/theme/upload?ip="+ip, strings.NewReader(string(second)))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	env := decodeEnv(t, rec)
	if env["ok"] == true {
		t.Error("a duplicate upload replaced a theme with nobody asking")
	}
	if !strings.Contains(asString(env["error"]), "already on the card") {
		t.Errorf("env = %v, want the firmware's duplicate sentence", env)
	}

	rec = httptest.NewRecorder()
	req = httptest.NewRequest(http.MethodPost, "/api/theme/upload?ip="+ip+"&overwrite=1", strings.NewReader(string(second)))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	if env = decodeEnv(t, rec); env["ok"] != true {
		t.Errorf("overwrite=1 refused: %v", env)
	}
	if c := called(); c.query != "overwrite=1" {
		t.Errorf("orb saw ?%s, want overwrite=1", c.query)
	}
	n := 0
	for _, th := range f.Themes() {
		if th.Name == "Tourbillon" {
			n++
		}
	}
	if n != 1 {
		t.Errorf("%d themes named Tourbillon after an overwrite, want one", n)
	}
}

// A FORMAT-1 container is the fragmented-identity artifact: its name lived in the
// filename, which no orb reads any more. The hub refuses it before the bytes cross the
// room, so the operator learns the file is old instead of watching a 422 arrive late.
func TestThemeUploadRefusesAFormat1Container(t *testing.T) {
	s, ip, called, _ := fakeOrbHub(t)
	file := orbfake.Container("skeleton")
	file[4] = 1 // FORMAT 1
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/theme/upload?ip="+ip, strings.NewReader(string(file)))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	env := decodeEnv(t, rec)
	if env["ok"] == true || !strings.Contains(asString(env["error"]), "FORMAT") {
		t.Errorf("env = %v, want the FORMAT-1 refusal", env)
	}
	if c := called(); c.path != "" {
		t.Errorf("a FORMAT-1 file reached the orb: %+v", c)
	}
}

func TestThemeUploadRefusesAnUnlearnedAddress(t *testing.T) {
	s, _, called, _ := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/theme/upload?ip=203.0.113.7", strings.NewReader(string(orbfake.Container("x"))))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	env := decodeEnv(t, rec)
	if env["ok"] == true || !strings.Contains(asString(env["error"]), "not an orb address") {
		t.Errorf("env = %v, want the learned-address refusal", env)
	}
	if c := called(); c.path != "" {
		t.Errorf("an unlearned upload address reached the orb: %+v", c)
	}
}

// Every theme route reaches an orb, so every one sits behind the launch token: an
// uncredentialed cross-site POST must not be able to delete a theme off a card.
func TestThemeRoutesAreTokenGuarded(t *testing.T) {
	s, ip, _, _ := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	for _, p := range []string{"/api/theme/list", "/api/theme/install",
		"/api/theme/delete", "/api/theme/rescan", "/api/theme/upload"} {
		req, _ := http.NewRequest(http.MethodPost, srv.URL+p, strings.NewReader(themeBody(ip, auroraID)))
		req.Header.Set("Origin", "http://attack.example")
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Fatalf("%s: %v", p, err)
		}
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if resp.StatusCode != http.StatusForbidden {
			t.Errorf("%s accepted a cross-origin uncredentialed POST: %d %s", p, resp.StatusCode, b)
		}
	}
}

// The device theme calls must live in one module, and the FORMAT-2 contract with them:
// the id rule, the name rule and the container read all sit in internal/orbnet, so a
// second copy cannot drift from the firmware the way the client's did.
func TestThemeDeviceCallsLiveInOneModule(t *testing.T) {
	hub := readFile(t, "theme.go")
	if strings.Contains(hub, `"/api/v1/sd`) {
		t.Error("theme.go builds device URLs itself; every device theme route belongs in internal/orbnet/themes.go")
	}
	if strings.Contains(hub, "CheckThemeName(") || strings.Contains(hub, "QueryEscape") {
		t.Error("theme.go re-implements a device-side rule; the gates belong in internal/orbnet")
	}
	mod := readFile(t, "internal/orbnet/themes.go")
	for _, route := range []string{`"/api/v1/sd"`, `"/api/v1/sd/bundle?id=`, `"/api/v1/sd/delete?id=`, `"/api/v1/sd/theme`, `"/api/v1/sd/rescan`} {
		if !strings.Contains(mod, route) {
			t.Errorf("internal/orbnet/themes.go never dials %s, so the device sees a route it does not register", route)
		}
	}
	if strings.Contains(mod, "sd/install?name=") || strings.Contains(mod, "sd/delete?name=") {
		t.Error("the theme client addresses install and delete by name; the firmware takes ?id= only")
	}
}
