# g5-1: Orb Manager's editor origin allows eval only for apps listing babel@7.29.0 or support@1

## Goal

Slice G5 of orb-os-net `docs/design.md` §12, ruling 38 (mirrors ruling 37 on the site). Today
`editorCSP()` (tools/orb-manager/editororigin.go) gives EVERY app's editor origin
`'unsafe-eval'`. After this step only an app whose `editor.json` `runtime` list contains
`babel@7.29.0` or `support@1` gets it, on its own `<app>.localhost:<editor port>` origin; every
other app's editor origin gets S2's policy with `'unsafe-eval'` removed and nothing else
changed. The control port's headers do not change at all. The hub page says which apps have it.

## Read first

- orb-os-net `docs/design.md` §0 rulings 37 (incl. "As built (R37)") and 38 (mounted read-only
  at `/home/greg/docker/worktrees/orb-os-net-fu2-lead`), and that repo's `src/lib/studio-headers.ts`
  (`RELAX_FOR_RUNTIME`, `cspForRuntimeIds`): the site's version of the same rule.
- `tools/orb-manager/editororigin.go`: `editorCSP`, `editorHandler`, `editorGuard`,
  `editorHostLabelOf`; `editororigin_test.go` (how S2 tests pin headers).
- `tools/orb-manager/runtime.go` (`editorRuntimeMissing`: how an editor.json's runtime list is
  read) and `editor.go` (`hostedEditor`, and where the editor listener finds an app's
  `editor.json` inside its `.orbapp` on the shelf).
- `tools/orb-manager/library.go` around line 148 (`HasEditor`, the Themes tab rows) and
  `tools/orb-manager/web/app.js` (`themeAppsRows`, how the Themes tab draws a row).
- `tools/orb-manager/web/runtime/_selftest/editor-origin-live.js` (the S2 live gate:
  `buildHostileApp`, the hostile probe list, the 8 checks, `--only`, `--json`).

## Contract

- `editororigin.go`:
  - `const editorCSPEval = "'unsafe-eval'"` and `var editorEvalRuntimes = []string{"babel@7.29.0", "support@1"}`.
  - `func (s *Server) editorCSP(eval bool) string`. With `eval == true` it returns exactly the
    string `editorCSP()` returns at the base commit (byte for byte: `script-src 'self'
    'unsafe-inline' 'unsafe-eval' blob:` …). With `eval == false` it returns that same string
    with ` 'unsafe-eval'` (leading space included) removed from `script-src` and no other byte
    changed. Build both from one template so they cannot drift.
  - `func (s *Server) editorAllowsEval(slug string) bool`: true only when the app `slug` is on
    this hub's shelf with an editor whose `editor.json` parses and whose `runtime` array contains
    one of `editorEvalRuntimes` as an exact string (no prefix match: `babel@7` or
    `babel@7.29.1` is false). Any read or parse error, no app, no editor, or no runtime list →
    false. Read it from the same `.orbapp` bytes the editor listener serves for that slug (the
    same source of truth; no separate cache that can disagree). If it reads the zip per call,
    that is fine; a small cache keyed by the shelf file's path + size + mtime is allowed.
  - `editorHandler`: the CSP header is still set before the guard and before any route, on every
    response including refusals. The slug is the Host label `editorHostLabelOf` parses; when the
    Host does not parse, or names an app that does not qualify, the strict policy is used.
    `Referrer-Policy` and `X-Content-Type-Options` unchanged.
- The Themes tab's app rows (the JSON that carries `has_editor`) gain
  `"editor_eval": bool` (true exactly when `editorAllowsEval(slug)`), and `web/app.js` shows, on
  such a row, the line: `This app's editor may run eval (it uses babel@7.29.0 or support@1).`
  (name the runtime it actually lists: `(it uses babel@7.29.0)`, `(it uses support@1)`, or
  both joined with ` and `). Rows without it show nothing new.
- The control listener, `main.go`'s headers, and every other header are untouched.

