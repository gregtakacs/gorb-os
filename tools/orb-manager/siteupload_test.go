package main

// H4 step 2's doors: POST /api/site/upload reads one library entry's design the way the
// reopen door reads it, and answers §6's table from the site's §2 answer; GET
// /api/site/local-designs is the picker over everything the hub could upload. The fake
// site's upload door (sitelink_test.go) records bearer, query and bytes per ask.

import (
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// linkedWriteHub is linkedSiteHub's H4 twin: the same filed link on both sides of the
// wire, with the scopes a consent page that said "read and add" would have earned, on a
// hub with a writable library to upload from. It returns the hub and the bearer.
func linkedWriteHub(t *testing.T, hs *hubSite, handle string) (*Server, string) {
	t.Helper()
	s := linkHub(t)
	root := t.TempDir()
	s.lib = &Library{AppsDir: filepath.Join(root, "apps"), ThemesDir: filepath.Join(root, "themes")}
	if err := os.MkdirAll(s.lib.ThemesDir, 0o755); err != nil {
		t.Fatal(err)
	}
	tok := hubRandom(32)
	hs.seedLink(s.installID, tok)
	link := &siteLink{Token: tok, Site: siteReleaseBase(), LinkedAt: 1730000000, Handle: handle,
		Scopes: []string{siteScopeRead, siteScopeWrite}}
	if err := s.saveSiteLink(link); err != nil {
		t.Fatalf("saveSiteLink: %v", err)
	}
	return s, tok
}

// uploadCall is the door with a typed body, answered in the hub's own envelope.
func uploadCall(t *testing.T, s *Server, body string) (int, map[string]any) {
	t.Helper()
	rec := rawCall(t, s, http.MethodPost, "/api/site/upload", []byte(body))
	var env map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("upload: %d %s", rec.Code, rec.Body)
	}
	return rec.Code, env
}

// putDraft files a design through the hub's own save door, so the draft under test is a
// file the hub wrote, with the saved envelope it rewrites.
func putDraft(t *testing.T, s *Server, app, name string, design []byte) {
	t.Helper()
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app="+app+"&name="+url.QueryEscape(name), design); env["ok"] != true {
		t.Fatalf("save %s/%s: %v", app, name, env)
	}
}

// putTheme files a bake on the shelf, design inside or not depending on the caller.
func putTheme(t *testing.T, s *Server, name string, bake []byte) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(s.lib.ThemesDir, strings.ToLower(name)+".orbtheme"), bake, 0o644); err != nil {
		t.Fatal(err)
	}
}

// flightsDoc is designDoc for another app: the save door checks the document's app, so a
// second app's library needs a document that speaks of it.
func flightsDoc(name string, art int) []byte {
	return []byte(strings.Replace(string(designDoc(name, "v1.2.16", art)), `"app":"clock"`, `"app":"flights"`, 1))
}

