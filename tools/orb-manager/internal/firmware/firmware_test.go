package firmware

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// testManifest is the real shape idf.py emits, including the ../assets/ form for
// the clock art: that path leaves the build dir, which is the whole reason a
// bundle has to carry the images rather than point at them.
const testManifest = `{"flash_settings":{"flash_mode":"dio","flash_size":"16MB","flash_freq":"80m"},` +
	`"flash_files":{"0x0":"bootloader/bootloader.bin","0x8000":"partition_table/partition-table.bin",` +
	`"0x11000":"ota_data_initial.bin","0x20000":"gorb-os.bin","0x420000":"../assets/clock_assets.bin"}}`

// espImage builds bytes with a valid ESP image header so the offset-0 validation
// passes, plus an appended esp_app_desc_t for the provenance check.
// espImage builds bytes with a valid ESP image header so the offset-0 validation
// passes, plus an appended esp_app_desc_t for the provenance check.
//
// The header is the real packed shape, all 24 bytes (esp_app_format.h:79-107),
// with the 2-byte LE chip ID at 12-13 carrying ESP_CHIP_ID_ESP32S3 = 9. The
// flash gate reads that field, so a fixture that left it zero would be an image
// for the original ESP32 and would be rightly refused. The descriptor sits
// wherever findAppDesc locates its magic, so lengthening the header cannot move
// a field a test depends on.
func espImage(extra []byte) []byte {
	head := make([]byte, 24)
	head[0], head[1], head[2], head[3] = 0xE9, 0x01, 0x02, 0x22
	head[12], head[13] = 0x09, 0x00
	out := make([]byte, 0, len(head)+len(extra))
	out = append(out, head...)
	return append(out, extra...)
}

func le32(b []byte, v uint32) {
	b[0], b[1], b[2], b[3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24)
}

func makeFiles(t *testing.T) map[string][]byte {
	t.Helper()
	desc := make([]byte, 176)
	desc[0], desc[1], desc[2], desc[3] = 0x32, 0x54, 0xCD, 0xAB
	copy(desc[16:48], "8eb4bf0-dirty")
	copy(desc[48:80], "gorb-os")
	copy(desc[80:96], "01:58:41")
	copy(desc[96:112], "Oct 12 2026")
	copy(desc[112:144], "v5.4")
	app := espImage(append(desc, bytes.Repeat([]byte{0x01}, 4096)...))

	table := make([]byte, 64)
	table[0], table[1], table[2], table[3] = 0xAA, 0x50, 0x01, 0x02
	le32(table[4:], 0x9000)
	le32(table[8:], 0x8000)
	copy(table[12:28], "nvs")

	return map[string][]byte{
		"bootloader/bootloader.bin":           espImage(nil),
		"partition_table/partition-table.bin": table,
		"ota_data_initial.bin":                bytes.Repeat([]byte{0xAA}, 8192),
		"gorb-os.bin":                         app,
		"../assets/clock_assets.bin":          bytes.Repeat([]byte("art"), 512),
	}
}

