package main

// orb-os-net §7.1 H1: the hub's half of "Import from orb-os.net" — the link, the fetch,
// the checks, the ticket, the keep with its origin record — run through the same
// s.routes(mux) the binary serves, against a site that is an httptest handler.

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// siteDesignID is a design's 22 characters, the example link's own id.
const siteDesignID = "AbCdEfGhIjKlMnOpQrStUv"

// siteAsk is one request the hub made to a site: what it asked for, and how it asked.
type siteAsk struct {
	path string
	head http.Header
}

// siteServer stands a design site: h answers every request, and every request is remembered.
func siteServer(t *testing.T, h func(w http.ResponseWriter, r *http.Request)) (*httptest.Server, *[]siteAsk) {
	t.Helper()
	var asks []siteAsk
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		asks = append(asks, siteAsk{path: r.URL.Path, head: r.Header.Clone()})
		h(w, r)
	}))
	t.Cleanup(srv.Close)
	return srv, &asks
}

// siteTestLink is the link the owner pastes: the test site's address plus the design's path.
func siteTestLink(srv *httptest.Server, id string) string {
	return srv.URL + "/t/" + id
}

func bundleOf(t *testing.T, doc map[string]any) []byte {
	t.Helper()
	b, err := json.Marshal(doc)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// goodBundle is §8.4's shape as a site that works serves it: the design the link named,
// for app, under sharedName, carrying each art body under its own hash.
func goodBundle(t *testing.T, id string, version int, app, sharedName string, art ...[]byte) []byte {
	t.Helper()
	assets := map[string]string{}
	var orbAssets []any
	for _, body := range art {
		sum := sha256.Sum256(body)
		sha := hex.EncodeToString(sum[:])
		assets[sha] = base64.StdEncoding.EncodeToString(body)
		orbAssets = append(orbAssets, map[string]any{"sha256": sha, "prefix": "data:image/png;base64"})
	}
	design := map[string]any{"format": "wc-design", "schema": 1, "app": app, "themeName": sharedName}
	if orbAssets != nil {
		design["orbAssets"] = orbAssets
	}
	return bundleOf(t, map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": id, "version": version, "app": app,
		"sharedName": sharedName, "name": sharedName, "handle": "greg",
		"design": design, "assets": assets,
	})
}

// pngArt is a picture as the site's rules read one: a real PNG header naming w × h,
// then filler bytes seeded so two pictures of one size are still two pictures. The hub
// reads only the header, so no CRC here is the CRC a decoder would check.
func pngArt(w, h int, seed byte) []byte {
	b := []byte{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
		0, 0, 0, 13, 'I', 'H', 'D', 'R',
		byte(w >> 24), byte(w >> 16), byte(w >> 8), byte(w),
		byte(h >> 24), byte(h >> 16), byte(h >> 8), byte(h),
		8, 6, 0, 0, 0}
	crc := []byte{seed, seed ^ 0x5a, seed ^ 0xa5, seed ^ 0xff}
	b = append(b, crc...)
	for i := 0; i < 8; i++ {
		b = append(b, seed+byte(i))
	}
	return b
}

// siteImport posts a paste and answers the hub's envelope.
func siteImport(t *testing.T, s *Server, link string) map[string]any {
	t.Helper()
	body, err := json.Marshal(map[string]string{"link": link})
	if err != nil {
		t.Fatal(err)
	}
	return designCall(t, s, http.MethodPost, "/api/site/import", body)
}

func importErr(t *testing.T, s *Server, link string) string {
	t.Helper()
	env := siteImport(t, s, link)
	if env["ok"] == true {
		t.Fatalf("import of %q was accepted: %v", link, env)
	}
	msg, _ := env["error"].(string)
	return msg
}

func ticketOf(t *testing.T, env map[string]any) string {
	t.Helper()
	if env["ok"] != true {
		t.Fatalf("import refused: %v", env)
	}
	tk, _ := env["data"].(map[string]any)["ticket"].(string)
	if tk == "" {
		t.Fatalf("no ticket in %v", env)
	}
	return tk
}

// bundleHeld is the bundle door's own question: is this import still on the hub?
func bundleHeld(t *testing.T, s *Server, tk string) bool {
	t.Helper()
	return rawCall(t, s, http.MethodGet, "/api/site/bundle?ticket="+tk, nil).Code == 200
}

// untokenedCall rides the same mux with no launch token: what a page that never loaded
// from this hub sends.
func untokenedCall(t *testing.T, s *Server, method, path string, body []byte) *httptest.ResponseRecorder {
	t.Helper()
	mux := http.NewServeMux()
	s.routes(mux)
	req := httptest.NewRequest(method, path, bytes.NewReader(body))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	return rec
}

// The link is the one thing this flow cannot guess at, so the reading of it is picky and
// says one sentence when the text is not a design's link.
func TestSiteLinkNamesOneDesignOnOneSite(t *testing.T) {
	ok := []struct{ paste, site, id string }{
		{"https://orb-os.net/t/" + siteDesignID, "https://orb-os.net", siteDesignID},
		{"  https://orb-os.net/t/" + siteDesignID + "/  ", "https://orb-os.net", siteDesignID},
		{"https://orb-os.net/t/" + siteDesignID + "?utm=share#copy", "https://orb-os.net", siteDesignID},
		{"orb.takacsfamily.com/t/" + siteDesignID, "https://orb.takacsfamily.com", siteDesignID},
		{"HTTPS://ORB-OS.NET/t/" + siteDesignID, "https://orb-os.net", siteDesignID},
		{"http://127.0.0.1:8080/t/" + siteDesignID, "http://127.0.0.1:8080", siteDesignID},
		{"http://localhost/t/" + siteDesignID, "http://localhost", siteDesignID},
		{"http://[::1]:8080/t/" + siteDesignID, "http://[::1]:8080", siteDesignID},
	}
	for _, c := range ok {
		site, id, err := parseSiteLink(c.paste)
		if err != nil {
			t.Errorf("parseSiteLink(%q): %v", c.paste, err)
			continue
		}
		if site != c.site || id != c.id {
			t.Errorf("parseSiteLink(%q) = %q %q, want %q %q", c.paste, site, id, c.site, c.id)
		}
	}
	bad := []string{
		"", "   ", "hello", "orb-os.net", "https://orb-os.net/", "https://orb-os.net/t/",
		"https://orb-os.net/t/AbCdEfGhIjKlMnOpQrStu",  // 21 characters
		"https://orb-os.net/t/" + siteDesignID + "/x", // a path that goes on
		"https://greg@orb-os.net/t/" + siteDesignID,   // userinfo
		"ftp://orb-os.net/t/" + siteDesignID,          // not a web scheme
		"https://orb-os.net/" + siteDesignID,          // no /t/ to say what the id is
	}
	for _, p := range bad {
		if _, _, err := parseSiteLink(p); err == nil || err.Error() != siteLinkRefused {
			t.Errorf("parseSiteLink(%q) = %v, want the one link sentence", p, err)
		}
	}
	// A remote http:// link is not a malformed paste: the owner has a link, and the one
	// thing they can do about it is get the https one.
	if _, _, err := parseSiteLink("http://orb-os.net/t/" + siteDesignID); err == nil ||
		err.Error() != "the link must start with https://" {
		t.Errorf("a remote http link = %v, want the https sentence", err)
	}
}

// siteHostOf is the host, port and all, the refusal sentences name.
func siteHostOf(srv *httptest.Server) string { return strings.TrimPrefix(srv.URL, "http://") }

