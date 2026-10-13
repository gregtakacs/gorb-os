package main

import (
	"encoding/json"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The Theme card's server side. Every call here is the hub dialing ONE address the
// operator named or the USB console reported, through the orbnet theme module, which is
// the single place the device theme API is spoken.
//
// FORMAT 2 decides who names a theme, and it is not the caller of these endpoints.
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

// handleThemeInstall queues an install on the orb. The device answers when the job is
// queued and reboots itself once the copy verifies, so the reply is not a claim that
// the glass changed; the caller re-reads the list and the orb's status for that.
func (s *Server) handleThemeInstall(w http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w, r, true)
	if !ok {
		return
	}
	raw, err := s.device.ThemeInstall(t.IP, t.ID)
	s.themeReply(w, raw, err)
}

// handleThemeDefault restores the built-in fallback face.
func (s *Server) handleThemeDefault(w http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w, r, false)
	if !ok {
		return
	}
	raw, err := s.device.ThemeDefault(t.IP)
	s.themeReply(w, raw, err)
}

// handleThemeDelete removes a theme from the card. It is the one theme call that is
// not a queued job, so its body is the outcome and passes through as it is.
func (s *Server) handleThemeDelete(w http.ResponseWriter, r *http.Request) {
	t, ok := s.themeTarget(w, r, true)
	if !ok {
		return
	}
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
	c, err := orbnet.ContainerName(body)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	overwrite := r.URL.Query().Get("overwrite") == "1"
	raw, err := s.device.ThemeUpload(t.IP, body, overwrite)
	if de, ok := err.(*orbnet.DeviceError); ok && de.OverwriteConflict() {
		s.jsonErr(w, de.Error()+" — send it again with overwrite to replace it")
		return
	}
	if err != nil {
		s.themeReply(w, nil, err)
		return
	}
	// The device's own reply, plus the identity the hub read off the file: the card
	// cannot name what it pushed without this, and the id is what the row will arrive
	// with after the re-read.
	var payload map[string]any
	if uerr := json.Unmarshal(raw, &payload); uerr != nil {
		s.jsonOK(w, map[string]any{"deviceText": string(raw), "name": c.Name, "id": c.ID})
		return
	}
	payload["name"] = c.Name
	payload["id"] = c.ID
	s.jsonOK(w, payload)
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
// request, from list/default/rescan/upload, where the orb alone is.
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
