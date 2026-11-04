# Writing a gorb-os app

An app is a wasm32 module that runs inside the OS's wasm3 interpreter. It ships in
the code region of an `.orbapp` container. The clock, the HSB editor and the flight
tracker in this directory are all built this way, and the firmware holds none of
their code. The full design record is
[docs/app-platform-plan.md](../docs/app-platform-plan.md) §43.

An app does not have to live in this repo. The SDK builds the same container from any
directory, and every gorb-os release attaches it as `orb-sdk-<version>.tar.gz`.

## An app is a directory

```
my_app/
  app.json        the manifest: slug, title, caps, settings, beat
  version.txt     the app's own release, v<MAJOR>.<MINOR>.<PATCH>
  *.c, *.h        the sources; every .c file at the top level is compiled
```

`app.json`:

```json
{
  "slug": "weather",
  "title": "Weather",
  "caps": ["net", "location"],
  "period_ms": 1000,
  "settings": [
    {"id": "units", "type": "enum", "min": 0, "max": 1, "default": 0},
    {"id": "key",   "type": "secret", "host": "api.example.com"}
  ]
}
```

| Key | Meaning |
|---|---|
| `slug` | the app's id: lowercase a–z, digits and `_`, at most 23 characters. It names the card file, the settings namespace and the slot |
| `title` | what the dial shows |
| `caps` | the owned capabilities the app uses: `power`, `net`, `sd_read`, `themes`, `sound`, `location`. The declaration is the consent (D19): an import the app did not declare answers a refusal, and the owner can revoke any of them on the dial |
| `settings` | up to 16, each `int`, `enum`, `toggle`, `string` or `secret` (below) |
| `period_ms` | the fastest beat the app may ask for, at least 1000. Leave it out and the app never ticks |
| `memory_kb` | the module's linear memory, 64 (default) or 128 |
| `shelf` | in this repo only: `true` puts the app on the hub's shelf; `false` builds and checks it but keeps it off |
| `editor` | in this repo only: names the editor region a tree tool builds (the clock's theme editor) |

Kind, serial and the header's version all come from the SDK and `version.txt`, so
there is nothing else to declare.

## The SDK

```
apps/sdk/           (the same files at the top of orb-sdk-<version>.tar.gz)
  orbsdk.py         build, pack, install, scaffold
  orb.h             the whole OS surface: imports, types, entry points
  orb_stage.h       the stage, for an app that ships a theme
  clk_types.h       theme record layouts (a byte copy of main/art/clk_types.h)
  orb_rt.c          runtime glue linked into every app (orb_logf, libc shims)
  libc/             string.h, stdio.h, stdlib.h, math.h: what C you get
  binds.json        every orb.* import the OS binds, with its signature
  template/         the starting point `orbsdk.py new` copies
```

```
orbsdk.py new <dir> [--slug S] [--title T]   scaffold an app from the template
orbsdk.py build [<appdir>...]                compile to <appdir>/build/<slug>.wasm
orbsdk.py pack [<appdir>] [-o DIR]           build, then write <slug>.orbapp
orbsdk.py install <appdir|file> --orb HOST   pack, upload to the orb's card, install
orbsdk.py inspect <file.orbapp>              read a container back
```

The compiler is clang targeting `wasm32 -mcpu=mvp` (no proposals), `-O2 -Werror`,
freestanding, with no libc beyond `sdk/libc`. Override it with `ORB_CLANG`. You need
clang and lld (`wasm-ld`). The build refuses:

- any import outside module `orb`;
- an `orb` import that `binds.json` does not list, or one whose signature differs.

Builds are deterministic: no timestamps and no paths, so unchanged sources rebuild
byte-identical. Inside this repo, every build also checks that `binds.json` matches the
firmware's bind table (`s_binds` in
[main/app/orb_app_wasm.c](../main/app/orb_app_wasm.c)) and that `clk_types.h` matches
`main/art/clk_types.h`. After changing the bind table, run
`python3 apps/sdk/orbsdk.py binds --write`.

## An app in its own repo

```sh
curl -fsSL https://github.com/gregtakacs/gorb-os/releases/download/v1.2.6/orb-sdk-v1.2.6.tar.gz | tar -xz
python3 orb-sdk-v1.2.6/orbsdk.py new weather --title Weather
cd weather && make                      # build/weather.orbapp
make install ORB=192.168.1.50           # onto an orb (it reboots to load it)
```

The template carries everything a repo needs:

- a `Makefile` that fetches the SDK release named in `sdk.txt` into `.sdk/`;
- a hello-world `main.c`;
- a `.gitignore`;
- a GitHub workflow that builds on every push and attaches the `.orbapp` to a release
  when you push a tag equal to `version.txt`.

Anyone can then import that `.orbapp` in Orb Manager (Apps → Import an app).

An app built against an SDK runs on that OS release and every later one, as long as
the imports it uses are still bound. The loader refuses a module with an import the
running firmware does not bind, so moving `sdk.txt` forward is how an app picks up
new calls.

## Folding an app into gorb-os

An app moves into this repo unchanged:

1. Copy its directory to `apps/<slug>/`. The directory name must equal the slug.
2. Add `"shelf": true` to its `app.json` if it should ship with the hub.
3. Run `python3 tools/make_factory_apps.py`. It builds every app here through
   `orbsdk.py`, writes the shelf ones into `tools/orb-manager/apps/`, and the hub
   compiles them in.
4. Commit the directory and the container.

`make_factory_apps.py --check` (part of `make check`) holds the committed containers
to their sources. `--unreleased DIR` writes the `"shelf": false` apps to DIR for a
bench install.

Each app carries its own release in `version.txt`. The tree's root `version.txt`
stamps the firmware only: an OS release leaves every app's stamp alone, and an app
release needs no firmware build. One stamp names one set of bytes, by construction: the
header's stamp is `version.txt` plus the container's own content hash
(`v1.2.16+9ec5595`), the build tail the firmware fills with its git SHA. Any byte change
is a new stamp, and a rebuild of unchanged sources is the same one. Move `version.txt`
for a release (once per push), not for every commit. The `+hash` never orders: two
builds of one release are neither older nor newer, so the hub serves an app's editor
from its shelf only to an orb running that exact build, and the Apps face offers
Update when the shelf holds another build of the orb's release.

## Install

`orbsdk.py install` does both steps. By hand: upload the container to the card
(`POST /api/v1/sd/app?overwrite=1`), then install it
(`POST /api/v1/apps/install?id=<slug>`). You can also install from the dial's
APPS page, or import the file in Orb Manager. Installing a code app reboots the orb,
because modules load only at boot.

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
menu (D49).

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
