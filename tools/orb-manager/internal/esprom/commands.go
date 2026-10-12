package esprom

import (
	"context"
	"encoding/binary"
	"fmt"
	"time"
)

// ROM bootloader opcodes (esptool loader.py ESPLoader).
const (
	opFlashBegin   = 0x02
	opFlashData    = 0x03
	opFlashEnd     = 0x04
	opMemBegin     = 0x05
	opMemEnd       = 0x06
	opMemData      = 0x07
	opSync         = 0x08
	opWriteReg     = 0x09
	opReadReg      = 0x0A
	opSpiSetParams = 0x0B
	opSpiAttach    = 0x0D
	opChangeBaud   = 0x0F
	opSpiFlashMD5  = 0x13
	opGetSecurity  = 0x14
)

const (
	flashWriteSize = 0x400 // ROM FLASH_WRITE_SIZE (esptool 0x400)
	checksumInit   = 0xEF
	espImageMagic  = 0xE9 // first byte of an ESP app image
	syncPayload    = "\x07\x07\x12\x20"
)

// ESP32-S3 specifics (targets/esp32s3.py).
const (
	s3MagicValue       = 0x00000009
	s3ImageChipID      = 9
	s3MacEFuseReg      = 0x60007044
	s3SpiAttachArg     = 0 // default on-board SPI flash pins
	chipDetectMagicReg = 0x40001000
	s3SpiBlk           = 64 * 1024
	s3SpiSec           = 4 * 1024
	s3SpiPage          = 256
)

// timeoutFor mirrors esptool timeout_per_mb for the ROM erase/write path.
func timeoutFor(op string, bytes int) time.Duration {
	mb := (bytes + 1024*1024 - 1) / (1024 * 1024)
	if mb < 1 {
		mb = 1
	}
	switch op {
	case "erase":
		return time.Duration(mb) * 50 * time.Second
	default:
		d := time.Duration(mb) * 20 * time.Second
		if d < 3*time.Second {
			d = 3 * time.Second
		}
		return d
	}
}

// Device is a connection to a chip's ROM bootloader over a serial ReadWriter.
// It owns nothing beyond the wire: the caller opens/closes the port.
type Device struct {
	rw     ReadWriter
	dec    slipDecoder
	chipID uint32
	logf   func(string, ...any)
	// ctx carries the operator's cancellation, if any. nil means "run to
	// completion", which is what the headless bench tools ask for.
	ctx context.Context
}

// New wraps an open serial port. Nothing is sent until Sync is called.
func New(rw ReadWriter, logf func(string, ...any)) *Device {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	return &Device{rw: rw, logf: logf}
}

// WithContext attaches the cancellation that readPacket and command honour on the
// next loop turn. It returns the Device so a caller can chain it onto New.
func (d *Device) WithContext(ctx context.Context) *Device {
	d.ctx = ctx
	return d
}

// readPacket reads one SLIP packet, honouring an absolute deadline and the
// operator's cancellation. The serial read itself carries a 500 ms timeout
// (OpenPort), so one loop turn is bounded and a cancel lands within about that
// long without this loop ever blocking on a dead chip.
func (d *Device) readPacket(deadline time.Time) ([]byte, error) {
	tmp := make([]byte, 512)
	for {
		if err := ctxErr(d.ctx); err != nil {
			return nil, err
		}
		if time.Now().After(deadline) {
			return nil, ErrTimeout
		}
		n, err := d.rw.Read(tmp)
		if n > 0 {
			if pkt, ok := d.dec.Feed(tmp[:n]); ok {
				return pkt, nil
			}
		}
		if err != nil {
			if ne, ok := err.(interface{ Timeout() bool }); ok && ne.Timeout() {
				continue
			}
			return nil, err
		}
	}
}

