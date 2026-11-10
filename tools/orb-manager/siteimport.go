package main

// orb-os-net design §7.1 (import by link) and §8.4 (the design bundle), H1: the hub's half
// of "Import from orb-os.net".
//
// The owner pastes a design's link — https://<site>/t/<id> — into the Themes tab. The hub
// fetches that design's bundle from GET <site>/api/hub/theme/<id>, checks what came back,
// and holds it under a one-time import ticket. Nothing is filed yet at that point: the
// editor opened with ?import=<ticket> reads the bundle back (a reload must work, so a
// ticket can be read until it expires), bakes it under the name the design carries, and
// posts the bake to /api/site/keep. That door files the bake with keepTheme — the same
// filer every other keep path uses — and writes an origin sidecar beside it, so the
// library can say later where the theme came from (H3's update checks read that record).
//
// The hub never rewrites a bundle: the bytes the site served are the bytes the editor
// gets back. The hub also never follows a redirect: a site that answers 302 is sending a
// sign-in page, not a theme, and dialing the place it points at would hand the hub's
// request to somewhere the owner never named.

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// siteBundleMax bounds an imported bundle: the largest design the site can serve here.
// Reading is capped at this plus one byte, so an oversized body is refused rather than
// streamed into memory.
const siteBundleMax = 32 << 20

// siteLinkMax bounds the paste. A link is well under a hundred bytes; anything bigger is
// not a link, and the door is not obliged to chew on it.
const siteLinkMax = 4096

const (
	siteTicketMax = 8                // live imports held at once; a ninth drops the oldest
	siteTicketTTL = 15 * time.Minute // long enough to open an editor and bake
	siteBundleFmt = "orb-design-bundle"
)

// siteLinkRefused is what every unreadable paste gets: one sentence, naming the shape a
// link has, rather than a diagnosis of which rule the text broke.
const siteLinkRefused = "paste a theme’s link from orb-os.net, like https://orb-os.net/t/AbCdEfGhIjKlMnOpQrStUv"

// siteTicketGone is the answer on either ticket door once the import is no longer held.
const siteTicketGone = "that import has expired: paste the link again"

// siteLinkRe is the whole path a design link has: the /t/ prefix and the 22-character id,
// with an optional trailing slash. Query and fragment are not the path's business.
var siteLinkRe = regexp.MustCompile(`^/t/([A-Za-z0-9_-]{22})/?$`)

// siteHexRe is a bundle image's content hash, as the site writes it: lowercase hex.
var siteHexRe = regexp.MustCompile(`^[0-9a-f]{64}$`)

// errSiteBundleNo is checkBundle's "this is not a design bundle at all", the case whose
// sentence names the site rather than the theme. Everything else it refuses is a bundle
// that arrived damaged, which the owner is entitled to hear described.
var errSiteBundleNo = errors.New("not a design bundle")

// parseSiteLink reads the pasted text: a design's link, with or without its scheme, and
// nothing else. The scheme is https, except plain http to this machine, which is how a
// developer's own site gets tested. The site is answered back canonical — lowercase host,
// port kept — so the fetch and the sidecar hold one spelling of it.
func parseSiteLink(raw string) (site, id string, err error) {
	text := strings.TrimSpace(raw)
	if text == "" {
		return "", "", errors.New(siteLinkRefused)
	}
	if !strings.Contains(text, "://") {
		text = "https://" + text
	}
	u, perr := url.Parse(text)
	if perr != nil {
		return "", "", errors.New(siteLinkRefused)
	}
	scheme := strings.ToLower(u.Scheme)
	if u.User != nil {
		return "", "", errors.New(siteLinkRefused)
	}
	switch scheme {
	case "https":
	case "http":
		// Plain http reaches the internet as easily as any other scheme, so it is allowed
		// only where "this hub's own machine" is the only reading the host can have.
		switch h := strings.ToLower(u.Hostname()); h {
		case "127.0.0.1", "localhost", "::1":
		default:
			return "", "", errors.New("the link must start with https://")
		}
	default:
		return "", "", errors.New(siteLinkRefused)
	}
	if u.Host == "" {
		return "", "", errors.New(siteLinkRefused)
	}
	m := siteLinkRe.FindStringSubmatch(u.EscapedPath())
	if m == nil {
		return "", "", errors.New(siteLinkRefused)
	}
	return scheme + "://" + strings.ToLower(u.Host), m[1], nil
}

