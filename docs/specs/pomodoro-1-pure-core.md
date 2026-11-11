# pomodoro-1: the phase machine, pure, with a host suite that bites

## Goal

The brain of the pomodoro (§64.3): a cycle of N focuses with short breaks and a long break, the
knob's three moves over it, the wall-clock service pass, the restart, and what the ring should be —
in C with **no SDK import at all**, compiled on host gcc and mutated by a suite of the repo's own
style. Nothing on the glass yet: the app is pomodoro-2, the looks pomodoro-3, shipping pomodoro-4.
Slice **pomodoro** of `docs/specs/pomodoro-design.md`.

## Read first

- `docs/specs/pomodoro-design.md` §2 (the cycle), §3 (the machine and the pure surface, which this
  step writes word for word), §5 (`pm_restore`'s cases), §6 (the turn's clamp and the ladder), §7
  (the ring for a state), §9 (why the chime is the caller's job).
- `apps/radar/radar.h` / `apps/radar/radar.c` — the pure-logic split out of an app for a host suite:
  the header's opening comment, `static inline` ladders, no platform types.
- `tools/host_radar_test.py` — the suite's exact shape: compile the shipped C with
  `gcc -std=c11 -O1 -Wall -Wextra -Werror`, `MUTATIONS` as (find, replace) pairs read from the
  source, the `--mutations` loop, `%d/%d mutants caught`, exit 1 on any escape.
- `docs/specs/ring-design.md` §2 and §6 — the record and the clamps this mirrors; and its
  **Orchestrator amendments** (mutation verifies gate on exit status).
- `docs/app-design-rules.md` "Two kinds of dial", "Which way is which", "The ring".

## Contract

Create `apps/pomodoro/pomodoro.h`, `apps/pomodoro/pomodoro.c`, `tools/host_pomodoro_test.py`.
`pomodoro.c` includes only `"pomodoro.h"`, which includes only `<stdint.h>` and `<stddef.h>` — no
`orb.h`, no `orb_stage.h`, no `<math.h>` (no trig anywhere: the fraction and the ladder are
integer). Every time-dependent entry takes its time as an argument.

The public surface is design §3.2 **exactly**: the `PM_*` constants, `pm_cfg_t`, `pm_t`, `pm_raw_t`,
`pm_ring_t`, and `pm_cycle_len`, `pm_phase_at`, `pm_focus_at`, `pm_next_k`, `pm_len_s`, `pm_press`,
`pm_turn`, `pm_at_start`, `pm_pos_step`, `pm_land`, `pm_service`, `pm_restore`, `pm_store`,
`pm_changed`, `pm_ring_alpha`, `pm_ring_for`, `pm_fraction`, `pm_ring_effect`, `pm_ring_phase`,
`pm_state_text`, `pm_phase_text`, `pm_count_text`.

Constants this step fixes (design §4, §7):

```c
#define PM_MIN_S 60              /* a turn never goes below a minute                    */
#define PM_ARC_A 0xE6            /* the arc's base alpha (230)                          */
#define PM_TRACK_A 0x33          /* the track: the same colour at 20%                   */
#define PM_BREATHE_MS 4000       /* ring-1's floor: breathe is slow only                */
#define PM_FLASH_MS 2000         /* twice a second: one second on, one off              */
#define PM_STEP_PCT 4            /* the least fraction move worth a send (thousandths)  */
#define PM_PACK_MUL 8            /* pm_st = k * PM_PACK_MUL + state                     */
```

`pm_ring_t` is `{ int fraction; int effect; uint32_t arc; uint32_t track; int period_ms; }`, the
five fields of `orb_ring_t` in the same order and meaning (`arc`/`track` carry their alpha in bits
31..24). The core must not name the SDK's type: pomodoro-2 copies the five fields into an
`orb_ring_t`, the way ring-1's `orb_ring_rec` mirrors `orb.h` rather than importing it.
`pm_ring_effect` returns `0`/`1`/`2` (the `ORB_RING_*` values, written out as the numbers they are
with a comment naming each constant) or `-1` for "clear": `PM_STOP` and a `ring_on` of 0 and an
opacity of 0 are all `-1` (design §7).

`pm_cfg_t` holds minutes-to-seconds already converted by the caller, plus `cap_s[]` (each setting's
own max in seconds) — the machine never reads the manifest, so a test can hand it a focus setting of
25 with a cap of 90 and check the turn stops at 90.

## Behaviour and edge cases

The suite pins, at minimum (each one a check, and each rule at least one mutant):

- **The cycle.** `pm_cycle_len(4) == 8`; `pm_phase_at` over a whole cycle for N = 1, 2, 4 and 12 is
  focus/short/focus/…/focus/long, with the long break **only** at `k = 2N-1` (N = 1 ⇒ focus, long);
  `pm_focus_at` is `k/2+1` on a focus and 0 on either break; `pm_next_k(2N-1) == 0`,
  `pm_next_k(k) == k+1` otherwise.
- **Press.** STOP→RUN with `rem == len == 25*60`; RUN→PAUSE with `rem` kept and `end_s = 0`;
  PAUSE→RUN with `end_s` re-anchored (the caller passes now); WAIT and WAITED → RUN at
  `pm_next_k(k)` with the **new** phase's setting length (a press over a short break starts a 25-min
  focus, not the 6 that just ran); four presses through a whole cycle land on the long break, and
  the press over it returns to `k = 0`.
