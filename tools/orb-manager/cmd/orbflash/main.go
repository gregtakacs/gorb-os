// Command orbflash is the headless face of the native Go flasher, for scripted
// bench use and for validating the protocol without the web UI. It drives the
// same esprom.Session the manager uses. Nothing here needs esptool or Python.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

func main() {
	port := flag.String("port", "", "serial device (e.g. /dev/ttyACM0 or a /dev/serial/by-id path)")
	build := flag.String("build", "", "gorb-os build dir (has flasher_args.json)")
	mode := flag.String("mode", "probe", "probe | app | full")
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

	// Flash modes re-enter download mode inside Session (idempotent) and apply
	// the MAC gate if --mac was given.
	var plan esprom.FlashPlan
	var want [6]byte
	if *mode == "full" {
		plan, err = esprom.PlanFromBuildDir(*build)
	} else {
		if *build == "" {
			fmt.Fprintln(os.Stderr, "orbflash: --build is required")
			os.Exit(2)
		}
		plan = esprom.AppOnlyPlan(*build+"/gorb-os.bin", *build+"/ota_data_initial.bin")
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "orbflash:", err)
		os.Exit(1)
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
