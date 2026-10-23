# Two migrations: single-slot theme, clock through the compositor

Written 2026-10-21. Supersedes the A/B ping-pong clause in `orb_theme.h` and the
"clock routes its own layers" exception in `orb_layers.h`. Both migrations share
one premise: the owner already accepts a themeless boot (D31), so crash safety
does not need to cost half the partition.

---

## Migration 1: Eliminate A/B -- one 8 MB theme slot

### What changes and why

The `assets` partition is 8 MB. Today it is two 4 MB slots that alternate so a
torn store never degrades the glass. D13 retired that premise on 2026-10-19:
a fresh flash has no theme and the dial renders from OS primitives. D31 makes
the base face a designed state, not a degraded one. The owner accepts that a
cut mid-store costs a re-press.

One 8 MB slot doubles the per-bundle ceiling. D29's 4,128,768 B art limit becomes
~8 MB. One theme can carry art for all 8 installed apps simultaneously. The A/B
machinery becomes dead code.

### Partition layout (before and after)

Before: slot A header @ 0x000000, art @ 0x00010000 (4 MB cap).
       Slot B header @ 0x00400000, art @ 0x00410000 (4 MB cap).

After:  One slot, header @ 0x000000, art @ 0x00010000 (8 MB cap).

### New store sequence (replaces ping-pong)

1. Owner presses store/flip. The flip page (P6.4, already landed) takes the
   glass and paints a themed loading UI (five primitives, one progress bar).
2. `orb_theme_install_bundle(id, why)` opens the SD file, validates the header,
   erases slot at offset 0, streams payload into slot, reads back, CRCs,
   writes the header LAST. Same verify-then-header-last discipline as D9.
3. On success: reboot. On power cut: header unwritten, slot invalid, next
   boot sees no drawable bundle, `use_no_art()`, base digital face.

The loading page IS the A/B slot: it protects the live pixels because there are
no live pixels to protect. The compositor is idle or painting a progress bar.

### Boot recovery (replaces `boot_resolve`)

```
orb_theme_boot():
  read header at 0x000000
  if valid bundle: adopt, from_container = true
  else:
    if NVS `theme_staged` names a theme id:
      check SD for /sd/themes/<id>.orbtheme
      if present: re-queue the store (the file is still there)
      if absent:  clear the staged word
    use_no_art()
```

One boot attempt, then the base face. No pending-operation counter. D11
already killed the stage-then-ask flow. If the owner wants a theme, they
press store again. The `theme_staged` word is a witness, not a queue.

### What gets deleted

| Symbol / mechanism | Where | Why it was there |
|---|---|---|
| `SLOT_B_HDR`, `SLOT_B_ART`, `SLOT_A_HDR`, `SLOT_A_ART` | `orb_theme.c:98-103` | Two-slot addressing |
| `boot_resolve()`, `install_target()`, `s_art`, `boot_art_t` | `orb_theme.c:369-416` | Write-the-non-live-slot logic |
| `THEME_LIVE_SLOT_A`, `THEME_LIVE_SLOT_B` strings | `orb_theme.h:144-145` | Slot designator vocabulary |
| `THEME_NVS_LIVE` (all three values) | `orb_theme.c:57`, `.h:143` | "Which slot to draw" |
| `s_damage_why`, `s_damage_id` pair | `orb_theme.c:286+` | "One slot rotted, the other saved you" |
| The ping-pong comments at `orb_theme.h:16-49` | header doc block | Explained the alternation |
| `host_tz_test.py`'s `boot_resolve` vectors | test suite | Truth-tabled the two-slot decision |

`THEME_NVS_STAGED` survives as a diagnostic witness.
`theme_live` NVS key survives but only answers `"slot"` or `""`. The flip door
becomes: store, reboot. No "find which slot holds this id" search.

### What survives unchanged

