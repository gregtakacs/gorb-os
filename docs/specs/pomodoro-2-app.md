# pomodoro-2: the app — main.c, app.json, the knob, the ring, the wall clock

## Goal

The app itself: `apps/pomodoro/main.c` drives the face from the state machine in `pomodoro.h`, owns
the four gestures, keeps the countdown in `orb_service` against the wall clock, sets the OS ring
there, sounds the chime there, and persists its state in four hidden settings so it survives a
reboot and an in-place restart. `app.json` declares everything the owner edits. Slice **pomodoro**
of `docs/specs/pomodoro-design.md`; the looks and editor are pomodoro-3, shipping is pomodoro-4.

## Read first

- `docs/specs/pomodoro-design.md` §3 (the machine, already built in pomodoro-1), §4 (the settings
  table, which this step copies into `app.json`), §5 (the hidden four and when they are written),
  §6 (the gestures), §7 (the ring), §8 (off the glass), §9 (the chime), §10 (the rows and the bare
  face), and its Owner questions.
- `docs/specs/ring-design.md` §2 — `orb_ring_t`, `orb_ring_set`, `orb_ring_clear`, `ORB_RING_*`, and
  the amendments. On this branch those symbols exist in `apps/sdk/orb.h`; use them, never a
  re-declaration.
- `apps/radar/main.c` — the app idiom to imitate: `find_rows()`/`say()`/`pose()`/`hide()`
  (:434-448, :975-992), `stage_read` and its 2 s retry (:1030-1045), the visible flag (:356) and
  `redraw()`'s early return (:949), `orb_service` reloading on `orb_cfg_gen` (:1118-1173),
  `orb_input` landing an in-flight choice first (:1184) and `orb_push` with the let-go (:1208-1224),
  `up_ms()`/`now_s()` (:36-37).
- `apps/weather/main.c` — a themed app's labels and settings reads; `apps/README.md` "The knob",
  "Settings", "Off the glass".
- `apps/pomodoro/pomodoro.h` (pomodoro-1) — the machine this step calls. Do not duplicate a rule
  from it in `main.c`.

## Contract

Create `apps/pomodoro/main.c`, `apps/pomodoro/app.json`, `apps/pomodoro/version.txt`
(exactly `v1.7.0`). Modify nothing outside `apps/pomodoro/`.

`app.json` (design §4, §13 — `"shelf"` is `false` here; pomodoro-4 turns it):

```json
{
  "slug": "pomodoro", "title": "Pomodoro",
  "caps": ["ring", "sound"], "period_ms": 1000,
  "settings": [ …the eleven of §4, then the four hidden ones of §5, in that order… ],
  "shelf": false
}
```

No `"editor"` and no `"os_min"` yet: `orbsdk.py:127` refuses an `os_min` on an app that ships no
editor, and the editor arrives in pomodoro-3, which adds `"editor": "editor"` and the placeholder
`"os_min": "v9.9.9"` together. `"shelf": false` keeps the app out of the container check until
pomodoro-4 commits its container.

The fifteen settings are the eleven of §4 in that order, then the four hidden ones of §5, each
`{"type": "int", "show": "none", "label": …, "min": …, "max": …}` with ids and ranges exactly as
§5's table — `orbapp.py` requires a `label` even for a `show: "none"` entry, and refuses an int
without a min, max and default.

Two `help` strings are contract, because the labels are shortened and §64.3's wording has to live
somewhere the owner reads it (the hub shows `help` under the label):

- `autostart`: `"When a phase ends, Pomodoro waits for a press before the next one. On its own starts
  the next phase by itself, and chimes at every change either way."`
- `focuses`: `"How many focuses before the long break: four is the classic pomodoro. One makes a
  focus and a long break, and back again."`

The rest of the `help` lines are one or two sentences in §64.3's own words (25 minutes, the 5-minute
short break, the 15-minute long break, the chime, the ring's opacity over any app, and one colour per
state so the owner can read the state from across the room).

`main.c`:

