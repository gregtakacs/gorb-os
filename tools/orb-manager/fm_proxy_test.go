// fm_proxy_test.go — the v1.1.18 doors as one lifecycle through the proxy.
//
// The fake answers these; the proxy must forward them — and the write must
// STREAM (the hub's own 8 MB buffer cap must not be what stops a 20 MB upload;
// here that is proven by the streamed body arriving whole with a declared
// length, not by weighing hub RAM). The factory door must arrive with its
// confirm word intact, because a proxy that lost the query would silently
// turn a typed-ALL reset into a permanent refusal.
package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

func TestProxyFileLifecycleOnTheFake(t *testing.T) {
	backend := httptest.NewServer(orbfake.New())
	defer backend.Close()
	s := proxyWith(t, backend)
	call := func(method, url, body string, cl int64) (int, map[string]any) {
		t.Helper()
		req := httptest.NewRequest(method, "/orb/"+testOrbIP+url, strings.NewReader(body))
		if cl >= 0 {
			req.ContentLength = cl
		}
		rr := httptest.NewRecorder()
		s.proxyOrb(rr, req)
		var j map[string]any
		_ = json.Unmarshal(rr.Body.Bytes(), &j)
		return rr.Code, j
	}

	// A fresh card is never truly empty: orb_sd_init creates /sd/apps and
	// /sd/themes at mount, and the double seeds the same two. An assertion of zero
	// rows here would describe a volume no real orb boots with.
	if code, j := call("GET", "/api/v1/sd/ls", "", -1); code != 200 || len(j["entries"].([]any)) != 2 {
		t.Fatalf("fresh root: code=%d body=%v want the two store directories", code, j)
	}
	if code, _ := call("POST", "/api/v1/sd/mkdir?path=photos/2026", "{}", 2); code != 200 {
		t.Fatalf("mkdir: code=%d", code)
	}
	// The streamed write: declared length, octet-stream, body through the hub.
	body := strings.Repeat("x", 5000)
	code, j := call("POST", "/api/v1/sd/write?path=photos/2026/a.txt", body, int64(len(body)))
	if code != 200 || j["bytes"] != float64(len(body)) {
		t.Fatalf("write: code=%d body=%v want 200/5000 landed", code, j)
	}
	if code, j := call("POST", "/api/v1/sd/write?path=photos/2026/a.txt", "y", 1); code != 409 {
		t.Errorf("overwrite refused without the word: code=%d body=%v, want 409", code, j)
	}
	code, _ = call("POST", "/api/v1/sd/write?path=photos/2026/a.txt&overwrite=1", "z", 1)
	if code != 200 {
		t.Errorf("overwrite=1: code=%d, want 200", code)
	}
	// A push into apps/ lands even when nothing created that directory first: the
	// store dirs are deletable from the FM and the device re-creates them before the
	// write, which is the invariant that bought the guard's removal its safety. The
	// hub must not grow its own policy on top — 403 here is still the failure shape.
	if code, _ := call("POST", "/api/v1/sd/write?path=apps%2Fpushed.orbapp", "z", 1); code != 200 {
		t.Errorf("push into apps/: code=%d, want 200 — the device rebuilds the store dir", code)
	}
	if code, _ := call("POST", "/api/v1/sd/rm?path=apps", "{}", 2); code != 409 {
		t.Errorf("rm non-empty apps/: code=%d, want 409 — deletable, but not by silently eating contents", code)
	}
	if code, _ := call("POST", "/api/v1/sd/rm?path=apps&recursive=1", "{}", 2); code != 200 {
		t.Errorf("rm -r apps/: code=%d, want 200 — the whole tree is the owner's to clear", code)
	}
	// And the self-heal has to actually hold: the next push lands without a reboot.
	if code, _ := call("POST", "/api/v1/sd/write?path=apps%2Fagain.orbapp", "z", 1); code != 200 {
		t.Errorf("push after deleting apps/: code=%d, want 200 — orb_sd_ensure_store_dirs is the only thing that makes the delete safe", code)
	}
	if code, _ := call("POST", "/api/v1/sd/write?path=nodir%2Ff.txt", "z", 1); code != 404 {
		t.Errorf("missing parent: code=%d, want the device's 404 forwarded — the self-heal is the two store dirs, not every directory", code)
	}
	if code, _ := call("POST", "/api/v1/sd/rm?path=photos", "{}", 2); code != 409 {
		t.Fatalf("rm non-empty: code=%d, want 409 forwarded", code)
	}
	code, j = call("POST", "/api/v1/sd/rm?path=photos&recursive=1", "{}", 2)
	if code != 200 || j["removed"].(float64) < 3 {
		t.Fatalf("rm recursive: code=%d removed=%v, want the tree counted", code, j["removed"])
	}
	// Back to the two directories the device owns: the test's own tree is gone and
	// nothing the owner did not ask for was taken with it.
	if code, j := call("GET", "/api/v1/sd/ls", "", -1); code != 200 || len(j["entries"].([]any)) != 2 {
		t.Fatalf("root after rm: code=%d entries=%v, want just the two store dirs again",
			code, j["entries"])
	}
}

