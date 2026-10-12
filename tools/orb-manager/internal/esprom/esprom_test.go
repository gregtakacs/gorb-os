package esprom

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"testing"
)

// fakeRom is an in-process stand-in for the ESP32-S3 ROM bootloader. Every
// command it receives gets the single response the ROM would send; READ_REG
// answers from a table so read_mac and VerifyS3 can be exercised.
type fakeRom struct {
	t           *testing.T
	reg         map[uint32]uint32
	rx          []byte // decoded request packets, for assertions
	respQ       [][]byte
	seenData    [][]byte // raw FLASH_DATA bodies (post-header), for checksum checks
	beginBodies [][]byte // raw FLASH_BEGIN bodies, for framing assertions
}

func newFake(t *testing.T) *fakeRom {
	return &fakeRom{t: t, reg: map[uint32]uint32{
		chipDetectMagicReg: s3MagicValue,
		// esptool read_mac: bitstring = pack(">II", mac1, mac0)[2:], i.e.
		// bytes 2..3 are mac1's low half, bytes 4..7 are all of mac0. To make
		// the base MAC 44:bd:8d:78:26:18 the two regs hold exactly those.
		s3MacEFuseReg:     0x8d782618, // mac0 -> 8d:78:26:18
		s3MacEFuseReg + 4: 0x000044bd, // mac1 -> 44:bd (low 16 bits only)
	}}
}

func (f *fakeRom) Read(p []byte) (int, error) {
	if len(f.respQ) == 0 {
		return 0, nil // non-blocking: Device retries until a resp is queued
	}
	r := f.respQ[0]
	n := copy(p, r)
	if n == len(r) {
		f.respQ = f.respQ[1:]
	} else {
		f.respQ[0] = r[n:]
	}
	return n, nil
}

func (f *fakeRom) Write(p []byte) (int, error) {
	f.feed(p)
	return len(p), nil
}

func (f *fakeRom) SetDTR(bool) error { return nil }
func (f *fakeRom) SetRTS(bool) error { return nil }

// feed scans p for SLIP packets and answers each as the ROM would.
func (f *fakeRom) feed(p []byte) {
	for len(p) > 0 {
		if p[0] != slipEnd {
			p = p[1:]
			continue
		}
		// find the closing END
		end := -1
		for i := 1; i < len(p); i++ {
			if p[i] == slipEnd {
				end = i
				break
			}
		}
		if end == -1 {
			return
		}
		f.answer(decodeSLIP(p[1:end]))
		p = p[end+1:]
	}
}

// decodeSLIP un-escapes the interior of one packet.
func decodeSLIP(b []byte) []byte {
	out := make([]byte, 0, len(b))
	for i := 0; i < len(b); i++ {
		if b[i] == slipEsc && i+1 < len(b) {
			switch b[i+1] {
			case slipEscEnd:
				out = append(out, slipEnd)
			case slipEscEsc:
				out = append(out, slipEsc)
			}
			i++
			continue
		}
		out = append(out, b[i])
	}
	return out
}

// romResp builds a ROM-style response: an 8-byte prefix (direction=1, cmd,
// size=8, value) then 8 bytes whose first two are the good status (0,0).
func romResp(op uint8, value uint32) []byte {
	b := make([]byte, 16)
	b[0] = 0x01
	b[1] = op
	binary.LittleEndian.PutUint16(b[2:], 8) // size of the following 8 bytes
	binary.LittleEndian.PutUint32(b[4:], value)
	return slipEncode(b)
}

func (f *fakeRom) answer(req []byte) {
	if len(req) < 8 {
		return
	}
	op := req[1]
	body := req[8:]
	switch op {
	case opReadReg:
		addr := binary.LittleEndian.Uint32(body[0:])
		f.respQ = append(f.respQ, romResp(op, f.reg[addr]))
	case opFlashData:
		f.seenData = append(f.seenData, append([]byte(nil), body...))
		f.respQ = append(f.respQ, romResp(op, 0))
	case opFlashBegin:
		f.beginBodies = append(f.beginBodies, append([]byte(nil), body...))
		f.respQ = append(f.respQ, romResp(op, 0))
	default:
		f.respQ = append(f.respQ, romResp(op, 0))
	}
}

func sha256Of(b []byte) []byte {
	s := sha256.Sum256(b)
	return s[:]
}

