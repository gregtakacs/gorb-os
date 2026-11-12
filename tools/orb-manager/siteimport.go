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
//
// H2 §5 grows this door a second body, {"designId": …}: one of the signed-in owner's own
// designs, named by id instead of pasted as a link, fetched from the site this hub is linked
// to with that link's bearer on the same GET. Everything past the fetch is the same code —
// one bundle door, one check, one ticket, one origin record — and an own design imports
// exactly like a shared one and tags "from <host>" the same.

import (
	"context"
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
	"strconv"
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

// siteDesignIDRe is §5's import-by-id grammar: the same 22 characters siteLinkRe admits
// inside a /t/ path, named on its own because the id body carries no path to match. A hub
// that let a longer, shorter or punctuated id through would be dialing a design the site
// cannot have, and §2.6's answer to that is the stranger's 404.
var siteDesignIDRe = regexp.MustCompile(`^[A-Za-z0-9_-]{22}$`)

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
	// S3 (ruling 35): a well-formed link to a host this hub never opens is refused here,
	// before any request — which is the whole point of the gate. A paste that is not a
	// link at all still gets the one link sentence, because shape is the first question.
	if site := scheme + "://" + strings.ToLower(u.Host); !siteAllowed(site) {
		return "", "", siteNotAllowed(site)
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
	// Schema is the design schema the bundle names, or 0 when it names none: A1 §7.5 looks it
	// up in the store's readers to say which app version the theme needs.
	Schema int
	Raw    []byte
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
	// S3b: the shared name is a name the site let someone type, and the hub is about to
	// bake it into an orb and file it on a shelf under its own spelling rules — so the
	// site's "<name> by @<handle>" is split here and both halves are read. A bundle that
	// fails this one is whole, like bad art: re-downloading changes nothing, so it is
	// worded "can't be imported" and not "is damaged".
	if _, _, sentence := checkSharedName(shared); sentence != "" {
		return nil, &bundleArtError{sentence}
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
	if sc, err := siteJSONNumber(obj["schema"]); err == nil && sc >= 1 && sc <= 1<<20 && sc == float64(int64(sc)) {
		b.Schema = int(sc)
	}
	return b, nil
}

// checkBundleArt walks the design's orbAssets sidecar and proves the bundle carries the
// art it claims: every hash has a body in assets, every body decodes, and every decoded
// body still is the bytes its hash names. A bundle that fails this imports an image the
// editor would draw wrong, so it does not import. S3 (ruling 35) adds the site's own
// bounds beside those promises: every picture, sidecar or inline, is a PNG, JPEG, GIF or
// WebP whose header names sides within artMaxSide. Art refusals come back as
// *bundleArtError so the owner hears "can't be imported", not "is damaged".
func checkBundleArt(design map[string]json.RawMessage, assets map[string]string) error {
	if err := checkSidecarArt(design, assets); err != nil {
		return err
	}
	// Art the design carries inline — a data URL in the document itself, not in the
	// sidecar — clears the same bar. The sidecar is the site's storage shape, not a
	// permission: a bundle may sketch around it, and the hub is the last checkpoint.
	// checkInlineArt walks the document with sorted keys, so one bundle refuses with
	// one sentence every time.
	doc := make(map[string]any, len(design))
	for k, raw := range design {
		if k == "orbAssets" {
			continue
		}
		var v any
		if json.Unmarshal(raw, &v) != nil {
			continue
		}
		doc[k] = v
	}
	return checkInlineArt(doc)
}

// checkSidecarArt proves the orbAssets rows hold what they promise, and then that what
// they hold is a picture a design may carry.
func checkSidecarArt(design map[string]json.RawMessage, assets map[string]string) error {
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
		// S3 (ruling 35): the bytes are the ones the bundle promised; now the site's
		// rules say what they are allowed to be. The prefix is checked after the hashes
		// so a damaged bundle still reports itself as damaged, never as oversized art.
		prefix, ok := siteJSONString(row["prefix"])
		if !ok {
			return &bundleArtError{artRefusalType}
		}
		if s := artRefusal(prefix, img); s != "" {
			return &bundleArtError{s}
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

// siteFetchError is a site answer a caller must act on beyond the sentence the owner
// reads: the HTTP status behind it, carried beside wording that is already final. Every
// message a siteFetchError returns is the sentence fetchSiteBundle has always answered
// with — the type exists so H2's import-by-id door can tell a 401 (the link died) from a
// 404 (a wrong id is a wrong paste) without either one rewriting what the H1 door says.
type siteFetchError struct {
	status int
	msg    string
}

func (e *siteFetchError) Error() string { return e.msg }

// fetchSiteBundle asks the site for one design's bundle and returns its bytes. Every
// failure comes back already worded for the owner: the site's name is in the sentence, and
// the reason is one of the ones §7.1 distinguishes — unreachable, closed to us, private,
// deleted, refused, oversized, or not a theme at all.
//
// bearer is H2 §2.6's own-bundle door: an empty bearer is the anonymous H1 paste, byte for
// byte what it always sent, and a non-empty one is the hub's link credential, sent to the
// site it was minted for and to nobody else. One client, one redirect rule, one cap — the
// two doors differ by this header and nothing else.
func (s *Server) fetchSiteBundle(r *http.Request, site, id, bearer string) ([]byte, error) {
	host := siteHost(site)
	req, err := http.NewRequestWithContext(r.Context(), http.MethodGet, site+"/api/hub/theme/"+id, nil)
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", host, err)
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "orb-manager/"+version)
	if bearer != "" {
		req.Header.Set("Authorization", "Bearer "+bearer)
	}
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", host, err)
	}
	defer resp.Body.Close()
	switch {
	case resp.StatusCode == http.StatusUnauthorized && bearer != "":
		// §2.6: a bearer that does not resolve to a live link is told so even for a public
		// design, so a hub holding a dead token learns its token is dead. What the owner
		// hears is the door's own sentence, which is this call's caller to word.
		return nil, &siteFetchError{http.StatusUnauthorized, fmt.Sprintf("%s refused the link's credential", host)}
	case resp.StatusCode >= 300 && resp.StatusCode < 400:
		return nil, fmt.Errorf("%s sent a sign-in page instead of the theme: the site isn’t open to Orb Manager yet", host)
	case resp.StatusCode == http.StatusNotFound:
		return nil, &siteFetchError{http.StatusNotFound, fmt.Sprintf("that theme is private, or %s has no such theme", host)}
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

// removeThemeOrigin takes a theme's sidecar with it when the theme is dropped, and the by-name
// gone marker beside it too: both are facts about the same theme, and a marker left standing
// after its theme is gone is a fact about a file that no longer exists — and, the day a theme of
// that name comes back, a silence nobody chose. Having neither is the normal case, so a missing
// file is success.
func removeThemeOrigin(themePath string) error {
	for _, p := range []string{themeOriginPath(themePath), themeGonePath(themePath)} {
		if err := os.Remove(p); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
	}
	return nil
}

// --- the resolve client -------------------------------------------------------------

// resolveAnswer is what the site says about one design an imported theme came from. The
// fields are the minimum G3 needs to draw a row, and slice H3 owns the whole response:
// §7.4's point-3 name-based lookup and any handle history live in the site's own handler,
// so this client sends the record it holds and reads back at most these keys.
//
// Gone and NotYet are the two ways the site says nothing about the design, and they are
// not the same sentence: Gone is the site's own answer that the design is no longer
// shared, and NotYet is a site that has no resolve door to answer at — which the hub
// reports as information and keeps checking.
type resolveAnswer struct {
	Resolved   bool
	DesignID   string
	App        string
	SharedName string
	Version    int
	Gone       bool
	NotYet     bool
}

// resolveOrigin asks the site a theme came from for that design's CURRENT version and
// CURRENT shared name (§7.4). A 404 or a 410 is not a transport failure and is not
// reported as one, and it is not taken for news about the design either: only a body that
// says so in its own JSON (§7.4's Gone answer, the one H3 will serve) is Gone. Any other
// not-found — a generic 404 page, a router's `{"error":"not found"}`, an empty body — is
// a site that cannot answer this question yet, which is NotYet: the row says so, nothing
// is recorded, and the next check asks again. Anything else — unreachable, sign-in page,
// HTTP 500 — is an error, and the row that could not be checked says so rather than
// claiming up to date.
//
// H2's credential rides along on exactly one condition: this hub holds a live link to the
// very site being asked, which is the site that minted the token. Every other origin is
// asked anonymously, as the paste door has always been.
func (s *Server) resolveOrigin(ctx context.Context, o *ThemeOrigin) (*resolveAnswer, error) {
	if o == nil || o.Site == "" || o.DesignID == "" {
		return nil, errors.New("this theme has no origin to resolve")
	}
	// S3 (ruling 35): the update check asks the same gate the import does. An origin
	// sidecar naming another host is refused before its socket opens, so the check
	// reports it exactly as plainly as an import does — and stops the asking.
	if !siteAllowed(o.Site) {
		return nil, siteNotAllowed(o.Site)
	}
	return s.resolveAsk(ctx, o.Site, "site="+url.QueryEscape(o.Site)+
		"&designId="+url.QueryEscape(o.DesignID)+
		"&version="+strconv.Itoa(o.Version))
}

// resolveName asks the site the hub is configured with about a theme its shelf holds under a
// shared name and records nothing about (§7.4's point 3, h3-design.md §7.2): a theme that came
// off a file rather than through the import door has no sidecar to resolve by, and its name and
// its app are the only question that can be asked of it. The door answers a name with the name
// its bundle door would give the same caller at the design's CURRENT version, so an adoption
// never records a name the site would not have credited, and an old handle on the shelf reads
// back as the rename the site already made.
//
// `app` is part of the question because the door requires it: one shared name can exist in two
// apps, and the hub always knows the app the theme on its shelf themes. The site asked is the
// configured one — the same origin a bake is fetched from — whether or not this hub's link
// points there: one origin of truth, and a ` by @<someone>` naming another account is simply a
// miss here rather than a second origin to go hunting for.
//
// The answer's classification table is resolveOrigin's, byte for byte and shared through
// resolveAsk, because a hub with two resolve clients that disagree about what a 404 means would
// be a hub with two stories about one shelf.
func (s *Server) resolveName(ctx context.Context, site, app, name string) (*resolveAnswer, error) {
	if site == "" || app == "" || name == "" {
		return nil, errors.New("this theme has no name to resolve")
	}
	if !siteAllowed(site) {
		return nil, siteNotAllowed(site)
	}
	return s.resolveAsk(ctx, site, "site="+url.QueryEscape(site)+
		"&app="+url.QueryEscape(app)+
		"&name="+url.QueryEscape(name))
}

// resolveAsk is the resolve door's whole HTTP half both clients share: the gate the caller has
// already checked is the only thing standing between here and the socket, one GET with the hub's
// ordinary door headers, H2's credential on the one condition siteBearerFor states, and the
// reading of an answer a design's identity fits into a line.
func (s *Server) resolveAsk(ctx context.Context, site, query string) (*resolveAnswer, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, site+"/api/hub/resolve?"+query, nil)
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", siteHost(site), err)
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "orb-manager/"+version)
	if bearer := s.siteBearerFor(site); bearer != "" {
		req.Header.Set("Authorization", "Bearer "+bearer)
	}
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", siteHost(site), err)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound || resp.StatusCode == http.StatusGone {
		// The body is read to a ceiling rather than drained: the whole Gone answer is a
		// short JSON object, and a site with no resolve door answers with a page that is
		// nothing but words. A body that cannot be read is no answer either, which is the
		// same NotYet as a body that does not say it.
		b, _ := io.ReadAll(io.LimitReader(resp.Body, resolveAnswerBodyMax))
		if resolveSaysGone(resp.Header.Get("Content-Type"), b) {
			return &resolveAnswer{Gone: true}, nil
		}
		return &resolveAnswer{NotYet: true}, nil
	}
	if resp.StatusCode >= 300 && resp.StatusCode < 400 {
		return nil, fmt.Errorf("%s sent a sign-in page instead of an answer: the site isn’t open to Orb Manager yet", siteHost(site))
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return nil, fmt.Errorf("%s answered HTTP %d", siteHost(site), resp.StatusCode)
	}
	var body struct {
		DesignID   string `json:"designId"`
		App        string `json:"app"`
		Version    int    `json:"version"`
		SharedName string `json:"sharedName"`
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, resolveAnswerBodyMax))
	if err != nil {
		return nil, fmt.Errorf("could not read %s's answer: %v", siteHost(site), err)
	}
	if err := json.Unmarshal(b, &body); err != nil {
		return nil, fmt.Errorf("%s answered with something that is not a design", siteHost(site))
	}
	if body.DesignID == "" || body.Version < 1 {
		return nil, fmt.Errorf("%s answered without a design id or a version", siteHost(site))
	}
	return &resolveAnswer{Resolved: true, DesignID: body.DesignID, App: body.App,
		SharedName: body.SharedName, Version: body.Version}, nil
}

