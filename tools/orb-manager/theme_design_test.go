package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// D43 P7 (§8): the Edit door's read. A card theme's design comes back as the exact bytes
// the orb streamed, checked against the size and CRC it sent ahead of them; a theme with
// none answers the device's own sentence, and the row carries the design's size so the
// page knows whom to offer Edit.
func TestTheDesignRelayReadsTheCardsDesign(t *testing.T) {
	s, ip, _, f := fakeOrbHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	id, _ := orbnet.ThemeIDOf("clock", "aurora")
	get := func(q string) *httptest.ResponseRecorder {
		rec := httptest.NewRecorder()
		req := httptest.NewRequest(http.MethodGet, "/api/theme/design?"+q, nil)
		req.Header.Set(tokenHeader, s.token)
		mux.ServeHTTP(rec, req)
		return rec
	}
	rec := get("ip=" + ip + "&id=" + id)
	var env map[string]any
	_ = json.Unmarshal(rec.Body.Bytes(), &env)
	if rec.Code == 200 || !strings.Contains(env["error"].(string), "carries no design") {
		t.Fatalf("a design-less theme answered %d %s, want the device's sentence", rec.Code, rec.Body.String())
	}
	d := []byte(`{"format":"wc-design","schema":1,"dial":"modern"}`)
	f.SetDesign(id, d)
	rec = get("ip=" + ip + "&id=" + id)
	if rec.Code != 200 || !bytes.Equal(rec.Body.Bytes(), d) {
		t.Fatalf("relay = %d %q, want 200 with the design verbatim", rec.Code, rec.Body.String())
	}
	list, err := s.device.ThemeList(ip)
	if err != nil {
		t.Fatal(err)
	}
	for _, r := range list.Themes {
		if r.ID == id && r.Design != uint32(len(d)) {
			t.Errorf("the row reports design %d, want %d", r.Design, len(d))
		}
	}
	// the shelf leg names its own refusal for a stem the library does not hold
	rec = get("name=nowhere")
	if rec.Code == 200 {
		t.Errorf("a shelf stem that is not there answered 200")
	}
}
