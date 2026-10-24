package orbnet

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"syscall"
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
	cr := &countingReader{src: bytes.NewReader(body), fn: progress}
	raw, err := c.postOnce(ip, path, cr, contentType, len(body))
	// One silent re-dial when the socket died with no response. The hub never
	// reuses a connection (NewClient sets DisableKeepAlives), so a reset means
	// the device's own httpd killed this request mid-flight: it admits 3 sockets
	// and lru-purges to take a new one, and a status poll racing a Send can be
	// the victim; a reboot or a WiFi drop mid-store does the same. We saw no
	// response byte, so whatever the device did with the first attempt, it
	// answered nothing. Re-sending is safe: h_theme_upload truncates its scratch
	// at begin, and if the first attempt somehow completed a store whose answer
	// was lost with the socket, the re-send comes back as a duplicate refusal —
	// which is the truth the operator can act on — never a silent double-write.
	// Timeouts deliberately do NOT retry: a slow finish may have already stored
	// the theme, and re-sending 1.3 MB to check is worse than the message.
	if err != nil && deadConnErr(err) {
		cr.reset()
		if progress != nil {
			progress(0)
		}
		raw, err = c.postOnce(ip, path, cr, contentType, len(body))
	}
	return raw, err
}

// postOnce builds a fresh POST (fresh body reader each attempt) and round-trips
// it once. A fresh *http.Request is required: the transport consumed the body
// of the first one, and reusing a request whose reader is spent would send zero
// bytes under a full Content-Length — a truncated upload the device aborts.
func (c *Client) postOnce(ip, path string, cr *countingReader, contentType string, n int) (json.RawMessage, error) {
	var rd io.Reader
	if n > 0 {
		rd = cr
	}
	req, err := http.NewRequest(http.MethodPost, c.url(ip, path), rd)
	if err != nil {
		return nil, err
	}
	if n > 0 {
		req.ContentLength = int64(n)
	}
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	return c.roundTrip(req)
}

// deadConnErr reports an error the transport raised at the socket, before any
// response byte: the exact family of "connection reset by peer" the browser
// showed as a red banner when a rebooted orb invalidated a pooled connection.
func deadConnErr(err error) bool {
	return errors.Is(err, syscall.ECONNRESET) ||
		errors.Is(err, syscall.EPIPE) ||
		errors.Is(err, syscall.ECONNABORTED)
}

// countingReader reports cumulative bytes read, which is bytes the transport has
// handed to the socket — the closest witness of "how much has left this building"
// a client of net/http gets without touching the server.
type countingReader struct {
	src *bytes.Reader
	fn  func(int64)
	got int64
}

func (cr *countingReader) Read(p []byte) (int, error) {
	n, err := cr.src.Read(p)
	if n > 0 {
		cr.got += int64(n)
		if cr.fn != nil {
			cr.fn(cr.got)
		}
	}
	return n, err
}

// reset rewinds the body and the counter so a retry re-sends the whole file.
// The whole file matters: how much of the first attempt the device's SD write
// took is unknowable from the client, and h_theme_upload truncates its scratch
// at begin, so a full re-send always matches Content-Length exactly.
func (cr *countingReader) reset() {
	if _, err := cr.src.Seek(0, io.SeekStart); err != nil {
		return
	}
	cr.got = 0
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

// CommitToken reads the one-shot commit token out of a duplicate refusal's body.
// A device that answers OverwriteConflict with a token is offering to file the
// copy it already staged: ThemeCommit spends the offer with an empty POST instead
// of pushing the same megabytes a second time. "" means the device offered nothing
// (older firmware), and the caller's only path is the full re-upload it always was.
func (e *DeviceError) CommitToken() string {
	var env struct {
		Commit string `json:"commit"`
	}
	if err := json.Unmarshal(e.Body, &env); err != nil {
		return ""
	}
	return env.Commit
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
