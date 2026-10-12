// Command orbvendor makes the Theme Editor work with no internet.
//
// The composer's only external dependencies are three files support.js pulls from
// unpkg.com (React, ReactDOM, Babel) and eleven Google Fonts stylesheets. The hub can
// only be used offline if those are served locally, and support.js is vendor bytes
// this repo keeps byte-identical — it cannot be edited to point somewhere else. What
// it does expose is the override hooks it was built with: window.__resources maps a
// CDN URL to a local src, and that is the seam this tool fills.
//
// It writes web/composer/vendor/: the three scripts, one fonts.css, the woff2 files
// it references, and a MANIFEST.json recording every URL, its sha256, its size and
// the SRI the composer expects. Re-running is a no-op when the bytes already match,
// so drift is visible in git rather than silent.
//
//	orbvendor -out web/composer/vendor             # download what is missing
//	orbvendor -out web/composer/vendor -verify     # check the tree, fetch nothing
package main

import (
	"crypto/sha256"
	"crypto/sha512"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// fetched is one downloaded font file, keyed by the URL it came from.
type fetched struct {
	url, local string
	data       []byte
}

func fetch(client *http.Client, url string) ([]byte, error) {
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", chromeUA)
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	// 32 MB is far past the largest file here (Babel at 3 MB) and short of letting a
	// redirect to a huge object exhaust memory.
	b, err := io.ReadAll(io.LimitReader(resp.Body, 32<<20))
	if err != nil {
		return nil, err
	}
	if len(b) == 0 {
		return nil, fmt.Errorf("%s returned an empty body", url)
	}
	return b, nil
}

func sum(b []byte) string { s := sha256.Sum256(b); return hex.EncodeToString(s[:]) }

// sri384 is the SRI form the composer pins: "sha384-" plus base64 of the digest.
func sri384(b []byte) string {
	s := sha512.Sum384(b)
	return "sha384-" + base64.StdEncoding.EncodeToString(s[:])
}

// checkSRIsMatchSupport proves the URLs and digests this tool vendors are the same
// ones support.js will demand, by reading them out of that file rather than trusting
// the copy above. If support.js is re-vendored to a new React and this list is not
// updated, vendoring fails here instead of shipping a tree that cannot load.
func checkSRIsMatchSupport(path string) error {
	src, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("read %s: %w", path, err)
	}
	text := string(src)
	for _, s := range cdnScripts {
		u := regexp.QuoteMeta(s.url)
		m := regexp.MustCompile(`var\s+\w+URL\s*=\s*"` + u + `";\s*var\s+\w+SRI\s*=\s*"([^"]+)"`).FindStringSubmatch(text)
		if m == nil {
			if !regexp.MustCompile(`"` + u + `"`).MatchString(text) {
				return fmt.Errorf("support.js no longer references %s; this vendor list is stale", s.url)
			}
			return fmt.Errorf("support.js references %s but no pinned SRI follows it", s.url)
		}
		if m[1] != s.sri {
			return fmt.Errorf("SRI for %s disagrees with support.js\n  here:       %s\n  support.js: %s", s.url, s.sri, m[1])
		}
	}
	return nil
}

// cdnScripts are the three unpkg files support.js loads, with the exact URLs and
// SRI values it pins (support.js lines defining REACT_URL/REACT_DOM_URL/BABEL_URL).
// They are repeated here on purpose: the check below fails if the two ever disagree,
// which is what turns "we vendored a different file" from a silent supply-chain
// substitution into a build error.
var cdnScripts = []struct {
	url, sri, local string
}{
	{"https://unpkg.com/react@18.3.1/umd/react.production.min.js",
		"sha384-DGyLxAyjq0f9SPpVevD6IgztCFlnMF6oW/XQGmfe+IsZ8TqEiDrcHkMLKI6fiB/Z", "react.production.min.js"},
	{"https://unpkg.com/react-dom@18.3.1/umd/react-dom.production.min.js",
		"sha384-gTGxhz21lVGYNMcdJOyq01Edg0jhn/c22nsx0kyqP0TxaV5WVdsSH1fSDUf5YJj1", "react-dom.production.min.js"},
	{"https://unpkg.com/@babel/standalone@7.29.0/babel.min.js",
		"sha384-m08KidiNqLdpJqLq95G/LEi8Qvjl/xUYll3QILypMoQ65QorJ9Lvtp2RXYGBFj1y", "babel.min.js"},
}