// What the site answered is what the owner hears, in the one sentence that fits it. The
// hub asks once and never follows a pointer: a site that answers 302 is offering a sign-in
// page, and dialing where it points would hand our request to a place nobody named.
func TestSiteImportDoorAnswersWhatTheSiteSaid(t *testing.T) {
	s, _, _ := libHub(t)
	cases := []struct {
		what string
		h    func(w http.ResponseWriter, r *http.Request)
		want func(host string) string // nil is the case that succeeds
	}{
		{"the design as served", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write(goodBundle(t, siteDesignID, 3, "clock", "Skeleton by @greg", pngArt(9, 9, 40)))
		}, nil},
		{"a sign-in redirect", func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Path == "/signin" {
				_, _ = w.Write([]byte("<html>sign in</html>"))
				return
			}
			http.Redirect(w, r, "/signin", http.StatusFound)
		}, func(host string) string {
			return host + " sent a sign-in page instead of the theme: the site isn’t open to Orb Manager yet"
		}},
		{"a private design", func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "not yours", http.StatusNotFound)
		}, func(host string) string { return "that theme is private, or " + host + " has no such theme" }},
		{"a deleted design", func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "gone", http.StatusGone)
		}, func(string) string { return "that theme was deleted by its owner" }},
		{"a site that will not share", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusConflict)
			_, _ = w.Write([]byte(`{"message":"the owner paused sharing"}`))
		}, func(host string) string {
			return host + " can’t share that theme right now: the owner paused sharing"
		}},
		{"a 409 naming its reason in the other field", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusConflict)
			_, _ = w.Write([]byte(`{"error":"quota"}`))
		}, func(host string) string { return host + " can’t share that theme right now: quota" }},
		{"a 409 that says nothing", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusConflict)
		}, func(host string) string { return host + " can’t share that theme right now: HTTP 409" }},
		{"a site that broke", func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "boom", http.StatusInternalServerError)
		}, func(host string) string { return host + " answered HTTP 500" }},
	}
	for _, c := range cases {
		srv, asks := siteServer(t, c.h)
		host := siteHostOf(srv)
		held := len(s.tickets().live)
		if c.want == nil {
			env := siteImport(t, s, siteTestLink(srv, siteDesignID))
			if env["ok"] != true {
				t.Errorf("%s: import refused: %v", c.what, env)
				continue
			}
			d, _ := env["data"].(map[string]any)
			if d["version"] != float64(3) || d["app"] != "clock" || d["sharedName"] != "Skeleton by @greg" ||
				d["handle"] != "greg" || d["site"] != strings.ToLower(srv.URL) {
				t.Errorf("%s: data = %v", c.what, d)
			}
			if len(*asks) != 1 || (*asks)[0].path != "/api/hub/theme/"+siteDesignID {
				t.Errorf("%s: the site saw %v, want one bundle ask", c.what, *asks)
			} else if got := (*asks)[0].head; got.Get("Accept") != "application/json" ||
				!strings.HasPrefix(got.Get("User-Agent"), "orb-manager/") {
				t.Errorf("%s: the site saw headers %v, wanting the hub's Accept and User-Agent", c.what, got)
			}
			continue
		}
		if got := importErr(t, s, siteTestLink(srv, siteDesignID)); got != c.want(host) {
			t.Errorf("%s: error = %q, want %q", c.what, got, c.want(host))
		}
		if n := len(s.tickets().live); n != held {
			t.Errorf("%s: a refused import is held anyway: %d tickets live", c.what, n)
		}
		for _, a := range *asks {
			if a.path != "/api/hub/theme/"+siteDesignID {
				t.Errorf("%s: the hub dialed %s, wanting only the bundle route", c.what, a.path)
			}
		}
	}
}

// A bundle can arrive whole and still be wrong: the wrong design, no version, art that is
// not the art its hash names. Those refusals blame the theme, and say which.
func TestSiteImportRefusesADamagedDesign(t *testing.T) {
	s, _, _ := libHub(t)
	art := pngArt(4, 4, 11)
	shaOf := func(b []byte) string { sum := sha256.Sum256(b); return hex.EncodeToString(sum[:]) }
	artSha := shaOf(art)
	damaged := func(why string, doc map[string]any) {
		srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write(bundleOf(t, doc)) })
		if got := importErr(t, s, siteTestLink(srv, siteDesignID)); got != "the theme from "+siteHostOf(srv)+" is damaged: "+why {
			t.Errorf("damaged bundle: error = %q, want %q", got, why)
		}
	}
	notTheme := func(body string) {
		srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "text/html")
			_, _ = w.Write([]byte(body))
		})
		if got := importErr(t, s, siteTestLink(srv, siteDesignID)); got != siteHostOf(srv)+" did not answer with a theme" {
			t.Errorf("not a theme %q: error = %q", body, got)
		}
	}
	assets := func(m map[string]string) map[string]string { return m }
	damaged("it names another design", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": "ZzYyXxWwVvUuTtSsRrQqPp", "version": 1,
		"app": "clock", "sharedName": "Skeleton by @greg", "design": map[string]any{}, "assets": assets(nil),
	})
	damaged("it has no version", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID,
		"app": "clock", "sharedName": "Skeleton by @greg", "design": map[string]any{}, "assets": assets(nil),
	})
	damaged("it has no version", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 0,
		"app": "clock", "sharedName": "Skeleton by @greg", "design": map[string]any{}, "assets": assets(nil),
	})
	damaged("it names no app", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 2,
		"app": "Clock!", "sharedName": "Skeleton by @greg", "design": map[string]any{}, "assets": assets(nil),
	})
	damaged("it has no shared name", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 2,
		"app": "clock", "design": map[string]any{}, "assets": assets(nil),
	})
	damaged(fmt.Sprintf("its shared name is over %d bytes", orbnet.ThemeNameMax), map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 2, "app": "clock",
		"sharedName": strings.Repeat("n", orbnet.ThemeNameMax+1), "design": map[string]any{}, "assets": assets(nil),
	})
	damaged("it carries no design", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 2, "app": "clock",
		"sharedName": "Skeleton by @greg", "assets": assets(nil),
	})
	damaged("its art is not readable", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 2, "app": "clock",
		"sharedName": "Skeleton by @greg", "design": map[string]any{}, "assets": []string{"not an object"},
	})
	damaged("an image is missing ("+artSha[:12]+")", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 2, "app": "clock",
		"sharedName": "Skeleton by @greg", "design": map[string]any{
			"orbAssets": []any{map[string]any{"sha256": artSha}}}, "assets": assets(nil),
	})
	damaged("an image is missing (zzzz)", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 2, "app": "clock",
		"sharedName": "Skeleton by @greg", "design": map[string]any{
			"orbAssets": []any{map[string]any{"sha256": "zzzz"}}}, "assets": assets(nil),
	})
	damaged("its art is not readable", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 2, "app": "clock",
		"sharedName": "Skeleton by @greg", "design": map[string]any{
			"orbAssets": []any{map[string]any{"sha256": artSha}}},
		"assets": assets(map[string]string{artSha: "this is not base64!!"}),
	})
	damaged("an image came back changed ("+artSha[:12]+")", map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": siteDesignID, "version": 2, "app": "clock",
		"sharedName": "Skeleton by @greg", "design": map[string]any{
			"orbAssets": []any{map[string]any{"sha256": artSha}}},
		"assets": assets(map[string]string{artSha: base64.StdEncoding.EncodeToString([]byte("changed"))}),
	})
	notTheme("<!doctype html><html><body>a design, they said</body></html>")
	notTheme(`[1,2,3]`)
	notTheme(`"orb-design-bundle"`)
	notTheme(`{"format":"orb-design-bundle","bundle":2,"id":"` + siteDesignID + `"}`)
	notTheme(`{"bundle":1,"id":"` + siteDesignID + `"}`)
}

