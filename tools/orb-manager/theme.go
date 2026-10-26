package main

import (
	"context"
	"encoding/json"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The Theme card's server side. Every call here is the hub dialing ONE address the
// operator named or the USB console reported, through the orbnet theme module, which is
// the single place the device theme API is spoken.
//
// FORMAT 2 settled who names a theme, and FORMAT 3 inherits the rule unchanged: it is not
// the caller of these endpoints.
// The theme's identity is the name embedded in its container; the 12-hex id the orb
// lists beside it is that name's hash, and it is the only handle the device accepts on
// install and delete. So these handlers take an id where they used to take a name, and
// the upload takes no name at all: the name arrives inside the bytes, is read out of
// the header by orbnet.ContainerName, and the hub reports where the file will land
// rather than being told.
//
// These are deliberately NOT folded into /orb/: a theme action needs the address gate,
// the id gate, and one place to add the orb's own identity later, and each is clearer
// as a named endpoint than as another arm of a generic forwarder. The proxy keeps
// serving the read-only status and config calls the Manage card already makes.

// themeTarget is the request every theme call takes: the orb to dial, and for the two
// calls that act on one theme, that theme's device-issued id. The address is checked
// against the learned set exactly as the proxy checks it, because a theme call reaches
// the device as hard as a forward does — harder, since install and delete write to the
// card.
type themeTarget struct {
	IP string `json:"ip"`
	ID string `json:"id,omitempty"`
}

// handleThemeList returns the card's rows plus the state needed to word them. Each row
// comes back with the name the owner baked and the id every later call needs.
func (s *Server) handleThemeList(w http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w, r, false)
	if !ok {
		return
	}
	list, err := s.device.ThemeList(t.IP)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.jsonOK(w, list)
}

// handleThemeInstall queues a Migration 1 store on the orb (POST /api/v1/sd/bundle).
// The store door answers as soon as the job is queued and acks reboot:automatic —
// under the single slot the store IS the theme change, so the reply is a promise of a
// reboot, never a claim that the glass already moved; the caller watches the row's
// stored witness and the reboot for the outcome. The route keeps its /api/theme/install
// name so the card's button needs no change; only the device verb it dials moved.
func (s *Server) handleThemeInstall(w http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w, r, true)
	if !ok {
		return
	}
	// The store is a queued job that acks a reboot it has not performed yet. Holding the
	// lead across the queue keeps the hub's own probe from racing the reboot the device
	// is about to do on its own, which is the one collision that would make the operator
	// blame the theme.
	//
	// The hold used to END at the ack, and the ack arrives before the copy does: the
	// door answers the moment the job is queued, and the worker then erases the slot and
	// writes up to 8 MB for tens of seconds afterward. The sweep re-claimed the port in
	// that window and pulsed DTR/RTS into a board mid-copy — live on orb-B on
	// 2026-10-22, where a 5.6 MB store tore at 47% with rst=ESP_RST_USB in its status
	// frame and the operator watched a frozen progress bar. Holding the lead through
	// waitForStore below is the fix; the queue was never the dangerous part.
	release, rerr := s.holdDevicePort(r.Context(), t.IP, deviceTimeout)
	if rerr != nil {
		s.jsonErr(w, rerr.Error())
		return
	}
	defer release()
	raw, err := s.device.ThemeStore(t.IP, t.ID)
	if err == nil {
		outcome, note := s.waitForStore(r.Context(), t.IP, t.ID)
		raw = withStoreOutcome(raw, outcome, note)
	}
	s.themeReply(w, raw, err)
}

// handleThemeDelete removes a theme from the card. It is the one theme call that is
// not a queued job, so its body is the outcome and passes through as it is.
func (s *Server) handleThemeDelete(w http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w, r, true)
	if !ok {
		return
	}
	// A write to the card like any other: a reset halfway leaves a directory the orb
	// re-reads differently, and the rescan is what says so.
	release, rerr := s.holdDevicePort(r.Context(), t.IP, deviceTimeout)
	if rerr != nil {
		s.jsonErr(w, rerr.Error())
		return
	}
	defer release()
	s.dropPreviews(t.IP)
	raw, err := s.device.ThemeDelete(t.IP, t.ID)
	s.themeReply(w, raw, err)
}

// handleThemeRescan asks the orb to re-read the card, which is how a theme that
// arrived by another route shows up in the list without rebooting the orb.
func (s *Server) handleThemeRescan(w http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w, r, false)
	if !ok {
		return
	}
	raw, err := s.device.ThemeRescan(t.IP)
	s.themeReply(w, raw, err)
}

