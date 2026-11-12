package main

// h3-design.md §7.1 (H3 hub step 2): the inbox client — a hold on disk that survives a
// restart.
//
// Once a minute the hub asks its site link's inbox door (GET <siteBase>/api/hub/inbox),
// and for every item it does not already hold it fetches the PINNED version's bundle from
// GET <siteBase>/api/hub/inbox/<itemId>/bundle — the bundle door, never the theme door, so
// a design that moved from 4 to 7 since the send still delivers 4 (§3.3). The bytes go to
// disk BEFORE the claim is made: one file per item at <configDir>/site-inbox/<itemId>.json,
// directory 0700 created on first use, file mode 0600, written temp+rename+fsync in the
// state files' atomic style. Only then does the hub POST /api/hub/inbox/deliver; a claim
// answered missing (or with any error) deletes the file, because the site says this is not
// ours to deliver and the next poll re-lists the truth.
//
// The pass re-drives the disk before the network, which is what makes a restart free: an
// imported hold whose theme reached the shelf is deleted, an unclaimed hold is re-claimed,
// and only then does the hub ask for anything new. A crash between fetch and claim loses
// nothing: the bytes are on this disk, and the next pass files the claim the dead pass
// never made rather than re-fetching it.
//
// The tray's doors read the directory and never the network. Add runs the held bytes
// through the unchanged checkBundle, files them in the unchanged siteTicketStore, and
// answers exactly what handleSiteImport answers, so the browser's editor hand-off is the
// paste import's own flow. The keep path then writes the .origin.json sidecar that lets a
// later settle delete the hold. Dismiss deletes the file and tells the site, whose
// "missing" answer is precisely the agreement a dismiss means.

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// siteInboxEvery is the minute between asks: the slice's promise is that a send waits at
// most one interval to reach a computer.
const siteInboxEvery = 60 * time.Second

// siteInboxMax caps the holds on disk, the list door's own cap mirrored: a hub that held
// more could never have been offered more. A full hold stops the pass mid-list — the rest
// of the list stays waiting on the site and is asked again in a minute, so nothing is
// dropped and nothing is refused.
const siteInboxMax = 32

// siteInboxBodyMax bounds one inbox bundle: exactly the bundle cap the import door
// already enforces. It is a reuse, not a second number — a hub holding bytes its own
// check-and-ticket path would refuse would be a hold that cannot be added.
const siteInboxBodyMax = siteBundleMax

// siteInboxBootDelay is how long the loop waits before its first pass, mirroring the
// release check's boot delay: the hub has a page to serve and a cable to notice first.
const siteInboxBootDelay = 20 * time.Second

// siteInboxDirName is the hold directory's one name, a sibling of state.json and
// site-link.json in the config dir.
const siteInboxDirName = "site-inbox"

// siteInboxIDRe is the item-id grammar, the same 22 characters siteDesignIDRe admits. A
// file in the hold directory whose name does not match is skipped and logged, never
// opened: the directory is the hub's own, but a corrupt one is not a puzzle to be solved
// at 3am.
// The site mints inbox item ids with Prisma's cuid() (25 characters today); the grammar is
// the filename-safe set with a bound, not one length, so a hold file name can never be a path.
var siteInboxIDRe = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)

// siteInboxEnvelope is one hold file: exactly these keys, the pinned identity the list
// door named, the two flags that only move forward, and the bundle door's bytes base64-ed.
type siteInboxEnvelope struct {
	ItemID   string `json:"itemId"`
	DesignID string `json:"designId"`
	Name     string `json:"name"`
	App      string `json:"app"`
	Version  int    `json:"version"`
	SentAt   string `json:"sentAt"`
	Claimed  bool   `json:"claimed"`
	Imported bool   `json:"imported"`
	Raw      string `json:"raw"`
}

// siteInboxItem is one row of the list door's answer (§3.1): the item's id is "id", and
// the design it pins lives under "designId".
type siteInboxItem struct {
	ID       string `json:"id"`
	DesignID string `json:"designId"`
	Version  int    `json:"version"`
	App      string `json:"app"`
	Name     string `json:"name"`
	SentAt   string `json:"sentAt"`
}

