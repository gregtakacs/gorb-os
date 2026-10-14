package main

import (
	"encoding/json"
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

// loadFirmwareFile reads a bundle the user picked. The bytes arrive from an
// upload, so nothing here walks a directory tree looking for a firmware: one
// named file is the whole contract, and every shape error surfaces as a message
// the UI can show instead of a stat failure deep in the flasher.
func loadFirmwareFile(name string, raw []byte) (*firmware.Bundle, error) {
	if len(raw) == 0 {
		return nil, fmt.Errorf("%s is empty", name)
	}
	b, err := firmware.Load(raw, name)
	if err != nil {
		return nil, err
	}
	if _, err := b.AppPlan(); err != nil {
		return nil, err
	}
	return b, nil
}

// startFirmware loads the built-in bundle, or the one -firmware names, and makes
// it active. Called before bind so a broken bundle is a startup failure.
func (s *Server) startFirmware(path string) error {
	if path != "" {
		raw, err := os.ReadFile(path)
		if err != nil {
			return fmt.Errorf("read firmware file %s: %w", path, err)
		}
		b, err := loadFirmwareFile(nameOf(path), raw)
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
		return map[string]any{"present": false, "images": []any{}, "semver": semverPayload(""),
			"source": "", "version": "", "project": "", "sha256": "", "built": "", "idf": ""}, nil
	}
	return firmwareSummaryPayload(b), nil
}

// handleFirmwareUpload takes a bundle the user chose and, if it parses and
// carries the images the flash needs, makes it the active firmware. A bad file
// is refused with the reason and leaves the built-in firmware in place: picking a
// wrong file must never leave the hub unable to flash.
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
	b, err := loadFirmwareFile(hdr.Filename, raw)
	if err != nil {
		s.jsonErr(w, err.Error())
		return
	}
	s.adoptFirmware(b)
	s.jsonOK(w, firmwareSummaryPayload(b))
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

func (s *Server) jsonErr(w http.ResponseWriter, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusBadRequest)
	_ = json.NewEncoder(w).Encode(map[string]any{"ok": false, "error": msg})
}
