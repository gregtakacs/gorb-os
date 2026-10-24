// Package orbfake is a stand-in orb that answers the theme routes exactly as
// main/net/orb_net.c registers them. It is written from the firmware, not from the
// hub's client, and that distinction is the whole point of it: the first version of
// this double was built by reading themes.go, so it answered ?name=, the client kept
// sending ?name=, and the pair agreed with each other while every real orb answered
// 400. A mock that mirrors the code under test proves the code agrees with itself.
//
// So this file quotes orb_net.c's h_sd / h_bundle_install / h_theme_del / h_theme_upload
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
	"hash/crc32"
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
	// Stored mirrors h_sd's row witness under Migration 1's single slot: a copy of
	// this file sits in the flash slot the dial draws. serveBundle lands it on
	// acknowledge — the double has no worker for the copy to lag behind — and an
	// active row is stored by definition, since active means the slot is drawn.
	Stored bool
}

// Orb is the fake. The zero value is not useful; build one with New.
type Orb struct {
	mu     sync.Mutex
	themes []*Theme
	active string

	// The staged-commit contract, mirrored field-for-field from main/art/orb_theme.c's
	// s_up/s_stage_tok: a duplicate refusal KEEPS the verified body (the firmware keeps
	// the scratch on the card), issues a one-shot token, and the next empty POST with
	// ?commit=<token> files the keep. stageN counts tokens so a hub test can tell a
	// spent offer from a stale one.
	stageBody []byte
	stageID   string
	stageTok  string
	stageN    uint32
	tryID     string
	tryWhy    string
	stagedID  string
	stagedWhy string
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
	// device's. New seeds the inside-the-firmware rows into it, so a fresh double already
	// reports a three-row ledger the way a fresh device does.
	apps      map[string]*CardApp
	appsOrder []string

	// JobBusy mirrors orb_jobs_busy: with a card job in flight every door on an installed
	// app answers 409 about the job, ahead of any lookup. Seed it to exercise the ordering.
	JobBusy bool
	// BusyPolls scripts job_busy:true on the next N /api/v1/sd reads whatever JobBusy
	// says, decrementing on each — a copy the device itself still reports live, for
	// the waits that must outlast the hub's idle budget (§32.5). BusyForever answers
	// busy on every read: the hub's busy-ceiling test.
	BusyPolls   int
	BusyForever bool
	// TearStore scripts the failure orb-B ate on 2026-10-22: the store copy that lands
	// pixels and never its header. The next bundle or upload leaves the staged witness
	// where it would have landed the store, so the hub's wait loop meets the torn verdict
	// through the door that armed it, which is exactly the response theme.go's
	// waitForStore has to read correctly. SetStaged records the same witness by hand
	// for tests that never arm a store to get it.
	TearStore bool

	// ReviveFails makes the fault-record clear on a failed app's ON refuse, which is the
	// only path where ESP_FAIL reaches the client and door_http_status's default arm sends
	// 500 rather than 409. UninstallSticks is its twin on the uninstall door, where the slot
	// erased but the registry kept the row.
	ReviveFails     bool
	UninstallSticks bool
}