// previewHit is one cached plane: the device's own bytes, the status it answered
// them with, and the moment the hub learned them.
type previewHit struct {
	body []byte
	code int
	at   time.Time
}

// previewCacheTTL bounds how long a cached plane may stand in for the card's.
// Ten minutes is "not this page paint, not this work session" — the route being
// cached is the one that measurably stalls a whole httpd task for ~10 s, so the
// cheap answer is to ask it once per card-and-theme, not once per repaint.
const previewCacheTTL = 10 * time.Minute

// dropPreviews forgets every cached plane for one address. A theme shipping,
// replacing, or leaving changes what the card would answer for an id this hub
// already drew, and a stale thumbnail is a lie the Manage card cannot see.
func (s *Server) dropPreviews(ip string) {
	s.previewMu.Lock()
	defer s.previewMu.Unlock()
	for k := range s.previews {
		if strings.HasPrefix(k, ip+"|") {
			delete(s.previews, k)
		}
	}
}

// handleThemePreview passes one theme's preview plane through as the bytes it is:
// [u16 w][u16 h][RGB565 LE]. No JSON envelope — a plane is not a reply, and wrapping
// it would base64-megabytes the answer the card unpacks by hand — and no re-cutting
// of the status: the device's quiet 404 for a theme that carries no preview arrives
// at the card as a quiet 404, because every caller's honest answer to that is to
// draw the text row it drew before planes existed. The gates are the theme gates,
// the same id grammar and the same learned address.
//
// The relay dials on the preview client, not the 8 s management one: the device
// answers this route after a card walk, a region read and a ~100 KB stream, which
// measured over the 8 s budget on the bench — every cut looked to the page like a
// missing plane and painted the quiet-black square this card used to argue about.
// 200s are cached per address+id (previewCacheTTL), and dropPreviews clears an
// address's rows whenever a theme is shipped or deleted, so the cache never outlives
// the event that makes it wrong for longer than it takes to notice.
func (s *Server) handleThemePreview(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	t, ok := s.checkedTarget(w, strings.TrimSpace(q.Get("ip")), strings.TrimSpace(q.Get("id")), true)
	if !ok {
		return
	}
	key := t.IP + "|" + t.ID
	s.previewMu.Lock()
	hit, fresh := s.previews[key], false
	if time.Since(hit.at) < previewCacheTTL {
		fresh = true
	}
	s.previewMu.Unlock()
	if !fresh || hit.code != http.StatusOK {
		body, code, err := s.preview.ThemePreview(t.IP, t.ID)
		if err != nil {
			// The route's failure is an event with a reason, not a missing plane:
			// the hub's own sentence says timeout, refusal, or gone, and the page
			// shows it instead of silently hiding the square.
			s.jsonErr(w, err.Error())
			return
		}
		hit = previewHit{body: body, code: code, at: time.Now()}
		if code == http.StatusOK {
			s.previewMu.Lock()
			s.previews[key] = hit
			s.previewMu.Unlock()
		}
	}
	if hit.code == http.StatusOK {
		w.Header().Set("Content-Type", "application/octet-stream")
	} else {
		w.Header().Set("Content-Type", "application/json")
	}
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(hit.code)
	_, _ = w.Write(hit.body)
}

// handleThemeStem answers the one question a non-secure browser context cannot
// answer for itself: the 12-hex identity of a theme name. The composer names its
// .orbtheme downloads with window.crypto.subtle, which browsers define only on
// https:// and http://localhost; a hub reached over a LAN IP (`http://10.x:3779`)
// has no subtle, and the download used to fall back to the raw title as the file
// name — a stem no orb lists under an app's prefix, which is how "uploaded from
// the hub" files end up invisible on the card. The hub holds the same rule as
// orbnet's own pair-hash and can answer for any origin it serves, so the fallback
// becomes a fetch instead of a rename-the-lie. This is a question, not a door:
// it writes nothing, dials nothing, and the device re-derives the same stem from
// the container header the moment the file is actually uploaded.
func (s *Server) handleThemeStem(w http.ResponseWriter, r *http.Request) {
	slug := strings.TrimSpace(r.URL.Query().Get("slug"))
	if slug == "" {
		slug = "clock"
	}
	name := r.URL.Query().Get("name")
	stem, err := orbnet.ThemeIDOf(slug, name)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.jsonOK(w, map[string]any{"stem": stem})
}

