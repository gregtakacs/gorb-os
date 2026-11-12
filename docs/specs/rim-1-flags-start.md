# rim-1: the ring's flags and start, end to end

> **Read first: `docs/specs/rim-design.md` "Orchestrator amendments".** Verify ends with `make check-sandbox` (the firmware builds in the sandbox now); the pomodoro's paused breath is 2500 ms; its ring send threshold is 2 permille.

Part 1 of `docs/specs/rim-design.md` (read it first — §2, §3, §8, §9). This step builds
overlay-design amendment 1, the two words the ring has always reserved: `flags` (`ORB_RING_MATCH`,
`ORB_RING_CCW`) and `start`, from the record through the span maths to the pomodoro that asks for
them. It changes no geometry, no timing and no display glue.

## Goal

`orb_ring_rec.flags` / `.start` and `orb_ring_t.flags` / `.start` exist and mean what rim-design §2
says; `ring_row_parts(y, fraction, start_deg, ccw, …)` cuts the annulus with a wedge that starts at
`start_deg` and runs counter- or clock-wise; `ring_clamp` masks and folds; the pomodoro's
`ring_dir`, `ring_start` and "Match the app" reach the record; and the host suites prove all of it
with a reference matrix and mutants that bite.

## Read first

`docs/specs/rim-design.md` §2/§3/§8/§9 · `docs/specs/overlay-design.md` amendment 1 ·
`docs/app-platform-plan.md` §64.7/§64.8/§64.9/§64.11 · `main/display/orb_ring.h` and `orb_ring.c`
(the record, `ring_clamp`, `ring_row_parts` and its `p_hit`/`e_first`/`e_last` helpers,
`orb_ring_disp_stamp`, `ring_effect_alpha`) · `tools/host_ring_test.py` (the reference model, the
fraction sweep, the mutant table) · `apps/sdk/orb.h`'s `orb_ring_t` · `apps/README.md`'s ring
section · `apps/pomodoro/main.c` (`load_settings`, `pick_colours`, `ring_send`, `ring_moved`) ·
`apps/pomodoro/pomodoro.h`/`.c` (`pm_cfg_t`, `pm_ring_t`, `pm_ring_for`) · `apps/pomodoro/app.json`
(`ring_dir`, `ring_start`, `ring_col_away`) · `tools/host_pomodoro_test.py` (the `shape` rule: no
`orb_*` names and no floats in `pomodoro.c`).

## Contract

1. **`main/display/orb_ring.h`** — add `ORB_RING_CCW 0x02u` and `ORB_RING_FLAGS_MASK 0x03u` beside
   the `ORB_RING_MATCH 0x01u` that §64.9 already shipped (its value and meaning do not move); in
   `orb_ring_rec` rename byte-1 `match` to `flags` and turn the reserved `uint16_t _r1` into
   `uint16_t start`, at the layout and offsets of rim-design §2, keeping the existing 16-byte
   `_Static_assert`. The header's comment block still says 16 bytes and the wire is unchanged.
2. **`ring_clamp`** — `flags &= ORB_RING_FLAGS_MASK;` and `start = (uint16_t)(start % 360);`
   beside the existing clamps, idempotent; nothing else about the record moves. Unknown flag bits
   are dropped, not honoured.
3. **`ring_row_parts`** — the signature of rim-design §3 (`int y, int fraction, int start_deg,
   int ccw, orb_ring_part_t *parts, int max`). Wedge semantics, the two-ray cut, the
   monotone-angle lemma, `fraction` 0/1000 short-circuits, trig once per row call, no float at the
   radius boundary, `ORB_RING_PARTS_MAX` stays 4. Keep the file IDF-free (`gcc -std=c11 -Wall
   -Wextra -Werror` compiles it alone, no `esp_*` include).
4. **The stamp and the service** — `orb_ring_disp_stamp` resolves `flags`/`start` once per tile
   call (the `ORB_RING_MATCH` colour override and the two numbers it hands `ring_row_parts`), so
   its per-pixel cost does not grow; `orb_ring_disp_service`'s dirty-row arithmetic is unchanged
   (a wedge still lives inside rows 2..463). Every `ring_row_parts` call site passes the record's
   own `start`/`ccw` — no call site may be left passing only a fraction.
5. **`apps/sdk/orb.h`** — `orb_ring_t` renames byte-1 `match` to `flags` and turns `uint16_t _res1`
   into `uint16_t start` at the same offsets, its `_Static_assert` and doc comment updated (there is
   no `_res0` to remove: byte-1 was the match ask). `binds.json`,
   `imp_ring_set` and the capability bits do not change: the record is passed verbatim.
6. **`apps/README.md`** — the ring section restates the record with the two new words and their
   clamps, in the section's own voice: the byte-1 bullet ("**`match` asks the ring to wear the
   theme**", :419-423) becomes the `flags` bullet with `ORB_RING_MATCH`'s meaning unchanged and the
   older-firmware sentence kept, a new bullet covers `ORB_RING_CCW`/`start` (drains the other way
   from that angle, `start` wraps at 360, an older OS draws twelve-clockwise), and the example
   literal at :404 gains `.flags` and `.start` only if it still reads as an example rather than a
   dump of the struct.
