package main

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// A signed macOS app bundle (Orb Manager.app) carries the shipped library in
// Contents/Resources/{apps,themes}. The hub never reads or writes it there: writing into a
// signed bundle breaks its signature, and the next launch is refused by Gatekeeper. The
// bundle's library is seed stock instead. At launch it is copied into the user's own
// library (the user-config rung of libLadder), which the hub then reads and writes as any
// other library: an app or theme the user lacks is copied in, and an app the bundle ships
// at a NEWER version replaces the user's older copy, so updating the app updates the shelf.
// A theme the user already has is left alone (it may be the user's own, under that name).
// Windows and Linux ship as plain folders beside the binary, which libLadder already reads.

// bundleResources answers the Resources directory of the macOS app bundle the binary runs
// from, or "" when it does not run from one (every other platform, and a bare binary).
func bundleResources(exeDir string) string {
	if !strings.HasSuffix(filepath.ToSlash(exeDir), ".app/Contents/MacOS") {
		return ""
	}
	return filepath.Join(filepath.Dir(exeDir), "Resources")
}

// seedFromBundle copies the bundle's library into userLib (<user config>/orb-manager),
// returning one line per file it copied. A kind the bundle does not carry is skipped; an
// unreadable or unparsable bundled app is reported and skipped, never half-copied.
func seedFromBundle(res, userLib string) ([]string, error) {
	var did []string
	for _, kind := range []string{libAppsSubdir, libThemesSubdir} {
		src := filepath.Join(res, kind)
		ents, err := os.ReadDir(src)
		if err != nil {
			continue
		}
		dst := filepath.Join(userLib, kind)
		if err := os.MkdirAll(dst, 0o755); err != nil {
			return did, fmt.Errorf("the user library %s cannot be made: %w", dst, err)
		}
		for _, e := range ents {
			if e.IsDir() || strings.HasPrefix(e.Name(), ".") {
				continue
			}
			from, to := filepath.Join(src, e.Name()), filepath.Join(dst, e.Name())
			body, err := os.ReadFile(from)
			if err != nil {
				did = append(did, fmt.Sprintf("skipped %s: %v", e.Name(), err))
				continue
			}
			have, herr := os.ReadFile(to)
			switch {
			case herr != nil:
				// not in the user's library yet: copy it in
			case bytes.Equal(have, body):
				continue
			case kind == libAppsSubdir:
				// an app: replace only an older release with the bundle's newer one
				nb, e1 := orbnet.ParseApp(body)
				ob, e2 := orbnet.ParseApp(have)
				if e1 != nil || e2 != nil {
					continue
				}
				if c, ok := orbnet.AppVersionCompare(nb.Version, ob.Version); !ok || c <= 0 {
					continue
				}
			default:
				continue // a theme the user already has stays theirs
			}
			tmp := to + ".seed"
			if err := os.WriteFile(tmp, body, 0o644); err != nil {
				did = append(did, fmt.Sprintf("skipped %s: %v", e.Name(), err))
				continue
			}
			if err := os.Rename(tmp, to); err != nil {
				os.Remove(tmp)
				did = append(did, fmt.Sprintf("skipped %s: %v", e.Name(), err))
				continue
			}
			did = append(did, fmt.Sprintf("seeded %s/%s from the app bundle", kind, e.Name()))
		}
	}
	return did, nil
}
