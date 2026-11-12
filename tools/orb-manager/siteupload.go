package main

// H4 step 2's two upload doors. handleSiteUpload reads one library entry's design the way
// handleLibDesign's default branch does — the draft when it has one, the design the theme
// carries otherwise, never the bake (ruling 43) — and posts it to §2's site door with the
// link's bearer, turning the site's answer into the one envelope §6 fixes. Two names ride the
// body and do two jobs: `name` says which entry to read, `siteName` — when the body carried
// one — says what the site calls it.
// handleSiteLocalDesigns is the dialog's picker: every entry on this hub that has a
// design, drafts and baked themes alike, newest first.
import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// siteUploadBodyMax bounds what the hub reads of the site's answer to an upload. The 201
// object is six fields and a shareRefused two; a refusal is an error word and a sentence.
// A body past this is not an answer this door can act on, and the rest is not read.
const siteUploadBodyMax = 8 << 10

// siteUploadMaxBytes is the site's MAX_FILE_BYTES as §6's pre-check names it: a design
// over it is refused here, before the hub dials a megabyte to be told. The sentence is
// the site's own, so the owner reads the same words whichever side measured.
const siteUploadMaxBytes = 10 << 20

// The door's own sentences, fixed by H4 §6 and §7. The first two are refusals this door
// can see coming without asking the site; the third is what a read-only link hears, in
// the ruling's words — the refusal is about the link's scopes, not its life, so the
// credential stays exactly where it is.
const (
	siteUploadMsgNoDesign  = "that theme carries no design, so it can't be uploaded"
	siteUploadMsgTooLarge  = "that design is over the 10 MB a saved design may be"
	siteUploadMsgReconnect = "Reconnect to allow uploads"
)

// designsRoot is the designs base the writable library keeps its per-app folders under:
// designsDir without the app, for the walk §6's picker does — every app folder that
// exists is an app with designs, whether or not any request named it.
func (l *Library) designsRoot() string {
	t := l.writableThemesDir()
	if t == "" {
		return ""
	}
	return filepath.Join(filepath.Dir(t), libDesignsSubdir)
}

// handleSiteUpload answers POST /api/site/upload with
// {"app","name","siteName"?,"visibility"?,"replaceId"?} naming an entry in this hub's library.
// The two names are two things: `name` always names the local entry to read, and the name the
// site is dialed with is `siteName` when the body carried one, the entry's own otherwise —
// which is what lets "Upload as a new design" send the picked design out under a typed name
// instead of looking for a library entry that the typing has not created. The refusal order
// mirrors §2's fixed one down as far as the hub can check locally — the link's two states, its
// scopes, the body's grammar of both names, the entry and its design — because a hub that
// cannot upload, or has no such design, must learn that before a socket opens. Then comes one
// attempt at the site door, never two: a retry would turn one confirmed Replace into two
// versions.
func (s *Server) handleSiteUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if s.disconnectedSiteLink() != nil {
		s.jsonErr(w, siteMsgDisconnected)
		return
	}
	l := s.liveSiteLink()
	if l == nil {
		s.jsonErr(w, siteMsgNotConnected)
		return
	}
	if !slices.Contains(siteLinkScopes(l), siteScopeWrite) {
		s.jsonErr(w, siteUploadMsgReconnect)
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, siteLinkMax+1))
	if err != nil || len(body) > siteLinkMax {
		s.jsonErr(w, "expected ?app=<slug>")
		return
	}
	var in struct {
		App        string  `json:"app"`
		Name       string  `json:"name"`
		SiteName   *string `json:"siteName"`
		Visibility string  `json:"visibility"`
		ReplaceID  string  `json:"replaceId"`
	}
	// An unreadable body is an absent app: the same sentence the grammar check answers,
	// because the door cannot tell a form bug from a stranger's paste, and both mean the
	// same thing to the owner — the request did not name a design.
	if json.Unmarshal(body, &in) != nil {
		in.App = ""
	}
	if !designAppRe.MatchString(in.App) {
		s.jsonErr(w, "expected ?app=<slug>")
		return
	}
	name, err := designName(in.Name)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	// The second name is the one the site hears. A body may carry none — the plain upload
	// and the pre-H4 body — and then the entry's own name is the site's name, unchanged. One
	// carried is held to the same grammar as the first, because the hub that lets a bad one
	// through is a hub that dials a socket to be told, and `siteName: ""` is a name that was
	// typed and erased, not an absent field: designName's own sentence refuses it here.
	siteName := ""
	if in.SiteName != nil {
		if siteName, err = designName(*in.SiteName); err != nil {
			s.jsonErr(w, err.Error())
			return
		}
	}
	lib := s.library()
	dir := lib.designsDir(in.App)
	if dir == "" {
		s.jsonErr(w, "the hub has no writable library directory to keep designs in")
		return
	}
	e := lib.entryFold(in.App, name)
	if e == nil {
		s.jsonErr(w, "no “"+name+"” in the "+in.App+" library on this hub")
		return
	}
	design, err := uploadDesignBytes(dir, e)
	if err != nil {
		if errors.Is(err, orbnet.ErrNoDesign) {
			s.jsonErr(w, siteUploadMsgNoDesign)
		} else {
			s.jsonErr(w, strings.TrimPrefix(err.Error(), "orbnet: "))
		}
		return
	}
	if len(design) > siteUploadMaxBytes {
		s.jsonErr(w, siteUploadMsgTooLarge)
		return
	}
	s.siteUploadPost(w, r.Context(), l, in.App, name, siteName, design, in.Visibility, in.ReplaceID)
}

