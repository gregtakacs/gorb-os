# Pomodoro design: `apps/pomodoro`, the app, its eight looks, and the chain that builds it

Slice **pomodoro** of issue #11, `docs/app-platform-plan.md` §64.3: a new SDK app with a phase
machine on the knob, the OS ring as its progress, a theme face with an editor, and a shelf
default. **Design only**: this file, one spec per step (`pomodoro-1`..`pomodoro-4`) and
`pomodoro-chain.json`. No product code is written by this task.

The base is the ring slice's final branch (the chain's `base` is the literal `RING_TIP`, which
the orchestrator fills in): `orb_ring_t`, `orb_ring_set`, `orb_ring_clear`, `ORB_RING_*`, the cap
`ring` and the `"type": "color"` setting all exist by step 1. `docs/specs/ring-design.md` and its
**Orchestrator amendments** are read here as given, not redesigned: this app is the ring's first
user and uses that API exactly as that design writes it.

## 0. The rulings this stands on

Each is the owner's; the bracket is the note it comes from. Where a note and the code as it
stands disagree, the note wins and this says so where it matters.

- **One dial, four gestures** (`gorb-os-knob-one-dial`, app-design-rules "The knob"): turn is the
  app's one dial, press its one action, press-and-turn its second axis, hold always the OS's. This
  app's dial is a **value dial** (app-design-rules "Two kinds of dial"): the turn's change stays
  and **nothing returns on idle**. §51's sticky home and 20 s idle return are a *view* dial's
  rules and are deliberately not applied to a time the owner just set, so `apps/sdk/orb_dial.h`
  is not used (§6).
- **Nothing off the glass** (`gorb-os-nothing-off-the-glass`): no display work and no card reads
  off the glass. The countdown, the chime and the ring live in `orb_service`, which draws nothing.
- **The ring is the OS's** (§64.1, ring-design): an app never draws it, only says what it should
  be; one ring, the latest caller owns it; the cap `ring` is the consent; breathe slow only.
- **Apps are atomic** (`gorb-os-apps-are-atomic`): the pomodoro ships its own editor inside
  `apps/pomodoro/editor/`, knows nothing of any other app, and shares only the OS/SDK and the kit.
- **Editor UI is kit-level** (`gorb-os-editor-ui-is-kit-level`, theme-editor-kit-plan §8.2): the
  start sheet, calibration, colour picker, look, CSS, library, Render and send are the host's;
  only the fields, parts, views, `draw` and `bake` are the app's.
- **Settings are declared** (`app-settings-on-hub-page`, §57): no settings UI on the glass or in
  the editor; `show: "none"` is for state the owner never edits.
- **One page per app** (§58/§58.5): the dial page is the schema's settings, then Themes and
  Permissions. Nothing here adds a page.
- **Sound levels and the 350 Hz floor** (§59, `gorb-os-2-audio-hardware`): a sound off the glass is
  a one-shot kept above ~350 Hz on a 2 W micro speaker; the OS volume stays on top.
- **Theme text as labels** (`gorb-os-theme-text-labels`): every word is theme text in the theme's
  own fonts, sent as a string with a tone through `orb_stage_text`, never a widget.
- **Two of three at small size** (`orb-small-icons-two-of-three`), judged at **true size on a
  177 dpi monitor** (`user-monitor-177dpi`), never enlarged — hence the PNG evidence in §12.
- **Row cost** (`gorb-os-row-cost`): ~16 B internal and ~500 B PSRAM per row, ceiling 64. The
  face's 12 rows are ~190 B internal and ~6 KB PSRAM and cost no hot path: this glass is nearly
  still (§7).
- **The guest's native stack** (`gorb-os-guest-native-stack`): `orb_service` does its work in four
  small functions, none of them recursive.
- **Shipping** (`gorb-os-rebake-shelf-on-bump`, `gorb-os-2-bump-app-version-on-push`,
  `gorb-os-flights-needs-os-1-2-8`): one bump per push, a re-baked shelf default, and an `os_min`
  that gates installs (§11).
- **§63 in-place restarts**: every theme landing restarts the engine, so a running timer is its
  end time on the wall clock in the app's storage, never a count in RAM.

## 1. What the slice adds, mapped to §64.3

| §64.3 | this slice | step |
|---|---|---|
| "focus, short break, focus, … for N focuses (default 4), then a long break, then again" | the cycle in `pomodoro.c`: `pm_phase_at`, `pm_focus_at`, `pm_next_k` | 1 |
| "focus length (default 25 min), short break (5), long break (15), focuses before the long break (4)" | `focus_len`, `break_len`, `long_len`, `focuses` | 2 |
| "start the next phase on its own or wait for a press" | `autostart`, default wait | 2 |
| "a chime at each change (cap `sound`)" | `chime`, the theme's sound slot 0, played from `orb_service` | 2, 3 |
| "the ring on or off, its opacity" | `ring_on`, `ring_op` (`int` 0..100, unit `%`, §64.2) | 2 |
| "a colour for each state: focus, short break, long break" | `col_focus`, `col_short`, `col_long`, `"type": "color"` (§64.2) | 2 |
| "press starts, pauses and resumes (and, with a phase over and waiting, starts the next)" | `pm_press` | 1, 2 |
| "turn … a minute a click (a fast spin, more); it is a value dial" | `pm_turn`; the OS's own acceleration supplies the "more" | 1, 2 |
| "press-and-turn moves between phases … lands on the let-go" | `pm_pos_step`, `pm_land`, `orb_push` | 1, 2 |
| "**Hold** is the OS's" | no hold, no `power` cap; the app never touches it | — |
| "the ring shows the phase draining over any app … paused breathes, over and waiting flashes, off clears" | `pm_ring_for`, set from `orb_service` | 1, 2 |
| "it runs off the glass: the countdown … against the wall clock, the ring and the chime come from there" | §8 | 2 |
| "the phase and its end time survive a reboot or an in-place restart (§63)" | four hidden settings + `pm_restore` | 1, 2 |
| "its face is a theme … an editor that starts from the eight looks (64.4) and a shelf default (Scope)" | §10, §11 | 3, 4 |

No firmware change and no hub Go change in this slice (§13): the app needs only what the ring
slice adds plus what already ships. §55 alerts are out of scope — the ring *is* the notice.

## 2. Phases and the cycle

One cycle holds `N = focuses` (1..12, default 4) focuses and is a flat sequence of `2N` slots,
indexed `k = 0 .. 2N-1`:

```
k:      0     1     2     3    …    2N-2   2N-1
     focus  short  focus  short  …   focus   long
```

