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
//
// The second shape a user can pick is a whole-flash image: the single 0xE9 blob
// `esptool merge_bin` lays out from those same files, written at 0x0. It carries
// no manifest, so it gets no manifest-derived promises back — one write, no app
// slot to OTA, no per-image progress — and Load proves its provenance out of the
// bytes instead. Both shapes are Load's to parse; callers just get a Bundle.
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
	"path/filepath"
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
	// Merged marks a whole-flash image: one blob written at 0x0, with no manifest
	// beside it. Everything the hub derives from a manifest's names — the app slot
	// an OTA pushes, the image list a per-image progress bar walks — is absent on
	// this shape, so the doors that need those things say so instead of failing
	// with a missing-file error that describes none of it.
	Merged bool
	// Foreign marks firmware the hub accepted only because the operator named it:
	// its esp_app_desc_t announces a project that is not gorb-os. It is a record of
	// a decision already taken, not a second gate — the panel says so out loud, and
	// the doors that reason from "this is gorb-os" (the OTA version comparison, the
	// theme id the device derives) close on it rather than guessing.
	Foreign bool
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
	if b.Merged {
		return esprom.FlashPlan{}, b.errNoAppSlot()
	}
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

// errNoAppSlot says why a whole-flash image cannot serve the two doors that need
// one app image pulled out of the set: the WiFi update and the bench app-only
// reflash. It is a shape fact, not a damage report — the blob is flashable, it is
// simply already the whole device, so there is no half of it to address by name.
func (b *Bundle) errNoAppSlot() error {
	return fmt.Errorf("firmware %s is one whole-flash image written at 0x0, so it has no separately addressable app slot: flash it over the cable, or load a .zip bundle to update an orb over WiFi", b.Source)
}

// Ready is the gate a picked file passes before it becomes the firmware the hub
// flashes. The two shapes are proved differently on purpose. A bundle's
// whole-device plan is settled by its manifest, so what Ready asks of it is the
// one thing a cable flash does not need and every other door does: a named app
// slot. A merged image is already the whole device, so its proof is the set of
// gates loadMerged ran — size, header, chip, project, partition table — and
// demanding an app slot from it would ask for a manifest the shape cannot have.
func (b *Bundle) Ready() error {
	if !b.Merged {
		_, err := b.AppPlan()
		return err
	}
	if len(b.plan.Images) != 1 || b.plan.Images[0].Offset != mergedOffset {
		return fmt.Errorf("firmware %s is not a one-image plan at 0x0", b.Source)
	}
	// A foreign blob may legitimately carry a layout this hub cannot read, and
	// loadMerged kept the table only if it parsed. Demanding one here would
	// re-impose the gate the operator overrode, two calls after the override was
	// honoured; the wipe door refuses on its own when it has no table to aim with.
	if b.Foreign && len(b.partTable) == 0 {
		return nil
	}
	if _, err := esprom.NvsRegions(b.partTable); err != nil {
		return fmt.Errorf("firmware %s: %w", b.Source, err)
	}
	return nil
}

// PartitionTable returns the bytes the plan writes at 0x8000. The factory reset
// takes its erase targets from these, which is what keeps a wipe aimed at the
// layout actually being flashed.
func (b *Bundle) PartitionTable() []byte { return b.partTable }

// LonelyPlan is the plan that turns any S3 into a lonely board: this bundle's
// bootloader, partition table and initial otadata, with the lonely app in the
// ota_0 slot. The boot images come from the bundle on purpose — a lonely board
// then sits inside the exact geometry a New Orb flash overwrites, so the joke
// is always reversible by the normal door.
func (b *Bundle) LonelyPlan(lonelyBin []byte) (esprom.FlashPlan, error) {
	// The lonely image is three files from the ACTIVE firmware plus one the hub
	// embeds, assembled at the orb's own offsets. A whole-flash blob is those first
	// three fused into one span with no manifest naming them, so say the shape and
	// the way out rather than reporting a missing bootloader the file does contain.
	if b.Merged {
		return esprom.FlashPlan{}, fmt.Errorf("firmware %s is one whole-flash image written at 0x0, so it has no separate bootloader and partition table for a lonely plan: load a .zip bundle to send this orb lonely, or flash the lonely image with orbflash", b.Source)
	}
	boot := b.imageNamed("bootloader.bin")
	pt := b.imageNamed("partition-table.bin")
	var ota []byte
	if im := b.imageNamed("ota_data_initial.bin"); im != nil {
		ota = im.Data
	}
	if boot == nil || pt == nil {
		return esprom.FlashPlan{}, fmt.Errorf("firmware %s has no bootloader or partition table to build a lonely plan", b.Source)
	}
	return esprom.LonelyPlan(lonelyBin, boot.Data, pt.Data, ota)
}

