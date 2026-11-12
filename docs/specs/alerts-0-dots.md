# The pomodoro's dots count the cycle you set

## Goal

The dots row under the time always draws twelve cells, so "3 of 4" shows three filled and nine
hollow, and every break shows none filled. The row must show exactly N cells for a cycle of N
focuses, filled for the focuses done, so a 4-focus cycle reads as four dots. This is a design
correction by the orchestrator to `docs/specs/pomodoro-looks.md` / pomodoro-design §10.1's row 13
("thirteen frames, f filled for f = 0..12"), which was wrong. The dot art per look is unchanged:
only how many cells a frame draws, and which frame the app picks, change.

## Read first

- `apps/pomodoro/editor/pomodoro-bake.js`: `N_FRAMES`, `DOT_N`, `DOT_SZ`, `DOT_GAP`, `DOT_W`,
  `dotFrame(d, filled)` and `dotFrames(d)` (the eight dot styles), and `picture()` (the preview
  draws the dots row too).
- `apps/pomodoro/editor/_selftest/bake_check.py` (the row-13 check: "thirteen frames") and
  `apps/pomodoro/editor/_selftest/bake.html`.
- `apps/pomodoro/main.c`: the redraw's `R[S_DOTS]` block, which picks `pm_focus_at(v->k, cfg.focuses)`.
- `apps/pomodoro/pomodoro.c` / `.h`: `pm_phase_at`, `pm_focus_at`, `pm_cycle_len` (the cycle is
  focus, short, focus, short, ..., focus, long: focus at even k, short at odd k, long at the last
  slot 2N-1).
- `tools/host_pomodoro_test.py`: its checks, its MUTATIONS list and its main.c source rules.
- `tools/rebake_shelf.py` and `tools/orb-manager/themes/pomodoro-scope.orbtheme` (the shelf
  default, re-baked when its bake changes).

## Contract

1. **The frames.** Row 13 carries one frame per pair (N, n) with N = 1..12 cells and n = 0..N
   filled, in this order: N = 1 (n = 0, 1), then N = 2 (n = 0, 1, 2), ..., then N = 12 (n = 0..12).
   That is 90 frames. The index of (N, n) is `(N - 1) * (N + 2) / 2 + n`.
   - `N_FRAMES` becomes 90. Each frame keeps today's canvas size (`DOT_W` x `DOT_H`, the width of
     twelve cells) and draws its N cells **centred** in it, at today's cell size and gap, in the
     look's own dot style (filled for the first n, hollow after).
2. **The pure pick** in `apps/pomodoro/pomodoro.c`, declared in `pomodoro.h`:
   `int pm_dots_frame(int k, int focuses)` returns that index for the slot k of a cycle of
   `focuses` focuses (clamped to 1..12): n is the focus number during a focus (`k / 2 + 1`, the one
   in hand counts), the focuses done during a short break (`(k + 1) / 2`), and N during the long
   break. Out-of-range input is clamped, never trusted.
3. **The app** (`apps/pomodoro/main.c`) picks `pm_dots_frame(v->k, cfg.focuses)` instead of
   `pm_focus_at(...)` for the dots row, and keeps everything else in that block.
4. **The check** (`bake_check.py`) asserts row 13 has 90 frames of art, and samples frames: (1, 0),
   (4, 3), (12, 12) by index, each with the right number of cells drawn (count the dark/light
   cell blobs in the frame's pixels, or the bake's own STAT line if it reports cells).
