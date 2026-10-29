# Provisioning-first boot — implementation plan

**Status: PLAN ONLY. Not implemented.** Nothing here is written to `main/` yet.
Supersedes `docs/ap-onboarding-qr-plan.md` §"non-goals" and the persistent
isolated-AP mode. Read that file for the LVGL threading invariant and the
two-stage screen pattern, which this plan keeps.

## The direction change

Three rules, in order of how much they cost:

1. **A persistent AP-only Orb is deleted from the design.** The Orb is a network
   device. It always wants its configured station, and nothing an owner can
   select may park it on its own AP forever. `CONFIG_ORB_NET_AP_ISOLATED`, the
   `mode` NVS key, the serial `MODE ap` verb and the hub's `c-ap` state token all
   go away.
2. **The AP exists in exactly two states:** `UNPROVISIONED` (no stored credentials)
   and `RESCUE` (credentials stored, link dead). In both, the glass shows one
   static QR. Nothing else is loaded, drawn or ticking.
3. **The rest of the OS is gated behind a connected network.** Theme projection,
   `appinstall`, the carousel, `idle_dim` and the 15 fps dial all wait. On a board
   that cannot reach its network, the clock is not the product — the pairing page
   is.

## Problem

A clean flash today boots the whole OS first and the radio last. Measured on
orb-A (44:bd:8d:60:80:88), live capture 2026-10-29:

```
I (1046) display: Display ready (466x466), LVGL buffers: 18640 B x2 internal DMA RAM
I (1091) art_bank: bank "assets" @0x430000, 1175552 B mapped, 18 MMU pages
I (1096) appinstall: resolved 1 installed app from the "apps" partition
I (1165) main: gorb-os running
I (7166) orb_net: [heap] pre-netif  internal free=59391 | dma_free=51887
I (7184) orb_net: [heap] post-wifi_init internal free=10731 largest=7168 | dma_free=3227
I (7188) orb_net: [heap] post-nettask   internal free=4667 largest=4096 | dma_free=239
W (7187) orb_net: no stored network — provisioning AP "orb-8089" (open)
W (7223) wifi:alloc eb len=752 type=4 fail
Guru Meditation Error: Core 0 panic'ed (LoadProhibited) EXCVADDR: 0x0000002c
  ieee80211_hostap_attach <- wifi_softap_start <- _do_wifi_start
```

Six seconds and 55 KB of internal DMA RAM go into an OS the owner cannot use, and
the SoftAP beacon — one contiguous 752-byte management buffer — then has nowhere
to land. `ieee80211_hostap_attach` dereferences the null the driver returned and
the board reboots. The panic precedes the first RF calibration write, so the loop
never self-heals, and the config page is never reachable. `0c50881` stops the
bleed by lending the tile ring across `esp_wifi_start()`. That is a patch on the
symptom. The ordering is the disease.

The same ordering is why an unprovisioned Orb shows a theme it did not choose and
a dial nobody asked for while its owner is trying to pair it.

## Grounded facts (verified against this tree)

- **The gate is one NVS read.** `orb_net.c:7187` already computes
  `const bool creds_in_flash = (ssid[0] != '\0');`. Partition `storage`, namespace
  `orb.net`, keys `ssid`/`pass`/`mode`/`tz`/`name` (`orb_net.c:93-94,1040-1042`).
  A read-only `nvs_open` answers rule 3 before `art_bank_init` costs anything.
- **Rescue already exists.** `CONFIG_ORB_NET_AP_RESCUE_FAILS=8` raises the AP after
  eight failed join attempts (`orb_net.c:2048-2049`), and
  `s_prov_recovery_session` (`orb_net.c:180`) already separates a factory session
  from a recovery one. Rule 2's second state is a routing change, not a new
  mechanism.
- **`orb_net_start()` is called at `main.c:290`**, after `display_init()` (180),
  `orb_carousel_init()` (234) and `idle_dim_init()` (253). The comment there says
  this is so "the dial is already ticking while the radio comes up". Rule 3
  inverts that priority on purpose, for unprovisioned boards only.
