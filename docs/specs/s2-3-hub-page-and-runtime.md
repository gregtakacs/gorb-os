# Orb Manager's page and theme@1: open editors on their own origin, talk to them across it

## Goal

Slice **S2** of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md`, ruling 33,
§6.6), step 3 of 4. Steps s2-1/s2-2 moved every app editor to its own origin,
`http://<label>.localhost:<editor port>` (one per app), reachable only with a per-launch editor
token, and removed `/editor/...` from the control port. Read `tools/orb-manager/editororigin.go`
(the guard, the route list, `GET /api/editor/origin?app=<slug>` → `{origin, token}`).

The hub page (`web/app.js`) still builds same-origin iframe URLs (`/editor/...`) and assumes
it can reach into the frame; theme@1 still posts its messages to `location.origin`, which is
now the editor's own origin, so the hub page would never hear them. This step makes Edit,
Compose, Rebake all, Send to orb, Import from orb-os.net and the G3 Updates/rename bake work
again across the origin boundary. Browser code plus one Go string; no server behaviour changes.

## Read first

- `tools/orb-manager/editororigin.go` (s2-1/s2-2), and `tools/orb-manager/README.md`'s new
  section "Editors run on their own origin".
- `tools/orb-manager/web/app.js` lines ~3975–4260: `themeReopenButton`, `openThemeEditor`,
  `openLibraryEditor`, `editorVeilSource`, the `edVeil` load listener, `themeEditorApp`,
  `themeEditorFrom`, `closeThemeEditor`, `themeBridge` (origin check, `theme.update`,
  `theme.imported`, `theme.save`), and the callers of `openLibraryEditor` (~5326, ~5445: the
  site import and the G3 apply). `hubFetch` (grep it) is the control-API fetch to reuse.
- `tools/orb-manager/web/index.html` line ~487: the iframe and its `sandbox`.
- `tools/orb-manager/web/runtime/theme@1/send.js` ~line 96 (`busy`, `theme.busy` message).
- `tools/orb-manager/web/runtime/theme@1/session.js` ~line 630 (`importFile`'s hard-coded
  `/api/library/keep`) and ~line 742 (`theme.imported` message).
- `tools/orb-manager/runtime.go` `runtimeRefused` (~196): two inline `postMessage(..., location.origin)`.
- Tests that pin these: `tools/orb-manager/ui_test.go` (grep `openLibraryEditor`, `themeBridge`,
  `origin`), `web/runtime/_selftest/headless-gorbhost.test.js` (~531–570: `posted[0].origin === 'http://hub'`),
  `headless-send.test.js`, `headless-session.test.js`, `runtime_test.go`.

## Contract

### `web/app.js`

1. New module-level `let themeEditorOrigin = "";` beside `themeEditorApp`, and a function

   ```js
   // editorFrameSrc asks the hub where this app's editor lives (its own origin, s2) and the
   // launch's token for it, and builds the frame's URL. pathAndQuery starts with /editor/.
   async function editorFrameSrc(slug, pathAndQuery) { ... }
   ```

   It calls `hubFetch("/api/editor/origin?app=" + encodeURIComponent(slug))`, reads
   `{ok, data:{origin, token}}`, throws `Error(j.error || "HTTP " + status)` on failure, and
   returns `{ origin, src: origin + pathAndQuery + (pathAndQuery.includes("?") ? "&" : "?") + "et=" + encodeURIComponent(token) }`.
   No caching (a token is per hub launch; one small call per open is fine).

2. `openThemeEditor(row, theme)` and `openLibraryEditor(slug, ticket)` become `async`. They keep
   building exactly the same path+query they build today (`row.editor_url` + `?orb=…&app=…&label=…&theme=…&src=…&entry=…&rebake=1`,
   and `/editor/hub/<slug>/?app=…&import=…`), pass it through `editorFrameSrc`, set
   `themeEditorOrigin = origin` and then `frame.src = src`. Everything else they do (veil, title,
   `themeEditorFrom`, `themeEditorApp`, `pane.hidden`, `body.editing`, the warm hint) stays.
   Guard against a stale answer: a counter `editorOpenGen` incremented on each open and on
   `closeThemeEditor`; an answer whose generation is no longer current is dropped. If
   `editorFrameSrc` throws: remove `loading` from the pane, set the veil/state text to
   `The editor could not be opened: <message>`, and for an import (`openLibraryEditor`) call the
   same failure path the import already uses when an editor never answers (look at
   `siteImportClosed`/`siteImportEnded`; pick the one that frees the door and says why).
   Callers that ignored the return value keep doing so (a promise is fine).

3. `closeThemeEditor` also clears `themeEditorOrigin = ""` (and bumps `editorOpenGen`).

4. `themeBridge`: replace `if (ev.origin !== window.location.origin) return;` with
   `if (!themeEditorOrigin || ev.origin !== themeEditorOrigin) return;   // not this app's editor origin`
   and update the comment above it: the frame is on the app's own origin (s2), so the check is
   "the origin this page opened for this app", still together with `ev.source === frame.contentWindow`.
   `back()` keeps answering at `ev.origin`.

