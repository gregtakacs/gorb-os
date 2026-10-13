// device.go holds the fake's non-theme surface: the seven routes internal/orbnet
// dials that the theme double predates. Every body here is transcribed from
// main/net/orb_net.c's handler, with the line number named beside it, for the
// reason the package doc gives: a double written from the client agrees with the
// client and proves nothing.
//
// The states a hub needs to exercise are knobs (ScanFailure, JoinFailure, StaUp),
// not query parameters, because orbnet's client has no way to send one.
package orbfake

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
)

// Device limits, copied from orb_net.c so a test can drive the fake into the same
// corner the firmware enforces. NET_SSID_MAX/NET_PASS_MAX/NET_TZ_MAX are at
// orb_net.c:79-81, JOIN_BODY_MAX at :2175, NET_CONFIG_BODY_MAX at :83,
// SCAN_MAX_NETWORKS at :1790.
const (
	SSIDMax       = 32
	PassMax       = 64
	TZMax         = 64
	JoinBodyMax   = 256
	ConfigBodyMax = 128
	ScanMaxNets   = 12
	// APManualS is CONFIG_ORB_NET_AP_MANUAL_S, the seconds a hand-opened AP stays up.
	APManualS = 900
)

// Net is one row of h_wifi_scan's cached table. The field order is the firmware's
// (orb_net.c:2087): ssid, rssi, ch, open.
type Net struct {
	SSID string
	RSSI int
	Ch   int
	Open bool
}

// device is the fake's radio/config state, kept out of Orb's existing fields so
// New's theme arguments and every current call site stay exactly as they were.
type device struct {
	// State is h_status's state: connected | joining | provisioning | down.
	// The firmware derives it from link flags (orb_net.c:1601); a test that wants a
	// specific one sets it directly.
	State string
	// MAC is the field item 1 added to /api/v1/status: lowercase 12 hex, no
	// separators. Fleet identity keys on it and never on IP.
	MAC  string
	IP   string
	RSSI int
	// TZ is the stored zone name; empty means the firmware's "UTC" fallback.
	TZ string
	// TZOffset is what the device measured, not what was stored: h_config_post
	// returns the EFFECT, so a zone newlib degraded to UTC cannot look like a win.
	TZOffset int

	// Networks is /api/v1/wifi/scan's answer once the scan has settled.
	Networks []Net
	// Scanning mirrors the async arm of h_wifi_scan: the first GET says
	// {"status":"scanning"} and nothing else until this clears. It starts true
	// because the firmware's state starts at SCAN_IDLE, where the first GET arms a
	// scan rather than answering it — a double that had a list ready before anyone
	// asked would let the hub pass a test no orb can satisfy.
	Scanning bool
	// ScanFailure makes scan answer 500, standing in for the SCAN_TIMEOUT branch
	// (orb_net.c:2160) and the SCAN_FAILED one (:2171).
	ScanFailure string
	// ScanBusy answers the 409 an in-flight association earns (:2167).
	ScanBusy bool

	// JoinFailure makes /api/v1/wifi/join answer 400 {"error":…} with this reason,
	// standing in for the device rejecting a value it cannot express.
	JoinFailure string
	// Joined records the last accepted join, so a test can assert what the orb was
	// told to associate with. Pass is kept: the hub only ever sends one.
	Joined struct{ SSID, Pass, TZ string }
	// AssocFails and WifiReasonName are h_status's failure fields. ReasonName is a
	// width-varying field: "handshake-timeout" is the longest spelling the
	// firmware has, and it is one of the two reasons the status body is a range.
	AssocFails     uint32
	WifiReason     uint32
	WifiReasonName string

	// StaUp is s_sta_up: whether there is a lease. h_wifi_ap refuses to close the
	// AP without one, and that refusal is the lockout guard worth having a double for.
	StaUp  bool
	APOpen bool
	// ApIP is the SoftAP's own address while it is up. The firmware's responder
	// hands a querier on 192.168.4.0/24 THAT address rather than the STA's
	// (orb_mdns.c build_answer's subnet test), and it is the one half of item 4's
	// join this bench cannot put on air — the AP never raises here, so
	// WIFI_EVENT_AP_START never fires and the announcement it would send has never
	// been seen. Hosting it in the fake is how the per-interface A record gets
	// tested at all; it is not evidence about the device.
	ApIP string
	// FW is the version string both readers must agree on: h_status's fw and the
	// TXT fw= the browser reads. One field, read twice, because two copies is how
	// a fake device and a fake browser start agreeing with each other instead of
	// with the orb. The device fills both from esp_app_get_description() once.
	FW string
	// Provisioning is orb_mdns_id_t.provisioning: the SoftAP is up and no lease
	// has landed, which is item 10's factory-reset case and the reason the TXT
	// carries a state at all.
	Provisioning bool

	// Rebooted counts accepted /api/v1/reboot calls.
	Rebooted int
	// Screened records each accepted /api/v1/screen argument, in order.
	Screened []string
}

