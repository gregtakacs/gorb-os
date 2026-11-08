package orbfake

import (
	"net/http/httptest"
	"strings"
	"testing"
)

// The carousel route (§59, h_carousel_get/post): the installed apps in the owner's order,
// each saying whether it takes part, and the rotation; a form body sets any part of it.
func TestCarouselOrderAndRotation(t *testing.T) {
	o := New().WithCardApps(
		CardApp{Stem: "weather", Slug: "weather", Title: "Weather", Installed: true},
		CardApp{Stem: "flights", Slug: "flights", Title: "Flights", Installed: true},
		CardApp{Stem: "thermo", Slug: "thermo", Title: "Thermo"},
	)
	srv := httptest.NewServer(o)
	defer srv.Close()

	code, body := appDo(t, srv, "GET", "/api/v1/carousel", nil)
	if code != 200 || !strings.Contains(body, `"rotate":{"on":false,"every_s":60}`) ||
		strings.Index(body, `"weather"`) > strings.Index(body, `"flights"`) || strings.Contains(body, "thermo") {
		t.Fatalf("fresh carousel: %d %s", code, body)
	}
	code, body = appDo(t, srv, "POST", "/api/v1/carousel", []byte("order=flights,weather&rotate=1&every_s=300&apps=flights"))
	if code != 200 || strings.Index(body, `"flights"`) > strings.Index(body, `"weather"`) ||
		!strings.Contains(body, `{"slug":"flights","title":"Flights","rotate":true}`) ||
		!strings.Contains(body, `{"slug":"weather","title":"Weather","rotate":false}`) ||
		!strings.Contains(body, `"rotate":{"on":true,"every_s":300}`) {
		t.Fatalf("set order and rotation: %d %s", code, body)
	}
	code, body = appDo(t, srv, "POST", "/api/v1/carousel", []byte("every_s=5"))
	if code != 400 || !strings.Contains(body, "10 s to a day") {
		t.Fatalf("an interval under 10 s must be refused: %d %s", code, body)
	}
	code, body = appDo(t, srv, "POST", "/api/v1/carousel", []byte("apps="))
	if code != 200 || strings.Contains(body, `"rotate":true`) || !strings.Contains(body, `"every_s":300`) {
		t.Fatalf("an empty apps= leaves none taking part and the rest as it was: %d %s", code, body)
	}
	// The device decodes nothing (httpd_query_key_value): percent-encoded commas are one
	// unknown slug, so the owner's order is lost (slot order, weather first, again) and
	// nobody joins the rotation; no error is said. Gold, 2026-11-08.
	code, body = appDo(t, srv, "POST", "/api/v1/carousel", []byte("order=flights%2Cweather&apps=weather%2Cflights"))
	if code != 200 || strings.Index(body, `"weather"`) > strings.Index(body, `"flights"`) || strings.Contains(body, `"rotate":true}`) {
		t.Fatalf("an encoded comma must not split the list, as on the device: %d %s", code, body)
	}
}
