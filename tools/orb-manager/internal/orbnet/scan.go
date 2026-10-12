package orbnet

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"time"
)

// orbStatusGrace is the per-attempt budget for a single orb status fetch. The
// orb's httpd is a single poll thread behind the render loop, so first byte can
// idle well past a UI-tuned timeout.
const orbStatusGrace = 2 * time.Second

// scanClient is the orbStatus client with a deadline that fits a momentarily
// busy single-socket orb (observed 180–530 ms per status on the bench), unlike
// the UI client's tight 400 ms timeout.
var scanClient = &http.Client{Timeout: orbStatusGrace}

// orbStatus fetches and classifies one host's status. The orb's httpd has a
// 10-socket backlog and its own config page polls status at a few Hz, so a
// single GET intermittently gets refused or stalls (bench: 3 of 48 burst
// requests answered). Only callers that already know the orb's address reach
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
