package orbnet

// The device-side APP doors, quoted from main/net/orb_net.c: GET /api/v1/apps
// (h_apps_list), POST /api/v1/sd/app (h_app_upload — the raw-bytes twin of
// ThemeUpload, named by the container's own header), POST /api/v1/apps/install?id=<stem>
// (h_apps_install — queues the card-to-slot job and answers 202), and
// POST /api/v1/apps/enable?id=<slug>&on=0|1 (h_app_enable — the owner's toggle).
// D36's hub seed was their only hub-side caller; S5 deleted the seed route and
// left these as the library surface for the same doors the Apps face's drop
// zone already dials. One install path, unchanged — only the hub-side presser
// of it is gone.

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
)

// AppCardRow is one .orbapp sitting on the card — h_apps_list's "apps" array.
// Why carries the walk's refusal when OK is false; Stem is the only handle the
// install door takes. Version is the container's own release stamp (D38, the
// card tier's seventh word) — the number the Apps face ages against the shelf
// to decide whether the Update button is owed. "" is the device's silence on a
// pre-D38 board and an unstamped container alike; the face reads that as "no
// update can be proven", never as "current".
type AppCardRow struct {
	Stem    string `json:"stem"`
	Slug    string `json:"slug"`
	OK      bool   `json:"ok"`
	Why     string `json:"why"`
	Version string `json:"version,omitempty"`
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
//
// The five identity words are D37's addition and they answer a question the rest of
// the row cannot: not "is this app installed" but "is it the build I have". The dial
// reports them for card rows only — a compiled row was never a container and sends no
// such keys, which is why GeomCRC reads 0 there rather than reading as a match. See
// AppContainer.Diff for what a comparison against a hub-side file is actually worth.
type AppInstalledRow struct {
	Slug    string `json:"slug"`
	Title   string `json:"title"`
	Slot    int    `json:"slot"`
	Caps    string `json:"caps"`
	Enabled bool   `json:"enabled"`
	Failed  bool   `json:"failed"`
	// Compiled marks the built-in tier — the one shell row D35 left in the image, which
	// the firmware emits FIRST with this key set (apps_send_installed) and which an owner
	// can neither toggle nor uninstall. The face has filtered on it since §7; the field is
	// here so a Go caller answers "how many apps does this dial carry" the same way the
	// Apps face does, instead of counting the shell as inventory the owner installed. A
	// quarantoned built-in still carries it, and Failed is what makes that row worth shown.
	Compiled bool `json:"compiled,omitempty"`

	GeomCRC   uint32 `json:"geom_crc,omitempty"`
	CodeCRC   uint32 `json:"code_crc,omitempty"`
	CodeSize  uint32 `json:"code_size,omitempty"`
	Serial    uint32 `json:"serial,omitempty"`
	EditorCRC uint32 `json:"editor_crc,omitempty"`
	// Version is S6's seventh tuple word, and the only member that is words. The device
	// omits the key for a compiled row exactly as it omits the six above, so "" here
	// means both "not on the card" and "card row, no stamp" — CarriesIdentity already
	// refuses to read a blank as an opinion, and the pair stays honest because of it.
	Version string `json:"version,omitempty"`
}

// CarriesIdentity reports whether a row plausibly sent the identity tuple. Only the
// installed tier's card rows do, and the distinction is not cosmetic: the firmware OMITS
// the keys for a compiled row, Go decodes the absent numbers as zeros, and a comparison
// against invented zeros would call a built-in clock stale.
//
// Five zeros means "no tuple", which is a judgement and not a proof. A card row CAN hold
// individual zero words — a native app's code_size is 0, an app with an empty geometry
// table has geom_crc 0, and an app shipping no editor has editor_crc 0 — so a container
// with every one of those at once decodes as though it sent nothing. That row reports
// uncomparable, which costs the hub an opinion and never costs it a wrong one: the failure
// mode is "could not check", never "up to date".
func (a AppInstalledRow) CarriesIdentity() bool {
	return a.GeomCRC != 0 || a.CodeCRC != 0 || a.CodeSize != 0 || a.Serial != 0 || a.EditorCRC != 0
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

// AppList is the part of GET /api/v1/apps a card-tier reader needs.
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

// MaxEditorRegion mirrors ORBAPP_EDITOR_MAX: the format caps one app's authoring
// payload at 2 MiB, and the client refuses past it rather than trusting a device's
// Content-Length before it allocates.
const MaxEditorRegion = 0x200000

// ErrNoEditor is a device's 404 on the editor door. That is the ordinary answer
// for every app written before D37 and for every app whose author ships no editor,
// so it is a named value rather than a formatted error: the caller gets to say
// "this app ships no editor" instead of "the orb refused", which are different
// sentences to whoever is reading the card.
var ErrNoEditor = errors.New("this app ships no editor")

// EditorRegion fetches one app's editor region (GET /api/v1/apps/editor?id=) as
// raw bytes. D37 §5.2's read-only door: the firmware streams a container region it
// verified at the store and installs nothing from, and the hub unpacks the ZIP
// these bytes are on the way in.
func (c *Client) EditorRegion(ip, stem string) ([]byte, error) {
	if !AppStemOK(stem) {
		return nil, fmt.Errorf("orbnet: %q is not an app name the device would accept", stem)
	}
	res, err := c.h.Get(c.url(ip, "/api/v1/apps/editor?id="+url.QueryEscape(stem)))
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	if res.StatusCode == http.StatusNotFound {
		return nil, ErrNoEditor
	}
	if res.StatusCode != http.StatusOK {
		b, _ := io.ReadAll(io.LimitReader(res.Body, 4096))
		return nil, fmt.Errorf("orbnet: %s refused the editor read (%d): %s",
			ip, res.StatusCode, strings.TrimSpace(string(b)))
	}
	body, err := io.ReadAll(io.LimitReader(res.Body, int64(MaxEditorRegion)+1))
	if err != nil {
		return nil, err
	}
	if len(body) > MaxEditorRegion {
		return nil, fmt.Errorf("orbnet: %s sent an editor region past the %d B the format caps it at",
			ip, MaxEditorRegion)
	}
	return body, nil
}

// AppStemOK is the character check the device runs on the id before it touches the
// card: a plain name, no separator, nothing that could read as a path. Strict here
// so the hub never dials a route the firmware would refuse anyway.
func AppStemOK(s string) bool {
	if s == "" || len(s) > 55 { // ORB_SD_NAME_MAX
		return false
	}
	for _, r := range s {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9',
			r == '_', r == '-':
		default:
			return false
		}
	}
	return true
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

// MaxAppUpload mirrors ORB_APP_UPLOAD_MAX: the door refuses on the declared length
// before a byte reaches the card, so the client states the same ceiling rather than
// learning it from a 413. D37 grew the device bound by one term — a single editor
// region at ORBAPP_EDITOR_MAX, plus the one sector of padding that can precede it.
// The region never installs, so this raises the largest legal FILE and leaves the
// largest legal slot exactly where P4b priced it.
const MaxAppUpload = 0x40000 + 0x20000 + 0x200000 + 0x1000

// AppUpload pushes .orbapp bytes to POST /api/v1/sd/app[?overwrite=1]. There is
// no name argument for the same reason ThemeUpload has none: the destination is
// the slug inside the container, and the firmware files it there. overwrite is
// the one client decision. A starter file already on a card is an earlier copy
// of the same artifact, and replacing it with the very bytes the door refused
// is the idempotent answer the Apps face's Install press sends.
func (c *Client) AppUpload(ip string, body []byte, overwrite bool) (json.RawMessage, error) {
	_, err := c.AppUploadProgress(ip, body, overwrite, nil)
	return nil, err
}

// AppUploadProgress is AppUpload with a byte-progress callback, and it exists for the same
// reason ThemeUploadProgress does: a FORMAT 3 container is hundreds of kilobytes crossing
// WiFi into a synchronous card write, and every screen in the chain was silent until the
// device finally answered. progress fires with the running total of bytes the transport has
// written, may be nil, and is called from the transfer goroutine only.
//
// The device's own page allows 900 seconds for this POST. A caller holding the 8-second
// management client was timing out on a healthy board at roughly the 50 KB mark and
// reporting the board as broken, so the hub's callers take the bulk client (see
// otaTimeout in main.go) — the same correction h_ota already documents for firmware pushes.
func (c *Client) AppUploadProgress(ip string, body []byte, overwrite bool, progress func(int64)) (json.RawMessage, error) {
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
	return c.devicePostCounted(ip, route, body, "application/octet-stream", progress)
}