// fontSubsetsToKeep limits what is vendored. Google serves every script per family —
// latin, latin-ext, cyrillic, greek, vietnamese, hebrew, arabic, thai, devanagari —
// which for these 36 families is 160 files. The composer is a watch-face layout tool
// whose display type is set in Latin text, so latin and latin-ext are vendored and the
// rest fall back to the browser's own stack online. Widening this is a one-line change
// and a size decision the repo owner should see in a diff.
var fontSubsetsToKeep = map[string]bool{"latin": true, "latin-ext": true}

// chromeUA gets the woff2 forms of the Google CSS; a generic UA is served TTFs.
const chromeUA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " +
	"(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"

type manifest struct {
	GeneratedAt string          `json:"generatedAt"`
	Scripts     []manifestEntry `json:"scripts"`
	FontsCSS    string          `json:"fontsCss"`
	Fonts       []manifestEntry `json:"fonts"`
}

type manifestEntry struct {
	URL      string `json:"url"`
	Local    string `json:"local"`
	SHA256   string `json:"sha256"`
	Bytes    int    `json:"bytes"`
	SRI      string `json:"sri,omitempty"`
	SRIField string `json:"sriField,omitempty"`
}

func main() {
	out := flag.String("out", "web/composer/vendor", "directory to write the vendored assets into")
	source := flag.String("composer", "tools/watch-composer", "the composer dir whose .dc.html names the font links")
	verify := flag.Bool("verify", false, "check the existing tree against the manifest and download nothing")
	flag.Parse()

	if *verify {
		if err := verifyTree(*out); err != nil {
			fmt.Fprintln(os.Stderr, "orbvendor: verify:", err)
			os.Exit(1)
		}
		fmt.Println("orbvendor: vendored tree matches its manifest")
		return
	}
	if err := vendor(*out, *source); err != nil {
		fmt.Fprintln(os.Stderr, "orbvendor:", err)
		os.Exit(1)
	}
}

var (
	fontLinkRe = regexp.MustCompile(`href="(https://fonts\.googleapis\.com/css2[^"]+)"`)
	faceRe     = regexp.MustCompile(`(?s)/\*\s*([a-z0-9-]+)\s*\*/\s*@font-face\s*\{[^}]*?\}`)
	fontURLRe  = regexp.MustCompile(`url\((https://[^)]+\.woff2)\)`)
)

// collectFonts re-fetches every font stylesheet the composer asks for, keeps only the
// subsets named by fontSubsetsToKeep, downloads those files, and returns one CSS whose
// src URLs point at the local fonts/ directory.
//
// The subset filter is the reason the result is 93 files and not 160: Google serves
// every script per family, and a watch-face layout tool sets its display type in Latin.
func collectFonts(client *http.Client, source string) (string, []fetched, error) {
	entry := filepath.Join(source, "Watch Composer.dc.html")
	page, err := os.ReadFile(entry)
	if err != nil {
		return "", nil, fmt.Errorf("read composer entry %s: %w", entry, err)
	}
	seenLink := map[string]bool{}
	seenFont := map[string]bool{}
	var out strings.Builder
	var files []fetched
	for _, m := range fontLinkRe.FindAllStringSubmatch(string(page), -1) {
		u := strings.ReplaceAll(m[1], "&amp;", "&")
		if seenLink[u] {
			continue
		}
		seenLink[u] = true
		css, err := fetch(client, u)
		if err != nil {
			return "", nil, fmt.Errorf("font stylesheet %s: %w", u, err)
		}
		for _, face := range faceRe.FindAllStringSubmatch(string(css), -1) {
			subset, block := face[1], face[0]
			if !fontSubsetsToKeep[subset] {
				continue
			}
			fm := fontURLRe.FindStringSubmatch(block)
			if fm == nil {
				continue
			}
			furl := fm[1]
			name := filepath.Base(furl)
			if !seenFont[furl] {
				seenFont[furl] = true
				b, err := fetch(client, furl)
				if err != nil {
					return "", nil, fmt.Errorf("font %s: %w", furl, err)
				}
				files = append(files, fetched{url: furl, local: "fonts/" + name, data: b})
			}
			out.WriteString(strings.Replace(block, furl, "fonts/"+name, 1))
			out.WriteByte('\n')
		}
	}
	if len(files) == 0 {
		return "", nil, fmt.Errorf("no font files selected; the subset filter or the Google CSS layout changed")
	}
	sort.Slice(files, func(i, j int) bool { return files[i].local < files[j].local })
	return out.String(), files, nil
}

