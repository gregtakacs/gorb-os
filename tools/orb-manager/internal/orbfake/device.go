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
// orb_net.c:92-95, NET_NAME_MAX at :109, JOIN_BODY_MAX at :2849,
// NET_CONFIG_BODY_MAX and NET_NAME_BODY_MAX at :96 and :114, SCAN_MAX_NETWORKS at
// :1790.
const (
	SSIDMax = 32
	PassMax = 64
	TZMax   = 64
	// NameMax is NET_NAME_MAX, and it is the same number as orbfleet.MaxName on
	// purpose: the card and the device must refuse the 33rd byte at the same point.
	// device_contract_test.go pins all three.
	NameMax       = 32
	JoinBodyMax   = 320
	ConfigBodyMax = 128
	NameBodyMax   = 128
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
	// Name is the device's own, and the DEVICE is the authority for it — the
	// hub's fleet row caches this value rather than owning it. Empty means
	// unnamed, which the firmware reports as "" and never as a placeholder, so a
	// hub test can tell "the owner has not named this board" from "no status read
	// has landed yet".
	Name string

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
	// Mode is the persistent reachability choice POST /api/v1/wifi/mode writes: "" is
	// the station default, "ap" is the isolated mode. It is kept as the stored word
	// rather than as a bool because that is what flash holds, and the firmware's
	// reject list ("AP", "AP-isolated", a 4-byte value) is a statement about the
	// STRING, not about a flag.
	Mode string
	// ModeSets counts accepted mode writes, the same way Rebooted counts reboots: a
	// hub test that asserts the reply alone cannot tell "the orb accepted the mode"
	// from "the hub parsed a 200 and the orb said nothing".
	ModeSets int
	// ModeRefused answers 409, standing in for a firmware built with
	// CONFIG_ORB_NET_AP_ISOLATED=n, where the door exists and refuses by design
	// rather than being absent.
	ModeRefused bool
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
	// Staged and StagedBytes are h_ota's effect: the image a caller last pushed to
	// /api/v1/ota, and its length. A reboot consumes them the way the bootloader
	// consumes a staged slot — FW moves to the staged stamp and the stage clears —
	// so a hub test can assert the two-request shape (stage, then reboot) rather
	// than a double that lies about booting what it never accepted. OTANextFW is
	// the stamp the reboot boots into: the double does not parse app-image bytes,
	// and a test naming the version it staged is the honest way to get one.
	Staged         []byte
	StagedBytes    int
	OTANextFW      string
	RebootedViaOTA bool
	// Reprov counts accepted /api/v1/reprov calls — the D2 intent request. Kept apart
	// from Rebooted because the two mean different things to a caller: one says "come
	// back as you were", the other says "come back in your AP", and a hub test that
	// could not tell them apart could not tell a mis-wired button from a correct one.
	Reprov int
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

// ModeSet reports the stored reachability word ("" = station, "ap" = isolated) and how
// many accepted writes put it there. A hub test asserting only the reply body cannot
// tell a mode the orb stored from a mode the orb refused.
func (o *Orb) ModeSet() (string, int) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.dev == nil {
		return "", 0
	}
	return o.dev.Mode, o.dev.ModeSets
}

// SetModeRefused makes POST /api/v1/wifi/mode answer 409, the CONFIG_ORB_NET_AP_ISOLATED=n
// build: the door is registered and refuses on purpose.
func (o *Orb) SetModeRefused(v bool) {
	o.mu.Lock()
	d := o.deviceLocked()
	d.ModeRefused = v
	o.mu.Unlock()
}

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

// DeviceName snapshots what POST /api/v1/name and the join body left in the double's
// NVS equivalent. A separate accessor rather than an eleventh slot on Device(): that
// signature is already ten positional returns deep, and a caller who wants the name
// should not have to count commas to get it.
func (o *Orb) DeviceName() string {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.deviceLocked().Name
}