// TestSiteUploadSendsTheDocument is §6's read rule and §2's wire rule in one ask: the
// draft's exact file bytes, app and name through the query as the body spelled them, the
// bearer and the same four headers every bearer call carries, the site's 201 passed
// through as data with its url unmodified — and the hub's own file untouched, because an
// upload that edited the document would leave the two copies disagreeing about a name.
func TestSiteUploadSendsTheDocument(t *testing.T) {
	hs := hubSiteServer(t)
	s, tok := linkedWriteHub(t, hs, "greg")
	var sent http.Header
	hs.uploadHook = func(w http.ResponseWriter, r *http.Request) {
		sent = r.Header.Clone()
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusCreated)
		_, _ = io.WriteString(w, `{"id":"d-1","app":"clock","name":"Skeleton","version":1,"visibility":"private","url":"https://orb-os.net/t/d-1"}`)
	}
	design := designDoc("Skeleton", "v1.2.16", 4)
	putDraft(t, s, "clock", "Skeleton", design)
	onDisk, err := os.ReadFile(filepath.Join(s.lib.designsDir("clock"), designFile("Skeleton")))
	if err != nil {
		t.Fatal(err)
	}
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Skeleton"}`); env["ok"] != true {
		t.Fatalf("upload: %v", env)
	} else if d, _ := env["data"].(map[string]any); d == nil || d["url"] != "https://orb-os.net/t/d-1" || d["version"] != float64(1) {
		t.Errorf("data = %v, want the site's object passed through", env["data"])
	}
	if got := hs.uploadAsks(); got != 1 {
		t.Fatalf("the site was asked %d times, want once", got)
	}
	ask, _ := hs.uploadAsk(0)
	if string(ask.body) != string(onDisk) {
		t.Error("the door posted bytes that are not the draft file")
	}
	q, err := url.ParseQuery(ask.query)
	if err != nil || q.Get("app") != "clock" || q.Get("name") != "Skeleton" || q.Has("visibility") || q.Has("replaceId") {
		t.Errorf("query = %q, want app and name only", ask.query)
	}
	if ask.bearer != tok {
		t.Errorf("bearer = %q, want the link's own token", ask.bearer)
	}
	for h, want := range map[string]string{
		"Content-Type":  "application/json",
		"Accept":        "application/json",
		"User-Agent":    "orb-manager/" + version,
		"Cache-Control": "no-store",
	} {
		if got := sent.Get(h); got != want {
			t.Errorf("%s = %q, want %q", h, got, want)
		}
	}
	after, err := os.ReadFile(filepath.Join(s.lib.designsDir("clock"), designFile("Skeleton")))
	if err != nil || string(after) != string(onDisk) {
		t.Error("the upload rewrote the hub's own draft file")
	}

	// The optional parameters ride only when the body named them, and a case-folded
	// request sends the site the body's spelling while reading the file the hub wrote —
	// designFile hashes the exact name, so those two spellings are not interchangeable.
	putDraft(t, s, "clock", "Big Ben", designDoc("Big Ben", "v1.2.16", 3))
	bigBen, err := os.ReadFile(filepath.Join(s.lib.designsDir("clock"), designFile("Big Ben")))
	if err != nil {
		t.Fatal(err)
	}
	hs.uploadHook = nil
	if _, env := uploadCall(t, s, `{"app":"clock","name":"big ben","visibility":"unlisted","replaceId":"d-9"}`); env["ok"] != true {
		t.Fatalf("replace upload: %v", env)
	}
	ask, _ = hs.uploadAsk(1)
	q, _ = url.ParseQuery(ask.query)
	if q.Get("name") != "big ben" || q.Get("visibility") != "unlisted" || q.Get("replaceId") != "d-9" {
		t.Errorf("query = %q, want the body's own spelling and both optional members", ask.query)
	}
	if string(ask.body) != string(bigBen) {
		t.Error("a case-folded upload read a file other than the entry's own")
	}
}

// TestSiteUploadSendsAThemesDesign is ruling 43's other half: an entry with no draft
// uploads the design carried inside the bake, never the bake itself — a bake cannot be
// opened again, and an upload must be.
func TestSiteUploadSendsAThemesDesign(t *testing.T) {
	hs := hubSiteServer(t)
	s, _ := linkedWriteHub(t, hs, "greg")
	design := designDoc("Weather", "v1.2.16", 5)
	bake := withDesign(orbfake.Container("Weather"), string(design))
	putTheme(t, s, "Weather", bake)
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Weather"}`); env["ok"] != true {
		t.Fatalf("upload: %v", env)
	}
	if got := hs.uploadAsks(); got != 1 {
		t.Fatalf("asked %d times, want once", got)
	}
	ask, _ := hs.uploadAsk(0)
	if string(ask.body) != string(design) {
		t.Errorf("the door posted %d bytes, want the carried design's %d exactly", len(ask.body), len(design))
	}
	if len(ask.body) >= len(bake) {
		t.Error("the door posted more than the design alone could be")
	}
}