// command sends one ESPROM request and returns (value, body).
// Frame: dir(1) cmd(1) size(2) chk(4) then the 16-byte ESPROM body (esptool command()).
func (d *Device) command(op uint8, body []byte, chk uint32, wait bool, timeout time.Duration) (uint32, []byte, error) {
	if err := ctxErr(d.ctx); err != nil {
		return 0, nil, err
	}
	hdr := make([]byte, 8)
	hdr[0] = 0x00
	hdr[1] = op
	binary.LittleEndian.PutUint16(hdr[2:], uint16(len(body)))
	binary.LittleEndian.PutUint32(hdr[4:], chk)
	pkt := append(hdr, body...)
	if _, err := d.rw.Write(slipEncode(pkt)); err != nil {
		return 0, nil, fmt.Errorf("esprom: write op 0x%02x: %w", op, err)
	}
	if !wait {
		return 0, nil, nil
	}
	deadline := time.Now().Add(timeout)
	for retry := 0; retry < 100; retry++ {
		p, err := d.readPacket(deadline)
		if err != nil {
			return 0, nil, err
		}
		if len(p) < 8 {
			continue
		}
		if p[0] != 1 {
			continue
		}
		if op == 0 || p[1] == op {
			return binary.LittleEndian.Uint32(p[4:]), p[8:], nil
		}
	}
	return 0, nil, fmt.Errorf("esprom: response did not match op 0x%02x", op)
}

// checkCommand runs command and validates the 2-byte status, returning value.
func (d *Device) checkCommand(desc string, op uint8, body []byte, chk uint32, timeout time.Duration) (uint32, error) {
	val, data, err := d.command(op, body, chk, true, timeout)
	if err != nil {
		return 0, err
	}
	if len(data) < 2 {
		return 0, fmt.Errorf("esprom: %s: short status (%d bytes)", desc, len(data))
	}
	if data[0] != 0 {
		return 0, fmt.Errorf("esprom: %s failed (status %d/%d)", desc, data[0], data[1])
	}
	return val, nil
}

// Sync performs the ROM SYNC handshake: a SYNC packet with the magic preamble
// + 32 pad bytes, then 7 empty polls (esptool ESPLoader.sync).
func (d *Device) Sync() error {
	body := []byte(syncPayload)
	for i := 0; i < 32; i++ {
		body = append(body, 0x55)
	}
	if _, _, err := d.command(opSync, body, 0, true, 3*time.Second); err != nil {
		if IsCancelled(err) {
			return err
		}
		return ErrNoSync
	}
	for i := 0; i < 7; i++ {
		if _, _, err := d.command(opSync, nil, 0, true, 3*time.Second); err != nil {
			if IsCancelled(err) {
				return err
			}
			return ErrNoSync
		}
	}
	return nil
}

// ReadReg reads a 32-bit register/memory address from the ROM.
func (d *Device) ReadReg(addr uint32) (uint32, error) {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], addr)
	return d.checkCommand("read target memory", opReadReg, b[:], 0, 3*time.Second)
}

// VerifyS3 reads the chip detect magic and rejects a non-ESP32-S3.
func (d *Device) VerifyS3() error {
	v, err := d.ReadReg(chipDetectMagicReg)
	if err != nil {
		_, data, serr := d.command(opGetSecurity, nil, 0, true, 3*time.Second)
		if serr != nil {
			return fmt.Errorf("esprom: chip detect: %w (security: %v)", err, serr)
		}
		if len(data) >= 16 {
			d.chipID = binary.LittleEndian.Uint32(data[len(data)-16:])
		}
		return nil
	}
	if v != s3MagicValue {
		return fmt.Errorf("%w: magic 0x%08x", ErrChipMismatch, v)
	}
	return nil
}

// ReadMAC returns the base MAC from eFuse via the ROM (targets/esp32s3.py).
func (d *Device) ReadMAC() ([6]byte, error) {
	var mac [6]byte
	m0, err := d.ReadReg(s3MacEFuseReg)
	if err != nil {
		return mac, err
	}
	m1, err := d.ReadReg(s3MacEFuseReg + 4)
	if err != nil {
		return mac, err
	}
	raw := make([]byte, 8)
	binary.BigEndian.PutUint32(raw[0:], m1)
	binary.BigEndian.PutUint32(raw[4:], m0)
	copy(mac[:], raw[2:8])
	return mac, nil
}

