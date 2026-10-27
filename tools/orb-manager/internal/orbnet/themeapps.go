package orbnet

// D37's enumeration of theme surfaces (§7). One app per pane on the Themes face,
// and the list of panes is a device answer: an app that can take themes is one the
// LEDGER says is installed, enabled, and declares the `themes` cap. The card's
// theme rows then say how many themes sit under each. The old face enumerated the
// rows, so a theme-aware app with none written yet had no pane at all and looked
// theme-blind to whoever was trying to author its first theme. This is the inversion.
//
// Two device reads, both already implemented and already gated, and no new firmware
// verb: GET /api/v1/apps for the capability, GET /api/v1/themes for the rows. The
// firmware is the only place that knows the capability table; this module does not
// keep a second copy of it, and no app slug is hard-coded here.

import (
	"fmt"
	"sort"
)

// ThemeApp is one entry in the theme enumeration: an installed, enabled app that
// declares the `themes` cap, with the count of theme rows the card holds under it.
type ThemeApp struct {
	Slug   string `json:"slug"`
	Title  string `json:"title"`
	Caps   string `json:"caps"`
	Count  int    `json:"count"`
	Active bool   `json:"active"`
	Failed bool   `json:"failed"`
	// HasEditor reports whether an editor opens for this app. A device that reports an
	// editor region decides it — D37's premise is that the payload travels inside the
	// container. Where none is reported the hub falls back to any editor it hosts for the
	// slug (editorAvailability), because an app with no region can still have a working
	// editor, and this field answers "can I author themes for this app here".
	HasEditor bool   `json:"has_editor"`
	EditorURL string `json:"editor_url,omitempty"`
	// Drift names the identity words on which the orb's installed container and the
	// container the hub is serving disagree, empty when they match. It needs a copy on
	// the hub's side to compare against, so it stays empty wherever Client.Containers is
	// nil — a hub holding no first-party container has no opinion about freshness, and
	// saying "up to date" would be the same lie in the other direction.
	Drift      []string `json:"drift,omitempty"`
	Comparable bool     `json:"comparable"`
}

// ThemeApps is the payload GET /api/theme/apps returns.
type ThemeApps struct {
	IP    string     `json:"ip"`
	Apps  []ThemeApp `json:"apps"`
	Mount bool       `json:"mounted"`
	// AppErr carries the app-ledger read's failure without discarding the theme
	// rows, and vice versa. A half-answer beats a lost pane: the face can still
	// show what the card holds while it says the ledger could not be read.
	AppErr string `json:"app_error,omitempty"`
	// ThemeErr is the same concession on the other half.
	ThemeErr string `json:"theme_error,omitempty"`
	Active   string `json:"active,omitempty"`
	FreeMB   uint32 `json:"free_mb,omitempty"`
}

