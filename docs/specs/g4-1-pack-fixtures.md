# G4 step 1: `orbsdk.py` packs `_fixtures/` into the editor region, and requires every past fixture

## Goal

Slice G4 (ruling 36) of the orb-os-net design, gorb-os half, step 1 of 2 — the source of truth
for the region format. `orbsdk.py`'s editor region (the bytes a `.orbapp` carries) today holds
only the editor directory's top-level files; the schema fixtures an app keeps for the migration
check stay in the source tree, so a hub or orb-os.net holding only the container cannot run the
check from it. This step packs the editor's `_fixtures/schema-<n>.orbdesign` samples into the
region, tightens the build's fixture rule to require a fixture for **every** past schema (not
only N-1) and to refuse the same stray/newer names `check-migrations` already refuses, updates
the host test's now-wrong assertions, and regenerates the shelf `.orbapp`s so they carry their
fixtures.

## Read first

- `docs/design.md` (read-only at `/home/greg/docker/worktrees/orb-os-net-g4-lead/docs/design.md`):
  ruling 36 verbatim (§0); §6.5 point 4 (the migration check and its fixtures).
- `docs/specs/g4-design.md` — the format contract, the tightened rule, the ordering; this step is
  "g4-1" there.
- `apps/sdk/orbsdk.py`: `EDITOR_KEYS` (~406), `editor_dir` (~413), `editor_schema` (~425),
  `FIXTURES`/`fixture_name` (~440-443), `fixtures_dir` (~461), `schema_fixture_rule` (~472),
  `editor_region` (~488, the `files = sorted(... os.path.isfile ...)` scan at ~510, the members
  build ~543, the ZIP write + read-back ~549-563), `container` (~568), `FIXTURE_RE` (~804),
  `_fixtures_to_run` (~807, the scan to mirror), `cmd_check_migrations` (~837).
- `tools/make_factory_apps.py`: `editor_region(slug, m)` now returns `b""`; the shelf is built by
  `orbsdk` through `container`. The `apps` make target and `--check` freshness byte-compare.
- `tools/host_sdk_editor_test.py`: the region-shape assertions (~77 flat names, ~166-168
  "`_fixtures` stays out of the region"), the schema-fixture refusals (~160-166), and `MUTANTS`
  (~250-259, esp. `"subdirectories ride"` and `"no fixture rule"`).
- `tools/orb-manager/extract.go`: `extractEditorRegion`/`plainRelPath`/`writeRegion` already
  unpack nested members unchanged — the reader to prove compatibility against (no edit).

## Contract

### 1. `apps/sdk/orbsdk.py` — the region carries fixtures

- In `editor_region`, after computing the top-level `files` list (keep that scan and its
  `os.path.isfile` — it still excludes **every** subdirectory, so `_selftest/` and any other dir
  stay out), build a **separate** fixture-member list from `fixtures_dir(appdir, m)`: every file
  directly under `_fixtures/` whose name matches `FIXTURE_RE` **and** whose schema `n ≤ schema`
  (past or own). A name under `_fixtures/` that is not `schema-<n>.orbdesign`, and any `n >
  schema`, are `fail(...)` using the **same sentences** `_fixtures_to_run` (orbsdk.py:814-821)
  uses today (factor that scan into a shared helper both call, so build and check cannot diverge).
  A `_fixtures/` folder that does not exist contributes nothing.
- The emitted member list is `["editor.json"]` first, then **one** sorted list of full member
  names: the top-level `files`, `"orb-app.js"`, and the fixtures keyed as `_fixtures/schema-<n>.orbdesign`
  (their full relative path). `_` (0x5F) sorts before letters, so fixtures appear before top-level
  `.js` files — deterministic. Everything else (STORE, ZIP_DATE, `0o644<<16`, canonical
  `editor.json`, the read-back CRC loop, the `EDITOR_MAX` cap) is unchanged.
- The fixture bytes are the raw file bytes; no transform. They read back like any member.

### 2. The build's fixture rule (tightened)

- `schema_fixture_rule(slug, schema, fdir)`: for `schema ≥ 2`, require a fixture for **every**
  past schema `k` in `1 … schema-1`; `fail(...)` naming the **first** missing `k`, with the
  existing "...must keep a design of schema N... check-migrations proves... `--save-current`
  before raising schema" sentence. Schema `None`/`1` needs nothing. Reuse the same scan/helper as
  §1 so a stray or newer fixture is refused here too, before packing.
- Keep the `editor_region` call site ordering (schema check → fixture rule → build the region).
  `tools/make_factory_apps.py` `editor_region` returning `b""` is correct (the SDK is the writer);
  do not add fixture logic there — `container` calls `editor_region`, which now enforces the rule.

