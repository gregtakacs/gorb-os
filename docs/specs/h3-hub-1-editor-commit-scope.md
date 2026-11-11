# Scope the editor listener's theme commit and upload-status to the editor's own app

## Goal

Security fix from the 2026-11-11 lifecycle review, slice H3's first hub step (gorb-os
`tools/orb-manager`). On S2's editor listener (`routesEditorListener` in `editororigin.go`) every
library, design and send route is wrapped in a per-app scope check (`editorScopeApp`,
`editorScopeContainer`, …) except two: `/api/theme/commit` and `/api/theme/upload-status`. Today an
editor token for app A can spend a commit token that app B's send was handed, and can read the
progress of app B's send to any orb. Wrap both so an editor can only commit, and only read the
status of, a send of a theme of its own app. The control port (`main.go`'s routes) is unchanged.

## Read first

- `tools/orb-manager/editororigin.go`: `routesEditorListener`, the S2-2 scope section
  (`editorScopeOf`, `editorScopeOther`, `editorScopeNoApp`, `editorScopeApp`, `editorBodyCapped`,
  `editorScopeContainer`) — the pattern to imitate exactly.
- `tools/orb-manager/theme.go`: `handleThemeUpload` (around the `OverwriteConflict` branch that
  calls `holdForShelf(de.CommitToken(), body)` and answers `"commit": de.CommitToken()`),
  `streamThemeUpload` (find whether it also hands out a commit token), `handleThemeCommit`,
  `handleThemeSendStatus`, `themeSend`, `setSend`, `sendSnapshot`.
- `tools/orb-manager/shelfkeep.go`: `holdForShelf`, `takeForShelf`, `shelfPendingMax`.
- `tools/orb-manager/editororigin_test.go`: `editorAPICall`, `TestEditorAPIScopeIsItsApp` (how a
  scope refusal is asserted), and how other tests build a Server (`newHub()`), a fake device and a
  theme container for a given app.

## Contract

1. **Commit tokens remember their app.** Wherever the hub hands a page a device commit token (the
   duplicate refusal in `handleThemeUpload`, and the same in `streamThemeUpload` if it hands one
   out), it records `token → app`, where app is the container's own `Sections[0]` (the app the orb
   files it under, as `editorScopeContainer` reads it). The record is bounded like `shelfPending`
   (at most `shelfPendingMax` entries, oldest/any dropped first, same `sendMu` lock) and the entry
   is removed when the token is spent by either port's commit. Name: `commitApps map[string]string`
   on `Server` (or fold the app into the existing shelf hold; either is fine if bounded and locked).
   An empty token records nothing.
2. **`editorScopeCommit(next)`** wraps `/api/theme/commit` on the editor listener only:
   `mux.Handle("/api/theme/commit", editorMethod(http.MethodPost, s.editorScopeCommit(s.handleThemeCommit)))`.
   It reads the body with `editorBodyCapped(r, 4096)` (the body is `{ip, commit}`; put back for the
   handler), decodes `commit`, and:
   - a body that does not decode, or an empty `commit` → passed to the handler unjudged (it has its
     own sentences for both);
   - a token the hub has no app record for → **403** via `reject`, message
     `"this editor is the <scope> app's: it can only commit a theme it sent"`;
   - a token recorded for another app → `editorScopeOther(w, r, scope, thatApp)`;
   - a token recorded for this editor's app → `next`.
3. **The send tracker remembers its app.** `themeSend` gains `App string` with `json:"-"` (never
   in the answer). Every `setSend` that starts tracking a send from a parsed container (the precheck
   verdict, the `"uploading"` start, and the stream path's start) sets it to `c.Sections[0]` (empty
   when the container names no section). Other phase updates leave it alone.
4. **`editorScopeSendStatus(next)`** wraps `/api/theme/upload-status` on the editor listener only:
   it reads `?ip=`, takes `s.sendSnapshot(ip)`, and if the snapshot's `App` is non-empty and not
   this editor's scope → `editorScopeOther(w, r, scope, snapshot.App)`; otherwise `next` (so an
   idle orb, or a send of this app's theme, answers exactly as today).
5. The control port's `/api/theme/commit` and `/api/theme/upload-status` (`main.go`) keep
   `requireControl` and nothing else: the hub page is trusted with every app.

## Behaviour and edge cases

- The scope refusals are 403 in the `reject` envelope, like every other S2-2 refusal.
- A commit token held for app A and spent through the control port is removed from the record, so
  a later editor commit of it is refused as "no record".
- A body over 4096 bytes is passed to the handler as-capped (it will fail to decode there).

## Acceptance criteria

New tests in `tools/orb-manager/editororigin_test.go` (or a new `editorcommit_test.go`):

1. `TestEditorCommitScopedToItsApp`: a duplicate refusal for a **flights** container records its
   token; the clock editor (`editorAPICall`-style request on the editor listener, Host
   `clock.localhost`, clock token) POSTs `/api/theme/commit` with that token → 403 whose body names
   flights; the flights editor with the same token reaches the handler (the fake device sees the
   commit); a token never handed out → 403 "it can only commit a theme it sent".
2. `TestEditorSendStatusScopedToItsApp`: a send of a flights container tracked for an ip; the clock
   editor GETs `/api/theme/upload-status?ip=<ip>` → 403; the flights editor → 200 with the phase;
   an ip with no send → 200 `idle` for either editor; the control port answers the flights status
   unchanged.
3. Both tests FAIL on the base without the wrappers (check it: temporarily register the two routes
   unwrapped, run, restore; say so under VERIFIED).
4. `TestEditorAPIOnlyTheListedRoutes`, `TestEditorAPIScopeIsItsApp` and every existing `TestEditor*`
   test still pass.
5. `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH; cd tools/orb-manager && go vet ./... && go test ./... -count=1` → ok.

## Out of scope

- Anything else in the editor listener's route list; the control port's routes; the device
  protocol; the commit token's format.
- Do not modify: `web/`, `internal/`, `sitelink.go`, `siteimport.go`, `updates.go`.
