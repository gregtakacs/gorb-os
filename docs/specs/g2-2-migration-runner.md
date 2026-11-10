# G2 step 2: the migration runner

## Goal

Slice G2 of the orb-os-net design (§12), second step (§6.5 point 3). A **migration
runner** ships with the editor runtime: given one unpacked editor and some design files,
it opens each design headless in that editor through the session's own `check` and
`apply` path (the path a person's Open takes), saves it as the editor would, and reports
per design either success (from which schema to which, and that a second open-and-save
changes nothing) or the refusal sentence. Optionally it bakes each migrated design to an
`.orbtheme`. It works for any editor that runs on `theme@1` (clock, flights, weather, an
outside app's), because it drives the editor in headless Chromium and reaches the session
through one hook in `theme@1/session.js`, never through app code. The site will use it
to check a new app version against every stored design (M1), and step 3's
`orbsdk.py check-migrations` will run it over each app's `_fixtures/`.

This step also commits the first fixtures: one sample design per schema each shelf editor
has had.

## Read first

- `/home/greg/docker/Projects/orb-os-net/docs/design.md` (read-only mount): §6.5 in full.
- `tools/orb-manager/web/runtime/theme@1/session.js` in full (after step 1): `api.apply`,
  `api.check`, `api.migrate`, `api.doc`, the start check, `OrbTheme.hostInfo`.
- `tools/orb-manager/web/runtime/_selftest/editors-smoke.js` in full: **imitate it** for
  the Chromium handling (finding the browser, `--headless=new --dump-dom`, a fresh profile
  per run, the stderr filter, the wall-clock kill), the static server (`readTree`, content
  types, the `<head>` injection) and the PASS/FAIL voice.
- `apps/flights/editor/flights-editor.js` lines 205-230 and `apps/weather/editor/weather-editor.js`
  around line 240: a real `host` (note `content()` reads React state through a ref that a
  render updates, so a design applied with `host.apply` reaches `api.doc` only after a
  render).
- `apps/clock/editor/Watch Composer.dc.html` around lines 895-910 and 3700-3720: the
  clock's host and its `DESIGN_SCHEMA` / `DESIGN_MIGRATE`.
- `apps/flights/editor/flights-bake.js` lines 160-200: `upgrade1`, flights' one migration
  step (schema 1 → 2), and the shape of a schema-1 design it reads.
- `tools/orb-manager/extract.go` (`extract-editors`, the `editors.json` report) and
  `Makefile` targets `check-editor`, `check-web`.

## Contract

### 1. The session hook (`theme@1/session.js`)

At the very end of `OrbTheme.session(host)`, just before `return api;`:

```js
    // the migration runner (_runner/migrate.js) hands itself the session and its host here,
    // so it can open designs through the same check and apply a person's Open takes; a page
    // that sets no window.GorbRunner never reaches this
    if (typeof root.GorbRunner === 'function') root.GorbRunner(api, host);
```

Nothing else in `session.js` changes.

### 2. `tools/orb-manager/web/runtime/_runner/migrate.js`

Plain node (no npm packages), executable, `'use strict'`, a header comment in
`editors-smoke.js`'s style saying what it is and who calls it (the site's migration check,
`orbsdk.py check-migrations`). The directory starts with `_`, so the hub's `//go:embed web`
leaves it out; nothing serves it.

```
node migrate.js --runtime <dir> --editor <dir> [--slug <slug>] [--chrome <path>]
                [--bake <out dir>] [--json <file>] [--timeout <seconds>] <design file>...
node migrate.js --runtime <dir> --editor <dir> [--slug <slug>] [--chrome <path>] --save-current <file>
```

**Inputs.**
- `--runtime`: either an `orb-manager export-runtime` tree or the source tree
  `tools/orb-manager/web/runtime`. A request `/runtime/<id>/<file>` is answered from
  `<runtime>/<id>/<file>`, else from `<runtime>/vendor/<file>` (the source tree keeps the
  library ids `react@…`, `babel@…`, `fonts@1` under `vendor/`), else 404. Refuse `..`
  and empty segments as `readTree` does.
- `--editor`: one unpacked editor: a folder holding `editor.json` and the region's files
  (an `extract-editors` `<out>/<slug>/` folder, or an unzipped editor region). Its
  `editor.json` gives `entry`, `runtime` and `schema`. The slug is `--slug`, else the
  `slug` in the folder's `orb-app.js` (`window.__ORB_APP = {…};`, parse the object literal
  as JSON), else exit 2 with `no slug: the editor has no orb-app.js; pass --slug`.
