package orbnet

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
)

// The two verbs the device-side surfaces need, shared by the theme client. Both
// return the device's own body on 2xx and an error carrying the firmware's own
// {"error":...} words otherwise: a hub that reworded a device refusal would hide the
// reason the firmware gave — a full card, a bad name, a job already running.

func (c *Client) deviceGet(ip, path string) (json.RawMessage, error) {
	req, err := http.NewRequest(http.MethodGet, c.url(ip, path), nil)
	if err != nil {
		return nil, err
	}
	return c.roundTrip(req)
}

func (c *Client) devicePost(ip, path string, body []byte, contentType string) (json.RawMessage, error) {
	var rd io.Reader
	if body != nil {
		rd = bytes.NewReader(body)
	}
	req, err := http.NewRequest(http.MethodPost, c.url(ip, path), rd)
	if err != nil {
		return nil, err
	}
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	return c.roundTrip(req)
}

func (c *Client) roundTrip(req *http.Request) (json.RawMessage, error) {
	resp, err := c.h.Do(req)
	if err != nil {
		return nil, fmt.Errorf("orbnet: %s unreachable: %w", req.URL.Host, err)
	}
	defer resp.Body.Close()
	raw, err := io.ReadAll(io.LimitReader(resp.Body, maxDeviceReply+1))
	if err != nil {
		return nil, fmt.Errorf("orbnet: reading %s from %s: %w", req.URL.Path, req.URL.Host, err)
	}
	if int64(len(raw)) > maxDeviceReply {
		return nil, fmt.Errorf("orbnet: %s sent more than the %d B a device reply may be", req.URL.Host, maxDeviceReply)
	}
	if resp.StatusCode >= 300 {
		return nil, deviceError(req, resp.StatusCode, raw)
	}
	if !json.Valid(raw) {
		return nil, fmt.Errorf("orbnet: %s answered %s with something that is not JSON: %.120s",
			req.URL.Host, req.URL.Path, strings.TrimSpace(string(raw)))
	}
	return raw, nil
}

// maxDeviceReply bounds what one device call may pull. The firmware's own replies are
// small by construction — h_sd caps its JSON at 2400 B — so this is a ceiling against
// whatever else happens to answer on port 80, not a guess at a real size.
const maxDeviceReply = 1 << 20

// deviceError turns a non-2xx device reply into an error carrying the firmware's
// words. Every h_* handler answers {"error":"..."}; anything else is reported by
// status, because a captive portal or a proxy in the way deserves to be named rather
// than silently parsed.
func deviceError(req *http.Request, status int, raw []byte) error {
	var env struct {
		Error string `json:"error"`
	}
	if err := json.Unmarshal(raw, &env); err == nil && env.Error != "" {
		return fmt.Errorf("orbnet: the orb refused %s: %s", req.URL.Path, env.Error)
	}
	return fmt.Errorf("orbnet: %s answered %s with HTTP %d", req.URL.Host, req.URL.Path, status)
}