// WriteReg writes a 32-bit register with a mask and optional post-delay.
// Body is <IIII addr,value,mask,delay_us> — esptool ESPLoader.write_reg.
func (d *Device) WriteReg(addr, val, mask uint32) error {
	b := make([]byte, 16)
	binary.LittleEndian.PutUint32(b[0:], addr)
	binary.LittleEndian.PutUint32(b[4:], val)
	binary.LittleEndian.PutUint32(b[8:], mask)
	binary.LittleEndian.PutUint32(b[12:], 0)
	return d.checkCommand2("write target memory", opWriteReg, b, 0, 3*time.Second)
}

// DisableWatchdogs is required before any erase/write on an ESP32-S3 driven
// over native USB-Serial-JTAG. Those two watchdogs are not fed by the ROM in
// this mode and reset the chip partway through FLASH_BEGIN, surfacing as the
// erase failing with status [1,5]. The register sequence is esptool
// ESP32S3ROM.disable_watchdogs() verbatim (targets/esp32s3.py:331).
const (
	rtcCntlBase       = 0x60008000
	rtcWDTWProtect    = rtcCntlBase + 0x0B0
	rtcWDTConfig0     = rtcCntlBase + 0x098
	rtcWDTWKey        = 0x50D83AA1
	rtcSWDWProtect    = rtcCntlBase + 0x0B8
	rtcSWDConf        = rtcCntlBase + 0x0B4
	rtcSWDAutoFeedEn  = 1 << 31
	rtcSWDWKey        = 0x8F1D312A
	uartdevBufNo      = 0x3FCEF14C // ROM .bss: which console is in use
	uartdevBufNoUSBJS = 4          // value when USB-JTAG/Serial is the console
)

func (d *Device) DisableWatchdogs() error {
	if err := d.WriteReg(rtcWDTWProtect, rtcWDTWKey, 0xFFFFFFFF); err != nil {
		return err
	}
	if err := d.WriteReg(rtcWDTConfig0, 0, 0xFFFFFFFF); err != nil { // RTC WDT off
		return err
	}
	if err := d.WriteReg(rtcWDTWProtect, 0, 0xFFFFFFFF); err != nil { // relock
		return err
	}
	if err := d.WriteReg(rtcSWDWProtect, rtcSWDWKey, 0xFFFFFFFF); err != nil {
		return err
	}
	conf, err := d.ReadReg(rtcSWDConf)
	if err != nil {
		return err
	}
	if err := d.WriteReg(rtcSWDConf, conf|rtcSWDAutoFeedEn, 0xFFFFFFFF); err != nil {
		return err
	}
	return d.WriteReg(rtcSWDWProtect, 0, 0xFFFFFFFF)
}

// UsesUSBJTAGSerial reads the ROM console selector; true means the chip is
// being driven over native USB-Serial-JTAG and needs the watchdogs disabled.
func (d *Device) UsesUSBJTAGSerial() bool {
	v, err := d.ReadReg(uartdevBufNo)
	return err == nil && (v&0xFF) == uartdevBufNoUSBJS
}

// SpiAttach enables the SPI flash pins (ROM path passes is_legacy=0).
func (d *Device) SpiAttach() error {
	b := make([]byte, 8)
	binary.LittleEndian.PutUint32(b[0:], s3SpiAttachArg) // + 4 reserved bytes, zeroed
	return d.checkCommand2("configure SPI flash pins", opSpiAttach, b, 0, 3*time.Second)
}

// SpiSetParams tells the ROM the flash geometry. 16MB is the orb's only size.
func (d *Device) SpiSetParams(sizeBytes uint32) error {
	b := make([]byte, 24)
	binary.LittleEndian.PutUint32(b[0:], 0) // fl_id
	binary.LittleEndian.PutUint32(b[4:], sizeBytes)
	binary.LittleEndian.PutUint32(b[8:], s3SpiBlk)
	binary.LittleEndian.PutUint32(b[12:], s3SpiSec)
	binary.LittleEndian.PutUint32(b[16:], s3SpiPage)
	binary.LittleEndian.PutUint32(b[20:], 0xFFFF) // status_mask
	return d.checkCommand2("SPI flash set params", opSpiSetParams, b, 0, 3*time.Second)
}

