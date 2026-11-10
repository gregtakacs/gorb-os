# G4 step 2: Orb Manager's editor doors refuse `_fixtures/` (extract still unpacks them)

## Goal

Slice G4 (ruling 36) of the orb-os-net design, gorb-os half, step 2 of 2. After g4-1 the editor
region carries `_fixtures/schema-<n>.orbdesign`. `orb-manager extract-editors` must keep unpacking
them (that is how the migration check gets them from a container), but the hub's editor *serving*
doors — the per-orb `/editor/<ip>/<slug>/…` and the hub `/editor/hub/<slug>/…` — currently open
whatever member a cleaned path names, so they would serve a fixture as editor code. A fixture is
migration evidence, not something an editor session loads. This step makes those doors refuse a
`_fixtures/` path, matching what g4-site-1 does for the site. (Owner question 1 in
`docs/specs/g4-design.md` is answered *yes, refuse on the hub too*; if the orchestrator reverses
that, delete this step — nothing else depends on it.)

## Read first

- `docs/specs/g4-design.md` — "What each reader does today" (the editor.go bullets) and the Owner
  questions.
- `tools/orb-manager/editor.go`: `serveEditor` (~388, the `..`/leading-`/` reject then
  `path.Clean`), `serveHubEditor` (~430, same shape), and the shared `serveEditorFile` (~508, the
  half both doors call: it resolves `file`, opens it with `editorBytes`, and is the single place a
  refusal covers both doors). `editorManifestName` (~51), `editorRefused` (~591), `editorBytes`
  (~626).
- `tools/orb-manager/editor_test.go`: how a serving test drives a request — the `editorGet(t, s,
  "/editor/"+ip+"/<slug>/<file>", withToken)` helper and the region fixture each test builds — and
  `TestEditorGatesAddressAndPath` (~298), the table of paths the door refuses, to imitate.
- `tools/orb-manager/editororigin_test.go` / `check-editor-origin` in the root `Makefile`: the gate
  this step's change is subject to.

## Contract

- Name the two new tests `TestEditorDoorRefusesFixtures` (drives `editorGet` at
  `/editor/<ip>/<slug>/_fixtures/schema-1.orbdesign` and the hub door `/editor/hub/<slug>/_fixtures/schema-1.orbdesign`,
  both 404) and `TestExtractEditorsUnpacksFixtures` (builds a region carrying a `_fixtures/...`
  member and asserts `extract-editors` writes it). These two names are what the chain's `verify` greps for.

- In `serveEditorFile`, after `file` is resolved (the `file == ""`/`.` → manifest step) and before
  `editorBytes` opens any member other than the manifest it dispatches on, refuse a request whose
  path is in the `_fixtures` directory: `file == "_fixtures"` or `strings.HasPrefix(file,
  "_fixtures/")`. Answer `http.StatusNotFound` via the same `http.Error(w, "this app's editor ships
  no "+file, http.StatusNotFound)` style the not-found member uses today (do not leak that a
  fixture exists). The entry document and every other member are unaffected.
- Put the check in `serveEditorFile` (not `serveEditor`/`serveHubEditor`) so both doors get it from
  one place. `extract-editors` is a different command path (`extract.go`, untouched) and still
  unpacks fixtures.
- `serveEditor`/`serveHubEditor` keep rejecting `..`/leading-`/` as before; add no new grammar.

## Behaviour and edge cases

- `GET /editor/<ip>/<slug>/_fixtures/schema-1.orbdesign` → 404, body does not echo fixture content.
- `GET /editor/hub/<slug>/_fixtures/schema-1.orbdesign` → 404 (same shared gate).
- `GET /editor/<ip>/<slug>/_fixtures` (bare dir) → 404.
- A path that merely *contains* `_fixtures` as a substring but is not in that directory — e.g.
  `/my_fixtures.js`, `/a/_fixtures.json`, `/_fixturesX/y` — is served normally (prefix is the
  literal directory `_fixtures/`, matched after the leading-slash strip, exact for the bare name).
- `%5Ffixtures/...` (an encoded leading underscore) reaching the door: `serveEditor` already
  rejects a `%2e%2e`-style escape in its gate test; the cleaned `file` from a legitimately-encoded
  `_fixtures/` is still caught because the check runs on the post-`path.Clean` value.
- The manifest (`editor.json`) and the entry document still serve; a fixture is not the entry, so
  the `asksForEntry`-equivalent here (`file == ""`) never resolves to `_fixtures`.
- `orb-manager extract-editors` on a g4-1 container still writes `_fixtures/...` to disk (regression
  guard: extract.go unchanged, and its test proves a nested member unpacks).

## Acceptance criteria

1. `bash docs/specs/g4-gotest.sh` → the new hub-refusal test passes; it requests
   `/editor/…/_fixtures/schema-1.orbdesign` on both the per-orb and hub doors and expects 404, and
   asserts `extract-editors` still unpacks the fixture. Existing editor tests still pass.
2. `make check-editor` → exit 0 (the Go editor package builds and its tests pass).
3. `make check-editor-origin` → exit 0 (this step changes hub editor serving, so this gate runs).
4. The refusal test proves the substring cases (`/my_fixtures.js`, `/_fixturesX/y`) are still served.

## Out of scope

- Do not change `extract.go`, `apps/sdk/orbsdk.py`, the firmware, or any site file.
- Do not change which members `extract-editors` unpacks (fixtures must keep unpacking).
- No new editor path grammar; no change to token/runtime-gating logic in `serveEditorFile`.
- Do not modify `orb-web.lock.json`; no push/merge/deploy.

## Notes

- One guard in `serveEditorFile` covers both doors because both call it — do not duplicate in
  `serveEditor` and `serveHubEditor`.
- 404 (not 403) so the response is indistinguishable from "no such member", the same shape the
  door already returns for an absent file.
