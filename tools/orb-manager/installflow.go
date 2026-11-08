package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"path"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The library install (S6, plan §2 and §3): the hub's first-party files, pushed onto a dial
// through the device's own doors.
//
// This restores what S5 deleted, with one substitution and one narrowing.
//
// The substitution is the source. S5's seed read an //go:embed tree; this reads the
// resolved Library, so the files the Library pane lists are the files an install sends —
// there is no second copy to keep in step, and an operator who edits apps/ by hand has
// changed what the hub can install with no rebuild. The gate shape is the one S5 kept and is
// kept again here for the same reasons: a firmware floor, a mounted card checked before
// anything writes, the port claim held across the whole run so the liveness sweep cannot
// pulse DTR into a board mid-store, and every step asking the device what it already has
// before it writes, so a re-press after any partial costs a read and not a second copy.
//
// The narrowing is the automatic arm. Only the clock rides it. S5's seed sent the whole
// pair plus the theme, and it could, because it was a fixed two-file artifact; a library
// is a directory an operator fills, and a join that refilled a board an owner had just
// emptied, out of a directory full of experiments, would be the hub overruling the one
// person whose shelf it is. Everything else in apps/ waits for a press.

// installMinFW is the oldest firmware that reads these containers. The floor is the load-
// bearing number in this file, and it moved twice on the strength of the wrong evidence, so
// the reasoning is written down.
//
// It is v1.1.28 because the FORMAT 3 reader is THIS tree's uncommitted change: at HEAD,
// main/app/orb_app_manifest.h still says `ORBAPP_FORMAT 2u`. A format change is a change to
// what a shipped image promises, so .clinerules and docs/architecture.md both require the
// stamp to move with it — and version.txt had not moved, which is what let a floor name a
// release that could not read the files behind it. The floor is therefore the stamp this
// tree will build, not the last tag: no release out of the door today reads FORMAT 3, and
// v1.1.28 is the first release that can.
//
// The failure mode of getting this wrong is not subtle and it was paid for on hardware: with
// the floor at v1.1.27 the gate waved a format-2 board through, the hub pushed 660 KB over
// WiFi, and the device refused it with `app format 3; this build reads format 2`. A floor
// that is too HIGH refuses a board it could have served; too LOW wastes a minute and blames
// the board. TestInstallFloorNamesTheReleaseThatReadsFormat3 is the crosscheck that keeps the
// floor, the reader and the stamp from drifting apart again.
//
// v1.2.19 moved it again with FORMAT 4 (app-platform-plan §57: the settings schema region).
// An older board refuses every shelf container by number, so the floor names the release
// that reads them, and the hub says "update the firmware" before it pushes a byte.
//
// v1.2.30 is the merge of the radar line (bench builds v1.2.19-v1.2.29, never released,
// FORMAT 3 readers) with §57's: a board on one of those bench builds stamps above v1.2.21
// yet refuses FORMAT 4, so the floor is the first stamp above both that reads it.
const installMinFW = "v1.2.30"

// faceMinFW is the oldest firmware carrying the glass door (POST /api/v1/apps/face).
// The seeding arm parks the clock on the glass through that door, because an enabled,
// card-backed, themed row behind a menu is a half-set-up orb and the modal has no
// business calling it verified (owner ruling, 2026-10-29). A board behind this floor
// has no network lever for the glass at all — enable only walks a row — so the park
// says the door is missing and names the knob flick instead of refusing a board that
// cannot answer. v1.1.35 is the stamp that ships the door.
const faceMinFW = "v1.1.35"

func faceMinVersion() firmware.Version {
	v, ok := firmware.ParseVersion(faceMinFW)
	if !ok {
		panic("installflow.go: faceMinFW is not a versioned stamp: " + faceMinFW)
	}
	return v
}

// parkGlass puts the seeded clock on the glass through the firmware's face door, and waits
// the boot the door arms. It reports the second value false in the one case this hub cannot
// fix and cannot see: a dial behind faceMinFW has no glass door and no way to report the
// glass it was parked on, so the run gate drops to the five facts verifySeed reads and the
// operator hears the knob flick out loud. Refusing that board would blame it for a door the
// hub itself has not flashed. `parked` armed nothing and returns straight to the verify; the
// reboot is only the device's answer to a park it had to perform.
func (s *Server) parkGlass(ctx context.Context, ip string, send func(installStep)) (bool, error) {
	st, err := s.device.Status(ip)
	if err != nil {
		return false, fmt.Errorf("asking the dial before parking the glass: %w", plainDeviceErr(err, 0))
	}
	v, ok := firmware.ParseVersion(st.FW)
	if !ok || v.Compare(faceMinVersion()) < 0 {
		send(installStep{Step: "face", Action: "skipped",
			Note: fmt.Sprintf("%s has no glass door: flick the knob onto the clock to finish", st.FW)})
		return false, nil
	}
	send(installStep{Step: "face", Action: "parking", Note: "putting the clock on the glass"})
	body, err := s.device.AppFace(ip, "clock")
	if err != nil {
		return true, fmt.Errorf("the dial refused the face: %w", plainDeviceErr(err, 0))
	}
	var ans struct {
		State string `json:"state"`
	}
	state := ""
	if json.Unmarshal(body, &ans) == nil {
		state = ans.State
	}
	if state == "parked" {
		send(installStep{Step: "face", Action: "parked", Note: "the clock already wore the glass"})
		return true, nil
	}
	// The door armed a boot 750 ms out. Wait it out on the CYCLE, the shape every
	// reboot the ladder itself caused uses, on the same reboot-shaped beat, and
	// refuse the seed if the board does not come back — the slate stays armed and
	// the next announce retries. The cycle and not the bare wait is mandatory
	// here (bench, Bronze, 2026-10-29): the firmware answers the face door while
	// it is dying, and waitOrbBack's first probe reads that corpse as the return,
	// so the verify dialed a mid-reboot ledger, found the menu still on the glass,
	// and refused a park that had actually landed.
	live, ok := s.waitOrbCycle(ctx, ip, installRebootWait,
		beatFor(send, "face", "waiting through the park's reboot"))
	if !ok {
		return true, fmt.Errorf("the dial did not come back after the face park within %d s",
			int(installRebootWait/time.Second))
	}
	s.setOrbIP(live)
	send(installStep{Step: "face", Action: "parked", Note: "the clock is the glass"})
	return true, nil
}

// The install poll's leash, unchanged from S5's seed for its documented reason: the door
// acks the QUEUE and never the landing, so the seed watches the ledger the way the Apps
// face does. themeStoreBudget's posture applies — a device still saying busy outruns the
// soft deadline once and dies at the hard stop, rather than at a timeout that would
// re-issue a job the worker already holds. Vars, so a test can run the ladder on a clock
// a copy cannot hide behind.
var (
	installWait = 45 * time.Second
	installPoll = 1000 * time.Millisecond
	installHard = 4 * time.Minute
	// installRebootWait is the patience every post-reboot wait spends. 75 s was
	// the first cut and it bailed on live boards: the store's own reboot cuts
	// this firmware's WiFi for longer than that, and the wait dialled only the
	// address it started with, so a board that took a fresh lease during the
	// silence was waited for at a dead address until the budget died (owner
	// report, 2026-10-29 PM: every join-time seed after the first store died
	// on "i/o timeout" against the old lease). 3 minutes covers the boot, the
	// rejoin and the DHCP round trip, and waitOrbBack re-asks WHERE the chip
	// lives on every miss, so the window follows the board instead of the lease.
	installRebootWait = 3 * time.Minute
)

// installStep is one streamed line of an install run, NDJSON the way /api/flash and
// /api/provision stream theirs — an install crosses WiFi and takes minutes, and a silent
// spinner over that is indistinguishable from a hang.
type installStep struct {
	Step   string `json:"step"`   // a slug, "theme", or "install" for the run itself
	Action string `json:"action"` // uploaded, installing, installed, enabled, uploaded-theme, storing, stored, rebooting, parking, parked, skipped, unchecked, done, refused
	Note   string `json:"note,omitempty"`
	// Phase names the plan entry the line belongs to, and Plan rides the run's first line
	// (action "plan") with every entry, so the panel can say "step 3 of 7" and give a byte
	// transfer a bar and a wait only the orb can end a timer (owner, 2026-11-06: the bar sat
	// at 100% through the slot write, the store and both reboots, saying only "waiting").
	Phase string     `json:"phase,omitempty"`
	Plan  []planStep `json:"plan,omitempty"`
}

// planStep is one entry of a run's plan. Kind is "bytes" for a transfer the bar can
// measure and "wait" for one only the orb can end (a slot write, a store, a reboot);
// Budget is a wait's give-up point in seconds, so the panel can say how long it will try.
type planStep struct {
	ID     string `json:"id"`
	Label  string `json:"label"`
	Kind   string `json:"kind"`
	Budget int    `json:"budget,omitempty"`
}

