package main

// H3-hub-1: the two send doors the S2-2 scope layer left unscoped.
//
// Every other library, design and send route on the editor listener asks whose app the request
// is about; /api/theme/commit and /api/theme/upload-status named only an address or a token, so
// until now clock's editor could spend a token that flights' refused send had been handed, and
// could read the progress line of a send of flights' theme to any orb. Both answers are the
// hub's own knowledge of the send, so both scopes now come from the hub's own record of it: the
// app a token was issued for, and the app a running send's container carried.
//
// These are wire claims in gateEditorCall's shape, one per app pair, plus the two claims the
// commit record has to back its door with: it is bounded, and it forgets a spent token.

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// commitHub is a hub with a fake orb and a shelf in a temp dir, so the theme a send keeps on the
// shelf lands in the test rather than in the operator's library.
func commitHub(t *testing.T) (*Server, string, func() orbCall, *orbfake.Orb) {
	t.Helper()
	s, ip, called, orb := fakeOrbHubWith(t, orbfake.New())
	dir := t.TempDir()
	s.lib = &Library{AppsDir: filepath.Join(dir, "apps"), ThemesDir: filepath.Join(dir, "themes")}
	if err := os.MkdirAll(s.lib.ThemesDir, 0o755); err != nil {
		t.Fatal(err)
	}
	return s, ip, called, orb
}

// commitThemeFor bakes a minimal FORMAT-3 container that themes `app`, the one fixture shape
// these tests need and the one orbfake.Container does not write: its bake carries clock's slug,
// and the app a container names IS the scope H3-hub-1 weighs. Same layout field for field — one
// section, one plate row, no art, the index record and both checksums over the slug given — so
// the hub's own reader accepts it the way it accepts the double's.
func commitThemeFor(t *testing.T, app, name string) []byte {
	t.Helper()
	const (
		hdrBytes   = 128
		idxWords   = 10
		idxHead    = 8
		sector     = 4096
		nameWord   = 14
		hdrCRCWord = 28
	)
	words := func(vals ...uint32) []byte {
		var b []byte
		for _, v := range vals {
			var w [4]byte
			binary.LittleEndian.PutUint32(w[:], v)
			b = append(b, w[:]...)
		}
		return b
	}
	slug := []byte(app)
	blob := words(1, 0, 26, uint32(len(slug)))           // n_elem, n_spr, n_scal, n_slug
	blob = append(blob, words(make([]uint32, 26)...)...) // the app's own defaults, all zero
	blob = append(blob, words(1, 3, 0, 0, 1, 0, 0, 466, 466, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)...)
	blob = append(blob, slug...)
	for len(blob)%4 != 0 {
		blob = append(blob, 0)
	}
	record := make([]byte, idxWords*4)
	blobOff := uint32(idxHead + idxWords*4)
	binary.LittleEndian.PutUint16(record[0:], uint16(blobOff+uint32(len(blob))-uint32((len(slug)+3)&^3)))
	binary.LittleEndian.PutUint16(record[2:], uint16(len(slug)))
	binary.LittleEndian.PutUint16(record[4:], 3) // abi_min
	binary.LittleEndian.PutUint16(record[6:], 3) // abi_max
	binary.LittleEndian.PutUint32(record[8:], 1) // n_elem
	binary.LittleEndian.PutUint32(record[12:], blobOff)
	binary.LittleEndian.PutUint32(record[16:], uint32(len(blob)))
	binary.LittleEndian.PutUint32(record[20:], crc32.ChecksumIEEE(blob))
	region := append(words(1, 0), record...)
	region = append(region, blob...)

	artOff := uint32(hdrBytes+len(region)+sector-1) & ^uint32(sector-1)
	b := make([]byte, artOff)
	put := func(word int, v uint32) { binary.LittleEndian.PutUint32(b[word*4:], v) }
	put(0, 0x474F5254) // the bundle magic
	put(1, 3)          // FORMAT 3
	put(2, 3)          // the geometry ABI
	put(3, uint32(len(b)))
	put(4, hdrBytes)
	put(5, uint32(len(region)))
	put(6, crc32.ChecksumIEEE(region))
	put(9, 1) // n_sections
	put(13, artOff)
	copy(b[nameWord*4:], name)
	z := append([]byte(nil), b[:hdrBytes]...)
	for i := hdrCRCWord * 4; i < hdrCRCWord*4+4; i++ {
		z[i] = 0
	}
	put(hdrCRCWord, crc32.ChecksumIEEE(z))
	copy(b[hdrBytes:], region)
	return b
}

