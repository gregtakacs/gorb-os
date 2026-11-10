package main

// orb-os-net design §6.4, G1 slice step 2: the hub owns the contract between an editor
// and whatever serves its library.
//
// TestEditorAPIContract runs one scripted scenario through the same s.routes(mux) the
// binary serves and records every request and answer into contract/editor-api.json.
// orb-os-net replays that file against its own endpoints, so a change to an endpoint the
// editor uses must be a change to the file: the test rebuilds the fixture on every run
// and fails when the committed bytes are not what the handlers now answer.
//
// The hub's millisecond timestamps are the only volatile values, normalised to 0;
// TestEditorAPIContractIsDeterministic is what proves nothing else is (two fresh hubs,
// one scenario each, byte-identical encodings), and TestEditorAPIContractCoversTheRuntime
// proves the fixture still names every path the runtime asks the hub for.
//
// Regenerate with: make -C tools/orb-manager contract

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

const (
	// contractPath is the committed fixture, relative to this package.
	contractPath = "contract/editor-api.json"
	// contractWriteEnv asks the test to write the fixture instead of checking it.
	contractWriteEnv  = "ORB_WRITE_CONTRACT"
	contractAbout     = "Every request an editor's library calls make, and every answer the hub gave, recorded from the hub's own handlers by TestEditorAPIContract so another server can replay them; the send door's /api/orb/known, /api/theme/upload, /api/theme/commit and /api/theme/upload-status and /api/editor/source stay out because a hosted editor never calls them."
	contractGenerator = "tools/orb-manager/contract_test.go TestEditorAPIContract"
	// contractStale is what a fixture the handlers moved past has to say, and how to fix it.
	contractStale = "contract/editor-api.json is out of date with the hub's handlers: run make -C tools/orb-manager contract and commit the file"
)

// contractVolatile lists the JSON object keys whose values are replaced by 0 wherever
// they appear: the millisecond stamps the hub writes, and the file system reports back.
var contractVolatile = []string{"at"}

// The fixture's shapes, one struct per level, so the members keep this order in the file.

type contractDoc struct {
	Format    int            `json:"format"`
	About     string         `json:"about"`
	Generator string         `json:"generator"`
	Volatile  []string       `json:"volatile"`
	Steps     []contractStep `json:"steps"`
}

type contractStep struct {
	Name     string       `json:"name"`
	Request  contractReq  `json:"request"`
	Response contractResp `json:"response"`
}

type contractReq struct {
	Method      string          `json:"method"`
	Path        string          `json:"path"`
	Query       [][2]string     `json:"query,omitempty"`
	ContentType string          `json:"contentType,omitempty"`
	Body        json.RawMessage `json:"body,omitempty"`
	BodyBase64  string          `json:"bodyBase64,omitempty"`
	BodyText    string          `json:"bodyText,omitempty"`
}

type contractResp struct {
	Status      int             `json:"status"`
	ContentType string          `json:"contentType,omitempty"`
	Body        json.RawMessage `json:"body,omitempty"`
	BodyBase64  string          `json:"bodyBase64,omitempty"`
	BodyText    string          `json:"bodyText,omitempty"`
}

// contractRun is one scenario against one fresh hub: its routes, the steps recorded so
// far, and the keys to normalise.
type contractRun struct {
	t        *testing.T
	token    string
	handler  http.Handler
	steps    []contractStep
	volatile map[string]bool
}

// newContractRun puts a hub's routes behind one handler: the same routes() call the
// binary makes, on a library with nothing in it yet.
func newContractRun(t *testing.T) *contractRun {
	t.Helper()
	s, _, _ := libHub(t)
	mux := http.NewServeMux()
	s.routes(mux)
	volatile := map[string]bool{}
	for _, k := range contractVolatile {
		volatile[k] = true
	}
	return &contractRun{t: t, token: s.token, handler: mux, volatile: volatile}
}

