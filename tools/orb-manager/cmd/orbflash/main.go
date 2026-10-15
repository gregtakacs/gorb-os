// Command orbflash is the headless face of the native Go flasher, for scripted
// bench use and for validating the protocol without the web UI. It drives the
// same esprom.Session the manager uses. Nothing here needs esptool or Python.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
)

func main() {
	port := flag.String("port", "", "serial device (e.g. /dev/ttyACM0 or a /dev/serial/by-id path)")
	fwPath := flag.String("firmware", "", "a firmware bundle (.zip) to flash instead of the built-in one")
	mode := flag.String("mode", "probe", "probe | md5 | app | full | wipe")
	macStr := flag.String("mac", "", "require this bound MAC (aa:bb:cc:dd:ee:ff) before flashing")
	flag.Parse()

	if *port == "" {
		fmt.Fprintln(os.Stderr, "orbflash: --port is required")
		flag.Usage()
		os.Exit(2)
	}

	pt, err := esprom.OpenPort(*port)
	if err != nil {
		fmt.Fprintln(os.Stderr, "orbflash:", err)
		os.Exit(1)
	}
	defer pt.Close()

	logf := func(s string) { fmt.Fprintln(os.Stderr, "  "+s) }
	d := esprom.New(pt, func(f string, a ...any) { logf(fmt.Sprintf(f, a...)) })

	if err := esprom.EnterDownloadMode(pt); err != nil {
		fmt.Fprintln(os.Stderr, "orbflash: enter download mode:", err)
		os.Exit(1)
	}
	if err := syncWithRetry(d); err != nil {
		fmt.Fprintln(os.Stderr, "orbflash: sync:", err)
		os.Exit(1)
	}
	if err := d.VerifyS3(); err != nil {
		fmt.Fprintln(os.Stderr, "orbflash:", err)
		os.Exit(1)
	}
	mac, err := d.ReadMAC()
	if err != nil {
		fmt.Fprintln(os.Stderr, "orbflash: read MAC:", err)
		os.Exit(1)
	}
	fmt.Printf("connected: ESP32-S3 MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5])

	if *mode == "probe" {
		fmt.Println("probe OK: download-mode entry, sync, chip id, and eFuse MAC all verified.")
		_ = esprom.HardReset(pt) // boot it back into the app
		return
	}

	// The read-only digest: what the flash holds right now, with nothing written
	// and nothing erased. It is how a wipe is checked from the far side, and how a
	// blank chip is told apart from a board running something, over the cable and
	// with no mDNS or HTTP involved.
	if *mode == "md5" {
		// Same reason the flash and wipe paths do it: on USB-Serial/JTAG nothing
		// feeds the watchdogs, and hashing 16 MB is long enough for them to bite
		// mid-command (esptool _post_connect does exactly this for this transport).
		if d.UsesUSBJTAGSerial() {
			if err := d.DisableWatchdogs(); err != nil {
				fmt.Fprintln(os.Stderr, "orbflash: disable watchdogs:", err)
				os.Exit(1)
			}
		}
		if err := d.SpiAttach(); err != nil {
			fmt.Fprintln(os.Stderr, "orbflash: spi attach:", err)
			os.Exit(1)
		}
		// The geometry first. The ROM answers FLASH_MD5 with status [1,9] unless
		// SPI_SET_PARAMS has configured the chip — esptool's no-stub verify_flash
		// prints "Enabling default SPI flash mode" then "Configuring flash size"
		// for exactly this reason, and skipping the second step is what made the
		// digest read look unsupported on the bench (2026-10-14).
		if err := d.SpiSetParams(esprom.OrbFlashSize); err != nil {
			fmt.Fprintln(os.Stderr, "orbflash: spi set params:", err)
			os.Exit(1)
		}
		digest, err := d.FlashMD5(0, esprom.OrbFlashSize)
		if err != nil {
			fmt.Fprintln(os.Stderr, "orbflash: md5:", err)
			os.Exit(1)
		}
		fmt.Printf("md5 of the whole flash: %s\n", digest)
		if blank := esprom.BlankMD5(esprom.OrbFlashSize); digest == blank {
			fmt.Printf("the flash is blank (erased): that is the digest of all 0xFF, %s\n", blank)
		}
		_ = esprom.HardReset(pt)
		return
	}

	// The erase writes nothing, so it needs no bundle: this is the mode that works
	// on a bench with no firmware loaded, which is exactly when a board is most
	// likely to be sent back to nothing. Wipe re-enters download mode itself, the
	// same idempotent reset Session does after this same preamble.
	if *mode == "wipe" {
		if err := esprom.Wipe(pt, esprom.WipeOptions{OnEvent: logf}); err != nil {
			fmt.Fprintln(os.Stderr, "orbflash: WIPE FAILED:", err)
			os.Exit(1)
		}
		fmt.Println("chip erased: flash verified blank against the ROM digest. The orb will not boot until it is flashed.")
		return
	}

	// Flash modes re-enter download mode inside Session (idempotent) and apply
	// the MAC gate if --mac was given.
	// Firmware comes from the bundle compiled into this binary, or from one named
	// with -firmware. There is no --build: the flasher is a standalone tool, and a
	// person flashing an orb has no ESP-IDF installed.
	b, err := loadFlasherFirmware(*fwPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, "orbflash:", err)
		os.Exit(1)
	}
	var plan esprom.FlashPlan
	var want [6]byte
	if *mode == "full" {
		plan = b.Plan()
	} else {
		plan, err = b.AppPlan()
		if err != nil {
			fmt.Fprintln(os.Stderr, "orbflash:", err)
			os.Exit(1)
		}
	}
	if macStr != nil && *macStr != "" {
		w, perr := parseMAC(*macStr)
		if perr != nil {
			fmt.Fprintln(os.Stderr, "orbflash:", perr)
			os.Exit(2)
		}
		want = w
	}
	opts := esprom.Options{Plan: plan, OnEvent: logf}
	if *macStr != "" {
		opts.HasMAC = true
		opts.WantMAC = want
	}
	fmt.Printf("flashing %d image(s) to %s\n", len(plan.Images), *port)
	if err := esprom.Session(pt, opts); err != nil {
		fmt.Fprintln(os.Stderr, "orbflash: FLASH FAILED:", err)
		os.Exit(1)
	}
	fmt.Println("flash complete")
}

func syncWithRetry(d *esprom.Device) error {
	var last error
	for i := 0; i < 12; i++ {
		if last = d.Sync(); last == nil {
			return nil
		}
	}
	return last
}

// parseMAC accepts aa:bb:cc:dd:ee:ff into the esprom [6]byte wire type.
func parseMAC(s string) ([6]byte, error) {
	var m [6]byte
	if _, err := fmt.Sscanf(s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]); err != nil {
		return m, fmt.Errorf("orbflash: bad MAC %q: %w", s, err)
	}
	return m, nil
}

// loadFlasherFirmware returns the built-in bundle, or the one -firmware names.
func loadFlasherFirmware(path string) (*firmware.Bundle, error) {
	if path == "" {
		return firmware.BuiltIn()
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read firmware file %s: %w", path, err)
	}
	return firmware.Load(raw, path)
}
