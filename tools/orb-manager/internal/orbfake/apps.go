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
	appNameMax  = 31                // ORB_SD_NAME_MAX
	appHdrWords = 39                // through W_ART_SIZE, the last word the double reads
	appUploadMa = 0x40000 + 0x20000 // ORB_APP_UPLOAD_MAX
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
}

// sdNameOK is orb_sd_name_ok (orb_sd.c): 1..31 bytes of [A-Za-z0-9._-], no leading '.'.
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
func capsNames(caps uint32) string {
	var parts []string
	for i := 0; i < 4; i++ {
		if caps&(1<<uint(i)) != 0 {
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
		if _, had := o.apps[a.Slug]; !had {
			o.appsOrder = append(o.appsOrder, a.Slug)
		}
		o.apps[a.Slug] = &a
	}
	return o
}

// cardRowsLocked is the card tier: every row with a stem, in registration order.
func (o *Orb) cardRowsLocked() []*CardApp {
	var out []*CardApp
	for _, slug := range o.appsOrder {
		if a := o.apps[slug]; a != nil && a.Stem != "" {
			out = append(out, a)
		}
	}
	return out
}

func (o *Orb) installedRowsLocked() []*CardApp {
	var out []*CardApp
	for _, slug := range o.appsOrder {
		if a := o.apps[slug]; a != nil && a.Installed {
			out = append(out, a)
		}
	}
	return out
}

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
				`"ok":%s,"why":%q}`,
			jsonSafe(a.Stem), jsonSafe(a.Slug), jsonSafe(a.Title), a.AppID,
			jsonSafe(capsNames(a.Caps)), a.Period, a.Serial, a.NSet, a.ArtSz, a.Bytes,
			map[bool]string{true: "true", false: "false"}[a.Why == ""], jsonSafe(a.Why)))
	}
	writeJSON(w, 200, fmt.Sprintf(
		`{"mounted":true,"card":%q,"count":%d,"apps":[%s],"installed":[%s]}`,
		jsonSafe(o.Card), len(rows), strings.Join(rows, ","), installed))
}

// installedJSONLocked formats the flash tier as h_apps's installed rows do. walkable is
// the iface bit — whether the carousel can land on it at all — and on_glass marks the row
// the panel is showing, which is the one uninstall refuses.
func (o *Orb) installedJSONLocked() string {
	var rows []string
	for i, a := range o.installedRowsLocked() {
		rows = append(rows, fmt.Sprintf(
			`{"slug":%q,"title":%q,"slot":%d,"caps":%q,"period_ms":%d,"n_settings":%d,`+
				`"enabled":%s,"failed":%s,"walkable":%s,"on_glass":%s}`,
			jsonSafe(a.Slug), jsonSafe(a.Title), i, jsonSafe(capsNames(a.Caps)),
			a.Period, a.NSet, tf(a.Enabled), tf(a.Failed), tf(!a.Failed), tf(a.OnGlass)))
	}
	return strings.Join(rows, ",")
}

// ledgerJSONLocked formats h_status's "apps" array. It is the broker's pass line, so it
// lists every registered row including the failed and disabled ones the carousel hides:
// the ledger exists precisely to answer "what can this thing do" about apps that are not
// on screen. The double reports the counters a freshly booted orb reports (nothing called,
// nothing denied) and takes grants from the container's declaration, except on a failed
// row, where the broker revoked them at boot and the honest answer is an empty list.
func (o *Orb) ledgerJSONLocked() string {
	var rows []string
	for _, a := range o.installedRowsLocked() {
		grants := capsNames(a.Caps)
		faults := 0
		status := "ready"
		switch {
		case a.Failed:
			grants, faults, status = "", 1, "failed"
		case !a.Enabled:
			status = "disabled"
		case a.OnGlass:
			status = "running"
		}
		rows = append(rows, fmt.Sprintf(
			`{"slug":%q,"grants":[%s],"declared":[%s],"calls":0,"denials":0,`+
				`"denied_mask":0,"beat_ms":%d,"cfg_used":0,"faults":%d,"failed":%s,`+
				`"status":%q}`,
			jsonSafe(a.Slug), quotedList(grants), quotedList(capsNames(a.Caps)),
			a.Period, faults, tf(a.Failed), status))
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
		writeJSON(w, 400, `{"error":"id is missing or not a usable app name"}`)
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
	q := r.URL.Query()
	slug, on := q.Get("id"), q.Get("on")
	if slug == "" {
		writeJSON(w, 400, `{"error":"id is missing"}`)
		return
	}
	if on != "0" && on != "1" {
		writeJSON(w, 400, `{"error":"on must be 0 or 1"}`)
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	a := o.apps[slug]
	if a == nil || !a.Installed {
		writeJSON(w, 404, fmt.Sprintf(`{"error":"no installed app %q"}`, jsonSafe(slug)))
		return
	}
	if on == "1" && a.Failed {
		writeJSON(w, 409, `{"error":"the app stays failed"}`)
		return
	}
	a.Enabled = on == "1"
	writeJSON(w, 200, fmt.Sprintf(`{"state":%q,"slug":%q}`,
		map[bool]string{true: "enabled", false: "disabled"}[a.Enabled], jsonSafe(slug)))
}

// serveAppUninstall mirrors h_app_uninstall. Uninstalling the row on the glass is the
// INVALID_STATE the firmware refuses: the carousel cannot walk out of an app it is showing,
// so the caller has to leave it first. The double clears the install rather than the card
// row, matching the device — the staged container stays where it is and can go back in.
func (o *Orb) serveAppUninstall(w http.ResponseWriter, r *http.Request) {
	slug := r.URL.Query().Get("id")
	if slug == "" {
		writeJSON(w, 400, `{"error":"id is missing"}`)
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	a := o.apps[slug]
	if a == nil || !a.Installed {
		writeJSON(w, 404, fmt.Sprintf(`{"error":"no installed app %q"}`, jsonSafe(slug)))
		return
	}
	if a.OnGlass {
		writeJSON(w, 409, `{"error":"it is on the glass; leave it before uninstalling"}`)
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
