package main

// Ruling 38 (slice G5): 'unsafe-eval' on the editor port is per app.
//
// The unit tests below pin three things the live gate cannot: the exact bytes of the
// two CSP shapes (the strict one differs from the S2 policy by one token and nothing
// else), the shelf read that decides which shape one origin gets — including every
// way a package can decline to ask (no app, no editor, no manifest, a manifest whose
// runtime list is a string) and every near-miss that must NOT relax (babel@7.28.0,
// support@2) — and the Themes face's answer, which is the same judgement a line the
// operator reads before pressing into someone's editor.

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// editorCSPEval is the policy string this file's predecessor served every editor
// document: the byte-for-byte answer editorCSP(true) must keep giving an app whose
// package asks for Babel or the support composer, so the relaxation is an absence of
// change for exactly those apps. It is a literal and not a call: a test that derived
// its expectation from the function under test could not see the token vanish from
// both shapes at once.
const editorCSPEval = "default-src 'self'; script-src 'self' 'unsafe-inline' 'unsafe-eval' blob:; " +
	"style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; font-src 'self' data:; " +
	"media-src 'self' data: blob:; connect-src 'self' data: blob:; worker-src 'self' blob:; " +
	"frame-src 'self' blob: data:; object-src 'none'; base-uri 'self'; form-action 'self'; " +
	"frame-ancestors 'self' http://127.0.0.1:9999 http://localhost:9999"

// editorCSPStrict is the ruling-38 default: the same policy with exactly
// " 'unsafe-eval'" taken out of script-src — the same string read left to right with
// the token deleted. Literal beside literal, so the difference between the two is
// visible as text rather than argued as behaviour.
const editorCSPStrict = "default-src 'self'; script-src 'self' 'unsafe-inline' blob:; " +
	"style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; font-src 'self' data:; " +
	"media-src 'self' data: blob:; connect-src 'self' data: blob:; worker-src 'self' blob:; " +
	"frame-src 'self' blob: data:; object-src 'none'; base-uri 'self'; form-action 'self'; " +
	"frame-ancestors 'self' http://127.0.0.1:9999 http://localhost:9999"

// cspHub stands a hub whose shelf holds the named fixtures under the given slugs, so
// the per-app decision reads a real container the repo's own writer laid. The ports
// match the two pinned CSP strings (control 9999), and the region memo is dropped on
// cleanup: its key is a directory path, and a test that left its entries standing
// would hand the next one a shelf that is no longer there.
func cspHub(t *testing.T, shelf map[string]string) *Server {
	t.Helper()
	dir := t.TempDir()
	for slug, fixture := range shelf {
		body, err := os.ReadFile(filepath.Join("..", "..", "tests", "fixtures", fixture+".orbapp"))
		if err != nil {
			t.Fatalf("no fixture at tests/fixtures/%s.orbapp: %v", fixture, err)
		}
		if err := os.WriteFile(filepath.Join(dir, slug+".orbapp"), body, 0o644); err != nil {
			t.Fatalf("write %s: %v", slug, err)
		}
	}
	s := newHub()
	// The two pinned CSP literals name these ports, so the hub wears them too:
	// frame-ancestors carries the control port and every Host below carries the
	// editor one. A default-port change in main.go must not rewrite a security pin.
	s.controlPort, s.editorPort = 9999, 10000
	s.lib = &Library{AppsDir: dir}
	t.Cleanup(editorInvalidate)
	return s
}

func TestEditorCSPEvalIsS2Bytes(t *testing.T) {
	s := cspHub(t, nil)
	if got := s.editorCSP(true); got != editorCSPEval {
		t.Errorf("the relaxing policy changed shape:\n got %s\nwant %s", got, editorCSPEval)
	}
	if got := s.editorCSP(false); got != editorCSPStrict {
		t.Errorf("the strict policy is not the pinned S2-minus-eval:\n got %s\nwant %s", got, editorCSPStrict)
	}
	// The policy is one template: the token appears exactly once in the relaxed
	// answer and nowhere in the strict one, so the pair below really is the pair
	// the function switches between.
	if strings.Count(editorCSPEval, " 'unsafe-eval'") != 1 || strings.Contains(editorCSPStrict, "unsafe-eval") {
		t.Fatal("the pinned strings are not the pair the function is meant to switch between")
	}
}