// NotGorbOS is the refusal every project gate returns, wrapped in the source and
// the sentence that explains it. It exists so the one caller that has been handed
// the operator's yes — the upload handler, and the card behind it — can tell this
// refusal from every other and offer the second send, rather than string-matching
// an error message that was written to be read by a person.
type NotGorbOS struct{ Project string }

func (e *NotGorbOS) Error() string {
	return fmt.Sprintf("is the project %q, not gorb-os", e.Project)
}

// Options relaxes the provenance gates for one deliberate pick. The zero value is
// the strict default every door uses: an uploaded file, a --firmware file at
// startup, an orbflash -firmware file. Only the operator's own second click on a
// named refusal sets AllowForeign, so the gate can never be off by accident.
type Options struct {
	// AllowForeign accepts an image whose esp_app_desc_t names a project other
	// than gorb-os. It is not "accept anything": size, the 0xE9 header, the chip
	// ID and the descriptor's own hygiene are all still enforced, because those
	// decide whether the write is survivable, not whether it is ours. What it does
	// change is the partition-table gate — see loadMerged — because a vendor
	// factory image lays out its own flash, and the hub has no business grading
	// somebody else's layout once it has been told to write it.
	AllowForeign bool
}

// Load parses the firmware bytes a user picked, in either shape the hub accepts:
// a release bundle (the zip a release step produces: flasher_args.json plus the
// images it names) or a whole-flash image (the 0xE9 blob `esptool merge_bin`
// lays out from those same images, written at 0x0). The shape comes from the
// bytes, never from the filename, so renaming a bundle to .bin cannot change how
// it is parsed and a mislabeled file is refused with what was actually found.
//
// Every image a manifest names must be present, so a partial or hand-edited
// bundle fails here rather than half-way into a flash.
func Load(raw []byte, source string) (*Bundle, error) {
	return LoadWithOptions(raw, source, Options{})
}

// LoadWithOptions is Load with the provenance gates opened by one explicit
// operator decision. Callers that have not been given that decision call Load.
func LoadWithOptions(raw []byte, source string, o Options) (*Bundle, error) {
	if len(raw) == 0 {
		return nil, fmt.Errorf("%s is empty", source)
	}
	if len(raw) > maxBundle {
		return nil, fmt.Errorf("firmware file too large (%d MB; the limit is %d MB)", len(raw)>>20, maxBundle>>20)
	}
	if bytes.HasPrefix(raw, []byte("PK\x03\x04")) {
		return loadBundle(raw, source, o)
	}
	if esprom.IsESPImage(raw) {
		return loadMerged(raw, source, o)
	}
	return nil, fmt.Errorf("%s is neither a firmware bundle nor an ESP image: it starts with 0x%02X, where a bundle starts with PK and an image with 0xE9", source, raw[0])
}