// runPhases stamps each line of a run with the plan entry it belongs to. The ladder's own
// sends stay as they are: their actions already say which stage spoke, and a wait inherits
// the stage it sits in. installRun names the two stages no action marks — the opening
// check and the restart behind an install — by setting cur itself.
type runPhases struct {
	cur, theme string
}

func (p *runPhases) stamp(send func(installStep)) func(installStep) {
	return func(st installStep) {
		if st.Phase == "" {
			p.cur = p.phaseOf(st)
			st.Phase = p.cur
		}
		send(st)
	}
}

func (p *runPhases) phaseOf(st installStep) string {
	switch st.Step {
	case "theme":
		if ph := themePhase(st.Action); ph != "" {
			return ph + ":" + p.theme
		}
	case "install", "slate", "face", "verify":
	default: // a slug
		switch st.Action {
		case "uploading":
			return "send:" + st.Step
		case "uploaded", "installing", "installed", "skipped":
			return "install:" + st.Step
		case "restarted":
			return "restart"
		}
	}
	return p.cur
}

// installPlan is the run's plan, written before the first dial: every stage the run can
// reach, in order. A stage the orb makes unnecessary (the card already holds the file, the
// card already carries a theme) is still listed, and the panel marks it skipped, so the
// count an owner reads at the start is the count they watch finish.
func installPlan(lib *Library, slugs []string, card bool) []planStep {
	titles := map[string]string{}
	p := &LibraryPayload{}
	listAppLibrary(lib.AppsDir, p)
	for _, a := range p.Apps {
		if a.Title != "" {
			titles[a.Slug] = a.Title
		}
	}
	title := func(slug string) string {
		if t := titles[slug]; t != "" {
			return t
		}
		return slug
	}
	secs := func(d time.Duration) int { return int(d / time.Second) }
	restart := secs(cycleQuietGrace + installRebootWait)
	plan := []planStep{{ID: "check", Label: "Checking the orb", Kind: "wait", Budget: restart}}
	for _, slug := range slugs {
		if !card {
			plan = append(plan, planStep{ID: "send:" + slug, Label: "Sending " + title(slug) + " to the orb's card", Kind: "bytes"})
		}
		plan = append(plan, planStep{ID: "install:" + slug, Label: "Installing " + title(slug), Kind: "wait", Budget: secs(installHard)})
	}
	plan = append(plan, planStep{ID: "restart", Label: "Restarting the orb to load it", Kind: "wait", Budget: restart})
	for _, slug := range slugs {
		if slug != "clock" && !lib.hasAppTheme(slug) {
			continue
		}
		for _, st := range themeSteps(title(slug)+"'s theme", restart) {
			st.ID += ":" + slug
			plan = append(plan, st)
		}
	}
	return plan
}

// themeSteps is a theme install's stages, the same five wherever a theme goes on: the
// file crosses to the SD card, the orb checks it and parks the dial, the theme area packs
// the other apps' themes down when this one will not fit as it stands, the copy moves the
// theme into flash, and the orb restarts to wear it (owner, 2026-11-06). Making room is
// skipped on the rows when the orb never reports it. Older firmware reports no stage at
// all, and its whole store then shows on the check row.
func themeSteps(what string, restart int) []planStep {
	return []planStep{
		{ID: "theme-send", Label: "Copying " + what + " to the orb's SD card", Kind: "bytes"},
		{ID: "theme-check", Label: "Checking the theme file", Kind: "wait", Budget: int(themeStoreBudget / time.Second)},
		{ID: "theme-room", Label: "Making room in the orb's flash", Kind: "bytes"},
		{ID: "theme-store", Label: "Moving the theme into flash", Kind: "bytes"},
		{ID: "theme-restart", Label: "Restarting the orb to wear the theme", Kind: "wait", Budget: restart},
	}
}

// themePhase maps a theme line's action to its themeSteps row; "" leaves the line on the
// row it is already on (a wait's beat).
func themePhase(action string) string {
	switch action {
	case "uploading", "uploaded-theme", "skipped", "unchecked":
		return "theme-send"
	case "storing", "checking-file":
		return "theme-check"
	case "making-room":
		return "theme-room"
	case "moving", "stored":
		return "theme-store"
	case "rebooting":
		return "theme-restart"
	}
	return ""
}

// storeStages turns the stages a running store reports into step lines: one when the
// stage changes, and one per ~5% of its bytes, so a 2.8 MB move is a bar that fills and
// not a row that sits. "rebooting" is the caller's own line.
func storeStages(send func(installStep), step string) storeStage {
	last, lastPct := "", -1
	return func(stage string, done, total uint32) {
		action := map[string]string{"checking": "checking-file", "parking": "checking-file",
			"making room": "making-room", "storing": "moving"}[stage]
		if action == "" {
			return
		}
		pct := -1
		if total > 0 {
			pct = int(uint64(done) * 100 / uint64(total))
		}
		if stage == last && (pct < 0 || (pct-lastPct < 5 && pct != 100) || pct == lastPct) {
			return
		}
		last, lastPct = stage, pct
		note := ""
		if total > 0 {
			note = fmt.Sprintf("%d of %d KB", done/1024, total/1024)
		}
		send(installStep{Step: step, Action: action, Note: note})
	}
}

