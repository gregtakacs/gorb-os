package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"hash/crc32"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

func designCall(t *testing.T, s *Server, method, path string, body []byte) map[string]any {
	t.Helper()
	rec := rawCall(t, s, method, path, body)
	var env map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("%s %s: %d %s", method, path, rec.Code, rec.Body)
	}
	return env
}

func rawCall(t *testing.T, s *Server, method, path string, body []byte) *httptest.ResponseRecorder {
	t.Helper()
	mux := http.NewServeMux()
	s.routes(mux)
	req := httptest.NewRequest(method, path, bytes.NewReader(body))
	req.Header.Set(tokenHeader, s.token)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	return rec
}

// reheader recomputes a theme header's checksum after a test pokes a word inside it: the
// 128 bytes with word 28 zeroed, as orb_bundle3_hdr_parse sums them.
func reheader(b []byte) []byte {
	h := append([]byte{}, b[:128]...)
	binary.LittleEndian.PutUint32(h[28*4:], 0)
	binary.LittleEndian.PutUint32(b[28*4:], crc32.ChecksumIEEE(h))
	return b
}

// withDesign appends a design after a theme's banks and points the header at it (D43 P7).
func withDesign(body []byte, design string) []byte {
	out := append(append([]byte{}, body...), design...)
	binary.LittleEndian.PutUint32(out[29*4:], uint32(len(body)))
	binary.LittleEndian.PutUint32(out[30*4:], uint32(len(design)))
	binary.LittleEndian.PutUint32(out[31*4:], crc32.ChecksumIEEE([]byte(design)))
	return reheader(out)
}

// withABI restamps a theme's geometry ABI: a bake for a dialect the firmware no longer reads.
func withABI(body []byte, abi uint32) []byte {
	out := append([]byte{}, body...)
	binary.LittleEndian.PutUint32(out[2*4:], abi)
	return reheader(out)
}

func designDoc(name, ver string, art int) []byte {
	return []byte(`{"format":"wc-design","schema":1,"app":"clock","appVersion":"` + ver + `","made":"2026-11-04T00:00:00Z",` +
		`"dial":"skeleton","themeName":"` + name + `","assets":[{"id":"bg-1","dataUrl":"data:image/png;base64,` + strings.Repeat("A", art) + `"}],` +
		`"saved":{"name":"ignored","at":1,"extra":{"dial":"skeleton"}}}`)
}

func libHub(t *testing.T) (*Server, string, string) {
	t.Helper()
	s, ip, _, _ := fakeOrbHub(t)
	root := t.TempDir()
	s.lib = &Library{AppsDir: filepath.Join(root, "apps"), ThemesDir: filepath.Join(root, "themes")}
	if err := os.MkdirAll(s.lib.ThemesDir, 0o755); err != nil {
		t.Fatal(err)
	}
	return s, ip, root
}

type entryJSON struct {
	Name  string `json:"name"`
	State string `json:"state"`
	Draft *struct {
		Name  string          `json:"name"`
		Bytes int             `json:"bytes"`
		Extra json.RawMessage `json:"extra"`
	} `json:"draft"`
	Theme *struct {
		Stem      string `json:"stem"`
		MadeWith  string `json:"made_with"`
		CanReopen bool   `json:"can_reopen"`
		Stale     string `json:"stale"`
	} `json:"theme"`
}

func entriesOf(t *testing.T, s *Server, app string) map[string]entryJSON {
	t.Helper()
	rec := rawCall(t, s, http.MethodGet, "/api/library/entries?app="+app, nil)
	var env struct {
		OK   bool        `json:"ok"`
		Data []entryJSON `json:"data"`
	}
	if json.Unmarshal(rec.Body.Bytes(), &env) != nil || !env.OK {
		t.Fatalf("entries: %s", rec.Body)
	}
	out := map[string]entryJSON{}
	for _, e := range env.Data {
		if _, dup := out[e.Name]; dup {
			t.Errorf("two entries named %q", e.Name)
		}
		out[e.Name] = e
	}
	return out
}

