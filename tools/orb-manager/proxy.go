package main

import (
	"bytes"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"strings"
)

// proxyOrb forwards /orb/<ipv4>/api/v1/... to http://<ipv4>/api/v1/... on the same
// origin, so the browser never needs the orb to send CORS headers (it sends none).
//
// Three gates stand between a request and the dial, because this is the only route
// in the hub that reaches off the box:
//
//  1. The address must be an IPv4 LITERAL the hub has LEARNED — over the USB console
//     during a flash or a provision, or from the operator committing to it in the
//     Manage card. An arbitrary address in that box used to be dialled, which made
//     the hub an SSRF pivot out of 127.0.0.1: any page the browser visited could
//     have the hub knock on 192.168.1.1, on a neighbour's laptop, or on a cloud
//     metadata address, and read what came back through the same-origin reply.
//  2. The path must be a route the firmware actually registers, and the method one
//     that route accepts. The old check was a /api/v1/ prefix, which left the
//     device's /api/v1/ota — the raw-image write the hub's own policy forbids — one
//     URL away.
//  3. The exchange is byte-bounded in both directions, so a route that streams turns
//     into a truncated reply rather than an unbounded proxy.
func (s *Server) proxyOrb(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/orb/")
	host, apiPath, ok := strings.Cut(rest, "/")
	if !ok || !isIPv4(host) {
		proxyError(w, http.StatusBadRequest, "expected /orb/<ipv4>/api/v1/...")
		return
	}
	// Normalise the path before matching: the allowlist is exact, so a trailing
	// slash or a doubled one must not smuggle a route past it.
	route := "/" + strings.Trim(apiPath, "/")
	if !strings.HasPrefix(route, "/api/v1/") || route == "/api/v1/" {
		proxyError(w, http.StatusBadRequest, "expected /orb/<ipv4>/api/v1/...")
		return
	}
	// One gate, one answer: the grant set decides reachability and the fleet may still
	// take it away when the board at that address is no longer the board that was
	// allowed. Keep the call here rather than re-testing grants.allowed above: the two
	// checks read the same state at different times, and a route that consulted only one
	// of them is how a rebind becomes a successful dial of the wrong orb.
	if ok, why := s.mayDial(host); !ok {
		proxyError(w, http.StatusForbidden, why)
		return
	}
	if !routeAllowed(route, r.Method) {
		proxyError(w, http.StatusForbidden, "route "+r.Method+" "+route+" is not one the hub forwards. "+
			"Device firmware, not an arbitrary path, is flashed from the Onboard card.")
		return
	}
	target := "http://" + host + ":" + s.device.Port() + route
	if r.URL.RawQuery != "" {
		target += "?" + r.URL.RawQuery
	}
	// The body is copied under a cap rather than streamed: io.LimitReader would
	// silently truncate an upload, so a too-large one is refused outright. The
	// device's multipart theme upload is the only big body the UI sends, and the
	// hub's own upload cap (firmware.MaxAssetBytes) is smaller than this.
	var body io.Reader
	if r.Body != nil && r.Method != http.MethodGet && r.Method != http.MethodDelete {
		raw, rerr := io.ReadAll(io.LimitReader(r.Body, proxyRequestCap+1))
		if rerr != nil {
			proxyError(w, http.StatusBadGateway, "reading the request body: "+rerr.Error())
			return
		}
		if int64(len(raw)) > proxyRequestCap {
			proxyError(w, http.StatusRequestEntityTooLarge, "request body over the proxy's 4 MiB cap")
			return
		}
		body = bytes.NewReader(raw)
	}
	req, err := http.NewRequestWithContext(r.Context(), r.Method, target, body)
	if err != nil {
		proxyError(w, http.StatusBadGateway, err.Error())
		return
	}
	if ct := r.Header.Get("Content-Type"); ct != "" && body != nil {
		req.Header.Set("Content-Type", ct)
	} else if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
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
	// Copy under a cap that is one byte larger than what may be forwarded, so a
	// reply that overflows is DETECTED rather than silently trimmed: the loop this
	// replaced broke on any read error and dropped it, so half a JSON body reached
	// the client looking complete.
	buf := make([]byte, 8192)
	var n int64
	// One status reply a poll is worth a copy of. The card's Manage panel polls
	// /api/v1/status through here and nothing else in the hub sees that reply, so
	// without this fold the device's authority over its own name would be real only
	// when somebody happened to press a button. Accumulated for this one route and
	// method, under the same cap the copy already enforces; every other proxied
	// reply streams through untouched, because a theme list is 40 KiB of SD-card
	// filenames and copying it to refresh one string is a bad trade.
	var statusBody []byte
	foldStatus := route == "/api/v1/status" && r.Method == http.MethodGet &&
		resp.StatusCode == http.StatusOK
	for {
		if n >= proxyResponseCap {
			log.Printf("orb-manager: proxy reply from %s hit the %d B cap; the browser got a truncated body", host, proxyResponseCap)
			return
		}
		lim := int64(len(buf))
		if proxyResponseCap-n < lim {
			lim = proxyResponseCap - n
		}
		rn, rerr := resp.Body.Read(buf[:lim])
		if rn > 0 {
			if foldStatus && n+int64(rn) <= proxyResponseCap {
				statusBody = append(statusBody, buf[:rn]...)
			}
			if _, werr := w.Write(buf[:rn]); werr != nil {
				return // the browser went away; nothing left to report to
			}
			n += int64(rn)
		}
		if rerr == io.EOF {
			s.foldDeviceName(host, statusBody, foldStatus)
			return
		}
		if rerr != nil {
			log.Printf("orb-manager: proxy read from %s aborted after %d B: %v", host, n, rerr)
			return
		}
	}
}

// devicePortOrDefault lived on the hub until the proxy and the device API were
// caught dialing different ports; the answer now comes from orbnet.Client, which both
// use. See Client.DevicePort.

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

// copyProxyHeaders mirrors the device reply's content type and nothing else. It
// used to forward every header the orb sent, which is how a device-side
// Access-Control-Allow-* or a stale Server banner would reach the browser: the hub
// is the origin the page talks to, so only the payload's own type is the browser's
// business. Content-Length and Connection are excluded as before, since the hub's
// reply has its own framing.
func copyProxyHeaders(w http.ResponseWriter, resp *http.Response) {
	if ct := resp.Header.Get("Content-Type"); ct != "" {
		w.Header().Set("Content-Type", ct)
	}
}
