package esprom

import (
	"context"
	"crypto/md5"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"strings"
	"time"
)

// Whole-chip erase, decided by one fact about the ROM: esptool marks both
// erase_flash (0xD0) and erase_region (0xD1) @stub_function_only, so neither
// opcode is a ROM command. Uploading a stub to get them is not on the table —
// the point of this package is a flasher with nothing to install — so a chip
// erase uses the erase the ROM genuinely has. FLASH_BEGIN erases
// [offset, offset+erase_size) before it accepts a single byte (esptool
// loader.flash_begin: "Start downloading to Flash (performs an erase)"), so
// offset 0 plus erase_size = the flash capacity plus num_blocks=0 is the whole
// chip and nothing else.
//
// The result is not assumed. FLASH_MD5 (0x13) IS a ROM command — esptool keeps a
// no-stub branch for it — so the erase is checked against the digest an all-0xFF
// flash must have. "Wiped" here means proven, the same standard the flash path
// holds its hashes to.

// md5ReplyLen is the length of the ROM's FLASH_MD5 answer: 32 hex characters. It
// is also the offset of the two status bytes, because on this command the ROM
// sends the digest first (esptool reads the status at data[resp_data_len]).
const md5ReplyLen = 32

// EraseFlash ROM-erases the whole SPI flash, every byte of it, back to 0xFF.
// sizeBytes must be the flash capacity, sector-aligned. This removes the
// first-stage bootloader along with everything else, which is what leaves the
// chip as it shipped: the ROM answers, the screen is black, mDNS says nothing.
func (d *Device) EraseFlash(sizeBytes uint32) error {
	if sizeBytes == 0 || sizeBytes%4096 != 0 {
		return fmt.Errorf("esprom: chip erase of %d bytes is not a sector-aligned flash size", sizeBytes)
	}
	b := make([]byte, 20) // the same five-word body as flashBegin
	binary.LittleEndian.PutUint32(b[0:], sizeBytes)
	binary.LittleEndian.PutUint32(b[4:], 0) // num_blocks = 0: erase only, no data follows
	binary.LittleEndian.PutUint32(b[8:], flashWriteSize)
	binary.LittleEndian.PutUint32(b[12:], 0) // offset 0: from the very first byte
	binary.LittleEndian.PutUint32(b[16:], 0) // encrypted_write = 0
	return d.checkCommand2("chip erase", opFlashBegin, b, 0, timeoutFor("erase", int(sizeBytes)))
}

// EraseFlashVerified erases the whole chip and then proves it blank, over the
// same already-attached SPI session. It returns the digest the ROM read back.
//
// The proof is the point. A FLASH_BEGIN the ROM acknowledges only says the erase
// command was accepted; the one request that can leave a flash half-erased is
// followed by nothing the host can see on the wire. FLASH_MD5 over the whole
// capacity closes that gap: an erased SPI flash reads all 0xFF, so its digest is
// the MD5 of that byte pattern and nothing else can match it.
func (d *Device) EraseFlashVerified(sizeBytes uint32) (string, error) {
	if err := d.EraseFlash(sizeBytes); err != nil {
		return "", err
	}
	got, err := d.FlashMD5(0, sizeBytes)
	if err != nil {
		return got, fmt.Errorf("erase not proven: %w — the ROM accepted the erase, but the blank flash could not be confirmed", err)
	}
	want := blankMD5(sizeBytes)
	if !strings.EqualFold(got, want) {
		return got, fmt.Errorf("flash is not blank after the erase: the ROM reads %s, an erased flash reads %s", got, want)
	}
	return got, nil
}

// FlashMD5 asks the ROM to digest a flash range and returns the lowercase hex
// digest. It is the only way to look inside a flash over the ROM: MEM_READ and
// SPI_READ are stub-only, and this chip answers both with the [1,5] no-stub
// failure, which is why identity comes off the boot console instead.
func (d *Device) FlashMD5(offset, size uint32) (string, error) {
	b := make([]byte, 16)
	binary.LittleEndian.PutUint32(b[0:], offset)
	binary.LittleEndian.PutUint32(b[4:], size)
	_, data, err := d.command(opSpiFlashMD5, b, 0, true, timeoutFor("md5", int(size)))
	if err != nil {
		return "", err
	}
	if len(data) < md5ReplyLen+2 {
		if len(data) >= 2 && data[0] != 0 {
			return "", fmt.Errorf("esprom: flash md5 failed (status %d/%d)", data[0], data[1])
		}
		return "", fmt.Errorf("esprom: flash md5 returned %d bytes, want a %d-character digest", len(data), md5ReplyLen)
	}
	if data[md5ReplyLen] != 0 {
		return "", fmt.Errorf("esprom: flash md5 failed (status %d/%d)", data[md5ReplyLen], data[md5ReplyLen+1])
	}
	return hexDigest(data[:md5ReplyLen]), nil
}

