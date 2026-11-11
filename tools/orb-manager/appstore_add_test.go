package main

// A1 §7.2–§7.3 (docs/specs/a1-design.md): POST /api/site/apps/add, the one way an orb-os.net
// app reaches this hub's shelf, and the `.orbapp.origin.json` beside a kept app that records
// where those bytes came from. The store is a fake site on loopback (GORB_SITE), the app it
// serves is the repo's own sentinel fixture, and every case below is told by what the shelf
// and the sidecar hold afterwards rather than by what the door printed.

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// addShelf is libHub with the apps directory made, so the writable rung of the library
// resolves inside the test's own tree instead of falling back to a config directory.
func addShelf(t *testing.T) (*Server, string) {
	t.Helper()
	s, _, root := libHub(t)
	if err := os.MkdirAll(s.lib.AppsDir, 0o755); err != nil {
		t.Fatal(err)
	}
	return s, root
}

// addFixture is a file the fake store serves: the sentinel container stamps itself with the
// widest legal release the grammar admits (v1.4.2-rc.1+build.7), so it exercises §7.2 check 5's
// head comparison at the same time it passes it, and `plain` stamps nothing and names itself
// something else, which is what the wrong-file cases need.
func addFixture(t *testing.T, name string) (body []byte, slug, version string) {
	t.Helper()
	path := cloneFixture(t, t.TempDir(), name)
	body, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	c, err := orbnet.ParseApp(body)
	if err != nil {
		t.Fatalf("the sentinel fixture does not parse: %v", err)
	}
	return body, c.Slug, c.Version
}

// addCatalogue is one store entry as §4.1 serves it, wrapped so a case can overwrite a field
// (the hash, the version) rather than hand-write JSON every time.
func addCatalogue(t *testing.T, e siteCatalogue) string {
	t.Helper()
	body, err := json.Marshal(map[string]any{"apps": []siteCatalogue{e}})
	if err != nil {
		t.Fatal(err)
	}
	return string(body)
}

// addStore is the fake site's two doors: the catalogue the hub reads first, and the download
// it dials after. Either can be told to answer badly, which is how each refusal gets tested
// without a second hub.
type addStore struct {
	catalogue string
	download  func(w http.ResponseWriter, r *http.Request)
}

func (st *addStore) serve(w http.ResponseWriter, r *http.Request) {
	switch {
	case r.URL.Path == "/api/hub/apps":
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(st.catalogue))
	case strings.HasPrefix(r.URL.Path, "/api/hub/app/"):
		st.download(w, r)
	default:
		http.NotFound(w, r)
	}
}

// addPress is one owner's press: POST /api/site/apps/add naming a slug and a version.
func addPress(t *testing.T, s *Server, slug, version string) map[string]any {
	t.Helper()
	return designCall(t, s, http.MethodPost, "/api/site/apps/add",
		[]byte(`{"slug":"`+slug+`","version":"`+version+`"}`))
}

// addHeld is what the shelf says after any press: the bytes it holds under that slug (nil for
// nothing there) and the origin record beside them (nil for no readable record).
func addHeld(t *testing.T, root, slug string) ([]byte, *AppOrigin) {
	t.Helper()
	dest := filepath.Join(root, "apps", slug+".orbapp")
	body, err := os.ReadFile(dest)
	if err != nil {
		if !os.IsNotExist(err) {
			t.Fatalf("the shelf would not say what it holds under %s: %v", slug, err)
		}
		if _, err := os.Stat(appOriginPath(dest)); err == nil {
			t.Fatalf("the shelf holds no %s.orbapp and a sidecar beside it anyway", slug)
		}
		return nil, nil
	}
	return body, readAppOrigin(dest)
}

// addOffer is the catalogue entry for a store that hands over these exact bytes.
func addOffer(slug, version string, body []byte) siteCatalogue {
	return siteCatalogue{Slug: slug, Title: "Sentinel", Developer: "@gr3g", Version: version,
		Stamp: version, Caps: []string{"net", "themes"}, Sha256: sha256Hex(body),
		Size: int64(len(body)), HasEditor: true}
}

