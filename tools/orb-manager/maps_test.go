package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// The map tool's forwarder (maps.go, radar-plan §2.4 and §0 ruling 10): the hub asks a
// tile host as gorb-os, and only the host the Map tiles row names or the tile hosts its
// TileJSON lists.

type tileHosts struct {
	src, tiles *httptest.Server
	ua         atomic.Value
	inFlight   int32
	peak       int32
	slow       time.Duration
}

func newTileHosts(t *testing.T) *tileHosts {
	h := &tileHosts{}
	h.tiles = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h.ua.Store(r.Header.Get("User-Agent"))
		n := atomic.AddInt32(&h.inFlight, 1)
		for {
			p := atomic.LoadInt32(&h.peak)
			if n <= p || atomic.CompareAndSwapInt32(&h.peak, p, n) {
				break
			}
		}
		time.Sleep(h.slow)
		atomic.AddInt32(&h.inFlight, -1)
		w.Header().Set("Content-Type", "application/x-protobuf")
		_, _ = w.Write([]byte("tile " + r.URL.Path))
	}))
	h.src = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h.ua.Store(r.Header.Get("User-Agent"))
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"tiles":["` + h.tiles.URL + `/planet/2026/{z}/{x}/{y}.pbf"]}`))
	}))
	t.Cleanup(func() { h.src.Close(); h.tiles.Close() })
	return h
}

func mapsFetch(t *testing.T, s *Server, u string) *httptest.ResponseRecorder {
	t.Helper()
	return rawCall(t, s, http.MethodGet, "/api/maps/fetch?u="+url.QueryEscape(u), nil)
}

func mapsSource(t *testing.T, s *Server, src string) map[string]any {
	t.Helper()
	body, _ := json.Marshal(map[string]string{"tilejson": src})
	return designCall(t, s, http.MethodPost, "/api/maps/source", body)
}

func TestMapsSourceDefaultsToOpenFreeMapAndPersists(t *testing.T) {
	path := filepath.Join(t.TempDir(), "state.json")
	s := newHub()
	s.statePath = path
	env := designCall(t, s, http.MethodGet, "/api/maps/source", nil)
	d := env["data"].(map[string]any)
	if d["tilejson"] != defaultMapTiles || d["default"] != defaultMapTiles {
		t.Fatalf("a fresh hub's source: %v", d)
	}
	if ua := d["userAgent"].(string); !strings.HasPrefix(ua, "gorb-os/") || !strings.Contains(ua, "github.com/gregtakacs/gorb-os") {
		t.Errorf("the User-Agent names the app and its home: %q", ua)
	}
	if env := mapsSource(t, s, "ftp://example.com/x"); env["ok"] != false {
		t.Errorf("a non-http source is refused: %v", env)
	}
	if env := mapsSource(t, s, "https://user:pw@example.com/planet"); env["ok"] != false {
		t.Errorf("a source with credentials is refused: %v", env)
	}
	if env := mapsSource(t, s, "https://nas.lan/planet"); env["ok"] != true {
		t.Fatalf("a self-hosted source: %v", env)
	}
	cold := newHub()
	cold.statePath = path
	cold.loadState()
	if got := cold.maps.source(); got != "https://nas.lan/planet" {
		t.Errorf("the source survives a restart: %q", got)
	}
	if env := mapsSource(t, cold, ""); env["data"].(map[string]any)["tilejson"] != defaultMapTiles || cold.maps.stored() != "" {
		t.Errorf("an empty source is the default again, and the state file keeps nothing for it")
	}
}

func TestMapsFetchIsNotAnOpenProxy(t *testing.T) {
	h := newTileHosts(t)
	s := newHub()
	other := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write([]byte("no")) }))
	defer other.Close()
	srcURL := h.src.URL + "/planet"
	if env := mapsSource(t, s, srcURL); env["ok"] != true {
		t.Fatalf("set source: %v", env)
	}
	tile := h.tiles.URL + "/planet/2026/9/117/211.pbf"
	if rec := mapsFetch(t, s, tile); rec.Code != http.StatusForbidden {
		t.Errorf("a tile host is not fetched before the TileJSON naming it was read: %d", rec.Code)
	}
	rec := mapsFetch(t, s, srcURL)
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"tiles"`) {
		t.Fatalf("the TileJSON comes back untouched: %d %s", rec.Code, rec.Body)
	}
	if ua, _ := h.ua.Load().(string); ua != mapsUserAgent() {
		t.Errorf("the hub asks as gorb-os: %q", ua)
	}
	rec = mapsFetch(t, s, tile)
	if rec.Code != http.StatusOK || rec.Body.String() != "tile /planet/2026/9/117/211.pbf" ||
		rec.Header().Get("Content-Type") != "application/x-protobuf" {
		t.Errorf("a tile the TileJSON names: %d %q %q", rec.Code, rec.Body, rec.Header().Get("Content-Type"))
	}
	if rec := mapsFetch(t, s, other.URL+"/anything"); rec.Code != http.StatusForbidden {
		t.Errorf("any other host is refused: %d", rec.Code)
	}
	if rec := mapsFetch(t, s, "file:///etc/passwd"); rec.Code != http.StatusBadRequest {
		t.Errorf("a non-http URL is refused: %d", rec.Code)
	}
	mapsSource(t, s, "https://nas.lan/planet")
	if rec := mapsFetch(t, s, tile); rec.Code != http.StatusForbidden {
		t.Errorf("a new source forgets the old one's tile hosts: %d", rec.Code)
	}
	req := httptest.NewRequest(http.MethodGet, "/api/maps/fetch?u="+url.QueryEscape(srcURL), nil)
	rr := httptest.NewRecorder()
	mux := http.NewServeMux()
	s.routes(mux)
	mux.ServeHTTP(rr, req)
	if rr.Code == http.StatusOK {
		t.Errorf("the forwarder wants the launch token like every control route")
	}
}

func TestMapsFetchHoldsFourInFlight(t *testing.T) {
	h := newTileHosts(t)
	h.slow = 60 * time.Millisecond
	s := newHub()
	mapsSource(t, s, h.src.URL+"/planet")
	mapsFetch(t, s, h.src.URL+"/planet")
	atomic.StoreInt32(&h.peak, 0)
	var wg sync.WaitGroup
	for i := 0; i < 12; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			if rec := mapsFetch(t, s, h.tiles.URL+"/t/"+string(rune('a'+i))); rec.Code != http.StatusOK {
				t.Errorf("tile %d: %d", i, rec.Code)
			}
		}(i)
	}
	wg.Wait()
	if p := atomic.LoadInt32(&h.peak); p > mapsInFlight || p < 2 {
		t.Errorf("twelve asks at once reach the tile host at most %d at a time (and in parallel): peak %d", mapsInFlight, p)
	}
}