// checkCommand2 runs checkCommand, discarding the value.
func (d *Device) checkCommand2(desc string, op uint8, body []byte, chk uint32, timeout time.Duration) error {
	_, err := d.checkCommand(desc, op, body, chk, timeout)
	return err
}

func xorChecksum(data []byte, state byte) byte {
	for _, b := range data {
		state ^= b
	}
	return state
}

// flashBegin erases and begins writing size bytes at the sector-aligned
// offset. The ROM erases up front, so this blocks for many seconds on big
// images; the ReadWriter must not impose a shorter read deadline than
// timeoutFor returns.
//
// The parameter block is FIVE uint32s, not four. esptool appends the 5th
// "encrypted_write" word whenever the chip advertises SUPPORTS_ENCRYPTED_FLASH,
// and the ESP32-S3 ROM does — omitting it (a 16-byte body) makes the ROM read a
// garbage fifth word and reject the erase with status [1,5]. erase_size is the
// image length verbatim (esptool get_erase_size returns size on ESP32-S3), and
// encrypted is 0 because the orb's manifest is "encrypted": false throughout.
func (d *Device) flashBegin(size int, offset uint32) error {
	numBlocks := uint32((size + flashWriteSize - 1) / flashWriteSize)
	b := make([]byte, 20)
	binary.LittleEndian.PutUint32(b[0:], uint32(size)) // erase_size = padded image len
	binary.LittleEndian.PutUint32(b[4:], numBlocks)
	binary.LittleEndian.PutUint32(b[8:], flashWriteSize)
	binary.LittleEndian.PutUint32(b[12:], offset)
	binary.LittleEndian.PutUint32(b[16:], 0) // encrypted_write = 0
	return d.checkCommand2("enter Flash download mode", opFlashBegin, b, 0, timeoutFor("erase", size))
}

// flashBlock writes one 1KB block with its sequence number. data must be
// exactly flashWriteSize bytes (padded with 0xFF by the caller).
func (d *Device) flashBlock(data []byte, seq uint32) error {
	body := make([]byte, 16+len(data))
	binary.LittleEndian.PutUint32(body[0:], uint32(len(data)))
	binary.LittleEndian.PutUint32(body[4:], seq)
	copy(body[16:], data)
	chk := uint32(xorChecksum(data, checksumInit))
	return d.checkCommand2(fmt.Sprintf("write block seq %d", seq), opFlashData, body, chk, timeoutFor("write", len(data)))
}

// flashEnd leaves flash mode. The ESP32-S3 ROM does not implement FLASH_END —
// esptool only calls it on the stub path — so the ROM answers status [1,6]
// (ESP_BAD_COMMAND). We never send it on the no-stub path; ending the session
// is a physical hard reset via RTS instead. It stays defined for the stub path
// an orb never takes, and for parity with the ROM command table.
func (d *Device) flashEnd(reboot bool) error {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, boolInt(!reboot))
	return d.checkCommand2("leave Flash mode", opFlashEnd, b, 0, 3*time.Second)
}

func boolInt(v bool) uint32 {
	if v {
		return 1
	}
	return 0
}

// CaptureBoot asserts DTR, then forwards every line the boot console emits for
// secs. It opens no port: the caller holds one already. ctx aborts the read early
// for a caller that has been cancelled; a nil ctx reads the whole window, which is
// what the identity probe wants. Exported for that probe, which is the only way to
// learn what an orb is running: an experiment against live hardware (2026-10-12,
// two orbs) confirmed the ESP32-S3 ROM answers both MEM_READ and SPI_READ(0x10)
// with the [1,5] no-stub failure, so flash bytes — and therefore the running app's
// esp_app_desc_t — cannot be read over the ROM at all. The bootloader prints that
// same descriptor on the console about 0.7 s after a cold boot, which is what this
// captures.
func CaptureBoot(ctx context.Context, pt *Port, secs int) []string {
	var out []string
	observeBootOn(ctx, pt, secs, func(line string) bool {
		out = append(out, line)
		return false
	})
	return out
}

