package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// orbNet is the one path every pin in this file reads from.
const orbNet = "main/net/orb_net.c"

// The device verbs, pinned to the firmware source rather than to anybody's reading
// of it, in the same arrangement firmware_contract_test.go uses for the theme
// surface: the double and the client are both checked against main/net/orb_net.c,
// so the two can only agree by both being right.
//
// Before this file the allowlist check saw only the /api/v1/sd rows. Every other
// device route in deviceRoutes — wifi/scan, wifi/join, wifi/ap, config, reboot —
// was asserted by nothing but the client that calls it, which is exactly the
// arrangement that let the ?name= break through.

// deviceHandlers names the C function behind each verb, so a rename in the
// firmware fails these pins loudly instead of turning them into a vacuous pass.
var deviceHandlers = map[string]string{
	"scan":  "h_wifi_scan",
	"join":  "h_wifi_join",
	"ap":    "h_wifi_ap",
	"confG": "h_config_get",
	"confP": "h_config_post",
	"boot":  "h_reboot",
	"scr":   "h_screen",
	"stat":  "h_status",
}

func handler(t *testing.T, src, which string) string {
	t.Helper()
	name, ok := deviceHandlers[which]
	if !ok {
		t.Fatalf("no handler registered for %q", which)
	}
	return cFunction(t, src, name)
}

// The literals the fake answers with must be literals the firmware answers with.
// Anything else is the double inventing a shape and the client believing it.
func TestDeviceVerbBodiesAreTheFirmwaresOwnWords(t *testing.T) {
	src := firmwareFile(t, orbNet)
	cases := []struct {
		which  string
		bodies []string
	}{
		{"scan", []string{`{"status":"scanning"}`, `{"error":"busy", "detail":"association in progress"}`, `{"error":"scan timed out"}`, `{"error":"scan failed"}`}},
		{"join", []string{`{"state":"joining"}`, `{"error":"ssid required or too long"}`, `{"error":"body too large"}`, `{"error":"content-length required"}`, `{"error":"tz rejected"}`}},
		{"ap", []string{`{"state":"ap_closing"}`, `{"error":"no lease — the AP is the only way in"}`, `"state\":\"ap_opening\"`, `"ap_ssid\"`, `"seconds"`}},
		{"confP", []string{`{"state":"zoned"`, `"tz\"`, `"posix\"`, `"tz_offset\"`, `{"error":"tz rejected"}`}},
		{"boot", []string{`{"state":"rebooting"}`}},
		{"scr", []string{`{"state":"accepted"}`, `{"error":"to, turn, push or hold required"}`, `"unknown app"`}},
	}
	for _, c := range cases {
		body := handler(t, src, c.which)
		emitted := strings.Join(fakeBodies(t, c.which), "\n")
		for _, lit := range c.bodies {
			// Compare in unescaped space: the firmware's reply is a C string
			// literal, so every quote in it is backslash-escaped in the source, and
			// matching decoded JSON against raw source finds nothing while reading
			// like a pass.
			if !strings.Contains(unescapeC(body), unescapeC(lit)) {
				t.Errorf("the fake answers %s with %s, but the firmware's %s never emits that string",
					c.which, lit, deviceHandlers[c.which])
			}
			// And the other direction, without which this test only proves the
			// firmware said something: a literal dropped from the fake must fail
			// here, not quietly leave a body nobody ever exercises.
			if !strings.Contains(unescapeC(emitted), unescapeC(lit)) {
				t.Errorf("the firmware emits %s but the fake never does, so no hub test reaches it", lit)
			}
		}
	}
}

