package esprom

import (
	"context"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"time"
)

// Image is one blob to place at a flash offset, mirroring an entry in a
// bundle's flash_files map. Offset must be sector-aligned for the ROM
// (no-stub) path.
type Image struct {
	Offset uint32
	// Name is the display basename from the manifest (bootloader.bin, ...). It
	// is never joined with a directory: the bytes travel with the plan, so there
	// is no path left to resolve, and no OS path semantics to get wrong.
	Name string
	// Data is the image as packaged. Treat it as read-only: a bundle is built
	// once and reused across every flash of the hub's lifetime.
	Data []byte
}

// FlashPlan is the ordered set of images to write plus the flash geometry the
// ROM needs to be told about. A Plan comes from flasher_args.json so the
// offsets/mode come from the build, never hardcoded (the serflash.sh lesson).
type FlashPlan struct {
	Images    []Image
	FlashSize uint32 // bytes, e.g. 16MB
	FlashMode byte   // 0=qio 1=qout 2=dio 3=dout
	FlashFreq byte   // low nibble of the header size_freq byte; 0xFF = keep
}

func parseImageHeader(b []byte) (magic byte, numSeg byte, flashMode byte, sizeFreq byte, entry uint32, err error) {
	if len(b) < 8 {
		return 0, 0, 0, 0, 0, fmt.Errorf("esprom: image too short to have a header (%d B)", len(b))
	}
	return b[0], b[1], b[2], b[3], binary.LittleEndian.Uint32(b[4:8]), nil
}

// imageChipIDOff is where a packed esp_image_header_t carries its 2-byte LE
// chip ID: 8 bytes of magic/segments/flash-mode/entry, then wp_pin and the
// three spi_pin_drv bytes (esp_app_format.h:79-89). The header is asserted to
// be exactly 24 bytes there, so a short blob is not a header to read.
const imageChipIDOff = 12

// chipIDName maps esp_chip_id_t (esp_app_format.h:20-28) to the words a human
// needs. The ordinary cause of a mismatch here is picking an unrelated ESP-IDF
// build out of a Downloads folder, and "chip ID 13" does not tell that person
// which build they picked, and "ESP32-C6" does.
func chipIDName(id uint16) string {
	switch id {
	case 0x0000:
		return "ESP32"
	case 0x0002:
		return "ESP32-S2"
	case 0x0005:
		return "ESP32-C3"
	case s3ImageChipID:
		return "ESP32-S3"
	case 0x000C:
		return "ESP32-C2"
	case 0x000D:
		return "ESP32-C6"
	case 0x0010:
		return "ESP32-H2"
	case 0x0012:
		return "ESP32-P4"
	case 0x0017:
		return "ESP32-C5"
	case 0xFFFF:
		return "no chip ID"
	}
	return fmt.Sprintf("unknown chip (ID %d)", id)
}

// CheckPlanChipIDs refuses a plan that would write an image built for another
// Espressif chip onto the ESP32-S3.
//
// VerifyS3 proves the chip on the wire is an S3; nothing proved what was about
// to be written into it. s3ImageChipID sat declared and unreferenced for exactly
// this long. The damage this stops is not a wasted flash: an app image for a
// different chip still writes cleanly at its offset, and the orb comes back dead
// with no bootloader complaint, because the ROM accepted the bytes.
//
// Only an image carrying the ESP image magic is checked. A partition table, an
// otadata blob and the assets bin are raw tables with no header, and refusing
// them would break every real flash, so they pass here and the decision to
// leave them alone is tested, not assumed.
func CheckPlanChipIDs(p FlashPlan) error {
	for _, img := range p.Images {
		if len(img.Data) == 0 {
			continue
		}
		if img.Data[0] != espImageMagic {
			continue // headerless by design: partition table, otadata, assets
		}
		if len(img.Data) < imageChipIDOff+2 {
			return fmt.Errorf("%s: ESP image is %d B, too short to carry a chip ID", img.Name, len(img.Data))
		}
		id := binary.LittleEndian.Uint16(img.Data[imageChipIDOff : imageChipIDOff+2])
		if uint16(s3ImageChipID) != id {
			return fmt.Errorf("%s is built for the %s (chip ID %d), not the ESP32-S3 this hub flashes; check the bundle",
				img.Name, chipIDName(id), id)
		}
	}
	return nil
}

// pad4 appends 0xFF so len(b) is a multiple of 4 (esptool pad_to(image,4)).
// pad4 rounds an image up to a 4-byte flash boundary. It always copies: the
// bytes it is handed come from a bundle shared by every flash this hub serves,
// and append would write the padding into that slice's spare capacity —
// corrupting the next flash, and any sibling slice sharing the same backing
// array, from an operation that is supposed to be read-only.
func pad4(b []byte) []byte {
	r := len(b) % 4
	if r == 0 {
		return b
	}
	out := make([]byte, len(b)+4-r)
	copy(out, b)
	return out
}