// commitBody is the {ip, commit} envelope a page sends to spend a staged commit.
func commitBody(ip, tok string) []byte {
	return []byte(fmt.Sprintf(`{"ip":%q,"commit":%q}`, ip, tok))
}

// 1. A commit token belongs to the app whose refused send it came from.
func TestEditorCommitScopedToItsApp(t *testing.T) {
	s, ip, called, orb := commitHub(t)
	const (
		own      = "flights"
		other    = "clock"
		ownMsg   = "this editor is the clock app's: it cannot reach the flights app's themes"
		noneMsg  = "this editor is the flights app's: it can only commit a theme it sent"
		neverTok = "0000000000000000"
	)
	dials := func() int { return len(orb.Calls()) }
	container := commitThemeFor(t, own, "Sunny")

	// The token these claims are about: flights' own send, refused as a duplicate by the card
	// that already files that name, hands it back. A refusal for another app's container never
	// reaches this door at all — editorScopeContainer refuses that upload first — so a record
	// is written only for a send the app itself made.
	if env := decodeEnv(t, gateEditorCall(t, s, own, http.MethodPost, "/api/theme/upload?ip="+ip, container, nil)); env["ok"] != true {
		t.Fatalf("flights' own first send: %v", env)
	}
	env := decodeEnv(t, gateEditorCall(t, s, own, http.MethodPost, "/api/theme/upload?ip="+ip, container, nil))
	tok, _ := env["commit"].(string)
	if env["duplicate"] != true || tok == "" {
		t.Fatalf("the second send = %v, want the duplicate refusal with its commit token", env)
	}

	// Clock's editor, holding a token it was never handed, is refused with the app the token is
	// the rule about named — and the orb is never dialed, which is the difference between a
	// refusal and a delay.
	before := dials()
	gateRefused(t, gateEditorCall(t, s, other, http.MethodPost, "/api/theme/commit", commitBody(ip, tok), nil),
		http.StatusForbidden, ownMsg, "clock's editor spending flights' commit token")
	if n := dials() - before; n != 0 {
		t.Errorf("the refused commit let the hub dial %d times, want none", n)
	}

	// Flights' own editor with the SAME token reaches the handler, and the handler dials the
	// commit it was asked for: the scope layer narrowed this door, it did not close it.
	rec := gateEditorCall(t, s, own, http.MethodPost, "/api/theme/commit", commitBody(ip, tok), nil)
	if env = decodeEnv(t, rec); env["ok"] != true {
		t.Fatalf("flights' own commit of its own token: %v", env)
	}
	if c := called(); c.path != "/api/v1/sd/theme" || !strings.Contains(c.query, "commit="+tok) {
		t.Errorf("the orb saw %s ?%s, want POST /api/v1/sd/theme?commit=<token>", c.path, c.query)
	}

	// A token this hub never issued named no app to begin with, and a spent one names none any
	// more: both are refused with the rule rather than the other app's sentence, and the refusal
	// is the hub's own — the orb is not dialed for either.
	before = dials()
	for _, c := range []struct{ tok, why string }{
		{neverTok, "the flights editor holding a token the hub never handed out"},
		{tok, "flights' own commit of a token it had already spent"},
	} {
		gateRefused(t, gateEditorCall(t, s, own, http.MethodPost, "/api/theme/commit", commitBody(ip, c.tok), nil),
			http.StatusForbidden, noneMsg, c.why)
	}
	if n := dials() - before; n != 0 {
		t.Errorf("the two unknown-token commits let the hub dial %d times, want none", n)
	}

	// The control port keeps every app: the hub page spends the next refusal's token, and that
	// spend forgets the record too — so the app's own editor is refused a token the hub page
	// already committed for it.
	if env = decodeEnv(t, gateEditorCall(t, s, own, http.MethodPost, "/api/theme/upload?ip="+ip, container, nil)); env["duplicate"] != true {
		t.Fatalf("a third send = %v, want a second duplicate refusal", env)
	}
	spent, _ := env["commit"].(string)
	if spent == "" {
		t.Fatalf("that refusal carried no token: %v", env)
	}
	if env = decodeEnv(t, gateControlCall(t, s, http.MethodPost, "/api/theme/commit", commitBody(ip, spent),
		map[string]string{tokenHeader: s.token})); env["ok"] != true {
		t.Fatalf("the hub page committing flights' token: %v", env)
	}
	gateRefused(t, gateEditorCall(t, s, own, http.MethodPost, "/api/theme/commit", commitBody(ip, spent), nil),
		http.StatusForbidden, noneMsg, "an editor committing a token the hub page had already spent")

	// A body this layer cannot read is the handler's to judge, not a scope refusal: not JSON at
	// all, no token at all, and one longer than the {ip, commit} envelope is ever that. Each
	// reaches the sentence the handler wrote for it instead of the scope layer's.
	for _, c := range []struct{ body, why string }{
		{`not json`, "a body that is not JSON"},
		{`{}`, "a commit call naming no token"},
		{fmt.Sprintf(`{"ip":%q,"commit":%q,"pad":"%s"}`, ip, tok, strings.Repeat("p", 5000)),
			"an envelope over 4 KiB"},
	} {
		rec := gateEditorCall(t, s, other, http.MethodPost, "/api/theme/commit", []byte(c.body), nil)
		if strings.Contains(rec.Body.String(), "it can only commit") ||
			strings.Contains(rec.Body.String(), "cannot reach") {
			t.Errorf("%s from clock's editor was refused as a scope violation: %.200s", c.why, rec.Body)
		}
	}

	// The record itself, which is what the door above stands on: bounded like the hold of bytes
	// it accompanies, the newest kept when one has to go, and an empty token or an empty app
	// records nothing at all.
	for i := 0; i < shelfPendingMax+3; i++ {
		s.holdCommitApp(fmt.Sprintf("tok%02d", i), own)
	}
	if n := len(s.commitApps); n > shelfPendingMax {
		t.Errorf("the record holds %d entries after %d issues, want at most %d", n, shelfPendingMax+3, shelfPendingMax)
	}
	if s.commitAppOf("tok06") != own {
		t.Error("the newest token was dropped instead of an older one")
	}
	s.holdCommitApp("", own)
	s.holdCommitApp("tok-no-app", "")
	if n := len(s.commitApps); n != shelfPendingMax {
		t.Errorf("an empty token or an empty app entered the record: %d entries, want %d", n, shelfPendingMax)
	}
	s.dropCommitApp("tok06")
	for _, q := range []string{"tok06", "never-issued"} {
		if app := s.commitAppOf(q); app != "" {
			t.Errorf("%q names %q in the record, want no record", q, app)
		}
	}
}

