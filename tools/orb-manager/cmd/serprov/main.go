// Command serprov is the wired-provisioning round-trip tester for Path 2. It
// opens an orb's console, reboots it into its app (which re-arms the boot scan +
// stdin reader), collects the PROVSCAN network list, writes one PROV line, and
// confirms the firmware's PROV-ACK. Pass --port to choose which orb to
// provision; there is no default and no bench-state file consulted. It drives
// the same esprom.ProvisionOn the manager's /api/provision handler uses.
package main

import (
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

func main() {
	ssid := flag.String("ssid", "", "network to provision (required)")
	pass := flag.String("pass", "", "passphrase (empty = open network)")
	tz := flag.String("tz", "", "optional POSIX TZ rule")
	portFlag := flag.String("port", "", "serial port to provision (/dev/ttyACMx or a /dev/serial/by-id path; required)")
	verbose := flag.Bool("v", true, "echo every console line as it arrives")
	scan := flag.Duration("scan", 22*time.Second, "budget to catch PROVSCAN-END")
	ack := flag.Duration("ack", 20*time.Second, "budget to catch PROV-ACK")
	ipw := flag.Duration("ip", 25*time.Second, "after the ack, budget to catch the DHCP lease banner")
	flag.Parse()
	if *ssid == "" {
		fmt.Fprintln(os.Stderr, "serprov: --ssid is required")
		os.Exit(2)
	}

	// The port is always the user's explicit choice.
	port := *portFlag
	if port == "" {
		fmt.Fprintln(os.Stderr, "serprov: --port is required (a /dev/ttyACMx or /dev/serial/by-id path)")
		os.Exit(2)
	}
	fmt.Printf("serprov: %s\n", port)

	// One operation per port at a time, so a serprov run can't collide with the
	// manager's flash/provision on the same cable.
	portLock, lockErr := orbbind.LockPort(port)
	if lockErr != nil {
		fmt.Fprintln(os.Stderr, "serprov:", lockErr)
		os.Exit(1)
	}
	defer portLock.Release()

	pt, err := esprom.OpenPort(port)
	if err != nil {
		fmt.Fprintln(os.Stderr, "serprov:", err)
		os.Exit(1)
	}
	defer pt.Close()

	fmt.Println("serprov: opening console (this resets the board into its app)...")
	onLine := func(l string) {
		if *verbose {
			fmt.Printf("    . %s\n", l)
		}
	}
	res, err := esprom.ProvisionOn(pt, *ssid, *pass, *tz, *scan, *ack, *ipw, onLine)
	if err != nil {
		fmt.Fprintln(os.Stderr, "serprov:", err)
		os.Exit(1)
	}

	fmt.Printf("\nserprov: AP observed: %v | scanned %d network(s) | sent=%v ack=%v\n",
		res.APSeen, len(res.Networks), res.Sent, res.Ack)
	for _, n := range res.Networks {
		fmt.Printf("    ch%-2d rssi%-4d %s %q\n", n.Ch, n.RSSI, map[bool]string{true: "open", false: "   "}[n.Open], n.SSID)
	}
	if !res.Ack {
		fmt.Println("\nserprov: NOT PROVEN — no PROV-ACK after writing the PROV line.")
		os.Exit(1)
	}
	if res.IP != "" {
		fmt.Printf("\nserprov: PROVEN — orb joined and took DHCP lease %s (read off the USB console).\n", res.IP)
	} else {
		fmt.Println("\nserprov: PROVEN — orb accepted the PROV line and is joining (no lease banner seen yet; find it on the LAN).")
	}
}
