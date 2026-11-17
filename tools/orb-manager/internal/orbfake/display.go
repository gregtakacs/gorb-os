package orbfake

import (
	"encoding/json"
	"fmt"
	"net/http"
)

// Display mirrors the dial's DISPLAY page (/api/v1/display, app-platform-plan §67):
// dim after idle, and the idle timeout. The zero value reads as the firmware's
// defaults through displayLocked.
type Display struct {
	Set      bool // false: the defaults below have not been overwritten
	DimOff   bool
	TimeoutS uint32
}

// idle_dim's clamp (IDLE_DIM_TIMEOUT_MIN_S/MAX_S) and the Kconfig default.
const (
	displayTimeoutMin     = 5
	displayTimeoutMax     = 86400
	displayTimeoutDefault = 600
)

func (o *Orb) displayJSON() string {
	o.mu.Lock()
	d := o.deviceLocked().Display
	o.mu.Unlock()
	to := d.TimeoutS
	if !d.Set || to == 0 {
		to = displayTimeoutDefault
	}
	dim := 1
	if d.DimOff {
		dim = 0
	}
	return fmt.Sprintf(`{"dim":%d,"timeout_s":%d}`, dim, to)
}

func (o *Orb) serveDisplay(w http.ResponseWriter) {
	writeJSON(w, 200, o.displayJSON())
}

// serveDisplayPost is h_display_post: either key or both (ints), the timeout clamped
// the way idle_dim clamps it, a body naming neither refused.
func (o *Orb) serveDisplayPost(w http.ResponseWriter, r *http.Request) {
	body, ok := readBoundedBody(w, r, 256)
	if !ok {
		return
	}
	var m struct {
		Dim      *int64 `json:"dim"`
		TimeoutS *int64 `json:"timeout_s"`
	}
	if json.Unmarshal(body, &m) != nil || (m.Dim == nil && m.TimeoutS == nil) {
		writeJSON(w, 400, `{"error":"no display setting in body"}`)
		return
	}
	if m.TimeoutS != nil && *m.TimeoutS <= 0 {
		writeJSON(w, 400, `{"error":"timeout_s must be positive"}`)
		return
	}
	o.mu.Lock()
	d := &o.deviceLocked().Display
	if !d.Set {
		d.Set, d.TimeoutS = true, displayTimeoutDefault
	}
	if m.Dim != nil {
		d.DimOff = *m.Dim == 0
	}
	if m.TimeoutS != nil {
		t := *m.TimeoutS
		if t < displayTimeoutMin {
			t = displayTimeoutMin
		}
		if t > displayTimeoutMax {
			t = displayTimeoutMax
		}
		d.TimeoutS = uint32(t)
	}
	o.mu.Unlock()
	writeJSON(w, 200, o.displayJSON())
}