// siteInboxDeliverAnswer is §3.2's body: what the site just marked delivered to this
// link, and what is not ours to deliver — which covers unknown, foreign, already
// delivered and hidden alike, and never arrives as an error.
type siteInboxDeliverAnswer struct {
	Delivered []string `json:"delivered"`
	Missing   []string `json:"missing"`
}

// siteInboxDir is the hold directory: <configDir>/site-inbox. An empty statePath has no
// config dir to hold anything in, and every door and pass treats that as an empty inbox.
func (s *Server) siteInboxDir() string {
	if s.statePath == "" {
		return ""
	}
	return filepath.Join(filepath.Dir(s.statePath), siteInboxDirName)
}

// --- the hold directory ---------------------------------------------------------------

// siteInboxWriteEnv files one envelope atomically at 0600, creating the directory 0700 on
// first use: temp file inside the directory, fsync, rename, directory fsync. A
// half-written hold would read back as no hold at all, and the whole point of a hold is
// that the bytes survive what comes next.
func (s *Server) siteInboxWriteEnv(dir, itemID string, env *siteInboxEnvelope) error {
	if dir == "" {
		return errors.New("no config directory: nowhere to hold the inbox")
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return err
	}
	body, err := json.Marshal(env)
	if err != nil {
		return err
	}
	to := filepath.Join(dir, itemID+".json")
	tmp := to + ".tmp"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	if _, err := f.Write(body); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, to); err != nil {
		os.Remove(tmp)
		return err
	}
	syncSiteInboxDir(dir)
	return nil
}

// syncSiteInboxDir flushes a directory entry so the rename itself survives the crash that
// follows it. A platform that will not let a directory be fsynced has already been given
// the best flush its filesystem offers.
func syncSiteInboxDir(dir string) {
	d, err := os.Open(dir)
	if err != nil {
		return
	}
	_ = d.Sync()
	_ = d.Close()
}

// siteInboxReadEnv reads one hold file back and proves it names the item its filename
// names, and that the hold can still do its job: an envelope that names a different id, or
// has lost its design, its version or its bytes, is a corrupt hold, and a hold you cannot
// tie to an item is not a hold.
func siteInboxReadEnv(path, itemID string) (*siteInboxEnvelope, error) {
	body, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var env siteInboxEnvelope
	if json.Unmarshal(body, &env) != nil {
		return nil, errors.New("not a hold envelope")
	}
	if env.ItemID != itemID {
		return nil, fmt.Errorf("the hold names item %q, not %q", env.ItemID, itemID)
	}
	if env.DesignID == "" || env.Version < 1 || env.Raw == "" {
		return nil, errors.New("the hold is missing its design, version or bytes")
	}
	return &env, nil
}

// siteInboxEntry is one readable hold with the directory metadata the tray's order needs.
type siteInboxEntry struct {
	path    string
	modTime time.Time
	env     *siteInboxEnvelope
}

// siteInboxList reads the directory's readable holds, in the order it found them. Files
// whose name is not an item id, and envelopes that will not read, are each logged and
// skipped — never opened by anything else, and never deleted by this code: a corrupt hold
// is fixed by a human with the directory open, not by a background pass guessing.
func siteInboxList(dir string) []siteInboxEntry {
	var out []siteInboxEntry
	if dir == "" {
		return out
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return out
	}
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".json") {
			continue
		}
		id := strings.TrimSuffix(name, ".json")
		if !siteInboxIDRe.MatchString(id) {
			log.Printf("orb-manager: site inbox: %s is not an item id: skipped, never opened", name)
			continue
		}
		path := filepath.Join(dir, name)
		env, err := siteInboxReadEnv(path, id)
		if err != nil {
			log.Printf("orb-manager: site inbox: %s: %v", name, err)
			continue
		}
		info, err := e.Info()
		if err != nil {
			log.Printf("orb-manager: site inbox: %s: %v", name, err)
			continue
		}
		out = append(out, siteInboxEntry{path: path, modTime: info.ModTime(), env: env})
	}
	return out
}

// siteInboxCount is the hold count the fetch loop re-reads each iteration: every file in
// the directory named like an item, readable or not. The cap bounds the disk, and
// unreadable bytes are still bytes.
func siteInboxCount(dir string) int {
	if dir == "" {
		return 0
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return 0
	}
	n := 0
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".json") {
			continue
		}
		if siteInboxIDRe.MatchString(strings.TrimSuffix(name, ".json")) {
			n++
		}
	}
	return n
}