// patchBootloaderFlashParams replicates esptool _update_image_flash_params for
// the image written at 0x0: rewrite bytes 2-3 with the chosen mode/size/freq
// and, when the image carries an appended SHA256 (extended-header byte 15 ==
// 1), recompute that digest over the pre-digest image. Non-bootloader images
// pass through untouched.
func patchBootloaderFlashParams(raw []byte, offset uint32, p FlashPlan) ([]byte, error) {
	if offset != 0 || len(raw) < 8 {
		return raw, nil
	}
	if raw[0] != espImageMagic {
		return raw, nil // not an image; esptool leaves it and only warns
	}
	flashMode := p.FlashMode
	sizeNibble := raw[3] & 0xF0
	freqNibble := raw[3] & 0x0F
	if p.FlashFreq != 0xFF {
		freqNibble = p.FlashFreq & 0x0F
	}
	if p.FlashSize != 0 {
		sizeNibble = sizeForFlashSize(p.FlashSize)
	}
	newMode, newSF := flashMode, sizeNibble|freqNibble
	shaAppended := len(raw) >= 24 && raw[8+15] == 1
	if raw[2] == newMode && raw[3] == newSF && !shaAppended {
		return raw, nil
	}
	img := make([]byte, len(raw))
	copy(img, raw)
	img[2], img[3] = newMode, newSF
	if !shaAppended || len(img) < 32 {
		return img, nil
	}
	dataLen := len(img) - 32
	sum := sha256.Sum256(img[:dataLen])
	copy(img[dataLen:], sum[:])
	return img, nil
}

// sizeForFlashSize maps a byte count to the header's high-nibble code (esptool
// ESP32_FLASH_SIZES). 16MB == 0x40 is the orb's only case today.
func sizeForFlashSize(bytes uint32) byte {
	switch bytes {
	case 1 << 20:
		return 0x00
	case 2 << 20:
		return 0x10
	case 4 << 20:
		return 0x20
	case 8 << 20:
		return 0x30
	case 16 << 20:
		return 0x40
	case 32 << 20:
		return 0x50
	default:
		return 0x40
	}
}

// Options drive one end-to-end flash session against an already-open port.
type Options struct {
	Plan       FlashPlan
	WantMAC    [6]byte // when HasMAC, the chip's eFuse MAC must equal this
	HasMAC     bool
	OnProgress func(imgIndex, totalImgs int, off uint32, written, total int)
	OnEvent    func(string)
	// OnMAC receives the eFuse base MAC after it is read. The MAC is read even
	// without the gate when this is set: the hub's onboarding flow matches the
	// flashed orb to its LAN address by MAC, so identity is wanted regardless
	// of whether this clone owns the device.
	OnMAC func([6]byte)
	// Wipe lists NVS regions to ROM-erase after every image write, for the
	// explicit factory-reset path. Empty on the ordinary flash: the manifest
	// never touches NVS and credentials are meant to survive a reflash.
	Wipe []EraseTarget
	// ObserveBoot, when set, reads the boot console on the held port right
	// after the session's HardReset. That is the only moment the ROM still owns
	// the USB-Serial-JTAG peripheral, so the reset is honoured and the chip's
	// one-shot provisioning/association banner is not dropped (IDF suppresses
	// console output whenever no host holds the port). The callback sees each
	// line and returns true to stop early once it has its verdict. BootSeconds
	// bounds the read window (default 12 s).
	ObserveBoot func(line string) bool
	BootSeconds int
	// Ctx cancels the session between wire operations: the operator closed the
	// panel, hit Cancel, or the hub is shutting down. Leave it nil for the bench
	// tools, which have no one to cancel them. A cancelled session stops at the
	// next block boundary and returns ErrCancelled without resetting the chip, so
	// the orb stays in the bootloader — a reflash is always safe from there, and a
	// half-written app slot never boots to a partial image.
	Ctx context.Context
}