// New returns a fake orb seeded with the given themes, ids recomputed from each name
// so a caller cannot seed a row whose id disagrees with its name.
func New(themes ...Theme) *Orb {
	o := &Orb{Card: "SDSC-8G", Mounted: true,
		apps: map[string]*CardApp{}}
	// Seed the inside-the-firmware rows first, in device order. h_status walks the registry
	// from row zero, so a double that omits them answers a status request with "apps":[]
	// where every real orb names clock, color_picker and menu — and the hub's Manage card
	// cannot tell "no apps installed" from "the device reported nothing".
	for _, slug := range builtInSlugs {
		o.apps[slug] = &CardApp{Stem: slug, Slug: slug, Title: slug,
			Installed: true, Enabled: true, Builtin: true}
		o.appsOrder = append(o.appsOrder, slug)
	}
	for i := range themes {

		t := themes[i]
		t.ID = themeID(t.Name)
		if t.Active {
			// The single slot being drawn IS stored: h_sd marks such a row
			// stored true, and source answers "slot".
			t.Stored = true
			o.active = t.Name
		}
		o.themes = append(o.themes, &t)
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

// SetStaged records the torn-store witness — a store armed whose header never landed —
// the way the firmware does, so the card can be pointed at it by id. One re-store
// clears it, exactly as the device's does.
func (o *Orb) SetStaged(id, why string) {
	o.mu.Lock()
	o.stagedID, o.stagedWhy = id, why
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
	case r.URL.Path == "/api/v1/wifi/mode" && r.Method == http.MethodPost:
		o.serveWifiMode(w, r)
	case r.URL.Path == "/api/v1/config" && r.Method == http.MethodGet:
		o.serveConfigGet(w)
	case r.URL.Path == "/api/v1/config" && r.Method == http.MethodPost:
		o.serveConfigPost(w, r)
	case r.URL.Path == "/api/v1/name" && r.Method == http.MethodPost:
		o.serveNamePost(w, r)
	case r.URL.Path == "/api/v1/reboot" && r.Method == http.MethodPost:
		o.serveReboot(w)
	case r.URL.Path == "/api/v1/ota" && r.Method == http.MethodPost:
		o.serveOTA(w, r)
	case r.URL.Path == "/api/v1/reprov" && r.Method == http.MethodPost:
		o.serveReprov(w)
	case r.URL.Path == "/api/v1/screen" && r.Method == http.MethodGet:
		o.serveScreen(w, r)
	case r.URL.Path == "/api/v1/fb" && r.Method == http.MethodGet:
		o.serveFb(w, r)
	case r.URL.Path == "/api/v1/sd" && r.Method == http.MethodGet:
		o.serveSD(w)
	case r.URL.Path == "/api/v1/sd/delete" && r.Method == http.MethodPost:
		o.serveDelete(w, r)
	case r.URL.Path == "/api/v1/sd/theme" && r.Method == http.MethodPost:
		o.serveUpload(w, r)
	case r.URL.Path == "/api/v1/sd/bundle" && r.Method == http.MethodPost:
		o.serveBundle(w, r)
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
	// 7c9ff63's recovery doors, mirrored gate-for-gate.
	case r.URL.Path == "/api/v1/apps/clear" && r.Method == http.MethodPost:
		o.serveAppClear(w, r)
	case r.URL.Path == "/api/v1/apps/factory" && r.Method == http.MethodPost:
		o.serveAppFactory(w)
	case r.URL.Path == "/api/v1/sd/app" && r.Method == http.MethodPost:
		o.serveAppUpload(w, r)
	default:
		writeJSON(w, 404, `{"error":"no such route on this orb"}`)
	}
}

// serveSD formats h_sd's reply: the row shape is name, id, why, bytes, serial, ok,
// active, changed, stored — there is no title field, and the try and staged witnesses
// key on _id. Migration 1 retired the live word and the damage pair with the A/B slot.
func (o *Orb) serveSD(w http.ResponseWriter) {
	o.mu.Lock()
	defer o.mu.Unlock()
	busy := o.JobBusy || o.BusyForever || o.BusyPolls > 0
	if o.BusyPolls > 0 {
		o.BusyPolls--
	}
	var b strings.Builder
	fmt.Fprintf(&b, `{"mounted":%t,"card":%q,"uploading":false,"job_busy":%t,"total_mb":7581,"free_mb":7100,`+
		`"source":%q,"active":%q,"card_count":%d,"try_id":%q,"try_why":%q,`+
		`"staged_id":%q,"staged_why":%q,"themes":[`,
		o.Mounted, o.MountedCard(), busy, o.sourceLocked(), o.active, len(o.themes),
		o.tryID, o.tryWhy, o.stagedID, o.stagedWhy)
	for i, t := range o.themes {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, `{"name":%q,"id":%q,"why":%q,"bytes":%d,"serial":%d,"ok":%t,"active":%t,"changed":%t,"stored":%t}`,
			t.Name, t.ID, t.Why, t.Bytes, t.Serial, t.OK, t.Active, t.Changed, t.Stored)
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

// sourceLocked answers h_sd's "source" the firmware's way: orb_theme_source() says
// "slot" when the single slot is drawn and "none" when the base face is.
func (o *Orb) sourceLocked() string {
	if o.active != "" {
		return "slot"
	}
	return "none"
}

// serveFb is h_fb: the one-shot frame capture over the PSRAM mirror. It reads pixels and
// changes nothing, so the double answers a small placeholder body rather than 652 KB of
// BMP — enough for the contract test to see the route is fakeable (not a 404) without
// shipping a whole frame. The real device gates it exactly like /api/v1/screen.
func (o *Orb) serveFb(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "image/bmp")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(200)
	_, _ = io.WriteString(w, "BM")
}

// serveBundle is h_bundle_install (P6.4), now the only theme write door: ?id= must pass
// orb_theme_id_ok or the answer is the firmware's 400, a second job while one runs is a
// 409, and the ack carries reboot:automatic — Migration 1 made the store the one press
// that changes the theme, rebooting onto the new face when the copy verifies. The double
// lands the outcome on acknowledge, the way it landed the slot witness before: no worker,
// and a hub that must poll for a witness its fake never gives would leave the
// store-then-watch flow the page implements entirely untested. A landing also clears the
// torn-store witness, as the device's does.
func (o *Orb) serveBundle(w http.ResponseWriter, r *http.Request) {
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
	if o.TearStore {
		o.stagedID, o.stagedWhy = id, "the copy reached the slot but its header did not"
	} else {
		for _, x := range o.themes {
			x.Active = false
		}
		t.Stored = true
		t.Active = true
		o.active = t.Name
		o.stagedID, o.stagedWhy = "", ""
	}
	o.mu.Unlock()
	writeJSON(w, 200, fmt.Sprintf(`{"state":"storing","id":%q,"reboot":"automatic"}`, id))
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
// the name INSIDE it, and the only client argument is ?overwrite=1 — plus, since the
// staged-commit contract, the empty ?commit=<token> POST that answers a refusal.
func (o *Orb) serveUpload(w http.ResponseWriter, r *http.Request) {
	// The commit branch stands before the Content-Length rule exactly as the firmware's
	// does: the confirmed replace carries NO body by design, and the rule that refuses
	// an empty upload must not swallow the one request that is empty on purpose.
	if tok := r.URL.Query().Get("commit"); tok != "" {
		o.serveCommit(w, tok)
		return
	}
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
			// The staged-commit mirror: the refusal KEEPS the verified body the way
			// upload_end keeps the scratch, mints the one-shot token the firmware
			// mints, and hands it back in the same field name. A duplicate with
			// nobody watching costs the double nothing; a hub that knows the token
			// can commit with an empty POST, and a hub test can demand that road.
			o.stageBody, o.stageID, o.stageTok = body, id, fmt.Sprintf("%016x", o.stageN+uint32(len(body))*0x9E3779B9+0x5bf03635)
			o.stageN++
			writeJSON(w, 409, fmt.Sprintf(`{"error":"a different theme with that name is already on the card","overwrite":false,"commit":%q}`, o.stageTok))
			return
		}
	}
	// h_theme_upload does not merely file the bytes: after a verified upload it
	// queues orb_theme_post_bundle(id) and answers reboot:automatic. The double
	// lands that queued store synchronously — the same concession serveBundle
	// already makes ("the double has no worker for the copy to lag behind") — so a
	// hub that watches for the stored witness after an upload, as theme.go's
	// waitForStore does, sees the same lifecycle the real board walks through.
	land := func(t *Theme) {
		if o.TearStore {
			o.stagedID, o.stagedWhy = id, "the copy reached the slot but its header did not"
			return
		}
		for _, x := range o.themes {
			x.Active = false
		}
		t.Stored, t.Active = true, true
		o.active = t.Name
		o.stagedID, o.stagedWhy = "", ""
	}
	for _, t := range o.themes {
		if t.ID == id {
			t.Bytes = len(body)
			t.Serial++
			t.OK, t.Why = true, ""
			land(t)
			writeJSON(w, 200, fmt.Sprintf(`{"state":"stored","id":%q,"bytes":%d,"reboot":"automatic"}`, id, len(body)))
			return
		}
	}
	o.themes = append(o.themes, &Theme{Name: name, ID: id, Bytes: len(body), Serial: 1, OK: true, Stored: !o.TearStore, Active: !o.TearStore})
	if o.TearStore {
		land(o.themes[len(o.themes)-1])
	}
	writeJSON(w, 200, fmt.Sprintf(`{"state":"stored","id":%q,"bytes":%d,"reboot":"automatic"}`, id, len(body)))
}