// deviceLocked returns the radio state, defaulting it on first use. Callers hold o.mu.
func (o *Orb) deviceLocked() *device {
	if o.dev == nil {
		o.dev = &device{State: "connected", MAC: "a4cf12aa11bb", IP: "192.0.2.7",
			// FW is the one place the fake's version lives: /api/v1/status and
			// the TXT fw= both read it, so they cannot drift apart into the
			// self-agreeing pair this package's doc comment was written against.
			FW: "orbfake", RSSI: -52, TZ: "US/Eastern", TZOffset: 300, WifiReasonName: "OK",
			Scanning: true,
			Networks: []Net{{SSID: "lab-2g", RSSI: -47, Ch: 6, Open: false}}}
	}
	return o.dev
}

// SetState sets the state /api/v1/status reports: connected, joining, provisioning
// or down.
func (o *Orb) SetState(s string) { o.mu.Lock(); d := o.deviceLocked(); d.State = s; o.mu.Unlock() }

// SetSTA sets the lease bit that both h_wifi_ap's close path and h_status read.
func (o *Orb) SetSTA(up bool) { o.mu.Lock(); d := o.deviceLocked(); d.StaUp = up; o.mu.Unlock() }

// SetAddress sets the MAC and the address /api/v1/status reports, and so the
// instance name the mDNS announcement derives from the same two facts. A test that
// wants an orb whose browse row and status row agree sets them here, once; there is
// deliberately no separate setter for the announcement's copy, because two setters
// is two fields that can disagree.
func (o *Orb) SetAddress(mac, ip string) {
	o.mu.Lock()
	d := o.deviceLocked()
	d.MAC, d.IP = mac, ip
	o.mu.Unlock()
}

// SetFirmware sets the version h_status reports as fw and the TXT fw= the browser
// reads. One call, one field: a double whose announcement and status could carry
// different versions is the double that hides a wrong stamp in one of them.
func (o *Orb) SetFirmware(fw string) { o.mu.Lock(); o.deviceLocked().FW = fw; o.mu.Unlock() }

// SetSoftAP raises or lowers the provisioning AP and the address it answers for.
// The device derives both from the driver; a test names them.
func (o *Orb) SetSoftAP(up bool, ip string) {
	o.mu.Lock()
	d := o.deviceLocked()
	d.APOpen, d.ApIP = up, ip
	o.mu.Unlock()
}

// SetProvisioning is orb_mdns_set_provisioning: the state= word of the
// announcement, and the one thing that tells a hub a factory-reset orb from a
// joined one without dialing an address no wired hub can route to.
func (o *Orb) SetProvisioning(p bool) { o.mu.Lock(); o.deviceLocked().Provisioning = p; o.mu.Unlock() }

// SetNetworks installs scan results and settles the scan, as scan_collect does.
func (o *Orb) SetNetworks(nets ...Net) {
	o.mu.Lock()
	d := o.deviceLocked()
	d.Networks = append([]Net(nil), nets...)
	d.Scanning = false
	o.mu.Unlock()
}

// SetScanFailure makes every scan answer 500 until cleared. Both of the firmware's
// failure states are permanent until a ?fresh=1 re-arms them, and the hub has to
// cope with that rather than with a one-off blip.
func (o *Orb) SetScanFailure(on bool) { o.SetScanFailureReason(ternary(on, "scan failed", "")) }

