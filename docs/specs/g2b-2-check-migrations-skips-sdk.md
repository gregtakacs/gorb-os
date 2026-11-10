# orbsdk.py check-migrations skips the SDK's own directory

## Goal

Owner ruling, 2026-11-10: `python3 apps/sdk/orbsdk.py check-migrations apps/*/` must exit 0.
Today it exits 1 because the glob also catches `apps/sdk/`, which has no `app.json`. The SDK
directory is the generic SDK for building new apps, not an app, so check-migrations skips it.
Any other directory without `app.json` must still fail exactly as it does now, so that a typo is
never silently skipped.

## Read first

- `apps/sdk/orbsdk.py`:
  - `SDK = os.path.dirname(os.path.abspath(__file__))` (line ~49);
  - `manifest(appdir)` (line ~98, `fail("%s has no app.json" % appdir)`);
  - `cmd_check_migrations` (line ~741).
- `tools/host_sdk_editor_test.py`, around line 190. The `say(argv)` helper and the
  check-migrations checks that use it. Add the new checks there.

## Contract

- In `cmd_check_migrations`'s loop, before `manifest(d)`: when
  `os.path.realpath(d) == os.path.realpath(SDK)`, print exactly
  `<d>: the SDK itself, not an app, skipped` (with `<d>` as given on the command line) and
  `continue`.
- Only `check-migrations` changes. `manifest()` and every other command (build, release, …) keep
  failing on the SDK directory as today.
- A directory without `app.json` that is not the SDK still fails with `<dir> has no app.json`.

## Acceptance criteria

1. New checks in `tools/host_sdk_editor_test.py`, using `say`:
   - `["check-migrations", <SDK dir>]` → rc 0, and the output contains
     `the SDK itself, not an app, skipped`;
   - the same with a trailing slash, and through a relative path, → rc 0;
   - `["check-migrations", <an empty temp dir>]` → rc 1, and `why` contains `has no app.json`;
   - `["check-migrations", <SDK dir>, <empty temp dir>]` → rc 1 (the skip doesn't hide a later
     failure).
2. `python3 tools/host_sdk_editor_test.py` → green.
3. `python3 apps/sdk/orbsdk.py check-migrations apps/*/` at the repo root → exit 0, and the
   output contains `apps/sdk/: the SDK itself, not an app, skipped`.

## Out of scope

- Anything outside `apps/sdk/orbsdk.py` and `tools/host_sdk_editor_test.py`. Don't touch the
  theme@1 hand-off from step 1.
