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

// holdCommitApp records which app a refusal handed its commit token to: the app the refused
// container's own header names, which is the app the orb files it under. The editor listener
// reads the record to refuse one app's editor a commit of another app's staged copy
// (H3-hub-1), so it is held under the same lock and the same bound as the bytes it accompanies
// — a refusal nobody commits is dropped rather than held for the life of the hub. An empty
// token or an app-less container records nothing: there is no scope to answer with.
func (s *Server) holdCommitApp(token, app string) {
	if token == "" || app == "" {
		return
	}
	s.sendMu.Lock()
	defer s.sendMu.Unlock()
	if s.commitApps == nil {
		s.commitApps = map[string]string{}
	}
	if len(s.commitApps) >= shelfPendingMax {
		for k := range s.commitApps {
			delete(s.commitApps, k)
			break
		}
	}
	s.commitApps[token] = app
}

// commitAppOf is whose app a held commit token was handed for, "" when the hub holds no
// record of it — a token it never issued, or one already spent.
func (s *Server) commitAppOf(token string) string {
	s.sendMu.Lock()
	defer s.sendMu.Unlock()
	return s.commitApps[token]
}

// dropCommitApp forgets a token that has been spent, by either port's commit. The device's
// token is one-shot, so after a commit that landed it names nothing the hub may still scope:
// the next ask for it is an unknown token, and on the editor port that is a refusal.
func (s *Server) dropCommitApp(token string) {
	s.sendMu.Lock()
	defer s.sendMu.Unlock()
	delete(s.commitApps, token)
}

func (s *Server) keepOnShelf(body []byte) {
	if dest, err := s.library().keepTheme(body, true); err != nil {
		log.Printf("theme shelf: not kept: %v", err)
	} else {
		log.Printf("theme shelf: kept %s", dest)
	}
}

// keepTheme files one theme on the shelf by (app, name) and returns the path written. A
// shelf file already holding that app and name, in any format, is the one overwritten, so
// a rebake replaces its stale copy. K10: the theme carries its design, so the app's draft
// under that name is folded away once the theme is on the shelf.
func (l *Library) keepTheme(body []byte, fold bool) (string, error) {
	c, err := orbnet.ContainerName(body)
	if err != nil {
		return "", err
	}
	app := "clock"
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
	shelfThemes(dir, func(p string, _ os.FileInfo, t *orbnet.ShelfThemeInfo) {
		if dest == "" && shelfApp(t) == app && t.Name == c.Name {
			dest = p
		}
	})
	if dest == "" {
		dest = filepath.Join(dir, app+"-"+strings.TrimSuffix(designFile(c.Name), ".orbdesign")+".orbtheme")
	}
	if err := writeFileAtomic(dest, body); err != nil {
		return "", err
	}
	if dd := l.designsDir(app); fold && dd != "" {
		if err := os.Remove(filepath.Join(dd, designFile(c.Name))); err == nil {
			log.Printf("theme shelf: %q's draft folded into its theme", c.Name)
		}
	}
	return dest, nil
}