// SetScanFailureReason chooses which of the two failure bodies the next scan answers:
// "scan timed out" is the SCAN_TIMEOUT branch (orb_net.c:2160, the driver never
// answered SCAN_DONE inside the window) and anything else stands for SCAN_FAILED
// (:2171). They read the same to a page and differ in what an owner should suspect,
// so a double that could only produce one would leave the other untested.
func (o *Orb) SetScanFailureReason(why string) {
	o.mu.Lock()
	d := o.deviceLocked()
	d.ScanFailure, d.ScanBusy, d.Scanning = why, false, false
	o.mu.Unlock()
}

func ternary(cond bool, a, b string) string {
	if cond {
		return a
	}
	return b
}

// SetScanBusy answers the 409 that an association in progress earns.
func (o *Orb) SetScanBusy(on bool) {
	o.mu.Lock()
	d := o.deviceLocked()
	d.ScanBusy, d.Scanning = on, false
	o.mu.Unlock()
}

// SetJoinFailure makes the next join answer 400 with this reason, standing in for
// the device refusing a value the fake cannot validate on its own.
func (o *Orb) SetJoinFailure(why string) {
	o.mu.Lock()
	o.deviceLocked().JoinFailure = why
	o.mu.Unlock()
}

// Device snapshots the radio and config state a test steered into the fake.
func (o *Orb) Device() (state, mac, ip, tz string, offset int, staUp, apOpen, scanning bool, rebooted int, joined [3]string) {
	o.mu.Lock()
	defer o.mu.Unlock()
	d := o.deviceLocked()
	return d.State, d.MAC, d.IP, d.TZ, d.TZOffset, d.StaUp, d.APOpen, d.Scanning, d.Rebooted,
		[3]string{d.Joined.SSID, d.Joined.Pass, d.Joined.TZ}
}

// ScreenCalls returns the /api/v1/screen arguments accepted so far, in order.
func (o *Orb) ScreenCalls() []string {
	o.mu.Lock()
	defer o.mu.Unlock()
	return append([]string(nil), o.deviceLocked().Screened...)
}

// --- wifi/scan (h_wifi_scan, orb_net.c:2136) ------------------------------------
//
// The firmware's scan is a state machine and so is the fake, because that shape is
// the bug class this double exists to catch: httpd has ONE worker, so a blocking
// scan starved the status poll and the timezone fetch behind it and the page hung
// on "loading…" (orb_net.c:1772-1781). The fix made scan answer
// {"status":"scanning"} at HTTP 200 — 200 on purpose, so the page's shared fetch
// helper treats it as a reply and not an error. A double that answered the network
// list synchronously would let the hub pass a test no real orb could satisfy.
func (o *Orb) serveWifiScan(w http.ResponseWriter, r *http.Request) {
	o.mu.Lock()
	d := o.deviceLocked()
	// ?fresh=1 re-arms from any settled state but is a no-op mid-scan, so two
	// eager taps cannot stack scans (orb_net.c:2141).
	if r.URL.Query().Has("fresh") && !d.Scanning {
		d.Scanning = true
	}
	scan, busy, fail := d.Scanning, d.ScanBusy, d.ScanFailure
	var body string
	if !scan && !busy && fail == "" {
		body = scanRows(d.Networks)
	}
	o.mu.Unlock()
	switch {
	case scan:
		writeJSON(w, 200, `{"status":"scanning"}`)
	case busy:
		writeJSON(w, 409, `{"error":"busy", "detail":"association in progress"}`)
	case fail != "":
		writeJSON(w, 500, `{"error":`+quoteJSONString(fail)+`}`)
	default:
		writeJSON(w, 200, body)
	}
}

// scanRows formats the cached table as scan_collect renders s_scan_json
// (orb_net.c:2078-2103): ssid quoted, rssi and ch bare, open a JSON bool. The
// firmware commits an entry only if it fits, because a half-written one parses as
// nothing and renders as an empty network list; this loop stops clean at
// SCAN_MAX_NETWORKS for the same reason.
func scanRows(nets []Net) string {
	var b strings.Builder
	b.WriteString(`{"networks":[`)
	for i, n := range nets {
		if i >= ScanMaxNets {
			break
		}
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, `{"ssid":"%s","rssi":%d,"ch":%d,"open":%t}`,
			jsonSafe(n.SSID), n.RSSI, n.Ch, n.Open)
	}
	b.WriteString("]}")
	return b.String()
}