`phase(k)` is the focus for even `k`, the short break for an odd `k` below `2N-1`, and the long
break at `k = 2N-1`. `focus_no(k)` is `k/2 + 1` for a focus and 0 for a break. With `N = 1` the
cycle is focus then long break, which is what one-focus-before-a-long-break means. "Then again" is
`pm_next_k`: after `2N-1` it returns to `k = 0`, and the count of completed cycles is not kept —
the dots row shows `k`'s focus number within the cycle, which is the only figure the face shows.

A phase's *length* is its setting (`focus_len`, `break_len`, `long_len`) in seconds, except that
the owner's turn may have made *this* phase longer or shorter: the turned value is the phase's
live length (`pm_len`) and never writes the setting, so the next focus of the cycle starts from
25 minutes again. That is what "turn changes the current phase's remaining time" says, and it is
why the turned number does not need an "undo".

## 3. The state machine

### 3.1 States, events, transitions and the ring

Five run states. `PM_WAIT` and `PM_WAITED` are one state to the owner ("a phase is over and
waiting") and two to the machine, so a restart cannot chime the same boundary twice.

| # | state | means | the ring (§7) |
|---|---|---|---|
| 0 | `PM_STOP` | ready: this phase, at its full length, not started | cleared |
| 1 | `PM_RUN` | counting down against the wall clock | the phase's colour, steady, fraction = remaining/length |
| 2 | `PM_PAUSE` | held, its remaining kept | the phase's colour, breathe, the fraction frozen |
| 3 | `PM_WAIT` | the phase ended, its chime not yet sounded | the phase's colour, flash, fraction 0 |
| 4 | `PM_WAITED` | over and waiting; it has already chimed (the chime is off, or a restart found it ended) | the phase's colour, flash, fraction 0 |

| event | from | to | what happens | ring |
|---|---|---|---|---|
| press | STOP | RUN | starts at `len`, `end_s = now + len` | clear → steady |
| press | RUN | PAUSE | freezes: `rem = end_s - now`, `end_s = 0` | steady → breathe |
| press | PAUSE | RUN | resumes: `end_s = now + rem` | breathe → steady |
| press | WAIT / WAITED | RUN at `pm_next_k(k)` | the next phase starts at its setting's length; the long break's press closes the cycle and opens the next at `k = 0` | the colour may change, steady |
| turn ±d | STOP | STOP | `rem = len = clamp(rem + 60d, 60, cap)` | stays clear |
| turn ±d | RUN | RUN | `rem = clamp(rem + 60d, 60, cap)`, `end_s = now + rem`, `len = (len - rem₀) + rem` (the elapsed arc is never rewound) | steady, fraction follows |
| turn ±d | PAUSE | PAUSE | `rem = len` clamped as above | breathe, fraction follows |
| turn ±d | WAIT / WAITED | PAUSE | the finished phase is rewound to the turned minute and held: the waiting ends and a press runs it | flash → breathe |
| press-and-turn ±d | any | (unchanged) | the *choice* moves and shows in the header; the countdown holds where it was, nothing sounds | unchanged |
| let-go `orb_push(0)` | any | STOP at the chosen `k` | the choice lands: that phase at its setting's length, not started | follows the new phase |
| a press or a plain turn while choosing | any | as its own row above | the choice lands first (radar's rule), and the gesture is that let-go rather than a second action | follows |
| service, `now ≥ end_s` | RUN | `autostart` ? RUN at `pm_next_k(k)` : WAIT | one boundary per pass, `PM_EV_END` returned so `main.c` sounds the chime; a phase missed long ago is not made up | steady → flash, or steady on the new phase |
| settings changed (`orb_cfg_gen`) | any | unchanged | colours, opacity, off, chime and autostart apply at once; the lengths and `focuses` apply at the next phase, and a `focuses` that shrank below the current `k` clamps `k` into the new cycle | re-set when anything it names moved |
| restart / reboot | — | restored | `pm_restore` (§5.2); a running phase whose end passed comes back `PM_WAITED` | follows the restored state |
| hold | any | (the OS's) | the app list; the machine does not move and a `PM_RUN` keeps counting off the glass | unchanged |

Nothing wraps: `pm_next_k` wraps only the *cycle* (that is what "then again" is), the turn stops at
its cap, and the press-and-turn ladder stops at both ends of the cycle (§6.3).

### 3.2 The pure surface (`apps/pomodoro/pomodoro.h`)

The machine is host-testable C with **no SDK import at all** (`radar.h`/`radar.c` are the model):
no `orb.h`, no `orb_stage.h`, nothing platform-flavoured, so `tools/host_pomodoro_test.py` compiles
`pomodoro.c` as it stands with gcc. Every time arrives as an argument.

```c
#define PM_STOP 0
#define PM_RUN  1
#define PM_PAUSE 2
#define PM_WAIT   3          /* ended, chime owed                        */
#define PM_WAITED 4          /* ended, already chimed (or the chime is off) */
#define PM_FOCUS 0
#define PM_SHORT 1
#define PM_LONG  2
#define PM_EV_NONE  0
#define PM_EV_END   1        /* a phase ended: sound the chime, flash    */
#define PM_EV_START 2        /* a phase started on its own (autostart)   */
#define PM_EV_LIMIT 4        /* the last turn hit an end and stopped      */

typedef struct {
    int32_t len_s[3];        /* the three length settings, seconds        */
    int32_t cap_s[3];        /* each one's max, seconds: the turn's ceiling */
    int32_t focuses;         /* N, 1..12                                 */
    int32_t autostart;       /* 0 wait for a press, 1 chain on            */
    int32_t ring_on;         /* 0 => the ring is cleared, not invisible   */
    int32_t opacity;         /* ring_op, 0..100                            */
    uint32_t col[3];         /* 0xRRGGBB per phase, from the color settings */
} pm_cfg_t;

typedef struct {
    int16_t st;              /* PM_STOP .. PM_WAITED                     */
    int16_t k;               /* the cycle slot, 0 .. 2N-1                */
    int32_t len;             /* this phase's length, seconds             */
    int32_t rem;             /* remaining, seconds (its meaning follows st) */
    int64_t end_s;           /* the wall second a PM_RUN phase ends      */
} pm_t;

typedef struct { int32_t st_k, len, end, rem; } pm_raw_t;   /* the four hidden settings */

/* the cycle */
int  pm_cycle_len(int focuses);                                   /* 2N */
int  pm_phase_at(int k, int focuses);                             /* PM_FOCUS|PM_SHORT|PM_LONG */
int  pm_focus_at(int k, int focuses);                             /* 1..N, 0 on a break */
int  pm_next_k(int k, int focuses);                               /* wraps at the cycle's end */
int  pm_len_s(const pm_cfg_t *c, int phase);

/* the gestures */
void pm_press(pm_t *s, const pm_cfg_t *c);
int  pm_turn(pm_t *s, const pm_cfg_t *c, int delta, int64_t now_s);      /* PM_EV_* bits */
int  pm_at_start(const pm_t *s);
int  pm_pos_step(int pos, int delta, int at_start, int k, int focuses);  /* the ladder, ends stop */
void pm_land(pm_t *s, const pm_cfg_t *c, int pos);

/* the service and the store */
int  pm_service(pm_t *s, const pm_cfg_t *c, int64_t now_s);             /* PM_EV_* bits */
void pm_restore(pm_t *s, const pm_cfg_t *c, const pm_raw_t *raw, int synced, int64_t now_s);
void pm_store(const pm_t *s, pm_raw_t *raw);
int  pm_changed(const pm_raw_t *a, const pm_raw_t *b);                  /* 0 writes nothing */

/* what the glass and the ring should show */
int  pm_ring_alpha(int base_alpha, int opacity);                            /* base*op/100, down */
int  pm_ring_for(const pm_t *s, const pm_cfg_t *c, pm_ring_t *out);         /* 1 set, 0 clear    */
int  pm_fraction(const pm_t *s);                                            /* 0..1000 */
int  pm_ring_effect(const pm_t *s);                                     /* ORB_RING_*, or -1 = clear */
int  pm_ring_phase(const pm_t *s);                                      /* whose colour */
void pm_state_text(const pm_t *s, char *buf, int cap);                  /* "" | "Paused" | "Waiting" */
void pm_phase_text(const pm_t *s, char *buf, int cap);                  /* "Focus" | "Short break" | "Long break" */
void pm_count_text(const pm_t *s, int focuses, char *buf, int cap);     /* "3 of 4" | "break" */
```

`pm_restore`/`pm_store` are the only place the packing (`st_k = k * 8 + st`) is written, which keeps
that trick in one file and one test, and `pm_changed` is what stops a settings re-read from writing
four NVS keys that already hold those values.

## 4. The settings (§57 schema, `apps/pomodoro/app.json`)

Eleven the owner edits, four the app keeps to itself (§5): **15 of the 16 the loader is sized for**
(`orbapp.py` `SETTINGS_MAX = 16`). Ids are ≤ 15 characters and labels ≤ 23 bytes, both enforced by
`orbapp.py`; every int carries `step` and `unit` so the dial's stepper says what it means.

| id | type | label | range / choices | default | show | why the range |
|---|---|---|---|---|---|---|
| `focus_len` | int | Focus length | 5..90, step 5, unit " min" | 25 | both | 25 is the pomodoro. 5 is the floor: below that it is a kitchen timer and the chimes become a tick loop. 90 is a long work block, and it is the ceiling every `focus_len` turn stops against. |
| `break_len` | int | Short break | 1..30, step 1, unit " min" | 5 | both | 5, the classic. 1 because a break of nothing is no break; 30 because a break twice the shortest focus is not a break. |
| `long_len` | int | Long break | 5..60, step 5, unit " min" | 15 | both | 15, the classic. 5 so the reward is a real break; 60 because an hour off is not a pomodoro cycle any more, and it bounds the stored `pm_len`. |
| `focuses` | int | Focuses per long break | 1..12, step 1 | 4 | both | 4, the classic. 1 lets a cycle be focus-then-long-break. 12 bounds the ladder (24 slots either way), the dots row (13 frames) and `pm_st`'s packing (`23*8+4 = 188 < 200`). |
| `autostart` | toggle | Start next on its own | "wait for a press", "on its own" | 0 | both | §64.3 says the default is wait: a hold is the way out, so a timer must not run ahead of someone who walked off. |
| `chime` | toggle | Chime at each change | "off", "on" | 1 | both | The cap `sound` consent; off leaves the ring's flash as the only notice (§9). |
| `ring_on` | toggle | Ring | "off", "on" | 1 | both | §64.3 "the ring on or off". Off means `orb_ring_clear`, not an invisible record (§7). |
| `ring_op` | int | Ring opacity | 0..100, step 5, unit "%" | 85 | both | 0..100 is §64.2's word for an opacity. Step 5 is 21 stops, enough for a knob. 85 so it reads over any app without hiding what is under it. |
| `col_focus` | color | Focus colour | the ring-3 palette | `0xD62828` (Ruby) | both | §64.3 "a `color` for each state". Ruby is the tomato, and it is on the dial's palette so the stepper names it. |
| `col_short` | color | Short break colour | the ring-3 palette | `0x0FA36B` (Emerald) | both | Emerald: go. On the palette. |
| `col_long` | color | Long break colour | the ring-3 palette | `0x2E93E4` (Sky) | both | Sky, away from both, and away from the amber the OS's own copy arc and carousel use. |

Three defaults from `docs/specs/ring-design.md` §9.1's named palette on purpose: the dial shows a
**name** for each, not "custom", and an owner who prefers another colour gets it from the hub's
picker (ring-3's amendment 5: on the dial a colour turns through the palette like an enum, in the
page's own press-to-edit flow — no press-and-turn on a settings page).

Two labels are shorter than §64.3's sentence because `orbapp.py` caps a label at 23 bytes: "Focuses
before the long break" is **Focuses per long break**, and "start the next phase on its own or wait
for a press" is **Start next on its own** with the two choices carrying the rest ("wait for a
press" / "on its own"). Every `help` line repeats §64.3's wording in full for the hub.

`caps: ["ring", "sound"]`: the two the app uses. No `net`, no `location`, no `power` (see Owner questions), no
`themes` (wearing its own theme is the free `CAP_ART`; `themes` is enumerating and installing
others). `period_ms: 1000` (the face's seconds), `memory_kb` left at the 64 default.

## 5. The hidden state settings, and what survives a restart

Four `int` settings with `"show": "none"` — state the owner never edits is still *declared*,
because the store is the OS's and there is no other place an app may write:

| id | type | range | default | holds |
|---|---|---|---|---|
| `pm_st` | int | 0..200 | 0 | `k * 8 + state`: the cycle slot and the run state in one key. 0 doubles as "no state yet", so a first install is a fresh STOP at `k = 0` with no special case |
| `pm_len` | int | 0..5400 | 0 | this phase's length, seconds (the ring's denominator; 5400 = the 90-minute focus ceiling) |
| `pm_end` | int | 0..2147483647 | 0 | the wall second a running phase ends; 0 unless the state is `PM_RUN` |
| `pm_rem` | int | 0..5400 | 0 | seconds remaining as of this write: the paused/stopped value, and the estimate a restart reads when the clock has not synced |

### 5.1 When they are written

**Only on a state change**, never on a beat: a press, a turn, a choice landing, a phase boundary, an
autostart, a restore, and a settings re-read that moved one of them. `pm_changed` compares the
quartet first and `imp_cfg_set` (`main/app/orb_app_wasm.c:617`) already no-ops a value that has not
moved, so a pass that changed nothing costs no flash. A running phase writes **nothing at all** for
its whole 25 minutes: that is why an end second is stored rather than a countdown. `orb_cfg_set`
goes through the jobs worker, off the tick.

Two consequences a worker must not be surprised by, both read from the firmware this session: the
app's **own write moves `orb_cfg_gen()`** (`s->cfg_local++`), so the service's settings reload must
be idempotent — and it is, because the four hidden keys *are* the state, so reloading re-reads what
the app just stored; and a value outside its declared min/max is refused `ESP_ERR_INVALID_ARG`, so
the ranges above are load-bearing, not decoration.

### 5.2 `pm_restore`, the four cases

| stored | the clock | restored |
|---|---|---|
| no key at all, or `pm_st = 0` | any | STOP at `k = 0`, `len = rem = focus_len`: a first run has no state to find |
| STOP or PAUSE | any | the same, at `pm_rem` (clamped into 60..cap, with `len ≥ rem`) |
| RUN | synced (`orb_now` answers 1), `rem = pm_end - now` | `rem > 0` ⇒ RUN with the seconds left; `rem ≤ 0` ⇒ **PM_WAITED**: it ended while the orb was away, and one boundary is one boundary — the phases it would have led are not invented (see Owner questions) |
| RUN | not yet synced (a cold boot with no network: `orb_now` answers 0 and its second is a guess) | **PAUSE at `pm_rem`**: the elapsed time cannot be known, so the face says Paused at the minute it last stored and waits for a press rather than chiming at a 1970 guess |
| WAIT or WAITED | any | WAITED, always silent: that boundary already had its chime (§3.1) |

An `int32` end second runs out in 2038. Every other app on this platform stores a wall time in the
same frozen C9 `int` (§57.2), a phase's end is at most 90 minutes from now, and the value is
rewritten at every boundary, so the limit is the platform's and is noted rather than designed
around.

## 6. The knob

### 6.1 Press — the one action

`orb_input(delta, pressed)`: with a choice in flight the choice lands first (radar's
`apps/radar/main.c:1184`), and then `pressed` is `pm_press` — start, pause, resume, or, over and
waiting, the next phase. One action, always the same one, wherever the dial stands. No double
press, no mode, no arming: `orb_knob_dial()` once from `orb_init`.

### 6.2 Turn — a value dial

`pm_turn(s, cfg, delta, now_s)` moves the remaining time **60 s a click**. The acceleration is not
the app's to invent: the OS's input layer already delivers `|delta|` of 2 or 4 on a quick spin
(`main/knob/knob_accel.h`, plan §51.1), so "a fast spin, more" is what the same call already does.
The ends stop at 60 s and at the phase's setting max (`cap_s[phase]`); `PM_EV_LIMIT` says the last
click hit one, which is what puts the minute figure on the glass with a stop mark rather than
leaving the owner wondering whether the knob was broken. Being a value dial it has **no idle return
and no home**: nothing about a running timer may be undone by walking away (app-design-rules "Two
kinds of dial"), which is the one place this design declines `orb_dial.h` and §51's third rule,
deliberately and by the rule book's own leave.

### 6.3 Press-and-turn — the phases

`orb_push(delta)`, exported, which is the whole opt-in (README "Press-and-turn"). Clockwise is the
**next** phase, anticlockwise **back**, exactly as §64.3 says, and `delta > 0` is clockwise with
nothing negating it (app-design-rules "Which way is which"). Clicks arrive one by one and
unaccelerated. The choice is an integer `pos` off where the phase is now:

| pos | names |
|---|---|
| 0 | this phase, as it stands (the sticky middle: the let-go here changes nothing) |
| -1 | this phase **back at its start** (§64.3's anticlockwise first step) |
| -2 … | earlier phases of the cycle, one a click, stopping at `k = 0` |
| +1 … | later phases of the cycle, one a click, stopping at `k = 2N-1` (the long break) |

`pm_pos_step(pos, delta, at_start, k, focuses)` is the ladder; the ends stop, so the travel is
bounded by one cycle each way and no wrap is possible. While chosen, the face's header row names the
phase (`pm_phase_text` of the slot it points at, `Focus 3` for a focus) and the countdown **holds**,
exactly as radar holds its loop while a zoom is being chosen (`apps/radar/main.c:415`). The choice
**lands on the let-go**: `orb_push(0)` → `pm_land` → STOP at the chosen phase at its setting's
length. A press or a plain turn while choosing lands it too, and is then that let-go and nothing
else. Radar's `ZOOM_LOST_MS` safety becomes `PM_CHOICE_LOST_MS` 60000: an OS that never sends the
let-go must not strand the app in choosing.

It is a second axis, never the only way to the app's point: press still starts, pauses, resumes and
advances over and waiting, so an owner who never finds it has a whole pomodoro.

### 6.4 Hold

The OS's, always (§51). The app exports no hold handling and declares no `power` cap.

## 7. The ring, from the service

`pm_ring_for(s, cfg, pm_ring_t *out)` answers 1 to set and 0 to clear, so the ring's shape is one
pure function the host suite can pin (requirement 8's "what the ring should be for a state").
`pm_ring_t` is pomodoro.h's own plain record — `fraction, effect, arc, track, period_ms` — and
`main.c` copies its five fields into the `orb_ring_t` it hands `orb_ring_set`, exactly as ring-1's
`orb_ring_rec` mirrors `orb.h`'s layout rather than importing it.

| state | effect | fraction | arc | track | period |
|---|---|---|---|---|---|
| RUN | `ORB_RING_STEADY` | `rem * 1000 / len`, clamped 0..1000 | the phase's colour at `0xE6` scaled | the same colour at `0x33` scaled | — |
| PAUSE | `ORB_RING_BREATHE` | frozen | as RUN | as RUN | 4000 (ring-1's `ORB_RING_BREATHE_MIN_MS`) |
| WAIT / WAITED | `ORB_RING_FLASH` | 0: the phase is gone, the rim is all track and the flash is the message | as RUN | as RUN | 2000 (twice `ORB_RING_FLASH_MIN_MS`: one second on, one off) |
| STOP, `ring_on` off, `ring_op` 0 | — | — | — | — | `orb_ring_clear()` |

- **Draining, clockwise from twelve**: the arc shrinks as the phase goes, which is the fraction
  convention ring-design §2 gives and the copy arc already draws.
- **The track is the same colour at `0x33` (20%), chosen here** — §64.3 asks for one choice. The
  arc's base is `0xE6`: the state is read from across the room, and an arc at 230/255 over the glass
  is near the copy arc's own rim, which that file draws as an opaque `0x66DF` over a `0x3186` track
  (ring-design §4 read those two as the tile's byte order).
- **`ring_op` scales both**: `alpha = base * opacity / 100`, integer, rounded down
  (`pm_ring_alpha(base, op)`), so 85 gives `0xC3` and `0x2B`. At 0 both are 0 and ring-design §4
  draws nothing — the app still clears rather than owning an invisible record, so `/status` names no
  owner when the owner asked for no ring.
- **Paused and waiting reuse the phase's colour** with their effect, as §64.3 requires: no fourth
  colour, no grey.
- **Only when it matters**: `orb_service` sends a record when the state, the phase, a colour, the
  opacity or the on/off moved, or when the fraction moved **≥ 4 thousandths** since the last send.
  One second of a 25-minute phase is 0.67 thousandths, so a running phase re-sets the ring about
  every 6 s — inside ring-1's own 500 ms grid and its 5‰ step, so the OS's clamp is never the thing
  that saves it. A phase rollover always sends.
- The app never draws the ring, and never sets it from `orb_tick` or `orb_input`: one path, from
  `orb_service`, so the look is identical on the glass and three apps away over a spreadsheet.

## 8. Off the glass

- The countdown is kept in `orb_service` against **`orb_now`'s wall second**, not a tick count:
  `orb_uptime_ms`/`orb_uptime_us` reset at boot and the state has to outlive one, so an *absolute*
  second is the only thing worth storing (§5). The uptime clock is used for what it is right for:
  the choice timeout and the beat, as radar's `up_ms()` uses it. `orb_now` answering 0 "while it is
  still a guess" (`apps/sdk/orb.h:47`) is the whole reason §5.2 has an unsynced case.
- The chime and the ring come from there too (§7, §9). `orb_service` calls no `orb_stage_*`,
  `orb_cv_*` or `orb_ui_*` at all, and reads no card: nothing to fetch, nothing to load.
- A visible flag as README "Off the glass" asks: set in `orb_activate`/`orb_resume`/`orb_show`,
  cleared in `orb_suspend`; `redraw()` returns at once when it is clear, and a dirty flag carries
  the face's changes to `orb_resume`.
- `orb_tick` (1 s, on the glass only) re-reads the remaining seconds from the state and repaints the
  rows that moved. Off the glass the beat stops; the service does not.
- Coming back to the app after 20 minutes finds the ring's fraction already right: the state is
  derived from the clock each pass, never accumulated.

## 9. The chime

- The sound is the theme's **slot 0** (`orb_stage.h`: "a theme's sounds are numbered slots whose
  meaning is the app's own"). `apps/pomodoro/editor/pomodoro-bake.js` synthesizes the built-in: one
  strike, ~0.6 s, 16-bit mono 22050 Hz, energy at **660 Hz and 990 Hz with nothing below 350 Hz**
  (`gorb-os-2-audio-hardware`: a 2 W micro speaker), encoded by the baker's own `soundEncode`
  (ADPCM, §59.1's budget rule).
- It sounds in `orb_service`, once, on the pass that found the boundary, and only from `PM_WAIT`:
  the state is written to `PM_WAITED` before that pass returns, so a second pass, a reboot or an
  in-place restart over a theme landing cannot double it (§63).
- One-shot, never a loop (app-design-rules "On the glass and off it"), and
  `orb_sound_play(0, 0, 0)` answers negative during quiet hours — taken as "do not tell me by
  sound", with no further action. The ring is still flashing.
- The start, the pause and a landed choice are **silent**: §64.3 asks for a chime "at each change",
  and the change is a phase boundary. A beep per press would be the tick loop §59's owner just
  moved away from.
- No `chime_vol` of its own: the clock's two levels exist because it plays a *loop*; one strike at
  the OS volume needs no ratio (§59's reason, Owner question 2).

## 10. The face

### 10.1 What is on it, and the rows that carry it

Everything is a theme row: a plate for the ground and the furniture, three fonts, five labels and
one art row, plus the preview the kit bakes for the carousel: **12 rows of the 64 a theme may
carry** (`gorb-os-row-cost`) — about 190 B of internal RAM and 6 KB of PSRAM, and no hot path,
because this glass redraws once a second at most and only on the glass.

| slot | kind | tag | what it is | the app sends |
|---|---|---|---|---|
| plate 0 | plate | — | the ground: the look's gradient, stars, paper or enamel | (baked) |
| plate 1 | plate | — | the glass: the furniture drawn under the words — the chapter ring, the dial's 12 and 3, a title block, whatever the look is | (baked) |
| font 0 | text | — | the big figure: digits, colon, space, `%` spare — 96 px, editable 64..144 | — |
| font 1 | text | — | the body face: the phase name, the header | — |
| font 2 | text | — | the quiet face: the count and the state | — |
| AUX 8 | label | 8 | **the remaining time**, `25:00`, centred, in font 0 | `mm:ss`, tone normal; `00:00` and tone warning over and waiting |
| AUX 9 | label | 9 | **the phase name**: `Focus`, `Short break`, `Long break` | `pm_phase_text`, tone normal on a focus, accent on a break |
| AUX 10 | label | 10 | **the focus count**: `3 of 4`, `break` on a break | `pm_count_text`, tone quiet |
| AUX 13 | aux art | 13 | **the dots**: `n of N` as filled and hollow dots, one bank of 13 frames (0..12 filled), app-picked with `orb_stage_frame` | frame = the focus number |
| AUX 11 | label | 11 | **the state**: `Paused`, `Waiting`, nothing while running | `pm_state_text`, quiet / warning |
| AUX 12 | label | 12 | **the header**: the press-and-turn choice while it moves | `Focus 3`, `Long break`, … tone accent, hidden the rest of the time |

Word for word with `gorb-os-theme-text-labels`: the app says *what* the text is and its tone; the
theme owns the face, size, colour, panel and border. No widget, ever, on the themed path — the
labels are how the tracker got back to L4 and the same veto applies here.

`mm:ss` is the figure, not `m min`: the same 5 characters at any length, so the row never reflows
and the owner reads the seconds going. `00:00` is what a finished phase shows while it waits.

**Legibility** (`user-monitor-177dpi`, radar's own 26 px floor for a look's capitals): the big
figure at 96 px is ~9 mm tall on the 1.75 in panel (266 ppi), a figure read at arm's length and across a
desk; the body at 30 px and the quiet at 24 px both clear the platform's 18 px floor. The dots are
10 px each with 6 px gaps (N = 12 spans 186 px, inside the glass), so the count carries **shape and
colour** and not detail, per `orb-small-icons-two-of-three`. Every look is judged on a true-size PNG
(§12), never enlarged.

### 10.2 Does the face draw its own arc? No — and why

The face is **still furniture plus words**; the only live progress indicator on it is the numbers.
Reasons, in the order the rule book gives them:

1. §64.1 makes the ring the OS's and draws it *over whatever is on the glass* — **including this
   app's own face**. An arc in the theme would be a second arc on the same rim, one of them wrong
   the moment `ring_op` is low or the owner is off the app.
2. `app-design-rules` "The ring": "don't use it to decorate your own face (draw that in your
   theme)" reads the other way for a progress arc: the ring is for an *ongoing state* and this app's
   ongoing state is exactly that, so the state is the ring's and the face's is the number.
3. Nothing off the glass (§`gorb-os-nothing-off-the-glass`): a themed arc would have to be a live
   row the app re-composites every few seconds, which is the row cost and the PSRAM the pomodoro is
   supposed to *not* spend while the owner is in Weather. The OS ring costs the app nothing at all
   off the glass.
4. It is still the look's own dial. A theme may bake a **static** chapter ring, ticks, a bezel or a
   track at its own alpha (`plate 1`), and a look may bake its own dots row; the OS ring then becomes
   the one moving thing on it, which is the sharpest version of the same design. Scope's chapter
   ring with the pomodoro's own steady arc over it is the shelf default for that reason.
5. The `orb_stage_*` surface has no import that would draw a live arc in theme colours at a runtime
   fraction (`orb_stage_opacity` is a whole row, `orb_stage_frame` picks a baked frame), so the only
   way to a face arc is the app's own `orb_cv_*` pixels — which is explicitly the ring's job.
   A theme that wants 1200 frames of a draining ring could bake them as a frames-picked row, and
   that costs the pool 1200 plates for something the rim already does; it stays out.

### 10.3 With no theme (the bare face)

`orb_stage_open` failing, or a theme that is not this app's layout (radar's `R_time < 0` test),
falls back to a bare face on a canvas: `orb_cv_create` at 466×466, `orb_cv_fill` black, the
remaining time once in `ORB_FONT_48 | ORB_CV_CENTER` and the phase, the count and the state on one
line in `ORB_FONT_18`, and the choice in place of that line while press-and-turn moves. Radar's note
that a widget takes the dial off its fast path is about its live layers; the pomodoro has none, so
the cost here is only that the bare face is ugly — and the ring, the chime, the knob and the state
all still work. Radar's 2 s retry (`stage_read(true)` from the service) is copied: a theme may bind
late,      and the bare form is never taken for granted.

## 11. The eight looks, and the editor that starts from them

`docs/app-design-rules.md` "Looks" and §64.4: every themed app starts from the same eight looks, one
name each, in this order, plus New (blank), and **nothing is borrowed between looks**. The pomodoro
starts from all eight. Its keys are the look's own name — `scope`, `paper`, `brass`, `blueprint`,
`station`, `celestial`, `silver`, `sectional`, `blank` — so this app does not inherit radar's
`vintage` key for Brass; §64.4's "the stale old names go" is those apps' own step to take, and the
pomodoro starts clean rather than copying the drift.

Each look's ground, faces and inks below are **read off the same-named look** in
`apps/radar/editor/radar-bake.js` (the palette table at lines 39–219) and the matching weather and
flights presets, so parity is by value and not by name. What is the pomodoro's own is the
*furniture*: a kitchen timer, a printed card, an enamel dial, a title block, a platform clock, an
astrolabe, a machined bezel, a chart log box.

| # | look | ground (top → foot) | faces | ink / quiet / accent | the pomodoro's furniture |
|---|---|---|---|---|---|
| 1 | **Scope** (the shelf default) | `#0a1f14` → `#020805` | IBM Plex Mono, 26 px body, 96 px figure | `#b4ffd2` / `#5fbf8a` / `#e0b040` | a phosphor chapter ring with 12 ticks and a 60-minute scale; the dots as small squares; no sweep (a timer that counts down needs no second thing turning) |
| 2 | **Paper** | `#f2ead8` → `#e3d7bd` | Playfair Display for the figure, IBM Plex Sans for the body | `#2a2218` / `#5a4a36` / `#a04a18` | a printed card: a hairline rule above and below, the phase in small caps, the count as `3 of 4`, no ring at all |
| 3 | **Brass** | `#2e332e` → `#151815` | Cormorant Garamond | `#f2e6c8` / `#a8946a` / `#ff9a3c` | dark enamel under a brass bezel, a red five-minute hand's shadow, the chapter in Roman numerals at 12/3/6/9 only |
| 4 | **Blueprint** | `#174a82` → `#0c2c58` | IBM Plex Mono | `#eaf4ff` / `#a8cdf0` / `#ffe07a` | a title block bottom-left, dashed chapter ticks, a leader line to the figure, everything 1 px thin |
| 5 | **Station** | `#ffffff` → `#e6e6e3` | Jost | `#111111` / `#555555` / `#e3000f` | the railway clock: a black rim, a red centre, the largest figure of the eight, the count in the arrow's place |
| 6 | **Celestial** | `#251a55` → `#0b0820` (with stars) | Space Grotesk | `#edebff` / `#9c95c9` / `#ffd27a` | an astrolabe's arc and a gilded hour scale; the dots as small stars, gold when filled |
| 7 | **Silver** | `#3a3f46` → `#1c1f24` | Jost | `#f2f4f7` / `#aab2bd` / `#7fc8ff` | a machined bezel with a knurled edge, engraved chapter marks (a light tint, no coloured rim, per the small-icon ruling) |
| 8 | **Sectional** | `#eef1e4` → `#dfe5cf` | Barlow Condensed | `#1f2a44` / `#3e5a80` / `#c2187a` | a chart's log box: magenta window, blue rules, the count as a flight-strip block |
| + | **New** (blank) | `#1c1c1c` → `#101010` | the system default | `#e8e8e8` / `#a8a8a8` / `#c8c8c8` | nothing: every row present, no furniture, so an author starts from a face and not from a hole |

Notes on parity: radar's Scope has a sweep and a bottom track; the pomodoro's Scope keeps the colour
family and the 26 px body but takes its own layout, because the very feature that would make a
pomodoro's Scope "radar's" (a sweeping arm) is the thing §10.2 refuses. The grounds, faces and inks
are the shared part — "each look's palette, fonts and grounds belong to the same-named look" — and
the furniture is the app's own.

**The editor** ships inside the app (`apps/pomodoro/editor`, `"editor": "editor"`) and is a hosted
editor (`editor.json`: `{"baker": 3, "editor": 1, "host": 1, "module": ["pomodoro-bake.js",
"pomodoro-editor.js"], "runtime": ["baker@1", "fonts@1", "host@1", "studio@1", "theme@1", "ui@1",
"react@18.3.1"], "schema": 1}`), registering with `OrbHost.register(app)` exactly as
`apps/radar/editor/radar-editor.js:226-260` does: `slug`, `title`, `schema`, `check`, `complete`,
`blank`, `templates` (the eight, in order, each with its `STARTER_NOTES` line), `picture`, `steps`,
`parts`, `fields`, `views`, `draw`, `bake`, `fit` (`{rows: 12, cap: 64}`). No start sheet, no
colour picker, no calibration and no send of its own: those are the kit's
(`host@1/host.js:601-620`, `studio@1` `S.StartFlow`), per `gorb-os-editor-ui-is-kit-level`.
The kit has no sound pane, so the chime is the look's built-in and not author-replaceable in this
release (see Owner questions).

Its five steps and parts: **Ground** (the gradient and any stars), **Dial** (the furniture: the
chapter marks, the bezel, the title block), **Words** (three faces and their sizes and inks),
**Count** (the dots row: filled and hollow, their size and colour), **Ring** (the *static* track and
chapter alpha only — the moving ring is the OS's and the editor says so in a note). Its `views`
mirror what the glass can show, so the author sees the states the app can be in: `phase` (focus,
short, long), `state` (running 25:00, paused, waiting, choosing), `count` (1 of 4, 3 of 4, 4 of 4).
Declaring `schema: 1` brings `orbsdk.py check-migrations`'s requirement with it, so
`apps/pomodoro/editor/_fixtures/schema-1.orbdesign` holds one sample design of schema 1, saved with
`orbsdk.py check-migrations --save-current` (ruling 36: it travels in the editor region).

The bake is `apps/pomodoro/editor/_selftest/` copying `apps/radar/editor/_selftest/` shape:
`bake.html` (staged beside `baker@1/orbtheme-baker.js` and the vendor fonts, bakes every starter
plus a couple of switched variants, prints `B64 <name> <ms> <base64>` into `#out`) and
`bake_check.py` (headless Chromium through `tools/orbchrome.py`, then for each theme: reads back
with `orbbundle`/`theme_schema`, the slug is `pomodoro`, plates `[0,1]`, fonts `[0,1,2]`, labels of
shape text at 8..12, the dots row at 13 an art row with 13 frames, a preview present, the design
region carried, the size inside the pool's budget, and the shipped projector
(`host_clock3_test.run(exe, "--dump3f", path, "pomodoro")`, as radar does) accepting it), honouring
`POMODORO_BAKE_KEEP=DIR` so `tools/rebake_shelf.py` can take its Scope.

## 12. The gate, as concrete checks

The slice is done when every one holds. Workers never touch an orb, a serial port or a device: the
last two checks are the orchestrator's on Gold.

1. `python3 tools/host_pomodoro_test.py` exits 0 (the machine: transitions, the turn's clamp, the
   ladder, restore, the ring for a state, the packing) and
   `python3 tools/host_pomodoro_test.py --mutations` **exits non-zero if any mutant escapes**,
    printing `mutants caught N/N` with the two numbers equal (ring-design amendment 3: the gate is
    the exit status, never a grepped word).
2. `gcc -std=c11 -Wall -Wextra -Werror -I apps/pomodoro -c apps/pomodoro/pomodoro.c -o /dev/null`
   exits 0 — proof the core needs no SDK and no ESP-IDF.
3. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` exits 0: the SDK binds match, every app
   builds including `apps/pomodoro`, its committed container matches its sources, and
   `rebake_shelf.py --check` no longer finds `pomodoro-scope.orbtheme` missing or trailing
   `version.txt`.
4. `make check-editor` exits 0 (the runtime suites still green with a new registered app), and
   `python3 apps/sdk/orbsdk.py check-migrations apps/pomodoro` opens `_fixtures/schema-1.orbdesign`
   in the packed editor.
5. `python3 apps/pomodoro/editor/_selftest/bake_check.py` exits 0: nine bakes (eight looks and
   blank), each passing the gates listed in §11.
6. `grep` gates that the verify must not pass silently: `orb_ring_set`, `orb_ring_clear`,
   `ORB_RING_BREATHE`, `ORB_RING_FLASH`, `orb_push`, `orb_service`, `"show": "none"` and
   `"type": "color"` are all present in the new files, and `apps/pomodoro/version.txt` is exactly
   `v1.7.0`.
7. **Art judged by eye**: every look and New, baked and rendered at true size (466 px at 177 dpi,
   `user-monitor-177dpi`) to a PNG in `$DELEGATE_EVIDENCE_DIR/looks/<name>.png`, one per look, plus
   the five states of Scope (running, paused, waiting, choosing, bare) in the same directory. The
   orchestrator opens them; a step that produces no PNGs has not finished its art step.
8. Orchestrator on Gold: the ring drains over Weather in the focus colour, breathes on a pause and
   flashes over and waiting; the chime sounds once at a boundary with the app three apps away; a
   reboot mid-focus comes back with the same seconds left; an in-place restart (wearing a new theme)
   does the same; `Permissions` shows the `ring` cap and revoking it answers the refusal and drops
   the ring; `/status` names `pomodoro` as the ring's owner and `/fb` cannot see the ring (§64.1);
   turning while a phase is over ends the flash.
9. `version.txt` and `os_min` are the orchestrator's to release; §13.

## 13. Shipping

- `apps/pomodoro/version.txt` is **`v1.7.0`**, the release line the tree is on (the root
  `version.txt` is `v1.7.0`, radar `v1.7.0`, clock `v1.7.1`, flights `v1.7.2`). A brand-new app
  starts at the third-digit zero of the current line; the second only moves at a real release
  (app-design-rules "Shipping").
- `app.json`: `"shelf": true`, so `tools/make_factory_apps.py` builds it into
  `tools/orb-manager/apps/pomodoro.orbapp` and the hub compiles it in. `--check` then holds that
  container to its sources, so the container is committed with the code that made it.
- **`os_min` is a placeholder the orchestrator replaces, not a value a step invents.** pomodoro-3
  adds it with the editor (`orbsdk.py:127` refuses an `os_min` on an app that ships none), as the
  literal `"os_min": "v9.9.9"`, which `orbsdk.py:124`'s grammar accepts, which the hub
  reads as "refuse this install on firmware below v9.9.9" and therefore refuses **everywhere** —
  the safe failure while the ring's firmware is unreleased (the flights lesson: an app installed on
  firmware that cannot bind its imports is a failed row with no reason). Deliberately implausible
  so nobody ships it by accident. When the orchestrator cuts the release that carries ring-design's
  ring, the placeholder becomes that version, the app's `version.txt` moves once for the push, and
  `python3 tools/rebake_shelf.py` re-bakes the shelf default so its `appVersion` names it
  (`gorb-os-rebake-shelf-on-bump`).
- `tools/rebake_shelf.py` gains three lines: `DEFAULTS["pomodoro"] = "pomodoro-scope.orbtheme"`, a
  `bake_pomodoro(v)` that runs the app's own `bake_check.py` with `POMODORO_BAKE_KEEP` and keeps
  `scope.orbtheme` (the shape of `bake_radar`, lines 145–160), and `BAKE["pomodoro"]`. The shelf
  file lands at `tools/orb-manager/themes/pomodoro-scope.orbtheme`, named like the other three so
  `stale()`'s message and the one-name-per-look rule both hold.
- `make check-apps` runs `rebake_shelf.py --check`, so a shelf default that trails the app fails the
  build; `make rebake` is the fix and needs Chromium (present on this box: `/usr/bin/chromium`).
- **The user guide** (`docs/user-guide.md`) gains one row in the knob table (`:89`) and one short
  paragraph, both written out verbatim in the pomodoro-4 spec: the row reads
  `| **Pomodoro** | the phase now | the next phase | back to this phase's start, then the phases before it | start, pause, resume — and, when a phase is over, the next one |`,
  plus two sentences saying the turn is minutes on the phase in hand and that the phases are
  press-and-turn, landing when the knob is let go. The "Press and turn" sentence that today names
  Radar and Flights gains Pomodoro, whose gesture changes *what happens next* rather than the view.
- **The SDK README needs no change** (requirement 10): the app uses `orb_ring_*`, `orb_push` and
  `show: "none"`, all already documented by `apps/README.md` and `docs/app-design-rules.md`; the
  ring's own SDK entry is ring-2's, not this slice's.
- `Makefile` needs no change: `check-apps` finds every `apps/*/app.json`, `check-host` globs
  `tools/host_*_test.py`, and `check-migrations` takes the app directories from a wildcard.
- **No firmware change and no hub Go change.** Everything the app needs beyond what ships is
  ring-design's own (the `ring` cap in `orb_svc.h` and the hub's `appCapOwnerAll`, the `color` type
  in `orbapp.py`/`orb_setui.c`/`tabs.js`). If the OS turns out to be missing something this app
  needs, it is an owner question with the smallest OS change that would do (see Owner questions), never a design
  around a ruling.

## 14. The files, per step

| step | creates | modifies |
|---|---|---|
| **pomodoro-1** the pure core | `apps/pomodoro/pomodoro.h`, `apps/pomodoro/pomodoro.c`, `tools/host_pomodoro_test.py` | — |
| **pomodoro-2** the app | `apps/pomodoro/app.json`, `apps/pomodoro/version.txt`, `apps/pomodoro/main.c` | — |
| **pomodoro-3** the editor and the eight looks | `apps/pomodoro/editor/editor.json`, `editor/pomodoro-bake.js`, `editor/pomodoro-editor.js`, `editor/_fixtures/schema-1.orbdesign`, `editor/_selftest/bake.html`, `editor/_selftest/bake_check.py` | `apps/pomodoro/app.json` (`"editor": "editor"`, `"os_min": "v9.9.9"`) |
| **pomodoro-4** shipping | `tools/orb-manager/apps/pomodoro.orbapp`, `tools/orb-manager/themes/pomodoro-scope.orbtheme` | `apps/pomodoro/app.json` (`"shelf": true`), `tools/rebake_shelf.py`, `docs/user-guide.md` |

`"shelf"` stays `false` until step 4 so steps 1–3 can pass `make check-apps` without a container
committed against sources still moving; step 4 flips it, packs, and commits the container with the
shelf theme.

## 15. The chain

`docs/specs/pomodoro-chain.json`: `{"name":"pomodoro","repo":"../..","base":"RING_TIP",
"retries":2,"steps":[…four…]}` — `base` left as the literal `RING_TIP` for the orchestrator to fill
with the ring slice's tip, since `orb_ring_t` and the `color` type do not exist until it lands.
`hard: true` on steps 1, 2 and 3 (real algorithmic and visual content), plain on step 4. Every
verify: `set -o pipefail` wherever it pipes, the new symbols grepped so a verify cannot pass
silently, `--mutations` gated on exit status, `make check-apps` with
`ORB_CLANG=/usr/lib/llvm-21/bin/clang`, `make check-editor`, and step 3's own `bake_check.py` with
its PNGs.

## Owner questions

Each has a chosen option (marked), so the chain never stops on one.

1. **Does a phase missed entirely get made up?** A focus that ended 3 hours before the orb was seen
   again: run the short break, or hold? — **Chosen: hold one boundary.** The restore comes back
   `PM_WAITED` at `00:00` and a press moves one phase on. Chaining three missed phases would charge
   the owner for time they did not work, and a pomodoro that lies about that is worse than one that
   waits.
2. **`chime_vol`, the pomodoro's own level (§59's spirit)?** — **Chosen: not in this slice.** One
   strike at the OS volume needs no ratio, the clock's two levels exist for its loop, and the
   settings quota is at 15 of 16. If the owner wants it, it is one int and one `stage_snd_play_v`
   call (both already exist), and there is exactly one slot left in the table.
3. **The chime with no theme (the bare face)?** — **Chosen: silent.** A theme sound is the only
   sound an SDK app has (`orb_sound_play` plays "one of this app's own theme sounds"), and the hub
   installs the shelf Scope with the app, so the bare face means a broken card. The countdown, the
   ring and the flash all still work; the face's line says `Waiting`.
4. **A per-app chime that must wake a dimmed glass (`power` cap)?** — **Chosen: no.** The OS's quiet
   hours already answer the hour the owner asked not to be heard (§9), and the ring's flash is the
   notice that works without waking anything. `power` would be a second consent for a nicety.
5. **May the face echo the phase's colour (a Ruby figure during a focus)?** — **Chosen: no, tones
   carry it.** `orb_stage_text` sends a *tone* and the theme decides what that tone's colour is
   (§10.1); there is no import that overrides a label's colour at runtime, and inventing one for
   this would put a theme's ink under an app's settings. If the owner wants colour-over-app, the
   smallest OS change is a `stage_tint(row, rgb)` import that multiplies a text row's ink.
6. **A kitchen-timer second app (one phase, no cycle)?** — **Chosen: out of scope.** §64.3 names one
   app with a cycle. `focuses = 1` gets closest today.
7. **Press-and-turn when the app has no `Focus 3`-style header (New, blank)?** — **Chosen: the state
   row says it** (radar's own fallback: `else if (zooming && R_zoom < 0) st = ZOOM[zs]`), so a theme
   with no header row still shows the choice, on the row the app knows is there.

## 17. Not in this slice

Any change outside `docs/specs/` in *this* task; the ring and the `color` setting (ring-design);
§55 alerts; firmware and hub changes; a `chime_vol`; a kitchen-timer mode; sound files an author can
replace (the kit has no sound pane); an app-to-app share of looks, art or editors (atomic); touching
another app's `PRESETS`, `STARTERS` or shelf file (§64.4's gaps are weather's and flights' own
step, not this one).
