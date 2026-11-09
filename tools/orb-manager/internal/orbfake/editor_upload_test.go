package orbfake

import (
	"archive/zip"
	"bytes"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

// An uploaded container keeps its release stamp and its editor region, and the double
// streams that region back from /api/v1/apps/editor as the device does: the hosted
// editors (theme-editor-kit-plan §8) are benched against it. The container is one the
// tree really ships, so the word positions are held to a writer, not to this file.
func TestAnUploadKeepsItsVersionAndEditor(t *testing.T) {
	body, err := os.ReadFile("../../apps/weather.orbapp")
	if err != nil {
		t.Skip("no built weather.orbapp: ", err)
	}
	o := New()
	srv := httptest.NewServer(o)
	defer srv.Close()
	r, err := http.Post(srv.URL+"/api/v1/sd/app", "application/octet-stream", bytes.NewReader(body))
	if err != nil || r.StatusCode != 200 {
		t.Fatalf("upload: %v %v", err, r)
	}
	r.Body.Close()
	a := o.apps["weather"]
	if a == nil || !strings.HasPrefix(a.Version, "v") {
		t.Fatalf("the upload's version was not kept: %+v", a)
	}
	r, err = http.Get(srv.URL + "/api/v1/apps/editor?id=weather")
	if err != nil || r.StatusCode != 200 {
		t.Fatalf("editor: %v %v", err, r)
	}
	region, _ := io.ReadAll(r.Body)
	r.Body.Close()
	zr, err := zip.NewReader(bytes.NewReader(region), int64(len(region)))
	if err != nil {
		t.Fatalf("the served region is not the container's ZIP: %v", err)
	}
	found := false
	for _, f := range zr.File {
		found = found || f.Name == "editor.json"
	}
	if !found {
		t.Error("the served region carries no editor.json")
	}
}