// siteHost is the host of a canonical site, port and all: the name the refusal sentences
// blame, which has to match what the owner typed closely enough to be recognisable.
func siteHost(site string) string {
	if i := strings.Index(site, "://"); i >= 0 {
		return site[i+3:]
	}
	return site
}

// siteBundle is what the hub learned from a bundle: the fields its tickets and its origin
// record carry, and the raw bytes, which are the bundle as the site served it.
type siteBundle struct {
	ID         string
	Version    int
	App        string
	SharedName string
	Name       string
	Handle     string
	Raw        []byte
}

// checkBundle reads the site's answer as the design bundle §8.4 describes and checks the
// parts the hub acts on: that it is a bundle, that it is the design the link named, that
// it names one app and one name the hub can bake into, and that its art is what its own
// hashes say. The design document itself is carried, not interpreted: the hub is not the
// editor. A refusal says why, in the words the owner's sentence ends with.
func checkBundle(raw []byte, id string) (*siteBundle, error) {
	var obj map[string]json.RawMessage
	if json.Unmarshal(raw, &obj) != nil {
		return nil, errSiteBundleNo
	}
	if format, ok := siteJSONString(obj["format"]); !ok || format != siteBundleFmt {
		return nil, errSiteBundleNo
	}
	if bundle, err := siteJSONNumber(obj["bundle"]); err != nil || bundle != 1 {
		return nil, errSiteBundleNo
	}
	b := &siteBundle{Raw: raw}
	if got, ok := siteJSONString(obj["id"]); !ok || got != id {
		return nil, errors.New("it names another design")
	}
	v, err := siteJSONNumber(obj["version"])
	if err != nil || v < 1 || v != float64(int64(v)) {
		return nil, errors.New("it has no version")
	}
	b.Version = int(v)
	b.App, _ = siteJSONString(obj["app"])
	if !orbnet.AppStemOK(b.App) {
		return nil, errors.New("it names no app")
	}
	shared, ok := siteJSONString(obj["sharedName"])
	if !ok || shared == "" {
		return nil, errors.New("it has no shared name")
	}
	// The name the bake carries is the orb's name field, so the orb's own ceiling applies
	// before the hub promises to bake into it.
	if len(shared) > orbnet.ThemeNameMax {
		return nil, fmt.Errorf("its shared name is over %d bytes", orbnet.ThemeNameMax)
	}
	b.SharedName = shared
	var design map[string]json.RawMessage
	if json.Unmarshal(obj["design"], &design) != nil {
		return nil, errors.New("it carries no design")
	}
	var assets map[string]string
	if json.Unmarshal(obj["assets"], &assets) != nil {
		return nil, errors.New("its art is not readable")
	}
	if err := checkBundleArt(design, assets); err != nil {
		return nil, err
	}
	b.Name, _ = siteJSONString(obj["name"])
	b.Handle, _ = siteJSONString(obj["handle"])
	return b, nil
}

// checkBundleArt walks the design's orbAssets sidecar and proves the bundle carries the
// art it claims: every hash has a body in assets, every body decodes, and every decoded
// body still is the bytes its hash names. A bundle that fails this imports an image the
// editor would draw wrong, so it does not import.
func checkBundleArt(design map[string]json.RawMessage, assets map[string]string) error {
	raw, ok := design["orbAssets"]
	if !ok || string(raw) == "null" {
		return nil // a design with no sidecar has no art to fail
	}
	var list []json.RawMessage
	if json.Unmarshal(raw, &list) != nil {
		return errors.New("its art is not readable")
	}
	for _, el := range list {
		var row map[string]json.RawMessage
		if json.Unmarshal(el, &row) != nil {
			return errors.New("its art is not readable")
		}
		sha, ok := siteJSONString(row["sha256"])
		if !ok {
			return errors.New("its art is not readable")
		}
		body, ok := assets[sha]
		if !ok || !siteHexRe.MatchString(sha) {
			return fmt.Errorf("an image is missing (%s)", siteHex12(sha))
		}
		img, err := base64.StdEncoding.DecodeString(body)
		if err != nil {
			return errors.New("its art is not readable")
		}
		if sum := sha256.Sum256(img); hex.EncodeToString(sum[:]) != sha {
			return fmt.Errorf("an image came back changed (%s)", siteHex12(sha))
		}
	}
	return nil
}

// siteHex12 is a hash's first twelve characters, the amount of it a person can match
// against a design's own image list without a terminal open.
func siteHex12(sha string) string {
	if len(sha) > 12 {
		return sha[:12]
	}
	return sha
}