// uploadDesignBytes reads the entry's design exactly as handleLibDesign's default branch
// does — that read is what Reopen opens, and what uploads must be (ruling 43): the draft
// file when the entry has one, unsent changes and all, and otherwise the design carried
// inside the theme, never the bake. The draft is found under the entry's own spelling:
// designFile hashes the exact name, so a request that folded case must still land on the
// file the hub wrote.
func uploadDesignBytes(dir string, e *LibEntry) ([]byte, error) {
	if e.Draft != nil {
		return os.ReadFile(filepath.Join(dir, designFile(e.Name)))
	}
	if e.Theme == nil {
		return nil, orbnet.ErrNoDesign
	}
	raw, err := os.ReadFile(e.path)
	if err != nil {
		return nil, err
	}
	t, err := orbnet.ShelfTheme(raw)
	if err != nil {
		return nil, err
	}
	if t.Design == nil {
		return nil, orbnet.ErrNoDesign
	}
	return t.Design, nil
}

// siteUploadPost makes the one attempt §6 allows and answers its result table: 201 passes
// the site's object through as data, untouched — a shareRefused inside it is part of the
// answer, not a refusal (the site stored the design); a 401 is H2's rule unchanged; the
// scope refusal and the clash get the two bodies the dialog needs; everything else — a
// 429, a 500, a stall, a body that forgot to answer — is the site's message when it sent
// one and orb-os.net did not answer when it did not, in one pass-through so no hub-side
// table of codes can fall out of date. The query's name is siteName when the dialog typed or
// confirmed one and the entry's own name otherwise; the bytes are whatever that entry's read
// returned, so "Upload as a new design" is one entry read under one name and posted under
// another, never a search for a file the typing has not written.
func (s *Server) siteUploadPost(w http.ResponseWriter, ctx context.Context, l *siteLink,
	app, name, siteName string, design []byte, visibility, replaceID string) {
	if siteName == "" {
		siteName = name
	}
	q := url.Values{"app": {app}, "name": {siteName}}
	if visibility != "" {
		q.Set("visibility", visibility)
	}
	if replaceID != "" {
		q.Set("replaceId", replaceID)
	}
	ctx, cancel := context.WithTimeout(ctx, siteUploadTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, l.Site+"/api/hub/designs?"+q.Encode(), bytes.NewReader(design))
	if err != nil {
		s.jsonErr(w, siteMsgNoAnswer)
		return
	}
	req.Header.Set("Authorization", "Bearer "+l.Token)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "orb-manager/"+version)
	req.Header.Set("Cache-Control", "no-store")
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		// The timeout, a refused socket and a dead DNS all read the same to the owner:
		// the site did not answer, and the credential it did not revoke stays live.
		s.jsonErr(w, siteMsgNoAnswer)
		return
	}
	defer resp.Body.Close()
	switch resp.StatusCode {
	case http.StatusUnauthorized:
		s.forgetSiteLink(l)
		s.jsonErr(w, siteMsgDisconnected)
		return
	case http.StatusCreated:
		raw := readSiteUploadBody(resp.Body)
		if raw == nil || !json.Valid(raw) || !jsonIsObject(raw) {
			s.jsonErr(w, siteMsgNoAnswer)
			return
		}
		s.jsonOKRaw(w, raw)
	default:
		s.siteUploadRefuseSite(w, readSiteUploadBody(resp.Body))
	}
}

// jsonIsObject is siteDesignsObject's envelope check without its designs field: the 201
// body is the site's object and the hub re-sends it whole, so "an object" is all the hub
// needs to know before it hands these bytes to a page.
func jsonIsObject(b []byte) bool {
	var obj map[string]json.RawMessage
	return json.Unmarshal(b, &obj) == nil
}

// readSiteUploadBody reads the answer up to the cap, or nothing: a body past the cap is
// not an answer this door can act on, and half of it must not be shown as though it were.
func readSiteUploadBody(r io.Reader) []byte {
	b, err := io.ReadAll(io.LimitReader(r, int64(siteUploadBodyMax)+1))
	if err != nil || len(b) > siteUploadBodyMax {
		return nil
	}
	return bytes.TrimRight(b, " \t\r\n")
}

