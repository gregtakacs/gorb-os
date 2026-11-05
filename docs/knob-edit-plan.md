# Knob editing in the shell, and quiet hours the owner sets

Status: **asked by the owner 2026-11-04 (§0); K1–K4 built the same day (§3). The orb side
is benched on Bronze; the hub's section is not yet seen in a browser.**

## 0. Owner asks, 2026-11-04

1. **Quiet hours are the owner's to set, on the hub and on the orb**, to any hours, not
   only the four nights the Sound page's ring offers.
2. **On the orb, a value is edited with the knob**: press a row to enter, turn to adjust
   with dynamic speed and rounding as the colour picker's HSB channels do, press to save.
3. **The same for the volume and the other settings already in play** (the display's dim
   timeout).

The DST report that started this was not a DST fault: Gold played sounds because a test
loaded a theme that carries them, where the baked default skeleton has none.

## 1. Design

### 1.1 The edit mode (`main/menu/menu_view.c`, `main/menu/knob_edit.c`)

- A row is **editable** when its page says so: it answers a range (min, max, wrap, the
  largest fast step), its current value, a formatter, and a commit.
- **Press** on an editable row enters the edit: the row shows the value it will set, the
  hint says "rotate: adjust   push: save   hold: cancel".
- **Turn**: each detent moves the value by a step chosen from the turn's speed, the colour
  picker's model (an EMA of detents a second, 1 when slow, up to the row's largest step
  when fast, reset after 220 ms still). A step above 1 lands on the next multiple of
  itself in the turn's direction (37, fast up: 40, 45), so a quick spin rests on round
  numbers. The value clamps at its ends, or wraps where the row says so (an hour).
- **Press** saves through the row's commit; **hold** cancels and leaves the value as it
  was. Leaving the page or the menu cancels.
- The step maths is pure C (`knob_edit.c`) with a host suite.

### 1.2 The rows

| Page | Row | Range | Fast step |
|---|---|---|---|
| Sound | Volume | 0–100, "off" at 0 | 5 |
| Sound | Quiet hours | press toggles on/off; on restores the last hours | — |
| Sound | Quiet from | 0–23 wrap, "22:00" | 3 |
| Sound | Quiet until | 0–23 wrap, "07:00" | 3 |
| Display | timeout | 1–120 min | 5 |

The display's preset list (`PG_TO`) goes: its ruling (presets, not a spinner) is
superseded by ask 3.

### 1.3 Quiet hours remembered

`orb_sound` keeps the last hours set (NVS `qlf`/`qlt`) so turning quiet hours back on
restores them. `/api/v1/sound` is unchanged on the wire (-1 is off); its GET adds
`quiet_last_from`/`quiet_last_to`.

### 1.4 The hub

The Orb tab gains a **Sound** section through the allowlisted `/api/v1/sound`: volume,
quiet hours on/off, from and until.

## 2. Slices

| Slice | Content | Gate |
|---|---|---|
| K1 | `knob_edit.c` and its host suite | mutants caught |
| K2 | The edit mode in `menu_view.c`; the Sound and Display rows | `host_menu_test.py` green; Bronze walk |
| K3 | Remembered quiet hours in `orb_sound` and the API | host suites; GET on Bronze |
| K4 | The hub's Sound section | `make check-hub`; set from the hub, read on the orb |

## 3. As built (2026-11-04)

- **K1.** `main/menu/knob_edit.c`: the colour picker's speed model with a row's own largest
  step, the snap to that step's multiples, the clamp and the wrap.
  `tools/host_knob_edit_test.py`: 24 checks, 7 of 7 mutants (two more were equivalent, the
  interpolation's own clamp answering for them, and were dropped).
- **K2.** The edit mode in `menu_view.c` (§1.1), a page's `edit` hook, and three appended
  app hooks (`opt_edit`, `opt_fmt`, `opt_set`; NULL keeps a page press-only, so every wasm
  app's rows are as they were). The Sound page has Volume, Quiet hours (on/off), Quiet from
  and Quiet until; the display's timeout edits in minutes. `PG_TO` is retired as
  `PG_TO_RETIRED`, kept in the enum so a stored menu position's page numbers still mean
  what they meant, and never restored. `host_menu_test.py`: the picker's checks rewritten
  for the edit mode, 11 new mutants, 87 caught in all.
- **K3.** `orb_sound` keeps the last hours (NVS `qlf`/`qlt`); `/api/v1/sound` GET adds
  `quiet_on`, `quiet_last_from`, `quiet_last_to`; POST takes `quiet_on`, which restores the
  last hours unless hours ride beside it.
- **K4.** The hub's Orb tab has a Sound card: volume, quiet hours on/off, from and until
  (disabled while off, showing the hours they return with), Save. `tabs.js` parses and
  `make check-hub` is green; the hub would not run beside the owner's (one hub at a time),
  so the card itself is unseen.
- **Benched on Bronze**, driven through `/api/v1/screen?turn=&push=&hold=`:
  - the Sound page with its five rows;
  - an edit of Quiet from: two turns took 22:00 through the wrap to 00:00, a hold
    cancelled with 22:00 kept, and turn and save set 23:00;
  - Quiet hours off and on again restored 23:00–07:00;
  - the timeout edit read 10 → 13 min on its own row, and was cancelled;
  - `quiet_on` posted off and on, with hours.

  The owner's 22:00–07:00, volume off and the clock on the glass were put back.

## 4. Half-hour quiet hours (2026-11-04)

The owner wants quiet hours in 30-minute steps. Each end keeps its hour (`quiet_from`,
`quiet_to`, and the remembered `quiet_last_*`) and gains its half hour beside it,
`quiet_from_m`/`quiet_to_m`/`quiet_last_from_m`/`quiet_last_to_m`, 0 or 30 (NVS `qfm`, `qtm`,
`qlfm`, `qltm`; a board saved before them reads :00). The hours are untouched so
`stage_snd_cfg` still hands apps hours; `stage_snd_quiet(hour)` answers for hour:00, and the
mixer asks by the minute (`orb_sound_quiet_at_min`). The API reports and takes the half hours;
an end posted in hours alone is on the hour, anything but 0 or 30 is refused. The knob's From
and Until rows step 0..47 half hours and read `22:30`; the hub's pickers offer 48 times.

Bench on Bronze: 22:30-06:30 set, turned off (the half hours kept as the last set), on again
(restored), across a reboot while off, hours alone (:00), and `quiet_from_m: 15` refused; the
hub's Sound card then set 22:00-07:00 back.

The same flash carried a fix found on the way: `h_app_upload` read the body by
`sizeof rx`, and `rx` had become a PSRAM pointer, so every app upload arrived 8 bytes per
recv. A small app got through; clock.orbapp (907 KB) was reset at 538-700 KB every time,
while the same bytes through `/api/v1/sd/write` landed whole. With the size named
(`APP_RX`, 4096) the clock installs in about 10 s.