// fakeBodies drives one verb of the double hard enough to produce every body the
// cases above claim, including the refusals. Bodies are read off the wire, not from
// the Go source, so the fake cannot pass by still containing the right string in a
// handler no request path reaches.
func fakeBodies(t *testing.T, which string) []string {
	t.Helper()
	f := orbfake.New()
	srv := httptest.NewServer(f)
	defer srv.Close()
	var out []string
	get := func(path string) { _, b := httpFetch(t, http.MethodGet, srv.URL+path, ""); out = append(out, b) }
	post := func(path, body string) {
		_, b := httpFetch(t, http.MethodPost, srv.URL+path, body)
		out = append(out, b)
	}
	switch which {
	case "scan":
		get("/api/v1/wifi/scan") // arms: scanning
		f.SetScanBusy(true)
		get("/api/v1/wifi/scan")
		f.SetScanBusy(false)
		f.SetScanFailureReason("scan timed out")
		get("/api/v1/wifi/scan")
		f.SetScanFailureReason("scan failed")
		get("/api/v1/wifi/scan")
	case "join":
		post("/api/v1/wifi/join", `{"ssid":"lab-2g","pass":"hunter2"}`)
		post("/api/v1/wifi/join", `{}`)
		post("/api/v1/wifi/join", `{"ssid":"`+strings.Repeat("s", 40)+`"}`)
		post("/api/v1/wifi/join", `{"ssid":"lab-2g","tz":"bad\\\\zone"}`)
		post("/api/v1/wifi/join", `{"ssid":"`+strings.Repeat("t", 300)+`"}`)
		// content-length required: only an unknown-length body gets ContentLength
		// -1 past Go, which is what httpd reports when the header is absent.
		out = append(out, httpChunked(t, srv.URL+"/api/v1/wifi/join"))
	case "ap":
		f.SetSTA(false)
		post("/api/v1/wifi/ap", `{"on":"off"}`)
		f.SetSTA(true)
		post("/api/v1/wifi/ap", `{"on":"off"}`)
		post("/api/v1/wifi/ap", `{}`)
	case "confP":
		post("/api/v1/config", `{"tz":"US/Eastern"}`)
		post("/api/v1/config", `{}`)
		post("/api/v1/config", `{"tz":"bad\\\\zone"}`)
	case "boot":
		post("/api/v1/reboot", `{}`)
	case "scr":
		get("/api/v1/screen?to=clock")
		get("/api/v1/screen")
		get("/api/v1/screen?to=not_an_app")
	}
	if len(out) == 0 {
		t.Fatalf("no fake bodies collected for %s: the driver for this verb is missing", which)
	}
	return out
}

// httpChunked POSTs with an unknown-length body, the only shape that reaches the
// handler with ContentLength -1 and so the only one that can produce the firmware's
// content-length-required refusal. A real orb's httpd refuses the same way when the
// header is simply absent.
func httpChunked(t *testing.T, url string) string {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, url, struct{ io.Reader }{
		strings.NewReader(`{"ssid":"lab-2g"}`),
	})
	if err != nil {
		t.Fatal(err)
	}
	req.ContentLength = -1
	req.Header.Set("Transfer-Encoding", "chunked")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<16))
	return string(b)
}

// httpFetch returns the status and body of one call, tolerating refusals.
func httpFetch(t *testing.T, method, url, body string) (int, string) {
	t.Helper()
	req, err := http.NewRequest(method, url, strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<16))
	return resp.StatusCode, string(b)
}

// unescapeC drops the backslashes a C string literal uses to quote its JSON, so a
// needle can be written as the body the device actually sends.
func unescapeC(s string) string { return strings.ReplaceAll(s, `\`, "") }

// The scan row shape is the one a wizard renders, so its spelling is a contract and
// not a detail: ssid/rssi/ch/open in that order, open a bare bool.
func TestScanRowShapeMatchesScanCollect(t *testing.T) {
	src := firmwareFile(t, orbNet)
	render := sectionContaining(t, src, "s_scan_json, sizeof(s_scan_json), \"{\\\"networks\\\":[")
	for _, frag := range []string{`\"ssid\"`, `\"rssi\"`, `\"ch\"`, `\"open\"`} {
		if !strings.Contains(render, frag) {
			t.Errorf("scan_collect renders no %s, so the fake's row shape is invented", frag)
		}
	}
	f := orbfake.New()
	f.SetNetworks(orbfake.Net{SSID: "lab-2g", RSSI: -47, Ch: 6, Open: true})
	srv := httptest.NewServer(f)
	defer srv.Close()
	b, err := clientGet(t, srv, "/api/v1/wifi/scan")
	if err != nil {
		t.Fatal(err)
	}
	var got struct {
		Networks []struct {
			SSID string `json:"ssid"`
			RSSI int    `json:"rssi"`
			Ch   int    `json:"ch"`
			Open bool   `json:"open"`
		} `json:"networks"`
	}

	if json.Unmarshal(b, &got) != nil || len(got.Networks) != 1 || got.Networks[0].SSID != "lab-2g" {
		t.Fatalf("fake scan body = %s", b)
	}
	// Field order, because the firmware's snprintf fixes it and a client that
	// decodes by position rather than by name must not be able to pass quietly.
	if i := strings.Index(string(b), `"ssid"`); i < 0 || strings.Index(string(b), `"open"`) < i {
		t.Errorf("fake scan rows are not in the firmware's order: %s", b)
	}

}