// serveCommit is orb_theme_upload_recommit: the empty POST that spends a refusal's
// token. The staged body is filed under the id the refusal derived from it (the
// double re-verifies nothing — it verified the body when it staged it, and the real
// device re-reads it from the card), the store lands the same synchronous way
// serveUpload lands it, and a token that does not answer the one standing offer is a
// 410 with the firmware's own sentence shape. The offer is one-shot: success clears
// it, so a replayed commit finds nothing to file.
func (o *Orb) serveCommit(w http.ResponseWriter, tok string) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.stageBody == nil || tok != o.stageTok {
		writeJSON(w, 410, `{"error":"commit refused","detail":"no staged upload answers to that token"}`)
		return
	}
	body, id := o.stageBody, o.stageID
	o.stageBody, o.stageID, o.stageTok = nil, "", ""
	for _, t := range o.themes {
		if t.ID == id {
			t.Bytes = len(body)
			t.Serial++
			t.OK, t.Why = true, ""
			if o.TearStore {
				o.stagedID, o.stagedWhy = id, "the copy reached the slot but its header did not"
			} else {
				for _, x := range o.themes {
					x.Active = false
				}
				t.Stored, t.Active = true, true
				o.active = t.Name
				o.stagedID, o.stagedWhy = "", ""
			}
			writeJSON(w, 200, fmt.Sprintf(`{"state":"stored","id":%q,"bytes":%d,"reboot":"automatic","committed":"staged"}`, id, len(body)))
			return
		}
	}
	writeJSON(w, 410, fmt.Sprintf(`{"error":"commit refused","detail":"the staged copy is no longer on the card"}`))
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

