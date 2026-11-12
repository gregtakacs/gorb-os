# fu2-5: make check-apps enforces ruling 39 (bytes moved ⇒ version bumped), applied to this slice

## Goal

Ruling 39: a shelf `.orbapp`, or the hub's firmware bundle, whose working-tree bytes differ from the
newest reachable release tag's copy while its `version.txt` is **not higher** than at that tag is a
gate failure — the app shipped changed bytes under the same version. Add a check for this to
`make check-apps`, make CI able to run it (the `apps` job checks out with no tags today, so the
check would find no reference tag and must be given one), and apply the rule to this slice's own
output: rebuild the shelf and confirm clock/flights/weather stay `v1.7.1` and nothing else moved.
Do not touch orb-os-net.

## Read first

- `docs/specs/fu2-design.md` §"Item 6" and Owner questions 2–3.
- `Makefile`: `check-apps` (line 37: `orbsdk.py binds`, `make_factory_apps.py --check`,
  `make_default_theme.py --check`, `rebake_shelf.py --check`), `apps` (line 121), `rebake`
  (line 115); the `PYTHON ?= python3` var (line 19).
- `tools/rebake_shelf.py` and `tools/make_factory_apps.py` (how the shelf list and container bytes
  are produced; which apps are factory/shelf apps).
- `.github/workflows/ci.yml`: the `apps` job's `actions/checkout@v7` (no `fetch-depth`, ~line 23)
  and the `firmware` job's `fetch-depth: 0` (~line 82) to copy.
- Release/version ground truth: root `version.txt` (`v1.7.0`), each shelf app's version at
  `apps/<slug>/version.txt` (clock/flights/weather `v1.7.1`; radar/color_picker `v1.7.0`), the
  shelf container bytes at `tools/orb-manager/apps/<slug>.orbapp`, the firmware bundle `tools/orb-manager/internal/firmware/bundle/orb-firmware.zip`, and
  `RELEASING.md` (bundle version must equal root `version.txt`). Newest reachable `vX.Y.Z` tag at
  this HEAD is `v1.7.0`.

## Contract

### New `tools/check_app_versions.py` (added to `check-apps`)

Run after `rebake_shelf.py --check`. Behaviour:
- Determine the newest reachable **release** tag: `git tag --merged HEAD` (or `git describe`-style
  reachability), keep only `^v\d+\.\d+\.\d+$` (suffixed hub-rebuild tags like `v1.2.9-2` are not
  releases — Owner question 2), compare numerically on the triple, take the greatest. If none is
  reachable, print a clear message and **exit non-zero** (do not silently pass).
- Reference set — each shelf app `tools/orb-manager/apps/<slug>.orbapp` (the ones the factory/shelf
  tooling lists: clock, flights, weather, radar, color_picker), versioned by that app's
  `apps/<slug>/version.txt`; plus the hub firmware bundle
  `tools/orb-manager/internal/firmware/bundle/orb-firmware.zip`, versioned by the tree's root
  `version.txt` (Owner question 3).
- For each: compare working-tree bytes to the tag's copy (`git show <tag>:<path> | cmp - <path>`,
  treating a path absent at the tag as "changed"). If bytes **differ** and the current version is
  **not strictly higher** than the version at the tag (compare the tag's `version.txt` / root
  `version.txt`), print one failure line naming the app (or `firmware`) and **both** versions
  (current, tag), and exit non-zero. If bytes match, pass regardless of version.
- Exit 0 with a short "app versions: N checked against <tag>" summary when all pass.

### `Makefile`

`check-apps` gains `$(PYTHON) tools/check_app_versions.py` as its last line.

### `.github/workflows/ci.yml`

The `apps` job's `actions/checkout@v7` gets `with:\n          fetch-depth: 0` so tags are present
and the new check is honest in CI (matches the `firmware` job). No other job changes.

### Applied to this slice

Run `make apps` (or `python3 tools/rebake_shelf.py`) then `make check-apps`. Items 2–4 changed only
hub runtimes (`studio@1`, `theme@1`) and docs, not app editor sources, so no `.orbapp` bytes should
move and each app keeps its version (clock/flights/weather `v1.7.1`, radar/color_picker `v1.7.0`).
If a shelf app's bytes do move and it is not one of the three already at `v1.7.1`, bump its
`apps/<slug>/version.txt` to `v1.7.1` and rebuild before finishing. The check must pass at HEAD.

## Tests / gate

`export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH` then:
- `python3 tools/check_app_versions.py` exits 0 and prints the summary naming `v1.7.0` as reference.
- Forced-failure case (must fail, then recover): make a throwaway byte change to a copy of a shelf
  app whose version equals the tag's (e.g. radar `v1.7.0`) — e.g. `cp` the app, append a byte to the
  real file — run the check, confirm it prints `radar` and both versions and exits non-zero, then
  `git checkout --` to restore and confirm it passes again. Do not leave the change.
- `grep -q 'check_app_versions' Makefile`; `grep -A3 'actions/checkout@v7' .github/workflows/ci.yml`
  shows `fetch-depth: 0` in the `apps` job.
- `make check-apps` (full target, including the new line) passes; `sh docs/specs/s3-gotest.sh` green.
- `git diff --stat HEAD -- tools/orb-manager/contract` empty (this step must not touch the fixture).