- **The QR widget is compiled out.** `sdkconfig`: `# CONFIG_LV_USE_QRCODE is not
  set`. `lv_qrcode` is present in the managed LVGL 9.2 (`ap-onboarding-qr-plan.md`
  §Grounded facts). Turn it on in **both** `sdkconfig.defaults` and the tracked-
  out local `sdkconfig`: `sdkconfig.defaults` only applies to a fresh `sdkconfig`,
  which is exactly how `0c50881`'s buffer trims were silently lost in the sibling
  clone `gorb-os-2` (measured there: static RX 10/32/32/32 against the committed
  6/12/12/12, plus `RELEASE_OPTIMIZE` off). Do not delete `gorb-os`'s `sdkconfig`
  — it holds the correct values.
- **No text entry exists anywhere on the board.** `lv_keyboard`, `lv_textarea`,
  and any character picker: absent from `main/` by grep. The panel is knob-only by
  ruling (`touch_cst9217.cpp` is excluded from the firmware build on purpose). So
  the passphrase picker in §4 is new work, not a wiring job — and it is why
  `menu_view.h`'s AP row says it needs a keyboard.
- **The two-stage armed screen is the house pattern.** `menu_view.h`'s header
  comment: inert landing, knob press arms, rotate scrolls, press chooses,
  long-press always exits. Reuse it verbatim for the SSID list and the picker.
- **The captive portal works.** `CONFIG_ORB_NET_CAPTIVE_DNS=y`, DHCP on
  `192.168.4.1`, SSID `orb-%02X%02X` of `mac[4..5]` (`orb_net.c:1261`).
- **Every widget call must be on the LVGL task.** See `ap-onboarding-qr-plan.md`
  §3. The AP-up trigger marshals through the router's 10 ms poll edge or
  `lv_async_call()`, never from `on_wifi`.
## Design

### 1. One gate, read before anything expensive

New module `main/setup/boot_gate.{c,h}`:

```c
typedef enum { ORB_BOOT_LINKED, ORB_BOOT_UNPROVISIONED, ORB_BOOT_RESCUE } orb_boot_phase_t;
orb_boot_phase_t boot_gate_early(void);   /* read-only nvs_open on "storage"/"orb.net" */
void             boot_gate_link(void);    /* called by orb_net on first lease this boot */
bool             orb_boot_setup_phase(void);
```

Called first in `app_main`, ahead of `art_bank_m2_probe()` (`main.c:151`),
`display_init()` (180), `orb_carousel_init()` (234) and `idle_dim_init()` (253).
It reads `ssid` only: empty means `UNPROVISIONED`. `RESCUE` is not decided at boot —
it is decided at runtime by the existing eight-failure ladder
(`CONFIG_ORB_NET_AP_RESCUE_FAILS`, `orb_net.c:2048`) — so `boot_gate_early()` returns
`LINKED` and the AP path promotes the session later.

`UNPROVISIONED` takes the setup branch in §2 and never reaches the theme, the app
walk or the carousel. `LINKED` takes today's branch unchanged. The gate is not a
fork in the networking code: `orb_net_start()` keeps one job in both branches. Only
the clock's OS comes up later.

### 2. The setup screen is the only thing that exists

`main/setup/setup_view.{c,h}`, one screen, built once:

- Black background, QR centred at 240 px, SSID in 24 px text above, one hint line
  below, the fixed address `192.168.4.1` in 16 px. No theme reads: the screen paints
  from the built-in fallback face only, so `art_bank_init()` and the projection are
  skipped, not merely hidden. The `slot holds bundle "Aviator"` / `projected into a
  dial` lines and their 18 MMU pages must be absent from a setup-phase boot log — that
  is the measurable test for rule 3.
- **No tile ring in this phase.** `display_init()` takes a `setup` flag that funds
  one buffer and runs serially, the same single-buffer mode `ring_refund()` already
  falls back to when it cannot afford two (`display.c:563`). This frees the
  2 × 18,640 B up front instead of borrowing it back under a 100 ms drain bound, so
  the beacon's 752 B is never contended. Keep the `0c50881` loan: it protects
  `RESCUE`, where the ring is already funded when the ladder raises the AP.
