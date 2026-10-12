package orbnet

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"time"
)

// scanClient is the orbStatus client with a deadline that fits a momentarily
// busy single-socket orb (observed 180–530 ms per status on the bench), unlike
// the UI client's tight 400 ms timeout.
var scanClient = &http.Client{Timeout: orbStatusGrace}

// Found is one orb discovered on the LAN.
type Found struct {
	IP     string `json:"ip"`
	Status Status `json:"status"`
}

// maxScanHosts caps a sweep. A /24 is 254; anything larger is refused outright.
const maxScanHosts = 254

const (
	dialTimeout    = 600 * time.Millisecond // TCP gate: lwIP answers SYN, slowly
	orbStatusGrace = 2 * time.Second        // a busy orb can take ~500 ms+
	// scanConcurrency balances two measured facts about the orb's lwIP stack.
	// A 40-wide burst of dials still lands ~5 SYNs, so a moderate window finds
	// the one orb; far lower concurrency serialises the ~250 empty IPs that
	// each burn the full connect timeout and blows past a usable wall time.
	scanConcurrency = 32
)

// Sweep hosts a bounded subnet for orbs. cidr must be an IPv4 CIDR no wider
// than /24, or a bare IPv4 (single host). Refusing anything larger is a hard
// safety gate, not a nicety: this tool is pointed at a home LAN.
//
// It is two-gated because the orb's ESP-IDF httpd is single-poll-thread with a
// tiny listen backlog: a naive fan-out of status GETs starves the real orb and
// it gets dropped (verified on the bench). A bare TCP dial is cheap and lwIP
// still answers it under load, so the pricier status probe runs only on hosts
// that accepted on :80 — and the concurrency window is tuned so those dials
// actually get through to the orb rather than swamping its backlog.
func (c *Client) Sweep(cidr string) ([]Found, error) {
	hosts, err := hostList(cidr)
	if err != nil {
		return nil, err
	}
	out := []Found{} // never nil: the UI expects [], not null
	var (
		wg  sync.WaitGroup
		mu  sync.Mutex
		sem = make(chan struct{}, scanConcurrency)
	)
	dialer := &net.Dialer{Timeout: dialTimeout}
	for _, h := range hosts {
		wg.Add(1)
		sem <- struct{}{}
		go func(ip string) {
			defer wg.Done()
			defer func() { <-sem }()
			conn, derr := dialer.Dial("tcp", net.JoinHostPort(ip, "80"))
			if derr != nil {
				return // nothing listening on :80 — not an orb, cheap reject
			}
			conn.Close()
			s, ok := c.orbStatus(ip)
			if ok {
				mu.Lock()
				out = append(out, Found{IP: ip, Status: s})
				mu.Unlock()
			}
		}(h)
	}
	wg.Wait()
	return out, nil
}

// orbStatus fetches and classifies one host's status. The orb's httpd has a
// 10-socket backlog and its own config page polls status at a few Hz, so a
// single GET intermittently gets refused or stalls (bench: 3 of 48 burst
// requests answered). Only hosts that already passed the cheap TCP dial reach
// here, so retries are affordable. But a clean 200 whose body is not orb-shaped
// is a definitive "not an orb" — retrying it would only waste wall time on a
// printer or router serving JSON on :80 — so we retry transport/HTTP errors
// only, and give up immediately on a valid-but-foreign response.
func (c *Client) orbStatus(ip string) (Status, bool) {
	for attempt := 0; attempt < 3; attempt++ {
		s, ok, retry := c.orbStatusOnce(ip)
		if ok {
			return s, true
		}
		if !retry {
			return Status{}, false
		}
		time.Sleep(150 * time.Millisecond)
	}
	return Status{}, false
}

// orbStatusOnce returns (status, classified, shouldRetry). shouldRetry is false
// for a definitive non-orb (a clean 200 that failed the shape check).
func (c *Client) orbStatusOnce(ip string) (Status, bool, bool) {
	var s Status
	ctx, cancel := context.WithTimeout(context.Background(), orbStatusGrace)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.url(ip, "/api/v1/status"), nil)
	if err != nil {
		return s, false, false
	}
	resp, err := scanClient.Do(req)
	if err != nil {
		return s, false, true // transport error: the orb may just be busy
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return s, false, true // 5xx/conn-reset under load: retryable
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8192))
	if err != nil {
		return s, false, true
	}
	if !classifyOrbJSON(body, &s) {
		return s, false, false // valid JSON, just not an orb
	}
	return s, true, false
}

// classifyOrbJSON decodes a status body and requires the orb-shaped fields.
// h_status always reports fw and slot, so a generic :80 JSON server is
// rejected on both being empty.
func classifyOrbJSON(body []byte, s *Status) bool {
	if err := json.Unmarshal(body, s); err != nil {
		return false
	}
	return s.Slot != "" || s.FW != ""
}

// hostList expands cidr to the addresses to probe, enforcing the /24 guard.
func hostList(cidr string) ([]string, error) {
	if ip := net.ParseIP(cidr); ip != nil {
		if ip.To4() == nil {
			return nil, fmt.Errorf("orbnet: only IPv4 household scans are supported")
		}
		return []string{ip.String()}, nil
	}
	_, ipnet, err := net.ParseCIDR(cidr)
	if err != nil {
		return nil, fmt.Errorf("orbnet: scan target %q is not a CIDR or IPv4: %w", cidr, err)
	}
	if ipnet.IP.To4() == nil {
		return nil, fmt.Errorf("orbnet: only IPv4 household scans are supported")
	}
	ones, bits := ipnet.Mask.Size()
	if bits != 32 || ones < 24 {
		return nil, fmt.Errorf("orbnet: refusing to scan %s — only /24 or smaller subnets are allowed", cidr)
	}
	var hosts []string
	for ip := ip4(ipnet.IP); ip4Next(ip); {
		hosts = append(hosts, dup(ip))
	}
	if len(hosts) > maxScanHosts {
		return nil, fmt.Errorf("orbnet: refusing to scan %s (%d hosts) — capped at %d", cidr, len(hosts), maxScanHosts)
	}
	return hosts, nil
}

// ip4 copies to a mutable 4-byte slice (network address, host octet at [3]).
func ip4(ip net.IP) []byte {
	return append([]byte(nil), ip.To4()...)
}

// ip4Next advances the host octet; returns false at the network base (.0) or
// broadcast (.255), which we never probe.
func ip4Next(ip []byte) bool {
	ip[3]++
	return ip[3] != 0 && ip[3] != 255
}

func dup(ip []byte) string { return net.IP(append([]byte(nil), ip...)).String() }
