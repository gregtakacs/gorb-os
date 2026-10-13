// Command orbfake runs the internal/orbfake device double as a listening server, for
// a human driving a real hub through a browser. It answers the theme routes
// main/net/orb_net.c registers, with h_sd's FORMAT-2 row shapes, so the Theme card can
// be exercised with no orb on a cable.
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

	log.Printf("orbfake listening on %s (card %s, %d themes)", *addr, f.Card, len(f.Themes()))
	for _, t := range f.Themes() {
		log.Printf("  %-20s id=%s ok=%t active=%t", t.Name, t.ID, t.OK, t.Active)
	}
	log.Fatal(http.ListenAndServe(*addr, f))
}
