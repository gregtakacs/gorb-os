# pomodoro-editor-4: onto the shelf — the container, the Scope default with its chimes, the guide

## Goal

Ship it. `"shelf": true`, the `.orbapp` built into the hub's shelf apps, the Scope shelf default — ground,
furniture, words, dots, ring colours and its two chimes — baked by `tools/rebake_shelf.py` so it names the
app's release, and a Pomodoro row in the user guide's knob table. After this step `make check-apps` is green
with the pomodoro on the shelf and the shelf's Scope theme chimes. Slice **pomodoro-editor** of
`docs/specs/pomodoro-editor-design.md` §8.

## Read first

- `docs/specs/pomodoro-editor-design.md` §8, and §6 for what `bake_check.py` gates (the shelf theme passes
  through that door, not a softer one).
- `docs/specs/pomodoro-design.md` §13 (shipping: the version, the placeholder `os_min`, the three lines in
  `rebake_shelf.py`, the guide row, why the SDK README does not move).
- `tools/make_factory_apps.py` — `app_dirs()` (:74), `"shelf": true` as the release switch, `--check`'s byte
  comparison, and `--unreleased` (:114, :132).
- `tools/rebake_shelf.py` — `DEFAULTS` (:40), `app_version`, `made_with`, `stale()` (:60), `bake_radar()`
  (:145-160, the shape `bake_pomodoro` copies), `BAKE` (:162) and `main()`'s `--check` message.
- `apps/pomodoro/editor/_selftest/bake_check.py` — the `POMODORO_BAKE_KEEP` contract step 2 established, and
  what it puts in that directory.
- `docs/user-guide.md` 73-104 — the knob table, its column order, and the "Press and turn." paragraph that
  names Radar and Flights today.
- Notes honoured here: `gorb-os-rebake-shelf-on-bump`, `gorb-os-2-bump-app-version-on-push`.

## Contract

- `apps/pomodoro/app.json`: `"shelf": true`, and nothing else. `"editor": "editor"` and `"os_min": "v9.9.9"`
  are step 3's and stay as they are; `version.txt` stays `v1.7.0`.
- `tools/orb-manager/apps/pomodoro.orbapp`: written by `python3 tools/make_factory_apps.py` and committed,
  because `--check` holds the committed bytes to the sources.
- `tools/rebake_shelf.py`, three additions and nothing else:
  `DEFAULTS["pomodoro"] = "pomodoro-scope.orbtheme"`, a `def bake_pomodoro(v)` that runs the app's own
  `bake_check.py` with `POMODORO_BAKE_KEEP=keep`, refuses a `made_with` that is not `v`, and copies
  `scope.orbtheme` into `SHELF`; and `"pomodoro": bake_pomodoro` in `BAKE`.
- `tools/orb-manager/themes/pomodoro-scope.orbtheme`: written by `python3 tools/rebake_shelf.py pomodoro` and
  committed. Its design's `appVersion` reads `v1.7.0`; unpacked, it carries SOUND rows at slots 1, 2 and 3
  and a `pm_values_t` whose `chime` reads `1, 2, 3` — the file on the shelf is the one the device plays, so
  a theme without chimes on the shelf is a shipped bug, and criterion 3 below is the sentence that catches it.
- `docs/user-guide.md`: one row in the knob table after Radar's, and the "Press and turn." paragraph naming
  three apps now. The row, verbatim:

  ```
  | **Pomodoro** | the phase now | the next phase | back to this phase's start, then the phases before it | start, pause, resume — and, when a phase is over, the next one |
  ```

  and this sentence, where the two-app list stands today:

  > Turning in Pomodoro is the clock, not the view: a click is a minute on the phase in hand, and the change
  > stays — nothing about a running timer comes back on its own. Hold the knob down and turn to move between
  > phases: clockwise to the next, anticlockwise back to this one's start and then to the phases before it.
  > The header names the phase you have reached, and it takes effect when you let go.

  Rewrite the containing sentence to keep all three apps true rather than appending a fourth clause to a
  list that has grown past reading.
- `docs/app-platform-plan.md` is not touched: §64 is the owner's ledger and already records the ruling.

## Behaviour and edge cases

- `rebake_shelf.py --check` before the bake says `pomodoro-scope.orbtheme` is missing; after it, all five
  defaults are current. Four apps plus Skeleton is now five.
- A rebuild from unchanged sources answers the same bytes: run `make_factory_apps.py` twice and say so. The
  stamp is a content hash, so a container that moves without a source moving is a mistake.
- The gate is `bake_check.py`'s, reached through `bake_pomodoro`: a bake that fails, or names a release other
  than the app's, leaves the old file where it is and answers non-zero.