// --- wifi/join (h_wifi_join, orb_net.c:2177) ------------------------------------
//
// The body caps at JOIN_BODY_MAX, and an overlong value is REJECTED rather than
// truncated (json_str, orb_net.c:2745's rule): a 40-char SSID quietly becoming its
// first 32 sends the orb hunting for a network that does not exist, which the user
// sees as an unexplained join failure. A missing pass is an open network, not an
// error (orb_net.c:2221). tz is absent means "leave the stored zone alone"; {"tz":""}
// is what clears it (:2230).
//
// On hardware the reply usually dies with the connection, because applying
// credentials calls esp_wifi_disconnect() on the socket carrying it
// (orb_net.c:2270-2283). The double answers cleanly; the hub's tolerance for a lost
// reply is the thing to test against a real orb, not invented here.
func (o *Orb) serveWifiJoin(w http.ResponseWriter, r *http.Request) {
	body, ok := readBoundedBody(w, r, JoinBodyMax)
	if !ok {
		return
	}
	var req struct {
		SSID *string `json:"ssid"`
		Pass *string `json:"pass"`
		TZ   *string `json:"tz"`
	}
	if json.Unmarshal(body, &req) != nil {
		writeJSON(w, 400, `{"error":"ssid required or too long"}`)
		return
	}
	ssid, pass, tz := dereq(req.SSID), dereq(req.Pass), dereq(req.TZ)
	if ssid == "" || len(ssid) > SSIDMax || len(pass) > PassMax || len(tz) > TZMax {
		writeJSON(w, 400, `{"error":"ssid required or too long"}`)
		return
	}
	o.mu.Lock()
	d := o.deviceLocked()
	why := d.JoinFailure
	// Two guards, both the firmware's: the decoded value must be printable ASCII
	// with no quote or backslash and within TZMax (tz_reject_reason), and the RAW
	// body must contain no escape in the tz value at all (tz_body_value). The second
	// is what encoding/json erases by decoding, so it is checked on the bytes.
	if why == "" && req.TZ != nil && (tzRejected(*req.TZ) || rawTzEscaped(body)) {
		why = "tz rejected"
	}
	if why == "" {
		d.Joined.SSID, d.Joined.Pass = ssid, pass
		if req.TZ != nil {
			d.Joined.TZ, d.TZ = *req.TZ, *req.TZ
		}
		d.State = "joining"
		d.AssocFails = 0 // a human just said "try these": the back-off ladder restarts
		d.Scanning = false
	}
	o.mu.Unlock()
	if why != "" {
		writeJSON(w, 400, `{"error":`+quoteJSONString(why)+`}`)
		return
	}
	writeJSON(w, 200, `{"state":"joining"}`)
}

// --- wifi/ap (h_wifi_ap, orb_net.c:2322) ----------------------------------------
//
// The body is optional: {} opens for CONFIG_ORB_NET_AP_MANUAL_S seconds. Both
// spellings of the close are accepted, because the page sends {"on":"off"} and a
// person at a curl prompt sends {"on":false} (orb_net.c:2348). Closing is refused
// without a lease: an orb reachable by no other route is precisely the lockout the
// rescue path exists for, so "ok, closed" there would be actively harmful
// (orb_net.c:2363-2372).
func (o *Orb) serveWifiAP(w http.ResponseWriter, r *http.Request) {
	body, ok := readBoundedBody(w, r, ConfigBodyMax)
	if !ok {
		return
	}
	closeNow := false
	if strings.TrimSpace(string(body)) != "" {
		var req struct {
			On any `json:"on"`
		}
		if json.Unmarshal(body, &req) == nil {
			closeNow = spelledOff(req.On)
		}
	}
	o.mu.Lock()
	d := o.deviceLocked()
	if closeNow {
		if !d.StaUp {
			o.mu.Unlock()
			writeJSON(w, 409, `{"error":"no lease — the AP is the only way in"}`)
			return
		}
		d.APOpen = false
		o.mu.Unlock()
		writeJSON(w, 200, `{"state":"ap_closing"}`)
		return
	}
	suffix := ""
	if len(d.MAC) >= 4 {
		suffix = strings.ToUpper(d.MAC[len(d.MAC)-4:])
	}
	d.APOpen = true
	o.mu.Unlock()
	// The SSID is in the reply because a phone has to find it in a network list, and
	// the seconds because "how long do I have" is the first question a person
	// standing at a wall with a phone asks (orb_net.c:2381).
	writeJSON(w, 200, fmt.Sprintf(`{"state":"ap_opening","ap_ssid":"orb-%s","seconds":%d}`,
		suffix, APManualS))
}

