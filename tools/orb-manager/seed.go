package main

import (
	"bytes"
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// A released hub is one executable. The shipped shelf (the first-party apps and the default
// theme a new orb wears) is compiled in as seed stock, and at launch it is copied into the
// user's own library, <user config>/orb-manager/{apps,themes}, which the hub then reads and
// writes like any other library directory. The directory stays the library (library.go):
// what the Library pane lists is what an install sends, and an operator who drops a file in
// has changed what the hub installs with no rebuild.
//
// Seeding only ever feeds that one rung. A -library flag, a library beside the executable
// (make manager's dev layout) or a clone's tree wins the ladder as before, and nothing is
// written to the user config dir on their behalf.
//
// The ledger, .seeded.json beside apps/ and themes/, is what makes the seed polite. It
// records each file the hub placed and the sha256 of the bytes it wrote, so a later launch
// can tell three cases apart that the directory alone cannot:
//
//   - a file the hub placed and the owner deleted: never put back. Deleting from the
//     library is the owner's decision, and a respawn would overrule it every launch.
//   - a file the hub placed and nobody touched: a newer release may replace it (an app
//     only at a higher version, so a release never downgrades; a theme whenever its bytes
//     moved, since a theme carries no version to compare).
//   - a file the owner changed, or had before the hub ever seeded: theirs, never touched.
//
// Deleting the whole user library, ledger included, is the way back to the shipped shelf.

//go:embed apps/*.orbapp themes/*.orbtheme
var shippedLib embed.FS

// seedLedgerName is the ledger's file name inside the user library root.
const seedLedgerName = ".seeded.json"

// seedLedger maps "apps/clock.orbapp" to the sha256 of the bytes the hub last wrote there.
// An empty hash marks a name the owner held before the hub seeded it: theirs for good.
type seedLedger map[string]string

func sha256Hex(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

// seedUserLibrary copies the shipped shelf (src, laid out as apps/ and themes/) into
// userLib for the kinds named, under the ledger's rules, and answers one line per file it
// wrote. An unreadable ledger is reported and read as empty, which costs at most one
// respawn of a deleted file, never an overwrite: an existing file the ledger does not know
// is still treated as the owner's.
func seedUserLibrary(src fs.FS, userLib string, kinds []string) ([]string, error) {
	var did []string
	led, lerr := loadSeedLedger(userLib)
	if lerr != nil {
		did = append(did, lerr.Error())
	}
	changed := false
	for _, kind := range kinds {
		ents, err := fs.ReadDir(src, kind)
		if err != nil {
			continue
		}
		dst := filepath.Join(userLib, kind)
		if err := os.MkdirAll(dst, 0o755); err != nil {
			return did, fmt.Errorf("the user library %s cannot be made: %w", dst, err)
		}
		for _, e := range ents {
			if e.IsDir() {
				continue
			}
			key := path.Join(kind, e.Name())
			body, err := fs.ReadFile(src, key)
			if err != nil {
				did = append(did, fmt.Sprintf("skipped %s: %v", key, err))
				continue
			}
			to := filepath.Join(dst, e.Name())
			have, herr := os.ReadFile(to)
			exists := herr == nil
			rec, known := led[key]
			write := false
			switch {
			case !known && !exists:
				write = true
			case !known && bytes.Equal(have, body):
				led[key], changed = sha256Hex(body), true
			case !known:
				// There before the ledger: an older shipped app is still upgraded, anything
				// else is the owner's from now on.
				if kind == libAppsSubdir && appIsNewer(body, have) {
					write = true
				} else {
					led[key], changed = "", true
				}
			case !exists, rec == "", sha256Hex(have) != rec, bytes.Equal(have, body):
				// deleted, the owner's, changed by the owner, or already current
			case kind == libAppsSubdir:
				write = appIsNewer(body, have)
			default:
				write = true
			}
			if !write {
				continue
			}
			if err := writeFileAtomic(to, body); err != nil {
				did = append(did, fmt.Sprintf("skipped %s: %v", key, err))
				continue
			}
			// A1 §7.3, the third writer of a `<slug>.orbapp`: a seeded app is a shipped app and so
			// is never site-sourced. A record the store left beside a slug this seed overwrote
			// would describe bytes the site did not send, so it comes down. Recorded as a refusal
			// rather than aborting the seed, the way the unreadable ledger is: the bytes are right
			// and only their provenance note is left standing.
			if kind == libAppsSubdir && strings.EqualFold(filepath.Ext(e.Name()), ".orbapp") {
				if err := removeAppOrigin(to); err != nil {
					did = append(did, fmt.Sprintf("seeded %s with an origin record standing beside it: %v", key, err))
				}
			}
			led[key], changed = sha256Hex(body), true
			did = append(did, fmt.Sprintf("seeded %s into %s", key, userLib))
		}
	}
	if changed {
		if err := led.save(userLib); err != nil {
			return did, err
		}
	}
	return did, nil
}

// loadSeedLedger reads userLib's ledger. A missing one is empty; an unreadable one is
// empty too, with the error saying so.
func loadSeedLedger(userLib string) (seedLedger, error) {
	p := filepath.Join(userLib, seedLedgerName)
	led := seedLedger{}
	b, err := os.ReadFile(p)
	if err != nil {
		return led, nil
	}
	if err := json.Unmarshal(b, &led); err != nil {
		return seedLedger{}, fmt.Errorf("the seed ledger %s is unreadable (%v); starting a new one", p, err)
	}
	return led, nil
}

func (led seedLedger) save(userLib string) error {
	p := filepath.Join(userLib, seedLedgerName)
	b, _ := json.MarshalIndent(led, "", "  ")
	if err := writeFileAtomic(p, append(b, '\n')); err != nil {
		return fmt.Errorf("the seed ledger %s cannot be written: %w", p, err)
	}
	return nil
}

// RestorableApp is a compiled-in app the library lacks, or holds only at an older
// version: what the Apps face's "Restore bundled apps" press would put back. Have is the
// library's version, "" when the app is not there at all.
type RestorableApp struct {
	Slug    string `json:"slug"`
	Title   string `json:"title"`
	Version string `json:"version"`
	Have    string `json:"have,omitempty"`
}

// restorableApps compares the shipped apps in src with the library's parsed rows. A file
// the library refused to parse counts as absent, so a restore replaces a broken copy. A
// library copy at the same or a newer version, or one that cannot be compared, is the
// owner's and is not offered.
func restorableApps(src fs.FS, have []LibraryFile) []RestorableApp {
	byslug := map[string]LibraryFile{}
	for _, f := range have {
		byslug[f.Slug] = f
	}
	out := []RestorableApp{}
	ents, _ := fs.ReadDir(src, libAppsSubdir)
	for _, e := range ents {
		body, err := fs.ReadFile(src, path.Join(libAppsSubdir, e.Name()))
		if err != nil {
			continue
		}
		c, err := orbnet.ParseApp(body)
		if err != nil {
			continue
		}
		r := RestorableApp{Slug: c.Slug, Title: c.Title, Version: c.Version}
		if f, ok := byslug[c.Slug]; ok {
			if cmp, ok := orbnet.AppVersionCompare(c.Version, f.Version); !ok || cmp <= 0 {
				continue
			}
			r.Have = f.Version
		}
		out = append(out, r)
	}
	return out
}

// restoreShippedApps files every app restorableApps offers into the library's writable apps
// dir, replacing a broken or older copy, and answers the slugs it filed. When that dir is
// the seeded user library (ledgerRoot, "" otherwise), each restored file is recorded as
// seeded again, so a later release keeps it current like any untouched shipped copy.
func restoreShippedApps(src fs.FS, lib *Library, ledgerRoot string) ([]string, error) {
	payload := &LibraryPayload{}
	if lib.AppsDir != "" {
		listAppLibrary(lib.AppsDir, payload)
	}
	want := map[string]bool{}
	for _, r := range restorableApps(src, payload.Apps) {
		want[r.Slug] = true
	}
	var led seedLedger
	if ledgerRoot != "" {
		led, _ = loadSeedLedger(ledgerRoot)
	}
	restored := []string{}
	ents, _ := fs.ReadDir(src, libAppsSubdir)
	for _, e := range ents {
		body, err := fs.ReadFile(src, path.Join(libAppsSubdir, e.Name()))
		if err != nil {
			continue
		}
		c, err := orbnet.ParseApp(body)
		if err != nil || !want[c.Slug] {
			continue
		}
		dest, err := lib.KeepOver(c.Slug, body, true)
		if err != nil {
			return restored, err
		}
		// A1 §7.3: a bundled app is never site-sourced. A record the store left beside a slug this
		// restore just wrote would describe bytes the site did not send, so it comes down with
		// them — and having no record to remove, which is every bundled app's case, is success.
		if err := removeAppOrigin(dest); err != nil {
			return restored, fmt.Errorf("%s was restored with a site record standing beside it: %w", dest, err)
		}
		restored = append(restored, c.Slug)
		if led != nil {
			led[path.Join(libAppsSubdir, c.Slug+".orbapp")] = sha256Hex(body)
		}
	}
	if led != nil && len(restored) > 0 {
		if err := led.save(ledgerRoot); err != nil {
			return restored, err
		}
	}
	return restored, nil
}

// appIsNewer answers whether container body carries a higher app version than have. An
// unparsable side or an incomparable pair is "no": the seed never replaces on a guess.
func appIsNewer(body, have []byte) bool {
	nb, e1 := orbnet.ParseApp(body)
	if e1 != nil {
		return false
	}
	ob, e2 := orbnet.ParseApp(have)
	if e2 != nil {
		// A copy of an older FORMAT cannot be read to compare, and no firmware this hub
		// installs onto can run it: the shipped copy supersedes it. Anything else that will
		// not parse is not ours to judge and stays.
		return orbnet.AppFormatOlder(have)
	}
	c, ok := orbnet.AppVersionCompare(nb.Version, ob.Version)
	return ok && c > 0
}

func writeFileAtomic(to string, body []byte) error {
	tmp := to + ".seed"
	if err := os.WriteFile(tmp, body, 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, to); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// seedKinds names the kinds the user library should be seeded for: those the ladder,
// resolved without any seeding, would answer from the user config dir or from nowhere.
// A flag, a library beside the executable or a clone keeps its kind unseeded.
func seedKinds(lib *Library) []string {
	var kinds []string
	for _, k := range []struct{ kind, source string }{
		{libAppsSubdir, lib.AppsSource},
		{libThemesSubdir, lib.ThemesSource},
	} {
		if k.source == "the user config dir" || k.source == "none found" {
			kinds = append(kinds, k.kind)
		}
	}
	return kinds
}
