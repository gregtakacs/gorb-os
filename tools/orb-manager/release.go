package main

// orb-os-net §7.6 (Orb Manager row), §9 (R1's endpoint), rulings 25 and 26: the site
// release check.
//
// On start and every day after, the hub asks the site's public
// `GET /api/hub/releases/latest` — the endpoint R1 ships, which needs no sign-in and no
// link — whether a newer gorb-os release exists, compares that tag with its own build
// stamp, and caches the answer. The web reads the cache from `GET /api/release/latest`;
// the check is the only thing that ever reaches the network here, so an offline hub
// keeps serving the last answer it had and a page reload cannot turn into a hammering
// on the site's door.
//
// What the answer can cost the operator is one Download button (§7.6: "Updating itself
// in place may come later"). G3 swaps no binary and applies nothing: the verdict is a
// fact about the site, reported and then left alone.
//
// The install id (§7.2, ruling 25) rides none of this. It is minted and stored by
// state.go and is sent nowhere until H2 opens `/connect`.

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"runtime"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
)

// defaultSiteBase is the origin the release check dials with no GORB_SITE: ruling 1 puts
// the site on orb.takacsfamily.com today, and §11's move to orb-os.net is a future change
// to this one string.
const defaultSiteBase = "https://orb.takacsfamily.com"

// releaseCheckBootDelay is how long the loop waits before its first dial. The hub has
// better things to do in its first seconds — bind, discovery, the launch cable probe —
// and an operator who opens the page immediately should get the cached (empty) answer
// rather than a verdict that raced the hub's own startup.
const releaseCheckBootDelay = 20 * time.Second

// releaseCheckInterval is §7.6's "daily". A day is longer than any plausible patience
// for a release note and shorter than a week of stale answers.
const releaseCheckInterval = 24 * time.Hour

// releaseBodyMax bounds what a release answer may cost in memory. R1's document is one
// version, one timestamp, some notes and a handful of asset rows; megabytes of it are
// either a broken site or a hostile one, and neither is worth reading.
const releaseBodyMax = 64 << 10

// errSiteNoRelease is the 404 R1 answers when nothing is published. It is a complete and
// ordinary answer — the site has nothing to offer — and so it is named apart from the
// failures, which are the ones that cost the hub its last known answer.
var errSiteNoRelease = errors.New("the site has no published release")

// siteReleaseBase is the origin the release check asks. GORB_SITE is the whole origin
// (scheme+host+port), so both §11's move and a bench pointing at a local site are one
// string. A value that is not an https:// origin is ignored rather than obeyed: the one
// way this hub reaches a public host must not be one typo away from plaintext HTTP, and
// the single exception is a loopback http:// origin, which is what a test's httptest
// server is and cannot be dialled any other way.
func siteReleaseBase() string {
	raw := strings.TrimRight(strings.TrimSpace(os.Getenv("GORB_SITE")), "/")
	if raw == "" {
		return defaultSiteBase
	}
	u, err := url.Parse(raw)
	if err != nil || u.Host == "" || u.User != nil || (u.Path != "" && u.Path != "/") {
		return defaultSiteBase
	}
	switch u.Scheme {
	case "https":
		return raw
	case "http":
		h := u.Hostname()
		if ip := net.ParseIP(h); h == "localhost" || (ip != nil && ip.IsLoopback()) {
			return raw
		}
	}
	return defaultSiteBase
}

// releaseInfo is the cached verdict, and the body GET /api/release/latest serves. The
// zero value — never fetched, nothing offered — is exactly what a hub that has not yet
// reached the site reports.
type releaseInfo struct {
	CheckedAt   int64  `json:"checkedAt"`   // unix seconds of the last answer that arrived
	Current     string `json:"current"`     // this hub's own build stamp
	Latest      string `json:"latest"`      // the site's newest published tag
	Notes       string `json:"notes"`       // the site's own words about that release
	DownloadURL string `json:"downloadUrl"` // this platform's asset, or the site's /download
	Verdict     string `json:"verdict"`     // firmware.VerdictUpgrade, or "" for nothing offered
	Fetched     bool   `json:"fetched"`     // false before the first answer and after one that did not arrive
}

// releaseClient is the check's own dialer: a public endpoint answering in milliseconds
// wants no minute of patience, and a redirect is an answer we did not ask the site for —
// the same discipline siteimport.go fetches a bundle with, bounded and unfollowed.
var releaseClient = &http.Client{
	Timeout: 15 * time.Second,
	CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	},
}

// releaseUserAgent is the caller's word for itself. It reads the package's build stamp
// out here because fetchLatestRelease names its own first result `version`, and a header
// written inside that function would silently carry the site's version instead.
func releaseUserAgent() string { return "orb-manager/" + version }

// siteRelease is R1's document, decoded only as far as this check reads it: the tag, the
// notes, and the asset list this platform's download link is picked out of. The fields R1
// sends that this does not name (publishedAt, sha256, size, filename) are left unread
// rather than asserted about.
type siteRelease struct {
	Version string `json:"version"`
	Notes   string `json:"notes"`
	Assets  []struct {
		Platform string `json:"platform"`
		Filename string `json:"filename"`
		URL      string `json:"url"`
	} `json:"assets"`
}

