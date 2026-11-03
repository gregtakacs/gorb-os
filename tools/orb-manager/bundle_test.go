package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestBundleResourcesOnlyInsideAnAppBundle(t *testing.T) {
	if got := bundleResources("/Applications/Orb Manager.app/Contents/MacOS"); got != "/Applications/Orb Manager.app/Contents/Resources" {
		t.Errorf("inside a bundle: %q", got)
	}
	for _, d := range []string{"/usr/local/bin", `C:\Program Files\Orb Manager`, "/home/x/orb-manager-linux-amd64", ""} {
		if got := bundleResources(d); got != "" {
			t.Errorf("%q is no app bundle, answered %q", d, got)
		}
	}
}

// The bundle seeds the user's library: what is missing comes in, a newer app replaces an
// older one, and nothing the user already holds is overwritten otherwise.
func TestSeedFromBundle(t *testing.T) {
	appBody := func(name string) []byte {
		b, err := os.ReadFile(filepath.Join("apps", name))
		if err != nil {
			t.Skipf("no shipped %s to seed from: %v", name, err)
		}
		return b
	}
	clock := appBody("clock.orbapp")
	picker := appBody("color_picker.orbapp")
	res, user := t.TempDir(), t.TempDir()
	for k, files := range map[string]map[string][]byte{
		"apps":   {"clock.orbapp": clock, "color_picker.orbapp": picker},
		"themes": {"skeleton.orbtheme": []byte("bundled theme"), "night.orbtheme": []byte("bundled night")},
	} {
		os.MkdirAll(filepath.Join(res, k), 0o755)
		for n, b := range files {
			os.WriteFile(filepath.Join(res, k, n), b, 0o644)
		}
	}
	// the user already has their own night theme, and an app the bundle does not match
	os.MkdirAll(filepath.Join(user, "themes"), 0o755)
	os.WriteFile(filepath.Join(user, "themes", "night.orbtheme"), []byte("the user's own night"), 0o644)
	os.MkdirAll(filepath.Join(user, "apps"), 0o755)
	os.WriteFile(filepath.Join(user, "apps", "color_picker.orbapp"), []byte("not a container"), 0o644)

	lines, err := seedFromBundle(res, user)
	if err != nil {
		t.Fatal(err)
	}
	read := func(p ...string) string {
		b, _ := os.ReadFile(filepath.Join(append([]string{user}, p...)...))
		return string(b)
	}
	if read("apps", "clock.orbapp") != string(clock) || read("themes", "skeleton.orbtheme") != "bundled theme" {
		t.Errorf("missing items were not seeded: %v", lines)
	}
	if read("themes", "night.orbtheme") != "the user's own night" {
		t.Errorf("a theme the user holds was overwritten")
	}
	if read("apps", "color_picker.orbapp") != "not a container" {
		t.Errorf("an app whose version cannot be compared was replaced")
	}
	// a second launch copies nothing
	if again, _ := seedFromBundle(res, user); len(again) != 0 {
		t.Errorf("a second launch seeded again: %v", again)
	}
	if _, err := os.Stat(filepath.Join(res, "apps", ".orb-writable-probe")); err == nil {
		t.Errorf("the bundle was written to")
	}
}

// An app is aged by its container version, not its bytes: a newer bundled release
// replaces the user's older copy, and an older one never downgrades a newer copy. The two
// shipped containers stand in for two releases (clock v1.2.9 is newer than the picker's
// v1.2.2); only the version stamps are compared, so the swapped names do not matter.
func TestSeedFromBundleAgesAppsByVersion(t *testing.T) {
	newer, err1 := os.ReadFile(filepath.Join("apps", "clock.orbapp"))
	older, err2 := os.ReadFile(filepath.Join("apps", "color_picker.orbapp"))
	if err1 != nil || err2 != nil {
		t.Skipf("no shipped apps to seed from: %v %v", err1, err2)
	}
	res, user := t.TempDir(), t.TempDir()
	os.MkdirAll(filepath.Join(res, "apps"), 0o755)
	os.MkdirAll(filepath.Join(user, "apps"), 0o755)
	// up: the bundle ships the newer, the user holds the older
	os.WriteFile(filepath.Join(res, "apps", "up.orbapp"), newer, 0o644)
	os.WriteFile(filepath.Join(user, "apps", "up.orbapp"), older, 0o644)
	// down: the bundle ships the older, the user holds the newer
	os.WriteFile(filepath.Join(res, "apps", "down.orbapp"), older, 0o644)
	os.WriteFile(filepath.Join(user, "apps", "down.orbapp"), newer, 0o644)

	lines, err := seedFromBundle(res, user)
	if err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(filepath.Join(user, "apps", "up.orbapp")); string(b) != string(newer) {
		t.Errorf("a newer bundled app did not replace the older copy: %v", lines)
	}
	if b, _ := os.ReadFile(filepath.Join(user, "apps", "down.orbapp")); string(b) != string(newer) {
		t.Errorf("an older bundled app downgraded the user's newer copy: %v", lines)
	}
	if len(lines) != 1 {
		t.Errorf("want exactly the one upgrade reported, got %v", lines)
	}
}