// spelledOff accepts the two spellings one intent has, matching the firmware's
// off/false/0 set (orb_net.c:2350-2357) plus the JSON bool a curl user types.
func spelledOff(v any) bool {
	switch t := v.(type) {
	case bool:
		return !t
	case string:
		return t == "off" || t == "false" || t == "0"
	}
	return false
}

// --- config (h_config_get :1650, h_config_post :1708) ---------------------------
//
// Zone is one row of the picker. IANA is emitted only where it differs from Name:
// the canonical spelling IS the key for most rows, and at 62 rows the body is
// dominated by that repetition (orb_net.c:1673-1676). Clients fall back with
// z.iana || z.name, so dropping the condition makes a picker that cannot be
// distinguished from a broken one — hence the subset below carries both kinds.
type Zone struct {
	Name  string
	POSIX string
	IANA  string
	Label string
}

// zones is a subset of the firmware's s_tz_tbl (orb_net.c:267), copied verbatim
// rather than hand-typed, and TestFakeZonesMatchTheFirmwaresTable checks each row
// against the firmware source so a drift here fails the build. A subset is enough
// for the hub: it renders whatever it is handed and the picker's behaviour does not
// depend on carrying all 62 rows in a test double.
var zones = []Zone{
	{"UTC", "UTC0", "UTC", "UTC"},
	{"US/Eastern", "EST5EDT,M3.2.0,M11.1.0", "America/New_York", "New York"},
	{"US/Pacific", "PST8PDT,M3.2.0,M11.1.0", "America/Los_Angeles", "Los Angeles"},
	{"US/Arizona", "MST7", "America/Phoenix", "Phoenix (no clock change)"},
	{"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0", "Europe/London", "London"},
	{"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Berlin", "Berlin"},
	{"Asia/Tokyo", "JST-9", "Asia/Tokyo", "Tokyo"},
	{"Asia/Kolkata", "KOLKA-5:30", "Asia/Kolkata", "Kolkata"},
	{"Pacific/Honolulu", "HONOL10", "Pacific/Honolulu", "Honolulu"},
	{"America/Santiago", "SANTI4SANTID3,M9.1.0/0,M4.1.0/0", "America/Santiago", "Santiago"},
}

func tzKnown(name string) bool {
	if name == "" || len(name) > TZMax {
		return false
	}
	for _, z := range zones {
		if z.Name == name {
			return true
		}
	}
	return !tzRejected(name) // any well-formed raw POSIX rule is a legal value
}

// tzRejected mirrors tz_reject_reason (orb_net.c:1247): printable ASCII only, no
// quote or backslash, non-empty and within TZMax. It deliberately does NOT consult
// the zone table, because the endpoint accepts any raw POSIX rule, and a double that
// demanded a table hit would 400 requests the device applies.
func tzRejected(tz string) bool {
	if tz == "" || len(tz) > TZMax {
		return true
	}
	for i := 0; i < len(tz); i++ {
		c := tz[i]
		if c < 0x20 || c > 0x7e || c == '"' || c == '\\' {
			return true
		}
	}
	return false
}

// rawTzEscaped is the guard that only the undecoded bytes can give: json_str drops
// the backslash and keeps the character after it, so {"tz":"EST5EDT\t"} reaches the
// device's own guard as the printable EST5EDTt and gets stored. tz_body_value
// refuses the escape outright for that reason (orb_net.c, the "may not contain an
// escape" branch), and a double built on encoding/json would decode and accept it.
func rawTzEscaped(body []byte) bool {
	i := bytes.Index(body, []byte(`"tz"`))
	if i < 0 {
		return false
	}
	rest := body[i:]
	q := bytes.IndexByte(rest, ':')
	if q < 0 {
		return false
	}
	inner := rest[q+1:]
	end := bytes.IndexByte(inner, '"')
	if end < 0 {
		end = len(inner)
	}
	return bytes.IndexByte(inner[:end], '\\') >= 0
}