// h_libraryInstall runs one install for one orb and streams the steps. It is the press, and it
// is also what the automatic arm calls, so the two paths cannot drift apart the way a
// route and its separate caller could.
//
// The gates are every device-dialing route's: the address must be one this hub learned
// (checkedTarget), and the route stands behind the launch token at registration because
// it writes to somebody's board.
func (s *Server) h_libraryInstall(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	ip := strings.TrimSpace(r.URL.Query().Get("ip"))
	only := strings.TrimSpace(r.URL.Query().Get("only"))
	automatic := r.URL.Query().Get("auto") == "1"
	// D38: the row's Update press. The shelf's copy rides to the card even when
	// the card already holds a readable file and the dial already runs the slug:
	// the press was earned by the shelf being NEWER, and a run that skipped its
	// way to nothing would let the button lie.
	force := r.URL.Query().Get("force") == "1"
	// D38: the row's Install press when the CARD already holds a readable copy. The
	// shelf's bytes need not cross the room a second time to install a file the card
	// already holds, but the press still runs the streamed ladder so the owner sees
	// installing / installed / enabled instead of a silent POST and a spinner.
	card := r.URL.Query().Get("card") == "1"
	if _, ok := s.checkedTarget(w, ip, "", false); !ok {
		return
	}
	if automatic && s.noAutoInstall {
		s.jsonErr(w, "the automatic install is off (-no-autoinstall); press Install on Clock on the Apps tab")
		return
	}
	release, rerr := s.holdDevicePort(r.Context(), ip, deviceTimeout)
	if rerr != nil {
		s.jsonErr(w, rerr.Error())
		return
	}
	defer release()

	flusher, _ := w.(http.Flusher)
	send := func(st installStep) {
		b, _ := json.Marshal(st)
		w.Write(append(b, "\n"...))
		if flusher != nil {
			flusher.Flush()
		}
	}

	ctx, cancel := context.WithTimeout(r.Context(), 12*time.Minute)
	defer cancel()
	// The join-time seed ends in a park and a verify after the run, and those are a stage
	// the owner waits through too: the plan names it, and every line after the run is its.
	verifying := false
	if automatic && only == "clock" {
		raw := send
		send = func(st installStep) {
			if st.Action == "plan" {
				st.Plan = append(st.Plan, planStep{ID: "verify", Label: "Checking the orb is ready",
					Kind: "wait", Budget: int(installRebootWait / time.Second)})
			}
			if verifying && st.Phase == "" {
				st.Phase = "verify"
			}
			raw(st)
		}
	}
	// A board this hub just adopted arrives wearing the shipped baseline and nothing
	// else, so what it carries is erased before the baseline goes on. Only the automatic
	// arm wipes: a hand press means an operator is holding the shelf, and an operator who
	// pressed Install on their own orb never asked for a wipe (see cleanslate.go).
	slateMAC := ""
	if automatic {
		slateMAC = s.slateForIP(ip)
		if s.seedEndsWithoutCard(ip, slateMAC, send) {
			return
		}
		// D38's leave-the-owner's-shelf-alone rule used to live in the page, ahead of this
		// route, and it gated out the one case it must not: a board whose SD card still
		// carries the last orb life's containers — which is every lonely-erased board, since
		// a ROM chip erase does not touch the card. The leftovers read as the owner's doing,
		// the seed never ran, and the first flash of a new orb kept a shelf of stale apps and
		// got no face (bench, 2026-10-29). The gate now reads the same ledger from the same
		// place the wipe does, so an adoption outranks it and an owner's curated shelf still
		// stands. An unreadable ledger falls through and lets the ladder decide, as before.
		if !s.slate.pending(slateMAC) {
			if held, err := s.shelfHoldsOwnersContent(ip); err == nil && held {
				// A standing shelf only outranks the seed when the shelf WORKS. The bench
				// (2026-10-29, Bronze): a leftover hollow row read as owner content, this
				// run skipped to done, and the page headline claimed the clock was on the
				// glass while the card held zero files. A skip must earn its `done` on the
				// same post-read the ladder earns on; a shelf that cannot answer it is
				// reseeded, not skipped — the ladder's own hollow-row gate refills a dead
				// promise and installTheme stands down the moment the card carries themes.
				if verr := s.verifySeedRun(ctx, ip, send); verr == nil {
					send(installStep{Step: "install", Action: "skipped",
						Note: "the card or the registry already holds what the owner put there"})
					send(installStep{Step: "install", Action: "done"})
					return
				} else {
					log.Printf("orb-manager: the standing shelf on %s did not verify (%s); reseeding the baseline", ip, verr.Error())
					send(installStep{Step: "install", Action: "reseeding",
						Note: verr.Error() + " — refilling it from the library"})
				}
			}
		}
	}
	if slateMAC != "" && s.slate.pending(slateMAC) {
		send(installStep{Step: "slate", Action: "wiping", Note: "first flash of a new orb"})
		if err := s.cleanSlate(ctx, ip, send); err != nil {
			log.Printf("orb-manager: the clean slate on %s was refused: %s", ip, err.Error())
			send(installStep{Step: "slate", Action: "refused", Note: err.Error()})
			s.jsonErr(w, err.Error())
			return
		}
	}
	if err := s.installRun(ctx, ip, only, force, card, send); err != nil {
		// The step goes to the browser AND the log. A run that fails after a reboot
		// can leave the page before the line does — the whole ladder then disappears
		// with the tab, and the hub's log shows only the probe exemption that said
		// "installing" (bench, new board, 2026-10-29). The operator needs the reason
		// after the tab is gone.
		log.Printf("orb-manager: the library install on %s was refused (only=%q force=%v card=%v): %s",
			ip, only, force, card, err.Error())
		send(installStep{Step: "install", Action: "refused", Note: err.Error()})
		return
	}
	// On the automatic arm the run's `done` is not the end of the story. installRun reports
	// installed off the ledger row and the theme step, and a row is a promise rather than the
	// bytes behind it — the bench caught a hub printing installed with nothing on the card
	// twice on 2026-10-29. verifySeed re-reads the device's own answers and only a clean
	// verdict lets the adoption finish. A failure here leaves the slate armed, so the next
	// announce retries the whole seed rather than leaving a board half-installed and marked
	// done (see cleanslate.go and the retry note below). The gate is NOT conditioned on the
	// slate mark: a board this hub adopts from a row it cannot map (a lease moved, a grants
	// row lost) is exactly a board whose green must be earned, and verify reads only.
	if automatic && only == "clock" {
		verifying = true
		send(installStep{Step: "verify", Action: "checking", Note: "reading back the clock, its face and the glass"})
		// The ladder's last steps reboot the board, and the chip may answer at a
		// different address when it comes back. Verify the CHIP, not the address
		// the run happened to start on.
		if live, ok := s.waitOrbBack(ctx, ip, installRebootWait); ok {
			ip = live
		}
		// Park the glass, then wait out the boot the park armed, then read the ledger
		// again. The order is the fix: installTheme stands down on a card that already
		// carries themes, and the theme store was the only network path that ever wrote
		// the resume record — so a seeded clock could finish enabled, backed and themed
		// with the menu still on the glass (owner ruling, 2026-10-29). A park that comes
		// back `parked` armed no boot and costs nothing. A dial too old for the door
		// drops the gate to the five facts and says the knob flick out loud, because the
		// hub cannot move a glass it has no handle on and must not call that a green.
		gated, prerr := s.parkGlass(ctx, ip, send)
		if prerr != nil {
			log.Printf("orb-manager: the join seed on %s could not park the glass: %s", ip, prerr.Error())
			send(installStep{Step: "install", Action: "refused", Note: prerr.Error()})
			return
		}
		var verr error
		if gated {
			verr = s.verifySeededGlass(ctx, ip, send)
		} else {
			verr = s.verifySeedRun(ctx, ip, send)
		}
		if verr != nil {
			log.Printf("orb-manager: the join seed on %s did not verify: %s", ip, verr.Error())
			send(installStep{Step: "install", Action: "refused", Note: verr.Error()})
			return
		}
	}
	send(installStep{Step: "install", Action: "done"})
	// The adoption is only over when the baseline landed on top of the wipe. Consuming
	// the mark here means a run that died midway is retried on the next announce rather
	// than leaving a board half-seeded and marked done.
	if slateMAC != "" {
		s.slate.clear(slateMAC)
	}
	log.Printf("orb-manager: installed the library on %s (only=%q auto=%v)", ip, only, automatic)
}

// seedEndsWithoutCard is the join seed's first read. A new orb with no SD card is a finished
// setup, not a failed one: the firmware and the WiFi are on it, and only the apps wait for a
// card (owner, 2026-11-07: the seed "errored out on the app copy" with no word that things
// were fine). It streams `nocard` with the next step and a `done`, and the page words the
// ending. The slate mark goes with it, because a board with no card has nothing for a wipe to
// clear, and a mark left standing would wipe the apps the owner installs by hand on the next
// join. A card the orb cannot read is the same ending with the orb's own reason in front: the
// fix is the card, and the apps follow it. A ledger that does not answer decides nothing here;
// the ladder's own patient reads take it from there.
func (s *Server) seedEndsWithoutCard(ip, slateMAC string, send func(installStep)) bool {
	listing, err := s.device.AppsList(ip)
	if err != nil || listing.Mounted {
		return false
	}
	log.Printf("orb-manager: the join seed on %s found no usable SD card (%s); setup ends without apps", ip, listing.Why)
	if slateMAC != "" {
		s.slate.clear(slateMAC)
	}
	send(installStep{Step: "install", Action: "nocard", Note: noCardSentence(listing.Why)})
	send(installStep{Step: "install", Action: "done"})
	return true
}

// noCardSentence is the join seed's word for a new orb with no usable SD card: setup is
// done, and the apps follow once a card is in. why is the orb's own reason (orb_sd_why):
// an empty socket answers "no card answered" after its boot try, a card in a format the orb
// cannot read names the format. The Apps tab's Install press is the same ladder the seed
// runs, and a clock install brings its face with it.
func noCardSentence(why string) string {
	head := "this orb has no SD card, so no apps were copied"
	if why != "" && !strings.HasPrefix(why, "no card") {
		head = "this orb cannot use its SD card (" + why + "), so no apps were copied"
	}
	return head + ". Everything else is set up. Insert a microSD card (FAT32), then press Install " +
		"on Clock on the Apps tab: its face comes with it, and any other app installs the same way"
}

