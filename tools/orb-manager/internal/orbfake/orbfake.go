// Package orbfake is a stand-in orb that answers the theme routes exactly as
// main/net/orb_net.c registers them. It is written from the firmware, not from the
// hub's client, and that distinction is the whole point of it: the first version of
// this double was built by reading themes.go, so it answered ?name=, the client kept
// sending ?name=, and the pair agreed with each other while every real orb answered
// 400. A mock that mirrors the code under test proves the code agrees with itself.
//
// So this file quotes orb_net.c's h_sd / h_theme_install / h_theme_del / h_theme_upload
// and keeps its OWN copy of the 12-hex identity rule (idOK, themeID) rather than
// importing orbnet's. When the two copies drift, the contract test fails, which is the
// only arrangement that can catch client-and-double agreeing on the wrong thing.
package orbfake

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"strings"
	"sync"
)

// Theme is one card row, holding the fields h_sd actually formats: the owner's name,
// the id derived from it, and the two load-state bits a page needs.
type Theme struct {
	Name    string
	ID      string
	Why     string
	Bytes   int
	Serial  int
	OK      bool
	Active  bool
	Changed bool
}

// Orb is the fake. The zero value is not useful; build one with New.
type Orb struct {
	mu        sync.Mutex
	themes    []*Theme
	active    string
	live      string
	tryID     string
	tryWhy    string
	damageID  string
	damageWhy string
	calls     []string

	// Card is the SD card id h_sd reports when mounted.
	Card string
	// Mounted answers "is there a card at all".
	Mounted bool
	// Verbose logs every request it answers, for the live cmd/orbfake run.
	Verbose bool
	// Busy makes the next install or upload answer 409 the way the firmware does when a
	// job is already on the worker. It is a knob rather than a query parameter because
	// the hub's client has no way to send one, and a double you cannot drive into the
	// awkward state from the caller's side is a double that never tests that state.
	Busy bool
	// NoAPFallback stands in for CONFIG_ORB_NET_AP_FALLBACK=n: /api/v1/reprov refuses
	// instead of arming. Same reason as Busy — no caller on the LAN can ask for that
	// build, and "the hub handles a refusal gracefully" is exactly the test a fleet of
	// ordinary orbs would never exercise on its own.
	NoAPFallback bool

	// dev is the radio/config/screen state, defaulted on first use by deviceLocked.
	dev *device

	// apps is the P4 card-app store, keyed by slug, with appsOrder holding the
	// registration order so the installed tier's slot field is positional like the
	// device's. Nil until WithCardApps or an upload seeds it, which is what makes a
	// fresh double answer "no apps" the way a fresh device does.
	apps      map[string]*CardApp
	appsOrder []string
}

// New returns a fake orb seeded with the given themes, ids recomputed from each name
// so a caller cannot seed a row whose id disagrees with its name.
func New(themes ...Theme) *Orb {
	o := &Orb{Card: "SDSC-8G", Mounted: true, live: "default",
		apps: map[string]*CardApp{}}
	for i := range themes {
		t := themes[i]
		t.ID = themeID(t.Name)
		o.themes = append(o.themes, &t)
		if t.Active {
			o.active = t.Name
			o.live = "sd"
		}
	}
	return o
}

// Calls returns the request line of everything dialed so far, for a test to assert on.
func (o *Orb) Calls() []string {
	o.mu.Lock()
	defer o.mu.Unlock()
	out := make([]string, len(o.calls))
	copy(out, o.calls)
	return out
}

// SetTry records the last refused install, the way the firmware does, so the card can
// be pointed at it by id.
func (o *Orb) SetTry(id, why string) { o.mu.Lock(); o.tryID, o.tryWhy = id, why; o.mu.Unlock() }

// SetDamage records what boot could not load.
func (o *Orb) SetDamage(id, why string) {
	o.mu.Lock()
	o.damageID, o.damageWhy = id, why
	o.mu.Unlock()
}

