package esprom

import (
	"encoding/binary"
	"strings"
	"testing"
	"time"
)

// The chip erase is the one operation in this package that destroys everything
// at once, so every claim about it is pinned here rather than left to a bench run.

// TestEraseFlashIsAWholeChipFlashBegin pins the mechanism. esptool's erase_flash
// is @stub_function_only, so op 0xD0 is not a ROM command and sending it to a
// bare S3 answers the [1,5] unsupported status; the real ROM erase is FLASH_BEGIN
// with erase_size = the flash capacity and num_blocks = 0, so nothing follows.
// The offset must be 0 and the size the whole chip, or the erase is a region
// erase wearing a bigger name.
func TestEraseFlashIsAWholeChipFlashBegin(t *testing.T) {
	f := newFake(t)
	d := New(f, nil)
	if err := d.EraseFlash(appFlashSize); err != nil {
		t.Fatalf("EraseFlash: %v", err)
	}
	if len(f.beginBodies) != 1 {
		t.Fatalf("want one FLASH_BEGIN, got %d", len(f.beginBodies))
	}
	b := f.beginBodies[0]
	if len(b) != 20 {
		t.Fatalf("erase body len = %d, want 20 (five words)", len(b))
	}
	if got := binary.LittleEndian.Uint32(b[0:]); got != appFlashSize {
		t.Errorf("erase_size = %#x, want %#x (the whole chip)", got, appFlashSize)
	}
	if got := binary.LittleEndian.Uint32(b[4:]); got != 0 {
		t.Errorf("num_blocks = %d, want 0 (erase only — any blocks would be written over the erase)", got)
	}
	if got := binary.LittleEndian.Uint32(b[12:]); got != 0 {
		t.Errorf("offset = %#x, want 0", got)
	}
	if got := binary.LittleEndian.Uint32(b[16:]); got != 0 {
		t.Errorf("encrypted_write = %d, want 0", got)
	}
	// The stub-only opcode must never reach the wire: the ROM rejects it, and a
	// future refactor that "simplifies" the erase to 0xD0 would fail on hardware
	// and nowhere else.
	if f.sawOp(0xD0) {
		t.Error("chip erase sent 0xD0 — that opcode is stub-only and the ROM rejects it")
	}
}

func TestEraseFlashRefusesSpansThatAreNotAFlashSize(t *testing.T) {
	f := newFake(t)
	d := New(f, nil)
	// Zero would ask the ROM to erase nothing and call it a wipe; anything not on
	// a sector boundary cannot be erased at all. An aligned span of any size is
	// allowed, because the orb is not the only board this package can address.
	for _, size := range []uint32{0, 4097, 4095} {
		if err := d.EraseFlash(size); err == nil {
			t.Errorf("EraseFlash(%d): want refusal, got nil", size)
		}
	}
	if len(f.beginBodies) != 0 {
		t.Error("refused erases must not reach the wire")
	}
}

// TestChipEraseTimeoutMatchesEsptool pins the wait. The ROM erases inside
// FLASH_BEGIN and says nothing while it does, so a timeout shorter than the chip
// needs turns a working erase into a reported failure — the worst kind, because
// the board really is blank.
func TestChipEraseTimeoutMatchesEsptool(t *testing.T) {
	if got := timeoutFor("erase", int(appFlashSize)); got < 16*30*time.Second {
		t.Errorf("erase timeout for 16MB = %v, want at least 8 minutes of headroom", got)
	}
	if got := timeoutFor("md5", int(appFlashSize)); got < time.Minute {
		t.Errorf("md5 timeout for 16MB = %v, want minutes: the ROM hashes the whole chip itself", got)
	}
}

// TestFlashMD5ReadsTheDigestBeforeTheStatus pins the one response layout that
// differs from every other ROM command: the digest occupies the reply-data slot
// and the status bytes trail it. Read the status first and a failed command
// parses as a digest of garbage.
func TestFlashMD5ReadsTheDigestBeforeTheStatus(t *testing.T) {
	f := newFake(t)
	f.erased = true
	d := New(f, nil)
	got, err := d.FlashMD5(0, appFlashSize)
	if err != nil {
		t.Fatalf("FlashMD5: %v", err)
	}
	if want := blankMD5(appFlashSize); got != want {
		t.Errorf("digest = %s, want the erased-flash digest %s", got, want)
	}
}

// TestEraseFlashVerifiedProvesTheBlank pins the two directions of the proof: an
// erase that lands reports the digest, and one that does not must refuse loudly
// rather than tell the operator the chip is blank when it is not.
func TestEraseFlashVerifiedProvesTheBlank(t *testing.T) {
	f := newFake(t)
	d := New(f, nil)
	got, err := d.EraseFlashVerified(appFlashSize)
	if err != nil {
		t.Fatalf("erase of a chip that erased cleanly: %v", err)
	}
	if !strings.EqualFold(got, blankMD5(appFlashSize)) {
		t.Errorf("reported digest = %s, want %s", got, blankMD5(appFlashSize))
	}

	bad := newFake(t)
	bad.erased = true
	bad.md5Bad = true
	if _, err := New(bad, nil).EraseFlashVerified(appFlashSize); err == nil {
		t.Fatal("a flash that does not read blank must be an error, not a success")
	} else if !strings.Contains(err.Error(), "not blank") {
		t.Errorf("error should name the blank-flash mismatch, got: %v", err)
	}
}

// TestBlankMD5IsWhatAnErasedFlashDigestsTo checks the expected digest against an
// independent computation, so a bug in blankMD5 cannot agree with itself.
func TestBlankMD5IsWhatAnErasedFlashDigestsTo(t *testing.T) {
	// md5 of 4 KiB of 0xFF, computed independently with:
	//   head -c 4096 /dev/zero | tr '\0' '\377' | md5sum
	const want4K = "6ae59e64850377ee5470c854761551ea"
	if got := blankMD5(4096); got != want4K {
		t.Errorf("blankMD5(4096) = %s, want %s", got, want4K)
	}
	// The orb's own capacity, and the digest every successful wipe must report.
	const want16M = "d5929b7a5ad99f511fc83d7d0b48b85f"
	if got := blankMD5(appFlashSize); got != want16M {
		t.Errorf("blankMD5(16MB) = %s, want %s", got, want16M)
	}
	// Sizes must not collide: the digest covers the length as well as the bytes.
	if blankMD5(4096) == blankMD5(8192) {
		t.Error("blankMD5 is not size-dependent")
	}
}

// TestHexDigestAcceptsRawBytesToo covers the defensive branch: a ROM that answered
// with 16 raw bytes rather than 32 hex characters must be reported as a mismatch,
// not mis-parsed as something it is not.
func TestHexDigestAcceptsRawBytesToo(t *testing.T) {
	raw := make([]byte, 16)
	for i := range raw {
		raw[i] = byte(i)
	}
	if got := hexDigest(raw); got != "000102030405060708090a0b0c0d0e0f" {
		t.Errorf("hexDigest(raw) = %s", got)
	}
	if got := hexDigest([]byte("6CD5424CF30C6C70DF0F6A1B3CDECDB5")); got != "6cd5424cf30c6c70df0f6a1b3cdecdb5" {
		t.Errorf("hexDigest(hex text) = %s, want it lowercased", got)
	}
}
