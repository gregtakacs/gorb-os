# ring-2: the ring's firmware glue — wasm import, cap, rim stamp, copy-wins, /status

## Goal

Make the ring real on the device: bind `orb_ring_set`/`orb_ring_clear` in the wasm shim behind a
new owner cap `ring`; grow the cap tables (the manifest, the Permissions sub-menu, the `/status`
ledger) to count it; stamp the ring into the DMA tiles as they leave (never the mirror) through
the same two `display.c` sites the copy arc uses, with **the copy arc winning while a copy runs**
and the ring returning after; add a `"ring"` block to `/status`. Slice **ring** of
`docs/specs/ring-design.md`, on top of ring-1's pure core (design §2, §3, §5, §8, §13). The
firmware files below can only be fully compiled by an ESP-IDF build (the orchestrator builds and
flashes); the sandbox checks the SDK/manifest/cap half.

## Read first

- `docs/specs/ring-design.md` §2 (the API + packing), §3 (module layout), §5 (record drop), §8
  (the rim dispatch + copy-wins), §13 (arbitration), §10 (gate: which files are IDF-only).
- `main/display/orb_edge_arc.c` / `.h` — the service (row marks + the off-glass corner
  invalidate + the 10 Hz throttle) and stamp to mirror; the exact ring-1 core is the geometry.
- `main/display/display.c` — the two stamp sites (`:969` tiled flush, `:2170` `fx_frame` after
  the shadow), and `display_mark_rows` (`:1968`).
- `main/main.c:63` (`orb_edge_arc_service()` on the LVGL task) — where the ring's service runs.
- `main/app/orb_app_wasm.c` — the `s_binds[]` table (`:1876`+), a struct-pointer import as a
  model (`imp_map_open` at `:1303`, `m3ApiGetArgMem`/`m3ApiCheckMem`), a cap gate (`MAP_GATE`/
  `NET_GATE` and `orb_grant_ok`), and `slot_release` (`:208`) where net/blob/map release.
- `main/app/orb_svc.h` (the `orb_cap_t` enum, `SVC` macro), `main/app/orb_app_manifest.c:603`
  (`orbapp_caps_names` NAMES[]), `main/menu/menu_view.c:282` (`OWNER_CAPS[]` + its
  `_Static_assert`), `main/net/orb_net.c:2567` (`OC[]` + assert) and the `/status` snprintf
  (`:2654`-`:2737`, the `glass.tiles` field and the trailing `"apps"` key).
- `apps/sdk/orb.h` (the `ORB_IMPORT` family + the ARGB convention near `:358`),
  `apps/README.md` (the caps table, "The ring").
- A prior firmware+SDK spec to imitate: `docs/specs/fscale-1-firmware-scale-round.md`.

## Contract

**Files that only an ESP-IDF build checks (state this in the step):** `main/display/orb_ring.c`
gains `orb_ring_disp_service()`/`orb_ring_disp_stamp()` (a thin glue section, `esp_timer`,
`display_mark_rows`, the LVGL corner invalidate — same shape as `orb_edge_arc_service`/`_stamp`);
`main/display/orb_edge_arc.c` gains `bool orb_edge_arc_shown(void)`; `main/display/display.c`
swaps the two `orb_edge_arc_stamp(...)` calls for the rim dispatch (design §8); `main/main.c:63`
adds `orb_ring_disp_service()` after the arc's service. All keep the core (`ring_*`) free of IDF
includes; only this glue file includes `esp_timer.h`/`display.h`.

- **`orb_ring_t` + the two imports (apps/sdk/orb.h).** Add exactly the struct and constants in
  design §2 (16 B, `_Static_assert(sizeof(orb_ring_t)==16)`), `ORB_RING_STEADY/_FLASH/_BREATHE`,
  and `ORB_IMPORT("ring_set") int32_t orb_ring_set(const orb_ring_t*)` /
  `ORB_IMPORT("ring_clear") int32_t orb_ring_clear(void)`. The struct field order MUST equal
  ring-1's `orb_ring_rec` (shared layout).
- **The shim (orb_app_wasm.c).** `imp_ring_set` (`i(i)`): `CALLER()`;
  `m3ApiGetArgMem(const uint8_t*, p); m3ApiCheckMem(p,16);`
  `if (!orb_grant_ok(s->app, CAP_RING)) { orb_ring_drop(slot_index(s)); m3ApiReturn(ESP_ERR_NOT_ALLOWED); }`
  then copy 16 B into an `orb_ring_rec`, `ring_set(...s_own..., slot_index(s), &rec, esp_timer_get_time())`,
  `m3ApiReturn(ESP_OK)`; a NULL pointer ⇒ `ESP_ERR_INVALID_ARG`. `imp_ring_clear` (`i()`):
  same gate, else `orb_ring_drop`/`ring_clear(slot)`, return `ESP_OK`. Add both rows to `s_binds[]`
  beside `sound_play`. The `s_own[ORB_APP_CAP]`/`s_owner`/`s_seq` tables are
  `EXT_RAM_BSS_ATTR` (design §7): define them in the glue section of `orb_ring.c`, initialised in
  the ring init. Add `orb_ring_drop(slot_index(s))` to `slot_release` (`:208`, next to
  `orb_map_release_app`).