// A design the size of a card game is not a theme anyone wears: the hub reads a little past
// the ceiling, stops, and says the theme is too big rather than holding the megabytes.
func TestSiteImportRefusesABundleTooBigToHold(t *testing.T) {
	s, _, _ := libHub(t)
	block := bytes.Repeat([]byte("x"), 1<<20)
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		for i := 0; i < siteBundleMax/(1<<20)+2; i++ {
			_, _ = w.Write(block)
		}
	})
	if got := importErr(t, s, siteTestLink(srv, siteDesignID)); got != "that theme is too big to import (over 32 MB)" {
		t.Errorf("oversized bundle: error = %q", got)
	}
}

// The paste door takes one method and one field, and a paste that is not a link is refused
// in the same sentence whether it arrived empty, huge or as somebody's shopping list.
func TestSiteImportDoorRefusesWhatIsNotALink(t *testing.T) {
	s, _, _ := libHub(t)
	rec := rawCall(t, s, http.MethodGet, "/api/site/import", nil)
	if rec.Code != http.StatusMethodNotAllowed || !strings.Contains(rec.Body.String(), "POST only") {
		t.Errorf("GET /api/site/import = %d %s, want 405 POST only", rec.Code, rec.Body)
	}
	for _, body := range []string{`{}`, `{"link":""}`, `{"link":"   "}`, `not json`, `[1,2]`, `{"link":7}`} {
		env := designCall(t, s, http.MethodPost, "/api/site/import", []byte(body))
		if env["ok"] != false || env["error"] != siteLinkRefused {
			t.Errorf("import of %s = %v, want the link sentence", body, env)
		}
	}
	env := designCall(t, s, http.MethodPost, "/api/site/import",
		[]byte(`{"link":"https://orb-os.net/t/`+siteDesignID+`?pad=`+strings.Repeat("p", siteLinkMax)+`"}`))
	if env["ok"] != false || env["error"] != siteLinkRefused {
		t.Errorf("an oversized paste = %v, want the link sentence", env)
	}
}

// The editor reads the bundle back from the hub, byte for byte: the hub is a courier for
// the site's own bytes, and a reload has to find the same ones.
func TestSiteBundleDoorHandsTheServedBytesBack(t *testing.T) {
	s, _, _ := libHub(t)
	served := []byte("{\n  \"format\": \"orb-design-bundle\", \"bundle\": 1, \"id\": \"" + siteDesignID +
		"\", \"version\": 7, \"app\": \"clock\", \"sharedName\": \"Skeleton by @greg\",\n   \"design\": " +
		"{\"dial\":\"skeleton\"}, \"assets\": {}, \"siteFieldTheHubNeverHeardOf\": true\n}\n")
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(served)
	})
	tk := ticketOf(t, siteImport(t, s, siteTestLink(srv, siteDesignID)))
	rec := rawCall(t, s, http.MethodGet, "/api/site/bundle?ticket="+tk, nil)
	if rec.Code != 200 || !bytes.Equal(rec.Body.Bytes(), served) {
		t.Fatalf("bundle = %d %.80s, want the site's bytes untouched", rec.Code, rec.Body)
	}
	if ct, cc := rec.Header().Get("Content-Type"), rec.Header().Get("Cache-Control"); ct != "application/json" || cc != "no-store" {
		t.Errorf("bundle headers = %q %q, want application/json and no-store", ct, cc)
	}
	if rec2 := rawCall(t, s, http.MethodGet, "/api/site/bundle?ticket="+tk, nil); !bytes.Equal(rec2.Body.Bytes(), served) {
		t.Error("a second read of the same ticket got something else; an editor reload must work")
	}
	var env map[string]any
	rec3 := rawCall(t, s, http.MethodGet, "/api/site/bundle?ticket=00000000000000000000000000000000", nil)
	if json.Unmarshal(rec3.Body.Bytes(), &env) != nil || env["error"] != siteTicketGone {
		t.Errorf("an unknown ticket = %.120s, want the expired sentence", rec3.Body)
	}
	if env := designCall(t, s, http.MethodGet, "/api/site/bundle", nil); env["ok"] != false || env["error"] != siteTicketGone {
		t.Errorf("no ticket at all = %v, want the expired sentence", env)
	}
	if rec := rawCall(t, s, http.MethodPost, "/api/site/bundle?ticket="+tk, nil); rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("POST /api/site/bundle = %d, want 405", rec.Code)
	}
}

// libEntryView is one /api/library/entries row, seen here only for the fields the import
// flow is supposed to move: the theme's file, and where it came from.
type libEntryView struct {
	Name  string `json:"name"`
	State string `json:"state"`
	Theme *struct {
		File   string       `json:"file"`
		Stem   string       `json:"stem"`
		Origin *ThemeOrigin `json:"origin"`
	} `json:"theme"`
}

func entriesView(t *testing.T, s *Server, app string) map[string]libEntryView {
	t.Helper()
	rec := rawCall(t, s, http.MethodGet, "/api/library/entries?app="+app, nil)
	var env struct {
		OK   bool           `json:"ok"`
		Data []libEntryView `json:"data"`
	}
	if json.Unmarshal(rec.Body.Bytes(), &env) != nil || !env.OK {
		t.Fatalf("entries: %d %.200s", rec.Code, rec.Body)
	}
	out := map[string]libEntryView{}
	for _, e := range env.Data {
		out[e.Name] = e
	}
	return out
}

// shelfNames lists what the themes directory holds, so a refusal can be proved to have left
// nothing behind rather than merely answered no.
func shelfNames(t *testing.T, dir string) []string {
	t.Helper()
	files, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	var out []string
	for _, f := range files {
		out = append(out, f.Name())
	}
	return out
}

// The keep door is the only thing that files an import: it takes the editor's bake, files
// it the way every other keep does, and writes the record of the design beside it.
func TestSiteKeepFilesTheThemeAndRecordsWhereItCameFrom(t *testing.T) {
	s, _, root := libHub(t)
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(goodBundle(t, siteDesignID, 4, "clock", "Skeleton by @greg", []byte("art")))
	})
	tk := ticketOf(t, siteImport(t, s, siteTestLink(srv, siteDesignID)))
	bake := withDesign(orbfake.Container("Skeleton by @greg"), string(designDoc("Skeleton by @greg", "v1.2.30", 1)))
	env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake)
	if env["ok"] != true {
		t.Fatalf("keep: %v", env)
	}
	d, _ := env["data"].(map[string]any)
	file, _ := d["file"].(string)
	if d["name"] != "Skeleton by @greg" || !strings.HasSuffix(file, ".orbtheme") {
		t.Fatalf("keep data = %v, want the baked name and its shelf file", d)
	}
	shelf := filepath.Join(root, "themes", file)
	body, err := os.ReadFile(shelf)
	if err != nil {
		t.Fatalf("shelf = %v: %v", shelfNames(t, filepath.Join(root, "themes")), err)
	}
	if !bytes.Equal(body, bake) {
		t.Error("the shelf holds something other than the bytes the editor baked")
	}
	o := d["origin"].(map[string]any)
	if o["site"] != strings.ToLower(srv.URL) || o["designId"] != siteDesignID || o["version"] != float64(4) {
		t.Errorf("origin = %v, want this site, this design, version 4", o)
	}
	side, err := os.ReadFile(themeOriginPath(shelf))
	if err != nil {
		t.Fatalf("no sidecar beside the theme: %v", err)
	}
	var rec ThemeOrigin
	if json.Unmarshal(side, &rec) != nil {
		t.Fatalf("sidecar is not JSON: %.80s", side)
	}
	if rec.Site != strings.ToLower(srv.URL) || rec.DesignID != siteDesignID || rec.Version != 4 || rec.ImportedAt <= 0 {
		t.Errorf("sidecar = %+v", rec)
	}
	if !bytes.Contains(side, []byte("\n  ")) {
		t.Errorf("sidecar is not indented JSON: %s", side)
	}
	e := entriesView(t, s, "clock")["Skeleton by @greg"]
	if e.Theme == nil || e.Theme.Origin == nil {
		t.Fatalf("entries = %+v, want the theme with its origin", e)
	}
	if e.Theme.Origin.Site != rec.Site || e.Theme.Origin.DesignID != siteDesignID || e.Theme.Origin.Version != 4 {
		t.Errorf("entries origin = %+v, want the sidecar's", e.Theme.Origin)
	}
	// The ticket is spent: the same bake cannot be kept twice from one import.
	if got := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake)["error"]; got != siteTicketGone {
		t.Errorf("a second keep = %v, want the expired sentence", got)
	}
	if got := designCall(t, s, http.MethodGet, "/api/site/bundle?ticket="+tk, nil)["error"]; got != siteTicketGone {
		t.Errorf("the bundle after the keep = %v, want the expired sentence", got)
	}
	// Forgetting the entry takes the record with the theme, so an orphan sidecar cannot
	// come back to claim a later theme of the same name.
	name := url.QueryEscape("Skeleton by @greg")
	if env := designCall(t, s, http.MethodPost, "/api/library/forget?app=clock&name="+name+"&what=all", nil); env["ok"] != true {
		t.Fatalf("forget: %v", env)
	}
	if left := shelfNames(t, filepath.Join(root, "themes")); len(left) != 0 {
		t.Errorf("forgetting the entry left %v on the shelf", left)
	}
}