func pack(t *testing.T, files map[string][]byte, manifest string) []byte {
	t.Helper()
	blob, err := WriteBundle([]byte(manifest), func(rel string) ([]byte, error) {
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

func TestBundleRoundTrip(t *testing.T) {
	files := makeFiles(t)
	blob := pack(t, files, testManifest)
	b, err := Load(blob, "test.zip")
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if len(b.Images) != 5 {
		t.Fatalf("images = %d, want the five the manifest names", len(b.Images))
	}
	// Offsets must arrive low to high, matching esptool's write order.
	for i := 1; i < len(b.Images); i++ {
		if b.Images[i].Offset <= b.Images[i-1].Offset {
			t.Errorf("image %d offset 0x%X does not follow 0x%X", i, b.Images[i].Offset, b.Images[i-1].Offset)
		}
	}
	var art []byte
	for _, im := range b.Images {
		if im.Name == "clock_assets.bin" {
			art = im.Data
		}
	}
	// The image whose manifest path escapes the build dir must still be byte-exact.
	if !bytes.Equal(art, files["../assets/clock_assets.bin"]) {
		t.Error("clock_assets.bin did not survive the bundle intact")
	}
	if b.Project != "gorb-os" || b.Version != "8eb4bf0-dirty" || b.IDFVersion != "v5.4" {
		t.Errorf("provenance = %q/%q/%q, want the values stamped in the app image",
			b.Project, b.Version, b.IDFVersion)
	}
	if !strings.Contains(b.Built, "Oct 12 2026") {
		t.Errorf("built = %q, want the date from the app descriptor", b.Built)
	}
	if len(b.SHA256) != 64 {
		t.Errorf("sha256 = %q, want a full digest", b.SHA256)
	}
}

// TestLoadRefusesIncompleteBundle: a manifest naming an image the archive lacks
// must fail at load. Half a firmware is worse than none — it would flash and
// leave an orb that will not boot.
func TestLoadRefusesIncompleteBundle(t *testing.T) {
	blob := dropEntry(t, pack(t, makeFiles(t), testManifest), "images/gorb-os.bin")
	if _, err := Load(blob, "broken.zip"); err == nil {
		t.Fatal("Load on a bundle missing gorb-os.bin: want error, got nil")
	} else if !strings.Contains(err.Error(), "missing") {
		t.Errorf("error = %v, want it to name the missing image", err)
	}
}

// dropEntry rebuilds a bundle without one entry, so the loader's own missing-image
// guard is reached rather than the packer's earlier one.
func dropEntry(t *testing.T, blob []byte, gone string) []byte {
	t.Helper()
	zr, err := zip.NewReader(bytes.NewReader(blob), int64(len(blob)))
	if err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	zw := zip.NewWriter(&out)
	found := false
	for _, f := range zr.File {
		if f.Name == gone {
			found = true
			continue
		}
		rc, err := f.Open()
		if err != nil {
			t.Fatal(err)
		}
		w, err := zw.Create(f.Name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := io.Copy(w, rc); err != nil {
			t.Fatal(err)
		}
		rc.Close()
	}
	if !found {
		t.Fatalf("fixture bundle has no %q to drop", gone)
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return out.Bytes()
}

func TestLoadRefusesNonBundle(t *testing.T) {
	if _, err := Load([]byte("just some bytes, not a zip"), "notes.txt"); err == nil {
		t.Error("Load on a text file: want error, got nil")
	}
	// A valid zip with no manifest is the likely mistake: an unrelated archive.
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	f, _ := zw.Create("readme.md")
	f.Write([]byte("hello"))
	zw.Close()
	if _, err := Load(buf.Bytes(), "other.zip"); err == nil || !strings.Contains(err.Error(), ManifestName) {
		t.Errorf("Load(zip without manifest) = %v, want it to name %s", err, ManifestName)
	}
}

// TestBundleAppPlan: picking a firmware must leave app-only mode usable, since
// that is what a bench reflash reaches for, and it must come from the same
// bundle so both modes share one provenance.
func TestBundleAppPlan(t *testing.T) {
	b, err := Load(pack(t, makeFiles(t), testManifest), "test.zip")
	if err != nil {
		t.Fatal(err)
	}
	plan, err := b.AppPlan()
	if err != nil {
		t.Fatalf("AppPlan: %v", err)
	}
	if len(plan.Images) != 2 {
		t.Fatalf("app plan has %d images, want the app slot plus otadata", len(plan.Images))
	}
	if plan.Images[0].Name != "gorb-os.bin" || plan.Images[0].Offset != 0x20000 {
		t.Errorf("app image = %s @0x%X, want gorb-os.bin @0x20000", plan.Images[0].Name, plan.Images[0].Offset)
	}
	if len(b.PartitionTable()) == 0 {
		t.Error("PartitionTable empty: the factory reset has nothing to resolve erase targets from")
	}
}

func TestAppPlanWithoutAppImage(t *testing.T) {
	files := makeFiles(t)
	delete(files, "gorb-os.bin")
	manifest := `{"flash_settings":{"flash_mode":"dio","flash_size":"16MB"},` +
		`"flash_files":{"0x0":"bootloader/bootloader.bin"}}`
	b, err := Load(pack(t, files, manifest), "noapp.zip")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := b.AppPlan(); err == nil {
		t.Error("AppPlan on a bundle with no app image: want error, got nil")
	}
}

// TestBundleCarriesManifestVerbatim: the manifest is authority. A packer that
// rewrote it would silently relocate an image.
func TestBundleCarriesManifestVerbatim(t *testing.T) {
	blob := pack(t, makeFiles(t), testManifest)
	zr, err := zip.NewReader(bytes.NewReader(blob), int64(len(blob)))
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range zr.File {
		if f.Name != ManifestName {
			continue
		}
		rc, _ := f.Open()
		raw, _ := io.ReadAll(rc)
		rc.Close()
		if string(raw) != testManifest {
			t.Errorf("the bundle rewrote the manifest; it must carry idf.py's bytes unchanged\ngot:  %s\nwant: %s", raw, testManifest)
		}
		var fa struct {
			FlashFiles map[string]string `json:"flash_files"`
		}
		if err := json.Unmarshal(raw, &fa); err != nil {
			t.Fatalf("manifest in bundle does not parse: %v", err)
		}
		if len(fa.FlashFiles) != 5 {
			t.Errorf("manifest lists %d images, want 5", len(fa.FlashFiles))
		}
		return
	}
	t.Fatal("no manifest entry in the bundle")
}

func TestBundleKeyFlattensSafely(t *testing.T) {
	for rel, want := range map[string]string{
		"bootloader/bootloader.bin":  "images/bootloader/bootloader.bin",
		"../assets/clock_assets.bin": "images/assets/clock_assets.bin",
		"..\\..\\etc\\passwd":        "images/etc/passwd",
		"./gorb-os.bin":              "images/gorb-os.bin",
	} {
		if got := bundleKey(rel); got != want {
			t.Errorf("bundleKey(%q) = %q, want %q", rel, got, want)
		}
	}
}

// TestWriteBundleFromBuildDir exercises the one code path allowed to consult OS
// paths: the release packer resolving a real build dir, ../assets/ and all.
func TestWriteBundleFromBuildDir(t *testing.T) {
	root := t.TempDir()
	build := filepath.Join(root, "build")
	files := map[string]bool{
		filepath.Join(build, "bootloader", "bootloader.bin"):           false,
		filepath.Join(build, "partition_table", "partition-table.bin"): false,
		filepath.Join(build, "ota_data_initial.bin"):                   false,
		filepath.Join(build, "gorb-os.bin"):                            false,
		filepath.Join(root, "assets", "clock_assets.bin"):              false,
	}
	for p := range files {
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, espImage([]byte("body")), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	blob, err := WriteBundle([]byte(testManifest), func(rel string) ([]byte, error) {
		return os.ReadFile(filepath.Join(build, rel))
	})
	if err != nil {
		t.Fatalf("WriteBundle: %v", err)
	}
	b, err := Load(blob, "from-build.zip")
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if len(b.Images) != 5 {
		t.Fatalf("images = %d, want 5", len(b.Images))
	}
	// No entry may keep a traversal: it would extract outside its directory.
	zr, err := zip.NewReader(bytes.NewReader(blob), int64(len(blob)))
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range zr.File {
		if strings.Contains(f.Name, "..") {
			t.Errorf("bundle entry %q keeps a traversal", f.Name)
		}
	}
}

// TestBuiltInBundleIsUsable: this bundle is what an end user flashes with no
// choices made, so it must exist, parse, and carry the real image set.
func TestBuiltInBundleIsUsable(t *testing.T) {
	b, err := BuiltIn()
	if err != nil {
		t.Fatalf("BuiltIn: %v — internal/firmware/bundle/%s must be committed", err, DefaultName)
	}
	if len(b.Images) != 4 {
		t.Errorf("built-in bundle has %d images, want the four D36 leaves in the set (D36 took the art bank out)", len(b.Images))
	}
	if b.Version == "" {
		t.Error("built-in bundle reports no version: the UI would flash an orb of unknown provenance")
	}
	if len(b.PartitionTable()) == 0 {
		t.Error("built-in bundle carries no partition table: factory reset cannot resolve erase targets")
	}
	if _, err := b.AppPlan(); err != nil {
		t.Errorf("built-in bundle cannot answer AppPlan: %v", err)
	}
}

// TestBuiltInBundleMatchesBuildDirWhenPresent is the staleness guard: when a real
// build is at hand, the committed bundle must describe exactly those bytes.
func TestBuiltInBundleMatchesBuildDirWhenPresent(t *testing.T) {
	dir := os.Getenv("ORB_BUILD_DIR")
	if dir == "" {
		t.Skip("ORB_BUILD_DIR not set")
	}
	raw, err := os.ReadFile(filepath.Join(dir, ManifestName))
	if err != nil {
		t.Skipf("no %s under %s: %v", ManifestName, dir, err)
	}
	blob, err := WriteBundle(raw, func(rel string) ([]byte, error) {
		return os.ReadFile(filepath.Join(dir, rel))
	})
	if err != nil {
		t.Fatalf("pack from build dir: %v", err)
	}
	onDisk, err := Load(blob, "on-disk")
	if err != nil {
		t.Fatal(err)
	}
	built, err := BuiltIn()
	if err != nil {
		t.Fatal(err)
	}
	if len(built.Images) != len(onDisk.Images) {
		t.Errorf("built-in has %d images, the build dir has %d — rerun make firmware",
			len(built.Images), len(onDisk.Images))
	}
	for i := range built.Images {
		a, c := built.Images[i], onDisk.Images[i]
		if a.Offset != c.Offset || a.Name != c.Name || !bytes.Equal(a.Data, c.Data) {
			t.Errorf("image %d differs (built-in %s@0x%X vs build %s@0x%X) — rerun make firmware",
				i, a.Name, a.Offset, c.Name, c.Offset)
		}
	}
}
