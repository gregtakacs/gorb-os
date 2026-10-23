package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// §32's three hub doors, tested on the wire: the precheck that answers a duplicate
// before bytes cross the room, the store verdict that replaces the ack-and-hope the
// send used to end with, and the status witness that speaks while the POST blocks.
// They ride the same fakeOrbHub double as theme_test.go — the double now lands the
// store an upload arms, as h_theme_upload does, so the verdict these handlers report
// is read from the card state the double maintains.

// The precheck is a question, not a send: the device door must never see the bytes.
func TestAPrecheckAnswersBeforeAnyBytesCrossTheRoom(t *testing.T) {
	s, ip, _, f := fakeOrbHub(t)
	file := orbfake.Container("aurora")
	// Seed the collision the way the device creates one: an overwrite upload moves
	// the card's row to a serial the copy still in hand does not carry — same name,
	// different bytes, the §5 refusal.
	if _, err := s.device.ThemeUpload(ip, file, true); err != nil {
		t.Fatalf("seed overwrite: %v", err)
	}
	doorCalls := func() int {
		n := 0
		for _, l := range f.Calls() {
			if strings.HasPrefix(l, "POST /api/v1/sd/theme") {
				n++
			}
		}
		return n
	}
	before := doorCalls()
	id, _ := orbnet.ThemeIDOf("aurora")
	mux := http.NewServeMux()
	s.routes(mux)
	rec := postThemeRaw(t, s, mux, "/api/theme/upload?ip="+ip+"&precheck=1", file)
	env := decodeEnv(t, rec)
	data, _ := env["data"].(map[string]any)
	if env["ok"] != true || data["duplicate"] != true || data["id"] != id {
		t.Fatalf("precheck = %v, want duplicate true for %s", env["data"], id)
	}
	if got := doorCalls(); got != before {
		t.Errorf("the upload door was dialed %d times past the seed; precheck must answer with zero bytes crossing the room", got-before)
	}
}

// The whole reason theme.go now blocks: the store the upload door armed tore (the
// fake's TearStore scripts the 47% USB-reset of 2026-10-22), and the response must
// say torn — not the old silent "stored" that let an operator boot a base face
// believing the swap had landed.
func TestATornStoreComesBackAsATornVerdict(t *testing.T) {
	s, ip, _, f := fakeOrbHub(t)
	f.TearStore = true
	file := orbfake.Container("torn-dial")
	mux := http.NewServeMux()
	s.routes(mux)
	rec := postThemeRaw(t, s, mux, "/api/theme/upload?ip="+ip, file)
	env := decodeEnv(t, rec)
	data, _ := env["data"].(map[string]any)
	if env["ok"] != true {
		t.Fatalf("body = %v, want the upload accepted and the verdict carried", env)
	}
	if data["store"] != "torn" {
		t.Errorf("store = %v, want torn", data["store"])
	}
	if note, _ := data["store_note"].(string); !strings.Contains(note, "header") {
		t.Errorf("store_note = %q, want the staged witness's reason", note)
	}
}

// The status witness the pages poll while the POST blocks: idle before anything
// moves, the live phase after the handler records it, keyed per address.
func TestASendStatusWitnessReportsThePhase(t *testing.T) {
	s, ip, _, _ := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	env := decodeEnv(t, getSendStatus(t, s, mux, ip))
	data, _ := env["data"].(map[string]any)
	if data["phase"] != "idle" {
		t.Errorf("phase before any send = %v, want idle", data["phase"])
	}
	s.setSend(ip, func(st *themeSend) { st.Phase, st.Sent, st.Total = "storing", 1234, 5678 })
	data, _ = decodeEnv(t, getSendStatus(t, s, mux, ip))["data"].(map[string]any)
	if data["phase"] != "storing" || data["sent"].(float64) != 1234 {
		t.Errorf("witness = %v, want the hub's live phase and count", data)
	}
}

// postThemeRaw dials an upload-shaped route: the body is the container bytes, not
// the JSON envelope the other theme calls carry.
func postThemeRaw(t *testing.T, s *Server, mux *http.ServeMux, path string, body []byte) *httptest.ResponseRecorder {
	t.Helper()
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(string(body)))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	return rec
}