// installRun is the route's body. With only set it sends that one slug and nothing else —
// the automatic arm's shape, and also what a per-row press asks for. With only empty it
// walks the library's app listing in slug order. Either way the theme step follows a clock
// that is live on the dial, and only then (D36's rule: the skeleton theme rides with the
// clock, so a board whose owner keeps only the picker never takes a theme nobody asked
// for). The clock arm of that gate is load-bearing: the join-time run asks for exactly
// `only=clock`, so a theme step reachable only from a full run never fires on the one
// board that has no face yet. force
// belongs to a single-slug press only; the full run stays ask-first, because the run is a
// refill and the refill does not overwrite a shelf the owner curated (D38). card (D38's
// Install press on a card-resident row) pairs with exactly one only: the ladder runs with
// NO shelf blob at all, so nothing re-uploads and the card's own readable copy installs —
// but every step still streams, because a press without visible steps is a press the
// owner cannot trust.
func (s *Server) installRun(ctx context.Context, ip, only string, force, card bool, send func(installStep)) error {
	lib := s.library()
	slugs := []string{}
	if only != "" {
		slugs = []string{only}
	} else {
		p := &LibraryPayload{}
		listAppLibrary(lib.AppsDir, p)
		for _, a := range p.Apps {
			slugs = append(slugs, a.Slug)
		}
		if len(slugs) == 0 {
			return fmt.Errorf("the library at %s holds no loadable app to install", lib.AppsDir)
		}
	}
	if card && only == "" {
		return fmt.Errorf("a card-only install names exactly one slug")
	}
	phases := &runPhases{cur: "check"}
	send(installStep{Step: "install", Action: "plan", Phase: "check", Plan: installPlan(lib, slugs, card)})
	send = phases.stamp(send)
	st, err := s.device.Status(ip)
	if err != nil {
		// A board that does not answer the ladder's first dial is usually mid-boot
		// from the commit its own join just made — the seed fires seconds after the
		// join dialog sees one status line, and the config apply reboots right
		// behind it (owner report, 2026-10-29 PM: every join seed died here, and
		// the clock and theme never went out). Wait the boot out on the CHIP —
		// following its lease — and only then call the run refused.
		send(installStep{Step: "install", Action: "waiting",
			Note: "the orb is not answering yet — waiting through its boot before the run"})
		var back bool
		ip, back = s.waitOrbBack(ctx, ip, installRebootWait,
			beatFor(send, "install", "waiting through its boot"))
		if !back {
			return fmt.Errorf("the orb did not answer its status: %w", err)
		}
		if st, err = s.device.Status(ip); err != nil {
			return fmt.Errorf("the orb did not answer its status: %w", err)
		}
	}
	if v, ok := firmware.ParseVersion(st.FW); !ok {
		return fmt.Errorf("the dial reports %q, which is not a versioned firmware: flash it to %s first", st.FW, installMinFW)
	} else if v.Compare(installMinVersion()) < 0 {
		return fmt.Errorf("this orb runs firmware %s, and these apps need %s or newer: update its firmware first (Firmware tab, or flash it over the cable), then install the apps",
			st.FW, installMinFW)
	}
	listing, err := s.device.AppsList(ip)
	if err != nil {
		// A status line can come from a board that is already dying — the reboot
		// door answers its last request while it shuts its WiFi down — and a
		// freshly booted board takes longer to re-scan its card than the 8 s a
		// control dial allows. The read that opens the run gets one silence-first
		// wait and one re-hand (bench, 2026-10-29 PM: a seed died on exactly this
		// ledger dial thirty seconds into a boot it was otherwise surviving).
		send(installStep{Step: "install", Action: "waiting",
			Note: "the orb did not answer its ledger — waiting through its boot before the run"})
		var back bool
		ip, back = s.waitOrbCycle(ctx, ip, installRebootWait,
			beatFor(send, "install", "waiting through its boot for the ledger"))
		if back {
			listing, err = s.device.AppsList(ip)
		}
		if err != nil {
			return fmt.Errorf("the orb did not answer its app ledger: %w", err)
		}
	}
	if !listing.Mounted {
		if listing.Why != "" && listing.Why != "no card in the socket" {
			return fmt.Errorf("the orb's SD card is not usable — %s. Apps are installed onto the card", listing.Why)
		}
		return fmt.Errorf("no card in the socket — the install lands files on the card, so insert or mount one first")
	}

	fwv, _ := firmware.ParseVersion(st.FW)
	rebooted := false
	cycled := false // the last install's reboot was seen through to the board's return
	for _, slug := range slugs {
		var blob []byte
		if !card {
			b, err := lib.Container(slug)
			if err != nil {
				return fmt.Errorf("reading %s from the library: %w", slug, err)
			}
			// A library copy of an older FORMAT is a release no firmware this hub installs
			// onto can run; the copy compiled into this hub is the same app, current. Send
			// that rather than a container the orb refuses by number (owner, 2026-11-08: a
			// re-flashed orb's setup died on "writing clock to the card: rejected: app
			// format 3; this build reads format 4").
			if orbnet.AppFormatOlder(b) {
				if sb, serr := fs.ReadFile(shippedLib, path.Join(libAppsSubdir, slug+".orbapp")); serr == nil {
					if _, perr := orbnet.ParseApp(sb); perr == nil {
						send(installStep{Step: slug, Action: "checking",
							Note: "the hub library's copy is for older firmware; sending the copy built into this hub (Restore bundled apps puts it in the library)"})
						b = sb
					}
				}
			}
			// The app's own floor (app.json os_min): an older OS loads it as a failed row
			// with no reason (Gold, 2026-11-04), so the hub says it before a byte moves.
			if need := appOSMin(b); need != "" {
				if nv, ok := firmware.ParseVersion(need); ok && fwv.Compare(nv) < 0 {
					return fmt.Errorf("%s needs firmware %s or newer, and this orb runs %s: update its firmware on the Firmware tab first", slug, need, st.FW)
				}
			}
			if len(b) == 0 {
				return fmt.Errorf("%s is not in the library at %s", slug, lib.AppsDir)
			}
			blob = b
		}
		landed := false // an install that wrote a slot reboots the orb; a skip does not
		sendOne := func(st installStep) {
			if st.Step == slug && st.Action == "installed" {
				landed = true
				cycled = false
			}
			if st.Step == slug && st.Action == "restarted" {
				cycled = true
			}
			send(st)
		}
		if err := s.installOneApp(ctx, ip, slug, blob, force && only != "", sendOne); err != nil {
			return err
		}
		rebooted = rebooted || landed
	}
	// The theme rides the CLOCK, not the full-library run. The join-time install
	// asks for exactly one slug (`only=clock`), and a clock with no theme on the
	// card has no face to wear: the shell keeps the glass, the row reports installed
	// and enabled, and the owner sees no clock on the dial (bench, new board,
	// 2026-10-29: clean flash plus Wi-Fi setup drew no clock, and the card held zero
	// themes because this step sat behind a gate the join path never opens). D36's
	// rule stands underneath this one: the theme goes only onto a dial the clock is
	// actually on, so a lone color_picker press still seeds nothing. installTheme
	// skips on its own whenever the card already carries themes, so riding every
	// clock run cannot tread on an owner's shelf.
	//
	// Every themed app, not the clock alone (owner, 2026-11-04: flights installed on Gold
	// with no theme): each app this run installed that has a default in the library gets it
	// when the card holds no theme of its own, and only once the app is live on the dial.
	waited := !rebooted || cycled // nothing landed, or its reboot is already behind the run
	for _, slug := range slugs {
		// the clock always asks (a clock with no face draws nothing, and a library with no
		// theme is an error worth saying); another app only when the library has one for it
		if slug != "clock" && !lib.hasAppTheme(slug) {
			continue
		}
		// An install ends with the orb rebooting to load the app, and a ledger read in that
		// gap answers nothing; the seed waits the boot out and asks again rather than read
		// silence as "not installed" (Gold, 2026-11-04: flights landed and got no theme).
		// The orb also answers for a beat while it goes down, so the wait is the cycle-shaped
		// one, once per run, before the first seed's question.
		if !waited {
			waited = true
			phases.cur = "restart"
			send(installStep{Step: "install", Action: "restarting", Note: "the orb restarts to load the app"})
			if back, ok := s.waitOrbCycle(ctx, ip, installRebootWait,
				beatFor(send, "theme", "waiting for the orb to come back before its theme")); ok {
				ip = back
			}
		}
		if !s.installAppLive(ip, slug) {
			continue
		}
		phases.theme = slug
		phases.cur = "theme-send:" + slug
		if err := s.installTheme(ctx, ip, slug, send); err != nil {
			return err
		}
	}
	// An install with no theme behind it still reboots the orb, and a run that said done
	// into that gap let the page re-read a board that was mid-boot (owner, 2026-11-06: done
	// means the orb is back). The same cycle-shaped wait, once.
	if !waited {
		phases.cur = "restart"
		send(installStep{Step: "install", Action: "restarting", Note: "the orb restarts to load the app"})
		if _, ok := s.waitOrbCycle(ctx, ip, installRebootWait,
			beatFor(send, "install", "waiting for the orb to come back with the app")); !ok {
			return fmt.Errorf("the app landed but the orb has not answered again within %s", installRebootWait)
		}
	}
	return nil
}

// library answers the one question a nil field would otherwise make a runtime surprise:
// every route and every install path asks the same way, and a hub that resolved nothing gets
// an empty Library rather than a panic three calls into a streamed run.
func (s *Server) library() *Library {
	s.pmu.Lock()
	lib := s.lib
	s.pmu.Unlock()
	if lib == nil {
		return &Library{}
	}
	return lib
}

// installMinVersion parses installMinFW, which is a constant written in the scheme; a failure is
// a source bug and must crash at startup rather than negotiate at runtime.
func installMinVersion() firmware.Version {
	v, ok := firmware.ParseVersion(installMinFW)
	if !ok {
		panic("installflow.go: installMinFW is not a versioned stamp: " + installMinFW)
	}
	return v
}

