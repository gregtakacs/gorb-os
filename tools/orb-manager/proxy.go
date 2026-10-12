package main

import (
	"encoding/json"
	"net/http"
	"strings"
)

// proxyOrb forwards /orb/<host>/api/v1/... to http://<host>/api/v1/... on the
// same origin, so the browser never needs the orb to send CORS headers (it
// sends none). Only the device API namespace is exposed and the host must be a
// bare IPv4 literal, which keeps this from becoming an open proxy.
func (s *Server) proxyOrb(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/orb/")
	host, apiPath, ok := strings.Cut(rest, "/")
	if !ok || !strings.HasPrefix("/"+apiPath, "/api/v1/") || !isIPv4(host) {
		proxyError(w, http.StatusBadRequest, "expected /orb/<ipv4>/api/v1/...")
		return
	}
	target := "http://" + host + ":80/" + apiPath
	if r.URL.RawQuery != "" {
		target += "?" + r.URL.RawQuery
	}
	req, err := http.NewRequestWithContext(r.Context(), r.Method, target, r.Body)
	if err != nil {
		proxyError(w, http.StatusBadGateway, err.Error())
		return
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := s.device.HTTP().Do(req)
	if err != nil {
		// A wired-only hub cannot reach an orb sitting on its own provisioning
		// AP (192.168.4.1), or the orb is simply down. Report that as JSON, not
		// the plaintext http.Error body — the client parses every /orb reply as
		// the device API's JSON, and a bare string there surfaces only as a
		// cryptic "is not valid JSON" parse error instead of this reason.
		proxyError(w, http.StatusBadGateway, "orb unreachable: "+err.Error())
		return
	}
	defer resp.Body.Close()
	copyProxyHeaders(w, resp)
	w.WriteHeader(resp.StatusCode)
	buf := make([]byte, 8192)
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			_, _ = w.Write(buf[:n])
		}
		if rerr != nil {
			break
		}
	}
}

// proxyError writes a hub-side proxy failure in the same {"error":...} envelope
// the device API uses, so /orb/... never hands the browser a non-JSON body to
// JSON.parse. The status still carries the real HTTP code for logging.
func proxyError(w http.ResponseWriter, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

// isIPv4 guards the proxy against open-proxy abuse.
func isIPv4(host string) bool {
	parts := strings.Split(host, ".")
	if len(parts) != 4 {
		return false
	}
	for _, p := range parts {
		if p == "" || len(p) > 3 {
			return false
		}
		for _, c := range p {
			if c < '0' || c > '9' {
				return false
			}
		}
	}
	return true
}

func copyProxyHeaders(w http.ResponseWriter, resp *http.Response) {
	for k, vs := range resp.Header {
		if strings.EqualFold(k, "Content-Length") || strings.EqualFold(k, "Connection") {
			continue
		}
		for _, v := range vs {
			w.Header().Add(k, v)
		}
	}
}
