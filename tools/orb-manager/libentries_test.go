package main

import (
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// fu2-4 (ruling 35): the basic-character rule is newer than some names a hub already holds.
// Such an entry lists, opens and forgets under its old name — no door but the create-name one
// reads characters — and a save under the old name does not hard-fail, because a name the
// library holds exactly is not a new one. The studio's rename prompt posts the renamed save
// through that same create-name door: here is the hub's side of it, with the rule unloosened.
func TestAHeldNameRenamedThroughTheNormalDoor(t *testing.T) {
	s, _, _ := libHub(t)
	const held = "café ✨"
	if err := os.WriteFile(filepath.Join(s.lib.ThemesDir, "cafe-.orbtheme"), orbfake.Container(held), 0o644); err != nil {
		t.Fatal(err)
	}
	q := url.QueryEscape(held)
	e := entriesOf(t, s, "clock")[held]
	if e.Theme == nil {
		t.Fatalf("the shelf's non-basic name is not listed: %+v", e)
	}
	// a save under the held name is the ordinary replace, not the character refusal
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name="+q, designDoc(held, "v1.2.16", 1)); env["ok"] != true {
		t.Errorf("save under the held name: %v", env["error"])
	}
	// the entry opens under the old name
	rec := rawCall(t, s, http.MethodGet, "/api/library/design?app=clock&name="+q, nil)
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "wc-design") {
		t.Errorf("open under the held name: %d %s", rec.Code, rec.Body)
	}
	// the prompt's cleaned suggestion is a plain new name: it passes checkNewName like any Save As
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name=cafe", designDoc("cafe", "v1.2.16", 1)); env["ok"] != true {
		t.Fatalf("save under the cleaned name: %v", env["error"])
	}
	all := entriesOf(t, s, "clock")
	if all["cafe"].Draft == nil || all[held].Theme == nil {
		t.Errorf("entries = %v, want the new draft beside the old entry", all)
	}
	// and the old entry still forgets under its own name
	if env := designCall(t, s, http.MethodPost, "/api/library/forget?app=clock&name="+q+"&what=all", nil); env["ok"] != true {
		t.Errorf("forget under the held name: %v", env["error"])
	}
	if e := entriesOf(t, s, "clock")[held]; e.Name != "" {
		t.Errorf("the forgotten entry is still listed: %+v", e)
	}
	// the rule itself did not move: the same bad name, no longer held, is refused again
	if env := designCall(t, s, http.MethodPost, "/api/design/save?app=clock&name="+q, designDoc(held, "v1.2.16", 1)); env["ok"] == true || env["error"] != themeNameRefusalChars {
		t.Errorf("save under a name the library no longer holds: %v, want the character sentence", env["error"])
	}
}