// K9/K10: a draft is a file on the hub, one per name, art and all, and only this app's.
func TestDraftsAreFilesOnTheHubOnePerName(t *testing.T) {
	s, _, root := libHub(t)
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=Big%20sub", designDoc("Big sub", "v1.2.16", 10)); env["ok"] != true {
		t.Fatalf("save: %v", env)
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=Big%20sub", designDoc("Big sub", "v1.2.16", 9000)); env["ok"] != true {
		t.Fatalf("resave: %v", env)
	}
	dir := filepath.Join(root, "designs", "clock")
	if files, _ := os.ReadDir(dir); len(files) != 1 {
		t.Fatalf("%d files in %s after two saves under one name, want one", len(files), dir)
	}
	e := entriesOf(t, s, "clock")["Big sub"]
	if e.State != "draft" || e.Draft == nil || e.Draft.Bytes < 9000 || !strings.Contains(string(e.Draft.Extra), "skeleton") {
		t.Errorf("entry = %+v, want one draft with the art's bytes and the editor's extras", e)
	}
	rec := rawCall(t, s, http.MethodGet, "/api/library/design?app=clock&name=Big%20sub", nil)
	var got map[string]any
	if json.Unmarshal(rec.Body.Bytes(), &got) != nil || got["format"] != "wc-design" || !strings.Contains(rec.Body.String(), strings.Repeat("A", 9000)) {
		t.Fatalf("design = %.200s, want the saved document with its art", rec.Body)
	}
	if sv, _ := got["saved"].(map[string]any); sv["name"] != "Big sub" {
		t.Errorf("saved block = %v, want the hub's own name, not the body's", got["saved"])
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=flights&name=x", designDoc("x", "v1", 1)); env["ok"] == true {
		t.Error("the clock's design was filed under flights")
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=x", []byte(`{"format":"zip"}`)); env["ok"] == true {
		t.Error("a document that is not a design was filed")
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=..%2Fx&name=x", designDoc("x", "v1", 1)); env["ok"] == true {
		t.Error("an app slug with a path in it was accepted")
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=..%2F..%2Fescape", designDoc("e", "v1", 1)); env["ok"] != true {
		t.Fatalf("save: %v", env)
	}
	if _, err := os.Stat(filepath.Join(root, "escape.orbdesign")); err == nil {
		t.Error("a name walked out of the designs folder")
	}
}

