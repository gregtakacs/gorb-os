package main

// l1-design.md §4.5 (L1 hub step 2): the library doors on the editor port. Step 1's
// OrbTheme.library asks hostUrl('/assets') and hostUrl('/asset?sha256=…'); under Orb
// Manager those are these two paths, and under a linked hub they are the site's bearer
// routes /api/hub/assets and /api/hub/asset seen through this hub. The doors imitate
// handleSiteDesigns line for line: the same bearer, the same headers, the same patience,
// the same no-redirect client, the same three sentences, the same rule that only a 401
// costs the hub its credential — because the library is the account's live list, and a
// hub that cached it would offer a pick the site has since refused.
//
// These two doors carry NO app scope, unlike the library and design routes beside them:
// ruling 42's library is the account's across every app, and design §6 question 1 chose
// to let every editor token read it (the CSP still keeps the bytes from leaving the
// page). That is also why they exist only on the editor port — the hub's own page reads
// its designs through /api/site/designs and has no use for asset rows — and why the
// control port answers 404 for them.

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"regexp"
)

// siteAssetsBodyMax bounds the site's answer to the library read: §2.4's rows are for a
// person to scroll through, and an answer larger than 4 MB is not that list, so the hub
// does not read the rest of it.
const siteAssetsBodyMax = 4 << 20

// siteAssetBytesMax is the cap on one asset's bytes: the site's own upload ceiling is
// 10 MB (ruling 35's and 41's files), so an answer larger than that plus one byte is
// either a broken site or a lie about a hash. One plus the cap is read, never more.
const siteAssetBytesMax = 10 << 20

// siteMsgNoAsset is the one sentence both the bad-hash 400 and the site's 404 wear: the
// caller asked for a file this hub cannot hand over, and which half of "cannot" is not
// the caller's business.
const siteMsgNoAsset = "no such asset"

// siteAssetHashRe is §2.3's sha256 as a query value: exactly 64 lower-case hex digits,
// which is also what keeps the value that goes into the site URL plain. An upper-case
// hash is not "the same hash" here — the site's rows are keyed lower-case, and a door
// that normalized would be inventing a second spelling.
var siteAssetHashRe = regexp.MustCompile(`^[0-9a-f]{64}$`)

// fetchSiteAssets asks §2.4's /api/hub/assets door for the account's library with the
// link's bearer and hands back the answer's bytes, three outcomes exactly as
// fetchSiteDesigns has them: the list; refused (and only refused — the caller words the
// owner's sentence and marks the link); or a failure whose sentence is already the
// answer, because a stall, a 500, a redirect, an oversized body or a body that is not
// the document is not a revocation and leaves the credential exactly where it was.
func (s *Server) fetchSiteAssets(ctx context.Context, site, token string) (raw json.RawMessage, refused bool, err error) {
	ctx, cancel := context.WithTimeout(ctx, siteCallTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, site+"/api/hub/assets", nil)
	if err != nil {
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "orb-manager/"+version)
	req.Header.Set("Cache-Control", "no-store")
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusUnauthorized {
		return nil, true, nil
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		// Includes the 3xx the no-redirect client hands back as an answer: a site pointing
		// the hub somewhere else is not answering this question.
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, int64(siteAssetsBodyMax)+1))
	if err != nil || len(b) > siteAssetsBodyMax {
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	b = bytes.TrimRight(b, " \t\r\n")
	if !siteAssetsObject(b) {
		return nil, false, errors.New(siteMsgNoAnswer)
	}
	return json.RawMessage(b), false, nil
}

// siteAssetsObject is §2.4's one shape: a JSON object whose assets field is an array.
// The rows inside are the site's to word and the hub's to pass on — the hub does not
// parse LibraryAsset structs it would only have to keep in step with a site free to add
// a field — exactly as siteDesignsObject lets a newer designs answer through untouched.
func siteAssetsObject(b []byte) bool {
	var obj map[string]json.RawMessage
	if json.Unmarshal(b, &obj) != nil {
		return false
	}
	rows, ok := obj["assets"]
	if !ok {
		return false
	}
	return json.Unmarshal(rows, new([]json.RawMessage)) == nil
}

