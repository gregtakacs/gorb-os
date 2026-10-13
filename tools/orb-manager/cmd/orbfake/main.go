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
//	orbfake -advertise                  # also answer _orb._tcp over mDNS on 5353, so
//	                                    # the hub's browse list and tools/bench/browse_mdns.py
//	                                    # have something to find with no orb on a cable.
//	                                    # It binds with SO_REUSEADDR, so a responder
//	                                    # already on 5353 on this box is not a problem.
//	                                    # -mdns-port moves the answer port off 5353;
//	                                    # nothing but a browser told about the port can
//	                                    # then find the fake, the hub included.
package main

import (
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:8099", "address to listen on")
	advertise := flag.Bool("advertise", false,
		"announce _orb._tcp over mDNS too, so a real browser (the hub, or tools/bench/browse_mdns.py) can find this fake without an orb on a cable")
	mac := flag.String("mac", "44bd8d782618", "the 12 hex digits to announce and to serve from /api/v1/status; the instance name is derived from them as the firmware derives it")
	staIP := flag.String("ip", "192.168.2.34", "the IPv4 to answer the A record with")
	fw := flag.String("fw", "094fa22", "the fw= string; the status route reports the same value, one field two readers, as on the device")
	port := flag.Int("mdns-port", 0, "UDP port to answer mDNS on; 0 takes the standard 5353, and an ephemeral one is what lets a second advertiser run beside avahi")
	provisioning := flag.Bool("provisioning", false, "announce state=provisioning, the factory-reset shape item 10 depends on")
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

	// The identity is set before either reader can see it — the status route and the
	// advertiser take it from the same fields, which is the agreement Item 6's
	// cross-lock exists to test. Setting it after the log line below is what made the
	// startup line report a different MAC from the one on the wire.
	f.SetAddress(*mac, *staIP)
	f.SetFirmware(*fw)
	f.SetSTA(true)
	f.SetProvisioning(*provisioning)

	state, gmac, ip, _, _, _, _, _, _, _ := f.Device()
	log.Printf("orbfake listening on %s (card %s, %d themes, state=%s mac=%s ip=%s)",
		*addr, f.Card, len(f.Themes()), state, gmac, ip)
	for _, t := range f.Themes() {
		log.Printf("  %-20s id=%s ok=%t active=%t", t.Name, t.ID, t.OK, t.Active)
	}

	var adv *orbfake.Advertiser
	if *advertise {
		adv = f.Advertise(orbfake.AdvertiseConfig{ListenPort: *port})
		if err := adv.Serve(); err != nil {
			log.Fatalf("orbfake: advertise: %v", err)
		}
		// Serve returns once the burst is out; the responder keeps answering on its
		// own goroutine. The totals are printed on the way out, where a browse
		// actually has an answer to give.
		log.Printf("orbfake advertising orb-%s._orb._tcp (mac=%s fw=%s) on udp/%d: announce burst sent, answers and goodbyes until stop",
			macTail(*mac), *mac, *fw, orElseInt(*port, 5353))
	}

	// Ctrl-C (or the systemd stop) sends the TTL-0 flush before the process goes, so
	// a hub that had this fake on its card drops it now rather than showing a phantom
	// orb until the 120 s TTL expires, and prints the counters that answer "did the
	// browser ever ask?" without guessing.
	stopping := make(chan os.Signal, 1)
	signal.Notify(stopping, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-stopping
		if adv != nil {
			adv.Goodbye()
			read, answered, broken, neighbours, why := adv.Stats()
			log.Printf("orbfake mDNS totals: read=%d answered=%d broken=%d neighbours=%d last_defect=%q",
				read, answered, broken, neighbours, why)
			_ = adv.Close()
		}
		os.Exit(0)
	}()
	log.Fatal(http.ListenAndServe(*addr, f))
}

// macTail is the six digits the firmware builds the instance name out of, spelled
// the same way in the log line as on the wire.
func macTail(mac string) string {
	if len(mac) < 6 {
		return mac
	}
	return mac[len(mac)-6:]
}

func orElseInt(v, alt int) int {
	if v == 0 {
		return alt
	}
	return v
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