// SetDeviceName plants a name in the double's NVS equivalent WITHOUT going over the
// wire — the one thing every other setter here avoids, and the reason it has a
// comment. The hub's cache is now a cache of a fact the device owns, so a test has to
// be able to move the device's copy out from under the hub: that is what the onboarding
// flow, a curl on the bench, or a second hub does. Status and join both echo the same
// single field, so no second copy here can disagree with the first.
func (o *Orb) SetDeviceName(name string) {
	o.mu.Lock()
	o.deviceLocked().Name = name
	o.mu.Unlock()
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
		Name *string `json:"name"`
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
	// The name's pair of guards, same reasoning: absent leaves the stored name
	// alone, and an explicit "" clears it — which is why this branch distinguishes
	// a nil pointer from a pointer to an empty string rather than calling the
	// value truthy.
	if why == "" && req.Name != nil && (nameRejected(*req.Name) || rawNameEscaped(body)) {
		why = "name rejected"
	}
	if why == "" {
		d.Joined.SSID, d.Joined.Pass = ssid, pass
		if req.TZ != nil {
			d.Joined.TZ, d.TZ = *req.TZ, *req.TZ
		}
		if req.Name != nil {
			// Readable through DeviceName(), and deliberately NOT added to the
			// Joined tuple: that is a [3]string at five call sites, and a fourth
			// slot there buys nothing a getter cannot do.
			d.Name = *req.Name
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

// The mode door's three refusals, named so the scrape in door_bodies_test.go can hold
// them against h_wifi_mode's own text. A hand-typed refusal here is a sentence no orb
// has ever sent, which is the exact rot that test exists to catch.
const (
	noModeBody  = `{"error":"mode required (\"ap\" or \"sta\")"}`
	badModeBody = `{"error":"mode must be \"ap\" or \"sta\""}`
	modeOffBody = `{"error":"AP-isolated mode is not in this build"}`
)

// --- wifi/mode (h_wifi_mode) -----------------------------------------------------
//
// The persistent reachability choice, and the only HTTP door out of an isolation the
// page can itself put an orb into. Body is {"mode":"ap"|"sta"}; the reply names the
// mode that was stored rather than echoing the request, so a hub test can tell a
// device that normalised "sta" to the empty default from one that parroted a string.
// It reboots as its last act, exactly like /api/v1/reboot, so the double counts the
// accepted write and leaves the radio state as the new boot would find it: AP-isolated
// means the SoftAP is the only address, and the STA lease is gone.
func (o *Orb) serveWifiMode(w http.ResponseWriter, r *http.Request) {
	// h_wifi_mode's own char body[64]: it refuses content_len > 63 with 413, so the
	// bound is the buffer minus its NUL, not the buffer size.
	body, ok := readBoundedBody(w, r, 63)
	if !ok {
		return
	}
	var req struct {
		Mode *string `json:"mode"`
	}
	if json.Unmarshal(body, &req) != nil || req.Mode == nil {
		writeJSON(w, 400, noModeBody)
		return
	}
	o.mu.Lock()
	d := o.deviceLocked()
	if d.ModeRefused {
		o.mu.Unlock()
		writeJSON(w, 409, modeOffBody)
		return
	}
	o.mu.Unlock()
	// net_mode_set's own accept set: "ap", or "sta"/"" for the station default.
	// Anything else — "AP", "station", 4 bytes — is refused with the firmware's
	// wording, because the exact strings are what the hub's card shows a person.
	mode := *req.Mode
	switch mode {
	case "ap":
	case "sta", "":
		mode = "sta"
	default:
		writeJSON(w, 400, badModeBody)
		return
	}
	o.mu.Lock()
	d = o.deviceLocked()
	d.Mode = "" // "sta" is the absent key, the station default
	if mode == "ap" {
		d.Mode = "ap"
	}
	d.ModeSets++
	if mode == "ap" {
		d.APOpen, d.StaUp, d.ApIP = true, false, "192.168.4.1"
	}
	o.mu.Unlock()
	// 750 ms of reboot grace, then the CDC-less equivalent of dropping the client:
	// the reply is the last thing this boot says on the old mode.
	writeJSON(w, 200, `{"state":"rebooting","mode":"`+mode+`"}`)
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

// nameRejected mirrors name_reject_reason (orb_net.c) including the two checks
// tzRejected has no reason to make: angle brackets, which orbfleet.cleanWord already
// refuses, and the whitespace form. The whitespace rules are here because the hub
// normalizes with strings.Fields BEFORE it dials, and a double that accepted
// "  Hall  " would let a hub test pass against an orb that stores a different
// string than the card displayed — the exact drift the feature exists to remove.
//
// Empty is NOT rejected here, because on POST /api/v1/name an empty value is a
// clear rather than a malformed one; the caller decides which of the two it sent.
func nameRejected(name string) bool {
	if len(name) > NameMax {
		return true
	}
	if name == "" {
		return false
	}
	prev := byte(' ')
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c < 0x20 || c > 0x7e || c == '"' || c == '\\' || c == '<' || c == '>' {
			return true
		}
		if c == ' ' && prev == ' ' {
			return true
		}
		prev = c
	}
	return name[0] == ' ' || name[len(name)-1] == ' '
}

// rawNameEscaped is nameRejected's undecoded-bytes half, for the same reason
// rawTzEscaped exists: encoding/json erases the escape by decoding it, so the only
// way to see {"name":"Hall\u0000Clock"} is to look at the bytes.
func rawNameEscaped(body []byte) bool {
	return rawKeyEscaped(body, "name")
}

// rawKeyEscaped scans the raw value for one key. Shared with tz so the two cannot
// drift apart in the one respect that matters — both must look at the bytes the JSON
// decoder would have laundered.
func rawKeyEscaped(body []byte, key string) bool {
	i := bytes.Index(body, []byte(`"`+key+`"`))
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

// rawTzEscaped is the guard that only the undecoded bytes can give: json_str drops
// the backslash and keeps the character after it, so {"tz":"EST5EDT\t"} reaches the
// device's own guard as the printable EST5EDTt and gets stored. tz_body_value
// refuses the escape outright for that reason (orb_net.c, the "may not contain an
// escape" branch), and a double built on encoding/json would decode and accept it.
func rawTzEscaped(body []byte) bool {
	return rawKeyEscaped(body, "tz")
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

// serveNamePost mirrors h_name_post (orb_net.c), including the one place its
// contract deliberately differs from the config handler above: a body WITHOUT the
// "name" key answers 200 with the name unchanged, because name is one field of
// several a client may be setting, while tz is the only thing /api/v1/config owns
// and a body missing it can only be a broken client. A test that conflates the two
// would pass against a firmware that rejects a request the real device accepts.
func (o *Orb) serveNamePost(w http.ResponseWriter, r *http.Request) {
	body, ok := readBoundedBody(w, r, NameBodyMax)
	if !ok {
		return
	}
	var req struct {
		Name *string `json:"name"`
	}
	if json.Unmarshal(body, &req) != nil {
		writeJSON(w, 400, `{"error":"name rejected"}`)
		return
	}
	o.mu.Lock()
	d := o.deviceLocked()
	if req.Name != nil {
		name := *req.Name
		if name != "" && (nameRejected(name) || rawNameEscaped(body)) {
			o.mu.Unlock()
			writeJSON(w, 400, `{"error":"name rejected"}`)
			return
		}
		d.Name = name
	}
	out := d.Name
	o.mu.Unlock()
	writeJSON(w, 200, fmt.Sprintf(`{"state":"named","name":%q}`, out))
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

// --- ota (h_ota, orb_net.c:3522) -------------------------------------------------
//
// The firmware's rules, mirrored rather than invented: Content-Length is
// mandatory (411 without it), 96 bytes is the floor below which the body is not
// an image (400), and the success body names the slot and points at the reboot
// that activates it. The double stages bytes and waits; it does not apply them
// until serveReboot says so.
func (o *Orb) serveOTA(w http.ResponseWriter, r *http.Request) {
	if r.ContentLength <= 0 {
		writeJSON(w, 411, `{"error":"Content-Length required"}`)
		return
	}
	blob, err := io.ReadAll(io.LimitReader(r.Body, r.ContentLength+1))
	if err != nil || int64(len(blob)) != r.ContentLength {
		writeJSON(w, 400, `{"error":"upload aborted by client"}`)
		return
	}
	if len(blob) < 96 {
		writeJSON(w, 400, `{"error":"body too small to be an image"}`)
		return
	}
	o.mu.Lock()
	d := o.deviceLocked()
	d.Staged, d.StagedBytes = blob, len(blob)
	o.mu.Unlock()
	writeJSON(w, 200, `{"state":"staged","slot":"b","bytes":`+strconv.Itoa(len(blob))+
		`,"reboot":"POST /api/v1/reboot"}`)
}

// OTAState reports what /api/v1/ota last staged and whether a reboot consumed
// it, for the hub-side test that wants the two-request shape rather than its
// own call log.
func (o *Orb) OTAState() (stagedBytes int, bootedStaged bool, fw string) {
	o.mu.Lock()
	defer o.mu.Unlock()
	d := o.deviceLocked()
	return d.StagedBytes, d.RebootedViaOTA, d.FW
}

// SetOTANextFW names the stamp the next staged-and-rebooted image will report,
// the double's honest stand-in for a device that reads the version out of the
// app image it just validated.
func (o *Orb) SetOTANextFW(fw string) { o.mu.Lock(); o.deviceLocked().OTANextFW = fw; o.mu.Unlock() }

// --- reboot (h_reboot, orb_net.c:2552) ------------------------------------------
//
// The device arms a one-shot esp_timer for 750 ms and answers before it fires, so
// the caller gets its reply and then loses the connection. Counting the accepted
// calls lets a test assert the hub asked exactly once.
func (o *Orb) serveReboot(w http.ResponseWriter) {
	o.mu.Lock()
	d := o.deviceLocked()
	d.Rebooted++
	// h_ota staged a slot and h_reboot is the request that activates it: the
	// running stamp moves to what the caller declared, once. A double that moved
	// FW on the upload would let a hub test pass on a client that never learns
	// the reboot is a second request (orb_net.c says so in the body it echoes).
	if d.StagedBytes > 0 {
		d.RebootedViaOTA = true
		if d.OTANextFW != "" {
			d.FW = d.OTANextFW
		}
		d.Staged, d.StagedBytes = nil, 0
	}
	o.mu.Unlock()
	writeJSON(w, 200, `{"state":"rebooting"}`)
}

// serveReprov mirrors h_reprov: both refusals come before any action, and the accepted
// answer is the same body the firmware sends from in front of a 750 ms timer.
//
// The double performs the boot transition in the call, because it has no boot to wait
// for. That is the same compromise serveReboot made from the start, and it is the useful
// one: a hub test that had to poll its way through a fake reboot would be asserting on
// the double's theatre rather than on the hub's handling.
func (o *Orb) serveReprov(w http.ResponseWriter) {
	o.mu.Lock()
	d := o.deviceLocked()
	switch {
	case d.Provisioning || d.State == "provisioning":
		// The firmware's guard is orb_net_is_provisioning(), which is the boot branch's
		// flag and not merely "the AP is up": an orb whose owner opened the AP from the
		// config page is connected, reachable and still worth rebooting into recovery.
		// Refusing on APOpen here would make the double disagree with the device.
		o.mu.Unlock()
		writeJSON(w, 409, `{"error":"already provisioning"}`)
		return
	case o.NoAPFallback:
		o.mu.Unlock()
		writeJSON(w, 409, `{"error":"no AP fallback in this build"}`)
		return
	}
	d.Reprov++
	d.State, d.Provisioning = "provisioning", true
	d.APOpen, d.ApIP, d.StaUp = true, "192.168.4.1", false
	o.mu.Unlock()
	writeJSON(w, 200, `{"state":"rebooting","into":"provisioning"}`)
}

// Reprovisioned reports how many /api/v1/reprov requests the double accepted. Separate
// from Device() rather than another slot in its tuple: that signature has callers across
// the hub's tests, and a reprov counter does not belong with the radio fields.
func (o *Orb) Reprovisioned() int {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.deviceLocked().Reprov
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
	// The device's own name, echoed in the firmware's position — after tz_offset,
	// before ap. jsonSafe because a test steers an odd string into the double
	// directly, which the real device's own guard would have refused on the way in.
	name := jsonSafe(d.Name)
	fw := d.FW
	if fw == "" {
		fw = "orbfake"
	}
	if tz == "" {
		tz, reason = "UTC", "OK"
	}
	// h_status's "apps" array is the broker's pass line, so it is computed under the
	// lock like everything else here and interpolated after: a double that always said
	// "[]" would let a hub test pass against an orb that has apps on it.
	ledger := o.ledgerJSONLocked()
	o.mu.Unlock()
	writeJSON(w, 200, `{"state":"`+state+`","ip":"`+ip+`","mac":"`+mac+
		`","rssi":`+strconv.Itoa(rssi)+`,"fw":"`+fw+`","idf":"v5.4",`+
		`"heap_internal":131072,"heap_internal_largest":65536,"heap_psram":2097152,`+
		`"art_ready":true,"art_bytes":8388608,"art_pages":2048,"art_banks":8,"art_refusals":0,`+
		`"slot":"a","img":"valid","synced":`+fmt.Sprint(state == "connected")+
		`,"epoch":1760000000,"tz":"`+tz+`","tz_offset":`+strconv.Itoa(offset)+
		`,"name":"`+name+`"`+
		`,"ap":`+fmt.Sprint(ap)+`,"assoc_fails":`+strconv.FormatUint(uint64(fails), 10)+
		`,"wifi_reason":0,"wifi_reason_name":"`+jsonSafe(reason)+`",`+
		`"knob":{"raw":4000,"det":100,"spd10":40,"span":9,"valley":2,"ab":3,"vhits":90,"shits":10},`+
		`"idle":{"on":true,"dimmed":false,"since_s":12,"timeout_s":60,"lit_pct":80,`+
		`"dim_pct":20,"wakes":3,"worst_mg":44,"worst_dark_mg":12,"stamps":6,"writes_failed":0},`+
		`"apps":`+ledger+`}`)
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
