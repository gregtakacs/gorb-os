# alerts-1: a backed ring that cannot be missed

## Goal

Deliver `docs/app-platform-plan.md` §64.12 end to end for the ring and the pomodoro's alerts. A
record may set `ORB_RING_BACKED 0x04u`, and when it does the whole rim band is painted black with the
arc drawn on it, ignoring the opacity byte. Pomodoro flash/breathe alerts become backed; the running
drain remains unbacked and still obeys `ring_op`.

## Read first

- `docs/specs/alerts-design.md` §0, §1, §2, §4.1, §6 — the backing rule and cost invariant.
- `main/display/orb_ring.h`, `main/display/orb_ring.c` — record, `ring_clamp`, stamp, service,
  `ring_effect_alpha`, `ring_alpha_moved`, `ring_lanes`.
- `apps/sdk/orb.h` — public `orb_ring_t` mirror.
- `apps/README.md` — ring section.
- `apps/pomodoro/pomodoro.h`, `pomodoro.c`, `main.c` — `pm_ring_t`, `pm_ring_for`, `ring_send`,
  `ring_moved`.
- `tools/host_ring_test.py`, `tools/host_pomodoro_test.py` — reference model and mutant style.

## Contract

1. **`main/display/orb_ring.h`** add `#define ORB_RING_BACKED 0x04u` beside `ORB_RING_MATCH` and
   `ORB_RING_CCW`; widen `ORB_RING_FLAGS_MASK` from `0x03u` to `0x07u`; add
   `#define ORB_RING_BACKED_MIN_ALPHA 96`. Keep `orb_ring_rec` size unchanged.
2. **`ring_clamp`** masks flags with the new mask. Unknown bits are dropped.
3. **Pure helpers** add `ring_backed565(uint32_t argb, int arc)`,
   `ring_paint_alpha(const orb_ring_rec *r, int effect_alpha)`, and
   `ring_backed_moved(int was_paint, int now_paint)`.
4. **Stamp** for a backed winning record paints every pixel inside the ring annulus: arc pixels use
   the arc colour scaled toward black for breathe, full colour/black alternation for flash; non-arc
   band pixels use black; pixels outside the annulus are untouched. Do not skip a backed off-phase.
5. **Service send gate** uses backed paint movement only when either old or new record is backed.
   For unbacked records, keep today's alpha-movement logic exactly.
6. **SDK/README** mirror the bit and document it in the existing ring docs. No `.a` rebuild is part
   of this step unless `make check-apps` requires generated artifacts already tracked.
7. **Pomodoro** add `int backed;` to `pm_ring_t` appended after existing fields and extend any
   `_Static_assert` message without breaking offsets; set it only for alert states (pause breathe and
   wait/waited alerts), not the running drain; OR `ORB_RING_BACKED` in `ring_send()` when set and
   compare it in `ring_moved()`.
8. **Tests** add host-ring cases for mask, colour, paint alpha, breathe floor, flash off-phase band,
   outside annulus untouched, and lane-count unchanged; add pomodoro cases for backed alert vs
   running drain and `ring_moved` transition. Mutants must bite.

## Behaviour and edge cases

- A zeroed record is unchanged: not backed, same output.
- `flags = 0xF8` clamps to 0; `flags = 0x04` survives.
- Backed solid ignores alpha and still paints the band black.
- Backed breathe never reveals the app underneath; alpha floor is 64 and the pixel is blended against
  black, not destination alpha.
- Backed flash alternates coloured and black band; black phase still causes a send/paint.
- A record without the backing bit must not change alpha, movement or lane behaviour.
- Pomodoro running drain uses `ring_op`; pause and alerts use backed.

## Acceptance criteria

1. `python3 tools/host_ring_test.py 2>&1 | tail -3 | grep -q 'RESULT: pass'`
2. `python3 tools/host_ring_test.py --mutations > /tmp/alerts1m.log 2>&1; grep -qE 'mutants caught ([0-9]+)/\1' /tmp/alerts1m.log && ! grep -q MISSED /tmp/alerts1m.log`
3. `python3 tools/host_pomodoro_test.py --mutations > /tmp/alerts1p.log 2>&1; grep -qE 'mutants caught ([0-9]+)/\1' /tmp/alerts1p.log && ! grep -q MISSED /tmp/alerts1p.log`
4. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps`
5. `make check-sandbox`
6. `grep -qE 'define ORB_RING_BACKED[[:space:]]+0x04u' main/display/orb_ring.h`
7. `grep -qE 'define ORB_RING_FLAGS_MASK[[:space:]]+0x07u' main/display/orb_ring.h`
8. `grep -q ORB_RING_BACKED apps/sdk/orb.h`
9. `git diff --name-only` lists only the Contract files plus `docs/specs/*`.

## Out of scope

Do not touch `apps/pomodoro/app.json`, `apps/pomodoro/version.txt`, `main/app`, `main/input`,
`main/net`, `tools/orb-manager`, press caps, `orb_press_*`, settings cap, or any router.

## Notes

The backing must not change lane geometry; if the lane numbers move, the implementation has violated
§64.12's cost claim. Keep host tests pure and avoid adding ESP-IDF-only tests.
## Orchestrator amendments

See alerts-design.md § Orchestrator amendments: `ORB_RING_BACKED_MIN_ALPHA` is 96, not 64; the trough is the colour at 96/255 on black.