- Bump `version.txt` to `v1.7.1` in a scratch edit, watch `--check` refuse, put it back, watch it pass, and
  report both. The file must be left exactly as found (`git diff apps/pomodoro/version.txt` empty).
- No hub Go change is needed or allowed: `seed.go` embeds `apps/*.orbapp themes/*.orbtheme` by glob, so the
  new container and the new default are picked up by the rule that picked radar's.
- `POMODORO_EVIDENCE` is not set for this step's runs: the shelf bake writes themes into a temp directory and
  nothing into the repo.

## Acceptance criteria

1. `grep -q '"shelf": true' apps/pomodoro/app.json && grep -q '"os_min": "v9.9.9"' apps/pomodoro/app.json &&
   test -f tools/orb-manager/apps/pomodoro.orbapp && test -f
   tools/orb-manager/themes/pomodoro-scope.orbtheme` → 0.
2. `ORB_CLANG=/usr/lib/llvm-21/bin/clang python3 tools/make_factory_apps.py --check` → 0, its container list
   naming `pomodoro`.
3. The shelf theme is the app's own release and it chimes:
   `python3 -c "import sys,json;sys.path.insert(0,'tools');import orbbundle as ob;d,w=ob.read_design(open('tools/orb-manager/themes/pomodoro-scope.orbtheme','rb').read());assert w is None and json.loads(d)['appVersion']=='v1.7.0'"`
   → 0, and `python3 -c "import sys;sys.path.insert(0,'tools');import orbbundle as ob;h,secs=ob.read_bundle('tools/orb-manager/themes/pomodoro-scope.orbtheme');els=secs[0]['blob']['elements'];snd=[e['slot'] for e in els if int(e['class'])==10];assert sorted(snd)==[1,2,3] and sorted(int(e['kind']) for e in els if int(e['class'])==10)==[4,4,4];print('sound slots',snd)"`
   → 0, printing `sound slots [1, 2, 3]`.

4. `POMODORO_BAKE_KEEP=/tmp/pmship ORB_CHROME=$(command -v chromium) python3
   apps/pomodoro/editor/_selftest/bake_check.py` → 0, and `/tmp/pmship/scope.orbtheme` byte-identical to the
   committed `tools/orb-manager/themes/pomodoro-scope.orbtheme` (`cmp`) — the shelf's copy is the copy the
   app's own gate just passed, not an earlier one.
5. `python3 tools/rebake_shelf.py --check` → 0, naming five defaults. Then prove the gate bites: set
   `apps/pomodoro/version.txt` to `v1.7.1`, watch `--check` refuse, restore it, and report both outcomes with
   `git diff --stat apps/pomodoro/version.txt` empty at the end.
6. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → 0, with the pomodoro now on the shelf.
7. `make check-editor` → 0.
8. `command -v go` → and then `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH && make check-hub`
   → 0. If there is no Go on PATH (this sandbox has none), say so in the summary and run
   `python3 tools/host_orbapp_test.py | tail -1` and `python3 tools/host_orbapp_install_test.py` instead, so
   the container's shape is still proved by something.
9. `python3 -c "d=open('docs/user-guide.md').read();i=d.index('Press and turn');assert '| **Pomodoro** |' in
   d;assert 'Pomodoro' in d[i:i+1200]"` → 0.
10. `python3 tools/host_pomodoro_test.py --mutations` → 0: nothing on the path to the shelf may weaken the
    core.

## Out of scope

Any look, furniture, recipe or field change (steps 1-3), `main.c`, `pomodoro.c/.h`, `version.txt`'s value,
the real `os_min` (the orchestrator's, when the ring's firmware is cut), `docs/app-platform-plan.md`,
`docs/specs/`, another app's shelf file, the hub's Go, `.github/`, and `apps/sdk/`. No new CI workflow:
`.github/` already runs `make check`.

## Notes

- The container and the shelf default land together on purpose: that pairing is what `check-apps` refuses to
  separate (`gorb-os-rebake-shelf-on-bump` — an Update that changed nothing).
- `rebake_shelf.py` starts a real headless Chromium; expect a minute. Its profile is a temp directory, not a
  repo file. Stop only the pid you started, and never by pattern.
- The shelf theme is the owner's first chime. If Scope's sonar ping is wrong, the fix is step 2's recipe
  table and a re-bake, not an edit of the `.orbtheme`.
- If `made_with` and `version.txt` disagree after a clean run, the suspect is the fixture, not the comparison:
  re-run `python3 apps/sdk/orbsdk.py check-migrations apps/pomodoro --save-current` into a scratch path and
  look at what the packed editor stamps.

