package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

// The OS maps' tool (radar-plan §2.4, R4). The page builds a place's .orbmap levels in
// the browser (web/runtime/maps@1/orbmap.js) from OpenMapTiles vector tiles, and writes
// them to the orb's card. The tiles come through here, for one reason: a tile host asks
// an offline download for a User-Agent naming the app (OpenFreeMap's terms; §0 ruling
// 10), and a page cannot set one. So the hub asks, as
//
//	gorb-os/<version> (+https://github.com/gregtakacs/gorb-os)
//
// and hands the bytes back untouched. It is not an open proxy: it answers only the
// TileJSON the Map tiles row names and the hosts that TileJSON's tile URLs point at, at
// most four requests at a time (the plan's fetch, and OpenFreeMap's ask).
//
// The source is the hub's, kept in its state file: an OpenMapTiles-schema TileJSON,
// OpenFreeMap's planet by default, and a self-hosted planet is the same tool pointed
// elsewhere.

const defaultMapTiles = "https://tiles.openfreemap.org/planet"

const (
	mapsInFlight = 4
	mapsBodyMax  = 8 << 20 // a z6 tile of a dense region is ~400 KB; a TileJSON is small
	mapsTimeout  = 30 * time.Second
)

type mapsFwd struct {
	mu     sync.Mutex
	src    string          // the TileJSON URL; "" = the default
	hosts  map[string]bool // tile hosts the last TileJSON read through here named
	sem    chan struct{}
	client *http.Client
}

func (m *mapsFwd) init() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.sem == nil {
		m.sem = make(chan struct{}, mapsInFlight)
		m.client = &http.Client{Timeout: mapsTimeout}
	}
}

func (m *mapsFwd) source() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.src == "" {
		return defaultMapTiles
	}
	return m.src
}

// stored is what the state file keeps: nothing for the default.
func (m *mapsFwd) stored() string {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.src
}

func (m *mapsFwd) set(src string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if src == defaultMapTiles {
		src = ""
	}
	if src != m.src {
		m.hosts = nil // the old source's tile hosts are not this one's
	}
	m.src = src
}

// mapsURL accepts an absolute http(s) URL with a host and no credentials.
func mapsURL(raw string) (*url.URL, error) {
	u, err := url.Parse(strings.TrimSpace(raw))
	if err != nil || (u.Scheme != "https" && u.Scheme != "http") || u.Host == "" || u.User != nil {
		return nil, errors.New("a map tiles source is an http(s) URL with a host, like " + defaultMapTiles)
	}
	return u, nil
}

// allowed says whether u may be fetched, and whether it is the TileJSON itself.
func (m *mapsFwd) allowed(u *url.URL) (ok, isSource bool) {
	src := m.source()
	su, err := url.Parse(src)
	if err != nil {
		return false, false
	}
	if u.String() == src {
		return true, true
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	return strings.EqualFold(u.Host, su.Host) || m.hosts[strings.ToLower(u.Host)], false
}

// learn records the hosts a TileJSON's tile templates name.
func (m *mapsFwd) learn(body []byte) {
	var tj struct {
		Tiles []string `json:"tiles"`
	}
	if json.Unmarshal(body, &tj) != nil {
		return
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.hosts == nil {
		m.hosts = map[string]bool{}
	}
	for _, t := range tj.Tiles {
		// a template's {z}/{x}/{y} are not URL syntax; the host is all we read
		if u, err := url.Parse(strings.NewReplacer("{", "", "}", "").Replace(t)); err == nil &&
			(u.Scheme == "https" || u.Scheme == "http") && u.Host != "" {
			m.hosts[strings.ToLower(u.Host)] = true
		}
	}
}

func mapsUserAgent() string {
	return "gorb-os/" + version + " (+https://github.com/gregtakacs/gorb-os)"
}

func (s *Server) routesMaps(mux *http.ServeMux) {
	s.maps.init()
	mux.Handle("/api/maps/source", s.requireControl(http.HandlerFunc(s.handleMapsSource)))
	mux.Handle("/api/maps/fetch", s.requireControl(http.HandlerFunc(s.handleMapsFetch)))
}

// GET answers {tilejson, default}; POST {"tilejson": URL} sets it ("" = the default).
func (s *Server) handleMapsSource(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
	case http.MethodPost:
		var in struct {
			TileJSON string `json:"tilejson"`
		}
		if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&in); err != nil {
			s.jsonErr(w, "expected {\"tilejson\": URL}")
			return
		}
		src := strings.TrimSpace(in.TileJSON)
		if src != "" {
			if _, err := mapsURL(src); err != nil {
				s.jsonErr(w, err.Error())
				return
			}
		}
		s.maps.set(src)
		s.saveState()
	default:
		http.Error(w, "GET or POST", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "data": map[string]any{
		"tilejson": s.maps.source(), "default": defaultMapTiles, "userAgent": mapsUserAgent()}})
}

// GET ?u=URL: the source's TileJSON or one of its tiles, fetched as the hub, the bytes
// and the upstream status passed back. A refusal is 403 with the reason in the body.
func (s *Server) handleMapsFetch(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET", http.StatusMethodNotAllowed)
		return
	}
	u, err := mapsURL(r.URL.Query().Get("u"))
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	ok, isSource := s.maps.allowed(u)
	if !ok {
		http.Error(w, "the hub fetches only the Map tiles source ("+s.maps.source()+") and the tile hosts it names", http.StatusForbidden)
		return
	}
	select {
	case s.maps.sem <- struct{}{}:
		defer func() { <-s.maps.sem }()
	case <-r.Context().Done():
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), mapsTimeout)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	req.Header.Set("User-Agent", mapsUserAgent())
	resp, err := s.maps.client.Do(req)
	if err != nil {
		http.Error(w, "the tile host did not answer: "+err.Error(), http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, mapsBodyMax+1))
	if err != nil {
		http.Error(w, "the tile host's answer broke off: "+err.Error(), http.StatusBadGateway)
		return
	}
	if len(body) > mapsBodyMax {
		http.Error(w, "the tile host's answer is over 8 MB", http.StatusBadGateway)
		return
	}
	if isSource && resp.StatusCode == http.StatusOK {
		s.maps.learn(body)
	}
	if ct := resp.Header.Get("Content-Type"); ct != "" {
		w.Header().Set("Content-Type", ct)
	}
	w.Header().Set("Cache-Control", "no-store") // the page keeps tiles in its own Cache Storage
	w.WriteHeader(resp.StatusCode)
	_, _ = w.Write(body)
}
