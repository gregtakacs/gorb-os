# overlay-2: the OS reads the theme's colour and the ring matches the app

> **Read first: `docs/specs/overlay-design.md` "Orchestrator amendments".** The reserved byte is `flags` (MATCH 0x01, CCW 0x02), `_r1` becomes `start` (degrees), and this step adds the direction and start to the span maths and the host suite.

> **Read first: `docs/specs/overlay-design.md` §4, §5, §6, §8 (checks 4-5); then
> `docs/specs/ring-design.md` §3, §6, §8 and `docs/specs/ring-2-firmware-sdk.md`.** Base
> `RING_TIP`, and overlay-1 must be in: this step reads a field the C reader only starts carrying
> there.

## Goal

Three small, separate things that only work together: the live theme answers its overlay colour to
the OS (`orb_theme.c`), the ring record carries the app's request to use it (a named `match` byte
where the record had a reserved zero), and the ring's service substitutes that colour into the
winning record's arc and track while keeping their alphas. After this step, an app that sets
`match` draws the OS ring in whatever colour the theme on the glass names, and an app that sets
nothing is untouched.

## Read first

- `docs/specs/overlay-design.md` §4 (the accessor, and why the word must be copied out of the
  region), §5 (why the flag is the reserved byte and not a bit of `effect`), §6 (the substitution
  and the tick that re-applies it), §8 checks 4-5.
- `main/art/orb_theme.c` — `orb_theme_geo_t` in `orb_theme.h:88-117`, `adopt_bundle3` (:3002): the
  `orb_bundle_parse_region` call (:3019), `sole_sec` (:3023), the `s_geo` fill block (:3106-3131),
  and `heap_caps_free(region)` at :3060 with its "the parse's copy dies with the map" comment; and
  the pair path that fills `s_geo` without a container (:490, `s_geo.from_container = false`), where
  `overlay` must be 0.
- `main/display/orb_ring.h` / `orb_ring.c` at RING_TIP — the record and its `_Static_assert`,
  `ring_clamp`, `ring_pick`, `ring_effect_alpha`, `effect_name`, and the IDF half's
  `orb_ring_disp_service` (its 500 ms grid, `s_rec`, `s_have`, `pick_slot`, `mark_band`).
- `apps/sdk/orb.h`'s ring section (the record, `ORB_RING_*`, `orb_ring_set`'s `i(i)` signature) and
  `main/app/orb_app_wasm.c`'s `imp_ring_set` (a 16-byte `m3ApiCheckMem` + verbatim `memcpy`).
- `tools/host_ring_test.py` — the suite's style, and the `MUTATIONS` list to extend.
- `apps/README.md`'s ring section (the record is documented there; it must not go stale).

## Contract

**`main/art/orb_theme.h`.** After `orb_theme_live_slug()`:

```c
/* The live theme's overlay colour (§64.9, docs/specs/overlay-design.md §4): 0x01RRGGBB, or 0 when
 * the theme names none — a theme baked before this, the compiled-in pair, or no theme at all.
 * Callable from the LVGL task: it reads a scalar the bind wrote before the theme went live. */
uint32_t orb_theme_overlay(void);
```

and `uint32_t overlay;` in `orb_theme_geo_t` beside `values_off`, so the value is visible to anyone
reading the geo the way the values are.