// resolveAnswerBodyMax bounds what a resolve answer is read to, whichever status carried
// it. A design's current version and name, and the one word that says a design is retired,
// are all short JSON; megabytes under a 404 are a page and nothing else.
const resolveAnswerBodyMax = 8192

// resolveSaysGone is §7.4's Gone answer and the only thing read as one: a not-found whose
// Content-Type is application/json and whose body is a JSON object carrying "gone" true,
// e.g. {"gone":true}. A page, an empty body, JSON that says anything else, and a body that
// is not a JSON object all answer a different question than this one, which is the site
// not having the door yet rather than the design having gone away. Anything other than a
// plain true — "true", 1, a null — is not a true either.
func resolveSaysGone(contentType string, body []byte) bool {
	if !strings.EqualFold(strings.TrimSpace(strings.Split(contentType, ";")[0]), "application/json") {
		return false
	}
	var ans struct {
		Gone *bool `json:"gone"`
	}
	if json.Unmarshal(body, &ans) != nil || ans.Gone == nil {
		return false
	}
	return *ans.Gone
}

// --- the doors ----------------------------------------------------------------------

func (s *Server) routesSiteImport(mux *http.ServeMux) {
	mux.Handle("/api/site/import", s.requireControl(http.HandlerFunc(s.handleSiteImport)))
	mux.Handle("/api/site/bundle", s.requireControl(http.HandlerFunc(s.handleSiteBundle)))
	mux.Handle("/api/site/keep", s.requireControl(http.HandlerFunc(s.handleSiteKeep)))
}