// getSendStatus dials the read-only witness route the way a polled page does.
func getSendStatus(t *testing.T, s *Server, mux *http.ServeMux, ip string) *httptest.ResponseRecorder {
	t.Helper()
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/api/theme/upload-status?ip="+ip, nil)
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	return rec
}

// §32.5: a copy the device reports live outruns the idle budget and is still
// held to its verdict. On 2026-10-23 evening the hub answered "lost — no store
// witness within 150 s" for a 5.5 MB send while /api/v1/sd kept returning
// job_busy:true — and that answer released the claim, re-admitting the sweep to
// the port the copy still owned. The wait now follows the device's own word;
// the fake scripts the busy streak longer than a shrunk budget and lands the
// store after it, and the POST must return stored, later than the budget.
func TestAStoreThatOutlivesTheIdleBudgetIsHeldToItsVerdict(t *testing.T) {
	s, ip, _, f := fakeOrbHub(t)
	budget, poll, ceil := themeStoreBudget, themeStorePoll, themeStoreBusyCeiling
	defer func() {
		themeStoreBudget, themeStorePoll, themeStoreBusyCeiling = budget, poll, ceil
	}()
	themeStoreBudget, themeStorePoll, themeStoreBusyCeiling =
		120*time.Millisecond, 30*time.Millisecond, 30*time.Second
	f.BusyPolls = 8 // ~240 ms of attested work at a 30 ms beat: twice the idle budget
	file := orbfake.Container("hold-dial")
	mux := http.NewServeMux()
	s.routes(mux)
	t0 := time.Now()
	rec := postThemeRaw(t, s, mux, "/api/theme/upload?ip="+ip, file)
	env := decodeEnv(t, rec)
	data, _ := env["data"].(map[string]any)
	if env["ok"] != true || data["store"] != "stored" {
		t.Fatalf("store verdict = %v, want stored for a copy that outlived the idle budget: %s", env["data"], rec.Body.String())
	}
	if d := time.Since(t0); d < themeStoreBudget {
		t.Errorf("the POST returned after %v, under the %v idle budget: the hold never happened", d, themeStoreBudget)
	}
}

// The ceiling is real: a board wedged on job_busy forever must still come back
// answered — lost, with the ceiling named — because a hub that never releases
// would hold the port forever. The verdict must arrive at the ceiling, not at
// the idle budget: shrinking only the budget must not end the wait.
func TestAStoreStuckBusyEndsAtTheCeilingNotTheIdleBudget(t *testing.T) {
	s, ip, _, f := fakeOrbHub(t)
	budget, poll, ceil := themeStoreBudget, themeStorePoll, themeStoreBusyCeiling
	defer func() {
		themeStoreBudget, themeStorePoll, themeStoreBusyCeiling = budget, poll, ceil
	}()
	themeStoreBudget, themeStorePoll, themeStoreBusyCeiling =
		60*time.Millisecond, 20*time.Millisecond, 300*time.Millisecond
	f.BusyForever = true
	file := orbfake.Container("wedge-dial")
	mux := http.NewServeMux()
	s.routes(mux)
	t0 := time.Now()
	rec := postThemeRaw(t, s, mux, "/api/theme/upload?ip="+ip, file)
	env := decodeEnv(t, rec)
	data, _ := env["data"].(map[string]any)
	if env["ok"] != true || data["store"] != "lost" {
		t.Fatalf("store verdict = %v, want lost at the busy ceiling: %s", env["data"], rec.Body.String())
	}
	note, _ := data["store_note"].(string)
	if !strings.Contains(note, "busy ceiling") {
		t.Errorf("the lost note says %q, want it to name the ceiling the wait ended at", note)
	}
	if d := time.Since(t0); d < themeStoreBusyCeiling {
		t.Errorf("released after %v, before the %v busy ceiling: the idle budget ended a busy wait again", d, themeStoreBusyCeiling)
	}
}
