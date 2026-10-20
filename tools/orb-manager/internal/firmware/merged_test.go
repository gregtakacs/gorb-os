package firmware

import (
	"bytes"
	"encoding/binary"
	"errors"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// A whole-flash image is the shape with no manifest to vouch for it, so every
// claim the hub used to read off flasher_args.json has to be earned from the bytes
// themselves. These are the gates that replace it, each pinned to the mistake it
// stops rather than to the branch it covers.

// ptRow is one gen_esp32part entry: magic AA50, type/subtype, LE offset and
// size, 16-byte label, LE flags.
func ptRow(ptype, sub byte, off, size uint32, label string) []byte {
	e := make([]byte, ptEntrySize)
	e[0], e[1], e[2], e[3] = 0xAA, 0x50, ptype, sub
	binary.LittleEndian.PutUint32(e[4:], off)
	binary.LittleEndian.PutUint32(e[8:], size)
	copy(e[12:28], label)
	return e
}

// ptMD5Row is the checksum row gen_esp32part appends after the last partition.
// Its magic is 0xEBEB, not 0xAA50, which is exactly why the table walk has to
// know about it: a reader that only accepts AA50 sees a corrupt table.
func ptMD5Row() []byte {
	e := make([]byte, ptEntrySize)
	e[0], e[1] = 0xEB, 0xEB
	return e
}

// mergedTable builds the table a gorb-os image carries at 0x8000: the rows the
// layout declares, the MD5 row, then 0xFF padding out to the 3 KB of the image.
func mergedTable(rows ...[]byte) []byte {
	table := bytes.Join(rows, nil)
	table = append(table, ptMD5Row()...)
	pad := 3072 - len(table)
	if pad < 0 {
		pad = 0
	}
	return append(table, bytes.Repeat([]byte{0xFF}, pad)...)
}

// mergedImage is an ESP image header (24 bytes, packed) plus a body. digest sets
// the appended-SHA256 flag every IDF build sets, so a fixture that skipped it
// would not look like the real thing to the offset-0 rewrite.
func mergedImage(chipID uint16, digest bool, body []byte) []byte {
	h := make([]byte, 24)
	h[0], h[1], h[2], h[3] = 0xE9, 0x02, 0x02, 0x4f
	binary.LittleEndian.PutUint32(h[4:], 0x403710ae) // plausible entry point
	h[12], h[13] = byte(chipID), byte(chipID>>8)
	if digest {
		h[8+15] = 1
	}
	return append(h, body...)
}

// mergedDesc is an esp_app_desc_t: magic, then the fields at their real offsets.
func mergedDesc(project, version string) []byte {
	d := make([]byte, appDescShaOff+32)
	binary.LittleEndian.PutUint32(d, appDescMagic)
	copy(d[16:48], version)
	copy(d[48:80], project)
	copy(d[80:96], "01:58:41")
	copy(d[96:112], "Oct 12 2026")
	copy(d[112:144], "v5.4")
	return d
}

// mergedOpts shapes a blob. project empty plants no descriptor; rows nil uses the
// gorb-os layout; chipID uses S3.
type mergedOpts struct {
	project string
	version string
	chipID  uint16
	assets  bool
	rows    [][]byte
	tableAt func([]byte) []byte // replaces the table bytes wholesale
}

// mergedBlob lays a blob out the way merge_bin does, at the offsets partitions.csv
// declares, and pads every gap with 0xFF the way a blank flash reads.
func mergedBlob(t *testing.T, o mergedOpts) []byte {
	t.Helper()
	chip := o.chipID
	if chip == 0 {
		chip = 9 // ESP_CHIP_ID_ESP32S3
	}
	app := mergedImage(chip, true, mergedDesc(o.project, o.version))
	app = append(app, bytes.Repeat([]byte{0x01}, 4096)...)

	end := 0x20000 + len(app)
	if o.assets {
		end = 0x420000 + 512
	}
	blob := bytes.Repeat([]byte{0xFF}, end)

	write := func(off int, b []byte) { copy(blob[off:], b) }
	write(0x0, mergedImage(chip, true, bytes.Repeat([]byte{0x02}, 2048)))

	rows := o.rows
	if rows == nil {
		rows = [][]byte{
			ptRow(1, 2, 0x9000, 0x8000, "nvs"),
			ptRow(1, 0, 0x11000, 0x2000, "otadata"),
			ptRow(1, 1, 0x13000, 0x1000, "phy_init"),
			ptRow(0, 16, 0x20000, 0x200000, "ota_0"),
			ptRow(0, 17, 0x220000, 0x200000, "ota_1"),
			ptRow(1, 6, 0x420000, 0x800000, "assets"),
			ptRow(1, 2, 0xd60000, 0x6000, "storage"),
		}
	}
	table := mergedTable(rows...)
	if o.tableAt != nil {
		table = o.tableAt(table)
	}
	write(0x8000, table)
	write(0x11000, bytes.Repeat([]byte{0xFF}, 8192))
	write(0x20000, app)
	if o.assets {
		write(0x420000, bytes.Repeat([]byte("art"), 171))
	}
	return blob
}

// The shape the whole change is about: a .bin off a build server becomes the
// firmware a hub flashes, and reports honestly what it is.
func TestLoadAcceptsWholeFlashImage(t *testing.T) {
	b := mustMerged(t, mergedOpts{project: "gorb-os", version: "1.4.2"})
	if !b.Merged {
		t.Error("Merged = false, so the UI would promise per-image progress and an OTA slot the blob cannot give")
	}
	if b.Project != "gorb-os" || b.Version != "1.4.2" {
		t.Errorf("provenance = %q %q, read out of the descriptor the blob carries", b.Project, b.Version)
	}
	if b.Built != "Oct 12 2026 01:58:41" {
		t.Errorf("Built = %q, want the stamp's date and time", b.Built)
	}
	if b.SHA256 == "" {
		t.Error("no digest: the operator could not quote which file this hub carries")
	}
	if err := b.Ready(); err != nil {
		t.Errorf("a complete whole-flash image failed Ready: %v", err)
	}
	// The table is the rows, not the padded image: the walk stops at the first
	// blank row, so what NvsRegions gets is whole entries and nothing after them.
	table := b.PartitionTable()
	if len(table) == 0 || len(table)%ptEntrySize != 0 {
		t.Errorf("PartitionTable = %d B, want whole 32-byte rows", len(table))
	}
	regions, err := esprom.NvsRegions(table)
	if err != nil {
		t.Fatalf("NvsRegions on a picked merged blob: %v — the factory reset would fail on this file", err)
	}
	if len(regions) != 2 {
		t.Errorf("erase targets = %d, want nvs and storage from the layout", len(regions))
	}
}

// One image, at the reset vector, flagged Verbatim, and named for what it is. The
// flag is not cosmetic: the flasher's offset-0 rewrite would otherwise digest the
// entire blob and overwrite the last 32 bytes of whatever partition sits at its
// end. See internal/esprom/verbatim_test.go for that byte-level proof.
func TestMergedBlobIsAOneImageVerbatimPlanAtZero(t *testing.T) {
	b := mustMerged(t, mergedOpts{project: "gorb-os", version: "1.4.2"})
	imgs := b.Plan().Images
	if len(imgs) != 1 {
		t.Fatalf("plan has %d images, want the one write a merged blob is", len(imgs))
	}
	if imgs[0].Offset != 0x0 {
		t.Errorf("offset = 0x%X, want 0x0", imgs[0].Offset)
	}
	if !imgs[0].Verbatim {
		t.Error("Verbatim = false on a merged plan")
	}
	// Named after the file it came from, the way a manifest names a bundle's
	// images, so the log line points at something the operator can go look at.
	if imgs[0].Name != "orb-merged.bin" {
		t.Errorf("name = %q, want the picked file's basename", imgs[0].Name)
	}
	if mergedName("") != MergedName || mergedName("/") != MergedName {
		t.Errorf("mergedName fallback = %q/%q, want %q", mergedName(""), mergedName("/"), MergedName)
	}
}

// A whole-flash image is all of the device, so it has no half of it to address.
// Every door that needs one app image must say so rather than send the wire a
// slice the hub cut out by guessing at the layout.
func TestMergedBlobDeclinesTheAppSlotDoors(t *testing.T) {
	b := mustMerged(t, mergedOpts{project: "gorb-os", version: "1.4.2"})
	if got := b.AppImage(); got != nil {
		t.Errorf("AppImage returned %d B: an OTA image cut out of a blob by offset guesswork", len(got))
	}
	if _, err := b.AppPlan(); err == nil {
		t.Error("AppPlan succeeded on a merged blob, which has no manifest-named app")
	} else if !strings.Contains(err.Error(), "whole-flash") {
		t.Errorf("AppPlan error = %v, want it to name the shape as the reason", err)
	}
}

// The project gate, at the one place it can be bypassed by shape: a raw blob has
// no manifest naming gorb-os, so the descriptor inside it is the only claim of
// provenance, and a build of someone's hello_world must not be flashable just
// because it is a valid S3 image.
func TestLoadRefusesAForeignProjectInARawBlob(t *testing.T) {
	_, err := Load(mergedBlob(t, mergedOpts{project: "hello_world", version: "1.0"}), "other.bin")
	if err == nil {
		t.Fatal("a whole-flash build of another project was accepted")
	}
	if !strings.Contains(err.Error(), "hello_world") {
		t.Errorf("error = %v, want it to name the project the blob declares", err)
	}
}

// The inversion of the bundle rule, pinned on both sides so neither can drift:
// the same descriptor-less bytes are a warning inside a manifest and a refusal on
// their own, because only the manifest is a promise about what the file is.
func TestLoadRefusesADescriptorlessRawBlob(t *testing.T) {
	_, err := Load(mergedBlob(t, mergedOpts{}), "nodesc.bin")
	if err == nil {
		t.Fatal("a whole-flash image with no descriptor anywhere was accepted")
	}
	if !strings.Contains(err.Error(), "esp_app_desc_t") {
		t.Errorf("error = %v, want it to say the descriptor is what is missing", err)
	}
	files := makeFiles(t)
	files["gorb-os.bin"] = espImage(nil)
	if _, err := Load(pack(t, files, testManifest), "nodesc.zip"); err != nil {
		t.Errorf("the descriptor-less bundle must still load and report unknown: %v", err)
	}
}

func mustMerged(t *testing.T, o mergedOpts) *Bundle {
	t.Helper()
	b, err := Load(mergedBlob(t, o), "orb-merged.bin")
	if err != nil {
		t.Fatalf("a good whole-flash image was refused: %v", err)
	}
	return b
}

// A build for another Espressif chip writes cleanly at its offset and boots as
// nothing, with no complaint from the ROM. The refusal has to name the chip found,
// because the ordinary cause is an unrelated download out of a browser folder.
func TestLoadRefusesAWrongChipBlob(t *testing.T) {
	_, err := Load(mergedBlob(t, mergedOpts{project: "gorb-os", version: "1.0", chipID: 0x000D}), "c6.bin")
	if err == nil {
		t.Fatal("a whole-flash ESP32-C6 image was accepted")
	}
	if !strings.Contains(err.Error(), "ESP32-C6") {
		t.Errorf("error = %v, want it to name the chip", err)
	}
}

// The two size gates, one at each end. Over the flash is a write that runs off the
// end of the chip; a few KB is a bootloader extract, which is most of what a person
// grabs from a build dir and cannot boot anything.
func TestLoadRefusesAWrongSizedBlob(t *testing.T) {
	if _, err := Load(append([]byte{0xE9, 0x01}, make([]byte, 4096)...), "bootloader_only.bin"); err == nil {
		t.Error("a 4 KB blob was accepted as a whole-flash image")
	} else if !strings.Contains(err.Error(), "too small") {
		t.Errorf("error = %v, want it to say the blob is too small", err)
	}
	huge := make([]byte, mergedMax+1)
	huge[0] = 0xE9
	if _, err := Load(huge, "huge.bin"); err == nil {
		t.Error("a blob larger than the flash was accepted")
	} else if !strings.Contains(err.Error(), "larger than") {
		t.Errorf("error = %v, want it to say the blob is larger than the flash", err)
	}
}

// The wrong-file case the operator actually hits: a text file, or an archive that
// is not a firmware. The message names both shapes the hub takes and the byte it
// found, so the answer is in the error rather than in a guess.
func TestLoadRefusesANonFirmwareFile(t *testing.T) {
	if _, err := Load([]byte("just some bytes"), "notes.txt"); err == nil {
		t.Error("a text file was accepted as firmware")
	} else if !strings.Contains(err.Error(), "0x6A") {
		t.Errorf("error = %v, want the byte actually found", err)
	}
	// A real zip with no manifest is the likely mistake, and the zip path must
	// still report it the way it always did.
	notFirmware := rawZip(t, map[string][]byte{"readme.md": []byte("hello")})
	if _, err := Load(notFirmware, "other.zip"); err == nil || !strings.Contains(err.Error(), ManifestName) {
		t.Errorf("Load(zip without manifest) = %v, want it to name %s", err, ManifestName)
	}
}

// The lonely image borrows the active firmware's bootloader, partition table and
// initial otadata and writes them at the orb's offsets. A merged blob holds all
// three, fused, with nothing naming them — so the door refuses with the shape and
// the way out, not with "no bootloader" about a file that is mostly bootloader.
func TestMergedBlobClosesTheLonelyDoor(t *testing.T) {
	b := mustMerged(t, mergedOpts{project: "gorb-os", version: "1.4.2"})
	_, err := b.LonelyPlan([]byte("lonely app bytes"))
	if err == nil {
		t.Fatal("a lonely plan was assembled out of a blob that names none of its parts")
	}
	if !strings.Contains(err.Error(), "whole-flash") {
		t.Errorf("error = %v, want it to name the shape", err)
	}
	if !strings.Contains(err.Error(), ".zip") {
		t.Errorf("error = %v, want it to point at the shape that does work", err)
	}
}

// The table walk, and the reason it exists at all: a gorb-os table ends with a
// 0xEBEB MD5 row and then 0xFF, so a reader that only knows the 0xAA50 row magic
// calls a good table corrupt. The fixture below keeps the MD5 row; this one proves
// a table that stops being a table mid-way is caught.
func TestLoadRefusesAMergedBlobWhoseTableIsNotOne(t *testing.T) {
	_, err := Load(mergedBlob(t, mergedOpts{
		project: "gorb-os", version: "1.0",
		tableAt: func(tbl []byte) []byte { return bytes.Repeat([]byte{0x77}, len(tbl)) },
	}), "scrambled.bin")
	if err == nil {
		t.Fatal("a blob with garbage at 0x8000 was accepted")
	}
	if !strings.Contains(err.Error(), "partition table") {
		t.Errorf("error = %v, want it to blame the partition table", err)
	}
}

// A table the factory reset cannot read rows out of is refused at pick time. The
// alternative is a hub that accepts the file and then fails the first time an
// operator asks to send an orb back to provisioning.
func TestLoadRefusesAMergedBlobWithNoNVSRow(t *testing.T) {
	_, err := Load(mergedBlob(t, mergedOpts{
		project: "gorb-os", version: "1.0",
		rows: [][]byte{ptRow(0, 16, 0x20000, 0x200000, "ota_0")},
	}), "no_nvs.bin")
	if err == nil {
		t.Fatal("a blob whose layout has no NVS partition was accepted")
	}
	if !strings.Contains(err.Error(), "nvs") {
		t.Errorf("error = %v, want it to name the missing partition", err)
	}
}

// The app is the descriptor that says gorb-os, not the first four bytes that
// happen to match and not the first candidate that names anything. A blob carrying
// another project's descriptor in its padding — a stale segment, somebody else's
// otadata — must still resolve to the gorb-os app rather than refuse the honest
// build over somebody else's bytes.
func TestScanDescriptorsWeighsEveryCandidateNotTheFirst(t *testing.T) {
	blob := mergedBlob(t, mergedOpts{project: "gorb-os", version: "1.4.2"})
	// A complete, project-naming descriptor at 0x12000, well before the app's own
	// at 0x20018, and it is not ours.
	early := 0x12000
	copy(blob[early:], mergedDesc("stray_app", "0.1"))
	f, ours, seen := scanDescriptors(blob)
	if !ours {
		t.Fatal("a foreign descriptor ahead of the app hid the app")
	}
	if seen < 2 {
		t.Errorf("seen = %d, want both the planted candidate and the app weighed", seen)
	}
	if f.Project != "gorb-os" || f.Version != "1.4.2" {
		t.Errorf("resolved to %q %q, want the app's own stamp", f.Project, f.Version)
	}
	// And the refusal still quotes the foreign name when nothing matches, because
	// "not gorb-os" is only useful if it says what was found instead.
	copy(blob[0x20018:], mergedDesc("gorb_other", "9.9"))
	if _, _, seen := scanDescriptors(blob); seen < 2 {
		t.Errorf("seen = %d after the app was replaced", seen)
	}
}

// Ready is the hub's one gate, and it must not soften into "whatever Load
// returned". A bundle with no app image still flashes over a cable, so the hub that
// also serves OTA has to refuse to adopt it — the shape is right and the promise is
// not there.
func TestReadyRefusesABundleWithoutAnAppSlot(t *testing.T) {
	noApp := `{"flash_settings":{"flash_mode":"dio","flash_size":"16MB","flash_freq":"80m"},` +
		`"flash_files":{"0x0":"bootloader/bootloader.bin","0x8000":"partition_table/partition-table.bin"}}`
	b, err := Load(pack(t, makeFiles(t), noApp), "boot_only.zip")
	if err != nil {
		t.Fatalf("a manifest that names no app is still a well-formed bundle: %v", err)
	}
	if err := b.Ready(); err == nil {
		t.Fatal("Ready accepted a firmware with no app slot to OTA")
	}
}

// ---- the foreign override ----------------------------------------------------
// The gate above is right about a stray build and wrong as an absolute: the
// vendor of this exact panel ships a factory demo, and restoring it is a thing a
// person with the board on their desk legitimately wants. These pin the shape of
// the yes — one explicit flag, everything that decides survivability still refused.

// The DFRobot case, named the way the real file names itself.
func TestAllowForeignAcceptsAVendorImageAndSaysWhoseItIs(t *testing.T) {
	const vendor = "esp32-s3-touch-amoled-brookesia"
	raw := mergedBlob(t, mergedOpts{project: vendor, version: "1.0"})

	// First the refusal, so the accepted case below is proof of a flag working and
	// not of a gate that was never there.
	if _, err := Load(raw, "FactoryOnly.bin"); err == nil {
		t.Fatal("the default Load accepted a foreign project")
	}
	b, err := LoadWithOptions(raw, "FactoryOnly.bin", Options{AllowForeign: true})
	if err != nil {
		t.Fatalf("AllowForeign refused the file it exists for: %v", err)
	}
	if !b.Merged || !b.Foreign {
		t.Errorf("Merged, Foreign = %v, %v: a vendor blob is both", b.Merged, b.Foreign)
	}
	// The name is reported, not rewritten. A panel that said "gorb-os" about this
	// file would be the same lie the gate exists to prevent.
	if b.Project != vendor {
		t.Errorf("Project = %q, want the vendor's own name kept", b.Project)
	}
	if err := b.Ready(); err != nil {
		t.Errorf("an adopted foreign blob must pass the hub's own gate: %v", err)
	}
	if len(b.Plan().Images) != 1 || b.Plan().Images[0].Offset != 0 {
		t.Error("a foreign whole-flash image is still one verbatim write at 0x0")
	}
	if !b.Plan().Images[0].Verbatim {
		t.Error("a vendor header must survive byte for byte; the rewrite would break its digest")
	}
}

// The gate the flag does NOT open: a C6 blob is unwritable hardware, whatever the
// operator believes about it.
func TestAllowForeignStillRefusesAnotherChip(t *testing.T) {
	_, err := LoadWithOptions(mergedBlob(t, mergedOpts{project: "brookesia_demo", chipID: 13}), "c6.bin",
		Options{AllowForeign: true})
	if err == nil {
		t.Fatal("AllowForeign accepted an image built for another chip")
	}
	if !strings.Contains(err.Error(), "ESP32-C6") {
		t.Errorf("error = %v, want the chip gate's own words", err)
	}
}

// And the gate the flag cannot open because there would be nothing left to
// confirm: a file that names no project at all gives a second send no name to
// repeat back, so it stays refused.
func TestAllowForeignStillRefusesADescriptorlessBlob(t *testing.T) {
	_, err := LoadWithOptions(mergedBlob(t, mergedOpts{}), "blank.bin", Options{AllowForeign: true})
	if err == nil {
		t.Fatal("AllowForeign accepted a blob that names no project")
	}
	if !strings.Contains(err.Error(), "esp_app_desc_t") {
		t.Errorf("error = %v, want the missing-descriptor reason", err)
	}
}

// A vendor layout is allowed to be one this hub cannot read: the write is verbatim
// and the ROM does not consult the wipe bookkeeping. Refusing it at pick time would
// send the operator to a command line for no safety gained.
func TestAllowForeignKeepsAnUnreadableLayoutOutOfTheGate(t *testing.T) {
	// A table whose second row starts with junk: mergedPartitionTable stops on it.
	garbage := func(table []byte) []byte {
		copy(table[ptEntrySize:], []byte{0x11, 0x22, 0x33, 0x44})
		return table
	}
	raw := mergedBlob(t, mergedOpts{project: "brookesia_factory", tableAt: garbage})

	if _, err := Load(raw, "vendor.bin"); err == nil {
		t.Fatal("a gorb-os claim was accepted on an unreadable table")
	}
	b, err := LoadWithOptions(raw, "vendor.bin", Options{AllowForeign: true})
	if err != nil {
		t.Fatalf("an unreadable vendor layout must not refuse the write: %v", err)
	}
	if len(b.PartitionTable()) != 0 {
		t.Error("a table that did not parse must not be handed to the wipe as if it had")
	}
	// Ready is where that could resurface as a demand. It must not be.
	if err := b.Ready(); err != nil {
		t.Errorf("Ready re-imposed the layout gate the override opened: %v", err)
	}
	// The wipe resolves its own targets, so it is the door that says no.
	if _, err := esprom.NvsRegions(b.PartitionTable()); err == nil {
		t.Error("a wipe of a firmware with no readable table must fail with a reason")
	}
}

// The hub's second-send path keys off this type, so the type has to survive the
// wrapping the gates do — errors.As is the only acceptable way to read it.
func TestNotGorbOSErrorSurvivesWrapping(t *testing.T) {
	_, err := Load(mergedBlob(t, mergedOpts{project: "hello_world"}), "other.bin")
	var ng *NotGorbOS
	if !errors.As(err, &ng) {
		t.Fatalf("err = %v, want it to unwrap to *NotGorbOS", err)
	}
	if ng.Project != "hello_world" {
		t.Errorf("Project = %q, want the name the blob declares", ng.Project)
	}
	// A different refusal must not answer to it: the UI offers a button on this
	// type alone, and a wrong yes is worse than no yes.
	if _, err := Load(mergedBlob(t, mergedOpts{}), "blank.bin"); errors.As(err, &ng) {
		t.Error("a missing descriptor was reported as a foreign project, which would offer a useless button")
	}
}
