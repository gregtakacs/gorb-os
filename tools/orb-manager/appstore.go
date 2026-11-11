package main

// A1 §7.1–§7.3 (docs/specs/a1-design.md): GET /api/site/apps reads the site's store,
// POST /api/site/apps/add is the only way one of its apps reaches this hub's shelf, and the
// `.origin.json` beside a kept `.orbapp` records where those bytes came from.
//
// The owner's Apps face asks the hub what orb-os.net holds; the hub fetches the public
// catalogue from GET <GORB_SITE>/api/hub/apps through its own no-redirect site client,
// holds one copy in memory for five minutes, and answers the face with it. The catalogue
// is public, so the hub forwards no bearer, cookie or token: what needs control is letting
// the hub's own page make the call at all, which is requireControl's job.
//
// A site that is down, throttling, unreachable, slow (the client's own timeout) or that
// answers with a redirect, a too-big body or garbage is not an empty store: the hub serves
// the last copy it read, marked stale, and says one line to the log about why. A hub that
// has never read the catalogue has nothing stale to offer and answers the ordinary refusal
// envelope, naming the site host the owner configured. The copy lives in memory only — a
// hub that persisted a catalogue to state.json would be recommending a store it can no
// longer see, and A1 never removes or installs anything from a stale read.

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// siteCatalogueMax bounds a catalogue answer: the whole store is one JSON document, and a
// site that sends more than a megabyte of it is not answering the question the hub asked.
// Reading is capped at this plus one byte so an oversized body is refused, not streamed.
const siteCatalogueMax = 1 << 20

// siteCatalogueTTL is how long one read is fresh: five minutes. A hub is a single shelf,
// not a cache tier, so the freshness window is a courtesy to the site rather than a hit rate.
const siteCatalogueTTL = 5 * time.Minute

// siteCatalogue is one entry of the store as §4.1 serves it. The seven describe-fields
// (version, stamp, caps, osMin, sha256, size, hasEditor) arrive as JSON null for an app with
// no orderable live release but at least one withdrawn row; decoding into plain value fields
// leaves them zero ("", 0, false, nil) with no error, which is exactly the "not orderable"
// signal the caller reads. Unknown fields are ignored, so a later site never breaks an old hub.
type siteCatalogue struct {
	Slug      string            `json:"slug"`
	Title     string            `json:"title"`
	Developer string            `json:"developer"`
	Version   string            `json:"version"`
	Stamp     string            `json:"stamp"`
	Caps      []string          `json:"caps"`
	OSMin     string            `json:"osMin"`
	Sha256    string            `json:"sha256"`
	Size      int64             `json:"size"`
	HasEditor bool              `json:"hasEditor"`
	Readers   map[string]string `json:"readers"`
	Withdrawn []siteWithdrawn   `json:"withdrawn"`
}

// siteWithdrawn is one withdrawn release of an app, with the reason its author or an admin
// gave. Reason is "" for a row withdrawn before A1's column existed.
type siteWithdrawn struct {
	Version string `json:"version"`
	Reason  string `json:"reason"`
}

// siteCatalogueEntry is the whole document /api/hub/apps answers: the apps and nothing else.
type siteCatalogueEntry struct {
	Apps []siteCatalogue `json:"apps"`
}

// siteCatalogueCache is the hub's one held read, guarded so two faces opening at once share
// one dial. The zero value is ready to use: at zero means nothing has ever been read, so
// there is nothing stale to serve.
type siteCatalogueCache struct {
	mu   sync.Mutex
	at   time.Time
	apps []siteCatalogue
}

// orderable reports whether an entry can be added or offered for Update. A withdrawn-only
// entry (a null version) feeds the banner and nothing else.
func (e siteCatalogue) orderable() bool { return e.Version != "" }

// routesAppStore mounts the two store doors: the catalogue read and the one add press. Both
// are requireControl doors, and both are called only from main.go's !s.siteHidden block, so a
// hub that has no -site has no /api/site/apps and no /api/site/apps/add at all: no route, no
// chip, and no dial.
func (s *Server) routesAppStore(mux *http.ServeMux) {
	mux.Handle("/api/site/apps", s.requireControl(http.HandlerFunc(s.serveSiteApps)))
	mux.Handle("/api/site/apps/add", s.requireControl(http.HandlerFunc(s.h_siteAppsAdd)))
}