// Container writes a FORMAT-3 .orbtheme carrying name, for callers that need a file the
// double will file. One section, one plate element, no art — enough that every field a
// reader checks exists and agrees, because a fixture that skips the index would let the
// hub's index walk go untested. tools/host_orbbundle_test.py pins the real packer against
// the C reader field by field; this is the LAN shape of the same file, written here from
// the layout rather than imported from orbnet, so the two agree by rule and not by
// construction.
func Container(name string) []byte { return ContainerWithSerial(name, 0) }

// ContainerWithSerial is Container with the header's install serial set, so a test can
// bake a container whose serial agrees with a row already on the fake's list — the one
// shape that turns the hub's ?precheck=1 verdict from "duplicate" into "exact". The
// header CRC is recomputed below, so the serial stays inside the checksummed span.
func ContainerWithSerial(name string, serial uint32) []byte {
	slug := []byte("clock")
	blob := appendWords(nil, 1, 0, 26, uint32(len(slug))) // n_elem, n_spr, n_scal, n_slug
	// The clock dialect's 26 scalars, every one at the app's own default: the fixture
	// themes nothing, and D31 keeps those words the app's rather than the theme's.
	blob = appendWords(blob, make([]uint32, 26)...)
	// One plate row: n, class, flags, order, alpha, slot, frame0, n_frames, x, y,
	// w, h, pivot_x, pivot_y, radius, degrees, scale16 — and now, appended, kind
	// and src (theme_schema.ELEMENT_FIELDS, D32's ruling A), both zero because a
	// plate answers with its slot. The two trailing zeros are the whole append.
	blob = appendWords(blob, 1, 3, 0, 0, 1, 0, 0, 466, 466, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
	blob = append(blob, slug...)
	for len(blob)%4 != 0 {
		blob = append(blob, 0)
	}

	blobOff := uint32(fIdxHead + fIdxWords*4)
	region := appendWords(nil, 1, 0) // n_sections, reserved
	region = append(region, idxRecord(
		blobOff+uint32(len(blob))-uint32((len(slug)+3)&^3), uint16(len(slug)),
		1, blobOff, uint32(len(blob)), crc32.ChecksumIEEE(blob),
		0, 0, 0, 0, 0)...)
	region = append(region, blob...)

	artOff := uint32(fHdrBytes+len(region)+fSector-1) & ^uint32(fSector-1)
	b := make([]byte, artOff)
	put := func(word int, v uint32) { binary.LittleEndian.PutUint32(b[word*4:], v) }
	put(0, fMagic)
	put(1, fFormat)
	put(2, fABI)
	put(3, uint32(len(b)))
	put(4, fHdrBytes)
	put(5, uint32(len(region)))
	put(6, crc32.ChecksumIEEE(region))
	put(7, 0)       // art_size
	put(8, 0)       // art_crc
	put(9, 1)       // n_sections — where FORMAT 2 kept scr_w
	put(11, serial) // serial
	put(13, artOff)
	copy(b[fNameWord*4:], name)
	put(fHdrCRCWord, hdrCRC(b))
	copy(b[fHdrBytes:], region)
	return b
}

// The FORMAT 3 header's word positions and caps, restated here for the same reason idOK
// and themeID are: the double must fail when the client's copy drifts, so it may not
// import them. Word 9 is the one a v2 reader would misread — FORMAT 2 kept scr_w there and
// FORMAT 3 counts app sections there, which is why the FORMAT word is checked first.
const (
	fHdrBytes   = 128
	fMagic      = 0x474F5254 // 'T','R','O','G' little-endian on the wire
	fFormat     = 3
	fABI        = 3
	fABI2       = 4 // ORB_BUNDLE3_ABI2: C4's delta dialect, the same stamp the firmware door takes
	fNameWord   = 14
	fNameBytes  = 56
	fHdrCRCWord = 28
	fIdxWords   = 10 // a 40-byte index record
	fIdxHead    = 8  // n_sections + reserved
	fSector     = 4096
	// The two D29 install sums — ORB_BUNDLE_GEOM_CAP and ORB_BUNDLE_ART_CAP (the one
	// exported ORB_THEME_SLOT_CAP, 8,323,072 B; §7.4). `== cap` is
	// accepted at both: geometry's erase covers it sector-aligned and lands exactly on the
	// banks, and the banks have slot-less-art-offset to live in.
	fGeomCap = 0xF000
	fArtCap  = 0x7F0000
)

func le32(v uint32) []byte {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], v)
	return b[:]
}