- Design files: each is a design document as JSON (`.orbdesign` or any name). A file that
  does not parse as JSON is a FAIL for that file (`not JSON: <message>`), not a crash.
- Exit 2 with a usage line for missing `--runtime`/`--editor`, a missing `editor.json`, or no
  design files and no `--save-current`. No Chromium (same search as `editors-smoke.js`,
  plus `google-chrome-stable`) → exit 2 with the same sentence the smoke suite prints.

**The page.** One `http.createServer` on `127.0.0.1:0` serves the editor at
`/studio/<slug>/` (entry document for `/studio/<slug>/` and for `.../editor.json`, like the
smoke suite), its files under that folder, and the runtime under `/runtime/`. The entry
document gets two scripts right after `<head …>`: first
`window.GorbHost = {"kind":"web","api":"/studio-api","send":false,"publish":false,"inbox":false};`
then the runner's probe. API answers: `GET /studio-api/library/entries…` →
`{ok:true,data:[]}`; any other `/studio-api/…` or `/api/…` →
404 `{ok:false,error:'the migration runner has no library'}` (record them; they are not
failures). Internal routes under `/__runner/` (below). Every other path → 404.

**The probe** (an inline string, ES5, like the smoke suite's `PROBE`):
- records `error` events and `unhandledrejection`s (same filter for `{{` template srcs);
- defines `window.GorbRunner = function (api, host) {…}` that keeps the **latest** pair it
  is handed and starts the run once (a later session replaces the pair before the run
  starts; the run uses the pair current when it starts);
- the run: wait until `host.ready()` is true (poll every 100 ms, give up after 30 s of page
  time with `the editor never became ready`); then `GET /__runner/designs` →
  `[{name, doc}]` (or `{save: true}` for `--save-current`); then for each design, in the
  order given, one at a time:
  1. `why = api.apply(clone(doc), 'the migration runner')` (`clone` = JSON round trip). A
     string → result `{name, ok: false, why}`.
  2. Otherwise **settle** (wait two `requestAnimationFrame`s and then a 200 ms
     `setTimeout`, so a React editor has rendered the applied state), then
     `d1 = await api.doc(true)`.
  3. Reopen `d1` the same way (`api.apply(clone(d1), 'the migration runner')`, settle,
     `d2 = await api.doc(true)`). `stable` is true when `d1` and `d2` are equal as
     canonical JSON (keys sorted at every depth) with `made` removed from both. A refusal
     on the reopen is `{ok: false, why: 'it opened, but its own save does not reopen: ' + why2}`.
  4. With `--bake`: if `host.bake` is a function, `bytes = await host.bake()`; a
     `Uint8Array`/`ArrayBuffer` is POSTed to `/__runner/bake?i=<index>` as
     `application/octet-stream`; `null` or a throw → the result's `bake` is the reason
     (`'the editor’s bake returned nothing'`, or the message), and the design fails. No
     `host.bake` → `bake: 'this editor cannot bake'`, a failure.
  5. Result `{name, ok: true, from: <doc.schema as given>, to: d1.schema, appVersion:
     d1.appVersion, stable, bake: <bytes length or the reason>}`, POSTed to
     `/__runner/result` as JSON as soon as it is known (one POST per design, so a
     timeout still keeps what finished).
  Any exception inside one design's steps → `{name, ok: false, why: 'threw: ' + message}`
  and the run continues with the next design.
- `--save-current`: after ready, `d = await api.doc(true)`, POST it to `/__runner/save`.
- At the end (or on the ready timeout) POST `/__runner/done` with
  `{errors, schema: host.schema(), version: api.version(), slug: api.slug()}`.

**Chromium.** One run for the whole list: `--headless=new --no-sandbox --disable-gpu
--no-first-run --user-data-dir=<fresh tmp> --virtual-time-budget=3600000 --dump-dom <url>`.
The runner kills Chromium as soon as `/__runner/done` arrives (do not wait for the virtual
budget), and after `--timeout` seconds of wall time (default 600). If no session ever
reached `GorbRunner` (no `done`, or `done` before any session), every design is
`{ok:false, why: 'the editor never started a theme@1 session' + (errors ? ': ' + errors : '')}`.
A design with no result when Chromium is gone → `{ok:false, why: 'no answer from the
editor before the timeout'}`.

**Output.** One line per design, in input order:
- `ok   <name>: schema <from> -> <to>, saved as <appVersion>, stable` (`, baked <n> B`
  with `--bake`);
- `FAIL <name>: <why>` (or `FAIL <name>: opened, but a second open and save changed it`
  when `stable` is false; or the bake reason);

then `migrated <k> of <n> designs` and exit 0 when every design is ok, stable and (with
`--bake`) baked, else exit 1. `--bake <dir>` writes design *i*'s bytes to
`<dir>/<basename of the design file without extension>.orbtheme` (create the dir). `--json
<file>` writes `{format: 1, editor: {slug, version, schema: <editor.json schema or null>},
results: [{file, name, ok, why, from, to, appVersion, stable, bake}]}` (absent fields as
`null`). `--save-current <file>` writes the document as JSON (`JSON.stringify(d, null, 1)`
plus a newline), prints `saved <slug> schema <n> to <file>`, exit 0; no session or a
failure → exit 1 with the reason. `VERBOSE=1` prints every request.

`name` is the design file's basename. The runner never writes into the editor folder, the
runtime or a design file.

### 3. Its own suite: `tools/orb-manager/web/runtime/_selftest/runner-check.js`

Plain node, not `headless-*.test.js` (it needs Chromium). It builds scratch editors in a
temp dir: an `editor.json` (`{"editor":1,"entry":"index.html","runtime":["theme@1"],"schema":3}`),
an `orb-app.js` (`window.__ORB_APP = {"schema": 3, "slug": "tint", "version": "v1.3.0"};`)
and an `index.html` that loads `/runtime/theme@1/session.js` and starts a session whose
host keeps `{color, size}` in a plain object, `schema()` 3, `migrate()` steps `1` (adds
`size: 10`) and `2` (renames `colour` → `color`), `content()` returning a copy, `apply()`
refusing a design with no `color`, `ready()` true, `bake()` returning
`new TextEncoder().encode('BAKED:' + color)`. Variants of that editor are made by small
substitutions. It runs `migrate.js` (as a child process, `--runtime` = the source tree)
and checks, PASS/FAIL lines and `all green (N checks)` / `N FAILED`, exit 0/1:

1. schema-1 (`{colour:'red'}`), schema-2 (`{colour:'red', size: 4}`) and schema-3 designs
   all open; the JSON report says `from` 1/2/3, `to` 3, `stable` true; exit 0.
2. A same-schema design whose `appVersion` is `v9.0.0` opens (ruling 21).
3. A schema-4 design fails with the `reads schema 3` sentence; exit 1, and the other designs
   in the same run still report ok.
4. An editor variant with step `1` removed: the schema-1 design fails with
   `no step from schema 1 to 2`; the schema-2 one still opens.
5. Another app's design (`app: 'clock'`) fails by name; a file that is not JSON fails with
   `not JSON`.
6. An unstable editor variant (`content()` adds `n: <a counter that increases on every
   call>`) → `FAIL … a second open and save changed it`, exit 1.
7. A stamp/host mismatch variant (`orb-app.js` says schema 2, the host 3) → every design
   fails with `never started a theme@1 session` and the start check's sentence
   `reads design schema 3, but its editor.json says 2`.
8. `--bake <tmp>` writes `schema-1.orbtheme` holding exactly `BAKED:red`; a variant whose
   `bake()` resolves `null` fails with `bake returned nothing`.
9. `--save-current` writes a document with `format: 'wc-design'`, `schema: 3`,
   `app: 'tint'`, `appVersion: 'v1.3.0'`.
10. A variant that never becomes ready → `never became ready`, exit 1, in under 60 s wall.
11. No request in any run reached `/api/` (the runner's `VERBOSE=1` request list, or
    have the runner include the API requests it saw in the `--json` report as
    `requests: [...]` — your choice; say which).

Add to the root `Makefile` a target **`check-runner`** (in `.PHONY`, one comment line in the
header list like `check-web`'s: needs Chromium, so not in `check`) that runs
`$(NODE) tools/orb-manager/web/runtime/_selftest/runner-check.js`.

### 4. The fixtures

Create, each a design document as the editor writes it (`api.doc(true)`), pretty-printed
as `--save-current` writes it:

- `apps/flights/editor/_fixtures/schema-2.orbdesign`, `apps/weather/editor/_fixtures/schema-1.orbdesign`,
  `apps/clock/editor/_fixtures/schema-1.orbdesign`: with `--save-current` from the
  **shipped** editors (`orb-manager extract-editors tools/orb-manager/apps <tmp>`, then
  `--editor <tmp>/<slug>`). If an editor's starting design is a blank or "start sheet"
  with nothing chosen (the clock may need a preset picked), say so and use what
  `api.doc(true)` gives once `host.ready()` is true; do not add app code to change it.
- `apps/flights/editor/_fixtures/schema-1.orbdesign`: a real schema-1 flights design.
  Preferred source: the flights editor as it was before schema 2, from git history
  (`git log --oneline -- apps/flights/editor`; commit `f8e356f` introduced schema 2, so
  `f8e356f^` has the schema-1 editor). Unpack that commit's `apps/flights/editor/*` (top
  level files) into a temp folder, write an `orb-app.js` stamp with the slug and that
  commit's `apps/flights/version.txt`, and run `--save-current` against it with today's
  runtime source tree. If that old editor does not load on today's runtime, build the
  document by hand from what `upgrade1` reads (every field it touches, values from that
  commit's presets), and say which you did. Either way the fixture's `schema` is 1 and
  its `appVersion` is the old release.
- The `_fixtures/` folders are subdirectories of the editor folders, so the SDK's region
  build already leaves them out (verify: `make_factory_apps.py --check` stays fresh, and the
  clock region's member list is unchanged).

## Acceptance criteria

Run from the repo root with `export PATH=/usr/local/go/bin:$PATH`.

1. `make check-runner` → `all green`.
2. With `go -C tools/orb-manager build -o /tmp/om . && rm -rf /tmp/ex && /tmp/om extract-editors tools/orb-manager/apps /tmp/ex`:
   - `node tools/orb-manager/web/runtime/_runner/migrate.js --runtime tools/orb-manager/web/runtime --editor /tmp/ex/flights apps/flights/editor/_fixtures/*.orbdesign`
     → both ok, `schema 1 -> 2` and `schema 2 -> 2`, stable, exit 0;
   - the same for `/tmp/ex/weather` with weather's fixture and `/tmp/ex/clock` with the
     clock's → ok, stable, exit 0;
   - flights with `--bake /tmp/bk` → both baked, and `/tmp/bk/schema-1.orbtheme` starts
     with the bytes `TROG`.
3. A copy of the flights fixture with `appVersion` set to `v9.9.9` opens in `/tmp/ex/flights`;
   a copy with `schema` set to 3 fails with `reads schema 2`.
4. `make check-editor` and `make -C tools/orb-manager check` still pass;
   `python3 tools/make_factory_apps.py --check` → fresh.

Paste the outputs of 2 and 3 in your report.

## Out of scope

- `orbsdk.py` (check-migrations, the fixture rule), the SDK template, the SDK tarball,
  `orb-web.sh`, docs, `.github/`: step 3.
- Any change to the editors' code, `version.txt` files, the `.orbapp` files, `main/`,
  `components/`.
- `editors-smoke.js` and `fake-hub.js` stay as they are (copy what you need, do not
  refactor them into a shared module).

## Notes

- Code style: the runtime's (ES5 inside the probe string, terse comments that say why);
  node scripts like `editors-smoke.js`.
- Chromium is `/usr/bin/chromium` in this worker. Under `--virtual-time-budget` page timers
  run on virtual time, so the 30 s ready limit is page time; the wall-clock `--timeout`
  is the real backstop.
- If a real editor turns out unstable (a second open and save changes the document), do
  not change the editor: report which fields differ, with both values, under DEVIATIONS.
- Commits: plain subject lines in the repo's style, no Co-Authored-By or tool-credit
  trailers.