// serveSiteApps answers GET /api/site/apps: the catalogue in the hub's standard envelope,
// with the time the served copy was fetched and whether it is a stale one. A non-GET verb is
// 405 from the door, the same way the site import doors say it. An unfetchable site with no
// cached copy is the ordinary refusal envelope, naming the configured host.
func (s *Server) serveSiteApps(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	refresh := r.URL.Query().Get("refresh") == "1"
	apps, at, stale, err := s.fetchSiteCatalogue(refresh)
	if err != nil {
		s.jsonErr200(w, err.Error())
		return
	}
	if apps == nil {
		apps = []siteCatalogue{}
	}
	s.jsonOK(w, map[string]any{"apps": apps, "checkedAt": at.UnixMilli(), "stale": stale,
		"site": siteReleaseBase()})
}

// fetchSiteCatalogue returns the catalogue to answer with, the time that copy was fetched,
// whether it is stale, and an error only when there is no copy to serve at all. Inside the
// five-minute window it is the cache; a refresh press (or an expired cache) dials the site.
// A failed dial that finds a held copy serves it as stale and says one line to the log; one
// with nothing held returns the refusal, already worded with the site's host.
func (s *Server) fetchSiteCatalogue(refresh bool) ([]siteCatalogue, time.Time, bool, error) {
	c := &s.siteCatalogue
	c.mu.Lock()
	defer c.mu.Unlock()
	if !refresh && !c.at.IsZero() && time.Since(c.at) < siteCatalogueTTL {
		return c.apps, c.at, false, nil
	}
	apps, err := s.readSiteCatalogue()
	if err != nil {
		if c.at.IsZero() {
			return nil, time.Time{}, false, err
		}
		log.Printf("site apps: %v: serving the catalogue read at %s as stale", err, c.at.Format(time.RFC3339))
		return c.apps, c.at, true, nil
	}
	c.at = time.Now()
	c.apps = apps
	return apps, c.at, false, nil
}

// readSiteCatalogue asks the configured site for its store, once, over the no-redirect
// client. Nothing is cached here: the caller files a success and holds the last good read.
// The request carries no credential of any kind — the catalogue is public — and a redirect
// is an answer, not a step to take (that is siteHTTP's rule, not a second one written here).
func (s *Server) readSiteCatalogue() ([]siteCatalogue, error) {
	site := siteReleaseBase()
	host := siteHost(site)
	req, err := http.NewRequest(http.MethodGet, site+"/api/hub/apps", nil)
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", host, err)
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "orb-manager/"+version)
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", host, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 300 && resp.StatusCode < 400 {
		return nil, fmt.Errorf("%s sent a redirect instead of the store: Orb Manager does not follow one", host)
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return nil, fmt.Errorf("%s answered HTTP %d", host, resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, int64(siteCatalogueMax)+1))
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", host, err)
	}
	if len(body) > siteCatalogueMax {
		return nil, fmt.Errorf("%s sent a store over %d bytes", host, siteCatalogueMax)
	}
	var entry siteCatalogueEntry
	if err := json.Unmarshal(body, &entry); err != nil {
		return nil, fmt.Errorf("%s answered with something that is not the store", host)
	}
	return entry.Apps, nil
}

// jsonErr200 is jsonErr's envelope at the status this door answers a refusal with: the
// catalogue face hides its section on an ok:false, and a 400 there reads as a broken press
// rather than as "the store could not be read right now".
func (s *Server) jsonErr200(w http.ResponseWriter, msg string) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"ok": false, "error": msg})
}

// --- the add door (§7.2) ---------------------------------------------------------------
//
// One press, one app, one door: the hub asks the store for the newest live version of one slug,
// checks the answer four ways before it writes a byte, files it with the same KeepOver the
// browser import uses, and records where the bytes came from in the sidecar beside it. It
// installs nothing — the row's own Install verb is the only thing in this build that touches a
// board (ruling 18), and the six checks below are why that verb can be trusted on a file the
// owner never held in their hand.