// hexDigest normalises the ROM's digest to lowercase hex. The ROM sends the
// digest as hex text; the shape is passed through if it is not, so an unexpected
// answer compares as different rather than being silently mangled.
func hexDigest(b []byte) string {
	if len(b) == md5ReplyLen {
		printable := true
		for _, c := range b {
			if (c < '0' || c > '9') && (c < 'a' || c > 'f') && (c < 'A' || c > 'F') {
				printable = false
				break
			}
		}
		if printable {
			return strings.ToLower(string(b))
		}
	}
	return hex.EncodeToString(b)
}

// blankMD5 is the digest an erased flash must have: the MD5 of sizeBytes of 0xFF.
// It is computed rather than hardcoded so any capacity checks its own answer.
func blankMD5(sizeBytes uint32) string {
	h := md5.New()
	chunk := make([]byte, 1<<16)
	for i := range chunk {
		chunk[i] = 0xFF
	}
	for remaining := int(sizeBytes); remaining > 0; {
		n := len(chunk)
		if n > remaining {
			n = remaining
		}
		h.Write(chunk[:n])
		remaining -= n
	}
	return hex.EncodeToString(h.Sum(nil))
}

// BlankMD5 is the digest an erased flash of sizeBytes holds: the MD5 of that many
// 0xFF bytes. Exported so a caller that erased a chip outside this package — or a
// bench script checking a wipe from the far side — can compare against the same
// truth instead of a hardcoded string.
func BlankMD5(sizeBytes uint32) string { return blankMD5(sizeBytes) }

// OrbFlashSize is the capacity every orb board carries and every manifest this
// tool ships declares, in bytes: the span a whole-chip erase covers.
const OrbFlashSize = appFlashSize

// WipeOptions drives one whole-chip erase on an already-open port.
type WipeOptions struct {
	// FlashSize is the span to erase. Zero means the orb's own geometry: 16 MB,
	// the capacity every manifest this tool carries declares and the value
	// SpiSetParams already sends for this board.
	FlashSize uint32
	OnEvent   func(string)
	// OnMAC receives the eFuse MAC before anything is erased, so the caller knows
	// which board it is unmaking even if the erase then fails halfway through.
	OnMAC func([6]byte)
	// SkipVerify erases without proving the result. Only a caller that will
	// immediately reflash should set it: an unverified erase is an assertion.
	SkipVerify bool
	Ctx        context.Context
}

// Wipe erases an orb back to a factory-blank chip: download mode, the chip gate,
// the watchdog disable the USB-JTAG transport needs, the MAC read that names the
// board, the whole-chip erase, and a digest check that the flash really reads all
// 0xFF. It then hard-resets, leaving the chip in its ROM with no second stage to
// run — no console, no AP, no announcement, nothing for the fleet card to show.
//
// It repeats Session's preamble rather than sharing it. The erase has no plan to
// gate on, no images to write and no boot console to read, and bending Session to
// cover both shapes puts the path that ships at risk for the sake of tidiness.
func Wipe(pt *Port, o WipeOptions) error {
	size := o.FlashSize
	if size == 0 {
		size = appFlashSize
	}
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

	// The chip gate is not politeness here. Everything gets erased, so the board
	// on the wire had better be the ESP32-S3 the operator meant to wipe.
	if err := d.VerifyS3(); err != nil {
		return err
	}

	// Same reason as the flash path: USB-Serial/JTAG does not feed the RTC/SWD
	// watchdogs, and a 16 MB erase blocks long enough for them to reset the chip
	// mid-command (the FLASH_BEGIN status [1,5] the field notes record).
	if d.UsesUSBJTAGSerial() {
		if err := d.DisableWatchdogs(); err != nil {
			return fmt.Errorf("disable watchdogs: %w", err)
		}
		logf("watchdogs disabled (USB-Serial/JTAG)")
	}

	if o.OnMAC != nil {
		mac, err := d.ReadMAC()
		if err != nil {
			return fmt.Errorf("read MAC from eFuse: %w", err)
		}
		o.OnMAC(mac)
	}

	if err := d.SpiAttach(); err != nil {
		return err
	}
	if err := d.SpiSetParams(size); err != nil {
		return err
	}

	if err := ctxErr(o.Ctx); err != nil {
		return err
	}
	logf(fmt.Sprintf("erasing %d bytes from 0x0 - every image, every partition, the factory config", size))
	if o.SkipVerify {
		if err := d.EraseFlash(size); err != nil {
			return err
		}
	} else {
		got, err := d.EraseFlashVerified(size)
		if err != nil {
			return err
		}
		logf("verified blank, md5 " + got)
	}

	// Reset so the board is left where a blank chip belongs — in its ROM, waiting
	// to be flashed — rather than parked in download mode by this session.
	logf("hard reset - there is nothing installed to boot into")
	if err := HardReset(pt); err != nil {
		logf(fmt.Sprintf("hard reset note: %v", err))
	}
	return nil

}