## Behaviour and edge cases

- An app whose editor.json lists both runtimes gets one `'unsafe-eval'`, not two.
- A label that differs only in case from a qualifying slug follows whatever `editorHostLabelOf`
  already does with case; do not add new normalisation.
- Replacing an app on the shelf with a version that drops babel must drop the relaxation on the
  next response (no stale cache).

## Tests (Go, `editororigin_test.go` or a new `editoreval_test.go`)

- `TestEditorCSPEvalIsS2Bytes`: `editorCSP(true)` equals the literal base-commit policy string
  (copy it into the test from the base `editorCSP()`), for a fixed control port.
- `TestEditorCSPStrictDropsOnlyEval`: `editorCSP(false)` equals `editorCSP(true)` with
  ` 'unsafe-eval'` removed, and does not contain `unsafe-eval`.
- `TestEditorAllowsEvalOnlyForTheTwoRuntimes`: table over editor.json runtime lists:
  none, `[]`, `["react@18.3.1"]`, `["babel@7.29.0"]`, `["support@1"]`, both, `["babel@7"]`,
  `["babel@7.29.1"]`, `["Babel@7.29.0"]`, malformed JSON, no editor.json.
- `TestEditorListenerCSPPerApp`: through `editorHandler()` with real shelf apps: the babel app's
  editor document, its `/runtime/...` asset and a refused request on its Host all carry the eval
  policy; the clock's and an unknown label's carry the strict one; a Host that does not parse
  carries the strict one.
- `TestControlHeadersUnchanged`: the control listener's response headers for `/`, an API route
  and a refusal equal what they were at the base commit (pin the literal values).
- `TestThemesRowsSayEditorEval`: the rows JSON has `editor_eval` true only for the babel app.

## Live gate (`editor-origin-live.js`)

- Seed one more test app beside `hostile`, built the same way as `buildHostileApp`: slug
  `babeltest`, an editor whose `editor.json` lists `babel@7.29.0` (plus whatever the hub needs to
  serve it), whose page script runs `eval("6*7")` and records the result (or the error name) on
  `window.__evalProbe`.
- New check 9 `babel-evals`: the babeltest editor, opened on `babeltest.localhost:<edPort>`,
  records 42.
- New check 10 `others-cannot-eval`: the hostile app's own page script also attempts
  `eval("6*7")` and records the result or error name on `window.__evalProbe`; it must record
  `EvalError`. In the clock editor's document, CDP `Runtime.evaluate` of
  `(function(){try{return String(eval("6*7"))}catch(e){return e.name}})()` with
  `allowUnsafeEvalBlockedByCSP: false` (that parameter defaults to true and would bypass CSP)
  must return `EvalError`. Run the same probe with the same parameter in the babeltest document:
  it must return `42` (proving the probe itself can see eval when allowed).
- The summary line becomes `editor-origin-live: 10/10 passed`; all 8 S2 checks unchanged; the
  hostile probe list still has every probe it has today (count it at the base commit; it must
  not shrink) and every one still refused.
- The Themes tab in the live hub shows the eval line for babeltest and not for clock.

## Acceptance criteria

1. `cd tools/orb-manager && go vet ./... && go test -run 'EditorCSP|EditorAllowsEval|EditorListenerCSP|ControlHeadersUnchanged|ThemesRowsSayEditorEval' -v .` → all PASS.
2. `sh docs/specs/s3-gotest.sh` → green.
3. `make check-editor-origin ARGS="--json /tmp/g5.json"` → `editor-origin-live: 10/10 passed`.
4. `make check-editor check-web check-runner check-migrations` → pass.
5. `git diff --stat <base> -- tools/orb-manager/main.go tools/orb-manager/security.go apps` → empty.

## Out of scope

- The site; any other directive; `'unsafe-inline'` (stays as S2 had it); `wasm-unsafe-eval`;
  worker-src; the control port; shelf app bytes.