// siteAddBodyMax bounds an add request: a press names two short strings, and a hub that read an
// unbounded body before refusing one would be letting a stranger set its memory budget.
const siteAddBodyMax = 4096

// The sentences §7.2 fixes. Every refusal this door makes is one of them. The same words cover
// an unknown slug, a withdrawn one and an old live version, so the door cannot be walked one
// probe at a time by a caller that is only asking for a list.
const (
	siteAddNoOffer     = "orb-os.net does not offer %s %s."
	siteAddThrottled   = "orb-os.net is asking us to wait a minute."
	siteAddUnreachable = "orb-os.net could not be reached."
	siteAddNoHash      = "orb-os.net sent %s %s with no sha256 to check the bytes against"
	siteAddOversize    = "orb-os.net sent a %s bigger than the %d MB the hub's own import takes"
	siteAddHashDiff    = "the bytes orb-os.net sent for %s %s hash to %s and the store named %s"
	siteAddNotThatApp  = "the file orb-os.net sent is not that app: %s"
	siteAddHeld        = "The library already holds %s from somewhere else: remove it from the library first."
	siteAddNeedsPress  = "add needs a slug and a version: {\"slug\": \"aurora\", \"version\": \"v1.3.0\"}"
)

// h_siteAppsAdd answers POST /api/site/apps/add with {"slug":…,"version":…}. The checks run in
// §7.2's order and stop at the first refusal, so a body is never hashed before the store said it
// owed these bytes, and never written before the shelf said it was safe to write into.
func (s *Server) h_siteAppsAdd(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	press, err := io.ReadAll(io.LimitReader(r.Body, int64(siteAddBodyMax)+1))
	if err != nil {
		s.jsonErr(w, "the add request did not arrive: "+err.Error())
		return
	}
	if len(press) == 0 || len(press) > siteAddBodyMax {
		s.jsonErr(w, siteAddNeedsPress)
		return
	}
	var named struct {
		Slug    string `json:"slug"`
		Version string `json:"version"`
	}
	// A number where a string belongs, or an object where a pair of strings belongs, is the same
	// refusal as an empty body: the caller did not say which app to add either way.
	if json.Unmarshal(press, &named) != nil {
		s.jsonErr(w, siteAddNeedsPress)
		return
	}
	slug, rel := named.Slug, named.Version
	if !orbnet.AppStemOK(slug) || !orbnet.AppVersionOK(rel) {
		s.jsonErr(w, siteAddNeedsPress)
		return
	}

	// check 1: the store, read through the five-minute cache, offers this exact version. The hub
	// adds the newest live release and nothing else (ruling 21), which is also what makes the
	// hash of check 4 a statement about the bytes this press asked for.
	apps, _, _, err := s.fetchSiteCatalogue(false)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	entry, offered := siteCatalogueOffer(apps, slug)
	if !offered || entry.Version != rel {
		s.jsonErr(w, fmt.Sprintf(siteAddNoOffer, slug, rel))
		return
	}

	// check 2: the download door answered 200. A redirect is an answer, not a step — siteHTTP
	// never follows one — and 404 is what §4.2 says for an unknown, pending, rejected, skipped
	// or withdrawn release alike, a withdrawal that landed between the listing and this press
	// included.
	site := siteReleaseBase()
	dial, err := http.NewRequest(http.MethodGet,
		site+"/api/hub/app/"+url.PathEscape(slug)+"/"+url.PathEscape(rel), nil)
	if err != nil {
		s.jsonErr(w, siteAddUnreachable)
		return
	}
	dial.Header.Set("Accept", "application/octet-stream")
	dial.Header.Set("User-Agent", "orb-manager/"+version)
	resp, err := s.siteHTTP().Do(dial)
	if err != nil {
		s.jsonErr(w, siteAddUnreachable)
		return
	}
	defer resp.Body.Close()
	switch resp.StatusCode {
	case http.StatusOK:
	case http.StatusNotFound:
		s.jsonErr(w, fmt.Sprintf(siteAddNoOffer, slug, rel))
		return
	case http.StatusTooManyRequests:
		s.jsonErr(w, siteAddThrottled)
		return
	default: // a 3xx, a 5xx or anything else: the site did not hand over the file
		s.jsonErr(w, siteAddUnreachable)
		return
	}
	// A 200 with no hash is a 200 about bytes this hub cannot name, and §10's answer is to
	// refuse rather than to trust the absence of a check.
	claim := strings.ToLower(strings.TrimSpace(resp.Header.Get("X-Orb-Sha256")))
	if claim == "" {
		s.jsonErr(w, fmt.Sprintf(siteAddNoHash, slug, rel))
		return
	}

	// check 3: no bigger than a hand-carried app may be. A Content-Length that lies upward is
	// refused on the header, so an 8 MB body is never buffered to learn it was 9; one that lies
	// downward is caught by the capped read.
	if resp.ContentLength > appUploadMax {
		s.jsonErr(w, fmt.Sprintf(siteAddOversize, slug, appUploadMax>>20))
		return
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, int64(appUploadMax)+1))
	if err != nil {
		s.jsonErr(w, siteAddUnreachable)
		return
	}
	if len(body) > appUploadMax {
		s.jsonErr(w, fmt.Sprintf(siteAddOversize, slug, appUploadMax>>20))
		return
	}

	// check 4: sha256(body) equals what the catalogue named and what the download door claimed.
	// The two agreeing with each other and with the bytes is the point of the check, so the
	// refusal names the hash the site said and the hash the bytes say.
	sum := sha256Hex(body)
	if said := strings.ToLower(entry.Sha256); sum != said {
		s.jsonErr(w, fmt.Sprintf(siteAddHashDiff, slug, rel, sum, said))
		return
	}
	if sum != claim {
		s.jsonErr(w, fmt.Sprintf(siteAddHashDiff, slug, rel, sum, claim))
		return
	}

	// check 5: the file is the app the catalogue named, not merely an app. The slug and version
	// compared here are the request's, never the container's — the container only has to agree —
	// and a `+build` tail says how a release was built, not which release it claims to be.
	c, err := orbnet.ParseApp(body)
	if err != nil {
		s.jsonErr(w, fmt.Sprintf(siteAddNotThatApp, strings.TrimPrefix(err.Error(), "orbnet: ")))
		return
	}
	if c.Slug != slug || versionHead(c.Version) != versionHead(rel) {
		s.jsonErr(w, fmt.Sprintf(siteAddNotThatApp,
			fmt.Sprintf("its header names itself %s %s", c.Slug, versionHead(c.Version))))
		return
	}

	// check 6, the last gate before the write: the shelf is safe to write into. An empty slot is.
	// So is a copy this hub added from this same site. A copy with no sidecar — a hand import or
	// a bundled app — or one whose sidecar names a different site is not the store's to replace,
	// and an Update on such a row never happens either (§7.4).
	lib := s.library()
	held := filepath.Join(lib.writableAppsDir(), slug+".orbapp")
	_, statErr := os.Stat(held)
	overwrote := statErr == nil
	if overwrote {
		if o := readAppOrigin(held); o == nil || o.Site != site {
			s.jsonErr(w, fmt.Sprintf(siteAddHeld, slug))
			return
		}
	}

	// The ordinary door, asked the way the browser import asks it.
	dest, err := lib.KeepOver(slug, body, true)
	if err != nil {
		// Reported, not swallowed: the bytes passed every check and the disk said no, which the
		// owner is owed as the library's own sentence rather than as a shrug.
		s.jsonErr(w, err.Error())
		return
	}
	// K9's rule, same as the import door's keep: the hub serves its library's editor, so a new
	// copy retires every remembered region instead of leaving a replaced app's old editor up.
	editorInvalidate()
	origin := &AppOrigin{Site: site, Slug: slug, Version: rel, Stamp: entry.Stamp,
		SHA256: sum, ImportedAt: time.Now().UnixMilli()}
	if err := writeAppOrigin(dest, origin); err != nil {
		// The app is on the shelf and its record is not. Say exactly that: a half-pair is an
		// ordinary import with no chip, which is a worse answer than the truth about this press.
		s.jsonErr(w, "the app was kept, but where it came from could not be recorded: "+err.Error())
		return
	}
	log.Printf("site add: %s %s %s -> %s", site, slug, rel, dest)
	s.jsonOK(w, map[string]any{"slug": slug, "version": rel, "kept": dest, "overwrote": overwrote})
}