// installOneApp brings one library slug onto the dial: on the card, in a slot, turned on.
// Each half asks the device first, so a re-press after any partial costs a read and not a
// second copy, and an owner's hand-installed clock is honoured untouched — only its
// enable word may get written, and only from off. force (D38's Update press) inverts
// both asks: the shelf's bytes cross again even where a readable card file and a live
// slot already stand, because the press was made precisely because those copies are
// older than the shelf.
func (s *Server) installOneApp(ctx context.Context, ip, slug string, blob []byte, force bool, send func(installStep)) error {
	listing, err := s.device.AppsList(ip)
	if err != nil {
		return fmt.Errorf("the orb did not answer its app ledger: %w", err)
	}
	if !force {
		for _, row := range listing.Installed {
			if row.Slug != slug {
				continue
			}
			// A row is not the bytes. The dial resolves an installed row from its
			// `apps` partition header, and a card-tier app keeps its code on the card
			// (the firmware reports code_size 0 for such a row), so the row can stand
			// with nothing behind it: a remove-from-card, a card swapped out, or a
			// store that died mid-write each leaves a row that never reaches the glass, and answers "installed" to a hub that skipped on the row
			// alone (bench, new board, 2026-10-29: clean flash then Wi-Fi setup, and
			// the hub printed installed while no byte had ever crossed). A built-in
			// row carries its code in the image and has no card copy to miss.
			if needsCardBytes(row) && !cardHolds(listing, slug) {
				send(installStep{Step: slug, Action: "installing",
					Note: "the row stands but the card copy is gone — sending the bytes"})
				break
			}
			send(installStep{Step: slug, Action: "skipped", Note: "already on the dial"})
			return nil
		}
	}

	// The card write, unless the file already sits there clean. A row the dial refuses
	// (bad CRC, foreign slug) is the operator's fact to read, not the install's to
	// overwrite — unless the press itself said overwrite: the Update press exists
	// because the shelf is newer than both copies, and it pushes the shelf's bytes
	// over a readable card file and a refused one alike (D38).
	onCard := false
	if !force {
		for _, row := range listing.Apps {
			if row.Stem == slug {
				// A card copy refused only for its FORMAT is a release the firmware has
				// moved past (orb_app_manifest.c's "app format N; this build reads format
				// M"): the library's bytes are its replacement, so they go over it. Every
				// orb that took v1.2.32's firmware from v1.2.16 held four (owner,
				// 2026-11-08). Any other refusal stays the operator's fact to read.
				if !row.OK && len(blob) > 0 && strings.HasPrefix(row.Why, "app format ") {
					send(installStep{Step: slug, Action: "installing",
						Note: "the card copy is for older firmware (" + row.Why + ") — replacing it"})
					break
				}
				if !row.OK {
					return fmt.Errorf("%s.orbapp sits on the card but the dial refuses it: %s", slug, row.Why)
				}
				onCard = true
				break
			}
		}
	}
	// A card-only press (empty blob) with no readable card copy under it is a race
	// between the ledger read the face painted from and this one — the file moved
	// out from under the button. Name that, do not "upload" zero bytes.
	if len(blob) == 0 && !onCard {
		return fmt.Errorf("%s.orbapp is no longer readable on the card — re-read the list and press again", slug)
	}
	if !onCard {
		// The bulk client, not the control client. h_app_upload writes the whole container
		// to the card inside the request and answers only at the end, and the firmware's own
		// page allows 900 seconds for it; the 8-second management budget cut a healthy board
		// at roughly the 50 KB mark and the refusal blamed the orb. The progress callback
		// turns the same stretch into the one fact the screen can honestly state: how many
		// bytes the transport has written.
		total := len(blob)
		last, sentLast := 0, int64(0)
		_, uerr := s.ota.AppUploadProgress(ip, blob, true, func(sent int64) {
			sentLast = sent
			// One line per ~10 percent: a line per chunk is the noise the watch loop's
			// every-other-tick rule exists to prevent, and a percent is all a bar needs.
			pct := int(sent * 100 / int64(total))
			if pct-last < 10 && int(sent) != total {
				return
			}
			last = pct
			send(installStep{Step: slug, Action: "uploading",
				Note: fmt.Sprintf("%d of %d KB", sent/1024, int64(total)/1024)})
		})
		if uerr != nil {
			// sentLast, not total: a refusal that claims the whole file crossed when it
			// stalled at a fifth of it sends the operator to blame the wrong thing.
			return fmt.Errorf("writing %s to the card: %w", slug, plainDeviceErr(uerr, int(sentLast)))
		}
		send(installStep{Step: slug, Action: "uploaded"})
	}

	// The install: queue it, then watch the ledger for the row.
	if _, err := s.device.AppInstallCard(ip, slug); err != nil {
		return fmt.Errorf("installing %s: %w", slug, plainDeviceErr(err, 0))
	}
	send(installStep{Step: slug, Action: "installing"})
	deadline := time.Now().Add(installWait)
	hard := time.Now().Add(installHard)
	// The dial reboots 750 ms after the slot write (orb_jobs_reboot_soon), so this poll
	// usually rides the whole reboot: the ledger goes silent, and the row appears on the
	// board that came back. A row seen after a silence is a reboot already over, and the
	// run says so instead of then spending the cycle wait's 20 s of quiet proving it
	// (owner, 2026-11-06: "restarting" lit up long after the orb was back).
	silent := false
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(installPoll):
		}
		list, err := s.device.AppsList(ip)
		if err != nil {
			silent = true
		}
		if err == nil {
			for _, row := range list.Installed {
				if row.Slug == slug {
					send(installStep{Step: slug, Action: "installed"})
					if silent {
						send(installStep{Step: slug, Action: "restarted", Note: "the orb restarted to load it while it installed"})
					}
					return nil
				}
			}
			// A row that failed the ladder's own gates shows up refused on the card with a
			// fresh install refusing to appear: name it rather than let the poll run blind
			// on a board that has already said no.
			for _, row := range list.Apps {
				if row.Stem == slug && !row.OK {
					return fmt.Errorf("the dial refused %s after the install: %s", slug, row.Why)
				}
			}
		}
		if time.Now().After(hard) {
			return fmt.Errorf("%s did not land within %s — the copy may still be running; press Install again to check",
				slug, installHard)
		}
		if time.Now().After(deadline) {
			deadline = time.Now().Add(installWait) // one full extension, then the hard stop
		}
	}
}

// needsCardBytes reports whether an installed row can only run from a card copy.
// The firmware emits exactly one built-in row (the shell, `compiled:true`) and it
// carries its code inside the image, so an empty card costs it nothing. Every
// other row the dial reports is card-tier: orb_app_install_resolve_boot rebuilt it
// from an `apps` partition HEADER, and for the native apps that exist today the
// header carries no code_size to point at, so the bytes live only in card/apps.
// The row is the promise; the card file is the substance.
func needsCardBytes(row orbnet.AppInstalledRow) bool {
	return !row.Compiled
}

// cardHolds reports whether the card tier of a fresh ledger listing carries a
// readable copy of one slug. The installed tier and the card tier answer
// different questions — "the dial has a row" and "the card has the bytes" — and
// an install may satisfy only the first. installOneApp asks this before it
// believes an installed row, and the dial's own remove-from-card is what empties
// the answer while the row stays.
func cardHolds(listing *orbnet.AppList, slug string) bool {
	for _, row := range listing.Apps {
		if row.Stem == slug {
			return row.OK
		}
	}
	return false
}

