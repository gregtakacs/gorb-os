// Package orbnet talks to an orb's on-device HTTP API and finds orbs on the
// LAN. The browser talks only to the manager's own origin; this package is the
// one that reaches the orb, which is what sidesteps the device's missing CORS
// headers. Endpoints mirror main/net/orb_net.c exactly.
package orbnet

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"
)

// Status mirrors the subset of the hand-formatted GET /api/v1/status body
// (orb_net.c h_status) the manager displays. The firmware emits art_ready,
// synced and ap as bare true/false JSON booleans, rssi/tz_offset as numbers,
// and slot/fw always populated (fw from app->version, slot label or "?"),
// which is what the scan classifies an orb on.
//
// MAC is the chip's BASE MAC as 12 lowercase hex digits with no separators. It
// is deliberately not the SoftAP MAC, which is base+1 on this target and would not
// join to the USB serial descriptor or the flasher's eFuse read. It is empty
// when the firmware could not read it; callers must never treat an empty MAC as
// a match, which is why the fleet store keys on it only after macNorm rejects
// empty.
type Status struct {
	State    string `json:"state"`
	IP       string `json:"ip"`
	MAC      string `json:"mac"`
	Rssi     int    `json:"rssi"`
	FW       string `json:"fw"`
	IDF      string `json:"idf"`
	Slot     string `json:"slot"`
	Img      string `json:"img"`
	ArtReady bool   `json:"art_ready"`
	ArtBytes uint32 `json:"art_bytes"`
	Synced   bool   `json:"synced"`
	TZ       string `json:"tz"`
	TZOffset int    `json:"tz_offset"`
	// Name is the orb's own, read from the device rather than from the hub's
	// fleet row. Empty means the device is unnamed, and that is a different fact
	// from "the device could not be asked" — which arrives as a status error, not
	// as an empty Name — so the hub's cache rule distinguishes the two: only a
	// successful status read is allowed to clear a cached name.
	//
	// The DEVICE is the authority for this string. orbfleet.Orb.Name is a cache of
	// it, kept so the card can render a fleet without dialing every board.
	Name       string `json:"name"`
	AP         bool   `json:"ap"`
	AssocFails uint32 `json:"assoc_fails"`
}

// Client fetches orb JSON over plain HTTP with bounded timeouts.
type Client struct {
	h *http.Client
	// DevicePort is the TCP port every dial to an orb uses. The device's httpd
	// listens on 80, so this is unset in production; it exists because the hub must
	// dial the proxy and the API on ONE port, and a second port setting somewhere
	// else is how the two drifted apart in tests. A bench test against a local fake
	// orb is the only thing that sets it.
	DevicePort string
}

// DefaultPort is the port an orb's own httpd listens on: CONFIG_ORB_NET_HTTPD_PORT,
// named once here so the hub's dial default, the browse's SRV port and the bench's
// -orb-port override have one answer to point at. TestFirmwarePortIsWhatBothSidesSay
// pins it against the firmware's Kconfig, because a browse that promises a port the
// proxy never dials is a listing of orbs that cannot be managed.
const DefaultPort = 80

// Port is the dial port, defaulted. Every caller takes the port from here so there
// is exactly one answer to "which port does the hub reach an orb on".
func (c *Client) Port() string {
	if c.DevicePort == "" {
		return strconv.Itoa(DefaultPort)
	}
	return c.DevicePort
}

// NewClient returns a client tuned for a flaky bench link: a per-request timeout
// short enough that one orb which stopped answering — asleep, reflashed, or simply
// gone after a DHCP rebind between the announcement that listed it and the dial —
// fails its read in seconds instead of holding the card open until the OS gives up.
// Every dial here is one address the hub was handed or overheard, so the timeout
// bounds a single host, never a range.
func NewClient(respTimeout time.Duration) *Client {
	return &Client{h: &http.Client{
		Timeout: respTimeout,
		Transport: &http.Transport{
			DialContext:         (&net.Dialer{Timeout: 800 * time.Millisecond}).DialContext,
			MaxIdleConnsPerHost: 2,
			DisableKeepAlives:   true,
		},
	}}
}

// WithTimeout is the same orb-tuned transport on a different per-request budget:
// a shallow client whose only difference is Client.Timeout. It exists for the
// preview leg, which is a management call whose reply happens to be ~100 KB
// read off the card and streamed over the link — seconds of patience the 8 s
// management budget was never priced for, and a fresh 8 s dial of the same
// stalling route on every page reload is how a missing thumbnail becomes a wedge.
func (c *Client) WithTimeout(d time.Duration) *Client {
	cp := *c
	hc := *c.h
	hc.Timeout = d
	cp.h = &hc
	return &cp
}

// HTTP exposes the underlying http.Client so the same-origin proxy reuses the
// orb-tuned transport (short dial, no keepalive) instead of the default one.
func (c *Client) HTTP() *http.Client { return c.h }

func (c *Client) url(ip, path string) string {
	if !strings.Contains(ip, ":") {
		ip = net.JoinHostPort(ip, c.Port())
	}
	return "http://" + ip + path
}

// Status fetches one orb's status by ip.
func (c *Client) Status(ip string) (*Status, error) {
	s, ok := c.orbStatus(ip)
	if !ok {
		return nil, fmt.Errorf("orbnet: %s did not answer as an orb on /api/v1/status", ip)
	}
	return &s, nil
}

// GetConfig returns the raw JSON body of /api/v1/config (the tz table) so the
// UI can render the picker without the manager re-encoding the schema.
func (c *Client) GetConfig(ip string) (json.RawMessage, error) {
	return c.getRaw(ip, "/api/v1/config")
}