// TestSiteUploadAsNewUsesTheTypedName is the dialog's "Upload as a new design" door: the body's
// `name` names the entry the hub reads and the `siteName` the human typed names the design the
// site is asked to store, so uploading the library's `Aurora` as `Aurora 2` posts Aurora's bytes
// under `?name=Aurora 2` instead of refusing a design the typing never wrote. The two names are
// not interchangeable in either direction — a case-folded `name` still reads the file the hub
// wrote, and a `siteName` the grammar refuses is refused with that rule's own sentence before the
// hub looks up anything or dials anyone. A Replace is the same field carrying the clash answer's
// stored name, beside the id that makes it a version.
func TestSiteUploadAsNewUsesTheTypedName(t *testing.T) {
	hs := hubSiteServer(t)
	s, _ := linkedWriteHub(t, hs, "greg")
	putDraft(t, s, "clock", "Aurora", designDoc("Aurora", "v1.2.16", 6))
	onDisk, err := os.ReadFile(filepath.Join(s.lib.designsDir("clock"), designFile("Aurora")))
	if err != nil {
		t.Fatal(err)
	}

	// The typed name is what reaches the site, in the wire's own spelling of a space, and the
	// bytes are the entry the picker named.
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Aurora","siteName":"Aurora 2"}`); env["ok"] != true {
		t.Fatalf("upload as a new design: %v", env)
	}
	ask, ok := hs.uploadAsk(0)
	if !ok {
		t.Fatal("the typed name never reached the site")
	}
	if !strings.Contains(ask.query, "name=Aurora+2") {
		t.Errorf("query = %q, want the typed name as ?name=Aurora+2", ask.query)
	}
	q, err := url.ParseQuery(ask.query)
	if err != nil || q.Get("app") != "clock" || q.Get("name") != "Aurora 2" || q.Has("visibility") || q.Has("replaceId") {
		t.Errorf("query = %q, want app and the typed name only", ask.query)
	}
	if string(ask.body) != string(onDisk) {
		t.Error("a new-name upload posted bytes that are not the picked entry's draft")
	}

	// Folded lookup, exact file, typed name untouched: the two names never substitute.
	if _, env := uploadCall(t, s, `{"app":"clock","name":"aurora","siteName":"Aurora 3"}`); env["ok"] != true {
		t.Fatalf("case-folded new-name upload: %v", env)
	}
	ask, _ = hs.uploadAsk(1)
	q, _ = url.ParseQuery(ask.query)
	if q.Get("name") != "Aurora 3" || string(ask.body) != string(onDisk) {
		t.Errorf("query = %q over %d bytes, want Aurora 3 and the entry's own draft file", ask.query, len(ask.body))
	}

	// No second name at all: the entry's own name is the site's, unchanged from before the
	// field existed, which is the plain upload's whole body.
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Aurora"}`); env["ok"] != true {
		t.Fatalf("plain upload: %v", env)
	}
	ask, _ = hs.uploadAsk(2)
	q, _ = url.ParseQuery(ask.query)
	if q.Get("name") != "Aurora" {
		t.Errorf("query = %q, want the entry's own name when the body named no other", ask.query)
	}

	// The refusals, each before a socket: an explicit empty, a blank, a bound-busting name, and
	// a bad second name on a body whose first entry does not exist either — the grammar comes
	// first, so the sentence the human reads is the one about what they typed.
	before := hs.uploadAsks()
	long := strings.Repeat("n", 65)
	for _, tc := range []struct{ body, want string }{
		{`{"app":"clock","name":"Aurora","siteName":""}`, "a design needs a name"},
		{`{"app":"clock","name":"Aurora","siteName":"   "}`, "a design needs a name"},
		{`{"app":"clock","name":"Aurora","siteName":"` + long + `"}`, "a design's name is at most 64 characters"},
		{`{"app":"clock","name":"Ghost","siteName":"` + long + `"}`, "a design's name is at most 64 characters"},
	} {
		if _, env := uploadCall(t, s, tc.body); env["ok"] != false || env["error"] != tc.want {
			t.Errorf("upload %s = %v, want %q", tc.body, env, tc.want)
		}
	}
	if got := hs.uploadAsks(); got != before {
		t.Errorf("a refused second name asked the site %d times, want never", got-before)
	}

	// A Replace is the same field with the clash answer's stored spelling, and the id that turns
	// the post into a version of what the site already holds.
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Aurora","siteName":"Big Ben","replaceId":"d-7"}`); env["ok"] != true {
		t.Fatalf("replace upload: %v", env)
	}
	ask, _ = hs.uploadAsk(before)
	q, _ = url.ParseQuery(ask.query)
	if q.Get("name") != "Big Ben" || q.Get("replaceId") != "d-7" || q.Get("app") != "clock" {
		t.Errorf("query = %q, want the stored name and the clash's id", ask.query)
	}
	if string(ask.body) != string(onDisk) {
		t.Error("a Replace posted bytes other than the picked entry's design")
	}
}