func siteJSONString(r json.RawMessage) (string, bool) {
	var s string
	if len(r) == 0 || json.Unmarshal(r, &s) != nil {
		return "", false
	}
	return s, true
}

func siteJSONNumber(r json.RawMessage) (float64, error) {
	var f float64
	if len(r) == 0 {
		return 0, errors.New("no number")
	}
	err := json.Unmarshal(r, &f)
	return f, err
}

// --- the ticket store ---------------------------------------------------------------

// siteTicket is one imported bundle waiting to be baked: where it came from, what it
// should bake into, and the bytes to bake from.
type siteTicket struct {
	site       string
	id         string
	version    int
	app        string
	name       string
	sharedName string
	handle     string
	raw        []byte
	at         time.Time
}

// siteTicketStore holds the imports the hub has fetched and not yet kept. An editor reload
// re-reads its ticket, so reading does not consume; only a keep does, and it consumes the
// ticket once the theme is on the shelf, so a refused bake can be tried again.
//
// Tickets die two ways: fifteen minutes old, or when a ninth arrives and an eighth one is
// beside it. The cap is what bounds the held megabytes on a hub whose owner pastes designs
// and never bakes them. The zero store is ready to use.
type siteTicketStore struct {
	mu    sync.Mutex
	live  map[string]*siteTicket
	order []string // ticket keys, oldest first: who goes when the store is full
	ttl   time.Duration
	now   func() time.Time
}

func (st *siteTicketStore) clock() time.Time {
	if st.now != nil {
		return st.now()
	}
	return time.Now()
}

func (st *siteTicketStore) life() time.Duration {
	if st.ttl > 0 {
		return st.ttl
	}
	return siteTicketTTL
}

// add holds one bundle and names it with the ticket its doors ask for.
func (st *siteTicketStore) add(t *siteTicket) (string, error) {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", err
	}
	ticket := hex.EncodeToString(b[:])
	st.mu.Lock()
	defer st.mu.Unlock()
	if st.live == nil {
		st.live = map[string]*siteTicket{}
	}
	now := st.clock()
	st.dropExpired(now)
	t.at = now
	for len(st.order) >= siteTicketMax {
		delete(st.live, st.order[0])
		st.order = st.order[1:]
	}
	st.live[ticket] = t
	st.order = append(st.order, ticket)
	return ticket, nil
}

// get reads a live ticket without consuming it.
func (st *siteTicketStore) get(ticket string) (*siteTicket, bool) {
	st.mu.Lock()
	defer st.mu.Unlock()
	st.dropExpired(st.clock())
	t, ok := st.live[ticket]
	return t, ok
}

// consume takes a kept import out of the store.
func (st *siteTicketStore) consume(ticket string) {
	st.mu.Lock()
	defer st.mu.Unlock()
	delete(st.live, ticket)
	for i, k := range st.order {
		if k == ticket {
			st.order = append(st.order[:i], st.order[i+1:]...)
			break
		}
	}
}

// dropExpired forgets what no longer counts as live, and is a no-op while the oldest hold
// is still young, which is the case every ordinary run is in. Callers hold the lock.
func (st *siteTicketStore) dropExpired(now time.Time) {
	life := st.life()
	for len(st.order) > 0 {
		if t := st.live[st.order[0]]; t != nil && now.Sub(t.at) <= life {
			break
		}
		delete(st.live, st.order[0])
		st.order = st.order[1:]
	}
	for k, t := range st.live {
		if now.Sub(t.at) > life {
			delete(st.live, k)
		}
	}
}

// tickets is the hub's held imports.
func (s *Server) tickets() *siteTicketStore { return &s.siteTickets }

// --- the fetch ----------------------------------------------------------------------

// siteClientDefault is the fetch client every hub that was not handed one uses, built once:
// a bundle is tens of megabytes, so the patience is a minute, and a redirect is an answer,
// not a step to take.
var siteClientDefault *http.Client
var siteClientOnce sync.Once

func (s *Server) siteHTTP() *http.Client {
	if s.siteClient != nil {
		return s.siteClient
	}
	siteClientOnce.Do(func() {
		siteClientDefault = &http.Client{
			Timeout: 60 * time.Second,
			CheckRedirect: func(*http.Request, []*http.Request) error {
				return http.ErrUseLastResponse
			},
		}
	})
	return siteClientDefault
}

