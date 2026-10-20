# Slice 2 handoff: boot on base, with no flashed default

Transient working doc. Delete it when slice 2 lands. It exists because the previous
session's summary had three anchors wrong, and a plan built on those anchors sends the
next agent hunting for lines that do not exist. Every claim below was re-verified at
`5ec0643` on 2026-10-19. Re-check with `grep -n` before editing: line numbers move.

## Mission

Stop flashing the default theme bank, so a board with no installed theme boots the base
digital face instead of drawn-by-default art. Then prove the base face renders with a
pixel capture rather than a log line.

## Verified starting state

- HEAD `f5f8459` (this doc) on `main`, on top of `5ec0643`. Worktree clean. The device still
  runs the `5ec0643` build, since nothing here reflashes it.
- orb-A at `192.168.2.32` runs `5.0.0+15.g5e8655e` and boots the themed path. `/api/v1/sd`
  reports `source: slot-B, active: av-kit, live: card`.
- The pixel path works both doors. `python3 tools/fbgrab.py --min-ink 5` passes; the dial
  puts ink at 97.48 percent and mean luma at 54.
- `sdkconfig.defaults` is clean: `ORB_BENCH_*` set only in `sdkconfig.defaults.bench-*`,
  `ORB_BASE_RENDER_FORCE=n`, `ORB_DEBUG_HTTP=y`.
- The base face has still never produced a visible frame. That is what this slice fixes.

## Three corrections to the incoming summary

1. **The flash rule is in the top-level `CMakeLists.txt:62-107`, not `main/CMakeLists.txt`.**
   `main/CMakeLists.txt` mentions `clock_assets.bin` only in comments (38, 97, 107). The
   rule calls `esptool_py_flash_target_image(flash "${_art_partition}" ...)` at line 101,
   inside `if(EXISTS "${_art_image}")`. An `else()` at 104 already prints
   `assets/clock_assets.bin absent` and continues. Removing the asset therefore needs no
   new build logic: deleting the file takes that branch by itself.
2. **`host_art_residency_test.py` has no D13 block and no `use_baked_in()` stub.** It pins
   one string, `"esp_err_t art_bank_map_at_0(size_t need_bytes) { return art_bank_map(0,
   need_bytes); }"`, at line 95. Keep `art_bank_map_at_0()` in `art_bank.c` and that test
   needs no edit. Deleting it is optional and costs a test edit for no behaviour.
3. **There is no null-art crash to guard against, and the planned guard is in the wrong
   place.** The hazard is an early `return`, not a wild read. See the next section.

## The real blocker: the art gate runs before the base render

Inside `clock_view_boot()` the order is:

```
1774  const esp_err_t art_err = art_bank_ready() && art_bank_bytes() >= ...
1783      return art_err;                      <-- bail
1785  s_art = art_bank_ptr() + blob_off;       <-- NULL + offset
1786  orb_bg_init(s_art);
1790  s_scr = lv_obj_create(NULL);
1811  if (ORB_BASE_RENDER_FORCE || !orb_theme_geo()->from_container)  <-- base face
1824      return ESP_OK;
1845  orb_bg_classic_init();                   <-- themed only
```

The gate at 1774 demands a resident bank before the base branch at 1811 gets a chance. So
the moment the flashed default is gone, a card-less board fails the gate, returns
`ESP_ERR_INVALID_SIZE`, and never reaches `clock_base_show()`. The glass stays at the
panel's reset black. Line 1785 also computes a wild pointer and hands it to `orb_bg_init()`.

The fix is a reorder, not a guard: decide residency first, then demand art only on the
themed leg. The gate's own comment at 1767-1774 already names this failure ("a rule written
while a default always existed turns into a blank screen on a shelf"), and the code does the
thing the comment warns about.

`orb_bg_classic_init()` at 1845 sits behind the themed path's own gate and is unreachable
from the base face. Do not add a check there. It protects nothing.

## Decide first, then write code

**Slice 1 already half-made the open decision.** `use_baked_in()` sets
`from_container = false` (`orb_theme.c:764`). The branch at 1811 fires on `!from_container`.
So on a card-less board *today*, with the default still flashed and mapped, the base face
wins and the flashed dial art never draws. The default's pixels are already dead weight on
that board. orb-A is unaffected only because it runs a slot-B container.

That makes `6e1c6c9` wrong to describe itself as "shipping behaviour unchanged", and it makes
§26:3951's "the flashed default is still preferred" incomplete. Two ways out:

- **(a) Accept it.** The base face is the card-less state, the flashed default is unused, and
  slice 2 only stops paying for it. This matches the owner's digital choice.
- **(b) Restore preference.** Then the branch at 1811 keys on residency plus a real "no art"
  test rather than `!from_container`, and the default keeps its flash line.

Ask the owner which, in one question, before touching code. Option (a) is smaller and the one
the plan implies. Do not discover this in the pixels after the flash line is gone.

Still open per §26:3998-4002: what the glass shows on a shelf before any theme arrives.
Precedent says reuse the unconfigured-network on-screen ask.

## Process

**Step 0. Baseline capture, while the dial is still up.** Run
`tools/fbgrab.py --out /tmp/pre-slice2.bmp`. Diff against it at the end.