// addSend serves a body with the header the real door sends, unless a case says otherwise.
func addSend(body []byte, claim *string, status int) func(http.ResponseWriter, *http.Request) {
	return func(w http.ResponseWriter, _ *http.Request) {
		if status != http.StatusOK {
			w.WriteHeader(status)
			return
		}
		if claim != nil {
			if *claim != "" {
				w.Header().Set("X-Orb-Sha256", *claim)
			}
		} else {
			w.Header().Set("X-Orb-Sha256", sha256Hex(body))
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		_, _ = w.Write(body)
	}
}

// The main line, both halves of it: a press adds the app the store named, files it where the
// ordinary import files it, and records where it came from beside it — and a second press over
// the hub's own site-sourced copy is allowed and re-stamps the record.
func TestSiteAppsAddWritesSidecar(t *testing.T) {
	body, slug, version := addFixture(t, "sentinel")
	s, root := addShelf(t)
	store := &addStore{catalogue: addCatalogue(t, addOffer(slug, version, body)),
		download: addSend(body, nil, http.StatusOK)}
	srv, asks := siteServer(t, store.serve)
	t.Setenv("GORB_SITE", srv.URL)

	env := addPress(t, s, slug, version)
	if env["ok"] != true {
		t.Fatalf("the add press was refused: %v", env)
	}
	data, _ := env["data"].(map[string]any)
	if data["slug"] != slug || data["version"] != version || data["overwrote"] != false {
		t.Errorf("the door answered %v, want this app, this release and overwrote=false", data)
	}
	dest, _ := data["kept"].(string)
	if dest != filepath.Join(root, "apps", slug+".orbapp") {
		t.Errorf("kept = %q, want it inside the hub's own writable apps directory", dest)
	}
	held, origin := addHeld(t, root, slug)
	if string(held) != string(body) {
		t.Errorf("the shelf holds %d bytes and the store sent %d: the file was rewritten",
			len(held), len(body))
	}
	if origin == nil {
		t.Fatal("the app landed with no origin record beside it")
	}
	if origin.Site != srv.URL || origin.Slug != slug || origin.Version != version {
		t.Errorf("the record names %s / %s %s, want this site and this app at this release",
			origin.Site, origin.Slug, origin.Version)
	}
	if origin.SHA256 != sha256Hex(body) || origin.Stamp != version {
		t.Errorf("the record carries sha256 %q and stamp %q, want the hash the bytes were checked against and the store's own stamp",
			origin.SHA256, origin.Stamp)
	}
	if origin.ImportedAt <= 0 {
		t.Errorf("the record was stamped at %d, want a real time", origin.ImportedAt)
	}
	// The record is a file beside the app, in the library's own JSON shape, and the two dialled
	// requests are the whole conversation: the store read and this one file.
	raw, err := os.ReadFile(appOriginPath(dest))
	if err != nil {
		t.Fatalf("no sidecar at %s: %v", appOriginPath(dest), err)
	}
	if !strings.HasSuffix(string(raw), "}\n") || !strings.Contains(string(raw), `"sha256": "`) {
		t.Errorf("the sidecar is not the library's indented JSON with its hash in it: %s", raw)
	}
	if len(*asks) != 2 || (*asks)[0].path != "/api/hub/apps" ||
		(*asks)[1].path != "/api/hub/app/"+slug+"/"+version {
		t.Errorf("the hub dialed %v, want one catalogue read and one download", *asks)
	}

	// The face reads the record off the listing, and the sidecar itself is no row and no
	// refusal — the apps listing admits only .orbapp files, as the themes listing admits only
	// .orbtheme.
	list := designCall(t, s, http.MethodGet, "/api/library", nil)["data"].(map[string]any)
	rows, _ := list["apps"].([]any)
	if len(rows) != 1 {
		t.Fatalf("the listing shows %d apps, want the one this press added", len(rows))
	}
	row, _ := rows[0].(map[string]any)
	rec, _ := row["origin"].(map[string]any)
	if rec == nil || rec["site"] != srv.URL || rec["version"] != version {
		t.Errorf("the app row's origin is %v, want this site at this release", row["origin"])
	}
	if files, _ := list["refused"].([]any); len(files) != 0 {
		t.Errorf("the listing refused %v; a sidecar is no row and no refusal", files)
	}

	// K9's rule rides this door too: a kept copy retires every remembered editor region, so the
	// hub cannot go on serving the editor of the bytes that were just replaced. One is stood up
	// by hand here, because the only proof the call happened is that it is gone afterwards.
	regions.mu.Lock()
	regions.slots[editorKey{ip: "127.0.0.1:9", slug: slug}] = &editorSlot{lib: "an old stamp"}
	regions.mu.Unlock()
	t.Cleanup(editorInvalidate)

	// A second press over the hub's own site-sourced copy is the Update case and it is allowed:
	// the bytes are the same, and the record is re-stamped rather than left standing.
	second := addPress(t, s, slug, version)
	if second["ok"] != true {
		t.Fatalf("re-adding the hub's own site copy was refused: %v", second)
	}
	if d := second["data"].(map[string]any); d["overwrote"] != true {
		t.Errorf("over the hub's own copy overwrote = %v, want true", d["overwrote"])
	}
	if again := readAppOrigin(dest); again == nil || again.SHA256 != origin.SHA256 {
		t.Errorf("the second press left the record as %v, want it re-filed the same way", again)
	}
	regions.mu.Lock()
	left := len(regions.slots)
	regions.mu.Unlock()
	if left != 0 {
		t.Errorf("the add press left %d remembered editor regions standing; a kept copy retires them", left)
	}
}

// Every refusal §7.2 lists, told by the sentence it says and by the shelf it did not touch.
// The order matters as much as the answer: the size case carries a wrong hash too, and the
// refusal must be the size one, because a hub that hashed first would have read the whole
// file it was refusing.
func TestSiteAppsAddRefuses(t *testing.T) {
	body, slug, version := addFixture(t, "sentinel")
	sum := sha256Hex(body)
	claim := "b" + sum[1:]
	missing := ""
	tooBig := make([]byte, appUploadMax+1)
	other, otherSlug, _ := addFixture(t, "plain")
	if otherSlug == slug {
		t.Fatalf("the two fixtures share the slug %q; the wrong-app case needs two", otherSlug)
	}
	junk := []byte("this is not an orbapp at all")
	for _, c := range []struct {
		name      string
		catalogue string
		download  func(http.ResponseWriter, *http.Request)
		said      string
	}{
		{"not on the store", addCatalogue(t, addOffer("weather", "v1.0.0", body)),
			addSend(body, nil, http.StatusOK), "does not offer"},
		{"not this release", addCatalogue(t, addOffer(slug, "v9.9.9", body)),
			addSend(body, nil, http.StatusOK), "does not offer"},
		{"withdrawn", `{"apps":[{"slug":"sentinel","title":"Sentinel","developer":"@gr3g",` +
			`"version":null,"stamp":null,"caps":null,"osMin":null,"sha256":null,"size":null,` +
			`"hasEditor":null,"readers":{},"withdrawn":[{"version":"v1.2.0","reason":"crash on wake"}]}]}`,
			addSend(body, nil, http.StatusOK), "does not offer"},
		{"gone from the door", addCatalogue(t, addOffer(slug, version, body)),
			addSend(body, nil, http.StatusNotFound), "does not offer"},
		{"throttled", addCatalogue(t, addOffer(slug, version, body)),
			addSend(body, nil, http.StatusTooManyRequests), "asking us to wait a minute"},
		{"site broken", addCatalogue(t, addOffer(slug, version, body)),
			addSend(body, nil, http.StatusInternalServerError), "could not be reached"},
		{"site redirecting", addCatalogue(t, addOffer(slug, version, body)),
			func(w http.ResponseWriter, r *http.Request) {
				http.Redirect(w, r, "http://"+r.Host+"/elsewhere", http.StatusFound)
			}, "could not be reached"},
		{"no hash claimed", addCatalogue(t, addOffer(slug, version, body)),
			addSend(body, &missing, http.StatusOK), "with no sha256 to check the bytes against"},
		{"too big", addCatalogue(t, addOffer(slug, version, tooBig)),
			addSend(tooBig, nil, http.StatusOK), "bigger than the 8 MB the hub's own import takes"},
		{"hash not the store's", addCatalogue(t, siteCatalogue{Slug: slug, Title: "Sentinel",
			Developer: "@gr3g", Version: version, Stamp: version, Sha256: strings.Repeat("a", 64),
			Size: int64(len(body))}), addSend(body, nil, http.StatusOK), "hash to " + sum},
		{"header disagrees", addCatalogue(t, addOffer(slug, version, body)),
			addSend(body, &claim, http.StatusOK), "and the store named " + claim},
		{"another app's file", addCatalogue(t, addOffer(slug, version, other)),
			addSend(other, nil, http.StatusOK), "is not that app"},
		{"not a container", addCatalogue(t, addOffer(slug, version, junk)),
			addSend(junk, nil, http.StatusOK), "is not that app"},
	} {
		t.Run(c.name, func(t *testing.T) {
			s, root := addShelf(t)
			store := &addStore{catalogue: c.catalogue, download: c.download}
			srv, _ := siteServer(t, store.serve)
			t.Setenv("GORB_SITE", srv.URL)
			env := addPress(t, s, slug, version)
			if env["ok"] != false {
				t.Fatalf("the door accepted it: %v", env)
			}
			if msg, _ := env["error"].(string); !strings.Contains(msg, c.said) {
				t.Errorf("refused with %q, want the sentence containing %q", msg, c.said)
			}
			if held, origin := addHeld(t, root, slug); held != nil || origin != nil {
				t.Errorf("the refusal still changed the shelf: %d bytes, record %+v", len(held), origin)
			}
			if code := statusOfCall(t, s, http.MethodPost, "/api/site/apps/add",
				[]byte(`{"slug":"`+slug+`","version":"`+version+`"}`)).Code; code != http.StatusBadRequest {
				t.Errorf("a refusal answered HTTP %d, want 400 with the sentence in the envelope", code)
			}
		})
	}

	// The refusals a bad press earns before the hub has any reason to call anybody: no press,
	// the wrong shape, a slug with a space in it, a version that is not a release, and a body
	// too long to be a press. Each is the same sentence, and the site is never dialed.
	for _, press := range []string{
		``, `{}`, `5`, `"sentinel"`, `{"slug":"","version":"` + version + `"}`,
		`{"slug":"not a slug","version":"` + version + `"}`,
		`{"slug":"` + slug + `","version":"1.4.2"}`,
		`{"slug":"` + slug + `","version":"v1.4.2..1"}`,
		strings.Repeat("x", siteAddBodyMax+1),
	} {
		s, root := addShelf(t)
		srv, asks := siteServer(t, func(w http.ResponseWriter, r *http.Request) {
			t.Errorf("a press that named no app dialed the site at %s", r.URL.Path)
		})
		t.Setenv("GORB_SITE", srv.URL)
		rec := rawCall(t, s, http.MethodPost, "/api/site/apps/add", []byte(press))
		if rec.Code != http.StatusBadRequest {
			t.Errorf("press %q answered %d, want 400", truncForMsg(press), rec.Code)
			continue
		}
		var env map[string]any
		if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
			t.Fatalf("press %q answered %s", truncForMsg(press), rec.Body)
		}
		if env["ok"] != false {
			t.Errorf("press %q said %v, want the refusal envelope", truncForMsg(press), env)
		}
		if msg, _ := env["error"].(string); !strings.Contains(msg, "add needs a slug and a version") {
			t.Errorf("press %q refused with %q, want the press sentence", truncForMsg(press), msg)
		}
		if held, origin := addHeld(t, root, slug); held != nil || origin != nil {
			t.Errorf("press %q still touched the shelf", truncForMsg(press))
		}
		if len(*asks) != 0 {
			t.Errorf("press %q dialed the site %d times", truncForMsg(press), len(*asks))
		}
	}

	// The door is a POST, and says it the way every other site door says it.
	shut, _ := addShelf(t)
	if rec := rawCall(t, shut, http.MethodGet, "/api/site/apps/add", nil); rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("GET /api/site/apps/add = %d, want 405", rec.Code)
	}
}

