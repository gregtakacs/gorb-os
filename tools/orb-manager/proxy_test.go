package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The /orb proxy is the browser's only route to an orb, and the UI parses every
// reply as the device API's JSON. A hub-side failure (bad path, unreachable
// orb) must therefore be a JSON body too — the old plaintext http.Error surfaced
// only as a cryptic "is not valid JSON" in the browser.

func TestProxyBadPathIsJSON(t *testing.T) {
	s := &Server{device: orbnet.NewClient(300 * time.Millisecond)}
	rr := httptest.NewRecorder()
	// Host is not a bare IPv4, so it is rejected before any network call.
	req := httptest.NewRequest(http.MethodGet, "/orb/10.0.0/2/api/v1/wifi/scan", nil)
	s.proxyOrb(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rr.Code)
	}
	var body map[string]string
	if err := json.Unmarshal(rr.Body.Bytes(), &body); err != nil {
		t.Fatalf("error body is not JSON (%q): %v", rr.Body.String(), err)
	}
	if body["error"] == "" {
		t.Errorf("error field empty; body = %q", rr.Body.String())
	}
}

func TestProxyUnreachableIsJSON(t *testing.T) {
	// 192.0.2.1 is TEST-NET-1 (RFC 5737), never routable, so the dial fails fast
	// and deterministically — exactly the wired-hub-can't-reach-the-AP case.
	s := &Server{device: orbnet.NewClient(400 * time.Millisecond)}
	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/orb/192.0.2.1/api/v1/wifi/scan", nil)
	s.proxyOrb(rr, req)

	if rr.Code != http.StatusBadGateway {
		t.Fatalf("status = %d, want 502", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); ct != "application/json" {
		t.Errorf("Content-Type = %q, want application/json", ct)
	}
	var body map[string]string
	if err := json.Unmarshal(rr.Body.Bytes(), &body); err != nil {
		t.Fatalf("error body is not JSON (%q): %v", rr.Body.String(), err)
	}
	if !strings.HasPrefix(body["error"], "orb unreachable:") {
		t.Errorf("error = %q, want it to start with 'orb unreachable:'", body["error"])
	}
}
