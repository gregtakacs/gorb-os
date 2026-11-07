package orbfake

import (
	"encoding/json"
	"fmt"
	"net/http"
)

// Units mirrors orb_units_t (weather-plan §2.1): indexes into the key lists below,
// in orb_units.c's order. The zero value is metric, the orb's default.
type Units struct {
	Temp, Speed, Dist, Time int
	Gen                     uint32
}

// unitKeys is orb_units.c KEYS: the route's spelling, by quantity then value. Time
// (24h, 12h) joined in OS v1.2.16.
var unitKeys = [4][]string{{"c", "f"}, {"kmh", "mph", "kn"}, {"km", "mi", "nm"}, {"24h", "12h"}}

func (o *Orb) unitsJSON() string {
	o.mu.Lock()
	u := o.deviceLocked().Units
	o.mu.Unlock()
	return fmt.Sprintf(`{"temp":%q,"speed":%q,"dist":%q,"time":%q,"gen":%d}`,
		unitKeys[0][u.Temp], unitKeys[1][u.Speed], unitKeys[2][u.Dist], unitKeys[3][u.Time], u.Gen+1)
}

func (o *Orb) serveUnits(w http.ResponseWriter) {
	writeJSON(w, 200, o.unitsJSON())
}

// serveUnitsPost is h_units_post: any subset of the three keys, a value outside
// the list refused whole, a body naming none refused as a request for nothing.
func (o *Orb) serveUnitsPost(w http.ResponseWriter, r *http.Request) {
	body, ok := readBoundedBody(w, r, 256)
	if !ok {
		return
	}
	var m map[string]json.RawMessage
	if json.Unmarshal(body, &m) != nil {
		writeJSON(w, 400, `{"error":"no unit in body"}`)
		return
	}
	o.mu.Lock()
	d := o.deviceLocked()
	u := d.Units
	dst := [4]*int{&u.Temp, &u.Speed, &u.Dist, &u.Time}
	named := 0
	for k, name := range []string{"temp", "speed", "dist", "time"} {
		raw, has := m[name]
		if !has {
			continue
		}
		var v string
		found := -1
		if json.Unmarshal(raw, &v) == nil {
			for i, key := range unitKeys[k] {
				if key == v {
					found = i
				}
			}
		}
		if found < 0 {
			o.mu.Unlock()
			writeJSON(w, 400, `{"error":"unknown unit"}`)
			return
		}
		*dst[k] = found
		named++
	}
	if named == 0 {
		o.mu.Unlock()
		writeJSON(w, 400, `{"error":"no unit in body"}`)
		return
	}
	if u.Temp != d.Units.Temp || u.Speed != d.Units.Speed || u.Dist != d.Units.Dist {
		u.Gen++
	}
	d.Units = u
	o.mu.Unlock()
	writeJSON(w, 200, o.unitsJSON())
}
