package orbfake

import (
	"fmt"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
)

// Carousel mirrors orb_rotation.c (app-platform-plan §59): the owner's order and the
// rotation. Zero value: no order, rotation off at 60 s, no app taking part.
type Carousel struct {
	Order   []string
	On      bool
	EveryS  uint32
	Rotates []string
}

func (o *Orb) carouselLocked() *Carousel {
	d := o.deviceLocked()
	if d.Carousel.EveryS == 0 {
		d.Carousel.EveryS = 60
	}
	return &d.Carousel
}

// carouselJSON is carousel_send: the installed card apps in the walk's order (the owner's
// order first, the rest in slot order), each saying whether it takes part.
func (o *Orb) carouselJSON() string {
	o.mu.Lock()
	defer o.mu.Unlock()
	c := o.carouselLocked()
	rank := func(slug string, slot int) int {
		for i, s := range c.Order {
			if s == slug {
				return i
			}
		}
		return 16 + slot
	}
	type row struct {
		slug, title string
		rank        int
	}
	var rows []row
	for slot, slug := range o.appsOrder {
		a := o.apps[slug]
		if a == nil || !a.Installed || a.Failed || a.Builtin {
			continue
		}
		t := a.Title
		if t == "" {
			t = slug
		}
		rows = append(rows, row{slug, t, rank(slug, slot)})
	}
	sort.SliceStable(rows, func(i, j int) bool { return rows[i].rank < rows[j].rank })
	in := map[string]bool{}
	for _, s := range c.Rotates {
		in[s] = true
	}
	var b strings.Builder
	b.WriteString(`{"apps":[`)
	for i, r := range rows {
		if i > 0 {
			b.WriteString(",")
		}
		fmt.Fprintf(&b, `{"slug":%q,"title":%q,"rotate":%t}`, r.slug, r.title, in[r.slug])
	}
	fmt.Fprintf(&b, `],"rotate":{"on":%t,"every_s":%d}}`, c.On, c.EveryS)
	return b.String()
}

func csvSlugs(v string) []string {
	var out []string
	seen := map[string]bool{}
	for _, s := range strings.Split(v, ",") {
		s = strings.TrimSpace(s)
		if s != "" && !seen[s] {
			seen[s] = true
			out = append(out, s)
		}
	}
	return out
}

// serveCarouselPost is h_carousel_post: a form body, any of order, rotate, every_s, apps.
func (o *Orb) serveCarouselPost(w http.ResponseWriter, r *http.Request) {
	body, ok := readBoundedBody(w, r, 640)
	if !ok {
		return
	}
	q := rawForm(string(body))
	o.mu.Lock()
	c := o.carouselLocked()
	next := *c
	if v, has := q["order"]; has {
		next.Order = csvSlugs(v[0])
	}
	if v := q.Get("rotate"); v != "" {
		next.On = v == "1"
	}
	if v := q.Get("every_s"); v != "" {
		n, _ := strconv.ParseUint(v, 10, 32)
		if n < 10 || n > 86400 {
			o.mu.Unlock()
			writeJSON(w, 400, `{"error":"the interval runs 10 s to a day"}`)
			return
		}
		next.EveryS = uint32(n)
	}
	if v, has := q["apps"]; has {
		next.Rotates = csvSlugs(v[0])
	}
	*c = next
	o.mu.Unlock()
	writeJSON(w, 200, o.carouselJSON())
}

// rawForm reads a form body the way the device's httpd_query_key_value does: split on & and
// the first =, and NOTHING decoded. "order=a%2Cb" is one slug named "a%2Cb" on the device,
// which matches no app: the owner's order falls back to slot order and no error is said. A double that
// decoded the body (url.ParseQuery, until 2026-11-08) passed a hub that sent its commas
// percent-encoded, while Gold ignored every reorder and every second rotation box.
func rawForm(body string) url.Values {
	q := url.Values{}
	for _, kv := range strings.Split(body, "&") {
		if kv == "" {
			continue
		}
		k, v, _ := strings.Cut(kv, "=")
		q[k] = append(q[k], v)
	}
	return q
}
