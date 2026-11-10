package main

import (
	"context"
	"encoding/json"
	"io"
	"io/fs"
	"net/http"
	"net/http/httptest"
	"os"
	"path"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// This file holds the ladder's shape rather than its steps: the waits a provisioning run owes a
// rebooting dial, and the sentences a seeding run may not close on. installflow.go reads its own
// text because the bench's worst failures of 2026-10-29 were shape failures — code that worked,
// wired to the wrong patience, and reporting a healthy board as a dead one.

// installFn cuts one function out of installflow.go for the text gates.
func installFn(t *testing.T, src, decl string) string {
	t.Helper()
	at := strings.Index(src, decl)
	if at < 0 {
		t.Fatalf("%s is gone from installflow.go", decl)
	}
	fn := src[at:]
	if n := strings.Index(fn, "\n}\n"); n > 0 {
		fn = fn[:n]
	}
	return fn
}

// TestTheCausedRebootsWaitOnTheCycle pins the patience of the waits that follow a reboot the hub
// caused. A dial behind its boot loop answers HTTP for a beat while it is dying, so the first
// reply a probe gets is the previous instance's corpse; waitOrbCycle refuses any answer until it
// has watched the board go quiet (firmware's own claim, 2026-10-29). parkGlass broke this rule the
// day the door shipped and refused a seed its own park had just landed (bench, Bronze,
// 2026-10-29), so the rule is a gate now, not a comment.
func TestTheCausedRebootsWaitOnTheCycle(t *testing.T) {
	raw, err := os.ReadFile("installflow.go")
	if err != nil {
		t.Fatalf("installflow.go: %v", err)
	}
	src := string(raw)
	for _, want := range []struct{ what, needle string }{
		{"the theme step's post-store wait", "s.waitOrbCycle(context.Background(), ip, installRebootWait, beats...)"},
		{"the park's post-reboot wait", "s.waitOrbCycle(ctx, ip, installRebootWait,"},
		{"the silence the cycle demands", "var cycleQuietGrace = 20 * time.Second"},
		{"the budget behind every caused reboot", "installRebootWait = 3 * time.Minute"},
	} {
		if !strings.Contains(src, want.needle) {
			t.Errorf("%s lost its patience: installflow.go no longer says %q", want.what, want.needle)
		}
	}
	if park := installFn(t, src, "func (s *Server) parkGlass"); strings.Contains(park, "s.waitOrbBack(") {
		t.Error("parkGlass waits with the bare probe again: a door that answers while the board reboots is a false refuse waiting to happen")
	}
	if back := installFn(t, src, "func (s *Server) installWaitBack"); !strings.Contains(back, "s.waitOrbCycle(") {
		t.Error("installWaitBack no longer routes through the cycle")
	}
}

// The refusal a seeding arm writes must be a lever, not a diagnosis. The owner's bench report of
// 2026-10-29 read "the dial is not showing it — open the orb to retry" on an orb whose only
// missing step was a knob flick, and a sentence an operator cannot act on reads as the hub failing
// rather than as the orb waiting.
func TestTheSeedingRefusalNamesTheLever(t *testing.T) {
	s, _ := deviceHub(t, goodApps, goodSD)
	note := ""
	err := s.verifySeededGlass(context.Background(), "127.0.0.1",
		func(st installStep) { note = st.Note })
	if err == nil {
		t.Fatal("a clock the dial is not showing verified green")
	}
	// "flick the knob" is the whole lever: the page has no Seed press any more, and a
	// refusal naming one sent the owner looking for a button that is not there.
	for _, want := range []string{"flick the knob"} {
		if !strings.Contains(note, want) {
			t.Errorf("the refusal never named %q, so the operator has nothing to do: %s", want, note)
		}
	}
	for _, ban := range []string{"open the orb to retry", "try again", "Seed"} {
		if strings.Contains(note, ban) {
			t.Errorf("the refusal is back to %q, which is an alarm and not an instruction: %s", ban, note)
		}
	}
	if !strings.Contains(err.Error(), "flick the knob") {
		t.Errorf("the stream offered the lever and the result withheld it: %s", err)
	}
	if !isSeedFailure(err) {
		t.Error("the refusal is not a seed failure, so the run would clear its slate and leave the board half set up")
	}
}

// The tolerated branch — the owner's own shelf, or a dial behind the door's floor — may report the
// glass as it found it and may not promise anything about it. An operator reads this sentence as a
// green run, so it states what the hub read and stops there.
func TestTheToleratedSkipDescribesAndPromisesNothing(t *testing.T) {
	got := seedNote(seedFacts{OnGlass: false})
	if !strings.Contains(got, "only the knob") {
		t.Errorf("the tolerated skip hides the one lever: %s", got)
	}
	for _, ban := range []string{"no card", "retry", "provision again"} {
		if strings.Contains(got, ban) {
			t.Errorf("the tolerated skip claims %q it never read: %s", ban, got)
		}
	}
}

// installTheme stands down the moment the card carries themes: they are the owner's shelf, and
// overwriting an owner's face with the library's first file is not a seed. It must stand down
// without touching the upload door, because that door reboots the board and a skipped step has no
// business spending a boot.
func TestThemeStepStandsDownWithoutDialingTheDoor(t *testing.T) {
	var paths []string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		paths = append(paths, r.URL.Path)
		w.Header().Set("Content-Type", "application/json")
		if r.URL.Path == "/api/v1/sd" {
			w.Write([]byte(`{"mounted":true,"card_count":3,"active":"Aviator","themes":[{"name":"Aviator"}]}`))
			return
		}
		w.Write([]byte(`{}`))
	}))
	t.Cleanup(srv.Close)
	s := newHub()
	s.device = orbnet.NewClient(deviceTimeout)
	s.device.DevicePort = portOf(srv.URL)
	s.ota = orbnet.NewClient(otaTimeout)
	s.ota.DevicePort = portOf(srv.URL)

	step, note := "", ""
	if err := s.installTheme(context.Background(), "127.0.0.1", "clock",
		func(st installStep) { step, note = st.Action, st.Note }); err != nil {
		t.Fatalf("the theme step refused a card holding its owner's themes: %v", err)
	}
	if step != "skipped" || !strings.Contains(note, "the card already carries themes") {
		t.Errorf("the theme step answered %s / %q instead of standing down", step, note)
	}
	for _, p := range paths {
		if strings.Contains(p, "/api/v1/apps/theme") {
			t.Errorf("a skipped theme step dialed the upload door (%s) and rebooted a board it had no reason to touch", p)
		}
	}
}