// TestSiteUploadThemeWithNoDesign is §6's local refusal, the neighbour of the reopen
// door's sentence: an entry whose only half is a bake with nothing inside has nothing
// that could be reopened, so it has nothing to upload, and the site never hears of it.
func TestSiteUploadThemeWithNoDesign(t *testing.T) {
	hs := hubSiteServer(t)
	s, _ := linkedWriteHub(t, hs, "greg")
	putTheme(t, s, "Aurora", orbfake.Container("Aurora"))
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Aurora"}`); env["ok"] != false || env["error"] != siteUploadMsgNoDesign {
		t.Errorf("upload = %v, want the no-design sentence", env)
	}
	if got := hs.uploadAsks(); got != 0 {
		t.Errorf("the site was asked %d times, want never", got)
	}
}

// TestSiteUploadAnswerMatrix is §6's table, one row per answer the hub can bring back:
// the two the credential decides, the two the dialog needs shaped, and the one
// pass-through that means no hub-side list of site codes can ever fall out of date.
func TestSiteUploadAnswerMatrix(t *testing.T) {
	clashBody := `{"error":"name_taken","message":"orb-os.net already has a design called “Skeleton”.","existing":{"id":"d-7","name":"Big Ben","visibility":"private"}}`
	cases := []struct {
		name    string
		status  int
		body    string
		ct      string
		stall   bool
		wantErr string
		wantMsg string
		wantKey string // a member the answer must carry, or "!" one it must not
		wantVal string
		keepTok bool
		goneTok bool
	}{
		{
			// The 403 the hub words itself, in the ruling's words, and the token the
			// refusal is not about stays in the file.
			name: "scope", status: http.StatusForbidden, keepTok: true,
			body:    `{"error":"insufficient_scope","message":"this Orb Manager may only read designs: reconnect to allow uploads"}`,
			wantErr: "Reconnect to allow uploads",
		},
		{
			name: "clash", status: http.StatusConflict, keepTok: true, body: clashBody,
			wantErr: "name_taken", wantMsg: "orb-os.net already has a design called “Skeleton”.",
			wantKey: "nameTaken", wantVal: "Big Ben",
		},
		{
			// The race §3.2 names: a clash with no existing object is the same
			// answer without that member, which the dialog draws as the sentence alone.
			name: "clash-no-existing", status: http.StatusConflict, keepTok: true,
			body:    `{"error":"name_taken","message":"orb-os.net already has a design called “Skeleton”.","existing":null}`,
			wantErr: "name_taken", wantMsg: "orb-os.net already has a design called “Skeleton”.", wantKey: "!nameTaken",
		},
		{
			// H2's rule unchanged: the only answer that costs the credential.
			name: "401", status: http.StatusUnauthorized, goneTok: true,
			body: `{"error":"unauthenticated"}`, wantErr: siteMsgDisconnected,
		},
		{
			// Everything else is the site's sentence, verbatim, whatever the code.
			name: "throttle", status: http.StatusTooManyRequests, keepTok: true,
			body:    `{"error":"too_many_requests","message":"too many requests, try again shortly"}`,
			wantErr: "too many requests, try again shortly",
		},
		{
			name: "replace-mismatch", status: http.StatusConflict, keepTok: true,
			body:    `{"error":"replace_mismatch","message":"that design is named “Big Ben”, not “skeleton 2”"}`,
			wantErr: "that design is named “Big Ben”, not “skeleton 2”",
		},
		{
			// A refusal that forgot its sentence, a body that is not JSON at all and
			// a 201 that is neither: the site did not answer, and the hub says so.
			name: "silent-500", status: http.StatusInternalServerError, keepTok: true,
			body: `{"error":"storage_full"}`, wantErr: siteMsgNoAnswer,
		},
		{
			name: "not-json", status: http.StatusBadGateway, keepTok: true,
			body: "<html>the site is having a moment</html>", wantErr: siteMsgNoAnswer,
		},
		{
			name: "201-not-an-object", status: http.StatusCreated, keepTok: true,
			body: "[1,2]", wantErr: siteMsgNoAnswer,
		},
		{
			// An answer past the cap is not this door's answer at all, whatever it
			// would have said inside the part the hub read.
			name: "oversize", status: http.StatusConflict, keepTok: true,
			body:    `{"error":"name_taken","message":"` + strings.Repeat("x", siteUploadBodyMax) + `"}`,
			wantErr: siteMsgNoAnswer,
		},
		{
			name: "stall", stall: true, keepTok: true, wantErr: siteMsgNoAnswer,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			hs := hubSiteServer(t)
			s, tok := linkedWriteHub(t, hs, "greg")
			putDraft(t, s, "clock", "Skeleton", designDoc("Skeleton", "v1.2.16", 2))
			hs.uploadHook = func(w http.ResponseWriter, r *http.Request) {
				if tc.stall {
					time.Sleep(300 * time.Millisecond)
					return
				}
				w.Header().Set("Content-Type", tc.ct)
				w.WriteHeader(tc.status)
				_, _ = io.WriteString(w, tc.body)
			}
			if tc.stall {
				old := siteUploadTimeout
				siteUploadTimeout = 40 * time.Millisecond
				defer func() { siteUploadTimeout = old }()
			}
			_, env := uploadCall(t, s, `{"app":"clock","name":"Skeleton"}`)
			if env["ok"] != false || env["error"] != tc.wantErr {
				t.Fatalf("answer = %v, want error %q", env, tc.wantErr)
			}
			if tc.wantMsg != "" && env["message"] != tc.wantMsg {
				t.Errorf("message = %v, want the site's sentence %q", env["message"], tc.wantMsg)
			}
			switch tc.wantKey {
			case "":
			case "!nameTaken":
				if _, leaked := env["nameTaken"]; leaked {
					t.Errorf("nameTaken = %v, want absent", env["nameTaken"])
				}
			default:
				nt, _ := env[tc.wantKey].(map[string]any)
				if nt == nil || nt["name"] != tc.wantVal {
					t.Errorf("%s = %v, want the site's existing object naming %q", tc.wantKey, env[tc.wantKey], tc.wantVal)
				}
			}
			file := string(readSiteLinkFile(t, s))
			if tc.keepTok && !strings.Contains(file, tok) {
				t.Error("the refusal cost the hub a credential it should have kept")
			}
			if tc.goneTok && (strings.Contains(file, tok) || !strings.Contains(file, `"disconnected": true`)) {
				t.Errorf("the 401 left the file as %s, want the token gone and the flag set", file)
			}
		})
	}
}

