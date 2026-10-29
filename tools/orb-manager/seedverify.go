package main

// Two questions this file answers, both of them about the end of a provisioning run.
//
// The first is whether a clock the hub just reported as installed is actually on the glass.
// The install ladder reports `installed` off the ledger row and `done` off the row plus the
// theme step, and a row is a promise rather than the bytes behind it: installOneApp says so at
// its own skip gate, and the bench showed the failure twice on 2026-10-29 — once where the hub
// printed installed with no byte having crossed, and once where the operator's card held
// nothing at all while the Apps face still offered Uninstall. `done` was therefore not evidence.
// verifySeed reads the two facts that would have caught both: the ledger row's own enable and
// failed bits, whether a card-tier row still has its card copy, and the card's theme count.
//
// The second is where a chip is answering right now. A provisioning run reboots the board at
// the theme store (D32), and a DHCP renew inside that window moves the address the join dialog
// has been holding since the handshake. The MAC is the only stable handle, and the fleet is the
// only thing that has seen the board since, so the page asks rather than guessing.

import (
	"context"
	"net/http"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbmdns"
)

// seedFacts is what verifySeed found, in the words the refusal uses. It is a struct and not an
// error so the caller can log the whole picture and still send one sentence.
type seedFacts struct {
	RowFound  bool
	Enabled   bool
	Failed    bool
	OnGlass   bool
	CardBytes bool
	CardCount int
	Mounted   bool
}

// verifySeed checks the seeded clock against the device's own answers. It is called only after
// a run that reported success, and only on the automatic arm, where nothing stood between the
// hub and a false green.
func (s *Server) verifySeed(ctx context.Context, ip string) (seedFacts, error) {
	var f seedFacts
	list, err := s.device.AppsList(ip)
	if err != nil {
		return f, err
	}
	f.Mounted = list.Mounted
	f.CardCount = s.cardThemeCount(ctx, ip)
	for _, row := range list.Installed {
		if row.Slug != "clock" {
			continue
		}
		f.RowFound = true
		f.Enabled = row.Enabled
		f.Failed = row.Failed
		f.OnGlass = row.OnGlass
		f.CardBytes = !needsCardBytes(row) || cardHolds(list, "clock")
		break
	}
	switch {
	case !f.RowFound:
		return f, errSeed("the ledger answers no clock row, so nothing reached the dial")
	case f.Failed:
		return f, errSeed("the dial refused the clock container it has")
	case !f.Enabled:
		return f, errSeed("the clock row stands switched off, so the glass draws no face")
	case !f.CardBytes:
		return f, errSeed("the clock row stands but its card copy is gone")
	case f.CardCount == 0:
		return f, errSeed("the card holds no theme, so the clock has no face to wear")
	}
	return f, nil
}

// cardThemeCount asks the device how many themes its card carries. A read that fails counts as
// zero: verifySeed is a gate on a claim of success, and an unreadable card does not support one.
func (s *Server) cardThemeCount(ctx context.Context, ip string) int {
	tl, err := s.device.ThemeList(ip)
	if err != nil {
		return 0
	}
	return tl.CardCount
}

// errSeed is a verification refusal, named so h_libraryInstall can tell its own "the baseline
// did not land" from a device refusal and retry the whole adoption instead.
type seedError struct{ msg string }

func (e *seedError) Error() string { return e.msg }

func errSeed(format string) error { return &seedError{msg: format} }

// verifySeedRun wraps verifySeed with the step stream the run speaks, so a seed that verified
// shows the operator "verified" and one that did not shows the sentence the caller refuses with.
// It reads the ledger once more after installRun's theme reboot, which is the whole point: the
// claims made during the ladder are before the last reboot, and this is after it.
func (s *Server) verifySeedRun(ctx context.Context, ip string, send func(installStep)) error {
	f, err := s.verifySeed(ctx, ip)
	if err != nil {
		send(installStep{Step: "verify", Action: "refused", Note: err.Error()})
		return err
	}
	send(installStep{Step: "verify", Action: "verified",
		Note: seedNote(f)})
	return nil
}