// Session flashes a whole plan on an open port: enter download mode, sync,
// attach the flash, write every image, then hard-reset into the new app. The
// caller owns the Port lifecycle (so it can also hold the device lock).
func Session(pt *Port, o Options) error {
	logf := func(s string) {
		if o.OnEvent != nil {
			o.OnEvent(s)
		}
	}
	d := New(pt, nil).WithContext(o.Ctx)
	if err := ctxErr(o.Ctx); err != nil {
		return err
	}

	if err := EnterDownloadMode(pt); err != nil {
		return fmt.Errorf("enter download mode: %w", err)
	}
	// The CDC endpoint re-enumerates after the reset, so sync may need retries.
	var syncErr error
	for attempt := 0; attempt < 12; attempt++ {
		if err := d.Sync(); err == nil {
			syncErr = nil
			break
		} else {
			syncErr = err
			if IsCancelled(syncErr) {
				return syncErr
			}
		}
		if err := sleepCtx(o.Ctx, 200*time.Millisecond); err != nil {
			return err
		}
	}
	if syncErr != nil {
		return fmt.Errorf("sync: %w (chip may not have entered download mode)", syncErr)
	}
	logf("synced with ROM bootloader")

	if err := d.VerifyS3(); err != nil {
		return err
	}

	// Refuse a wrong-chip bundle here, before SpiAttach and the erase: the first
	// write is the point of no return for the sectors it lands on, and a plan
	// that cannot boot should never reach the point where the old firmware is
	// gone. Checked over the whole plan at once rather than per image below, so
	// the second image cannot be the one that discovers it.
	if err := CheckPlanChipIDs(o.Plan); err != nil {
		return fmt.Errorf("refusing to flash: %w", err)
	}

	// USB-JTAG/Serial does not feed the RTC/SWD watchdogs, so they reset the
	// chip mid-erase (FLASH_BEGIN status [1,5]). Disable them before the flash
	// exactly as esptool's _post_connect does for this transport.
	if d.UsesUSBJTAGSerial() {
		if err := d.DisableWatchdogs(); err != nil {
			return fmt.Errorf("disable watchdogs: %w", err)
		}
		logf("watchdogs disabled (USB-Serial/JTAG)")
	}

	// MAC gate: prove the chip on this port is the bound orb before erasing.
	// The MAC is also read (ungated) when the caller only wants the identity,
	// so the hub can find the flashed orb on the LAN afterwards.
	if o.HasMAC || o.OnMAC != nil {
		mac, err := d.ReadMAC()
		if err != nil {
			return fmt.Errorf("read MAC from eFuse: %w", err)
		}
		if o.HasMAC && mac != o.WantMAC {
			return fmt.Errorf("wrong orb: chip MAC %02x:%02x:%02x:%02x:%02x:%02x is not the bound device",
				mac[0], mac[1], mac[2], mac[3], mac[4], mac[5])
		}
		if o.HasMAC {
			logf("MAC gate passed")
		}
		if o.OnMAC != nil {
			o.OnMAC(mac)
		}
	}

	if err := d.SpiAttach(); err != nil {
		return err
	}
	if err := d.SpiSetParams(o.Plan.FlashSize); err != nil {
		return err
	}

	n := len(o.Plan.Images)
	for i, img := range o.Plan.Images {
		// One check per image, so walking away between two images stops the
		// session rather than dutifully writing the rest of the manifest.
		if err := ctxErr(o.Ctx); err != nil {
			return err
		}
		raw := img.Data
		if len(raw) == 0 {
			continue
		}
		if img.Offset == 0 {
			if _, _, _, _, _, err := parseImageHeader(raw); err != nil {
				return fmt.Errorf("%s: %w", img.Name, err)
			}
		}
		data, err := patchBootloaderFlashParams(raw, img.Offset, o.Plan)
		if err != nil {
			return err
		}
		data = pad4(data)
		logf(fmt.Sprintf("writing %s (%d B) @0x%X", img.Name, len(data), img.Offset))
		iprog := func(written, total int) {
			if o.OnProgress != nil {
				o.OnProgress(i, n, img.Offset, written, total)
			}
		}
		if err := d.WriteImage(img.Offset, data, iprog); err != nil {
			return fmt.Errorf("flash %s @0x%X: %w", img.Name, img.Offset, err)
		}
	}

	// Deliberate factory reset: erase the NVS partitions the manifest
	// preserves, after the images are safely down. Done inside the same
	// flash-attached session (watchdogs already disabled, SPI already
	// attached) and before the reset into the new app, which then finds an
	// empty config and opens the provisioning AP.
	for _, er := range o.Wipe {
		if err := ctxErr(o.Ctx); err != nil {
			return err
		}
		if er.Offset%4096 != 0 || er.Size%4096 != 0 {
			return fmt.Errorf("erase %s @0x%X+0x%X: not sector aligned", er.Label, er.Offset, er.Size)
		}
		logf(fmt.Sprintf("erasing %s @0x%X (%d B) - config wipe", er.Label, er.Offset, er.Size))
		if err := d.EraseRegion(er.Offset, er.Size); err != nil {
			return fmt.Errorf("erase %s @0x%X: %w", er.Label, er.Offset, err)
		}
	}

	logf("hard reset into new firmware")
	if err := HardReset(pt); err != nil {
		logf(fmt.Sprintf("hard reset note: %v", err))
	}
	// Read the boot console on the port still held across the reset. This is the
	// only reliable capture point for the provisioning banner: the ROM owns the
	// peripheral right here, so the reset landed and IDF has not yet dropped the
	// one-shot lines it suppresses whenever no host is attached.
	if o.ObserveBoot != nil {
		secs := o.BootSeconds
		if secs <= 0 {
			// The default has to outlast the firmware's own scan, not merely
			// its AP banner: orb_net.c gives the radio task 20 s to close a
			// scan (SCAN_TIMEOUT_US), and the boot scan is armed a beat after
			// the AP comes up. A 12 s window regularly cut the PROVSCAN rows
			// off mid-list and left the join dialog's picker empty — the hub
			// stops early the moment it has a verdict, so a longer ceiling
			// only costs time on a board that says nothing at all.
			secs = 28
		}
		observeBootOn(o.Ctx, pt, secs, o.ObserveBoot)
	}
	return nil
}

func baseName(p string) string {
	for i := len(p) - 1; i >= 0; i-- {
		if p[i] == '/' || p[i] == '\\' {
			return p[i+1:]
		}
	}
	return p
}
