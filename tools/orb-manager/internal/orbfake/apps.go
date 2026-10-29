// This file is the P4 card-app surface of the double, written from main/net/orb_net.c's
// h_apps / h_apps_install / h_app_enable / h_app_uninstall / h_app_upload and from the
// container header in main/app/orb_app_manifest.h — not from themes.go and not from any
// hub client. That distinction is this package's whole reason to exist (orbfake.go's
// header comment): a double built from the caller's side agrees with the caller about the
// wrong thing, which is exactly how the ?name= bug survived. Every status code and body
// shape below is quoted from the firmware rather than invented.

package orbfake

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"net/http"
	"strings"
)

// Header words the double reads, mirrored from orb_app_manifest.h's enum and spelled once
// here for the same reason the firmware spells them once there: a width change must move
// both sides together or the sentinel lint goes red.
const (
	appMagic    = 0x00415247 // 'G','R','A' — deliberately unlike the theme magic
	appThemeMgc = 0x474F5254 // "GORT": a theme handed here gets named, not miscalled
	wSerial     = 4
	wAppID      = 9
	wSlug       = 10 // 6 words = 24 B
	wTitle      = 16 // 8 words = 32 B
	wPeriodMs   = 30
	wCaps       = 31
	wNSettings  = 32
	wArtSize    = 38 // W_SET_OFF 33, GEOM_OFF 34, GEOM_SIZE 35, GEOM_CRC 36, ART_OFF 37
	slugBytes   = 24
	titleBytes  = 32
	appNameMax  = 55 // ORB_SD_NAME_MAX
	appHdrWords = 39 // through W_ART_SIZE, the last word the double reads
	// ORB_APP_UPLOAD_MAX, recomputed by D37: the installed bill (a slot's geometry
	// budget plus the header block and its alignment gap) PLUS one editor region at
	// ORBAPP_EDITOR_MAX plus the one sector of padding that can sit in front of it.
	// The region never installs, so it adds to the largest FILE and not to the
	// largest slot — the distinction §4.2 is built on.
	appUploadMa = 0x40000 + 0x20000 + 0x200000 + 0x1000
	// D37's editor triple: W_EDITOR_OFF/SIZE/CRC, the three words after
	// W_WASM_MEM and before W_HDR_CRC in orb_app_manifest.h. The double reads the
	// offset so an editor-bearing container can model one on the card.
	wEditorOff = 47
	wEditorSz  = 48
	wEditorCrc = 49
	appAbi     = 3 // ORBAPP_GEOM_ABI / orbapp.GEOM_ABI
)

// capNames is orbapp_caps_names's table (orb_app_manifest.c:400) in bit order. The double
// keeps its own copy for the same reason it keeps its own idOK: sharing the hub's copy
// would make the two agree by construction instead of by agreement.
var capNames = [4]string{"power", "net", "sd_read", "themes"}

// CardApp is one app the double knows about: the container staged on the card and the
// flash row it becomes once installed. One struct for both because the firmware walks one
// registry and reports two views of it.
type CardApp struct {
	Stem   string // the card file's stem — the only handle install accepts
	Slug   string // the header's own slug, which is what enable/uninstall key on
	Title  string
	AppID  uint32
	Caps   uint32
	Serial uint32
	Period uint32
	NSet   uint32
	ArtSz  uint32
	Bytes  uint32
	Why    string // disclosure line; empty when the row is clean

	Installed bool
	Enabled   bool
	Failed    bool
	OnGlass   bool
	// Status is the app's own runtime line as h_status reports it. The device sends "" for
	// a built-in that never reported one, so the double sends "" and a caller sets it
	// deliberately rather than the formatter inventing a word.
	Status string
	// NoIface marks a card whose behaviour no firmware module implements. orb_app_enable
	// refuses to turn one on at all, so a Manage card that offered a toggle for it would
	// be offering a button that can only ever fail.
	NoIface bool
	// Editor holds the D37 editor region's bytes. GET /api/v1/apps/editor streams
	// exactly these, and the double stores bytes rather than a placement: the device
	// derives editor_off from the regions ahead of it, so a fixture that set only an
	// offset would model a container no writer can emit.
	Editor []byte
	// GeomCRC, CodeCRC and CodeSize are three of the six words the firmware compares
	// uploads on, carried per row so h_apps's installed tier can report the tuple a real
	// dial reports. EditorCRC is not a stored field: it is the checksum of the Editor
	// bytes above, which is how the device derives it, and a double that let a fixture set
	// it by hand could be made to claim an editor it does not hold.
	GeomCRC  uint32
	CodeCRC  uint32
	CodeSize uint32
	// Builtin marks a row that ships inside the firmware. door_open refuses to
	// enable/disable/uninstall those with NOT_SUPPORTED, and they sit below
	// ORB_APP_BASE.
	//
	// The comment this field used to carry said such rows appear in h_status's ledger but
	// NOT in /api/v1/apps's installed tier, citing orb-B on fw 9442432. That was true of
	// the firmware then and is false of the firmware now: apps_send_installed (orb_app_net.c
	// :776) walks the WHOLE ledger including ORB_APP_MENU, emits the built-in FIRST, and
	// stamps it with "compiled":true. A double that dropped the row outright modelled a
	// device no current build is, and it made every hub test written against it blind to
	// the one state the Apps face must never list — the shell counted as the owner's
	// inventory. Built-ins are emitted with the flag, which is the fact the face filters on.
	Builtin bool
	// Version is the release stamp the container's header carries, reported by the device
	// for card rows and omitted for a built-in, which was never a container. It is what
	// lets the hub answer "is the copy on this dial the copy on my shelf" without shipping
	// either file anywhere, and the whole of the Apps face's update wording.
	Version string
}

