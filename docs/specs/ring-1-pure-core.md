# ring-1: the ring's pure core (geometry, blend, effect clock, clamps, ownership)

## Goal

The host-testable brain of the OS ring (§64.1): a C module with **no ESP-IDF and no LVGL
includes** that owns the ring's geometry, the alpha blend over a byte-swapped RGB565 tile pixel,
the effect clock (steady/flash/breathe), the OS clamps, and the one-record-per-app ownership
table. It has no notion of DMA, LVGL or `esp_timer` — the caller passes `now_us` in and reads the
result out (design §3). Slice **ring** of `docs/specs/ring-design.md`; the firmware glue is
ring-2, the `color` setting is ring-3. This step ships the module, its header, and
`tools/host_ring_test.py` compiled on gcc with mutations that prove the checks bite.

## Read first

- `docs/specs/ring-design.md` §4 (geometry+blend), §5 (ownership table), §6 (clamps+cost),
  §7 (RAM), §13 (the arbitration table — the ring shows nothing off-phase / when unowned).
- `main/display/orb_edge_arc.c` — the arc's `wire()` byte swap, `R_OUT 231`/`R_IN 224`,
  `CX=CY=233`, the `mark_segment`/`orb_edge_arc_stamp` row+pixel geometry to reuse (do not link),
  the `0x66DF`/`0x3186` wire colours that prove the tile order.
- `tools/host_setui_test.py` — the **exact** host-suite style to imitate: compile the shipped C
  with `gcc -std=c11 -O1 -Wall -Wextra -Werror`, `MUTATIONS` as (find, replace) pairs, the
  `--mutations` loop, `RESULT: pass` / `%d of %d mutants caught` output.
- `main/app/orb_svc.h:69` — the cap enum (so the core's refusal contract matches ring-2);
  `main/app/orb_app.h:295` `#define ORB_APP_CAP 8`.

## Contract

Create `main/display/orb_ring.h` and `main/display/orb_ring.c` (pure), and
`tools/host_ring_test.py`. `orb_ring.c` includes only `<stdint.h> <math.h> <stddef.h>`.

Header (`orb_ring.h`) — the pure surface:

```c
#define ORB_RING_STEADY 0
#define ORB_RING_FLASH  1
#define ORB_RING_BREATHE 2
/* clamps (design §6) */
#define ORB_RING_STEP_MS 500
#define ORB_RING_BREATHE_MIN_MS 4000
#define ORB_RING_FLASH_MIN_MS   1000
#define ORB_RING_STEP_PCT 5
#define ORB_RING_DEFAULT_PERIOD_MS 4000

typedef struct { uint16_t fraction; uint8_t effect; uint8_t _r0;
                 uint32_t arc; uint32_t track; uint16_t period_ms; uint16_t _r1;
} orb_ring_rec;                 /* 16 B — the same layout as orb.h's orb_ring_t (ring-2) */

/* one tile pixel, wire-order RGB565 in and out; argb top byte = alpha; a==0 returns bg */
uint16_t ring_blend565(uint16_t bg565_swapped, uint32_t argb);
/* the glass rows the annulus touches for this fraction: 0..1000 -> [2,464], out (y0,y1) */
void     ring_rows_for(int fraction, int *y0, int *y1);
/* effect resolved to one instant: STEADY=255; FLASH on/off by (now_ms/period) halves;
   BREATHE a slow 0..255 ramp over the clamped period; returns an arc+track alpha multiplier 0..255 */
int      ring_effect_alpha(const orb_ring_rec *r, int64_t now_ms);
/* clamp in place: fraction 0..1000, effect to a known value, period up to the effect floor */
void     ring_clamp(orb_ring_rec *r);
/* ownership (design §5): the caller supplies n-sized parallel arrays — the record table,
   owner[slot] (-1 = cleared/unowned) and seq[slot] (a monotonic stamp). No internal statics. */
void     ring_init (orb_ring_rec *tbl, int *owner, int n);        /* all owner[i] = -1 */
void     ring_set  (orb_ring_rec *tbl, int *owner, int64_t *seq, int n, int slot,
                    const orb_ring_rec *r, int64_t now_us);   /* clamps, stores, seq[slot]=now_us/1000 */
void     ring_clear(orb_ring_rec *tbl, int *owner, int n, int slot);  /* owner[slot] = -1 */
/* newest owned record folded+clamped into *out; false when none. Ties in seq break by slot. */
int      ring_pick (const orb_ring_rec *tbl, const int *owner, const int64_t *seq,
                    int n, orb_ring_rec *out);
```

`ring_blend565`: invert the wire swap, unpack r5/g6/b5, `out = bg + (src-bg)*a/255` per channel
(src = the ARGB low 24 bits), repack to 565, re-swap. `a==0` ⇒ return `bg` unchanged, byte for
byte; `a==255` ⇒ the pure source colour. `ring_clamp`: fraction floors at 0 and clamps to 1000,
an unknown `effect` becomes `ORB_RING_STEADY`, `period_ms==0` becomes
`ORB_RING_DEFAULT_PERIOD_MS`, else `period_ms` is **raised** to the effect's floor (`BREATHE`≥
4000, `FLASH`≥1000; steady ignores its period). `ring_effect_alpha`: steady 255; flash 255 for
the first half of the clamped period and 0 for the second (`((now_ms/period) & 1) ? 0 : 255`);
breathe ramps 0→255→0 smoothly across the clamped period with `cosf`. Off-phase ⇒ 0 (the glass
is untouched, design §13). `ring_pick` walks ≤`n` slots, keeps `owner[i]>=0`, takes the max `seq`
(ties by the higher slot), copies that record into `*out` after `ring_clamp`, returns 1; returns
0 if none owned. No `malloc`, no static state — the caller's arrays are the table, so the same
code drives the PSRAM `s_own[ORB_APP_CAP]` ring-2 allocates and the host suite's stack arrays.

