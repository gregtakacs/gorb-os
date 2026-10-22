package esprom

import (
	"context"
	"fmt"
	"regexp"
	"strings"
	"time"
)

// Identity (what an orb is running) arrives on the boot console, not from the
// ROM: the ESP32-S3 ROM refuses MEM_READ and SPI_READ without a stub, so flash
// bytes are unreadable. The 2nd-stage bootloader prints the running app's own
// esp_app_desc_t about 0.7 s after a cold boot, and holding the port with DTR
// asserted is what makes IDF emit it at all. Probing therefore costs a reboot.
type OrbInfo struct {
	Port    string `json:"port"`
	IsOrb   bool   `json:"isOrb"`
	Reason  string `json:"reason,omitempty"` // why it is not an orb, or why the probe failed
	MAC     string `json:"mac,omitempty"`
	Project string `json:"project,omitempty"`
	Version string `json:"version,omitempty"`
	// CompileTime and ElfSHA256 are the ordering and identity basis. A git
	// describe is not comparable, so "newer than" is decided by build time and
	// "the same firmware" by the ELF digest.
	CompileTime string `json:"compileTime,omitempty"`
	CompiledAt  string `json:"compiledAt,omitempty"` // RFC3339, when CompileTime parsed
	ElfSHA256   string `json:"elfSha256,omitempty"`
	IDF         string `json:"idf,omitempty"`
	IP          string `json:"ip,omitempty"` // the lease the console reported, if it joined
	// Name is what the board calls itself, read off its boot banner. The board owns
	// this fact and the hub only caches it, so a cable-only board that was named
	// elsewhere still arrives on the card — see ParseOrbName.
	Name     string `json:"name,omitempty"`
	ProbedAt string `json:"probedAt,omitempty"`
	// State is the firmware's own token — "b", "c-ap" or "c-sta" — from a console
	// line. A reboot-paid probe reads the boot banner and fills it; so does the free
	// cable proof (cableproof.go), which is the only way a board with no lease ever
	// says anything about itself. Empty means the chip never got round to saying.
	State string `json:"state,omitempty"`
	// Verdict is filled in by the hub, not the probe: it compares what was found
	// against the firmware that would flash next, which the flasher has no
	// business knowing.
	Verdict string `json:"verdict,omitempty"`
}

// The app_init lines IDF's bootloader prints. Labels match on their prefix, so
// the column padding between label and value never matters.
var (
	// The label class must admit digits and hyphens: "ELF file SHA256" and
	// "ESP-IDF" are the two fields that matter most, and a letters-only class
	// dropped both in silence the first time this ran against real hardware.
	appInitRe    = regexp.MustCompile(`app_init:\s*([A-Za-z0-9 -]+?):\s*(.+?)\s*$`)
	gorbBannerRe = regexp.MustCompile(`main:\s*===\s*gorb-os booting`)
)

// ParseAppInfo pulls the descriptor out of captured boot console lines. found is
// false when nothing identified the device, which is how a bare ESP32 board or
// an Arduino is told apart from an orb.
func ParseAppInfo(lines []string) (info OrbInfo, found bool) {
	for _, line := range lines {
		for _, m := range appInitRe.FindAllStringSubmatch(line, -1) {
			label := strings.ToLower(strings.TrimSpace(m[1]))
			val := strings.TrimSpace(m[2])
			switch label {
			case "project name":
				info.Project, found = val, true
			case "app version":
				info.Version = val
			case "compile time":
				info.CompileTime = val
			case "elf file sha256":
				// IDF truncates it and marks the truncation; keep the prefix.
				info.ElfSHA256 = strings.TrimSuffix(val, "...")
			case "esp-idf":
				info.IDF = val
			}
		}
		if gorbBannerRe.MatchString(line) {
			found = true
		}
		if ip, ok := ParseOrbIP(line); ok {
			info.IP = ip
		}
		if nm, ok := ParseOrbName(line); ok {
			info.Name = nm
		}
	}
	if info.CompileTime != "" {
		if t, err := time.Parse("Jan 2 2006 15:04:05", strings.Join(strings.Fields(info.CompileTime), " ")); err == nil {
			info.CompiledAt = t.UTC().Format(time.RFC3339)
		}
	}
	return info, found
}

// CompileEpoch is the ordering key for "is this firmware newer". An unparsable
// or absent compile time returns 0, which callers must render as unknown rather
// than as "older".
func (i OrbInfo) CompileEpoch() int64 {
	if i.CompiledAt == "" {
		return 0
	}
	t, err := time.Parse(time.RFC3339, i.CompiledAt)
	if err != nil {
		return 0
	}
	return t.Unix()
}

// probeWindow bounds the console capture. The descriptor prints near 0.7 s; the
// rest buys the "associated: config page at" line, a freebie the Manage card uses.
const probeWindowSecs = 9

// ProbeOrb opens one port, asks the ROM who it is, then cold-boots it and reads
// the descriptor off the console. It resets the orb: its screen blanks for a
// couple of seconds and it re-joins Wi-Fi. That is why nothing here is automatic
// and why the UI asks the user to click it one port at a time.
//
// The two OS calls of a probe, kept as seams so the exit guarantee below is
// testable against fake control lines without hardware. Production is the sysfs
// classifier and the real opener; the hub already fakes at its own layer (the
// probeOrb field), but that seam cannot see the chip's control-line history.
var (
	probeClassify = classifyPort
	probeOpen     = OpenPort
)