// --- the pass -------------------------------------------------------------------------

// siteInboxLoop is the minute's rhythm, copied from the release check's once-then-tick:
// one pass after a short boot delay, then one per siteInboxEvery on a ticker. A ticker
// drops ticks rather than queueing them, so a pass that outlives the interval costs the
// next tick and not a backlog. A failed pass is one log line: the site will still be
// there in a minute, and the tray door works off disk regardless.
func (s *Server) siteInboxLoop(ctx context.Context) {
	select {
	case <-ctx.Done():
		return
	case <-time.After(siteInboxBootDelay):
	}
	t := time.NewTicker(siteInboxEvery)
	defer t.Stop()
	for {
		if err := s.pollSiteInbox(ctx); err != nil {
			log.Printf("orb-manager: site inbox: %v", err)
		}
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
	}
}

// pollSiteInbox runs one pass: the disk first, then the network. An unlinked hub — no
// link, or a link with no token — is not an error; there is nothing to ask and nothing an
// answer could change, and the pass returns nil having done nothing. The mutex is held
// across the whole pass: a tray Add or Dismiss mid-pass would race the settle and the
// claim bookkeeping, and a control request waiting a few seconds behind the minute's pass
// is the cheaper disagreement.
func (s *Server) pollSiteInbox(ctx context.Context) error {
	link := s.liveSiteLink()
	if link == nil || strings.TrimSpace(link.Token) == "" {
		return nil
	}
	s.siteInboxMu.Lock()
	defer s.siteInboxMu.Unlock()
	if ctx.Err() != nil {
		return ctx.Err()
	}
	s.siteInboxSettle()
	s.siteInboxReclaim(ctx, link)
	return s.siteInboxFetch(ctx, link)
}

// siteInboxSettled answers the settle question: does the library now hold a theme whose
// origin record names this envelope's design at a version at least the envelope's? The
// sidecar the keep path writes is the only evidence read, and a theme re-baked or updated
// past the pinned version counts — the news the hold carried is on the shelf either way.
func (s *Server) siteInboxSettled(env *siteInboxEnvelope) bool {
	dir := s.library().ThemesDir
	if dir == "" {
		return false
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		return false
	}
	for _, e := range entries {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".orbtheme") {
			continue
		}
		o := readThemeOrigin(filepath.Join(dir, e.Name()))
		if o != nil && o.DesignID == env.DesignID && o.Version >= env.Version {
			return true
		}
	}
	return false
}

// siteInboxSettle is step 1: every hold whose import completed and reached the shelf dies,
// which is what keeps a week of sends from accreting bytes for themes already installed.
// An imported hold whose keep never came back stays: the tray still offers Add, and a bake
// that never arrived is a retriable state, not a hole to bury.
func (s *Server) siteInboxSettle() {
	dir := s.siteInboxDir()
	for _, ent := range siteInboxList(dir) {
		if !ent.env.Imported || !s.siteInboxSettled(ent.env) {
			continue
		}
		if err := os.Remove(ent.path); err != nil && !errors.Is(err, os.ErrNotExist) {
			log.Printf("orb-manager: site inbox: the settled hold %s could not be removed: %v", ent.env.ItemID, err)
		}
	}
}

// siteInboxReclaim is step 2: every hold written but never claimed — the crash that sat
// between the fsync and the deliver — asks for its claim again, one id per POST as the
// spec's body prescribes. delivered rewrites the envelope claimed; anything else deletes
// the hold, because the item was never consumed by the claim and the next list either
// re-offers it or honestly says it is gone.
func (s *Server) siteInboxReclaim(ctx context.Context, link *siteLink) {
	dir := s.siteInboxDir()
	var ids []string
	for _, ent := range siteInboxList(dir) {
		if !ent.env.Claimed {
			ids = append(ids, ent.env.ItemID)
		}
	}
	if len(ids) == 0 || ctx.Err() != nil {
		return
	}
	sort.Strings(ids)
	site := siteReleaseBase()
	for _, id := range ids {
		ans, why := s.siteInboxDeliver(ctx, site, link.Token, []string{id})
		if why != "" {
			s.siteInboxDropHolds(why, []string{id})
			continue
		}
		if !containsSiteInboxID(ans.Delivered, id) {
			s.siteInboxDropHolds("answered missing", []string{id})
			continue
		}
		env, err := siteInboxReadEnv(filepath.Join(dir, id+".json"), id)
		if err != nil {
			continue // dismissed or corrupted between the listing and this claim's answer
		}
		env.Claimed = true
		if err := s.siteInboxWriteEnv(dir, id, env); err != nil {
			// The site's claim already stands; leaving the file unclaimed re-asks a minute
			// from now and the second answer will be missing, which deletes it. A hold the
			// import may already be reading dies to a write error is the wrong trade.
			log.Printf("orb-manager: site inbox: the claim on %s could not be filed: %v", id, err)
		}
	}
}