5. **Re-bake** the shelf default `tools/orb-manager/themes/pomodoro-scope.orbtheme` with the
   repo's own tooling (`tools/rebake_shelf.py`, or the way pomodoro-editor-4 shipped it), so
   `make check-apps` stays green. Do not bump `apps/pomodoro/version.txt` (one bump per push; the
   push's bump is made elsewhere).

## Behaviour and edge cases

- 4 focuses: focus 3 shows 3 of 4 filled; the short break after it shows 3 of 4; the long break
  shows 4 of 4; a fresh cycle's first focus shows 1 of 4.
- 1 focus: the focus shows 1 of 1; the long break 1 of 1.
- 12 focuses: indices stay under 90 everywhere (the last is 89).
- `focuses` outside 1..12 is clamped before the index is computed.

## Acceptance criteria

1. `python3 tools/host_pomodoro_test.py` → ends `0 failed`, with new checks for every case above.
2. `python3 tools/host_pomodoro_test.py --mutations` → `mutants caught N/N`, no MISSED; add mutants
   for: the one-in-hand not counted, the break showing 0, the long break showing N - 1, a wrong
   triangle offset, and the clamp removed.
3. `ORB_CHROME=$(command -v chromium) python3 apps/pomodoro/editor/_selftest/bake_check.py` → green.
4. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → green (the shelf theme re-baked).
5. `make check-sandbox` → green apart from its named skips.

## Out of scope

- The dot styles themselves, the row's position, any other row, any look's furniture.
- The ring, settings, the press, the OS: later steps of this chain own those.
- Do not modify: `main/`, `tools/orb-manager/*.go`, `apps/pomodoro/version.txt`.

## Notes

- The orchestrator's design call, not open: N cells for the cycle the owner set; the focus in hand
  counts; breaks show the focuses done; the long break shows the full set.
- ~90 small frames add on the order of 100-200 KB to a theme; that is accepted.

## Orchestrator amendment (answers the worker's question)

Raise the pick limit to 96, in this step: `CLK_PICK_MAX` 64 -> 96 in `main/art/clk_types.h`,
`apps/sdk/clk_types.h`, `tools/theme_schema.py` and `orbtheme-baker.js`, with their sentinels and
any test pins that state 64. It is a validation limit only (`pick_n` is uint16; nothing is sized
by it), so it costs no RAM, and the pomodoro already needs this firmware. Keep row 13 one row of
90 frames. Regenerating `tools/orb-manager/apps/pomodoro.orbapp` is allowed (check-apps needs it).

## Orchestrator amendment 2 (replaces amendment 1 and contract 1-3): two rows, no limit raise

Measured in round 3: 90 frames of 198x14 RGB565A8 added 643 KB to the theme (1.48 -> 2.13 MB),
too dear for a pool that is nearly full. The design changes; the pick limit stays 64.

1. **Revert the CLK_PICK_MAX raise** everywhere (main/art/clk_types.h, apps/sdk/clk_types.h,
   tools/theme_schema.py, orbtheme-baker.js, and the test pins you moved): back to 64, exactly as
   on the base branch.
2. **Two picked rows, drawn one over the other at the same place:**
   - **slot 13, the track:** 12 frames, frame N-1 = N hollow cells (N = 1..12), **left-aligned**
     in today's `DOT_W` canvas at today's cell size and gap;
   - **slot 14, the fill:** 13 frames, frame n = n filled cells (n = 0..12), left-aligned the same
     way, each filled cell fully covering the hollow cell under it. Frame 0 is empty.
   The fill row is emitted after the track row, so it draws on top.
3. **Centring is the app's:** both rows are posed with the same x offset,
   `ox = (12 - N) * PM_DOT_PITCH / 2`, where `PM_DOT_PITCH` (in `pomodoro.h`) is the bake's cell
   pitch (`DOT_SZ + DOT_GAP`); `bake_check.py` asserts the two agree.
4. **The pure pick** becomes `void pm_dots(int k, int focuses, int *cells, int *filled)` (both
   clamped; N = focuses clamped to 1..12; filled = the focus number during a focus, the focuses
   done on a short break, N on the long break). The app picks track frame `cells - 1`, fill frame
   `filled`, and poses both with the ox above. Remove `pm_dots_frame`.
5. `bake_check.py`: rows 13 (12 frames) and 14 (13 frames) exist, picked, at the same position;
   sample track N=4 (4 cells), fill n=3 (3 cells), and fill 0 (empty); the pitch agrees.
6. Re-bake the shelf pomodoro-scope; report its size (expect about +100 KB over the base, not +643).
