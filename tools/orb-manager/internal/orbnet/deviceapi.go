package orbnet

import (
	"bytes"
	"encoding/json"
	"errors"
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
	return c.devicePostCounted(ip, path, body, contentType, nil)
}

// devicePostCounted is devicePost with one addition: progress, called after every
// chunk of the body the transport has actually written to the wire. It exists
// because a theme upload is megabytes over WiFi and the hub's caller was watching
// a status line that could not say a single true word about the transfer until the
// device answered. progress may be nil, which is exactly the old path.
//
// The ContentLength pin is load-bearing: http.NewRequest learns the length by
// type-asserting *bytes.Reader, and a counting wrapper hides it, which would send
// the request chunked — and h_theme_upload answers a chunked body with 411. The
// firmware requires Content-Length (a finished upload and a vanished client must
// not look alike), so the length is stated explicitly here.
func (c *Client) devicePostCounted(ip, path string, body []byte, contentType string, progress func(int64)) (json.RawMessage, error) {
	var rd io.Reader
	if body != nil {
		rd = bytes.NewReader(body)
		if progress != nil {
			rd = &countingReader{r: rd, fn: progress}
		}
	}
	req, err := http.NewRequest(http.MethodPost, c.url(ip, path), rd)
	if err != nil {
		return nil, err
	}
	if body != nil {
		req.ContentLength = int64(len(body))
	}
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	return c.roundTrip(req)
}

// countingReader reports cumulative bytes read, which is bytes the transport has
// handed to the socket — the closest witness of "how much has left this building"
// a client of net/http gets without touching the server.
type countingReader struct {
	r   io.Reader
	fn  func(int64)
	got int64
}

func (cr *countingReader) Read(p []byte) (int, error) {
	n, err := cr.r.Read(p)
	if n > 0 {
		cr.got += int64(n)
		cr.fn(cr.got)
	}
	return n, err
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

// DeviceError is a non-2xx answer from the orb, carrying the status and the body it
// came with. The message alone is enough to show an operator, but a caller that has to
// *act* on a refusal needs the structured reply behind it — h_theme_upload answers 409
// with {"overwrite":false} precisely so the page can ask "replace it?" and re-send, and
// that decision is lost if the only thing the hub kept is a sentence.
type DeviceError struct {
	Status int
	Path   string
	Body   json.RawMessage
	err    error
}

func (e *DeviceError) Error() string { return e.err.Error() }

// OverwriteConflict reports the one device refusal a caller can resolve by asking the
// operator: a verified file that would replace a DIFFERENT theme already filed under
// the same name, offered by the firmware only when ?overwrite=1 was not sent.
func (e *DeviceError) OverwriteConflict() bool {
	if e.Status != http.StatusConflict {
		return false
	}
	var env struct {
		Overwrite *bool `json:"overwrite"`
	}
	if err := json.Unmarshal(e.Body, &env); err != nil {
		return false
	}
	return env.Overwrite != nil && !*env.Overwrite
}

// maxDeviceReply bounds what one device call may pull. The firmware's own replies are
// small by construction — h_sd writes into a static json[6144] and reports an overflow
// rather than truncating — so this is a ceiling against whatever else happens to answer
// on port 80, not a guess at a real size.
const maxDeviceReply = 1 << 20

// deviceError turns a non-2xx device reply into an error carrying the firmware's
// words. Every h_* handler answers {"error":"..."}; anything else is reported by
// status, because a captive portal or a proxy in the way deserves to be named rather
// than silently parsed.
func deviceError(req *http.Request, status int, raw []byte) error {
	var env struct {
		Error  string `json:"error"`
		Detail string `json:"detail"`
	}
	base := fmt.Sprintf("orbnet: %s answered %s with HTTP %d", req.URL.Host, req.URL.Path, status)
	if err := json.Unmarshal(raw, &env); err == nil && env.Error != "" {
		base = fmt.Sprintf("orbnet: the orb refused %s: %s", req.URL.Path, env.Error)
		// h_theme_upload's verdict is {"error":"rejected","detail":"…"}: the reason a
		// container was refused is the only thing an operator can act on, and h_ota puts
		// its reason beside it the same way. Dropping detail turns a diagnosable
		// rejection into "rejected".
		if env.Detail != "" {
			base += " (" + env.Detail + ")"
		}
	}
	return &DeviceError{Status: status, Path: req.URL.Path, Body: json.RawMessage(append([]byte(nil), raw...)), err: errors.New(base)}
}