- The FORMAT 3 bundle header at offset 0 (magic, version, CRC, section index).
- The streaming ingest (`orb_theme_install_bundle`'s chunk read + CRC path).
- `use_no_art()` and the base digital face.
- The `/sd/themes/<id>.orbtheme` naming and `theme_id()` hashing.
- Header-last write order (D9).

### Consequences for the writer side

`orbbundle.py:check_budget` and the browser's `packBlob3` budget gate reference
`ORB_THEME_SLOT_CAP`. They update to the 8 MB ceiling in the same commit. The
D29 budget sentence changes from "4,128,768 B" to "8,323,072 B" (8 MB less the
64 KB header+geometry block).

### Why this is the right tradeoff

The A/B slot cost 50% of theme storage to protect against one scenario: power
cut mid-store with the SD card removed. In practice, a store takes 4-5 seconds
(§24.8 measured 4,398 ms at av-kit size). The odds of a power cut in that window
are vanishingly small, and the recovery (insert card, press store) is one action
on a page the owner was already looking at. The base face tells the time.

The gain is concrete: a single theme bundle fits art for all 8 app slots at the
full flagship ceiling (~1 MB each). That removes the "my theme covers 3 of my
apps, the rest show the base face" problem that D29's 4 MB ceiling creates.

---

## Migration 2: Clock through the compositor, not its own renderer

### Confirmation that the machinery is already landed

The clock does NOT need new compositor code. Every piece it needs is in the tree
today and tested:

| Piece | Landed at | Location |
|---|---|---|
| Pixel path (`gfx_blend_px`, `gfx_paint_row_ctx`, `gfx_commit`) | P1 | `main/gfx/orb_gfx.c` |
| Layer table (`orb_layers_jobs` converts layer rows into `gfx_commit`'s job list) | P5.5 | `main/gfx/orb_layers.c` |
| Broker gating (`svc_layer_add/set/touch/del`, CAP_UI, live/refused counters) | P5.5 | `main/app/orb_svc.c:523-562` |
| Frame scheduler (`orb_sched_l4_active`, row-mark gate, flush pass) | P5 | `main/gfx/orb_sched.c` |
| Asset resolver (`<slug>/<name>` to `orb_asset_t`) | P5b | `main/assets/orb_assets.c` |
| FORMAT 3 bundle reader (`orb_bundle_find`, element table, sprite catalogue) | P6 | `main/art/orb_bundle.c` |
| Element table to resolved-asset pairs (`orb_elems_at`) | P6 | `main/assets/orb_elems.c` |

`orb_layers.h:25-34` records the exception this migration closes: "the clock
does not route through here... it still drives `clock_layer_commit` directly."
Section 32 (2026-10-21) says why the commit hook has no caller: "no built-in
app uses the layer table, the first caller is the wasm tier." This migration
makes the clock that first caller.

### What the clock becomes

Today: `clock_layer_commit()` (lines 1048-1130) builds a `gfx_paint_ctx_t[]`
array, calls `gfx_paint_ctx_init()` per layer, hands the list to `gfx_commit()`.
The app owns the entire render path.

After: `clock_tick()` computes angles and calls `layer_set(handle, &desc)` per
moving element. The scheduler's commit hook calls `orb_layers_jobs()` on the
app's table and hands the result to `gfx_commit()`. The app never touches a
paint context, sprite pointer, or `gfx_mark_rows` call.

### Responsibility split

App (per tick):
- Compute angles (`hand_angles`, `comp_value`, tourbillon pose). Clock policy.
- Call `layer_set(handle, new_angle, new_x, new_y)` per element that moved.
- Call `layer_touch(handle, damage_rect)` for partial repaint.

OS compositor (per frame):
- Walk the active app's layer table in z-order via `orb_layers_jobs()`.
- Convert each row to a paint job (bake, blend, per-pixel loop at 185 ns/px).
- Row-mark, flush-gate, DMA-commit.
- Frame-rate scheduling (already in `orb_sched.c`).

### Changes to `clock_view.c`

| Current | After |
|---|---|
| `clock_layer_commit()` (~80 lines) | Deleted. The scheduler's hook replaces it. |
| `mark_all_layers()` (50 lines) | `layer_touch(app, h, NULL)` per handle at bounce. |
| `comp_add_jobs()`, `aux_add_jobs()` (~40 lines each) | `layer_add` at init, `layer_set` at tick. |
| `blob()` accessor: mmap art base | Gone. `orb_asset_t.bank_ptr` carries the resolved pointer. |
| `s_ang_10[5]`, `s_hand[5]`, `s_shadow[5]` | Feed `layer_set`. Array positions become layer handles. |
| `clk_theme.order[]` z-stack walk | Init-time only: sets initial `layer_add` order. |

The file shrinks by approximately 300 lines. What remains is policy: angle
computation, GIF frame selection, cadence, and the init that resolves elements
and builds the table.

### The commit hook

Two new public symbols land in the same commit:

- `orb_svc_layer_table(const orb_app_t *)` in `orb_svc.c`/`.h` — a getter
  returning the app's `orb_layer_tab_t *`, beside the existing
  `orb_svc_layers_live` / `orb_svc_layers_refused` counters. The broker already
  owns one table per registry slot; this exposes a read pointer to the
  scheduler. No such getter exists today — the commit adds it.
- `ORB_LAYER_JOB_MAX` in `orb_layers.h`. The table caps at `ORB_LAYER_MAX` (8
  rows at writing time — `orb_layers.h:83`), and `orb_layers_jobs()` emits at most one job per
  row (`orb_layers.c:313-327`: a row is skipped or emitted, never expanded).
  The honest ceiling is therefore `ORB_LAYER_MAX`. (Commit 3 raised the table to
  16 — the shipped analog bake names nine composite rows; the owner's
  decision, recorded in the commit body.) The clock's file-private
  `CLK_JOB_MAX` (`clock_view.c:869` = 12 + 32 + aux) is larger only because
  the old path packs shadow pairs and complication blits as separate jobs;
  that count belongs to the deleted renderer, not the table.
  `#define ORB_LAYER_JOB_MAX ORB_LAYER_MAX`.

One new function in the scheduler's flush path (`main/gfx/orb_sched.c`), called
after the active app's `tick()` and before DMA commit:

```c
void orb_sched_commit_layers(const orb_app_t *active)
{
    orb_layer_tab_t *t = orb_svc_layer_table(active);
    if (!t || orb_layers_count(t) == 0) return;
    gfx_paint_ctx_t jc[ORB_LAYER_JOB_MAX];
    int nj = orb_layers_jobs(t, art_bank_ptr(), jc, ORB_LAYER_JOB_MAX);
    if (nj) gfx_commit(jc, nj);
}
```

`theme_base` is NOT NULL, and `orb_theme_bank_ptr()` does not exist. The real
base is `art_bank_ptr()` (`art_bank.h:232`): the mapped live payload, NULL
before init. `orb_layers.c:319` resolves each row as
`t->base[i] ? t->base[i] : theme_base`, and a base-less row draws nothing
(`:320-323`). Baked rows carry their own `base[i]`; a tier-1 theme row answers
against the payload it was resolved against — the same mapping
`orb_asset_resolve` computes `art.off` into (`orb_assets.c:407`: "the slot is
an OFFSET into the active theme's art payload"). The clock's current `blob()`
value is `art_bank_ptr() + orb_theme_geo()->blob_off` (`clock_view.c:2001`).
The `+ blob_off` step must survive somewhere: either `t->base[i]` carries the
blob base per row at `layer_add` (the clock knows it), or the resolver already
bakes it in for container themes (`orb_theme.c:1153` rewrites `off` into the
container convention at adopt). Prove which one with a host vector test BEFORE
deleting `blob()` in commit 3; if the conventions disagree, the fix is at the
resolver, not a `+ blob_off` hack inside the hook.

This is the function `orb_layers.h:27` says has no caller.

### Landed status — Migration 2, Commit 4, 2026-10-22

Two claims above did not survive the tree, and the plan's own stop rules say
a commit reports its deviations rather than rewrites its plan:

- **`blob()` did not go, and not for want of the proof.** The host vectors
  (tools/host_orblayers_test.py section 14) proved the conventions agree:
  the fold lands as `off = bank_off + e->off` at the clock's own resolve step,
  `orb_elems.c` hands the catalogue offset back verbatim, and the shipped
  commit pass reads `art_bank_ptr()` with no `+ blob_off`. The deletion still
  cannot land because `blob()` has three live callers OUTSIDE the L4 render —
  `make_layer()` and `comp_slot_show()` fill the classic path's LVGL
  descriptors with it, and `tourb_chords()` reads through it — and the classic
  path is not dead: a theme with no GIF bank has no canvas, no L4, and no
  layer-table commit, and draws those widgets. `orb_layers.h` now records the
  corrected ownership: P9's in-image bundles retire the fallback, and the
  accessor goes with it.
- **The hook lands at the display flush, not after the tick.** The plan's
  "after the active app's tick" call site (Commit 2's interim) cannot promise
  a full-frame weld for a repaint the tick did not cause — a bounce, a menu
  return — so Commit 4 moved the weld to the flush the retired
  `clock_layer_commit` always had: `orb_sched.c`'s `sched_flush_commit()`,
  registered at `orb_sched_start()`, gated on the glass the way that hook's
  load-bearing comment demanded. It composites the ACTIVE app's table at the
  point no DMA has read a marked row. `ORB_LAYER_JOB_MAX`, the table at 16,
  and the `orb_svc_layer_table()` getter all landed as written.
- **`mark_all_layers()` did not become bounce-time `layer_touch`es.** The
  shipped marking discipline was already mark-only-what-moved
  (`CLOCK_L4_MARK_STATIC_BOXES=0`, docs/rows-capacity-2026-10-10.md); the
  blanket pass early-returned, so Commit 4 deleted it and the
  `orb_sched_policy_t` slot beside it. Wake damage — the one box no mark site
  can mark, a row going hidden-to-visible — is exactly one `layer_touch` per
  waking row, issued by the tick's pose pass.

What DID land as planned: `clock_layer_commit`, `comp_add_jobs`, `aux_add_jobs`
and the job-list defines are gone (clock_view.c shed its ~300 lines: −18.6 KB
of text); the tick poses the hands and `layer_set`s the moved rows; the table
is the only L4 commit path; the gfx-parity harness was re-seamed, not relaxed
(`tools/gfx_parity_test.py` now documents the retired mark seam and pins the
flush-hook tick body against the same 3116942 reference, identical).

### Bench record — Commit 4, orb-B, 2026-10-22

Board state first: the P9 built-in-bundle default-first rule moved the live
slot from `av-kit` to the factory `Modern` bake when the re-stamped firmware
booted — so the bench covers BOTH live paths, deliberately:

- **Classic fallback (Modern, plate-anim, no canvas).** fw `v1.1.0+49b0a95`,
  `layer table: 9 rows, 0 refused`, min-ink 70.63% (pass 1.5%), two grabs
  60 s apart diff 2.99% — hands advance, the no-L4 dial is untouched by the
  migration.
- **L4 table path (av-kit re-stored through the store door, GIF delta
  bank).** Boot log `layer table: 9 rows, 0 hidden, 0 folded, 0 refused
  (base 0)` beside `bg gif delta bank: 15 frames @ 67 ms (14.9 fps)`.
  Status ledger `layers: [9, 0]`, `fw v1.1.0+49b0a95`. min-ink passes on
  both grabs; the 60 s pair diff (4.02%) is hands + GIF phase.
- **A/B against the commit-3 image** (`serflash.sh --rollback`, stamp
  `v1.1.0+325f567`, same av-kit theme): both firmwares measure
  `clock 12 fps | flush frames 12 avg 77 ms`, phases
  `te ~113-161 dma ~88-105 hook ~658-690 (paint ~580-615)`, and
  `blend/s by layer hr ~63.5k min ~124k sec ~104.8k glass ~596k aux ~47.6k`
  on c4 and hr ~63.5k min ~124k sec ~104.7k glass ~596k aux ~47.6k on the
  rollback. An earlier c4-vs-c3 pass read 15 fps on c3 and 12 on c4; the
  re-measure with both images at the same board moment put c3 at 12 too —
  the drift was the bench environment, not the commit. The frame-pair
  cross-diff (6.1%) sits at the same magnitude as the within-firmware pair
  diffs (3.4-4.0%) plus the minutes the boots were apart, and the ASCII
  renders show hands, sub-dials and glass present on both. The board was
  re-flashed to the commit image after the comparison (`AUDIT OK`).

On the live bake the table commits the nine rows the projection names: plate
over canvas, three hands with their three shadows, two pointer rows, glass.
The window (BANK) pose path is host-vector-covered (section 14) and had no
live GIF-theme window to light — the next GIF bake with a complication runs
it on glass.

### ORB_BEHAVIOUR_CLOCK stays, internals change

Kind 3 remains (C6 append-only). The compiled clock boots before any wasm engine
and owns the boot screen. What changes is the module body: `clock_tick()` stops
drawing and starts calling `layer_set`. `clock_activate()` stops building LVGL
widget trees and starts calling `layer_add`. The iface hooks stay; their bodies
shrink to policy + table updates.

A wasm clock in the future names kind 2 and calls the same `layer_set` through
the shim's import table. Same architecture, different ABI.

### What the wasm path needs (future, not this migration)

`orb_app_wasm.c:188` has one import bound: `orb.now`. A wasm clock needs
`orb.layer_add`, `orb.layer_set`, `orb.layer_touch`, `orb.layer_del` appended
to `s_binds[]`. Each import resolves the calling slot, checks CAP_UI through
the broker, and forwards to the same `orb_svc_t` entry. Mechanical once the
table has a proven consumer (the clock).

### Sequencing (four commits)

**Commit 1: single slot, writers, and tests** (Migration 1, standalone). The
`assets` row in `partitions.csv` is ALREADY one 8 MB row (0x420000, 0x800000):
the A/B slots are two 4 MB regions INSIDE it, so the csv does not change. In
`orb_theme.c`: delete the `SLOT_B_*` addressing, `boot_resolve`,
`install_target`, and the live-slot NVS machinery; `SLOT_CAP` becomes 0x00800000
and `ORB_THEME_SLOT_CAP` 0x007F0000. Update every budget literal found by
`grep -rn "4128768|4,128,768|0x003F0000|0x3F0000|ORB_THEME_SLOT_CAP" main/
tools/` — that list includes `orb_bundle.h:61`, `orb_bundle.c:183-184`,
`clk_types.h:42`, `orb_theme.h:310`, `main/Kconfig:263`, `orbbundle.py:55+`,
`bench_rung_table.py:39`, `host_sd_test.py:298`, `mk_bundle_fix.py:140`, and
the browser baker's `SLOT_CAP` (line 100) and `SLOT_CAP3/SLOT_ART_CAP3` (line
1677). `host_tz_test.py` deletes `boot_resolve` vectors, gains single-slot
store vectors. D29's budget sentence: "4,128,768 B" becomes "8,323,072 B".
`orb_theme.h`'s header comment rewrites. No partition-table re-push is needed
(the row is unchanged); a board carrying old slot-B-only data boots themeless
once and re-stores.

**Commit 2: the commit hook and its getter.** Add
`orb_svc_layer_table(const orb_app_t *)` to `orb_svc.c`/`.h`, add
`ORB_LAYER_JOB_MAX` to `orb_layers.h`, add `orb_sched_commit_layers()` in
`orb_sched.c` and call it from the flush path after the active app's `tick()`
returns (see the hook section above for the exact body and base). The clock
still uses its own `clock_layer_commit` in this commit; the hook is a no-op
because the clock's layer table is empty. Sentinel: build a table manually,
call the hook, assert the expected `gfx_commit` call count.

**Commit 3: clock init migrates.** The clock's row is a registry row like any
other, so `clock_view_init()` resolves its elements the way every other app
will: open the live bundle section, `orb_elems_at(sec, cls, slot, ...)` per
element (currently zero callers in firmware — this is the wiring), then
`orb_asset_resolve()` per answer, then `layer_add` per resolved desc in the
bake's z-order. It does NOT read the legacy `orb_theme_geo()` adapter: that
adapter is the migration's destination, not its source. New `s_handle[]`
array replaces `s_hand[]`/`s_shadow[]` pointers. `ORB_LAYER_MAX` was 8
(`orb_layers.h:83`): the shipped analog bake on the bench board turned
out to name nine composite rows (ten clock-section elements; the plate is the
canvas, not a row) — so the constant is 16 as of this commit, the owner's
decision, and any bake that overruns even that refuses at add with the refusal
counted, never truncated. (Commit 3 also lands the rows `show=0`: the legacy
commit hook still owns the mark set until Commit 4 deletes it, and two paths
cannot share one. The ledger — live rows and refusals — is the pass line this
commit; the pixels move in the next.) No caps change: the clock keeps `sd_read|themes`, and `CAP_UI` is a
FREE cap — `orb_grant_ok` passes free caps unconditionally
(`orb_svc.c:175-177`), and free caps are undeclarable by the writer's own rule
(`orbapp.py:144`). So `tools/make_factory_apps.py` and the generated
`orb_app_factory_blobs.c` do NOT move in this commit. Pass line:
`orb_svc_layers_live(clock_row)` reports the bake's row count after boot. The
old commit path still draws.

**Commit 4: clock tick migrates, old path deleted.** `clock_tick()` calls
`layer_set` per moved hand. Delete `clock_layer_commit`, `comp_add_jobs`,
`aux_add_jobs`, `blob()`, and the `display_set_commit_hook(clock_layer_commit)`
registration (`clock_view.c:2136`) — the scheduler hook is the only commit
path. Pass line: same 12 fps sweep, same 0.22 ms/row, same 185-232 ns/px.
Tourbillon, sub-dials, and carriage render identically on the bench.

### What this does NOT do

- Replace the compositor. `gfx_commit`, `gfx_paint_row_ctx`, `gfx_blend_px`,
  `gfx_mark_rows`, the flush gate, and DMA stay byte-identical.
- Move the clock to wasm. Kind 3 stays compiled (boots before any engine).
- Change the theme format. FORMAT 3 already carries per-app element tables.
- Add new broker entries. `layer_add/set/touch/del` are already wired and gated.

---

## How the two migrations interact

Single-slot gives the clock art for every app in one bundle. The layer-table
path gives every app one rendering mechanism. The two changes unlock each other.

One theme covering 8 apps pays off only when all 8 can render from it through
the same compositor. Today only the clock can (it reads the bundle directly via
`orb_bundle_find`). The layer table is the shared path every other app takes,
and the clock's migration gives that table its first consumer.

Once both land:
- Store one bundle, every app themes, full 8 MB available.
- Apps call `layer_set`, the OS paints at the compositor's native speed.
- Power cut mid-store: base face tells time, one press to recover.
- No A/B machinery, no slot-designator NVS key, no damage-reporting pair.
- The wasm clock (and every third-party app) uses the same table the clock now
  populates. The import bindings are mechanical additions to `s_binds[]`.

---

## Hardware bench (each commit must clear this, not just the build)

The bound board is `.orb-target` at the clone root: orb-B, MAC
`44:bd:8d:78:26:18`, IP `192.168.2.34` (tools re-prove the address against the
MAC via ip-neigh). Every flash goes through `tools/serflash.sh`, which gates on
the MAC, takes a per-orb flock, and refuses an image whose embedded stamp does
not equal `tools/orb_version.sh` — so commit, then `idf.py reconfigure &&
idf.py build`, then flash. Serial flash overruns the 30 s tool cap: launch it
detached (`setsid bash -c 'tools/serflash.sh' </dev/null
>/tmp/serflash-run.log 2>&1 & disown`) and poll `/tmp/serflash-orb-B.log` for
`SERFLASH_DONE`. Boot logs come from `python3 tools/capf.py 15
/tmp/boot.log` — opening the port resets the board, which is the point. Pixels
come from `python3 tools/fbgrab.py --out /tmp/dial-<step>.bmp`; parity comes
from `python3 tools/fbgrab.py --diff /tmp/dial-before.bmp
/tmp/dial-after.bmp` (percent pixels changed, mean/max luma delta).

Transfer feedback comes from `python3 tools/orb_watch.py`, and it reads HTTP only
— never the port. Opening the bound ttyACM asserts DTR/RTS and resets the S3, so a
watcher that grabbed the serial to be helpful would cause the failure it reports.
It prints one line per second (installed art, card transfer state, free space,
heap), names the holder of the port from `/proc` so a competing hub or `capf.py` is
visible rather than mysterious, and shouts when the board reboots underneath a
transfer, naming the reset reason from the boot that ended the previous one. On a
WSL bench it also scans `dmesg` for the usbip `ECONNRESET` signature: a dropped
serial tunnel pulses the chip into reset, the upload dies silently, and the card
shows no `.part` and no refusal row. That silence is the gap this tool closes.

Per-commit pass lines:

- **Commit 1 (single slot):** boot log shows the theme adopting from the one
  slot (`source:` line in `/api/v1/status`), no damage report, no slot-B read.
  Torn-store bench: `curl --data-binary @big.orbtheme
  http://192.168.2.34/api/v1/sd/theme` interrupted mid-upload (kill the curl),
  reboot via capf's port-open reset, boot log must end at the base face with
  `source: none`; re-store the same file to completion, expect themed boot.
- **Commit 2 (hook):** the clock frame is pixel-identical to the pre-hook grab
  (`fbgrab --diff` reports 0.0% changed at the same second modulo the ticking
  hand: diff two same-theme grabs across a full minute and compare the delta to
  the same baseline pair taken before the commit).
- **Commit 3 (init migrates):** `/api/v1/status` (or the app row on the owner
  page) reports the clock's layer count = the bake's row count, refused = 0.
  Frame still pixel-current.
- **Commit 4 (tick migrates, old path deleted):** same diff-parity method as
  Commit 2, plus the fps line in the serial log unchanged (~12 fps sweep,
  ~0.22 ms/row), and a minute of hands moving on camera-free proof: two grabs
  60 s apart show the second hand advanced, `fbgrab --min-ink 1.5` passes on
  both.