func TestEditorCSPStrictDropsOnlyEval(t *testing.T) {
	s := cspHub(t, nil)
	strict, relaxed := s.editorCSP(false), s.editorCSP(true)
	if strict != strings.Replace(relaxed, " 'unsafe-eval'", "", 1) {
		t.Errorf("strict is not relaxed minus that one token:\n got %s\nwant %s", strict, relaxed)
	}
	if strings.Contains(strict, "unsafe-eval") {
		t.Error("the strict policy still names 'unsafe-eval': the token was not dropped, only rearranged")
	}
	names := func(policy string) map[string]bool {
		out := map[string]bool{}
		for _, dir := range strings.Split(policy, "; ") {
			name, _, _ := strings.Cut(dir, " ")
			out[name] = true
		}
		return out
	}
	relaxedDirs, strictDirs := names(relaxed), names(strict)
	if len(relaxedDirs) != len(strictDirs) {
		t.Fatalf("the two policies carry %d and %d directives: a directive appeared or vanished",
			len(relaxedDirs), len(strictDirs))
	}
	for name := range relaxedDirs {
		if !strictDirs[name] {
			t.Errorf("the strict policy lost a whole directive: %q", name)
		}
	}
	// The one allowance the strict answer still shares with the relaxed one that a
	// plain site would not: inline, which ruling 38 left alone.
	if !strings.Contains(strict, "'unsafe-inline'") {
		t.Error("the strict policy dropped an allowance ruling 38 did not name")
	}
}

func TestEditorAllowsEvalOnlyForTheTwoRuntimes(t *testing.T) {
	// Every fixture here is a real container, laid by the repo's own writer; what
	// differs is the one member that matters, editor.json's runtime list. The two
	// allowing apps ask in two different sentences — Babel among a full runtime set,
	// the support composer with its theme list — so the answer cannot be an accident
	// of position or of a lone-element array.
	s := cspHub(t, map[string]string{
		"evalbabel":   "evalbabel",   // runtime: react@18.3.1, babel@7.29.0
		"evalsupport": "evalsupport", // runtime: support@1, theme@1
		"evalplain":   "evalplain",   // runtime: react@18.3.1, theme@1
		"evalnear":    "evalnear",    // babel@7.28.0, babel@7.2.9, support@2, support@10, babel@7.29.0-beta.1
		"evalstr":     "evalstr",     // runtime is a string, not a list
		"evaljunk":    "evaljunk",    // editor.json is not valid JSON
		"evalnojson":  "evalnojson",  // an editor region carrying no editor.json
		"evalnoed":    "evalnoed",    // a container shipping no editor at all
		"sentinel":    "sentinel",    // the pre-D43 manifest: entry and title only
	})
	wants := map[string]bool{
		"evalbabel":   true,
		"evalsupport": true,
		"evalplain":   false,
		"evalnear":    false,
		"evalstr":     false,
		"evaljunk":    false,
		"evalnojson":  false,
		"evalnoed":    false,
		"sentinel":    false,
		"nosuchapp":   false, // the shelf carries no such app: false, and no panic
	}
	for slug, want := range wants {
		if got := s.editorAllowsEval(slug); got != want {
			t.Errorf("editorAllowsEval(%q) = %v, want %v", slug, got, want)
		}
	}
	if s.editorAllowsEval("") {
		t.Error("the empty slug relaxed anything: a Host that names no app gets the strict policy")
	}
	// The empty shelf answers false for the relaxing apps too, and does not panic:
	// a hub started without -library-apps reads its CSP exactly as it reads its apps.
	empty := newHub()
	empty.lib = &Library{AppsDir: t.TempDir()}
	t.Cleanup(editorInvalidate)
	if empty.editorAllowsEval("evalbabel") {
		t.Error("an app the hub does not carry relaxed anything")
	}
}

// evalListenerReq drives the real handler — guard, refusal shape, headers and all —
// with a Host naming one app, and asks only what this slice changed: the CSP header.
func evalListenerReq(t *testing.T, s *Server, host, path string) *httptest.ResponseRecorder {
	t.Helper()
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, path, nil)
	req.Host = host
	s.editorHandler().ServeHTTP(rec, req)
	return rec
}

