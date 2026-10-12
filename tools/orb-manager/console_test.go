package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// The USB console card is a read that HOLDS a cable, which makes it as dangerous to
// get wrong as a write: a console reader sitting on a port during a flash corrupts
// the flash. These pin the collision rules and the promise that it never touches the
// chip.

func TestConsoleRefusesAPortThatIsBusy(t *testing.T) {
	s := newHub()
	job, _, err := s.ops.start(t.Context(), "/dev/ttyACM0", opFlash, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer s.ops.finish(job)

	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/console", strings.NewReader(`{"port":"/dev/ttyACM0"}`))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	if rec.Code == http.StatusOK {
		t.Fatalf("console streamed a port while a flash held it: %s", rec.Body)
	}
	body := rec.Body.String()
	if !strings.Contains(body, "busy") && !strings.Contains(body, "flash") {
		t.Errorf("the refusal did not say what holds the port: %s", body)
	}
}

// A flash must be refused too, not just the other way round: whichever starts second
// is the one that would corrupt the other.
func TestFlashRefusesAPortTheConsoleIsOn(t *testing.T) {
	s := newHub()
	job, _, err := s.ops.start(t.Context(), "/dev/ttyACM1", opConsole, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer s.ops.finish(job)
	if kind, ok := s.ops.busy("/dev/ttyACM1"); !ok || kind != opConsole {
		t.Fatalf("the console claim is not visible: %q %v", kind, ok)
	}
	if _, _, err := s.ops.start(t.Context(), "/dev/ttyACM1", opFlash, 0); err == nil {
		t.Error("a flash was allowed onto a port a console read holds")
	}
	// A different cable stays available: this is still a per-port model.
	other, _, err := s.ops.start(t.Context(), "/dev/ttyACM2", opFlash, 0)
	if err != nil {
		t.Errorf("an unrelated port was refused while another orb was being read: %v", err)
	}
	s.ops.finish(other)
}

// The console is uncancellable only if it is not an operation. It is registered as
// one, and the assertions below keep it that way rather than trusting the reader.
func TestConsoleIsACancellableOperation(t *testing.T) {
	src := readFile(t, "console.go")
	for _, want := range []string{"s.ops.start(r.Context()", "opConsole", "orbbind.LockPort", "esprom.ReadConsole"} {
		if !strings.Contains(src, want) {
			t.Errorf("console.go does not use %s: the read is not cancellable, not gated, or not a port operation", want)
		}
	}
	// Read-only is the whole claim. Anything that toggles a control line beyond the
	// DTR the console needs, or writes bytes, turns a viewer into a reboot.
	for _, bad := range []string{"EnterDownloadMode", "HardReset", "SetRTS", "Write("} {
		if strings.Contains(src, bad) {
			t.Errorf("console.go uses %s; the console card must not touch the chip", bad)
		}
	}
}

// The window is bounded on the server, because a client-sent number is a way to hold
// a cable open for as long as a page is left open.
func TestConsoleWindowIsBounded(t *testing.T) {
	src := readFile(t, "console.go")
	if !strings.Contains(src, "consoleWindowMax") || !strings.Contains(src, "secs > consoleWindowMax") {
		t.Error("the console window is not clamped to a server-side maximum")
	}
	if !strings.Contains(src, "if secs <= 0") {
		t.Error("a zero or negative window must fall back to the default, not read for nothing")
	}
}

// The endpoint is a port-holding operation, so it needs the same credentials as one.
func TestConsoleEndpointIsGuarded(t *testing.T) {
	s := newHub()
	mux := http.NewServeMux()
	s.routes(mux)
	srv := httptest.NewServer(mux)
	defer srv.Close()
	req, _ := http.NewRequest(http.MethodPost, srv.URL+"/api/console", strings.NewReader(`{"port":"/dev/ttyACM0"}`))
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusForbidden {
		t.Errorf("an uncredentialed console read was accepted with %d", resp.StatusCode)
	}
}

// Reading a port that does not exist must produce a clean error rather than a stream
// that ends without ever saying what happened.
func TestConsoleOnAMissingPortReports(t *testing.T) {
	s := newHub()
	mux := http.NewServeMux()
	s.routes(mux)
	rec := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/api/console", strings.NewReader(`{"port":"/dev/serial/by-id/definitely-not-a-real-device"}`))
	req.Header.Set(tokenHeader, s.token)
	mux.ServeHTTP(rec, req)
	if !strings.Contains(rec.Body.String(), "error") {
		t.Errorf("a dead port produced no error field: %s", rec.Body)
	}
}