// siteInboxFetch is steps 3 and 4: ask the list door, hold what is new, then claim the
// new holds in one deliver call for the pass. A dead list answer ends the pass — with one
// log line, never a crash and never a tray outage, because the tray reads disk.
func (s *Server) siteInboxFetch(ctx context.Context, link *siteLink) error {
	site := siteReleaseBase()
	items, err := s.siteInboxListDoor(ctx, site, link.Token)
	if err != nil {
		var stErr *siteFetchError
		if errors.As(err, &stErr) && stErr.status == http.StatusUnauthorized {
			// The same answer, forgotten the same way, as §5's designs door and the import
			// doors: a bearer the site will not resolve is a link over, and the hub stops
			// asking rather than logging a minute of 401s forever.
			s.forgetSiteLink(link)
			log.Printf("orb-manager: site inbox: %v", err)
			return nil
		}
		return err
	}
	held := map[string]bool{}
	dir := s.siteInboxDir()
	for _, ent := range siteInboxList(dir) {
		held[ent.env.ItemID] = true
	}
	var written []string
	for _, it := range items {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if !siteInboxIDRe.MatchString(it.ID) || !siteDesignIDRe.MatchString(it.DesignID) || it.Version < 1 {
			log.Printf("orb-manager: site inbox: the site listed %q with an unreadable identity: skipped", it.ID)
			continue
		}
		if held[it.ID] {
			continue
		}
		if siteInboxCount(dir) >= siteInboxMax {
			// Full is not an error: the rest of the list stays waiting, is offered again in
			// a minute, and the unclaimed holds made before the wall get claimed below.
			log.Printf("orb-manager: site inbox: %d holds on disk is the cap: the rest of the list waits for next minute", siteInboxMax)
			break
		}
		raw, err := s.siteInboxBundle(ctx, site, link.Token, it.ID)
		if err != nil {
			// Non-200 and oversize both mean "no file, ask again next minute": the item
			// stays waiting on the site either way, so one line is the whole response.
			log.Printf("orb-manager: site inbox: %s: %v", it.ID, err)
			continue
		}
		// The same checks an Import from orb-os.net applies, before anything is held or
		// claimed: a bundle this hub would refuse to open is left waiting on the site.
		if _, err := checkBundle(raw, it.DesignID); err != nil {
			log.Printf("orb-manager: site inbox: %s: %v", it.ID, err)
			continue
		}
		env := &siteInboxEnvelope{
			ItemID: it.ID, DesignID: it.DesignID, Name: it.Name, App: it.App,
			Version: it.Version, SentAt: it.SentAt, Raw: base64.StdEncoding.EncodeToString(raw),
		}
		if err := s.siteInboxWriteEnv(dir, it.ID, env); err != nil {
			log.Printf("orb-manager: site inbox: %s could not be held: %v", it.ID, err)
			continue
		}
		held[it.ID] = true
		written = append(written, it.ID)
	}
	if len(written) == 0 {
		return nil
	}
	// Step 4: one deliver call for the pass's new holds, §3.2's per-id outcomes applied
	// exactly as step 2 applies them.
	ans, why := s.siteInboxDeliver(ctx, site, link.Token, written)
	if why != "" {
		s.siteInboxDropHolds(why, written)
		return nil
	}
	delivered := map[string]bool{}
	for _, id := range ans.Delivered {
		delivered[id] = true
	}
	var missing []string
	for _, id := range written {
		if !delivered[id] {
			missing = append(missing, id)
			continue
		}
		env, err := siteInboxReadEnv(filepath.Join(dir, id+".json"), id)
		if err != nil {
			continue
		}
		env.Claimed = true
		if err := s.siteInboxWriteEnv(dir, id, env); err != nil {
			log.Printf("orb-manager: site inbox: the claim on %s could not be filed: %v", id, err)
		}
	}
	if len(missing) > 0 {
		s.siteInboxDropHolds("answered missing", missing)
	}
	return nil
}