// K10: the library is one entry per name — a draft, a theme, or a theme with unsent
// changes — and an entry opens at its newest design.
func TestLibraryIsOneEntryPerName(t *testing.T) {
	s, _, _ := libHub(t)
	put := func(file string, body []byte) {
		if err := os.WriteFile(filepath.Join(s.lib.ThemesDir, file), body, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	put("tourbillon.orbtheme", withDesign(orbfake.Container("Tourbillon"), string(designDoc("Tourbillon", "v1.2.12", 1))))
	put("aurora.orbtheme", orbfake.Container("Aurora")) // baked before designs rode along
	for _, n := range []string{"Tourbillon", "Sketch"} {
		if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name="+n, designDoc(n, "v1.2.16", 1)); env["ok"] != true {
			t.Fatalf("save %s: %v", n, env)
		}
	}
	es := entriesOf(t, s, "clock")
	if len(es) != 3 {
		t.Fatalf("entries = %v, want three names", es)
	}
	if e := es["Tourbillon"]; e.State != "changed" || e.Theme == nil || e.Draft == nil || e.Theme.MadeWith != "v1.2.12" || !e.Theme.CanReopen {
		t.Errorf("Tourbillon = %+v, want a theme with unsent changes, made with v1.2.12", e)
	}
	if e := es["Sketch"]; e.State != "draft" || e.Theme != nil {
		t.Errorf("Sketch = %+v, want a draft", e)
	}
	if e := es["Aurora"]; e.State != "theme" || e.Theme.CanReopen {
		t.Errorf("Aurora = %+v, want a theme that cannot be reopened", e)
	}
	if len(entriesOf(t, s, "flights")) != 0 {
		t.Error("the clock's entries listed under flights")
	}
	// the newest design: the draft over the theme it changes; the theme's when there is no draft
	if rec := rawCall(t, s, http.MethodGet, "/api/library/design?app=clock&name=Tourbillon", nil); !strings.Contains(rec.Body.String(), `"v1.2.16"`) {
		t.Errorf("Tourbillon opened at %.120s, want its draft", rec.Body)
	}
	if rec := rawCall(t, s, http.MethodGet, "/api/library/design?app=clock&name=Tourbillon&from=theme", nil); !strings.Contains(rec.Body.String(), `"v1.2.12"`) {
		t.Errorf("from=theme opened %.120s, want the theme's design beside the draft", rec.Body)
	}
	// a rebake (fold=0) replaces the baked half and leaves the unsent changes standing
	rebaked := withDesign(orbfake.Container("Tourbillon"), string(designDoc("Tourbillon", "v1.2.16", 2)))
	if env := designCall(t, s, http.MethodPost, "/api/library/keep?app=clock&fold=0", rebaked); env["ok"] != true {
		t.Fatalf("keep fold=0: %v", env)
	}
	if e := entriesOf(t, s, "clock")["Tourbillon"]; e.State != "changed" || e.Theme.MadeWith != "v1.2.16" {
		t.Errorf("Tourbillon = %+v after a rebake, want the new theme with its changes still beside it", e)
	}
	if env := designCall(t, s, http.MethodPost, "/api/library/forget?app=clock&name=Tourbillon&what=draft", nil); env["ok"] != true {
		t.Fatalf("discard: %v", env)
	}
	if rec := rawCall(t, s, http.MethodGet, "/api/library/design?app=clock&name=Tourbillon", nil); !strings.Contains(rec.Body.String(), `base64,AA"`) {
		t.Errorf("Tourbillon opened at %.200s after discarding its changes, want the (rebaked) theme's design", rec.Body)
	}
	if e := entriesOf(t, s, "clock")["Tourbillon"]; e.State != "theme" {
		t.Errorf("Tourbillon = %+v after Discard changes, want the theme alone", e)
	}
	if env := designCall(t, s, http.MethodGet, "/api/library/design?app=clock&name=Aurora", nil); env["ok"] == true {
		t.Error("a theme with no design answered one")
	}
	if env := designCall(t, s, http.MethodPost, "/api/library/forget?app=clock&name=Tourbillon&what=all", nil); env["ok"] != true {
		t.Fatalf("forget: %v", env)
	}
	if _, err := os.Stat(filepath.Join(s.lib.ThemesDir, "tourbillon.orbtheme")); err == nil {
		t.Error("forgetting the entry left its shelf file")
	}
	if es := entriesOf(t, s, "clock"); len(es) != 2 {
		t.Errorf("entries after forget = %v, want Sketch and Aurora", es)
	}
}

// K10: keeping a theme folds its draft away, and replaces whatever shelf file held the
// name, a stale bake included; a stale theme is listed as needing a rebake.
func TestKeepFoldsTheDraftAndReplacesAStaleBake(t *testing.T) {
	s, _, root := libHub(t)
	stale := withABI(withDesign(orbfake.Container("Tourbillon"), string(designDoc("Tourbillon", "v1.2.10", 1))), 5)
	if _, err := orbnet.ContainerName(stale); err == nil {
		t.Fatal("the restamped bake still parses; the test needs a stale one")
	}
	if err := os.WriteFile(filepath.Join(s.lib.ThemesDir, "old-name.orbtheme"), stale, 0o644); err != nil {
		t.Fatal(err)
	}
	e := entriesOf(t, s, "clock")["Tourbillon"]
	if e.Theme == nil || e.Theme.Stale == "" || !e.Theme.CanReopen || e.Theme.MadeWith != "v1.2.10" {
		t.Fatalf("Tourbillon = %+v, want a stale theme whose design survives", e)
	}
	if rec := rawCall(t, s, http.MethodGet, "/api/library/design?app=clock&name=Tourbillon", nil); !strings.Contains(rec.Body.String(), `"v1.2.10"`) {
		t.Errorf("a stale theme's design = %.120s, want it readable for the rebake", rec.Body)
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=Tourbillon", designDoc("Tourbillon", "v1.2.16", 1)); env["ok"] != true {
		t.Fatal(env)
	}
	fresh := withDesign(orbfake.Container("Tourbillon"), string(designDoc("Tourbillon", "v1.2.16", 1)))
	if env := designCall(t, s, http.MethodPost, "/api/library/keep?app=flights", fresh); env["ok"] == true {
		t.Error("the clock's theme was kept under flights")
	}
	if env := designCall(t, s, http.MethodPost, "/api/library/keep?app=clock", fresh); env["ok"] != true {
		t.Fatalf("keep: %v", env)
	}
	files, _ := os.ReadDir(s.lib.ThemesDir)
	if len(files) != 1 || files[0].Name() != "old-name.orbtheme" {
		t.Errorf("shelf = %v, want the stale file replaced in place", files)
	}
	if b, _ := os.ReadFile(filepath.Join(s.lib.ThemesDir, "old-name.orbtheme")); !bytes.Equal(b, fresh) {
		t.Error("the stale file does not hold the fresh bake")
	}
	if e := entriesOf(t, s, "clock")["Tourbillon"]; e.State != "theme" || e.Theme.Stale != "" {
		t.Errorf("Tourbillon = %+v, want one current theme, its draft folded", e)
	}
	if d, _ := os.ReadDir(filepath.Join(root, "designs", "clock")); len(d) != 0 {
		t.Errorf("drafts = %v, want the draft folded into the theme", d)
	}
}

// K9: a theme the hub stores on an orb is kept on the shelf, filed by app and name, so a
// re-send of the same name replaces its copy rather than adding one.
func TestStoredThemesAreKeptOnTheShelfOnePerName(t *testing.T) {
	s, ip, _, _ := fakeOrbHub(t)
	dir := t.TempDir()
	s.lib = &Library{AppsDir: dir, ThemesDir: dir}
	first := orbfake.Container("Tourbillon")
	if err := os.WriteFile(filepath.Join(dir, "hand-named.orbtheme"), first, 0o644); err != nil {
		t.Fatal(err)
	}
	other := orbfake.Container("Aurora")
	if env := designCall(t, s, http.MethodPost, "/api/theme/upload?ip="+ip, other); env["ok"] != true {
		t.Fatalf("upload: %v", env)
	}
	if _, err := s.device.ThemeUpload(ip, first, false); err != nil {
		t.Fatal(err)
	}
	second := orbfake.Container("Tourbillon")
	second[200] ^= 0xFF
	env := designCall(t, s, http.MethodPost, "/api/theme/upload?ip="+ip, second)
	tok, _ := env["commit"].(string)
	if env["duplicate"] != true || tok == "" {
		t.Fatalf("env = %v, want the duplicate refusal and its token", env)
	}
	if env := designCall(t, s, http.MethodPost, "/api/theme/commit", []byte(`{"ip":"`+ip+`","commit":"`+tok+`"}`)); env["ok"] != true {
		t.Fatalf("commit: %v", env)
	}
	files, _ := os.ReadDir(dir)
	names := map[string][]byte{}
	for _, f := range files {
		b, _ := os.ReadFile(filepath.Join(dir, f.Name()))
		c, err := orbnet.ContainerName(b)
		if err != nil {
			t.Fatalf("%s: %v", f.Name(), err)
		}
		if _, dup := names[c.Name]; dup {
			t.Errorf("two shelf files hold %q", c.Name)
		}
		names[c.Name] = b
	}
	if len(names) != 2 || names["Aurora"] == nil {
		t.Errorf("shelf = %d themes, want Tourbillon and the sent Aurora", len(names))
	}
	if !bytes.Equal(names["Tourbillon"], second) {
		t.Error("the committed replacement did not replace the shelf's Tourbillon")
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "hand-named.orbtheme")); !bytes.Equal(b, second) {
		t.Error("the replacement was not written over the file that already held the name")
	}
}