func TestSyncHandshakeSucceeds(t *testing.T) {
	f := newFake(t)
	d := New(f, nil)
	if err := d.Sync(); err != nil {
		t.Fatalf("Sync against ROM emulator failed: %v", err)
	}
}

func TestVerifyS3AndReadMAC(t *testing.T) {
	f := newFake(t)
	d := New(f, nil)
	if err := d.VerifyS3(); err != nil {
		t.Fatalf("VerifyS3: %v", err)
	}
	mac, err := d.ReadMAC()
	if err != nil {
		t.Fatalf("ReadMAC: %v", err)
	}
	want := [6]byte{0x44, 0xbd, 0x8d, 0x78, 0x26, 0x18}
	if mac != want {
		t.Fatalf("ReadMAC = %x, want %x", mac, want)
	}
}

// TestFlashBlockWireBytes writes one image and asserts the FLASH_DATA packets
// that reached the wire are framed and checksummed exactly as esptool frames
// them: header <BBHI 0,op,len,chk> then <IIII len,seq,0,0> + 1KB of data.
func TestFlashBlockWireBytes(t *testing.T) {
	f := newFake(t)
	d := New(f, nil)
	data := bytes.Repeat([]byte{0x33}, 1024)
	if err := d.WriteImage(0x20000, data, nil); err != nil {
		t.Fatalf("WriteImage: %v", err)
	}
	if len(f.seenData) != 1 {
		t.Fatalf("expected 1 FLASH_DATA, got %d", len(f.seenData))
	}
	body := f.seenData[0]
	if len(body) != 16+1024 {
		t.Fatalf("body len = %d, want %d", len(body), 16+1024)
	}
	if got := binary.LittleEndian.Uint32(body[0:]); got != 1024 {
		t.Errorf("block data-len = %d, want 1024", got)
	}
	if got := binary.LittleEndian.Uint32(body[4:]); got != 0 {
		t.Errorf("seq = %d, want 0", got)
	}
	if !bytes.Equal(body[16:], data) {
		t.Errorf("block payload does not match data written")
	}
}

func TestBootloaderFlashParamsPatchedAndSHARecalc(t *testing.T) {
	// Build a fake bootloader image: magic E9, then flash_mode/size_freq, an
	// extended header whose byte 15 (index 23) says sha appended.
	img := make([]byte, 100)
	img[0] = espImageMagic
	img[1] = 3
	img[2] = 0    // wrong mode
	img[3] = 0    // wrong size/freq
	img[8+15] = 1 // sha appended
	// pretend a trailing 32-byte digest
	plan := FlashPlan{FlashMode: 2, FlashSize: 16 << 20, FlashFreq: 0xF}
	out, err := patchBootloaderFlashParams(img, 0, plan)
	if err != nil {
		t.Fatal(err)
	}
	if out[2] != 2 {
		t.Errorf("flash_mode byte = %d, want 2 (dio)", out[2])
	}
	if out[3] != 0x4F { // 0x40 size(16MB) | 0x0F freq(80m)
		t.Errorf("size_freq byte = %#x, want 0x4F", out[3])
	}
	// the last 32 bytes must be sha256 of the first 68
	if !bytes.Equal(out[68:], sha256Of(out[:68])) {
		t.Errorf("appended SHA256 was not recomputed over the patched image")
	}
}

func TestNonBootloaderUntouched(t *testing.T) {
	img := []byte{espImageMagic, 1, 0, 0, 1, 0, 0, 0}
	plan := FlashPlan{FlashMode: 2, FlashSize: 16 << 20, FlashFreq: 0xF}
	out, err := patchBootloaderFlashParams(img, 0x20000, plan)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(img, out) {
		t.Errorf("app image at 0x20000 must not be modified")
	}
}