func TestEditorListenerCSPPerApp(t *testing.T) {
	s := cspHub(t, map[string]string{
		"evalbabel":   "evalbabel",
		"evalsupport": "evalsupport",
		"evalplain":   "evalplain",
	})
	ep := strconv.Itoa(s.editorPort)
	appHost := func(slug string) string { return editorHostLabel(slug) + ".localhost:" + ep }
	// The document request a browser actually makes: its own token on the URL, the
	// one channel an iframe load has. Requests below that the guard refuses carry the
	// strict policy by this file's own ordering (the header goes on before the
	// guard, the relaxation only after it passes), so only a scoped request can
	// ever answer the relaxed one — which is part of what this table is pinning.

	for _, tc := range []struct {
		host, path, want, why string
	}{
		{host: appHost("evalbabel"), path: "/editor/hub/evalbabel/?et=" + s.editorToken("evalbabel"), want: editorCSPEval, why: "the app's package asks for babel@7.29.0"},
		{host: appHost("evalsupport"), path: "/editor/hub/evalsupport/?et=" + s.editorToken("evalsupport"), want: editorCSPEval, why: "the app's package asks for support@1"},
		{host: appHost("evalplain"), path: "/editor/hub/evalplain/?et=" + s.editorToken("evalplain"), want: editorCSPStrict, why: "the app asks for neither runtime"},
		{host: appHost("nosuchapp"), path: "/editor/hub/nosuchapp/?et=" + s.editorToken("nosuchapp"), want: editorCSPStrict, why: "the hub carries no such app"},
		{host: "127.0.0.1:" + ep, path: "/editor/hub/evalbabel/?et=" + s.editorToken("evalbabel"), want: editorCSPStrict, why: "a Host that is no app label at all"},
		{host: "clock.evil.example:" + ep, path: "/editor/hub/evalbabel/?et=" + s.editorToken("evalbabel"), want: editorCSPStrict, why: "a rebinded name"},
		{host: appHost("evalbabel"), path: "/editor/hub/evalbabel/?et=" + s.editorToken("evalbabel"), want: editorCSPEval, why: "again: the decision is stable across responses"},
	} {
		rec := evalListenerReq(t, s, tc.host, tc.path)
		if got := rec.Header().Get("Content-Security-Policy"); got != tc.want {
			t.Errorf("CSP for Host %s (%s):\n got %s\nwant %s", tc.host, tc.why, got, tc.want)
		}
	}

	// The policy is on the refusal too, and it is the strict one: a request whose
	// token belongs to another app has passed no gate, and a 403 whose headers
	// relaxed would be the one response shape an attacker picks for a probe. The
	// path below names evalbabel and the Origin names evalplain, so the guard
	// refuses it before the route ever runs.
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/api/library/entries?app=evalbabel", nil)
	req.Host = appHost("evalbabel")
	req.Header.Set("Origin", "http://evalplain.localhost:"+ep)
	req.Header.Set(editorTokenHeader, s.editorToken("evalbabel"))
	s.editorHandler().ServeHTTP(rec, req)
	if rec.Code == http.StatusOK || rec.Code == http.StatusFound {
		t.Fatalf("cross-origin API call under a scoped token: status %d, want a refusal", rec.Code)
	}
	if got := rec.Header().Get("Content-Security-Policy"); got != editorCSPStrict {
		t.Errorf("the cross-origin refusal carried the relaxing policy:\n got %s\nwant %s", got, editorCSPStrict)
	}

	// Replacing the app changes the answer on the next response with no restart: the
	// memo keys on the shelf file's size and mtime, and a swapped container is a
	// different file. Copy evalplain over evalbabel and the relaxed origin goes strict.
	babel := filepath.Join(s.lib.AppsDir, "evalbabel.orbapp")
	plain, err := os.ReadFile(filepath.Join(s.lib.AppsDir, "evalplain.orbapp"))
	if err != nil {
		t.Fatalf("read evalplain back: %v", err)
	}
	if err := os.WriteFile(babel, plain, 0o644); err != nil {
		t.Fatalf("replace evalbabel: %v", err)
	}
	rec = evalListenerReq(t, s, appHost("evalbabel"), "/editor/hub/evalbabel/?et="+s.editorToken("evalbabel"))
	if got := rec.Header().Get("Content-Security-Policy"); got != editorCSPStrict {
		t.Errorf("the replaced evalbabel.orbapp still served the relaxed policy:\n got %s\nwant %s", got, editorCSPStrict)
	}
}

func TestControlHeadersUnchanged(t *testing.T) {
	// Ruling 38 moved one CSP token and touched nothing else: the control listener's
	// pinned headers stay the release's, and the strings below are this commit's
	// literals rather than calls, for the same reason the CSP pair above is a pair of
	// literals — a test that reads the producer cannot watch it stop producing.
	s := cspHub(t, map[string]string{"evalbabel": "evalbabel"})
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/state", nil))
	if rec.Code == http.StatusOK {
		t.Fatal("/api/state answered unauthenticated; the header pin below would prove nothing")
	}
	// The editor listener's refusal headers, on the relaxed origin and on a Host
	// that names no app at all: guard-level headers do not depend on the answer.
	for _, host := range []string{"evalbabel.localhost:" + strconv.Itoa(s.editorPort), "127.0.0.1:1"} {
		rec := evalListenerReq(t, s, host, "/api/state")
		for h, want := range map[string]string{
			"Referrer-Policy":        "no-referrer",
			"X-Content-Type-Options": "nosniff",
		} {
			if got := rec.Header().Get(h); got != want {
				t.Errorf("Host %s: %s = %q, want the unchanged %q", host, h, got, want)
			}
		}
	}
}

