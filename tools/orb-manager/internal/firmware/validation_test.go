package firmware

import (
	"archive/zip"
	"bytes"
	"fmt"
	"strings"
	"testing"
)

// A bundle is a file the operator picks out of a directory, and Load is the only
// thing standing between it and the wire. These pin the three limits that make
// "the parser read it" mean something: total expansion, member count, and the
// project the image itself claims to be.

// rawZip writes entries verbatim, so a test can build an archive WriteBundle would
// never produce — which is exactly the input these guards exist for.
func rawZip(t *testing.T, entries map[string][]byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for name, data := range entries {
		w, err := zw.Create(name)
		if err != nil {
			t.Fatalf("create %s: %v", name, err)
		}
		if _, err := w.Write(data); err != nil {
			t.Fatalf("write %s: %v", name, err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// baseEntries is a member set the manifest is happy with, so a refusal below comes
// from the guard under test and not from a missing image further down.
func baseEntries() map[string][]byte {
	return map[string][]byte{
		ManifestName:                                 []byte(testManifest),
		"images/gorb-os.bin":                         espImage(nil),
		"images/bootloader/bootloader.bin":           espImage(nil),
		"images/partition_table/partition-table.bin": espImage(nil),
		"images/ota_data_initial.bin":                bytes.Repeat([]byte{0xAA}, 8192),
		"images/assets/clock_assets.bin":             []byte("art"),
	}
}

// A zip bomb is small on disk and enormous once inflated. Each member was capped
// before, and that was the wrong number: the members are summed into a map, so the
// aggregate is what the hub actually holds.
func TestLoadRefusesOversizedExpansion(t *testing.T) {
	entries := baseEntries()
	// One byte past the allowance, so this exercises the ceiling itself rather than
	// a bundle that merely happens to be large. Highly compressible, so the
	// compressed fixture stays tiny and the test still proves the cap is on the
	// INFLATED size, which is the number that costs the hub anything.
	big := bytes.Repeat([]byte("A"), maxExpanded+1)
	entries["images/gorb-os.bin"] = big
	blob := rawZip(t, entries)
	if len(blob) > maxBundle {
		t.Fatalf("the fixture is no longer a bomb: %d B compressed", len(blob))
	}
	if _, err := Load(blob, "bomb.zip"); err == nil {
		t.Fatal("a bundle expanding past the allowance was accepted")
	} else if !strings.Contains(err.Error(), "expand") {
		t.Errorf("error = %v, want it to name the expansion limit", err)
	}
}

// The budget applies across many members too, not only to one big one.
func TestLoadRefusesAggregateExpansion(t *testing.T) {
	entries := baseEntries()
	piece := maxExpanded / 2
	for i := 0; i < 4; i++ {
		entries[fmt.Sprintf("images/pad%d.bin", i)] = bytes.Repeat([]byte("B"), piece)
	}
	if _, err := Load(rawZip(t, entries), "wide.zip"); err == nil {
		t.Fatal("members totalling past the allowance were accepted")
	} else if !strings.Contains(err.Error(), "expand") {
		t.Errorf("error = %v, want it to name the expansion limit", err)
	}
}

// An archive whose bulk is thousands of files is not a firmware release, and the
// per-entry cap alone would wave it through at one byte each.
func TestLoadRefusesTooManyEntries(t *testing.T) {
	entries := baseEntries()
	for i := 0; i < maxZipEntries+8; i++ {
		entries[fmt.Sprintf("images/junk%03d.bin", i)] = []byte{byte(i)}
	}
	if _, err := Load(rawZip(t, entries), "many.zip"); err == nil {
		t.Fatal("an archive with more members than a release can have was accepted")
	} else if !strings.Contains(err.Error(), "files") {
		t.Errorf("error = %v, want it to name the member count", err)
	}
}

// withDescriptor rewrites one esp_app_desc_t field in the app image, padded to the
// field's full 32 bytes so no bytes from the previous value survive to confuse an
// assertion. off is the field's offset inside the descriptor.
func withDescriptor(t *testing.T, files map[string][]byte, off int, value string) {
	t.Helper()
	app := append([]byte{}, files["gorb-os.bin"]...)
	i := bytes.Index(app, []byte{0x32, 0x54, 0xCD, 0xAB})
	if i < 0 {
		t.Fatal("fixture lost its descriptor")
	}
	if i+off+32 > len(app) {
		t.Fatalf("descriptor field at %d+32 runs past the image", off)
	}
	var field [32]byte
	copy(field[:], value)
	copy(app[i+off:], field[:])
	files["gorb-os.bin"] = app
}

// The descriptor inside the app image says what project the bytes are. A stray
// ESP-IDF build out of a Downloads folder announces itself, and flashing it would
// leave an orb booting someone else's firmware with a gorb-os filename on it.
func TestLoadRefusesAForeignProject(t *testing.T) {
	files := makeFiles(t)
	withDescriptor(t, files, 48, "hello_world")
	if _, err := Load(pack(t, files, testManifest), "other.zip"); err == nil {
		t.Fatal("a bundle announcing another project was accepted")
	} else if !strings.Contains(err.Error(), "hello_world") {
		t.Errorf("error = %v, want it to name the project that was found", err)
	}
}

// The real project must load, and an image with NO descriptor must still load: the
// built-in bundle has a descriptor, a hand-packed image legitimately may not, and the
// UI says "unknown firmware" rather than the hub refusing bytes it can flash
// faithfully.
func TestLoadAcceptsGorbOSAndUndescribed(t *testing.T) {
	if _, err := Load(pack(t, makeFiles(t), testManifest), "ok.zip"); err != nil {
		t.Fatalf("the good bundle was refused: %v", err)
	}
	files := makeFiles(t)
	files["gorb-os.bin"] = espImage(nil)
	b, err := Load(pack(t, files, testManifest), "nodesc.zip")
	if err != nil {
		t.Fatalf("a descriptor-less bundle was refused: %v", err)
	}
	if b.Project != "" {
		t.Errorf("Project = %q, want empty so the UI can say unknown", b.Project)
	}
}

// Descriptor fields are attacker-supplied bytes that end up rendered. Dropping
// control characters where they are parsed means no renderer has to remember to,
// and a future field that forgets to escape still cannot smuggle an escape
// sequence into a label.
func TestDescriptorFieldsDropControlCharacters(t *testing.T) {
	files := makeFiles(t)
	withDescriptor(t, files, 16, "a\x00b\x1b[2J\x07c\n")

	b, err := Load(pack(t, files, testManifest), "ctrl.zip")
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	if strings.ContainsAny(b.Version, "\x00\x1b\x07\n\r\t") {
		t.Errorf("Version %q still carries control characters", b.Version)
	}
	// "[2J" survives on purpose: it is printable text, and stripping markup is the
	// client's escaping job. What the parser guarantees is that no control character
	// — the NUL that truncates a C string, the ESC that starts a terminal sequence —
	// leaves the bundle at all.
	if b.Version != "ab[2Jc" {
		t.Errorf("Version = %q, want the printable characters of the planted value", b.Version)
	}
}

// The chip gate, at the one place a wrong file actually arrives: the bundle
// dialog. A build for another Espressif chip writes cleanly at its offset and
// boots as nothing, with no complaint from the ROM, so refusing it has to happen
// while the operator still has a Flash button to leave disabled.
func TestLoadRefusesAnImageForAnotherChip(t *testing.T) {
	files := makeFiles(t)
	app := append([]byte{}, files["gorb-os.bin"]...)
	app[12], app[13] = 0x0D, 0x00 // ESP_CHIP_ID_ESP32C6
	files["gorb-os.bin"] = app

	_, err := Load(pack(t, files, testManifest), "c6.zip")
	if err == nil {
		t.Fatal("a bundle holding an ESP32-C6 app image was accepted")
	}
	if !strings.Contains(err.Error(), "ESP32-C6") {
		t.Errorf("error = %v, want it to name the chip that was found", err)
	}
	if !strings.Contains(err.Error(), "gorb-os.bin") {
		t.Errorf("error = %v, want it to name the offending image", err)
	}
}

// The gate must not become a demand that every member be an app image: the real
// manifest's partition table and otadata carry no 0xE9 header at all, and
// refusing them would disable flashing entirely.
func TestLoadAcceptsHeaderlessManifestMembers(t *testing.T) {
	b, err := Load(pack(t, makeFiles(t), testManifest), "good.zip")
	if err != nil {
		t.Fatalf("the good bundle was refused: %v", err)
	}
	for _, name := range []string{"partition-table.bin", "ota_data_initial.bin"} {
		if b.imageNamed(name) == nil {
			t.Errorf("%s missing from the loaded plan", name)
		}
	}
}

// The bootloader is checked too, not just the app: it is the image that decides
// whether anything boots, and a wrong-chip bootloader is the one mismatch an
// operator cannot recover from by reflashing the app.
func TestLoadRefusesAWrongChipBootloader(t *testing.T) {
	files := makeFiles(t)
	boot := append([]byte{}, files["bootloader/bootloader.bin"]...)
	boot[12], boot[13] = 0x00, 0x00 // ESP_CHIP_ID_ESP32
	files["bootloader/bootloader.bin"] = boot

	if _, err := Load(pack(t, files, testManifest), "esp32-bootloader.zip"); err == nil {
		t.Fatal("a bundle holding an ESP32 bootloader was accepted")
	}
}