// EraseRegion ROM-erases a sector-aligned flash range (FLASH_ERASE_REGION,
// esptool loader <IIH: offset, size, timeout — the trailing word is read as
// the u16 timeout and 0 lets the ROM use its default). It is how the hub
// wipes the NVS partitions the full-device manifest deliberately never
// touches, so a returning orb can be sent back to provisioning on purpose
// rather than by accident.
// EraseRegion erases a sector-aligned flash range the only way the ESP32-S3
// ROM allows without a stub: FLASH_BEGIN itself. The ROM's FLASH_BEGIN erases
// [offset, offset+erase_size) up front (esptool loader.py flash_begin: "Start
// downloading to Flash (performs an erase)") and num_blocks=0 means the host
// then writes nothing — esptool's run() uses that same zero form
// (flash_begin(0, 0)) on the no-stub path. ESP_ERASE_REGION (0xD1) is
// @stub_function_only in esptool and never reaches the ROM; op 0x10 is not an
// erase at all and the ROM answers [1,5]. It is how the hub wipes the NVS
// partitions the full-device manifest deliberately never touches, so a
// returning orb can be sent back to provisioning on purpose rather than by
// accident.
func (d *Device) EraseRegion(offset, size uint32) error {
	if offset%4096 != 0 || size%4096 != 0 {
		return fmt.Errorf("esprom: erase region 0x%X+0x%X not sector aligned", offset, size)
	}
	b := make([]byte, 20)                      // same five-word body as flashBegin
	binary.LittleEndian.PutUint32(b[0:], size) // erase_size
	binary.LittleEndian.PutUint32(b[4:], 0)    // num_blocks = 0: erase only
	binary.LittleEndian.PutUint32(b[8:], flashWriteSize)
	binary.LittleEndian.PutUint32(b[12:], offset)
	binary.LittleEndian.PutUint32(b[16:], 0) // encrypted_write = 0
	return d.checkCommand2(fmt.Sprintf("erase region 0x%X+0x%X", offset, size), opFlashBegin, b, 0, timeoutFor("erase", int(size)))
}

// Progress reports bytes written so far and the total for the current image.
type Progress func(written, total int)

// WriteImage erases and writes one contiguous blob at a sector-aligned
// offset: the ROM-path flashBegin/flashBlock* sequence. No FLASH_END follows —
// the ESP32-S3 ROM does not implement it (see flashEnd). The caller ends the
// whole plan with a hard reset.
func (d *Device) WriteImage(offset uint32, data []byte, p Progress) error {
	if err := ctxErr(d.ctx); err != nil {
		return err
	}
	if err := d.flashBegin(len(data), offset); err != nil {
		return err
	}
	total := len(data)
	for seq := 0; seq*flashWriteSize < total; seq++ {
		// Checked between blocks only. The ROM takes a whole 1 KB block or
		// rejects it, so stopping here leaves the flash consistent and the next
		// full flash rewrites it from FLASH_BEGIN.
		if err := ctxErr(d.ctx); err != nil {
			return err
		}
		blk := data[seq*flashWriteSize:]
		if len(blk) > flashWriteSize {
			blk = blk[:flashWriteSize]
		}
		pad := make([]byte, flashWriteSize)
		copy(pad, blk)
		for i := len(blk); i < flashWriteSize; i++ {
			pad[i] = 0xFF // pad the final block with erased-state bytes
		}
		if err := d.flashBlock(pad, uint32(seq)); err != nil {
			return err
		}
		if p != nil {
			w := (seq + 1) * flashWriteSize
			if w > total {
				w = total
			}
			p(w, total)
		}
	}
	return nil
}