// A bake is only this import's theme if it is the design's: the app and the name the bundle
// named have to be the app and the name the container carries, because the origin record
// written beside the file claims both. A refusal leaves the shelf as it was.
func TestSiteKeepRefusesABakeThatIsNotTheImport(t *testing.T) {
	s, _, root := libHub(t)
	shelf := filepath.Join(root, "themes")
	cases := []struct {
		what  string
		app   string
		body  []byte
		where string // the sentence's own words
	}{
		{"another design's name", "clock", orbfake.Container("Trendy by @greg"),
			"the bake is named “Trendy by @greg”, not “Skeleton by @greg”"},
		{"another app's bake", "flights", orbfake.Container("Skeleton by @greg"),
			"that theme is not one of the flights app’s"},
		{"not a theme at all", "clock", []byte("the editor sent its own HTML"), ""},
		{"too big for an orb", "clock", bytes.Repeat([]byte("x"), orbnet.MaxThemeUpload+1),
			"that theme is over what an orb will accept"},
	}
	for _, c := range cases {
		srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
			_, _ = w.Write(goodBundle(t, siteDesignID, 2, c.app, "Skeleton by @greg"))
		})
		tk := ticketOf(t, siteImport(t, s, siteTestLink(srv, siteDesignID)))
		got, _ := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, c.body)["error"].(string)
		if got == "" {
			t.Errorf("%s: the keep was accepted", c.what)
		} else if c.where != "" && !strings.Contains(got, c.where) {
			t.Errorf("%s: keep = %q, want a refusal saying %q", c.what, got, c.where)
		} else if strings.HasPrefix(got, "orbnet: ") {
			t.Errorf("%s: keep = %q, want the hub's own sentence, not the library package's", c.what, got)
		}
		if left := shelfNames(t, shelf); len(left) != 0 {
			t.Errorf("%s: a refused bake left %v on the shelf", c.what, left)
		}
		// the ticket survives a refusal: the owner fixes the bake and sends it again
		if !bundleHeld(t, s, tk) {
			t.Errorf("%s: a refused bake spent its import's ticket", c.what)
		}
	}
	if env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket=deadbeefdeadbeefdeadbeefdeadbeef",
		orbfake.Container("Skeleton by @greg")); env["error"] != siteTicketGone {
		t.Errorf("an unknown ticket = %v, want the expired sentence", env)
	}
	if rec := rawCall(t, s, http.MethodGet, "/api/site/keep?ticket="+siteDesignID, nil); rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("GET /api/site/keep = %d, want 405", rec.Code)
	}
}

// An import whose record cannot be written says so, keeps the theme it was told to keep,
// and leaves the ticket live so the next try can complete the pair.
func TestSiteKeepReportsARecordItCouldNotWrite(t *testing.T) {
	s, _, root := libHub(t)
	shelf := filepath.Join(root, "themes")
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(goodBundle(t, siteDesignID, 2, "clock", "Skeleton by @greg"))
	})
	bake := orbfake.Container("Skeleton by @greg")
	env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+
		ticketOf(t, siteImport(t, s, siteTestLink(srv, siteDesignID))), bake)
	if env["ok"] != true {
		t.Fatalf("the first keep: %v", env)
	}
	dest := filepath.Join(shelf, env["data"].(map[string]any)["file"].(string))
	// A directory standing where the sidecar goes is the cheapest write failure a test can
	// arrange, and it is the same failure a read-only tree or a full disk would be.
	blocker := themeOriginPath(dest)
	if err := os.Remove(blocker); err != nil {
		t.Fatal(err) // the first keep wrote the record; the block needs the empty space
	}
	if err := os.Mkdir(blocker, 0o755); err != nil {
		t.Fatal(err)
	}
	tk := ticketOf(t, siteImport(t, s, siteTestLink(srv, siteDesignID)))
	if got, _ := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake)["error"].(string); !strings.HasPrefix(got,
		"the theme was kept, but where it came from could not be recorded: ") {
		t.Fatalf("keep with the record blocked = %q, want the half-kept sentence", got)
	}
	if b, err := os.ReadFile(dest); err != nil || !bytes.Equal(b, bake) {
		t.Errorf("the theme was not left on the shelf beside its blocked record: %v", err)
	}
	if !bundleHeld(t, s, tk) {
		t.Error("the ticket of a half-kept import is gone: the retry cannot complete the pair")
	}
	if err := os.Remove(blocker); err != nil {
		t.Fatal(err)
	}
	if env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake); env["ok"] != true {
		t.Fatalf("the retry = %v, want the record written", env)
	}
	if o := readThemeOrigin(dest); o == nil || o.Version != 2 {
		t.Errorf("origin after the retry = %+v, want the retry to complete it", o)
	}
}

// Importing the same design twice holds two tickets, and the keep that lands last is the
// one the shelf and the record describe: a newer version over the older bake, one file.
func TestSiteKeepNewerVersionRewritesTheRecord(t *testing.T) {
	s, _, root := libHub(t)
	shelf := filepath.Join(root, "themes")
	version := 4
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(goodBundle(t, siteDesignID, version, "clock", "Skeleton by @greg"))
	})
	link := siteTestLink(srv, siteDesignID)
	bake := orbfake.Container("Skeleton by @greg")
	first := ticketOf(t, siteImport(t, s, link))
	second := ticketOf(t, siteImport(t, s, link))
	if first == second {
		t.Error("importing a design twice handed out one ticket twice")
	}
	keep := func(tk string) string {
		env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake)
		if env["ok"] != true {
			t.Fatalf("keep %s: %v", tk, env)
		}
		return env["data"].(map[string]any)["file"].(string)
	}
	file := keep(first)
	version = 5
	file = keep(ticketOf(t, siteImport(t, s, link)))
	if !bundleHeld(t, s, second) {
		t.Error("keeping one import's bake spent the other import's ticket")
	}
	if left := shelfNames(t, shelf); len(left) != 2 || !slices.Contains(left, file) ||
		!slices.Contains(left, file+".origin.json") {
		t.Fatalf("shelf = %v, want the theme and its record under one name", left)
	}
	if o := readThemeOrigin(filepath.Join(shelf, file)); o == nil || o.Version != 5 {
		t.Errorf("origin = %+v, want the version baked last", o)
	}
}

