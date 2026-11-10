# G4 — Fixtures travel in the `.orbapp` (ruling 36)

Design only. This file says **what** each step builds and **why it is cut here**; the step
specs under `docs/specs/g4-*.md` are the contract an implementer works from. No product code
in this slice.

## The ruling, in one paragraph

Ruling 36 (docs/design.md:205-213) closes the gap between "the shelf app carries an editor"
and "the shelf app can prove its old designs still open". Today `_fixtures/schema-N.orbdesign`
lives only in the source tree (`apps/<slug>/editor/_fixtures/`); it never reaches the `.orbapp`,
so a hub — or orb-os.net — that holds only the container cannot run the migration check from
the container alone. Ruling 36 says: pack the fixtures **into the editor region** as
`_fixtures/schema-N.orbdesign`, **refuse at build** an app whose schema rose without a fixture
for **every** past schema (1 … N-1, not only N-1), **rebuild the shelf apps** so their
`.orbapp`s carry the fixtures, and make sure the **site does not serve `_fixtures/`** on a
studio origin. The region stays deterministic (fixed date, fixed mode, STORE, `editor.json`
first, names in a fixed order), and is read back before it ships.

## What each reader does today, file by file

The fixture rule must not surprise a reader that only ever saw top-level editor files. These
are the places a region's member list is read, checked, served, or byte-compared:

- **`apps/sdk/orbsdk.py` — the writer and the one real rule.** `editor_region()` (orbsdk.py:488)
  packs only the directory's top-level files: `files = sorted(n for n in os.listdir(d) if not
  n.startswith(".") and n != "editor.json" and os.path.isfile(...))` (orbsdk.py:510). A
  subdirectory such as `_fixtures/` is skipped, because `os.path.isfile` is false for a
  directory. The region is STORE-only, ZIP_DATE fixed `(1980,1,1,0,0,0)`, mode `0o644<<16`,
  `editor.json` written first, then the rest in name order with the `orb-app.js` stamp
  (orbsdk.py:543-557); it is read back and CRC-checked before it ships (orbsdk.py:558-563) and
  capped at `orbapp.EDITOR_MAX` (2 MiB). `schema_fixture_rule()` (orbsdk.py:472) is the **only**
  fixture rule and it is too weak: for an editor on schema N it checks that `schema-(N-1)`
  exists and stops. `container()` (orbsdk.py:568) calls `editor_region()` when app.json names an
  editor and no region was passed in.
- **`tools/make_factory_apps.py`** — `editor_region(slug, m)` now returns `b""`: the clock became a
  directory editor packed by `orbsdk` in E7, so the SDK is the single writer. The `--check`
  freshness gate byte-compares `tools/orb-manager/apps/*.orbapp`. Regenerating the shelf (the
  `apps` make target) is therefore what makes the shelf fixtures real bytes.
- **`tools/orbapp.py`** — `EDITOR_MAX = 0x200000` (2 MiB). The editor region is opaque bytes to
  the container writer; it imposes **no member-name rule**, so nested names are already allowed.
- **`tools/orb-manager/extract.go` — already keeps fixtures.** `extractEditorRegion` (extract.go:338)
  writes **every** ZIP member; `plainRelPath` (extract.go:456) accepts `/`, so a nested
  `_fixtures/schema-1.orbdesign` is a legal target, and `writeRegion` (extract.go:514)
  `MkdirAll`s the parent before writing. The report's `files` list already includes them, so
  `extract-editors` output matches ruling 36 with **no change**.
- **`main/app/orb_app_manifest.c`** — the firmware checks the region's size against
  `ORBAPP_EDITOR_MAX` and nothing else; the region is opaque. Firmware needs **no change**.
- **`tools/orb-manager/editor.go` — the hub serves fixtures today.** `serveEditor` (editor.go:388)
  and `serveHubEditor` (editor.go:430) both reject only `..` and a leading `/`, then hand a
  cleaned path to `serveEditorFile` (editor.go:508), which opens whatever member the name names.
  Nothing denies a `_fixtures/` prefix, so the hub's editor origin **would** serve
  `/_fixtures/schema-1.orbdesign`. Ruling 36's second half — the site does not serve fixtures —
  is silent about the hub, but the same reasoning (a fixture is migration evidence, not part of
  the running editor) applies to the hub door. This is an **Owner question** (below).
- **Site readers** (the `orb-os-net` repo, read-only here):
  - `src/lib/orb-upload.ts` — `readEditorFiles` (orb-upload.ts:216) re-reads and re-hashes **every**
    file the report lists, and `readContainer` (orb-upload.ts:293) puts them all on `Container.editorFiles`.
    `review.ts` writes those to `editor_file` rows on go-live (review.ts:261-285). Today
    `tests/orb-upload.test.ts:378` asserts a `_fixtures/schema-1.orbdesign` **is** an editorFile row.
  - `src/lib/orb-web-loader.ts` — `readEditorFiles` (orb-web-loader.ts:426) is the same discipline for
    a hub batch load; it lands `editor_file` rows the same way (orb-web-loader.ts:688).
  - `src/app/studio-origin/[slug]/[[...path]]/route.ts` — `plainName` (route.ts:113) accepts nested
    names, so `GET /<ver>/_fixtures/schema-1.orbdesign` **would** serve the fixture from its
    `editor_file` row. This is the ruling-36 gap on the site side.
  - `src/lib/check-plan.ts` — `readFixtures` (check-plan.ts:50) reads the fixtures **from disk**
    (`editorDir/_fixtures`), independent of the DB rows. So the migration check does **not** need
    the `editor_file` rows to serve fixtures; excluding fixtures from those rows does not break it.

## The region format after G4 (the contract, precisely)

Same determinism, now with fixtures folded into the one sorted member list:

- Members are `(name, bytes)` pairs. `editor.json` is first, always (unchanged).
- After it, the member names are the **full relative paths** of everything that travels: the
  editor's top-level files (as today), the `orb-app.js` stamp, **and** every
  `_fixtures/schema-<n>.orbdesign` the check would accept — i.e. every fixture whose `n` is at
  or below the editor's own schema (the past schemas 1 … schema-1, and an own-schema sample the
  check also opens). They are emitted in **one** lexicographically-sorted list of full member
  names (`_fixtures/...` sorts before a top-level `.js` because `_` (0x5F) < alphanumerics —
  deterministic, matching today's style). The clock and weather (schema 1, each holding a
  `schema-1.orbdesign` sample) therefore pack that one own-schema fixture, exactly as
  `check-migrations` opens it.
- STORE only, ZIP_DATE `(1980,1,1,0,0,0)`, mode `0o644<<16`, read back and CRC-verified, capped
  at `EDITOR_MAX` — all unchanged. A fixture is just another opaque member.
- What a region must **not** carry, refused at build like `check-migrations` refuses it today
  (orbsdk.py:814-821): a name under `_fixtures/` that is not `schema-<n>.orbdesign`, and a
  fixture whose `n` is **newer** than the editor's own schema. Past schemas below the editor's
  are required present (the build rule, tightened below). `_selftest/` and dotfiles stay home
  (unchanged).

## The build rule, tightened (ruling 36's second sentence)

`schema_fixture_rule` (orbsdk.py:472) currently stops at `schema-(N-1)`. Ruling 36 requires a
fixture for **every** past schema 1 … N-1, so a two-hop-old design can still be proven to open.
The tightened rule mirrors what `_fixtures_to_run` (orbsdk.py:807) already enforces at check
time, so build and check refuse the same things: a missing **past** schema fails (naming the
first gap), and a stray name or a fixture **newer** than the editor's own schema fails. The
cleanest implementation is for the build to reuse `_fixtures_to_run`'s scan (or factor the scan
into one helper the two share) rather than invent a second, divergent rule. The rule applies to
every editor with a schema ≥ 2; clock=1 and weather=1 are unaffected except that their own-schema
`schema-1.orbdesign` now packs, and flights=2 keeps requiring `schema-1` (which it holds).

## The migration check already fits, in both directions

`check-migrations` (orbsdk.py:804+) reads fixtures from the **source tree** (`editor/_fixtures`)
via `FIXTURE_RE`/`_fixtures_to_run` (orbsdk.py:804-807), and `_fixtures_to_run` **already loops
`range(1, schema)`** and fails on a missing past schema — so the source-tree side of ruling 36's
"every past schema" rule is enforced there today and stays. G4 does not weaken it; the SDK build
rule is the front door, `check-migrations` remains the back. The one addition G4 makes worth
proving: after packing, `check-migrations` must still pass whether it reads the source tree or
the region unpacked by `extract-editors`, since the two must now agree byte for byte.

## What G4 does **not** touch (waits for a later slice)

- **Ruling 37** (the CSP allowance so an editor can `fetch` its own fixtures) — out of scope
  here; it is a separate ruling and not implied by packing bytes into a region the editor never
  fetches over HTTP.
- **Firmware** — no member-name rule exists to add; opaque region, size cap only.
- **`_selftest/`** — stays out of the region, unchanged.
- **Radar editor** — radar has no schema and no fixtures; unaffected.
- Any change to `orb-web.lock.json`, pushes, merges, deploys: none.

## The gate (run per step; the reviewer re-runs it)

- gorb-os: `make check-apps check-hub check-editor check-web check-runner check-migrations`, and
  **`check-editor-origin`** for the step that changes hub editor serving (g4-2). Regenerating the
  shelf is `make apps` (g4-1). `g4-gotest.sh` runs the Go editor tests by name and fails if a step
  touched a file it must not.
- site: `npx vitest run tests/orb-upload.test.ts tests/check-plan.test.ts tests/studio-no-signin.test.ts
  tests/review.test.ts tests/editor-version.test.ts tests/d1-gate.test.ts` (re-run
  `review.test.ts` alone if the only failure is its known-in-parallel 503-no-checker case), then
  `npx tsc --noEmit` and `npm run build`. No API keys, no `.env`.

## Ordering / why three steps

1. **g4-1 (gorb-os, source of truth for the format).** Pack fixtures into the region, tighten the
   build rule to every-past-schema, refuse stray/own-or-newer fixtures, update the host test's
   now-wrong assertions, and `make apps` so the shelf `.orbapp`s carry fixtures. Everything
   downstream reads the region g4-1 writes, so it is first.
2. **g4-2 (gorb-os hub).** Refuse `_fixtures/` on the hub's editor doors (defense-in-depth, Owner
   question answered yes below), keeping `extract-editors` unchanged (it must still unpack them).
3. **g4-site-1 (orb-os-net).** Exclude `_fixtures/` from what becomes an `editor_file` row and
   refuse it at the studio origin, so the site does not serve fixtures (ruling 36's last line).
   It depends only on g4-1 having produced containers that carry fixtures; the site reads them
   the same way regardless.

g4-2 and g4-site-1 both depend on g4-1 (they need real fixtures to test against) but are
independent of each other and of different repos, so they are separate plans: the gorb-os chain
runs g4-1 then g4-2; the site chain runs g4-site-1.

## Owner questions

1. **Does the hub refuse `_fixtures/` too, or only the site?** Ruling 36 names only the site, but
   the hub editor origin serving a fixture is the same leak the ruling closes on the site.
   **PICKED: yes, refuse on the hub too (g4-2).** It is defense-in-depth, cheap, consistent with
   the ruling's reason (a fixture is evidence, not editor code), and `extract-editors` still
   unpacks them, so migration checks are unaffected. If the orchestrator would rather the hub
   keep serving them, drop g4-2 alone; nothing else depends on it.
2. **Can a reviewer see the fixtures in the review diff?** Ruling 36 does not say. **PICKED: no —
   fixtures are filtered out of the `editor_file`/`review_file` rows, so they never appear in a
   review diff or on the studio origin; the reviewer judges the container, which the SDK already
   read back.** This keeps the site's serving surface to editor code only. If the orchestrator
   wants fixtures visible in review, that is a one-line change in g4-site-1's row filter.

