package esprom

import "testing"

// TestLonelyPlanGeometry pins the offsets, the order, and the guard. This is
// the load-bearing wall of the lonely image: it shares the orb bundle's
// partition layout on purpose, so a New Orb flash over a lonely board
// overwrites every byte this plan writes, and the reversibility promise is
// arithmetic rather than hope.
func TestLonelyPlanGeometry(t *testing.T) {
	app := []byte("lonely-app-bytes")
	boot := []byte("bootloader")
	pt := []byte("partitions")
	ota := []byte("otadata")
	p, err := LonelyPlan(app, boot, pt, ota)
	if err != nil {
		t.Fatalf("LonelyPlan: %v", err)
	}
	want := []struct {
		off  uint32
		name string
	}{
		{0x0, "bootloader.bin"},
		{0x8000, "partition-table.bin"},
		{0x11000, "ota_data_initial.bin"},
		{0x20000, "lonely.bin"},
	}
	if len(p.Images) != len(want) {
		t.Fatalf("plan writes %d images, want %d", len(p.Images), len(want))
	}
	for i, w := range want {
		if p.Images[i].Offset != w.off || p.Images[i].Name != w.name {
			t.Errorf("image %d = 0x%X/%s, want 0x%X/%s", i, p.Images[i].Offset, p.Images[i].Name, w.off, w.name)
		}
	}
	if p.FlashSize != 16<<20 || p.FlashMode != 2 || p.FlashFreq != 0xFF {
		t.Errorf("plan flash settings = size %d mode %d freq %d, want 16MB/dio/keep", p.FlashSize, p.FlashMode, p.FlashFreq)
	}
	if p.Images[3].Data == nil || string(p.Images[3].Data) != string(app) {
		t.Error("the app slot does not carry the lonely image")
	}
}

// TestLonelyPlanRefusesGaps: a lonely image, a bootloader, a partition table
// and an otadata all mean nothing alone. Every one is mandatory — an empty
// piece would leave a board that cannot boot, which is the exact failure this
// image exists to prevent.
func TestLonelyPlanRefusesGaps(t *testing.T) {
	all := [][]byte{{1}, {2}, {3}, {4}}
	for i := range all {
		args := [][]byte{{1}, {2}, {3}, {4}}
		args[i] = nil
		if _, err := LonelyPlan(args[0], args[1], args[2], args[3]); err == nil {
			t.Errorf("LonelyPlan accepted an empty image at argument %d", i+1)
		}
	}
}