// fetchSiteAsset asks §2.4's /api/hub/asset door for one file's bytes. Four outcomes, in
// the order the door reads them: refused (401 — the caller marks the link), notFound
// (404 — the account simply does not hold that hash, and the link lives), the bytes, or
// siteMsgNoAnswer for everything else, including a body over the cap and — the check
// that makes the bytes worth having — a body whose SHA-256 is not the hash asked for.
// Nothing is stored on disk: the bytes exist in this call or not at all.
func (s *Server) fetchSiteAsset(ctx context.Context, site, token, want string) (b []byte, refused, notFound bool, err error) {
	ctx, cancel := context.WithTimeout(ctx, siteCallTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, site+"/api/hub/asset?sha256="+want, nil)
	if err != nil {
		return nil, false, false, errors.New(siteMsgNoAnswer)
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Accept", "*/*")
	req.Header.Set("User-Agent", "orb-manager/"+version)
	req.Header.Set("Cache-Control", "no-store")
	resp, err := s.siteHTTP().Do(req)
	if err != nil {
		return nil, false, false, errors.New(siteMsgNoAnswer)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusUnauthorized {
		return nil, true, false, nil
	}
	if resp.StatusCode == http.StatusNotFound {
		return nil, false, true, nil
	}
	if resp.StatusCode < 200 || resp.StatusCode > 299 {
		return nil, false, false, errors.New(siteMsgNoAnswer)
	}
	b, err = io.ReadAll(io.LimitReader(resp.Body, int64(siteAssetBytesMax)+1))
	if err != nil || len(b) > siteAssetBytesMax {
		return nil, false, false, errors.New(siteMsgNoAnswer)
	}
	wantBytes, err := hex.DecodeString(want)
	whole := sha256.Sum256(b)
	if err != nil || !bytes.Equal(whole[:], wantBytes) {
		// The site handed bytes that are not the file it was asked for. That is the site
		// failing the one promise this door exists to keep, and it is the site's bad day,
		// not a revocation: the credential stays exactly where it was.
		return nil, false, false, errors.New(siteMsgNoAnswer)
	}
	return b, false, false, nil
}

// --- the doors ---------------------------------------------------------------------------

// handleSiteAssets answers GET /api/assets on the editor port: the account's library,
// passed through verbatim as data, with the link rules handleSiteDesigns keeps — a file
// already marked disconnected is believed without dialing, no credential says so, a 401
// rewrites the file and says the link ended, every other failure is the site's problem.
func (s *Server) handleSiteAssets(w http.ResponseWriter, r *http.Request) {
	if s.disconnectedSiteLink() != nil {
		s.jsonErr(w, siteMsgDisconnected)
		return
	}
	l := s.liveSiteLink()
	if l == nil {
		s.jsonErr(w, siteMsgNotConnected)
		return
	}
	raw, refused, err := s.fetchSiteAssets(r.Context(), l.Site, l.Token)
	switch {
	case refused:
		s.forgetSiteLink(l)
		s.jsonErr(w, siteMsgDisconnected)
	case err != nil:
		s.jsonErr(w, err.Error())
	default:
		s.jsonOKRaw(w, raw)
	}
}

// handleSiteAsset answers GET /api/asset?sha256=<hex> on the editor port: one file's
// bytes, checked against the hash on the way through and handed out as opaque bytes —
// the browser is told nothing about what they are, because the asset row already named
// the mediaType and the editor is the one choosing to hold them. A hash that is not a
// hash never reaches the network; the link rules are the list door's; the site's 404 is
// echoed as a 404 because for this question "the account does not hold that file" is a
// complete answer, not a hub failure.
func (s *Server) handleSiteAsset(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	vals, ok := q["sha256"]
	if len(q) != 1 || !ok || len(vals) != 1 || !siteAssetHashRe.MatchString(vals[0]) {
		s.jsonErr(w, siteMsgNoAsset)
		return
	}
	ask := vals[0]
	if s.disconnectedSiteLink() != nil {
		s.jsonErr(w, siteMsgDisconnected)
		return
	}
	l := s.liveSiteLink()
	if l == nil {
		s.jsonErr(w, siteMsgNotConnected)
		return
	}
	b, refused, notFound, err := s.fetchSiteAsset(r.Context(), l.Site, l.Token, ask)
	switch {
	case refused:
		s.forgetSiteLink(l)
		s.jsonErr(w, siteMsgDisconnected)
	case notFound:
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		_, _ = w.Write([]byte(`{"ok":false,"error":"no such asset"}`))
	case err != nil:
		s.jsonErr(w, err.Error())
	default:
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(b)
	}
}
