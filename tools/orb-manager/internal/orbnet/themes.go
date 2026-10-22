package orbnet

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/url"
	"regexp"
)

// The device-side THEME API, in one place: what main/net/orb_net.c registers, and
// nothing invented. FORMAT 2 (docs/theme-identity-plan.md) settled what identifies a
// theme, and FORMAT 3 carries those two halves unchanged:
//
//   - the NAME is the theme's one identifier. It lives inside the container's
//     header, the owner set it when they baked the file, and it is what a human
//     reads, labels and searches by.
//   - the ID is theme_id(name): hex(sha256(name))[:12], the stem main/art/orb_theme.c
//     files the card under. It is derived, never typed, and it is the ONLY way a
//     caller may address a card file. orb_theme_resolve_id() finds the file by
//     walking the directory and re-verifying the header name, so a crafted id can
//     select nothing that is not a real, loadable file.
//
// So store and delete carry ?id=, exactly as h_bundle_install and h_theme_del read
// them, and an upload carries no name at all: the bytes carry it and
// orb_theme_upload_end() decides where they land. The only client-controlled upload
// decision is ?overwrite=1, which governs whether a verified file may replace a
// DIFFERENT theme already filed under the same name.
//
// NAMING TRAP, which is why this comment is here: this `id` is a THEME id. The orb's
// own device identity is a different `id` and must arrive as its own parameter. A
// reader who assumes the two are the same will build a client that addresses an orb
// by a theme.

// ThemeEntry is one row of GET /api/v1/sd's "themes" array, as h_sd formats it from
// orb_theme_scan. Name and ID are the same theme seen two ways — the label, and the
// handle every later request must come back with. Why is the reason a row is not
// loadable when OK is false. Changed is the card holding bytes newer than the
// installed slot's, which is the one thing the active tag cannot say on its own: an
// overwrite leaves the name, and therefore the id, alone, so a row can read active
// while the card underneath it has moved.
type ThemeEntry struct {
	Name    string `json:"name"`
	ID      string `json:"id"`
	Why     string `json:"why"`
	Bytes   uint32 `json:"bytes"`
	Serial  uint32 `json:"serial"`
	OK      bool   `json:"ok"`
	Active  bool   `json:"active"`
	Changed bool   `json:"changed"`
	// Stored is this file's row witness that a copy sits in the single flash slot
	// the dial draws (h_sd's "stored"). Migration 1 retired the A/B pair and with
	// it the second press: one press of store IS the theme change, and this bool is
	// what tells the caller the copy landed — the door acks the queue, not the write.
	Stored bool `json:"stored"`
}

// ThemeList is the part of h_sd's reply the card needs. Active is the installed
// theme's NAME, not its id, because it is the sentence the operator reads; Source is
// the firmware's own word for what the glass is drawing ("slot" or "none"). TryID and
// StagedID name by id, because they are 12 hex and the row they belong to carries the
// same 12 hex. Themes is an empty list rather than nil: the client takes .length on
// it, and null has none.
type ThemeList struct {
	Mounted   bool   `json:"mounted"`
	Card      string `json:"card"`
	Uploading bool   `json:"uploading"`
	// JobBusy mirrors orb_jobs_busy: a queued or running theme job. The store and
	// flip doors ack the queue, so this is the only witness that separates "the
	// copy is happening" from "the press went nowhere" while a job is in flight.
	JobBusy bool `json:"job_busy"`

	TotalMB   uint32 `json:"total_mb"`
	FreeMB    uint32 `json:"free_mb"`
	Source    string `json:"source"`
	Active    string `json:"active"`
	CardCount int    `json:"card_count"`
	TryID     string `json:"try_id"`
	TryWhy    string `json:"try_why"`
	// StagedID/StagedWhy are Migration 1's torn-store witness: a store whose pixels
	// reached the slot and whose header never did. Boot draws the base face beside
	// it and one re-store clears it (orb_theme_staged_*). The damage-reporting pair
	// it replaces is retired with the A/B choice: one slot leaves no rotted other
	// half to explain.
	StagedID  string       `json:"staged_id"`
	StagedWhy string       `json:"staged_why"`
	Themes    []ThemeEntry `json:"themes"`
}

// ThemeIDHex is ORB_THEME_ID_HEX: the length of a card filename stem.
const ThemeIDHex = 12

// themeIDRe is orb_theme_id_ok in main/art/orb_theme.c, which admits exactly 12
// chars of [0-9a-f] and nothing else — uppercase, 11 or 13 chars, '/' and "../" all
// refused before the card is touched. The device checks it again on the resolve path;
// this is the edge, so a malformed id is refused here rather than dialled.
var themeIDRe = regexp.MustCompile(`^[0-9a-f]{12}$`)

// CheckThemeID reports whether id is a theme id this device could have issued.
func CheckThemeID(id string) error {
	if !themeIDRe.MatchString(id) {
		return fmt.Errorf("orbnet: %q is not a theme id: it must be the 12 lowercase hex chars the device issued for a theme it is holding", id)
	}
	return nil
}

// ThemeIDOf is the firmware's theme_id() and tools/orbtheme.py's theme_id(): the card
// stem a name yields, hex(sha256(name))[:12] over the UTF-8 bytes up to the NUL, never
// over the padded NAME_MAX array. Use it to show an operator where a file will land,
// and to check a row's id against the name beside it — never to build a card path, and
// never to address a theme the device has not listed. An empty name is refused for the
// firmware's own reason: otherwise every nameless theme hashes to one constant.
func ThemeIDOf(name string) (string, error) {
	if err := CheckThemeName(name); err != nil {
		return "", err
	}
	d := sha256.Sum256([]byte(name))
	return hex.EncodeToString(d[:])[:ThemeIDHex], nil
}

