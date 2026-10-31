package orbnet

import (
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// The trace's whole job is to name what was on the wire: the path, the lane
// class, the answer and the bytes the caller actually read.
func TestTraceNamesEveryRequest(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = io.WriteString(w, `{"mounted":true}`)
	}))
	defer srv.Close()
	host := strings.TrimPrefix(srv.URL, "http://")
	ip, port := host[:strings.LastIndex(host, ":")], host[strings.LastIndex(host, ":")+1:]

	var mu sync.Mutex
	var lines []string
	SetTrace(func(format string, args ...any) {
		mu.Lock()
		lines = append(lines, fmt.Sprintf(format, args...))
		mu.Unlock()
	})
	defer SetTrace(nil)

	c := NewGatedClient(5*time.Second, NewDialGate(), ClassControl).Preempting()
	c.DevicePort = port
	_, _ = c.ThemeList(ip)

	mu.Lock()
	defer mu.Unlock()
	if len(lines) != 1 {
		t.Fatalf("one request traced as %d lines: %q", len(lines), lines)
	}
	for _, want := range []string{"GET ", "/api/v1/sd", "[control+preempt]", " 200 ", "16B"} {
		if !strings.Contains(lines[0], want) {
			t.Errorf("trace line %q does not carry %q", lines[0], want)
		}
	}
}