// handleThemeUpload takes the bytes of a .orbtheme the operator picked and pushes them
// to the orb. The file never touches the hub's disk: it is read under a cap, its own
// header is checked, and the bytes go straight on.
//
// The header check is the point. The firmware files an upload under the name INSIDE
// the container, so a name the hub accepted or displayed here would be decoration
// unless it were read from the file itself. Checking the container before the dial also
// means a FORMAT 1 file — the old slug+title pair, unreadable by any current orb — is
// refused with the reason in the sentence, instead of 2 MB crossing the room to reach a
// 422.
//
// ?overwrite=1 forwards the operator's one decision: the firmware answers 409 with
// {"overwrite":false} when a DIFFERENT theme already holds that name, the card asks, a
// nd the retry comes back with the flag set.
//
// ?precheck=1 asks the same question BEFORE any byte crosses the room: the hub reads
// the id and serial out of the container it already holds, consults the device's theme
// list, and answers duplicate-or-not for the cost of one GET. The device's duplicate
// policy (§5) is unchanged and still decided by upload_end — precheck is a question,
// not a verdict. It compares serials, not the full identity triple: same name and same
// serial rides the device's own idempotent path (a quiet no-op, nothing to ask); same
// name and a different serial is the collision. Same serial with different payload CRCs
// — the cut the serial alone does not capture — still reaches the device's late 409,
// and every caller keeps its inline replace prompt wired for exactly that case.
func (s *Server) handleThemeUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	ip := strings.TrimSpace(r.URL.Query().Get("ip"))
	t, ok := s.checkedTarget(w, ip, "", false)
	if !ok {
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, int64(orbnet.MaxThemeUpload)+1))
	if err != nil {
		s.jsonErr(w, "reading the upload: "+err.Error())
		return
	}
	if len(body) > orbnet.MaxThemeUpload {
		s.jsonErr(w, "that file is over the "+strconv.Itoa(orbnet.MaxThemeUpload)+" B an orb will accept")
		return
	}
	// The precheck body is a head slice, not the file: the strict parse would refuse it
	// for the art tail it never claimed to carry, and every first Send press would read
	// "could not get a clean read" off a perfectly good bake. The upload leg keeps the
	// strict door — it has the whole file in hand.
	var c *orbnet.ThemeContainer
	if r.URL.Query().Get("precheck") == "1" {
		c, err = orbnet.ContainerHead(body)
	} else {
		c, err = orbnet.ContainerName(body)
	}
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	overwrite := r.URL.Query().Get("overwrite") == "1"
	if r.URL.Query().Get("precheck") == "1" {
		verdict, note := s.themeDuplicate(ip, c)
		s.setSend(ip, func(st *themeSend) {
			st.Phase, st.ID, st.Total, st.Sent, st.Note = verdict, c.ID, int64(len(body)), 0, note
		})
		s.jsonOK(w, map[string]any{"duplicate": verdict == "duplicate", "exact": verdict == "exact",
			"note": note, "name": c.Name, "id": c.ID, "serial": c.Serial})
		return
	}
	// The claim goes in before the bytes move, not before the dial: this is the one
	// theme call whose failure the operator cannot see, because a reset mid-transfer
	// makes the firmware delete the .part and answer as if nothing had been sent.
	release, rerr := s.holdDevicePort(r.Context(), t.IP, otaTimeout)
	if rerr != nil {
		s.jsonErr(w, rerr.Error())
		return
	}
	defer release()
	// The upload rides the ota-tuned client, not the 8 s management client: the
	// device opens the card file and writes 4 KB chunks straight to FAT, and a
	// ~490 KB container took longer than one management call should ever take.
	// The 8 s budget killed the POST mid-body, the device deleted the .part, and
	// the operator watched a card that never changed (the theme-twin of h_ota's
	// ~2 MB reasoning above).
	s.setSend(ip, func(st *themeSend) {
		st.Phase, st.ID, st.Total, st.Sent, st.Note = "uploading", c.ID, int64(len(body)), 0, ""
	})
	// A theme shipping changes what the card would answer for its id's preview —
	// same id, new pixels — so this address's cached planes retire before the bytes
	// move, and the card re-reads the plane the device is about to hold.
	s.dropPreviews(t.IP)
	raw, err := s.ota.ThemeUploadProgress(ip, body, overwrite, func(n int64) {
		s.setSend(ip, func(st *themeSend) { st.Sent = n })
	})
	if de, ok := err.(*orbnet.DeviceError); ok && de.OverwriteConflict() {
		// The refusal now usually carries a one-shot commit token naming the copy the
		// device kept staged on its card. The page offers Replace as a zero-byte
		// commit instead of a second upload; an empty token is an older firmware that
		// deleted the scratch, and the page's only path stays the re-upload.
		s.setSend(ip, func(st *themeSend) {
			st.Phase, st.Note = "duplicate", "a different theme with that name is already on the card"
		})
		s.jsonErrFields(w, de.Error()+" — send it again with overwrite to replace it", map[string]any{
			"duplicate": true, "commit": de.CommitToken(),
		})
		return
	}
	if err != nil {
		s.setSend(ip, func(st *themeSend) { st.Phase, st.Note = "rejected", err.Error() })
		s.themeReply(w, nil, err)
		return
	}
	// The device's own reply, plus the identity the hub read off the file: the card
	// cannot name what it pushed without this, and the id is what the row will arrive
	// with after the re-read.
	var payload map[string]any
	if uerr := json.Unmarshal(raw, &payload); uerr != nil {
		payload = map[string]any{"deviceText": string(raw)}
	}
	payload["name"] = c.Name
	payload["id"] = c.ID
	// The reply above is the door acking the QUEUE, not the store: the copy the door
	// armed now runs unheld if this handler releases at the ack. It used to — which is
	// how the identity sweep's DTR/RTS pulse reached orb-B mid-write on 2026-10-22 and
	// tore a 5.6 MB store at 47% (its status frame owned the evidence as rst=11,
	// ESP_RST_USB). The claim stays held and the handler watches the job to its
	// outcome, so the response this route finally sends names what landed in the slot,
	// and the board cannot be reset by this hub's own probe while the copy runs.
	s.setSend(ip, func(st *themeSend) { st.Phase = "queued" })
	outcome, note := s.waitForStore(r.Context(), ip, c.ID)
	s.setSend(ip, func(st *themeSend) { st.Phase, st.Note = outcome, note })
	payload["store"] = outcome
	if note != "" {
		payload["store_note"] = note
	}
	s.jsonOK(w, payload)
}

