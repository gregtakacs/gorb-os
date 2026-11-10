# G2 step 1: `editor.json` `schema`, and `api.check` refusing on schema only

## Goal

Slice G2 of the orb-os-net design (§12), first step. Every theme editor's `editor.json`
names the design schema it reads (`"schema": N`), the SDK checks it and stamps it beside
the release into `orb-app.js`, and `theme@1`'s session refuses to start when the editor's
own `host.schema()` disagrees with it, so the two can never drift (design §6.5 point 1).
`api.check` stops refusing a design because a newer release of the app saved it: only a
newer **schema** is refused (ruling 21, §6.5 point 2). The shelf `.orbapp` files are
rebuilt so the shelf check stays fresh.

## Read first

- `/home/greg/docker/Projects/orb-os-net/docs/design.md` (read-only mount): §0 rulings 20
  and 21, §6.5 in full. Nothing else there matters for this step.
- `tools/orb-manager/web/runtime/theme@1/session.js` in full, especially `api.version`,
  `api.doc`, `api.migrate`, `api.check`, `api.apply`, `api.sayOpened`, and the header
  comment's list of refusals.
- `tools/orb-manager/web/runtime/_selftest/headless-session.test.js` (the "design
  document" block near line 190) and `headless-test.js`: the test style.
- `apps/clock/editor/_selftest/headless-design.test.js` lines 55-80: the clock's
  envelope assertions.
- `apps/sdk/orbsdk.py`: `EDITOR_KEYS`, `editor_region`, `container`.
- `tools/host_sdk_editor_test.py` in full (including `MUTANTS`): the SDK editor suite.
- `tools/make_clock_region.py`: `_app_stamp`, `_manifest`, `_rewrite`, `members`.
  `tools/host_clock_region_test.py`: its pins on `editor.json`.
- `tools/make_factory_apps.py` (`editor_region`, `--check`); root `Makefile`
  (`check-apps`, `check-editor`, `apps`).
- `tools/orb-manager/extract.go` (`extractRow`, `extractEditor`, `extractOneApp`) and
  `extract_test.go` (the test that compares the report with the shipped `editor.json`).
- `apps/flights/editor/editor.json`, `apps/weather/editor/editor.json`,
  `apps/radar/editor/editor.json`, and where each editor's `schema()` comes from
  (`flights-bake.js` `SCHEMA = 2`, `weather-bake.js` `SCHEMA = 1`, the clock's
  `DESIGN_SCHEMA = 1` in `apps/clock/editor/Watch Composer.dc.html`).

## Contract

### 1. `editor.json` gains `schema`

- `apps/flights/editor/editor.json` → add `"schema": 2`; `apps/weather/editor/editor.json`
  → add `"schema": 1`. Keep each file's existing one-line, sorted-key layout.
- `apps/radar/editor/editor.json` does **not** change (its editor has no `theme@1`
  session and no design document), so `radar.orbapp` must come out byte-identical.
- The clock's region (`tools/make_clock_region.py`): `_manifest()` gains `"schema"`, read
  from the tracked composer, not typed twice. Add a function `design_schema()` that reads
  `apps/clock/editor/Watch Composer.dc.html` and finds `DESIGN_SCHEMA = <int>;` with an
  anchored regex; it must match **exactly once**, else `SystemExit("make_clock_region:
  the composer source no longer states DESIGN_SCHEMA = <n>; exactly once — re-point the
  bake")`, in the style of `_rewrite`'s other refusals.

### 2. The SDK (`apps/sdk/orbsdk.py`)

- `EDITOR_KEYS` gains `"schema"`; the refusal sentence for unknown keys becomes
  `editor.json carries <keys>; it names entry, runtime, schema, editor and baker`.
- In `editor_region`, after the runtime check:
  - when `schema` is present it must be an `int` (not a `bool`) `>= 1`, else
    `fail("editor.json's schema is the design schema the editor reads, a whole number from 1, not %r" % (schema,))`;
  - when `runtime` lists `theme@1` and `schema` is absent:
    `fail("%s's editor lists theme@1, so its editor.json must name the design schema it reads (\"schema\": 1 for a first release)" % m["slug"])`.
    An editor that does not list `theme@1` may leave `schema` out.
- The `orb-app.js` stamp gains `"schema": <n>` when `editor.json` has one (same
  `json.dumps(st, sort_keys=True)`), so it reads e.g.
  `window.__ORB_APP = {"schema": 2, "slug": "flights", "version": "v1.4.2"};`. No schema,
  no key: radar's stamp is unchanged.
- Put the schema check in one helper used by `editor_region`,
  `def editor_schema(ej, slug)` → the int, or `None` when absent and allowed (step 3 uses it
  for the fixture rule).
- Update the module comment above `EDITOR_KEYS` (one clause on `schema`).

### 3. The clock's stamp

`make_clock_region._app_stamp()` gains `"schema": design_schema()`. It feeds both the inline
`window.__ORB_APP=` script in the entry and the `orb-app.js` member, so both carry it.

### 4. `theme@1/session.js`

- **Start check.** Inside `OrbTheme.session(host)`, before `api` is returned (put it right
  after the `api` object and the helper `var`s are defined, before `api.slug`): when
  `root.__ORB_APP` is an object whose `schema` is not `undefined`, and
  `root.__ORB_APP.schema !== host.schema()`, throw
  `new Error('this editor reads design schema ' + host.schema() + ', but its editor.json says ' + root.__ORB_APP.schema + ': the two must agree — set editor.json’s schema to the editor’s and rebuild the app')`.
  A missing stamp, or a stamp with no `schema` (an unpackaged editor, an app built before
  this SDK, the clock's node suites), skips the check.
- **`api.check`, schema only.** Remove the block that refuses a design whose `appVersion`
  is newer than `api.version()` (the `ver`/`theirs`/`mine2` lines and their comment).
  Replace the comment with one line saying why that is safe: any change to what a design
  holds raises its schema (ruling 21), so a newer release with the same schema has nothing
  this editor would drop. The order of the remaining checks is unchanged: not a
  `wc-design` → another app's → older schema migrated (refusing when a step is missing) →
  a schema that is not a whole number from 1, or newer than this editor's, refused with
  today's sentence. Tighten only this: the schema is read as `Number(o.schema)` and must
  satisfy `Number.isInteger(n) && n >= 1`, else the same refusal sentence as a newer schema.
  (Today `1.5` or `'abc'` slip into odd paths.) `api.migrate` is unchanged.
- `api.apply`, `api.lastMade`, `api.sayOpened`: unchanged. A same-schema design from a newer
  release opens silently and saves as this editor's release.
- Header comment: the refusals bullet no longer lists "a design from a newer release of the
  app"; it says "a newer schema, or an older one with no step". Add `__ORB_APP.schema` to
  the session-usage comment in one short clause (the start check).

### 5. Tests

- `tools/orb-manager/web/runtime/_selftest/headless-session.test.js`, the design-document
  block: replace the assertion "a design from a newer release is refused" with
  **"a same-schema design from a newer release opens"** (`appVersion: 'v1.4.0+0000000'`
  → `apply` returns `null`, the content is applied, `lastMade` is `null`), keep the older /
  other-build ones, and add:
  1. `appVersion: 'v9.0.0'` with `schema: 1` opens (a much newer release, same schema);
  2. `schema: 2` with `appVersion: 'v1.2.0'` (an *older* release) is still refused with
     the `reads schema 1` sentence;
  3. `schema` of `0`, `1.5`, `'abc'`, missing → refused with the same sentence;
  4. `schema: '1'` (a numeric string) opens;
  5. **start check**: with `window.__ORB_APP = {slug:'tint', version:'v1.3.0', schema: 2}`
     and an editor whose `schema()` is 1, `OrbTheme.session(...)` throws an Error whose
     message contains `reads design schema 1, but its editor.json says 2`; with
     `schema: 1` it starts; with no `schema` key it starts; with no `__ORB_APP` it starts.
     Restore `window.__ORB_APP` afterwards as the block expects.
- `apps/clock/editor/_selftest/headless-design.test.js`: the two assertions that a newer
  clock (`v1.10.0`, and `v1.3.0+3f9a1c2`) is refused become assertions that they **open**
  (`=== null`), with names saying a same-schema design from a newer release opens (ruling 21).
  Keep the rest. If another clock suite asserts the old refusal, flip it the same way and
  list it in your report.
- `tools/host_sdk_editor_test.py`: the scratch app's default `editor.json` lists `theme@1`,
  so give it `"schema": 1` and update the canonical-bytes pin and the `orb-app.js` pin
  accordingly. Add checks: `schema` `0`, `-1`, `1.5`, `"2"`, `true` refused (`"design schema"`);
  `theme@1` listed and no `schema` refused (`"must name the design schema"`); no `theme@1`
  and no `schema` packs, and its `orb-app.js` has no `schema` key; `schema: 3` stamps
  `"schema": 3` into `orb-app.js`; `editor_schema` returns 3 / `None` for those two. Add
  `MUTANTS` rows for "schema type unchecked" and "theme@1 without schema passes", and
  check that `python3 tools/host_sdk_editor_test.py --mutations` passes (if a mutant row
  that already existed before your change is stale — its text not found — say so in the
  report and fix its text to match today's code).
- `tools/host_clock_region_test.py`: pin `editor.json`'s `schema` equal to
  `design_schema()` and to `1` today, and the inline `__ORB_APP` / `orb-app.js` stamps
  carrying the same schema.

### 6. `extract-editors` reports the schema

`tools/orb-manager/extract.go`: `extractEditor` gains
`Schema int \`json:"schema,omitempty"\`` (comment: `editor.json's design schema, 0 when it
names none`), filled from the region's `editor.json` (a non-integer or missing value is 0;
never a refusal — the hub does not judge schemas). `extract_test.go`'s manifest comparison
also compares `schema`. No other Go change.

### 7. Rebuild the shelf

The editor regions of clock, flights and weather change, so their `.orbapp` files must be
rebuilt: `python3 tools/make_factory_apps.py` (clang 21 is installed in this worker:
`ORB_CLANG=/usr/lib/llvm-21/bin/clang` is already set in the environment). **Before
changing anything**, run `python3 tools/make_factory_apps.py --check` once and confirm it
says the seed containers are fresh (that proves this clang reproduces the shelf); if it
does not, stop and report it with its output. After the rebuild only
`tools/orb-manager/apps/{clock,flights,weather}.orbapp` may change; `radar.orbapp` and
`color_picker.orbapp` must be byte-identical. Do **not** change any `version.txt`.

## Acceptance criteria

Run from the repo root (put Go on `PATH` first: `export PATH=/usr/local/go/bin:$PATH`).

1. `python3 tools/make_factory_apps.py --check` → `seed containers fresh` (after your rebuild).
2. `git status --short tools/orb-manager/apps` → exactly the three `.orbapp` files above.
3. `make check-apps` → passes.
4. `python3 tools/host_sdk_editor_test.py && python3 tools/host_sdk_editor_test.py --mutations`
   → both pass. `python3 tools/host_clock_region_test.py` → passes.
   `python3 tools/host_orbapp_install_test.py` → passes.
5. `make check-editor` → passes (runtime suites `all N suites green`, the clock suite twice).
6. `make -C tools/orb-manager check` → passes.
7. `go -C tools/orb-manager build -o /tmp/om . && rm -rf /tmp/ex && /tmp/om extract-editors tools/orb-manager/apps /tmp/ex && cat /tmp/ex/flights/editor.json /tmp/ex/flights/orb-app.js /tmp/ex/clock/editor.json /tmp/ex/clock/orb-app.js && grep -o '"schema":[0-9]*' /tmp/ex/editors.json`
   → flights' `editor.json` has `"schema":2` and its `orb-app.js` `"schema": 2`; the
   clock's both say 1; `editors.json` reports schema 2, 1 and 1 for flights, clock and
   weather, and none for radar. Paste the output in your report.

## Out of scope

- The migration runner, `check-migrations`, the build's fixture rule, `_fixtures/`, the
  SDK template, docs (`apps/README.md`, `BUILDING.md`): later steps.
- Any change to the editors' code (`apps/*/editor/*.js`, the composer `.html`), to
  `version.txt` files, `main/`, `components/`, `.github/`.
- `send.js`, `studio@1`, the GorbHost block, `fake-hub.js` (unless a test truly needs it;
  say so).

## Notes

- Code style: ES5 `var`/`function` in the runtime IIFE, single quotes, terse comments that
  say why; Python in the files' own voice (`fail(...)` sentences, no f-strings where the file
  uses `%`).
- `make check-editor` runs Python reference lines that need Pillow/NumPy; both are installed.
- Commits: plain subject lines in the repo's style, no Co-Authored-By or tool-credit trailers.