// siteInboxDropHolds deletes the holds a refused claim spoke for, with one line saying
// why: the site's "missing" and a dead bearer's 401 mean the same thing to this disk —
// not ours to deliver, and the next list is the truth.
func (s *Server) siteInboxDropHolds(why string, ids []string) {
	dir := s.siteInboxDir()
	for _, id := range ids {
		if err := os.Remove(filepath.Join(dir, id+".json")); err != nil && !errors.Is(err, os.ErrNotExist) {
			log.Printf("orb-manager: site inbox: the hold %s could not be dropped: %v", id, err)
		}
	}
	log.Printf("orb-manager: site inbox: the claim for %s %s: the holds were dropped, and the next pass re-lists the truth",
		strings.Join(ids, ", "), why)
}

// --- the site's three doors ------------------------------------------------------------

// containsSiteInboxID is membership in one of §3.2's id arrays.
func containsSiteInboxID(ids []string, want string) bool {
	for _, id := range ids {
		if id == want {
			return true
		}
	}
	return false
}

// siteInboxRequest builds a call the site's inbox doors expect: the link's bearer to the
// site it was minted for and nowhere else, no-store so no cache can answer for a minute's
// news, and the hub's own UA. The timeout is the site call's existing one, the same
// patience the designs door asks with.
func siteInboxRequest(ctx context.Context, method, url, bearer string, body []byte) (*http.Request, context.Context, context.CancelFunc, error) {
	cctx, cancel := context.WithTimeout(ctx, siteCallTimeout)
	var rd io.Reader
	if body != nil {
		rd = bytes.NewReader(body)
	}
	req, err := http.NewRequestWithContext(cctx, method, url, rd)
	if err != nil {
		cancel()
		return nil, ctx, func() {}, err
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "orb-manager/"+version)
	req.Header.Set("Cache-Control", "no-store")
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if bearer != "" {
		req.Header.Set("Authorization", "Bearer "+bearer)
	}
	return req, cctx, cancel, nil
}

// siteInboxListDoor asks §3.1 and returns its items in the door's own oldest-first order.
// A 401 comes back as a status-carrying siteFetchError so the caller can forget the link
// exactly as the import doors do; every other non-200, and a body over the designs door's
// cap, is an error the pass logs and ends on.
func (s *Server) siteInboxListDoor(ctx context.Context, site, bearer string) ([]siteInboxItem, error) {
	req, _, cancel, err := siteInboxRequest(ctx, http.MethodGet, site+"/api/hub/inbox", bearer, nil)
	if err != nil {
		return nil, err
	}
	defer cancel()
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", siteHost(site), err)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusUnauthorized {
		return nil, &siteFetchError{http.StatusUnauthorized, fmt.Sprintf("%s refused the link's credential", siteHost(site))}
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("%s answered HTTP %d at the inbox door", siteHost(site), resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, int64(siteDesignsBodyMax)+1))
	if err != nil {
		return nil, fmt.Errorf("could not read %s: %v", siteHost(site), err)
	}
	if len(body) > siteDesignsBodyMax {
		return nil, fmt.Errorf("%s sent an inbox list the designs door's cap would not carry", siteHost(site))
	}
	var out struct {
		Items []siteInboxItem `json:"items"`
	}
	if err := json.Unmarshal(body, &out); err != nil {
		return nil, fmt.Errorf("%s answered the inbox door with something unreadable", siteHost(site))
	}
	return out.Items, nil
}

