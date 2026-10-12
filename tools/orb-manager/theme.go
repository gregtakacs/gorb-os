package main

import (
	"encoding/json"
	"io"
	"net"
	"net/http"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// The Theme card's server side. Every call here is the hub dialing ONE address the
// operator named or the USB console reported, through the orbnet theme module — the
// single place docs/theme-identity-plan.md will attach an orb `id` to later.
//
// These are deliberately NOT folded into /orb/: a theme action needs the address gate,
// the name gate, and one place to add the id, and each is clearer as a named endpoint
// than as another arm of a generic forwarder. The proxy keeps serving the read-only
// status and config calls the Manage card already makes.

// themeTarget is the request every theme call takes: the orb to dial, and for the two
// mutating-by-name calls the theme to act on. The address is checked against the
// learned set exactly as the proxy checks it, because a theme call reaches the device
// as hard as a forward does — harder, since install and delete write to the card.
type themeTarget struct {
	IP   string `json:"ip"`
	Name string `json:"name,omitempty"`
}

// handleThemeList returns the card's rows plus the state needed to word them.
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
	raw, err := s.device.ThemeInstall(t.IP, t.Name)
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
	raw, err := s.device.ThemeDelete(t.IP, t.Name)
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

// handleThemeUpload takes a theme.bin the operator picked and pushes it to the orb
// under the name they gave. The file never touches the hub's disk: it is read under a
// cap, the name is checked against the firmware's own rule, and the bytes go straight
// on. Refusing an over-large body here rather than streaming it means the device never
// half-writes a theme the operator cannot see.
func (s *Server) handleThemeUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	// The body is the file, so the target arrives as query parameters: a multipart
	// parse would only re-buffer bytes that are going straight back out.
	ip := strings.TrimSpace(r.URL.Query().Get("ip"))
	name := strings.TrimSpace(r.URL.Query().Get("name"))
	t, ok := s.checkedTarget(w, ip, name, true)
	if !ok {
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, int64(orbnet.MaxThemeUpload)+1))
	if err != nil {
		s.jsonErr(w, "reading the upload: "+err.Error())
		return
	}
	raw, err := s.device.ThemeUpload(t.IP, t.Name, body)
	s.themeReply(w, raw, err)
}

// themeTarget decodes and checks a JSON theme request.
func (s *Server) themeTarget(w http.ResponseWriter, r *http.Request, needName bool) (themeTarget, bool) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return themeTarget{}, false
	}
	var t themeTarget
	if err := json.NewDecoder(r.Body).Decode(&t); err != nil {
		s.jsonErr(w, "bad request: "+err.Error())
		return themeTarget{}, false
	}
	if !s.checkedTargetInto(w, &t, needName) {
		return themeTarget{}, false
	}
	return t, true
}

// checkedTarget applies the gates every device-touching call shares, for callers that
// have the fields already (the upload, whose body is the file). needName distinguishes
// install/delete/upload, where the name is the request, from list/default/rescan.
func (s *Server) checkedTarget(w http.ResponseWriter, ip, name string, needName bool) (themeTarget, bool) {
	t := themeTarget{IP: ip, Name: name}
	if !s.checkedTargetInto(w, &t, needName) {
		return themeTarget{}, false
	}
	return t, true
}

// checkedTargetInto is the whole gate, in one place, so a theme call cannot be added
// that forgets one of its three parts: an address must exist, must be an IP literal,
// and must be one this hub learned or was handed. A hostname is refused rather than
// resolved: the hub dials devices it has seen, not names a page invented.
func (s *Server) checkedTargetInto(w http.ResponseWriter, t *themeTarget, needName bool) bool {
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
	t.Name = strings.TrimSpace(t.Name)
	if needName && t.Name == "" {
		s.jsonErr(w, "no theme named")
		return false
	}
	if t.Name != "" {
		if err := orbnet.CheckThemeName(t.Name); err != nil {
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