- **The cap (`ring`).** `orb_svc.h`: `CAP_RING = 10`, and `CAP_N` becomes 11. Grow every
  owner-cap table by the appended name (append last, the enum note forbids re-order):
  `orb_app_manifest.c` NAMES[] `"…, \"location\", \"ring\""`; `menu_view.c` OWNER_CAPS[]
  `…, CAP_RING` (its `_Static_assert(N_OWNER_CAPS == CAP_N - CAP_POWER)` then holds at 7);
  `orb_net.c` OC[] `…, CAP_RING` (same assert). Refusal uses the existing `ESP_ERR_NOT_ALLOWED`
  value (`SVC` macro) — the code the other caps answer. In `orb_svc.c` the revoke path calls
  `orb_ring_drop` for the revoked slot (beside the `CAP_POWER` `idle_dim_app_release` hook).
- **Regenerate `apps/sdk/binds.json`** with `python3 apps/sdk/orbsdk.py binds --write`.
- **`/status` (orb_net.c).** In the status `snprintf`, add after the `glass` field (design §8):
  `"ring":{"owner":"%s","fraction":%d,"effect":"%s","on_glass":%s}` where `owner` is the owning
  app's slug (or `null` when unowned), `fraction` the latest fraction, `effect` one of
  `steady|flash|breathe`, and `on_glass` true only when a record is owned, the effect is on at
  this instant, and the tiled path is live (not the untiled fallback). `/fb` reads the mirror, so
  it still cannot see the ring (design §10, requirement 8). Expose `orb_ring_status()` from the
  glue returning these four (a `null`/off record when none) so the net code stays thin.
- **apps/README.md**: add a `ring` row to the caps table and to the "The ring" note; the declaration
  is the consent, the owner revokes it per app, `orb_ring_set/clear` are the calls.

## Behaviour and edge cases

## Behaviour and edge cases

- Undeclared or revoked `ring`: `orb_ring_set` and `orb_ring_clear` answer `ESP_ERR_NOT_ALLOWED`
  and drop that app's record (design §5); the row is marked refused on the dial (`*`), as the
  other caps are. An app that never declares it loads fine (the bind table links a name only when
  the module imports it).
- The copy arc wins: while `orb_edge_arc_shown()` (a copy is running, `s_shown >= 0`) the ring
  draws nothing and the arc keeps owning the rim; the ring's ownership record is untouched, and
  the first frame after the copy's `orb_edge_arc_set(-1)` stamps the ring again — the ring
  returns with no work by the app (design §8).
- `orb_ring_clear` when the caller owns the ring promotes the next most recent record (design §5);
  clearing with no record is a harmless no-op.
- On uninstall the app's record is dropped (`slot_release`), the next owner surfaces.
- `on_glass` is false on the untiled fallback (no DMA ring funded) — the ring is a tiles-path
  feature only, like the arc.
- A record set from `orb_service` while the app is off the glass still owns and shows (the app
  drew nothing; the OS stamps), matching the nothing-off-the-glass ruling.

## Acceptance criteria (sandbox half; firmware half noted)

Run all in the sandbox; the `idf.py build`/flash/bench steps are the orchestrator's (workers never
touch a device or serial).

1. `python3 apps/sdk/orbsdk.py binds` → `binds.json matches the firmware` (proves ring_set/ring_clear
   are in `s_binds[]` and the regenerated json agrees; a missing bind or a stale json fails).
2. `grep -q '"ring_set"' apps/sdk/binds.json && grep -q '"ring_clear"' apps/sdk/binds.json` → both match.
3. `grep -q 'CAP_RING' main/app/orb_svc.h && grep -q '"ring"' main/app/orb_app_manifest.c &&
   grep -q 'CAP_RING' main/menu/menu_view.c` → the cap is in the enum, the name table, and the
   Permissions list.
4. `python3 tools/host_menu_test.py --mutations` and `python3 tools/host_svc_test.py` and
   `python3 tools/host_orbapp_reader_test.py` and `python3 tools/host_orbapp_install_test.py` →
   all pass (the cap-count `CAP_N=11` and the 7-entry owner table are consistent: a wrong count
   breaks these gcc builds).
5. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → exit 0.
6. Firmware-only, orchestrator: `idf.py build` clean; on the orb a themed app under a live ring
   still animates, `/status` names `ring.owner`, `/fb` cannot see it, and a copy mid-flight shows
   the arc then the ring returns.

## Out of scope

The `color` setting type (ring-3). No pomodoro. Do not change the copy-arc driver
(`copy_quiet_begin`/`_end`) or the untiled fallback path. Do not put `esp_timer.h`/`display.h`
into the pure `ring_*` core (keep it host-compilable; the IDF glue is a separate section/file). Do
not touch `version.txt`/`os_min`.

## Notes

- The rim dispatch at `display.c` is the only change to that file's flush loop: two call sites,
  each an `if (orb_edge_arc_shown()) … else orb_ring_disp_stamp(...)`. Keep the arc's `if` first
  (copy wins by construction).
- The `_Static_assert`s in `menu_view.c` and `orb_net.c` are the safety net that the appended
  `CAP_RING` did not silently leave a table one short — leave them exactly as they read
  (`== CAP_N - CAP_POWER`, now 7).
- The status string is inside one big `snprintf`; add the `"ring"` object's format and its four
  arguments in matching order right after the `glass` field so the `%s`/`%d` alignment holds.
