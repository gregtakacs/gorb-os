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
	// HasEditor reports whether an editor ships for this app. D37's whole premise
	// is that the editor travels inside the app's container, so the truthful answer
	// comes from a container region that firmware does not ship until GEOM_ABI 3
	// (§5). Until an orb reports one, the hub serves the editor it already has for
	// the app whose editor predates the system — see editorAvailability below.
	HasEditor bool   `json:"has_editor"`
	EditorURL string `json:"editor_url,omitempty"`
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
func editorAvailability(slug string) (bool, string) {
	if slug == "clock" {
		return true, "/composer/"
	}
	return false, ""
}
