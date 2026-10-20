package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbfake"
)

// adopt.go's two doors, tested against the real double rather than a hand-rolled
// reply: the fake answers /api/v1/status the way orb_net.c does, which is the
// only answer the add dialog is allowed to trust, and it stages /api/v1/ota the
// way the bootloader does, which is the only shape the update handler may drive.

// fakeOrbServer hosts one fake device and points the hub's three dial clients at
// its port. The address every test dials is loopback, which the add dialog does
// not refuse — see adopt.go for why the bench is the exception.
func fakeOrbServer(t *testing.T, s *Server, o *orbfake.Orb) (string, string) {
	t.Helper()
	srv := httptest.NewServer(o)
	t.Cleanup(srv.Close)
	_, port, _ := strings.Cut(strings.TrimPrefix(srv.URL, "http://"), ":")
	s.orb.DevicePort = port
	s.device.DevicePort = port
	s.ota.DevicePort = port
	return "127.0.0.1", port
}

func postJSONHandler(t *testing.T, h http.HandlerFunc, body string) (int, map[string]any) {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, "/", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	h(rec, req)
	var out map[string]any
	_ = json.Unmarshal(rec.Body.Bytes(), &out)
	return rec.Code, out
}

// The dialog's promise in two halves: an orb answering earns a row and a grant,
// and anything else — including a host that answers with something that is not
// an orb — earns a refusal and no change to the hub at all. A typed typo must
// cost the operator one sentence, not a poisoned address book.
func TestAddByIPAdoptsOnlyAnAnsweringOrb(t *testing.T) {
	s := newHub()
	o := orbfake.New()
	o.SetAddress("44bd8d782618", "127.0.0.1")
	o.SetFirmware("v1.0.0+aaaaaaa")
	ip, port := fakeOrbServer(t, s, o)

	code, env := postJSONHandler(t, s.handleOrbAdd, fmt.Sprintf(`{"ip":%q}`, ip))
	if code != http.StatusOK || env["ok"] != true {
		t.Fatalf("add of an answering orb refused: %d %v", code, env["error"])
	}
	if env["data"].(map[string]any)["mac"] != "44bd8d782618" {
		t.Errorf("the row arrived with the wrong mac: %v", env["data"])
	}
	if _, ok := s.fleet.ByMAC("44bd8d782618"); !ok {
		t.Error("the answering orb never became a fleet row")
	}
	if !s.grants.allowed(ip) || s.grants.bound(ip) == "" {
		t.Error("the adopted address was not granted, or was granted unbound")
	}

	// A host that answers with a shape that is not an orb gets refused, and the
	// hub must not have moved: no grant, no row.
	not := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"product":"toaster","toast":"done"}`)
	}))
	t.Cleanup(not.Close)
	_, nport, _ := strings.Cut(strings.TrimPrefix(not.URL, "http://"), ":")
	s.device.DevicePort = nport
	code, env = postJSONHandler(t, s.handleOrbAdd, `{"ip":"127.0.0.1"}`)
	if code == http.StatusOK || env["ok"] == true {
		t.Errorf("a toaster was adopted: %d %v", code, env)
	}
	if s.grants.allowed("198.18.0.1") {
		t.Error("a refused add learned an address anyway")
	}
	s.device.DevicePort = port

	for _, bad := range []string{"example.com", "999.1.1.1", "", "10.0.0.0.1"} {
		code, env = postJSONHandler(t, s.handleOrbAdd, fmt.Sprintf(`{"ip":%q}`, bad))
		if code == http.StatusOK && env["ok"] == true {
			t.Errorf("%q was accepted as an address", bad)
		}
	}
}

// The bundle the update tests run on: a real WriteBundle with a stamped app
// descriptor, because the version gate reads the bundle's stamp out of the
// image's own bytes and no shortcut is honest about that.

// The update door's gates, in the order they fire: an address the hub will not
// dial, a stamp that is not an upgrade, and only then the two device requests
// that do the work — stage into the spare slot, reboot into it.
func TestOrbUpdateGatesThenStagesAndReboots(t *testing.T) {
	s := newHub()
	s.adoptFirmware(stampedBundle(t, "v1.0.13+aaaaaaa"))
	o := orbfake.New()
	o.SetAddress("44bd8d782618", "127.0.0.1")
	o.SetFirmware("v1.0.0+aaaaaaa")
	o.SetOTANextFW("v1.0.13+aaaaaaa")
	ip, _ := fakeOrbServer(t, s, o)

	// Ungated: mayDial refuses before any dial at all.
	code, env := postJSONHandler(t, s.handleOrbUpdate, fmt.Sprintf(`{"ip":%q}`, ip))
	if env["ok"] == true || !strings.Contains(fmt.Sprint(env["error"]), "will not dial") {
		t.Fatalf("update of an ungranted address: %d %v", code, env["error"])
	}

	// Granted, and the downgrade refusal says which side is newer in the hub's
	// own words rather than leaving the button to discover it.
	s.grants.learnAs(ip, "44:bd:8d:78:26:18")
	o.SetFirmware("v9.9.9+bbbbbbb")
	_, env = postJSONHandler(t, s.handleOrbUpdate, fmt.Sprintf(`{"ip":%q}`, ip))
	if env["ok"] == true || !strings.Contains(fmt.Sprint(env["error"]), "newer than this hub") {
		t.Errorf("a downgrade was not refused as one: %v", env["error"])
	}
	// The refusal is also a contract with the page, which must recognise a
	// downgrade without parsing prose and know that a second press exists. The
	// fields are the whole mechanism: read them or do not offer the retry.
	if env["overridable"] != true {
		t.Errorf("a downgrade refusal must carry overridable:true so the modal can offer one confirm: %v", env)
	}
	if env["verdict"] != "downgrade" {
		t.Errorf("a downgrade refusal must carry verdict \"downgrade\", got %v", env["verdict"])
	}
	// Forced, the same downgrade is pushed: rollback is enabled in the bootloader,
	// so the orb reverts an image that never validates and the operator's judgement
	// is a press rather than a refusal.
	if _, env = postJSONHandler(t, s.handleOrbUpdate, fmt.Sprintf(`{"ip":%q,"force":true}`, ip)); env["ok"] != true {
		t.Errorf("a forced downgrade was not pushed: %v", env["error"])
	}
	// A no-op gets no retry: forcing it would reboot a board to change nothing.
	o.SetFirmware("v1.0.13+aaaaaaa")
	_, env = postJSONHandler(t, s.handleOrbUpdate, fmt.Sprintf(`{"ip":%q}`, ip))
	if env["overridable"] == true {
		t.Error("a no-op refusal carries overridable, so the modal would offer a press that changes nothing")
	}
	if env["verdict"] == nil {
		t.Error("a no-op refusal carries no verdict, so the page cannot say why it stopped")
	}
	if env["ok"] == true || !strings.Contains(fmt.Sprint(env["error"]), "already runs") {
		t.Errorf("a same-version update was not refused as a no-op: %v", env["error"])
	}
	o.SetFirmware("v1.0.0+aaaaaaa")
	o.SetOTANextFW("v1.0.13+aaaaaaa")

	// The upgrade path. The device must show the staged slot consumed by exactly
	// the reboot this handler sent, and the reply must carry both stamps: the
	// page renders the line "v1.0.0 → v1.0.13" out of the body, and a hub that
	// reported only one of them could not mean it.
	o.SetFirmware("v1.0.0+aaaaaaa")
	code, env = postJSONHandler(t, s.handleOrbUpdate, fmt.Sprintf(`{"ip":%q}`, ip))
	if code != http.StatusOK || env["ok"] != true {
		t.Fatalf("the upgrade was refused: %d %v", code, env["error"])
	}
	data := env["data"].(map[string]any)
	if data["from"] != "v1.0.0+aaaaaaa" || data["to"] != "v1.0.13+aaaaaaa" {
		t.Errorf("the reply does not name both stamps: %v", data)
	}
	staged, booted, fw := o.OTAState()
	if staged != 0 || !booted {
		t.Errorf("the device did not take the two-request shape: staged=%d booted=%v", staged, booted)
	}
	if fw != "v1.0.13+aaaaaaa" {
		t.Errorf("after the reboot the orb reports %q, want the stamped version", fw)
	}

	// And a second request for the same board finds nothing to do: the gate is
	// live state, not a one-time permission.
	_, env = postJSONHandler(t, s.handleOrbUpdate, fmt.Sprintf(`{"ip":%q}`, ip))
	if env["ok"] == true {
		t.Error("the update door opened twice for one image")
	}
}

// The New Orb gate reads one fact off the port list — the vendor is Espressif —
// and one absence: no orb has ever answered as that MAC. The unit half of that
// is theVID test below; the product id must not enter the decision, because the
// board in download mode and the board mid-boot-loop enumerate differently and
// the door must find both.
func TestEspressifGateIgnoresProductID(t *testing.T) {
	yes := esprom.USBClass{Known: true, Vendor: 0x303A, Product: 0x1001}
	dl := esprom.USBClass{Known: true, Vendor: 0x303A, Product: 0x0002}
	ard := esprom.USBClass{Known: true, Vendor: 0x2341, Product: 0x0043}
	unk := esprom.USBClass{Known: false}
	for _, tc := range []struct {
		u    esprom.USBClass
		want bool
	}{{yes, true}, {dl, true}, {ard, false}, {unk, false}} {
		if got := tc.u.IsEspressif(); got != tc.want {
			t.Errorf("IsEspressif(%#04x:%#04x known=%v) = %v, want %v",
				tc.u.Vendor, tc.u.Product, tc.u.Known, got, tc.want)
		}
	}
	if dl.IsOrbDevice() {
		t.Error("download mode must not read as a flashed orb")
	}
}
func stampedBundle(t *testing.T, version string) *firmware.Bundle {
	t.Helper()
	desc := make([]byte, 300)
	desc[0], desc[1], desc[2], desc[3] = 0x32, 0x54, 0xCD, 0xAB
	copy(desc[16:48], version)
	copy(desc[48:80], "gorb-os")
	copy(desc[80:96], "01:58:41")
	copy(desc[96:112], "Oct 14 2026")
	copy(desc[112:144], "v5.5")
	head := make([]byte, 24)
	head[0], head[1], head[2], head[3] = 0xE9, 0x01, 0x02, 0x22
	head[12], head[13] = 0x09, 0x00
	app := append(append(head, desc...), strings.Repeat("x", 4096)...)
	table := make([]byte, 64)
	table[0], table[1], table[2], table[3] = 0xAA, 0x50, 0x01, 0x02
	manifest := []byte(`{"flash_settings":{"flash_mode":"dio","flash_size":"16MB"},` +
		`"flash_files":{"0x0":"bootloader.bin","0x8000":"partition-table.bin",` +
		`"0x20000":"gorb-os.bin"}}`)
	files := map[string][]byte{
		"bootloader.bin":      head,
		"partition-table.bin": table,
		"gorb-os.bin":         app,
	}
	blob, err := firmware.WriteBundle(manifest, func(rel string) ([]byte, error) {
		data, ok := files[rel]
		if !ok {
			return nil, fmt.Errorf("no such image %q", rel)
		}
		return data, nil
	})
	if err != nil {
		t.Fatalf("WriteBundle: %v", err)
	}
	b, err := firmware.Load(blob, "stamped.zip")
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	return b
}