// writeFileIfChanged leaves an identical file alone, so re-running the vendoring
// touches nothing and produces no diff.
func writeFileIfChanged(path string, b []byte) error {
	if old, err := os.ReadFile(path); err == nil && string(old) == string(b) {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, b, 0o644)
}

// verifyTree checks the committed vendor directory against its own manifest, so the
// suite can assert that what ships is what was downloaded and that no vendored file
// was edited by hand — the exact risk of committing third-party code into a repo.
func verifyTree(out string) error {
	b, err := os.ReadFile(filepath.Join(out, "MANIFEST.json"))
	if err != nil {
		return fmt.Errorf("no MANIFEST.json in %s: %w", out, err)
	}
	var m manifest
	if err := json.Unmarshal(b, &m); err != nil {
		return fmt.Errorf("MANIFEST.json is not valid JSON: %w", err)
	}
	if len(m.Scripts) != len(cdnScripts) {
		return fmt.Errorf("manifest lists %d scripts, expected %d", len(m.Scripts), len(cdnScripts))
	}
	for _, e := range m.Scripts {
		b, err := os.ReadFile(filepath.Join(out, e.Local))
		if err != nil {
			return fmt.Errorf("%s: %w", e.Local, err)
		}
		if sum(b) != e.SHA256 {
			return fmt.Errorf("%s sha256 = %s…, manifest says %s… — a vendored file was edited", e.Local, sum(b)[:16], e.SHA256[:16])
		}
		// The composer enforces the SRI at load time; check it here so a bad file is
		// caught by a build rather than by a blank editor.
		if e.SRI != "" && sri384(b) != e.SRI {
			return fmt.Errorf("%s no longer matches the SRI the composer will demand", e.Local)
		}
	}
	for _, e := range m.Fonts {
		b, err := os.ReadFile(filepath.Join(out, e.Local))
		if err != nil {
			return fmt.Errorf("%s: %w", e.Local, err)
		}
		if len(b) != e.Bytes || sum(b) != e.SHA256 {
			return fmt.Errorf("%s does not match its manifest entry", e.Local)
		}
	}
	return nil
}

// vendor downloads everything and writes the manifest. It is idempotent: a file whose
// bytes already match is left alone, so a re-run produces no diff.
func vendor(out, source string) error {
	supportPath := filepath.Join(source, "support.js")
	if err := checkSRIsMatchSupport(supportPath); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Join(out, "fonts"), 0o755); err != nil {
		return err
	}
	m := manifest{GeneratedAt: time.Now().UTC().Format(time.RFC3339)}

	client := &http.Client{Timeout: 90 * time.Second}
	for _, s := range cdnScripts {
		b, err := fetch(client, s.url)
		if err != nil {
			return fmt.Errorf("download %s: %w", s.url, err)
		}
		// The SRI support.js pins is checked before the file is kept: without it the
		// hub could happily serve a substituted React and nothing would notice.
		if got := sri384(b); got != s.sri {
			return fmt.Errorf("%s: SRI mismatch\n  pinned in support.js: %s\n  downloaded:        %s", s.url, s.sri, got)
		}
		if err := writeFileIfChanged(filepath.Join(out, s.local), b); err != nil {
			return err
		}
		m.Scripts = append(m.Scripts, manifestEntry{URL: s.url, Local: s.local, SHA256: sum(b), Bytes: len(b), SRI: s.sri})
		fmt.Printf("  %-28s %8d B  sri ok\n", s.local, len(b))
	}

	css, fonts, err := collectFonts(client, source)
	if err != nil {
		return err
	}
	for _, f := range fonts {
		if err := writeFileIfChanged(filepath.Join(out, "fonts", filepath.Base(f.local)), f.data); err != nil {
			return err
		}
		m.Fonts = append(m.Fonts, manifestEntry{URL: f.url, Local: "fonts/" + filepath.Base(f.local), SHA256: sum(f.data), Bytes: len(f.data)})
	}
	m.FontsCSS = "fonts.css"
	if err := writeFileIfChanged(filepath.Join(out, "fonts.css"), []byte(css)); err != nil {
		return err
	}
	b, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(out, "MANIFEST.json"), append(b, '\n'), 0o644); err != nil {
		return err
	}
	fmt.Printf("orbvendor: %d script(s), %d font file(s) into %s\n", len(m.Scripts), len(m.Fonts), out)
	return nil
}