**Step 1. Commit the reorder alone.** Move the residency decision above the art gate in
`clock_view_boot()`. Touch nothing else in this commit. On orb-A nothing changes: it still
resolves a container, still passes the gate, still draws the dial. Confirm with
`fbgrab.py --diff /tmp/pre-slice2.bmp /tmp/step1.bmp --max-diff 5`. A true zero is impossible,
since the digits and the hand move between grabs. Expect a small non-zero figure sitting in
the digits rather than the plate.

**Step 2. Stop flashing the default.** Delete `assets/clock_assets.bin`. Take out the flash
rule at `CMakeLists.txt:62-107`, or leave it and let its existing `else()` branch fire. Prefer
deleting the rule: a build line that exists only to warn about a file that is never there is
a lie waiting to rot. Then delete `use_baked_in()` (`orb_theme.c:744-765`) and its two calls
(2069, 2209), and replace the default leg of `orb_theme_boot()` with a plain no-art state.
Keep `boot_resolve()`, the D13 table, `install_target()` and the `SLOT_A_HDR`/`SLOT_B_HDR`
defines: the install discipline survives the default. Keep `art_bank_map_at_0()` unless you
want the test edit at `host_art_residency_test.py:95`.

**Step 3. Delete the override.** Remove `ORB_BASE_RENDER_FORCE` from `main/Kconfig:277-291`
and the `#if`/`#define` pair plus branch term at `clock_view.c:1793-1801`. The branch becomes
the real condition. A leftover `=y` in someone's `sdkconfig` would otherwise force the base
face over a perfectly good theme.


**Step 4. Fix the prose that promises a default.** Sentences to retarget: `orb_theme.c:723`,
`:1053`, `:2145`, `:2148`, `:2181`, `:2184`, `:2200`, `:2207`; `orb_assets.h:28`;
`orb_theme.h:23`; `main.c:76-78` ("or to the baked-in one"); `orb_net.c:5340` and `:5346`,
which are user-visible `confirm()` strings promising the factory clock comes back; and
`host_tz_test.py:497-501`. Then rewrite the D13 rows at `host_tz_test.py:719-742`: rows
expecting `BOOT_ART_DEFAULT` need a new meaning, since default now means no art rather than
the flashed bank.

**Step 5. Partition table in the flash path.** §26:3986 also wants the two-slot layout
learned at flash time. Check whether the table already ships in `build/flash_args` before
writing anything. It may already be there, making this a sentence rather than work.

**Step 6. Run the battery with mutations.** `.clinerules:246` documents the convention: run
the `tools/host_*_test.py` files directly, no runner. The four that this slice must keep green
are `host_tz_test.py` (holds the D13 table, the 34-route lint and the landing-page linter),
`host_art_residency_test.py` (pins the `art_bank_map_at_0` source line), `host_sd_test.py` and
`host_mdns_test.py`. Run each with `--mutations` at least once: the previous session missed a
stale anchor precisely because mutation passes are opt-in per file. Treat a red anchor as a
spec to fix, not a failure.

**Step 7. Flash and capture the base face.** Flash a card-less board, or clear slot B's
liveness on orb-A. Open the menu first and grab it as the low-ink reference, then grab the
base face:

```
python3 tools/fbgrab.py --out /tmp/menu.bmp --min-ink 1
python3 tools/fbgrab.py --out /tmp/base.bmp --min-ink 1
python3 tools/fbgrab.py --diff /tmp/base.bmp /tmp/menu.bmp
```

Expect low ink, in the 1-3 percent band the menu sits at, and a diff against the dial near 97
percent. Both figures together prove the base face drew and that the frame is not stale. Read
the `X-Orb-FB` `commit=` word to tell a fresh frame from a stale one.

## Hazards already paid for

- **Aborting a grab mid-transfer stalls the whole device for 1-2 minutes.** The HTTP server is
  a single task, and it sits in the socket timeout until the device recovers. Do not
  ctrl-C a grab. The failure looks like a dead orb.
- **`?direct=1` switches rendering, which defeats its own purpose on a repainting screen.** It
  arms a one-shot LVGL task handler, so it can only capture inside a commit window. On the
  clock the repaint lands 8 ms too early and returns a pre-switch frame. On the static menu
  and the base face it is accurate and halves the cost. Prefer the default path.
- **A capture costs the carrying frame 19.5 ms.** Never run it from a draw callback or an
  `lv_timer`; it would deadlock on the handler it waits behind.
- **The bench decoder and PIL are not a display.** `fbgrab.py` proves geometry, ink and
  luma. Colour fidelity, burn-in, and touch latency still need a person.
- Do not delete the `assets` partition from the table. Old firmware and slot A still refer
  to it, and D13's install discipline depends on that offset existing.

## Optional hardening, worth one commit of its own

- Pin the 565-to-8-bit expansion in a host test. The device packs 16-bit and the host
  expands it, so a test can only cover the host half. That half carried the luma bug and
  shipped at a third of its true value until a cross-format comparison caught it.
- Add a repo-level battery runner that passes `--mutations` by default. There is none today,
  so every test's mutation pass depends on someone remembering the flag, and `.clinerules:246`
  only names three of the twenty-three host tests that exist.
