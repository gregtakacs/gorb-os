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

// The MAC is the fleet's join key, so these pin the two properties everything
// downstream depends on: the BASE MAC the firmware reports survives the decode
// verbatim, and an orb that reported none stays empty rather than becoming a
// value that could match some other orb.
func TestStatusCarriesTheBaseMAC(t *testing.T) {
	// Shaped like the real h_status body: 12 lowercase hex digits, no separators.
	body := `{"state":"connected","ip":"192.168.1.50","mac":"44bd8d608088"` +
		`,"rssi":-52,"fw":"8eb4bf0-dirty","idf":"v5.4","slot":"B","img":"valid"}`
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write([]byte(body))
	}))
	defer srv.Close()

	s, classified, retryable, err := NewClient(500 * time.Millisecond).orbStatusOnce(strings.TrimPrefix(srv.URL, "http://"))
	if !classified || retryable || err != nil {
		t.Fatalf("orbStatusOnce: classified=%v retryable=%v err=%v", classified, retryable, err)
	}
	if s.MAC != "44bd8d608088" {
		t.Errorf("MAC = %q, want the base MAC exactly as the device sent it", s.MAC)
	}
	// The neighbour is checked with it, because both are strings in the same
	// position of the same object: a tag pointing at the wrong key is the mistake
	// this field would most quietly make, and it would look like an orb list full
	// of IPs.
	if s.IP != "192.168.1.50" {
		t.Errorf("IP = %q, want the lease address kept separate from the MAC", s.IP)
	}
	if s.Slot != "B" || s.FW == "" {
		t.Errorf("the orb-shape fields regressed: %+v", s)
	}
}

// Firmware that cannot read its MAC, or firmware predating the field, must leave
// it empty. A default that looked like any other string would let the fleet store
// bind two different orbs together.
func TestStatusWithoutMACLeavesItEmpty(t *testing.T) {
	var s Status
	if !classifyOrbJSON([]byte(`{"slot":"A","fw":"1.0"}`), &s) {
		t.Fatal("an orb-shaped body without a mac was not classified as an orb")
	}
	if s.MAC != "" {
		t.Errorf("MAC = %q, want empty so no caller can mistake it for an identity", s.MAC)
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