// The theme step's gate read is a card walk, and a card walk is slow on a board
// that has just rebuilt an app: /api/v1/sd answered in 16 s idle on a degraded
// orb (bench, 2026-10-31), and the bare 8 s control dial refused a clock update
// that had already landed. A walk past 8 s must still be heard.
func TestThemeStepRidesOutASlowCardWalk(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if r.URL.Path == "/api/v1/sd" {
			time.Sleep(deviceTimeout + time.Second)
			w.Write([]byte(`{"mounted":true,"card_count":7,"active":"Skeleton","themes":[{"name":"Skeleton"}]}`))
			return
		}
		w.Write([]byte(`{}`))
	}))
	t.Cleanup(srv.Close)
	s := newHub()
	s.device = orbnet.NewClient(deviceTimeout)
	s.device.DevicePort = portOf(srv.URL)

	step := ""
	if err := s.installTheme(context.Background(), "127.0.0.1", "clock",
		func(st installStep) { step = st.Action }); err != nil {
		t.Fatalf("a slow card walk refused the run: %v", err)
	}
	if step != "skipped" {
		t.Errorf("the theme step answered %q; a card with themes read slowly is still a card with themes", step)
	}
}

// An unread theme list is not a failed run: the apps the run came for have
// landed, and the seed's gate could not decide, so it writes nothing and says so.
func TestThemeStepUnreadCardIsNotARefusal(t *testing.T) {
	var paths []string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		paths = append(paths, r.URL.Path)
		if r.URL.Path == "/api/v1/sd" {
			http.Error(w, `{"error":"card walk failed"}`, http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{}`))
	}))
	t.Cleanup(srv.Close)
	s := newHub()
	s.device = orbnet.NewClient(deviceTimeout)
	s.device.DevicePort = portOf(srv.URL)
	s.ota = orbnet.NewClient(otaTimeout)
	s.ota.DevicePort = portOf(srv.URL)

	step := ""
	if err := s.installTheme(context.Background(), "127.0.0.1", "clock",
		func(st installStep) { step = st.Action }); err != nil {
		t.Fatalf("an unread theme list refused the run: %v", err)
	}
	if step != "unchecked" {
		t.Errorf("the theme step answered %q; wanted unchecked", step)
	}
	for _, p := range paths {
		if p != "/api/v1/sd" {
			t.Errorf("an unread card still dialed %s: nothing may be written to a shelf the hub could not read", p)
		}
	}
}

// A card busy with an upload or a store answers its list without walking it, so
// card_count reads 0 for a card that may hold the owner's shelf. The seed must
// read that as unread and write nothing.
func TestThemeStepDoesNotSeedABusyCard(t *testing.T) {
	for _, busy := range []string{`"uploading":true`, `"job_busy":true`} {
		var paths []string
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			paths = append(paths, r.URL.Path)
			w.Header().Set("Content-Type", "application/json")
			if r.URL.Path == "/api/v1/sd" {
				w.Write([]byte(`{"mounted":true,"card_count":0,` + busy + `,"themes":[]}`))
				return
			}
			w.Write([]byte(`{}`))
		}))
		s := newHub()
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		s.ota = orbnet.NewClient(otaTimeout)
		s.ota.DevicePort = portOf(srv.URL)
		step := ""
		if err := s.installTheme(context.Background(), "127.0.0.1", "clock",
			func(st installStep) { step = st.Action }); err != nil {
			t.Fatalf("%s: a busy card refused the run: %v", busy, err)
		}
		if step != "unchecked" {
			t.Errorf("%s: the theme step answered %q; a busy card is unread, not empty", busy, step)
		}
		for _, p := range paths {
			if p != "/api/v1/sd" {
				t.Errorf("%s: a busy card was written to (%s)", busy, p)
			}
		}
		srv.Close()
	}
}

// The seed is per app (owner, 2026-11-04: flights installed on Gold beside the clock's
// Skeleton and got no theme). A card holding only clock themes does not stand the flight
// tracker's step down; it goes on to the library for a flights theme. One of the app's own
// does stand it down.
func TestThemeStepIsPerApp(t *testing.T) {
	cards := map[string]string{
		"clock only":  `{"mounted":true,"card_count":1,"themes":[{"name":"Skeleton","app":"clock"}]}`,
		"flights too": `{"mounted":true,"card_count":2,"themes":[{"name":"Skeleton","app":"clock"},{"name":"Modern","app":"flights"}]}`,
	}
	for label, body := range cards {
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			if r.URL.Path == "/api/v1/sd" {
				w.Write([]byte(body))
				return
			}
			w.Write([]byte(`{}`))
		}))
		s := newHub()
		s.lib = &Library{ThemesDir: t.TempDir()} // an empty shelf: seeding must say so
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		step := ""
		err := s.installTheme(context.Background(), "127.0.0.1", "flights", func(st installStep) { step = st.Action })
		srv.Close()
		if label == "clock only" && (err == nil || !strings.Contains(err.Error(), "no theme for flights")) {
			t.Errorf("%s: the flights seed stood down (step %q, err %v); the clock's themes are not flights'", label, step, err)
		}
		if label == "flights too" && (err != nil || step != "skipped") {
			t.Errorf("%s: a card with a flights theme still went on to seed (step %q, err %v)", label, step, err)
		}
	}
}

// The apps name their OS floor (app.json os_min), carried in their editor region's stamp,
// and the install run reads it from there: the flight tracker and the weather need v1.2.11's
// orb_knob_dial (plan §51), and the clock v1.2.9 (its themes' gear plans play on that
// release's scheduler). The weather now needs v1.2.17: its temperature fonts are sparse (the
// digits, the minus and the degree sign alone), which no earlier reader accepts. All three now
// need v1.2.19: §57's FORMAT 4 container carries the settings schema, which no earlier
// reader accepts, and the OS renders their settings from it. The clock needs v1.2.20: it plays
// its ticks and chimes at the owner's two levels through stage_snd_play_v (§59). The weather
// needs v1.2.30: it reads the orb's places (orb_place, radar-plan §2.1), merged in that release.
// Weather and flights need v1.2.37: their themes' fonts carry the accent marks (radar-plan
// §6.22), 129 glyphs, which an earlier reader refuses.
func TestShelfAppsCarryTheirOSFloor(t *testing.T) {
	for slug, want := range map[string]string{"flights": "v1.2.38", "weather": "v1.2.38", "clock": "v1.2.20"} {
		body, err := os.ReadFile(filepath.Join("apps", slug+".orbapp"))
		if err != nil {
			t.Fatalf("%s: %v", slug, err)
		}
		if got := appOSMin(body); got != want {
			t.Errorf("%s.orbapp names os_min %q; want %q", slug, got, want)
		}
	}
}

// TestTheRunSaysItsPlanFirst pins the panel's step count (owner, 2026-11-06: "step x of n",
// not a bar at 100% saying "waiting"). The run's first line is the plan, before any dial,
// and every line after it names the stage it belongs to, so a skip lands on its own row.
func TestTheRunSaysItsPlanFirst(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/api/v1/status":
			w.Write([]byte(`{"state":"connected","fw":"` + installMinFW + `"}`))
		case "/api/v1/apps":
			w.Write([]byte(`{"mounted":true,"apps":[{"stem":"clock","ok":true}],"installed":[{"slug":"clock"}]}`))
		case "/api/v1/sd":
			w.Write([]byte(`{"mounted":true,"card_count":1}`))
		default:
			w.Write([]byte(`{}`))
		}
	}))
	defer srv.Close()
	s := newHub()
	s.lib = &Library{}
	s.device = orbnet.NewClient(deviceTimeout)
	s.device.DevicePort = portOf(srv.URL)

	var steps []installStep
	if err := s.installRun(context.Background(), "127.0.0.1", "clock", false, true,
		func(st installStep) { steps = append(steps, st) }); err != nil {
		t.Fatalf("the run refused: %v", err)
	}
	if len(steps) == 0 || steps[0].Action != "plan" {
		t.Fatalf("the run did not open with its plan: %v", steps)
	}
	var ids []string
	for _, p := range steps[0].Plan {
		ids = append(ids, p.ID)
	}
	// the card carries themes, so no seed can happen and none is listed (owner, 2026-11-09:
	// "not needed" under every theme)
	want := "check install:clock restart"
	if got := strings.Join(ids, " "); got != want {
		t.Errorf("a card install of the clock plans %q; want %q", got, want)
	}
	// a card with no theme of the clock's lists its seed, in the firmware's own steps, and on
	// firmware whose store only chooses it lists them BEFORE the install, where the run walks
	// them so its one restart boots the app already wearing its face (owner, 2026-11-09)
	planIDs := func(chooses bool, seeded map[string]bool) string {
		var out []string
		for _, p := range installPlan(&Library{}, []string{"clock"}, true, chooses, seeded) {
			out = append(out, p.ID)
		}
		return strings.Join(out, " ")
	}
	for _, c := range []struct {
		chooses bool
		seeded  map[string]bool
		want    string
	}{
		{false, map[string]bool{"weather": true}, "check install:clock restart theme-send:clock theme-check:clock theme-room:clock theme-store:clock theme-restart:clock"},
		{true, map[string]bool{"weather": true}, "check theme-send:clock theme-check:clock install:clock restart"},
		{true, nil, "check theme-send:clock theme-check:clock install:clock restart"},
		{true, map[string]bool{"clock": true}, "check install:clock restart"},
	} {
		if got := planIDs(c.chooses, c.seeded); got != c.want {
			t.Errorf("chooses %v, card themes %v: plans %q; want %q", c.chooses, c.seeded, got, c.want)
		}
	}
	phase := map[string]string{}
	for _, st := range steps[1:] {
		if st.Phase == "" {
			t.Errorf("%s/%s names no stage", st.Step, st.Action)
		}
		phase[st.Step+"/"+st.Action] = st.Phase
	}
	if phase["clock/skipped"] != "install:clock" {
		t.Errorf("the skips landed on the wrong rows: %v", phase)
	}
}

// TestAnInstallThatRodeTheRebootSaysSo pins the restart row's timing (owner, 2026-11-06:
// "restarting" lit up long after the orb was back). The dial reboots 750 ms after the slot
// write, so the ledger poll usually goes silent and finds the row on the board that came
// back; that run must say restarted, and one that found the row with no silence must not.
func TestAnInstallThatRodeTheRebootSaysSo(t *testing.T) {
	old := installPoll
	installPoll = 10 * time.Millisecond
	defer func() { installPoll = old }()
	for _, rode := range []bool{true, false} {
		installed, dark := false, 0
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			switch r.URL.Path {
			case "/api/v1/apps":
				if installed && rode && dark < 2 {
					dark++
					w.WriteHeader(http.StatusServiceUnavailable)
					return
				}
				if installed {
					w.Write([]byte(`{"mounted":true,"apps":[{"stem":"sentinel","ok":true}],"installed":[{"slug":"sentinel"}]}`))
					return
				}
				w.Write([]byte(`{"mounted":true,"apps":[{"stem":"sentinel","ok":true}],"installed":[]}`))
			case "/api/v1/apps/install":
				installed = true
				w.Write([]byte(`{"ok":true,"queued":true}`))
			default:
				w.Write([]byte(`{}`))
			}
		}))
		s := newHub()
		s.lib = &Library{}
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		said := false
		err := s.installOneApp(context.Background(), "127.0.0.1", "sentinel", nil, false, false, func(st installStep) {
			if st.Action == "restarted" {
				said = true
			}
		})
		srv.Close()
		if err != nil {
			t.Fatalf("rode=%v: the install refused: %v", rode, err)
		}
		if said != rode {
			t.Errorf("rode=%v: restarted said %v", rode, said)
		}
	}
}

// TestAnInstallWaitsOutABusyWorker: v1.6.0 runs theme fills and card reconciles on the
// orb's one job worker, so an install can meet "a card job is already running" (owner,
// 2026-11-09, mid Install everything). The step waits, says so, and installs; any other
// 409 still refuses at once.
func TestAnInstallWaitsOutABusyWorker(t *testing.T) {
	old := installPoll
	installPoll = 10 * time.Millisecond
	defer func() { installPoll = old }()
	for _, body := range []string{`{"error":"a card job is already running"}`, `{"error":"something else"}`} {
		busy, installed := 3, false
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			switch r.URL.Path {
			case "/api/v1/apps":
				if installed {
					w.Write([]byte(`{"mounted":true,"apps":[{"stem":"sentinel","ok":true}],"installed":[{"slug":"sentinel"}]}`))
					return
				}
				w.Write([]byte(`{"mounted":true,"apps":[{"stem":"sentinel","ok":true}],"installed":[]}`))
			case "/api/v1/apps/install":
				if busy > 0 {
					busy--
					w.WriteHeader(http.StatusConflict)
					w.Write([]byte(body))
					return
				}
				installed = true
				w.Write([]byte(`{"state":"installing","stem":"sentinel"}`))
			default:
				w.Write([]byte(`{}`))
			}
		}))
		s := newHub()
		s.lib = &Library{}
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		waited := 0
		err := s.installOneApp(context.Background(), "127.0.0.1", "sentinel", nil, false, false, func(st installStep) {
			if st.Action == "waiting" {
				waited++
			}
		})
		srv.Close()
		if strings.Contains(body, "card job") {
			if err != nil || !installed || waited != 1 {
				t.Errorf("busy worker: want one wait then the install, got err=%v installed=%v waits=%d", err, installed, waited)
			}
		} else if err == nil || installed {
			t.Errorf("other 409: want a refusal, got err=%v installed=%v", err, installed)
		}
	}
}

// TestAnOlderFormatCardCopyIsReplaced: an orb that took v1.2.32's firmware from v1.2.16 holds
// every app's card copy in FORMAT 3, which the dial refuses by number. Install sends the
// library's bytes over such a copy instead of refusing (owner, 2026-11-08); a card copy
// refused for any other reason still stops the run, and nothing is uploaded over it.
func TestAnOlderFormatCardCopyIsReplaced(t *testing.T) {
	old := installPoll
	installPoll = 10 * time.Millisecond
	defer func() { installPoll = old }()
	for _, why := range []string{"app format 3; this build reads format 4", "header CRC mismatch"} {
		uploaded, installed := false, false
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			switch r.URL.Path {
			case "/api/v1/apps":
				if installed {
					w.Write([]byte(`{"mounted":true,"apps":[{"stem":"sentinel","ok":true}],"installed":[{"slug":"sentinel"}]}`))
					return
				}
				if uploaded {
					w.Write([]byte(`{"mounted":true,"apps":[{"stem":"sentinel","ok":true}],"installed":[]}`))
					return
				}
				w.Write([]byte(`{"mounted":true,"apps":[{"stem":"sentinel","ok":false,"why":"` + why + `"}],"installed":[]}`))
			case "/api/v1/sd/app":
				io.Copy(io.Discard, r.Body)
				uploaded = true
				w.Write([]byte(`{"state":"stored"}`))
			case "/api/v1/apps/install":
				installed = true
				w.Write([]byte(`{"ok":true,"queued":true}`))
			default:
				w.Write([]byte(`{}`))
			}
		}))
		s := newHub()
		s.lib = &Library{}
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		s.ota = orbnet.NewClient(otaTimeout)
		s.ota.DevicePort = portOf(srv.URL)
		err := s.installOneApp(context.Background(), "127.0.0.1", "sentinel", []byte("the library's bytes"), false, false, func(installStep) {})
		srv.Close()
		if strings.HasPrefix(why, "app format") {
			if err != nil || !uploaded || !installed {
				t.Errorf("%q: want the copy replaced and installed, got err=%v uploaded=%v installed=%v", why, err, uploaded, installed)
			}
		} else if err == nil || uploaded {
			t.Errorf("%q: want a refusal and no upload, got err=%v uploaded=%v", why, err, uploaded)
		}
	}
}

// TestStoreStagesGetTheirOwnRows pins the theme's three on-orb stages (owner, 2026-11-06:
// copied to the card, moved to flash, likely reorganised to fit). Each stage the orb reports
// becomes its own action, byte counts thin to one line per ~5%, and the run's phase stamp
// puts every one on its themeSteps row.
func TestStoreStagesGetTheirOwnRows(t *testing.T) {
	var got []installStep
	p := &runPhases{cur: "theme-send:clock", theme: "clock"}
	on := storeStages(p.stamp(func(st installStep) { got = append(got, st) }), "theme")
	on("checking", 0, 0)
	on("parking", 0, 0)
	for d := uint32(0); d <= 100; d++ {
		on("making room", d*1024, 100*1024)
	}
	on("storing", 0, 2048*1024)
	on("storing", 2048*1024, 2048*1024)
	on("rebooting", 0, 0)
	phases := map[string]int{}
	for _, st := range got {
		phases[st.Phase]++
	}
	if phases["theme-check:clock"] == 0 || phases["theme-room:clock"] == 0 || phases["theme-store:clock"] != 2 {
		t.Fatalf("the stages landed on %v", phases)
	}
	if n := phases["theme-room:clock"]; n > 22 {
		t.Errorf("a 100-step move sent %d lines; one per ~5%% is the budget", n)
	}
	if last := got[len(got)-1]; last.Action != "moving" || last.Note != "2048 of 2048 KB" {
		t.Errorf("the move's last line is %+v; want its full count", last)
	}
}

// TestABatchRestartsTheOrbOnce: Update everything restarted the orb after every app
// (owner, 2026-11-09). On firmware that lists restart_owed, a run of several apps installs
// each with ?defer=1, waits for its stem to be owed rather than for a row the old release
// already stands in, and asks for one /api/v1/reboot at the end. Firmware without the key
// installs one app per restart, as before, and is never asked to reboot by the hub.
func TestABatchRestartsTheOrbOnce(t *testing.T) {
	op, ow, og, oh := installPoll, installRebootWait, cycleQuietGrace, installHard
	installPoll, installRebootWait, cycleQuietGrace, installHard = 10*time.Millisecond, 200*time.Millisecond, 50*time.Millisecond, 2*time.Second
	defer func() { installPoll, installRebootWait, cycleQuietGrace, installHard = op, ow, og, oh }()
	lib := t.TempDir()
	for _, slug := range []string{"clock", "weather"} {
		b, err := fs.ReadFile(shippedLib, path.Join(libAppsSubdir, slug+".orbapp"))
		if err != nil {
			t.Skipf("no shipped %s: %v", slug, err)
		}
		os.WriteFile(filepath.Join(lib, slug+".orbapp"), b, 0o644)
	}
	for _, defers := range []bool{true, false} {
		var mu sync.Mutex
		var installs []string
		owed := []string{}
		reboots := 0
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			mu.Lock()
			defer mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			switch r.URL.Path {
			case "/api/v1/status":
				w.Write([]byte(`{"state":"connected","ip":"127.0.0.1","fw":"v1.6.1+aaaaaaa"}`))
			case "/api/v1/sd/app":
				io.Copy(io.Discard, r.Body)
				w.Write([]byte(`{"state":"stored"}`))
			case "/api/v1/apps/install":
				id := r.URL.Query().Get("id")
				installs = append(installs, id+map[bool]string{true: "+defer", false: ""}[r.URL.Query().Get("defer") == "1"])
				if defers && r.URL.Query().Get("defer") == "1" {
					owed = append(owed, id)
				}
				w.Write([]byte(`{"state":"installing"}`))
			case "/api/v1/reboot":
				reboots++
				owed = []string{}
				w.Write([]byte(`{"state":"rebooting"}`))
			case "/api/v1/apps":
				tail := ""
				if defers {
					b, _ := json.Marshal(owed)
					tail = `,"restart_owed":` + string(b)
				}
				w.Write([]byte(`{"mounted":true,"apps":[{"stem":"clock","ok":true},{"stem":"weather","ok":true}],` +
					`"installed":[{"slug":"clock","version":"v1.0.0"},{"slug":"weather","version":"v1.0.0"}]` + tail + `}`))
			default:
				w.Write([]byte(`{}`))
			}
		}))
		s := newHub()
		s.lib = &Library{AppsDir: lib}
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		s.ota = orbnet.NewClient(otaTimeout)
		s.ota.DevicePort = portOf(srv.URL)
		s.installRun(context.Background(), "127.0.0.1", "clock,weather", true, false, func(installStep) {})
		srv.Close()
		mu.Lock()
		if defers {
			if want := []string{"clock+defer", "weather+defer"}; !slices.Equal(installs, want) {
				t.Errorf("deferring firmware: installs %v, want %v", installs, want)
			}
			if reboots != 1 {
				t.Errorf("deferring firmware: the hub asked for %d reboots, want exactly one", reboots)
			}
		} else {
			if want := []string{"clock", "weather"}; !slices.Equal(installs, want) {
				t.Errorf("older firmware: installs %v, want %v (no defer it cannot read)", installs, want)
			}
			if reboots != 0 {
				t.Errorf("older firmware: the hub asked for %d reboots; each install reboots on its own", reboots)
			}
		}
		mu.Unlock()
	}
}

// TestAStoreOnlyRebootsOlderFirmware: from v1.6.0 a theme store chooses and does not
// reboot, and a hub that still waited for the restart showed "Restarting the orb" for the
// whole quiet grace while nothing restarted (owner, Gold, 2026-11-09). Older firmware, and
// a status that will not parse, keep the restart wait.
func TestAStoreOnlyRebootsOlderFirmware(t *testing.T) {
	for fw, want := range map[string]bool{"v1.6.0+e6dc820": false, "v1.6.1": false, "v1.2.38+adee2d7": true, "dev": true} {
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(`{"state":"connected","fw":"` + fw + `"}`))
		}))
		s := newHub()
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		if got := s.storeReboots("127.0.0.1"); got != want {
			t.Errorf("firmware %s: storeReboots %v, want %v", fw, got, want)
		}
		srv.Close()
	}
}

// TestARefusedCardWriteIsSentAgain: Gold refused radar.orbapp once with "the card refused
// the write" under a live dial and took it on every later try (owner, 2026-11-09). The hub
// sends a refused write again, at most cardWriteTries times, and says so on the step; any
// other refusal fails at once.
func TestARefusedCardWriteIsSentAgain(t *testing.T) {
	op, oh, oz := installPoll, installHard, cardWriteRetryPause
	installPoll, installHard, cardWriteRetryPause = 10*time.Millisecond, 2*time.Second, time.Millisecond
	defer func() { installPoll, installHard, cardWriteRetryPause = op, oh, oz }()
	blob := cloneFixture(t, t.TempDir(), "sentinel")
	body, err := os.ReadFile(blob)
	if err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		refusals, wantUploads int
		reply                 string
		ok                    bool
	}{
		{1, 2, `{"error":"the card refused the write (I/O error)","dma_free":5120}`, true},
		{9, cardWriteTries, `{"error":"the card refused the write (I/O error)"}`, false},
		{9, 1, `{"error":"rejected","detail":"app format"}`, false},
	} {
		var mu sync.Mutex
		uploads, installed := 0, false
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			mu.Lock()
			defer mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			switch r.URL.Path {
			case "/api/v1/sd/app":
				io.Copy(io.Discard, r.Body)
				uploads++
				if uploads <= c.refusals {
					w.WriteHeader(500)
					w.Write([]byte(c.reply))
					return
				}
				w.Write([]byte(`{"state":"stored"}`))
			case "/api/v1/apps/install":
				installed = true
				w.Write([]byte(`{"ok":true,"queued":true}`))
			case "/api/v1/apps":
				if installed {
					w.Write([]byte(`{"apps":[],"installed":[{"slug":"sentinel"}]}`))
					return
				}
				w.Write([]byte(`{"apps":[],"installed":[]}`))
			default:
				w.Write([]byte(`{}`))
			}
		}))
		s := newHub()
		s.lib = &Library{AppsDir: filepath.Dir(blob)}
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		s.ota = orbnet.NewClient(otaTimeout)
		s.ota.DevicePort = portOf(srv.URL)
		var notes []string
		err := s.installOneApp(context.Background(), "127.0.0.1", "sentinel", body, false, false, func(st installStep) {
			if strings.Contains(st.Note, "sending it again") {
				notes = append(notes, st.Note)
			}
		})
		srv.Close()
		if (err == nil) != c.ok {
			t.Errorf("%d refusals of %q: err %v, want ok=%v", c.refusals, c.reply, err, c.ok)
		}
		if uploads != c.wantUploads {
			t.Errorf("%d refusals of %q: %d uploads, want %d", c.refusals, c.reply, uploads, c.wantUploads)
		}
		if len(notes) != c.wantUploads-1 {
			t.Errorf("%d refusals of %q: the steps said it was sent again %d times, want %d: %v",
				c.refusals, c.reply, len(notes), c.wantUploads-1, notes)
		}
	}
}

// TestAnUpgradeKeepsTheWornTheme: an upgrade replaces a card copy and the app keeps the
// theme it wears (owner, 2026-11-09). The push asks for ?keep=1, a "filed" answer ends it
// with no store followed, and an orb before v1.6.4, which would choose the theme, is not
// sent it at all.
func TestAnUpgradeKeepsTheWornTheme(t *testing.T) {
	blob, err := os.ReadFile(filepath.Join("themes", "skeleton.orbtheme"))
	if err != nil {
		t.Skipf("no shelf theme: %v", err)
	}
	c, err := orbnet.ContainerName(blob)
	if err != nil {
		t.Fatal(err)
	}
	for _, fw := range []string{"v1.6.4+0d9a87b", "v1.6.3+a0c1765"} {
		var mu sync.Mutex
		var uploads []string
		listed := 0
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			mu.Lock()
			defer mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			switch r.URL.Path {
			case "/api/v1/status":
				w.Write([]byte(`{"state":"connected","fw":"` + fw + `"}`))
			case "/api/v1/sd/theme":
				io.Copy(io.Discard, r.Body)
				uploads = append(uploads, r.URL.RawQuery)
				w.Write([]byte(`{"state":"filed","id":"` + c.ID + `","reboot":"none"}`))
			case "/api/v1/sd":
				listed++
				w.Write([]byte(`{"mounted":true,"themes":[]}`))
			default:
				w.Write([]byte(`{}`))
			}
		}))
		s := newHub()
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		s.ota = orbnet.NewClient(otaTimeout)
		s.ota.DevicePort = portOf(srv.URL)
		var last installStep
		ok := s.streamThemeStore(context.Background(), "127.0.0.1", blob, "Skeleton", c, true, true,
			func(st installStep) { last = st })
		srv.Close()
		mu.Lock()
		if !ok {
			t.Errorf("%s: the upgrade failed: %v", fw, last)
		}
		if strings.HasPrefix(fw, "v1.6.4") {
			if len(uploads) != 1 || !strings.Contains(uploads[0], "keep=1") {
				t.Errorf("%s: uploads %v, want one with keep=1", fw, uploads)
			}
			if last.Action != "done" || listed != 0 {
				t.Errorf("%s: ended %s/%q after %d store reads; a filed upgrade has no store to follow", fw, last.Action, last.Note, listed)
			}
		} else if len(uploads) != 0 || last.Action != "skipped" {
			t.Errorf("%s: uploads %v, last %s; an orb that would choose it is not sent it", fw, uploads, last.Action)
		}
		mu.Unlock()
	}
}

// TestASeedGoesBeforeTheRestart pins the run's order on both firmware postures (owner,
// 2026-11-09: "We install the app, we reboot, then we install the theme. Why don't we just
// install the app, we install the theme and then reboot as the last thing?"). A store that
// chooses without rebooting (v1.6.0+) is seeded BEFORE the app is installed, so the one
// restart the run ends with boots the app already wearing its face; a store that reboots the
// orb on the store itself keeps the old order, because seeding ahead of it would cost a second
// restart. The third case is the refusal this reordering makes possible — a seed that cannot
// dress the app standing behind it — and pins that it costs the run its refusal and not its app.
// The fake is one install onto a card holding no themes of its own, and it records every device
// call, so the order is the board's own answer and not a reading of code.
func TestASeedGoesBeforeTheRestart(t *testing.T) {
	op, ow, og, oh := installPoll, installRebootWait, cycleQuietGrace, installHard
	tp, tb := themeStorePoll, themeStoreBudget
	installPoll, installRebootWait, cycleQuietGrace, installHard = 10*time.Millisecond, 300*time.Millisecond, 50*time.Millisecond, 2*time.Second
	themeStorePoll, themeStoreBudget = 10*time.Millisecond, 2*time.Second
	defer func() {
		installPoll, installRebootWait, cycleQuietGrace, installHard = op, ow, og, oh
		themeStorePoll, themeStoreBudget = tp, tb
	}()
	for _, c := range []struct {
		fw      string
		chooses bool
		slug    string
	}{
		{"v1.6.4+aaaaaaa", true, "weather"},
		{"v1.5.9+aaaaaaa", false, "weather"},
		// the theme only dresses the app the owner asked for, and on choosing firmware the seed
		// now sits in front of it — so a clock the shelf has no face for is refused there, and
		// the install it stands in front of still goes in (the same refusal the old ordering
		// took after the apps had landed)
		{"v1.6.4+aaaaaaa", true, "clock"},
	} {
		dir := t.TempDir()
		for _, src := range []string{
			path.Join(libAppsSubdir, c.slug+".orbapp"),
			path.Join(libThemesSubdir, "weather-scope.orbtheme"),
		} {
			b, err := fs.ReadFile(shippedLib, src)
			if err != nil {
				t.Skipf("no shipped %s: %v", src, err)
			}
			dest := filepath.Base(src)
			if err := os.WriteFile(filepath.Join(dir, dest), b, 0o644); err != nil {
				t.Fatal(err)
			}
		}
		var mu sync.Mutex
		var calls []string
		themeID, landed := "", false
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			mu.Lock()
			defer mu.Unlock()
			calls = append(calls, r.Method+" "+r.URL.Path)
			w.Header().Set("Content-Type", "application/json")
			switch {
			case r.URL.Path == "/api/v1/status":
				w.Write([]byte(`{"state":"connected","ip":"127.0.0.1","fw":"` + c.fw + `"}`))
			case r.URL.Path == "/api/v1/sd" && r.Method == http.MethodGet:
				// an empty card until the theme lands, then its row wearing the witness the
				// firmware of that release reports a landed store by
				if themeID == "" {
					w.Write([]byte(`{"mounted":true,"card_count":0,"themes":[]}`))
					return
				}
				chosen, stored := "false", "true"
				if c.chooses {
					chosen, stored = "true", "false"
				}
				w.Write([]byte(`{"mounted":true,"card_count":1,"themes":[{"name":"Scope","app":"weather","id":"` +
					themeID + `","ok":true,"chosen":` + chosen + `,"stored":` + stored + `}]}`))
			case r.URL.Path == "/api/v1/sd/theme":
				b, err := io.ReadAll(r.Body)
				if err != nil {
					http.Error(w, `{"error":"short body"}`, http.StatusBadRequest)
					return
				}
				tc, err := orbnet.ContainerName(b)
				if err != nil {
					http.Error(w, `{"error":"the card refused the container"}`, http.StatusBadRequest)
					return
				}
				themeID = tc.ID
				w.Write([]byte(`{"state":"staged","id":"` + tc.ID + `","reboot":"none"}`))
			case r.URL.Path == "/api/v1/sd/app":
				io.Copy(io.Discard, r.Body)
				w.Write([]byte(`{"state":"stored"}`))
			case r.URL.Path == "/api/v1/apps" && r.Method == http.MethodGet:
				if !landed {
					w.Write([]byte(`{"mounted":true,"apps":[],"installed":[]}`))
					return
				}
				w.Write([]byte(`{"mounted":true,"apps":[{"stem":"` + c.slug + `","ok":true}],` +
					`"installed":[{"slug":"` + c.slug + `","version":"v1.0.0"}]}`))
			case r.URL.Path == "/api/v1/apps/install":
				landed = true
				w.Write([]byte(`{"state":"installing"}`))
			case r.URL.Path == "/api/v1/reboot":
				w.Write([]byte(`{"state":"rebooting"}`))
			default:
				w.Write([]byte(`{}`))
			}
		}))
		s := newHub()
		s.lib = &Library{AppsDir: dir, ThemesDir: dir}
		for _, cl := range []*orbnet.Client{s.device, s.ota, s.orb} {
			cl.DevicePort = portOf(srv.URL)
		}
		var steps []installStep
		err := s.installRun(context.Background(), "127.0.0.1", c.slug, false, false,
			func(st installStep) { steps = append(steps, st) })
		srv.Close()
		if c.slug == "clock" {
			// The refusal is the run's last word, and the clock is still on the card.
			if err == nil || !strings.Contains(err.Error(), "no theme for clock") {
				t.Fatalf("%s seeding a clock the shelf cannot dress: the run ended %v; want the seed's own refusal", c.fw, err)
			}
			mu.Lock()
			sent, installed := false, false
			for _, l := range calls {
				sent = sent || l == "POST /api/v1/sd/app"
				installed = installed || l == "POST /api/v1/apps/install"
			}
			mu.Unlock()
			if !sent || !installed {
				t.Fatalf("%s seeding a clock the shelf cannot dress: the seed's refusal cost the run its app: %v", c.fw, calls)
			}
			refused := ""
			for _, st := range steps {
				if st.Action == "refused" {
					refused = st.Phase
				}
			}
			if refused != "theme-send:clock" {
				t.Errorf("%s seeding a clock the shelf cannot dress: the refusal printed on %q; want the seed's own send row", c.fw, refused)
			}
			continue
		}
		if err != nil {
			t.Fatalf("%s: the run refused: %v", c.fw, err)
		}
		if len(steps) == 0 || steps[0].Action != "plan" {
			t.Fatalf("%s: the run did not open with its plan: %+v", c.fw, steps)
		}
		rows := []string{}
		for _, p := range steps[0].Plan {
			rows = append(rows, p.ID)
		}

		// Which door crossed first, in the board's own words.
		mu.Lock()
		at := func(pref string) int {
			for i, l := range calls {
				if strings.HasPrefix(l, pref) {
					return i
				}
			}
			return -1
		}
		up, ins := at("POST /api/v1/sd/theme"), at("POST /api/v1/apps/install")
		mu.Unlock()
		if up < 0 || ins < 0 {
			t.Fatalf("%s: the run sent no theme upload or no install: %v", c.fw, calls)
		}
		if first := up < ins; first != c.chooses {
			t.Errorf("%s: the theme upload went %s the app install; on this firmware the seed must go %s it",
				c.fw, map[bool]string{true: "before", false: "after"}[first],
				map[bool]string{true: "before", false: "after"}[c.chooses])
		}

		// The plan the panel drew before the first dial lists its rows where the run walks them.
		// An absent row is -1 and not 0: a plan that lost its seed rows is a failure, not a
		// row that happened to sort first.
		row := func(id string) int {
			for i, got := range rows {
				if got == id {
					return i
				}
			}
			return -1
		}
		seedRow, installRow, restartRow := row("theme-send:weather"), row("install:weather"), row("restart")
		if seedRow < 0 || installRow < 0 || restartRow < 0 {
			t.Fatalf("%s: the plan lost a row: %v", c.fw, rows)
		}
		if c.chooses {
			if !(seedRow < installRow && installRow < restartRow) {
				t.Errorf("%s: plans %v; want the seed before the install and the restart after both", c.fw, rows)
			}
		} else if !(installRow < restartRow && restartRow < seedRow) {
			t.Errorf("%s: plans %v; want the old order — install, restart, then the seed", c.fw, rows)
		}

		// Every theme line sits on one of its own theme rows, and a choosing store is never
		// asked to reboot.
		phase := map[string]string{}
		for _, st := range steps[1:] {
			if st.Step != "theme" {
				continue
			}
			phase[st.Action] = st.Phase
			if strings.HasPrefix(st.Phase, "install:") {
				t.Errorf("%s: the theme line %s stamped %s; a seed has no install row to sit on", c.fw, st.Action, st.Phase)
			}
		}
		for action, want := range map[string]string{
			"uploading": "theme-send:weather", "uploaded-theme": "theme-send:weather", "storing": "theme-check:weather",
		} {
			if phase[action] != want {
				t.Errorf("%s: the theme %s line landed on %q; want %s", c.fw, action, phase[action], want)
			}
		}
		if c.chooses {
			if phase["worn"] != "theme-check:weather" || phase["rebooting"] != "" {
				t.Errorf("%s: a choosing store reported %v; want a worn theme on its check row and no reboot", c.fw, phase)
			}
		}
		// The run still ends behind the restart that loads the app, whichever order it took.
		back := false
		for _, st := range steps[1:] {
			if st.Step == "install" && st.Action == "restarting" && st.Phase == "restart" {
				back = true
			}
		}
		if !back {
			t.Errorf("%s: the run never waited for the orb to come back with the app: %+v", c.fw, steps[max(0, len(steps)-3):])
		}
	}
}

