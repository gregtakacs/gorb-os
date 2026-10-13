// Command orbfake runs the internal/orbfake device double as a listening server, for
// a human driving a real hub through a browser. It answers the theme routes
// main/net/orb_net.c registers, with h_sd's FORMAT-2 row shapes, so the Theme card can
// be exercised with no orb on a cable. It answers the provisioning verbs too — wifi
// scan/join/ap, config, reboot, screen — so the Add-an-orb wizard is testable without
// a radio anywhere near the bench.
//
// The routes and JSON live in internal/orbfake, which the Go suite drives too. One
// double, one contract: the first version of this file was written by reading the hub's
// client, agreed with it on ?name=, and the pair stayed green while real orbs answered
// 400. Read this behaviour out of the firmware, never out of themes.go.
//
//	orbfake -addr 127.0.0.1:8099        # the orb's httpd listens on 80, so run
//	                                    # the hub against this port instead.
package main

import (
	"flag"
	"log"
	"net/http"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:8099", "address to listen on")
	flag.Parse()

	f := orbfake.New(
		orbfake.Theme{Name: "aurora", Bytes: 412032, Serial: 7, OK: true, Active: true},
		orbfake.Theme{Name: "carousel", Bytes: 388123, Serial: 9, OK: true},
		orbfake.Theme{Name: "damaged", Why: "art short", Bytes: 12, Serial: 2, OK: false},
	)
	f.Verbose = true

	// The scan the double serves is async because the firmware's is, so a human needs
	// something playing the role of the radio task or the wizard hangs on "scanning"
	// forever. The Go suite settles scans itself and never goes through this.
	go emulateRadioTask(f)

	state, mac, ip, _, _, _, _, _, _, _ := f.Device()
	log.Printf("orbfake listening on %s (card %s, %d themes, state=%s mac=%s ip=%s)",
		*addr, f.Card, len(f.Themes()), state, mac, ip)
	for _, t := range f.Themes() {
		log.Printf("  %-20s id=%s ok=%t active=%t", t.Name, t.ID, t.OK, t.Active)
	}
	log.Fatal(http.ListenAndServe(*addr, f))
}

// emulateRadioTask settles a scan 600 ms after it is armed, which is the shape of
// what net_post(NET_CMD_SCAN) plus scan_collect do on hardware.
func emulateRadioTask(f *orbfake.Orb) {
	wasScanning := false
	var armed time.Time
	for {
		_, _, _, _, _, _, _, scanning, _, _ := f.Device()
		switch {
		case scanning && !wasScanning:
			armed, wasScanning = time.Now(), true
		case scanning && wasScanning && time.Since(armed) > 600*time.Millisecond:
			f.SetNetworks(
				orbfake.Net{SSID: "lab-2g", RSSI: -47, Ch: 6},
				orbfake.Net{SSID: "lab-5g", RSSI: -58, Ch: 36},
				orbfake.Net{SSID: "guest", RSSI: -71, Ch: 11, Open: true},
			)
			wasScanning = false
		case !scanning:
			wasScanning = false
		}
		time.Sleep(80 * time.Millisecond)
	}
}