// PostConfig posts {"tz": v} to /api/v1/config and returns the effect body.
func (c *Client) PostConfig(ip, tz string) (json.RawMessage, error) {
	return c.postJSON(ip, "/api/v1/config", map[string]string{"tz": tz})
}

// PostName posts {"name": v} to /api/v1/name and returns the effect body. An empty
// v is a request to CLEAR the device's name, which is why this does not reject it:
// every other writer in this package treats "" as a no-op, and here it is the most
// meaningful thing you can send.
//
// The device validates before it stores, so a name the hub would have accepted
// (a non-ASCII one, or 33 bytes) comes back as an error here. Callers that offer a
// name field should run orbfleet.CheckNameASCII first and refuse before dialing,
// so the operator reads one message instead of a 400 attributed to an IP.
func (c *Client) PostName(ip, name string) (json.RawMessage, error) {
	return c.postJSON(ip, "/api/v1/name", map[string]string{"name": name})
}

// PostAP opens (on=true) or closes (on=false) the provisioning SoftAP.
func (c *Client) PostAP(ip string, on bool) (json.RawMessage, error) {
	v := "on"
	if !on {
		v = "off"
	}
	return c.postJSON(ip, "/api/v1/wifi/ap", map[string]string{"on": v})
}

// PostJoin associates the orb to a LAN SSID with a passphrase, a tz and a name.
// Empty tz and empty name each mean "leave what the device has alone"; the keys are
// omitted from the body rather than sent empty, because an empty name IS a command
// on this endpoint (it clears) and an onboarding call that passed "" meaning "no
// opinion" must not erase a name the board already carried.
func (c *Client) PostJoin(ip, ssid, pass, tz, name string) (json.RawMessage, error) {
	body := map[string]string{"ssid": ssid}
	if pass != "" {
		body["pass"] = pass
	}
	if tz != "" {
		body["tz"] = tz
	}
	if name != "" {
		body["name"] = name
	}
	return c.postJSON(ip, "/api/v1/wifi/join", body)
}

// WifiScan asks the orb to scan its radio and returns the AP list JSON.
func (c *Client) WifiScan(ip string) (json.RawMessage, error) {
	return c.getRaw(ip, "/api/v1/wifi/scan")
}

// Reboot asks the orb to restart into its active slot.
func (c *Client) Reboot(ip string) (json.RawMessage, error) {
	return c.postJSON(ip, "/api/v1/reboot", map[string]string{})
}

func (c *Client) getRaw(ip, path string) (json.RawMessage, error) {
	resp, err := c.h.Get(c.url(ip, path))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<16))
	if resp.StatusCode >= 400 {
		return body, fmt.Errorf("orbnet: GET %s%s -> %s", ip, path, resp.Status)
	}
	return body, nil
}

func (c *Client) postJSON(ip, path string, payload any) (json.RawMessage, error) {
	buf, err := json.Marshal(payload)
	if err != nil {
		return nil, err
	}
	resp, err := c.h.Post(c.url(ip, path), "application/json", strings.NewReader(string(buf)))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<16))
	if resp.StatusCode >= 400 {
		return body, fmt.Errorf("orbnet: POST %s%s -> %s", ip, path, resp.Status)
	}
	return body, nil
}

// progressReader counts bytes as the transport consumes them. It exists because
// the h_ota handler holds the one httpd task for the whole upload: no orb answers
// a status GET while its flash is being written, so the live byte count of a push
// can only be read where the push originates — the writer's side of the socket.
// http.Transport pulls the body through Read, so the count is the bytes actually
// crossing the wire, not the Content-Length the call declared. The callback runs
// on the goroutine blocked in Do, which is the goroutine that owns the request;
// callers must not assume any other ordering. A read error mid-body still reports
// what was consumed before it, which is the honest figure at the moment the wire
// died.
type progressReader struct {
	r     *bytes.Reader
	fn    func(sent, total int64)
	sent  int64
	total int64
}

func (p *progressReader) Read(b []byte) (int, error) {
	n, err := p.r.Read(b)
	p.sent += int64(n)
	if p.fn != nil && (n > 0 || err != nil) {
		p.fn(p.sent, p.total)
	}
	return n, err
}

// PostImage POSTs raw bytes — an app image — to one device route and returns the
// reply. It exists for /api/v1/ota, one of the two orb endpoints whose body is
// not JSON (h_theme_upload takes a raw container the same way): the firmware
// stages the bytes it receives with Content-Length as the declared size, so this
// sends a real length rather than streaming through a hub the size of the image.
// The caller owns the bytes; nothing here reads a disk.
func (c *Client) PostImage(ctx context.Context, ip, path string, contentType string, blob []byte) (json.RawMessage, error) {
	return c.PostImageProgress(ctx, ip, path, contentType, blob, nil)
}

// PostImageProgress is PostImage with the live byte count: onProgress, when not
// nil, is called with (bytes sent, total) as the transport consumes the body.
// The upload is the longest silence an update can carry — minutes of WiFi and
// flash on a slow link, with the orb itself unable to answer while h_ota holds
// the httpd task — so a caller that owes a progress bar takes this form.
func (c *Client) PostImageProgress(ctx context.Context, ip, path string, contentType string, blob []byte, onProgress func(sent, total int64)) (json.RawMessage, error) {
	var body io.Reader = bytes.NewReader(blob)
	if onProgress != nil {
		body = &progressReader{r: bytes.NewReader(blob), fn: onProgress, total: int64(len(blob))}
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.url(ip, path), body)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", contentType)
	req.ContentLength = int64(len(blob))
	resp, err := c.h.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<16))
	if resp.StatusCode >= 400 {
		return out, fmt.Errorf("orbnet: POST %s%s -> %s", ip, path, resp.Status)
	}
	return out, nil
}