- No 15 fps tick. Nothing invalidates, so LVGL draws once. The dial's frame-budget
  lines (`clock: clock 11 fps | ...`) must not appear in a setup-phase log.
- `idle_dim` is not initialised, which makes §7's mitigations mandatory.

### 3. QR payload: join first, then the page

The user scans and the phone joins the AP — that is the ask, and it rules out the
URL-only payload of `ap-onboarding-qr-plan.md`: a URL QR on a phone that has not
joined opens a browser onto an unreachable address.

- Primary: `WIFI:T:nopass;S:orb-8089;H:false;;`. No secret is disclosed — the AP is
  open by design. Long-press swaps to the URL payload `http://192.168.4.1/` for
  phones that ignore `WIFI:` payloads, and the label names which one is up. Both the
  swap and the join-then-capture sequence are **bench-verify items**; do not trust
  either until a real phone joins from the glass.
- Captive DNS stays on, so a phone that joins is pushed to the page by the OS with
  no scanning at all.

### 4. On-device entry, with one knob and one button

Press on the QR dismisses it and opens the direct path. Reuse `menu_view`'s
two-stage contract: press arms, rotate moves, press chooses, long-press always exits.

```
SETUP_QR → (press) → NET_LIST → (press) → KEY_ARM → (hold) → JOINING → LINKED
```

- `NET_LIST`: call `prov_promote_apsta()` first (`orb_net.c:1386`) — that function
  exists precisely so a scan can happen on an already-started, settled, AP-only
  driver, the regime proven not to Guru. Drain the existing scan results
  (`scan_collect()`, `orb_net.c:3350`) into SSID rows with signal bars, deduplicated
  by SSID, strongest first, 5 GHz rows marked unjoinable rather than hidden. Rotate
  scrolls, press selects.
- `KEY_ARM`: two rows on screen — group row and character row. Rotate moves through
  `[a-z] [A-Z] [0-9] - _ . @ space`; press commits and advances the cursor. Hold
  600 ms backspaces one slot; hold 2 s submits. Uses the existing
  `knob_take_long_press()` / `knob_held_ms()` API, no new hardware state.
- Entry is masked by default; a hold on the filled field toggles reveal, so a
  mis-keyed character is visible before submit.
- Submitting writes `ssid`/`pass` through the existing `PROV` store path
  (`orb_net.c:1040-1042`) on the radio task — never on the LVGL task, because an NVS
  write off an internal stack is the `esp_task_stack_is_sane_cache_disabled` abort
  documented at `orb_net.c:7374`.
- A wrong key is `WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT`, already named `wrong-key` by
  `reason_name()` (`orb_net.c:1890`): say that, keep the SSID, reopen the picker at
  slot 0, do not wipe the entry.

### 5. Delete the persistent AP-only mode

`MODE ap` and `CONFIG_ORB_NET_AP_ISOLATED` park a configured Orb on its own AP
forever, unreachable and un-updatable. Rule 1 removes that state:

- `main/Kconfig`: delete `ORB_NET_AP_ISOLATED`; keep `ORB_NET_AP_FALLBACK`, which is
  the provisioning path and is what rule 2 keeps.
- `orb_net.c`: drop `ap_isolated` and its branches (the boot log near 7226, the
  forced-provisioning wording, the `s_prov_recovery_session` term at 7196, the mode
  getter at 791). `MODE` accepts `sta` only; `ap` answers "the Orb is a network
  device; that mode is gone" and changes nothing.
- NVS migration: a board booting with `mode="ap"` stored clears the key once and
  logs it. Silent migration is wrong — the board changes behaviour under its owner.
- Hub: drop `c-ap` from `stateWords()` (`tools/orb-manager/cableproof.go:552`) and
  fold the row into the existing `b` wording — "it waits for a network: provision it
  to give the row an address" is already exactly true for both phases. A device still
  reporting `c-ap` must render that raw token, not a guess, per that function's own
  rule.