// TestSiteUploadOnceOnly is §6's no-retry rule read at the door's counter: a failed
// upload asked once, and the owner pressing again is the human's second attempt, not the
// hub's. A retry would turn one confirmed Replace into two versions.
func TestSiteUploadOnceOnly(t *testing.T) {
	hs := hubSiteServer(t)
	s, _ := linkedWriteHub(t, hs, "greg")
	putDraft(t, s, "clock", "Skeleton", designDoc("Skeleton", "v1.2.16", 2))
	hs.uploadHook = func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusConflict)
		_, _ = io.WriteString(w, `{"error":"name_taken","message":"orb-os.net already has a design called “Skeleton”."}`)
	}
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Skeleton"}`); env["error"] != "name_taken" {
		t.Fatalf("first press: %v", env)
	}
	if got := hs.uploadAsks(); got != 1 {
		t.Fatalf("one upload asked %d times, want the one attempt", got)
	}
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Skeleton"}`); env["error"] != "name_taken" {
		t.Fatalf("second press: %v", env)
	}
	if got := hs.uploadAsks(); got != 2 {
		t.Fatalf("two uploads asked %d times, want one ask each", got)
	}
}

// TestSiteUploadScopeRefusalKeepsTheLink is §9's first worked example: a link written
// before H4 carries no scopes key, reads as read-only, and the door says the ruling's
// sentence without ever dialing — the refusal is about what the credential may do, not
// about its life, so the file must come out of this exactly as it went in.
func TestSiteUploadScopeRefusalKeepsTheLink(t *testing.T) {
	hs := hubSiteServer(t)
	s, tok := linkedSiteHub(t, hs, "greg")
	before := readSiteLinkFile(t, s)
	putDraft(t, s, "clock", "Skeleton", designDoc("Skeleton", "v1.2.16", 2))
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Skeleton"}`); env["ok"] != false || env["error"] != "Reconnect to allow uploads" {
		t.Fatalf("upload on a read-only link = %v", env)
	}
	if got := hs.uploadAsks(); got != 0 {
		t.Errorf("the site was asked %d times, want never", got)
	}
	if got := hs.designAsks(); got != 0 {
		t.Errorf("the door dialed %d times in all, want never", got)
	}
	if after := readSiteLinkFile(t, s); string(after) != string(before) || !strings.Contains(string(after), tok) {
		t.Error("the refusal changed the credential file")
	}
}

