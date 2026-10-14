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
	"time"

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

// maxZipEntries bounds how many members a bundle may carry. A real release is the
// manifest plus five images; the ceiling leaves room for a future partition and
// still refuses an archive whose bulk is thousands of files rather than firmware.
const maxZipEntries = 32

// maxExpanded caps the DECOMPRESSED total, which is the number a zip bomb is built
// out of. Capping each entry was not the same thing: the entries are summed into a
// map, so an archive of many high-ratio members expanded to entries x maxBundle and
// the hub held all of it. 48 MB is ~18x the largest bundle anyone ships.
const maxExpanded = 48 << 20

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
	// The aggregate is counted while reading, not after: the point of the cap is
	// to stop before the expansion is resident. Entry count is capped too, so the
	// work of a hostile archive is bounded by both its fan-out and its volume.
	var expanded int64
	members := 0
	for _, f := range blob.File {
		if f.FileInfo().IsDir() {
			continue
		}
		members++
		if members > maxZipEntries {
			return nil, fmt.Errorf("firmware bundle %s has %d files; a gorb-os release has the manifest and its images, not %d",
				source, members, maxZipEntries)
		}
		name := path.Clean("/" + f.Name)
		name = strings.TrimPrefix(name, "/")
		if uint64(f.UncompressedSize64) > uint64(maxExpanded-expanded) {
			return nil, fmt.Errorf("firmware bundle %s expands beyond the %d MB a bundle is allowed to be", source, maxExpanded>>20)
		}
		data, err := readZipFile(f, maxExpanded-expanded)
		if err != nil {
			return nil, fmt.Errorf("read %s from bundle: %w", name, err)
		}
		expanded += int64(len(data))
		if expanded > maxExpanded {
			return nil, fmt.Errorf("firmware bundle %s expands beyond the %d MB a bundle is allowed to be", source, maxExpanded>>20)
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
	// A bundle whose app image announces a different project is not this product's
	// firmware, however well it is zipped — the ordinary cause is picking an
	// unrelated ESP-IDF build out of a Downloads folder. Refusing it here is the
	// difference between a wrong file disabling the Flash button and a wrong file
	// sitting at 0x20000 when the orb next boots. An ABSENT descriptor is not
	// refused: synthetic and third-party-packaged images legitimately have none, and
	// the UI says "unknown firmware" rather than inventing a project.
	if b.Project != "" && !strings.EqualFold(b.Project, "gorb-os") {
		return nil, fmt.Errorf("firmware bundle %s is the project %q, not gorb-os; the hub only flashes gorb-os releases", source, b.Project)
	}
	// Same gate the flasher applies, applied here: a bundle holding an image for
	// another Espressif chip should disable the Flash button, not start a flash
	// that ends with an orb whose sectors are written and whose app cannot boot.
	// esprom.CheckPlanChipIDs skips the headerless members (partition table,
	// otadata, assets), so this is not a demand that every file be an app image.
	if err := esprom.CheckPlanChipIDs(esprom.FlashPlan{Images: b.Images}); err != nil {
		return nil, fmt.Errorf("firmware bundle %s: %w", source, err)
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

// readZipFile inflates one member, never past the budget it is handed. Reading
// without a ceiling is what let a small archive expand to whatever memory the hub had.
func readZipFile(f *zip.File, budget int64) ([]byte, error) {
	rc, err := f.Open()
	if err != nil {
		return nil, err
	}
	defer rc.Close()
	if budget < 0 {
		budget = 0
	}
	const headroom = 1 // one extra byte is how an overflow is DETECTED, not assumed
	raw, err := io.ReadAll(io.LimitReader(rc, budget+headroom))
	if err != nil {
		return nil, err
	}
	if int64(len(raw)) > budget {
		return nil, fmt.Errorf("%s: bundle expands past its allowance", f.Name)
	}
	return raw, nil
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
	i, ok := findAppDesc(app)
	if !ok {
		return "", "", "", ""
	}
	d := app[i:]
	// Every field is bytes a hand-edited or attacker-supplied image controls, and
	// they end up rendered in the browser. Keep printable ASCII and drop the rest,
	// NUL and newline included: the client escapes markup, but escaping is one line
	// a future field can be added without, and no orb's version string needs a
	// control character to be legible.
	field := func(off, n int) string {
		return strings.Map(func(r rune) rune {
			if r >= 0x20 && r < 0x7f {
				return r
			}
			return -1
		}, string(d[off:off+n]))
	}
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

// AppImage returns the app-bin payload — the single image an OTA takes. The
// device's /api/v1/ota stages one app slot, so this is exactly one image's
// bytes, found the same way AppPlan finds it: by name, with the partition
// offset as the tiebreak for a manifest that renamed it.
func (b *Bundle) AppImage() []byte {
	if im := b.imageNamed("gorb-os.bin"); im != nil {
		return im.Data
	}
	for _, im := range b.Images {
		if im.Offset == 0x20000 { // the ota_0 slot from partitions.csv
			return im.Data
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

// Release is the three facts about a firmware build that can be honestly compared:
// what release it declares, when it was compiled and the digest of the ELF it came
// from. A declared version orders the releases; a git describe like "8eb4bf0-dirty"
// carries no ordering, so a pre-scheme stamp is never the basis for "newer" and the
// build timestamp decides instead.
type Release struct {
	Version    string
	CompiledAt time.Time // zero when the build stamp was unparsable
	ElfSHA256  string    // truncated prefix is fine; comparison is by prefix
}

// Release reads the running-orb descriptor out of an app image's
// esp_app_desc_t, so a bundle and an orb are described in the same terms.
func (b *Bundle) Release() Release {
	app := b.imageNamed("gorb-os.bin")
	if app == nil {
		return Release{}
	}
	return releaseFromApp(app.Data)
}

func releaseFromApp(app []byte) Release {
	_, version, _, built := appDescriptor(app)
	r := Release{Version: version}
	// The digest is binary, not text: hex-encode the bytes rather than treating
	// them as a string, which would mangle anything above 0x7F.
	if i, ok := findAppDesc(app); ok {
		r.ElfSHA256 = hex.EncodeToString(app[i+appDescShaOff : i+appDescShaOff+32])
	}
	if t, err := time.Parse("Jan 2 2006 15:04:05", strings.TrimSpace(built)); err == nil {
		r.CompiledAt = t.UTC()
	}
	return r
}

// appDescShaOff is esp_app_desc_t's app_sha256 field, 32 bytes, right after
// idf_ver — the digest of the ELF, which is the same string the bootloader
// prints as "ELF file SHA256". It is measured from the descriptor, not from the
// start of the image: the descriptor is the first thing in the app's first
// segment, 32 bytes past the image header, and indexing from 0 reads the header
// and returns four bytes of nonsense.
const appDescShaOff = 144

// findAppDesc locates the esp_app_desc_t inside an app image by its magic word.
func findAppDesc(app []byte) (int, bool) {
	want := make([]byte, 4)
	binary.LittleEndian.PutUint32(want, appDescMagic)
	i := bytes.Index(app, want)
	if i < 0 || i+appDescShaOff+32 > len(app) {
		return 0, false
	}
	return i, true
}

// Verdict is how the firmware about to be flashed relates to the one running.
type Verdict string

const (
	VerdictUnknown     Verdict = "unknown"      // cannot tell: no comparable stamp
	VerdictSame        Verdict = "same"         // same build; flashing changes nothing
	VerdictUpgrade     Verdict = "upgrade"      // the selected firmware is newer
	VerdictDowngrade   Verdict = "downgrade"    // the selected firmware is older
	VerdictSameVersion Verdict = "same-version" // same declared release, different build
)

// Compare decides what flashing target at the orb that is running. The caller
// must have established the two are the same kind of device; comparing an orb
// against a foreign build is a question with no sensible answer.
//
// The rules, in order, and why each is where it is:
//   - Same ELF digest means the same bytes: nothing to do.
//   - Both sides carry a declared version: the triple orders them, and a
//     pre-release sits below the release it precedes. This outranks the
//     timestamp, which measures a clock rather than a release.
//   - Either side predates the version scheme: the later build stamp wins, the
//     only ordering an unnamed build can offer.
//   - Equal versions, or equal describes with different stamps: report
//     same-version rather than upgrade/downgrade, because a rebuild of one
//     release is not a new release.
//   - Anything missing: unknown. Guessing "newer" here would push a user into
//     replacing a working orb on no evidence.
func Compare(orb, target Release) Verdict {
	if orb.ElfSHA256 != "" && target.ElfSHA256 != "" {
		a, b := normSha(orb.ElfSHA256), normSha(target.ElfSHA256)
		n := min(len(a), len(b))
		if n >= 8 && a[:n] == b[:n] {
			return VerdictSame
		}
	}
	// A declared version outranks a build timestamp, because a timestamp measures
	// when someone's clock was, and a triple measures what the release was.
	ov, ook := ParseVersion(orb.Version)
	tv, tok := ParseVersion(target.Version)
	if ook && tok {
		switch c := ov.Compare(tv); {
		case c < 0:
			return VerdictUpgrade
		case c > 0:
			return VerdictDowngrade
		default:
			// One release, different bytes: a rebuild, a different commit, or a
			// dirty tree. Not an upgrade, and not nothing — the orb still reboots.
			return VerdictSameVersion
		}
	}
	// At least one side predates the scheme, so fall through to the timestamp
	// ladder. Declaring a named release newer than an unnamed build would push a
	// user into replacing a working orb on an assumption.
	if orb.CompiledAt.IsZero() || target.CompiledAt.IsZero() {
		return VerdictUnknown
	}
	switch {
	case target.CompiledAt.After(orb.CompiledAt):
		if sameVersion(orb.Version, target.Version) {
			return VerdictSameVersion
		}
		return VerdictUpgrade
	case orb.CompiledAt.After(target.CompiledAt):
		if sameVersion(orb.Version, target.Version) {
			return VerdictSameVersion
		}
		return VerdictDowngrade
	default:
		return VerdictSame
	}
}

func normSha(s string) string {
	return strings.ToLower(strings.TrimRight(strings.TrimSpace(s), "."))
}

// sameVersion reports two stamps naming one release. A parsed pair ignores commit
// and -dirty, which is what a rebuild of one release means. A legacy pair keeps the
// old rule of dropping -dirty and comparing the rest, because a bare git describe
// carries no other structure to compare.
func sameVersion(a, b string) bool {
	av, aok := ParseVersion(a)
	bv, bok := ParseVersion(b)
	if aok && bok {
		return av.Compare(bv) == 0
	}
	t := func(s string) string { return strings.TrimSuffix(strings.TrimSpace(s), "-dirty") }
	return t(a) != "" && t(a) == t(b)
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// BuiltIn returns the firmware the binary shipped with — what an end user flashes
// without touching a file dialog, and what "use the built-in firmware" returns to.
func BuiltIn() (*Bundle, error) {
	if len(embedded) == 0 {
		return nil, fmt.Errorf("no firmware is compiled into this build of the hub")
	}
	return Load(embedded, "built-in")
}
