package orbfake

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// These rows pin the double against the firmware's published shapes rather than against
// anything in the hub's client: a route the hub may dial must answer the way orb_net.c
// answers, refusals included. Each expectation quotes the handler named in apps.go.

func appDo(t *testing.T, srv *httptest.Server, method, path string, body []byte) (int, string) {
	t.Helper()
	var rd io.Reader
	if body != nil {
		rd = strings.NewReader(string(body))
	}
	req, err := http.NewRequest(method, srv.URL+path, rd)
	if err != nil {
		t.Fatal(err)
	}
	if body != nil {
		req.ContentLength = int64(len(body))
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	return resp.StatusCode, string(b)
}

func TestAppsListingTiersAndCount(t *testing.T) {
	o := New().WithCardApps(
		CardApp{Stem: "hue", Slug: "hue", Title: "Hue", Caps: 0x2},
		CardApp{Stem: "broken", Slug: "broken", Title: "Broken", Why: "geom crc mismatch"},
	)
	srv := httptest.NewServer(o)
	defer srv.Close()

	code, body := appDo(t, srv, http.MethodGet, "/api/v1/apps", nil)
	if code != 200 {
		t.Fatalf("GET apps -> %d %s", code, body)
	}
	var got struct {
		Mounted   bool             `json:"mounted"`
		Count     int              `json:"count"`
		Apps      []map[string]any `json:"apps"`
		Installed []map[string]any `json:"installed"`
	}
	if err := json.Unmarshal([]byte(body), &got); err != nil {
		t.Fatalf("apps body is not JSON: %s", body)
	}
	if !got.Mounted || got.Count != 2 {
		t.Errorf("mounted=%v count=%d, want true/2 for two staged containers", got.Mounted, got.Count)
	}
	if len(got.Installed) != 0 {
		t.Errorf("a staged app must not appear installed before a POST: %v", got.Installed)
	}
	for _, a := range got.Apps {
		if a["stem"] == "broken" && a["ok"] == true {
			t.Error("a row with a why must report ok:false")
		}
	}

	// h_apps's unmounted answer is count -1, not count 0: an absent card and an empty
	// apps directory are different problems for whoever is reading this.
	o.Mounted = false
	_, body = appDo(t, srv, http.MethodGet, "/api/v1/apps", nil)
	if !strings.Contains(body, `"mounted":false,"count":-1`) {
		t.Errorf("unmounted listing = %s, want count -1", body)
	}
}

// id25 is one character past the handler's 24-byte id buffer (ORB_APP_SLUG_MAX + 1).
const id25 = "aaaaaaaaaaaaaaaaaaaaaaaaa"

func TestAppVerbRefusals(t *testing.T) {
	o := New().WithCardApps(

		CardApp{Stem: "hue", Slug: "hue", Installed: true, Enabled: true},
		CardApp{Stem: "glass", Slug: "glass", Installed: true, Enabled: true, OnGlass: true},
		CardApp{Stem: "bad", Slug: "bad", Installed: true, Failed: true},
	)
	srv := httptest.NewServer(o)
	defer srv.Close()

	// Every row pins status AND body wording, because checking only the status is what let
	// this file pass while the device said something else. Each measured body comes off
	// orb-B (fw 9442432); the firmware's own page at orb_net.c:4477 shows r.d.error straight
	// to the operator, so the wording is user-visible and drift is a UI bug, not a cosmetic one.
	cases := []struct {
		path, wantBody string
		want           int
	}{
		{"/api/v1/apps/install", noStemBody, 400},
		{"/api/v1/apps/install?id=..%2Fevil", noStemBody, 400},
		{"/api/v1/apps/install?name=hue", noStemBody, 400},
		{"/api/v1/apps/enable?id=hue", badOnBody, 400},
		{"/api/v1/apps/enable?id=hue&on=2", badOnBody, 400},
		// q_param drops a decoded separator before the door ever sees a name.
		{"/api/v1/apps/enable?id=..%2Fevil&on=1", missingIDBody, 400},
		{"/api/v1/apps/uninstall?id=..%2Fevil", missingIDBody, 400},
		// A leading dot passes q_param and is caught by orb_sd_name_ok inside door_open.
		// Two different 400s from two layers, and the device really distinguishes them.
		{"/api/v1/apps/enable?id=..foo&on=1", notUsableNameBody, 400},
		{"/api/v1/apps/uninstall?id=..foo", notUsableNameBody, 400},
		{"/api/v1/apps/enable?id=nope&on=1", notFoundBody, 404},
		{"/api/v1/apps/uninstall?id=nope", notFoundBody, 404},
		// A 25-character id overflows the handler's 24-byte buffer and is truncated rather
		// than refused, so it lands on the lookup and misses.
		{"/api/v1/apps/enable?id=" + id25 + "&on=1", notFoundBody, 404},
		// Inside-the-firmware rows are refused on both doors with the same sentence, at 400
		// because NOT_SUPPORTED maps there. A Manage card that rendered a toggle for these
		// would ship a control that can only ever fail.
		{"/api/v1/apps/enable?id=clock&on=0", builtinBody, 400},
		{"/api/v1/apps/enable?id=menu&on=0", builtinBody, 400},
		{"/api/v1/apps/uninstall?id=color_picker", builtinBody, 400},
		{"/api/v1/apps/uninstall?id=glass", onDialBody, 409},
	}
	for _, c := range cases {
		code, body := appDo(t, srv, http.MethodPost, c.path, []byte("{}"))
		if code != c.want || !strings.Contains(body, c.wantBody) {
			t.Errorf("POST %s -> %d %s, want %d containing %s", c.path, code, body, c.want, c.wantBody)
		}
	}
	// The refusal body carries no caller text. Enabling a name that does not exist returns
	// the firmware's own sentence, not the name the caller supplied.
	_, body := appDo(t, srv, http.MethodPost, "/api/v1/apps/enable?id=zzz&on=1", []byte("{}"))
	if strings.Contains(body, "zzz") {
		t.Errorf("refusal body echoed caller input: %s", body)
	}
	// A card job in flight outranks the lookup: door_open checks busy before it searches,
	// so an unknown id on a busy orb reports the job. Reordering this in the double would
	// show an operator a reason the orb never had.
	o.JobBusy = true
	if code, body := appDo(t, srv, http.MethodPost, "/api/v1/apps/enable?id=nope&on=1", []byte("{}")); code != 409 ||
		!strings.Contains(body, doorBusyBody) {
		t.Errorf("busy orb, unknown id -> %d %s, want 409 job-busy", code, body)
	}
	o.JobBusy = false
	// Turning ON a failed app is the revive gesture (C2), not a dead end: the device clears
	// the fault record and lets it through. The old expectation here (flat 409) was wrong in
	// a way that would have cost the hub the only control that recovers a failed card.
	if code, body := appDo(t, srv, http.MethodPost, "/api/v1/apps/enable?id=bad&on=1", []byte("{}")); code != 200 ||
		!strings.Contains(body, `"enabled"`) {
		t.Errorf("revive via enable -> %d %s, want 200 enabled", code, body)
	}
	if o.apps["bad"].Failed {
		t.Error("enable on a failed app must clear the fault record")
	}
	// The one answer that looks wrong and is not: a well-formed stem naming nothing is
	// accepted, because h_apps_install queues before it looks. The failure surfaces on
	// the next listing rather than as a refusal here.
	code, body := appDo(t, srv, http.MethodPost, "/api/v1/apps/install?id=notthere", []byte("{}"))
	if code != 200 || !strings.Contains(body, `"installing"`) {
		t.Errorf("install of an unknown but valid stem -> %d %s, want 200 installing", code, body)
	}
	// And a real install flips the tiers.
	if code, _ = appDo(t, srv, http.MethodPost, "/api/v1/apps/install?id=hue", []byte("{}")); code != 200 {
		t.Fatalf("install hue -> %d", code)
	}
	_, body = appDo(t, srv, http.MethodGet, "/api/v1/apps", nil)
	if !strings.Contains(body, `"installed":[{`) {
		t.Errorf("installed tier stayed empty after a real install: %s", body)
	}
	if o.apps["hue"].Installed != true {
		t.Error("the row did not flip installed")
	}
}

func TestAppUploadTakesItsNameFromTheHeader(t *testing.T) {
	o := New()
	srv := httptest.NewServer(o)
	defer srv.Close()

	req, _ := http.NewRequest(http.MethodPost, srv.URL+"/api/v1/sd/app", strings.NewReader("x"))
	req.ContentLength = -1 // unknown length: the transport sends it chunked
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 411 {
		t.Errorf("upload with no length -> %s, want 411 like h_app_upload", resp.Status)
	}

	// A theme container is named rather than miscalled; the two magics differ on purpose.
	theme := make([]byte, appHdrWords*4)
	copy(theme, []byte{0x54, 0x52, 0x4F, 0x47}) // little-endian GORT
	if code, body := appDo(t, srv, http.MethodPost, "/api/v1/sd/app", theme); code != 422 ||
		!strings.Contains(body, "theme container") {
		t.Errorf("theme container -> %d %s, want 422 naming it", code, body)
	}

	// The destination is the header's slug. The query string says something else entirely
	// and must be ignored — that property is what makes the route safe to expose at all.
	cont := BuildAppContainer("dialtest", "Dial Test", 0x1, 60000, 7)
	if code, body := appDo(t, srv, http.MethodPost, "/api/v1/sd/app?id=../../etc", cont); code != 200 ||
		!strings.Contains(body, `"stored"`) {
		t.Fatalf("valid upload -> %d %s", code, body)
	}
	if o.apps["dialtest"] == nil {
		t.Fatal("container did not land under its own header slug")
	}
	if o.apps["../../etc"] != nil {
		t.Error("a caller-controlled query string influenced the destination")
	}
	// Uploading over an inside-the-firmware name is refused, not silently accepted: the
	// overwrite gate keys on the registry, and clock/color_picker/menu are already in it.
	// Only the status is pinned here — the exact wording for a built-in collision has not
	// been measured on hardware, and this file does not invent device strings again.
	if code, body := appDo(t, srv, http.MethodPost, "/api/v1/sd/app",
		BuildAppContainer("clock", "Wall Clock", 0x1, 60000, 7)); code == 200 {
		t.Errorf("upload over a built-in slug -> %d %s, want a refusal", code, body)
	}
	// h_status's apps array is the broker ledger and lists only what is registered in
	// flash, so an upload alone must leave it at the built-ins: the row is staged on the
	// card, not installed. The ledger row for it appears after the install that follows.
	_, sb := appDo(t, srv, http.MethodGet, "/api/v1/status", nil)
	if strings.Contains(sb, `"slug":"dialtest"`) {
		t.Errorf("a staged-only app appeared in the ledger: %s", sb)
	}
	if !strings.Contains(sb, `"apps":[{"slug":"clock"`) {
		t.Errorf("ledger lost the built-in rows a real orb always reports: %s", sb)
	}
	if code, body := appDo(t, srv, http.MethodPost, "/api/v1/apps/install?id=dialtest", []byte("{}")); code != 200 {
		t.Fatalf("install dialtest -> %d %s", code, body)
	}
	_, sb = appDo(t, srv, http.MethodGet, "/api/v1/status", nil)
	if !strings.Contains(sb, `"slug":"dialtest"`) {
		t.Errorf("status ledger omitted the installed app: %s", sb)
	}
}

func TestReprovRefusesBeforeArming(t *testing.T) {
	o := New()
	srv := httptest.NewServer(o)
	defer srv.Close()

	if code, body := appDo(t, srv, http.MethodPost, "/api/v1/reprov", []byte("{}")); code != 200 ||
		!strings.Contains(body, `"into":"provisioning"`) {
		t.Fatalf("first reprov -> %d %s", code, body)
	}
	if got := o.Reprovisioned(); got != 1 {
		t.Errorf("Reprovisioned() = %d, want 1", got)
	}
	st, _, _, _, _, staUp, ap, _, rebooted, _ := o.Device()
	if st != "provisioning" || !ap || staUp {
		t.Errorf("after reprov: state=%s ap=%v sta=%v, want provisioning/true/false", st, ap, staUp)
	}
	if rebooted != 0 {
		t.Errorf("reprov must not count as a plain reboot: Rebooted=%d", rebooted)
	}
	// Already there: a second request re-arms nothing, and re-arming would reset D4's
	// clock and drop the page the operator is standing on.
	if code, _ := appDo(t, srv, http.MethodPost, "/api/v1/reprov", []byte("{}")); code != 409 {
		t.Errorf("second reprov -> %d, want 409", code)
	}
	if got := o.Reprovisioned(); got != 1 {
		t.Errorf("a refused reprov still counted: %d", got)
	}

	// The AP-fallback=n build refuses rather than arming a state with no way in or out.
	off := New()
	off.NoAPFallback = true
	srv2 := httptest.NewServer(off)
	defer srv2.Close()
	if code, body := appDo(t, srv2, http.MethodPost, "/api/v1/reprov", []byte("{}")); code != 409 ||
		!strings.Contains(body, "no AP fallback") {
		t.Errorf("reprov on an AP-fallback=n build -> %d %s, want 409 naming the reason", code, body)
	}
	if off.Reprovisioned() != 0 {
		t.Error("refused reprov must not count")
	}
}