// TestSiteUploadTooLargeLocal is §6's pre-check with the site's own sentence: a design
// the door already knows the site will refuse is refused before a megabyte is dialed.
func TestSiteUploadTooLargeLocal(t *testing.T) {
	hs := hubSiteServer(t)
	s, _ := linkedWriteHub(t, hs, "greg")
	putDraft(t, s, "clock", "Big Ben", designDoc("Big Ben", "v1.2.16", 8_000_000))
	onDisk, err := os.ReadFile(filepath.Join(s.lib.designsDir("clock"), designFile("Big Ben")))
	if err != nil {
		t.Fatal(err)
	}
	if len(onDisk) <= siteUploadMaxBytes {
		t.Fatalf("the fixture is %d bytes, want over the 10 MB cap", len(onDisk))
	}
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Big Ben"}`); env["ok"] != false || env["error"] != siteUploadMsgTooLarge {
		t.Fatalf("upload = %v, want the site's 413 sentence at home", env)
	}
	if got := hs.uploadAsks(); got != 0 {
		t.Errorf("the site was asked %d times, want never", got)
	}
}

// TestSiteUploadBodyShape is everything the door refuses before it reads the library:
// the method, the link's two dead states, a body that is not the one object, the app's
// grammar, the name's rules, and a well-shaped request naming nothing — every refusal
// at 400 in jsonErr's shape, none of it dialed.
func TestSiteUploadBodyShape(t *testing.T) {
	hs := hubSiteServer(t)
	s, _ := linkedWriteHub(t, hs, "greg")
	long := strings.Repeat("n", 65)
	cases := []struct {
		body string
		want string
	}{
		{`{"name":"x"}`, "expected ?app=<slug>"},
		{`{"app":"Clock","name":"x"}`, "expected ?app=<slug>"},
		{`{"app":"../escape","name":"x"}`, "expected ?app=<slug>"},
		{`not json at all`, "expected ?app=<slug>"},
		{`[1,2]`, "expected ?app=<slug>"},
		{`{"app":"clock"}`, "a design needs a name"},
		{`{"app":"clock","name":"    "}`, "a design needs a name"},
		{`{"app":"clock","name":"` + long + `"}`, "a design's name is at most 64 characters"},
		{`{"app":"clock","name":"Ghost"}`, "no “Ghost” in the clock library on this hub"},
	}
	for _, tc := range cases {
		if _, env := uploadCall(t, s, tc.body); env["ok"] != false || env["error"] != tc.want {
			t.Errorf("upload %s = %v, want %q", tc.body, env, tc.want)
		}
	}
	if got := hs.uploadAsks(); got != 0 {
		t.Errorf("malformed uploads asked the site %d times, want never", got)
	}
	if rec := rawCall(t, s, http.MethodGet, "/api/site/upload", nil); rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("GET upload = %d, want 405", rec.Code)
	}

	// The two dead link states answer first, whatever the body claims: a hub told to
	// disconnect does not dial, and neither does a hub with nothing to dial with.
	if err := s.saveSiteLink(&siteLink{Site: siteReleaseBase(), Handle: "greg", LinkedAt: 1, Disconnected: true}); err != nil {
		t.Fatal(err)
	}
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Skeleton"}`); env["ok"] != false || env["error"] != siteMsgDisconnected {
		t.Errorf("disconnected upload = %v", env)
	}
	if err := s.clearSiteLink(); err != nil {
		t.Fatal(err)
	}
	if _, env := uploadCall(t, s, `{"app":"clock","name":"Skeleton"}`); env["ok"] != false || env["error"] != siteMsgNotConnected {
		t.Errorf("unlinked upload = %v", env)
	}
}

