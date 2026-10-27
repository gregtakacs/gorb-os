package orbnet

// The device-side APP doors, quoted from main/net/orb_net.c: GET /api/v1/apps
// (h_apps_list), POST /api/v1/sd/app (h_app_upload — the raw-bytes twin of
// ThemeUpload, named by the container's own header), POST /api/v1/apps/install?id=<stem>
// (h_apps_install — queues the card-to-slot job and answers 202), and
// POST /api/v1/apps/enable?id=<slug>&on=0|1 (h_app_enable — the owner's toggle).
// D36's hub seed is their only hub-side caller: the hub writes the starter pair
// onto the card and installs it exactly the way the device's own /apps page does,
// so there is one install path and the seed cannot drift from it.

import (
	"encoding/json"
	"fmt"
	"net/url"
	"strings"
)

// AppCardRow is one .orbapp sitting on the card — h_apps_list's "apps" array.
// Why carries the walk's refusal when OK is false; Stem is the only handle the
// install door takes.
type AppCardRow struct {
	Stem string `json:"stem"`
	Slug string `json:"slug"`
	OK   bool   `json:"ok"`
	Why  string `json:"why"`
}

// AppInstalledRow is one live slot — h_apps_list's "installed" array. Since D36
// no row carries a shipping label: the ledger lists rows, and where the bytes
// came from is nobody of the dial's business.
//
// Caps is the device's own disclosure string, comma-space separated in
// orb_cap_t order — e.g. "power, net, themes". The firmware already sends it
// (h_apps_list prints orb_app_manifest_caps into the row); the hub simply
// stopped reading it while the only theme-aware app on earth shipped inside the
// firmware and asking the question had one obvious answer. D37 asks it per app,
// so the field is now load-bearing: it is what makes "which apps on this orb
// can hold themes" a device answer instead of a hub constant.
type AppInstalledRow struct {
	Slug    string `json:"slug"`
	Title   string `json:"title"`
	Slot    int    `json:"slot"`
	Caps    string `json:"caps"`
	Enabled bool   `json:"enabled"`
	Failed  bool   `json:"failed"`
}

// HasCap reports whether one installed row declares a named capability, e.g.
// "themes". It splits on the device's own ", " separator and compares whole
// words, so a cap name that is a substring of another cannot match it.
func (a AppInstalledRow) HasCap(name string) bool {
	for _, c := range strings.Split(a.Caps, ",") {
		if strings.TrimSpace(c) == name {
			return true
		}
	}
	return false
}

// AppList is the part of GET /api/v1/apps the seed reads.
type AppList struct {
	Mounted   bool              `json:"mounted"`
	Count     int               `json:"count"`
	Apps      []AppCardRow      `json:"apps"`
	Installed []AppInstalledRow `json:"installed"`
}

// AppsList fetches GET /api/v1/apps.
func (c *Client) AppsList(ip string) (*AppList, error) {
	raw, err := c.deviceGet(ip, "/api/v1/apps")
	if err != nil {
		return nil, err
	}
	var l AppList
	if err := json.Unmarshal(raw, &l); err != nil {
		return nil, fmt.Errorf("orbnet: %s sent an app list that is not JSON: %w", ip, err)
	}
	if l.Apps == nil {
		l.Apps = []AppCardRow{}
	}
	if l.Installed == nil {
		l.Installed = []AppInstalledRow{}
	}
	return &l, nil
}

// AppInstallCard queues the card-to-slot copy for one stem (POST
// /api/v1/apps/install?id=). A nil error means the job was queued; the ledger
// shows the row when the worker lands it.
func (c *Client) AppInstallCard(ip, stem string) (json.RawMessage, error) {
	if stem == "" {
		return nil, fmt.Errorf("orbnet: refusing an install with no stem")
	}
	return c.devicePost(ip, "/api/v1/apps/install?id="+url.QueryEscape(stem), nil, "")
}

// AppEnable writes the owner's enable word (POST /api/v1/apps/enable?id=&on=).
func (c *Client) AppEnable(ip, slug string, on bool) (json.RawMessage, error) {
	if slug == "" {
		return nil, fmt.Errorf("orbnet: refusing an enable with no slug")
	}
	onv := "0"
	if on {
		onv = "1"
	}
	return c.devicePost(ip, "/api/v1/apps/enable?id="+url.QueryEscape(slug)+"&on="+onv, nil, "")
}

// MaxAppUpload mirrors ORB_APP_UPLOAD_MAX (0x40000+0x20000): the door refuses on
// the declared length before a byte reaches the card, so the client states the
// same ceiling rather than learning it from a 413.
const MaxAppUpload = 0x40000 + 0x20000

// AppUpload pushes .orbapp bytes to POST /api/v1/sd/app[?overwrite=1]. There is
// no name argument for the same reason ThemeUpload has none: the destination is
// the slug inside the container, and the firmware files it there. overwrite is
// the one client decision, and the seed sends it — a starter file already on a
// re-seeded card is the hub's own earlier write, and replacing it with the very
// bytes the door refused is the idempotent answer.
func (c *Client) AppUpload(ip string, body []byte, overwrite bool) (json.RawMessage, error) {
	if len(body) == 0 {
		return nil, fmt.Errorf("orbnet: refusing to upload an empty app file")
	}
	if len(body) > MaxAppUpload {
		return nil, fmt.Errorf("orbnet: app file is %d B, over the device's %d B ceiling", len(body), MaxAppUpload)
	}
	route := "/api/v1/sd/app"
	if overwrite {
		route += "?overwrite=1"
	}
	return c.devicePost(ip, route, body, "application/octet-stream")
}