// themeStoreBudget is the wait for one queued store to report an outcome while
// the device says it is NOT working: the pickup window before the worker takes
// the job, and the verdict window after its last busy word. A full-slot copy is
// minutes of erase and write at the device's chunk rate; the reboot and WiFi
// join after it run tens of seconds on a marginal board. The wait is a ceiling,
// not an expectation — an outcome returns on the poll that shows it.
//
// They are vars, not consts, so the tests can run the whole ladder on a clock a
// store cannot hide behind.
var (
	themeStoreBudget = 150 * time.Second

	// themeStorePoll is the list cadence while waiting. The device's httpd is a
	// single worker and the 8 s management client is the same one every card dial
	// uses; 1.5 s is the beat watchThemeJob already kept the device comfortable
	// with.
	themeStorePoll = 1500 * time.Millisecond

	// themeStoreBusyCeiling is the hard stop on a board that keeps answering
	// job_busy: true. The 23 Oct evening released a send at exactly
	// themeStoreBudget while the device was still testifying the copy was live —
	// a 5.5 MB store on a busy card outruns any fixed wait, and the release
	// re-admits the sweep to the port the copy still owns. A job the DEVICE
	// attests therefore outruns the idle budget, and only this ceiling ends the
	// wait: ten minutes is roughly ten times a healthy 5.5 MB store, and past
	// that a busy word names a wedge rather than a wait.
	themeStoreBusyCeiling = 10 * time.Minute
)

// handleThemeCommit spends a duplicate refusal's one-shot token on the operator's
// behalf: POST {ip, commit}, an empty crossing to the device that files the copy the
// refusal left staged and queues the store. It replaces the confirmed overwrite's
// second upload. The probe lead is held exactly as handleThemeInstall holds it — the
// commit queues the same store job, and a hub probe racing that reboot blames the
// theme. A 410 from the device (staged copy gone, stale, or corrupt) passes through
// untouched: the page then uploads the file for real, the only honest recovery.
func (s *Server) handleThemeCommit(w http.ResponseWriter, r *http.Request) {
	var q struct {
		IP     string `json:"ip"`
		Commit string `json:"commit"`
	}
	if err := json.NewDecoder(r.Body).Decode(&q); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return
	}
	t, ok := s.checkedTarget(w, q.IP, "", false)
	if !ok {
		return
	}
	if q.Commit == "" {
		s.jsonErr(w, "no commit token given — the token came with the duplicate refusal")
		return
	}
	release, rerr := s.holdDevicePort(r.Context(), t.IP, deviceTimeout)
	if rerr != nil {
		s.jsonErr(w, rerr.Error())
		return
	}
	defer release()
	raw, err := s.device.ThemeCommit(t.IP, q.Commit)
	s.themeReply(w, raw, err)
}

