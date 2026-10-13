package esprom

import (
	"strings"
	"testing"
)

// The chip gate exists because VerifyS3 proves what is attached and nothing
// proved what was about to be written into it. These pin the three things that
// make the gate real rather than theatrical: it reads the field the ROM reads,
// it refuses every other chip by name, and it leaves the headerless members
// alone so a real manifest still flashes.

// espImageAt builds a minimal but honestly-shaped ESP app image: the 0xE9 magic
// and the 2-byte LE chip ID at its packed offset (12-13).
func espImageAt(chipID uint16) []byte {
	b := make([]byte, 32)
	b[0] = espImageMagic
	b[1] = 0x01 // segment count
	b[12] = byte(chipID)
	b[13] = byte(chipID >> 8)
	return b
}

func planWith(imgs ...Image) FlashPlan { return FlashPlan{Images: imgs} }

// The build this hub ships must pass, or every flash is refused.
func TestChipGateAcceptsS3(t *testing.T) {
	p := planWith(
		Image{Name: "bootloader.bin", Offset: 0x0, Data: espImageAt(s3ImageChipID)},
		Image{Name: "gorb-os.bin", Offset: 0x20000, Data: espImageAt(s3ImageChipID)},
	)
	if err := CheckPlanChipIDs(p); err != nil {
		t.Fatalf("the correct chip was refused: %v", err)
	}
}

// Every other ID in esp_chip_id_t must be refused, and refused in words: the
// ordinary cause is a wrong build out of a Downloads folder, and the operator
// needs to read "ESP32-C6" to recognise it.
func TestChipGateRefusesEveryOtherChipByName(t *testing.T) {
	cases := []struct {
		id   uint16
		name string
	}{
		{0x0000, "ESP32"},
		{0x0002, "ESP32-S2"},
		{0x0005, "ESP32-C3"},
		{0x000C, "ESP32-C2"},
		{0x000D, "ESP32-C6"},
		{0x0010, "ESP32-H2"},
		{0x0012, "ESP32-P4"},
		{0x0017, "ESP32-C5"},
		{0xFFFF, "no chip ID"},
	}
	for _, c := range cases {
		err := CheckPlanChipIDs(planWith(Image{Name: "gorb-os.bin", Offset: 0x20000, Data: espImageAt(c.id)}))
		if err == nil {
			t.Fatalf("chip ID %#04x (%s) was accepted", c.id, c.name)
		}
		if !strings.Contains(err.Error(), c.name) {
			t.Errorf("chip ID %#04x: error = %v, want it to name %q", c.id, err, c.name)
		}
		if !strings.Contains(err.Error(), "gorb-os.bin") {
			t.Errorf("chip ID %#04x: error = %v, want it to name the image", c.id, err)
		}
	}
}

// The gate must read the chip ID, not a neighbouring field. Bytes 4-7 are the
// entry address and are trivially full of small numbers on a real image, so an
// implementation that read there would both pass wrong-chip images and fail good
// ones, silently. Garbage in 4-7 with the right ID must pass; the right-looking
// number in 4-7 with a wrong ID must not.
func TestChipGateReadsTheChipIDFieldNotTheEntryAddress(t *testing.T) {
	good := espImageAt(s3ImageChipID)
	// 9 hiding in the entry address, and a C3 saying so in its own field.
	good[4], good[5], good[6], good[7] = 0x09, 0x00, 0x00, 0x00
	good[12], good[13] = 0x05, 0x00
	if err := CheckPlanChipIDs(planWith(Image{Name: "gorb-os.bin", Data: good})); err == nil {
		t.Error("an ESP32-C3 image whose entry address reads 9 was accepted")
	}

	right := espImageAt(s3ImageChipID)
	right[4], right[5], right[6], right[7] = 0xAB, 0xCD, 0xEF, 0x01
	if err := CheckPlanChipIDs(planWith(Image{Name: "gorb-os.bin", Data: right})); err != nil {
		t.Errorf("a correct image with an unusual entry address was refused: %v", err)
	}
}

// A partition table, an otadata blob and an assets bin carry no ESP header. This
// is the difference between a gate and a broken hub: the real manifest writes all
// three, so refusing headerless bytes fails every flash there is.
func TestChipGateLeavesHeaderlessMembersAlone(t *testing.T) {
	p := planWith(
		Image{Name: "partition-table.bin", Offset: 0x8000, Data: append([]byte{0xAA, 0x50, 0x01, 0x02}, make([]byte, 60)...)},
		Image{Name: "ota_data_initial.bin", Offset: 0xE000, Data: fillFF(8192)},
		Image{Name: "clock_assets.bin", Offset: 0x420000, Data: []byte("art")},
		Image{Name: "empty", Offset: 0x10000, Data: nil},
	)
	if err := CheckPlanChipIDs(p); err != nil {
		t.Fatalf("headerless manifest members were refused: %v", err)
	}
}

// Half a header is not a header. An image that opens with the magic and then runs
// out before the chip ID is either corrupt or not an image, and neither belongs on
// the wire: the ROM would take the bytes and the orb would come back dead.
func TestChipGateRefusesATruncatedHeader(t *testing.T) {
	b := []byte{espImageMagic, 0x01, 0x02, 0x22, 0, 0, 0, 0, 0, 0, 0, 0, 0x09}
	err := CheckPlanChipIDs(planWith(Image{Name: "bootloader.bin", Data: b}))
	if err == nil {
		t.Fatal("a 13-byte ESP image was accepted")
	}
	if !strings.Contains(err.Error(), "too short") {
		t.Errorf("error = %v, want it to say the image is too short", err)
	}
}

// The whole plan is checked before the first write, so a wrong second image stops
// the flash rather than becoming a half-flashed orb.
func TestChipGateChecksBeyondTheFirstImage(t *testing.T) {
	p := planWith(
		Image{Name: "bootloader.bin", Offset: 0x0, Data: espImageAt(s3ImageChipID)},
		Image{Name: "gorb-os.bin", Offset: 0x20000, Data: espImageAt(0x000D)},
	)
	err := CheckPlanChipIDs(p)
	if err == nil {
		t.Fatal("a plan with a wrong-chip app image was accepted")
	}
	if !strings.Contains(err.Error(), "ESP32-C6") {
		t.Errorf("error = %v, want it to name the second image's chip", err)
	}
}

func fillFF(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = 0xFF
	}
	return b
}