- **Turn.** one click is 60 s; `+delta` and `-delta` both; the floor is `PM_MIN_S` (never 0: a
  turned-to-zero timer would look like a finished phase); the ceiling is `cap_s[phase]` and not the
  setting's current value, so 25 → 30 works while the setting stays 25; `PM_EV_LIMIT` when a click
  asked for more than the end allowed; in RUN the **elapsed arc is kept** (`len` grows by the same
  60 s the `rem` did, so `pm_fraction` never goes back up); in WAIT the state becomes PAUSE; in STOP
  `len` follows `rem` so the ring's denominator is never 0.
- **The ladder.** from mid-phase, `pos -1` is this phase at its start and `pos -2` the phase before;
  from a phase already at its start, the first anticlockwise click is `-2` (design §6.3's reading);
  clockwise from anywhere steps the cycle forward; both ends stop and repeat clicks there return the
  same `pos` (never a wrap, never a further move); `pm_land(0)` changes nothing at all.
- **Service.** nothing before the end (`PM_EV_NONE`, `rem` falls with the seconds); the pass at or
  after `end_s` returns `PM_EV_END` and leaves WAIT with `rem = 0, end_s = 0`; `autostart` leaves RUN
  at the next `k` with `PM_EV_END|PM_EV_START` and the chime owed by the caller for **that** boundary
  only; a second pass in WAIT returns `PM_EV_NONE` (no repeat chime); a boundary 3 hours late returns
  one `PM_EV_END`, no more (design, Owner question 1).
- **Store/restore.** `pm_store` then `pm_restore` round-trips every state; `pm_restore` with an
  all-zero `pm_raw_t` gives STOP at `k = 0` at the focus setting's length; a stored RUN with `synced`
  and `end` 10 s ahead gives RUN at ~10 s; with 40 s past gives **PM_WAITED** (silent: no `PM_EV_END`
  out of a restore); `synced == 0` gives **PM_PAUSE at `pm_rem`**; a stored `pm_st` of 199 or a
  negative `pm_rem` is clamped, not trusted; `pm_changed` is 0 for an identical quartet and 1 if any
  of the four moves.
- **Ring.** RUN steady, fraction `rem*1000/len` (25:00 of 25:00 ⇒ 1000, 12:30 ⇒ 500, and 0 s of a
  0-length phase must not divide); PAUSE breathe at period ≥ 4000; WAIT/WAITED flash at 2000 with
  fraction 0; STOP, `ring_on` off and opacity 0 all answer `-1`; opacity 85 scales `0xE6`→`0xC3` and
  `0x33`→`0x2B`, opacity 100 leaves both bases, and the track is never brighter than the arc;
  `PM_STEP_PCT` is what the caller compares, so expose it as a constant and check 3‰ is under it.
- **Texts.** `pm_state_text` is `""`, `"Paused"`, `"Waiting"`; `pm_phase_text` is `Focus`,
  `Short break`, `Long break`; `pm_count_text` is `"3 of 4"` on the third focus and `"break"` on
  either break, and always fits a 24-byte buffer at N = 12.

## Acceptance criteria

All must pass on this sandbox's gcc; no ESP-IDF, no device.

1. `python3 tools/host_pomodoro_test.py` → exit 0, last line `all pomodoro checks pass`.
2. `python3 tools/host_pomodoro_test.py --mutations` → exit 0, no `MISSED` line, and the line
   `mutants caught N/N` with the two numbers equal and N ≥ 14; **the command's exit status is the
   gate**, and the suite returns 1 when any mutant escapes (ring-design amendment 3).
3. `gcc -std=c11 -Wall -Wextra -Werror -I apps/pomodoro -c apps/pomodoro/pomodoro.c -o /dev/null`
   → exit 0 (proves the core stands alone).
4. `test -f apps/pomodoro/pomodoro.h -a -f apps/pomodoro/pomodoro.c -a -f tools/host_pomodoro_test.py`
   and `grep -q pm_pos_step apps/pomodoro/pomodoro.h && grep -q pm_restore apps/pomodoro/pomodoro.c`
   → the new files and symbols are really there.
5. `! grep -qE '#include +"orb' apps/pomodoro/pomodoro.c` → the core imports no SDK header
   (requirement 8).

## Out of scope

`main.c`, `app.json`, `version.txt`, the editor, the looks, the shelf, `docs/user-guide.md`, any
change under `main/`, `tools/orb-manager/` or `apps/sdk/`, and any other app. No `orb_ring_*` call
(that is pomodoro-2), no sound, no theme, no settings JSON. Do not add a `Makefile` rule:
`check-host` globs `tools/host_*_test.py`.

## Notes

- The whole point of the split is that a device-side invariant — a chime that fires once, an arc that
  never rewinds, a restore that cannot double a boundary — gets a cheap test the CI can break
  (the host suites are the cheap signal). If a rule cannot be expressed as a call and an assertion,
  it does not belong in this file.
- Keep `pomodoro.c` under ~300 lines and the suite under ~350, like `radar.c` (294) and
  `host_radar_test.py` (343).
- Every mutant must be a *behavioural* edit (a `>=` for a `>`, a dropped clamp, a wrong period), not
  a reformat. A mutant that no check notices means the check is not watching the rule it names.

`pm_ring_for(s, c, out)` fills `out->arc` and `out->track` with the phase colour OR'd with
`pm_ring_alpha(PM_ARC_A, c->opacity)` and `pm_ring_alpha(PM_TRACK_A, c->opacity)`, so the colour's
low 24 bits come from the settings and the top byte from the opacity — the composition ring-design
§2 tells an app to build. Add `opacity` and the three colours to `pm_cfg_t` for that
(`int32_t opacity; uint32_t col[3];`).