// A held import is megabytes the hub is carrying for nobody, so it ages out, and a hub
// whose owner pastes and never bakes carries only so many at once.
func TestSiteImportTicketsExpireAndTheStoreCaps(t *testing.T) {
	var st siteTicketStore
	fake := time.Unix(1700000000, 0)
	st.now = func() time.Time { return fake }
	add := func() string {
		tk, err := st.add(&siteTicket{site: "https://orb-os.net", id: siteDesignID, sharedName: "x"})
		if err != nil {
			t.Fatal(err)
		}
		return tk
	}
	one := add()
	if _, ok := st.get(one); !ok {
		t.Fatal("a fresh ticket is not live")
	}
	fake = fake.Add(siteTicketTTL + time.Minute)
	if _, ok := st.get(one); ok {
		t.Error("a fifteen-minute-old ticket is still live")
	}
	fake = fake.Add(-time.Hour)
	hold := []string{}
	for i := 0; i < siteTicketMax; i++ {
		hold = append(hold, add())
	}
	fake = fake.Add(time.Minute) // so the ninth is not also the first one's expiry
	ninth := add()
	for _, tk := range hold[1:] {
		if _, ok := st.get(tk); !ok {
			t.Errorf("ticket %s of eight was dropped before its time", tk)
		}
	}
	if _, ok := st.get(hold[0]); ok {
		t.Error("the ninth import did not drop the oldest ticket")
	}
	if _, ok := st.get(ninth); !ok {
		t.Error("the ninth import is not itself held")
	}
	st.consume(ninth)
	if _, ok := st.get(ninth); ok {
		t.Error("a consumed ticket still reads as live")
	}
	if len(st.order) != siteTicketMax-1 {
		t.Errorf("the store holds %d in order, want %d", len(st.order), siteTicketMax-1)
	}
}

// Every one of the three doors changes something on this machine or fetches from another,
// so every one of them is a control door: no launch token, no work.
func TestSiteImportDoorsRefuseAnUntokenedRequest(t *testing.T) {
	s, _, _ := libHub(t)
	for _, c := range []struct{ method, path string }{
		{http.MethodPost, "/api/site/import"},
		{http.MethodGet, "/api/site/bundle?ticket=" + siteDesignID},
		{http.MethodPost, "/api/site/keep?ticket=" + siteDesignID},
	} {
		rec := untokenedCall(t, s, c.method, c.path, []byte(`{"link":"https://orb-os.net/t/`+siteDesignID+`"}`))
		if rec.Code != http.StatusForbidden {
			t.Errorf("%s %s with no token = %d, want 403 from requireControl", c.method, c.path, rec.Code)
		}
	}
}

// A shelf theme baked here has no origin, and says so by saying nothing: the entries
// answer for it must be the bytes it always was. A sidecar that is not a record — no site,
// no design, a version below the first one — is the same nothing, and a sidecar is never a
// theme of its own.
func TestThemeOriginIsOnlyThereWhenTheRecordSaysSo(t *testing.T) {
	s, _, root := libHub(t)
	shelf := filepath.Join(root, "themes")
	put := func(file string, body []byte) {
		if err := os.WriteFile(filepath.Join(shelf, file), body, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	put("tourbillon.orbtheme", orbfake.Container("Tourbillon"))
	for _, junk := range []string{`{"site":"","designId":"` + siteDesignID + `","version":9}`,
		`{"site":"https://orb-os.net","designId":"","version":9}`,
		`{"site":"https://orb-os.net","designId":"` + siteDesignID + `","version":0}`,
		`{"site":"https://orb-os.net","designId":"` + siteDesignID + `"}`,
		`not json at all`} {
		put("tourbillon.orbtheme.origin.json", []byte(junk))
		if o := readThemeOrigin(filepath.Join(shelf, "tourbillon.orbtheme")); o != nil {
			t.Errorf("readThemeOrigin of %s = %+v, want nothing", junk, o)
		}
	}
	os.Remove(themeOriginPath(filepath.Join(shelf, "tourbillon.orbtheme")))
	rec := rawCall(t, s, http.MethodGet, "/api/library/entries?app=clock", nil)
	if bytes.Contains(rec.Body.Bytes(), []byte(`"origin"`)) {
		t.Errorf("entries = %.200s, want no origin key for a theme the hub never imported", rec.Body)
	}
	put("skeleton.orbtheme.origin.json", []byte(`{"site":"https://orb-os.net","designId":"`+siteDesignID+`","version":2}`))
	if es := entriesOf(t, s, "clock"); len(es) != 1 {
		t.Errorf("entries = %v, want Tourbillon alone: a record is not a theme", es)
	}
}

// The other ways a theme reaches the shelf — the editor's Send, Rebake all, a file import —
// keep the theme and its record as a pair: a rebake of an imported theme still came from
// that design. When an edited copy stops tracking it is G3's question, not this door's.
func TestThemeOriginSurvivesTheOtherKeepPaths(t *testing.T) {
	s, _, root := libHub(t)
	shelf := filepath.Join(root, "themes")
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(goodBundle(t, siteDesignID, 3, "clock", "Skeleton by @greg"))
	})
	env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+ticketOf(t,
		siteImport(t, s, siteTestLink(srv, siteDesignID))), orbfake.Container("Skeleton by @greg"))
	if env["ok"] != true {
		t.Fatalf("keep: %v", env)
	}
	dest := filepath.Join(shelf, env["data"].(map[string]any)["file"].(string))
	rebaked := withDesign(orbfake.Container("Skeleton by @greg"), string(designDoc("Skeleton by @greg", "v1.2.31", 1)))
	if env := designCall(t, s, http.MethodPost, "/api/library/keep?app=clock&fold=0", rebaked); env["ok"] != true {
		t.Fatalf("the rebake's keep: %v", env)
	}
	if b, err := os.ReadFile(dest); err != nil || !bytes.Equal(b, rebaked) {
		t.Errorf("the rebake did not replace the shelf's copy: %v", err)
	}
	o := readThemeOrigin(dest)
	if o == nil || o.Version != 3 || o.DesignID != siteDesignID {
		t.Errorf("origin after a rebake = %+v, want the import's own record left alone", o)
	}
	if e := entriesView(t, s, "clock")["Skeleton by @greg"]; e.Theme == nil || e.Theme.Origin == nil {
		t.Errorf("entries after a rebake = %+v, want the theme still wearing its origin", e)
	}
}

// A site that cannot be reached is described as unreachable, with the hub's own error
// words after it: the owner's next question is whether the address is right.
func TestSiteImportRefusesASiteItCannotReach(t *testing.T) {
	s, _, _ := libHub(t)
	got := importErr(t, s, "http://127.0.0.1:1/t/"+siteDesignID)
	if !strings.HasPrefix(got, "could not reach 127.0.0.1:1: ") || len(got) < 40 {
		t.Errorf("an unreachable site = %q, want the reach sentence naming the host and the error", got)
	}
}

