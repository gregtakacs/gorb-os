// fm.go — the v1.1.18 file-manager doors and the LAN factory door.
//
// The double keeps a tiny in-memory volume instead of a real FAT: what the
// hub and the page need to exercise is the CONTRACT (validation sentences,
// refusal codes, the overwrite and recursive words), and every byte of true
// FAT behaviour is already pinned at the device layer where it lives. The
// rules below mirror orb_sd.c's fm_* primitives deliberately closely — a fake
// that accepted what the device refuses turns every UI test into a lie.
package orbfake

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"regexp"
	"sort"
	"strings"
)

type fakeFS struct {
	files map[string]int64 // rel file paths -> declared size
	dirs  map[string]bool  // rel dir paths; the root "" is always there
}

func newFakeFS() *fakeFS {
	return &fakeFS{files: map[string]int64{}, dirs: map[string]bool{}}
}

var fmSeg = regexp.MustCompile(`^[A-Za-z0-9._-]{1,55}$`)

// fmRelOk mirrors orb_sd_fm_path's MUTATION rules. The read side's looser
// character class exists for names an MTP host wrote, and this volume has none
// the tests cannot name — one validator here is the honest simplification.
func fmRelOk(rel string) bool {
	if len(rel) > 127 || rel == "" {
		return false
	}
	if strings.HasPrefix(rel, "/") || strings.HasSuffix(rel, "/") || strings.Contains(rel, "//") {
		return false
	}
	segs := strings.Split(rel, "/")
	if len(segs) > 8 {
		return false
	}
	for _, s := range segs {
		if !fmSeg.MatchString(s) || s[0] == '.' {
			return false
		}
	}
	if segs[0] == "apps" || segs[0] == "themes" {
		return false // the stores keep their own doors
	}
	return true
}

func fmBadPath(w http.ResponseWriter) {
	writeJSON(w, 400, `{"error":"bad path"}`)
}

func (o *Orb) serveFactory(w http.ResponseWriter, r *http.Request) {
	if r.URL.Query().Get("confirm") != "ALL" {
		writeJSON(w, 400, `{"error":"confirm must be ALL"}`)
		return
	}
	o.mu.Lock()
	d := o.deviceLocked()
	d.Name = "The Orb" // the word a factory reset leaves behind
	d.TZ = ""
	o.mu.Unlock()
	writeJSON(w, 200, `{"state":"reset","wipe":"ok","reboot_in_ms":750}`)
}

type fmEnt struct {
	Name string `json:"name"`
	Size int64  `json:"size"`
	Dir  bool   `json:"dir"`
}

// relJoin is the stored-path join for a directory's children: the root is "",
// so a child of root is just its name.
func relJoin(dir, name string) string {
	if dir == "" {
		return name
	}
	return dir + "/" + name
}

func (o *Orb) serveFmList(w http.ResponseWriter, r *http.Request) {
	rel := r.URL.Query().Get("path")
	if rel != "" && !fmRelOk(rel) {
		fmBadPath(w)
		return
	}
	o.mu.Lock()
	fs := o.fm
	var ents []fmEnt
	found := fs.dirs[rel] || rel == "" // the root always exists
	if found {
		prefix := rel
		if prefix != "" {
			prefix += "/"
		}
		seen := map[string]bool{}
		for name := range fs.dirs {
			if name == "" || !strings.HasPrefix(name, prefix) {
				continue
			}
			child := strings.TrimPrefix(name, prefix)
			if child == "" || strings.Contains(child, "/") || seen[child] {
				continue
			}
			seen[child] = true
			ents = append(ents, fmEnt{Name: child, Dir: true})
		}
		for p, size := range fs.files {
			if !strings.HasPrefix(p, prefix) {
				continue
			}
			child := strings.TrimPrefix(p, prefix)
			if child == "" || strings.Contains(child, "/") {
				continue
			}
			ents = append(ents, fmEnt{Name: child, Size: size})
		}
	}
	o.mu.Unlock()
	if !found {
		writeJSON(w, 404, `{"error":"no such directory"}`)
		return
	}
	sort.Slice(ents, func(i, j int) bool {
		if ents[i].Dir != ents[j].Dir {
			return ents[i].Dir
		}
		return ents[i].Name < ents[j].Name
	})
	rows, _ := json.Marshal(ents)
	if ents == nil {
		rows = []byte("[]")
	}
	writeJSON(w, 200, fmt.Sprintf(`{"path":%q,"entries":%s,"truncated":false}`, rel, rows))
}

