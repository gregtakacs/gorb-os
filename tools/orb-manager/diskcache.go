package main

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// diskCache is the hub's memory of bytes an orb has already paid to send, kept
// across hub restarts. The orb's one HTTP task is the scarcest thing on the network:
// every theme preview is ~107 KB off the card and a clock editor region is ~670 KB,
// and before this a hub start re-read all of them because the only cache lived in
// process memory for ten minutes (bench, 2026-10-31: a startup read-ahead held the
// orb long enough that the page's zone table and Wi-Fi scan timed out).
//
// Entries are named by what the card says the content IS — a theme's id, bake serial
// and size; an app's slug and release — so a name either holds those exact bytes or
// is absent. Nothing here expires on a clock: a changed theme or a new release is a
// new name, and the old one ages out under the size bound.
//
// The zero value is disabled (no directory), which is what every test hub gets:
// only main() points a cache at the operator's disk.
type diskCache struct {
	dir string
	max int // entries kept; the oldest beyond it are removed on put
}

// cacheName keeps a name to the characters ids, serials and versions use, so a
// device-supplied string can never walk out of the directory.
func cacheName(parts ...string) string {
	var b strings.Builder
	for i, p := range parts {
		if i > 0 {
			b.WriteByte('_')
		}
		for _, r := range p {
			switch {
			case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '-', r == '.':
				b.WriteRune(r)
			default:
				b.WriteByte('-')
			}
		}
	}
	return b.String()
}

func (d diskCache) get(name string) ([]byte, bool) {
	if d.dir == "" || name == "" {
		return nil, false
	}
	b, err := os.ReadFile(filepath.Join(d.dir, name))
	if err != nil || len(b) == 0 {
		return nil, false
	}
	return b, true
}

func (d diskCache) has(name string) bool {
	if d.dir == "" || name == "" {
		return false
	}
	st, err := os.Stat(filepath.Join(d.dir, name))
	return err == nil && st.Size() > 0
}

// put writes through a temp file and a rename, so a reader never sees half an
// entry, then prunes to the bound. Failures are silent: a cache that cannot write
// costs the next read a dial, which is the behaviour before it existed.
func (d diskCache) put(name string, b []byte) {
	if d.dir == "" || name == "" || len(b) == 0 {
		return
	}
	if os.MkdirAll(d.dir, 0o755) != nil {
		return
	}
	p := filepath.Join(d.dir, name)
	tmp := p + ".tmp"
	if os.WriteFile(tmp, b, 0o644) != nil {
		return
	}
	if os.Rename(tmp, p) != nil {
		_ = os.Remove(tmp)
		return
	}
	d.prune()
}

func (d diskCache) prune() {
	if d.max <= 0 {
		return
	}
	ents, err := os.ReadDir(d.dir)
	if err != nil || len(ents) <= d.max {
		return
	}
	type fe struct {
		name string
		mod  int64
	}
	files := make([]fe, 0, len(ents))
	for _, e := range ents {
		if info, err := e.Info(); err == nil && info.Mode().IsRegular() {
			files = append(files, fe{e.Name(), info.ModTime().UnixNano()})
		}
	}
	sort.Slice(files, func(i, j int) bool { return files[i].mod > files[j].mod })
	for _, f := range files[min(d.max, len(files)):] {
		_ = os.Remove(filepath.Join(d.dir, f.name))
	}
}
