// Package firmware owns what gets flashed. The hub is a standalone app an end
// user runs: it never looks for an ESP-IDF build directory, because the person
// holding the cable does not have one. Firmware travels in a bundle — a zip of
// the exact bytes idf.py produced plus the flasher_args.json that names their
// offsets — embedded into the binary at release time, and loadable from a file
// the user picks when they want something else.
//
// Keeping the manifest beside the images is the point. Offsets, flash mode, and
// flash size are still derived from the build and never hardcoded; they simply
// arrive with the firmware they describe, so a plan cannot mix one build's
// offsets with another build's bytes.
package firmware

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	_ "embed"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"path"
	"sort"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// ManifestName is the file a bundle must carry: idf.py's own record of which
// image goes where.
const ManifestName = "flasher_args.json"

// ImageDir is the directory a bundle keeps the build images under. The release
// step flattens build-relative paths into it, so the ../assets/ spelling idf.py
// uses for the clock art never leaks into an archive as a traversal.
const ImageDir = "images"

// maxBundle caps a bundle so a stray pick of a huge file cannot exhaust the
// hub's memory. A real one is ~1.2 MB deflated.
const maxBundle = 32 << 20

// MaxBytes is that cap, exported so the HTTP handler bounds the upload the same
// way the parser does — one number, checked in one place, no way for them to
// disagree.
func MaxBytes() int64 { return maxBundle }

// Bundle is one loadable firmware release: a plan plus the provenance the UI
// shows, so the user can tell built-in from browsed-in and old from new.
type Bundle struct {
	// Source says where the bytes came from: "built-in" or the picked filename.
	Source string
	// Project, Version, IDFVersion and Built come from the app image's own
	// esp_app_desc_t, so they describe what is really inside rather than what a
	// filename claims.
	Project, Version, IDFVersion, Built string
	// SHA256 fingerprints the whole bundle — the thing to quote when a build
	// behaves differently than expected.
	SHA256 string
	// Images is the full-device image set, ordered low offset to high.
	Images []esprom.Image

	plan      esprom.FlashPlan
	partTable []byte
}

// Plan returns the full-device plan: every image the manifest lists.
func (b *Bundle) Plan() esprom.FlashPlan { return b.plan }

// AppPlan returns the bench reflash — the app slot plus the otadata pointing the
// bootloader at it — drawn from the same bundle, so both modes share one
// provenance and one set of bytes.
func (b *Bundle) AppPlan() (esprom.FlashPlan, error) {
	app := b.imageNamed("gorb-os.bin")
	if app == nil {
		return esprom.FlashPlan{}, fmt.Errorf("firmware %s has no gorb-os.bin to flash", b.Source)
	}
	var ota []byte
	if im := b.imageNamed("ota_data_initial.bin"); im != nil {
		ota = im.Data
	}
	return esprom.AppOnlyPlan("gorb-os.bin", app.Data, ota), nil
}

// PartitionTable returns the bytes the plan writes at 0x8000. The factory reset
// takes its erase targets from these, which is what keeps a wipe aimed at the
// layout actually being flashed.
func (b *Bundle) PartitionTable() []byte { return b.partTable }

// Load parses bundle bytes — the zip a release step produces, or one the user
// picked in a file dialog. Every image the manifest names must be present, so a
// partial or hand-edited bundle fails here rather than half-way into a flash.
func Load(raw []byte, source string) (*Bundle, error) {
	if len(raw) > maxBundle {
		return nil, fmt.Errorf("firmware file too large (%d MB; the limit is %d MB)", len(raw)>>20, maxBundle>>20)
	}
	blob, err := zip.NewReader(bytes.NewReader(raw), int64(len(raw)))
	if err != nil {
		return nil, fmt.Errorf("not a firmware bundle (%s): %w", source, err)
	}

	files := map[string][]byte{}
	for _, f := range blob.File {
		if f.FileInfo().IsDir() {
			continue
		}
		name := path.Clean("/" + f.Name)
		name = strings.TrimPrefix(name, "/")
		data, err := readZipFile(f)
		if err != nil {
			return nil, fmt.Errorf("read %s from bundle: %w", name, err)
		}
		files[name] = data
	}
	manifest, ok := files[ManifestName]
	if !ok {
		return nil, fmt.Errorf("firmware bundle %s has no %s, so it is not a gorb-os firmware release", source, ManifestName)
	}

	open := func(rel string) ([]byte, error) {
		for _, key := range []string{bundleKey(rel), path.Clean(strings.ReplaceAll(rel, "\\", "/"))} {
			if data, ok := files[key]; ok {
				return data, nil
			}
		}
		return nil, fmt.Errorf("named in %s but missing from the bundle", ManifestName)
	}
	plan, err := esprom.PlanFromManifest(manifest, open)
	if err != nil {
		return nil, fmt.Errorf("firmware bundle %s: %w", source, err)
	}

	b := &Bundle{Source: source, Images: plan.Images, plan: plan, SHA256: digestOf(raw)}
	if app := b.imageNamed("gorb-os.bin"); app != nil {
		b.Project, b.Version, b.IDFVersion, b.Built = appDescriptor(app.Data)
	}
	if pt := b.imageNamed("partition-table.bin"); pt != nil {
		b.partTable = pt.Data
	}
	return b, nil
}