// Themes snapshots the card.
func (o *Orb) Themes() []Theme {
	o.mu.Lock()
	defer o.mu.Unlock()
	out := make([]Theme, 0, len(o.themes))
	for _, t := range o.themes {
		out = append(out, *t)
	}
	return out
}

func (o *Orb) record(r *http.Request) {
	o.mu.Lock()
	line := r.Method + " " + r.URL.Path
	if r.URL.RawQuery != "" {
		line += "?" + r.URL.RawQuery
	}
	o.calls = append(o.calls, line)
	if len(o.calls) > 200 {
		o.calls = o.calls[len(o.calls)-200:]
	}
	if o.Verbose {
		log.Printf("orbfake: %s", line)
	}
	o.mu.Unlock()
}

func writeJSON(w http.ResponseWriter, code int, body string) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	_, _ = io.WriteString(w, body)
}

// ServeHTTP routes exactly the URIs orb_net.c's uri table registers for the theme
// surface, with the same methods. An unknown route is a 404 in the firmware's shape.
func (o *Orb) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	o.record(r)
	switch {
	case r.URL.Path == "/api/v1/status" && r.Method == http.MethodGet:
		o.serveStatus(w)
	case r.URL.Path == "/api/v1/wifi/scan" && r.Method == http.MethodGet:
		o.serveWifiScan(w, r)
	case r.URL.Path == "/api/v1/wifi/join" && r.Method == http.MethodPost:
		o.serveWifiJoin(w, r)
	case r.URL.Path == "/api/v1/wifi/ap" && r.Method == http.MethodPost:
		o.serveWifiAP(w, r)
	case r.URL.Path == "/api/v1/config" && r.Method == http.MethodGet:
		o.serveConfigGet(w)
	case r.URL.Path == "/api/v1/config" && r.Method == http.MethodPost:
		o.serveConfigPost(w, r)
	case r.URL.Path == "/api/v1/reboot" && r.Method == http.MethodPost:
		o.serveReboot(w)
	case r.URL.Path == "/api/v1/reprov" && r.Method == http.MethodPost:
		o.serveReprov(w)
	case r.URL.Path == "/api/v1/screen" && r.Method == http.MethodGet:
		o.serveScreen(w, r)
	case r.URL.Path == "/api/v1/sd" && r.Method == http.MethodGet:
		o.serveSD(w)
	case r.URL.Path == "/api/v1/sd/install" && r.Method == http.MethodPost:
		o.serveInstall(w, r)
	case r.URL.Path == "/api/v1/sd/default" && r.Method == http.MethodPost:
		writeJSON(w, 200, `{"state":"default","live":"default","reboot":"automatic"}`)
	case r.URL.Path == "/api/v1/sd/delete" && r.Method == http.MethodPost:
		o.serveDelete(w, r)
	case r.URL.Path == "/api/v1/sd/theme" && r.Method == http.MethodPost:
		o.serveUpload(w, r)
	case r.URL.Path == "/api/v1/sd/rescan" && r.Method == http.MethodPost:
		writeJSON(w, 200, fmt.Sprintf(`{"mounted":true,"card":%q}`, o.Card))
	// The P4 card-app verbs. Registered here in the same order orb_net.c's uri table
	// lists them, so a route the firmware grows and this switch does not fails the
	// contract test rather than quietly answering 404 like a made-up path.
	case r.URL.Path == "/api/v1/apps" && r.Method == http.MethodGet:
		o.serveApps(w)
	case r.URL.Path == "/api/v1/apps/install" && r.Method == http.MethodPost:
		o.serveAppInstall(w, r)
	case r.URL.Path == "/api/v1/apps/enable" && r.Method == http.MethodPost:
		o.serveAppEnable(w, r)
	case r.URL.Path == "/api/v1/apps/uninstall" && r.Method == http.MethodPost:
		o.serveAppUninstall(w, r)
	case r.URL.Path == "/api/v1/sd/app" && r.Method == http.MethodPost:
		o.serveAppUpload(w, r)
	default:
		writeJSON(w, 404, `{"error":"no such route on this orb"}`)
	}
}