7. **The pomodoro** — `pm_cfg_t` gains `dir`, `start`, `match` (`int`: 0/1, degrees, 0/1) filled
   from the settings at the pure boundary; `pm_ring_t` gains `dir`, `start`, `match`; `pm_ring_for`
   fills them, with `match` set only while the app is off the glass;    `apps/pomodoro/main.c`'s `ring_send()` sets `rec.flags` and `rec.start` from them and
   `ring_moved()` compares both; `load_settings` reads `v[10] ? 1 : 0` for dir,
   `((v[11] % 4) + 4) % 4 * 90` for start (PM_SET's own indices, and `v[13] == -2` with
   `!s_visible` for match, which is `pick_colours`'s existing shape). The three words are appended
   after `period_ms` so `pm_ring_t`'s existing offset `_Static_assert` and its "fraction, effect,
   arc, track, period_ms in that order" message are extended, not broken. The comment near
   `ring_send()` naming amendment J goes away with the code that needed it.
   `pomodoro.c` names no `orb_*` symbol and uses no float; `apps/pomodoro/version.txt` stays
   `v1.7.0`.

8. **Tests** — extend `tools/host_ring_test.py`'s reference with the wedge (`in_wedge`) and run the
   matrix of rim-design §3 over all 466 rows; add `ring_clamp` cases (flag masking, `start`
   wrapping at 359/360/361/719, idempotence); add the mutants named in §3 plus a clamp mutant.
   Extend `tools/host_pomodoro_test.py` with the three new cfg→ring asks and a `ring_moved` mutant.
   No existing check in either suite is deleted or relaxed.

## Behaviour and edge cases

- `start = 0, ccw = 0` is byte-for-byte today's ring at every fraction — the shipped case, so the
  pre-existing expectations must hold as numbers, not merely as "passes".
- `start = 90, ccw = 1, fraction = 250` is the left half of the band; `start = 270, ccw = 1,
  fraction = 250` is the right half. Both checked against the reference pixel for pixel.
- A wedge that straddles twelve (a large fraction from any `start` near 0°/360°) still yields at
  most 4 pieces per row, and the suite records the maximum number of pieces it saw.
- `ORB_RING_MATCH` with no theme overlay colour loaded falls back to the record's own colours, the
  way §64.9's on-glass match does; the flag survives a `ring_clamp` round trip.
- `flags = 0xFC` clamps to 0; `start = 720` clamps to 0; a second `ring_clamp` changes nothing.
- An app that never sets the new words is unaffected: a `memset`-zeroed record is exactly today's
  behaviour, and `orb_ring_disp_stamp`'s output for one is its output today.

## Acceptance criteria

1. `python3 tools/host_ring_test.py 2>&1 | tail -3 | grep -q 'RESULT: pass'` — exit 0.
2. `python3 tools/host_ring_test.py --mutations > /tmp/rim1m.log 2>&1; grep -qE 'mutants caught
   ([0-9]+)/\1' /tmp/rim1m.log && ! grep -q MISSED /tmp/rim1m.log` — exit 0, and the summary quotes
   the line with its count, which must be ≥ the base commit's 25.
3. `python3 tools/host_pomodoro_test.py --mutations > /tmp/rim1p.log 2>&1; grep -qE 'mutants caught
   ([0-9]+)/\1' /tmp/rim1p.log && ! grep -q MISSED /tmp/rim1p.log` — exit 0, count ≥ 54.
4. `gcc -std=c11 -Wall -Wextra -Werror -I main/display -c main/display/orb_ring.c -o /dev/null` —
   exit 0.
5. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` — green (the SDK's 16-byte record gate).
6. `ORB_CLANG=/usr/lib/llvm-21/bin/clang python3 apps/sdk/orbsdk.py build apps/pomodoro` — exit 0:
   the app's C really compiles against the new header, which `check-apps` does not cover.
7. Symbol gates, column-padding tolerant and value-checked so a comment mentioning a name cannot
   pass: `grep -qE 'define ORB_RING_CCW[[:space:]]+0x02u' main/display/orb_ring.h`,
   `grep -qE 'define ORB_RING_MATCH[[:space:]]+0x01u' main/display/orb_ring.h`,
   `grep -qE 'define ORB_RING_FLAGS_MASK[[:space:]]+0x03u' main/display/orb_ring.h`,
   `grep -q 'uint16_t start' main/display/orb_ring.h`,
   `grep -q 'uint16_t start' apps/sdk/orb.h`,
   `grep -q 'start_deg' main/display/orb_ring.h main/display/orb_ring.c`,
   `grep -q 'pm_ring_for' apps/pomodoro/pomodoro.c` (still there),
   `grep -q '\\.start' apps/pomodoro/pomodoro.c` and
   `! grep -qi 'amendment J' apps/pomodoro/main.c`.
8. `git diff --name-only` lists only the files of Contract 1-8 plus `docs/specs/*`.
9. `make check-host` is not run: rim-design §9.7 names the five environment failures that exist on
   the base commit. This step's half of it is criteria 1-3.

## Out of scope

`ring_lanes`, the lane constants, anything in `display.c`/`display.h`/`orb_edge_arc.c`, the step
grid and the period floors, `binds.json` and the SDK `.a`, the hub, `apps/pomodoro/app.json` (its
settings already exist), `version.txt`, and every on-device claim — this step is host-provable in
full, and rim-design §9's bench list is the orchestrator's.

## Notes

Why the wedge is two rays and why `ORB_RING_PARTS_MAX` survives is rim-design §3; the pomodoro
mapping and why dir/start/match cross the pure boundary as plain ints is §8; the 25 Hz grid, the
floors and the A/B of §4.4 belong to rim-2/rim-3 and must not be guessed at here.
