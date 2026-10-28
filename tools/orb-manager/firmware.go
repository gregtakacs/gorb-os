package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/firmware"
)

// firmwareBundle returns the firmware flashes right now. Never nil once startup
// has run: a hub with no readable built-in bundle refuses to start rather than
// serve an Onboard card whose Flash button cannot work.
func (s *Server) firmwareBundle() *firmware.Bundle {
	s.fmu.RLock()
	defer s.fmu.RUnlock()
	return s.fw
}

// adoptFirmware installs b as the firmware everything flashes from, and records
// the swap so a log line always ties an orb to the bytes it got.
func (s *Server) adoptFirmware(b *firmware.Bundle) {
	s.fmu.Lock()
	s.fw = b
	s.fmu.Unlock()
	log.Printf("orb-manager: firmware %s (%s, %d image(s), sha256 %s)", b.Source, b.Version, len(b.Images), short(b.SHA256))
}

// loadFirmwareFile reads a firmware the user picked, in either shape Load
// accepts. The bytes arrive from an upload, so nothing here walks a directory
// tree looking for a firmware: one named file is the whole contract, and every
// shape error surfaces as a message the UI can show instead of a stat failure
// deep in the flasher.
func loadFirmwareFile(name string, raw []byte, o firmware.Options) (*firmware.Bundle, error) {
	if len(raw) == 0 {
		return nil, fmt.Errorf("%s is empty", name)
	}
	b, err := firmware.LoadWithOptions(raw, name, o)
	if err != nil {
		return nil, err
	}
	if err := b.Ready(); err != nil {
		return nil, err
	}
	return b, nil
}

// startFirmware loads the built-in bundle, or the one -firmware names, and makes
// it active. Called before bind so a broken bundle is a startup failure.
func (s *Server) startFirmware(path string, allowForeign bool) error {
	if path != "" {
		raw, err := os.ReadFile(path)
		if err != nil {
			return fmt.Errorf("read firmware file %s: %w", path, err)
		}
		// A file named on the command line is as deliberate a choice as the second
		// click on a refusal, so -allow-foreign opens the same gate here.
		b, err := loadFirmwareFile(nameOf(path), raw, firmware.Options{AllowForeign: allowForeign})
		if err != nil {
			return err
		}
		s.adoptFirmware(b)
		return nil
	}
	b, err := firmware.BuiltIn()
	if err != nil {
		return fmt.Errorf("this build of the hub has no firmware compiled in: %w", err)
	}
	s.adoptFirmware(b)
	return nil
}

func nameOf(path string) string {
	if i := lastSep(path); i >= 0 {
		return path[i+1:]
	}
	return path
}

func lastSep(s string) int {
	for i := len(s) - 1; i >= 0; i-- {
		if s[i] == '/' || s[i] == '\\' {
			return i
		}
	}
	return -1
}

func short(s string) string {
	if len(s) > 12 {
		return s[:12]
	}
	return s
}

// firmwareName is the one-line identity used in the startup log.
func (s *Server) firmwareName() string {
	b := s.firmwareBundle()
	if b == nil {
		return "none"
	}
	if b.Version != "" {
		return fmt.Sprintf("%s %s, %d image(s)", b.Project, b.Version, len(b.Images))
	}
	return fmt.Sprintf("%s, %d image(s)", b.Source, len(b.Images))
}

// describeFirmware feeds the Onboard "what will I be flashing" panel. Its keys
// are a UI contract, pinned by TestBuildPanelFieldsMatchBetweenClientAndHandler:
// app.js reads present, images, source, version, project and sha256 here, and
// takes .length/.map on images, so that list must be a (possibly empty) array and
// never null. semver travels beside version as structure for any reader that wants
// the triple unparsed; app.js still renders the raw stamp, so adding it cannot
// change what the panel shows.
func (s *Server) describeFirmware(*http.Request) (any, error) {
	b := s.firmwareBundle()
	if b == nil {
		return map[string]any{"present": false, "merged": false, "foreign": false, "images": []any{}, "semver": semverPayload(""),
			"source": "", "version": "", "project": "", "sha256": "", "built": "", "idf": ""}, nil
	}
	return firmwareSummaryPayload(b), nil
}