// The async scan is the contract, not an implementation detail: scan answers
// {"status":"scanning"} at HTTP 200 until the radio task has rendered the table,
// because one httpd worker that blocked on the driver starved the status poll and
// the timezone fetch behind it (orb_net.c:1772-1781). A hub that treats the 200 as
// a result renders an empty network list and looks, to its owner, like a dead orb.
func TestScanIsAPollNotAnAnswer(t *testing.T) {
	f := orbfake.New()
	srv := httptest.NewServer(f)
	defer srv.Close()

	// First touch arms the scan: still 200, still no list. The firmware starts at
	// SCAN_IDLE and posts NET_CMD_SCAN, so nobody gets a table before a radio has
	// been asked for one.
	b, err := clientGet(t, srv, "/api/v1/wifi/scan")
	if err != nil {
		t.Fatalf("arming scan: %v", err)
	}
	if string(b) != `{"status":"scanning"}` {
		t.Fatalf("first scan = %s, want the firmware's scanning reply", b)
	}
	if b2, _ := clientGet(t, srv, "/api/v1/wifi/scan"); string(b2) != string(b) {
		t.Errorf("scan mid-flight = %s, want the same scanning reply", b2)
	}
	// A hub that gave up here and rendered what it had would show an empty network
	// list on a healthy orb, which is the bug this shape exists to prevent.
	var early struct {
		Networks []any `json:"networks"`
	}
	if json.Unmarshal(b, &early) == nil && len(early.Networks) > 0 {
		t.Errorf("scan answered with %d networks before the radio was told to scan", len(early.Networks))
	}
	// scan_collect lands, and the cached table is what every later reader gets.
	f.SetNetworks(orbfake.Net{SSID: "lab-2g", RSSI: -47, Ch: 6})
	if b, _ := clientGet(t, srv, "/api/v1/wifi/scan"); !strings.Contains(string(b), `"ssid"`) {
		t.Errorf("settled scan = %s, want the cached network list", b)
	}
	// Rescan re-arms rather than serving the cache it just settled into.
	if b, _ := clientGet(t, srv, "/api/v1/wifi/scan?fresh=1"); string(b) != `{"status":"scanning"}` {
		t.Errorf("fresh scan = %s, want scanning", b)
	}
	// A failed scan is permanent until re-armed: the page offers Rescan, and a
	// failure that healed itself on the next poll would hide a dead radio.
	f.SetScanFailure(true)
	f.SetNetworks()
	resp, err := http.Get(srv.URL + "/api/v1/wifi/scan")
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 500 {
		t.Errorf("failed scan = %s, want the 500 the page offers Rescan for", resp.Status)
	}
}

// sectionContaining returns a window of the firmware source around a marker, for a
// claim about a stretch of code cFunction's brace matching cannot isolate alone.
func sectionContaining(t *testing.T, src, marker string) string {
	t.Helper()
	i := strings.Index(src, marker)
	if i < 0 {
		t.Fatalf("main/net/orb_net.c contains no %q — the code the fake mirrors was rewritten", marker)
	}
	lo, hi := i-200, i+1400
	if lo < 0 {
		lo = 0
	}
	if hi > len(src) {
		hi = len(src)
	}
	return src[lo:hi]
}

func clientGet(t *testing.T, srv *httptest.Server, path string) ([]byte, error) {
	t.Helper()
	resp, err := http.Get(srv.URL + path)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(io.LimitReader(resp.Body, 1<<16))
	if err != nil {
		return nil, err
	}
	if resp.StatusCode >= 400 {
		return b, &httpStatusError{resp.StatusCode, string(b)}
	}
	return b, nil
}

type httpStatusError struct {
	code int
	body string
}

func (e *httpStatusError) Error() string { return http.StatusText(e.code) + ": " + e.body }

