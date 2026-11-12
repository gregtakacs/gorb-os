# Clear the last stale look names and run the whole looks gate

## Goal

Step 3 of the `looks` slice (design `docs/specs/looks-design.md` §1, §6, §8; closes the third
bullet of `docs/app-platform-plan.md` §64.4). Steps 1–2 shipped weather and flights with their new
art. This step removes the last stale old-look names in `tools/rebake_shelf.py`'s docstring ("Modern
for flights", "Radarscope for the radar") and runs the full cross-app gate end to end to prove the
two shipped apps and the untouched looks together. No app code changes, so no `version.txt` bump.

## Read first

- `docs/specs/looks-design.md` §1 (the two stale names), §5, §6 (the gate).
  `docs/app-platform-plan.md` §64.4; `docs/flights-map-plan.md` §6 (the renames that made these stale).
- `tools/rebake_shelf.py`: the module docstring (`:3-18`) and the comment at `:107`. **Only prose**:
  the look is now Scope in every app; the internal bake file name `modern.orbtheme` that the code
  reads at `:117` is flights' Scope look's art-family filename, not a display name — leave the code
  (and the filename) alone.
- The Makefile `check-apps`/`check-editor` targets, and the two `_selftest/bake_check.py` files.

## Contract

Modify only `tools/rebake_shelf.py`, and only its comment/docstring prose:

- Line ~5: `… the clock, Modern for flights, Scope for the weather, Radarscope for the radar.` →
  `… the clock, Scope for the weather, flights and the radar.` (or prose naming all four defaults
  Scope/Skeleton). Remove the words `Modern` and `Radarscope` as look names.
- Line ~7: `A stale shelf copy is how Modern read "made with v1.2.4"…` → `… how flights' Scope
  read "made with v1.2.4"…`.
- Line ~18: `… bake_check.py's for Modern.` → `… bake_check.py's for flights.`
- Line ~107 comment `keeping Modern.` → `keeping flights' Scope.`

Do not change any executable line, the `DEFAULTS` dict, the `modern.orbtheme` path at `:117`, or
how a shelf file is named.

## Behaviour and edge cases

- `tools/rebake_shelf.py --check` still reports the shelf current (a docstring change does not move
  any byte the gate reads).
- The comment at `:115-116` explaining that flights' bake keeps its default under `modern.orbtheme`
  and the shelf files it as `flights-scope` stays (it documents why the filename differs from the
  look name).

## Acceptance criteria

`$EVID` = `$DELEGATE_EVIDENCE_DIR`.

1. Stale look names gone from both places they were flagged (fails if present):
   `! grep -n "Flights · Modern" apps/weather/editor/weather-bake.js && ! grep -nE "Modern for flights|Radarscope" tools/rebake_shelf.py` → exit 0.
2. The tool still runs: `python3 tools/rebake_shelf.py --check` → prints `shelf defaults current …`,
   exit 0.
3. Weather ships and its looks hold: `python3 apps/weather/editor/_selftest/bake_check.py 2>&1 | tee
   "$EVID/wx-bake3.txt" | tail -1` → `20 themes baked, 0 failed`; and `grep -q "pack: 'guilloche'"
   apps/weather/editor/weather-bake.js && grep -q "pack: 'charted'" apps/weather/editor/weather-bake.js` → exit 0.
4. Flights ships and its looks hold: `python3 apps/flights/editor/_selftest/bake_check.py 2>&1 | tee
   "$EVID/fl-bake3.txt" | tail -1` → `… themes baked, 0 failed`; and `grep -q "'station'"
   apps/flights/art/art.json && grep -q "'sectional'\|\"sectional\"" apps/flights/art/art.json` → exit 0.
5. Both apps' shipped artefacts are fresh: `python3 tools/make_factory_apps.py --check` → the
   weather and flights containers are fresh (no `is stale` line).
6. Whole gate: `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → passes (SDK binds, factory
   apps, default theme, shelf all current).

## Out of scope

- Any app code, other tools, firmware, hub Go, the kit. No `version.txt` change (nothing here edits
  an app). Do not rename `modern.orbtheme` or any shelf file.

## Notes

- This step edits only comments, so it deliberately does **not** re-bake or bump anything: the
  shelf/`.orbapp` freshness comes from steps 1–2. Its job is to prove the shipped pair together and
  finish the stale-name clean-up §64.4 asks for.