// waitForStore watches the copy a store door just armed — the same ladder app.js's
// watchThemeJob runs, held hub-side this time so the cable claim cannot be released
// and the sweep re-admitted while the erase is live. It reads only the four
// witnesses GET /api/v1/sd already publishes: job_busy (the worker holds the job),
// the row's stored flag (the copy landed), staged_id (pixels reached the slot and
// the header did not — the torn witness), and try_id/try_why (the press's own
// refusal). Silence is an answer too: the reboot the door promised IS silence, so
// misses are tolerated and the self-reboot excuse is re-armed while they run, which
// is what keeps the rest of the hub from reading the reboot as defection and paying
// for it with a probe. A job the device itself reports busy extends the wait up to
// the busy ceiling: the 23 Oct release at the idle budget, while the board kept
// answering job_busy:true through a 5.5 MB copy, is the failure this rule exists
// to forbid — the claim's whole point is the port until the copy is done. Returns
// stored, torn, refused, or lost; lost is the honest "no verdict by the deadline",
// never a claim of failure. The context is detached from the request on purpose:
// a browser tab closing must not release the claim while the board is mid-erase —
// the exact window this function exists to hold.
func (s *Server) waitForStore(ctx context.Context, ip, id string) (string, string) {
	ctx, cancel := context.WithCancel(context.WithoutCancel(ctx))
	defer cancel()
	s.armSelfRebootFor(ip)
	sawBusy, misses := false, 0
	deadline := time.Now().Add(themeStoreBudget)
	hardStop := time.Now().Add(themeStoreBusyCeiling)
	for {
		// Poll first, then wait: a store that has already landed — the usual case for
		// a double, and the case for a re-store of a theme the slot holds — returns on
		// this tick instead of after a poll interval spent sleeping.
		s.setSend(ip, func(st *themeSend) {
			if st.Phase == "queued" && (sawBusy || misses > 0) {
				st.Phase = "storing"
			}
		})
		list, err := s.device.ThemeList(ip)
		switch {
		case err != nil:
			misses++
			if misses == 3 {
				// The excuse window lapses mid-wait on a slow join; re-arm on each
				// fresh patch of silence so the sweep keeps declining this port.
				s.armSelfRebootFor(ip)
			}
		default:
			misses = 0
			if list.JobBusy {
				sawBusy = true
				// The board attests the copy is live: the idle budget stops
				// applying and only the busy ceiling may end the wait.
				deadline = time.Now().Add(themeStoreBudget)
				if deadline.After(hardStop) {
					deadline = hardStop
				}
				break
			}
			if list.StagedID != "" && list.StagedID == id {
				return "torn", firstNonEmptyStr(list.StagedWhy, "the copy reached the slot but its header did not — one re-store recovers it")
			}
			if list.TryID == id && list.TryWhy != "" {
				return "refused", list.TryWhy
			}
			for _, row := range list.Themes {
				if row.ID == id && row.Stored {
					// Landed. The job now asks for the reboot that draws it — a self-
					// inflicted silence the excuse has to cover before the next poll
					// finds the board gone.
					s.armSelfRebootFor(ip)
					return "stored", ""
				}
			}
			if sawBusy {
				return "lost", "the store job ended without landing, tearing, or a refusal sentence"
			}
			// Not picked up yet: a longer job sat in front of it on the worker. Keep
			// waiting; the budget is the answer for a press that never starts.
		}
		if !time.Now().Before(deadline) {
			if sawBusy && !time.Now().Before(hardStop) {
				return "lost", "the orb still reported the store live at the " +
					strconv.Itoa(int(themeStoreBusyCeiling/time.Second)) +
					" s busy ceiling — the claim held throughout; the copy may still be running"
			}
			if sawBusy {
				return "lost", "the orb stopped answering after a live store job — the commit's own reboot may have taken it"
			}
			return "lost", "no store witness within " + strconv.Itoa(int(themeStoreBudget/time.Second)) + " s — the copy may still be running"
		}
		// Sleep one poll beat, clipped at the deadline so the last wait lands
		// exactly on it; the top of the next turn reads the deadline again and
		// answers. Polling to the deadline, not to the beat, would make every
		// mid-wait verdict invisible until the budget expired.
		sleep := themeStorePoll
		if d := time.Until(deadline); d < sleep {
			sleep = d
		}
		select {
		case <-ctx.Done():
			return "lost", "the wait was cancelled while the store ran"
		case <-time.After(sleep):
		}
	}
}