func (o *Orb) serveFmMkdir(w http.ResponseWriter, r *http.Request) {
	rel := r.URL.Query().Get("path")
	if rel == "" {
		writeJSON(w, 400, `{"error":"path is missing"}`)
		return
	}
	if !fmRelOk(rel) {
		fmBadPath(w)
		return
	}
	o.mu.Lock()
	fs := o.fm
	for p := range fs.files {
		if p == rel || strings.HasPrefix(p, rel+"/") {
			o.mu.Unlock()
			writeJSON(w, 409, `{"error":"a file is in the way"}`)
			return
		}
	}
	segs := strings.Split(rel, "/")
	walk := ""
	for _, s := range segs {
		walk = relJoin(walk, s)
		fs.dirs[walk] = true
	}
	o.mu.Unlock()
	writeJSON(w, 200, fmt.Sprintf(`{"state":"created","path":%q}`, rel))
}

func (o *Orb) serveFmWrite(w http.ResponseWriter, r *http.Request) {
	rel := r.URL.Query().Get("path")
	if rel == "" {
		writeJSON(w, 400, `{"error":"path is missing"}`)
		return
	}
	if !fmRelOk(rel) {
		fmBadPath(w)
		return
	}
	if r.ContentLength <= 0 {
		writeJSON(w, 400, `{"error":"the orb's file write needs a declared Content-Length"}`)
		return
	}
	if r.ContentLength > 128<<20 {
		writeJSON(w, 413, fmt.Sprintf(`{"error":"too large","max":%d}`, 128<<20))
		return
	}
	overwrite := r.URL.Query().Get("overwrite") == "1"
	o.mu.Lock()
	fs := o.fm
	_, exists := fs.files[rel]
	parent := ""
	if i := strings.LastIndex(rel, "/"); i >= 0 {
		parent = rel[:i]
	}
	parentOK := parent == "" || fs.dirs[parent]
	if !overwrite && exists {
		o.mu.Unlock()
		writeJSON(w, 409, `{"error":"it exists — pass overwrite to replace it"}`)
		return
	}
	if !parentOK {
		o.mu.Unlock()
		writeJSON(w, 404, `{"error":"a parent directory is missing"}`)
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, r.ContentLength+1))
	if err != nil || int64(len(body)) > r.ContentLength {
		o.mu.Unlock()
		writeJSON(w, 400, `{"error":"upload aborted by client"}`)
		return
	}
	fs.files[rel] = int64(len(body))
	o.mu.Unlock()
	writeJSON(w, 200, fmt.Sprintf(`{"state":"written","path":%q,"bytes":%d}`, rel, len(body)))
}

func (o *Orb) serveFmRm(w http.ResponseWriter, r *http.Request) {
	rel := r.URL.Query().Get("path")
	if rel == "" {
		writeJSON(w, 400, `{"error":"path is missing"}`)
		return
	}
	if !fmRelOk(rel) {
		fmBadPath(w)
		return
	}
	recursive := r.URL.Query().Get("recursive") == "1"
	o.mu.Lock()
	fs := o.fm
	if fs.dirs[rel] {
		prefix := rel + "/"
		var kids []string
		for d := range fs.dirs {
			if d != rel && strings.HasPrefix(d, prefix) {
				kids = append(kids, d)
			}
		}
		for f := range fs.files {
			if strings.HasPrefix(f, prefix) {
				kids = append(kids, f)
			}
		}
		if len(kids) > 0 && !recursive {
			o.mu.Unlock()
			writeJSON(w, 409, `{"error":"not empty — pass recursive to take the contents too"}`)
			return
		}
		if len(kids) > 1000 {
			o.mu.Unlock()
			writeJSON(w, 400, `{"error":"that tree is bigger than one rm may take"}`)
			return
		}
		for _, k := range kids {
			delete(fs.files, k)
			delete(fs.dirs, k)
		}
		delete(fs.dirs, rel)
		o.mu.Unlock()
		writeJSON(w, 200, fmt.Sprintf(`{"state":"deleted","path":%q,"removed":%d}`, rel, len(kids)+1))
		return
	}
	if _, isFile := fs.files[rel]; isFile {
		delete(fs.files, rel)
		o.mu.Unlock()
		writeJSON(w, 200, fmt.Sprintf(`{"state":"deleted","path":%q,"removed":1}`, rel))
		return
	}
	o.mu.Unlock()
	writeJSON(w, 404, `{"error":"no such file"}`)
}