// TestProxyDeleteOfANameTheDeviceItselfWouldNotWrite pins the two card paths the
// owner actually hit. The first is a directory a Windows host wrote: the page lists
// it, so the page must be able to delete it (D39 made the FM segment rule one rule
// rather than two). The second is a delete two levels deep, whose separator the
// browser necessarily sends as %2F — the firmware refused that escape at decode time
// and reported "path is missing" for a parameter that arrived whole. The fake reads
// its query with Go's stdlib and always decoded %2F correctly, which is exactly why
// the hub suite was green while the dial was broken: keep these rows here so the
// contract the fake claims stays the contract the device keeps.
func TestProxyDeleteOfANameTheDeviceItselfWouldNotWrite(t *testing.T) {
	backend := httptest.NewServer(orbfake.New())
	defer backend.Close()
	s := proxyWith(t, backend)
	call := func(method, urlPath, body string) (int, map[string]any) {
		t.Helper()
		req := httptest.NewRequest(method, "/orb/"+testOrbIP+urlPath, strings.NewReader(body))
		req.ContentLength = int64(len(body))
		rr := httptest.NewRecorder()
		s.proxyOrb(rr, req)
		var j map[string]any
		_ = json.Unmarshal(rr.Body.Bytes(), &j)
		return rr.Code, j
	}

	const win = "System Volume Information"
	if code, _ := call("POST", "/api/v1/sd/mkdir?path="+url.QueryEscape(win), "{}"); code != 200 {
		t.Fatalf("mkdir %q: code=%d, want 200 — a listed name must be writable too", win, code)
	}
	if code, _ := call("POST", "/api/v1/sd/rm?path="+url.QueryEscape(win), "{}"); code != 200 {
		t.Fatalf("rm %q: code=%d, want 200 — the listable-is-deletable asymmetry is back", win, code)
	}

	// Nested, spelled the way the browser spells it.
	if code, _ := call("POST", "/api/v1/sd/mkdir?path=photos%2F2026%2FJune", "{}"); code != 200 {
		t.Fatalf("nested mkdir: code=%d, want 200", code)
	}
	if code, j := call("POST", "/api/v1/sd/write?path=photos%2F2026%2FJune%2FIMG_0001.jpg", "x"); code != 200 {
		t.Fatalf("nested write: code=%d body=%v, want 200", code, j)
	}
	if code, _ := call("GET", "/api/v1/sd/ls?path=photos%2F2026", ""); code != 200 {
		t.Fatalf("nested ls two levels down: code=%d, want 200", code)
	}
	if code, j := call("POST", "/api/v1/sd/rm?path=photos%2F2026%2FJune%2FIMG_0001.jpg", "{}"); code != 200 {
		t.Fatalf("nested rm: code=%d body=%v, want 200 — an encoded separator must reach the path rule", code, j)
	}
	// Traversal still refuses, and refuses AS a path rather than as a missing one.
	if code, j := call("POST", "/api/v1/sd/rm?path=%2E%2E%2Fetc%2Fpasswd", "{}"); code == 200 {
		t.Fatalf("traversal rm returned 200: %v", j)
	} else if msg, _ := j["error"].(string); strings.Contains(msg, "missing") {
		t.Errorf("traversal refused with %q: want the path rule's sentence, not a denial that ?path= arrived", msg)
	}
	// The store trees are the owner's to clear. v1.1.26 refused them and the owner
	// hit that wall the same day; one rule now governs every row the page lists. An
	// empty themes/ deletes clean, and the next upload rebuilds it.
	if code, _ := call("POST", "/api/v1/sd/rm?path=themes", "{}"); code != 200 {
		t.Errorf("rm themes: code=%d, want 200 — the directory is deletable when empty", code)
	}
	if code, _ := call("POST", "/api/v1/sd/write?path=themes%2Frestyled.orbtheme", "z"); code != 200 {
		t.Errorf("upload after deleting themes/: code=%d, want 200 — orb_sd_ensure_store_dirs is what makes the delete safe", code)
	}
}

func TestProxyFactoryDoorCarriesItsWord(t *testing.T) {
	backend := httptest.NewServer(orbfake.New())
	defer backend.Close()
	s := proxyWith(t, backend)
	rr := httptest.NewRecorder()
	s.proxyOrb(rr, httptest.NewRequest(http.MethodPost, "/orb/"+testOrbIP+"/api/v1/factory", strings.NewReader("{}")))
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("factory without ALL: code=%d, want the device's 400 forwarded", rr.Code)
	}
	rr = httptest.NewRecorder()
	s.proxyOrb(rr, httptest.NewRequest(http.MethodPost, "/orb/"+testOrbIP+"/api/v1/factory?confirm=ALL", strings.NewReader("{}")))
	body, _ := io.ReadAll(io.LimitReader(rr.Body, 4096))
	var j map[string]any
	if err := json.Unmarshal(body, &j); err != nil || j["wipe"] != "ok" {
		t.Fatalf("factory with ALL: body=%s, want the reset ack", body)
	}
}
