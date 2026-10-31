package main

import (
	"bytes"
	"net/http"
	"os"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// calls counts the requests the fake orb saw whose line contains frag.
func calls(f *orbfake.Orb, frag string) int {
	return countCalls(f, func(l string) bool { return strings.Contains(l, frag) })
}

// A preview plane the hub has read once is the same bytes on any orb and after any
// restart, so a second hub over the same cache directory must draw it without asking
// the card. A theme the card re-bakes (a new serial) is a different plane and is read.
func TestPlanesSurviveAHubRestart(t *testing.T) {
	dir := t.TempDir()
	plane := make([]byte, 4+2*2*2)
	plane[0], plane[2], plane[4] = 2, 2, 0x1F

	var id string // the fake derives a row's id from its name, so it is read back
	hub := func(serial int) (*Server, string, *orbfake.Orb) {
		f := orbfake.New(orbfake.Theme{Name: "Aurora", App: "clock",
			Bytes: 4096, Serial: serial, OK: true})
		id = f.Themes()[0].ID
		f.SetPreview(id, plane)
		s, ip, _, _ := fakeOrbHubWith(t, f)
		s.planeCache = diskCache{dir: dir, max: 10}
		list, err := s.device.ThemeList(ip)
		if err != nil {
			t.Fatal(err)
		}
		s.rememberList(ip, list)
		return s, ip, f
	}

	s, ip, f := hub(7)
	hit, err := s.fetchPreviewPlane(ip, id)
	if err != nil || hit.code != http.StatusOK {
		t.Fatalf("first read: %d %v", hit.code, err)
	}
	if calls(f, "/api/v1/sd/preview") == 0 {
		t.Fatal("the first read never dialed the card")
	}

	s2, ip2, f2 := hub(7) // a restarted hub: fresh memory, same disk
	hit2, err := s2.fetchPreviewPlane(ip2, id)
	if err != nil || hit2.code != http.StatusOK || !bytes.Equal(hit2.body, plane) {
		t.Fatalf("restarted hub read: %d %v, body equal %v", hit2.code, err, bytes.Equal(hit2.body, plane))
	}
	if n := calls(f2, "/api/v1/sd/preview"); n != 0 {
		t.Errorf("a restarted hub dialed the card %d times for a plane it had on disk", n)
	}
	if !s2.previewFresh(ip2, id) {
		t.Error("previewFresh does not see the disk copy, so the read-ahead would dial for it")
	}

	s3, ip3, f3 := hub(8) // the theme was re-baked: same id, new serial
	if _, err := s3.fetchPreviewPlane(ip3, id); err != nil {
		t.Fatal(err)
	}
	if calls(f3, "/api/v1/sd/preview") == 0 {
		t.Error("a re-baked theme was served from the old bake's cache entry")
	}
}

// An editor region read off a dial running release X is cached under X, so the next
// hub to meet a dial on X serves it from disk; a dial on another release is read.
// The library gate still comes first (TestEditorIsServedFromTheLibraryOnlyWhenTheVersionsAgree).
func TestEditorRegionComesOffTheDiskForTheSameRelease(t *testing.T) {
	editorInvalidate()
	t.Cleanup(editorInvalidate)
	dir := t.TempDir()
	cache := t.TempDir()
	cloneFixture(t, dir, "sentinel")
	body, err := os.ReadFile(dir + "/sentinel.orbapp")
	if err != nil {
		t.Skipf("no sentinel fixture: %v", err)
	}
	region, err := orbnet.AppEditorRegion(body)
	if err != nil {
		t.Skipf("the sentinel fixture carries no editor region: %v", err)
	}

	open := func(dialStamp string) int {
		t.Helper()
		editorInvalidate() // a restart: no memo, only the disk
		orb := orbfake.New().WithCardApps(orbfake.CardApp{
			Stem: "sentinel", Slug: "sentinel", Title: "Sentinel",
			Installed: true, Enabled: true, Version: dialStamp, Editor: region,
		})
		s, ip, _, f := fakeOrbHubWith(t, orb)
		s.lib = &Library{AppsDir: t.TempDir()} // no shelf copy: only the dial or the disk
		s.editorCache = diskCache{dir: cache, max: 4}
		rec := editorGet(t, s, "/editor/"+ip+"/sentinel/", true)
		if rec.Code != http.StatusOK {
			t.Fatalf("%s: status = %d body %s", dialStamp, rec.Code, rec.Body)
		}
		return calls(f, "/api/v1/apps/editor")
	}

	if n := open("v1.0.0"); n == 0 {
		t.Fatal("the first open never read the dial")
	}
	if n := open("v1.0.0"); n != 0 {
		t.Errorf("a hub meeting the same release read the dial %d times for a region on disk", n)
	}
	if n := open("v1.0.1"); n == 0 {
		t.Errorf("a new release was served from the old release's cache entry (dialed %d)", n)
	}
}