// siteCatalogueOffer finds one slug's entry in a catalogue read. An entry whose describe-fields
// came back null (a withdrawn-only app, §4.1) is not an offer, and the caller says so with the
// one sentence an absent app and an unorderable one share.
func siteCatalogueOffer(apps []siteCatalogue, slug string) (siteCatalogue, bool) {
	for _, e := range apps {
		if e.Slug == slug {
			return e, e.orderable()
		}
	}
	return siteCatalogue{}, false
}

// versionHead is the vN.N.N head of a release stamp with its -prerelease and +build tails cut
// off: the part that says which release a file claims to be, which is all §7.2's check 5
// compares. A stamp with no tail is its own head.
func versionHead(stamp string) string {
	if i := strings.IndexAny(stamp, "-+"); i >= 0 {
		return stamp[:i]
	}
	return stamp
}

// --- the app origin sidecar (§7.3) -----------------------------------------------------
//
// The record of where a shelf app came from, beside the file rather than inside it: the bytes
// the site served are the bytes the hub keeps (ruling 19) and are never rewritten to carry hub
// bookkeeping. This is ThemeOrigin's twin, with the app's identity in its place, and it keeps
// the same three disciplines: the write is atomic, an unreadable record reads as no record at
// all, and taking down a record that was never there counts as success.