// builtInSlugs is the device's own inside-the-firmware set. Since D36 (v1.1.20)
// that is the shell menu alone: the clock and the color picker are hub-seeded card
// rows now, and a fresh dial's ledger carries one built-in row. Read off the
// v1.1.20 /api/v1/status ledger, not the hub's knownApps list.
var builtInSlugs = []string{"menu"}

// sdNameOK is orb_sd_name_ok (orb_sd.c): 1..55 bytes of [A-Za-z0-9._-], no leading '.'.
// Duplicated on purpose. The card stem is the one app identifier a caller controls, and a
// double that accepts what the device refuses is how a traversal hole stays invisible
// until it is on a device in someone's house.
func sdNameOK(s string) bool {
	if s == "" || len(s) > appNameMax || strings.HasPrefix(s, ".") {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		ok := c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' ||
			c == '.' || c == '_' || c == '-'
		if !ok {
			return false
		}
	}
	return true
}

// capsNames formats a bitset as orbapp_caps_names does: comma-space joined in bit order,
// and the empty string rather than a truncated list.
//
// The four owner caps sit at bits 4..7, NOT 0..3 (orb_cap_t: CAP_POWER = 4 ..
// CAP_THEMES = 7; orb_app_manifest.c indexes `1u << (CAP_POWER + i)`). Bits 0..3 are the
// free caps — time, config, ui, art — which §4.2 grants unconditionally, so the firmware
// never prints them. This loop iterated 0..3 against the owner names while still being
// documented as the manifest's twin: a test that set caps 0x1 read back "power" here and
// "time" on a board, and 0x8 — CAP_ART, free and unprintable — read back "themes". D37's
// enumeration keys off that exact word, so the double is corrected to the device instead
// of the hub learning a second, wrong, table from a passing suite.
func capsNames(caps uint32) string {
	var parts []string
	for i := 0; i < 4; i++ {
		if caps&(1<<uint(4+i)) != 0 {
			parts = append(parts, capNames[i])
		}
	}
	return strings.Join(parts, ", ")
}

// WithCardApps seeds the app store, then returns the orb so a test can chain it off New.
// Rows keep the order given, so the flash tier's slot field means the same positional
// thing it does on a device (h_apps's installed tier walks the registry, it does not sort).
func (o *Orb) WithCardApps(rows ...CardApp) *Orb {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.apps == nil {
		o.apps = map[string]*CardApp{}
	}
	for i := range rows {
		a := rows[i]
		if a.Slug == "" {
			a.Slug = a.Stem
		}
		if a.Builtin {
			// A caller cannot seed over an inside-the-firmware row: the device keeps those
			// below ORB_APP_BASE and refuses every door verb on them, so a double that let a
			// test re-register "clock" as a card would model an orb that does not exist.
			continue
		}
		if _, had := o.apps[a.Slug]; !had {
			o.appsOrder = append(o.appsOrder, a.Slug)
		}
		o.apps[a.Slug] = &a
	}
	return o
}