// The join caps are rejections, never truncations: a 40-char SSID clipped to 32
// sends the orb hunting for a network that does not exist, which reads to the owner
// as an unexplained failure with no hint that the name was too long.
func TestJoinRejectsWhatTheFirmwareRejects(t *testing.T) {
	f := orbfake.New()
	srv := httptest.NewServer(f)
	defer srv.Close()
	c := orbnet.NewClient(2 * time.Second)
	ip := strings.TrimPrefix(srv.URL, "http://")

	if b, err := c.PostJoin(ip, strings.Repeat("s", orbfake.SSIDMax+1), "hunter2", ""); err == nil {
		t.Errorf("overlong SSID accepted, body = %s; the firmware rejects rather than clips", b)
	}
	if _, err := c.PostJoin(ip, "lab-2g", strings.Repeat("p", orbfake.PassMax+1), ""); err == nil {
		t.Error("overlong passphrase accepted where json_str would reject it")
	}
	if _, err := c.PostJoin(ip, "", "", ""); err == nil {
		t.Error("empty SSID accepted where the device answers ssid required or too long")
	}
	// A zone the table does not hold is NOT a rejection: tz_reject_reason (orb_net.c
	// :1247) checks only printable ASCII, no quote or backslash, and length, because
	// any raw POSIX rule is a legal value. What the device does reject outright is an
	// ESCAPE in the body — json_str drops the backslash and keeps the character after
	// it, so EST5EDT\t would arrive at the guard as the printable EST5EDTt, get
	// stored, and be applied as a TZ nobody typed. A double built on encoding/json
	// alone decodes that escape and accepts the request, which is the exact drift
	// this file exists to catch, so it is asserted here rather than assumed.
	if _, err := c.PostJoin(ip, "lab-2g", "hunter2", "EST5EDT\\t"); err == nil {
		t.Error("join accepted an escaped tz; the device 400s it before anything is stored")
	}
	if _, err := c.PostJoin(ip, "lab-2g", "hunter2", strings.Repeat("Z", orbfake.TZMax+1)); err == nil {
		t.Error("overlong tz accepted where tz_reject_reason answers tz too long")
	}
	// An unknown-but-well-formed zone must be accepted and reported by its EFFECT.
	if _, err := c.PostJoin(ip, "lab-2g", "hunter2", "Mars/Olympus_Mons"); err != nil {
		t.Errorf("well-formed raw POSIX rule rejected: %v", err)
	}
	if _, err := c.PostJoin(ip, "lab-2g", "hunter2", "US/Eastern"); err != nil {
		t.Fatalf("PostJoin: %v", err)
	}
	state, _, _, _, _, _, _, _, _, joined := f.Device()
	if joined != [3]string{"lab-2g", "hunter2", "US/Eastern"} {
		t.Errorf("orb was told to join %v", joined)
	}
	if state != "joining" {
		t.Errorf("state after join = %q, want joining", state)
	}
}

// Closing the AP is refused while there is no lease: an orb with neither a lease nor
// an AP is precisely the lockout the rescue path exists for, so "ok, closed" there
// would be actively harmful (orb_net.c:2363-2372).
func TestAPCloseNeedsALeaseAndOpenReportsTheSSID(t *testing.T) {
	f := orbfake.New()
	f.SetSTA(false)
	srv := httptest.NewServer(f)
	defer srv.Close()
	c := orbnet.NewClient(2 * time.Second)
	ip := strings.TrimPrefix(srv.URL, "http://")

	if _, err := c.PostAP(ip, false); err == nil {
		t.Error("AP closed with no lease: the hub accepted a body that strands an orb")
	}
	f.SetSTA(true)
	if _, err := c.PostAP(ip, false); err != nil {
		t.Errorf("close with a lease: %v", err)
	}
	b, err := c.PostAP(ip, true)
	if err != nil {
		t.Fatalf("open AP: %v", err)
	}
	// The SSID comes from the MAC's last four hex digits, so a hub that tells the
	// owner which network to join is telling them the orb's real one.
	_, mac, _, _, _, _, _, _, _, _ := f.Device()
	if want := `"ap_ssid":"orb-` + strings.ToUpper(mac[len(mac)-4:]) + `"`; !strings.Contains(string(b), want) {
		t.Errorf("AP body = %s, want it to name %s", b, want)
	}
	if !strings.Contains(string(b), `"seconds":900`) {
		t.Errorf("AP body = %s, want the manual hold the owner needs", b)
	}
}

