# Writing a gorb-os app

An app is a wasm32 module that runs inside the OS's wasm3 interpreter. It ships in
the code region of an `.orbapp` container. The clock, the HSB editor and the flight
tracker in this directory are all built this way, and the firmware holds none of
their code. The full design record is
[docs/app-platform-plan.md](../docs/app-platform-plan.md) §43.

## The layout

```
apps/
  sdk/            the only thing an app may link against
    orb.h         the whole OS surface: imports, types, entry points
    orb_stage.h   the stage, for an app that ships a theme
    clk_types.h   theme record layouts (a byte copy of main/art/clk_types.h)
    orb_rt.c      runtime glue linked into every app (orb_logf, libc shims)
    libc/         string.h, stdio.h, stdlib.h, math.h: what C you get
  <slug>/         one directory per app: its .c and .h files, nothing else
```

The directory name is the app's slug. Every `.c` file in it is compiled.

## Build

```
python3 tools/build_apps.py            # every app, with sizes and import lists
python3 tools/build_apps.py <slug>     # one app
```

Output goes to `build/apps/<slug>.wasm`. The compiler is clang targeting
`wasm32 -mcpu=mvp` (no proposals), `-O2 -Werror`, freestanding, with no libc
beyond `sdk/libc`. You can override it with `ORB_CLANG`. The build refuses:

- any import outside module `orb`;
- an `orb` import the OS does not bind. The list is read from `s_binds` in
  [main/app/orb_app_wasm.c](../main/app/orb_app_wasm.c), and the signatures
  must match too;
- a `sdk/clk_types.h` that has drifted from `main/art/clk_types.h`.

Builds are deterministic: no timestamps and no paths, so unchanged sources
rebuild byte-identical.

## Package

A container is written only by [tools/orbapp.py](../tools/orbapp.py) (ruling
C11). The starter apps are declared in
[tools/make_factory_apps.py](../tools/make_factory_apps.py), and a new app gets
a row in its `APPS` table:

- **slug and title**;
- **kind**: always `KIND_WASM` (2). Kinds 1, 3 and 4 are retired and never
  reused;
- **caps**: the owned capabilities the app uses. Pick from `power`, `net`,
  `sd_read`, `themes`, `sound` and `location`. The declaration is the consent
  (D19). An import the app did not declare answers a refusal, and the owner can
  revoke any of them on the dial;
- **settings**: up to 16, each `int`, `enum`, `toggle`, `string` or `secret`.
  See below;
- **period_ms** (in `PERIOD_MS`): the fastest beat the app may ask for. If the
  app is absent from that table it never ticks. The floor is 1000 ms.

```
python3 tools/make_factory_apps.py                 # write the shelf containers
python3 tools/make_factory_apps.py --check         # are the committed ones fresh?
python3 tools/make_factory_apps.py --unreleased D  # held apps, to D, for a bench
python3 tools/orbapp.py --inspect <file.orbapp>    # read one back
```

The header serial comes from `version.txt`, so a version bump makes every
committed container stale until you re-run the tool.

## Install

Upload the container to the card (`POST /api/v1/sd/app`), then install it
(`POST /api/v1/apps/install?id=<slug>`). You can also install from the dial's
APPS page. Installing a code app reboots the orb, because modules load only at
boot.

An app is installed or it is not. There is no off switch. Uninstall erases the
slot along with every setting, secret, grant and fault record the app had, and
installing it again starts fresh with default options.

Each install slot holds at most 100 KB of container. The shipped apps run 9–26 KB.

## The ABI

[sdk/orb.h](sdk/orb.h) documents every call. In short:

| Group | Calls | Cap |
|---|---|---|
| time | `orb_now`, `orb_uptime_ms/us`, `orb_localtime`, `orb_tz_offset` | free |
| log | `orb_log`, `orb_logf` | free |
| settings | `orb_cfg_get/set`, `orb_cfg_get_str/set_str`, `orb_cfg_gen` | free (declared settings only) |
| beat | `orb_set_beat`, `orb_burst` | free |
| widgets | `orb_ui_create/set/text/align/refresh` | free |
| canvas | `orb_cv_create/fill/pixel/ring/disc/line/text/flush` | free |
| menu | `orb_reply`, `orb_armed` | free |
| JSON | `orb_json_begin/next/skip` | free |
| network | `orb_net_request/info/read/header/free`, `orb_net_cred`, `orb_net_get` | `net` |
| sound | `orb_sound_play/stop/active` | `sound` |
| location | `orb_home`, `orb_home_gen` | `location` |
| power | `orb_power_request` | `power` |
| stage | `orb_stage_*` ([sdk/orb_stage.h](sdk/orb_stage.h)) | free; draws only for the theme's own app |

Strings cross the boundary as NUL-terminated pointers into the app's own memory,
and the OS copies them. The OS never hands out a pointer into its own memory.

**Entry points** are exports the OS calls. All are optional except `orb_init`:

- `orb_init`: build the page. A nonzero return refuses the app;
- `orb_activate`, `orb_suspend`, `orb_resume`, `orb_show`: the app's page going
  on and off the glass;
- `orb_tick`: the beat, while the page is on the glass;
- `orb_service`: every service pass, on the glass or not;
- `orb_input`, `orb_set_armed`: the knob, while armed. A press arms and a hold
  disarms;
- `orb_opt_count`, `orb_opt_kind`, `orb_opt_label`, `orb_opt_value`,
  `orb_opt_inc`, `orb_opt_sel`, `orb_opt_arm`: the app's options page in the
  dial menu. `orb_opt_label` and `orb_opt_value` answer through `orb_reply`
  during the call.

The app's own menu page holds settings only. Themes are picked in the OS Themes
menu (D44).

### Secrets

A `secret` setting (for example an API key) is declared with the one host it may
be sent to:

```python
{"id": "key", "type": "secret", "host": "api.example.com"}
```

The app can never read it: `orb_cfg_get_str` refuses a secret. The app passes its
id as `cred` to `orb_net_request`. The OS then attaches it as a request header,
and only if the URL is `https://` and the host matches exactly. `orb_net_cred(id)`
tells the app whether the owner has set one. Owners set secrets from the hub or
with `POST /api/v1/apps/cfg?id=<slug>&key=<id>` (JSON body `{"value":…, "header":…}`).

### Network and JSON

Each app gets one request at a time. The request runs off the UI task, so poll
`orb_net_info` from `orb_tick` or `orb_service` until `state` is `ORB_NET_DONE` or
`ORB_NET_FAILED`. The body of a response with any status stays in an OS buffer.
Read it in pieces with `orb_net_read`, or tokenize it in place with
`orb_json_begin(NULL, 0)` and `orb_json_next`, then call `orb_net_free`. Tokens
carry keys up to 31 bytes and strings up to 95. Longer text is cut, and
`key_trunc` / `str_trunc` say so.

## Rules the OS enforces

- **Time budget.** `orb_init` gets 1000 ms. Every other entry point gets 250 ms
  (the shipped apps use under 40). An app that runs past its budget traps. Two
  traps quarantine it (C2), and the dial shows it as failed with a **retry**
  door. A deadline that lands while the OS runs a large transfer (a card upload, a theme store, an
  OTA) is extended by up to 2 s, so an app the transfer starved is not blamed. Do bounded work
  and return. For a long job, process N items per call and
  carry on next tick, as the flight tracker does with its 400 JSON events a pass.
- **Memory.** A module's linear memory is fixed at build time and capped at
  128 KB (`ORB_WASM_MEM_MAX`). The wasm shadow stack is 8 KB. Avoid large locals
  and recursion on deep input.
- **Native stack.** wasm3 on this chip spends one native frame per executed op,
  so guest calls run on a shared 64 KB PSRAM stack. The shipped apps peak at
  0.7–18.5 KB (`wasm_stack_peak` in `/api/v1/status`).
- **Settings writes are deferred.** `orb_cfg_set` updates the value at once, but
  the write to flash happens on the OS's worker afterwards. Commit on a natural
  edge (the editor closing), not on every detent.
- **ABI changes break installed modules.** The loader refuses a module with any
  unbound import. Renaming an import means rebuilding and reinstalling every app
  that uses it.