**`main/art/orb_theme.c`.** Read the colour into a local the moment the parse has succeeded —
`const uint32_t overlay = orb_bundle_overlay(&b);` beside the `sole_sec(&b)` call, before any early
return — and `s_geo.overlay = overlay;` in the `s_geo` fill block at :3106-3131, beside
`values_off`. The region is freed before that block runs (`heap_caps_free(region)`, :3060, "the
parse's copy dies with the map"), so a pointer into it or a second flash read are both out: the
scalar survives its region's death precisely because it is a copy, which is the same reason
`values_off` is an offset and not a pointer. On the compiled-pair path and every refusal path the
field is 0 — the static's zero, plus one explicit `s_geo.overlay = 0;` wherever the pair fills the
geo, so a theme swap cannot leak the last theme's colour onto an unthemed dial.
`orb_theme_overlay()` returns `s_geo.from_container ? s_geo.overlay : 0`.

**The record's byte.** `main/display/orb_ring.h` renames `uint8_t _r0` to `uint8_t match` and adds
`#define ORB_RING_MATCH 0x01u` beside the effect constants; `apps/sdk/orb.h` renames its `_res0` to
`match` and repeats the same define; `apps/README.md`'s ring section states the byte in the table it
already keeps and says what an OS without this slice does with it (nothing). Nothing else moves: the
offset is byte 1, the struct stays 16 bytes, both `_Static_assert`s still hold, `binds.json`'s
`ring_set` signature is still `i(i)`, and `orbsdk.py` has no new size to gate. `imp_ring_set` needs
no change — it already `memcpy`s the whole 16 bytes after checking them — and neither does the app
side of the seam. **No new cap, no new import, no new bind:** the app asks with a byte it already
sends, and the OS resolves it; the pomodoro never calls `orb_theme_overlay`.

**`ring_clamp`.** Normalises the byte: `r->match = r->match ? 1 : 0;` after the effect and period
clamps it already applies. That is the line that stops an app writing 7 into a reserved field from
getting a second meaning out of it.

**`ring_apply_overlay`.** In the pure core, declared beside `ring_effect_alpha`:

```c
/* Fold the live theme's overlay colour into a picked record (§64.9): `overlay` is 0x01RRGGBB or 0,
 * and anything that is not 0x01RRGGBB — including a record that did not ask — changes nothing.
 * Where it does apply, arc and track keep their own alphas and take this RGB; an alpha of 0 still
 * draws nothing, so a theme cannot unmute a record its app muted. Idempotent. */
void ring_apply_overlay(orb_ring_rec *r, uint32_t overlay);
```

Pure: no includes beyond the file's own, no time, no IDF, and no call to `orb_theme_overlay` — the
caller feeds the colour in so the rule is host-testable and the glue stays a policy-free three
lines.

**`orb_ring_disp_service`.** After the existing `ring_pick` and before `s_rec` is updated, and on
the same 500 ms grid the effect already runs on:

```c
const uint32_t ovl = orb_theme_overlay();
if (ovl != s_ovl) { s_ovl = ovl; changed = true; }   /* the theme under a standing ring (§6) */
ring_apply_overlay(&cur, ovl);
```

with `static uint32_t s_ovl;` beside `s_rec`, zeroed by `orb_ring_disp_init`. The existing
"something changed ⇒ `mark_band()`" path is what repaints, so a theme that lands while a ring is
standing is honoured within one grid step (500 ms) rather than at the next fraction move. Do not
apply the overlay inside `orb_ring_disp_stamp`: the stamp is the per-pixel path and must stay a
blend.

**`tools/host_ring_test.py`.** Vectors for checks 4 and 5 of design §8, in that file's existing
style, plus mutants: drop the alpha preservation (arc becomes `0xFF123456`), drop the `match` test
(a theme's colour overwrites a record that never asked), and make the presence test a nonzero test
(so `0x00ff8800` is read as a colour). Each must bite.

## Behaviour and edge cases

- `match = 0`, any theme: `ring_apply_overlay` is a byte-for-byte no-op, so every app on the base
  behaves exactly as ring-2 shipped it. This is the first vector, not an afterthought.
- `match = 1`, theme without a colour (`orb_theme_overlay()` answers 0): the record is unchanged —
  amendment H's "behaves as From the look" — and the ring still animates with its own effect.
- `match = 1`, theme `0x01123456`, record arc `0x80FF0000` track `0x3300FF00`: `0x80123456` and
  `0x33123456`. Applying it again answers the same 16 bytes.
- A muted record stays muted: arc alpha 0 answers alpha 0 over any theme colour; track alpha 0
  answers alpha 0. A theme colours a ring, it does not switch one on.
- `0x01000000` (black, present) applies; `0x02000000` and `0x00123456` do not.
- `match = 7` clamps to 1 and the rest of the record is untouched; `match = 0` stays 0.
- A theme swap under a standing ring: the service's `s_ovl` differs at the next tick, the annulus is
  re-marked, and the colour on the glass is the new theme's within one `ORB_RING_STEP_MS`. The
  reverse too — a theme with no colour taken down while a matching ring stands ⇒ the record's own
  colours come back (the substitution is applied to a freshly picked record each tick, so nothing
  is remembered in `s_rec` between ticks).
- `orb_ring_status()` still reports owner/fraction/effect only; `/status`'s shape does not change.
- The copy arc still wins the band (`orb_edge_arc_shown()` gates the dispatch, ring-2 §8) and keeps
  its own colour: this step changes no edge-arc file.

## Acceptance criteria

Steps 1-3 run on the sandbox; the ESP-IDF build is the orchestrator's (there is no `idf.py` here —
say so rather than pretending, and let the greps and the host suite carry the claim).

1. `python3 tools/host_ring_test.py | tail -1` → `RESULT: pass`.
2. `python3 tools/host_ring_test.py --mutations > /tmp/ov2m.log 2>&1` → `N of N mutants caught`, no
   `MISSED`.
3. `gcc -std=c11 -Wall -Wextra -Werror -I main/display -c main/display/orb_ring.c -o /dev/null` →
   exit 0, which is the proof the new function kept the core IDF-free (ring-1's discipline).
4. `grep -q 'ORB_RING_MATCH' main/display/orb_ring.h && grep -q 'ORB_RING_MATCH' apps/sdk/orb.h && grep -q 'ring_apply_overlay' main/display/orb_ring.h && grep -q 'orb_theme_overlay' main/art/orb_theme.h && grep -q 's_geo.overlay' main/art/orb_theme.c && grep -q 'match' apps/README.md` → all six really landed.
5. `gcc -fsyntax-only -std=gnu11 -I main -I main/art -I main/display main/art/orb_theme.c` with the
   IDF shims the repo's own host suites use, or — when those shims do not cover `orb_theme.c`, which
   they do not — the `idf.py build` the orchestrator runs; report which was used.
6. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → the SDK still matches the firmware and
   every app builds against the changed `orb.h` record.
7. `python3 apps/sdk/orbsdk.py binds` → green, and `git diff --stat -- apps/sdk/binds.json` empty:
   no new import was needed.
8. `make check-host` → every suite green (`orb_bundle.c` is compiled by `host_orb_bundle_c_test.py`,
   and the record's rename must not have broken a neighbour).

## Out of scope

Overlay-1's four legs, every editor and bake module, `apps/*/version.txt`, the shelf, the hub,
`orb_edge_arc.*`, `orb_net.c`'s `/status` (it names no colour today), §55's blips and any second
consumer of `orb_theme_overlay()`. No new cap, no new import, no change to `binds.json`, no change to
`orb_app_wasm.c`. Do not add a settings key or an NVS default for "the OS's colour": §64.9 puts the
colour in the theme, and the fallback is per-surface (design §6).

## Notes

- The reserved byte was chosen over a bit in `effect` for two reasons worth keeping in the commit
  message: `ring_clamp` validates `effect` and three readers consume it, so a flag bit there turns
  `FLASH | 0x80` into a silent *steady*; and an app that sets `match` against firmware without this
  slice keeps its own colours, which is the interim behaviour pomodoro-design amendment H already
  promises.
- `orb_theme_overlay()` reads one scalar, not the flash, and not the region: `adopt_bundle3` frees
  the region before anyone can call it, and a flash read on the LVGL task's 2 Hz tick would be a
  device-side surprise with no test. That is why the word becomes a geo field.
- The substitution belongs on the service's existing grid rather than on a bind hook, so the
  correctness argument is "at most one tick of lag", not "every path that can change a theme must
  remember to call this".


