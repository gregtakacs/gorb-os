# Alerts design: a ring backed with black, and a press the OS lends

Slice **alerts** of issue #11, `docs/app-platform-plan.md` §64.12 (the ring's alerts are opaque on
black) and §64.13 (a press anywhere can go to the timer). Design only: this file, `alerts-1`..`alerts-3`
and `alerts-chain.json`. No product code here; the three steps write it. The pomodoro is both
features' first user, and its looks, editor and chimes are another slice's work — nothing here
touches how the pomodoro draws its face.

## 0. The rulings that bind this slice

- **§64.12, the owner on Bronze:** a breathing or flashing ring blended over a busy app at the
  ring's opacity "is lost. An alert must be seen." **Ruling: a backed ring** — `flags` BACKED 0x04
  draws the whole rim band black with the arc on it at full opacity, "whatever the opacity byte
  says"; breathe fades the colour toward black and back, "never into the app beneath"; flash
  switches between the colour and black; "the lanes already cover the whole band, so a backed ring
  costs no extra pixels". The pomodoro "backs its alerts only": flash and breathe. **The running
  drain keeps the owner's `ring_op`**, so it stays quiet over other apps.
- **§64.13, the owner:** "leaving an app to reach the pomodoro just to start the next phase is a
  chore". **Ruling: the OS lends the press** — `orb_press_claim(on)` under a new cap `press`,
  declared in app.json and shown on the Permissions page like any other; while a claim stands a
  press on whatever app is on the glass goes to the claimant and "that app never sees it", read by
  its `orb_service` (services take no events today, so the slice designs the delivery, "e.g. a
  polled `orb_press_lent()` count"); turn and press-and-turn stay with the app on the glass and
  hold stays the OS's; one claimant at a time, latest caller wins, as with the ring (§64.1); a
  claimant uninstalled or faulted loses its claim; **the OS confirms a lent press with a short
  pulse of the claimant's ring**. The pomodoro's setting `press_any` (Off / When it's calling /
  Whenever it runs, default When it's calling).
- **app-design-rules.md § The knob** now names the one exception to "the press is the app's": an
  app the owner has let borrow it. This slice is that exception's implementation.
- **§64.7/§64.9/§64.11 stand.** Colours are the theme's unless a setting overrides them, the ring
  keeps its direction and start, and the lanes (rim-2/rim-3) are the ring's refresh. A backed ring
  rides the same 60 windows and the same 19,912 pixels — rim-design §4.6's rule that the cover is a
  property of the band alone is what makes §64.12's "no extra pixels" true, and it stays true
  because nothing here may make `ring_lanes()` depend on a record.
- **Internal RAM is the cliff** (`gorb-os-internal-ram-under-clock`): ~23 KB free under the clock.
  This slice adds **zero new internal .bss** — the claim's word and its counters are PSRAM
  (`EXT_RAM_BSS_ATTR`), like the ring's table and `orb_svc.c`'s `s_refused` — and zero new tasks and
  zero new internal DMA allocations. The one number that does move is the manifest's settings cap
  (§8), whose whole cost is PSRAM descriptors.
- **Host suites are the cheap signal** (`gorb-os-2-host-suites-are-cheap`): every verdict here is
  pure C compiled by `tools/host_ring_test.py`, `tools/host_pomodoro_test.py`,
  `tools/host_press_test.py` (new) or `tools/host_svc_test.py`, with mutants that must bite.
  `host_svc_test.py` compiles `main/app/orb_svc.c` **verbatim** against stub leaves, which is why
  the press machine can be host-proved and the router's four lines cannot.
- **No device, no ESP-IDF in the sandbox** (`make check-sandbox` says so and skips the build). The
  steps name the files only `idf.py build` checks; the orchestrator builds, flashes and benches.

## 1. What the slice adds, mapped to the rulings

| the ruling | this slice | step |
|---|---|---|
| "a ring record may ask for a black track (`flags` BACKED 0x04)" | `ORB_RING_BACKED 0x04u`, `ORB_RING_FLAGS_MASK 0x07u`, in `orb_ring_rec`, `orb_ring_t` and `apps/README.md` | alerts-1 |
| "the whole rim band is drawn black, and the arc on it at full opacity" | `ring_backed565()` + `ring_paint_alpha()`, the stamp's hoisted branch, `ring_clamp`'s wider mask | alerts-1 |
| "breathe fades the colour toward black … never into the app beneath" | the floor `ORB_RING_BACKED_MIN_ALPHA` 64, and `ring_backed_moved()` — the gate for a band whose off-phase is black, not nothing | alerts-1 |
| "flash switches between the colour and black" | a backed flash alternates the colour at 255 and black, the alpha byte ignored; the stamp no longer skips a backed off-phase | alerts-1 |
| "the pomodoro backs its alerts only … the running drain keeps `ring_op`" | `pm_ring_t.backed`, set for `PM_PAUSE` and `PM_WAIT`/`PM_WAITED` alone, and `ring_send()`'s flag byte | alerts-1 |
| "the lanes already cover the whole band, so it costs no extra pixels" | `ring_lanes()` unchanged and still record-free; the suite's 60 / 19,912 / 9,908 numbers stay | alerts-1 |
| "`orb_press_claim(on)`, under a new cap `press`" | `CAP_PRESS = 11`, the name table, every Permissions list, orbsdk, the hub's page | alerts-2 |
| "read by its `orb_service` … e.g. a polled `orb_press_lent()`" | the import pair `press_claim` `i(i)` / `press_lent` `i()`, and the machine in `main/input/orb_press.c` | alerts-2 |
| "a press … goes to the claimant instead, and that app never sees it" | one branch in `poll_timer_cb()`, after the hold and the press-and-turn have been dealt with | alerts-2 |
| "one claimant at a time, latest caller wins; uninstalled or faulted loses it" | `press_claim`'s latest-wins rule, and the drop on revoke / `slot_release` / the first trap | alerts-2 |
| "the OS confirms a lent press with a short pulse of the claimant's ring" | `orb_ring_disp_pulse()`: the claimant's own record drawn solid for `ORB_RING_PULSE_MS` 160 ms | alerts-2 |
| "the pomodoro claims and drops the press as its state moves" | `press_any` in `app.json`, `pm_cfg_t.press_any`, the pure `pm_press_want()`, `press_send()` in `orb_service()` | alerts-3 |
| "on its own glass it handles the press as it always has" | nothing in `orb_input` moves; the OS never lends over the claimant's own glass | alerts-2, alerts-3 |
| the loader's 16 settings are full | `ORBAPP_SETTINGS_MAX` and its three mirrors 16 → 18 | alerts-2 |

## 2. §64.12 — the backed ring

### 2.1 The record, the byte and the ABI

`main/display/orb_ring.h` adds `#define ORB_RING_BACKED 0x04u`, widens `ORB_RING_FLAGS_MASK`
`0x03u` → `0x07u`, and the public mirror in `apps/sdk/orb.h` (`orb_ring_t.flags`) adds the matching
`ORB_RING_BACKED` macro. `apps/README.md`'s ring row gets one clause: a backed record paints the
whole band, and opacity does not apply. No record grows and no wire format changes: the flag byte
already carries three values, and `ring_clamp()` masks it with `ORB_RING_FLAGS_MASK`, so an older
record with the new bit set simply loses it if the mask were not widened. The mask is the only ABI
change for step 1.

Backed is allowed with any `mode`; it changes the **paint**, not the animation. Backed on solid or
idle is legal, but it is a black band under a solid arc; the pomodoro only uses it on the alert
modes, and the host test pins both the general behaviour and the specific mode combinations.

### 2.2 What "black, then the arc on it" means at pixel level

The stamp already hoists a window's `argb` out of the pixel loop (rim-3's `ring_argb_for()` and
`ring_effect_alpha()`), and the lane cover spans the band. For backed, the hoisted value is selected
**per arc part, not per pixel**: if `arc != 0`, use the theme/effect colour; else use pure black
`0x0000`. Non-arc pixels in the annulus are painted black. Pixels outside the annulus are untouched;
"whole rim band" means the ring's own band, not the screen. If the effect alpha is black or the
record is off, it is still black — black may be the colour. The alpha byte is ignored for paint,
because §64.12 says full opacity "whatever the opacity byte says". `ring_paint_alpha()` returns 255
for an unbacked non-effect path and 255 for backed; `ring_effect_alpha()` remains unmodified for
unbacked paths.

For breathe, colour fades toward black, never into the app underneath. Implement this by scaling the
colour's alpha toward black with floor `ORB_RING_BACKED_MIN_ALPHA 64`, then blending against black.
Black on black at the trough is intentionally nearly invisible; at the peak the arc returns. For
flash, full colour then black. The old path can skip when `eff == 0`; backed must not, because it has
to draw the black track. The `disp_update()` "off" path also paints the band black when a backed
record wins the rim.

### 2.3 Sending, movement, and the alert gate

`ring_moved()` cannot mean the same thing for a backed alert as an unbacked one. Add
`ring_backed_moved(int was_paint, int now_paint)`, used only when either side was backed, and use
the paint state, not alpha movement: a flash record that is fully black and one that is fully
coloured both paint the band. Add pure helpers:

```c
uint16_t ring_backed565(uint32_t argb, int arc);
int ring_paint_alpha(const orb_ring_rec *r, int effect_alpha);
int ring_backed_moved(int was_paint, int now_paint);
```

The service's animation phase is shared for mode effects, but the service's send gate is
`ring_backed_moved(paint_before, paint_after)` when `r->flags & ORB_RING_BACKED`, and the existing
`ring_effect_alpha()` / `ring_alpha_moved()` when it is not. Do not use an unbacked alpha delta as
the send reason for a black trough; that would either never repaint the black track or repaint it
against the rule that backed paint ignores the alpha byte.

### 2.4 Cost

`ring_lanes()` is unchanged and must not depend on records; the existing test still asserts 60 lanes,
19,912 pixels, 9,908 extra. A backed record changes values inside already-covered lanes and the skip
decision, not the set of lanes. A stamp now has one branch outside the inner loop, not a second pass:
hoist a `uint16_t on` and `uint16_t off` and write `arc ? on : off` per pixel. No new DMA buffer,
no new queue item, no new internal byte.

## 3. §64.13 — a press the OS lends

### 3.1 Capability and ABI

`CAP_PRESS = 11`, `CAP_N = 12`, name `"press"`. It appears in `main/app/orb_svc.h/.c`, manifest
cap-name table, `apps/README.md`, `tools/orbapp.py`, `apps/sdk/orbsdk.py`, menu Permissions list,
`main/net/orb_net.c` ledger list, and the hub Go files. The wasm imports are:

```c
orb_press_claim(int32_t on)   // binds name press_claim, sig i(i)
orb_press_lent(void)          // binds name press_lent, sig i()
```

`apps/sdk/binds.json` regenerates from 99 to 101 imports. Apps must declare the cap in `app.json`;
a caller without it gets the same refusal path as any missing cap and no claim. Claim is keyed by
flash slot, not instance, so the same rule used for ring records can reclaim it on unload.

### 3.2 The state machine lives in its own pure module

New `main/input/orb_press.{c,h}`, compiled beside `input_router.c` in `main/CMakeLists.txt`, and a
new `tools/host_press_test.py`. The state table is PSRAM (`EXT_RAM_BSS_ATTR`), one word per slot
plus counters; the public API mirrors the ring and svc style:

```c
void orb_press_init(void);
int  orb_press_claim(int slot, int on);
int  orb_press_owner(void);
int  orb_press_take(int slot);
int  orb_press_lent(int slot);
void orb_press_drop(int slot);
```

`claim(1)` makes `slot` the one claimant, latest caller wins; `claim(0)` clears only if that slot owns
it. Presses delivered to a non-owner are ignored; pending unread presses saturate at
`ORB_PRESS_MAX_LENT = 4`; a fifth is swallowed and logged, never wrapped or spilled. `take()` returns
one queued press or zero and does not remove the claim. Claim state is RAM only, lost on boot and
reboot, as the spec requires.

### 3.3 Router rule

In `main/input/input_router.c`, only a plain press after hold and press-and-turn decisions is
eligible: turn and press-and-turn remain with the glass app; hold remains OS. Do not lend a press to
the carousel, theme carousel, shell, menu, Settings, share window, or requested HTTP screen change.
If `orb_press_owner()` is the current glass app, leave `pressed` exactly as today. If the owner is a
background service, queue the press, call `orb_ring_disp_pulse(owner)`, and clear `pressed` before the
glass app sees it so it does not arm. The claimant's `orb_service()` later polls `orb_press_lent()`
and consumes queued presses itself, treating multiple presses exactly like repeated presses on its own
glass.

### 3.4 Lifecycle drops

Drop the claim on grant revoke, uninstall/engine unload via `slot_release`, trap/fault near
`orb_svc_note_fault`, and import refusal without the cap. The drop path goes through `orb_press_drop()`
so queued presses die with the claim. This prevents a dead app's unread presses from being delivered
to a later app in the same slot.

### 3.5 Confirmation pulse

`ORB_RING_PULSE_MS 160` and `orb_ring_disp_pulse(int slot)` live in `orb_ring.c`. The pulse is the
claimant's own ring record, not OS art, honouring "the OS confirms … with a short pulse of the
claimant's ring." During the window, force full alpha and force sends for that record; on expiry
restore its effect alpha and normal send gate. No pulse if the claimant owns no record, another record
wins the rim, or the ring service is unavailable. The pulse is independent of backed, but a backed
record pulses its full band; an unbacked alert record pulses its arc. The host ring test proves the
timer logic and `ring_backed_moved()` interaction without the display driver.

## 4. The pomodoro's part

### 4.1 Backed alerts

`pm_ring_t` gains `int backed;` appended. `pm_ring_for()` sets `.backed` only for `PM_PAUSE`
(breathe/flash depending state machine) and `PM_WAIT`/`PM_WAITED` alert states, not the running drain.
`ring_send()` ORs `ORB_RING_BACKED` into `rec.flags`. `ring_moved()` compares backing so a transition
from unbacked running to backed alert sends. This is step 1 only; it must not add settings or press.

### 4.2 `press_any`

`apps/pomodoro/app.json` caps add `"press"` and settings add `press_any`, enum, choices
`["Off", "When it's calling", "Whenever it runs"]`, default index 1, help text in the app's voice.
Labels under 24 bytes. `pm_cfg_t` gets `int press_any`. `pm_press_want(const pm_cfg_t*, const pm_t*)`
is pure: Off returns 0; Calling returns 1 only in `PM_WAIT`/`PM_WAITED`; Whenever returns 1 in
`PM_WAIT`/`PM_WAITED`/`PM_RUN`/`PM_PAUSE`; `PM_STOP` returns 0.

`orb_service()` calls `orb_press_claim(pm_press_want(...))` and polls `orb_press_lent()`; each returned
press applies the same state transition a physical press on the app's own glass would cause, repeatedly
until zero. `orb_input()` remains unchanged and still handles presses when the pomodoro is foreground.
Step 3 bumps version to `v1.7.1`.

## 5. RAM, settings, and the 16 → 18 decision

`apps/pomodoro/app.json` currently has exactly 16 settings. Adding `press_any` makes 17, and the
current writer/loader refuses with `17 settings; the loader's table holds 16`. The chosen fix is to
raise the cap from 16 to **18**, not to rename, drop or collapse existing settings. Evidence measured
in the sandbox: the new 17-entry manifest schema is 3,948 bytes against a 6,144-byte schema limit, so
size is not the blocker; the table size is.

Cap mirrors are `ORBAPP_SETTINGS_MAX` in `main/app/orb_app_manifest.h`, `orb_app.h`, and
`main/app/orb_setui.h` row count, plus `tools/orbapp.py` `SETTINGS_MAX` and test pins in
`tools/host_orbapp_reader_test.py` and `tools/host_orbapp_install_test.py`. The firmware cost is
PSRAM only: `card_entry_t.settings_store`, `orb_setui_t.row`, and install scratch. A two-entry margin
leaves room for the alerts slice and one future setting without touching every mirror again.

Internal RAM impact is intentionally zero: no new internal `.bss`, no new queue, task, DMA buffer or
IRAM interrupt code. `orb_press.c` state is PSRAM BSS and small: one claimant slot word plus per-slot
press counts saturating at four. `orb_ring.c` pulse state is PSRAM too: one slot and expiry timestamp.
Step 1 changes no tables; step 3 changes one app manifest. If a compiler adds static bytes in internal
memory, the firmware build check must be reviewed by the orchestrator before flashing.

## 6. How each step is verified

Every step runs its named host suite plus `make check-sandbox`, never full `make check` in the
sandbox because ESP-IDF is absent. Each step spec gives literal acceptance commands and mutants.

- **alerts-1** verifies backed mask, colour and alpha helpers, stamping, send gate, unchanged lane
  numbers, and pomodoro backed alert states. Mutation tests must bite on mask width, ignore opacity,
  black trough, and running drain backed mistake.
- **alerts-2** verifies cap/ABI plumbing, state machine, lifecycle drops, saturation, latest caller
  wins, pulse expiry and refusal paths. Router integration remains device-only; host test covers pure
  module and any hook that can be compiled under existing stub suites.
- **alerts-3** verifies manifest pack at 17 settings, `pm_press_want`, service claim transitions,
  lent press consumption, and version bump. The pomodoro suite's `--mutations` must catch wrong
  calling-state claim and missed release.

Final slice gate is the harness command in the task and a diff check that only `docs/specs/` changed.

## 7. Step plan and ordering

1. `alerts-1-backed-ring.md` — §64.12 only. It is first because it is visible, testable, and does not
   require the press cap or router changes. It also proves the pomodoro's backed alert drawing can be
   changed before the more invasive OS lending exists.
2. `alerts-2-lent-press.md` — §64.13 OS surface only. It includes settings cap raise because step 3
   would otherwise fail packing. It does not touch pomodoro.
3. `alerts-3-pomodoro-press.md` — app adoption only: cap, setting, claim/release in service, consume
   lent presses, version bump. It depends on both ring backing and OS press availability.

`alerts-chain.json` lists these three steps in order. The dependency is real: step 3 needs step 1's
backed ring and step 2's `press` ABI plus settings cap.

## 8. Questions this design answers by choosing, not guessing

- Should the OS lend presses to foreground? No. The claimant's own foreground path is unchanged.
- Should lent press wake or raise the app? No. The claimant reads it from `orb_service()`.
- Should a service be allowed to queue more than four presses? No. Saturating at four avoids unbounded
  state and keeps a stalled service from replaying a flood when it wakes.
- Should the pulse be OS art? No. §64.13 says the claimant's ring, and the claimant may not have one;
  then no pulse is drawn.
- Should `press_any` default to whenever? No. Spec default is when calling.
- Should settings cap be 17 or 18? 18. Exactly 17 makes the next setting another cross-file bump.


## Orchestrator amendments (binding; they override the text above where they differ)

1. **Base.** The chain starts from this branch with the pomodoro editor slice merged in (its 8
   looks, editor and shipping also edit `apps/pomodoro/`). Read `apps/pomodoro/` as you find it at
   the step's start, not as this design quoted it.
2. **No version bump in this slice.** One bump per push: the editor slice's ship step already bumps
   `apps/pomodoro/version.txt`, and this slice lands in the same push. alerts-3 does not touch
   `version.txt`; its §4.2 sentence "Step 3 bumps version to v1.7.1" and its spec's criterion 6/5
   are withdrawn.
3. **The backed breathe's floor is 96, not 64** (`ORB_RING_BACKED_MIN_ALPHA 96`, about 38%): at
   the trough the arc reads as dimmed, never as gone. §2.2's "nearly invisible at the trough" is
   withdrawn; the trough is the colour at 96/255 on black.
4. **A press that only wakes the glass is never lent.** When the display is dimmed or off and the
   OS spends the press on waking it, the press goes nowhere, as today; the next press is lent.
5. **Scope gates** diff against the branch each step starts from (`delegate/<previous step>`;
   `delegate/alerts-design` for step 1), written `test -z "$(git diff --name-only <branch> -- …)"`.
   A gate against `HEAD` is empty once the harness commits, and checks nothing.
6. **Step 0, the dots (orchestrator's design correction).** `alerts-0-dots.md` runs first: the
   dots row shows N cells for the owner's cycle (90 frames, one per (N, n)), breaks show the
   focuses done. Since this plan was written the pomodoro also gained §64.14 (a turn's rules,
   down to 0:00 and paused, a press at 0:00 starts the next phase), §64.15 (Reset at the ladder's
   end; the ring holder keeps its display art) and 16 settings; read the app as you find it.
