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
	s := &Server{}
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
	s := &Server{}
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
	s := &Server{}
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