// truncForMsg keeps a refusal test's failure line short when the press was 4 KB of x.
func truncForMsg(press string) string {
	if len(press) > 24 {
		return press[:24] + "…"
	}
	return press
}

// check 6, the one that protects a copy the store has no claim on. A shelf copy with no record
// came off a disk in somebody's hand or out of the executable, and an app the hub added from a
// different site is somebody else's provenance: neither is this store's to replace, and the
// hub's own bytes must still be there afterwards.
func TestSiteAppsAddRefusesOverForeignCopy(t *testing.T) {
	body, slug, version := addFixture(t, "sentinel")
	for _, c := range []struct {
		name   string
		site   string // "" for no record at all; "junk" for one that will not read
		ok     bool
		rename string // a record whose own slug field lies
		record bool   // a readable record stood beside the file before the press
	}{
		{"hand import, no record", "", false, "", false},
		{"record from another site", "http://elsewhere.invalid", false, "", true},
		{"record that will not read", "junk", false, "", false},
		{"record naming another app", "", false, "clock", true},
		{"record from this site", "MINE", true, "", false},
	} {
		t.Run(c.name, func(t *testing.T) {
			s, root := addShelf(t)
			dest := filepath.Join(root, "apps", slug+".orbapp")
			if err := os.WriteFile(dest, body, 0o644); err != nil {
				t.Fatal(err)
			}
			switch c.site {
			case "", "MINE":
				if c.rename != "" {
					if err := writeAppOrigin(dest, &AppOrigin{Site: "http://elsewhere.invalid",
						Slug: c.rename, Version: version, SHA256: sha256Hex(body), ImportedAt: 1}); err != nil {
						t.Fatal(err)
					}
				}
			case "junk":
				if err := os.WriteFile(appOriginPath(dest), []byte(`{"site":`), 0o644); err != nil {
					t.Fatal(err)
				}
			default:
				if err := writeAppOrigin(dest, &AppOrigin{Site: c.site, Slug: slug,
					Version: version, SHA256: sha256Hex(body), ImportedAt: 1}); err != nil {
					t.Fatal(err)
				}
			}
			store := &addStore{catalogue: addCatalogue(t, addOffer(slug, version, body)),
				download: addSend(body, nil, http.StatusOK)}
			srv, _ := siteServer(t, store.serve)
			t.Setenv("GORB_SITE", srv.URL)
			if c.site == "MINE" {
				if err := writeAppOrigin(dest, &AppOrigin{Site: srv.URL, Slug: slug, Version: version,
					Stamp: version, SHA256: sha256Hex(body), ImportedAt: 1}); err != nil {
					t.Fatal(err)
				}
			}
			env := addPress(t, s, slug, version)
			held, origin := addHeld(t, root, slug)
			if string(held) != string(body) {
				t.Fatal("the press rewrote shelf bytes it should have left alone")
			}
			if c.ok {
				if env["ok"] != true {
					t.Fatalf("over the hub's own site copy the press was refused: %v", env)
				}
				if d := env["data"].(map[string]any); d["overwrote"] != true {
					t.Errorf("over the hub's own copy overwrote = %v, want true", d["overwrote"])
				}
				if origin == nil || origin.Site != srv.URL {
					t.Errorf("the record is %+v, want this site's", origin)
				}
				return
			}
			if env["ok"] != false {
				t.Fatalf("the door accepted an overwrite of %s: %v", c.name, env)
			}
			msg, _ := env["error"].(string)
			if !strings.Contains(msg, "replace it with") || env["held"] != true {
				t.Errorf("refused with %q (held=%v), want the question that asks to replace it", msg, env["held"])
			}
			if c.record && readAppOrigin(dest) == nil {
				t.Error("the refusal threw away the record that was beside the file it refused to touch")
			}
			// The owner confirms: the same press with replace swaps the copy for the store's and
			// writes this site's record beside it.
			env = designCall(t, s, http.MethodPost, "/api/site/apps/add",
				[]byte(`{"slug":"`+slug+`","version":"`+version+`","replace":true}`))
			if env["ok"] != true {
				t.Fatalf("the confirmed replace was refused: %v", env)
			}
			if _, o := addHeld(t, root, slug); o == nil || o.Site != srv.URL {
				t.Errorf("after the confirmed replace the record is %+v, want this site's", o)
			}
		})
	}
}