## Behaviour and edge cases

- `ring_blend565` with alpha 0 returns the background pixel exactly (a byte-equal assertion).
- alpha 255 over any background returns the source colour, round-tripped on a few RGB565-exact
  colours (5/6/5 quantisation is what is checked, not float equality).
- fraction 0 ⇒ the arc covers nothing (every annulus pixel is track); fraction 1000 ⇒ all arc.
  `ring_rows_for` at 0 and 1000 both return the full annulus (2..464): the row set the ring may
  touch, not the arc's extent (design §4).
- BREATHE period 100 → clamped to 4000 (never faster than the slow-breathe ruling); FLASH period
  500 → clamped to 1000; a steady record ignores its period.
- A record with an unknown `effect` (e.g. 7) clamps to STEADY and still shows.
- Ownership: A sets, B sets later (greater `now_us`) ⇒ B owns; B clears ⇒ A owns again (the next
  most recent, design §5); both cleared ⇒ `ring_pick` returns 0 (nothing on the glass).
- A `now_us` tie (same ms) between two apps breaks by the higher slot number (deterministic).
- Empty table (all `owner[i] = -1`) ⇒ pick returns 0, `ring_effect_alpha` is never consulted.

## Acceptance criteria

Run all; all must pass on the gcc-clang sandbox (no ESP-IDF).

1. `python3 tools/host_ring_test.py` → prints `RESULT: pass`, exit 0.
2. `python3 tools/host_ring_test.py --mutations` → every mutant bites: the last line is
   `N of N mutants caught` with N equal to the `MUTATIONS` count, and no `MISSED` line.
3. `gcc -std=c11 -Wall -Wextra -Werror -I main/display -c main/display/orb_ring.c -o /dev/null`
   exits 0 (proves it needs no ESP-IDF / `sdkconfig.h` — design §3, requirement 11).
4. `grep -q 'ORB_RING_BREATHE' main/display/orb_ring.h && grep -q 'ring_blend565' main/display/orb_ring.c`
   → both match (the new symbols are really there — the verify must not pass silently).

## Out of scope

`display.c`, `main.c`, `orb_net.c`, `orb_app_wasm.c`, `orb_svc.h`, `orb_app.h`, `apps/`, the hub,
`orb_edge_arc.c`, the `color` type (ring-3). No wasm import, no cap, no `/status`, no stamp call
site. Do not add DMA/LVGL includes to `orb_ring.c`. Do not touch any app's `version.txt`.

## Notes

- Byte order is the trap: the tile holds **wire-order** (byte-swapped) RGB565, proven by the arc
  writing `wire(0x66DF)`. A blend that forgets the swap shows as a red/blue-swapped ring — one
  required mutant (drop the swap on the output only) must make a check fail.
- The core is pure and allocation-free so the same functions run in the firmware (fed PSRAM
  arrays) and under host gcc (fed stack arrays): how a device-side invariant gets a cheap,
  breakable test (host-suites-cheap note).
- Keep it under ~350 lines across the module and the suite.