func (o *Orb) serveConfigGet(w http.ResponseWriter) {
	o.mu.Lock()
	tz := o.deviceLocked().TZ
	offset := o.deviceLocked().TZOffset
	o.mu.Unlock()
	if tz == "" {
		tz = "UTC"
	}
	var b strings.Builder
	b.WriteString(`{"zones":[`)
	for i, z := range zones {
		if i > 0 {
			b.WriteByte(',')
		}
		b.WriteString(`{"name":"` + z.Name + `","posix":"` + z.POSIX + `",`)
		if z.IANA != z.Name {
			b.WriteString(`"iana":"` + z.IANA + `",`)
		}
		b.WriteString(`"label":"` + jsonSafe(z.Label) + `"}`)
	}
	// The tail repeats the row's own words: the live zone, its resolved rule, and
	// the measured offset that proves a valid-looking-but-degenerate rule took.
	fmt.Fprintf(&b, `],"tz":%q,"posix":%q,"tz_offset":%d}`, tz, posixFor(tz), offset)
	writeJSON(w, 200, b.String())
}

func posixFor(name string) string {
	for _, z := range zones {
		if z.Name == name {
			return z.POSIX
		}
	}
	return name // already a raw POSIX rule
}

func (o *Orb) serveConfigPost(w http.ResponseWriter, r *http.Request) {
	body, ok := readBoundedBody(w, r, ConfigBodyMax)
	if !ok {
		return
	}
	// {"tz":""} is a real request meaning clear to UTC; a body WITHOUT the key is
	// not, because tz is the only setting this endpoint owns and answering 200 to {}
	// turns a client bug into a silent no-op the page reports as saved
	// (orb_net.c:1741-1746). The reason goes to the device log, never the reply.
	var req struct {
		TZ *string `json:"tz"`
	}
	if json.Unmarshal(body, &req) != nil || req.TZ == nil {
		writeJSON(w, 400, `{"error":"tz rejected"}`)
		return
	}
	tz := *req.TZ
	if tz != "" && (tzRejected(tz) || rawTzEscaped(body)) {
		writeJSON(w, 400, `{"error":"tz rejected"}`)
		return
	}
	o.mu.Lock()
	d := o.deviceLocked()
	d.TZ = tz
	d.TZOffset = offsetFor(tz)
	offset := d.TZOffset
	o.mu.Unlock()
	shown := tz
	if shown == "" {
		shown = "UTC"
	}
	writeJSON(w, 200, fmt.Sprintf(`{"state":"zoned","tz":%q,"posix":%q,"tz_offset":%d}`,
		shown, posixFor(shown), offset))
}

// offsetFor is the fake's stand-in for tz_offset_min: the hub needs a number that
// changes with the zone, which is the only way to test that a page reports the
// effect rather than an echo of what it sent.
func offsetFor(tz string) int {
	switch tz {
	case "", "UTC":
		return 0
	case "US/Eastern":
		return 300
	case "US/Pacific":
		return 480
	case "Europe/Berlin":
		return -60
	case "Asia/Tokyo":
		return -540
	}
	return 0
}

// --- reboot (h_reboot, orb_net.c:2552) ------------------------------------------
//
// The device arms a one-shot esp_timer for 750 ms and answers before it fires, so
// the caller gets its reply and then loses the connection. Counting the accepted
// calls lets a test assert the hub asked exactly once.
func (o *Orb) serveReboot(w http.ResponseWriter) {
	o.mu.Lock()
	d := o.deviceLocked()
	d.Rebooted++
	o.mu.Unlock()
	writeJSON(w, 200, `{"state":"rebooting"}`)
}

// --- screen (h_screen, orb_net.c:2657) -------------------------------------------
//
// Screen switch and synthetic knob events share one endpoint on purpose: they are
// one test (go to the menu, arm it, scroll, choose), and splitting them would make
// the caller trust ordering across two URLs (orb_net.c:2671-2673).
//
// The known-app list is the registry's own slugs, and "hue" is the color picker's
// registered alias. It is device-owned text: the firmware logs an unknown to= and
// puts only its own words in the body, because repeating caller-controlled text is
// how a quoting bug becomes someone else's problem (orb_net.c:2684-2692).
const knownApps = `["clock","color_picker","hue","menu"]`