// A sidecar is a claim about bytes the site sent, so every door that writes a `.orbapp` over
// which it has no such claim takes the claim down. All three writers are here: the hand import,
// the bundled restore and the bundled seed. The record's absence is the honest face of "the hub
// cannot say where this came from", which is also the face of no chip and no update offer.
func TestHandImportClearsAppSidecar(t *testing.T) {
	body, slug, version := addFixture(t, "sentinel")

	// The hand import door, keeping a copy the store had added.
	s, root := addShelf(t)
	dest := filepath.Join(root, "apps", slug+".orbapp")
	if err := os.WriteFile(dest, []byte("the store's copy"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := writeAppOrigin(dest, &AppOrigin{Site: "http://elsewhere.invalid", Slug: slug,
		Version: version, SHA256: sha256Hex(body), ImportedAt: 1}); err != nil {
		t.Fatal(err)
	}
	env := designCall(t, s, http.MethodPost, "/api/apps/import?keep=1&replace=1", body)
	if env["ok"] != true {
		t.Fatalf("the import door refused its own container: %v", env)
	}
	if data, _ := env["data"].(map[string]any); data["kept"] == "" || data["kept"] == nil {
		t.Fatalf("the import said it kept nothing: %v", env)
	}
	held, origin := addHeld(t, root, slug)
	if string(held) != string(body) {
		t.Error("the import did not file the bytes it was handed")
	}
	if origin != nil {
		t.Errorf("the hand import left a site record standing: %+v", origin)
	}

	// The restore, which refiles a bundled app over whatever was there.
	shipped := shippedClock(t)
	user := t.TempDir()
	src := shelf(shipped, []byte("skeleton"))
	if lines := seedAll(t, src, user); len(lines) == 0 {
		t.Fatalf("the seed wrote nothing to %s: %v", user, lines)
	}
	lib := &Library{AppsDir: filepath.Join(user, "apps")}
	restored := filepath.Join(user, "apps", "clock.orbapp")
	if err := writeAppOrigin(restored, &AppOrigin{Site: "http://elsewhere.invalid", Slug: "clock",
		Version: "v1.0.0", SHA256: sha256Hex(shipped), ImportedAt: 1}); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(restored); err != nil {
		t.Fatal(err)
	}
	if got, err := restoreShippedApps(src, lib, user); err != nil || len(got) != 1 {
		t.Fatalf("restore = %v, %v; want [clock]", got, err)
	}
	if o := readAppOrigin(restored); o != nil {
		t.Errorf("the restore refiled a bundled app with a site record beside it: %+v", o)
	}

	// The seed, which is the third writer and the one a launch runs: it files shipped apps into a
	// library that has none of them, and a record standing beside such a file is a lie.
	fresh := filepath.Join(t.TempDir(), "lib")
	if err := os.MkdirAll(filepath.Join(fresh, "apps"), 0o755); err != nil {
		t.Fatal(err)
	}
	seeded := filepath.Join(fresh, "apps", "clock.orbapp")
	if err := writeAppOrigin(seeded, &AppOrigin{Site: "http://elsewhere.invalid", Slug: "clock",
		Version: "v1.0.0", SHA256: sha256Hex(shipped), ImportedAt: 1}); err != nil {
		t.Fatal(err)
	}
	if lines := seedAll(t, src, fresh); len(lines) == 0 {
		t.Fatalf("the seed wrote nothing to a fresh library: %v", lines)
	}
	if o := readAppOrigin(seeded); o != nil {
		t.Errorf("the seed filed a bundled app with a site record beside it: %+v", o)
	}
}

// Drop's app branch is rm's, and it takes the record with the file — including for a copy that
// never had one, since a sidecar that was never there is not a deletion failure.
func TestLibraryRmTakesTheAppSidecar(t *testing.T) {
	body, slug, version := addFixture(t, "sentinel")
	s, root := addShelf(t)
	dest := filepath.Join(root, "apps", slug+".orbapp")
	if err := os.WriteFile(dest, body, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := writeAppOrigin(dest, &AppOrigin{Site: "http://elsewhere.invalid", Slug: slug,
		Version: version, SHA256: sha256Hex(body), ImportedAt: 1}); err != nil {
		t.Fatal(err)
	}
	if env := designCall(t, s, http.MethodPost, "/api/library/rm?kind=app&name="+slug, nil); env["ok"] != true {
		t.Fatalf("rm refused: %v", env)
	}
	if _, err := os.Stat(dest); !os.IsNotExist(err) {
		t.Errorf("the app is still on the shelf: %v", err)
	}
	if _, err := os.Stat(appOriginPath(dest)); !os.IsNotExist(err) {
		t.Error("the app went and its origin record stayed")
	}
	// A copy with no record is the ordinary case and rm still says ok.
	plainPath := filepath.Join(root, "apps", "plain.orbapp")
	plainBody, plainSlug, _ := addFixture(t, "plain")
	if err := os.WriteFile(plainPath, plainBody, 0o644); err != nil {
		t.Fatal(err)
	}
	if env := designCall(t, s, http.MethodPost, "/api/library/rm?kind=app&name="+plainSlug, nil); env["ok"] != true {
		t.Errorf("rm of an app with no record said %v, want the deletion to succeed", env)
	}
}
