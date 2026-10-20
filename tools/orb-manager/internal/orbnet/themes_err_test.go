package orbnet

import (
	"net/http"
	"strings"
	"testing"
	"time"
)

// themeUploadBody is a container the client will let past its FORMAT-2 gate, for the
// tests about what the DEVICE says rather than what the hub refuses first.
func themeUploadBody(t *testing.T) []byte { return fakeContainer(t, "aurora") }

// A device refusal is the operator's only diagnosis, so the firmware's own
// {"error":...} has to come back as the message rather than as "HTTP 409".
func TestThemeErrorsCarryTheFirmwaresOwnWords(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusConflict)
		_, _ = w.Write([]byte(`{"error":"a theme job is already running"}`))
	})
	_, err := NewClient(2*time.Second).ThemeStore(fakeIP(srv), "9b89025ce7a6")
	if err == nil {
		t.Fatal("a 409 was reported as success")
	}
	if !strings.Contains(err.Error(), "a theme job is already running") {
		t.Errorf("err = %v, want the firmware's sentence in it", err)
	}
	if strings.Contains(err.Error(), "HTTP 409") {
		t.Errorf("err = %v, want the firmware's reason rather than a bare status", err)
	}
}

// h_bundle_install's 400 is the sound the store door makes when a client names a theme by
// anything but an id. Surfacing the firmware's words here is what turns "the button does
// nothing" into "the hub is speaking the wrong format".
func TestThemeStoreReportsTheFirmwaresIdRefusal(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusBadRequest)
		_, _ = w.Write([]byte(`{"error":"id is missing or not a theme id"}`))
	})
	_, err := NewClient(2*time.Second).ThemeStore(fakeIP(srv), "9b89025ce7a6")
	if err == nil || !strings.Contains(err.Error(), "not a theme id") {
		t.Errorf("err = %v, want the firmware's id refusal carried through", err)
	}
}

// A device that answers with a status page still needs to be distinguishable from one
// that answered the firmware's way, so the status survives when there is no reason.
func TestThemeErrorFallsBackToTheStatus(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNotFound)
		_, _ = w.Write([]byte(`not json at all`))
	})
	if _, err := NewClient(2 * time.Second).ThemeDefault(fakeIP(srv)); err == nil {
		t.Fatal("a 404 was reported as success")
	}
}

// Whatever else answers on port 80 — a router admin page, a captive portal — must not
// be parsed as an orb.
func TestThemeClientRefusesNonJSON(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		_, _ = w.Write([]byte("<html><body>login</body></html>"))
	})
	_, err := NewClient(2 * time.Second).ThemeList(fakeIP(srv))
	if err == nil || !strings.Contains(err.Error(), "not JSON") {
		t.Errorf("err = %v, want the not-JSON refusal", err)
	}
}

// An upload the device would refuse for size is refused here first, so a card never
// half-receives a theme it cannot load, and an empty file is not a theme either.
func TestThemeUploadBounds(t *testing.T) {
	c := NewClient(2 * time.Second)
	if _, err := c.ThemeUpload("10.0.0.1", nil, false); err == nil {
		t.Error("an empty theme file was accepted")
	}
	if _, err := c.ThemeUpload("10.0.0.1", make([]byte, MaxThemeUpload+1), false); err == nil {
		t.Error("an over-ceiling theme file was accepted")
	}
	if MaxThemeUpload != 0x00400000+0x20000 {
		t.Errorf("MaxThemeUpload = %d, must equal ORB_THEME_UPLOAD_MAX in main/art/orb_theme.h", MaxThemeUpload)
	}
}

// The firmware's duplicate refusal is a decision, not a failure: the card has to be able
// to tell "ask the operator" from "report the error", which needs the status and the
// overwrite flag to survive the round trip.
func TestThemeUploadDuplicateIsRecognisable(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusConflict)
		_, _ = w.Write([]byte(`{"error":"a different theme with that name is already on the card","overwrite":false}`))
	})
	_, err := NewClient(2*time.Second).ThemeUpload(fakeIP(srv), themeUploadBody(t), false)
	if err == nil {
		t.Fatal("the duplicate was reported as success")
	}
	de, ok := err.(*DeviceError)
	if !ok {
		t.Fatalf("err is %T, want *orbnet.DeviceError so the card can act on it", err)
	}
	if !de.OverwriteConflict() {
		t.Errorf("status %d body %s not recognised as the overwrite prompt", de.Status, string(de.Body))
	}
}

// The other 409 — an upload already running — must not be mistaken for the duplicate,
// or the card offers to destroy a theme over a request that only needs waiting.
func TestThemeUploadBusyIsNotAPromptToOverwrite(t *testing.T) {
	srv := fakeOrbServer(t, func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusConflict)
		_, _ = w.Write([]byte(`{"error":"an upload is already in progress"}`))
	})
	_, err := NewClient(2*time.Second).ThemeUpload(fakeIP(srv), themeUploadBody(t), false)
	de, ok := err.(*DeviceError)
	if !ok {
		t.Fatalf("err is %T, want *orbnet.DeviceError", err)
	}
	if de.OverwriteConflict() {
		t.Error("a busy upload read as an offer to replace a theme")
	}
}

// A dial that cannot connect must say the orb was unreachable, not return an empty
// list that the card would render as "no themes on this card".
func TestThemeListOnADeadHostIsAnError(t *testing.T) {
	// 192.0.2.1 is TEST-NET-1: unroutable, so the dial fails for certain.
	_, err := NewClient(400 * time.Millisecond).ThemeList("192.0.2.1")
	if err == nil {
		t.Fatal("a dead host produced no error")
	}
	if !strings.Contains(err.Error(), "unreachable") {
		t.Errorf("err = %v, want it to say the orb was unreachable", err)
	}
}