// cardRowsLocked is the card tier: every staged container, in registration order. Built-ins
// are excluded because h_apps_list walks from ORB_APP_BASE, and because count is the number
// of .orbapp FILES on the card — a device with three built-ins and an empty apps directory
// reports count 0, which is exactly what orb-B reports.
func (o *Orb) cardRowsLocked() []*CardApp {
	var out []*CardApp
	for _, slug := range o.appsOrder {
		if a := o.apps[slug]; a != nil && a.Stem != "" && !a.Builtin {
			out = append(out, a)
		}
	}
	return out
}

// dropCardRowForFileLocked is the volume's effect on the app listing: h_apps_list answers
// by walking /sd/apps, so a filemanager delete of a container removes the row it produced.
// Only an apps/*.orbapp path does anything — every other file on the card (photos, logs, a
// theme) has no row to lose, and the staged rows WithCardApps planted without a file are
// left alone, because a test set those up deliberately and nothing about this delete
// touched them.
func (o *Orb) dropCardRowForFileLocked(rel string) {
	if !strings.HasPrefix(rel, "apps/") || !strings.HasSuffix(rel, ".orbapp") {
		return
	}
	stem := strings.TrimSuffix(strings.TrimPrefix(rel, "apps/"), ".orbapp")
	if stem == "" || strings.Contains(stem, "/") || o.apps == nil {
		return
	}
	a, ok := o.apps[stem]
	if !ok || a.Stem == "" || a.Builtin {
		return
	}
	delete(o.apps, stem)
	for i, s := range o.appsOrder {
		if s == stem {
			o.appsOrder = append(o.appsOrder[:i], o.appsOrder[i+1:]...)
			break
		}
	}
}

// cardInstalledLocked is h_apps's installed tier. It is NOT the same set as the ledger:
// h_apps_list walks from ORB_APP_BASE, so the inside-the-firmware rows never appear, while
// h_status walks from zero and always names them. Measured on orb-B: "installed":[] with a
// three-row ledger. A double that uses one walker for both tells the hub UI it has no apps
// on a device that has three, and a Manage card built on that shows nothing to manage.
func (o *Orb) cardInstalledLocked() []*CardApp {
	var out []*CardApp
	for _, a := range o.cardRowsLocked() {
		if a.Installed && !a.Builtin {
			out = append(out, a)
		}
	}
	return out
}

// allRowsLocked is the ledger's walk: every row REGISTERED IN FLASH, built-ins first. A
// container merely staged on the card is not in the registry yet, so it must not appear —
// h_status answers "what can this orb do", and a file sitting on a card does nothing.
func (o *Orb) allRowsLocked() []*CardApp {
	var out []*CardApp
	for _, slug := range o.appsOrder {
		if a := o.apps[slug]; a != nil && a.Installed {
			out = append(out, a)
		}
	}
	return out
}

// doorAppLocked returns the row a card verb operates on, or the refusal it must send, in
// door_open's exact order (orb_app_install.c:833): name rule, then busy, then lookup, then
// the built-in check. The order carries weight twice over. A card job in flight answers 409
// about the job even when the id names nothing, so a double that checked the lookup first
// would show the hub a reason the orb never had. And the name rule runs before the busy
// check, which is why enable?id=..foo on a busy orb reports the name, not the job.
func (o *Orb) doorAppLocked(slug string) (*CardApp, int, string) {
	if !sdNameOK(slug) {
		return nil, 400, notUsableNameBody
	}
	if o.JobBusy {
		return nil, 409, doorBusyBody
	}
	row := o.apps[slug]
	if row == nil || !row.Installed {
		return nil, 404, notFoundBody
	}
	if row.Builtin {
		return nil, 400, builtinBody
	}
	return row, 0, ""
}