// call runs one request through the hub's real routes and records it with its answer.
// The control token rides along because the hub demands it, and is not recorded. Each
// step then waits out a millisecond: the library orders itself by the hub's own stamps,
// so two writes must never land inside one and leave the fixture's order up to luck.
func (c *contractRun) call(name, method, target, contentType string, body []byte) contractStep {
	t := c.t
	t.Helper()
	path, rawQuery := target, ""
	if i := strings.IndexByte(target, '?'); i >= 0 {
		path, rawQuery = target[:i], target[i+1:]
	}
	req := httptest.NewRequest(method, target, bytes.NewReader(body))
	req.Header.Set(tokenHeader, c.token)
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	rec := httptest.NewRecorder()
	c.handler.ServeHTTP(rec, req)
	time.Sleep(time.Millisecond)

	step := contractStep{Name: name, Request: contractReq{Method: method, Path: path, Query: contractPairs(rawQuery), ContentType: contentType},
		Response: contractResp{Status: rec.Code, ContentType: rec.Header().Get("Content-Type")}}
	if len(body) > 0 {
		step.Request.Body, step.Request.BodyBase64, step.Request.BodyText = c.parts(contentType, body)
	}
	if rec.Body.Len() > 0 {
		step.Response.Body, step.Response.BodyBase64, step.Response.BodyText = c.parts(step.Response.ContentType, rec.Body.Bytes())
	}
	c.steps = append(c.steps, step)
	return step
}

// parts files one body the way its content type asks: as a JSON value when it is JSON
// and parses, as base64 for the theme bytes a keep carries, as text for anything else.
func (c *contractRun) parts(contentType string, body []byte) (json.RawMessage, string, string) {
	if v := c.jsonValue(contentType, body); v != nil {
		return v, "", ""
	}
	if strings.HasPrefix(contentType, "application/octet-stream") {
		return nil, base64.StdEncoding.EncodeToString(body), ""
	}
	return nil, "", string(body)
}

// jsonValue parses a body written as JSON and returns it normalised, or nil when the
// content type is not JSON or the bytes are not one JSON value.
func (c *contractRun) jsonValue(contentType string, body []byte) json.RawMessage {
	if !strings.HasPrefix(contentType, "application/json") {
		return nil
	}
	dec := json.NewDecoder(bytes.NewReader(body))
	dec.UseNumber()
	var v any
	if dec.Decode(&v) != nil {
		return nil
	}
	out, err := contractCompact(c.normalise(v))
	if err != nil {
		c.t.Fatalf("record a JSON body: %v", err)
	}
	return out
}

// contractCompact encodes a value with the HTML escaping off, so a sentence the hub wrote
// with an angle bracket or an apostrophe in it reads like the hub wrote it.
func contractCompact(v any) (json.RawMessage, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		return nil, err
	}
	return bytes.TrimRight(buf.Bytes(), "\n"), nil
}

// normalise replaces every volatile member's number with 0, at any depth, and nothing
// else: the names, stems, file names and serials this scenario meets are the hub's own
// doing, and repeat exactly.
func (c *contractRun) normalise(v any) any {
	switch t := v.(type) {
	case map[string]any:
		for k, val := range t {
			if c.volatile[k] {
				if _, number := val.(json.Number); number {
					t[k] = json.Number("0")
					continue
				}
			}
			t[k] = c.normalise(val)
		}
		return t
	case []any:
		for i := range t {
			t[i] = c.normalise(t[i])
		}
		return t
	}
	return v
}

// contractPairs reads a query back in the order it was sent, as [key, value] pairs, with
// the escaping the caller wrote undone.
func contractPairs(rawQuery string) [][2]string {
	if rawQuery == "" {
		return nil
	}
	out := [][2]string{}
	for _, part := range strings.Split(rawQuery, "&") {
		if part == "" {
			continue
		}
		k, v := part, ""
		if i := strings.IndexByte(part, '='); i >= 0 {
			k, v = part[:i], part[i+1:]
		}
		out = append(out, [2]string{queryPart(k), queryPart(v)})
	}
	return out
}