// h_config_post reports the EFFECT, never an echo: a zone newlib silently degraded to
// UTC still has to come back as UTC with offset 0, or the page claims success for a
// clock that never took.
func TestConfigPostReportsTheEffectNotTheEcho(t *testing.T) {
	f := orbfake.New()
	srv := httptest.NewServer(f)
	defer srv.Close()
	c := orbnet.NewClient(2 * time.Second)
	ip := strings.TrimPrefix(srv.URL, "http://")

	b, err := c.PostConfig(ip, "Asia/Tokyo")
	if err != nil {
		t.Fatalf("PostConfig: %v", err)
	}
	for _, frag := range []string{`"tz":"Asia/Tokyo"`, `"tz_offset":-540`} {
		if !strings.Contains(string(b), frag) {
			t.Errorf("config reply = %s, want %s", b, frag)
		}
	}
	// A body without the key is not a request: tz is the only thing this endpoint
	// owns, and a 200 to {} turns a client bug into a silent no-op the page reports
	// as saved.
	if resp, err := http.Post(srv.URL+"/api/v1/config", "application/json", strings.NewReader("{}")); err != nil {
		t.Fatal(err)
	} else {
		io.Copy(io.Discard, io.LimitReader(resp.Body, 1024))
		resp.Body.Close()
		if resp.StatusCode != 400 {
			t.Errorf("config {} -> %s, want the 400 that makes the caller's bug visible", resp.Status)
		}
	}
	if resp, err := http.Post(srv.URL+"/api/v1/config", "application/json",
		strings.NewReader(`{"tz":"`+strings.Repeat("Z", 200)+`"}`)); err != nil {
		t.Fatal(err)
	} else {
		io.Copy(io.Discard, io.LimitReader(resp.Body, 1024))
		resp.Body.Close()
		if resp.StatusCode != 413 {
			t.Errorf("oversized config body -> %s, want the 413 the firmware answers", resp.Status)
		}
	}

}

// TestDeviceRouteAllowlistMatchesTheFirmwaresUriTable only ever checked the
// /api/v1/sd rows: every wifi/*, config and reboot entry in deviceRoutes was
// asserted by nothing but the client that dials it. This closes that gap by walking
// the whole uri table, with the routes the hub deliberately refuses named rather
// than a silent filter that could quietly start exempting the wrong thing.
func TestEveryFirmwareDeviceRouteIsAllowedAndFakeable(t *testing.T) {
	src := firmwareFile(t, orbNet)
	rows := regexp.MustCompile(`\{\s*\.uri\s*=\s*"([^"]+)",\s*\.method\s*=\s*HTTP_(GET|POST),\s*\.handler\s*=\s*(\w+)\s*\}`)
	found := rows.FindAllStringSubmatch(src, -1)
	if len(found) == 0 {
		t.Fatal("no uri table parsed out of orb_net.c — its spelling changed and this pin is blind")
	}
	// /api/v1/ota takes a raw image and writes the inactive slot; the hub's rule is
	// that firmware comes from a validated bundle, so the bare-app path stays closed
	// (deviceRoutes' comment records why). The captive-portal probes are UI-unused.
	refused := map[string]bool{"/api/v1/ota": true, "/generate_204": true,
		"/hotspot-detect.html": true, "/connect/test.php": true}
	checked := 0
	for _, m := range found {
		uri, method := m[1], strings.ToUpper(m[2])
		if !strings.HasPrefix(uri, "/api/v1/") || refused[uri] {
			continue
		}
		checked++
		methods, ok := deviceRoutes[uri]
		if !ok {
			t.Errorf("the orb registers %s %s but the hub's allowlist omits it, so nothing can reach it", method, uri)
			continue
		}
		if !contains(methods, method) {
			t.Errorf("the orb registers %s %s but the hub allows %v", method, uri, methods)
		}
		// A route the hub may dial and the double cannot answer is a route no test
		// covers: the fake's 404 is exactly what an orb says to a made-up path.
		if code := fakeProbe(t, uri, method); code == 404 {
			t.Errorf("the fake 404s %s %s, which the hub is allowed to dial", method, uri)
		}
	}
	if checked < 8 {
		t.Fatalf("only %d device routes checked: the uri table's spelling changed and this test is blind", checked)
	}
}