// fetchSiteBundle asks the site for one design's bundle and returns its bytes. Every
// failure comes back already worded for the owner: the site's name is in the sentence, and
// the reason is one of the ones §7.1 distinguishes — unreachable, closed to us, private,
// deleted, refused, oversized, or not a theme at all.
func (s *Server) fetchSiteBundle(r *http.Request, site, id string) ([]byte, error) {
	host := siteHost(site)
	req, err := http.NewRequestWithContext(r.Context(), http.MethodGet, site+"/api/hub/theme/"+id, nil)
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
	switch {
	case resp.StatusCode >= 300 && resp.StatusCode < 400:
		return nil, fmt.Errorf("%s sent a sign-in page instead of the theme: the site isn’t open to Orb Manager yet", host)
	case resp.StatusCode == http.StatusNotFound:
		return nil, fmt.Errorf("that theme is private, or %s has no such theme", host)
	case resp.StatusCode == http.StatusGone:
		return nil, errors.New("that theme was deleted by its owner")
	case resp.StatusCode == http.StatusConflict:
		return nil, fmt.Errorf("%s can’t share that theme right now: %s", host, siteConflictMessage(resp.Body))
	case resp.StatusCode < 200 || resp.StatusCode > 299:
		return nil, fmt.Errorf("%s answered HTTP %d", host, resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, int64(siteBundleMax)+1))
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", host, err)
	}
	if len(body) > siteBundleMax {
		return nil, errors.New("that theme is too big to import (over 32 MB)")
	}
	return body, nil
}

// siteConflictMessage reads the site's own reason out of a 409's body, the way §8.4 writes
// it: "message", then "error", then nothing better than the status code.
func siteConflictMessage(rd io.Reader) string {
	var out struct {
		Message string `json:"message"`
		Error   string `json:"error"`
	}
	b, _ := io.ReadAll(io.LimitReader(rd, 8192))
	if json.Unmarshal(b, &out) == nil {
		if strings.TrimSpace(out.Message) != "" {
			return strings.TrimSpace(out.Message)
		}
		if strings.TrimSpace(out.Error) != "" {
			return strings.TrimSpace(out.Error)
		}
	}
	return "HTTP 409"
}

// --- the origin sidecar -------------------------------------------------------------

// ThemeOrigin is the record of where a shelf theme was imported from, held beside the
// theme rather than inside it: the bake's bytes are the orb's and are never rewritten to
// carry hub bookkeeping. H3's update check reads it to know which design to ask about.
type ThemeOrigin struct {
	Site       string `json:"site"`       // "https://orb.takacsfamily.com"
	DesignID   string `json:"designId"`   // the 22-char design id
	Version    int    `json:"version"`    // the design version that was baked
	ImportedAt int64  `json:"importedAt"` // Unix milliseconds
}

// themeOriginPath is a theme's sidecar: the theme's own path with the suffix appended, so
// the record sits with the file it describes and the shelf's .orbtheme listing never sees
// it. Built from the theme's path rather than joined with "/" for the reason the rest of
// the library is: the hub runs on Windows too.
func themeOriginPath(themePath string) string {
	return themePath + ".origin.json"
}

// writeThemeOrigin files the record with the library's own atomic write: a half-written
// sidecar would read back as no sidecar at all, and the theme would look homemade.
func writeThemeOrigin(themePath string, o *ThemeOrigin) error {
	body, err := json.MarshalIndent(o, "", "  ")
	if err != nil {
		return err
	}
	return writeFileAtomic(themeOriginPath(themePath), append(body, '\n'))
}

// readThemeOrigin returns the theme's origin, or nil when the shelf holds no readable one:
// no file, not JSON, or a record missing the things that make it a record.
func readThemeOrigin(themePath string) *ThemeOrigin {
	body, err := os.ReadFile(themeOriginPath(themePath))
	if err != nil {
		return nil
	}
	var o ThemeOrigin
	if json.Unmarshal(body, &o) != nil {
		return nil
	}
	if o.Site == "" || o.DesignID == "" || o.Version < 1 {
		return nil
	}
	return &o
}

// removeThemeOrigin takes a theme's sidecar with it when the theme is dropped. Having no
// sidecar is the normal case, so a missing file is success.
func removeThemeOrigin(themePath string) error {
	if err := os.Remove(themeOriginPath(themePath)); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return nil
}

// --- the doors ----------------------------------------------------------------------

func (s *Server) routesSiteImport(mux *http.ServeMux) {
	mux.Handle("/api/site/import", s.requireControl(http.HandlerFunc(s.handleSiteImport)))
	mux.Handle("/api/site/bundle", s.requireControl(http.HandlerFunc(s.handleSiteBundle)))
	mux.Handle("/api/site/keep", s.requireControl(http.HandlerFunc(s.handleSiteKeep)))
}