// handleSiteImport answers POST /api/site/import with one of two bodies: {"link": …}, the
// paste §7.1 H1 opened, or {"designId": …}, H2 §5's own-design door — one of the signed-in
// owner's designs fetched through the same bundle door with the link's bearer on it, which
// is what makes the site answer with the owner's own bundle. Both shapes end at the same
// fetch, the same checkBundle and the same ticket: one import path, one origin record. It
// files nothing; the theme reaches the shelf only when a bake arrives at /api/site/keep.
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
		Link     string `json:"link"`
		DesignID string `json:"designId"`
	}
	link, designID := "", ""
	if json.Unmarshal(body, &in) == nil {
		link = strings.TrimSpace(in.Link)
		designID = strings.TrimSpace(in.DesignID)
	}
	// One request, one shape. The two fields are alternatives, and a blank counts as absent:
	// a body naming both names two designs at once, and a body naming neither was sent by a
	// form nobody typed into. Both are the same mistake as an unreadable paste, so they are
	// the same sentence.
	if (link == "") == (designID == "") {
		s.jsonErr(w, siteLinkRefused)
		return
	}
	site, id, bearer := "", "", ""
	var own *siteLink
	if link != "" {
		site, id, err = parseSiteLink(link)
		if err != nil {
			s.jsonErr(w, err.Error())
			return
		}
	} else {
		own = s.liveSiteLink()
		if own == nil {
			// The door needs a link to be somebody's own design, and the hub says which half
			// it is missing: no credential at all is "not connected", never a fetch attempted
			// with nothing in the header.
			s.jsonErr(w, siteMsgNotConnected)
			return
		}
		if !siteDesignIDRe.MatchString(designID) {
			s.jsonErr(w, siteLinkRefused)
			return
		}
		// The site is never the caller's to name: an own design lives on the site this
		// hub's credential was minted for, which is the only origin a bearer may be sent to.
		site, id, bearer = siteReleaseBase(), designID, own.Token
	}
	raw, err := s.fetchSiteBundle(r, site, id, bearer)
	if err != nil {
		var stErr *siteFetchError
		switch {
		case errors.As(err, &stErr) && stErr.status == http.StatusUnauthorized:
			// §2.6: a dead bearer is told it is dead even in front of a public design, so
			// this is the moment the hub learns the link is over. It forgets the credential
			// exactly as §5's designs door does, on the same answer from the same site.
			s.forgetSiteLink(own)
			s.jsonErr(w, siteMsgDisconnected)
			return
		case errors.As(err, &stErr) && stErr.status == http.StatusNotFound && bearer != "":
			// §2.6 answers a design this link may not see with the stranger's 404, on
			// purpose. A mistyped id and a mistyped paste are one user mistake, so they are
			// one sentence.
			s.jsonErr(w, siteLinkRefused)
			return
		}
		s.jsonErr(w, err.Error())
		return
	}
	b, err := checkBundle(raw, id)
	if err != nil {
		var artErr *bundleArtError
		switch {
		case errors.Is(err, errSiteBundleNo):
			s.jsonErr(w, siteHost(site)+" did not answer with a theme")
		case errors.As(err, &artErr):
			// S3 (ruling 35): the bundle is whole; its art is outside what a design may
			// hold. Re-downloading fixes a damaged theme and fixes nothing here, so the
			// sentence says which one the owner is holding.
			s.jsonErr(w, "the theme from "+siteHost(site)+" can't be imported: "+artErr.sentence)
		default:
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
		"app": b.App, "name": b.Name, "sharedName": b.SharedName, "handle": b.Handle, "schema": b.Schema,
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
	// S3b, asked again at the door that files: import checked the shared name before it
	// wrote a ticket, and a ticket is a promise the keep is about to put on the shelf —
	// a promise made by an older hub, or by an import whose rules have since tightened,
	// is checked here rather than trusted.
	if _, _, sentence := checkSharedName(t.sharedName); sentence != "" {
		s.jsonErr(w, "the theme from "+siteHost(t.site)+" can't be imported: "+sentence)
		return
	}
	if err := s.checkNewName(t.app, c.Name); err != nil {
		s.jsonErr(w, err.Error())
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