// fakeProbe asks the double one request and reports the status it chose, so a route
// missing from the fake fails loudly instead of quietly answering 404 forever.
func fakeProbe(t *testing.T, uri, method string) int {
	t.Helper()
	f := orbfake.New()
	srv := httptest.NewServer(f)
	defer srv.Close()
	var body io.Reader
	if method == http.MethodPost {
		body = strings.NewReader("{}")
	}
	req, err := http.NewRequest(method, srv.URL+uri, body)
	if err != nil {
		t.Fatal(err)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	return resp.StatusCode
}

// The status body is the hub's only read of an orb, so every key the firmware
// formats must be a key the fake formats. A field the double drops decodes to the
// same zero as a device honestly reporting one, which is how a regression stays
// invisible: this is the pin that would have caught the double's status still
// carrying its pre-item-1 nine fields after the device grew mac.
func TestFakeStatusCarriesEveryFieldTheFirmwareFormats(t *testing.T) {
	src := firmwareFile(t, orbNet)
	body := cFunction(t, src, "h_status")
	keys := map[string]bool{}
	for _, k := range regexp.MustCompile(`\\"([a-z_0-9]+)\\":`).FindAllStringSubmatch(body, -1) {
		keys[k[1]] = true
	}
	if len(keys) < 20 {
		t.Fatalf("only %d keys parsed from h_status — its format string changed shape", len(keys))
	}
	f := orbfake.New()
	srv := httptest.NewServer(f)
	defer srv.Close()
	b, err := clientGet(t, srv, "/api/v1/status")
	if err != nil {
		t.Fatalf("GET status: %v", err)
	}
	var flat map[string]any
	if json.Unmarshal(b, &flat) != nil {
		t.Fatalf("the fake's status is not JSON: %s", b)
	}
	for _, k := range []string{"state", "ip", "mac", "rssi", "fw", "idf", "slot", "img",
		"art_ready", "synced", "tz", "tz_offset", "ap", "assoc_fails", "wifi_reason",
		"wifi_reason_name", "knob", "idle", "apps", "epoch", "heap_internal"} {
		if !keys[k] {
			t.Errorf("the fake reports %q but h_status formats no such key — the double invented a field", k)
			continue
		}
		if _, ok := flat[k]; !ok {
			t.Errorf("h_status formats %q but the fake omits it, so every test on it sees a silent zero", k)
		}
	}
}

// The fake's zone rows must be rows the device actually has, copied rather than
// retyped: a hand-typed POSIX rule is how a picker lands a whole hour off for six
// months a year (orb_net.c:282-288).
func TestFakeZonesAreRowsTheFirmwareShips(t *testing.T) {
	src := firmwareFile(t, orbNet)
	i := strings.Index(src, "static const tz_entry_t s_tz_tbl[] = {")
	if i < 0 {
		t.Fatal("no s_tz_tbl in orb_net.c — the table moved and this pin is blind")
	}
	f := orbfake.New()
	srv := httptest.NewServer(f)
	defer srv.Close()
	b, err := clientGet(t, srv, "/api/v1/config")
	if err != nil {
		t.Fatalf("GET config: %v", err)
	}
	var cfg struct {
		Zones []struct {
			Name  string `json:"name"`
			POSIX string `json:"posix"`
			IANA  string `json:"iana"`
			Label string `json:"label"`
		} `json:"zones"`
	}
	if json.Unmarshal(b, &cfg) != nil || len(cfg.Zones) == 0 {
		t.Fatalf("fake config body = %s", b)
	}
	for _, z := range cfg.Zones {
		if !strings.Contains(src[i:], `"`+z.Name+`",`) {
			t.Errorf("the fake offers zone %q, which the firmware does not ship", z.Name)
		}
		if !strings.Contains(src[i:], `"`+z.POSIX+`"`) {
			t.Errorf("zone %q carries posix %q, which is not the firmware's rule for it", z.Name, z.POSIX)
		}
	}
	// Both branches of the iana rule need coverage, or a fake that always emits it
	// and a client that always falls back both pass and nobody learns which.
	differing, same := 0, 0
	for _, z := range cfg.Zones {
		if z.IANA == "" || z.IANA == z.Name {
			same++
		} else {
			differing++
		}
	}
	if differing == 0 || same == 0 {
		t.Errorf("zones give %d rows with an alias and %d without; the picker needs both", differing, same)
	}
}

// Reboot is counted rather than observed: on hardware the reply goes out before the
// 750 ms timer fires and the socket dies with it, so the response alone cannot prove
// the hub asked exactly once.
func TestRebootIsAcceptedOnceAndReportedAsRebooting(t *testing.T) {
	f := orbfake.New()
	srv := httptest.NewServer(f)
	defer srv.Close()
	c := orbnet.NewClient(2 * time.Second)
	ip := strings.TrimPrefix(srv.URL, "http://")

	b, err := c.Reboot(ip)
	if err != nil {
		t.Fatalf("Reboot: %v", err)
	}
	if string(b) != `{"state":"rebooting"}` {
		t.Errorf("reboot body = %s, want the firmware's reply", b)
	}
	if _, _, _, _, _, _, _, _, rebooted, _ := f.Device(); rebooted != 1 {
		t.Errorf("reboot accepted %d times, want exactly one", rebooted)
	}
}