// siteInboxBundle asks §3.3 for one item's PINNED bundle — the inbox bundle door, never
// the theme door, which is what pins the version. Anything that is not a 200 within the
// import door's size cap is an error the pass logs and moves on over: the item stays
// waiting on the site, and next minute the list says whether it still exists.
func (s *Server) siteInboxBundle(ctx context.Context, site, bearer, itemID string) ([]byte, error) {
	req, _, cancel, err := siteInboxRequest(ctx, http.MethodGet, site+"/api/hub/inbox/"+itemID+"/bundle", bearer, nil)
	if err != nil {
		return nil, err
	}
	defer cancel()
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		return nil, fmt.Errorf("could not reach %s: %v", siteHost(site), err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("%s would not send item %s (HTTP %d)", siteHost(site), itemID, resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, int64(siteInboxBodyMax)+1))
	if err != nil {
		return nil, fmt.Errorf("could not read %s: %v", siteHost(site), err)
	}
	if len(body) > siteInboxBodyMax {
		return nil, errors.New("that theme is too big to import (over 32 MB)")
	}
	return body, nil
}

// siteInboxDeliver POSTs §3.2's claim for ids and decodes the answer. It returns either
// the answer or the one sentence of why the claim failed — an error status, a transport
// failure and an unreadable body alike, because to a held file they all mean the same
// thing, which siteInboxDropHolds then carries out.
func (s *Server) siteInboxDeliver(ctx context.Context, site, bearer string, ids []string) (*siteInboxDeliverAnswer, string) {
	payload, err := json.Marshal(map[string]any{"ids": ids})
	if err != nil {
		return nil, "could not be built"
	}
	req, _, cancel, err := siteInboxRequest(ctx, http.MethodPost, site+"/api/hub/inbox/deliver", bearer, payload)
	if err != nil {
		return nil, "could not be built"
	}
	defer cancel()
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		if errors.Is(err, context.Canceled) || errors.Is(err, context.DeadlineExceeded) {
			return nil, "ended before the site answered"
		}
		return nil, fmt.Sprintf("could not reach %s: %v", siteHost(site), err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return nil, fmt.Errorf("%s answered HTTP %d at the claim door", siteHost(site), resp.StatusCode).Error()
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, int64(siteDesignsBodyMax)+1))
	if err != nil || len(body) > siteDesignsBodyMax {
		return nil, fmt.Errorf("%s sent a claim answer that could not be read", siteHost(site)).Error()
	}
	var ans siteInboxDeliverAnswer
	if json.Unmarshal(body, &ans) != nil {
		return nil, fmt.Errorf("%s answered the claim door with something unreadable", siteHost(site)).Error()
	}
	return &ans, ""
}

// --- the tray's doors ------------------------------------------------------------------

// routesSiteInbox hangs the tray's three doors, all behind the control token like every
// other actor door. The spec names two and this registers three: a tray row that can only
// be added to the library is a tray that cannot say no, and Dismiss is §8's own door.
func (s *Server) routesSiteInbox(mux *http.ServeMux) {
	mux.Handle("/api/site/inbox", s.requireControl(http.HandlerFunc(s.handleSiteInboxList)))
	mux.Handle("/api/site/inbox/add", s.requireControl(http.HandlerFunc(s.handleSiteInboxAdd)))
	mux.Handle("/api/site/inbox/dismiss", s.requireControl(http.HandlerFunc(s.handleSiteInboxDismiss)))
}

// handleSiteInboxList answers GET /api/site/inbox from the directory and nothing else —
// never the network, so the tray opens on a plane's wifi and a dead site cannot make the
// block spin. Rows come newest-held-first (the file's mtime is when the hold arrived), a
// file that will not read is skipped, and a missing directory is an empty list, not a
// mistake. The answer carries the site's base beside the rows so §8's "from <site>" wording
// can name the real site rather than a guess.
func (s *Server) handleSiteInboxList(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	s.siteInboxMu.Lock()
	entries := siteInboxList(s.siteInboxDir())
	s.siteInboxMu.Unlock()
	sort.SliceStable(entries, func(i, j int) bool { return entries[i].modTime.After(entries[j].modTime) })
	rows := []map[string]any{}
	for _, ent := range entries {
		rows = append(rows, map[string]any{
			"itemId": ent.env.ItemID, "designId": ent.env.DesignID, "name": ent.env.Name,
			"app": ent.env.App, "version": ent.env.Version, "sentAt": ent.env.SentAt,
			"claimed": ent.env.Claimed, "imported": ent.env.Imported,
		})
	}
	s.jsonOK(w, map[string]any{"items": rows, "site": siteReleaseBase()})
}