func TestThemesRowsSayEditorEval(t *testing.T) {
	// The Themes face's rows carry ruling 38's answer: the operator presses into an
	// editor from THIS list, and the tab line below is only worth drawing if the row
	// it came from says the same thing the CSP will be. evalbabel's package asks for
	// babel@7.29.0 and evalplain's asks for nothing of the kind; both ship editors,
	// so has_editor alone cannot tell the two rows apart.
	dir := t.TempDir()
	regions := map[string][]byte{}
	for _, slug := range []string{"evalbabel", "evalplain"} {
		body, err := os.ReadFile(filepath.Join("..", "..", "tests", "fixtures", slug+".orbapp"))
		if err != nil {
			t.Skipf("no fixture at tests/fixtures/%s.orbapp: %v", slug, err)
		}
		if err := os.WriteFile(filepath.Join(dir, slug+".orbapp"), body, 0o644); err != nil {
			t.Fatalf("write %s: %v", slug, err)
		}
		region, err := orbnet.AppEditorRegion(body)
		if err != nil || len(region) == 0 {
			t.Skipf("the %s fixture carries no editor region: %v", slug, err)
		}
		regions[slug] = region
	}
	orb := orbfake.New(
		orbfake.Theme{Name: "aurora", App: "evalbabel", Bytes: 99, Serial: 1, OK: true, Active: true},
	).WithCardApps(
		orbfake.CardApp{Stem: "evalbabel", Slug: "evalbabel", Title: "Evalbabel", Caps: capThemes,
			Installed: true, Editor: regions["evalbabel"]},
		orbfake.CardApp{Stem: "evalplain", Slug: "evalplain", Title: "Evalplain", Caps: capThemes,
			Installed: true, Editor: regions["evalplain"]},
	)
	s, ip, _, _ := fakeOrbHubWith(t, orb)
	s.lib = &Library{AppsDir: dir}
	t.Cleanup(editorInvalidate)

	for round := 0; round < 2; round++ {
		rec := httptest.NewRecorder()
		req := httptest.NewRequest(http.MethodGet, "/api/theme/apps?ip="+ip, nil)
		req.Header.Set(tokenHeader, s.token)
		mux := http.NewServeMux()
		s.routes(mux)
		mux.ServeHTTP(rec, req)
		if rec.Code != http.StatusOK {
			t.Fatalf("round %d: status %d body %s", round, rec.Code, rec.Body)
		}
		var env struct {
			OK   bool `json:"ok"`
			Data struct {
				Apps []map[string]any `json:"apps"`
			} `json:"data"`
		}
		if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil || !env.OK {
			t.Fatalf("round %d: body is not an ok envelope: %v (%s)", round, err, rec.Body)
		}
		want := map[string]bool{"evalbabel": true, "evalplain": false}
		seen := 0
		for _, row := range env.Data.Apps {
			slug, _ := row["slug"].(string)
			exp, named := want[slug]
			if !named {
				continue
			}
			seen++
			ev, present := row["editor_eval"]
			if !present {
				t.Fatalf("round %d: the %s row answers no editor_eval at all: %v", round, slug, row)
			}
			if ev != exp {
				t.Errorf("round %d: %s row editor_eval = %v, want %v", round, slug, ev, exp)
			}
			if has, _ := row["has_editor"].(bool); !has {
				t.Errorf("round %d: %s carries an editor region on the dial and the row says has_editor false", round, slug)
			}
		}
		if seen != len(want) {
			t.Fatalf("round %d: rows came back for %d of the two apps the card lists: %v", round, seen, env.Data.Apps)
		}
	}
	// The stamp rode the response and not the memo: the cached bytes the next read
	// replays are still ThemeAppsFrom's own, because another face reading the same
	// enumeration must not inherit a ruling-38 answer this route put on it.
	raw, _, err := s.cardAppsRead(ip)
	if err != nil {
		t.Fatalf("the cached read is gone: %v", err)
	}
	if strings.Contains(string(raw), `"editor_eval":true`) {
		t.Error("the memoised /api/theme/apps bytes carry the eval stamp: the cache was written, not copied")
	}
}