// handleFirmwareUpload takes a bundle the user chose and, if it parses and
// carries the images the flash needs, makes it the active firmware. A bad file
// is refused with the reason and leaves the built-in firmware in place: picking a
// wrong file must never leave the hub unable to flash.
//
// probe=1 answers the same question without adopting the answer. The flash modal
// reads a file's project and shape the moment it is picked, so the operator can see
// what they are about to commit to, and a browse dialog that silently changed the
// hub's firmware would make the modal's Flash button a formality rather than a
// decision.
func (s *Server) handleFirmwareUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, firmware.MaxBytes())
	if err := r.ParseMultipartForm(firmware.MaxBytes()); err != nil {
		s.jsonErr(w, "unreadable firmware file: "+err.Error())
		return
	}
	file, hdr, err := r.FormFile("firmware")
	if err != nil {
		s.jsonErr(w, "no firmware file sent")
		return
	}
	defer file.Close()
	raw := make([]byte, hdr.Size)
	if _, err := io.ReadFull(file, raw); err != nil {
		s.jsonErr(w, "short read of "+hdr.Filename+": "+err.Error())
		return
	}
	allowForeign := r.FormValue("allow_foreign") == "1"
	b, err := loadFirmwareFile(hdr.Filename, raw, firmware.Options{AllowForeign: allowForeign})
	if err != nil {
		if !allowForeign {
			if p := foreignProjectName(err); p != "" {
				// The one refusal with a second send behind it. Carrying the name as
				// a field, and the fact that it is overridable, lets the card
				// offer the button and quote the project back rather than parse the
				// sentence it is displaying.
				s.jsonErrFields(w, err.Error(), map[string]any{
					"foreign_project": p, "overridable": true,
				})
				return
			}
		}
		s.jsonErr(w, err.Error())
		return
	}
	// A probe reports and does not adopt. Everything the modal needs to label its
	// choice — project, version, shape, the foreign flag — is already in this
	// payload, so one handler answers both questions and the two answers cannot
	// disagree about what the file is.
	if r.FormValue("probe") == "1" {
		s.jsonOK(w, firmwareSummaryPayload(b))
		return
	}
	s.adoptFirmware(b)
	s.jsonOK(w, firmwareSummaryPayload(b))
}

// foreignProjectName is the project a refused file did declare, when the only
// thing in the way was that it is not gorb-os. The upload handler offers a second
// send on this alone, so a wrong chip, an oversized blob or a file with no
// descriptor at all still gets one flat refusal and no button to press.
func foreignProjectName(err error) string {
	var ng *firmware.NotGorbOS
	if errors.As(err, &ng) {
		return ng.Project
	}
	return ""
}

// handleFirmwareReset returns the hub to the firmware it shipped with, so a
// browsed-in downgrade is always reversible without restarting anything.
func (s *Server) handleFirmwareReset(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	b, err := firmware.BuiltIn()
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.adoptFirmware(b)
	s.jsonOK(w, firmwareSummaryPayload(b))
}

func firmwareSummaryPayload(b *firmware.Bundle) map[string]any {
	imgs := make([]map[string]any, 0, len(b.Images))
	for _, im := range b.Images {
		imgs = append(imgs, map[string]any{
			"name": im.Name, "offset": fmt.Sprintf("0x%X", im.Offset), "bytes": len(im.Data),
		})
	}
	return map[string]any{
		"present": true, "images": imgs,
		"source": b.Source, "version": b.Version, "project": b.Project,
		"idf": b.IDFVersion, "built": b.Built, "sha256": b.SHA256,
		"semver": semverPayload(b.Version),
		// merged is the shape flag the panel needs: a whole-flash image replaces
		// the bootloader and every partition in one write, and it leaves this hub
		// with no app slot to push over WiFi.
		"merged": b.Merged,
		// foreign is the decision flag: this hub is flashing somebody else's
		// firmware because the operator said so. The panel says it back, because a
		// hub quietly running a vendor demo image is a hub that will confuse
		// whoever opens it next.
		"foreign": b.Foreign,
	}
}

// semverPayload is the version as structure, so the UI never parses a stamp. An
// unversioned build is reported as legacy rather than as v0.0.0, because a fake zero
// would render as the oldest release rather than as the unknown it is.
func semverPayload(raw string) map[string]any {
	v, ok := firmware.ParseVersion(raw)
	if !ok {
		return map[string]any{"legacy": true, "raw": raw, "declared": "",
			"major": 0, "minor": 0, "patch": 0, "pre": "", "commit": "", "dirty": false}
	}
	return map[string]any{"legacy": false, "raw": v.Raw, "declared": v.Declared(),
		"major": v.Major, "minor": v.Minor, "patch": v.Patch, "pre": v.Pre,
		"commit": v.Commit, "dirty": v.Dirty}
}

func (s *Server) jsonOK(w http.ResponseWriter, payload any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "data": payload})
}

// jsonOKRaw answers with device bytes already shaped as the payload, so a
// remembered read replays without a re-encode. The envelope is jsonOK's:
// encoding/json writes map keys sorted, so data comes first and ok second,
// and the replayed bytes carry the same order the live answer had.
func (s *Server) jsonOKRaw(w http.ResponseWriter, raw json.RawMessage) {
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write([]byte(`{"data":`))
	_, _ = w.Write(raw)
	_, _ = w.Write([]byte(`,"ok":true}`))
}

func (s *Server) jsonErr(w http.ResponseWriter, msg string) {
	s.jsonErrFields(w, msg, nil)
}

// jsonErrFields is jsonErr with named additions, for the refusals where the hub
// knows what the operator's next action is and the card must not guess at it from
// the wording of a sentence written for a person to read.
func (s *Server) jsonErrFields(w http.ResponseWriter, msg string, extra map[string]any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusBadRequest)
	out := map[string]any{"ok": false, "error": msg}
	for k, v := range extra {
		out[k] = v
	}
	_ = json.NewEncoder(w).Encode(out)
}