// serveSD formats h_sd's reply: the row shape is name, id, why, bytes, serial, ok,
// active, changed — there is no title field, and try/damage key on _id.
func (o *Orb) serveSD(w http.ResponseWriter) {
	o.mu.Lock()
	defer o.mu.Unlock()
	var b strings.Builder
	fmt.Fprintf(&b, `{"mounted":%t,"card":%q,"uploading":false,"total_mb":7581,"free_mb":7100,`+
		`"source":%q,"active":%q,"live":%q,"card_count":%d,"try_id":%q,"try_why":%q,`+
		`"damage_id":%q,"damage_why":%q,"themes":[`,
		o.Mounted, o.MountedCard(), o.sourceLocked(), o.active, o.live, len(o.themes),
		o.tryID, o.tryWhy, o.damageID, o.damageWhy)
	for i, t := range o.themes {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, `{"name":%q,"id":%q,"why":%q,"bytes":%d,"serial":%d,"ok":%t,"active":%t,"changed":%t}`,
			t.Name, t.ID, t.Why, t.Bytes, t.Serial, t.OK, t.Active, t.Changed)
	}
	b.WriteString("]}")
	writeJSON(w, 200, b.String())
}

func (o *Orb) MountedCard() string {
	if !o.Mounted {
		return ""
	}
	return o.Card
}

func (o *Orb) sourceLocked() string {
	if o.active != "" {
		return "sd"
	}
	return "default"
}

// serveInstall is h_theme_install: ?id= is required and must pass orb_theme_id_ok,
// or the answer is the firmware's 400 and the card is never touched.
func (o *Orb) serveInstall(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Query().Get("id")
	if !idOK(id) {
		writeJSON(w, 400, `{"error":"id is missing or not a theme id"}`)
		return
	}
	if o.takeBusy() {
		writeJSON(w, 409, `{"error":"a theme job is already running"}`)
		return
	}
	t := o.find(id)
	if t == nil {
		writeJSON(w, 404, `{"error":"no such theme on the card"}`)
		return
	}
	if !t.OK {
		writeJSON(w, 422, fmt.Sprintf(`{"error":"job refused: %s"}`, jsonSafe(t.Why)))
		return
	}
	o.mu.Lock()
	for _, x := range o.themes {
		x.Active = false
	}
	t.Active = true
	o.active = t.Name
	o.live = "sd"
	o.mu.Unlock()
	writeJSON(w, 200, fmt.Sprintf(`{"state":"installing","id":%q,"reboot":"automatic"}`, id))
}