// holdDevicePort claims the operation slot on the cable the orb at ip sits on, for the
// duration of a theme call, and returns the release. It is the reason an install stops
// dying halfway.
//
// The theme API is addressed by IP and the probe sweep is keyed by port, and the two
// never met before this. The identity sweep pulses DTR/RTS to read a bootloader, so the
// board resets, and a reset partway through an upload makes the firmware delete the
// .part file. Nothing was wrong with the file, nothing was wrong with the orb, and
// nothing was written down: the operator saw a card that never changed. Claiming the
// slot makes cableNeedsProbe and proveCable both decline that port until the transfer
// ends, which is the deferral the sweep already owes any other writer. The slot only
// binds THIS process's sweep, so the claim also takes the port's flock — the one guard
// that binds every process on the bench. A second hub instance tore a store on
// 23 Oct for exactly the reason the slot could not give it: the first hub's ledger
// said held, the second hub's probe checked only its own empty ledger and the port's
// flock, found that free, and paid the reset (docs/theme-delta-mmap-plan.md 32.4).
//
// It resolves the address the way the hub resolves every other one: the MAC the grant was
// committed for, falling back to the address book, then the MAC the USB sweep attributes
// to that chip. The MAC is canonicalized before the lookup, because the two halves of the
// hub spell it differently — the cable cache stores orbmdns.CanonicalMAC output, which has
// no separators at all, while a grant learned from the API or a fleet row can carry colons
// — and cablePortFor compares with plain ==. Handing it a colon form finds no port, the
// claim quietly does nothing, and the sweep resets the board mid-store: the exact failure
// this function exists to prevent, reintroduced through its own address lookup.
//
// Every way this can decline to claim is now announced. It used to return a no-op in
// silence for three of them, which meant a broken lookup and a genuinely cable-less orb
// looked identical in the log, and an operator watching a theme tear for the third time had
// nothing to read. Only the LAN-only case is routine; the other two are the lookup failing
// on a board that IS on a lead, which is precisely when the claim matters.
func (s *Server) holdDevicePort(ctx context.Context, ip string, budget time.Duration) (func(), error) {
	noop := func() {}
	if s.cables == nil || s.ops == nil {
		return noop, nil
	}
	mac := s.grants.bound(ip)
	if mac == "" && s.fleet != nil {
		for _, r := range s.fleet.Every() {
			if r.IP == ip && r.MAC != "" {
				mac = r.MAC
				break
			}
		}
	}
	if mac == "" {
		// Nothing ties this address to a chip. The call proceeds, and the board is
		// unprotected: say so, because this is the one case where a theme can tear and
		// the operator cannot see why.
		log.Printf("orb-manager: a theme call to %s cannot be tied to a chip, so no lead is held "+
			"and the identity sweep may reset it mid-transfer", ip)
		return noop, nil
	}
	// Canonical form on both sides of the compare, or the lookup silently finds nothing.
	port := s.cables.cablePortFor(macBare(mac), time.Now())
	if port == "" {
		// The hub knows the chip and the chip is not on a lead the host can see. Either
		// the orb is LAN-only, which is fine, or the cable pass has not attributed this
		// port yet, which is not. The two are indistinguishable from here, and the second
		// is the dangerous one, so the line is printed for both and worded so a reader
		// can tell which to go and check.
		log.Printf("orb-manager: %s is %s but no lead is attributed to it, so no port is held; "+
			"if it is on a cable, the sweep can reset it mid-transfer", ip, mac)
		return noop, nil
	}
	// The board is about to restart itself to draw what it was just sent, and the hub must
	// not read that self-inflicted silence as defection. Armed before the call: a store is
	// acked before the device performs it, so the excuse has to be on file by the time the
	// first dial fails. It is keyed on the chip, not the port, so it survives the
	// re-enumeration the restart causes.
	s.expectSelfReboot(mac)
	job, _, oerr := s.ops.start(ctx, port, opTheme, budget)
	if oerr != nil {
		// Something else owns the lead. It already keeps the probe off this port, which
		// is the whole protection this claim was for, so the theme call proceeds: the
		// wire is HTTP and the holder is somebody else's business.
		return noop, nil
	}
	// The operation slot only speaks to THIS process's sweep. The 23 Oct teardown
	// proved a second orb-manager on the same bench (a launcher start while a stale
	// instance still held the UI port) resets the board mid-store regardless: its
	// probeCableNow consults its own empty slot table, finds no flock on the lead,
	// and pays the DTR/RTS pulse while the first hub holds a perfectly real claim
	// in its own private ledger. The flock is the only guard that speaks across
	// processes — the probe takes it before every pulse precisely to decline
	// someone else's write — so the theme claim must hold it for the copy's full
	// length, not just the slot. A busy flock declines the same way a busy slot
	// does: the holder is another writer, and its own rule keeps the probe off.
	lock, lerr := orbbind.LockPort(port)
	if lerr != nil {
		s.ops.finish(job)
		return noop, nil
	}
	log.Printf("orb-manager: a theme call to %s holds %s; the identity sweep waits", ip, port)
	return func() { lock.Release(); s.ops.finish(job) }, nil
}