// AppOrigin is one `.orbapp`'s record of where it came from. Version is the release the store
// named and Stamp its full stamp as the store spelled it, and SHA256 is the hash the hub checked
// the bytes against — so a later read can tell what is on the shelf from what the site once
// said, without re-dialing it.
type AppOrigin struct {
	Site       string `json:"site"`
	Slug       string `json:"slug"`
	Version    string `json:"version"`
	Stamp      string `json:"stamp"`
	SHA256     string `json:"sha256"`
	ImportedAt int64  `json:"importedAt"`
}

// appOriginPath is an app's sidecar: the app's own path with the suffix appended, so the record
// sits with the file it describes. The apps listing filters on the .orbapp extension and so
// never sees it, exactly as the themes listing never sees a `.origin.json`. Built from the app's
// path rather than joined with "/" for the reason the rest of the library is: the hub runs on
// Windows too.
func appOriginPath(appPath string) string {
	return appPath + ".origin.json"
}

// writeAppOrigin files the record with the library's own atomic write: a half-written sidecar
// would read back as no sidecar at all, and the app would look homemade.
func writeAppOrigin(appPath string, o *AppOrigin) error {
	body, err := json.MarshalIndent(o, "", "  ")
	if err != nil {
		return err
	}
	return writeFileAtomic(appOriginPath(appPath), append(body, '\n'))
}

// readAppOrigin returns the app's origin, or nil when the shelf holds no readable one: no file,
// not JSON, or a record missing the things that make it a record. A hub that cannot say where a
// file came from says nothing rather than guessing, and an app with no sidecar is an ordinary
// import: no chip, no update offer.
func readAppOrigin(appPath string) *AppOrigin {
	body, err := os.ReadFile(appOriginPath(appPath))
	if err != nil {
		return nil
	}
	var o AppOrigin
	if json.Unmarshal(body, &o) != nil {
		return nil
	}
	if o.Site == "" || o.Slug == "" || o.Version == "" {
		return nil
	}
	return &o
}

// removeAppOrigin takes an app's sidecar down. Every writer of a `<slug>.orbapp` other than the
// add door calls it after its keep, so a sidecar never describes bytes the site did not send;
// having no sidecar is the normal case, so a missing file is success.
func removeAppOrigin(appPath string) error {
	if err := os.Remove(appOriginPath(appPath)); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return nil
}
