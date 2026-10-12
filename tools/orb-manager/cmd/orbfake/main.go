// Command orbfake is a stand-in orb for end-to-end checks of the hub's theme and
// console surface. It is not a firmware simulator: it answers the routes
// main/net/orb_net.c registers, with the JSON shapes h_sd emits, so the cards can be
// exercised without an orb on a cable. It is deliberately NOT the test double inside
// the Go suite, which uses httptest; this binary is for a live hub run by a human.
//
//	orbfake -addr 127.0.0.1:8099          # the orb's httpd listens on 80, so run
//	                                      # the hub with -orb-port 8099 or run this
//	                                      # on 80 from a root shell.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"strings"
	"sync"
)

type fakeOrb struct {
	mu       sync.Mutex
	themes   []map[string]any
	active   string
	calls    []string
	uploaded map[string]int
}

const sdCard = "SDSC-8G"

func (f *fakeOrb) sdJSON() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	rows := make([]string, 0, len(f.themes))
	for _, t := range f.themes {
		b, _ := json.Marshal(t)
		rows = append(rows, string(b))
	}
	return fmt.Sprintf(`{"mounted":true,"card":%q,"uploading":false,"total_mb":7581,`+
		`"free_mb":7100,"source":"sd","active":%q,"live":%q,"card_count":%d,`+
		`"try_slug":"","try_why":"","damage_slug":"","damage_why":"",`+
		`"themes":[%s]}`, sdCard, f.active, f.active, len(f.themes), strings.Join(rows, ","))
}

func writeJSON(w http.ResponseWriter, code int, body string) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)
	_, _ = io.WriteString(w, body)
}

func main() {
	addr := flag.String("addr", "127.0.0.1:8099", "address to listen on")
	flag.Parse()

	f := &fakeOrb{active: "aurora", uploaded: map[string]int{}}
	f.themes = []map[string]any{
		{"name": "aurora", "title": "Aurora", "why": "", "bytes": 412032, "serial": 7, "ok": true, "active": true},
		{"name": "carousel", "title": "Carousel", "why": "", "bytes": 388123, "serial": 9, "ok": true, "active": false},
		{"name": "damaged", "title": "Damaged", "why": "art short", "bytes": 12, "serial": 2, "ok": false, "active": false},
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/status", func(w http.ResponseWriter, r *http.Request) {
		f.record(r)
		writeJSON(w, 200, `{"state":"time","ip":"127.0.0.1","rssi":-52,"fw":"orbfake","idf":"v5.4",`+
			`"slot":"a","img":"gorb-os","art_ready":true,"synced":true,"tz":"EST5EDT","tz_offset":300,"ap":false}`)
	})
	mux.HandleFunc("/api/v1/sd", func(w http.ResponseWriter, r *http.Request) {
		f.record(r)
		writeJSON(w, 200, f.sdJSON())
	})
	mux.HandleFunc("/api/v1/sd/install", func(w http.ResponseWriter, r *http.Request) {
		f.record(r)
		name := r.URL.Query().Get("name")
		if !nameOK(name) {
			writeJSON(w, 400, `{"error":"name is missing or not a theme name"}`)
			return
		}
		f.mu.Lock()
		known := false
		for _, t := range f.themes {
			if t["name"] == name {
				known = true
			}
		}
		f.mu.Unlock()
		if !known {
			writeJSON(w, 404, `{"error":"no such theme on the card"}`)
			return
		}
		writeJSON(w, 200, fmt.Sprintf(`{"state":"installing","name":%q,"reboot":"automatic"}`, name))
	})
	mux.HandleFunc("/api/v1/sd/default", func(w http.ResponseWriter, r *http.Request) {
		f.record(r)
		writeJSON(w, 200, `{"state":"installing","name":"built-in","reboot":"automatic"}`)
	})
	mux.HandleFunc("/api/v1/sd/delete", func(w http.ResponseWriter, r *http.Request) {
		f.record(r)
		name := r.URL.Query().Get("name")
		if !nameOK(name) {
			writeJSON(w, 400, `{"error":"name is missing or not a theme name"}`)
			return
		}
		f.mu.Lock()
		defer f.mu.Unlock()
		for i, t := range f.themes {
			if t["name"] == name {
				f.themes = append(f.themes[:i], f.themes[i+1:]...)
				writeJSON(w, 200, `{"deleted":true}`)
				return
			}
		}
		writeJSON(w, 404, `{"error":"no such theme on the card"}`)
	})
	mux.HandleFunc("/api/v1/sd/theme", func(w http.ResponseWriter, r *http.Request) {
		f.record(r)
		name := r.URL.Query().Get("name")
		if !nameOK(name) {
			writeJSON(w, 400, `{"error":"name is missing or not a theme name"}`)
			return
		}
		if r.ContentLength <= 0 {
			writeJSON(w, 411, `{"error":"Content-Length required"}`)
			return
		}
		body, err := io.ReadAll(r.Body)
		if err != nil {
			writeJSON(w, 400, `{"error":"read failed"}`)
			return
		}
		f.mu.Lock()
		f.themes = append(f.themes, map[string]any{"name": name, "title": name, "why": "",
			"bytes": len(body), "serial": 1, "ok": true, "active": false})
		f.uploaded[name] = len(body)
		f.mu.Unlock()
		writeJSON(w, 200, fmt.Sprintf(`{"state":"uploaded","name":%q,"bytes":%d}`, name, len(body)))
	})
	mux.HandleFunc("/api/v1/sd/rescan", func(w http.ResponseWriter, r *http.Request) {
		f.record(r)
		writeJSON(w, 200, `{"rescan":true}`)
	})
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		f.record(r)
		writeJSON(w, 404, `{"error":"no such route on this fake orb"}`)
	})

	log.Printf("orbfake listening on %s (card %s, %d themes)", *addr, sdCard, len(f.themes))
	log.Printf("orbfake: hub must dial this port; run the hub with the same port via the orb-port it defaults to 80")
	log.Fatal(http.ListenAndServe(*addr, mux))
}

// nameOK mirrors orb_sd_name_ok in main/sd/orb_sd.c.
func nameOK(s string) bool {
	if s == "" || s[0] == '.' || len(s) > 31 {
		return false
	}
	for _, c := range s {
		switch {
		case c >= 'A' && c <= 'Z', c >= 'a' && c <= 'z', c >= '0' && c <= '9', c == '.', c == '_', c == '-':
		default:
			return false
		}
	}
	return true
}

func (f *fakeOrb) record(r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, r.Method+" "+r.URL.Path+"?"+r.URL.RawQuery)
	if len(f.calls) > 50 {
		f.calls = f.calls[len(f.calls)-50:]
	}
	log.Printf("orbfake: %s %s?%s", r.Method, r.URL.Path, r.URL.RawQuery)
}