5. `theme.update` (the editor's refusal page asks for the app to be updated on the orb): since
   the frame is now untrusted content on another origin, the hub page asks the person first.
   Before `streamInstall`, `if (!window.confirm("Update the " + m.app + " app on " + (current.name || themeIP) + " now?\n\nThe editor asked for it. Orb Manager installs its own copy of the app.")) { say("update not started"); return; }`.
   Replace `try { frame.contentWindow.location.reload(); } catch (e) { frame.src = frame.src; }`
   with `frame.src = frame.src;` (a cross-origin `location.reload` always throws).

6. The `edVeil` load listener: a cross-origin frame's `contentDocument` is `null` (it does not
   throw). New rule, with the comment rewritten to match: `doc === null` → an editor document
   (or the hub's own editor error page) loaded on the editor origin → lift the veil exactly as
   today; `doc` present but with no body children → the `about:blank` stray → ignore; `doc`
   present with content → lift (keeps any same-origin page working). Keep the `try/catch`
   (treat a throw like `null`).

7. Fix the comments that say the frame is "same-origin by construction" (above
   `openThemeEditor`, in `edVeil`, above `themeBridge`) so they describe the s2 model in one or
   two sentences each. Keep the string `"/editor/hub/"` in `openLibraryEditor` (ui_test pins it).

### `web/index.html`

No change to the iframe's `sandbox` (`allow-scripts allow-same-origin` stays: the editor keeps
its own origin for its own storage and its same-origin API calls, and still cannot navigate
the top window or open popups).

### theme@1 and the hub's refusal page — post to the parent at any origin

The editor no longer shares the parent's origin, and it does not know the parent's. The four
messages it sends (`theme.busy`, `theme.imported`, `theme.close`, `theme.update`) carry nothing
private (an app slug, a theme name, a file name, a flag), and the hub page checks
`ev.source` and `ev.origin` on receipt. So the target origin becomes `'*'`:

- `theme@1/send.js` `busy`: `root.parent.postMessage({ type: 'theme.busy', on: !!on }, '*')`.
- `theme@1/session.js` (~742): `root.parent.postMessage({ type: 'theme.imported', ... }, '*')` (same payload).
- `runtime.go` `runtimeRefused`: both inline `parent.postMessage(..., location.origin)` → `'*'`.
- `host@1/host.js` already posts `theme.close` with `'*'` — unchanged.

Add a one-line comment at each changed call: `// '*': the hub page is on another origin (S2); it checks source and origin itself`.

Do NOT touch the `GorbHost` block (it must stay byte-identical in send.js and session.js and
unchanged for the site).

### theme@1 — the hard-coded keep (orb-os-net B2)

`session.js` ~630, in `importFile`: `hubJson('/api/library/keep' + app(), …)` →
`hubJson(OrbTheme.hostUrl('/library/keep') + app(), …)`. (The same one-line fix exists on the
local branch `b2`; make it byte-identical to that line.) Then confirm no other hub route in
`theme@1/*.js` or `host@1/host.js` is hard-coded with `/api/`:
`grep -n "'/api/" tools/orb-manager/web/runtime/theme@1/*.js tools/orb-manager/web/runtime/host@1/host.js`
must print nothing. (`/orb/<ip>/api/v1/...` in host.js and `maps@1`'s `/api/maps/...` stay as
they are: the site serves neither, and the hub serves both on the editor origin.)

### Tests

- `headless-gorbhost.test.js`: the `theme.imported` assertion now expects `posted[0].origin === '*'`
  (update its message: "at any origin: the hub checks source and origin itself (S2)").
- `headless-send.test.js` (or the suite that covers `busy`): assert the `theme.busy` message is
  posted with target `'*'` (extend its fake `parent.postMessage` to record the origin).
- `headless-session.test.js`: add a check that `importFile` on a `.orbtheme` keeps through
  `OrbTheme.hostUrl('/library/keep')`: under `GorbHost {kind:'web'}` the keep goes to
  `/studio-api/library/keep?app=…`, under the hub to `/api/library/keep?app=…`.
- `ui_test.go`: update/extend the app.js source checks: `themeBridge` compares against
  `themeEditorOrigin`, never `window.location.origin`; `openThemeEditor` and `openLibraryEditor`
  go through `editorFrameSrc`; `editorFrameSrc` calls `/api/editor/origin`; the `theme.update`
  branch calls `window.confirm` before `streamInstall`.
- `runtime_test.go` (or wherever `runtimeRefused`'s HTML is checked): the page posts with `'*'`.

## Acceptance criteria

1. `node --check tools/orb-manager/web/app.js` → ok.
2. `cd tools/orb-manager && go vet ./... && go test ./...` → green (re-run
   `TestThePassParksForADemandAndResumesItsOwnQueue` alone if it is the only failure).
3. `cd tools/orb-manager/web/runtime/_selftest && node headless-gorbhost.test.js | tail -3` → `all green`;
   `node headless-session.test.js | tail -1` → `all green`; `node headless-send.test.js` → exit 0;
   `PY=python3 node headless-test.js` → exit 0.
4. `make check-editor && make check-runner && make check-web` (repo root) → exit 0.
5. `grep -n "'/api/" tools/orb-manager/web/runtime/theme@1/*.js tools/orb-manager/web/runtime/host@1/host.js` → nothing.
6. `git diff --stat s2 -- tools/orb-manager/contract apps tools/orb-manager/web/runtime/host@1/page.html` → empty.
7. The GorbHost block is still byte-identical in send.js and session.js (the existing suite checks this; it must pass).

## Out of scope

- Any Go behaviour change (only the two strings in `runtimeRefused`).
- The live browser test (s2-4).
- Do not modify: `apps/`, `tools/orb-manager/contract/`, `host@1/page.html`, the GorbHost block.
- No new message types, no MessageChannel, no change to what the messages carry.

## Notes

- Go: `/usr/local/go/bin/go`, `GOFLAGS=-buildvcs=false`. Node 24 is on PATH.
- `make check-web` builds `tools/orb-manager/bin/orb-manager` and the orb-web tarball first.
- The site's hosted editors (`GorbHost {kind:'web'}`) must behave exactly as before apart from
  the postMessage target (they post to themselves when unframed; `'*'` delivers the same).
