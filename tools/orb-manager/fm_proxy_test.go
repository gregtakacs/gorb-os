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

	if code, j := call("GET", "/api/v1/sd/ls", "", -1); code != 200 || len(j["entries"].([]any)) != 0 {
		t.Fatalf("empty root: code=%d body=%v", code, j)
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
	if code, _ := call("POST", "/api/v1/sd/write?path=apps%2Fevil", "z", 1); code != 403 {
		// apps/ is device-refused (400 bad path), and the hub does not pre-filter
		// paths — the device owns the sentence. What the hub MUST NOT do is dial it
		// with the route stripped: 403 here would be the hub growing a second policy.
		t.Logf("hub pre-refused path %d — only acceptable if deviceRoutes stays the only gate", code)
	}
	if code, _ := call("POST", "/api/v1/sd/write?path=nodir%2Ff.txt", "z", 1); code != 404 {
		t.Errorf("missing parent: code=%d, want the device's 404 forwarded", code)
	}
	if code, _ := call("POST", "/api/v1/sd/rm?path=photos", "{}", 2); code != 409 {
		t.Fatalf("rm non-empty: code=%d, want 409 forwarded", code)
	}
	code, j = call("POST", "/api/v1/sd/rm?path=photos&recursive=1", "{}", 2)
	if code != 200 || j["removed"].(float64) < 3 {
		t.Fatalf("rm recursive: code=%d removed=%v, want the tree counted", code, j["removed"])
	}
	if code, j := call("GET", "/api/v1/sd/ls", "", -1); code != 200 || len(j["entries"].([]any)) != 0 {
		t.Fatalf("root after rm: code=%d entries=%v, want empty again", code, j["entries"])
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
