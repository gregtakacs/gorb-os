package orbfake

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
)

// FeedKeyBodyMax is NET_FKEY_BODY_MAX in orb_net.c.
const FeedKeyBodyMax = 256

// serveFeedKey answers GET and POST /api/v1/flights/key the way the orb does:
// the key goes in and never comes back out; the reply says whether one is set,
// the header it rides in, and its last four characters. r == nil is the GET.
func (o *Orb) serveFeedKey(w http.ResponseWriter, r *http.Request) {
	state := ""
	if r != nil {
		body, ok := readBoundedBody(w, r, FeedKeyBodyMax)
		if !ok {
			return
		}
		var req struct {
			Key    *string `json:"key"`
			Header *string `json:"header"`
			Clear  bool    `json:"clear"`
		}
		if json.Unmarshal(body, &req) != nil {
			writeJSON(w, 400, `{"error":"key required (a string, at most 128)"}`)
			return
		}
		o.mu.Lock()
		d := o.deviceLocked()
		switch {
		case req.Clear:
			d.FeedKey, d.FeedKeyHeader, state = "", "", "cleared"
		case req.Key == nil || len(*req.Key) < 8 || len(*req.Key) > 128 || strings.ContainsAny(*req.Key, " \"\\"):
			o.mu.Unlock()
			writeJSON(w, 400, `{"error":"key must be 8 to 128 characters"}`)
			return
		default:
			d.FeedKey, d.FeedKeyHeader, state = *req.Key, "", "keyed"
			if req.Header != nil {
				d.FeedKeyHeader = *req.Header
			}
		}
		o.mu.Unlock()
	}
	o.mu.Lock()
	d := o.deviceLocked()
	key, hdr := d.FeedKey, d.FeedKeyHeader
	o.mu.Unlock()
	if hdr == "" {
		hdr = "x-api-key"
	}
	tail := ""
	if len(key) >= 4 {
		tail = key[len(key)-4:]
	}
	pre := ""
	if state != "" {
		pre = fmt.Sprintf(`"state":%q,`, state)
	}
	writeJSON(w, 200, fmt.Sprintf(`{%s"set":%t,"header":%q,"tail":%q}`, pre, key != "", hdr, tail))
}
