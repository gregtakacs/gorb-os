package main

// theme-editor-kit-plan K9: My designs are files on the hub.
//
// An editor's named designs used to live in the browser's localStorage, where a 5 MB
// quota kept their art out and a cleared profile lost them. The owner's ruling of
// 2026-11-04 moves them to the hub's disk beside the library: <root>/designs/<app>/,
// one .orbdesign per name, art included. A file here is the design document the editor
// writes (the same bytes "Download design file" saves), with one block the hub adds,
// "saved": {name, at, extra}, so the listing can name it without the editor's help and
// "Open a file" still reads it as a design.
//
// A name is the key: saving under a name the app already holds replaces that file, so
// the folder holds one design per name, the way the card holds one theme per name.

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
	"unicode"
)

const libDesignsSubdir = "designs"

// designMaxBytes bounds one saved design. Art rides inside (up to 1.5 MB an upload, as
// base64), and a design holding a dozen uploads is the far end of what an author makes.
const designMaxBytes = 64 << 20

var designAppRe = regexp.MustCompile(`^[a-z0-9_-]{1,32}$`)

// designsDir is the app's design folder: under the writable library root, beside apps/
// and themes/, so it lands where an import would and never inside a read-only tree.
func (l *Library) designsDir(app string) string {
	t := l.writableThemesDir()
	if t == "" {
		return ""
	}
	return filepath.Join(filepath.Dir(t), libDesignsSubdir, app)
}

// designName checks a design's name: what the author typed, trimmed, 1–64 characters,
// no control characters. It is shown back verbatim, so it is not reshaped here.
func designName(raw string) (string, error) {
	n := strings.TrimSpace(raw)
	if n == "" {
		return "", errors.New("a design needs a name")
	}
	if len([]rune(n)) > 64 {
		return "", errors.New("a design's name is at most 64 characters")
	}
	for _, r := range n {
		if unicode.IsControl(r) {
			return "", errors.New("a design's name cannot hold control characters")
		}
	}
	return n, nil
}

// designFile is the name's file: a readable stem plus a hash of the exact name, so two
// names that fold to one stem ("Big sub", "big-sub") never share a file, and the same
// name always lands on the same one.
func designFile(name string) string {
	var b strings.Builder
	dash := false
	for _, r := range strings.ToLower(name) {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') {
			b.WriteRune(r)
			dash = false
		} else if !dash && b.Len() > 0 {
			b.WriteByte('-')
			dash = true
		}
		if b.Len() >= 40 {
			break
		}
	}
	stem := strings.Trim(b.String(), "-")
	if stem == "" {
		stem = "design"
	}
	h := sha256.Sum256([]byte(name))
	return stem + "-" + hex.EncodeToString(h[:4]) + ".orbdesign"
}

// DesignRow is one saved design as the editor lists it.
type DesignRow struct {
	Name       string          `json:"name"`
	At         int64           `json:"at"`
	Bytes      int             `json:"bytes"`
	AppVersion string          `json:"appVersion,omitempty"`
	Made       string          `json:"made,omitempty"`
	Extra      json.RawMessage `json:"extra,omitempty"`
}

type designSaved struct {
	Name  string          `json:"name"`
	At    int64           `json:"at"`
	Extra json.RawMessage `json:"extra,omitempty"`
}

// designHead is the part of a design document the hub reads: the envelope, never the
// app's content past it.
type designHead struct {
	Format     string       `json:"format"`
	App        string       `json:"app"`
	AppVersion string       `json:"appVersion"`
	Made       string       `json:"made"`
	Saved      *designSaved `json:"saved"`
}

func (s *Server) routesDesigns(mux *http.ServeMux) {
	// K10: listing, reading and removing are the library's (libentries.go); a draft is saved here
	mux.Handle("/api/design/save", s.requireControl(http.HandlerFunc(s.handleDesignSave)))
}

// designApp reads ?app= and the folder it names, or answers the refusal itself.
func (s *Server) designApp(w http.ResponseWriter, r *http.Request) (string, string, bool) {
	app := r.URL.Query().Get("app")
	if !designAppRe.MatchString(app) {
		s.jsonErr(w, "expected ?app=<slug>")
		return "", "", false
	}
	dir := s.library().designsDir(app)
	if dir == "" {
		s.jsonErr(w, "the hub has no writable library directory to keep designs in")
		return "", "", false
	}
	return app, dir, true
}

func listDesigns(dir string) ([]DesignRow, error) {
	rows := []DesignRow{}
	entries, err := os.ReadDir(dir)
	if errors.Is(err, os.ErrNotExist) {
		return rows, nil
	}
	if err != nil {
		return nil, fmt.Errorf("the designs folder %s cannot be read: %w", dir, err)
	}
	for _, e := range entries {
		if e.IsDir() || filepath.Ext(e.Name()) != ".orbdesign" {
			continue
		}
		body, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			continue
		}
		var h designHead
		if json.Unmarshal(body, &h) != nil || h.Saved == nil || h.Saved.Name == "" {
			continue // not one the hub filed: a file dropped in by hand has no name to list
		}
		rows = append(rows, DesignRow{Name: h.Saved.Name, At: h.Saved.At, Bytes: len(body),
			AppVersion: h.AppVersion, Made: h.Made, Extra: h.Saved.Extra})
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].At > rows[j].At })
	return rows, nil
}

// handleDesignSave answers POST /api/design/save?app=&name= with the design document as
// the body. The envelope must be a design of this app; "saved" is the hub's to write.
func (s *Server) handleDesignSave(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	app, dir, ok := s.designApp(w, r)
	if !ok {
		return
	}
	name, err := designName(r.URL.Query().Get("name"))
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, designMaxBytes+1))
	if err != nil {
		s.jsonErr(w, "reading the design: "+err.Error())
		return
	}
	if len(body) > designMaxBytes {
		s.jsonErr(w, fmt.Sprintf("that design is over the %d MB a saved design may be", designMaxBytes>>20))
		return
	}
	var doc map[string]json.RawMessage
	if json.Unmarshal(body, &doc) != nil {
		s.jsonErr(w, "that is not a design document")
		return
	}
	var h designHead
	_ = json.Unmarshal(body, &h)
	if h.Format != "wc-design" {
		s.jsonErr(w, "that is not a design document")
		return
	}
	if h.App != "" && h.App != app {
		s.jsonErr(w, "that design is the "+h.App+" app’s, not "+app+"’s")
		return
	}
	saved := designSaved{Name: name, At: time.Now().UnixMilli()}
	if h.Saved != nil && len(h.Saved.Extra) > 0 {
		saved.Extra = h.Saved.Extra
	}
	doc["saved"], _ = json.Marshal(saved)
	out, err := json.Marshal(doc)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		s.jsonErr(w, "the designs folder could not be made: "+err.Error())
		return
	}
	dest := filepath.Join(dir, designFile(name))
	if err := writeFileAtomic(dest, out); err != nil {
		s.jsonErr(w, "the design could not be written: "+err.Error())
		return
	}
	s.jsonOK(w, DesignRow{Name: name, At: saved.At, Bytes: len(out), AppVersion: h.AppVersion, Made: h.Made, Extra: saved.Extra})
}
