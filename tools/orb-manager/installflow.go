package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
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
const installMinFW = "v1.1.28"

// The install poll's leash, unchanged from S5's seed for its documented reason: the door
// acks the QUEUE and never the landing, so the seed watches the ledger the way the Apps
// face does. themeStoreBudget's posture applies — a device still saying busy outruns the
// soft deadline once and dies at the hard stop, rather than at a timeout that would
// re-issue a job the worker already holds. Vars, so a test can run the ladder on a clock
// a copy cannot hide behind.
var (
	installWait       = 45 * time.Second
	installPoll       = 1000 * time.Millisecond
	installHard       = 4 * time.Minute
	installRebootWait = 75 * time.Second
)

// installStep is one streamed line of an install run, NDJSON the way /api/flash and
// /api/provision stream theirs — an install crosses WiFi and takes minutes, and a silent
// spinner over that is indistinguishable from a hang.
type installStep struct {
	Step   string `json:"step"`   // a slug, "theme", or "install" for the run itself
	Action string `json:"action"` // uploaded, installing, installed, enabled, uploaded-theme, storing, stored, rebooting, skipped, done, refused
	Note   string `json:"note,omitempty"`
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
		s.jsonErr(w, "the automatic install is off (-no-autoinstall); press Install on the Library card")
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
	// A board this hub just adopted arrives wearing the shipped baseline and nothing
	// else, so what it carries is erased before the baseline goes on. Only the automatic
	// arm wipes: a hand press means an operator is holding the shelf, and an operator who
	// pressed Install on their own orb never asked for a wipe (see cleanslate.go).
	slateMAC := ""
	if automatic {
		slateMAC = s.slateForIP(ip)
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
		if verr := s.verifySeedRun(ctx, ip, send); verr != nil {
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
	st, err := s.device.Status(ip)
	if err != nil {
		return fmt.Errorf("the orb did not answer its status: %w", err)
	}
	if v, ok := firmware.ParseVersion(st.FW); !ok {
		return fmt.Errorf("the dial reports %q, which is not a versioned firmware: flash it to %s first", st.FW, installMinFW)
	} else if v.Compare(installMinVersion()) < 0 {
		return fmt.Errorf("the dial runs %s and the library holds FORMAT 3 containers it will refuse: flash %s or newer first",
			st.FW, installMinFW)
	}
	listing, err := s.device.AppsList(ip)
	if err != nil {
		return fmt.Errorf("the orb did not answer its app ledger: %w", err)
	}
	if !listing.Mounted {
		return fmt.Errorf("no card in the socket — the install lands files on the card, so insert or mount one first")
	}

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
	for _, slug := range slugs {
		var blob []byte
		if !card {
			b, err := lib.Container(slug)
			if err != nil {
				return fmt.Errorf("reading %s from the library: %w", slug, err)
			}
			if len(b) == 0 {
				return fmt.Errorf("%s is not in the library at %s", slug, lib.AppsDir)
			}
			blob = b
		}
		if err := s.installOneApp(ctx, ip, slug, blob, force && only != "", send); err != nil {
			return err
		}
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
	if (only == "" || only == "clock") && s.installClockLive(ip) {
		if err := s.installTheme(ctx, ip, send); err != nil {
			return err
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
			// store that died mid-write each leaves a row that enables, never reaches
			// the glass, and answers "installed" to a hub that skipped on the row
			// alone (bench, new board, 2026-10-29: clean flash then Wi-Fi setup, and
			// the hub printed installed while no byte had ever crossed). A built-in
			// row carries its code in the image and has no card copy to miss.
			if needsCardBytes(row) && !cardHolds(listing, slug) {
				send(installStep{Step: slug, Action: "installing",
					Note: "the row stands but the card copy is gone — sending the bytes"})
				break
			}
			send(installStep{Step: slug, Action: "skipped", Note: "already on the dial"})
			if !row.Enabled && !row.Failed {
				if _, e := s.device.AppEnable(ip, slug, true); e != nil {
					return fmt.Errorf("enabling %s: %w", slug, plainDeviceErr(e, 0))
				}
				send(installStep{Step: slug, Action: "enabled"})
			}
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
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(installPoll):
		}
		list, err := s.device.AppsList(ip)
		if err == nil {
			for _, row := range list.Installed {
				if row.Slug == slug {
					send(installStep{Step: slug, Action: "installed"})
					if _, e := s.device.AppEnable(ip, slug, true); e != nil {
						return fmt.Errorf("enabling %s: %w", slug, plainDeviceErr(e, 0))
					}
					send(installStep{Step: slug, Action: "enabled"})
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
			return fmt.Errorf("%s did not land within %s — the copy may still be running; press Seed again to check",
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
			return fmt.Errorf("clearing %s: %w", row.Slug, plainDeviceErr(e, 0))
		}
		send(installStep{Step: row.Slug, Action: "uninstalled", Note: "clean slate"})
	}
	// Both library trees, by the names the device files them under. A card that holds
	// neither answers an error, which the caller reads as nothing to erase: a brand-new
	// board lands here with an empty card and must not be refused for it.
	for _, tree := range []string{"apps", "themes"} {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		if _, e := s.device.SDRemove(ip, tree, true); e != nil {
			log.Printf("orb-manager: clean slate on %s left %s alone: %s", ip, tree, plainDeviceErr(e, 0))
			send(installStep{Step: tree, Action: "skipped", Note: "nothing to erase"})
			continue
		}
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
	if len(rows) != 1 {
		// Two boards on one address is a contention the fleet already reports, and the
		// hub will not wipe on behalf of an address it cannot attribute.
		return ""
	}
	return rows[0].MAC
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
		// A row is a promise; the card copy and the enable bit are the substance. A row
		// that stands switched off with no card behind it is leftover from a broken orb
		// life, not a shelf anybody curated — counting it as the owner's is how an
		// adoption skipped over a zero-file card while the page said installed (bench,
		// 2026-10-29, Bronze). The ladder's hollow-row gate refills exactly this row.
		if !r.Enabled && !cardHolds(list, r.Slug) {
			continue
		}
		return true, nil
	}
	return len(list.Apps) > 0, nil
}

// installClockLive asks the ledger the one question the theme gate needs, at the moment it
// needs it: is a clock row on the dial right now.
func (s *Server) installClockLive(ip string) bool {
	list, err := s.device.AppsList(ip)
	if err != nil {
		return false
	}
	for _, row := range list.Installed {
		if row.Slug == "clock" && !row.Failed {
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
func (s *Server) installTheme(ctx context.Context, ip string, send func(installStep)) error {
	tl, err := s.device.ThemeList(ip)
	if err != nil {
		return fmt.Errorf("the orb did not answer its theme list: %w", err)
	}
	// The seed's question is "does the CARD carry themes", and card_count is the
	// firmware's own answer. It is NOT `active`: a fresh board boots the factory
	// bundle baked into its assets slot, so active reads "Aviator" before the card
	// holds a single file, and a gate on active skipped the seed on exactly the
	// board the seed exists for (bench, owner, 2026-10-29: first flash drew the
	// Aviator, not the skeleton). An owner's card copy is the fact that stands;
	// the factory slot is not a choice anybody made.
	if tl.CardCount > 0 {
		send(installStep{Step: "theme", Action: "skipped", Note: "the card already carries themes"})
		return nil
	}
	lib := s.library()
	name, blob, err := lib.firstTheme()
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
	// The shipped door speaks one sentence for two failures. upload_begin resolves the
	// themes dir through the CLOCK app's file-source registry, which is only built at
	// boot — so a clock this run installed and enabled without a reboot answers
	// NOT_FOUND, and the door calls it "no card in the socket" with a 503 while
	// /api/v1/sd says mounted true (bench, Bronze, 2026-10-29: every join-time theme
	// died here; the empty themes folder the owner found on the card IS this bug).
	// When the card swears it is mounted, that refusal is the door, not the socket:
	// reboot, wait for the dial back, and hand it the upload one more time. A card
	// that is truly gone keeps the honest error untouched.
	if terr != nil && strings.Contains(terr.Error(), "no card in the socket") {
		if list, lerr := s.device.AppsList(ip); lerr == nil && list.Mounted {
			log.Printf("orb-manager: the theme door on %s cried no-card over a mounted card -- rebooting to arm it", ip)
			send(installStep{Step: "theme", Action: "rebooting",
				Note: "the upload door waits on a boot the fresh enable never got — rebooting for it"})
			if _, rerr := s.device.Reboot(ip); rerr == nil && s.installWaitBack(ip) {
				s.dropThemeCache(ip)
				terr = doUp()
			}
		}
	}
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
	outcome, note := s.waitForStore(ctx, ip, c.ID)
	if outcome != "stored" {
		return fmt.Errorf("the store did not land: %s", note)
	}
	send(installStep{Step: "theme", Action: "stored"})
	send(installStep{Step: "theme", Action: "rebooting"})
	if !s.installWaitBack(ip) {
		return fmt.Errorf("the dial stored the theme but has not answered again within %s", installRebootWait)
	}
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
	total := len(body.blob)
	last, sentLast := 0, int64(0)
	_, uerr := s.ota.ThemeUploadProgress(ip, body.blob, false, func(sent int64) {
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
			Note: fmt.Sprintf("writing %s to the card: %s", body.name, plainDeviceErr(uerr, int(sentLast)))})
		return
	}
	// The upload door queued the store itself and the board reboots to draw it, so the
	// door that took the file is gone by the time it lands. Report the promise here and
	// let the face follow the outcome the way the install run's theme step does.
	//
	// Following it honestly needs the whole card memory retired, not just the ID photos.
	// This door writes the card and leaves the store to the device, so the list the hub
	// remembered from before the push still answers /api/theme/list for two minutes and
	// the page's re-read after its reboot watch paints the shelf row it already showed:
	// the theme looks like it never installed, and with no card row it carries no ID
	// photo either. h_theme_delete retires the same memory for the same reason; a card
	// that gained a file is no different from one that lost it.
	s.dropThemeCache(ip)
	send(installStep{Step: c.ID, Action: "done", Note: "stored; the board reboots onto it"})
}

// installWaitBack polls the short status client until the orb answers again — the store's own
// reboot, waited out inside the port hold so no sweep races it.
func (s *Server) installWaitBack(ip string) bool {
	deadline := time.Now().Add(installRebootWait)
	for time.Now().Before(deadline) {
		select {
		case <-time.After(time.Second):
		}
		if _, err := s.orb.Status(ip); err == nil {
			return true
		}
	}
	return false
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