// Until the site opens the door is shut (-site off): the library says so, the page hides
// Import and Upload on it, and no /api/site route answers. The zero Server is the door open.
func TestSiteDoorShutWithoutTheFlag(t *testing.T) {
	s, _, _ := libHub(t)
	if env := designCall(t, s, http.MethodGet, "/api/library", nil); env["data"].(map[string]any)["site"] != true {
		t.Errorf("an open hub's library says site = %v, want true", env["data"].(map[string]any)["site"])
	}
	s.siteHidden = true
	if env := designCall(t, s, http.MethodGet, "/api/library", nil); env["data"].(map[string]any)["site"] != false {
		t.Errorf("a shut hub's library says site = %v, want false", env["data"].(map[string]any)["site"])
	}
	for _, p := range []string{"/api/site/import", "/api/site/bundle", "/api/site/keep"} {
		if rec := rawCall(t, s, http.MethodPost, p, []byte(`{}`)); rec.Code != http.StatusNotFound {
			t.Errorf("POST %s on a shut hub = %d, want 404", p, rec.Code)
		}
	}
	// H2's site surface waits behind the same door: with -site off there is no press, no
	// status read, no design list, no disconnect and — lead review point 9 — not one request
	// leaves the hub for the site. A shut door that still opens a listener, mints a verifier,
	// POSTs a token exchange, reads a design list or revokes a bearer is a shut door with the
	// handle off.
	siteOff, siteOffAsks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		t.Errorf("a shut hub asked the site for %s", r.URL.Path)
	})
	t.Setenv("GORB_SITE", siteOff.URL)
	for _, c := range []struct{ method, path string }{
		{http.MethodPost, "/api/site/connect"},
		{http.MethodGet, "/api/site/link-status"},
		{http.MethodGet, "/api/site/designs"},
		{http.MethodPost, "/api/site/disconnect"},
	} {
		if rec := rawCall(t, s, c.method, c.path, nil); rec.Code != http.StatusNotFound {
			t.Errorf("%s %s on a shut hub = %d, want 404", c.method, c.path, rec.Code)
		}
	}
	if s.pendingSiteLink() {
		t.Error("a shut hub's press opened a connect attempt anyway")
	}
	if s.statePath != "" {
		if b, err := os.ReadFile(s.siteLinkPath()); err == nil {
			t.Errorf("a shut hub stored a credential: %s", b)
		}
	}
	if len(*siteOffAsks) != 0 {
		t.Errorf("a shut hub made %d requests to the site: %v", len(*siteOffAsks), *siteOffAsks)
	}
	// G3's release check and Updates list ask the site too, so they wait behind the same door,
	// and the page keeps the Updates section hidden until /api/updates answers.
	for _, p := range []string{"/api/release/latest", "/api/updates"} {
		if rec := rawCall(t, s, http.MethodGet, p, nil); rec.Code != http.StatusNotFound {
			t.Errorf("GET %s on a shut hub = %d, want 404", p, rec.Code)
		}
	}
	if b := blockOfLine(t, readWebFile(t, "index.html"), `id="mn-updates"`); !strings.Contains(b, "hidden") {
		t.Error("#mn-updates is not hidden in the markup, so a shut hub shows an Updates list it can't read")
	}
	if !strings.Contains(readAppJS(t), "if (r.status === 404) { if (section) section.hidden = true; return; }") {
		t.Error("loadUpdates no longer hides the section when the door is shut")
	}
	page, app := readWebFile(t, "index.html"), readAppJS(t)
	for _, id := range []string{"mn-site-btn", "mn-lib-upload"} {
		if b := blockOfLine(t, page, `id="`+id+`"`); !strings.Contains(b, "hidden") {
			t.Errorf("#%s is not hidden in the markup, so it shows before the hub says the site is open", id)
		}
	}
	// H2 step 3: My orb-os.net — the connect block and the design list — stand INSIDE
	// #mn-site-row, so a shut hub hiding that row (which it does every time the library says
	// site:false) puts them away with it. A sibling of the row would leak a Connect button onto
	// a release that has no /api/site/connect to answer it. The row closes at its first </div>.
	shutRow := regionOf(t, page, `id="mn-site-row"`, "</div>")
	for _, id := range []string{"mn-site-connect-block", "mn-site-designs-block"} {
		if !strings.Contains(shutRow, `id="`+id+`"`) {
			t.Errorf("#%s is not inside #mn-site-row, so a shut hub's row-hiding does not hide it", id)
		}
	}
	if !strings.Contains(app, `if (site) loadSiteLink()`) {
		t.Error("loadLibThemes reads the site link even for a shut hub, dialing a route this build never registers")
	}
	if !strings.Contains(app, `const site = data.site === true;`) {
		t.Error("loadLibThemes no longer reads the library's site word before showing the door")
	}
}

// The resolve client is the half of §7.4 that G3 owns, and H3 owns the door it dials, so
// it is tested against a fake rather than a site that does not exist yet: the answer it
// reads, the two status codes that mean "no longer shared" rather than "failed", and the
// one that must not be smoothed over.
func TestResolveOriginReadsTheAnswerAndTheGone(t *testing.T) {
	ctx := context.Background()
	s := newHub()
	orig := &ThemeOrigin{Site: "", DesignID: siteDesignID, Version: 3}

	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/hub/resolve" {
			http.NotFound(w, r)
			return
		}
		q := r.URL.Query()
		if q.Get("designId") != siteDesignID || q.Get("version") != "3" || q.Get("site") == "" {
			t.Errorf("the resolve query was %q, want the origin's site, design and version", r.URL.RawQuery)
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"designId":"` + siteDesignID + `","app":"clock","version":9,"sharedName":"Night dial by @greg","handle":"greg"}`))
	})
	orig.Site = strings.ToLower(srv.URL)
	got, err := s.resolveOrigin(ctx, orig)
	if err != nil {
		t.Fatalf("resolve: %v", err)
	}
	if !got.Resolved || got.Gone || got.Version != 9 || got.App != "clock" ||
		got.SharedName != "Night dial by @greg" || got.DesignID != siteDesignID {
		t.Errorf("resolve = %+v, want the design's current version and current shared name", got)
	}

	for _, code := range []int{http.StatusNotFound, http.StatusGone} {
		gone, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(code) })
		orig.Site = strings.ToLower(gone.URL)
		ans, err := s.resolveOrigin(ctx, orig)
		if err != nil {
			t.Errorf("a %d is not a transport failure: %v", code, err)
		}
		if ans == nil || !ans.Gone || ans.Resolved {
			t.Errorf("a %d read as %+v, want Gone and not Resolved", code, ans)
		}
	}

	bad, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(http.StatusInternalServerError) })
	orig.Site = strings.ToLower(bad.URL)
	if ans, err := s.resolveOrigin(ctx, orig); err == nil {
		t.Errorf("an HTTP 500 read as %+v and no error; a theme that could not be checked must never read as current", ans)
	}

	// No answer at all — an origin with nothing to dial — is a refusal rather than a
	// panic, because the list builds a row out of whatever the resolver said.
	if _, err := s.resolveOrigin(ctx, &ThemeOrigin{DesignID: siteDesignID, Version: 3}); err == nil {
		t.Error("an origin with no site resolved")
	}
	if _, err := s.resolveOrigin(ctx, nil); err == nil {
		t.Error("no origin resolved")
	}
}

// --- S3b: an SVG is art too -------------------------------------------------------------

// svgBody is one picture as S3b reads one: real SVG text, small enough and plain enough
// that the only thing standing between it and the shelf is the rules it must clear.
func svgBody() []byte {
	return []byte(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">` +
		`<rect width="24" height="24" fill="#1b3a5c"/><use href="#rect"/></svg>`)
}

// svgBundle is goodBundle's shape with one picture carried under the SVG prefix.
func svgBundle(t *testing.T, id string, version int, app, sharedName string, body []byte) []byte {
	t.Helper()
	sum := sha256.Sum256(body)
	sha := hex.EncodeToString(sum[:])
	return bundleOf(t, map[string]any{
		"format": "orb-design-bundle", "bundle": 1, "id": id, "version": version, "app": app,
		"sharedName": sharedName, "name": sharedName, "handle": "greg",
		"design": map[string]any{"format": "wc-design", "schema": 1, "app": app, "themeName": sharedName,
			"orbAssets": []any{map[string]any{"sha256": sha, "prefix": "data:image/svg+xml;base64"}}},
		"assets": map[string]string{sha: base64.StdEncoding.EncodeToString(body)},
	})
}

