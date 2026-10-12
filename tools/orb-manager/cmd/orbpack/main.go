// Command orbpack turns an ESP-IDF build directory into the firmware bundle the
// hub embeds. It runs at release time, on the machine that has the toolchain —
// never on the machine that flashes an orb.
//
//	orbpack -build ../../build -out internal/firmware/bundle/orb-firmware.zip
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
)

func main() {
	build := flag.String("build", "", "ESP-IDF build dir (has flasher_args.json)")
	out := flag.String("out", "", "bundle zip to write")
	flag.Parse()

	if *build == "" || *out == "" {
		fmt.Fprintln(os.Stderr, "orbpack: -build and -out are both required")
		flag.Usage()
		os.Exit(2)
	}
	// The manifest's own paths are the only thing read here, and only at release
	// time. ../assets/clock_assets.bin resolves against the build dir exactly as
	// idf.py means it to, and lands inside the bundle as a plain entry.
	open := func(rel string) ([]byte, error) {
		full, err := safeJoin(*build, rel)
		if err != nil {
			return nil, err
		}
		return os.ReadFile(full)
	}
	manifest, err := os.ReadFile(filepath.Join(*build, firmware.ManifestName))
	if err != nil {
		fmt.Fprintf(os.Stderr, "orbpack: %v — run idf.py build first\n", err)
		os.Exit(1)
	}
	blob, err := firmware.WriteBundle(manifest, open)
	if err != nil {
		fmt.Fprintln(os.Stderr, "orbpack:", err)
		os.Exit(1)
	}
	if err := os.WriteFile(*out, blob, 0o644); err != nil {
		fmt.Fprintln(os.Stderr, "orbpack:", err)
		os.Exit(1)
	}
	// Round-trip through the loader the hub actually uses: a bundle that will not
	// parse is worse than no bundle, and nothing else would notice until a flash.
	b, err := firmware.Load(blob, filepath.Base(*out))
	if err != nil {
		fmt.Fprintln(os.Stderr, "orbpack: wrote a bundle the hub cannot load:", err)
		os.Exit(1)
	}
	fmt.Printf("orbpack: %s — %d image(s), %s %s, %d B -> %s (%d B)\n",
		filepath.Base(*out), len(b.Images), b.Project, b.Version, len(manifest), *out, len(blob))
}

// safeJoin resolves a manifest-relative path under the build dir. The
// ../assets/clock_assets.bin form idf.py uses for the clock art is expected, so
// one level up is allowed; an absolute path, a drive letter, or a traversal past
// that parent is not.
func safeJoin(buildDir, rel string) (string, error) {
	if filepath.IsAbs(rel) || strings.ContainsRune(rel, ':') {
		return "", fmt.Errorf("image path %q must be relative to the build dir", rel)
	}
	root := filepath.Clean(buildDir)
	allowed := filepath.Dir(root)
	full := filepath.Clean(filepath.Join(root, rel))
	if full != root && !within(full, root) && !within(full, allowed) {
		return "", fmt.Errorf("image path %q escapes the build dir", rel)
	}
	return full, nil
}

// within reports whether child sits inside parent, without any ".." left over.
func within(child, parent string) bool {
	rel, err := filepath.Rel(parent, child)
	if err != nil || rel == "." || rel == ".." {
		return false
	}
	return !strings.HasPrefix(rel, ".."+string(filepath.Separator)) && rel != ".."
}