// The caller must hold the per-port lock — an identity probe resets the chip, so
// a flash sharing the wire would have its write corrupted by it.
//
// ctx abandons the probe between steps and returns ErrCancelled. It is honoured
// between the reset sequences, never inside one: EnterDownloadMode's control-line
// timings are the whole trick, and stopping half through one leaves the chip in an
// unknown state.
//
// Once download mode is entered, every exit path — answer, failure, timeout, cancel
// — hard-resets the chip before releasing it. A chip left in DOWNLOAD answers
// nothing: no Wi-Fi, no HTTP, no banner, so to a bench with no browser open it reads
// as a bricked board that was merely probed.
func ProbeOrb(ctx context.Context, port string) (OrbInfo, error) {
	info := OrbInfo{Port: port}
	if err := ctxErr(ctx); err != nil {
		return info, err
	}

	// Gate first, before any control-line edge: EnterDownloadMode toggles DTR/RTS,
	// which resets Arduinos, FTDI breakouts and meters sitting on the same bench.
	// Only the Espressif USB-JTAG peripheral may be probed, and a port the host
	// cannot attribute is treated as not-an-orb rather than given the benefit of
	// the doubt.
	usb := probeClassify(port)
	if !usb.IsOrbDevice() {
		switch {
		case !usb.Known:
			info.Reason = "this host cannot identify the device behind the port, so the hub will not reset it to ask"
		default:
			info.Reason = fmt.Sprintf("not an orb device (USB %04x:%04x)", usb.Vendor, usb.Product)
		}
		return info, nil
	}
	// The USB serial-number descriptor of this peripheral is the chip MAC, so the
	// host already knows it. Report it even when the probe cannot go further.
	info.MAC = usb.Serial

	pt, err := probeOpen(port)
	if err != nil {
		info.Reason = "port unavailable: " + err.Error()
		return info, nil
	}
	defer pt.Close()

	if err := ctxErr(ctx); err != nil {
		return info, err
	}
	if err := EnterDownloadMode(pt); err != nil {
		info.Reason = "could not enter download mode: " + err.Error()
		return info, nil
	}
	// From this line the chip owes a boot. An interrupted handshake — a silent ROM
	// after a disturbed entry, a cancel, a budget that expired — used to return
	// through the paths below without any reset, stranding the chip at
	// "waiting for download": invisible on a headless sweep, indistinguishable from
	// a bricked board from across the room. The defer is the guarantee; the normal
	// HardReset below clears the flag. Defer order matters: registered after
	// pt.Close(), so this fires first, on an fd still open to drive the line.
	bootedOut := false
	defer func() {
		if !bootedOut {
			_ = HardReset(pt)
			bootedOut = true
		}
	}()
	// WithContext is load-bearing: without it the Device's ctxErr sees nil, and a
	// probe's own handshake ignores the operator's Cancel and the op's deadline —
	// each Sync attempt burns its full 3 s window, which is how a cancelled
	// identify outlived its budget on the bench.
	d := New(pt, func(string, ...any) {}).WithContext(ctx)
	if err := d.Sync(); err != nil {
		if IsCancelled(err) {
			// A cancelled handshake is not the chip failing to answer; the probe
			// was stopped, and saying otherwise blames the orb (cancel.go).
			return info, err
		}
		// No ROM answer: a genuine serial device that is not an ESP32 in download
		// mode — the FTDI cable, the meter, the Arduino on the desk.
		info.Reason = "no bootloader answer - not an orb"
		return info, nil
	}
	if err := d.VerifyS3(); err != nil {
		info.Reason = "answered, but not an ESP32-S3"
		return info, nil
	}
	mac, err := d.ReadMAC()
	if err != nil {
		info.Reason = "chip answered but the MAC read failed: " + err.Error()
		return info, nil
	}
	info.MAC = formatMAC(mac) // eFuse beats the host's serial-number guess

	// Boot it back into its app and catch the descriptor. A chip that answers the
	// ROM and then prints no app_init block is an ESP32-S3 not running gorb-os: a
	// dev board, or an orb whose app failed to link.
	if err := HardReset(pt); err != nil {
		// Attempted: a failing pulse means a dead fd, and the defer could not do
		// better with it. Claim the exit so no second pulse hits a live line.
		bootedOut = true
		info.Reason = "reset failed: " + err.Error()
		return info, nil
	}
	bootedOut = true
	lines := CaptureBoot(ctx, pt, probeWindowSecs)
	if err := ctxErr(ctx); err != nil {
		// The operator walked away; do not report their cancelled probe as an
		// orb that failed to boot gorb-os.
		return info, err
	}
	parsed, found := ParseAppInfo(lines)
	if !found {
		info.Reason = "an ESP32-S3 at " + info.MAC + ", but no gorb-os firmware booted"
		return info, nil
	}
	info.Project, info.Version = parsed.Project, parsed.Version
	info.CompileTime, info.CompiledAt = parsed.CompileTime, parsed.CompiledAt
	info.ElfSHA256, info.IDF, info.IP = parsed.ElfSHA256, parsed.IDF, parsed.IP
	if info.Project != "" && !strings.EqualFold(info.Project, "gorb-os") {
		info.Reason = "runs different firmware: " + info.Project
		return info, nil
	}
	info.IsOrb = true
	info.Reason = "identified"
	info.ProbedAt = time.Now().UTC().Format(time.RFC3339)
	return info, nil
}

func formatMAC(mac [6]byte) string {
	return fmt.Sprintf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5])
}
