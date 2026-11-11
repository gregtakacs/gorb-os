# pomodoro-4: onto the shelf — the container, the Scope default, the user guide

## Goal

Ship it. `"shelf": true`, the `.orbapp` built into the hub's shelf apps, the Scope shelf default
baked by `tools/rebake_shelf.py` so it names the app's release, and a Pomodoro row in the user
guide's knob table with two sentences about its dial. After this step `make check-apps` is green
with the pomodoro on the shelf. Slice **pomodoro** of `docs/specs/pomodoro-design.md`.

## Read first

- `docs/specs/pomodoro-design.md` §13 (shipping: the version, the placeholder `os_min`, the three
  lines in `rebake_shelf.py`, the user-guide row, why the SDK README does not move) and §12 (the
  gate this step is most of).
- `tools/make_factory_apps.py` — the header, `--check`, `"shelf": true` as the release switch, and
  the unreleased apps' path (:114, :132).
- `tools/rebake_shelf.py` — `DEFAULTS` (:40), `stale()` (:60), `bake_radar()` (:145-160, the shape
  `bake_pomodoro` copies), `BAKE` (:162) and `main()`'s `--check` message.
- `apps/radar/editor/_selftest/bake_check.py` — the `POMODORO_BAKE_KEEP` contract pomodoro-3 built.
- `docs/user-guide.md` 73-104 — the knob table, its column order, and the "Press and turn"
  paragraph that names Radar and Flights today.
- Notes honoured here: `gorb-os-rebake-shelf-on-bump`, `gorb-os-2-bump-app-version-on-push`,
  `gorb-os-flights-needs-os-1-2-8`.

## Contract

- `apps/pomodoro/app.json`: `"shelf": true`. `"os_min"` stays the literal `"v9.9.9"` (design §13).
- `apps/pomodoro/version.txt` stays `v1.7.0`.
- `tools/orb-manager/apps/pomodoro.orbapp`: written by `python3 tools/make_factory_apps.py` and
  committed, because `--check` holds the committed bytes to the sources.
- `tools/rebake_shelf.py`: three additions and nothing else — `DEFAULTS["pomodoro"] =
  "pomodoro-scope.orbtheme"`, a `def bake_pomodoro(v)` that runs the app's own `bake_check.py` with
  `POMODORO_BAKE_KEEP=keep`, refuses a `made_with` that is not `v`, and copies `scope.orbtheme` into
  `SHELF`; and `"pomodoro": bake_pomodoro` in `BAKE`.
- `tools/orb-manager/themes/pomodoro-scope.orbtheme`: written by `python3 tools/rebake_shelf.py
  pomodoro` and committed. Its design's `appVersion` must read `v1.7.0`.
- `docs/user-guide.md`: one row in the knob table after Radar's, and one paragraph. The row, verbatim:

  ```
  | **Pomodoro** | the phase now | the next phase | back to this phase's start, then the phases before it | start, pause, resume — and, when a phase is over, the next one |
  ```

  and this, where "Press and turn." now stands, with Radar and Flights named as they are today:

  > Turning in Pomodoro is the clock, not the view: a click is a minute on the phase in hand, and the
  > change stays — nothing about a running timer comes back on its own. Hold the knob down and turn
  > to move between phases: clockwise to the next, anticlockwise back to this one's start and then to
  > the phases before it. The header names the phase you have reached, and it takes effect when you
  > let go.

  If the "Press and turn." sentence no longer fits (it will name three apps), rewrite it to keep all
  three true rather than append to it.
- `docs/app-platform-plan.md` is not touched: §64 is the owner's ledger and already records the ruling.

## Behaviour and edge cases

- `rebake_shelf.py --check` on a missing `pomodoro-scope.orbtheme` says so (`stale()` returns "is
  missing") and now covers four app defaults plus Skeleton; after the bake it prints all five current.
- A rebuild of unchanged sources answers the same bytes (`make_factory_apps.py --check` is a byte
  comparison and the stamp is a content hash): run it twice and say so.
- The shelf theme's gate is `bake_check.py`'s, reached through `bake_pomodoro`: a bake that fails, or
  that names a release other than the app's, leaves the old file where it is and answers non-zero.
- No hub Go change is needed or allowed: `seed.go:42` embeds `apps/*.orbapp themes/*.orbtheme` by
  glob, so the new container and the new default are picked up by the same rule that picked radar's.
  `make check-hub` is run anyway, to prove it.

## Acceptance criteria

1. `grep -q '\"shelf\": true' apps/pomodoro/app.json && grep -q '\"os_min\": \"v9.9.9\"`
   `apps/pomodoro/app.json && test -f tools/orb-manager/apps/pomodoro.orbapp && test -f`
   `tools/orb-manager/themes/pomodoro-scope.orbtheme`.
2. `ORB_CLANG=/usr/lib/llvm-21/bin/clang python3 tools/make_factory_apps.py --check` → exit 0, its
   container list naming `pomodoro`.
3. `python3 tools/rebake_shelf.py --check` → exit 0, naming `pomodoro-scope.orbtheme` against
   `v1.7.0`. Then prove the gate bites: set `apps/pomodoro/version.txt` to `v1.7.1`, watch `--check`
   fail, and put it back — report both outcomes.
4. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → exit 0.
5. `make check-editor` → exit 0.
6. `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH && make check-hub` → exit 0.
7. `python3 -c "d=open('docs/user-guide.md').read();i=d.index('Press and turn');assert '| **Pomodoro** |' in d;assert 'Pomodoro' in d[i:i+900]"` → exit 0.
8. `python3 tools/host_pomodoro_test.py --mutations` → exit 0 (nothing here may weaken the core).

## Out of scope

Any look or `main.c` or `pomodoro.c` change, `docs/app-platform-plan.md`, `docs/specs/`, the root
`version.txt`, another app's shelf file, the hub's Go, and the real `os_min` (the orchestrator's,
when the ring's firmware is cut). No new CI workflow: `.github/` already runs `make check`, and
`apps/sdk/template/` is the only place an app-level one lives.

## Notes

- The container and the shelf default land together on purpose: that pairing is exactly what
  `check-apps` refuses to separate (the stale-shelf note: an Update that changed nothing).
- `rebake_shelf.py` starts a real headless Chromium; expect a minute, and its profile in a temp
  directory, not in the repo. Kill nothing by pattern — the bake exits when the page is done.