// TestAThemeForAMissingAppSaysSo: §63's "added, its app not installed" half of a theme push.
// The theme still lands — a face filed for the day its app arrives is a good thing — but the
// run's final line says the dial cannot wear it yet and carries the app's slug in AppMissing
// so the page can offer the install. The offer needs all three: no ledger row on the orb,
// the .orbapp on the hub's shelf, a theme that names an app. A failed app list is the fourth
// way to say nothing: the push is not reported missing on one bad GET.
func TestAThemeForAMissingAppSaysSo(t *testing.T) {
	dir := t.TempDir()
	blob, err := fs.ReadFile(shippedLib, path.Join(libAppsSubdir, "clock.orbapp"))
	if err != nil {
		t.Skipf("no shipped clock.orbapp: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, "clock.orbapp"), blob, 0o644); err != nil {
		t.Fatal(err)
	}
	theme, err := os.ReadFile(filepath.Join("themes", "skeleton.orbtheme"))
	if err != nil {
		t.Skipf("no shelf theme: %v", err)
	}
	c, err := orbnet.ContainerName(theme)
	if err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		what      string
		appsDir   string
		installed string // JSON for the orb's installed ledger
		appsFail  bool
		want      string // AppMissing wanted on the done step
	}{
		{"filed for a bare orb with the app on the shelf", dir, `[]`, false, "clock"},
		{"filed for an orb already wearing the app", dir, `[{"slug":"clock","version":"v1.0.0"}]`, false, ""},
		{"filed for a bare orb with no clock on the shelf", t.TempDir(), `[]`, false, ""},
		{"filed for a bare orb whose app list would not read", dir, `[]`, true, ""},
	} {
		var mu sync.Mutex
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			mu.Lock()
			defer mu.Unlock()
			w.Header().Set("Content-Type", "application/json")
			switch r.URL.Path {
			case "/api/v1/status":
				w.Write([]byte(`{"state":"connected","fw":"v1.6.4+0d9a87b"}`))
			case "/api/v1/sd/theme":
				io.Copy(io.Discard, r.Body)
				w.Write([]byte(`{"state":"filed","id":"` + c.ID + `","reboot":"none"}`))
			case "/api/v1/apps":
				if tc.appsFail {
					w.WriteHeader(500)
					w.Write([]byte(`{"error":"the card is gone"}`))
					return
				}
				w.Write([]byte(`{"mounted":true,"apps":[],"installed":` + tc.installed + `}`))
			case "/api/v1/sd":
				w.Write([]byte(`{"mounted":true,"themes":[]}`))
			default:
				w.Write([]byte(`{}`))
			}
		}))
		s := newHub()
		s.lib = &Library{AppsDir: tc.appsDir}
		s.device = orbnet.NewClient(deviceTimeout)
		s.device.DevicePort = portOf(srv.URL)
		s.ota = orbnet.NewClient(otaTimeout)
		s.ota.DevicePort = portOf(srv.URL)
		var last installStep
		ok := s.streamThemeStore(context.Background(), "127.0.0.1", theme, "Skeleton", c, true, true,
			func(st installStep) { last = st })
		srv.Close()
		if !ok {
			t.Errorf("%s: the push failed: %+v", tc.what, last)
			continue
		}
		if last.Action != "done" {
			t.Errorf("%s: ended %s, want done: %+v", tc.what, last.Action, last)
			continue
		}
		if last.AppMissing != tc.want {
			t.Errorf("%s: AppMissing %q, want %q", tc.what, last.AppMissing, tc.want)
		}
		if want := "is not installed on this orb"; tc.want != "" && !strings.Contains(last.Note, want) {
			t.Errorf("%s: note %q does not say the app is missing", tc.what, last.Note)
		} else if tc.want == "" && strings.Contains(last.Note, want) {
			t.Errorf("%s: note %q says the app is missing when it should not", tc.what, last.Note)
		}
	}
}
