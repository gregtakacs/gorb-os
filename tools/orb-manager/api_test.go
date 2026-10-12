package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The Onboard port picker went blank when the LAN-discovery removal shed
// listPorts' object wrapper: app.js reads `const { ports } = ...`, so the bare
// array the handler started returning left ports undefined, `ports.length`
// threw, and the catch painted "(Cannot read properties of undefined)" where the
// cable names belonged. Nothing tested the shape, so the suite stayed green.
// These pin both ends of the contract.

func TestAPIPortsPayloadIsTheObjectTheClientDestructures(t *testing.T) {
	raw, err := json.Marshal(portsPayload([]string{"/dev/ttyACM0", "/dev/ttyACM1"}))
	if err != nil {
		t.Fatal(err)
	}
	if got := string(raw); got != `{"ports":["/dev/ttyACM0","/dev/ttyACM1"]}` {
		t.Errorf("payload = %s, want an object with a ports array", got)
	}
}

// A host with nothing plugged in reports an empty list, not null: the UI takes
// .length on the value, and null has none.
func TestAPIPortsPayloadHasNoNullList(t *testing.T) {
	for _, ports := range [][]string{nil, {}} {
		raw, err := json.Marshal(portsPayload(ports))
		if err != nil {
			t.Fatal(err)
		}
		if got := string(raw); got != `{"ports":[]}` {
			t.Errorf("payload for %v = %s, want {\"ports\":[]}", ports, got)
		}
	}
}