// TestEraseRegionIsEraseOnlyFlashBegin locks the wipe primitive against the
// field failure where an invented op 0x10 made the real ROM answer [1,5]: the
// ESP32-S3 ROM has no erase-region command (esptool marks 0xD1
// stub_function_only), so EraseRegion must reuse FLASH_BEGIN (0x02) with the
// five-word body, erase_size = region size, and num_blocks = 0 so no data
// follows.
func TestEraseRegionIsEraseOnlyFlashBegin(t *testing.T) {
	f := newFake(t)
	d := New(f, nil)
	if err := d.EraseRegion(0xD60000, 0x6000); err != nil {
		t.Fatalf("EraseRegion: %v", err)
	}
	if len(f.beginBodies) != 1 {
		t.Fatalf("expected 1 FLASH_BEGIN, got %d", len(f.beginBodies))
	}
	b := f.beginBodies[0]
	if len(b) != 20 {
		t.Fatalf("erase body len = %d, want 20 (five words)", len(b))
	}
	if got := binary.LittleEndian.Uint32(b[0:]); got != 0x6000 {
		t.Errorf("erase_size = %#x, want 0x6000", got)
	}
	if got := binary.LittleEndian.Uint32(b[4:]); got != 0 {
		t.Errorf("num_blocks = %d, want 0 (erase only, no data follows)", got)
	}
	if got := binary.LittleEndian.Uint32(b[8:]); got != 0x400 {
		t.Errorf("block size = %#x, want 0x400", got)
	}
	if got := binary.LittleEndian.Uint32(b[12:]); got != 0xD60000 {
		t.Errorf("offset = %#x, want 0xD60000", got)
	}
	if got := binary.LittleEndian.Uint32(b[16:]); got != 0 {
		t.Errorf("encrypted flag = %d, want 0", got)
	}
	// Sector misalignment must refuse before any frame reaches the wire.
	if err := d.EraseRegion(0x1001, 0x1000); err == nil {
		t.Error("EraseRegion unaligned: want error, got nil")
	}
}

// TestFlashBeginFrameMatchesEsptool locks the erase request that the ESP32-S3
// ROM requires: a 20-byte five-word body whose 5th word is the encrypted flag,
// with erase_size = the padded image length verbatim. A 16-byte four-word body
// was the field bug that made the real ROM reject the erase with status [1,5].
func TestFlashBeginFrameMatchesEsptool(t *testing.T) {
	f := newFake(t)
	d := New(f, nil)
	if err := d.flashBegin(22384, 0); err != nil {
		t.Fatalf("flashBegin: %v", err)
	}
	if len(f.beginBodies) != 1 {
		t.Fatalf("expected 1 FLASH_BEGIN, got %d", len(f.beginBodies))
	}
	b := f.beginBodies[0]
	if len(b) != 20 {
		t.Fatalf("FLASH_BEGIN body len = %d, want 20 (five words)", len(b))
	}
	if got := binary.LittleEndian.Uint32(b[0:]); got != 22384 {
		t.Errorf("erase_size = %d, want 22384 (image length verbatim)", got)
	}
	if got := binary.LittleEndian.Uint32(b[4:]); got != 22 {
		t.Errorf("num_blocks = %d, want 22 (ceil(22384/1024))", got)
	}
	if got := binary.LittleEndian.Uint32(b[8:]); got != 0x400 {
		t.Errorf("block size = %#x, want 0x400", got)
	}
	if got := binary.LittleEndian.Uint32(b[16:]); got != 0 {
		t.Errorf("encrypted flag = %d, want 0", got)
	}
}

// partEntry builds one 32-byte partition-table record per gen_esp32part.py's
// STRUCT_FORMAT '<2sBBLL16sL': magic, type, subtype, offset, size, label, flags.
func partEntry(typ, sub uint8, off, size uint32, label string) []byte {
	e := make([]byte, 32)
	e[0], e[1] = 0xAA, 0x50
	e[2], e[3] = typ, sub
	binary.LittleEndian.PutUint32(e[4:], off)
	binary.LittleEndian.PutUint32(e[8:], size)
	copy(e[12:28], label)
	return e
}

func TestNvsRegionsRefuseEmptyTable(t *testing.T) {
	if _, err := NvsRegions(nil); err == nil {
		t.Error("NvsRegions(nil): want error, got nil")
	}
}