// appendWords appends little-endian u32s, the shape every count in a FORMAT 3 bundle is
// written in.
func appendWords(b []byte, vals ...uint32) []byte {
	for _, v := range vals {
		b = append(b, le32(v)...)
	}
	return b
}

// idxRecord packs one 40-byte index record, in the layout orb_bundle.c's IDX_BYTES comment
// states and orbbundle.py's IDX_PACK writes: four u16s at 0,2,4,6 (slug_off, slug_len,
// abi_min, abi_max), seven u32s at 8,12,16,20,24,28,32 (n_elem, geom_off, geom_size,
// geom_crc, art_off, art_size, art_crc), two u16s at 36,38 (scr_w, scr_h). The u16 head is
// what a writer that read only the field names gets wrong: n_elem lands where a u32 layout
// would put geom_off, and every offset after it shifts.
func idxRecord(slugOff uint32, slugLen uint16, nElem, geomOff, geomSize, geomCRC,
	artOff, artSize, artCRC, scrW, scrH uint32) []byte {
	r := make([]byte, fIdxWords*4)
	binary.LittleEndian.PutUint16(r[0:], uint16(slugOff))
	binary.LittleEndian.PutUint16(r[2:], slugLen)
	binary.LittleEndian.PutUint16(r[4:], fABI)
	binary.LittleEndian.PutUint16(r[6:], fABI)
	binary.LittleEndian.PutUint32(r[8:], nElem)
	binary.LittleEndian.PutUint32(r[12:], geomOff)
	binary.LittleEndian.PutUint32(r[16:], geomSize)
	binary.LittleEndian.PutUint32(r[20:], geomCRC)
	binary.LittleEndian.PutUint32(r[24:], artOff)
	binary.LittleEndian.PutUint32(r[28:], artSize)
	binary.LittleEndian.PutUint32(r[32:], artCRC)
	binary.LittleEndian.PutUint16(r[36:], uint16(scrW))
	binary.LittleEndian.PutUint16(r[38:], uint16(scrH))
	return r
}