// localDesigns asks the picker and hands back its rows as maps, refusing an answer that
// is not the §6 shape — an object whose only member is a designs array.
func localDesigns(t *testing.T, s *Server) []map[string]any {
	t.Helper()
	env := designCall(t, s, http.MethodGet, "/api/site/local-designs", nil)
	if env["ok"] != true {
		t.Fatalf("local-designs: %v", env)
	}
	data, _ := env["data"].(map[string]any)
	if data == nil || len(data) != 1 {
		t.Fatalf("data = %v, want one member: the designs list", data)
	}
	rows, ok := data["designs"].([]any)
	if !ok {
		t.Fatalf("designs = %v, want an array", data["designs"])
	}
	out := make([]map[string]any, 0, len(rows))
	for _, r := range rows {
		m, ok := r.(map[string]any)
		if !ok {
			t.Fatalf("row = %v, want an object", r)
		}
		out = append(out, m)
	}
	return out
}

func rowKey(m map[string]any) string {
	return m["app"].(string) + "/" + m["name"].(string)
}

// TestSiteLocalDesignsListsAcrossApps is the picker's breadth: one call answering every
// app the library speaks of, rows in the §6 shape, newest first, and a changed entry
// sized by the half that would actually upload — the draft, unsent changes and all,
// never the bake sitting beside it.
func TestSiteLocalDesignsListsAcrossApps(t *testing.T) {
	s, _, _ := libHub(t)
	putDraft(t, s, "clock", "Sketch", designDoc("Sketch", "v1.2.16", 3))
	time.Sleep(3 * time.Millisecond)
	putDraft(t, s, "flights", "Boarding", flightsDoc("Boarding", 2))
	time.Sleep(3 * time.Millisecond)
	bake := withDesign(orbfake.Container("Tourbillon"), string(designDoc("Tourbillon", "v1.2.16", 30)))
	putTheme(t, s, "Tourbillon", bake)
	putDraft(t, s, "clock", "Tourbillon", designDoc("Tourbillon", "v1.2.16", 1))

	rows := localDesigns(t, s)
	if len(rows) != 3 {
		t.Fatalf("rows = %d, want the three names across two apps", len(rows))
	}
	if got := rowKey(rows[0]); got != "clock/Tourbillon" {
		t.Errorf("newest row = %s, want the last thing touched", got)
	}
	seen := map[string]map[string]any{}
	for _, m := range rows {
		seen[rowKey(m)] = m
	}
	sketch, err := os.ReadFile(filepath.Join(s.lib.designsDir("clock"), designFile("Sketch")))
	if err != nil {
		t.Fatal(err)
	}
	board, err := os.ReadFile(filepath.Join(s.lib.designsDir("flights"), designFile("Boarding")))
	if err != nil {
		t.Fatal(err)
	}
	tour, err := os.ReadFile(filepath.Join(s.lib.designsDir("clock"), designFile("Tourbillon")))
	if err != nil {
		t.Fatal(err)
	}
	for key, want := range map[string][]any{
		"clock/Sketch":     {float64(len(sketch)), "v1.2.16"},
		"flights/Boarding": {float64(len(board)), "v1.2.16"},
		"clock/Tourbillon": {float64(len(tour)), "v1.2.16"},
	} {
		m, ok := seen[key]
		if !ok {
			t.Errorf("no row for %s in %v", key, rows)
			continue
		}
		if m["bytes"] != want[0] || m["appVersion"] != want[1] {
			t.Errorf("%s = %v, want the draft's %v bytes and version %v", key, m, want[0], want[1])
		}
		if at, isNum := m["at"].(float64); !isNum || at <= 0 {
			t.Errorf("%s at = %v, want a save timestamp", key, m["at"])
		}
	}
	if seen["clock/Tourbillon"]["bytes"] == float64(len(bake)) {
		t.Error("a changed entry was sized by its bake")
	}

	// An empty library is an empty list, not an error; and the picker takes only GET.
	s2, _, _ := libHub(t)
	if rows := localDesigns(t, s2); len(rows) != 0 {
		t.Errorf("a fresh hub's picker = %v, want the empty list", rows)
	}
	if rec := rawCall(t, s, http.MethodPost, "/api/site/local-designs", nil); rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("POST local-designs = %d, want 405", rec.Code)
	}
}

// TestSiteLocalDesignsListsThemesToo is why drafts-only would lie: an entry whose draft
// was folded into a keep has its design inside the bake, and it is exactly as uploadable
// as any draft — with the carried design's size, not the bake's, because that is the
// byte count the door will weigh against the 10 MB. A bake carrying no design has nothing
// to reopen and gets no row, and an app with only a themes directory is still a library.
func TestSiteLocalDesignsListsThemesToo(t *testing.T) {
	s, _, _ := libHub(t)
	design := designDoc("Weather", "v1.2.16", 5)
	putTheme(t, s, "Weather", withDesign(orbfake.Container("Weather"), string(design)))
	putTheme(t, s, "Aurora", orbfake.Container("Aurora"))

	rows := localDesigns(t, s)
	if len(rows) != 1 {
		t.Fatalf("rows = %v, want only the theme that carries a design", rows)
	}
	m := rows[0]
	if rowKey(m) != "clock/Weather" {
		t.Errorf("row = %v, want the shelf theme under the app it themes", m)
	}
	if m["bytes"] != float64(len(design)) || m["appVersion"] != "v1.2.16" {
		t.Errorf("Weather = %v, want the carried design's %d bytes and its own version", m, len(design))
	}
}
