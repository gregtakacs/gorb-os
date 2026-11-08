package orbfake

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

// PlacesBodyMax is NET_PLACES_BODY_MAX in orb_net.c.
const PlacesBodyMax = 256

// Place mirrors orb_place_rec_t: one of the three places beside home (radar-plan §2.1).
type Place struct {
	Set       bool
	Lat, Lon  float64
	Name      string
	Zone      string // IANA: the one named, else the fake's stand-in proposal's
	ZoneNamed bool
}

// placeZoneJSON is h_places's zone fields: the zone, its offset now and its daylight flag,
// from Go's own zone data standing in for the orb's zone table and rule (orb_tzrule.c).
func placeZoneJSON(p Place, now time.Time) string {
	loc, err := time.LoadLocation(p.Zone)
	if p.Zone == "" || err != nil {
		return ""
	}
	t := now.In(loc)
	_, off := t.Zone()
	return fmt.Sprintf(`,"zone":%q,"zone_named":%t,"tz":"","tz_offset":%d,"dst":%t,"exact":true`, p.Zone, p.ZoneNamed, off/60, t.IsDST())
}

func (o *Orb) placesJSON(state string) string {
	o.mu.Lock()
	d := o.deviceLocked()
	ps, gen := d.Places, d.PlacesGen
	o.mu.Unlock()
	b := "{"
	if state != "" {
		b += fmt.Sprintf(`"state":%q,`, state)
	}
	b += fmt.Sprintf(`"gen":%d,"places":[`, gen)
	for i, p := range ps {
		if i > 0 {
			b += ","
		}
		if p.Set {
			b += fmt.Sprintf(`{"i":%d,"set":true,"lat":%.6f,"lon":%.6f,"name":%q%s}`, i, p.Lat, p.Lon, p.Name, placeZoneJSON(p, time.Now()))
		} else {
			b += fmt.Sprintf(`{"i":%d,"set":false}`, i)
		}
	}
	return b + "]}"
}

// servePlacesMaps follows h_places_maps: the maps gen moves.
func (o *Orb) servePlacesMaps(w http.ResponseWriter) {
	o.mu.Lock()
	d := o.deviceLocked()
	d.MapsGen++
	g := d.MapsGen
	o.mu.Unlock()
	writeJSON(w, 200, fmt.Sprintf(`{"state":"refreshed","maps_gen":%d}`, g))
}

func (o *Orb) servePlacesGet(w http.ResponseWriter) {
	writeJSON(w, 200, o.placesJSON(""))
}

// servePlacesPost follows h_places_post: the slot first, then clear or a position, the
// escape rule, the name's length; a point without a name is named as home's would be.
func (o *Orb) servePlacesPost(w http.ResponseWriter, r *http.Request) {
	raw, err := io.ReadAll(io.LimitReader(r.Body, PlacesBodyMax+1))
	if err != nil || len(raw) == 0 {
		writeJSON(w, 400, `{"error":"content-length required"}`)
		return
	}
	if len(raw) > PlacesBodyMax {
		writeJSON(w, 413, `{"error":"body too large"}`)
		return
	}
	var req struct {
		I     *int     `json:"i"`
		Clear int      `json:"clear"`
		Lat   *float64 `json:"lat"`
		Lon   *float64 `json:"lon"`
		Name  *string  `json:"name"`
		Zone  *string  `json:"zone"`
	}
	if json.Unmarshal(raw, &req) != nil || req.I == nil || *req.I < 0 || *req.I > 2 {
		writeJSON(w, 400, `{"error":"i (0-2) required"}`)
		return
	}
	i := *req.I
	if req.Clear != 0 {
		o.mu.Lock()
		d := o.deviceLocked()
		d.Places[i] = Place{}
		d.PlacesGen++
		o.mu.Unlock()
		writeJSON(w, 200, o.placesJSON("cleared"))
		return
	}
	if req.Lat == nil || req.Lon == nil {
		writeJSON(w, 400, `{"error":"lat and lon required"}`)
		return
	}
	if strings.Contains(string(raw), `\`) {
		writeJSON(w, 400, `{"error":"no escapes"}`)
		return
	}
	if *req.Lat < -90 || *req.Lat > 90 || *req.Lon < -180 || *req.Lon > 180 {
		writeJSON(w, 400, `{"error":"position out of range"}`)
		return
	}
	name := ""
	if req.Name != nil {
		if len(*req.Name) >= 40 {
			writeJSON(w, 400, `{"error":"name not a string, or too long"}`)
			return
		}
		name = *req.Name
	}
	// the zone: a named one the zone data holds, else the stand-in proposal's (the orb's is
	// its nearest city's)
	_, zone, _ := fakeProposal(*req.Lat, *req.Lon)
	named := req.Zone != nil && *req.Zone != ""
	if named {
		if _, err := time.LoadLocation(*req.Zone); err != nil || !strings.Contains(*req.Zone, "/") {
			writeJSON(w, 400, `{"error":"zone not in the zone table"}`)
			return
		}
		zone = *req.Zone
	}
	if name == "" {
		name, _, _ = fakeProposal(*req.Lat, *req.Lon)
	}
	o.mu.Lock()
	d := o.deviceLocked()
	d.Places[i] = Place{Set: true, Lat: *req.Lat, Lon: *req.Lon, Name: name, Zone: zone, ZoneNamed: named}
	d.PlacesGen++
	o.mu.Unlock()
	writeJSON(w, 200, o.placesJSON("placed"))
}
