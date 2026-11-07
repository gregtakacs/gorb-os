package orbfake

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
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
