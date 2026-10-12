package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
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

	s := &Server{buildDir: dir}
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
		t.Fatalf("present = false, want the manifest found at %s", dir)
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
