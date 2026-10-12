package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The theme endpoints are the Manage card's theme surface. What is under test is not
// the JSON shape but the two gates in front of a dial that WRITES to an SD card: the
// address must be one this hub learned, and the theme name must be one the firmware
// would accept. A regression on either is a hub touching a device the operator never
// named.

const hubSDReply = `{"mounted":true,"card":"SDSC","total_mb":7581,"free_mb":7100,` +
	`"source":"sd","active":"aurora","live":"aurora","card_count":1,` +
	`"themes":[{"name":"aurora","title":"Aurora","why":"","bytes":99,"serial":1,"ok":true,"active":true}]}`

// orbCall records what the fake orb was actually asked, so a claim about a route is
// checked on the wire rather than in a comment.
type orbCall struct{ method, path, query, body string }

// fakeOrbHub returns a hub whose device client dials a local fake orb, the address
// that orb answers at (already LEARNED, as onboarding would leave it), and a getter
// for the last request the orb saw. Nothing here reaches a real network.
func fakeOrbHub(t *testing.T) (*Server, string, func() orbCall) {
	t.Helper()
	var last orbCall
	orb := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		last = orbCall{method: r.Method, path: r.URL.Path, query: r.URL.RawQuery, body: string(b)}
		w.Header().Set("Content-Type", "application/json")
		if strings.Contains(r.URL.RawQuery, "busy") {
			w.WriteHeader(http.StatusConflict)
			_, _ = w.Write([]byte(`{"error":"a theme job is already running"}`))
			return
		}
		if r.URL.Path == "/api/v1/sd" {
			_, _ = w.Write([]byte(hubSDReply))
			return
		}
		_, _ = w.Write([]byte(`{"state":"installing","received":` + strconv.Itoa(len(b)) + `}`))
	}))
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
	s.grants.learn(host)
	return s, host, func() orbCall { return last }
}

// themeBody builds a call's JSON so tests do not hand-write strings with quotes in
// them, which is how a name like ../etc would have been mistyped rather than tested.
func themeBody(ip, name string) string {
	m := map[string]string{"ip": ip}
	if name != "" {
		m["name"] = name
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
	s, ip, called := fakeOrbHub(t)
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
	if rows, _ := data["themes"].([]any); len(rows) != 1 {
		t.Fatalf("themes = %v, want the orb's one row", data["themes"])
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
		"/api/theme/list", "/api/theme/install", "/api/theme/default",
		"/api/theme/delete", "/api/theme/rescan",
	} {
		s, _, called := fakeOrbHub(t)
		env := decodeEnv(t, postTheme(t, s, path, themeBody("192.168.9.99", "aurora")))
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
	s, _, called := fakeOrbHub(t)
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

// The name gate at the hub as well as in the client: a traversal spelling never
// becomes a path on the orb's card, and the check runs before the dial.
func TestThemeCallsRefuseBadNames(t *testing.T) {
	s, ip, called := fakeOrbHub(t)
	for _, tc := range []struct{ path, name string }{
		{"/api/theme/install", ""}, {"/api/theme/install", ".."}, {"/api/theme/install", "../etc"},
		{"/api/theme/install", "a/b"}, {"/api/theme/install", ".hidden"}, {"/api/theme/install", "has space"},
		{"/api/theme/delete", ""}, {"/api/theme/delete", "../secret"},
	} {
		env := decodeEnv(t, postTheme(t, s, tc.path, themeBody(ip, tc.name)))
		if env["ok"] == true {
			t.Errorf("%s name=%q was accepted by the hub", tc.path, tc.name)
		}
		if c := called(); c.path != "" {
			t.Errorf("%s name=%q reached the orb: %+v", tc.path, tc.name, c)
		}
	}
}

// The verbs and query spelling the card depends on, checked on the wire the orb sees.
// This is what catches a hub that starts calling a route the firmware never registers
// — a 404 that only ever shows up on glass.
func TestThemeRoutesReachTheOrbAsTheFirmwareRegistersThem(t *testing.T) {
	cases := []struct{ path, name, wantMethod, wantPath, wantQuery string }{
		{"/api/theme/list", "", http.MethodGet, "/api/v1/sd", ""},
		{"/api/theme/install", "aurora", http.MethodPost, "/api/v1/sd/install", "name=aurora"},
		{"/api/theme/default", "", http.MethodPost, "/api/v1/sd/default", ""},
		{"/api/theme/delete", "old", http.MethodPost, "/api/v1/sd/delete", "name=old"},
		{"/api/theme/rescan", "", http.MethodPost, "/api/v1/sd/rescan", ""},
	}
	for _, tc := range cases {
		s, ip, called := fakeOrbHub(t)
		postTheme(t, s, tc.path, themeBody(ip, tc.name))
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
	s, ip, _ := fakeOrbHub(t)
	env := decodeEnv(t, postTheme(t, s, "/api/theme/install", themeBody(ip, "busy")))
	if env["ok"] == true || !strings.Contains(asString(env["error"]), "a theme job is already running") {
		t.Errorf("env = %v, want the firmware's own sentence", env)
	}
}

// The upload carries the file as its body and the target as query parameters.
func TestThemeUploadForwardsTheFileBytes(t *testing.T) {
	s, ip, called := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/theme/upload?ip="+ip+"&name=aurora",
		strings.NewReader(strings.Repeat("A", 512)))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d body %s", rec.Code, rec.Body)
	}
	c := called()
	if c.method != http.MethodPost || c.path != "/api/v1/sd/theme" || c.query != "name=aurora" {
		t.Errorf("orb saw %s %s?%s", c.method, c.path, c.query)
	}
	if c.body != strings.Repeat("A", 512) {
		t.Errorf("forwarded body = %d B, want the 512 B verbatim", len(c.body))
	}
}

func TestThemeUploadRefusesAnUnlearnedAddress(t *testing.T) {
	s, _, called := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/theme/upload?ip=203.0.113.7&name=aurora", strings.NewReader("junk"))
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
	s, ip, _ := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	for _, p := range []string{"/api/theme/list", "/api/theme/install", "/api/theme/default",
		"/api/theme/delete", "/api/theme/rescan", "/api/theme/upload"} {
		req, _ := http.NewRequest(http.MethodPost, srv.URL+p, strings.NewReader(themeBody(ip, "aurora")))
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

// The device theme calls must live in one module. That is the reason iteration 5 is
// shaped this way: docs/theme-identity-plan.md will put an orb id on every one of
// them, and that change needs exactly one home rather than six handlers.
func TestThemeDeviceCallsLiveInOneModule(t *testing.T) {
	hub := readFile(t, "theme.go")
	if strings.Contains(hub, `"/api/v1/sd`) {
		t.Error("theme.go builds device URLs itself; every device theme route belongs in internal/orbnet/themes.go")
	}
	mod := readFile(t, "internal/orbnet/themes.go")
	for _, route := range []string{`"/api/v1/sd"`, `"/api/v1/sd/install`, `"/api/v1/sd/default`, `"/api/v1/sd/delete`, `"/api/v1/sd/theme`, `"/api/v1/sd/rescan`} {
		if !strings.Contains(mod, route) {
			t.Errorf("internal/orbnet/themes.go never dials %s, so the future ?id= switch will miss it", route)
		}
	}
}
