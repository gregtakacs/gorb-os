package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"fmt"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
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

// The flash panel lists images by name, never by path. The bundle makes that
// structural: images arrive as bytes beside their manifest, so there is no
// directory to spell and no UNC backslash form left to leak into the UI. The
// ../assets/ entry is the case worth pinning -- idf.py writes the clock art that
// way, and it must still surface as one clean basename.
func TestAPIBuildNamesAreBasenamesNotPaths(t *testing.T) {
	s := bundleServer(t)
	payload, err := s.describeFirmware(nil)
	if err != nil {
		t.Fatalf("describeFirmware: %v", err)
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
		t.Fatal("present = false, want the fixture bundle to be found")
	}
	if len(got.Images) != 3 {
		t.Fatalf("images = %d, want the three in the manifest", len(got.Images))
	}
	for i, want := range []string{"bootloader.bin", "orb.bin", "clock_assets.bin"} {
		if got.Images[i].Name != want {
			t.Errorf("image %d name = %q, want the bare %q", i, got.Images[i].Name, want)
		}
		if strings.ContainsAny(got.Images[i].Name, `/\`) {
			t.Errorf("image %d name %q is a path, not a name", i, got.Images[i].Name)
		}
	}
}

func TestAPIPortsEndpointWrapsThePortsArray(t *testing.T) {
	s := newHub()
	rr := httptest.NewRecorder()
	s.handlePorts(rr, httptest.NewRequest(http.MethodGet, "/api/ports", nil))

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	// Parse the way the browser does: a shape that only Go can read is no shape.
	var env struct {
		OK   bool `json:"ok"`
		Data struct {
			// Entries are objects, not bare strings: each carries the cached
			// identity so the card can show a MAC without rebooting anything.
			Ports []struct {
				Port  string `json:"port"`
				Known bool   `json:"known"`
			} `json:"ports"`
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
	for i, e := range env.Data.Ports {
		if e.Port == "" {
			t.Errorf("entry %d has no port name; the picker would render an empty option", i)
		}
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

	want := []string{"present", "images", "version", "project", "source", "sha256"}
	if strings.Join(fields, ",") != strings.Join(want, ",") {
		t.Errorf("the panel reads %v, want exactly %v — update this list and the handler together", fields, want)
	}
	// The guard is the part that turns a shape bug into a message instead of a
	// crash, so it must survive edits to this function.
	if !strings.Contains(block, "Array.isArray(b.images)") {
		t.Error("the panel must not take .length/.map on images unchecked: keep the Array.isArray guard")
	}

	s := bundleServer(t)
	payload, err := s.describeFirmware(nil)
	if err != nil {
		t.Fatalf("describeFirmware: %v", err)
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

	if strings.Join(fields, ",") != "platform,firmware" {
		t.Errorf("the meta line reads %v, want [platform firmware] — update this list and hello() together", fields)
	}
	s := bundleServer(t)
	s.repoDir = "/repo"
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

// fakeBundle builds a firmware bundle in memory the way the release step does,
// so the API tests exercise the same loader an uploaded file goes through.
func fakeBundle(t *testing.T) *firmware.Bundle {
	t.Helper()
	manifest := []byte(`{"flash_settings":{"flash_mode":"dio","flash_size":"16MB"},` +
		`"flash_files":{"0x0":"bootloader/bootloader.bin","0x10000":"orb.bin","0x420000":"../assets/clock_assets.bin"}}`)
	files := map[string][]byte{
		"bootloader/bootloader.bin":  {0xE9, 0x00, 0x02, 0x22, 1, 0, 0, 0, 'b'},
		"orb.bin":                    []byte("app"),
		"../assets/clock_assets.bin": []byte("art"),
	}
	blob, err := firmware.WriteBundle(manifest, func(rel string) ([]byte, error) {
		data, ok := files[rel]
		if !ok {
			return nil, fmt.Errorf("no such image %q", rel)
		}
		return data, nil
	})
	if err != nil {
		t.Fatalf("WriteBundle: %v", err)
	}
	b, err := firmware.Load(blob, "fake.zip")
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	return b
}

// bundleServer returns a hub with one firmware selected, which is every state a
// real hub is ever in: firmware is settled before it binds.
func bundleServer(t *testing.T) *Server {
	t.Helper()
	s := newHub()
	s.adoptFirmware(fakeBundle(t))
	return s
}

// uploadBundle POSTs one firmware bundle the way the Onboard card does.
func uploadBundle(t *testing.T, s *Server, filename string, blob []byte) (int, map[string]any) {
	t.Helper()
	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	fw, err := mw.CreateFormFile("firmware", filename)
	if err != nil {
		t.Fatal(err)
	}
	fw.Write(blob)
	mw.Close()
	req := httptest.NewRequest(http.MethodPost, "/api/firmware", &body)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	rec := httptest.NewRecorder()
	s.handleFirmwareUpload(rec, req)
	var out map[string]any
	_ = json.Unmarshal(rec.Body.Bytes(), &out)
	return rec.Code, out
}

func bundledServer(t *testing.T) *Server {
	t.Helper()
	s := newHub()
	b, err := firmware.BuiltIn()
	if err != nil {
		t.Fatalf("BuiltIn: %v", err)
	}
	s.adoptFirmware(b)
	return s
}

// Picking a firmware must swap what flashes, and the panel must say so rather
// than keep reporting the built-in one.
func TestFirmwareUploadSwapsTheFlashSource(t *testing.T) {
	s := bundledServer(t)
	blob := testBundleBytes(t)

	code, resp := uploadBundle(t, s, "gorb-os-older.zip", blob)
	if code != http.StatusOK || resp["ok"] != true {
		t.Fatalf("upload rejected: %d %v", code, resp)
	}
	b := s.firmwareBundle()
	if b.Source != "gorb-os-older.zip" {
		t.Errorf("source = %q, want the picked filename", b.Source)
	}
	// The plan the flasher will use must come from the new bundle, not the old one.
	_, plan, err := s.planFor("full")
	if err != nil {
		t.Fatalf("planFor: %v", err)
	}
	if len(plan.Images) == 0 {
		t.Error("plan after upload has no images")
	}
	if b.SHA256 == "" {
		t.Error("uploaded bundle reports no sha256: the UI cannot prove what is loaded")
	}
}

// A rejected pick must leave the built-in firmware exactly where it was. A user
// grabbing the wrong file cannot be able to break the Flash button.
func TestFirmwareUploadRejectsLeavesBuiltIn(t *testing.T) {
	s := bundledServer(t)
	before := s.firmwareBundle()

	for _, bad := range [][]byte{nil, []byte("not a zip at all")} {
		code, resp := uploadBundle(t, s, "wrong.zip", bad)
		if code != http.StatusBadRequest {
			t.Errorf("upload of a bad file: status %d, want 400 (%v)", code, resp)
		}
		if resp["ok"] != false {
			t.Errorf("bad upload reported ok: %v", resp)
		}
	}
	// A valid zip that is not a firmware release.
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	f, _ := zw.Create("readme.txt")
	f.Write([]byte("hello"))
	zw.Close()
	if code, _ := uploadBundle(t, s, "docs.zip", buf.Bytes()); code != http.StatusBadRequest {
		t.Errorf("upload of a non-firmware zip: status %d, want 400", code)
	}

	if got := s.firmwareBundle(); got != before || got.Source != "built-in" {
		t.Errorf("a rejected upload changed the firmware to %q; the built-in must survive", got.Source)
	}
	if _, _, err := s.planFor("full"); err != nil {
		t.Errorf("built-in firmware unusable after a rejected upload: %v", err)
	}
}

// The reset button is the way out of a browsed-in downgrade, so it must restore
// the shipped firmware without a restart.
func TestFirmwareResetReturnsToBuiltIn(t *testing.T) {
	s := bundledServer(t)
	if _, _ = uploadBundle(t, s, "custom.zip", testBundleBytes(t)); s.firmwareBundle().Source != "custom.zip" {
		t.Fatalf("setup: upload did not take")
	}
	rec := httptest.NewRecorder()
	s.handleFirmwareReset(rec, httptest.NewRequest(http.MethodPost, "/api/firmware/reset", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("reset status %d, want 200 (%s)", rec.Code, rec.Body)
	}
	if got := s.firmwareBundle().Source; got != "built-in" {
		t.Errorf("after reset source = %q, want built-in", got)
	}
	getRec := httptest.NewRecorder()
	s.handleFirmwareReset(getRec, httptest.NewRequest(http.MethodGet, "/api/firmware/reset", nil))
	if getRec.Code != http.StatusMethodNotAllowed {
		t.Errorf("GET reset status %d, want 405", getRec.Code)
	}
}

// The plan and the wipe targets must describe one artifact. If a firmware is
// picked between resolving the plan and erasing, erasing from the *new* table
// while writing the old images would wipe the wrong regions.
func TestPlanForReturnsTheBundleItCameFrom(t *testing.T) {
	s := bundledServer(t)
	b, plan, err := s.planFor("full")
	if err != nil {
		t.Fatalf("planFor: %v", err)
	}
	if b == nil {
		t.Fatal("planFor returned no bundle")
	}
	if len(plan.Images) != len(b.Images) {
		t.Fatalf("plan has %d images but bundle %s has %d", len(plan.Images), b.Source, len(b.Images))
	}
	regions, err := esprom.NvsRegions(b.PartitionTable())
	if err != nil {
		t.Fatalf("NvsRegions on the bundle's own table: %v", err)
	}
	if len(regions) == 0 {
		t.Error("no erase targets resolved: a factory reset would silently erase nothing")
	}
}

// An empty upload is a user mistake worth naming, not a zip-parse error.
func TestFirmwareUploadEmptyFileRefused(t *testing.T) {
	s := bundledServer(t)
	code, resp := uploadBundle(t, s, "empty.zip", []byte{})
	if code != http.StatusBadRequest {
		t.Errorf("empty upload status %d, want 400", code)
	}
	if msg, _ := resp["error"].(string); !strings.Contains(msg, "empty") {
		t.Errorf("error = %q, want it to say the file is empty", msg)
	}
}

// The firmware the hub boots with is the one the card must show: a hub that came
// up with nothing compiled in must say so instead of offering a dead button.
func TestHubWithoutFirmwareSaysSo(t *testing.T) {
	s := newHub()
	if _, _, err := s.planFor("full"); err == nil {
		t.Error("planFor with no firmware loaded: want error, got nil")
	}
	payload, err := s.describeFirmware(nil)
	if err != nil {
		t.Fatalf("describeFirmware with no firmware: %v", err)
	}
	keys := jsonKeys(t, payload)
	if keys["present"] != false {
		t.Errorf("present = %v, want false so the card says no firmware", keys["present"])
	}
	if imgs, ok := keys["images"].([]any); !ok || imgs == nil {
		t.Errorf("images = %#v, want an empty array and never null", keys["images"])
	}
}

// testBundleBytes packs a small but complete bundle to upload in a test, built
// through the same packer a release uses.
func testBundleBytes(t *testing.T) []byte {
	t.Helper()
	files := map[string][]byte{
		"bootloader/bootloader.bin":           {0xE9, 0x01, 0x02, 0x22, 0, 0, 0, 0, 'b'},
		"partition_table/partition-table.bin": nil,
		"ota_data_initial.bin":                bytes.Repeat([]byte{0xAA}, 8192),
		"gorb-os.bin":                         {0xE9, 0x01, 0x02, 0x22, 0, 0, 0, 0, 'a'},
		"../assets/clock_assets.bin":          []byte("art"),
	}
	table := make([]byte, 32)
	table[0], table[1], table[2], table[3] = 0xAA, 0x50, 0x01, 0x02
	table[4], table[5], table[6], table[7] = 0x00, 0x90, 0x00, 0x00
	table[8], table[9], table[10], table[11] = 0x00, 0x80, 0x00, 0x00
	copy(table[12:28], "nvs")
	files["partition_table/partition-table.bin"] = table
	manifest := []byte(`{"flash_settings":{"flash_mode":"dio","flash_size":"16MB"},` +
		`"flash_files":{"0x0":"bootloader/bootloader.bin","0x8000":"partition_table/partition-table.bin",` +
		`"0x11000":"ota_data_initial.bin","0x20000":"gorb-os.bin","0x420000":"../assets/clock_assets.bin"}}`)
	blob, err := firmware.WriteBundle(manifest, func(rel string) ([]byte, error) {
		data, ok := files[rel]
		if !ok {
			return nil, fmt.Errorf("no such image %q", rel)
		}
		return data, nil
	})
	if err != nil {
		t.Fatalf("WriteBundle: %v", err)
	}
	return blob
}

// The upload path is a three-way contract: the field name app.js appends, the
// name the handler reads, and the {ok,error} envelope the client checks. Rename
// any one of them and the user sees "still flashing the built-in firmware" with no
// reason, so all three are pinned to each other here.
func TestFirmwareUploadFieldNamesMatchBetweenClientAndHandler(t *testing.T) {
	src := readAppJS(t)

	appForm := regexp.MustCompile(`form\.append\("([a-z]+)",\s*file`)
	handlerRead := regexp.MustCompile(`r\.FormFile\("([a-z]+)"\)`)

	m := appForm.FindStringSubmatch(src)
	if m == nil {
		t.Fatal(`app.js must append the picked file under a named form field (form.append("firmware", file, ...))`)
	}
	clientField := m[1]

	fsrc := readFile(t, "firmware.go")
	h := handlerRead.FindStringSubmatch(fsrc)
	if h == nil {
		t.Fatal(`the hub must read the upload with r.FormFile("...")`)
	}
	if clientField != h[1] {
		t.Errorf(`field mismatch: app.js sends %q, the hub reads %q — the upload will be silently empty`, clientField, h[1])
	}

	// The client's success test is what keeps a 400 from reading as success.
	for _, want := range []string{`res.ok`, `j.ok === false`, `j.error`} {
		if !strings.Contains(src, want) {
			t.Errorf(`the upload path must check %s; without it a refused bundle looks accepted`, want)
		}
	}
	// A refused pick must leave the built-in firmware in place, and say so.
	if !strings.Contains(src, "still flashing the built-in firmware") {
		t.Error(`a refused upload must tell the user the built-in firmware is still what will flash`)
	}
}

// The endpoints the card calls must all exist under the same names, and the
// reset button must reach the handler that restores the shipped bundle.
func TestFirmwareEndpointsExistForTheClientCalls(t *testing.T) {
	src := readAppJS(t)
	mux := readFile(t, "main.go")
	for _, path := range regexp.MustCompile(`fetch\("(/api/[a-z/]+)"`).FindAllStringSubmatch(src, -1) {
		if !strings.Contains(mux, path[1]) {
			t.Errorf("app.js posts to %s but main.go registers no such route", path[1])
		}
	}
	for _, path := range []string{"/api/firmware", "/api/firmware/reset"} {
		if !strings.Contains(mux, path) {
			t.Errorf("the Onboard card needs %s registered", path)
		}
	}
}

// readFile is the sibling of readAppJS for the Go side of a pin.
func readFile(t *testing.T, name string) string {
	t.Helper()
	raw, err := os.ReadFile(name)
	if err != nil {
		t.Fatalf("read %s: %v (the pin is meaningless without it)", name, err)
	}
	return string(raw)
}

// The hub must never fall back to an ESP-IDF build dir. That coupling is what
// this whole change removed, and a regression would put an "install the toolchain"
// error back in front of an end user.
func TestHubHasNoBuildDirDependency(t *testing.T) {
	for _, f := range []string{"main.go", "flash.go", "api.go", "firmware.go"} {
		src := readFile(t, f)
		for _, banned := range []string{"buildDir", "PlanFromBuildDir", "idf.py build", "--build "} {
			if strings.Contains(src, banned) {
				t.Errorf("%s mentions %q — the hub must not depend on a build directory", f, banned)
			}
		}
	}
	// esprom must read no files either: images travel inside the plan.
	for _, f := range []string{"internal/esprom/plan.go", "internal/esprom/flash.go"} {
		src := readFile(t, f)
		for _, banned := range []string{`"os"`, "os.ReadFile", "filepath.Join"} {
			if strings.Contains(src, banned) {
				t.Errorf("%s touches the filesystem via %q — image bytes must come from the bundle", f, banned)
			}
		}
	}
}

// The identity card reads six new fields off the port entries, and the probe
// payload. Same failure mode as the port picker's blank dropdown: a handler that
// renames one of them makes the card silently print "undefined" next to somebody's
// orb. These are pinned to each other on both sides.
func TestIdentityFieldsMatchBetweenClientAndHandler(t *testing.T) {
	src := readAppJS(t)

	// Fields app.js reads from a port entry inside renderIdentity.
	body := blockContaining(t, src, "function renderIdentity")
	for _, f := range []string{"known", "isOrb", "mac", "version", "project", "verdict", "reason", "probedAt", "compileTime", "ip"} {
		if !strings.Contains(body, "chosen."+f) {
			t.Errorf("renderIdentity no longer reads chosen.%s — if that is deliberate, drop it from this list too", f)
		}
	}

	// The hub must actually send them. A cached entry is built with a real probe
	// result so the populated branch, not just the empty one, is exercised.
	s := newHub()
	// Seed a port the host really enumerates: handlePorts renders one entry per
	// live port, so a fabricated name would be correctly dropped and the pin would
	// test nothing. On a hub with no serial ports there is nothing to pin here.
	live, lerr := esprom.ListPorts()
	if lerr != nil || len(live) == 0 {
		t.Skip("no serial ports on this host: nothing to render an identity against")
	}
	probePort := live[0]
	s.identities.done(probePort, esprom.OrbInfo{
		Port: probePort, IsOrb: true, MAC: "44:bd:8d:60:80:88", Project: "gorb-os",
		Version: "8eb4bf0-dirty", CompileTime: "Oct 12 2026 01:58:41",
		CompiledAt: "2026-10-12T01:58:41Z", ElfSHA256: "d5fd8bb9d", IDF: "v5.4",
		IP: "192.168.2.32", Reason: "identified", ProbedAt: "2026-10-12T10:00:00Z",
	})
	rec := httptest.NewRecorder()
	s.handlePorts(rec, httptest.NewRequest(http.MethodGet, "/api/ports", nil))
	var env struct {
		OK   bool             `json:"ok"`
		Data []map[string]any `json:"-"`
		Raw  json.RawMessage  `json:"data"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &env); err != nil {
		t.Fatalf("envelope: %v (%s)", err, rec.Body)
	}
	var payload struct {
		Ports []map[string]any `json:"ports"`
	}
	if err := json.Unmarshal(env.Raw, &payload); err != nil {
		t.Fatalf("data: %v", err)
	}
	// Enumerate whatever this host really has and find the seeded one; the count
	// is the host's business, not the contract's.
	var entry map[string]any
	for _, e := range payload.Ports {
		if e["port"] == probePort {
			entry = e
		}
	}
	if entry == nil {
		t.Fatalf("the cached port is absent from %d entries: the hub dropped a port it knew about", len(payload.Ports))
	}
	for _, f := range []string{"port", "known", "isOrb", "mac", "project", "version", "compileTime", "compiledAt", "elfSha256", "idf", "ip", "reason", "probedAt", "verdict"} {
		if _, ok := entry[f]; !ok {
			t.Errorf("the hub sends no %q but app.js reads chosen.%s", f, f)
		}
	}
	if entry["known"] != true || entry["isOrb"] != true {
		t.Errorf("known/isOrb = %v/%v, want true/true for a cached orb", entry["known"], entry["isOrb"])
	}
	if entry["verdict"] == nil || entry["verdict"] == "" {
		t.Error("no verdict sent: the card cannot tell the user what flashing would do")
	}
}

// blockContaining lifts one function's text out of app.js, so a pin can assert on
// what that function actually reads rather than what the file mentions somewhere.
func blockContaining(t *testing.T, src, marker string) string {
	t.Helper()
	i := strings.Index(src, marker)
	if i < 0 {
		t.Fatalf("app.js has no %q — the identity card was renamed or removed and its pin is now meaningless", marker)
	}
	end := strings.Index(src[i+1:], "\n}\n")
	if end < 0 {
		t.Fatalf("unterminated block at %q", marker)
	}
	return src[i : i+1+end]
}

// Every verdict the hub can send needs wording. An unlisted verdict renders the
// card blank, which is worse than saying "cannot tell".
func TestEveryVerdictHasClientWording(t *testing.T) {
	src := readAppJS(t)
	body := blockContaining(t, src, "function verdictText")
	for _, v := range []string{"upgrade", "downgrade", "same", "same-version"} {
		if !strings.Contains(body, `case "`+v+`"`) {
			t.Errorf("verdictText has no wording for %q; the hub can send it", v)
		}
	}
	if !strings.Contains(body, "default:") {
		t.Error("verdictText needs a default branch: an unknown verdict must say so, not render blank")
	}
	// A downgrade must not be able to look like an upgrade.
	if !strings.Contains(body, "downgrade") || !strings.Contains(strings.ToLower(body), "older") {
		t.Error("the downgrade wording must say the loaded firmware is OLDER, in those terms")
	}
}

// A probe is a reboot. Two things follow, and both are pinned: nothing may call
// it implicitly, and the answer must be invalidated by a flash so a re-probe
// reports the new firmware rather than repeating the old one.
func TestProbeIsNeverImplicitAndIsInvalidatedByFlash(t *testing.T) {
	src := readAppJS(t)
	if n := strings.Count(src, `"/api/ports/probe"`); n != 1 {
		t.Errorf("app.js posts to /api/ports/probe %d times; it must be reached only from the Identify click", n)
	}
	if strings.Contains(src, "probePort()") && !strings.Contains(src, `$("#ob-probe").onclick = probePort`) {
		t.Error("probePort must be wired only to the Identify button")
	}
	if !strings.Contains(src, "btn.disabled = true") {
		t.Error("the Identify button must disable while a probe runs; a double click would reset the chip twice")
	}
	if !strings.Contains(src, "about 10 seconds") {
		t.Error("the card must warn the user the orb restarts and how long it takes")
	}

	flash := readFile(t, "flash.go")
	if !strings.Contains(flash, "s.identities.forget(req.Port)") {
		t.Error("a flash must invalidate the cached identity, or the card keeps reporting the pre-flash version")
	}
}

// The probe must never touch a port the host cannot attribute to the Espressif
// peripheral: EnterDownloadMode toggles DTR/RTS, and doing that to an Arduino on
// the same bench resets a device the user never asked about.
func TestProbeRefusesPortsItCannotAttribute(t *testing.T) {
	// A path no device backs: classifyPort cannot confirm the vendor, so the
	// probe must refuse rather than open it and start pulsing control lines.
	info, err := esprom.ProbeOrb("/dev/serial/by-id/definitely-not-a-real-device")
	if err != nil {
		t.Fatalf("ProbeOrb must report a refusal, not fail: %v", err)
	}
	if info.IsOrb {
		t.Error("a nonexistent device reported as an orb")
	}
	if info.Reason == "" {
		t.Error("refusal must say why, or the UI has nothing to show")
	}
}

// The comparison itself. A git describe has no order, so only the build timestamp
// and the ELF digest may decide what a user is about to do to their orb.
func TestCompareVersions(t *testing.T) {
	base := time.Date(2026, 10, 12, 1, 58, 41, 0, time.UTC)
	older := base.Add(-24 * time.Hour)
	newer := base.Add(24 * time.Hour)
	orb := firmware.Release{Version: "8eb4bf0", CompiledAt: base, ElfSHA256: "d5fd8bb9dc27eb12"}
	for _, tc := range []struct {
		name   string
		target firmware.Release
		want   firmware.Verdict
	}{
		{"identical bytes", firmware.Release{Version: "8eb4bf0", CompiledAt: base, ElfSHA256: "d5fd8bb9dc27ffff"}, firmware.VerdictSame},
		{"target newer", firmware.Release{Version: "fc74aa9", CompiledAt: newer}, firmware.VerdictUpgrade},
		{"target older", firmware.Release{Version: "1111111", CompiledAt: older}, firmware.VerdictDowngrade},
		{"target undated", firmware.Release{Version: "fc74aa9"}, firmware.VerdictUnknown},
		{"orb undated", firmware.Release{Version: "fc74aa9", CompiledAt: newer}, firmware.VerdictUnknown},
		{"same describe, rebuilt", firmware.Release{Version: "8eb4bf0", CompiledAt: newer}, firmware.VerdictSameVersion},
		{"dirty rebuild of same commit", firmware.Release{Version: "8eb4bf0-dirty", CompiledAt: newer}, firmware.VerdictSameVersion},
	} {
		t.Run(tc.name, func(t *testing.T) {
			probeOrb := orb
			if tc.name == "orb undated" {
				probeOrb = firmware.Release{Version: "8eb4bf0", ElfSHA256: ""}
				probeOrb.CompiledAt = time.Time{}
			}
			if got := firmware.Compare(probeOrb, tc.target); got != tc.want {
				t.Errorf("Compare(orb, %s) = %q, want %q", tc.name, got, tc.want)
			}
		})
	}
	// An undated orb against an undated target must be unknown, never "same":
	// claiming equality with no evidence would tell a user to leave a stale orb.
	if got := firmware.Compare(firmware.Release{Version: "a"}, firmware.Release{Version: "b"}); got != firmware.VerdictUnknown {
		t.Errorf("Compare of two undated builds = %q, want unknown", got)
	}
}

// The probe resets the chip, so it must refuse while a flash holds the wire. The
// guard has to read the flag, not the mutex: a flash holds s.mu only long enough
// to set it, so a TryLock-based check passes straight through a live flash and
// would reset a chip mid-write.
func TestProbeRefusesWhileFlashing(t *testing.T) {
	s := newHub()
	s.mu.Lock()
	s.flashing = true
	s.mu.Unlock() // held for the flag update only, exactly as handleFlash does it

	rec := httptest.NewRecorder()
	s.handleProbe(rec, httptest.NewRequest(http.MethodPost, "/api/ports/probe",
		strings.NewReader(`{"port":"/dev/ttyACM0"}`)))
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 while a flash is running", rec.Code)
	}
	var env struct {
		OK    bool   `json:"ok"`
		Error string `json:"error"`
	}
	json.Unmarshal(rec.Body.Bytes(), &env)
	if env.OK || !strings.Contains(env.Error, "flash") {
		t.Errorf("body = %s, want a refusal naming the running flash", rec.Body)
	}

	// And it must not have claimed the port while refusing.
	s.mu.Lock()
	s.flashing = false
	s.mu.Unlock()
}

// A probe takes the same per-port lock a flash does, so two hubs (or a hub and
// orbprobe) cannot reset and write the same wire at once.
func TestProbeTakesThePortLock(t *testing.T) {
	src := readFile(t, "identity.go")
	if !strings.Contains(src, "orbbind.LockPort") {
		t.Error("handleProbe must lock the port: without it a concurrent flash and probe share the wire")
	}
	if !strings.Contains(src, "s.flashRunning()") {
		t.Error(`handleProbe must test the flash flag via flashRunning(); probing the mutex is not the same check`)
	}
}