// siteUploadRefuseSite turns a refusal the site answered into the one shape §6 fixes for
// it. Only two answers are matched on their error word: the scope refusal, which the page
// turns into its Reconnect affordance and the hub must word itself, and the clash, which
// must carry the site's message and the site's `existing` object so the dialog can offer
// Replace and Upload as a new design. A clash whose body carries no existing — the race
// §3.2 names — is the same answer without that member. The status line the site chose is
// the site's business and reaches no one here: the hub answers its own envelope, and the
// page reads the body. Any other code is passed through in the site's own words; an
// answer that carries none is the site not answering.
func (s *Server) siteUploadRefuseSite(w http.ResponseWriter, body []byte) {
	var obj map[string]json.RawMessage
	code, message := "", ""
	var existing json.RawMessage
	if json.Unmarshal(body, &obj) == nil {
		_ = json.Unmarshal(obj["error"], &code)
		_ = json.Unmarshal(obj["message"], &message)
		existing = obj["existing"]
	}
	switch {
	case code == "insufficient_scope":
		s.jsonErr(w, siteUploadMsgReconnect)
	case code == "name_taken":
		if message == "" {
			message = siteMsgNoAnswer
		}
		extra := map[string]any{"message": message}
		if len(existing) > 0 && string(existing) != "null" {
			extra["nameTaken"] = existing
		}
		s.jsonErrFields(w, "name_taken", extra)
	case message != "":
		s.jsonErr(w, message)
	default:
		s.jsonErr(w, siteMsgNoAnswer)
	}
}

// --- the picker -------------------------------------------------------------------------

// localDesignsMax is §6's cap, the same five hundred §2.5 caps the site's list at: the
// picker draws the newest of what this hub holds, and an owner with more than that will
// find what they are looking for in the newest five hundred.
const localDesignsMax = 500

// localDesignRow is one picker row: the §6 shape, bytes and appVersion taken from the
// half that would actually upload — the draft when there is one, so the size the dialog
// shows is the size the door will weigh against the 10 MB pre-check.
type localDesignRow struct {
	App        string `json:"app"`
	Name       string `json:"name"`
	Bytes      int    `json:"bytes"`
	At         int64  `json:"at"`
	AppVersion string `json:"appVersion"`
}

// handleSiteLocalDesigns answers GET /api/site/local-designs: every entry that has a
// design, across every app the library speaks of — an app folder under designs/, or an app
// claimed by a shelf theme, because a theme directory with no designs folder is a library
// too. Drafts only would hide most of a real library: a design baked and sent keeps its
// design inside the theme and its draft folded away, so the entry counts when it has a
// draft or a theme whose can_reopen the entries read already computed — a bake with no
// design inside has nothing to upload.
func (s *Server) handleSiteLocalDesigns(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	lib := s.library()
	apps := map[string]bool{}
	if root := lib.designsRoot(); root != "" {
		if entries, err := os.ReadDir(root); err == nil {
			for _, de := range entries {
				if de.IsDir() {
					apps[de.Name()] = true
				}
			}
		}
	}
	if lib.ThemesDir != "" {
		shelfThemes(lib.ThemesDir, func(_ string, _ os.FileInfo, t *orbnet.ShelfThemeInfo) {
			apps[shelfApp(t)] = true
		})
	}
	rows := []localDesignRow{}
	for app := range apps {
		if !designAppRe.MatchString(app) {
			continue // a directory no slug grammar allows is not an app's library
		}
		entries, err := lib.entries(app)
		if err != nil {
			continue
		}
		for i := range entries {
			if row, ok := localDesignOf(app, &entries[i]); ok {
				rows = append(rows, row)
			}
		}
	}
	sort.Slice(rows, func(i, j int) bool {
		if rows[i].At != rows[j].At {
			return rows[i].At > rows[j].At
		}
		if rows[i].App != rows[j].App {
			return rows[i].App < rows[j].App
		}
		return rows[i].Name < rows[j].Name
	})
	if len(rows) > localDesignsMax {
		rows = rows[:localDesignsMax]
	}
	s.jsonOK(w, map[string]any{"designs": rows})
}

// localDesignOf is one entry's picker row, and whether it gets one at all: a draft says
// yes with its own size and version; a theme says yes only when it carries a design, and
// then with the carried design's size and the version that design names, not the bake's.
func localDesignOf(app string, e *LibEntry) (localDesignRow, bool) {
	if e.Draft != nil {
		return localDesignRow{App: app, Name: e.Name, Bytes: e.Draft.Bytes, At: e.At, AppVersion: e.Draft.AppVersion}, true
	}
	if e.Theme == nil || !e.Theme.CanReopen {
		return localDesignRow{}, false
	}
	raw, err := os.ReadFile(e.path)
	if err != nil {
		return localDesignRow{}, false
	}
	t, err := orbnet.ShelfTheme(raw)
	if err != nil {
		return localDesignRow{}, false
	}
	return localDesignRow{App: app, Name: e.Name, Bytes: len(t.Design), At: e.At, AppVersion: madeWith(t.Design)}, true
}
