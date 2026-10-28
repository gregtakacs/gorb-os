package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
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
	// Face and Slug ride on the warm kick only: which face the operator pressed
	// and, for an open editor pane, which app. Every other route ignores them.
	Face string `json:"face,omitempty"`
	Slug string `json:"slug,omitempty"`
}

// handleThemeList returns the card's rows plus the state needed to word them. Each row
// comes back with the name the owner baked and the id every later call needs.
//
// The read goes through the session's card memory, not straight to the board.
// Every face paint dialed GET /api/v1/sd afresh — the card's one walk, 0.3 s
// idle and 4-10 s behind a stream — and the answer on the second paint of one
// page is the answer the first one learned. A fresh remembered read answers
// without touching the lane; a cold one rides it the way the precheck does. The
// one mutation that changes this answer without the hub knowing is a store on
// the orb's own glass, and that is what the two-minute memory and the Rescan
// button are for.
func (s *Server) handleThemeList(w http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w, r, false)
	if !ok {
		return
	}
	// This read IS the Themes face painting: the page owns the lane now, and a
	// running pass parks at its next pick. The kick the page sends behind the
	// paint clears the stamp again — the press asked for warmed bytes, which
	// is the one demand that sets the pass free.
	s.recordPageDemand(t.IP)
	raw, err := s.sdListRead(t.IP)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.jsonOKRaw(w, raw)
}

// handleThemeApps answers D37's question — which apps on this orb can hold themes —
// by intersecting the device's app ledger with its theme rows. GET with ?ip=, like
// the send-status read beside it, because it dials a read and writes nothing.
// Remembered and ridden exactly as the list beside it is: the enumeration is two
// walks of the same single task, which is precisely the expensive half.
func (s *Server) handleThemeApps(w http.ResponseWriter, r *http.Request) {
	ip := strings.TrimSpace(r.URL.Query().Get("ip"))
	t, ok := s.checkedTarget(w, ip, "", false)
	if !ok {
		return
	}
	// The apps face's own paint, the same claim as the list route beside it.
	s.recordPageDemand(t.IP)
	raw, _, err := s.cardAppsRead(t.IP)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.jsonOKRaw(w, raw)
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
		// The slot moved, and the sd list carries the slot: active, stored and
		// the try witnesses all changed with the store's own reboot.
		s.dropThemeCache(t.IP)
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
	s.dropThemeCache(t.IP)
	raw, err := s.device.ThemeDelete(t.IP, t.ID)
	if err == nil {
		s.dropThemeCache(t.IP) // the row is gone from the next honest read
	}
	s.themeReply(w, raw, err)
}

// handleThemeRescan asks the orb to re-read the card, which is how a theme that
// arrived by another route shows up in the list without rebooting the orb.
//
// The press rides the lane the list read rides. The operator reaches for this
// button exactly when the page's answers look wrong, and a warm pass or a
// preview stream holding the card's one task is a common reason — the refused
// rescan of 2026-10-28 was that race. A busy lane returns a wait, not a verdict.
//
// A success retires every remembered answer for the address: the card just
// re-read itself, so what the hub knew a minute ago is the previous card.
//
// The refill runs in the background rather than ahead of the reply. h_sd_rescan
// calls orb_sd_remount() and answers only once the card is back, so the device
// dial already carries one whole cold walk of the card — tens of seconds on a big
// one — and dialling the rows again inside the same request made the press cost
// two walks back to back. That doubling is the "re-read is still way too slow"
// report. The page reloads its own rows the moment this reply lands, so the one
// honest read it needs happens there; the background pass refills what the reply
// retired and starts the read-ahead behind it.
func (s *Server) handleThemeRescan(w http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w, r, false)
	if !ok {
		return
	}
	var raw json.RawMessage
	err := rideOutLaneBudget(rescanRideBudget, func(remain time.Duration) error {
		var derr error
		raw, derr = s.device.WithTimeout(remain).ThemeRescan(t.IP)
		return derr
	})
	if err == nil {
		s.dropThemeCache(t.IP)
		dropEditorRegions(t.IP)
		go s.refillAfterRescan(t.IP)
	}
	s.themeReply(w, raw, err)
}

// refillAfterRescan re-learns the card a rescan just changed, and puts the
// read-ahead pass back on it. It is the rescan's own promise kept off the
// request's critical path: the rows land in the memory the next page read serves
// from, and the planes and regions follow behind them.
//
// No caller waits on it, so the page's own ride budget is the one it uses: a
// failed refill leaves the next page read cold, which is the state before the
// rescan and not a loss.
func (s *Server) refillAfterRescan(ip string) {
	list, err := s.themeListRidingOutLane(ip)
	if err != nil {
		return
	}
	s.rememberList(ip, list)
	s.kickWarm(ip, list)
}

