# G2 step 3: `orbsdk.py check-migrations`, the build's fixture rule, the template step

## Goal

Slice G2 of the orb-os-net design (§12), third step (§6.5 point 4). An app keeps one
sample design per past schema in `editor/_fixtures/schema-<n>.orbdesign`.
`orbsdk.py check-migrations <appdir>` runs the migration runner (step 2) over them in the
app's own packed editor, so removing a migration step fails it. `orbsdk.py build`/`pack`
(and the tree's `make apps`) refuse an editor whose `editor.json` `schema` is N > 1 when
there is no `schema-<N-1>.orbdesign` fixture, so raising the schema without keeping a
sample of the one before fails the build. The SDK template's workflow runs the check
before packing, the SDK tarball carries the runtime and the runner so that works outside
this repo, and the docs say how designs, schemas and migrations work.

## Read first

- `/home/greg/docker/Projects/orb-os-net/docs/design.md` (read-only mount): §0 rulings 20,
  21; §6.5 in full; §1's bullet on outside apps' release process.
- `apps/sdk/orbsdk.py` in full (after step 1: `EDITOR_KEYS`, `editor_schema`,
  `editor_region`, `container`, `cmd_build`, `cmd_pack`, `cmd_dist`, `DIST_*`, `main`).
- `tools/make_factory_apps.py` (`editor_region`, the clock builder) and
  `tools/make_clock_region.py` (`design_schema`, `make_region`, `members`).
- `tools/host_sdk_editor_test.py` in full: where the new SDK checks go.
- `tools/orb-manager/web/runtime/_runner/migrate.js` (step 2): its arguments, output and
  exit codes. `tools/orb-manager/web/runtime/_selftest/runner-check.js` for how it is driven.
- `apps/*/editor/_fixtures/` (step 2's fixtures).
- `apps/sdk/template/` in full: `Makefile`, `.github/workflows/build.yml`, `README.md`.
- `tools/orb-manager/packaging/orb-web.sh`; root `Makefile` (`check-*`, `sdk`, header
  comment list); `.github/workflows/ci.yml` (jobs `apps`, `editor`).
- Docs: `apps/README.md` (the key table, "The SDK", "Folding an app into gorb-os"),
  `BUILDING.md` (§ The one command, § Apps), `RELEASING.md` (the file table, "What is in
  the box").

## Contract

### 1. The fixture rule (`apps/sdk/orbsdk.py`)

- `FIXTURES = "_fixtures"`; `def fixture_name(n): return "schema-%d.orbdesign" % n`.
- `def fixtures_dir(appdir, m)` → `<editor dir>/_fixtures` for an app whose `editor` names a
  directory (`editor_dir`), else `<appdir>/editor/_fixtures` (a tree-built editor, the
  clock's), else `None` when the app has no `editor` key.
- `def schema_fixture_rule(slug, schema, fdir)`: when `schema` is an int > 1 and
  `<fdir>/schema-<schema-1>.orbdesign` is not a file, `fail("%s's editor reads design
  schema %d, so %s must keep a design of schema %d: the previous release's designs migrate
  from it, and check-migrations proves they still open. Save one with that release
  (orbsdk.py check-migrations --save-current, run before raising schema) and commit it" %
  (slug, schema, <fdir relative to the app dir, e.g. "editor/_fixtures/schema-1.orbdesign">, schema - 1))`.
  Schema 1 or `None` needs nothing.
- `editor_region` calls it right after the schema check.
- `cmd_build` (and so `orbsdk.py build`): for an app whose `editor` names a directory, run
  `editor_region(d, manifest(d), version(d))` **before** compiling, so a schema/fixture
  refusal comes first and needs no clang. (`container`, used by `pack` and `install`,
  already builds the region before compiling: keep that order.)
- `tools/make_factory_apps.py` `editor_region`, the `"clock"` branch: after
  `clock_region.make_region()`, call
  `orbsdk.schema_fixture_rule("clock", clock_region.design_schema(), orbsdk.fixtures_dir(<apps/clock>, m))`.
  So `make apps` and `make_factory_apps.py --check` refuse a raised clock schema with no
  fixture too.

### 2. `orbsdk.py check-migrations`

```
orbsdk.py check-migrations [<appdir>...] [--runtime DIR] [--chrome PATH] [--save-current]
```
(default appdir `.`; add it to the module docstring's command list and to `main`.)

For each app, in order, printing lines prefixed with the slug:

1. The packed editor: for a directory editor, unzip `editor_region(appdir, m, version)`
   into a temp dir; in a gorb-os clone, for a tree-built editor (`tools/make_<key>_region.py`
   exists for app.json's `editor` key), import that module and unzip its `make_region()`.
   No `editor` key → `<slug>: no editor, nothing to migrate`, next app. An `editor` key the
   SDK cannot build → fail as `container` does today.
2. `schema` from the packed `editor.json` (`editor_schema`). `None` →
   `<slug>: its editor names no design schema, nothing to migrate`, next app.
3. The fixtures: every file in `fixtures_dir` matching `^schema-(\d+)\.orbdesign$`. A
   file there that does not match that name → fail (`<file> in _fixtures is not named
   schema-<n>.orbdesign`). A fixture with n > schema → fail (`schema-<n>.orbdesign is
   newer than the schema this editor reads (<schema>)`). Each past schema 1..schema-1
   with no fixture → fail (`no fixture for schema <n>: check-migrations keeps one design per
   past schema`). These checks run before anything needs node or Chromium.
4. No fixtures at all (only possible at schema 1) → `<slug>: schema 1 and no fixtures,
   nothing to migrate yet`, next app.
5. Run `<node> <runtime>/_runner/migrate.js --runtime <runtime> --editor <tmp> [--chrome
   PATH] <fixtures in schema order>`, streaming its output. `<node>` is `$ORB_NODE`, else
   `node`; not found → fail (`check-migrations runs the editor in Node and Chromium: install
   Node 18+ and Chromium, or set ORB_NODE`). `<runtime>` is `--runtime`, else
   `<clone>/tools/orb-manager/web/runtime` in a clone, else `<sdk dir>/runtime` in a
   tarball. A non-zero exit → fail (`<slug>: a past design no longer opens in this editor`).
6. `--save-current`: instead of 3–5, run the runner with
   `--save-current <fixtures_dir>/schema-<schema>.orbdesign` (create the folder), refusing
   when that file exists (`... exists: a fixture is the design that release wrote, keep it`).

Exit 0 only when every app passed. The temp dirs are removed.

### 3. The SDK tarball carries the runtime

`DIST_*` / `cmd_dist`: add the clone's `tools/orb-manager/web/runtime/` tree as `runtime/`
in the tarball, every file except under `_selftest/` (so `runtime/_runner/migrate.js`,
`runtime/theme@1/…`, `runtime/vendor/…` ride). Deterministic as today. Print the
tarball's size in the `wrote` line (`(%d files, %d KB)`). The CI step "SDK tarball builds a
fresh app" in `.github/workflows/ci.yml` gains, after `make SDK=...`:
`make check-migrations SDK=/tmp/sdk/orb-sdk-$v` (the fresh app has no editor, so it says
nothing to migrate and passes).

### 4. `orb-web.sh` carries the runner

`tools/orb-manager/packaging/orb-web.sh`: also copy `web/runtime/_runner/` to
`orb-web-<version>/runner/` (the site's migration check runs `runner/migrate.js` with
`--runtime runtime/`). `export-runtime` does not carry it (it is not a runtime id).
Keep the archive reproducible; update the script's usage/comment and
`RELEASING.md`'s `orb-web` row.

### 5. The template

- `apps/sdk/template/Makefile`: `.PHONY` gains `check-migrations`; target
  `check-migrations: $(SDK)/orbsdk.py` → `$(ORBSDK) check-migrations .`; one line in the
  header comment.
- `apps/sdk/template/.github/workflows/build.yml`: between `Toolchain` and `Build`:
  ```yaml
      # Every past design schema's sample (editor/_fixtures/) must still open in this
      # editor; an app with no editor passes. Node and Chrome come with the runner image.
      - name: Migrations
        run: make check-migrations
  ```
- `apps/sdk/template/README.md`: in Layout, one row for `editor/_fixtures/` ("only once the
  app has a theme editor: one design per past schema, see apps/README.md"); in Release,
  one sentence that the workflow checks migrations before it packs.

### 6. The tree

- Root `Makefile`: target `check-migrations` (in `.PHONY`, a header comment line saying
  it needs Node and Chromium and so is not in `check`, like `check-web`):
  `$(PYTHON) apps/sdk/orbsdk.py check-migrations $(sort $(dir $(wildcard apps/*/app.json)))`
  (or an equivalent list of every app directory).
- `.github/workflows/ci.yml`, job `editor`: after `make check-editor`, a step
  `run: make check-runner check-migrations` with a comment that the runner image carries
  Chrome (set `CHROME` to `google-chrome` if the runner's search needs it; say what you
  chose).

### 7. Tests (`tools/host_sdk_editor_test.py`)

Add, all without clang, node or Chromium:
1. schema 2 with no `_fixtures/` → `editor_region` refused (`must keep a design of schema 1`);
   with `editor/_fixtures/schema-1.orbdesign` → packs; schema 1 with no fixtures packs;
   schema 3 with only `schema-1` → refused naming `schema-2`.
2. `orbsdk.main(["build", d])` with `ORB_CLANG=/nonexistent/clang` on the schema-2-no-fixture
   app → refused with the fixture sentence, not the clang one.
3. `check-migrations` on an app with no editor → exit 0, `nothing to migrate`; on an
   editor with no `theme@1` and no schema → `names no design schema`; a fixture
   `schema-5.orbdesign` beside schema 2 → refused (`newer than`); schema 3 with
   `schema-2` only → refused (`no fixture for schema 1`); a stray `notes.txt` in
   `_fixtures` → refused. (Call `main([...])` and catch `SystemExit`; none of these may
   reach node.)
4. `MUTANTS` rows: "no fixture rule" (the `editor_region` call removed) and "build compiles
   first" (the `cmd_build` region call removed).

### 8. Docs

- `apps/README.md`: the `editor` row mentions `schema` (the design schema the editor reads,
  required when it lists `theme@1`). A new section **Theme designs, schemas and
  migrations**, after the key table's section or near the editor material, short and in
  the file's voice: a design document's envelope; `schema` in `editor.json` must equal the
  editor's `schema()` (the session refuses to start otherwise); a design opens in any
  editor with the same schema, whichever release saved it, and is refused only by a newer
  schema (ruling 21) — so **any** change to what a design holds raises the schema by one and
  adds one pure step `n → n+1`; keep `editor/_fixtures/schema-<n>.orbdesign` for every past
  schema (save the current one with `orbsdk.py check-migrations --save-current` before
  raising); the build refuses a raised schema without the previous one; `orbsdk.py
  check-migrations` opens each in the packed editor; the template's CI runs it.
- `BUILDING.md`: § The one command — `check-runner` and `check-migrations` beside
  `check-web` (not in `check`, need Chromium); § Apps — one line for `make check-migrations`.
- `RELEASING.md`: the `orb-sdk` row says it carries the editor runtime and the migration
  runner; the `orb-web` row gains `runner/`.

## Acceptance criteria

From the repo root, `export PATH=/usr/local/go/bin:$PATH`.

1. `python3 tools/host_sdk_editor_test.py && python3 tools/host_sdk_editor_test.py --mutations` → pass.
2. `make check-migrations` → clock, flights and weather pass (flights runs 2 fixtures),
   radar and color_picker say nothing to migrate; exit 0.
3. Removing a step: `rm -rf /tmp/fl && cp -r apps/flights /tmp/fl`, then edit
   `/tmp/fl/editor/flights-editor.js` so `migrate` returns `{}`, then
   `python3 apps/sdk/orbsdk.py check-migrations /tmp/fl` → exit 1, and the output names
   `no step from schema 1 to 2`.
4. Raising schema without a fixture: `rm -rf /tmp/w && cp -r apps/weather /tmp/w && rm -rf /tmp/w/editor/_fixtures`,
   set `/tmp/w/editor/editor.json` `schema` to 2, then `python3 apps/sdk/orbsdk.py pack /tmp/w`
   and `python3 apps/sdk/orbsdk.py build /tmp/w` → both exit 1 with the fixture sentence.
   And in the tree: change the clock composer's `DESIGN_SCHEMA = 1;` to `2;` with
   `apps/clock/editor/_fixtures` moved away, `python3 tools/make_factory_apps.py --check`
   → refused with the fixture sentence; restore both (`git checkout`, move back) and show
   `git status --short` clean for them.
5. `make sdk`, unpack it to a temp dir, `python3 <sdk>/orbsdk.py new /tmp/hello`, then
   `make -C /tmp/hello check-migrations SDK=<sdk>` → nothing to migrate, exit 0; and
   `python3 <sdk>/orbsdk.py check-migrations apps/flights` (the tarball's own runtime)
   → passes.
6. `make -C tools/orb-manager web` → the archive lists `orb-web-<v>/runner/migrate.js`;
   built twice → same sha256.
7. YAML of `ci.yml` and the template's `build.yml` parses (`python3 -c "import yaml…"`, or
   say how you checked).
8. `make check-apps`, `make check-editor`, `make check-runner`, `make -C tools/orb-manager check` → pass.

Paste the outputs of 2–6 in your report.

## Out of scope

- The editors' code, `version.txt` files, the `.orbapp` files (none may change in this
  step: `make_factory_apps.py --check` stays fresh), `main/`, `components/`.
- `orbsdk.py publish`, any site code, `release.yml`.
- `migrate.js` itself, except a bug fix you need (say so).

## Notes

- `orbsdk.py` runs in two places (clone and tarball); keep both working, and keep the
  tarball free of anything the clone alone has.
- Python style: the file's `fail("...")` sentences, `%` formatting.
- Commits: plain subject lines in the repo's style, no Co-Authored-By or tool-credit trailers.
