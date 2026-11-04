package main

// theme-editor-kit-plan K9: the shelf keeps what the hub stored.
//
// A theme this hub lands on an orb (an editor's Send, the Themes face's Import) is
// written to the theme library too, so the Themes tab and the Edit door can show it off
// the hub's disk. The shelf is filed by app and baked name, the same identity the card
// files by: a theme under a name the shelf already holds for that app replaces that
// file, whatever the file is called, so the shelf holds one copy per name rather than
// one per bake. A failure to keep is logged and never fails the send: the orb has the
// theme either way.

import (
	"log"
	"os"
	"path/filepath"
	"strings"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbnet"
)

// shelfPendingMax bounds the held bytes: a refusal nobody commits is dropped once
// this many newer ones arrive, rather than held for the life of the hub.
const shelfPendingMax = 4

func (s *Server) holdForShelf(token string, body []byte) {
	if token == "" {
		return
	}
	s.sendMu.Lock()
	defer s.sendMu.Unlock()
	if s.shelfPending == nil {
		s.shelfPending = map[string][]byte{}
	}
	if len(s.shelfPending) >= shelfPendingMax {
		for k := range s.shelfPending {
			delete(s.shelfPending, k)
			break
		}
	}
	s.shelfPending[token] = body
}

func (s *Server) takeForShelf(token string) []byte {
	s.sendMu.Lock()
	defer s.sendMu.Unlock()
	b := s.shelfPending[token]
	delete(s.shelfPending, token)
	return b
}

func (s *Server) keepOnShelf(body []byte) {
	if dest, err := s.library().keepTheme(body); err != nil {
		log.Printf("theme shelf: not kept: %v", err)
	} else {
		log.Printf("theme shelf: kept %s", dest)
	}
}

// keepTheme files one theme on the shelf by (app, name) and returns the path written.
func (l *Library) keepTheme(body []byte) (string, error) {
	c, err := orbnet.ContainerName(body)
	if err != nil {
		return "", err
	}
	app := ""
	if len(c.Sections) > 0 {
		app = c.Sections[0]
	}
	dir := l.writableThemesDir()
	if dir == "" {
		return "", os.ErrNotExist
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	dest := ""
	if entries, err := os.ReadDir(dir); err == nil {
		for _, e := range entries {
			if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".orbtheme") {
				continue
			}
			have, err := os.ReadFile(filepath.Join(dir, e.Name()))
			if err != nil {
				continue
			}
			hc, err := orbnet.ContainerName(have)
			if err != nil {
				continue
			}
			happ := ""
			if len(hc.Sections) > 0 {
				happ = hc.Sections[0]
			}
			if happ == app && hc.Name == c.Name {
				dest = filepath.Join(dir, e.Name())
				break
			}
		}
	}
	if dest == "" {
		stem := strings.TrimSuffix(designFile(c.Name), ".orbdesign")
		if app != "" {
			stem = app + "-" + stem
		}
		dest = filepath.Join(dir, stem+".orbtheme")
	}
	return dest, writeFileAtomic(dest, body)
}