// macBare is orbmdns.CanonicalMAC's shape without its refusal: lower case, separators
// gone. It is what the cable cache stores, so it is what every lookup against that cache
// has to hand it, and what the self-reboot excuse is keyed on so one install's colon form
// still excuses the bare form the fleet row carries. A value that was never a MAC comes
// back stripped and matches nothing, which is the honest answer for a foreign chip.
func macBare(s string) string {
	t := strings.ToLower(strings.TrimSpace(s))
	return strings.NewReplacer(":", "", "-", "", ".", "", " ", "").Replace(t)
}

// themeTarget decodes and checks a JSON theme request.
func (s *Server) themeTarget(w http.ResponseWriter, r *http.Request, needID bool) (themeTarget, bool) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return themeTarget{}, false
	}
	var t themeTarget
	if err := json.NewDecoder(r.Body).Decode(&t); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return themeTarget{}, false
	}
	if !s.checkedTargetInto(w, &t, needID) {
		return themeTarget{}, false
	}
	return t, true
}

// checkedTarget applies the gates every device-touching call shares, for callers that
// have the fields already. needID distinguishes install/delete, where the id IS the
// request, from list/rescan/upload, where the orb alone is.
func (s *Server) checkedTarget(w http.ResponseWriter, ip, id string, needID bool) (themeTarget, bool) {
	t := themeTarget{IP: ip, ID: id}
	if !s.checkedTargetInto(w, &t, needID) {
		return themeTarget{}, false
	}
	return t, true
}

// checkedTargetInto is the whole gate, in one place, so a theme call cannot be added
// that forgets one of its parts: an address must exist, must be an IP literal, and must
// be one this hub learned or was handed. A hostname is refused rather than resolved: the
// hub dials devices it has seen, not names a page invented. Then the id, when the call
// needs one, must be the 12 hex the device issued — refusing it here is what stops the
// hub dialing a route the firmware is only going to 400.
func (s *Server) checkedTargetInto(w http.ResponseWriter, t *themeTarget, needID bool) bool {
	t.IP = strings.TrimSpace(t.IP)
	if t.IP == "" {
		s.jsonErr(w, "no orb address given — enter it on the Manage card first")
		return false
	}
	if net.ParseIP(t.IP) == nil || strings.Contains(t.IP, ":") {
		s.jsonErr(w, "not an IPv4 address: "+t.IP)
		return false
	}
	if !s.grants.allowed(t.IP) {
		s.jsonErr(w, "the hub will not dial "+t.IP+": it is not an orb address this hub learned over USB or was given. Enter the orb's address on the Manage card first.")
		return false
	}
	t.ID = strings.TrimSpace(t.ID)
	if needID && t.ID == "" {
		s.jsonErr(w, "no theme chosen — pick one from the list, which is where the orb's own ids come from")
		return false
	}
	if t.ID != "" {
		if err := orbnet.CheckThemeID(t.ID); err != nil {
			s.jsonErr(w, err.Error())
			return false
		}
	}
	return true
}

// themeReply passes the device's own JSON through under the hub's envelope. On a
// device failure the firmware's words ARE the answer, so they are surfaced verbatim:
// "a theme job is already running" and "no such theme on the card" are the two an
// operator has to be able to act on, and paraphrasing them loses the distinction.
func (s *Server) themeReply(w http.ResponseWriter, raw json.RawMessage, err error) {
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	if len(raw) == 0 {
		s.jsonOK(w, map[string]any{"accepted": true})
		return
	}
	var payload any
	if uerr := json.Unmarshal(raw, &payload); uerr != nil {
		s.jsonOK(w, map[string]any{"deviceText": string(raw)})
		return
	}
	s.jsonOK(w, payload)
}

// ---- the live send tracker ---------------------------------------------------
//
// One small state machine per address, guarded, holding what the upload handler is
// doing RIGHT NOW so a page polling /api/theme/upload-status can say "storing" when
// it is storing instead of showing a frozen button. The device owns the truth about
// the slot; this is the hub's witness of the transfer it is itself driving, which is
// why the phase words are the hub's own and the store verdict comes from the
// device's witnesses read back.

type themeSend struct {
	Phase string `json:"phase"` // clear|exact|duplicate|uploading|queued|storing|stored|torn|refused|rejected|lost
	ID    string `json:"id,omitempty"`
	Sent  int64  `json:"sent"`
	Total int64  `json:"total"`
	Note  string `json:"note,omitempty"`
	At    int64  `json:"at"`
}