### 6. The setup phase exits through one door

The AP is open and unauthenticated, so it must not linger by accident. Today the
factory session has no deadline at all (`orb_net.c:7220-7227`) — the hole rule 3
closes by never being in that state with a network configured.

- `UNPROVISIONED`: the AP stays up indefinitely; there is no network to fall back
  to. §7's guard applies for as long as it holds.
- `RESCUE`: the existing sojourn applies, `CONFIG_ORB_NET_PROV_SOJOURN_S=600`. Expiry
  tears the AP down and rejoins the retry ladder; it does not reboot.
- Exit in both cases is a lease, and only a lease: `boot_gate_link()` records it,
  `orb_net` posts a one-shot, and `app_main`'s deferred branch runs the theme, the
  app walk, the carousel and `idle_dim` exactly as today. `display_init()` funds the
  ring back to two buffers at that moment, so the glass goes from static QR to
  running clock in one repaint.
- Every existing door stays: serial `PROV`/`NAME`/`SCAN`/`MODE`/`RESET`, the hub over
  USB, and the page at `192.168.4.1`.

### 7. A static QR on AMOLED is a burn-in risk

`ap-onboarding-qr-plan.md` dismissed burn-in because its screen was AP-scoped at
~900 s. It is now unbounded: an unprovisioned Orb on a shelf holds one image for
days. This is a requirement, not a caveat:

- Shift the whole QR block 1 px on both axes every 30 s, and invert black/white
  every 5 min. The shift is small against a 240 px code on a 466 px panel, so
  legibility is unaffected.
- Run reduced panel brightness through the phase and restore it at `boot_gate_link`.
- Past 6 h, blank the panel for 60 s every 10 min. Joining still works — the SSID is
  in the phone's network list whether the panel is lit.

### 8. What the hub shows

An unprovisioned board is reachable only over the USB cable until it has a lease, so
the hub's row must name the SSID to join. Add `ap_ssid` and `setup_phase` to
`/api/v1/status`, and let the Apps and Themes faces render a "waiting to be
provisioned" card instead of the empty shelf `44ea14b` fixed. Seeding is unchanged
and still happens after first contact.

## Phasing

| Phase | Ships | Gate |
|---|---|---|
| P0 (done) | `0c50881` tile-ring loan | orb-A boots to a joinable AP |
| P1 | `boot_gate`, setup screen, no ring, QR, Kconfig deletion, hub token | clean flash shows QR under 2 s, no theme or app lines in the log |
| P2 | `NET_LIST` + `KEY_ARM` on-device entry | join a real WPA2 network with the knob alone |
| P3 | burn-in guard, `RESCUE` sojourn rework, status fields | 24 h soak on an unconfigured board |

P1 and P2 do not overlap: P2's screens ride P1's router slot.

## Bench tests that prove each rule

1. **Rule 3, negative:** clean flash, capture boot. `art_bank`, `orb_theme`,
   `appinstall`, `carousel` and `clock:` lines must be **absent**. If any appears,
   the gate is in the wrong place.
2. **Time-to-QR:** power-on to legible QR, target under 2 s against today's 7.2 s.
3. **Memory:** `stage_probe` before `esp_wifi_start()` shows `dma_free` above 40 KB
   in setup phase, with no `tile ring borrowed` line — the ring was never funded, not
   borrowed back.
4. **Rule 1, migration:** bind a board with `mode="ap"` in NVS, boot, expect one
   clear-the-key log line and a station join.
5. **P2:** join WPA2 by knob alone, then confirm the lease exits the phase and the
   clock appears with its theme.
6. **P3:** 24 h unprovisioned, then inspect the panel for retention against a
   never-driven reference unit.
7. **RESCUE:** pull the router's WAN or wrong the passphrase, wait out the ladder,
   expect the QR without a reboot and a clean return to the clock on restore.

## Touch-list

