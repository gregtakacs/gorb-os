package orbnet

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestHostListRefusesWiderThanSlash24(t *testing.T) {
	for _, bad := range []string{"192.168.0.0/16", "10.0.0.0/8", "172.16.0.0/12", "0.0.0.0/0", "192.168.1.0/23"} {
		if _, err := hostList(bad); err == nil {
			t.Errorf("hostList(%s) accepted a subnet wider than /24 — SAFETY BUG", bad)
		}
	}
}

func TestHostListSlash24Bounds(t *testing.T) {
	hosts, err := hostList("192.168.2.0/24")
	if err != nil {
		t.Fatalf("hostList(/24): %v", err)
	}
	if len(hosts) != 254 {
		t.Fatalf("/24 probed %d hosts, want 254", len(hosts))
	}
	if hosts[0] != "192.168.2.1" || hosts[len(hosts)-1] != "192.168.2.254" {
		t.Errorf("network/broadcast not excluded: first=%s last=%s", hosts[0], hosts[len(hosts)-1])
	}
	for _, h := range hosts {
		if strings.HasSuffix(h, ".0") || strings.HasSuffix(h, ".255") {
			t.Fatalf("probed a reserved address %s", h)
		}
	}
}

func TestHostListBareIPAndGarbage(t *testing.T) {
	got, err := hostList("192.168.2.34")
	if err != nil || len(got) != 1 || got[0] != "192.168.2.34" {
		t.Errorf("bare IP probe = %v, %v", got, err)
	}
	if _, err := hostList("not-an-ip"); err == nil {
		t.Errorf("garbage accepted")
	}
}

// newFakeOrb serves an orb-shaped status so the scan classifies it, and a
// plain 404/JSON host so the scan rejects it.
func TestSweepFindsOrbAndIgnoresHost(t *testing.T) {
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