// 2. A send's progress line is the sending app's own. The subject is not in the request — only
// an address — so the scope is whatever the hub is running there, which is the app the send's
// own container named (themeSend.App, never in the answer).
func TestEditorSendStatusScopedToItsApp(t *testing.T) {
	s, ip, _, _ := commitHub(t)
	const (
		own        = "flights"
		other      = "clock"
		ownMsg     = "this editor is the clock app's: it cannot reach the flights app's themes"
		otherMsg   = "this editor is the flights app's: it cannot reach the clock app's themes"
		statusPath = "/api/theme/upload-status?ip="
	)
	status := func(slug string) *httptest.ResponseRecorder {
		return gateEditorCall(t, s, slug, http.MethodGet, statusPath+ip, nil, nil)
	}
	phase := func(t *testing.T, rec *httptest.ResponseRecorder) any {
		t.Helper()
		if rec.Code != http.StatusOK {
			t.Fatalf("status %d body %.200s, want the witness answered", rec.Code, rec.Body)
		}
		d, _ := decodeEnv(t, rec)["data"].(map[string]any)
		return d["phase"]
	}

	// Nothing has been sent to this address yet, so the line says idle to every editor and to
	// the hub page, exactly as it did before this door was scoped.
	for _, slug := range []string{own, other} {
		if p := phase(t, status(slug)); p != "idle" {
			t.Errorf("%s's editor on an idle orb: phase %v, want idle", slug, p)
		}
	}
	if p := phase(t, gateControlCall(t, s, http.MethodGet, statusPath+ip, nil,
		map[string]string{tokenHeader: s.token})); p != "idle" {
		t.Errorf("the control port on an idle orb: phase %v, want idle", p)
	}

	// A send of a flights theme, made by flights' own editor: the line is flights' to read.
	if env := decodeEnv(t, gateEditorCall(t, s, own, http.MethodPost, "/api/theme/upload?ip="+ip,
		commitThemeFor(t, own, "Sunny"), nil)); env["ok"] != true {
		t.Fatalf("flights' own send: %v", env)
	}
	if p := phase(t, status(own)); p != "stored" {
		t.Errorf("flights' editor after its own send: phase %v, want the stored verdict", p)
	}
	gateRefused(t, status(other), http.StatusForbidden, ownMsg, "clock's editor reading flights' send")
	// The refusal is the only change: the answer the owning app gets carries no app at all, so
	// the line describes a transfer rather than naming one to every poller.
	if d, _ := decodeEnv(t, status(own))["data"].(map[string]any); d["app"] != nil || d["App"] != nil {
		t.Errorf("the witness = %v, want no app in the answer", d)
	}
	// The hub page is trusted with every app and reads what it always read.
	if p := phase(t, gateControlCall(t, s, http.MethodGet, statusPath+ip, nil,
		map[string]string{tokenHeader: s.token})); p != "stored" {
		t.Errorf("the control port after flights' send: phase %v, want the stored verdict unchanged", p)
	}

	// The rule is an editor's own app and not one app being privileged: clock sends a clock
	// theme to the same orb and reads its own line, and now it is flights that is refused.
	if env := decodeEnv(t, gateEditorCall(t, s, other, http.MethodPost, "/api/theme/upload?ip="+ip,
		orbfake.Container("Aurora"), nil)); env["ok"] != true {
		t.Fatalf("clock's own send: %v", env)
	}
	if p := phase(t, status(other)); p != "stored" {
		t.Errorf("clock's editor after its own send: phase %v, want the stored verdict", p)
	}
	gateRefused(t, status(own), http.StatusForbidden, otherMsg, "flights' editor reading clock's send")

	// The precheck verdict is a tracked start too: asking the duplicate question about a flights
	// bake moves the line back to flights' side of the pair without a byte crossing the room.
	if env := decodeEnv(t, gateEditorCall(t, s, own, http.MethodPost,
		"/api/theme/upload?ip="+ip+"&precheck=1", commitThemeFor(t, own, "Sunny")[:400], nil)); env["ok"] != true {
		t.Fatalf("flights' precheck: %v", env)
	}
	gateRefused(t, status(other), http.StatusForbidden, ownMsg, "clock's editor reading flights' precheck")

	// And the streamed door — the theme editor's own Send, which is the send this rule exists to
	// cover — is tracked the same way: refused to the other app, answered to its own.
	if rec := gateEditorCall(t, s, own, http.MethodPost, "/api/theme/upload?ip="+ip+"&overwrite=1",
		commitThemeFor(t, own, "Sunny"), map[string]string{"Accept": "application/x-ndjson"}); rec.Code != http.StatusOK {
		t.Fatalf("flights' streamed send: status %d body %.200s", rec.Code, rec.Body)
	}
	gateRefused(t, status(other), http.StatusForbidden, ownMsg, "clock's editor reading flights' streamed send")
	if p := phase(t, status(own)); p != "stored" {
		t.Errorf("flights' editor after its own streamed send: phase %v, want the door's verdict", p)
	}
}