// The hub is not the image pipeline: an SVG that clears the rules comes back out of the
// bundle door exactly as it went in, and the keep files the bake the editor made from it.
func TestSiteImportCarriesSvgArtByteForByte(t *testing.T) {
	s, _, root := libHub(t)
	body := svgBody()
	srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(svgBundle(t, siteDesignID, 6, "clock", "Compass by @greg", body))
	})
	env := siteImport(t, s, siteTestLink(srv, siteDesignID))
	tk := ticketOf(t, env)
	rec := rawCall(t, s, http.MethodGet, "/api/site/bundle?ticket="+tk, nil)
	if !bytes.Equal(rec.Body.Bytes(), svgBundle(t, siteDesignID, 6, "clock", "Compass by @greg", body)) {
		t.Errorf("the held bundle is not the bytes the site served: %s", rec.Body)
	}
	bake := withDesign(orbfake.Container("Compass by @greg"), string(designDoc("Compass by @greg", "v1.2.30", 1)))
	if env := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake); env["ok"] != true {
		t.Fatalf("keep: %v", env)
	}
	var themes []string
	for _, f := range shelfNames(t, filepath.Join(root, "themes")) {
		if strings.HasSuffix(f, ".orbtheme") {
			themes = append(themes, f)
		}
	}
	if len(themes) != 1 || !strings.HasPrefix(themes[0], "clock-compass-by-greg") {
		t.Errorf("shelf = %v, want the one import kept", themes)
	}
}

// An SVG over the weight ceiling, holding a handler or pointing outside itself is refused
// the way oversized art is: the bundle is whole, and the sentence says what its art is.
func TestSiteImportRefusesSvgOutsideTheBounds(t *testing.T) {
	s, _, _ := libHub(t)
	cases := []struct {
		what string
		body []byte
		want func(body []byte) string
	}{
		{"handler", []byte(`<svg xmlns="http://www.w3.org/2000/svg" onload="alert(1)"><rect width="2" height="2"/></svg>`), func([]byte) string { return svgRefusalHandler }},
		{"outside link", []byte(`<svg xmlns="http://www.w3.org/2000/svg"><image href="https://example.com/a.png"/></svg>`), func([]byte) string { return svgRefusalLink }},
		{"not an svg", []byte("text that is not markup, and is long enough to read as neither"), func([]byte) string { return svgRefusalNotSvg }},
		{"oversize", append([]byte(`<svg xmlns="http://www.w3.org/2000/svg">`),
			append(bytes.Repeat([]byte(" "), svgMaxBytes), []byte("</svg>")...)...),
			func(b []byte) string { return fmt.Sprintf(svgRefusalSizeFormat, (len(b)+1023)/1024) }},
	}
	for _, c := range cases {
		srv, _ := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			_, _ = w.Write(svgBundle(t, siteDesignID, 7, "clock", "Compass by @greg", c.body))
		})
		t.Helper()
		want := c.want(c.body)
		got := importErr(t, s, siteTestLink(srv, siteDesignID))
		if !strings.HasPrefix(got, "the theme from "+siteHostOf(srv)+" can't be imported: ") || !strings.HasSuffix(got, want) {
			t.Errorf("%s svg: error = %q, want the can't-be-imported wrapper ending %q", c.what, got, want)
		}
	}
}

// --- H2 §5: the same door, opened by a design's own id ---------------------------------------

// importIDHub stands the state §5's own-design door starts from: a hub with a shelf, a site
// that answers whatever the test wants from its §2.6 door, and a credential stored the way
// step 1's exchange leaves it. The bearer it returns is the one that credential carries, so
// the test can name the exact header the theme door must have seen.
func importIDHub(t *testing.T, h func(w http.ResponseWriter, r *http.Request)) (*Server, string, string, string, *[]siteAsk) {
	t.Helper()
	s, _, root := libHub(t)
	srv, asks := siteServer(t, h)
	t.Setenv("GORB_SITE", srv.URL)
	s.statePath = filepath.Join(t.TempDir(), "state.json")
	tok := hubRandom(32)
	if err := s.saveSiteLink(&siteLink{Token: tok, Site: siteReleaseBase(), LinkedAt: 1730000000, Handle: "greg"}); err != nil {
		t.Fatalf("saveSiteLink: %v", err)
	}
	return s, root, srv.URL, tok, asks
}

// siteImportID asks for an own design by id, the second body §5 gives the import door.
func siteImportID(t *testing.T, s *Server, id string) map[string]any {
	t.Helper()
	body, err := json.Marshal(map[string]string{"designId": id})
	if err != nil {
		t.Fatal(err)
	}
	return designCall(t, s, http.MethodPost, "/api/site/import", body)
}

// importIDErr is siteImportID's refusal sentence, or a failure if it was not refused.
func importIDErr(t *testing.T, s *Server, id string) string {
	t.Helper()
	env := siteImportID(t, s, id)
	if env["ok"] == true {
		t.Fatalf("import of %q was accepted: %v", id, env)
	}
	msg, _ := env["error"].(string)
	return msg
}

func importBody(t *testing.T, s *Server, body map[string]string) map[string]any {
	t.Helper()
	b, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	return designCall(t, s, http.MethodPost, "/api/site/import", b)
}

// An own design imports like a shared one and is asked for like a private one: the id body
// puts the link's bearer on the same §2.6 door the paste uses, the bundle that comes back is
// held under the same one-time ticket, and the record filed beside the kept theme names the
// configured site and the design's id — which is what lets H3 ask about it later and the
// library say "from <host>" the same way it does for a link.
func TestSiteImportIdUsesTheBearerAndTheTicket(t *testing.T) {
	bundle := goodBundle(t, siteDesignID, 7, "clock", "Skeleton by @greg", pngArt(64, 64, 7))
	s, root, site, tok, asks := importIDHub(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write(bundle)
	})
	env := siteImportID(t, s, siteDesignID)
	tk := ticketOf(t, env)
	d := env["data"].(map[string]any)
	if len(*asks) != 1 {
		t.Fatalf("the id import made %d requests, want the one the door needs: %+v", len(*asks), *asks)
	}
	ask := (*asks)[0]
	if ask.path != "/api/hub/theme/"+siteDesignID {
		t.Errorf("the id import dialed %s, want the §2.6 theme door for this design", ask.path)
	}
	if got := ask.head.Get("Authorization"); got != "Bearer "+tok {
		t.Errorf("the theme door saw %q, want exactly the stored bearer", got)
	}
	if d["site"] != site || d["id"] != siteDesignID || d["version"] != float64(7) ||
		d["sharedName"] != "Skeleton by @greg" {
		t.Errorf("the ticket answer = %v, want this site, this design, version 7, its own name", d)
	}
	// The bytes the editor will bake from are the bytes the site served, whole.
	held := rawCall(t, s, http.MethodGet, "/api/site/bundle?ticket="+tk, nil)
	if held.Code != http.StatusOK || !bytes.Equal(held.Body.Bytes(), bundle) {
		t.Errorf("the bundle door held %d bytes, want the site's own %d", held.Body.Len(), len(bundle))
	}
	// The keep is H1's unchanged, reached with H1's own bake: an own design is filed with a
	// sidecar naming the site this hub is linked to and the id it was asked for by.
	bake := withDesign(orbfake.Container("Skeleton by @greg"), string(designDoc("Skeleton by @greg", "v1.2.30", 1)))
	kept := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake)
	if kept["ok"] != true {
		t.Fatalf("keep: %v", kept)
	}
	file, _ := kept["data"].(map[string]any)["file"].(string)
	shelf := filepath.Join(root, "themes", file)
	side, err := os.ReadFile(themeOriginPath(shelf))
	if err != nil {
		t.Fatalf("no sidecar beside %s: %v", shelf, err)
	}
	var rec ThemeOrigin
	if json.Unmarshal(side, &rec) != nil {
		t.Fatalf("sidecar is not JSON: %.80s", side)
	}
	if rec.Site != site || rec.DesignID != siteDesignID || rec.Version != 7 || rec.ImportedAt <= 0 {
		t.Errorf("sidecar = %+v, want the configured site, this id, version 7", rec)
	}
	// One import, one ticket: the keep spent it, so the bake cannot be filed twice.
	if got := designCall(t, s, http.MethodPost, "/api/site/keep?ticket="+tk, bake)["error"]; got != siteTicketGone {
		t.Errorf("a second keep = %v, want the spent-ticket sentence", got)
	}
}

