package esprom

import (
	"strings"
	"testing"
)

// bootLog is verbatim from a live orb's console on 2026-10-12, captured through
// the probe itself. It is the fixture that matters: the field labels are IDF's,
// not ours, and a parser that quietly fails to match one reports an empty value
// instead of an error. That already happened — a letters-only label class dropped
// "ELF file SHA256" and "ESP-IDF", and every verdict silently lost its strongest
// same-build signal. Pin the whole block.
var bootLog = []string{
	"ESP-ROM:esp32s3-20210327",
	"rst:0x15 (USB_UART_CHIP_RESET),boot:0x2b (SPI_FAST_FLASH_BOOT)",
	"I (26) boot: ESP-IDF v5.4 2nd stage bootloader",
	"I (29) boot:  0 nvs              WiFi data        01 02 00009000 00008000",
	"I (32) boot:  7 storage          WiFi data        01 02 00d60000 00006000",
	"I (692) app_init: Application information:",
	"I (692) app_init: Project name:     gorb-os",
	"I (692) app_init: App version:      8eb4bf0-dirty",
	"I (692) app_init: Compile time:     Oct 12 2026 01:58:41",
	"I (692) app_init: ELF file SHA256:  d5fd8bb9d...",
	"I (692) app_init: ESP-IDF:          v5.4",
	"I (693) efuse_init: Min chip rev:     v0.0",
	"I (703) main: === gorb-os booting ===",
	"I (140) main: gorb-os running — knob: turn swaps menu/hue/clock",
	"I (4795) orb_net: associated: config page at http://192.168.2.34/",
}

func TestParseAppInfoAgainstLiveBootLog(t *testing.T) {
	info, found := ParseAppInfo(bootLog)
	if !found {
		t.Fatal("ParseAppInfo found nothing in a real orb boot log")
	}
	// Every field the identity card and the verdict depend on, asserted non-empty
	// by name so a label that stops matching fails here rather than shipping an
	// empty cell.
	for _, f := range []struct{ name, got string }{
		{"Project", info.Project},
		{"Version", info.Version},
		{"CompileTime", info.CompileTime},
		{"CompiledAt", info.CompiledAt},
		{"ElfSHA256", info.ElfSHA256},
		{"IDF", info.IDF},
		{"IP", info.IP},
	} {
		if f.got == "" {
			t.Errorf("%s parsed empty from the live boot log — the label regex no longer matches IDF's text", f.name)
		}
	}
	if info.Project != "gorb-os" {
		t.Errorf("Project = %q, want gorb-os", info.Project)
	}
	if info.Version != "8eb4bf0-dirty" {
		t.Errorf("Version = %q", info.Version)
	}
	if info.ElfSHA256 != "d5fd8bb9d" {
		t.Errorf("ElfSHA256 = %q, want the printed prefix with the ellipsis trimmed", info.ElfSHA256)
	}
	if info.IDF != "v5.4" {
		t.Errorf("IDF = %q", info.IDF)
	}
	if info.IP != "192.168.2.34" {
		t.Errorf("IP = %q", info.IP)
	}
	if info.CompiledAt != "2026-10-12T01:58:41Z" {
		t.Errorf("CompiledAt = %q, want the boot stamp normalised to RFC3339", info.CompiledAt)
	}
}

// A bare ESP32 board answers the ROM but has no gorb-os to report. The probe must
// say "not an orb", not render an empty identity card.
func TestParseAppInfoRejectsForeignBoot(t *testing.T) {
	for name, lines := range map[string][]string{
		"empty":      nil,
		"other app":  {"I (1) boot: hello", "I (692) app_init: Project name:  some-other-firmware", "I (692) app_init: App version:  1.0"},
		"noise only": {"ets Jul 29 2019 12:21:46", "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)"},
	} {
		info, found := ParseAppInfo(lines)
		if name == "other app" {
			if found && info.Project == "gorb-os" {
				t.Errorf("%s: foreign firmware accepted as an orb", name)
			}
			continue
		}
		if found {
			t.Errorf("%s: ParseAppInfo claimed an orb from %q", name, lines)
		}
		if info.Version != "" || info.MAC != "" {
			t.Errorf("%s: fields populated from a log that identifies nothing: %+v", name, info)
		}
	}
}

// CompileEpoch is the ordering basis for every verdict, so an unparsable stamp has
// to read as unknown (0) and never as an accidental 1970 build.
func TestCompileEpochIsHonestAboutMissingStamps(t *testing.T) {
	if n := (OrbInfo{}).CompileEpoch(); n != 0 {
		t.Errorf("CompileEpoch on no stamp = %d, want 0", n)
	}
	if n := (OrbInfo{CompiledAt: "not a time"}).CompileEpoch(); n != 0 {
		t.Errorf("CompileEpoch on garbage = %d, want 0", n)
	}
	if n := (OrbInfo{CompiledAt: "2026-10-12T01:58:41Z"}).CompileEpoch(); n <= 0 {
		t.Errorf("CompileEpoch on a real stamp = %d, want positive", n)
	}
}

// The MAC formatting the probe reports, checked against the shape the card prints.
func TestFormatMACShape(t *testing.T) {
	got := formatMAC([6]byte{0x44, 0xbd, 0x8d, 0x60, 0x80, 0x88})
	if !strings.HasPrefix(got, "44:bd:8d:") || len(got) != 17 {
		t.Errorf("formatMAC = %q, want a lower-case colon-separated 17-char MAC", got)
	}
}