func queryPart(s string) string {
	if out, err := url.QueryUnescape(s); err == nil {
		return out
	}
	return s
}

// scenario runs the scripted scenario: the library empty, a draft made and remade, a
// theme kept from bytes the test hands the hub rather than writes to its disk, the pair
// read back through every door the editor uses, and the refusals on the way out. It
// creates everything it later reads through the API itself, so a server replaying the
// steps in order from an empty library lands on the same answers.
func (c *contractRun) scenario() {
	const jsonCT, rawCT = "application/json", "application/octet-stream"
	const bigSub, tourbillon = "Big sub", "Tourbillon"
	theme := func(appVersion string, art int) []byte {
		return withDesign(orbfake.Container(tourbillon), string(designDoc(tourbillon, appVersion, art)))
	}

	c.call("list an empty library", http.MethodGet, "/api/library/entries?app=clock", "", nil)
	c.call("save a draft", http.MethodPost, "/api/design/save?app=clock&name=Big%20sub", jsonCT, designDoc(bigSub, "v1.2.16", 8))
	c.call("replace the draft", http.MethodPost, "/api/design/save?app=clock&name=Big%20sub", jsonCT, designDoc(bigSub, "v1.2.16", 12))
	c.call("list the one draft", http.MethodGet, "/api/library/entries?app=clock", "", nil)
	c.call("open the draft", http.MethodGet, "/api/library/design?app=clock&name=Big%20sub", "", nil)
	c.call("keep a baked theme", http.MethodPost, "/api/library/keep?app=clock", rawCT, theme("v1.2.12", 4))
	c.call("save a draft over a theme", http.MethodPost, "/api/design/save?app=clock&name=Tourbillon", jsonCT, designDoc(tourbillon, "v1.2.16", 4))
	listed := c.call("list a draft and a changed theme", http.MethodGet, "/api/library/entries?app=clock", "", nil)
	c.call("open a changed theme's draft", http.MethodGet, "/api/library/design?app=clock&name=Tourbillon", "", nil)
	c.call("open the theme beside its draft", http.MethodGet, "/api/library/design?app=clock&name=Tourbillon&from=theme", "", nil)
	c.call("read a shelf theme's design", http.MethodGet, "/api/theme/design?name="+url.QueryEscape(c.stemOf(listed, tourbillon)), "", nil)
	c.call("rebake and keep the changes", http.MethodPost, "/api/library/keep?app=clock&fold=0", rawCT, theme("v1.2.16", 6))
	c.call("list the theme made anew", http.MethodGet, "/api/library/entries?app=clock", "", nil)
	c.call("discard a theme's changes", http.MethodPost, "/api/library/forget?app=clock&name=Tourbillon&what=draft", "", nil)
	c.call("forget the whole entry", http.MethodPost, "/api/library/forget?app=clock&name=Tourbillon&what=all", "", nil)
	c.call("list the library once more", http.MethodGet, "/api/library/entries?app=clock", "", nil)
	c.call("list another app's library", http.MethodGet, "/api/library/entries?app=flights", "", nil)

	c.call("refuse an app slug with a path in it", http.MethodGet, "/api/library/entries?app=..%2Fx", "", nil)
	c.call("refuse another app's design", http.MethodPost, "/api/design/save?app=flights&name=x", jsonCT, designDoc("x", "v1.2.16", 1))
	c.call("refuse a document that is not a design", http.MethodPost, "/api/design/save?app=clock&name=x", jsonCT, []byte(`{"format":"zip"}`))
	c.call("refuse a blank name", http.MethodPost, "/api/design/save?app=clock&name=%20", jsonCT, nil)
	c.call("refuse a design that is not there", http.MethodGet, "/api/library/design?app=clock&name=Nope", "", nil)
	c.call("refuse to forget something else", http.MethodPost, "/api/library/forget?app=clock&name=Big%20sub&what=some", "", nil)
	c.call("refuse another app's theme", http.MethodPost, "/api/library/keep?app=flights", rawCT, theme("v1.2.16", 6))
	c.call("refuse a GET at the save door", http.MethodGet, "/api/design/save?app=clock&name=x", "", nil)
}