// The refusal bodies door_open and the two doors emit, with the status each maps to under
// door_http_status (orb_net.c): NOT_FOUND 404, INVALID_ARG 400, NOT_SUPPORTED 400,
// INVALID_STATE 409, anything else 500. Literals live here because a double cannot parse C
// at runtime; device_contract_test.go scrapes each one out of the firmware and fails when
// the two drift, which is the only way a hand-copied string stays honest.
//
// Note what the device never does: put caller-controlled text in a REFUSAL body. Every
// string below is the firmware's own sentence. The success bodies do echo the slug, because
// h_app_enable formats it after the work succeeded — that difference is deliberate, and a
// double that echoes on refusal would teach the hub to render a string it cannot trust.
const (
	doorBusyBody      = `{"error":"a card job is running — try again when it finishes"}`
	notFoundBody      = `{"error":"no app by that name is installed on this dial"}`
	builtinBody       = `{"error":"the shell menu is part of the dial — it cannot be turned off"}`
	onDialBody        = `{"error":"that app is on the dial — carousel away from it first"}`
	noIfaceBody       = `{"error":"no firmware module implements this app's behaviour — it cannot be shown"}`
	reviveFailBody    = `{"error":"the fault record would not clear — the app stays failed"}`
	notUsableNameBody = `{"error":"not a usable app name"}`
	missingIDBody     = `{"error":"id is missing"}`
	badOnBody         = `{"error":"on must be 0 or 1"}`
	noStemBody        = `{"error":"id is missing or not a usable app name"}`
	uninstallHoldBody = `{"error":"it came off the flash but the registry still holds it"}`
	// h_app_face's refusal: an off row is not a destination. Word-for-word the
	// firmware's sentence, pinned by door_bodies_test.go against orb_net.c.
	notEnabledBody = `{"error":"the row is switched off — enable it before parking the glass on it"}`
	// h_app_face's second refusal: a faulted row cannot wear the glass.
	failedFaceBody = `{"error":"the row carries a fault — clear it before parking the glass on it"}`
)

// appID reads the id query parameter the way q_param resolves it, which is NOT the way the
// name rule works. q_param rejects only an empty value and a decoded separator, so
// enable?id=..%2Fevil answers "id is missing"; a leading dot or a space survives q_param and
// is caught by orb_sd_name_ok inside door_open, so enable?id=..foo answers "not a usable app
// name" instead. The two 400s are different layers, and measuring the difference is the only
// reason this function does not just call sdNameOK. A value longer than the handler's 24-byte
// buffer is truncated rather than refused, so a 25-character id is accepted and matches
// nothing (both measured on orb-B, fw 9442432).
func appID(r *http.Request) string {
	id := r.URL.Query().Get("id")
	if id == "" || strings.ContainsRune(id, '/') || strings.ContainsRune(id, '\\') {
		return ""
	}
	if len(id) > appSlugMax {
		id = id[:appSlugMax]
	}
	return id
}

const appSlugMax = 24 // ORB_APP_SLUG_MAX, orb_app_manifest.h

// serveApps answers GET /api/v1/apps in h_apps's shape: card tier, then the installed
// array opened inline. A device with no card reports count -1 rather than count 0, and a
// card whose apps directory is missing reports mounted true with count -1: the firmware
// draws that line because it is the whole difference, to whoever is reading it, between
// "the folder is misnamed" and "the files are there but every one was refused".
func (o *Orb) serveApps(w http.ResponseWriter) {
	o.mu.Lock()
	defer o.mu.Unlock()
	installed := o.installedJSONLocked()
	if !o.Mounted {
		writeJSON(w, 200, `{"mounted":false,"count":-1,"apps":[],"installed":[`+
			installed+`]}`)
		return
	}
	var rows []string
	for _, a := range o.cardRowsLocked() {
		rows = append(rows, fmt.Sprintf(
			`{"stem":%q,"slug":%q,"title":%q,"app_id":"0x%08x","caps":%q,`+
				`"period_ms":%d,"serial":%d,"n_settings":%d,"art_size":%d,"file_size":%d,`+
				`"version":%q,"ok":%s,"why":%q}`,
			jsonSafe(a.Stem), jsonSafe(a.Slug), jsonSafe(a.Title), a.AppID,
			jsonSafe(capsNames(a.Caps)), a.Period, a.Serial, a.NSet, a.ArtSz, a.Bytes,
			jsonSafe(a.Version),
			map[bool]string{true: "true", false: "false"}[a.Why == ""], jsonSafe(a.Why)))
	}
	writeJSON(w, 200, fmt.Sprintf(
		`{"mounted":true,"card":%q,"count":%d,"apps":[%s],"installed":[%s]}`,
		jsonSafe(o.Card), len(rows), strings.Join(rows, ","), installed))
}