// verifySeededGlass is the seeding arm's gate: the five facts verifySeed checks, and on
// top of them the one fact seedNote used to wave away. A clock the hub just seeded must be
// the face the dial is showing, not merely eligible to be — an enabled, card-backed,
// themed row behind a menu is a half-set-up board wearing a green light (owner ruling,
// 2026-10-29). The caller parks the glass before it asks, so a refusal here means the dial
// was told and did not comply, which is the only version of this sentence an operator can
// act on. verifySeedRun stays the SKIP arm's gate: there the shelf is the owner's and so
// is the face, and nothing this run does gets to move it.
func (s *Server) verifySeededGlass(ctx context.Context, ip string, send func(installStep)) error {
	f, err := s.verifySeed(ctx, ip)
	if err != nil {
		send(installStep{Step: "verify", Action: "refused", Note: err.Error()})
		return err
	}
	if !f.OnGlass {
		why := "the clock is enabled, card-backed and themed, and the dial is not showing it — flick the knob onto the clock, or press Seed to make the hub park it again"
		send(installStep{Step: "verify", Action: "refused", Note: why})
		return errSeed(why)
	}
	send(installStep{Step: "verify", Action: "verified", Note: seedNote(f)})
	return nil
}

// seedNote words the verified step from the facts. The off-glass branch is the tolerated
// shape: the hub read five true facts, could not move the glass, and did not try. It reaches
// the operator two ways — the owner-shelf skip, where the face is the owner's own, and a
// seeding run on a dial behind faceMinFW, where parkGlass said the door is missing — so it
// claims neither cause and names the only lever left. verifySeededGlass never reaches this
// branch: the seeding arm on a dial with a door refuses instead.
//
// The refusal sentence carries that same lever. An owner report of 2026-10-29 caught the
// hub refusing a seed its own park had just landed (the corpse race below): the run said
// "not showing it — open the orb to retry" while the fix was one knob flick, and a verdict
// an operator cannot act on is an alarm, not a sentence. The flick is named even though the
// retry button sits next to it, because the flick finishes the orb either way and the retry
// only re-asks a dial that may already be compliant.
func seedNote(f seedFacts) string {
	if f.OnGlass {
		return "clock on the glass, theme on the card"
	}
	return "clock enabled and on the card; the glass shows something else, and only the knob moves it from here"
}

// isSeedFailure names the refusal class that must leave the adoption mark standing.
func isSeedFailure(err error) bool {
	_, ok := err.(*seedError)
	return ok
}

// h_orbWhere answers GET /api/orb/where?mac=<mac> with the address that chip is live at.
//
// It exists because every caller that held an IP from a handshake holds a lease, and because
// the page's own fleet scan is the slowest possible way to notice the lease moved. The fleet is
// the hub's live answer; the state file is the fallback for a board that has not announced to
// THIS run. A MAC the hub has never met is a 404 with a sentence, not an empty IP: the caller
// has to be able to tell "this chip is not on the LAN" from "the hub lost the row".
func (s *Server) h_orbWhere(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "GET only", http.StatusMethodNotAllowed)
		return
	}
	want, why := orbmdns.CanonicalMAC(r.URL.Query().Get("mac"))
	if want == "" {
		// A join over the wired door can finish holding an address and no MAC, and
		// the page's only handle is then the fleet itself: asked by ip, the route
		// answers the chip that lives at that address — the same reverse lookup
		// cleanSlate consults before it wipes. ClaimedBy answers live rows only, so
		// a stale address refuses rather than pointing at the wrong board.
		if at := r.URL.Query().Get("ip"); at != "" && s.fleet != nil {
			if rows := s.fleet.ClaimedBy(at); len(rows) == 1 && rows[0].MAC != "" {
				want = rows[0].MAC
			}
		}
	}
	if want == "" {
		s.jsonErr(w, "no usable mac: "+why)
		return
	}
	if ip := s.liveIPforMAC(want); ip != "" {
		s.jsonOK(w, map[string]any{"ip": ip, "mac": want})
		return
	}
	s.jsonErr(w, "the hub has no live address for "+want+" yet — the board has not checked in since it rebooted")
}

// liveIPforMAC resolves a canonical MAC to the address the board answers at right now. Fleet
// first — ByMAC is the store's live answer — then the every-row tier for a board that has not
// refreshed its liveness bit this instant, then the grants map as the last memory of a chip this
// hub committed to dial. It returns "" for a chip the hub has no current address for.
func (s *Server) liveIPforMAC(mac string) string {
	want, _ := orbmdns.CanonicalMAC(mac)
	if want == "" {
		return ""
	}
	if s.fleet != nil {
		if o, ok := s.fleet.ByMAC(want); ok && o.IP != "" {
			return o.IP
		}
		for _, o := range s.fleet.Every() {
			if c, _ := orbmdns.CanonicalMAC(o.MAC); c == want && o.IP != "" {
				return o.IP
			}
		}
	}
	if s.grants != nil {
		for _, ip := range s.grants.list() {
			if c, _ := orbmdns.CanonicalMAC(s.grants.bound(ip)); c == want && ip != "" {
				return ip
			}
		}
	}
	return ""
}