// cleanSlate empties a board this hub is adopting: every app row off the dial, and the
// library's own card trees erased so the seed that follows lands on a card nobody has
// curated. It runs before installRun and only for the automatic arm, and it is the only
// hub path that erases a card tree the operator did not name.
//
// Uninstall first, then the card. The order is the firmware's: a row whose card file is
// gone still resolves from its header and still holds its NVS words, while an uninstalled
// app leaves the card copy where the owner can see it. Erasing the card under a live row
// is precisely the hollow state bug 9 documents, so a wipe that took the files first
// would manufacture the very thing it is meant to prevent.
//
// The shell row is not touched. `compiled:true` is the firmware's own word for the built
// into the image, the dial cannot run without it, and no verb the hub can dial removes it.
func (s *Server) cleanSlate(ctx context.Context, ip string, send func(installStep)) error {
	list, err := s.device.AppsList(ip)
	if err != nil {
		return fmt.Errorf("the orb did not answer its app ledger: %w", err)
	}
	if !list.Mounted {
		return fmt.Errorf("no card in the socket — a clean slate needs one to write into")
	}
	for _, row := range list.Installed {
		if row.Compiled {
			continue
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		if _, e := s.device.AppUninstall(ip, row.Slug); e != nil {
			// A silent board behind an uninstall is the boot the previous row's
			// dial-rebuild took, not a refusal: wait it out once, at wherever the
			// chip answers now, and hand the call back before calling the slate a
			// failure (owner report, 2026-10-29 PM: i/o timeout killed slates
			// mid-erase on boards that were simply rebooting).
			var back bool
			if ip, back = s.waitOrbBack(ctx, ip, installRebootWait,
				beatFor(send, "install", "waiting through a reboot before re-handing the uninstall of "+row.Slug)); back {
				_, e = s.device.AppUninstall(ip, row.Slug)
			}
			if e != nil {
				return fmt.Errorf("clearing %s: %w", row.Slug, plainDeviceErr(e, 0))
			}
		}
		s.grants.learnAs(ip, s.macFor(ip))
		send(installStep{Step: row.Slug, Action: "uninstalled", Note: "clean slate"})
	}
	// Both library trees, by the names the device files them under. A card that holds
	// neither answers an error, which the caller reads as nothing to erase: a brand-new
	// board lands here with an empty card and must not be refused for it. A dial that
	// times out is a board mid-boot more often than a card refusing, and the ladder
	// died on exactly that reading (owner report, 2026-10-29 PM): the failure waits
	// the boot out once, at wherever the chip answers now, and hands the call back
	// before the tree is called unerasable.
	for _, tree := range []string{"apps", "themes"} {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		if _, e := s.device.SDRemove(ip, tree, true); e != nil {
			var back bool
			if ip, back = s.waitOrbBack(ctx, ip, installRebootWait,
				beatFor(send, "install", "waiting through a reboot before re-handing the erase of "+tree)); back {
				_, e = s.device.SDRemove(ip, tree, true)
			}
			if e != nil {
				log.Printf("orb-manager: clean slate on %s left %s alone: %s", ip, tree, plainDeviceErr(e, 0))
				send(installStep{Step: tree, Action: "skipped", Note: "nothing to erase"})
				continue
			}
		}
		s.grants.learnAs(ip, s.macFor(ip))
		send(installStep{Step: tree, Action: "erased", Note: "clean slate"})
	}
	// The rows the hub remembered are now false on every count, and the card list with
	// them. Same law the theme store obeys after a write: the next read must be a dial.
	s.dropThemeCache(ip)
	return nil
}

// slateForIP answers the one question the automatic arm cannot ask a board: was THIS
// address the chip that left a first flash. It is a MAC lookup and not a device read,
// because no device answer separates an adoption from an upgrade (see cleanslate.go).
func (s *Server) slateForIP(ip string) string {
	if s.fleet == nil {
		return ""
	}
	rows := s.fleet.ClaimedBy(ip)
	switch len(rows) {
	case 1:
		return rows[0].MAC
	case 0:
		// No live row yet: the lonely write forgot the chip's row, and a board seconds
		// out of its join may not have re-earned one when the page asks for the seed.
		// The board at the address names itself, and a mark only ever exists for a chip
		// this hub just flashed, so its own word is enough to find that mark.
		if st, err := s.device.Status(ip); err == nil && st != nil {
			return st.MAC
		}
		return ""
	default:
		// Two boards on one address is a contention the fleet already reports, and the
		// hub will not wipe on behalf of an address it cannot attribute.
		return ""
	}
}

// shelfHoldsOwnersContent is the hub's copy of the D38 gate: did the owner put anything on
// this board. Either tier counts — a container readable on the card, or a row in a slot — and
// the built-in shell row never counts, because the firmware wrote it and the owner cannot take
// it off. Counting it would mean no dial ever read as empty and a board out of the box sat
// uninstalled forever.
//
// It lives here rather than in the page for the reason installflow's own header gives: the
// page could not see the adoption mark, so a card of leftovers outranked a first flash.
func (s *Server) shelfHoldsOwnersContent(ip string) (bool, error) {
	list, err := s.device.AppsList(ip)
	if err != nil {
		return false, err
	}
	for _, r := range list.Installed {
		if r.Compiled || r.Failed {
			continue
		}
		// A row is a promise; the bytes behind it are the substance. A row that needs
		// a card copy and has none is leftover from a broken orb life, not a shelf anybody curated — counting it as the owner's is how an
		// adoption skipped over a zero-file card while the page said installed (bench,
		// 2026-10-29, Bronze). The ladder's hollow-row gate refills exactly this row.
		if needsCardBytes(r) && !cardHolds(list, r.Slug) {
			continue
		}
		return true, nil
	}
	return len(list.Apps) > 0, nil
}

// installClockLive asks the ledger the one question the theme gate needs, at the moment it
// needs it: is a clock row on the dial right now.
func (s *Server) installAppLive(ip, slug string) bool {
	list, err := s.device.AppsList(ip)
	if err != nil {
		return false
	}
	for _, row := range list.Installed {
		if row.Slug == slug && !row.Failed {
			return true
		}
	}
	return false
}

// installTheme offers the library's first theme onto a dial that has no face. An owner's
// themes on the card, or an installed theme already in the slot, both stop the step cold:
// the shelf is the owner's. The store door reboots the board by design (the single-slot
// store IS the theme change), so the run watches the landing through waitForStore and then
// waits for the dial to answer again.
//
// S6's change from S5 here is only where the bytes come from: the first .orbtheme the
// resolved themes/ directory holds. There is no built-in theme any more, and that is the
// point — a hub whose library has no theme says so instead of quietly sending the one a
// build embedded.
func (s *Server) installTheme(ctx context.Context, ip, slug string, send func(installStep)) error {
	// The card walk gets the page's ride, not a bare 8 s control dial: a board that
	// has just rebuilt an app re-walks a cold card, and an orb short on internal RAM
	// answered /api/v1/sd in 16 s idle (bench, 2026-10-31). And an unanswered list
	// is not a failed run. This step only SEEDS a face onto a card that has none;
	// the apps this run came for have already landed, and refusing the whole run
	// over the seed's own gate printed "update failed" over a clock that updated.
	// Unread, the card might be the owner's shelf, so nothing is written; the join
	// flow's verify still catches a board left with no theme (seedverify.go).
	tl, err := s.themeListRidingOutLane(ip)
	if err != nil {
		send(installStep{Step: "theme", Action: "unchecked",
			Note: "the orb did not answer its theme list, so no theme was seeded: " + err.Error()})
		return nil
	}
	// An upload or a store in flight owns the card, and the orb answers the list
	// without walking it: card_count reads 0 for a card that may be full. That is
	// "not read", never "empty" — seeding on it would push a theme over the owner's.
	if tl.Uploading || tl.JobBusy {
		send(installStep{Step: "theme", Action: "unchecked",
			Note: "the orb's card is busy with a theme job, so no theme was seeded"})
		return nil
	}
	// The seed's question is "does the CARD carry themes", and card_count is the
	// firmware's own answer. It is NOT `active`: a fresh board boots the factory
	// bundle baked into its assets slot, so active reads "Aviator" before the card
	// holds a single file, and a gate on active skipped the seed on exactly the
	// board the seed exists for (bench, owner, 2026-10-29: first flash drew the
	// Aviator, not the skeleton). An owner's card copy is the fact that stands;
	// the factory slot is not a choice anybody made.
	// Per app since the theme pool: each app wears its own theme, so the question is
	// whether the card carries one of THIS app's (a row with no app is an old clock row).
	// A count with no rows is a card the answer did not list: the owner's, left alone.
	if len(tl.Themes) == 0 && tl.CardCount > 0 {
		send(installStep{Step: "theme", Action: "skipped", Note: "the card already carries themes"})
		return nil
	}
	for _, t := range tl.Themes {
		if app := t.App; app == slug || (app == "" && slug == "clock") {
			send(installStep{Step: "theme", Action: "skipped", Note: "the card already carries themes for " + slug})
			return nil
		}
	}
	lib := s.library()
	name, blob, err := lib.appTheme(slug)
	if err != nil {
		return err
	}
	c, err := orbnet.ContainerName(blob)
	if err != nil {
		return fmt.Errorf("%s does not parse: %w", name, err)
	}
	// The theme is the largest single file in the library, so it gets the same live byte
	// count the app does rather than the older silent stretch.
	tTotal, tLast := len(blob), 0
	progress := func(sent int64) {
		pct := int(sent * 100 / int64(tTotal))
		if pct-tLast < 10 && int(sent) != tTotal {
			return
		}
		tLast = pct
		send(installStep{Step: "theme", Action: "uploading",
			Note: fmt.Sprintf("%d of %d KB", sent/1024, int64(tTotal)/1024)})
	}
	doUp := func() error { _, e := s.ota.ThemeUploadProgress(ip, blob, false, progress); return e }
	terr := doUp()
	// No reboot-retry here, and none is coming back. The door used to speak one
	// sentence for two failures — upload_begin resolved the themes dir through the
	// CLOCK app's boot-only file-source registry, so a clock this run installed and
	// enabled answered NOT_FOUND as "no card in the socket" over a mounted card
	// (bench, Bronze, 2026-10-29) — and this step paid a full reboot to disarm the
	// lie. Firmware v1.1.33 registers that source on the theme boot rung and gives
	// the refusal its own sentence, so a 503 here is a card genuinely absent, and
	// the owner's ruling retires the mitigation with it: no pre-1.1.33 board stays
	// in the fleet, so nothing left on this path needs a boot the enable never got.
	// A real no-card failure is the honest error below, untouched.
	if terr != nil {
		return fmt.Errorf("writing %s to the card: %w", name, plainDeviceErr(terr, 0))
	}
	// The card gained a file, so everything the hub remembers about it is retired —
	// not just the ID photos. A push ends at the device's own upload door, which never
	// comes back as the board you asked about, and a remembered card list answers
	// /api/theme/list for two minutes without dialing. That is the stale answer this
	// face would otherwise read as "the install did nothing", and it is the one hole
	// the delete door never had because it drops the whole memory (theme.go's
	// h_theme_delete). Same law, same verb; dropPreviews alone left the list standing.
	s.dropThemeCache(ip)
	send(installStep{Step: "theme", Action: "uploaded-theme"})
	// The upload door files the bytes and queues the store itself (h_theme_upload's
	// post_bundle tail) — a second ThemeStore press here would be a second copy for the
	// same promise, so the wait starts at the door's ack where handleThemeUpload's does.
	send(installStep{Step: "theme", Action: "storing"})
	outcome, note := s.waitForStoreStaged(ctx, ip, c.ID, storeStages(send, "theme"),
		beatFor(send, "theme", "waiting for the orb to store the theme"))
	if outcome != "stored" {
		return fmt.Errorf("the store did not land: %s", note)
	}
	send(installStep{Step: "theme", Action: "stored"})
	send(installStep{Step: "theme", Action: "rebooting"})
	live, back := s.installWaitBack(ip, beatFor(send, "theme", "waiting for it to come back wearing the new face"))
	if !back {
		return fmt.Errorf("the dial stored the theme but has not answered again within %s", installRebootWait)
	}
	ip = live
	// Back on the network and wearing the new face — and the reads the reboot's own
	// gap took could have refilled the memory with the card as it was BEFORE the
	// store. Retire it once more so the first read this run is followed by a dial
	// rather than an echo.
	s.dropThemeCache(ip)
	return nil
}

// h_libraryPush answers POST /api/library/push?kind=app|theme&name=<stem>&ip=. D38's
// card-side door: it moves ONE shelf file onto the orb's card and stops there for apps —
// no slot write, no reboot, no enable word — because "add to card" is a filing press, and
// filing is not installing. A theme has no such half-step: the device's own theme upload
// door queues the store the moment the file verifies (h_theme_upload's post_bundle tail),
// so pushing a shelf theme IS installing it, and the Themes face words the button that way.
//
// The response is the SAME NDJSON step-stream the install ladder sends (step/action/note),
// not one JSON line at the end: a push crosses the room and takes seconds, and the owner
// who pressed it is owed a moving bar and a plain verdict exactly as much as an installer
// is. It streams `uploading` with the running byte count, then `done`, or `refused` with
// the hub's own reason. The face's streamSteps reader drives the progress panel from these.
func (s *Server) h_libraryPush(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	q := r.URL.Query()
	ip := strings.TrimSpace(q.Get("ip"))
	kind := strings.TrimSpace(q.Get("kind"))
	name := strings.TrimSpace(q.Get("name"))
	if _, ok := s.checkedTarget(w, ip, "", false); !ok {
		return
	}
	if kind != "app" && kind != "theme" {
		s.jsonErr(w, "kind must be app or theme, not \""+kind+"\"")
		return
	}
	w.Header().Set("Content-Type", "application/x-ndjson")
	flusher, _ := w.(http.Flusher)
	send := func(st installStep) {
		b, _ := json.Marshal(st)
		w.Write(append(b, "\n"...))
		if flusher != nil {
			flusher.Flush()
		}
	}
	lib := s.library()
	// The push's plan: one byte transfer for an app, and for a theme the store and the
	// reboot the device's upload door sets off behind it. Each line names its stage by
	// its action, the way an install run's do.
	if kind == "theme" {
		send = themeRunSend(send, name)
	} else {
		send(installStep{Step: kind, Action: "plan", Phase: "send",
			Plan: []planStep{{ID: "send", Label: "Copying " + name + " to the orb's SD card", Kind: "bytes"}}})
		raw := send
		send = func(st installStep) { st.Phase = "send"; raw(st) }
	}
	release, rerr := s.holdDevicePort(r.Context(), ip, deviceTimeout)
	if rerr != nil {
		send(installStep{Step: kind, Action: "refused", Note: rerr.Error()})
		return
	}
	defer release()
	if kind == "app" {
		blob, err := lib.Container(name)
		if err != nil {
			send(installStep{Step: name, Action: "refused", Note: err.Error()})
			return
		}
		if len(blob) == 0 {
			send(installStep{Step: name, Action: "refused",
				Note: fmt.Sprintf("%s.orbapp is not in the library at %s", name, lib.AppsDir)})
			return
		}
		total := len(blob)
		last, sentLast := 0, int64(0)
		_, uerr := s.ota.AppUploadProgress(ip, blob, true, func(sent int64) {
			sentLast = sent
			pct := int(sent * 100 / int64(total))
			if pct-last < 10 && int(sent) != total {
				return
			}
			last = pct
			send(installStep{Step: name, Action: "uploading",
				Note: fmt.Sprintf("%d of %d KB", sent/1024, int64(total)/1024)})
		})
		if uerr != nil {
			send(installStep{Step: name, Action: "refused",
				Note: fmt.Sprintf("writing %s to the card: %s", name, plainDeviceErr(uerr, int(sentLast)))})
			return
		}
		// A file on the card is a changed card, whatever kind it was: the Themes
		// face's chooser counts themes per app from the card enumeration, and the
		// Apps face lists what the card holds. Both would ride this two-minute echo.
		s.dropThemeCache(ip)
		send(installStep{Step: name, Action: "done", Note: "file on the card; nothing installed"})
		return
	}
	body, err := lib.namedTheme(name)
	if err != nil {
		send(installStep{Step: name, Action: "refused", Note: err.Error()})
		return
	}
	c, err := orbnet.ContainerName(body.blob)
	if err != nil {
		send(installStep{Step: name, Action: "refused",
			Note: fmt.Sprintf("%s does not parse: %v", body.name, err)})
		return
	}
	// overwrite=1 is the Themes face's Update: the card holds an older bake under this
	// name, and the shelf's copy replaces it (the device refuses a same-name file without it)
	s.streamThemeStore(r.Context(), ip, body.blob, body.name, c, r.URL.Query().Get("overwrite") == "1", send)
}

// themeRunSend opens a theme store's streamed run: it sends the plan (themeSteps, named for
// what is going on) and returns the send that stamps each later line with its row. The
// Themes face's push and the theme editor's send both run through it, so the two show the
// same five steps (owner, 2026-11-06).
func themeRunSend(send func(installStep), what string) func(installStep) {
	restart := int((cycleQuietGrace + installRebootWait) / time.Second)
	send(installStep{Step: "theme", Action: "plan", Phase: "theme-send", Plan: themeSteps(what, restart)})
	phase := "theme-send"
	return func(st installStep) {
		if ph := themePhase(st.Action); ph != "" {
			phase = ph
		}
		st.Phase = phase
		send(st)
	}
}

// streamThemeStore sends one theme container to the orb's card and follows the store the
// device's upload door queues, through the reboot, to the orb answering again, speaking
// each stage as a step. It answers whether the run said done. label names the file in a
// refusal; the caller holds the device port.
func (s *Server) streamThemeStore(ctx context.Context, ip string, blob []byte, label string, c *orbnet.ThemeContainer, overwrite bool, send func(installStep)) bool {
	total := len(blob)
	last, sentLast := 0, int64(0)
	_, uerr := s.ota.ThemeUploadProgress(ip, blob, overwrite, func(sent int64) {
		sentLast = sent
		pct := int(sent * 100 / int64(total))
		if pct-last < 10 && int(sent) != total {
			return
		}
		last = pct
		send(installStep{Step: c.ID, Action: "uploading",
			Note: fmt.Sprintf("%d of %d KB", sent/1024, int64(total)/1024)})
	})
	if uerr != nil {
		send(installStep{Step: c.ID, Action: "refused",
			Note: fmt.Sprintf("writing %s to the card: %s", label, plainDeviceErr(uerr, int(sentLast)))})
		return false
	}
	// Retire the whole card memory, not just the ID photos: the list the hub remembered
	// from before the push would answer /api/theme/list for two minutes and paint the
	// shelf row it already showed. h_theme_delete does the same for the same reason.
	s.dropThemeCache(ip)
	// The upload door queued the store itself (h_theme_upload's post_bundle tail): the
	// orb now copies the file from card to flash and reboots onto it. The push follows
	// that to the end, as the install run's theme step does, and holds the lead through
	// it. It used to answer `done` here, and the page then read the theme list while
	// the orb was mid-copy and mid-reboot, and timed out (owner report, 2026-11-03).
	send(installStep{Step: c.ID, Action: "storing", Note: "the orb is copying it from the card to its flash"})
	outcome, note, back := s.followStoreStaged(ctx, ip, c.ID, storeStages(send, c.ID), func() {
		send(installStep{Step: c.ID, Action: "stored"})
		send(installStep{Step: c.ID, Action: "rebooting", Note: "stored; the orb reboots onto it"})
	}, beatFor(send, c.ID, "waiting for the orb to store the theme and come back"))
	if outcome != "stored" {
		send(installStep{Step: c.ID, Action: "refused", Note: "the store did not land: " + note})
		return false
	}
	if !back {
		send(installStep{Step: c.ID, Action: "refused",
			Note: fmt.Sprintf("the orb stored the theme but has not answered again within %s", installRebootWait)})
		return false
	}
	send(installStep{Step: c.ID, Action: "done", Note: "stored, and the orb is back drawing it"})
	return true
}

// beatFor turns one silent round of a patient wait into a step the screen can show. The
// operator report of 2026-10-29 PM was a provisioning window that showed one frozen sentence
// "for a really long time" while the hub sat inside a legitimate multi-minute wait; the fix is
// to say the wait is alive and how far it has gone, on the step the run is already on. It
// claims the WAIT, never the board: only a real answer earns a coming-back step.
func beatFor(send func(installStep), step, doing string) waitBeat {
	return func(elapsed, budget time.Duration, lastErr error) {
		note := fmt.Sprintf("%s — still waiting for the orb: %ds of %ds",
			doing, int(elapsed/time.Second), int(budget/time.Second))
		// A probe that misses inside a wait is the wait working, not a failure: the orb is
		// rebooting or busy moving flash. Quoting the miss as plainDeviceErr's sentence told
		// an owner the upload had failed while the store was making room (2026-11-06), so
		// the panel hears that silence is expected and the raw error goes to the hub's log.
		if lastErr != nil {
			note += " (it does not answer while it works; that is expected)"
			log.Printf("orb-manager: %s: %s", doing, lastErr.Error())
		}
		send(installStep{Step: step, Action: "waiting", Note: note})
	}
}

// installWaitBack is the post-reboot wait the theme step uses at both of its
// reboots: the cycle-shaped one, because a board answers its last request for a
// beat while it is dying.
func (s *Server) installWaitBack(ip string, beats ...waitBeat) (string, bool) {
	return s.waitOrbCycle(context.Background(), ip, installRebootWait, beats...)
}

// waitOrbCycle is waitOrbBack for the waits that sit directly behind a reboot the
// ladder itself caused. The firmware answers the reboot door — and the beat of
// traffic that follows it — while it is dying, so a wait that probes first reads
// that corpse as the return and hands the next dial to a board mid-reboot (bench,
// 2026-10-29 PM: the theme door's retry ate a second "no card" from a shutting-down
// board this way). The cycle is proved by SILENCE first: up to the quiet grace of
// it, and only the answer that follows a silence counts as the return. A board that
// never goes quiet inside the grace is a board whose reboot did not take, and the
// wait hands it back answering so the caller reports what it actually finds.
// waitBeat is one spoken round of a patient wait: how long it has been missing the board, the
// whole budget it has, and the error its last probe took. A caller that streams steps hands one
// in so a multi-minute wait never shows an operator a frozen screen.
type waitBeat func(elapsed, budget time.Duration, lastErr error)

// waitBeatEvery is how often a missing board gets reported. It is long enough that a beat is
// never the only thing on a screen and short enough that no operator ever watches ten blank
// seconds go by (owner report, 2026-10-29 PM: the provisioning window "shows nothing for a
// really long time").
const waitBeatEvery = 20 * time.Second

// cycleQuietGrace bounds the silence a caused reboot must show before an answer counts
// as the return. A variable so the tests that pin the corpse race do not spend 20 s on
// the clock a faked board cannot beat.
var cycleQuietGrace = 20 * time.Second

func (s *Server) waitOrbCycle(ctx context.Context, ip string, budget time.Duration, beats ...waitBeat) (string, bool) {
	quiet := time.NewTimer(cycleQuietGrace)
	defer quiet.Stop()
	start := time.Now()
	for alive := true; alive; {
		select {
		case <-ctx.Done():
			return ip, false
		case <-quiet.C:
			alive = false // the reboot never took; the board is stable and answering
		default:
		}
		if _, err := s.orb.Status(ip); err != nil {
			alive = false // gone: now the honest wait begins
			break
		}
		// The silence demand is itself up to 20 s of staring at a board that answers while
		// it dies, so it speaks on the same beat as the wait behind it.
		if elapsed := time.Since(start); len(beats) > 0 && elapsed >= waitBeatEvery {
			for _, beat := range beats {
				beat(elapsed, budget, nil)
			}
		}
		select {
		case <-ctx.Done():
			return ip, false
		case <-time.After(time.Second):
		}
	}
	return s.waitOrbBack(ctx, ip, budget, beats...)
}

// dialLive answers where the chip that lives at this address lives NOW: the MAC the
// grant or the fleet row carries for it, then the fleet's live answer for that chip.
// "" means no fresher answer than the address the caller already holds.
func (s *Server) dialLive(ip string) string {
	mac := s.macFor(ip)
	if mac == "" {
		return ""
	}
	return s.liveIPforMAC(mac)
}

// waitOrbBack waits a self-inflicted reboot out on the CHIP rather than the address.
// Every miss re-asks the fleet where the chip lives, so a board that takes a fresh
// lease through its own boot is found at the new number instead of died on at the
// old one (owner report, 2026-10-29 PM: the join-time seed bailed on "i/o timeout"
// against the pre-reboot address while the board lived happily under a new lease).
// The self-reboot excuse rides every probe so the cable sweep reads the silence as
// ours and stays off the port. The short status client carries the probes; the
// returned address is where the board answered, and every caller that dials again
// must dial THAT one. A ctx end answers false so a departed browser never strands a
// claim; budget exhaustion answers false too, and the caller refuses with it.
func (s *Server) waitOrbBack(ctx context.Context, ip string, budget time.Duration, beats ...waitBeat) (string, bool) {
	start := time.Now()
	deadline := start.Add(budget)
	nextBeat := waitBeatEvery
	mac := s.macFor(ip)
	for {
		if mac == "" {
			mac = s.macFor(ip)
		}
		if mac != "" {
			if live := s.liveIPforMAC(mac); live != "" && live != ip {
				ip = live
			}
			s.expectSelfReboot(mac)
		} else {
			s.armSelfRebootFor(ip)
		}
		_, err := s.orb.Status(ip)
		if err == nil {
			return ip, true
		}
		// The wait speaks. Three minutes of nothing reads to an operator as a hang — the
		// owner report of 2026-10-29 PM was exactly this window, "zero visible feedback for
		// a really long time" — so every ~20 s of miss reports the elapsed, the budget and
		// the error the last probe took. It claims only that the wait is alive; it never
		// claims the board is back.
		if elapsed := time.Since(start); len(beats) > 0 && elapsed >= nextBeat {
			for _, beat := range beats {
				beat(elapsed, budget, err)
			}
			for nextBeat <= elapsed {
				nextBeat += waitBeatEvery
			}
		}
		if !time.Now().Before(deadline) {
			return ip, false
		}
		select {
		case <-ctx.Done():
			return ip, false
		case <-time.After(2 * time.Second):
		}
	}
}

// deviceSentence unwraps an orbnet.DeviceError to the firmware's own words, so a refusal
// streamed here is the sentence the door gave and not Go's.
func deviceSentence(err error) error {
	if de, ok := err.(*orbnet.DeviceError); ok {
		var env struct {
			Error  string `json:"error"`
			Detail string `json:"detail"`
		}
		if json.Unmarshal(de.Body, &env) == nil && env.Error != "" {
			msg := env.Error
			if env.Detail != "" {
				msg += ": " + env.Detail
			}
			return fmt.Errorf("%s", msg)
		}
	}
	return err
}

// plainDeviceErr words a failed device call for a screen instead of a log. deviceSentence
// handles the case where the orb answered and said something; this handles the case where
// it answered with nothing, which is where the Go error text used to reach the operator
// whole — "Post \"http://192.168.2.32:80/api/v1/sd/app?overwrite=1\": context deadline
// exceeded (Client.Timeout exceeded while awaiting headers)" names a transport, a URL and
// an internal error value, and tells a person nothing about the board.
//
// A timeout on an upload is the load-bearing case, and the sentence says what actually
// happened rather than implying failure: the bytes crossed the room slowly, and the card
// write may well have finished after the hub stopped waiting. The honest instruction is to
// read the ledger before re-pressing, because installOneApp asks the ledger first and a
// re-press costs one read when the file did land.
//
// sent is the byte count the transport had written when the wait ended; pass 0 where the
// call carried no progress.
func plainDeviceErr(err error, sent int) error {
	s := err.Error()
	switch {
	case strings.Contains(s, "deadline exceeded"), strings.Contains(s, "timeout awaiting"):
		if sent > 0 {
			return fmt.Errorf("the orb stopped answering after %d KB of the upload — a card write "+
				"this size can outrun the hub's wait and often finishes anyway; check the app list "+
				"before pressing Install again", sent/1024)
		}
		return fmt.Errorf("the orb stopped answering mid-request; it is probably busy writing the card — " +
			"check what it lists before pressing Install again")
	case strings.Contains(s, "connection refused"):
		return fmt.Errorf("the orb refused the connection — it reboots after a card write, so it may be coming back up")
	case strings.Contains(s, "no such host"), strings.Contains(s, "unreachable"):
		return fmt.Errorf("the orb is not answering on the network any more")
	}
	return deviceSentence(err)
}
