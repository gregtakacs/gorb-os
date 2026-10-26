package orbnet

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"time"
)

// orbStatusGrace is the per-attempt budget for a single orb status fetch. The
// orb's httpd is a single poll thread behind the render loop, so first byte can
// idle well past a UI-tuned timeout.
const orbStatusGrace = 2 * time.Second

// orbStatus fetches and classifies one host's status, retrying transport
// failures up to three attempts. It dials over c.h — the client's own gated,
// orb-tuned transport — and not a package-global client: an ungateable status
// dial was the bus's bypass, and the ten-second sweep using it could SYN-pile
// a wedge the gate existed to serialize. The per-attempt context caps the wait
// to orbStatusGrace, so c.h's larger client timeout never stretches a sweep
// dial.
//
// ErrLaneBusy is returned as SOON as it appears, without retrying: re-queuing
// a dial the bus just declined spends the whole retry budget to likely hear
// the same thing, and the next sweep tick asks honestly anyway.
func (c *Client) orbStatus(ip string) (Status, bool, error) {
	var lastErr error
	for attempt := 0; attempt < 3; attempt++ {
		s, ok, retry, err := c.orbStatusOnce(ip)
		if ok {
			return s, true, nil
		}
		lastErr = err
		if err != nil && errors.Is(err, ErrLaneBusy) {
			return s, false, err
		}
		if !retry {
			return s, false, err // definitive non-orb, or a refusal worth reporting
		}
		time.Sleep(150 * time.Millisecond)
	}
	return Status{}, false, lastErr
}

// orbStatusOnce returns (status, classified, shouldRetry, err). shouldRetry is
// false for a definitive non-orb (a clean 200 that failed the shape check) and
// for ErrLaneBusy; err names why for the two failure kinds the caller must tell
// apart: a stall the bus reported, and everything else.
func (c *Client) orbStatusOnce(ip string) (Status, bool, bool, error) {
	var s Status
	ctx, cancel := context.WithTimeout(context.Background(), orbStatusGrace)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.url(ip, "/api/v1/status"), nil)
	if err != nil {
		return s, false, false, err
	}
	resp, err := c.h.Do(req)
	if err != nil {
		if errors.Is(err, ErrLaneBusy) {
			return s, false, false, fmt.Errorf("orbnet: %s: %w", ip, ErrLaneBusy)
		}
		return s, false, true, err // transport error: the orb may just be busy
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return s, false, true, fmt.Errorf("orbnet: %s answered /api/v1/status with %s", ip, resp.Status)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8192))
	if err != nil {
		return s, false, true, err
	}
	if !classifyOrbJSON(body, &s) {
		return s, false, false, nil // valid JSON, just not an orb
	}
	return s, true, false, nil
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
