# alerts-3: the pomodoro borrows the press

## Goal

Let the pomodoro use the new backed alerts and the lent press. Add its `press_any` setting, declare
the `press` capability, claim while allowed by state, consume lent presses in `orb_service`, and bump
the app version.

## Read first

- `docs/specs/alerts-design.md` §4.2, §6.
- `docs/specs/alerts-1-backed-ring.md`, `docs/specs/alerts-2-lent-press.md`.
- `apps/pomodoro/app.json`, `main.c`, `pomodoro.h`, `pomodoro.c`, `version.txt`.
- `tools/host_pomodoro_test.py`.

## Contract

1. **Manifest** `apps/pomodoro/app.json` caps include `"press"`; add setting `press_any` as an enum
   after existing settings with choices `["Off", "When it's calling", "Whenever it runs"]`, default 1,
   and help text in the pomodoro's voice. Keep labels under 24 bytes and the total at 17 settings.
2. **Config** `pm_cfg_t` gains `int press_any` parsed from the setting index used by the loader.
3. **Pure rule** add `int pm_press_want(const pm_cfg_t *c, const pm_t *s);` in `pomodoro.c`:
   `PM_STOP` returns 0; Off returns 0; Calling returns 1 only for wait/waited; Whenever returns 1 for
   wait/waited/run/pause.
4. **Service** `orb_service()` reads `pm_press_want` and calls `orb_press_claim()` accordingly; it
   polls `orb_press_lent()` and consumes each press by applying the same state transition a physical
   press on its own glass would apply. `orb_input()` unchanged.
5. **Tests** extend `tools/host_pomodoro_test.py` for `pm_press_want`, manifest packing at 17,
   service claim/release/lent consumption, version bump, and mutants for wrong call-window and missed
   release.
6. **Version** bump `apps/pomodoro/version.txt` from `v1.7.0` to `v1.7.1`.

## Behaviour and edge cases

- Foreground press behavior remains the current pomodoro behavior.
- A background pomodoro with `When it's calling` claims only while called/waiting.
- With `Whenever it runs`, a running drain also accepts lent presses but its ring remains unbacked.
- With `Off`, service claims 0 and any prior claim is dropped.
- Multiple queued presses advance multiple pomodoro state transitions if a physical press would.
- If the setting cap from step 2 is not present, manifest packing fails; do not remove settings.

## Acceptance criteria

1. `python3 tools/host_pomodoro_test.py --mutations > /tmp/alerts3p.log 2>&1; grep -qE 'mutants caught ([0-9]+)/\1' /tmp/alerts3p.log && ! grep -q MISSED /tmp/alerts3p.log`
2. `ORB_CLANG=/usr/lib/llvm-21/bin/clang python3 apps/sdk/orbsdk.py build apps/pomodoro`
3. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps`
4. `grep -q '"press"' apps/pomodoro/app.json && grep -q '"press_any"' apps/pomodoro/app.json`
5. `grep -q 'v1.7.1' apps/pomodoro/version.txt`
6. `make check-sandbox`
7. `git diff --name-only` touches only `apps/pomodoro/*`, `tools/host_pomodoro_test.py`, and
   `docs/specs/*`.

## Out of scope

Do not touch OS files under `main/`, SDK files under `apps/sdk/`, hub files, or ring implementation.
Do not redesign the pomodoro face, chimes, editor or visual style.

## Notes

This step is deliberately small after step 2 has moved the shared surface. Its value is proving the
pomodoro can actually use the new OS feature without foreground navigation.
## Orchestrator amendments

See alerts-design.md § Orchestrator amendments: no version bump (criterion 6 and acceptance 5 withdrawn), the base includes the editor slice, and a wake-only press is never lent.