// installedJSONLocked formats the flash tier as h_apps's installed rows do. walkable is
// the iface bit — whether the carousel can land on it at all — and on_glass marks the row
// the panel is showing, which is the one uninstall refuses.
//
// Built-ins ARE emitted, first, carrying "compiled":true. That mirrors apps_send_installed
// in orb_app_net.c:776, which walks the whole ledger from ORB_APP_MENU rather than from
// ORB_APP_BASE and stamps each built-in row. The identity tuple and the version are sent
// only for the card rows: a built-in was never a container and the firmware omits those
// keys, so a double that invented zeros here would let a hub test call the shell stale.
func (o *Orb) installedJSONLocked() string {
	var rows []string
	slot := 0
	for _, a := range o.allRowsLocked() {
		if !a.Builtin && (!a.Installed || a.Slug == "") {
			continue
		}
		// Mirrors apps_send_installed: the built-in row carries "compiled":true and a
		// "reason", and NO identity tuple or version, because a compiled row was never a
		// container and printing a zero version claims an answer about no file.
		tail := "}"
		if a.Builtin {
			tail = fmt.Sprintf(`,"compiled":true,"reason":"%s"}`, jsonSafe(a.Why))
		} else {
			tail = fmt.Sprintf(`,"geom_crc":%d,"code_crc":%d,"code_size":%d,"serial":%d,`+
				`"editor_crc":%d,"version":"%s"}`,
				a.GeomCRC, a.CodeCRC, a.CodeSize, a.Serial, editorCRC(a.Editor), jsonSafe(a.Version))
		}
		rows = append(rows, fmt.Sprintf(
			`{"slug":%q,"title":%q,"slot":%d,"caps":%q,"period_ms":%d,"n_settings":%d,`+
				`"enabled":%s,"failed":%s,"walkable":%s,"on_glass":%s`,
			jsonSafe(a.Slug), jsonSafe(a.Title), slot, jsonSafe(capsNames(a.Caps)),
			a.Period, a.NSet, tf(a.Enabled), tf(a.Failed), tf(!a.Failed), tf(a.OnGlass))+tail)
		slot++
	}
	return strings.Join(rows, ",")
}

// editorCRC is the region's own checksum, computed the way the writer computes it: the
// CRC32 of the region bytes and nothing else. An app that ships no editor reports 0, which
// is the same answer the container gives and the one the hub's reader expects to see, since
// D37 says three zeros means "ships nothing" rather than "refuse".
func editorCRC(b []byte) uint32 {
	if len(b) == 0 {
		return 0
	}
	return crc32.ChecksumIEEE(b)
}

// ledgerJSONLocked formats h_status's "apps" array. It is the broker's pass line, so it
// lists every registered row INCLUDING the built-ins, which is why it walks allRowsLocked
// rather than the card tier. Grants come from the container's declaration, except on a
// failed row, where the broker revoked them at boot and the honest answer is empty.
//
// status is the app's own runtime line, not a state field: orb-B reports "" for all three
// built-ins, so the double reports "" unless a caller sets Status deliberately. A double
// that invented "ready" would teach a hub UI to render a word no device ever sends.
func (o *Orb) ledgerJSONLocked() string {
	var rows []string
	for _, a := range o.allRowsLocked() {
		grants := capsNames(a.Caps)
		faults := 0
		if a.Failed {
			grants, faults = "", 1
		}
		rows = append(rows, fmt.Sprintf(
			`{"slug":%q,"grants":[%s],"declared":[%s],"calls":0,"denials":0,`+
				`"denied_mask":0,"beat_ms":%d,"cfg_used":0,"faults":%d,"failed":%s,`+
				`"status":%q}`,
			jsonSafe(a.Slug), quotedList(grants), quotedList(capsNames(a.Caps)),
			a.Period, faults, tf(a.Failed), jsonSafe(a.Status)))
	}
	return "[" + strings.Join(rows, ",") + "]"
}

// tf spells a bool the way C's ternaries in the firmware's format strings do. %t would
// work here and is the wrong kind of clever two files from a parser that only ever sees
// true/false, so the spelling stays visible.
func tf(b bool) string {
	if b {
		return "true"
	}
	return "false"
}

func quotedList(csv string) string {
	if csv == "" {
		return ""
	}
	var out []string
	for _, p := range strings.Split(csv, ", ") {
		out = append(out, fmt.Sprintf("%q", p))
	}
	return strings.Join(out, ",")
}