// hdrCRC is _hdr_crc in tools/orbbundle.py and orb_bundle3_hdr_parse's own sum in
// main/art/orb_bundle.c: crc32 over the 128 header bytes with the checksum word zeroed.
// zlib CRC-32 and esp_rom_crc32_le share the polynomial, so all three land on one value.
func hdrCRC(b []byte) uint32 {
	z := append([]byte(nil), b[:fHdrBytes]...)
	for i := fHdrCRCWord * 4; i < fHdrCRCWord*4+4; i++ {
		z[i] = 0
	}
	return crc32.ChecksumIEEE(z)
}

// MaxUpload is ORB_THEME_UPLOAD_MAX, restated here rather than imported from orbnet so
// this file keeps failing when the client's copy drifts. The device's copy
// (main/art/orb_theme.h:253) is 8 MB plus the header block's 0x20000 of wire slack —
// the whole 8 MB `assets` slot is one theme's since Migration 1, and this restatement
// carried the retired 4 MB pair's figure until C1 (§7.4).
const MaxUpload = 0x00800000 + 0x20000

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

// containerName is the firmware's read of the header — card_check3's gate, kept
// independent of orbnet for the same reason idOK is: this double must not agree with the
// client by construction. It returns the name and the reason it refused the container, in
// the sentences main/art/orb_bundle.c and main/net/orb_net.c actually answer with, in the
// order they check. No device on this network installs a FORMAT 2 container: the reader
// island went in 7c0fd4b, so a fixture that claimed otherwise would let the pair agree on
// a file nothing can mount.
func containerName(b []byte) (string, string) {
	if len(b) < fHdrBytes {
		return "", "shorter than the 128 B header"
	}
	w := func(i int) uint32 { return binary.LittleEndian.Uint32(b[i*4:]) }
	if w(0) != fMagic {
		return "", "not an .orbtheme file"
	}
	if f := w(1); f != fFormat {
		return "", "theme format this firmware cannot read"
	}
	if abi := w(2); abi != fABI && abi != fABI2 {
		return "", fmt.Sprintf("bundle geometry v%d; this build reads v%d and v%d — re-bake it", abi, fABI, fABI2)
	}
	if w(fHdrCRCWord) != hdrCRC(b) {
		return "", "container header is damaged — re-copy the file onto the card"
	}
	if g := w(5); g > fGeomCap {
		return "", fmt.Sprintf("geometry region is %d B; the dial's table area holds %d B", g, fGeomCap)
	}
	if a := w(7); a > fArtCap {
		return "", fmt.Sprintf("pixel payload is %d B; a theme slot holds %d B", a, fArtCap)
	}
	field := b[fNameWord*4 : fNameWord*4+fNameBytes]
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
