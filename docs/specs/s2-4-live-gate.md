# S2's live gate: a real headless Chromium against a real hub and `cmd/orbfake`, with a hostile editor

## Goal

Slice **S2** of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md`, ruling 33,
§6.6, §12 S2 row), step 4 of 4. Steps s2-1…s2-3 put every app editor on its own origin
(`http://<label>.localhost:<editor port>`, one per app, per-launch per-app token, narrow editor
API) and moved the hub page and theme@1 across that boundary. The Go suite proves the server
rules. This step proves the whole thing **in a real browser**, the gate of §12:

- from an editor page, `/api/flash`, provisioning, erase, app install and the LAN proxy are all
  refused — shown by a **hostile editor** that tries each;
- the control cookie never reaches an editor origin;
- Edit (and Compose), Rebake all, Send to orb and Import from orb-os.net still work, end to end,
  through the hub page's own functions, against a hub with `cmd/orbfake` as the orb;
- the shelf editors run under the editor origin's content security policy with no violation.

Deliver one Node script and one Makefile target. No product code changes, except a fix you
find is needed to make a flow work — if so, keep it minimal, explain it in the commit message,
and add a Go or headless test for it.

## Read first

- `tools/orb-manager/editororigin.go` and `tools/orb-manager/README.md` "Editors run on their own origin".
- `tools/orb-manager/web/app.js`: `editorFrameSrc`, `openThemeEditor`, `openLibraryEditor`,
  `themeBridge`, `closeThemeEditor`, `siteImportLanded`, and how the page reads `/api/theme/apps`
  rows (`themeAppsRows`, `editor_url`). Top-level `function`s in app.js are globals on `window`;
  `let` bindings are not.
- `tools/orb-manager/web/runtime/_selftest/editors-smoke.js` and
  `tools/orb-manager/web/runtime/_runner/migrate.js`: how this repo finds and launches Chromium
  (`chromePath`, flags, a fresh profile per run, killing the process group). Reuse the approach.
- `tools/orb-manager/cmd/orbfake/main.go` and `internal/orbfake/` (the device double: apps,
  editor regions, `/api/v1/sd/...`, theme upload, `/api/v1/home|places|units`).
- `tools/orb-manager/main.go` flags: `-no-window`, `-addr`, `-editor-addr`, `-orb-port`,
  `-library-apps`, `-library-themes`, `-no-autoinstall`, `-probe-cables`.
- `tools/orb-manager/siteimport_test.go` `goodBundle`/`bundleOf` and `siteimport.go`: the
  bundle a site answers `GET /api/hub/theme/<id>` with, and what the hub checks.
- `tools/orbapp.py` `write_container` / `read_container` (~1545, ~1659): to write the hostile
  `.orbapp` with an editor region; `apps/sdk/orbsdk.py` `editor_region` (~487) shows the region layout.
- `apps/clock/editor/_fixtures/schema-1.orbdesign` — a clock design to seed the library with.
- `tools/orb-manager/web/runtime/theme@1/send.js` (`api.send`, ~137) and `host@1/host.js` — how the
  editor's Send is triggered.

## Contract

### `tools/orb-manager/web/runtime/_selftest/editor-origin-live.js`

`node editor-origin-live.js [--chrome PATH] [--keep] [--json OUT.json]` — no npm dependencies
(Node 24: global `fetch` and `WebSocket`). Run from anywhere; it finds the repo root from its
own path.

1. **Build** the hub (`go build -o <tmp>/orb-manager ./tools/orb-manager`) and the fake
   (`go build -o <tmp>/orbfake ./tools/orb-manager/cmd/orbfake`) with `GOFLAGS=-buildvcs=false`
   (use `go` on PATH, else `/usr/local/go/bin/go`).
2. **Hostile app.** Write `<tmp>/apps/hostile.orbapp`: slug `hostile`, a hosted editor
   (`editor.json` `{"editor":1,"host":1,"module":["hostile.js"],"runtime":["baker@1","fonts@1","host@1","react@18.3.1","studio@1","theme@1","ui@1"]}`)
   whose `hostile.js` runs the attack list below and stores the results in
   `window.__hostile = {done: true, results: [{name, where, status|error}]}`. Use python3 with
   `tools/orbapp.py` (borrow the wasm code bytes from a shelf `.orbapp` via `read_container`).
   If the hub refuses the container, fix the fixture, not the hub.