// handleSiteImport answers POST /api/site/import with {"link": …}: the paste, fetched,
// checked and held under a ticket. It files nothing; the theme reaches the shelf only when
// a bake arrives at /api/site/keep.
func (s *Server) handleSiteImport(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, siteLinkMax+1))
	if err != nil || len(body) > siteLinkMax {
		s.jsonErr(w, siteLinkRefused)
		return
	}
	var in struct {
		Link string `json:"link"`
	}
	if json.Unmarshal(body, &in) != nil || strings.TrimSpace(in.Link) == "" {
		s.jsonErr(w, siteLinkRefused)
		return
	}
	site, id, err := parseSiteLink(in.Link)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	raw, err := s.fetchSiteBundle(r, site, id)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	b, err := checkBundle(raw, id)
	if err != nil {
		if errors.Is(err, errSiteBundleNo) {
			s.jsonErr(w, siteHost(site)+" did not answer with a theme")
		} else {
			s.jsonErr(w, "the theme from "+siteHost(site)+" is damaged: "+err.Error())
		}
		return
	}
	ticket, err := s.tickets().add(&siteTicket{
		site: site, id: id, version: b.Version, app: b.App,
		name: b.Name, sharedName: b.SharedName, handle: b.Handle, raw: b.Raw,
	})
	if err != nil {
		s.jsonErr(w, "the import could not be held: "+err.Error())
		return
	}
	log.Printf("site import: %s %s v%d -> %q", site, id, b.Version, b.SharedName)
	s.jsonOK(w, map[string]any{
		"ticket": ticket, "site": site, "id": id, "version": b.Version,
		"app": b.App, "name": b.Name, "sharedName": b.SharedName, "handle": b.Handle,
	})
}

// handleSiteBundle answers GET /api/site/bundle?ticket=: the held bundle, byte for byte as
// the site served it, for the editor the Themes tab opened. Readable until it expires, so
// an editor reload asks again and gets the same bytes.
func (s *Server) handleSiteBundle(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	t, ok := s.tickets().get(r.URL.Query().Get("ticket"))
	if !ok {
		s.jsonErr(w, siteTicketGone)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write(t.raw)
}

// handleSiteKeep answers POST /api/site/keep?ticket= with the baked .orbtheme the editor
// made from the held bundle. The bake must be that design's bake — the app the bundle named
// and the name the bundle named — before it is filed, because the origin record written
// beside it claims both.
func (s *Server) handleSiteKeep(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	ticket := r.URL.Query().Get("ticket")
	t, ok := s.tickets().get(ticket)
	if !ok {
		s.jsonErr(w, siteTicketGone)
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, int64(orbnet.MaxThemeUpload)+1))
	if err != nil || len(body) > orbnet.MaxThemeUpload {
		s.jsonErr(w, "that theme is over what an orb will accept")
		return
	}
	c, err := orbnet.ContainerName(body)
	if err != nil {
		s.jsonErr(w, strings.TrimPrefix(err.Error(), "orbnet: "))
		return
	}
	if len(c.Sections) == 0 || c.Sections[0] != t.app {
		s.jsonErr(w, "that theme is not one of the "+t.app+" app’s")
		return
	}
	if c.Name != t.sharedName {
		s.jsonErr(w, "the bake is named “"+c.Name+"”, not “"+t.sharedName+"”")
		return
	}
	// fold=false: the import's bake joins the shelf, it does not discard an unsent draft
	// sitting under the same name.
	dest, err := s.library().keepTheme(body, false)
	if err != nil {
		s.jsonErr(w, "the theme could not be kept: "+err.Error())
		return
	}
	origin := &ThemeOrigin{Site: t.site, DesignID: t.id, Version: t.version, ImportedAt: s.tickets().clock().UnixMilli()}
	if err := writeThemeOrigin(dest, origin); err != nil {
		// The theme is on the shelf and the record is not: say exactly that, and leave the
		// ticket live so a retry of the keep can complete the pair.
		s.jsonErr(w, "the theme was kept, but where it came from could not be recorded: "+err.Error())
		return
	}
	s.tickets().consume(ticket)
	log.Printf("site keep: %s %s v%d -> %s", t.site, t.id, t.version, dest)
	s.jsonOK(w, map[string]any{"name": c.Name, "file": filepath.Base(dest), "origin": origin})
}
