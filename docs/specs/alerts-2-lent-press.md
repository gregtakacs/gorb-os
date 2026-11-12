# alerts-2: the OS lends a plain press

## Goal

Deliver `docs/app-platform-plan.md` §64.13's OS surface for lent presses without touching the
pomodoro. Add a `press` capability and `orb_press_claim`/`orb_press_lent` imports, a pure claim and
queue state machine, a router path that diverts only plain presses away from a non-claimant glass app,
a lifecycle drop, and a ring confirmation pulse. Also raise the manifest setting cap to 18 so step 3
can pack the new pomodoro setting.

## Read first

- `docs/specs/alerts-design.md` §3, §5, §6.
- `main/app/orb_svc.h`, `orb_svc.c`, `orb_app_manifest.h`, `orb_app_manifest.c`, `orb_app.h`,
  `orb_app_wasm.c`, `orb_app_install.c`, `orb_setui.h`, `orb_setui.c`.
- `main/input/input_router.c`, `input_router.h`.
- `main/display/orb_ring.h`, `orb_ring.c`.
- `apps/sdk/orb.h`, `apps/sdk/binds.json`, `apps/sdk/orbsdk.py`, `apps/README.md`.
- `main/menu/menu_view.c`, `main/net/orb_net.c`, `tools/orbapp.py`.
- `tools/host_press_test.py` (new, modeled on `host_ring_test.py` and `host_svc_test.py`).

## Contract

1. **Capability** `CAP_PRESS = 11`, `CAP_N = 12`, name table entry `"press"` in `main/app/orb_svc.h`
   and `.c`. Update all visible cap lists and permission surfaces: manifest parser/refusal, menu,
   network ledger, `tools/orbapp.py`, `apps/sdk/orbsdk.py`, README, and hub Go files
   `tools/orb-manager/internal/orbnet/appcontainer.go` and `tools/orb-manager/internal/orbfake/apps.go`.
   Expand any cap-name rendering buffers from 64 to at least 80.
2. **ABI** add imports `press_claim` -> `orb_press_claim` sig `i(i)` and `press_lent` ->
   `orb_press_lent` sig `i()` to `main/app/orb_app_wasm.c`, `apps/sdk/orb.h`, and regenerate
   `apps/sdk/binds.json` from 99 to 101 imports. A caller without the cap returns 0 and records the
   normal refusal.
3. **State module** create `main/input/orb_press.h`, `main/input/orb_press.c`, and add both source
   entries beside `input_router.c` in `main/CMakeLists.txt`. Public API as design §3.2. State is
   `EXT_RAM_BSS_ATTR`, no internal BSS. Latest caller wins, queue cap 4, drop clears claim and queue.
4. **Router** in `main/input/input_router.c`, lend only a plain press after hold/press-and-turn.
   If owner exists and is not the glass app, queue it, pulse owner, and suppress glass delivery. If
   owner is glass app, leave unchanged. OS/carousel/menu/share/HTTP contexts never receive a lent
   press.
5. **Ring pulse** add `ORB_RING_PULSE_MS 160` and `orb_ring_disp_pulse(int slot)` in `orb_ring.c`.
   During the window, the claimant's own record is forced to full paint/sends. No pulse when no ring
   record or another record owns rim. PSRAM state only.
6. **Lifecycle** call `orb_press_drop(slot)` from revoke, uninstall/engine unload, import refusal
   without cap, and trap/fault handling. Do not create a new internal mutex; reuse existing locking.
7. **Settings cap** raise `ORBAPP_SETTINGS_MAX` and mirrors from 16 to 18 in `orb_app_manifest.h`,
   `orb_app.h`, `orb_setui.h`, `tools/orbapp.py`, and tests pins. Update install reader tests.
8. **Tests** add `tools/host_press_test.py` compiling `main/input/orb_press.c` directly and testing
   claim, release, latest wins, saturation, take, drop, owner, lifecycle API. Extend host ring test
   for pulse. Extend svc/manifest/orbapp/menu/net tests as needed for cap and 18 setting cap.

## Behaviour and edge cases

- A claim without the `press` cap is refused and leaves the previous owner unchanged.
- `orb_press_claim(0)` by a non-owner is ignored; by the owner clears the owner and queued presses.
- A new claimant steals the claim and old queued presses are discarded.
- Five simultaneous lent presses queue at most four; the fifth is swallowed and logged.
- A trapped, revoked, or unloaded app cannot later consume queued presses.
- The glass app under a lent press does not receive `pressed`; turns, press-and-turn and hold are
  unchanged.
- A pulse never happens for an app without a ring record.
- Existing 16-setting manifests still load; the new cap only raises rejection from 17 to 18.

## Acceptance criteria

1. `python3 tools/host_press_test.py 2>&1 | tail -3 | grep -q 'RESULT: pass'`
2. `python3 tools/host_press_test.py --mutations > /tmp/alerts2m.log 2>&1; grep -qE 'mutants caught ([0-9]+)/\1' /tmp/alerts2m.log && ! grep -q MISSED /tmp/alerts2m.log`
3. `python3 tools/host_ring_test.py 2>&1 | tail -3 | grep -q 'RESULT: pass'`
4. `python3 tools/host_svc_test.py 2>&1 | tail -3 | grep -q 'RESULT: pass'`
5. `grep -qE 'CAP_PRESS[[:space:]]*=[[:space:]]*11' main/app/orb_svc.h && grep -q '"press"' main/app/orb_svc.c`
6. `grep -qE 'ORBAPP_SETTINGS_MAX[[:space:]]+18' main/app/orb_app_manifest.h && grep -qE 'SETTINGS_MAX[[:space:]]*=[[:space:]]*18' tools/orbapp.py`
7. `python3 - <<'PY'\nimport json;d=json.load(open('apps/sdk/binds.json'));assert len(d['imports'])==101\nPY`
8. `make check-sandbox`
9. `git diff --name-only` touches no `apps/pomodoro/*` files.

## Out of scope

Do not touch `apps/pomodoro/*`, version files, app manifests under apps, or add the `press_any`
setting. Do not implement pomodoro behaviour; step 3 owns that.

## Notes

Router integration itself is device-visible and cannot be host-proven fully. The pure state module is
host-proven deliberately. Keep all new persistent-looking state in PSRAM because internal RAM is the
cliff. If an existing suite cannot support mutations, say so in the PR body rather than weakening the
gate.
## Orchestrator amendments

See alerts-design.md § Orchestrator amendments: a press the OS spends on waking a dimmed or dark glass is never lent; the host suite (or the router note, if the wake path is device-only) names this case.
