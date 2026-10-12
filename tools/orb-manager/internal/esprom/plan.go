package esprom

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// flasher_args.json is the manifest idf.py emits describing exactly which
// .bin goes at which offset and how to configure the SPI flash. Reading it is
// the same contract serflash.sh honours: the flasher never hardcodes offsets.
type flasherArgs struct {
	WriteFlashArgs []string `json:"write_flash_args"`
	FlashSettings  struct {
		FlashMode string `json:"flash_mode"`
		FlashSize string `json:"flash_size"`
		FlashFreq string `json:"flash_freq"`
	} `json:"flash_settings"`
	FlashFiles map[string]string `json:"flash_files"`
}

// PlanFromBuildDir reads <buildDir>/flasher_args.json and resolves every
// listed image path against buildDir (paths there are build-relative, e.g.
// bootloader/bootloader.bin and ../assets/clock_assets.bin).
func PlanFromBuildDir(buildDir string) (FlashPlan, error) {
	raw, err := os.ReadFile(filepath.Join(buildDir, "flasher_args.json"))
	if err != nil {
		return FlashPlan{}, fmt.Errorf("esprom: read flasher_args.json: %w (build the project first?)", err)
	}
	var fa flasherArgs
	if err := json.Unmarshal(raw, &fa); err != nil {
		return FlashPlan{}, fmt.Errorf("esprom: parse flasher_args.json: %w", err)
	}
	mode, ok := flashModeCode[strings.ToLower(fa.FlashSettings.FlashMode)]
	if !ok {
		mode = 2 // dio is this project's build mode
	}
	size, err := parseFlashSize(fa.FlashSettings.FlashSize)
	if err != nil {
		return FlashPlan{}, err
	}
	p := FlashPlan{FlashMode: mode, FlashSize: size, FlashFreq: 0xFF /* keep */}

	var offsets []uint32
	for offStr := range fa.FlashFiles {
		off, err := parseOffset(offStr)
		if err != nil {
			return FlashPlan{}, err
		}
		offsets = append(offsets, off)
	}
	// Sort ascending so images land low-to-high (matches esptool write order).
	for i := range offsets {
		for j := i + 1; j < len(offsets); j++ {
			if offsets[j] < offsets[i] {
				offsets[i], offsets[j] = offsets[j], offsets[i]
			}
		}
	}
	rev := map[uint32]string{}
	for offStr, rel := range fa.FlashFiles {
		off, _ := parseOffset(offStr)
		rev[off] = rel
	}
	for _, off := range offsets {
		rel := rev[off]
		full := filepath.Clean(filepath.Join(buildDir, rel))
		p.Images = append(p.Images, Image{Offset: off, Path: full})
	}
	if len(p.Images) == 0 {
		return FlashPlan{}, fmt.Errorf("esprom: no images listed in %s/flasher_args.json", buildDir)
	}
	return p, nil
}

// AppOnlyPlan builds a plan for just the application image (the A/B OTA code
// slot) plus the otadata that points the bootloader at it — the serial twin
// of serflash.sh. Offsets 0x20000 (ota_0) and 0x11000 come from partitions.csv.
const (
	otaDataOffset = 0x11000
	appOffset     = 0x20000
	appFlashSize  = 16 << 20
)

func AppOnlyPlan(appBin, otaDataInitial string) FlashPlan {
	imgs := []Image{{Offset: appOffset, Path: appBin}}
	if otaDataInitial != "" {
		imgs = append(imgs, Image{Offset: otaDataOffset, Path: otaDataInitial})
	}
	return FlashPlan{
		Images:    imgs,
		FlashSize: appFlashSize,
		FlashMode: 2,    // dio
		FlashFreq: 0xFF, // keep whatever the app image already declares
	}
}

var flashModeCode = map[string]byte{
	"qout": 1, "dio": 2, "dout": 3, "qio": 0,
}

func parseOffset(s string) (uint32, error) {
	s = strings.TrimSpace(s)
	v, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(s, "0x"), "0X"), 16, 64)
	if err != nil {
		return 0, fmt.Errorf("esprom: bad offset %q", s)
	}
	return uint32(v), nil
}

func parseFlashSize(s string) (uint32, error) {
	s = strings.TrimSpace(strings.ToUpper(s))
	if s == "" || s == "KEEP" || s == "DETECT" {
		return appFlashSize, nil
	}
	if !strings.HasSuffix(s, "MB") {
		return 0, fmt.Errorf("esprom: unsupported flash size %q", s)
	}
	mb, err := strconv.Atoi(strings.TrimSuffix(s, "MB"))
	if err != nil {
		return 0, fmt.Errorf("esprom: unsupported flash size %q", s)
	}
	return uint32(mb) << 20, nil
}

// EraseTarget names one flash region the hub may ROM-erase after the manifest
// writes — used for the deliberate factory reset that sends a returning orb
// back to provisioning. The full-device manifest never lists NVS (documented
// retention invariant in partitions.csv), so credentials surviving a reflash
// is by design; wiping must therefore be an explicit user act, and its
// regions come from the built partition table, never from constants.
type EraseTarget struct {
	Label  string
	Offset uint32
	Size   uint32
}

// Partition-table entry layout is gen_esp32part.py's STRUCT_FORMAT
// '<2sBBLL16sL': magic AA50, type, subtype, offset, size, label[16], flags.
// type 1 = data, subtype 2 = nvs.
func NvsRegions(buildDir string) ([]EraseTarget, error) {
	raw, err := os.ReadFile(filepath.Join(buildDir, "partition_table", "partition-table.bin"))
	if err != nil {
		return nil, fmt.Errorf("esprom: read partition table: %w", err)
	}
	out := []EraseTarget{}
	for i := 0; i+32 <= len(raw); i += 32 {
		e := raw[i : i+32]
		if e[0] != 0xAA || e[1] != 0x50 || e[2] != 0x01 || e[3] != 0x02 {
			continue // not a data/nvs entry (MD5 entry and app rows included)
		}
		label := cstring(e[12:28])
		if label != "nvs" && label != "storage" {
			continue
		}
		off := binary.LittleEndian.Uint32(e[4:8])
		size := binary.LittleEndian.Uint32(e[8:12])
		if off%4096 != 0 || size%4096 != 0 {
			return nil, fmt.Errorf("esprom: NVS partition %q at 0x%X+0x%X is not sector aligned — refusing to erase", label, off, size)
		}
		out = append(out, EraseTarget{Label: label, Offset: off, Size: size})
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("esprom: no nvs/storage partition found in the built partition table")
	}
	return out, nil
}

func cstring(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}