func (s *Server) setSend(ip string, f func(*themeSend)) {
	s.sendMu.Lock()
	defer s.sendMu.Unlock()
	if s.tsend == nil {
		s.tsend = map[string]*themeSend{}
	}
	st := s.tsend[ip]
	if st == nil {
		st = &themeSend{}
		s.tsend[ip] = st
	}
	f(st)
	st.At = time.Now().Unix()
}

func (s *Server) sendSnapshot(ip string) themeSend {
	s.sendMu.Lock()
	defer s.sendMu.Unlock()
	if st, ok := s.tsend[ip]; ok {
		return *st
	}
	return themeSend{Phase: "idle"}
}

// themeDuplicate answers the precheck against the device's own list with one of three
// verdicts, so the composer can act on the exact-match case instead of uploading over it:
// "duplicate" means the card holds a DIFFERENT build under this name (a blind send would
// be refused late at the door, and a Replace must be offered); "exact" means the card
// already holds THIS bake (name and content serial agree), so a full re-send only moves
// every byte again and re-arms an idempotent store that reboots onto the face already on
// the glass — the §32.9 double-send this precheck exists to stop; "clear" means the name
// is free. A list that cannot be fetched answers "clear" with the reason as the note: an
// unreachable orb is not a collision, and the real send will fail honestly a second later
// if the orb is really gone.
func (s *Server) themeDuplicate(ip string, c *orbnet.ThemeContainer) (string, string) {
	list, err := s.device.ThemeList(ip)
	if err != nil {
		// One retry at a short delay. The hour a Send is most often pressed is the
		// minute after an update, and a board back on its address before its card
		// scan settles answers the list door with nothing — which is a boot, not a
		// card state, and must not talk the hub out of reading the card. A second
		// failure is the real unknown, and answers as one.
		time.Sleep(1500 * time.Millisecond)
		list, err = s.device.ThemeList(ip)
	}
	if err != nil {
		return "clear", "the hub could not ask the orb, so it will not promise there is no collision: " + err.Error()
	}
	for _, row := range list.Themes {
		if row.ID != c.ID {
			continue
		}
		if row.Serial == c.Serial {
			return "exact", "the card already holds this exact theme; a re-send would only reboot onto it"
		}
		return "duplicate", "the card holds a different build of this name (serial " + strconv.FormatUint(uint64(row.Serial), 10) + ")"
	}
	return "clear", ""
}

// armSelfRebootFor is expectSelfReboot resolved from an address the way
// holdDevicePort resolves it, for the waits that run without a claim of their own.
func (s *Server) armSelfRebootFor(ip string) {
	if mac := s.macFor(ip); mac != "" {
		s.expectSelfReboot(mac)
	}
}

// macFor is the address half of holdDevicePort's lookup, standing alone: the MAC the
// grant was committed for, then the address book, bare form — which is what both
// expectSelfReboot and cablePortFor want.
func (s *Server) macFor(ip string) string {
	if s.grants == nil {
		return ""
	}
	mac := s.grants.bound(ip)
	if mac == "" && s.fleet != nil {
		for _, r := range s.fleet.Every() {
			if r.IP == ip && r.MAC != "" {
				mac = r.MAC
				break
			}
		}
	}
	return macBare(mac)
}

// withStoreOutcome adds the hub's store verdict to the device's own reply without
// disturbing the device's fields: the theme doors answer first and finish second, so
// the body stays the device's words and the hub adds only what it watched.
func withStoreOutcome(raw json.RawMessage, outcome, note string) json.RawMessage {
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		return raw
	}
	m["store"] = outcome
	if note != "" {
		m["store_note"] = note
	}
	out, err := json.Marshal(m)
	if err != nil {
		return raw
	}
	return out
}

func firstNonEmptyStr(a, b string) string {
	if a != "" {
		return a
	}
	return b
}

func ternaryStr(cond bool, a, b string) string {
	if cond {
		return a
	}
	return b
}

// handleThemeSendStatus answers the one question every theme sender had no way to
// ask while a send was in flight: where is it, honestly. The upload POST now blocks
// through the whole store — which is what keeps the cable claim honest — and a
// blocked POST is exactly the silent stretch this route replaces. The composer and
// the Manage card poll it every half second and paint what it says. It reports the
// hub's own byte counter for the transfer leg and the wait loop's verdict for the
// slot; the device's card-write count is not exported (the glass already shows that
// copy live at chunk boundaries, and the glass is the screen beside the board).
func (s *Server) handleThemeSendStatus(w http.ResponseWriter, r *http.Request) {
	ip := strings.TrimSpace(r.URL.Query().Get("ip"))
	if _, ok := s.checkedTarget(w, ip, "", false); !ok {
		return
	}
	s.jsonOK(w, s.sendSnapshot(ip))
}