// rememberList files a freshly dialed card read into the memory with the stamp
// of now, for the callers that hold the list already (the rescan's refill) and
// must not pay a second walk to warm the slot.
func (s *Server) rememberList(ip string, list *orbnet.ThemeList) {
	raw, _ := json.Marshal(list)
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	if s.cardReads == nil {
		s.cardReads = map[string]cardRead{}
	}
	h := s.cardReads[ip]
	h.raw, h.list, h.at, h.err = raw, list, time.Now(), ""
	s.cardReads[ip] = h
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
// cached still holds the device's whole httpd task for the length of the plane,
// two seconds warm and ten on a cold card, so the cheap answer is to ask it once
// per card-and-theme, not once per repaint.
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

// ---- the session's card memory: reads, read-ahead, and their retirement -----
//
// The Theme page's cost is not the bytes; it is the walk. GET /api/v1/sd answers
// in ~0.3 s on an idle card and 4-10 s when the board's one httpd task is mid-
// stream, the app enumeration walks the same board a second time, and a preview
// plane costs 2-10 s of that task per theme. Paying that walk on every click is
// what the operator sees as churn: slow fills, and refusals that name nothing,
// because the control budget expires at 1.5 s while a stranger's dial holds.
// So the hub remembers the answer once and serves it until something makes it a
// lie. sdCacheTTL is the session window the operator asked for, kept to two
// minutes because a theme STORED on the orb's own glass changes the list's
// active row through no hub event, and that is the only staleness the memory
// can carry. Every hub-side mutation retires the memory at once.
const sdCacheTTL = 2 * time.Minute

// sdFailWindow is how long a FAILED card read keeps its answer. Unlike a plane,
// a transport failure is not a stable fact, but re-dialing a dead board once per
// page paint turns one outage into ten timeouts.
//
// It is deliberately SHORTER than the Themes page's own retry wait (2.3 s, see
// themeListWithRetry), and that ordering is the whole point: the page's second
// try must get a real dial rather than an echo of the first failure, because a
// card walk that timed out once commonly lands on the next attempt — the board
// was cold, the card was mid-mount, or the one task was serving a plane. A
// window wider than the retry cadence would make the page's patience useless
// while looking like it worked.
const sdFailWindow = 2 * time.Second

// cardRead is one address's remembered card: the sd read (raw as the device
// answered it, plus the parsed rows the read-ahead pass needs), the app
// enumeration, and the dial in flight. A leader records while followers ride
// the leader's channel instead of opening a second dial on the same busy task.
// An invalidation closes the channel and clears the answers, and a leader whose
// dial is no longer registered discards its result because the card moved under it.
type cardRead struct {
	raw     json.RawMessage
	list    *orbnet.ThemeList
	at      time.Time
	apps    json.RawMessage
	appsVal *orbnet.ThemeApps
	appsAt  time.Time
	err     string
	errAt   time.Time
	dial    chan struct{}
}

// dropThemeCache retires every remembered card answer for one address: the
// planes, the sd list, the app enumeration, and any read-ahead pass walking
// them. Call it at every event that can change what the card answers: ship,
// delete, store, commit, rescan. Editor regions are deliberately not included.
// They belong to an app's container rather than to any theme row, and a ship
// that rewrites one theme's bytes must not make the next Edit door pay a
// megabyte walk again; the events that move regions, rescan and a reflash,
// drop them explicitly.
func (s *Server) dropThemeCache(ip string) {
	s.dropPreviews(ip)
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	if h, ok := s.cardReads[ip]; ok {
		if h.dial != nil {
			close(h.dial) // release any rider; the fresh read after this finds an empty slot
			h.dial = nil
		}
		h.raw, h.list, h.apps, h.appsVal = nil, nil, nil, nil
		h.err, h.errAt = "", time.Time{}
		s.cardReads[ip] = h
	}
	if s.warmSeq == nil {
		s.warmSeq = map[string]uint64{}
	}
	s.warmSeq[ip]++
	delete(s.warmPass, ip)
}

// sdListRead answers GET /api/v1/sd through the memory: the remembered bytes if
// fresh, the leader's in-flight dial if there is one, and otherwise this call's
// own ride on the lane. A failed cold dial with a remembered answer behind it
// serves the stale answer, because a card that was there sixty seconds ago
// beats a timeout that says nothing. A failed cold dial with nothing behind it
// returns the error, debounced for sdFailWindow so one outage is not re-walked
// on every paint.
// sdListReadWarm is the speculative card walk: the same memory, the same single
// flight, and the background transport under the dial.
func (s *Server) sdListReadWarm(ip string) (json.RawMessage, error) {
	if s.warm == nil {
		return s.sdListRead(ip)
	}
	c, _ := s.warmClients()
	return s.sdListReadOn(ip, c, true)
}

func (s *Server) sdListRead(ip string) (json.RawMessage, error) {
	return s.sdListReadOn(ip, s.device, false)
}

// sdListReadOn is the memory and the single flight; the dial it leads rides the
// caller's transport and budget. A background leader records its cut as an
// interruption, not a card failure, so the debounce window does not poison the
// page's next cold read with a cut it caused.
func (s *Server) sdListReadOn(ip string, dialClient *orbnet.Client, cuttable bool) (json.RawMessage, error) {
	dialBudget := pageRideBudget
	if cuttable {
		dialBudget = specListBudget
	}
	for {
		s.cardMu.Lock()
		h := s.cardReads[ip]
		if h.raw != nil && time.Since(h.at) < sdCacheTTL {
			raw := h.raw
			s.cardMu.Unlock()
			return raw, nil
		}
		if h.dial != nil {
			ch := h.dial
			s.cardMu.Unlock()
			<-ch
			continue
		}
		if h.err != "" && time.Since(h.errAt) < sdFailWindow {
			err := errors.New(h.err)
			s.cardMu.Unlock()
			return nil, err
		}
		ch := make(chan struct{})
		h.dial, h.err = ch, ""
		s.cardReads[ip] = h
		s.cardMu.Unlock()

		list, err := s.themeListRiding(ip, dialClient, dialBudget, cuttable)
		var raw json.RawMessage
		if err == nil {
			raw, _ = json.Marshal(list)
		}
		s.cardMu.Lock()
		cur := s.cardReads[ip]
		if cur.dial == ch { // not retired under this dial
			if err == nil {
				cur.raw, cur.list, cur.at = raw, list, time.Now()
				cur.err = ""
			} else {
				cur.err, cur.errAt = err.Error(), time.Now()
			}
			cur.dial = nil
			s.cardReads[ip] = cur
		}
		s.cardMu.Unlock()
		close(ch)
		if err != nil {
			if laneCut(err) {
				return nil, err // a cut is not the card's failure; no debounce stamp
			}
			s.cardMu.Lock()
			stale := s.cardReads[ip].raw
			s.cardMu.Unlock()
			if stale != nil {
				return stale, nil
			}
			return nil, err
		}
		return raw, nil
	}
}

// cardAppsRead answers the app enumeration behind the same slot. It is the one
// route on this page that wants BOTH of the board's slow answers, and /api/v1/sd
// is the expensive half of them, so it does not dial the card for itself: it
// takes the rows through the card read beside it, which is single-flighted and
// remembered, and dials only the app ledger. One page paint therefore costs one
// card walk, not two — the second one is what put the Themes page past the
// browser's patience on 2026-10-28. The enumeration keeps its own TTL.
func (s *Server) cardAppsRead(ip string) (json.RawMessage, *orbnet.ThemeApps, error) {
	return s.cardAppsReadOn(ip, s.device, false)
}

// cardAppsReadWarm is the pass's enumeration: the same memory, the same single
// flight, and the background transport under both legs — the walk may be cut
// mid-stride by a page demand, and the enumeration must come back as the cut it
// was rather than retried into the lane the page just took.
func (s *Server) cardAppsReadWarm(ip string) (json.RawMessage, *orbnet.ThemeApps, error) {
	if s.warm == nil {
		return s.cardAppsRead(ip)
	}
	c, _ := s.warmClients()
	return s.cardAppsReadOn(ip, c, true)
}

func (s *Server) cardAppsReadOn(ip string, c *orbnet.Client, cuttable bool) (json.RawMessage, *orbnet.ThemeApps, error) {
	s.cardMu.Lock()
	h := s.cardReads[ip]
	if h.apps != nil && time.Since(h.appsAt) < sdCacheTTL {
		raw, val := h.apps, h.appsVal
		s.cardMu.Unlock()
		return raw, val, nil
	}
	s.cardMu.Unlock()

	listRaw, listErr := s.sdListReadOn(ip, c, cuttable)
	var rows *orbnet.ThemeList
	if listErr == nil {
		if err := json.Unmarshal(listRaw, &rows); err != nil {
			listErr = errors.New("the card read answered with rows the hub could not parse")
			rows = nil
		}
	}

	apps, appErr := s.appsLedgerRiding(ip, c, cuttable)
	out, err := s.device.ThemeAppsFrom(ip, apps, appErr, rows, listErr)
	if err != nil {
		return nil, nil, err
	}
	raw, _ := json.Marshal(out)
	s.cardMu.Lock()
	cur := s.cardReads[ip]
	if cur.apps == nil || time.Since(cur.appsAt) >= sdCacheTTL {
		cur.apps, cur.appsVal, cur.appsAt = raw, out, time.Now()
		s.cardReads[ip] = cur
	}
	s.cardMu.Unlock()
	return raw, out, nil
}

// previewHold and previewRelease bracket one plane relay, and previewInFlight is
// what the liveness sweeper asks before it spends a tick. A relay and a fleet sweep
// both land on the same single-task httpd on the device, and the sweep wins that
// collision by cutting the stream — see handleThemePreview. The count, not a bool,
// because two tabs can dial two orbs at once and the last one out must not
// extinguish the other's hold.
func (s *Server) previewHold() {
	s.previewMu.Lock()
	s.previewBusy++
	s.previewMu.Unlock()
}

func (s *Server) previewRelease() {
	s.previewMu.Lock()
	s.previewBusy--
	s.previewMu.Unlock()
}

func (s *Server) previewInFlight() bool {
	s.previewMu.Lock()
	defer s.previewMu.Unlock()
	return s.previewBusy > 0
}

// previewFresh answers whether the plane cache already holds this card's answer
// for this id, so the read-ahead pass walks past a row the page already drew.
func (s *Server) previewFresh(ip, id string) bool {
	s.previewMu.Lock()
	defer s.previewMu.Unlock()
	h, ok := s.previews[ip+"|"+id]
	return ok && h.code == http.StatusOK && time.Since(h.at) < previewCacheTTL
}

// fetchPreviewPlane reads one card's plane through the single flight: the cache
// first, the in-flight relay second — a caller for a plane the read-ahead pass
// is already dialing rides that relay rather than opening a second one on a
// task that serves one request at a time — and the dial third. A plane read is
// idempotent, so a ride costs nothing but the wait. The sweep stands down for
// the length of the dial, exactly as the old relay path did: measured with
// v1.1.8's one-segment writes, four of six fetches came back short while the
// ten-second liveness tick landed on the same one-task httpd as the stream.
//
// 200s are remembered and failures are not, exactly as the old relay path
// ruled: a 404 is the design's quiet answer "this row carries no plane", the
// read can come back a whole plane the moment the row does, and TestThePreview
// RelayPassesQuietlyThenCaches guards that line. The read-ahead pass pays that
// re-read once per pass for a plane-less row, which is the same honest stall
// the page's queue paid on every paint before this pass existed.
func (s *Server) fetchPreviewPlane(ip, id string) (previewHit, error) {
	return s.fetchPreviewPlaneOn(ip, id, func(ip, id string) ([]byte, int, error) {
		return s.preview.ThemePreview(ip, id)
	})
}

// warmClients answers the two transports one read-ahead dial may ride: the card
// reader and the plane streamer. The shipped hub answers both with s.warm —
// ClassBackground, the one class the gate may interrupt — because every dial the
// pass dials is insurance, and insurance that cannot be cut is a promise the
// hub cannot keep to the page in front of it. A hub built without s.warm (every
// test hub, whose clients are assembled by hand) answers with the page's own
// clients: there is no gate to interrupt through there, and the between-dial
// yields are the half of the discipline those tests assert.
func (s *Server) warmClients() (list, plane *orbnet.Client) {
	if s.warm != nil {
		return s.warm, s.warm.WithTimeout(warmPlaneBudget)
	}
	return s.device, s.preview
}

// fetchPreviewPlaneWarm is the pass's plane read: the same cache, the same
// single flight, and the background transport under the dial. A page that
// arrives behind it becomes a rider on the shorter dial by construction — it
// waits out warmPlaneBudget rather than the whole pass, sees the miss when that
// dial dies, and leads its own full-budget read on the next loop turn. The pass
// yields its length to the page's patience without giving up the dedupe.
func (s *Server) fetchPreviewPlaneWarm(ip, id string) (previewHit, error) {
	_, plane := s.warmClients()
	return s.fetchPreviewPlaneOn(ip, id, func(ip, id string) ([]byte, int, error) {
		return plane.ThemePreview(ip, id)
	})
}

func (s *Server) fetchPreviewPlaneOn(ip, id string, fetch func(string, string) ([]byte, int, error)) (previewHit, error) {
	key := ip + "|" + id
	for {
		s.previewMu.Lock()
		if h, ok := s.previews[key]; ok && time.Since(h.at) < previewCacheTTL {
			s.previewMu.Unlock()
			return h, nil
		}
		if ch, fly := s.pvFly[key]; fly {
			s.previewMu.Unlock()
			<-ch
			continue
		}
		if s.pvFly == nil {
			s.pvFly = map[string]chan struct{}{}
		}
		ch := make(chan struct{})
		s.pvFly[key] = ch
		s.previewMu.Unlock()

		s.previewHold()
		body, code, err := fetch(ip, id)
		if errors.Is(err, orbnet.ErrTornPreview) {
			body, code, err = fetch(ip, id)
		}
		s.previewRelease()
		s.previewMu.Lock()
		if err == nil && code == http.StatusOK {
			s.previews[key] = previewHit{body: body, code: code, at: time.Now()}
		}
		delete(s.pvFly, key)
		s.previewMu.Unlock()
		close(ch)
		return previewHit{body: body, code: code, at: time.Now()}, err
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
// The read is fetchPreviewPlane's: the cache, the in-flight relay, and only then a
// dial on the preview client — never the 8 s management one, which measured over
// the budget for a route that answers after a card walk, a region read and a
// ~100 KB stream. Every cut of that old shape looked to the page like a missing
// plane and painted the quiet-black square this card used to argue about.
func (s *Server) handleThemePreview(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	t, ok := s.checkedTarget(w, strings.TrimSpace(q.Get("ip")), strings.TrimSpace(q.Get("id")), true)
	if !ok {
		return
	}
	// This route exists only for the page's own preview queue — the pass dials
	// fetchPreviewPlane directly, never through here — so the visit is the
	// queue saying "live page working" as much as it is a plane request. The
	// read-ahead pass stands down from planes while that stamp stays fresh.
	s.recordPagePreview(t.IP)
	hit, err := s.fetchPreviewPlane(t.IP, t.ID)
	if err != nil {
		// The route's failure is an event with a reason, not a missing plane:
		// the hub's own sentence says timeout, refusal, or gone, and the page
		// shows it instead of silently hiding the square.
		s.jsonErr(w, err.Error())
		return
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
	// A theme shipping changes what the card would answer for its id's preview
	// and for the list beside it, so this address's whole memory retires before
	// the bytes move and the read-ahead pass refills it once the store lands.
	s.dropThemeCache(t.IP)
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
	// The card now holds the bytes this ship wrote, and the store above may have
	// flipped the slot with them. Whatever the hub remembered about the rows,
	// the preview, and the slot belongs to the card that existed a minute ago.
	s.dropThemeCache(ip)
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
//
// The page that presses Store must out-wait this budget, not the other way round
// — the same order pageRideBudget keeps. web/app.js gives the install POST 165 s
// for exactly that; a shorter page budget prints Chrome's "signal timed out" over
// a store that lands fine (owner, 2026-10-28).
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
	if err == nil {
		// The staged copy landed and its store was queued: same retirement as
		// any other write to the card.
		s.dropThemeCache(t.IP)
	}
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

// themeDuplicate answers the precheck with one of three verdicts, so the composer can
// act on the exact-match case instead of uploading over it:
// "duplicate" means the card holds a DIFFERENT build under this name (a blind send would
// be refused late at the door, and a Replace must be offered); "exact" means the card
// already holds THIS bake (name and content serial agree), so a full re-send only moves
// every byte again and re-arms an idempotent store that reboots onto the face already on
// the glass — the §32.9 double-send this precheck exists to stop; "clear" means the name
// is free. A list that cannot be fetched answers "clear" with the reason as the note: an
// unreachable orb is not a collision, and the real send will fail honestly a second
// later if the orb is really gone.
//
// The read starts from the session's card memory whenever it is fresh. Every theme
// write this hub performs retires the memory the moment it lands, so inside the
// window the remembered rows are the rows a dial would return, and the first press
// after opening the Theme page answers off bytes already in hand — the 9.5 s and
// 10.2 s race-measured waits of 2026-10-28's precheck stop being the press's cost.
// A stale or missing memory falls back to the strict riding-out-lane dial below,
// and the device's own overwrite-conflict answer remains the final gate either
// way: a precheck that guesses "clear" costs a refused POST, never a lost theme.
func (s *Server) themeDuplicate(ip string, c *orbnet.ThemeContainer) (string, string) {
	list := s.freshList(ip)
	if list == nil {
		var err error
		list, err = s.themeListRidingOutLane(ip)
		if err != nil {
			return "clear", "the hub could not ask the orb, so it will not promise there is no collision: " + err.Error()
		}
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

// freshList returns the remembered rows inside the memory's own TTL, or nothing.
// It never dials and never waits on a rider's channel: the precheck's cold path
// is the strict dial, because a press must not inherit a stranger's wait.
func (s *Server) freshList(ip string) *orbnet.ThemeList {
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	h := s.cardReads[ip]
	if h.list == nil || time.Since(h.at) >= sdCacheTTL {
		return nil
	}
	return h.list
}

// themeListRidingOutLane is the precheck's read with the hub's own traffic
// waited out, and the second-press puzzle's answer. ErrLaneBusy is not the orb
// refusing: it is THIS hub holding the orb's lane for a transfer of its own —
// most often the Theme page's own preview relays, which stream for seconds each
// and fast-fail a control dial at its ~1.5 s queue budget, or an export's upload
// leg crossing a megabyte. The old one-retry-1.5 s shape stamped that as "could
// not get a clean read" on the first press every time previews were mid-flight,
// and the operator watched the page block its own send. A busy lane is a wait,
// not a verdict, so this waits: six tries at 1.5 s covers a preview pass. A torn
// transport — the dial dying mid-body, which the bench measured on /api/v1/sd on
// 2026-10-27 while a region read held the board's single socket — gets ONE
// redial at 400 ms, the same fresh-retry rule editorRegion applies: the list is
// an idempotent GET and the class clears on the second dial. Any other error
// returns at once.
//
// The dial itself stays CONTROL-class on the device transport. On 2026-10-28 a
// version of this read the sd list through the preview (bulk, 45 s) client and
// retried twenty times; the hub then held the orb's lane through long bulk
// dials, and the theme list, the app enumeration, and the wifi check — every
// other control route to that orb — starved behind it ("scan failed: orb busy"
// on a board answering direct in 4.4 s). The lane's own rule is that control
// dials pass queued bulk waits; a control read routed bulk inverts that on
// purpose and loses. What the precheck legitimately needs is more TIME on one
// cheap control dial — the race-measured sd read took 9.5 s and 10.2 s while a
// preview burst and an editor region streamed — so the dial gets 20 s on the
// device transport (WithTimeout shares the gate and the control class), and the
// loop stays short. One in-flight dial then costs the bus at most 20 s, not
// minutes.
// themeListRidingOutLane is the card walk on page-facing terms: control class, the
// shared ride-out loop, and a dial whose own timeout is whatever the ride still
// has left. That last part is what keeps the hub's promise — the read answers
// inside pageRideBudget, which the Themes page's 45 s fetch budget is priced
// around. See pageRideBudget for the failure that produced this shape.
func (s *Server) themeListRidingOutLane(ip string) (*orbnet.ThemeList, error) {
	return s.themeListRiding(ip, s.device, pageRideBudget, false)
}

// themeListRiding is the card walk on the transport the caller stands behind.
// The page stands behind the control client and the ride-out retries; the pass
// stands behind the background client, whose dial the gate may close, and a cut
// ride must NOT retry — the page that cut it owns the lane now, and riding back
// is the contention the interrupt exists to end. warmBudget names the wall the
// ride gets: pageRideBudget for the page, specListBudget for the pass, the same
// length on purpose so a walk the page would have ridden out never dies on the
// pass's shorter leash for any reason but the cut.
func (s *Server) themeListRiding(ip string, c *orbnet.Client, budget time.Duration, cuttable bool) (*orbnet.ThemeList, error) {
	var list *orbnet.ThemeList
	err := rideOutLaneBudget(budget, func(remain time.Duration) error {
		var lerr error
		list, lerr = c.WithTimeout(remain).ThemeList(ip)
		if cuttable && lerr != nil && laneCut(lerr) {
			return noRide(lerr)
		}
		return lerr
	})
	return list, err
}

// noRide wraps an error so rideOutLaneBudget treats it as terminal: the dial
// was cut, not the lane busy, and a retry would fight the page for the lane the
// page just claimed.
func noRide(err error) error { return &cutError{err} }

type cutError struct{ err error }

func (e *cutError) Error() string { return e.err.Error() }
func (e *cutError) Unwrap() error { return e.err }

// laneCut names what a closed background socket looks like from this side: the
// transport error the page's Interrupt leaves behind.
func laneCut(err error) bool {
	return orbnet.LaneCut(err)
}

// rideOutLane runs one idempotent device read until it answers or the failure
// stops being the lane's. A busy lane is a wait with a name, not a verdict, so
// this waits: six tries at 1.5 s covers one honest pass over a busy board. A
// torn transport, the dial dying mid-body, gets ONE redial at 400 ms, the same
// fresh-retry rule editorRegion applies, because the second dial clears what a
// radio hiccup caused. Any other error returns at once. Callers keep their own
// patience on the dial itself: control-class, deep timeout, never the bulk
// client. The lane's rule is that control passes queued bulk, and a control
// read routed bulk inverts that on purpose and starves every route beside it.
// rideOutLaneBudget runs one idempotent device read until it answers or the
// failure stops being the lane's, inside a wall-clock budget. A busy lane is a
// wait with a name, not a verdict, so this waits in 1.5 s slices. A torn
// transport — the dial dying mid-body — gets ONE redial at 400 ms, the same
// fresh-retry rule editorRegion applies, because the second dial clears what a
// radio hiccup caused. Any other error returns at once.
//
// read takes the time still left and must bound its own dial by it. That
// handoff is the whole point of the signature: a dial whose own timeout comes out
// of the remaining budget cannot overrun the budget, so the caller's promise
// about when it answers holds even when the lane frees at the last instant.
//
// A caller that a page is waiting on hands in its own budget under that page's
// fetch budget, and every such caller names pageRideBudget. A background pass
// passes zero and gets rideBudgetDefault.
func rideOutLaneBudget(budget time.Duration, read func(remain time.Duration) error) error {
	if budget <= 0 {
		budget = rideBudgetDefault
	}
	deadline := time.Now().Add(budget)
	var err error
	transportRetry := false
	for {
		left := time.Until(deadline)
		if left <= 0 {
			break
		}
		err = read(left)
		if err == nil {
			return nil
		}
		// Never sleep past the budget: a ride that overruns it answers after the
		// caller stopped listening, which is the failure this bound exists to kill.
		left = time.Until(deadline)
		if left <= 0 {
			break
		}
		wait := 1500 * time.Millisecond
		if !errors.Is(err, orbnet.ErrLaneBusy) {
			if transportRetry {
				return err
			}
			transportRetry = true
			wait = 400 * time.Millisecond
		}
		if wait > left {
			wait = left
		}
		time.Sleep(wait)
	}
	if err == nil {
		err = fmt.Errorf("card read waited out the hub's %s budget for the lane and never got it", budget)
	}
	return err
}

// rideBudgetDefault is the ride budget for a caller nobody is keeping a promise
// to: a background pass, whose failure costs a cold cache rather than a stalled
// page. It sits under pageRideBudget so a page that arrives while a pass is
// mid-ride never waits longer than the pass it rode in on.
const rideBudgetDefault = 20 * time.Second

// pageRideBudget is what a page-facing card read may spend on the lane, and the
// arithmetic that picks it is the reason the Themes page stopped lying.
//
// The card's one walk costs ~0.3 s on an idle card and a great deal more on a big
// one: a 32 GB card mounted at 20 MHz SPI with eight themes on it takes the FatFs
// walk tens of seconds cold, and all of it sits between the dial and the first
// response byte, because h_sd frames its JSON only once the scan is done. An 18 s
// bound was the wrong answer on exactly that hardware — the first cold read lost,
// and the operator read a sentence about an orb that was working fine
// (2026-10-28: the enumeration timed out while the list beside it, riding the FAT
// cache that very walk had warmed, came back clean). 30 s is priced over the cold
// walk; the page gives the fetch 45 s so the hub still answers first and says why.
//
// The order is the invariant, not the number: hub budget < page fetch budget. When
// the page is first to give up, the card prints Chrome's "signal timed out" where
// the orb's own reason belongs.
const pageRideBudget = 30 * time.Second

// rescanRideBudget is the rescan's ride, and it is longer than a plain card read
// on purpose: the press makes the device re-read the whole card before it
// answers, the lane may already be mid-stream when the press lands, and the page
// gives this POST 45 s (web/app.js #mn-rescan). 30 s of hub inside 45 s of page
// keep the same order pageRideBudget keeps: the hub fails first, and says why.
const rescanRideBudget = 30 * time.Second

// appsLedgerRidingOutLane dials the app ledger alone, on the same page-facing
// terms as the card walk beside it. The enumeration's other half — the theme
// rows — comes from the card read, which the hub remembers and single-flights, so
// this dial deliberately stops at /api/v1/apps. It used to be a ThemeAppsFor call
// that walked the card a second time inside itself; on an idle card that cost
// 0.3 s and went unnoticed, and on a card mid-stream it cost the page.
func (s *Server) appsLedgerRidingOutLane(ip string) (*orbnet.AppList, error) {
	return s.appsLedgerRiding(ip, s.device, false)
}

func (s *Server) appsLedgerRiding(ip string, c *orbnet.Client, cuttable bool) (*orbnet.AppList, error) {
	var apps *orbnet.AppList
	budget, wrap := pageRideBudget, func(e error) error { return e }
	if cuttable {
		budget = specListBudget
		wrap = func(e error) error {
			if laneCut(e) {
				return noRide(e)
			}
			return e
		}
	}
	err := rideOutLaneBudget(budget, func(remain time.Duration) error {
		var aerr error
		apps, aerr = c.WithTimeout(remain).AppsList(ip)
		return wrap(aerr)
	})
	return apps, err
}

// ---- the read-ahead pass ------------------------------------------------------
//
// The Theme page wants the same answers every time it opens: the list, the
// enumeration, one plane per row, and for the Edit door one region per app.
// Fetching them one click at a time makes every click pay a card walk, and the
// owner's reports — a refused send, a blank editor, a wifi check that timed
// out — were those walks colliding. So the hub fetches them all at once, from
// the moment a board proves itself alive rather than the moment a page opens.
// The pass dials on the same clients, through the same lane, one at a time,
// exactly as the page's own queue does. It is not a second lane. It is the
// page's own errand run before the clicks ask for it, and it dies the moment a
// mutation retires the answers it was filling.
//
// It is a scheduler, not a script. The order it runs is the order the operator
// is likely to reach for: the active theme's plane first (it is the row on the
// glass and the row the next click touches), the rest of the planes, then the
// editor regions — themes before apps, both before everything the face reads
// off the cheap status cache. And the order yields twice over: the face the
// operator just pressed jumps its own work to the front, and a page with a
// live preview queue takes the plane list off the pass entirely, because that
// queue is dialing the very bytes the pass would, caching them through the
// very door the pass uses, and a second fetcher for the same plane is just
// contention wearing a head start.
type warmRun struct {
	seq   uint64
	phase string // listing, planes, regions, done, failed
	note  string
	total int
	done  int
	at    time.Time
}

// warmHint is the face the operator's hand is on for this address, and the
// app slug when that face is an open editor pane. warmOrb re-reads it before
// every pick, which is what makes the reordering dynamic rather than a
// restart: a press that lands mid-pass pulls the regions in front of planes
// still queued, and the pass never discards the answers it already cached.
// The stamp expires the hint — a face left open overnight should not bend
// every pass for a week; the next press refreshes it and a cold start falls
// back to the likelihood order, which is the right guess when nobody has
// pressed anything.
type warmHint struct {
	face string // "themes", "apps", "edit", or any other face name (plain order)
	slug string // with face "edit": the app whose pane just opened
	at   time.Time
}

const (
	// warmHintLive is the hint's freshness: one working stretch on a face. The
	// page re-POSTs the warm kick on every cold paint of that face, so an open
	// face keeps itself first for as long as it redraws.
	warmHintLive = 10 * time.Minute
	// pagePvYield is how recently the PAGE must have fetched a plane for its
	// queue to count as live. It is deliberately older than the worst single
	// relay (previewTimeout, 60 s) so two slow consecutive thumbnails on a cold
	// card do not open a gap the pass squeezes into, and younger than any
	// abandoned tab: close the page and within ninety seconds the card is the
	// pass's again.
	pagePvYield = 90 * time.Second
	// demandStampLive is how recently a deliberate non-plane page read — the
	// zone table, the file manager's card walk, a wifi scan, a face's own paint
	// — keeps the pass off this orb's lane. It sits under pagePvYield because
	// those reads are short and bursty, and a pass parked all afternoon behind
	// one config read is insurance that has forgotten who it insures.
	demandStampLive = 45 * time.Second
	// specMax caps the speculative reads in flight hub-wide. A fleet boot
	// flips every board to alive at once; twenty card walks at the hub's front
	// door is the same self-inflicted stall the pass exists to prevent, so the
	// sweep kicks at most specMax boards ahead of the operator and the rest
	// warm at their flip on the next sweep or at first press.
	specMax = 2
	// specQueueWait bounds how long a route-kicked speculative read waits for a
	// slot. It is generous because the waiter is a background goroutine behind
	// a fire-and-forget POST, and the alternative is answering a deliberate
	// press with "idle" and nothing moving.
	specQueueWait = 5 * time.Minute
)

// warmLive bounds how long a pass with no progress still counts as running. A
// dial is capped by the budgets below and the pass stamps its clock at every
// step, so six silent minutes means the goroutine died with a hub going down
// around it and a fresh pass may take its place.
const warmLive = 6 * time.Minute

// The read-ahead's own budgets. Every dial the pass dials rides s.warm, the
// background-class stream client, and the numbers below are how much patience
// each kind of read gets THERE. The governing rule is Interrupt, not the
// timer: a page-facing read closes the pass's socket on arrival (demandLane),
// so a pass dial's length is only the worst case for a page that arrives
// before the demand lands — and that worst case is why none of these exceeds
// the page's own transport budget by design:
//
//   - specListBudget matches pageRideBudget exactly: the card walk costs the
//     same cold whether the page or the pass asks, and the pass must not die
//     on a slow card the page would have ridden out (the 2026-10-28 question-
//     marks report — a bound set below the device's own cold arithmetic).
//   - warmPlaneBudget sits under the proxy's deviceTimeout so one speculative
//     plane can never strand a page-facing proxy read past the budget that
//     read already carries. A speculative read that dies there is insurance
//     spent, not an answer lost: the page's own queue fetches the plane at the
//     full previewTimeout on first sight of the row.
//   - warmRegionBudget equals the page's region dial: the memo the page reads
//     is the one this fills, so it must not come back thin; the interrupt is
//     what bounds it, not a shorter timer.
const (
	specListBudget   = 30 * time.Second // == pageRideBudget, on purpose
	warmPlaneBudget  = 7 * time.Second  // < deviceTimeout
	warmRegionBudget = 15 * time.Second // == the page region dial
)

// warmParkTick and warmParkMax bound the pass's wait between picks while the
// page is reading this orb: a quarter-second poll, retired after ten minutes of
// a page that never goes quiet. The tick is small because the pass must resume
// the INSTANT the lane goes quiet — that is the whole bargain speculative work
// makes to be allowed to exist at all.
const (
	warmParkTick = 250 * time.Millisecond
	warmParkMax  = 10 * time.Minute
)

// kickWarm starts a read-ahead pass unless this address already has one
// running. Three things knock: POST /api/theme/warm, rung by the page when a
// card face asks for the reads; the rescan's refill; and the liveness edge
// behind warmSpeculative. The call is always safe: an address with a pass
// running, or a card whose answers are all fresh, takes the kick and dials
// nothing extra — and a press that arrives while a pass runs still lands its
// hint, because setWarmBoost ran before this and the running pass reads it at
// its next pick.
//
// The kick clears the address's page-demand stamp on purpose. The stamp is the
// pass's standing order to keep off a lane the operator is using, and the
// press that KICKS is the operator asking for the warmed bytes — the two facts
// cannot hold at once, and the newer one wins. Stamps set after the kick (the
// next proxy read, the next tab) still pause it.
func (s *Server) kickWarm(ip string, list *orbnet.ThemeList) {
	if list == nil {
		return
	}
	s.clearPageDemand(ip)
	s.cardMu.Lock()
	if w := s.warmPass[ip]; w != nil && (w.phase == "listing" || w.phase == "planes" || w.phase == "regions") &&
		time.Since(w.at) < warmLive {
		s.cardMu.Unlock()
		return
	}
	if s.warmSeq == nil {
		s.warmSeq = map[string]uint64{}
	}
	s.warmSeq[ip]++
	w := &warmRun{seq: s.warmSeq[ip], phase: "listing", at: time.Now()}
	s.warmPass[ip] = w
	s.cardMu.Unlock()
	go s.warmOrb(ip, w, list)
}

// warmSpeculative learns the card of a board nobody has opened yet, so the page
// the operator hits next costs one cache read instead of one card walk. Every
// part of the first paint that felt cold — the name, the card line, the row
// count — rides on this one read, because the status the sweep already folds
// into the tile is the free half and the sd list is the expensive one. The door
// is the same for both knockers and equally cheap to a board that is already
// remembered: the first check is the card memory, not the lane.
//
// page says who is waiting. A route press waits its turn for a slot; a sweep
// edge walks away from a full house, because that board's first real press
// re-knocks through the waiting door and the sweep must never queue behind its
// own insurance.
func (s *Server) warmSpeculative(ip string, page bool) {
	if ip == "" || s.cachedList(ip) != nil {
		return
	}
	// Kick-time yield, mirroring the sweep's own standing down: a plane relay in
	// flight owns the device's httpd task, and a page queue with live stamps is
	// about to want the lane itself. Speculative work is insurance, and insurance
	// that delays a deliberate press is worse than a cold first paint — the press
	// kicks its own warm through the route anyway, so standing down here loses a
	// head start, never the answer.
	if s.previewInFlight() || s.pageDemandLive(ip) {
		return
	}
	s.queueSpec(ip, true)
	defer s.queueSpec(ip, false)
	if !s.specSlot(page) {
		return
	}
	defer s.releaseSpec()
	// The speculative card walk rides the interruptible transport whenever the
	// hub has one: this is insurance behind a press, not a page riding its own
	// paint, and a cut here costs only the head start.
	var werr error
	if s.warm != nil {
		_, werr = s.sdListReadWarm(ip)
	} else {
		_, werr = s.sdListRead(ip)
	}
	if werr != nil {
		return // the board will try again at the next edge or press; a dead card is not news
	}
	list := s.cachedList(ip)
	if list == nil || !list.Mounted {
		return
	}
	s.kickWarm(ip, list)
}

// setWarmBoost files the face under the operator's hand. An empty face and slug
// clears the hint: a face with no warm work of its own (the Wi-Fi face, the
// files face, the Orb face) is the operator saying "nothing in particular",
// which is exactly the default likelihood order and must not inherit the last
// face's bend.
// cardFace names the faces whose operator means to read the card. A press on
// any other face is a press AWAY from card work: the pass pauses for it, and
// nothing on that face may start a card read-ahead.
func cardFace(face string) bool {
	return face == "themes" || face == "apps" || face == "edit"
}

func (s *Server) setWarmBoost(ip, face, slug string) {
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	if !cardFace(face) {
		delete(s.warmBoost, ip)
		return
	}
	if face == "" && slug == "" {
		delete(s.warmBoost, ip)
		return
	}
	if s.warmBoost == nil {
		s.warmBoost = map[string]warmHint{}
	}
	s.warmBoost[ip] = warmHint{face: face, slug: slug, at: time.Now()}
}

func (s *Server) warmBoostFor(ip string) warmHint {
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	h := s.warmBoost[ip]
	if h.face == "" || time.Since(h.at) > warmHintLive {
		return warmHint{}
	}
	return h
}

// warmStamp is one demand stamp: when the page last read this orb, and of what
// kind. 'p' is a plane relay — a queue fetching rows one dial at a time, owed
// the wide window because the queue itself may pause a slow card between dials.
// 'r' is any other deliberate page read — a face's own paint, the zone table, a
// wifi scan, a file list — a burst, owed only the short window. The pass's two
// rules read the kinds apart: a plane queue takes the plane phase off the
// pass's list for its whole window; any demand parks the whole pass until it
// ages out. A status poll is neither: proxyOrb exempts it, because the orbStream
// ticks every ten seconds and a pass parked by the heartbeat that discovers a
// page is open would never run for the page at all.
type warmStamp struct {
	at   time.Time
	kind byte // 'p' = plane relay, 'r' = other deliberate page read
}

// recordPagePreview stamps the moment the PAGE, not the pass, asked for a plane.
// handleThemePreview is the only caller, and the queue behind it fetches rows in
// the order the operator sees them — the one order that outranks the pass's guess.
func (s *Server) recordPagePreview(ip string) {
	s.previewMu.Lock()
	if s.pagePvAt == nil {
		s.pagePvAt = map[string]warmStamp{}
	}
	s.pagePvAt[ip] = warmStamp{at: time.Now(), kind: 'p'}
	s.previewMu.Unlock()
}

// pageFetching answers the plane question only: is a live page queue fetching
// the planes itself. The answer takes the pass's whole plane phase off its list
// — the queue is caching every 200 through the same single flight, so a second
// fetcher is contention with a head start.
func (s *Server) pageFetching(ip string) bool {
	s.previewMu.Lock()
	defer s.previewMu.Unlock()
	t, ok := s.pagePvAt[ip]
	return ok && t.kind == 'p' && time.Since(t.at) < pagePvYield
}

// pageDemandLive answers the park question: did the page, of any kind, read this
// orb recently enough that starting a dial now would race the operator. The
// plane kind parks too — a streaming queue owns the lane for the length of its
// window, and the pass has regions to do that the queue is not touching.
func (s *Server) pageDemandLive(ip string) bool {
	s.previewMu.Lock()
	defer s.previewMu.Unlock()
	t, ok := s.pagePvAt[ip]
	if !ok {
		return false
	}
	if t.kind == 'p' {
		return time.Since(t.at) < pagePvYield
	}
	return time.Since(t.at) < demandStampLive
}

// demandLane is the page's claim on one orb's lane, said before the page dials.
// It does two things the gate cannot do for itself. It closes the socket of a
// background dial holding the lane — the pass's own work, interruptible by
// definition, its answer refetchable — and it stamps the demand, which is what
// makes the pass stand down from starting the NEXT dial. A control dial that
// finds the lane held by a page relay still gets its honest busy report at
// 1.5 s: that holder is somebody's answer, and waiting it out is what the page
// queue's own budgets are for. The order is the whole priority mechanism: the
// page that interrupts first never queues behind insurance, and the insurance
// resumes at the pass's next pick once the lane goes quiet.
func (s *Server) demandLane(ip string) {
	s.recordPageDemand(ip)
	if s.gate == nil {
		return
	}
	port := "80"
	if s.device != nil {
		port = s.device.Port()
	}
	if s.gate.Interrupt(net.JoinHostPort(ip, port)) {
		s.cardMu.Lock()
		if s.warmCutAt == nil {
			s.warmCutAt = map[string]time.Time{}
		}
		s.warmCutAt[ip] = time.Now()
		s.cardMu.Unlock()
	}
}

// warmAborted answers the pass's narrow question: did a page dial cut MY dial,
// as opposed to the card refusing to answer at all. It reads the cut stamp the
// demand set, so a strike is only forgiven when the hub itself pulled the plug;
// a dead card still trips the pass exactly as before. It is called right after
// the fetch returns, before any other dial can raise a newer cut stamp, and a
// false positive only defers one strike — the pass's budget is three.
func (s *Server) warmAborted(ip string, began time.Time) bool {
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	t, ok := s.warmCutAt[ip]
	return ok && t.After(began)
}

// recordPageDemand stamps the last deliberate page read at this address, under
// the same lock and window as the plane demand. The card walks (the proxy's
// config and status reads, the app ledger, the file manager) are not planes,
// and the pass must stand down from them too — the page waiting on a config
// answer does not care that what holds the lane is "only" a read-ahead.
func (s *Server) recordPageDemand(ip string) {
	s.previewMu.Lock()
	if s.pagePvAt == nil {
		s.pagePvAt = map[string]warmStamp{}
	}
	// A plane demand carries the wider window; a short read must not shrink the
	// shield a queue that is still streaming is owed.
	if st, ok := s.pagePvAt[ip]; ok && st.kind == 'p' && time.Since(st.at) < pagePvYield {
		s.previewMu.Unlock()
		return
	}
	s.pagePvAt[ip] = warmStamp{at: time.Now(), kind: 'r'}
	s.previewMu.Unlock()
}

// plainDemandLive is the park question for the burst kind. A plane demand does
// NOT park the whole pass: its rule is the loop's plane-phase drop, and a pass
// parked over a streaming queue would idle exactly the region reads only the
// pass is doing.
func (s *Server) plainDemandLive(ip string) bool {
	s.previewMu.Lock()
	defer s.previewMu.Unlock()
	t, ok := s.pagePvAt[ip]
	return ok && t.kind == 'r' && time.Since(t.at) < demandStampLive
}

// clearPageDemand drops the address's demand stamp. kickWarm is the only
// caller, and the reason it may: the press that kicks is the page asking for
// the warmed bytes, so the demand that parked the pass is satisfied by the
// kick itself. A queue that goes on fetching after the kick re-stamps the
// demand on its next dial, and the pass yields to it again just as honestly.
// A plane demand survives the clear: the kick does not cancel a queue that is
// still streaming, and the pass meets that queue at its next pick, where
// pageFetching takes the plane phase off the list exactly as a pressing
// operator expects.
func (s *Server) clearPageDemand(ip string) {
	s.previewMu.Lock()
	if t, ok := s.pagePvAt[ip]; ok && t.kind == 'p' && time.Since(t.at) < pagePvYield {
		s.previewMu.Unlock()
		return
	}
	delete(s.pagePvAt, ip)
	s.previewMu.Unlock()
}

// warmWaitQuiet holds the pass at its next pick while the page is reading this
// orb. It is a wait, not a cancellation: the queues survive, the hints re-read
// at the resume, and a page that goes quiet resumes the pass with its order
// intact. The pass keeps stamping its clock while parked so a hub that resumes
// it never mistakes the pause for the death warmLive bounds. warmParkMax is the
// out: an orb whose page traffic never stops does not get a goroutine parked on
// it forever, and the note says so.
func (s *Server) warmWaitQuiet(ip string, w *warmRun) bool {
	if !s.plainDemandLive(ip) {
		return true
	}
	s.warmStep(ip, w, func(w *warmRun) { w.note = "waiting: the page is reading this orb" })
	deadline := time.Now().Add(warmParkMax)
	for s.plainDemandLive(ip) {
		if s.warmStopped(ip, w) {
			return false
		}
		if time.Now().After(deadline) {
			s.warmStep(ip, w, func(w *warmRun) {
				w.phase, w.note = "failed", "read-ahead gave up waiting: the page kept this orb's lane past the patience bound"
			})
			return false
		}
		time.Sleep(warmParkTick)
		s.warmStep(ip, w, func(*warmRun) {})
	}
	return true
}

// specSlot takes one of the hub-wide speculative slots, waiting only for the
// door that was told to. See specMax for why the house is capped at two.
func (s *Server) specSlot(page bool) bool {
	deadline := time.Now().Add(specQueueWait)
	for {
		s.cardMu.Lock()
		if s.specRuns < specMax {
			s.specRuns++
			s.cardMu.Unlock()
			return true
		}
		s.cardMu.Unlock()
		if !page || time.Now().After(deadline) {
			return false
		}
		time.Sleep(250 * time.Millisecond)
	}
}

func (s *Server) releaseSpec() {
	s.cardMu.Lock()
	s.specRuns--
	s.cardMu.Unlock()
}

// queueSpec files that a speculative read is decided-but-not-dialing, so
// warm-status answers "listing" rather than the lie "idle" during the window
// between a press and the pass taking the floor.
func (s *Server) queueSpec(ip string, on bool) {
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	if s.warmQueued == nil {
		s.warmQueued = map[string]int{}
	}
	if on {
		s.warmQueued[ip]++
		return
	}
	if s.warmQueued[ip] <= 1 {
		delete(s.warmQueued, ip)
		return
	}
	s.warmQueued[ip]--
}

// warmStopped reports whether this pass is still the address's pass. A
// mutation, a rescan, or a newer pass retired it, and the card's answers have
// moved on from the ones this pass set out to fill.
func (s *Server) warmStopped(ip string, w *warmRun) bool {
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	cur := s.warmPass[ip]
	return cur != w || cur.seq != w.seq
}

func (s *Server) warmStep(ip string, w *warmRun, mut func(*warmRun)) {
	s.cardMu.Lock()
	if cur := s.warmPass[ip]; cur == w && cur.seq == w.seq {
		mut(w)
		w.at = time.Now()
	}
	s.cardMu.Unlock()
}

// warmPlaneOrder lists the rows the way the operator will reach for them: the
// active theme first — it is the row the glass is wearing and the row the next
// click touches — then the rest in the card's own order.
func warmPlaneOrder(list *orbnet.ThemeList) []orbnet.ThemeEntry {
	out := make([]orbnet.ThemeEntry, 0, len(list.Themes))
	for _, t := range list.Themes {
		if t.Active {
			out = append(out, t)
		}
	}
	for _, t := range list.Themes {
		if !t.Active {
			out = append(out, t)
		}
	}
	return out
}

// warmOrb is the scheduler's worker: one ordered list of the reads this card
// owes, and a fresh decision before every pick. A live page queue takes the
// planes off the pass's list — the queue is fetching those very bytes and
// caching them through the same door, so a second fetcher is contention with a
// head start. The face under the operator's hand — the apps face, an open
// editor pane — pulls regions in front of the planes still queued. Everything
// else runs the likelihood order: active plane, plain planes, regions.
//
// Each answer lands in the cache the page reads, so a warmed click pays no dial
// at all. A read that fails is not retried here; the row's own fetch, or its
// retry chip, pays the one real re-dial. Three consecutive plane transport
// failures end the pass outright: a cold card that cannot answer one plane will
// not answer eight, and a pass that holds the lane through every one of them is
// the read-ahead meant to spare the card becoming the reason it is unreachable.
// A quiet 404 is not a strike — a row with no plane is an answer, not a stall.
func (s *Server) warmOrb(ip string, w *warmRun, list *orbnet.ThemeList) {
	planes := warmPlaneOrder(list)
	var regions []string
	regionNote := ""
	regionsLoaded := false
	total := len(planes)
	s.warmStep(ip, w, func(w *warmRun) { w.total, w.phase, w.done = total, "planes", 0 })

	// loadRegions walks the ledger once and files the editable slugs, with the
	// open pane's app at the front if a pane is open. The enumeration is cheap
	// beside a card walk, so the answer is held for the rest of the pass however
	// it got loaded.
	loadRegions := func() {
		if regionsLoaded {
			return
		}
		regionsLoaded = true
		_, apps, err := s.cardAppsReadWarm(ip)
		if err != nil {
			regionNote = "regions skipped: " + err.Error()
			return
		}
		regions = editableSlugs(apps)
		if b := s.warmBoostFor(ip); b.slug != "" {
			for i, sl := range regions {
				if sl == b.slug && i > 0 {
					regions = append([]string{sl}, append(regions[:i:i], regions[i+1:]...)...)
					break
				}
			}
		}
		s.warmStep(ip, w, func(w *warmRun) { w.total = total + len(regions) })
	}

	strikes := 0
	for {
		if s.warmStopped(ip, w) {
			return
		}
		// Every pick starts by giving the lane back. A pass that ignored a
		// live page would be insurance that delays the thing it insures, and
		// the yield costs one loop turn — the queues survive, the hints
		// re-read, and the resume keeps the order the scheduler had built.
		if !s.warmWaitQuiet(ip, w) {
			return
		}
		if len(planes) > 0 && s.pageFetching(ip) {
			planes = nil
			s.warmStep(ip, w, func(w *warmRun) { w.note = "planes left to the page's own queue" })
		}
		b := s.warmBoostFor(ip)
		wantRegion := b.face == "apps" || (b.face == "edit" && b.slug != "")
		if wantRegion || len(planes) == 0 {
			loadRegions()
		}
		if len(planes) == 0 && len(regions) == 0 {
			break // both queues drained, or the enumeration found nothing to read
		}
		// A region read runs when the face asks for it ahead of planes, or when
		// the plane queue is simply done. Both halves guard the pick below:
		// reaching planes[0] with an empty queue panicked the whole process —
		// a goroutine panic is not recoverable per-request — and the operator's
		// next fetch hit a connection refused hub.
		if len(regions) > 0 && (wantRegion || len(planes) == 0) {
			slug := regions[0]
			regions = regions[1:]
			s.warmStep(ip, w, func(w *warmRun) { w.phase = "regions" })
			began := time.Now()
			s.prefetchEditorRegionWarm(ip, slug)
			if s.warmAborted(ip, began) {
				// The hub cut this dial for a page read: not a card's silence,
				// no strike, and the loop's head waits for the quiet.
				continue
			}
			s.warmStep(ip, w, func(w *warmRun) { w.done++ })
			continue
		}
		if len(planes) == 0 {
			break // no hint, no regions to run: this pass is finished
		}
		t := planes[0]
		planes = planes[1:]
		s.warmStep(ip, w, func(w *warmRun) { w.phase = "planes" })
		if !s.previewFresh(ip, t.ID) {
			began := time.Now()
			if _, err := s.fetchPreviewPlaneWarm(ip, t.ID); err != nil {
				if s.warmAborted(ip, began) {
					s.warmStep(ip, w, func(w *warmRun) { w.done++ })
					continue
				}
				strikes++
				if strikes >= 3 {
					s.warmStep(ip, w, func(w *warmRun) {
						w.phase, w.note = "failed", "read-ahead stopped: the card did not answer the plane reads (the page's own retries still work)"
					})
					return
				}
			} else {
				strikes = 0
			}
		}
		s.warmStep(ip, w, func(w *warmRun) { w.done++ })
	}
	if s.warmStopped(ip, w) {
		return
	}
	s.warmStep(ip, w, func(w *warmRun) {
		if regionNote != "" {
			w.note = regionNote
		}
		if w.phase != "done" && w.phase != "failed" {
			w.phase = "done"
		}
	})
}

// editableSlugs names each app of the enumeration a region fetch is worth: an
// editor opens for it and the device reports the region itself, so the bytes
// live on the card rather than in the hub's vendor tree.
func editableSlugs(apps *orbnet.ThemeApps) []string {
	out := make([]string, 0, len(apps.Apps))
	seen := map[string]bool{}
	for _, a := range apps.Apps {
		if a.HasEditor && a.EditorURL != "" && !seen[a.Slug] {
			seen[a.Slug] = true
			out = append(out, a.Slug)
		}
	}
	return out
}

// handleThemeWarm files the face press and starts the read-ahead pass for the
// address named in the body, answering at once. The page calls it on every face
// press whether or not the draw was cold, and the call is always safe: an
// address with a pass running keeps its pass — the hint lands at its next pick —
// and a card with everything fresh dials nothing.
func (s *Server) handleThemeWarm(w2 http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w2, r, false)
	if !ok {
		return
	}
	// The hint lands before the kick decision, so a press that arrives mid-pass
	// reorders that pass instead of bouncing off it as "already running".
	s.setWarmBoost(t.IP, t.Face, t.Slug)
	if t.Face != "" && !cardFace(t.Face) {
		// The operator is on the Orb face, the Wi-Fi face, the files face:
		// pages the card owes nothing to. A read-ahead started here would put
		// a card walk in front of the answer the pressed face is waiting on —
		// the inversion this route exists to prevent, and the exact shape of
		// the zone-table failure this pass was written against. The boost is
		// cleared, the demand is stamped, and the pass stands down.
		s.recordPageDemand(t.IP)
		s.jsonOK(w2, s.warmView(t.IP))
		return
	}
	list := s.cachedList(t.IP)
	if list == nil {
		// Nothing remembered: the cold read runs BEHIND the reply, not inside
		// it. The press that found the page cold is exactly the press that must
		// not wait thirty seconds on a card walk to be acknowledged — the route
		// used to take that read inline, which is a second cold paint bolted
		// onto the first. warm-spec owns the read, the memory, and the pass.
		go s.warmSpeculative(t.IP, true)
		s.jsonOK(w2, s.warmView(t.IP))
		return
	}
	s.kickWarm(t.IP, list)
	s.jsonOK(w2, s.warmView(t.IP))
}

// handleThemeWarmStatus answers GET /api/theme/warm-status?ip= with the pass's
// progress, for the page to word its quiet line. It dials nothing, ever.
func (s *Server) handleThemeWarmStatus(w http.ResponseWriter, r *http.Request) {
	ip := strings.TrimSpace(r.URL.Query().Get("ip"))
	if _, ok := s.checkedTarget(w, ip, "", false); !ok {
		return
	}
	s.jsonOK(w, s.warmView(ip))
}

func (s *Server) warmView(ip string) map[string]any {
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	w := s.warmPass[ip]
	if w == nil {
		// A speculative read decided-but-not-dialing is a pass being born, and
		// "idle" would be the page watching for the line to appear and being
		// told nothing is happening while the slot is next-in-line.
		if s.warmQueued[ip] > 0 {
			return map[string]any{"phase": "listing"}
		}
		return map[string]any{"phase": "idle"}
	}
	return map[string]any{"phase": w.phase, "done": w.done, "total": w.total, "note": w.note}
}

// cachedList returns the remembered rows with no dial, for a kick that must
// not block. Nil means the memory has nothing and the caller decides what a
// cold read costs.
func (s *Server) cachedList(ip string) *orbnet.ThemeList {
	s.cardMu.Lock()
	defer s.cardMu.Unlock()
	return s.cardReads[ip].list
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
