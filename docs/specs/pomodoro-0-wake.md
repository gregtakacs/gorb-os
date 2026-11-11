# pomodoro-0: `orb_wake`, a one-shot wake an app may ask for from its service

> Read first: `docs/specs/pomodoro-design.md`, "Orchestrator amendments" (round 1, item 2) and
> "round 2". Plan §64.6 is the ruling.

## Goal

An app off the glass can light a dimmed glass for a few seconds: the pomodoro's "Wake the
screen" at a phase end. Today `orb_power_request` only holds the glass while the app is on it
(`main/app/orb_svc.c` `svc_power_request`, revoked on leave).

## Read first

- `main/app/orb_svc.c`:
  - `svc_power_request` and the `SVC()` cap gate;
  - `idle_dim_app_request` / `idle_dim_app_release`, and the idle-dim module they call (find it
    under `main/`): its quiet hours, the owner's display-off truth, and how a wake is applied.
- `main/app/orb_app_wasm.c`: `imp_power_request` and the `s_binds` table (around :1901).
- `apps/sdk/orb.h` (the power section, around :281), `apps/sdk/binds.json`, and `apps/sdk/orbsdk.py`
  `binds`.
- `apps/README.md`: the caps table and its prose.
- The host suites: `tools/host_ring_test.py` (ring-1, on the base branch) is the pattern, pure C on
  gcc with mutations that exit non-zero on an escaped mutant.

## Contract

- SDK: `ORB_IMPORT("wake") int32_t orb_wake(uint32_t seconds);` in `orb.h`'s power section, under
  cap `power`, with a doc comment that states every rule below.
- It returns:
  - `ESP_OK` when the glass is lit (or already lit);
  - `ESP_ERR_NOT_ALLOWED` without the cap, in the OS's quiet hours, or with the display off by
    the owner;
  - `ESP_ERR_INVALID_STATE` when this app woke the glass less than 60 s ago.
- `seconds` is clamped to 3..15.
- It never switches apps, never takes the glass, and never changes what is on it. It goes
  through idle_dim's own request path, so idle_dim stays the policy owner.
- **Pure decision in host-testable C:** a function with no ESP-IDF includes that takes
  (now_ms, the app's last-wake time, the requested seconds, quiet, display-off) and returns the
  verdict and the clamped seconds. It lives in a new `main/app/orb_wake.c` / `.h`, with
  `tools/host_wake_test.py` and its mutations: the clamp ends, the 60 s edge, quiet, display-off.
- The firmware glue (the svc entry, the wasm import, the `s_binds` row, the per-app last-wake
  record dropped in `slot_release`) cannot be compiled here (no ESP-IDF). Keep it thin. The
  orchestrator builds and benches it.
- `binds.json` regenerated (`orbsdk.py binds --write`). `apps/README.md` gets one paragraph on
  `orb_wake` under the caps.

## Out of scope

- The pomodoro app itself.
- §55 alerts.
- Any hub change: cap `power` already exists in the hub.

## Acceptance

`set -o pipefail; python3 tools/host_wake_test.py | tail -1 | grep -q 'RESULT: pass' && python3 tools/host_wake_test.py --mutations && python3 apps/sdk/orbsdk.py binds && grep -q '"wake"' apps/sdk/binds.json && grep -q 'orb_wake' apps/sdk/orb.h && grep -q 'orb_wake' apps/README.md && ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps`