3. **Start** `orbfake -addr 127.0.0.1:<free>` and the hub:
   `orb-manager -no-window -addr 127.0.0.1:<free> -editor-addr 127.0.0.1:<free> -orb-port <fake port> -library-apps <tmp>/apps -library-themes <tmp>/themes -no-autoinstall -probe-cables=false`
   with `HOME`, `XDG_CONFIG_HOME` (and anything else the hub keeps state in) pointed at temp
   dirs so nothing touches the real user's state. `<tmp>/apps` holds copies of
   `tools/orb-manager/apps/*.orbapp` plus `hostile.orbapp`. Capture the hub's log.
   Wait for both to answer. Kill everything (process groups) on exit, pass or fail.
4. **A fake site** (a Node `http` server on `127.0.0.1:<free>`) answering
   `GET /api/hub/theme/<id>` with a valid clock design bundle (build it from
   `schema-1.orbdesign` the way `goodBundle` does: shared name `S2 import by @greg`).
5. **Operator setup** over the control API with the launch token (read it from `GET /` the way
   the page gets it, or its `Set-Cookie`): allow the fake orb (`127.0.0.1`), install the shelf
   **clock** app onto it from the hub's library (the same call the Apps face makes), seed the
   hub library with one clock design (`POST /api/design/save?app=clock&name=S2 rebake`).
6. **Chromium** headless (`--headless=new --no-sandbox --disable-gpu --remote-debugging-port=0`,
   fresh `--user-data-dir`), driven over CDP. The editor iframe is cross-site, so it runs
   out of process: use `Target.setAutoAttach {autoAttach:true, flatten:true, waitForDebuggerOnStart:false}`
   and evaluate inside the iframe's own session. Enable `Network` on every session and record
   `Network.requestWillBeSentExtraInfo` headers, and `Log`/`Runtime.consoleAPICalled` +
   `Security`/CSP violation reports.
7. Open `http://127.0.0.1:<hub port>/` and run these checks, each printing `PASS <name>` or
   `FAIL <name>: <why>`:

   | name | what |
   |---|---|
   | `edit-opens-on-own-origin` | from the page, call `openThemeEditor(row)` with the clock row of `/api/theme/apps?ip=127.0.0.1` (fetch it in the page with `hubFetch`). The frame's URL origin is `http://clock.localhost:<editor port>`, the editor boots (its `/api/library/entries?app=clock` answers 200), and the pane's veil lifts (`#mn-theme-editor` loses `loading`) |
   | `rebake-all` | `openThemeEditor(row, {rebake: true})`; within 90 s the hub library (control `GET /api/library/entries?app=clock`) holds a baked theme for `S2 rebake` that it did not hold before |
   | `send-to-orb` | in the clock editor frame, trigger the editor's own Send (`api.send` / the Send button — whatever the shelf clock editor exposes) for a theme; within 90 s the fake orb's card lists a new theme (the hub's control `GET /api/theme/list?ip=127.0.0.1` or orbfake's `/api/v1/sd/ls`) |
   | `import-from-site` | `POST /api/site/import` with `http://127.0.0.1:<site port>/t/<id>` → a ticket; call `openLibraryEditor('clock', ticket)`; within 90 s the hub page receives `theme.imported` (observe `siteImportLanded` running, or the library gaining `S2 import by @greg` with `theme.origin.designId === <id>`) |
   | `no-control-cookie-on-editor-origin` | across every run above, no request to any `*.localhost:<editor port>` URL carried a `Cookie` header containing `orb_token`, and `document.cookie` inside the clock frame is `''` |
   | `no-csp-violation-in-shelf-editors` | no CSP violation was reported by the clock editor during the four flows |
   | `hostile-refused` | load the hostile editor into the page's editor iframe (`editorFrameSrc('hostile', '/editor/hub/hostile/?app=hostile')`, then set the frame's `src`), wait for `window.__hostile.done` in its session, and require every result to be refused (below) |
   | `hostile-did-nothing` | after the hostile run: the fake orb's installed apps, card listing and config are unchanged, the hub's flash/provision/update state shows nothing started, and the hub log shows a `refused` line for the attempts that reached it |

   **The hostile editor's attempts** (each one a `fetch`, recorded as `status` or `error`):
   - to the **control** port `http://127.0.0.1:<hub port>`, with `credentials: 'include'`, once
     with header `X-Orb-Editor: <its own token from ?et=>`, once with `X-Orb-Token: <same>`,
     and once as a `mode: 'no-cors'` simple `text/plain` POST (no custom header, so the browser
     really sends it): `POST /api/flash`, `POST /api/provision`, `POST /api/orb/cmd`
     (`{"command":"reset","operand":"ALL"}`), `POST /api/lonely`, `POST /api/library/install`,
     `POST /api/apps/import`, `POST /api/orb/update`, `GET /orb/127.0.0.1/api/v1/status`,
     `POST /orb/127.0.0.1/api/v1/sd/delete`;
   - to **its own** editor origin, with its token: the same paths;
   - **another app's** data: `GET http://clock.localhost:<editor port>/api/library/entries?app=clock`
     (cross-origin) and `GET /api/library/entries?app=clock` on its own origin;
   - `document.cookie`.
   **Refused** means: a network error (CORS/preflight), an opaque `no-cors` answer whose effect
   `hostile-did-nothing` then proves absent, or a status of 403, 404, 405 or 421 — never 2xx/3xx;
   and `document.cookie === ''`. The report lists every attempt with its outcome.