// ThemeNameMax is ORB_THEME_NAME_MAX - 1 from main/art/orb_theme.h: the name field is
// 56 bytes and the last is the NUL the C reader stops at, so 55 bytes is the longest
// name that survives the trip. The unit is BYTES, not characters — "Mirage — Gold" is
// 15 characters and 17 bytes, and cutting by characters is the bug that once made the
// printed card id hash a different string than the header stored.
const ThemeNameMax = 55

// CheckThemeName reports whether name is a theme name the device can hold. It mirrors
// the two canonical rules and invents nothing: tools/orbtheme.py's _setword refuses
// over 55 UTF-8 bytes rather than truncate, because the stored name IS the identity
// and half of it would hash to a card its owner never named; the studio's nameOk adds
// the embedded-NUL refusal, because the reader would stop early and the tail of the
// name would silently not be the name. FORMAT 1's ASCII slug charset is gone on
// purpose: a name is now free text, and "Ünit ☃" is a legal name that the old rule
// would have refused.
func CheckThemeName(name string) error {
	if name == "" {
		return fmt.Errorf("orbnet: a theme must be named: the card files a theme by sha256(name)[:12] and refuses a container with no name in it")
	}
	if len(name) > ThemeNameMax {
		return fmt.Errorf("orbnet: theme name %q is %d UTF-8 bytes; the header holds at most %d", name, len(name), ThemeNameMax)
	}
	for i := 0; i < len(name); i++ {
		if name[i] == 0 {
			return fmt.Errorf("orbnet: theme name %q holds a NUL at byte %d; the device would read the name as ending there", name, i)
		}
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

// ThemeStore queues POST /api/v1/sd/bundle?id= — Migration 1's one store door
// (orb_theme_post_bundle), the only route to a card theme now the FORMAT 2 install door
// is gone. The id is the one that row came back with: a caller cannot name a card file,
// and h_bundle_install answers 400 before the card is touched if it is not 12 lowercase
// hex. A nil error means the store job was queued. The door acks reboot:automatic on
// purpose: under the single slot the store IS the theme change, and the firmware
// reboots onto the new face once the copy verifies. The row's stored witness is where
// the landing shows; the try witness is where a refusal lands.
func (c *Client) ThemeStore(ip, id string) (json.RawMessage, error) {
	if err := CheckThemeID(id); err != nil {
		return nil, err
	}
	return c.devicePost(ip, "/api/v1/sd/bundle?id="+url.QueryEscape(id), nil, "")
}

// ThemeDelete removes a theme from the card by id. It is the only theme call that is
// not a queued job, so its reply is the outcome. Deleting by name is not available:
// the name is free text, and only an id selects one file for certain.
func (c *Client) ThemeDelete(ip, id string) (json.RawMessage, error) {
	if err := CheckThemeID(id); err != nil {
		return nil, err
	}
	return c.devicePost(ip, "/api/v1/sd/delete?id="+url.QueryEscape(id), nil, "")
}

// ThemeRescan asks the device to re-read the card, which is how a theme that arrived
// by another route shows up in the list without rebooting the orb.
func (c *Client) ThemeRescan(ip string) (json.RawMessage, error) {
	return c.devicePost(ip, "/api/v1/sd/rescan", nil, "")
}

// ThemeUpload pushes .orbtheme bytes to POST /api/v1/sd/theme[?overwrite=1].
//
// There is no name argument, because there is no name to give: h_theme_upload takes
// none, and orb_theme_upload_end() files the bytes under the id their OWN header name
// hashes to. A hub that sent ?name= would be saying something about the theme that the
// device ignores — which is precisely how a name typed into a box could drift from the
// name the card ended up filing. Read the name out of the container instead
// (ContainerName) and show the operator where their file will land.
//
// overwrite is the one client-controlled decision: without it a verified file that
// would replace a DIFFERENT theme under the same name comes back 409 with
// {"overwrite":false}, and the caller asks before re-sending with overwrite=true.
//
// The firmware demands a Content-Length and refuses an over-large one before it writes
// a byte, so the body is measured here rather than streamed and truncated in flight.
func (c *Client) ThemeUpload(ip string, body []byte, overwrite bool) (json.RawMessage, error) {
	if len(body) == 0 {
		return nil, fmt.Errorf("orbnet: refusing to upload an empty theme file")
	}
	if len(body) > MaxThemeUpload {
		return nil, fmt.Errorf("orbnet: theme file is %d B, over the device's %d B ceiling", len(body), MaxThemeUpload)
	}
	// The identity gate lives here rather than in the caller, for the reason the whole
	// module states: the hub is where a theme the device cannot file is refused, not
	// somewhere the device has to be trusted to catch it after 2 MB crossed the room.
	if _, err := ContainerName(body); err != nil {
		return nil, err
	}
	route := "/api/v1/sd/theme"
	if overwrite {
		route += "?overwrite=1"
	}
	return c.devicePost(ip, route, body, "application/octet-stream")
}

// MaxThemeUpload is ORB_THEME_UPLOAD_MAX from main/art/orb_theme.h, restated so the
// hub refuses the same size the device would, with a sentence an operator can act on
// instead of a 413 from a device two hops away. Migration 1 grew the single slot from
// 4 MB to 8, and the ceiling with it.
const MaxThemeUpload = 0x00800000 + 0x20000
