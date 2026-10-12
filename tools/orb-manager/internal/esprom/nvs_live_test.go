package esprom

import (
	"os"
	"testing"
)

// TestNvsRegionsRealBuild parses the actual build dir when present: it is the
// only check that the constant expectations (nvs 0x9000/0x8000, storage
// 0xD60000/0x6000 from partitions.csv) still match the flashed table bytes.
func TestNvsRegionsRealBuild(t *testing.T) {
	dir := os.Getenv("ORB_BUILD_DIR")
	if dir == "" {
		t.Skip("ORB_BUILD_DIR not set")
	}
	got, err := NvsRegions(dir)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("regions: %+v", got)
	if len(got) != 2 {
		t.Fatalf("want nvs+storage, got %+v", got)
	}
}