- `main/setup/boot_gate.{c,h}`, `main/setup/setup_view.{c,h}`, `main/setup/net_list.{c,h}`,
  `main/setup/key_arm.{c,h}` (new)
- `main/main.c` — gate ahead of line 151; OS bring-up behind a callable
  `orb_boot_os()` so both phase exits share one path
- `main/display/display.{c,h}` — minimal init flag (single buffer, no ring) and
  `display_ring_fund()` for the exit
- `main/net/orb_net.{c,h}` — scan API for the UI, `ap_ssid`/`setup_phase` in status,
  ladder raises the QR without a reboot
- `main/input/input_router.c` — `INPUT_SCREEN_SETUP`, press path into `NET_LIST`
- `main/Kconfig` — delete `ORB_NET_AP_ISOLATED`; add `ORB_BOOT_SETUP_QR`,
  `ORB_SETUP_BURNIN_GUARD`
- `sdkconfig.defaults` **and** the local `sdkconfig` — `CONFIG_LV_USE_QRCODE=y`
- `main/CMakeLists.txt` — the four new sources
- `tools/orb-manager/cableproof.go` + `internal/webui/` — drop `c-ap`, add the setup card
- `.clinerules` — rewrite the provisioning section once this lands. The current text
  documents persistent AP-only mode as a supported state, and a stale rule here is
  worse than no rule.
- `docs/ap-onboarding-qr-plan.md` — mark superseded, not deleted: it still carries
  the LVGL threading invariant and the widget-verification notes.

## Open questions to settle before P1

1. Exact `lv_qrcode` API names in this LVGL 9.2 revision — read the installed header.
2. Does the captive portal still capture a phone on `orb-8089` with no STA interface
   up? Settle this before the payload choice is final.
3. `KEY_ARM` charset ordering and the commit/backspace/finish gesture set. The
   reduced set above keeps entry near 30 detents per character; full ASCII needs a
   third row. Decide on the bench, by typing a real passphrase.
4. In `RESCUE`, does the QR replace the clock outright or dim over it? Rule 3 says
   replace. A board whose owner is away going dark is a worse story than one showing
   a dim clock, so this one is worth arguing before it is coded.
5. Whether `UNPROVISIONED` should ever hide the QR. The guard in §7 blanks the panel
   on a timer; the SSID text should probably survive that blank so the board still
   reads as pairing rather than dead.

## Risks and refusals

- **The `sdkconfig` trap is live.** `CONFIG_LV_USE_QRCODE` is currently unset and
  `sdkconfig` is gitignored, so it silently overrides `sdkconfig.defaults` on every
  existing clone. Measured on the sibling clone `gorb-os-2`: its stale `sdkconfig`
  carries static RX 10/32/32/32 where `sdkconfig.defaults` says 6/12/12/12. A P1
  build there ships no QR widget and fat radio buffers. P1's build step is
  `rm sdkconfig && idf.py reconfigure && idf.py build`, and the bench check greps the
  boot log for `static rx buffer num: 6` before anything else.
- **`app_main` becomes resumable.** Deferring OS bring-up until a lease lands means
  one code path serves cold boot, setup exit and rescue exit. Refuse any design where
  the second and third calls re-run `app_main` — the rollback marker at `main.c:276`
  and the USB one-shot claim at `main.c:296` are both once-per-boot by construction.
- **Open AP, unauthenticated `/api/v1/ota`.** The AP stays open, so the only wall is
  that it is short-lived and LAN-isolated. `RESCUE` must keep its sojourn deadline or
  this plan hands an attacker an indefinitely open admin page on a shelf device.
- **Do not route the setup QR through a theme.** The theme slot may hold a bank the
  panel cannot show, and a pairing screen whose pixels depend on flash contents is a
  pairing screen that can fail.

## Summary of the rule change

The Orb boots into one of two moods. Unprovisioned or unreachable, it is a pairing
target: one black screen, one QR, one knob gesture set, nothing else loaded.
Provisioned and linked, it is a clock, and the AP is a timed exception rather than a
place to live.