func loadBundle(raw []byte, source string, o Options) (*Bundle, error) {
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
		if !o.AllowForeign {
			return nil, fmt.Errorf("firmware bundle %s %w; the hub only flashes gorb-os releases", source, &NotGorbOS{b.Project})
		}
		// Recorded rather than refused: the panel has to say the true thing about
		// what this hub now carries, and the doors that depend on this being
		// gorb-os — the OTA stamp gate, the theme id the device derives — would
		// otherwise reason from a project name the bundle contradicts.
		b.Foreign = true
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

// ---- whole-flash images ------------------------------------------------------
//
// A whole-flash image is what `esptool merge_bin` builds from the same files a
// bundle carries: bootloader at 0x0, then partition table, otadata, app and art
// bank, each padded to its manifest offset with 0xFF, in one contiguous blob. It
// is the shape a person reaches for when they hold a build dir and no bundle. It
// carries no manifest, so the hub cannot ask it which image goes where and can
// only write the whole thing at 0x0 — one FLASH_BEGIN that erases and replaces
// everything from the bootloader to the end of the blob, nvs, otadata and phy_init
// included and both app slots with them. The gates below exist because the
// file gives up its own structure, so provenance is the only thing left to check,
// and it has to be read out of the bytes rather than off the filename.

const (
	// MergedName is the fallback name for a whole-flash image, used when the
	// source carries no basename of its own. Normally the image is named after the
	// file it came from, the same way a manifest's basename names a bundle's
	// images — so the flash log and a refusal both point at the file on disk.
	MergedName = "merged.bin"
	// mergedOffset is where the blob goes: the ROM's reset vector.
	mergedOffset = 0x0
	// ptOffset and ptEntrySize are gen_esp32part's fixed geometry — the table
	// lives at 0x8000 in 32-byte rows.
	ptOffset    = 0x8000
	ptEntrySize = 32
	// mergedMax is the orb's flash: every gorb-os build declares 16MB. A blob
	// past it cannot be written without running off the end of the chip.
	mergedMax = 16 << 20
	// mergedMin is the end of the nvs row the layout opens with. No whole-flash
	// gorb-os image is anywhere near this small; one that is has no app in it.
	mergedMin = 0x9000
)

func loadMerged(raw []byte, source string, o Options) (*Bundle, error) {
	if len(raw) < mergedMin {
		return nil, fmt.Errorf("%s is %d B: too small to be a whole-flash image, which must carry a bootloader at 0x0 and a partition table at 0x%X", source, len(raw), ptOffset)
	}
	if len(raw) > mergedMax {
		return nil, fmt.Errorf("%s is %d MB, larger than the %d MB flash this orb has, so a whole-flash image cannot fit", source, len(raw)>>20, mergedMax>>20)
	}
	img := esprom.Image{Offset: mergedOffset, Name: mergedName(source), Data: raw, Verbatim: true}
	// The chip gate, reused from the flasher so a build for another Espressif chip
	// gets the one refusal wording that already names the chip found. The blob's
	// own header sits at 0x0, which is where the gate reads it.
	if err := esprom.CheckPlanChipIDs(esprom.FlashPlan{Images: []esprom.Image{img}}); err != nil {
		// Returned as the gate worded it: the name inside that message is already
		// this file's own (see mergedName), and prefixing the source again would
		// read "orb-merged.bin: orb-merged.bin is built for ESP32-C6".
		return nil, err
	}
	// The project gate, inverted from the bundle's rule. A bundle with no
	// descriptor is accepted and reported as unknown, because its manifest named
	// the app image and the hub flashes those bytes faithfully whatever they
	// announce. A merged blob promises no such thing: it overwrites the running
	// firmware with everything in the file, so the file has to say what it is.
	best, ours, seen := scanDescriptors(raw)
	if seen == 0 {
		return nil, fmt.Errorf("%s holds no esp_app_desc_t that names a project, so nothing in it says what it is; a whole-flash image must be built from gorb-os, where a descriptor-less image is only accepted inside a .zip bundle", source)
	}
	if !ours {
		// The one gate AllowForeign answers, and the refusal it quotes is the
		// sentence the operator then confirms. A file that names no project at all
		// stays refused whatever the flags say, because there is no name for a
		// confirmation to repeat back.
		if !o.AllowForeign {
			return nil, fmt.Errorf("%s %w; the hub only flashes gorb-os releases", source, &NotGorbOS{best.Project})
		}
	}
	b := &Bundle{
		Source: source, Merged: true, Foreign: !ours,
		Project: best.Project, Version: best.Version, IDFVersion: best.IDF, Built: best.Built,
		SHA256: digestOf(raw), Images: []esprom.Image{img},
		// The header in the blob is authoritative, because Verbatim stops the
		// flasher rewriting it and merge_bin already stamped dio/16MB into it.
		// These fields still describe the write for SpiSetParams and for the
		// plan a bench tool prints.
		plan: esprom.FlashPlan{Images: []esprom.Image{img}, FlashSize: mergedMax, FlashMode: 2, FlashFreq: 0xFF},
	}
	// The partition table is read structurally, because the factory reset takes
	// its erase targets from these bytes. On a gorb-os image a table the wipe
	// cannot resolve is refused here rather than failing later at the moment an
	// operator asks to send an orb back to provisioning.
	//
	// On a foreign image it is kept if it parses and dropped if it does not. Its
	// layout is the vendor's, not ours, and the write is verbatim at 0x0: the ROM
	// does not care what the hub's wipe bookkeeping makes of it. The wipe door
	// then refuses itself, with the reason, if the table is unreadable.
	table, err := mergedPartitionTable(raw)
	if err != nil {
		if !o.AllowForeign {
			return nil, fmt.Errorf("%s: %w", source, err)
		}
		return b, nil
	}
	if _, err := esprom.NvsRegions(table); err != nil && !o.AllowForeign {
		return nil, fmt.Errorf("%s: %w", source, err)
	}
	b.partTable = table
	return b, nil
}

// appDescMagicBytes is the esp_app_desc_t magic as it appears in an image, least
// significant byte first — the same spelling findAppDesc builds.
var appDescMagicBytes = func() []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, appDescMagic)
	return b
}()

// appDescFields is one esp_app_desc_t read out of raw bytes, with the hygiene
// applied at the source — see descriptorAt.
type appDescFields struct{ Project, Version, IDF, Built string }