- `#include "orb.h"`, `"orb_stage.h"`, `"pomodoro.h"`; no `orb_dial.h` (design §6.2).
- Row tags: `PM_T_TIME 8`, `PM_T_PHASE 9`, `PM_T_COUNT 10`, `PM_T_STATE 11`, `PM_T_HEAD 12`,
  `PM_T_DOTS 13`, mirroring radar's `T_*` enum and its `find_rows()`; the dots row is found as an
  art row and posed with `orb_stage_frame`. A theme without a time row is not this app's layout
  (radar's `R_time < 0` test) and takes the bare face.
- `orb_init`: `orb_knob_dial()`, `load_settings()`, `stage_read(false)`, `pm_restore` from the four
  settings with `orb_now`'s sync flag, then the first ring send and the first `orb_store`. Answers
  `ORB_OK`.
- `orb_activate`/`orb_resume`/`orb_show` set the visible flag and redraw; `orb_suspend` clears it and
  frees nothing.
- `orb_input(delta, pressed)`: land an in-flight choice first, then `pressed ? pm_press` and
  `delta ? pm_turn`, then `redraw()` (which returns at once off the glass) and `save_if_moved()`.
  A turn that moved `PM_EV_LIMIT` puts a stop mark on the time row (`+`, `−` suffix is not available:
  the figure is the text, so the mark is the tone — `ORB_TONE_WARNING` for 1.2 s, as radar's flip
  pill times its own end).
- `orb_push(delta)`: `pm_pos_step` on the clicks, the header row names the choice
  (`Focus 3`, `Short break`, `Long break`, plus `…at its start` for `pos == -1`), and `orb_push(0)`
  → `pm_land`. While choosing, the state machine is not advanced: the ring keeps whatever the last
  pass set and `pm_service` is not called for the elapsed case (design §6.3). `PM_CHOICE_LOST_MS`
  60000 in the service is the safety for a let-go that never arrives, radar's own.
- `orb_service`: `if (orb_cfg_gen() != s_cfg_gen) load_and_restore()` (re-read the eleven, and the
  four if the hub moved them — idempotent by design §5.1); `ev = pm_service(&s, &cfg, now_s)`;
  `if (ev & PM_EV_END) chime()`; `ring_send()`; `save_if_moved()`; then, only if visible,
  `redraw()`. It calls no `orb_stage_*`, `orb_cv_*` or `orb_ui_*` unless `s_visible`, and the ring is
  the only OS surface it touches off the glass.
- `orb_tick`: recompute the seconds from `pm_service`'s state, and redraw the time row when the
  second's figure changed.
- `ring_send()`: `pm_ring_for` → `-1` ⇒ `orb_ring_clear()` once (never once a second); else send when
  the state, the phase, the alphas, the opacity, the effect or the fraction moved ≥ `PM_STEP_PCT`
  since the last send, or when the fraction crossed to 0. Keep the last sent record in a static and
  compare it.
- `chime()`: `orb_sound_play(0, 0, 0)` only when `chime` is on, and only from `PM_WAIT`; move the
  state to `PM_WAITED` before returning so a restart cannot sound it twice.
- `redraw()` sends the five labels through `say()` with the tones of design §10.1 and the dots
  row's frame, and the bare-face canvas path (`orb_cv_*`, `ORB_FONT_48`/`ORB_FONT_18`) when
  `s_themed` is false, with the 2 s retry radar uses.
- Time text: `mm:ss`, and `h:mm:ss` is never needed (the cap is 90 minutes).

## Behaviour and edge cases

- First run, nothing stored: `Focus 25:00`, STOP, no ring, no sound, the count row `1 of 4`.
- A press starts it; the same press 3 s later pauses it; a third resumes with the seconds it had
  left (never the setting's 25 again); over and waiting, a press moves one phase on.
- A turn of ±1 click is ±1 minute, and a spin that arrives as `delta = 4` is 4 minutes; both ends
  stop and say so; a turn cannot take a phase below `01:00`.
- Press-and-turn names the choice as it moves, holds the countdown, and lands on the let-go; a
  plain turn or a press mid-choice lands it first. Anticlockwise from a phase already at its start
  goes to the phase before. The long break is the last stop clockwise.
- Paused with the ring on: breathe. Over: flash. Off: `orb_ring_clear`, and `Permissions` shows
  `1 of 2` when the owner revokes `ring` and the ring goes (the OS's doing, `ESP_ERR_NOT_ALLOWED`
  ignored by the app — a refusal must not fault the row, and the app must not retry every pass).
- Off the glass for 30 minutes with `autostart` on: the phases chained, the chime sounded once per
  boundary, and the face on return is the phase it reached. With it off: one `PM_WAITED`.
- A hub change of a colour, of the opacity or of `ring_on` takes effect on the next service pass
  without a press; a change of a length or of `focuses` takes effect at the next phase, and a
  `focuses` that shrank under the current `k` clamps `k`.
- A reboot mid-focus comes back at the same seconds. An in-place restart (installing a new theme,
  §63) likewise, and its first service pass re-sets the ring the app owns.
- A `Waiting` face shows `00:00` and the phase's own colour, in the warning tone.

## Acceptance criteria

1. `test -f apps/pomodoro/main.c -a -f apps/pomodoro/app.json -a -f apps/pomodoro/version.txt` and
   `python3 -c "print(open('apps/pomodoro/version.txt').read().strip())" | grep -qx v1.7.0`.
2. `grep -q 'orb_ring_set' && grep -q 'orb_ring_clear' && grep -q ORB_RING_BREATHE && grep -q
   ORB_RING_FLASH && grep -q 'ORB_EXPORT("orb_push")' && grep -q 'ORB_EXPORT("orb_service")' &&
   grep -q pm_restore` — all against `apps/pomodoro/main.c` (the symbols must really be there; a
   verify that passes silently is a failed step).
3. The manifest is the fifteen, with a help line on everything the owner can see (this is the exact
   command the chain's verify runs, so it must be copied from here and not re-invented):
   `python3 -c 'import json;import sys;s=json.load(open("apps/pomodoro/app.json"))["settings"];sys.exit(0 if len(s)==15 and sum(1 for x in s if x.get("show")=="none")==4 and all(x.get("help") and len(x["help"])>20 for x in s if x.get("show")!="none") and sorted(x["id"] for x in s if x["type"]=="color")==["col_focus","col_long","col_short"] else 1)'`
   → exit 0.
4. `ORB_CLANG=/usr/lib/llvm-21/bin/clang python3 apps/sdk/orbsdk.py build apps/pomodoro` → exit 0,
   and the same with `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → exit 0.
5. **`orb_service` draws nothing, ever** — its own body may not name a display import at all (the
   drawing goes in `redraw()`, which it may call only under the visible flag). The verify runs:
   `python3 -c 'import sys;s=open("apps/pomodoro/main.c").read();b=s[s.index("orb_service"):];j=b.find("ORB_EXPORT",10);b=b[:j] if j>0 else b;bad=[n for n in ("orb_stage_","orb_cv_","orb_ui_","orb_blob_load") if n in b];print("orb_service names "+repr(bad) if bad else "orb_service draws nothing");sys.exit(1 if bad else 0)'`
   → exit 0, printing `orb_service draws nothing`. This is the chain's verify command, copied verbatim. Try it once with a guard deliberately removed and report that it failed, so the check is
   known to bite.
6. `python3 tools/host_pomodoro_test.py` still exits 0 (this step must not need a core change; if it
   does, the core is wrong and the suite is the place the fix goes).

## Out of scope

The editor, any look, the shelf (`"shelf": false` stays), `tools/rebake_shelf.py`,
`tools/orb-manager/`, `docs/user-guide.md`, the `Makefile`, `apps/sdk/`, anything under `main/`. No
`orb_dial.h`, no widgets on the themed path, no `orb_attention`, no `power` cap, no settings screen
of any kind (the OS draws the page), and no second chime sound.

## Notes

- **The state machine is not here.** `main.c` may hold no rule about phases, clamps, the ladder or
  the ring's shape; every such decision is a `pm_*` call, because pomodoro-1's suite is what proves
  them. A rule that appears twice is a rule neither step tests.
- radar's `s_visible` discipline (its `redraw()` returns at the top) is the one line this app must
  not get clever about, and `orb_service` is the only entry allowed to touch the ring.
- The OS's own 500 ms grid and 5‰ step (ring-1 §6) mean the app is never the last word on redraw
  cost; do not add a second, app-side throttle beyond `PM_STEP_PCT`.
- Write `main.c` in radar's voice: a comment where a trap is, no comment where a name says it, and
  statics grouped by what they belong to. Target 450–600 lines, including the bare face.