### 3. Regenerate the shelf (`make apps`)

Run the make target that rebuilds `tools/orb-manager/apps/*.orbapp` (the same one the `--check`
freshness gate byte-compares). After this step, `clock.orbapp`, `weather.orbapp` and
`flights.orbapp` carry their `_fixtures/schema-<n>.orbdesign` in the editor region;
`radar.orbapp`/`color_picker.orbapp` are unchanged (no schema fixtures). The committed shelf
binaries must be the ones `make apps` produces, or `--check` fails.

## Behaviour and edge cases

- schema 1, no `_fixtures/` → packs nothing; region is the top-level files only (unchanged bytes).
- schema 1 **with** an own-schema `schema-1.orbdesign` (clock, weather) → packs that one fixture;
  it is neither "newer" nor stray, so it travels and `check-migrations` still opens it. **This is
  the case a careless implementation breaks by refusing an own-schema fixture — it must not.**
- schema 3 holding `schema-1` and `schema-2` but not `schema-1`… → holds `schema-1`+`schema-2`
  (full past set) → packs both. Holds only `schema-2` → **refused naming schema-1, the first gap**
  (the case the old N-1-only rule let through). Holds only `schema-1` → refused naming schema-2.
- `_fixtures/notes.txt` or `_fixtures/schema-x.orbdesign` → refused (stray name), same sentence as
  `_fixtures_to_run`. `_fixtures/schema-9.orbdesign` on a schema-3 editor → refused (newer).
- `_fixtures/` present but empty, editor schema ≥ 2 → refused (missing past schema), no fixture run.
- A `_selftest/` or any non-`_fixtures` subdirectory still does **not** ride (the `files` scan's
  `os.path.isfile` is retained; the `"subdirectories ride"` MUTANT at host test ~256, whose
  old-string is `and os.path.isfile(os.path.join(d, n)))`, must still trip a build — so factor the
  `os.path.isfile` guard so removing it lets `_fixtures/`/`_selftest/` dirs ride as before).
- Rebuild determinism: two builds of the same app dir are byte-identical (fixture order fixed by
  the sorted full-name list).
- `check-migrations` after this step passes from the **source tree** exactly as before, and a new
  test proves the **packed region** unpacks fixtures whose bytes match the source (region and tree
  agree).

## Acceptance criteria

Each is a command the worker runs and reports.

1. `python3 tools/host_sdk_editor_test.py` → exit 0, all checks pass, with the region-shape
   assertions updated to: fixtures of past/own schema **are** in the region, `_selftest`/dotfiles
   are not, and the schema refusals cover the first-gap-not-only-N-1, stray and newer cases.
2. `bash docs/specs/g4-gotest.sh` (or `cd tools/orb-manager && go test ./...`) → all green; this
   step adds no Go test of its own (extract.go already unpacks nested members, and g4-2 adds the
   explicit `TestExtractEditorsUnpacksFixtures` regression for it), but the whole Go editor suite
   must still pass with a g4-1 region present.
3. `make apps && make check-apps` → `check-apps` (the shelf freshness `--check`) passes, so the
   committed `.orbapp`s match a fresh build that includes the fixtures.
4. `make check-editor check-migrations` → exit 0 (the SDK/host tests and the migration check over
   source-tree fixtures still pass).
5. `python3 -c "import sys; sys.path.insert(0,'apps/sdk'); import orbsdk; ..."` OR reading
   `clock.orbapp`/`flights.orbapp` via `tools/orbapp.py` shows the editor region ZIP lists
   `_fixtures/schema-1.orbdesign` (clock) and `_fixtures/schema-1.orbdesign`+`_fixtures/schema-2.orbdesign`
   (flights).

## Out of scope

- Do not change `tools/orb-manager/extract.go`, `editor.go` (g4-2), the firmware, or any site file.
- Do not add a fixture for a schema an app does not have; do not rename existing fixtures.
- Ruling 37 (CSP/fetching fixtures over HTTP) is a different slice — not here.
- Do not modify `orb-web.lock.json`; no push/merge/deploy.

## Notes

- The own-schema fixture travelling (clock/weather) is the subtle correctness point: `check-migrations`
  already accepts and runs an own-schema sample, and ruling 36's build rule is only about *past*
  schemas and *stray/newer* names. Mirror `_fixtures_to_run` and both hold.
- `editor_region` runs before compilation in `cmd_build`, so a fixture refusal needs no clang.
- Keep the region's read-back loop unchanged: it will now verify the fixture members too, for free.
