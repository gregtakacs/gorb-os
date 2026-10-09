# On-device QR onboarding screen — implementation plan

**Status: PLAN ONLY. Not implemented.** Nothing here is written to `main/` yet.

## Problem

The captive-portal DNS responder (commit `6bf89ea`) makes a phone that joins
`orb-xxxxxx` pop "Sign in to network" on its own — but only *after* the user has
already joined the right network from Settings. On a cold device with no stored
credentials the user has to open Wi-Fi Settings, find `orb-xxxxxx` (whose last
four hex digits vary per unit), and join it before the portal prompt appears at
all. The orb has a colour AMOLED panel and a knob. It should just **show a QR**
that a phone camera turns into "join this network, open this page", removing the
"which SSID is mine and what's the IP" step entirely.

## Scope / non-goals

- Show, while the provisioning SoftAP is up, a full-screen QR encoding the
  config page, plus the SSID as readable text and a one-line hint.
- Auto-show on AP up, auto-hide on AP down; knob long-press to leave early,
  knob press to bring it back while the AP is still up.
- Reuse the existing input_router screen machinery. **Non-goals:** no PSK in the
  QR (the orb is the joinee, not the joiner — a plain web URL QR is enough and
  avoids the Wi-Fi-transfer QR type + Android-camera-only support rabbit hole);
  no touch (the panel is knob-only for input); no change to the AP or DNS logic.

## Why a URL QR, not a `WIFI:` QR

A `WIFI:T:WPA;S:orb-xxxxxx;P:...;;` payload would pre-fill the join sheet, but
it leaks nothing useful here (the orb's AP has **no passphrase** — it is an open
provisioning network by design) and iOS camera does not join Wi-Fi from a QR at
all. A URL QR to `http://192.168.4.1/` is understood by every phone camera,
opens the browser straight onto the config page, and the OS still shows its own
"join this open network / sign in" flow. The SSID is printed as text above it so
a user joining manually still sees which network. So:

- QR payload: `http://192.168.4.1/`  (the AP's fixed gateway — see below).
- Label 1: `Join Wi-Fi: orb-XXXX` (the runtime SSID).
- Label 2: `then tap the sign-in notification` (the portal flow the DNS change unlocked).

The AP address is the default softAP gateway and never changes on this build
(`orb_net.c` logs `192.168.4.1`, DHCP server started on `WIFI_AP_DEF` with
`192.168.4.1`), so hard-coding it in the URL is honest — but read it via
`esp_netif_get_ip_info(s_ap_netif)` at show time rather than trusting the
literal, so a future change to the AP subnet cannot silently mis-target the QR.

## Grounded facts (verified against this tree, so the plan isn't hand-wavy)

- **LVGL**: managed `lvgl/lvgl ^9.2.0` (`main/idf_component.yml`). QR widget
  exists at `managed_components/lvgl__lvgl/include/lvgl/widgets/lv_qrcode.h`:
  `lv_obj_t *lv_qrcode_create(lv_obj_t *parent);` then
  `lv_result_t lv_qrcode_update(obj, const void *data, uint32_t len);`
  (this 9.x revision also has `lv_qrcode_render(obj)` for deferred mode and
  `lv_qrcode_set_size` / dark-light colour setters — **verify exact names against
  the installed header before use**, they are not all shown here).
- **The QR widget is currently COMPILED OUT**: `sdkconfig` has
  `# CONFIG_LV_USE_QRCODE is not set`. `lv_qrcode_*` symbols do not link until
  this is on.
- **Screens** are owned by `input_router.c`: `input_screen_t` =
  `INPUT_SCREEN_COLOR, INPUT_SCREEN_CLOCK, INPUT_SCREEN_COUNT`. Long-press does
  `set_screen((s_current + 1) % INPUT_SCREEN_COUNT, true)` — i.e. adding a new
  enum member automatically extends the long-press cycle. Each screen's widgets
  live in their own module built once at boot and loaded with `lv_screen_load()`
  (`clock_view_screen()`, `color_picker_screen()`); `clock_view_set_bounce()`
  shows the "park the off-screen animation" convention.
- **Knob** (`main/knob/knob.h`): `knob_take_long_press()` (true exactly once),
  `knob_take_press()`, `knob_held_ms()`, `knob_long_press_ms()`. All consumed on
  the LVGL task inside `poll_timer_cb`.
- **AP state is already visible to other modules**: `orb_net_is_provisioning()`
  in `main/net/orb_net.h`. The AP SSID is formed at `orb_net.c:512` as
  `snprintf(..., "orb-%02X%02X", mac[4], mac[5])`.
- **Threading invariant (this is the crux):** every widget call must run on the
  LVGL task (see `display.h` commit-hook comments and `input_router.c`'s
  "Runs on the LVGL task … the only context allowed to touch widgets"). The
  responder/AP run on the default esp_event task and the radio task. So the
  "AP came up, show the QR" trigger must NOT call `lv_*` from the event handler.

## Design

### 1. New module `main/setup/setup_view.{c,h}` (mirrors clock_view / color_picker)

```
esp_err_t setup_view_init(void);          // build the screen once, at boot
lv_obj_t *setup_view_screen(void);         // for input_router to lv_screen_load()
void      setup_view_show(const char *ssid, const char *url);
                                          // (re)encode QR + labels; LVGL-task only
```
- Statics: the `lv_obj_t *` for the screen, the qrcode widget, and the two
  labels. Built black-background, QR centred (~240 px on the panel), SSID label
  above, hint below, using the same fonts the clock already has linked.
- `setup_view_show()` calls `lv_qrcode_update(qr, url, strlen(url))` and
  `lv_label_set_text()` on both labels. Called only from the LVGL task.

### 2. Wire a fourth screen into `input_router.c`

- Add `INPUT_SCREEN_SETUP` to `input_screen_t` (before `INPUT_SCREEN_COUNT`).
- Call `setup_view_init()` at boot alongside `clock_view_init()` /
  `color_picker_init()` (wherever those live in `main.c`).
- `screen_name()` gains `case INPUT_SCREEN_SETUP: return "setup";`.
- `set_screen()`: load `setup_view_screen()` when `s==INPUT_SCREEN_SETUP`; the
  existing "park the clock animation" rule (`clock_view_set_bounce` tied to
  CLOCK only) naturally parks the tick while setup is on the glass.
- **Make the long-press cycle skip SETUP when the AP is down**, so normal use
  stays a clean clock↔colour loop and the QR only appears during provisioning:
  step to `s_current+1`, and if that lands on SETUP but
  `!orb_net_is_provisioning()`, step once more (wrapping past it).

### 3. Trigger, without crossing the LVGL thread boundary

- Simplest: the router poll (already on the LVGL task) watches
  `orb_net_is_provisioning()` for an edge each 10 ms tick — no new plumbing, no
  new event. On the AP-up edge call `setup_view_show("orb-XXXX",
  "http://192.168.4.1/")` then `input_router_show(INPUT_SCREEN_SETUP)`; on the
  AP-down edge, if currently on SETUP, fall back to
  `input_router_show(INPUT_SCREEN_CLOCK)`.
- Alternative (only if the 10 ms edge latency is unacceptable): post a custom
  `ORB_NET_EVT_AP_UP/DOWN` app event from the existing `WIFI_EVENT_AP_START` /
  `AP_STOP` handlers (next to `dns_start()`/`dns_stop()` so the QR and the
  responder share one lifecycle), and marshal into the LVGL task with
  `lv_async_call()` — never call `lv_*` from the event handler body itself.
- Read the SSID at show time: add `const char *orb_net_ap_ssid(void);` to
  `orb_net.h` (returns the last-built AP config SSID) rather than recomputing
  `orb-%02X%02X` in the UI, so there is one source of truth for the name.

### 4. Knob gestures on the SETUP screen

- Long-press already leaves (advances the cycle; with the skip logic it returns
  to colour/clock). In `poll_timer_cb`, `case INPUT_SCREEN_SETUP:` ignores
  `delta`/`pressed` — long-press handles exit.
- Re-show while AP up: on CLOCK or COLOUR, `knob_take_press()` while
  `orb_net_is_provisioning()` and SETUP was dismissed → re-show SETUP. Keep it
  AP-gated so a stray press on a normally-connected clock never pops a QR to a
  dead AP.

### 5. Config

- **Enable the widget:** add `CONFIG_LV_USE_QRCODE=y`. There is **no
  `sdkconfig.defaults`** today and `sdkconfig` is untracked, so create a repo-root
  `sdkconfig.defaults` with `CONFIG_LV_USE_QRCODE=y` so a fresh clone gets it —
  and remember the `ORB_NET_CAPTIVE_DNS` lesson: an *existing* `sdkconfig` will
  NOT be reconciled by a rebuild (the `# ... is not set` line stays put), so the
  line must be written into `sdkconfig` for local builds too.
- Gate the feature behind the project's own toggle for consistency with the DNS
  responder and the clock-only build: `config ORB_NET_PROVISION_QR` (default y,
  `depends on ORB_NET_ENABLE`), same help-text style as the captive-DNS block.
  Wrap `setup_view.*` calls in `#if CONFIG_ORB_NET_PROVISION_QR` and add the
  source to `main/CMakeLists.txt` under the same guard.

## Failure / edge cases to respect (same rigour as the rest of net/)

- **QR is a rendered image, not a runtime dependency** — if `LV_USE_QRCODE` ever
  regresses to off, the build must fail loudly (a `#error` in `setup_view.c` when
  the feature is on but the widget is not), rather than ship a blank onboarding
  screen with no clue.
- **Panel is on the LVGL task**; never touch widgets from `on_wifi`/net task —
  marshal via the poll edge or `lv_async_call`. A direct `lv_qrcode_update()`
  from the event task is the bug to avoid.
- **Dismiss must be possible even if the AP hangs open** — long-press always
  leaves SETUP regardless of AP state (the skip logic only governs auto-cycling,
  never the manual exit).
- **AMOLED burn-in:** the setup screen is transient (AP-scoped, ~900 s max hold)
  so no mitigation needed today, but note it if the AP hold ever grows.

## Test plan (what this bench can and cannot prove)

- **Host:** none of this is host-testable (pure LVGL). No `host_tz_test.py`
  additions — unlike the DNS responder's byte encoding, which the host suite
  already pins.
- **Build gate:** both `ORB_NET_PROVISION_QR=y` (+`LV_USE_QRCODE=y`) and the
  clock-only `=n` build clean, 0 warnings; `lv_qrcode_*` present in `=y` elf and
  absent in `=n` — the same `nm`/`strings` proof used for the DNS responder.
- **Bench (serial + power, no WiFi radio):** opening the AP via
  `POST /api/v1/wifi/ap {}` should log the router switching to `setup` and the
  QR encode; closing it should fall back to clock. Verify no LVGL-task assert
  (proves the threading invariant held) and that the clock's bounce timer is
  parked while setup is up (`display_bench_snapshot` frame budget drops).
- **Human / camera (the only real proof):** with the AP up, the panel shows the
  QR; a phone camera on the QR opens the browser on the config page, and the
  printed `orb-XXXX` matches `GET /api/v1/status`'s `ap_ssid`.

## Touch-list

- `main/setup/setup_view.{c,h}` (new)
- `main/input/input_router.{c,h}` — enum + `set_screen`/`screen_name`/long-press
  skip + SETUP dispatch + re-show-on-press
- `main/main.c` (wherever `clock_view_init`/`color_picker_init` are called) — add
  `setup_view_init()`
- `main/net/orb_net.{c,h}` — add `orb_net_ap_ssid()`; optionally the AP_UP/DOWN
  app events (only if the poll-edge approach is rejected)
- `main/Kconfig` — `ORB_NET_PROVISION_QR` toggle (default y, depends
  ORB_NET_ENABLE), same help-text style as the captive-DNS block
- `sdkconfig.defaults` (new) — `CONFIG_LV_USE_QRCODE=y`
- `main/CMakeLists.txt` — build `setup/setup_view.c` under the config guard

## Open questions to resolve before implementing

1. Exact QR size/positioning that reads reliably off this CO5300 panel at arm's
   length — pick a starting module size, then verify with a real camera.
2. Confirm the installed LVGL 9.x qrcode property API names (`set_size` / colour
   setters / whether `lv_qrcode_update` alone repaints) against
   `managed_components/lvgl__lvgl/include/lvgl/widgets/lv_qrcode.h`.
3. Whether to surface a manual "show onboarding QR" entry outside the AP-open
   path — probably not; keep it AP-scoped and simple.