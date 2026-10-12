package orbnet

import (
	"encoding/json"
	"fmt"
	"net/url"
	"regexp"
)

// The device-side THEME API, in one place.
//
// Everything the hub does to an orb's themes goes through this file, against the
// routes main/net/orb_net.c registers today: GET /api/v1/sd, POST /api/v1/sd/install,
// /sd/default, /sd/delete, /sd/theme, /sd/rescan. It is a module rather than calls
// sprinkled through the handlers because docs/theme-identity-plan.md plans to give
// every one of those routes an orb `id` (`?id=` on each call, so one hub can address
// more than the orb it last happened to learn). When that lands this is the only
// file that changes, and the hub's handlers and the card's buttons stay put.
//
// Names are NOT invented here. The firmware owns the slug rule (orb_sd_name_ok in
// main/sd/orb_sd.c) and this mirrors it exactly — ASCII letters, digits, '.', '_',
// '-', at most 31 characters, never starting with '.' — because a theme name is a
// path component under /sd/themes/ on the device, and the hub should be where a bad
// one is refused rather than somewhere the device has to be trusted to catch it.

// ThemeEntry is one row of GET /api/v1/sd's "themes" array, as h_sd formats it from
// orb_theme_scan.
type ThemeEntry struct {
	Name   string `json:"name"`
	Title  string `json:"title"`
	Why    string `json:"why"` // why it is not loadable, when ok is false
	Bytes  uint32 `json:"bytes"`
	Serial uint32 `json:"serial"`
	OK     bool   `json:"ok"`
	Active bool   `json:"active"`
}

// ThemeList is the part of h_sd's reply the card needs: the rows, which one is drawn,
// what the last refused install and the boot-time load failure said, and whether the
// card is mounted at all. Themes is an empty list rather than nil, because the client
// takes .length on it and null has none.
type ThemeList struct {
	Mounted   bool         `json:"mounted"`
	Card      string       `json:"card"`
	Uploading bool         `json:"uploading"`
	TotalMB   uint32       `json:"total_mb"`
	FreeMB    uint32       `json:"free_mb"`
	Source    string       `json:"source"`
	Active    string       `json:"active"`
	Live      string       `json:"live"`
	CardCount int          `json:"card_count"`
	TrySlug   string       `json:"try_slug"`
	TryWhy    string       `json:"try_why"`
	Damage    string       `json:"damage_slug"`
	DamageWhy string       `json:"damage_why"`
	Themes    []ThemeEntry `json:"themes"`
}

// themeNameRe mirrors orb_sd_name_ok. A leading dot is excluded by the first
// character class, which is what keeps ".." and the hidden entries out of the path.
var themeNameRe = regexp.MustCompile(`^[A-Za-z0-9_-][A-Za-z0-9._-]{0,30}$`)

// CheckThemeName reports whether name is a theme name the device will accept.
func CheckThemeName(name string) error {
	if !themeNameRe.MatchString(name) {
		return fmt.Errorf("orbnet: %q is not a theme name: letters, digits, '.', '_', '-', up to 31 characters, and not starting with '.'", name)
	}
	return nil
}

// ThemeList fetches GET /api/v1/sd.
func (c *Client) ThemeList(ip string) (*ThemeList, error) {
	raw, err := c.deviceGet(ip, "/api/v1/sd")
	if err != nil {
		return nil, err
	}
	var l ThemeList
	if err := json.Unmarshal(raw, &l); err != nil {
		return nil, fmt.Errorf("orbnet: %s sent a theme list that is not JSON: %w", ip, err)
	}
	if l.Themes == nil {
		l.Themes = []ThemeEntry{}
	}
	return &l, nil
}

// ThemeInstall queues POST /api/v1/sd/install?name=. The device replies as soon as
// the job is queued and reboots itself once the copy verifies, so a nil error means
// "accepted", never "the glass changed".
func (c *Client) ThemeInstall(ip, name string) (json.RawMessage, error) {
	if err := CheckThemeName(name); err != nil {
		return nil, err
	}
	return c.devicePost(ip, "/api/v1/sd/install?name="+url.QueryEscape(name), nil, "")
}

// ThemeDefault queues the same job for the built-in fallback face.
func (c *Client) ThemeDefault(ip string) (json.RawMessage, error) {
	return c.devicePost(ip, "/api/v1/sd/default", nil, "")
}

// ThemeDelete removes a theme from the card. It is the only theme call that is not a
// queued job, so its reply is the outcome.
func (c *Client) ThemeDelete(ip, name string) (json.RawMessage, error) {
	if err := CheckThemeName(name); err != nil {
		return nil, err
	}
	return c.devicePost(ip, "/api/v1/sd/delete?name="+url.QueryEscape(name), nil, "")
}

// ThemeRescan asks the device to re-read the card, which is how a theme that arrived
// by another route shows up in the list without rebooting the orb.
func (c *Client) ThemeRescan(ip string) (json.RawMessage, error) {
	return c.devicePost(ip, "/api/v1/sd/rescan", nil, "")
}

// ThemeUpload pushes theme.bin bytes to POST /api/v1/sd/theme?name=. The firmware
// demands a Content-Length and refuses an over-large one before it writes a byte
// (ORB_THEME_UPLOAD_MAX, 4 MB + 128 KB), so the body is measured here rather than
// streamed and truncated somewhere in flight.
func (c *Client) ThemeUpload(ip, name string, body []byte) (json.RawMessage, error) {
	if err := CheckThemeName(name); err != nil {
		return nil, err
	}
	if len(body) == 0 {
		return nil, fmt.Errorf("orbnet: refusing to upload an empty theme.bin for %q", name)
	}
	if len(body) > MaxThemeUpload {
		return nil, fmt.Errorf("orbnet: theme.bin is %d B, over the device's %d B ceiling", len(body), MaxThemeUpload)
	}
	return c.devicePost(ip, "/api/v1/sd/theme?name="+url.QueryEscape(name), body, "application/octet-stream")
}

// MaxThemeUpload is ORB_THEME_UPLOAD_MAX from main/art/orb_theme.h, restated so the
// hub refuses the same size the device would, with a sentence an operator can act on
// instead of a 413 from a device two hops away.
const MaxThemeUpload = 0x00400000 + 0x20000