// stemOf reads the shelf stem an entry listing carries for a name, the handle the Edit
// door takes. It is the hub's own answer, not a file name the test guessed at.
func (c *contractRun) stemOf(step contractStep, name string) string {
	t := c.t
	t.Helper()
	for _, e := range contractEntries(t, step) {
		if e.Name == name && e.Theme != nil && e.Theme.Stem != "" {
			return e.Theme.Stem
		}
	}
	t.Fatalf("the listing carries no shelf stem for %q: %s", name, step.Response.Body)
	return ""
}

// contractEntry is the part of a listed entry the scenario's assertions read.
type contractEntry struct {
	Name  string `json:"name"`
	State string `json:"state"`
	Theme *struct {
		Stem     string `json:"stem"`
		MadeWith string `json:"made_with"`
	} `json:"theme"`
}

// contractEntries reads an entry listing out of a step's recorded answer.
func contractEntries(t *testing.T, step contractStep) []contractEntry {
	t.Helper()
	var env struct {
		OK   bool            `json:"ok"`
		Data []contractEntry `json:"data"`
	}
	if len(step.Response.Body) == 0 || json.Unmarshal(step.Response.Body, &env) != nil || !env.OK {
		t.Fatalf("%s: not an ok answer: %d %s", step.Name, step.Response.Status, step.Response.Body)
	}
	return env.Data
}

// contractBuild runs the scenario in a fresh hub and returns the fixture it recorded.
func contractBuild(t *testing.T) (contractDoc, []contractStep) {
	t.Helper()
	c := newContractRun(t)
	c.scenario()
	return contractDoc{Format: 1, About: contractAbout, Generator: contractGenerator,
		Volatile: contractVolatile, Steps: c.steps}, c.steps
}

// contractEncode writes the fixture: two-space indentation, no HTML escaping, a final newline.
func contractEncode(t *testing.T, doc contractDoc) []byte {
	t.Helper()
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	enc.SetIndent("", "  ")
	if err := enc.Encode(doc); err != nil {
		t.Fatalf("encode the fixture: %v", err)
	}
	return buf.Bytes()
}