// serveAppInstall mirrors h_apps_install. A bad or missing id is a 400 and a running card
// job is a 409 rather than a 422 — the app may be perfectly installable and the answer is
// to wait for the first copy and reload. The interesting case is a well-formed stem that
// names no container: orb_app_install_post only validates the name and queues the job, so
// the device genuinely does not yet know it will fail and answers 200 installing. The
// double reproduces that; the failure shows up on the next listing instead of here.
func (o *Orb) serveAppInstall(w http.ResponseWriter, r *http.Request) {
	stem := r.URL.Query().Get("id")
	if !sdNameOK(stem) {
		writeJSON(w, 400, noStemBody)
		return
	}
	o.mu.Lock()
	if o.Busy {
		o.mu.Unlock()
		writeJSON(w, 409, `{"error":"a card job is already running"}`)
		return
	}
	var row *CardApp
	for _, a := range o.apps {
		if a.Stem == stem {
			row = a
			break
		}
	}
	o.mu.Unlock()
	writeJSON(w, 200, fmt.Sprintf(`{"state":"installing","stem":%q}`, jsonSafe(stem)))
	if row == nil {
		return // queued; the job that runs after this reply is what learns the truth
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	row.Installed, row.Enabled, row.Failed = true, true, row.Why != ""
}

// serveAppEnable mirrors h_app_enable: both 400s come before any lookup, and refusals come
// back through door_http_status's mapping — an unknown slug is ESP_ERR_NOT_FOUND, so a
// 404; an app that cannot be shown is ESP_ERR_INVALID_STATE, so a 409.
func (o *Orb) serveAppEnable(w http.ResponseWriter, r *http.Request) {
	slug, on := appID(r), r.URL.Query().Get("on")
	if slug == "" {
		writeJSON(w, 400, missingIDBody)
		return
	}
	if on != "0" && on != "1" {
		writeJSON(w, 400, badOnBody)
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	a, code, body := o.doorAppLocked(slug)
	if a == nil {
		writeJSON(w, code, body)
		return
	}
	// Disabling the row the dial is showing is refused before anything else: the panel would
	// otherwise drop the app under the user's finger while the registry still claimed it.
	if on == "0" && a.OnGlass {
		writeJSON(w, 409, onDialBody)
		return
	}
	if on == "1" {
		if a.NoIface {
			writeJSON(w, 409, noIfaceBody)
			return
		}
		if a.Failed {
			// Pressing ON on a failed app IS the revive gesture (C2): the device clears the
			// fault record first and refuses only if NVS won't take the write. A double that
			// flat-refused here hides the one control that recovers a failed card, and the
			// hub gets written to show a dead end where the orb offers a repair.
			if o.ReviveFails {
				writeJSON(w, 500, reviveFailBody) // ESP_FAIL: door_http_status's default arm
				return
			}
			a.Failed = false
		}
	}
	a.Enabled = on == "1"
	writeJSON(w, 200, fmt.Sprintf(`{"state":%q,"slug":%q}`,
		map[bool]string{true: "enabled", false: "disabled"}[a.Enabled], jsonSafe(slug)))
}

// serveAppFace mirrors h_app_face, the glass door. The gates run in the firmware's
// order: id, then the name/busy/installed lookup, then enabled, then fault, then
// walkable. The lookup is doorAppLocked's own first three arms and NOT its built-in
// refusal: h_app_face has no such gate, because the shell is a face like any other and
// parking on it is a legal thing to ask a dial. A double that refused it there would
// hide a state the real orb accepts, which is the drift this file's header is about.
// The double cannot boot, so the park is applied in place — the named row takes the
// glass and every other row loses it, which is exactly the ledger state a rebooted dial
// reports. A row already on the glass answers `parked` and changes nothing, the same
// no-reboot answer the firmware gives.
func (o *Orb) serveAppFace(w http.ResponseWriter, r *http.Request) {
	slug := appID(r)
	if slug == "" {
		writeJSON(w, 400, missingIDBody)
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	if !sdNameOK(slug) {
		writeJSON(w, 400, notUsableNameBody)
		return
	}
	if o.JobBusy {
		writeJSON(w, 409, doorBusyBody)
		return
	}
	a := o.apps[slug]
	if a == nil || !a.Installed {
		writeJSON(w, 404, notFoundBody)
		return
	}
	if !a.Enabled {
		writeJSON(w, 409, notEnabledBody)
		return
	}
	if a.Failed {
		writeJSON(w, 409, failedFaceBody)
		return
	}
	if a.NoIface {
		writeJSON(w, 409, noIfaceBody)
		return
	}
	if a.OnGlass {
		writeJSON(w, 200, fmt.Sprintf(`{"state":"parked","slug":%q}`, jsonSafe(slug)))
		return
	}
	for _, other := range o.apps {
		other.OnGlass = false
	}
	a.OnGlass = true
	writeJSON(w, 200, fmt.Sprintf(`{"state":"rebooting","slug":%q}`, jsonSafe(slug)))
}

// serveAppClear mirrors h_app_clear (7c9ff63), the third owner door. It skips
// door_open's gates on purpose: the row a caller needs here is a QUARANTINED built-in,
// which reads installed false after its two strikes, so door_open's NOT_FOUND and
// NOT_SUPPORTED arms would both refuse the one revival that does not wipe the owner's
// WiFi. The firmware reads one argument, answers its own sentences, and names the
// reboot in the success body rather than claiming it happened.
func (o *Orb) serveAppClear(w http.ResponseWriter, r *http.Request) {
	slug := appID(r)
	if slug == "" {
		writeJSON(w, 400, missingIDBody)
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	a := o.apps[slug]
	if a == nil {
		writeJSON(w, 404, notFoundBody)
		return
	}
	a.Failed = false
	a.Installed = true
	a.Enabled = true
	writeJSON(w, 200, fmt.Sprintf(`{"state":"cleared","slug":%q,"reboot":true}`, jsonSafe(slug)))
}

// serveAppUninstall mirrors h_app_uninstall. Uninstalling the row on the glass is the
// INVALID_STATE the firmware refuses: the carousel cannot walk out of an app it is showing,
// so the caller has to leave it first. The double clears the install rather than the card
// row, matching the device — the staged container stays where it is and can go back in.
func (o *Orb) serveAppUninstall(w http.ResponseWriter, r *http.Request) {
	slug := appID(r)
	if slug == "" {
		writeJSON(w, 400, missingIDBody)
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	a, code, body := o.doorAppLocked(slug)
	if a == nil {
		writeJSON(w, code, body)
		return
	}
	// The page confirms first; the device does not trust the confirm. Anything on the dial is
	// refused with the same sentence the disable door uses.
	if a.OnGlass {
		writeJSON(w, 409, onDialBody)
		return
	}
	if o.UninstallSticks {
		writeJSON(w, 409, uninstallHoldBody)
		return
	}
	a.Installed, a.Enabled = false, false
	writeJSON(w, 200, fmt.Sprintf(`{"state":"uninstalled","slug":%q}`, jsonSafe(slug)))
}

// serveAppUpload mirrors h_app_upload: 411 with no declared length, 413 refused on the
// declared length before a byte is read, 409 against a concurrent upload, and the
// destination taken from the container's own header rather than any caller-supplied name.
// A theme container is named rather than miscalled — the two magics differ on purpose.
// The double reads the header words it needs and stops there: it does not check the
// geometry CRC, so a test that needs a CRC refusal needs a real orb.
func (o *Orb) serveAppUpload(w http.ResponseWriter, r *http.Request) {
	if r.ContentLength < 0 {
		writeJSON(w, 411, `{"error":"Content-Length required"}`)
		return
	}
	if r.ContentLength > appUploadMa {
		writeJSON(w, 413, fmt.Sprintf(`{"error":"too large","max":%d}`, appUploadMa))
		return
	}
	body, ok := readBoundedBody(w, r, appUploadMa)
	if !ok {
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.Busy {
		writeJSON(w, 409, `{"error":"an upload is already in progress"}`)
		return
	}
	if len(body) < appHdrWords*4 {
		writeJSON(w, 422, `{"error":"rejected","detail":"shorter than the header"}`)
		return
	}
	hdr := make([]uint32, appHdrWords)
	for i := range hdr {
		hdr[i] = binary.LittleEndian.Uint32(body[i*4:])
	}
	switch hdr[0] {
	case appThemeMgc:
		writeJSON(w, 422, `{"error":"rejected","detail":"that is a theme container"}`)
		return
	case appMagic:
	default:
		writeJSON(w, 422, `{"error":"rejected","detail":"not a valid app file"}`)
		return
	}
	slug := headerField(body, wSlug, slugBytes)
	if !sdNameOK(slug) {
		writeJSON(w, 422, `{"error":"rejected","detail":"the header names no usable app"}`)
		return
	}
	prev := o.apps[slug]
	if prev != nil && prev.Stem != "" && r.URL.Query().Get("overwrite") != "1" {
		writeJSON(w, 409, `{"error":"a different app with that name is already on the card",`+
			`"overwrite":false}`)
		return
	}
	up := CardApp{Stem: slug, Slug: slug, Title: headerField(body, wTitle, titleBytes),
		AppID: hdr[wAppID], Caps: hdr[wCaps], Serial: hdr[wSerial], Period: hdr[wPeriodMs],
		NSet: hdr[wNSettings], ArtSz: hdr[wArtSize], Bytes: uint32(len(body))}
	if prev != nil {
		*prev = up
	} else {
		o.apps[slug] = &up
		o.appsOrder = append(o.appsOrder, slug)
	}
	// h_app_upload writes the container INTO the volume — /sd/apps/<slug>.orbapp, the
	// ("orb","apps") source's own directory — so a device that took an upload answers
	// /api/v1/sd/ls with the file and /api/v1/sd/rm can erase it. A double that kept the
	// row only in its app map made the file manager's view of a hub-pushed app a lie, and
	// the Apps face's Remove-from-card door untestable.
	o.fm.ensureStoreDirs()
	o.fm.files["apps/"+slug+".orbapp"] = int64(len(body))
	writeJSON(w, 200, fmt.Sprintf(`{"state":"stored","bytes":%d}`, len(body)))
}

// headerField lifts a fixed-width NUL-padded header string out of a raw container body.
func headerField(body []byte, word, width int) string {
	start := word * 4
	if start+width > len(body) {
		return ""
	}
	return strings.TrimRight(string(body[start:start+width]), "\x00")
}

// BuildAppContainer writes a minimal container the double will accept, so a hub test can
// exercise the upload path without a fixture binary checked into the repo. It is the
// writer's mirror in the same sense the header words are: the field positions come from the
// same enum, and only the fields the double reads are filled in.
func BuildAppContainer(slug, title string, caps, periodMs, serial uint32) []byte {
	body := make([]byte, appHdrWords*4+32)
	binary.LittleEndian.PutUint32(body[0:], appMagic)
	binary.LittleEndian.PutUint32(body[4:], 1) // ORBAPP_FORMAT
	binary.LittleEndian.PutUint32(body[8:], 1) // ORBAPP_GEOM_ABI
	binary.LittleEndian.PutUint32(body[wSerial*4:], serial)
	binary.LittleEndian.PutUint32(body[wAppID*4:], 0x5A000001)
	binary.LittleEndian.PutUint32(body[wPeriodMs*4:], periodMs)
	binary.LittleEndian.PutUint32(body[wCaps*4:], caps)
	copy(body[wSlug*4:], slug)
	copy(body[wTitle*4:], title)
	return body
}

// serveAppEditor answers GET /api/v1/apps/editor?id=<stem> the way h_app_editor
// does (orb_net.c): the raw region as application/octet-stream, no JSON envelope,
// and the device's own sentence as the 404 body when a row ships no editor. The
// double checks no CRC — the hub verifies the archive once it lands, and the
// device's checksum test is host_orbapp's, over the writer's own bytes.
func (o *Orb) serveAppEditor(w http.ResponseWriter, r *http.Request) {
	stem := strings.TrimSpace(r.URL.Query().Get("id"))
	o.mu.Lock()
	defer o.mu.Unlock()
	o.calls = append(o.calls, "GET /api/v1/apps/editor?id="+stem)
	if stem == "" {
		writeJSON(w, 400, `{"error":"id is missing"}`)
		return
	}
	if !o.Mounted {
		writeJSON(w, 404, `{"error":"no card in the socket"}`)
		return
	}
	row := o.apps[stem]
	// The card tier and the flash row are the same registry here, as they are on a
	// device: the region lives in the card file, so a row with no Editor answers
	// the way a board would for an app that ships none.
	if row == nil || row.Stem == "" {
		writeJSON(w, 404, `{"error":"no such app on the card"}`)
		return
	}
	if len(row.Editor) == 0 {
		writeJSON(w, 404, `{"error":"this app ships no editor"}`)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Length", fmt.Sprintf("%d", len(row.Editor)))
	w.WriteHeader(200)
	_, _ = w.Write(row.Editor)
}