// serveDelete is h_theme_del: same id gate, same 404 wording.
func (o *Orb) serveDelete(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Query().Get("id")
	if !idOK(id) {
		writeJSON(w, 400, `{"error":"id is missing or not a theme id"}`)
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	for i, t := range o.themes {
		if t.ID == id {
			o.themes = append(o.themes[:i], o.themes[i+1:]...)
			writeJSON(w, 200, fmt.Sprintf(`{"state":"deleted","id":%q}`, id))
			return
		}
	}
	writeJSON(w, 404, `{"error":"no such theme on the card"}`)
}

// serveUpload is h_theme_upload: the body is the container, the destination comes from
// the name INSIDE it, and the only client argument is ?overwrite=1.
func (o *Orb) serveUpload(w http.ResponseWriter, r *http.Request) {
	if r.ContentLength <= 0 {
		writeJSON(w, 411, `{"error":"Content-Length required"}`)
		return
	}
	if o.takeBusy() {
		writeJSON(w, 409, `{"error":"an upload is already in progress"}`)
		return
	}
	body, err := io.ReadAll(r.Body)
	if err != nil {
		writeJSON(w, 400, `{"error":"read failed"}`)
		return
	}
	if len(body) > MaxUpload {
		writeJSON(w, 413, fmt.Sprintf(`{"error":"too large","max":%d}`, MaxUpload))
		return
	}
	name, ferr := containerName(body)
	if ferr != "" {
		writeJSON(w, 422, fmt.Sprintf(`{"error":"rejected","detail":%q}`, ferr))
		return
	}
	id := themeID(name)
	overwrite := r.URL.Query().Get("overwrite") == "1"
	o.mu.Lock()
	defer o.mu.Unlock()
	for _, t := range o.themes {
		if t.ID == id && !overwrite {
			writeJSON(w, 409, `{"error":"a different theme with that name is already on the card","overwrite":false}`)
			return
		}
	}
	for _, t := range o.themes {
		if t.ID == id {
			t.Bytes = len(body)
			t.Serial++
			t.OK, t.Why = true, ""
			writeJSON(w, 200, fmt.Sprintf(`{"state":"stored","bytes":%d}`, len(body)))
			return
		}
	}
	o.themes = append(o.themes, &Theme{Name: name, ID: id, Bytes: len(body), Serial: 1, OK: true})
	writeJSON(w, 200, fmt.Sprintf(`{"state":"stored","bytes":%d}`, len(body)))
}

func (o *Orb) find(id string) *Theme {
	o.mu.Lock()
	defer o.mu.Unlock()
	for _, t := range o.themes {
		if t.ID == id {
			return t
		}
	}
	return nil
}

// takeBusy consumes one Busy: the firmware refuses a second job while one runs, and a
// knob that stayed set would make every later call in a test answer 409.
func (o *Orb) takeBusy() bool {
	o.mu.Lock()
	defer o.mu.Unlock()
	b := o.Busy
	o.Busy = false
	return b
}

// Container writes a FORMAT-2 .orbtheme header carrying name, for callers that need a
// file the double will file. Only the identity fields are populated; the payload is the
// device\'s business, and tools/host_sd_test.py pins the real packer against the C
// reader field by field.
func Container(name string) []byte {
	b := make([]byte, 512)
	b[0], b[1], b[2], b[3] = 0x54, 0x52, 0x4f, 0x47 // 'TROR' little-endian
	b[4] = 2                                        // FORMAT 2
	copy(b[14*4:], name)
	return b
}

// MaxUpload is ORB_THEME_UPLOAD_MAX, restated here rather than imported from orbnet so
// this file keeps failing when the client's copy drifts.
const MaxUpload = 0x00400000 + 0x20000

// idOK is orb_theme_id_ok: exactly 12 chars of [0-9a-f].
func idOK(s string) bool {
	if len(s) != 12 {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}

// themeID is main/art/orb_theme.c's theme_id() and tools/orbtheme.py's theme_id():
// hex(sha256(name))[:12] over the UTF-8 bytes up to the NUL.
func themeID(name string) string {
	d := sha256.Sum256([]byte(name))
	return hex.EncodeToString(d[:])[:12]
}

// containerName is the firmware's read of the header name, kept independent of orbnet
// for the same reason idOK is: this double must not agree with the client by
// construction. It returns the name and the reason it refused the container.
func containerName(b []byte) (string, string) {
	if len(b) < 128 {
		return "", "shorter than the 128 B header"
	}
	if binary.LittleEndian.Uint32(b[0:]) != 0x474F5254 {
		return "", "not a .orbtheme container"
	}
	if f := binary.LittleEndian.Uint32(b[4:]); f != 2 {
		return "", fmt.Sprintf("theme format %d this firmware cannot read", f)
	}
	field := b[14*4 : 14*4+56]
	end := len(field)
	for i, c := range field {
		if c == 0 {
			end = i
			break
		}
	}
	name := string(field[:end])
	if name == "" {
		return "", "a container with no name in it"
	}
	return name, ""
}

func jsonSafe(s string) string {
	b, _ := json.Marshal(s)
	return string(b[1 : len(b)-1])
}
