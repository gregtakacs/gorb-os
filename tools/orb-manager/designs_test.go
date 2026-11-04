package main

import (
	"bytes"
	"encoding/json"
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
	mux := http.NewServeMux()
	s.routes(mux)
	req := httptest.NewRequest(method, path, bytes.NewReader(body))
	req.Header.Set(tokenHeader, s.token)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	var env map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("%s %s: %d %s", method, path, rec.Code, rec.Body)
	}
	return env
}

// K9: My designs are files on the hub, one per name, art and all, and only this app's.
func TestDesignsAreFilesOnTheHubOnePerName(t *testing.T) {
	s, _, _, _ := fakeOrbHub(t)
	root := t.TempDir()
	s.lib = &Library{AppsDir: filepath.Join(root, "apps"), ThemesDir: filepath.Join(root, "themes")}
	if err := os.MkdirAll(s.lib.ThemesDir, 0o755); err != nil {
		t.Fatal(err)
	}
	doc := func(size int) []byte {
		return []byte(`{"format":"wc-design","schema":1,"app":"clock","appVersion":"v1.2.16","made":"2026-11-04T00:00:00Z",` +
			`"dial":"skeleton","assets":[{"id":"bg-1","dataUrl":"data:image/png;base64,` + strings.Repeat("A", size) + `"}],` +
			`"saved":{"name":"ignored","at":1,"extra":{"dial":"skeleton"}}}`)
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=Big%20sub", doc(10)); env["ok"] != true {
		t.Fatalf("save: %v", env)
	}
	// a second save under the same name replaces the first: one file, the newer bytes
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=Big%20sub", doc(9000)); env["ok"] != true {
		t.Fatalf("resave: %v", env)
	}
	dir := filepath.Join(root, "designs", "clock")
	files, _ := os.ReadDir(dir)
	if len(files) != 1 {
		t.Fatalf("%d files in %s after two saves under one name, want one", len(files), dir)
	}
	env := designCall(t, s, http.MethodGet, "/api/design/list?app=clock", nil)
	rows, _ := env["data"].([]any)
	if len(rows) != 1 {
		t.Fatalf("list = %v, want one row", env)
	}
	row := rows[0].(map[string]any)
	if row["name"] != "Big sub" || row["appVersion"] != "v1.2.16" || row["bytes"].(float64) < 9000 {
		t.Errorf("row = %v, want the name the save gave, the envelope's release and the art's bytes", row)
	}
	if ex, _ := row["extra"].(map[string]any); ex["dial"] != "skeleton" {
		t.Errorf("row extra = %v, want the editor's listing extras kept", row["extra"])
	}
	// the file reads back as the design, art inside, with the hub's name in "saved"
	mux := http.NewServeMux()
	s.routes(mux)
	req := httptest.NewRequest(http.MethodGet, "/api/design/get?app=clock&name=Big%20sub", nil)
	req.Header.Set(tokenHeader, s.token)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)
	var got map[string]any
	if json.Unmarshal(rec.Body.Bytes(), &got) != nil || got["format"] != "wc-design" || !strings.Contains(rec.Body.String(), strings.Repeat("A", 9000)) {
		t.Fatalf("get = %.200s, want the saved document with its art", rec.Body)
	}
	if sv, _ := got["saved"].(map[string]any); sv["name"] != "Big sub" {
		t.Errorf("saved block = %v, want the hub's own name, not the body's", got["saved"])
	}
	// refusals: another app's design, a non-design, a name the folder would not hold
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=flights&name=x", doc(1)); env["ok"] == true {
		t.Error("the clock's design was filed under flights")
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=x", []byte(`{"format":"zip"}`)); env["ok"] == true {
		t.Error("a document that is not a design was filed")
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=..%2Fx&name=x", doc(1)); env["ok"] == true {
		t.Error("an app slug with a path in it was accepted")
	}
	// a name with a path in it is only a name: the file stays in the app's folder
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=..%2F..%2Fescape", doc(1)); env["ok"] != true {
		t.Fatalf("save: %v", env)
	}
	if _, err := os.Stat(filepath.Join(root, "escape.orbdesign")); err == nil {
		t.Error("a name walked out of the designs folder")
	}
	if files, _ := os.ReadDir(dir); len(files) != 2 {
		t.Errorf("%d files, want the two names in the app's own folder", len(files))
	}
	if env := designCall(t, s, http.MethodPost, "/api/design/delete?app=clock&name=Big%20sub", nil); env["ok"] != true {
		t.Fatalf("delete: %v", env)
	}
	env = designCall(t, s, http.MethodGet, "/api/design/list?app=clock", nil)
	if rows, _ := env["data"].([]any); len(rows) != 1 {
		t.Errorf("list after delete = %v, want only the other name", env)
	}
}

// K9: a theme the hub stores on an orb is kept on the shelf, filed by app and name, so a
// re-send of the same name replaces its copy rather than adding one.
func TestStoredThemesAreKeptOnTheShelfOnePerName(t *testing.T) {
	s, ip, _, _ := fakeOrbHub(t)
	dir := t.TempDir()
	s.lib = &Library{AppsDir: dir, ThemesDir: dir}
	// a shelf file already holding this name under another file name is the one replaced
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