// scanDescriptors weighs every esp_app_desc_t in a blob, not just the first, and
// counts only the ones that name a project. A bundle names its app image in its
// manifest, so the first magic inside that one file is the descriptor; a merged
// blob is five images concatenated, and any headerless part of it can hold the four
// magic bytes by chance or carry a descriptor whose project_name was never
// stamped. The candidate that announces gorb-os is the app. best names one
// project-naming candidate for a refusal to quote when none matches, and seen
// counts those, so "nothing here names a project" and "the project named is not
// ours" stay two different answers.
func scanDescriptors(blob []byte) (best appDescFields, ours bool, seen int) {
	for from := 0; ; {
		j := bytes.Index(blob[from:], appDescMagicBytes)
		if j < 0 {
			return best, false, seen
		}
		at := from + j
		from = at + len(appDescMagicBytes)
		if at+appDescShaOff+32 > len(blob) {
			continue // a truncated candidate is not a descriptor
		}
		f := descriptorAt(blob, at)
		if f.Project == "" {
			continue // a descriptor with no project in it vouches for nothing
		}
		seen++
		if best.Project == "" {
			best = f
		}
		if strings.EqualFold(f.Project, "gorb-os") {
			return f, true, seen
		}
	}
}

// mergedName is the plan's name for a whole-flash image. A bundle takes its image
// names from the manifest's basenames; a merged blob has one image and one name
// available, which is the file the operator picked. Refusals and the flash log both
// read this, so "orb-merged.bin is built for ESP32-C6" names the thing to go back
// and look at.
func mergedName(source string) string {
	if n := strings.TrimSpace(filepath.Base(source)); n != "" && n != "." && n != string(filepath.Separator) {
		return n
	}
	return MergedName
}

// mergedPartitionTable lifts the partition table out of a whole-flash image. The
// bundle path gets these bytes from the manifest, which names the file; the blob
// carries no manifest, so the table comes from the one address the layout puts it
// at, copied row by row until the rows stop. A real table is the declared
// partitions, then gen_esp32part's MD5 row — magic 0xEBEB, not 0xAA50 — then 0xFF
// padding to the end of its 3 KB image, so the walk ends at the first all-blank
// row. Anything else means the blob is not laid out the way a gorb-os image is.
func mergedPartitionTable(raw []byte) ([]byte, error) {
	for i := ptOffset; i+ptEntrySize <= len(raw); i += ptEntrySize {
		e := raw[i : i+ptEntrySize]
		if (e[0] == 0xAA && e[1] == 0x50) || (e[0] == 0xEB && e[1] == 0xEB) {
			continue
		}
		if allFF(e) {
			if i == ptOffset {
				return nil, fmt.Errorf("no partition table at 0x%X: the image is blank flash there", ptOffset)
			}
			return raw[ptOffset:i], nil
		}
		return nil, fmt.Errorf("the partition table at 0x%X stops being one %d rows in (a row starts %02X %02X): this is not a gorb-os whole-flash image",
			ptOffset, (i-ptOffset)/ptEntrySize, e[0], e[1])
	}
	return nil, fmt.Errorf("the partition table at 0x%X runs off the end of the image", ptOffset)
}

func allFF(b []byte) bool {
	for _, c := range b {
		if c != 0xFF {
			return false
		}
	}
	return true
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
	f := descriptorAt(app, i)
	return f.Project, f.Version, f.IDF, f.Built
}

// descriptorAt reads the four fields of the esp_app_desc_t starting at index i of
// a blob. Every one is bytes a hand-edited or attacker-supplied image controls,
// and they end up rendered in the browser. Keep printable ASCII and drop the rest,
// NUL and newline included: the client escapes markup, but escaping is one line
// a future field can be added without, and no orb's version string needs a
// control character to be legible.
func descriptorAt(blob []byte, i int) appDescFields {
	d := blob[i:]
	field := func(off, n int) string {
		return strings.Map(func(r rune) rune {
			if r >= 0x20 && r < 0x7f {
				return r
			}
			return -1
		}, string(d[off:off+n]))
	}
	return appDescFields{
		Project: field(48, 32), Version: field(16, 32), IDF: field(112, 32),
		Built: strings.TrimSpace(field(96, 16) + " " + field(80, 16)),
	}
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
	// A whole-flash blob carries an app somewhere inside it, at 0x20000 of its own
	// body. This returns nil for it on purpose: slicing the app out would mean
	// trusting the layout rather than a manifest that names it, and an OTA image
	// the hub assembled by guesswork is worse than an honest refusal. See Ready.
	if b.Merged {
		return nil
	}
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
