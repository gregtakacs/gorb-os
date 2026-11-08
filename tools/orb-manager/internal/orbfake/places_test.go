package orbfake

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// The three places beside home (radar-plan §2.1) answer the way h_places_get and
// h_places_post do: a slot keeps its number, a point without a name is named, and the
// refusals come before anything is stored.
func TestPlacesSetNameRefuseClear(t *testing.T) {
	srv := httptest.NewServer(New())
	defer srv.Close()
	type reply struct {
		State  string `json:"state"`
		Gen    uint32 `json:"gen"`
		Places []struct {
			I    int     `json:"i"`
			Set  bool    `json:"set"`
			Lat  float64 `json:"lat"`
			Name string  `json:"name"`
		} `json:"places"`
	}
	read := func(body string) reply {
		var r reply
		if err := json.Unmarshal([]byte(body), &r); err != nil {
			t.Fatalf("not JSON: %v: %s", err, body)
		}
		return r
	}
	code, body := appDo(t, srv, http.MethodGet, "/api/v1/places", nil)
	if r := read(body); code != 200 || len(r.Places) != 3 || r.Places[0].Set || r.Places[2].I != 2 {
		t.Fatalf("GET on a fresh orb -> %d %s", code, body)
	}
	code, body = appDo(t, srv, http.MethodPost, "/api/v1/places", []byte(`{"i":2,"lat":38.7223,"lon":-9.1393,"name":"Lisbon"}`))
	if r := read(body); code != 200 || r.State != "placed" || !r.Places[2].Set || r.Places[2].Name != "Lisbon" || r.Places[0].Set || r.Gen != 1 {
		t.Fatalf("set place 3 -> %d %s", code, body)
	}
	code, body = appDo(t, srv, http.MethodPost, "/api/v1/places", []byte(`{"i":0,"lat":30.27,"lon":-97.74}`))
	if r := read(body); code != 200 || r.Places[0].Name == "" || !r.Places[2].Set {
		t.Fatalf("a point without a name is named, and place 3 stays -> %d %s", code, body)
	}
	for _, bad := range []string{`{"i":3,"lat":0,"lon":0}`, `{"lat":0,"lon":0}`, `{"i":1,"lat":91,"lon":0}`,
		`{"i":1,"lat":0}`, `{"i":1,"lat":0,"lon":0,"name":"a\"b"}`} {
		if code, body := appDo(t, srv, http.MethodPost, "/api/v1/places", []byte(bad)); code != 400 {
			t.Errorf("%s -> %d %s, want a refusal", bad, code, body)
		}
	}
	code, body = appDo(t, srv, http.MethodPost, "/api/v1/places", []byte(`{"i":0,"clear":1}`))
	if r := read(body); code != 200 || r.State != "cleared" || r.Places[0].Set || !r.Places[2].Set {
		t.Fatalf("clear place 1 keeps place 3 -> %d %s", code, body)
	}
}

// A place carries its zone as OS v1.2.38's h_places answers it: one the writer named, its
// offset now and its daylight flag, else the stand-in proposal's; an unknown zone is
// refused and leaves the slot as it was.
func TestPlacesCarryTheirZone(t *testing.T) {
	o := New()
	srv := httptest.NewServer(o)
	defer srv.Close()
	type place struct {
		Name      string `json:"name"`
		Zone      string `json:"zone"`
		ZoneNamed bool   `json:"zone_named"`
		TZOffset  *int   `json:"tz_offset"`
	}
	read := func(body string) []place {
		var r struct {
			Places []place `json:"places"`
		}
		if err := json.Unmarshal([]byte(body), &r); err != nil {
			t.Fatalf("not JSON: %v: %s", err, body)
		}
		return r.Places
	}
	code, body := appDo(t, srv, http.MethodPost, "/api/v1/places", []byte(`{"i":0,"lat":38.7223,"lon":-9.1393,"name":"Lisbon","zone":"Europe/Lisbon"}`))
	p := read(body)[0]
	loc, _ := time.LoadLocation("Europe/Lisbon")
	_, want := time.Now().In(loc).Zone()
	if code != 200 || p.Zone != "Europe/Lisbon" || !p.ZoneNamed || p.TZOffset == nil || *p.TZOffset != want/60 {
		t.Fatalf("a named zone -> %d %s", code, body)
	}
	code, body = appDo(t, srv, http.MethodPost, "/api/v1/places", []byte(`{"i":1,"lat":30.27,"lon":-97.74,"name":"Austin"}`))
	if q := read(body)[1]; code != 200 || q.Zone == "" || q.ZoneNamed || q.TZOffset == nil {
		t.Fatalf("an unnamed place gets the proposal's zone -> %d %s", code, body)
	}
	if code, body := appDo(t, srv, http.MethodPost, "/api/v1/places", []byte(`{"i":0,"lat":1,"lon":1,"zone":"Mars/Olympus"}`)); code != 400 || !strings.Contains(body, "zone not in the zone table") {
		t.Fatalf("an unknown zone -> %d %s", code, body)
	}
	_, body = appDo(t, srv, http.MethodGet, "/api/v1/places", nil)
	if q := read(body)[0]; q.Name != "Lisbon" || q.Zone != "Europe/Lisbon" {
		t.Fatalf("a refused zone left the slot as it was -> %s", body)
	}
}