8. Exit 0 only if every check passed; print a final line `editor-origin-live: N/N passed`.
   `--json` writes the full evidence (each check, each hostile attempt and outcome, the cookie
   scan, CSP reports). `--keep` leaves the temp dir and prints its path.

### Root `Makefile`

Add a target next to `check-web` (same `NODE` guard), not part of `check`:

```make
## check-editor-origin: S2's live gate — a headless Chromium against a real hub and cmd/orbfake:
## editors on their own origin, a hostile editor refused everything, Edit/Rebake/Send/Import work
check-editor-origin:
	@test -n "$(NODE)" || { echo 'no node on PATH (see BUILDING.md: the editor suites need node 20+)'; exit 1; }
	$(NODE) tools/orb-manager/web/runtime/_selftest/editor-origin-live.js
```

## Acceptance criteria

1. `make check-editor-origin` → exit 0, last line `editor-origin-live: 8/8 passed`.
2. Run it a second time → also 8/8 (no state leaks between runs).
3. `node tools/orb-manager/web/runtime/_selftest/editor-origin-live.js --json /tmp/s2.json && node -e "const j=require('/tmp/s2.json'); if(!j.hostile||j.hostile.length<30) process.exit(1)"` → exit 0 (all hostile attempts recorded).
4. `cd tools/orb-manager && GOFLAGS=-buildvcs=false go vet ./... && go test ./...` → green
   (re-run `TestThePassParksForADemandAndResumesItsOwnQueue` alone if it is the only failure).

## Out of scope

- No changes to `apps/`, `tools/orb-manager/contract/`, `host@1/page.html`.
- Do not add npm dependencies (no puppeteer/playwright).
- Do not weaken any S2 rule to make a flow pass. If a flow fails because of an S2 rule, stop
  and report it as a question (QUESTIONS) with the evidence, unless the fix is plainly a bug in
  how s2-1..s2-3 implemented the spec.

## Notes

- Chromium is `/usr/bin/chromium` in this environment; `*.localhost` resolves to loopback inside
  Chromium itself. If Chromium tries `::1` first and the editor port only listens on
  `127.0.0.1`, it falls back; if it does not, report it (do not change the bind silently).
- Theme bakes in headless Chromium take seconds to tens of seconds; use generous timeouts (90 s)
  and poll.
- `-no-window` keeps the hub from opening a browser itself.
- The hub refuses to start beside another hub holding its lock; the temp `HOME`/`XDG_CONFIG_HOME` avoid that.
