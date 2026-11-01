package orbfake

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"strings"
)

// HomeBodyMax is NET_HOME_BODY_MAX in orb_net.c.
const HomeBodyMax = 256

// Home mirrors orb_home_t: the OS home location (flight-tracker-design §6.4).
type Home struct {
	Set      bool
	Lat, Lon float64
	Place    string
	Zone     string // IANA name of the zone it set, "" when it left the zone alone
	TZ       string // the exact tz string it wrote
}

// fakeProposal stands in for the orb's city table: the double has no
// geography, so every point is "near Testville" in the Phoenix zone. The hub
// renders whatever the orb proposes; the real proposal is tools/host_geo_test.py's.
func fakeProposal(lat, lon float64) (place, zone, tz string) {
	return "near Testville", "America/Phoenix", "US/Arizona"
}

func (o *Orb) homeJSON(state string, prop string) string {
	o.mu.Lock()
	d := o.deviceLocked()
	h, tz, off := d.Home, d.TZ, d.TZOffset
	o.mu.Unlock()
	if tz == "" {
		tz = "UTC"
	}
	b := "{"
	if state != "" {
		b += fmt.Sprintf(`"state":%q,`, state)
	}
	if h.Set {
		b += fmt.Sprintf(`"home":{"set":true,"lat":%.6f,"lon":%.6f,"place":%q,"zone":%q,"follows":%t}`,
			h.Lat, h.Lon, h.Place, h.Zone, h.TZ != "" && h.TZ == tz)
	} else {
		b += `"home":{"set":false}`
	}
	b += prop
	label := tz
	if h.Set && h.TZ != "" && h.TZ == tz && h.Zone != "" {
		label = h.Zone
	}
	return b + fmt.Sprintf(`,"tz":%q,"zone_label":%q,"tz_offset":%d}`, tz, label, off)
}

func (o *Orb) serveHomeGet(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	var lat, lon float64
	switch {
	case q.Get("code") != "":
		// No decoder here either: a full code is one fixed point, which is
		// all the hub needs to prove it pins what the orb answered.
		if !strings.Contains(q.Get("code"), "+") {
			writeJSON(w, 400, `{"error":"no + in the code"}`)
			return
		}
		lat, lon = 33.448437, -112.073938
	case q.Get("lat") != "" && q.Get("lon") != "":
		var e1, e2 error
		lat, e1 = strconv.ParseFloat(q.Get("lat"), 64)
		lon, e2 = strconv.ParseFloat(q.Get("lon"), 64)
		if e1 != nil || e2 != nil || lat < -90 || lat > 90 || lon < -180 || lon > 180 {
			writeJSON(w, 400, `{"error":"latitude is not a number"}`)
			return
		}
	default:
		writeJSON(w, 200, o.homeJSON("", ""))
		return
	}
	place, zone, tz := fakeProposal(lat, lon)
	writeJSON(w, 200, o.homeJSON("", fmt.Sprintf(
		`,"proposal":{"lat":%.6f,"lon":%.6f,"place":%q,"zone":%q,"tz":%q,"exact":true,"city_km":30}`,
		lat, lon, place, zone, tz)))
}

func (o *Orb) serveHomePost(w http.ResponseWriter, r *http.Request) {
	body, ok := readBoundedBody(w, r, HomeBodyMax)
	if !ok {
		return
	}
	var req struct {
		Clear bool     `json:"clear"`
		Lat   *float64 `json:"lat"`
		Lon   *float64 `json:"lon"`
		TZ    *string  `json:"tz"`
		Zone  *string  `json:"zone"`
		Place *string  `json:"place"`
	}
	if json.Unmarshal(body, &req) != nil {
		writeJSON(w, 400, `{"error":"lat and lon (or code) required"}`)
		return
	}
	o.mu.Lock()
	d := o.deviceLocked()
	switch {
	case req.Clear:
		d.Home = Home{}
	case req.Lat == nil || req.Lon == nil || *req.Lat < -90 || *req.Lat > 90 || *req.Lon < -180 || *req.Lon > 180:
		o.mu.Unlock()
		writeJSON(w, 400, `{"error":"lat and lon (or code) required"}`)
		return
	default:
		place, zone, tz := fakeProposal(*req.Lat, *req.Lon)
		h := Home{Set: true, Lat: *req.Lat, Lon: *req.Lon, Place: place, Zone: zone, TZ: tz}
		if req.Place != nil && *req.Place != "" {
			h.Place = *req.Place
		}
		if req.TZ != nil {
			h.TZ, h.Zone = *req.TZ, ""
			if req.Zone != nil {
				h.Zone = *req.Zone
			}
			if *req.TZ == "keep" {
				h.TZ, h.Zone = "", ""
			}
		}
		if h.TZ != "" {
			d.TZ = h.TZ
			d.TZOffset = offsetFor(h.TZ)
		}
		d.Home = h
	}
	o.mu.Unlock()
	writeJSON(w, 200, o.homeJSON("homed", ""))
}