func (o *Orb) serveScreen(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	to := q.Get("to")
	haveTo, haveTurn := q.Has("to"), q.Has("turn")
	turn, push, hold := q.Get("turn"), q.Get("push"), q.Get("hold")

	if haveTo && !knownApp(to) {
		writeJSON(w, 400, `{"error":"unknown app","known":`+knownApps+`}`)
		return
	}
	if !haveTo && !haveTurn && push == "" && hold == "" {
		writeJSON(w, 400, `{"error":"to, turn, push or hold required"}`)
		return
	}
	o.mu.Lock()
	d := o.deviceLocked()
	for _, kv := range [][2]string{{"to", to}, {"turn", turn}, {"push", push}, {"hold", hold}} {
		if kv[1] != "" {
			d.Screened = append(d.Screened, kv[0]+"="+kv[1])
		}
	}
	o.mu.Unlock()
	writeJSON(w, 200, `{"state":"accepted"}`)
}

func knownApp(slug string) bool {
	switch slug {
	case "clock", "color_picker", "hue", "menu":
		return true
	}
	return false
}

// --- status (h_status, orb_net.c:1378) -------------------------------------------

// serveStatus renders the same field list and order as the firmware's snprintf
// (orb_net.c:1554-1600), including the mac item 1 added and the two fields whose
// width varies between boots — wifi_reason_name and idle.dimmed — which is why the
// body is a range and not a constant. The knob and idle blocks are here because the
// hub parses status with struct tags, and a field the device never sent decodes to
// the same zero as a device honestly reporting one.
func (o *Orb) serveStatus(w http.ResponseWriter) {
	o.mu.Lock()
	d := o.deviceLocked()
	state, mac, ip := d.State, d.MAC, d.IP
	rssi, tz, offset := d.RSSI, d.TZ, d.TZOffset
	reason, fails, ap := d.WifiReasonName, d.AssocFails, d.APOpen
	fw := d.FW
	if fw == "" {
		fw = "orbfake"
	}
	if tz == "" {
		tz, reason = "UTC", "OK"
	}
	o.mu.Unlock()
	writeJSON(w, 200, `{"state":"`+state+`","ip":"`+ip+`","mac":"`+mac+
		`","rssi":`+strconv.Itoa(rssi)+`,"fw":"`+fw+`","idf":"v5.4",`+
		`"heap_internal":131072,"heap_internal_largest":65536,"heap_psram":2097152,`+
		`"art_ready":true,"art_bytes":8388608,"art_pages":2048,"art_banks":8,"art_refusals":0,`+
		`"slot":"a","img":"valid","synced":`+fmt.Sprint(state == "connected")+
		`,"epoch":1760000000,"tz":"`+tz+`","tz_offset":`+strconv.Itoa(offset)+
		`,"ap":`+fmt.Sprint(ap)+`,"assoc_fails":`+strconv.FormatUint(uint64(fails), 10)+
		`,"wifi_reason":0,"wifi_reason_name":"`+jsonSafe(reason)+`",`+
		`"knob":{"raw":4000,"det":100,"spd10":40,"span":9,"valley":2,"ab":3,"vhits":90,"shits":10},`+
		`"idle":{"on":true,"dimmed":false,"since_s":12,"timeout_s":60,"lit_pct":80,`+
		`"dim_pct":20,"wakes":3,"worst_mg":44,"worst_dark_mg":12,"stamps":6,"writes_failed":0},`+
		`"apps":[]}`)
}

// --- shared helpers --------------------------------------------------------------

// readBoundedBody enforces the two rejections every POST handler in the firmware
// makes before it parses anything: a missing Content-Length is a 400, and an
// overlong body is a 413 rather than a clip (h_wifi_join :2192-2201, h_config_post
// :1721-1728, h_wifi_ap :2331). Clipping is the worse failure mode in each case.
func readBoundedBody(w http.ResponseWriter, r *http.Request, max int) ([]byte, bool) {
	if r.ContentLength < 0 {
		writeJSON(w, 400, `{"error":"content-length required"}`)
		return nil, false
	}
	if r.ContentLength > int64(max) {
		writeJSON(w, 413, `{"error":"body too large"}`)
		return nil, false
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, int64(max)+1))
	if err != nil || len(body) > max {
		writeJSON(w, 400, `{"error":"body read failed"}`)
		return nil, false
	}
	return body, true
}

func dereq(s *string) string {
	if s == nil {
		return ""
	}
	return *s
}

func quoteJSONString(s string) string {
	b, err := json.Marshal(s)
	if err != nil {
		return `""`
	}
	return string(b)
}