// Everything the own-design door can refuse, refused without a theme reaching the shelf: the
// hub with no link, the id that is not an id, the body with two shapes in it, and the four
// answers §2.6 gives a bearer call. A wrong id and a wrong paste are one mistake, so they are
// one sentence; only the 401 is news about the link rather than about the design.
func TestSiteImportIdRefusesWhatItCannotImport(t *testing.T) {
	t.Run("no link stored", func(t *testing.T) {
		s, _, _ := libHub(t)
		srv, asks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {})
		t.Setenv("GORB_SITE", srv.URL)
		s.statePath = filepath.Join(t.TempDir(), "state.json")
		if got := importIDErr(t, s, siteDesignID); got != "not connected to orb-os.net" {
			t.Errorf("an unlinked hub = %q, want the sentence that says so", got)
		}
		if len(*asks) != 0 {
			t.Errorf("an unlinked hub asked the site %d times, want not at all", len(*asks))
		}
	})
	// A disconnected file is a hub the site has refused already: the door says so from the
	// file and does not go asking again.
	t.Run("the link is already disconnected", func(t *testing.T) {
		s, _, _, _, asks := importIDHub(t, func(w http.ResponseWriter, r *http.Request) {})
		if err := s.saveSiteLink(&siteLink{Site: siteReleaseBase(), Handle: "greg", Disconnected: true}); err != nil {
			t.Fatal(err)
		}
		if got := importIDErr(t, s, siteDesignID); got != "not connected to orb-os.net" {
			t.Errorf("a disconnected hub = %q, want the sentence that says so", got)
		}
		if len(*asks) != 0 {
			t.Errorf("a disconnected hub asked the site %d times, want not at all", len(*asks))
		}
	})
	t.Run("ids that are not ids", func(t *testing.T) {
		s, _, _, _, asks := importIDHub(t, func(w http.ResponseWriter, r *http.Request) {
			t.Error("a hub with a malformed id still dialed its site")
		})
		for _, id := range []string{"short", "", "   ", strings.Repeat("a", 23), "AbCdEfGhIjKlMnOpQrStU.",
			"https://orb-os.net/t/" + siteDesignID, siteDesignID + "/download", "t/" + siteDesignID, "AbCdEfGhIjKlMnOpQrStU~", " " + strings.Repeat("a", 21) + "!"} {
			if got := importIDErr(t, s, id); got != siteLinkRefused {
				t.Errorf("designId %q = %q, want the one link sentence", id, got)
			}
		}
		if len(*asks) != 0 {
			t.Errorf("the malformed ids reached the site %d times", len(*asks))
		}
	})
	t.Run("one body, one shape", func(t *testing.T) {
		s, _, site, _, asks := importIDHub(t, func(w http.ResponseWriter, r *http.Request) {
			t.Error("a refused body still dialed its site")
		})
		for _, body := range []map[string]string{
			{"link": site + "/t/" + siteDesignID, "designId": siteDesignID},
			{"link": "  ", "designId": "  "},
			{"link": "", "designId": ""},
			{},
			{"sharedName": "Skeleton"},
		} {
			if env := importBody(t, s, body); env["ok"] == true || env["error"] != siteLinkRefused {
				t.Errorf("body %v = %v, want the one link sentence", body, env)
			}
		}
		if len(*asks) != 0 {
			t.Errorf("the two-shape bodies reached the site %d times", len(*asks))
		}
	})
	// §2.6 answers a design this link may not see with the stranger's 404, so the hub has one
	// sentence left for it — and a wrong id is not news about the credential, which stays.
	t.Run("the site has no such design", func(t *testing.T) {
		s, _, _, tok, asks := importIDHub(t, func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusNotFound)
			_, _ = w.Write([]byte(`{"error":"not_found"}`))
		})
		if got := importIDErr(t, s, siteDesignID); got != siteLinkRefused {
			t.Errorf("a 404 = %q, want the same sentence a wrong paste gets", got)
		}
		if l := s.loadSiteLink(); l == nil || l.Token != tok {
			t.Errorf("a wrong id cost the hub its credential: %+v", l)
		}
		if len(*asks) != 1 {
			t.Errorf("the 404 case asked the site %d times, want once", len(*asks))
		}
	})
	// A dead bearer on this door is the same event §5's designs door treats as the end of the
	// link: the sentence is the disconnected one and the file is rewritten, not deleted.
	t.Run("the site refused the bearer", func(t *testing.T) {
		s, _, _, _, asks := importIDHub(t, func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusUnauthorized)
			_, _ = w.Write([]byte(`{"error":"unauthenticated"}`))
		})
		if got := importIDErr(t, s, siteDesignID); got != "disconnected from orb-os.net" {
			t.Errorf("a 401 = %q, want the sentence that says the link is over", got)
		}
		if len(*asks) != 1 {
			t.Errorf("the 401 case asked the site %d times, want once", len(*asks))
		}
		if l := s.loadSiteLink(); l != nil {
			t.Errorf("the 401 left a live credential behind: %+v", l)
		}
		raw := s.disconnectedSiteLink()
		if raw == nil || raw.Handle != "greg" || raw.LinkedAt != 1730000000 {
			t.Errorf("the 401 left %+v, want the file rewritten as disconnected", raw)
		}
	})
	t.Run("a design that was deleted", func(t *testing.T) {
		s, _, _, _, _ := importIDHub(t, func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusGone)
		})
		if got := importIDErr(t, s, siteDesignID); got != "that theme was deleted by its owner" {
			t.Errorf("a 410 = %q, want the informative answer the paste door already gave", got)
		}
	})
	t.Run("an answer that is not a bundle", func(t *testing.T) {
		s, _, site, _, _ := importIDHub(t, func(w http.ResponseWriter, r *http.Request) {
			_, _ = w.Write([]byte(`{"format":"something else"}`))
		})
		if got := importIDErr(t, s, siteDesignID); got != siteHost(site)+" did not answer with a theme" {
			t.Errorf("a non-bundle = %q, want the paste door's own refusal", got)
		}
	})
	// The paste is still the anonymous door it always was, whatever the hub happens to hold:
	// §2.6 runs the H1 path unchanged when no Authorization header arrives, and an owner's
	// link is not this request's business. A 401 there is the site's answer about a design,
	// not news about a credential, so the link is left exactly where it was.
	t.Run("a paste still carries no bearer", func(t *testing.T) {
		s, _, site, tok, asks := importIDHub(t, func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusUnauthorized)
		})
		if got := importErr(t, s, site+"/t/"+siteDesignID); got != siteHost(site)+" answered HTTP 401" {
			t.Errorf("a paste facing a 401 = %q, want the answer the door has always given", got)
		}
		if len(*asks) != 1 {
			t.Fatalf("the paste asked the site %d times, want once", len(*asks))
		}
		if got := (*asks)[0].head.Get("Authorization"); got != "" {
			t.Errorf("the anonymous paste carried %q, want no Authorization header at all", got)
		}
		if l := s.loadSiteLink(); l == nil || l.Token != tok {
			t.Errorf("a 401 about somebody else's design undid the link: %+v", l)
		}
	})
}