// bundleKey maps a manifest-relative path to its name inside the bundle.
// Cleaning first is what makes ../assets/clock_assets.bin safe to carry: the
// traversal resolves against the build dir it came from, and the leading ../ is
// dropped so every image lands under ImageDir and nothing escapes the archive.
func bundleKey(rel string) string {
	clean := path.Clean(strings.ReplaceAll(rel, "\\", "/"))
	clean = strings.TrimPrefix(clean, "/") // an absolute path written as /etc/passwd
	for strings.HasPrefix(clean, "../") {  // repeat: one strip leaves ../../ a step short
		clean = clean[len("../"):]
	}
	clean = strings.TrimPrefix(clean, "./")
	if clean == "" || clean == "." {
		clean = "image.bin"
	}
	return ImageDir + "/" + clean
}

func readZipFile(f *zip.File) ([]byte, error) {
	rc, err := f.Open()
	if err != nil {
		return nil, err
	}
	defer rc.Close()
	return io.ReadAll(io.LimitReader(rc, maxBundle))
}

func digestOf(b []byte) string {
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

// esp_app_desc_t's magic, and the field offsets after it: version[32] at +16,
// project_name[32] at +48, time[16] at +80, date[16] at +96, idf_ver[32] at +112.
const (
	appDescMagic    = 0xABCD5432
	appDescFieldEnd = 144
)

// appDescriptor reads the version ESP-IDF stamps into every app image. Because
// it comes out of the bytes themselves, the version the UI shows is honest even
// when the user picked the file by hand and the filename says nothing.
func appDescriptor(app []byte) (project, version, idf, built string) {
	want := make([]byte, 4)
	binary.LittleEndian.PutUint32(want, appDescMagic)
	i := bytes.Index(app, want)
	if i < 0 || i+appDescFieldEnd+32 > len(app) {
		return "", "", "", ""
	}
	d := app[i:]
	field := func(off, n int) string { return strings.TrimRight(string(d[off:off+n]), "\x00") }
	built = strings.TrimSpace(field(96, 16) + " " + field(80, 16))
	return field(48, 32), field(16, 32), field(112, 32), built
}

func (b *Bundle) imageNamed(name string) *esprom.Image {
	for i := range b.Images {
		if b.Images[i].Name == name {
			return &b.Images[i]
		}
	}
	return nil
}

// WriteBundle packs a build's manifest and the images it names into the bundle
// format. It is the release step's whole job, and the only place in this program
// that consults OS paths: once the bytes are in a bundle they are self-describing.
func WriteBundle(manifest []byte, open esprom.OpenImage) ([]byte, error) {
	var fa struct {
		FlashFiles map[string]string `json:"flash_files"`
	}
	if err := json.Unmarshal(manifest, &fa); err != nil {
		return nil, fmt.Errorf("parse %s: %w", ManifestName, err)
	}
	if len(fa.FlashFiles) == 0 {
		return nil, fmt.Errorf("%s lists no images to flash", ManifestName)
	}
	var rels []string
	for _, rel := range fa.FlashFiles {
		rels = append(rels, rel)
	}
	sort.Strings(rels)

	var out bytes.Buffer
	cw := zip.NewWriter(&out)
	if err := putFile(cw, ManifestName, manifest); err != nil {
		return nil, err
	}
	seen := map[string]bool{}
	for _, rel := range rels {
		key := bundleKey(rel)
		if seen[key] {
			continue
		}
		seen[key] = true
		data, err := open(rel)
		if err != nil {
			return nil, fmt.Errorf("image %q: %w", rel, err)
		}
		if err := putFile(cw, key, data); err != nil {
			return nil, err
		}
	}
	if err := cw.Close(); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

func putFile(w *zip.Writer, name string, data []byte) error {
	f, err := w.Create(name)
	if err != nil {
		return err
	}
	_, err = f.Write(data)
	return err
}

// DefaultName is the bundle compiled into the binary. `make firmware` writes it
// from the current build; it is committed, so a plain `go build` needs neither
// ESP-IDF nor the build dir.
const DefaultName = "orb-firmware.zip"

//go:embed bundle/orb-firmware.zip
var embedded []byte

// BuiltIn returns the firmware the binary shipped with — what an end user flashes
// without touching a file dialog, and what "use the built-in firmware" returns to.
func BuiltIn() (*Bundle, error) {
	if len(embedded) == 0 {
		return nil, fmt.Errorf("no firmware is compiled into this build of the hub")
	}
	return Load(embedded, "built-in")
}