// fetchLatestRelease asks base for the site's newest published release. R1's 404
// ({"error":"no_release"}) comes back as errSiteNoRelease, which means "nothing to
// offer" and not "something went wrong": a site with nothing published is answering the
// question properly. Any other failure is returned as one.
func fetchLatestRelease(ctx context.Context, base string) (version, notes, downloadURL string, err error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, base+"/api/hub/releases/latest", nil)
	if err != nil {
		return "", "", "", fmt.Errorf("could not ask the site for its newest release: %v", err)
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", releaseUserAgent())
	resp, err := releaseClient.Do(req)
	if err != nil {
		return "", "", "", fmt.Errorf("could not reach the site: %v", err)
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(io.LimitReader(resp.Body, int64(releaseBodyMax)+1))
	if err != nil {
		return "", "", "", fmt.Errorf("the site's release answer did not arrive: %v", err)
	}
	if len(b) > releaseBodyMax {
		return "", "", "", errors.New("the site's release answer was too large to be one")
	}
	if resp.StatusCode == http.StatusNotFound {
		var e struct {
			Error string `json:"error"`
		}
		if json.Unmarshal(b, &e) == nil && e.Error == "no_release" {
			return "", "", "", errSiteNoRelease
		}
		return "", "", "", fmt.Errorf("the site answered HTTP %d", resp.StatusCode)
	}
	if resp.StatusCode >= 300 && resp.StatusCode < 400 {
		return "", "", "", errors.New("the site redirected the release question instead of answering it")
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return "", "", "", fmt.Errorf("the site answered HTTP %d", resp.StatusCode)
	}
	var doc siteRelease
	if err := json.Unmarshal(b, &doc); err != nil {
		return "", "", "", fmt.Errorf("the site's release answer was not the shape it documents: %v", err)
	}
	return doc.Version, doc.Notes, releaseDownloadURL(base, doc), nil
}

// releaseDownloadURL is the asset this machine would download, or the site's own download
// page when R1 published nothing for this platform. An absolute url is taken as given; a
// relative one is resolved against the site's origin.
func releaseDownloadURL(base string, doc siteRelease) string {
	want := releasePlatform()
	for _, a := range doc.Assets {
		if a.Platform != want || a.URL == "" {
			continue
		}
		if strings.HasPrefix(a.URL, "http://") || strings.HasPrefix(a.URL, "https://") {
			return a.URL
		}
		return base + "/" + strings.TrimPrefix(a.URL, "/")
	}
	return base + "/download"
}

// releasePlatform names this machine the way R1 names its assets: Go says darwin, the
// site says macos. Anything else is Go's own word, and an asset list with no such
// platform falls through to the site's download page.
func releasePlatform() string {
	if runtime.GOOS == "darwin" {
		return "macos"
	}
	return runtime.GOOS
}

// runReleaseCheck asks the site once and files the answer. The verdict is stored only
// when the site's tag is newer than this build's: firmware.Compare already decides that
// ordering for firmware dots and fails closed on anything unparsable — a `go build` stamp
// of "dev", a git describe — which is precisely why an untagged hub never claims an
// update.
//
// A check that could not reach the site costs the cache its freshness and its claim, and
// nothing else: the last latest, notes and link stand, so the card can still say what the
// hub knew, under fetched:false.
func (s *Server) runReleaseCheck(ctx context.Context) error {
	base := siteReleaseBase()
	siteVersion, notes, downloadURL, err := fetchLatestRelease(ctx, base)
	s.relMu.Lock()
	defer s.relMu.Unlock()
	s.rel.Current = version
	if err != nil {
		if !errors.Is(err, errSiteNoRelease) {
			s.rel.Fetched = false
			s.rel.Verdict = ""
			return err
		}
		// Nothing published: a fresh, complete answer that offers nothing.
		s.rel.Latest, s.rel.Notes, s.rel.DownloadURL = "", "", ""
		s.rel.Verdict = ""
		s.rel.Fetched = true
		s.rel.CheckedAt = time.Now().Unix()
		return nil
	}
	s.rel.Latest = siteVersion
	s.rel.Notes = notes
	s.rel.DownloadURL = downloadURL
	s.rel.Verdict = ""
	if firmware.Compare(firmware.Release{Version: version}, firmware.Release{Version: siteVersion}) == firmware.VerdictUpgrade {
		s.rel.Verdict = string(firmware.VerdictUpgrade)
	}
	s.rel.Fetched = true
	s.rel.CheckedAt = time.Now().Unix()
	return nil
}

// releaseCheckLoop is the daily ask, modelled on discover.go's upkeepLoop: its own
// context, one failure logged, the next tick retried. It is its own loop rather than a
// fold into upkeepLoop because upkeepLoop "sends nothing" and this does.
func (s *Server) releaseCheckLoop(ctx context.Context) {
	select {
	case <-ctx.Done():
		return
	case <-time.After(releaseCheckBootDelay):
	}
	t := time.NewTicker(releaseCheckInterval)
	defer t.Stop()
	for {
		if err := s.runReleaseCheck(ctx); err != nil {
			log.Printf("orb-manager: release check: %v (kept the last answer, asking again tomorrow)", err)
		}
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
	}
}

// serveReleaseLatest answers GET /api/release/latest with the cache, envelope and all. It
// dials nothing: the answer is what the last check learned, which is the point of caching
// it. The Updates list (g3-2) renders one row from it, and its only button is Download.
func (s *Server) serveReleaseLatest(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	s.relMu.Lock()
	info := s.rel
	s.relMu.Unlock()
	s.jsonOK(w, info)
}