func TestEditorAPIContract(t *testing.T) {
	doc, steps := contractBuild(t)
	assertScenario(t, steps)
	got := contractEncode(t, doc)

	if os.Getenv(contractWriteEnv) != "" {
		if err := os.MkdirAll(filepath.Dir(contractPath), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(contractPath, got, 0o644); err != nil {
			t.Fatal(err)
		}
		t.Logf("wrote %s with %d steps", contractPath, len(steps))
		return
	}
	want, err := os.ReadFile(contractPath)
	if err != nil {
		t.Fatalf("%s: %v — %s", contractPath, err, contractStale)
	}
	if !bytes.Equal(want, got) {
		t.Errorf("%s differs from the handlers at line %d: %s", contractPath, firstDiffLine(want, got), contractStale)
	}
}

// firstDiffLine names the line a committed fixture and a fresh recording first part at,
// so the failure points at the step that moved rather than at two whole files.
func firstDiffLine(want, got []byte) int {
	w, g := strings.Split(string(want), "\n"), strings.Split(string(got), "\n")
	for i := 0; i < len(w) && i < len(g); i++ {
		if w[i] != g[i] {
			return i + 1
		}
	}
	return min(len(w), len(g)) + 1
}

// assertScenario checks that the answers make sense as a library's behaviour, which is
// what the fixture is a picture of. The wording inside them it does not invent.
func assertScenario(t *testing.T, steps []contractStep) {
	t.Helper()
	if len(steps) != 25 {
		t.Fatalf("%d steps recorded, want 25", len(steps))
	}
	// Every step of the story answers 200. Four of them hand the editor a design document
	// as the body; the rest answer with the hub's ok envelope.
	for i, s := range steps[:17] {
		if s.Response.Status != http.StatusOK {
			t.Errorf("step %d %q: %d %s, want 200", i+1, s.Name, s.Response.Status, s.Response.BodyText)
			continue
		}
		if rawDocument(i) {
			if doc := contractJSON(t, s); doc["format"] != "wc-design" {
				t.Errorf("step %d %q: not a design document: %.200s", i+1, s.Name, s.Response.Body)
			}
			continue
		}
		if env := contractJSON(t, s); env["ok"] != true {
			t.Errorf("step %d %q: not ok: %s", i+1, s.Name, s.Response.Body)
		}
	}
	// an empty library, one draft, the pair, the theme alone again, then the draft alone.
	if es := contractEntries(t, steps[0]); len(es) != 0 {
		t.Errorf("a fresh hub's library = %+v, want nothing", es)
	}
	if es := contractEntries(t, steps[3]); len(es) != 1 || es[0].Name != "Big sub" || es[0].State != "draft" {
		t.Errorf("after one save = %+v, want the Big sub draft alone", es)
	}
	states := map[string]string{"Big sub": "draft", "Tourbillon": "changed"}
	if es := contractEntries(t, steps[7]); len(es) != len(states) {
		t.Errorf("after a keep and a save = %+v, want a draft and a changed theme", es)
	} else {
		for _, e := range es {
			if states[e.Name] != e.State {
				t.Errorf("step 8: %s = %s, want %s", e.Name, e.State, states[e.Name])
			}
		}
	}
	after := map[string]contractEntry{}
	for _, e := range contractEntries(t, steps[12]) {
		after[e.Name] = e
	}
	if len(after) != 2 {
		t.Errorf("after a rebake = %+v, want the changed theme beside the draft", after)
	} else if e := after["Tourbillon"]; e.State != "changed" {
		t.Errorf("after a rebake Tourbillon = %+v, want the theme still changed", e)
	} else if e.Theme == nil || e.Theme.MadeWith != "v1.2.16" {
		t.Errorf("after a rebake Tourbillon = %+v, want a theme made with v1.2.16", e)
	} else if e := after["Big sub"]; e.State != "draft" {
		t.Errorf("after a rebake Big sub = %+v, want the draft untouched", e)
	}
	if es := contractEntries(t, steps[15]); len(es) != 1 || es[0].Name != "Big sub" {
		t.Errorf("after forgetting Tourbillon = %+v, want the Big sub draft alone", es)
	}
	if es := contractEntries(t, steps[16]); len(es) != 0 {
		t.Errorf("flights sees %+v, want the clock's entries kept from it", es)
	}
	// The refusals answer a 4xx, and the ones that answer in JSON give the hub's reason.
	for _, s := range steps[17:] {
		if s.Response.Status < 400 || s.Response.Status > 499 {
			t.Errorf("%q: %d %s, want a 4xx", s.Name, s.Response.Status, s.Response.BodyText)
			continue
		}
		if len(s.Response.Body) == 0 {
			continue // the plain-text answers carry no envelope to read
		}
		env := contractJSON(t, s)
		if env["ok"] != false {
			t.Errorf("%q: ok is %v, want false: %s", s.Name, env["ok"], s.Response.Body)
		}
		if msg, _ := env["error"].(string); msg == "" {
			t.Errorf("%q: a refusal with no reason: %s", s.Name, s.Response.Body)
		}
	}
}

// rawDocument reports whether step i (0-based) answers with a design document rather
// than the hub's envelope: the doors that hand one to the editor.
func rawDocument(i int) bool {
	switch i + 1 {
	case 5, 9, 10, 11:
		return true
	}
	return false
}

func contractJSON(t *testing.T, s contractStep) map[string]any {
	t.Helper()
	var env map[string]any
	if len(s.Response.Body) == 0 || json.Unmarshal(s.Response.Body, &env) != nil {
		t.Fatalf("%q: not a JSON answer: %d %s", s.Name, s.Response.Status, s.Response.BodyText)
	}
	return env
}

// TestEditorAPIContractIsDeterministic builds the fixture twice, in two fresh hubs, and
// requires the two encodings to be byte-identical. Everything the scenario names it chose
// and everything it lists the hub filed from those bytes, so the only answers free to
// move are the timestamps — which is exactly what `volatile` claims, and what a normaliser
// that never fired would not prove.
func TestEditorAPIContractIsDeterministic(t *testing.T) {
	firstDoc, _ := contractBuild(t)
	secondDoc, _ := contractBuild(t)
	first, second := contractEncode(t, firstDoc), contractEncode(t, secondDoc)
	if !bytes.Equal(first, second) {
		t.Errorf("the same scenario in two fresh hubs recorded different fixtures, first differing at line %d: a value the normaliser does not cover moved",
			firstDiffLine(first, second))
	}
	if !bytes.Contains(first, []byte(`"at": 0`)) {
		t.Error("the fixture holds no normalised timestamp, so the normaliser never ran")
	}
}

// hostUrlCall finds one path as the runtime asks for it, at the seam G1 step 1 gave it.
// The pattern is the call as written, so a path that stops being a call stops being
// covered, and the test below says so rather than passing on a silent scan.
var hostUrlCall = regexp.MustCompile(`OrbTheme\.hostUrl\('(/[^'?]+)`)

// TestEditorAPIContractCoversTheRuntime holds the committed fixture to the runtime: every
// path theme@1 calls, prefixed /api, is a step's path. /editor/source reports on an orb,
// which a hosted editor never has (design §6.3), and is exempt; so are the site-only
// doors a hosted editor calls under GorbHost's assets flag (an image by hash, a shared
// design's bundle), which the hub never serves and its editors never ask.
var siteOnly = map[string]bool{"/asset": true, "/shared/bundle": true}

// hubOnly is the mirror case (H1 step h1-2): the hub's own import doors, which only the
// hub serves and only an import page asks. A scenario cannot record them — the ticket one
// of them answers for exists only after the hub has fetched a live site's bundle — so the
// fixture stays what h1-1 said it must be, byte-for-byte, and this test names the two.
var hubOnly = map[string]bool{"/site/bundle": true, "/site/keep": true}

func TestEditorAPIContractCoversTheRuntime(t *testing.T) {
	src, err := os.ReadFile(filepath.Join("web", "runtime", "theme@1", "session.js"))
	if err != nil {
		t.Fatal(err)
	}
	doc := readContract(t)
	calls := map[string]bool{}
	for _, m := range hostUrlCall.FindAllStringSubmatch(string(src), -1) {
		calls[m[1]] = true
	}
	if len(calls) < 6 {
		t.Fatalf("%d OrbTheme.hostUrl paths found in session.js, want at least 6: the scan went blind", len(calls))
	}
	recorded := map[string]bool{}
	for _, s := range doc.Steps {
		recorded[s.Request.Path] = true
	}
	missing := []string{}
	for path := range calls {
		if path == "/editor/source" || siteOnly[path] || hubOnly[path] {
			continue
		}
		if !recorded["/api"+path] {
			missing = append(missing, "/api"+path)
		}
	}
	if len(missing) > 0 {
		sort.Strings(missing)
		t.Errorf("the runtime calls %s, which no step of %s records", strings.Join(missing, ", "), contractPath)
	}
}

// readContract loads the committed fixture, the one a replaying server runs, rather than
// a fresh build of it.
func readContract(t *testing.T) contractDoc {
	t.Helper()
	raw, err := os.ReadFile(contractPath)
	if err != nil {
		t.Fatalf("%s: %v — %s", contractPath, err, contractStale)
	}
	var doc contractDoc
	if err := json.Unmarshal(raw, &doc); err != nil {
		t.Fatalf("%s: %v", contractPath, err)
	}
	return doc
}