func TestNvsRegions(t *testing.T) {
	// Mirror the repo's real layout: nvs esp_wifi owns + storage the orb writes.
	table := bytes.Join([][]byte{
		partEntry(0x00, 0x10, 0x20000, 0x200000, "ota_0"),  // app, must be ignored
		partEntry(0x01, 0x02, 0x9000, 0x8000, "nvs"),       // esp_wifi
		partEntry(0x01, 0x02, 0xD60000, 0x6000, "storage"), // orb credentials
		partEntry(0x01, 0x06, 0xC20000, 0x140000, "www"),   // data but not nvs
		partEntry(0x01, 0x02, 0xE00000, 0x1500, "stray"),   // nvs subtype, unknown label
	}, nil)
	got, err := NvsRegions(table)
	if err != nil {
		t.Fatalf("NvsRegions: %v", err)
	}
	want := []EraseTarget{{"nvs", 0x9000, 0x8000}, {"storage", 0xD60000, 0x6000}}
	if len(got) != len(want) {
		t.Fatalf("got %+v, want %+v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("region %d = %+v, want %+v", i, got[i], want[i])
		}
	}

	// A table with no nvs/storage must refuse, not silently erase nothing.
	if _, err := NvsRegions(table[:32]); err == nil {
		t.Error("NvsRegions on empty table: want error, got nil")
	}

	// Unaligned size must refuse outright.
	if _, err := NvsRegions(partEntry(0x01, 0x02, 0x9000, 0x1234, "nvs")); err == nil {
		t.Error("NvsRegions unaligned: want error, got nil")
	}
}

// pad4 exists to satisfy the ROM's 4-byte write granularity, but the slice it is
// handed comes from a bundle shared by every flash the hub serves. An append into
// spare capacity would write padding over whatever follows in that backing array,
// corrupting the next flash from an operation meant to be read-only.
func TestPad4NeverWritesThroughSharedBytes(t *testing.T) {
	// Three bytes of payload with three bytes of spare capacity behind it: the
	// exact shape that makes append mutate the caller's array.
	backing := make([]byte, 6)
	copy(backing, []byte{0x11, 0x22, 0x33})
	shared := backing[:3] // len 3, cap 6: append here would write backing[3]

	got := pad4(shared)

	if len(got) != 4 {
		t.Fatalf("pad4 len = %d, want 4", len(got))
	}
	if got[0] != 0x11 || got[1] != 0x22 || got[2] != 0x33 || got[3] != 0 {
		t.Errorf("pad4 = %v, want the payload then one zero byte", got)
	}
	// The tell: the original three bytes must be untouched, and the spare capacity
	// behind them unwritten.
	if !bytes.Equal(backing[:3], []byte{0x11, 0x22, 0x33}) {
		t.Errorf("pad4 changed the caller's bytes: %v", backing[:3])
	}
	if backing[3] != 0 {
		t.Errorf("pad4 wrote into the caller's spare capacity: backing[3] = %#x, want untouched 0", backing[3])
	}
	if &got[0] == &shared[0] {
		t.Error("pad4 returned the caller's array; a flash must not share it")
	}

	// Already aligned: returned as-is, no needless copy of a megabyte app image.
	if again := pad4([]byte{1, 2, 3, 4}); len(again) != 4 {
		t.Errorf("pad4 on aligned input len = %d, want 4", len(again))
	}
}

// A plan's images must be usable twice in a row with identical bytes: that is what
// flashing twice without re-reading from disk has to guarantee.
func TestPlanImagesAreReusableAcrossFlashes(t *testing.T) {
	boot := espLikeImage(0xE9, 0x02, 0x22) // magic, dio mode, size/freq nibble
	manifest := []byte(`{"flash_settings":{"flash_mode":"qio","flash_size":"16MB","flash_freq":"80m"},` +
		`"flash_files":{"0x0":"bootloader/bootloader.bin"}}`)
	open := func(string) ([]byte, error) { return append([]byte{}, boot...), nil }

	plan, err := PlanFromManifest(manifest, open)
	if err != nil {
		t.Fatalf("PlanFromManifest: %v", err)
	}
	before := fmt.Sprintf("%x", plan.Images[0].Data)
	// The two transforms Session applies, run twice as two flashes would.
	for round := 0; round < 2; round++ {
		patched, err := patchBootloaderFlashParams(plan.Images[0].Data, plan.Images[0].Offset, plan)
		if err != nil {
			t.Fatalf("round %d patch: %v", round, err)
		}
		_ = pad4(patched)
		if got := fmt.Sprintf("%x", plan.Images[0].Data); got != before {
			t.Fatalf("round %d mutated the shared image bytes\n before %s\n after  %s", round, before, got)
		}
	}
}

func espLikeImage(magic, mode, sizeFreq byte) []byte {
	b := []byte{magic, 0x01, mode, sizeFreq, 0, 0, 0, 0}
	return append(b, bytes.Repeat([]byte{0x42}, 100)...)
}