// handleSiteInboxAdd is the tray's Add: the held bytes go through the unchanged
// checkBundle and into the unchanged siteTicketStore, the envelope notes the import, and
// the answer is handleSiteImport's own success object — same keys, same order of
// construction, site = siteReleaseBase() — so the front-end hands off to the editor
// exactly as it does after a paste. Every refusal is the paste door's one sentence,
// unknown ids included: the tray cannot tell a lost file from a broken one and neither can
// the person clicking, and a 404 would describe the request wrongly.
func (s *Server) handleSiteInboxAdd(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	id, ok := siteInboxBodyID(w, r)
	if !ok {
		s.jsonErr(w, siteLinkRefused)
		return
	}
	s.siteInboxMu.Lock()
	defer s.siteInboxMu.Unlock()
	dir := s.siteInboxDir()
	env, err := siteInboxReadEnv(filepath.Join(dir, id+".json"), id)
	var b *siteBundle
	if err == nil {
		var raw []byte
		if raw, err = base64.StdEncoding.DecodeString(env.Raw); err == nil {
			b, err = checkBundle(raw, env.DesignID)
		}
	}
	if err != nil {
		s.jsonErr(w, siteLinkRefused)
		return
	}
	ticket, err := s.tickets().add(&siteTicket{
		site: siteReleaseBase(), id: env.DesignID, version: b.Version, app: b.App,
		name: b.Name, sharedName: b.SharedName, handle: b.Handle, raw: b.Raw,
	})
	if err != nil {
		s.jsonErr(w, "the import could not be held: "+err.Error())
		return
	}
	env.Imported = true
	if werr := s.siteInboxWriteEnv(dir, id, env); werr != nil {
		// The ticket is live and the tray has its answer; a hold that cannot note its own
		// import is a hold the next settle leaves standing, which is the safe direction of
		// wrong.
		log.Printf("orb-manager: site inbox: %s was added but the hold could not note it: %v", id, werr)
	}
	site := siteReleaseBase()
	log.Printf("site inbox add: %s %s v%d -> %q", site, env.DesignID, b.Version, b.SharedName)
	s.jsonOK(w, map[string]any{
		"ticket": ticket, "site": site, "id": env.DesignID, "version": b.Version,
		"app": b.App, "name": b.Name, "sharedName": b.SharedName, "handle": b.Handle, "schema": b.Schema,
	})
}

// handleSiteInboxDismiss deletes the hold and tells the site, whose claim answer — most
// honestly a "missing", since a dismissed item was never delivered to this link — is
// ignored by agreement: the hub is telling the site to stop offering this, and the site's
// "it is not waiting for you" is exactly that state, reached. A hold already gone is
// already dismissed, and the answer is still ok.
func (s *Server) handleSiteInboxDismiss(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	id, ok := siteInboxBodyID(w, r)
	if !ok {
		s.jsonErr(w, siteLinkRefused)
		return
	}
	s.siteInboxMu.Lock()
	err := os.Remove(filepath.Join(s.siteInboxDir(), id+".json"))
	s.siteInboxMu.Unlock()
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		s.jsonErr(w, "the hold could not be dismissed: "+err.Error())
		return
	}
	if err == nil {
		site := siteReleaseBase()
		s.siteInboxDeliver(r.Context(), site, s.siteBearerFor(site), []string{id})
	}
	s.jsonOK(w, map[string]any{})
}

// siteInboxBodyID reads the one field both tray POSTs carry. Every way it can fail —
// oversize body, unreadable JSON, missing or ungrammatical id — comes back not-ok, and it
// is the caller's sentence to answer with.
func siteInboxBodyID(w http.ResponseWriter, r *http.Request) (string, bool) {
	body, err := io.ReadAll(io.LimitReader(r.Body, int64(siteLinkMax)+1))
	if err != nil || len(body) > siteLinkMax {
		return "", false
	}
	var in struct {
		ItemID string `json:"itemId"`
	}
	id := ""
	if json.Unmarshal(body, &in) == nil {
		id = strings.TrimSpace(in.ItemID)
	}
	if !siteInboxIDRe.MatchString(id) {
		return "", false
	}
	return id, true
}
