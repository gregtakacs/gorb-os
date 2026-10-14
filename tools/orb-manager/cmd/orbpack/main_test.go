package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The release packer is the last place a version can still be decided, so it is the
// right place to refuse one. An end user flashes the built-in bundle with no choices
// made: shipping an unordered stamp would put the hub back to guessing upgrades from
// build timestamps, which is the defect the version scheme kills.

// orbpackManifest is idf.py's own shape, ../assets/ spelling and all — the form the
// packer must resolve against the build dir.
const orbpackManifest = `{"flash_settings":{"flash_mode":"dio","flash_size":"16MB"},` +
	`"flash_files":{"0x0":"bootloader/bootloader.bin","0x8000":"partition_table/partition-table.bin",` +
	`"0x11000":"ota_data_initial.bin","0x20000":"gorb-os.bin","0x420000":"../assets/clock_assets.bin"}}`

// appImageWithVersion builds an app image whose esp_app_desc_t declares version, so
// the packer sees exactly what a real build would report. The 24-byte header carries
// the ESP32-S3 chip ID at 12-13, because the hub's chip gate reads it and refuses an
// image that leaves it zero.
func appImageWithVersion(t *testing.T, version string) []byte {
	t.Helper()
	head := make([]byte, 24)
	head[0], head[1], head[2], head[3] = 0xE9, 0x01, 0x02, 0x22
	head[12], head[13] = 0x09, 0x00

	desc := make([]byte, 176)
	binary.LittleEndian.PutUint32(desc[0:4], 0xABCD5432)
	copy(desc[16:48], version)
	copy(desc[48:80], "gorb-os")
	copy(desc[80:96], "01:58:41")
	copy(desc[96:112], "Oct 12 2026")
	copy(desc[112:144], "v5.4")

	app := append(append([]byte{}, head...), desc...)
	return append(app, make([]byte, 512)...)
}

// writeBuildDir lays down the five images the manifest names, with the app image
// declaring the version under test.
func writeBuildDir(t *testing.T, version string) string {
	t.Helper()
	root := t.TempDir()
	build := filepath.Join(root, "build")
	table := make([]byte, 64)
	table[0], table[1], table[2], table[3] = 0xAA, 0x50, 0x01, 0x02
	binary.LittleEndian.PutUint32(table[4:], 0x9000)
	binary.LittleEndian.PutUint32(table[8:], 0x8000)
	copy(table[12:28], "nvs")
	boot := make([]byte, 24)
	boot[0], boot[1], boot[2], boot[3] = 0xE9, 0x01, 0x02, 0x22
	boot[12], boot[13] = 0x09, 0x00

	files := map[string][]byte{
		filepath.Join(build, "flasher_args.json"):                      []byte(orbpackManifest),
		filepath.Join(build, "bootloader", "bootloader.bin"):           boot,
		filepath.Join(build, "partition_table", "partition-table.bin"): table,
		filepath.Join(build, "ota_data_initial.bin"):                   bytes8k(),
		filepath.Join(build, "gorb-os.bin"):                            appImageWithVersion(t, version),
		filepath.Join(root, "assets", "clock_assets.bin"):              []byte("art"),
	}
	for p, data := range files {
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return build
}

func bytes8k() []byte { return make([]byte, 8192) }

// TestOrbpackRefusesAnUnversionedBundle drives the release step's exit code, which
// is what `make firmware` and the shell both act on.
func TestOrbpackRefusesAnUnversionedBundle(t *testing.T) {
	out := filepath.Join(t.TempDir(), "orb-firmware.zip")

	// A pre-scheme stamp — a bare git describe — is refused outright.
	if rc := pack(writeBuildDir(t, "0fa0ba5-dirty"), out, false); rc != 1 {
		t.Errorf("pack of an unversioned build returned %d, want 1", rc)
	}

	// -force is the bench escape hatch, and it must still produce a loadable bundle.
	if rc := pack(writeBuildDir(t, "0fa0ba5-dirty"), out, true); rc != 0 {
		t.Errorf("pack of an unversioned build with -force returned %d, want 0", rc)
	}

	// A declared version ships with no flag at all.
	if rc := pack(writeBuildDir(t, "v1.0.0+e1a2b3c"), out, false); rc != 0 {
		t.Errorf("pack of a versioned build returned %d, want 0", rc)
	}
}

// The refusal has to say what to do next, because the person hitting it is mid-
// release and the flag is not the answer they want. Pinned on the source rather than
// a captured pipe, which is how this suite reads a handler's contract elsewhere.
func TestOrbpackRefusalNamesTheFix(t *testing.T) {
	src, err := os.ReadFile("main.go")
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"refusing to ship", "Write version.txt, reconfigure, rebuild", "-force overrides this for a bench build"} {
		if !strings.Contains(string(src), want) {
			t.Errorf("the refusal no longer says %q", want)
		}
	}
}