// The flash manifest lists images by name for the UI. Names, not paths: a
// Windows build dir is a UNC path full of backslashes, and path.Base (slash
// only) hands the whole thing back as the display name.
func TestAPIBuildNamesAreBasenamesNotPaths(t *testing.T) {
	s := &Server{buildDir: writeFakeBuild(t)}
	payload, err := s.describeBuild(nil)
	if err != nil {
		t.Fatalf("describeBuild: %v", err)
	}
	raw, err := json.Marshal(payload)
	if err != nil {
		t.Fatal(err)
	}
	var got struct {
		Present bool `json:"present"`
		Images  []struct {
			Name string `json:"name"`
		} `json:"images"`
	}
	if err := json.Unmarshal(raw, &got); err != nil {
		t.Fatalf("payload shape: %v (%s)", err, raw)
	}
	if !got.Present {
		t.Fatal("present = false, want the manifest found in the fixture build dir")
	}
	if len(got.Images) != 2 {
		t.Fatalf("images = %d, want the two in the manifest", len(got.Images))
	}
	for i, want := range []string{"bootloader.bin", "orb.bin"} {
		if got.Images[i].Name != want {
			t.Errorf("image %d name = %q, want the bare %q", i, got.Images[i].Name, want)
		}
		if strings.ContainsAny(got.Images[i].Name, `/\`) {
			t.Errorf("image %d name %q is a path, not a name", i, got.Images[i].Name)
		}
	}
}

func TestAPIPortsEndpointWrapsThePortsArray(t *testing.T) {
	s := &Server{}
	rr := httptest.NewRecorder()
	s.json(s.listPorts)(rr, httptest.NewRequest(http.MethodGet, "/api/ports", nil))

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	// Parse the way the browser does: a shape that only Go can read is no shape.
	var env struct {
		OK   bool `json:"ok"`
		Data struct {
			Ports []string `json:"ports"`
		} `json:"data"`
		Error string `json:"error"`
	}
	if err := json.Unmarshal(rr.Body.Bytes(), &env); err != nil {
		t.Fatalf("body is not the {ok,data} envelope: %v (%s)", err, rr.Body.String())
	}
	if !env.OK {
		t.Fatalf("ok = false: %s", env.Error)
	}
	if env.Data.Ports == nil {
		t.Error("data.ports is null; the picker needs an array it can index")
	}
}

// The two ends are pinned against each other, because the bug was a disagreement
// between them: api.go may not return a bare list, and app.js may not stop
// destructuring the field it reads.
func TestPortPickerContractMatchesTheHandler(t *testing.T) {
	appJS, err := webFS.ReadFile("web/app.js")
	if err != nil {
		t.Fatal(err)
	}
	src := string(appJS)
	caller := `getJSON("/api/ports")`
	i := strings.Index(src, caller)
	if i < 0 {
		t.Fatal("app.js no longer calls /api/ports")
	}
	lo := max(i-40, 0)
	call := src[lo:min(i+len(caller)+40, len(src))]
	if !strings.Contains(call, `{ ports }`) {
		t.Errorf("the picker no longer destructures ports from the envelope; align it with portsPayload: %q", call)
	}
	if !strings.Contains(src, "Array.isArray(ports)") {
		t.Error("the picker must not take .length on a payload it has not checked: keep the Array.isArray guard")
	}
}

// --- the two-sided contract pins --------------------------------------------

// readAppJS loads the client so a test can read the field names off it.
func readAppJS(t *testing.T) string {
	t.Helper()
	raw, err := webFS.ReadFile("web/app.js")
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

// clientFields returns the payload fields app.js reads for one endpoint: the
// text from the `const <v> = await getJSON("<path>")` assignment through the end
// of its try block, filtered for <v>.<field> accesses. The name list is what
// makes drift visible on either side — the handler renaming a key the UI still
// reads, or the UI starting to read a key the handler never sends.
func clientFields(t *testing.T, src, path string) (fields []string, block string) {
	t.Helper()
	caller := `getJSON("` + path + `")`
	i := strings.Index(src, caller)
	if i < 0 {
		t.Fatalf("app.js no longer calls %s", path)
	}
	end := strings.Index(src[i:], "} catch")
	if end < 0 {
		t.Fatalf("no try/catch around the %s call: the panel would show a raw exception", path)
	}
	block = src[i : i+end]

	// The consuming variable is the last word on the left of the assignment.
	assign := src[strings.LastIndex(src[:i], "\n")+1 : i]
	eq := strings.Index(assign, "=")
	if eq < 0 {
		t.Fatalf("cannot read the consuming variable from %q", assign)
	}
	words := strings.Fields(strings.ReplaceAll(assign[:eq], "await", " "))
	if len(words) == 0 {
		t.Fatalf("unexpected assignment form: %q", assign)
	}
	v := words[len(words)-1]

	re := regexp.MustCompile(`\b` + regexp.QuoteMeta(v) + `\.([A-Za-z_][A-Za-z0-9_]*)`)
	seen := map[string]bool{}
	for _, m := range re.FindAllStringSubmatch(block, -1) {
		if !seen[m[1]] {
			seen[m[1]] = true
			fields = append(fields, m[1])
		}
	}
	return fields, block
}

// jsonKeys unmarshals a payload object into its key set.
func jsonKeys(t *testing.T, payload any) map[string]any {
	t.Helper()
	raw, err := json.Marshal(payload)
	if err != nil {
		t.Fatal(err)
	}
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatalf("payload is not a JSON object: %v (%s)", err, raw)
	}
	return m
}

// The build panel is the second panel the port picker's bug class would have
// hit: it took .length and .map on images with no guard, and nothing linked the
// key names the client reads to the keys the handler emits.
func TestBuildPanelFieldsMatchBetweenClientAndHandler(t *testing.T) {
	src := readAppJS(t)
	fields, block := clientFields(t, src, "/api/build")

	want := []string{"present", "images", "buildDir"}
	if strings.Join(fields, ",") != strings.Join(want, ",") {
		t.Errorf("the panel reads %v, want exactly %v — update this list and the handler together", fields, want)
	}
	// The guard is the part that turns a shape bug into a message instead of a
	// crash, so it must survive edits to this function.
	if !strings.Contains(block, "Array.isArray(b.images)") {
		t.Error("the panel must not take .length/.map on images unchecked: keep the Array.isArray guard")
	}

	dir := writeFakeBuild(t)
	s := &Server{buildDir: dir}
	payload, err := s.describeBuild(nil)
	if err != nil {
		t.Fatalf("describeBuild: %v", err)
	}
	keys := jsonKeys(t, payload)
	for _, f := range fields {
		if _, ok := keys[f]; !ok {
			t.Errorf("the panel reads b.%s but the handler sends no such key: %v", f, keys)
		}
	}

	// The image rows are the other half of the contract.
	imgs, ok := keys["images"].([]any)
	if !ok || len(imgs) == 0 {
		t.Fatalf("images = %#v, want a non-empty array", keys["images"])
	}
	first := jsonKeys(t, imgs[0])
	for _, f := range []string{"offset", "name", "bytes"} {
		if _, ok := first[f]; !ok {
			t.Errorf("the panel renders i.%s but no image carries it: %v", f, first)
		}
	}
	for _, f := range []string{"offset", "name", "bytes"} {
		if !strings.Contains(block, "i."+f) {
			t.Errorf("the handler sends image field %q that the panel no longer renders", f)
		}
	}
}

// /api/status writes the hub's own identity line. A rename there cannot crash
// the page, so it is exactly the kind of drift that would sit unnoticed while
// the header quietly read "undefined".
func TestMetaLineFieldsMatchBetweenClientAndHandler(t *testing.T) {
	src := readAppJS(t)
	fields, _ := clientFields(t, src, "/api/status")

	if strings.Join(fields, ",") != "platform,buildDir" {
		t.Errorf("the meta line reads %v, want [platform buildDir] — update this list and hello() together", fields)
	}
	s := &Server{repoDir: "/repo", buildDir: "/repo/build"}
	payload, err := s.hello(nil)
	if err != nil {
		t.Fatalf("hello: %v", err)
	}
	keys := jsonKeys(t, payload)
	for _, f := range fields {
		if _, ok := keys[f]; !ok {
			t.Errorf("the meta line reads m.%s but hello() sends no such key: %v", f, keys)
		}
	}
}

// writeFakeBuild lays down the smallest tree describeBuild accepts: a manifest
// naming two images that exist on disk.
func writeFakeBuild(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	boot := filepath.Join(dir, "bootloader")
	if err := os.MkdirAll(boot, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, f := range []string{filepath.Join(boot, "bootloader.bin"), filepath.Join(dir, "orb.bin")} {
		if err := os.WriteFile(f, []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	manifest := `{"flash_settings":{"flash_mode":"dio","flash_size":"16MB"},` +
		`"flash_files":{"0x0":"bootloader/bootloader.bin","0x10000":"orb.bin"}}`
	if err := os.WriteFile(filepath.Join(dir, "flasher_args.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}
