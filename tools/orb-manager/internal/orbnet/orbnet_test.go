package orbnet

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// TestStatusClassifiesOrb dials one known host:port and requires the orb-shaped
// status to decode — the same single-IP confirm /api/watch does on the
// caller-supplied address. The hub no longer sweeps a subnet to find it.
func TestStatusClassifiesOrb(t *testing.T) {
	status := Status{State: "live", Slot: "A", FW: "orb 1.5.0-11"}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/status" {
			http.NotFound(w, r)
			return
		}
		_ = json.NewEncoder(w).Encode(status)
	}))
	defer srv.Close()
	ipPort := strings.TrimPrefix(srv.URL, "http://")
	c := NewClient(500 * time.Millisecond)
	// Status on the host:port form must still classify as an orb.
	s, err := c.Status(ipPort)
	if err != nil {
		t.Fatalf("Status(%s): %v", ipPort, err)
	}
	if s.Slot != "A" {
		t.Errorf("status decode wrong: %+v", s)
	}
}

// A clean 200 that is not orb-shaped must be a definitive miss, not retried —
// that is what keeps the scan's wall time bounded against JSON servers.
func TestNonOrbJSONIsDefinitiveNotRetried(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(`{"hello":"world","uptime":123}`)) // 200, not orb-shaped
	}))
	defer srv.Close()
	ipPort := strings.TrimPrefix(srv.URL, "http://")
	c := NewClient(500 * time.Millisecond)
	start := time.Now()
	if _, err := c.Status(ipPort); err == nil {
		t.Errorf("non-orb JSON classified as an orb")
	}
	if d := time.Since(start); d > 1*time.Second {
		t.Errorf("non-orb host was retried (%.0f ms) — must reject immediately", d.Seconds()*1000)
	}
}