// ThemeAppsFor reads both halves and intersects them. A device with no SD card is
// not an error here: it answers with the apps and zero counts, which is exactly
// the state an author standing on a bare board needs to see.
func (c *Client) ThemeAppsFor(ip string) (*ThemeApps, error) {
	out := &ThemeApps{IP: ip, Apps: []ThemeApp{}}
	apps, appErr := c.AppsList(ip)
	if appErr != nil {
		out.AppErr = appErr.Error()
	} else {
		out.Mount = apps.Mounted
	}
	list, themeErr := c.ThemeList(ip)
	if themeErr != nil {
		out.ThemeErr = themeErr.Error()
	} else {
		out.Active = list.Active
		out.FreeMB = list.FreeMB
	}
	if appErr != nil && themeErr != nil {
		return nil, fmt.Errorf("theme enumeration: %s; and themes: %s", out.AppErr, out.ThemeErr)
	}

	// Count rows per owning app, including rows filed under an app the ledger does
	// not list — an orphan. Those themes are real bytes on the card, and a face that
	// only showed ledger apps would hide them with nowhere to delete them from.
	type appGroup struct {
		count  int
		active bool
	}
	groups := map[string]*appGroup{}
	if list != nil {
		for _, t := range list.Themes {
			app := t.App
			if app == "" {
				// A device too old to answer `app` on a row. The stems this
				// firmware files are all clock's, so attributing the row is a
				// fact about the device, not a guess about the theme.
				app = "clock"
			}
			g := groups[app]
			if g == nil {
				g = &appGroup{}
				groups[app] = g
			}
			if !t.OK {
				continue
			}
			g.count++
			if t.Active {
				g.active = true
			}
		}
	}

	if apps != nil {
		for _, a := range apps.Installed {
			if !a.HasCap("themes") {
				continue
			}
			g := groups[a.Slug]
			ta := ThemeApp{
				Slug: a.Slug, Title: a.Title, Caps: a.Caps, Failed: a.Failed,
			}
			if g != nil {
				ta.Count, ta.Active = g.count, g.active
				delete(groups, a.Slug)
			}
			ta.HasEditor, ta.EditorURL = editorAvailability(a.Slug)
			// A nonzero editor_crc is the device reporting a region it verified on the
			// card, and that beats any hub-side table: the app carries its own editor, so
			// the pane opens on the app's bytes and not on a hub copy that may be older
			// than the app. The URL carries the dial's address because the hub's loader
			// route is /editor/<ip>/<slug>/ — a path built from the slug alone names no
			// board, and the first real click of the slice found exactly that: the pane
			// asked, the route read "clock" as an address, and the editor never opened.
			//
			// A zero one does NOT take the pane away. "No region in this container" and
			// "no editor anywhere for this app" are different answers, and the fallback
			// carries a dial whose firmware predates the identity fields — its row cannot
			// answer, and the hub still knows the one editor that predates the whole
			// system. A board at the tuple firmware with a genuinely regionless container
			// answers zero, and editorFromRow's `answered` split keeps that case honest.
			if ed, answered := editorFromRow(a); answered && ed {
				ta.HasEditor, ta.EditorURL = true, "/editor/"+ip+"/"+a.Slug+"/"
			}
			if c.Containers != nil {
				if body, err := c.Containers(a.Slug); err == nil {
					if mine, perr := ParseApp(body); perr == nil {
						ta.Drift, ta.Comparable = mine.IdentityDiff(a)
					}
				}
			}
			out.Apps = append(out.Apps, ta)
		}
	}
	// Whatever is left is a card group with no ledger row. Show it, uneditable,
	// so the themes are reachable and deletable.
	for slug, g := range groups {
		out.Apps = append(out.Apps, ThemeApp{Slug: slug, Title: slug, Count: g.count, Active: g.active})
	}
	sort.Slice(out.Apps, func(i, j int) bool { return out.Apps[i].Slug < out.Apps[j].Slug })
	return out, nil
}

// editorAvailability answers, hub-side, whether an editor can open for one app and
// at what URL. This is a stopgap with an explicit end: the moment an orb reports an
// editor region from its container (§4), the answer comes from the device and this
// function is deleted. It exists so D37's inversion ships without waiting on the
// firmware slice, and it stays honest by returning a URL the hub can actually serve.
// editorFromRow reads an app's editor presence off the row's identity tuple, and reports
// whether the row could answer at all. editor_crc is the region's checksum, and the writer
// refuses a container that declares a region whose checksum is zero, so a nonzero word here
// is a device saying "this app ships an authoring payload". A row with no tuple (a compiled
// app, or a dial predating the fields) answers neither way, and the caller must not read its
// zero as "no editor".
func editorFromRow(a AppInstalledRow) (has bool, answered bool) {
	if !a.CarriesIdentity() {
		return false, false
	}
	return a.EditorCRC != 0, true
}

// editorAvailability answers, hub-side, whether an editor can open for one app and at what
// URL.
//
// D37 made the truthful answer a device answer — the row's editor_crc says whether a
// container carries a region — and that path goes through editorFromRow above. This is the
// fallback for a row that could not answer: a dial predating the identity fields, where the
// hub still knows the one editor that predates the whole system. It is a stopgap with an
// explicit end: when every dial in the fleet reports a tuple, delete this and take the
// device's answer unconditionally.
func editorAvailability(slug string) (has bool, url string) {
	if slug == "clock" {
		return true, "/composer/"
	}
	return false, ""
}
