package main

// The orb's time zone list, served to the page.
//
// The zone picker on the join dialog lists exactly the rows the DEVICE lists —
// the same table the firmware serves at /api/v1/config and accepts a POST
// against — so a name the page offers is a name the firmware will take. The
// hub cannot ask the device at that moment: a board that has just been flashed
// is sitting on its own provisioning AP with no route to it from a wired hub,
// and a board mid-join answers to nothing until it has a lease. So the hub
// carries the table, and TestZonesMatchTheFirmwareTable pins it against the
// firmware's source on every check, because a picker that drifts from the
// device's table is a picker that produces refusals.
//
// There are no coordinates here on purpose. "Use my location" maps a position
// to a zone in the browser, out of the browser's own IANA data: every row's
// iana key is a zone the browser can compute an offset for, and the location's
// solar offset is its longitude over 15. A hub-shipped coordinate table would
// be a second truth about the same cities, and second truths drift.

import (
	"net/http"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbzones"
)

// handleZones is the read side: the zone table in the same shape the device's
// own config page uses, plus the coordinates. It sends nothing, opens nothing
// and needs no token — a list of zone names is the least sensitive thing on
// this hub.
func (s *Server) handleZones(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	s.jsonOK(w, map[string]any{"zones": orbzones.All()})
}
