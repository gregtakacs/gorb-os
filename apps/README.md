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
    {"id": "detail", "type": "enum", "label": "Detail",
     "choices": ["brief", "full"], "default": 0},
    {"id": "place1", "type": "place", "label": "Place 1",
     "help": "Shown after home. Pick a city or type coordinates."},
    {"id": "key", "type": "secret", "label": "API key", "host": "api.example.com",
     "help": "Where to get one.", "links": [{"href": "https://example.com/keys", "text": "Get a key"}]}
  ]
}
```

| Key | Meaning |
|---|---|
| `slug` | the app's id: lowercase a–z, digits and `_`, at most 23 characters. It names the card file, the settings namespace and the slot |
| `title` | what the dial shows |
| `caps` | the owned capabilities the app uses: `power`, `net`, `sd_read`, `themes`, `sound`, `location`. The declaration is the consent (D19): an import the app did not declare answers a refusal, and the owner can revoke any of them on the dial |
| `settings` | up to 16, each with a type and a label; the OS draws them on the dial and in Orb Manager ([Settings](#settings)) |
| `period_ms` | the fastest beat the app may ask for, at least 1000. Leave it out and the app never ticks |
| `memory_kb` | the module's linear memory, 64 (default) or 128 |
| `shelf` | in this repo only: `true` puts the app on the hub's shelf; `false` builds and checks it but keeps it off |
| `editor` | the app's theme editor: names a folder of the app (`"editor": "editor"`) holding `editor.json` (`entry`, the `runtime` ids it needs, and `schema` — the design schema the editor reads, required when it lists `theme@1`) and the editor's files. `orbsdk.py` packs that folder into the container's editor region, which Orb Manager opens from the Themes page (the clock's region is built by `tools/make_clock_region.py`) |

Kind, serial and the header's version all come from the SDK and `version.txt`, so
there is nothing else to declare.

## Theme designs, schemas and migrations

A theme starts as a design document the app's editor writes: an envelope naming the app,
the release that saved it and the schema it holds, and the art inside (a `.orbdesign`
file is exactly that, with its art). `editor.json`'s `schema` must equal the schema the
editor itself reads — the session refuses to start otherwise — and a design opens in any
editor of the same schema, whichever release saved it: only a design from a *newer*
schema is refused (ruling 21). So any change to what a design holds raises the schema by
one and adds one pure step `n → n+1` to the editor's migrations, and the latest editor
carries every past design forward.

Each app keeps one sample design per past schema, in `editor/_fixtures/schema-<n>.orbdesign`
(ruling 36: every sample at or below the app's own schema travels in the editor region under
that same name, so a hub holding only the container can run the check; `_selftest/` beside it
stays home). Before raising `schema`, save the current design with `orbsdk.py
check-migrations --save-current` and commit it; the
build refuses a raised schema with no fixture for any past schema, naming the first gap. `orbsdk.py
check-migrations` runs the OS's migration runner over the fixtures in the app's own packed
editor (it needs Node and Chromium), so deleting a migration step fails the check, and the
template's CI workflow runs it before packing.

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

## Settings

An app declares its settings and the OS does the rest: it stores them, draws the app's
page in the dial's menu and on Orb Manager's App settings page, and serves them to the
hub at `GET /api/v1/apps/settings?id=<slug>`. An app has no settings UI of its own, on
the knob or in its theme editor. Each entry in `settings`:

| Field | For | Meaning |
|---|---|---|
| `id` | all | the key: a–z, 0–9 and `_`, at most 15 characters |
| `type` | all | `int`, `number`, `enum`, `multi`, `toggle`, `text`, `place` or `secret` |
| `label` | all | what the dial and the hub call it, at most 23 bytes. Required |
| `help` | all | a sentence or two for the hub, at most 480 bytes |
| `links` | all | `[{"href", "text"}]` shown under the help, at most 3 |
| `show` | all | `both`, `dial`, `hub` or `none`. Default `both`, and `hub` for `text`, `place` and `secret` (nobody types on a knob) |
| `min`, `max` | int, number | the range, in the setting's own units |
| `step`, `unit` | int, number | the knob's step (default 1) and a suffix such as `"°"`, `"%"` or `" s"` |
| `decimals` | number | 1 to 3: the value is stored as an integer times 10^decimals |
| `choices` | enum, multi, toggle | the option labels in value order. A label may be `{"km": …, "mi": …, "nm": …}`, picked by the orb's distance unit. A toggle's two are its off and on (default `"off"`, `"on"`) |
| `default` | all but secret | int and number: a value; enum: an index; multi: a list of indices; toggle: 0 or 1; text and place: a string |
| `host`, `header` | secret | the one host the OS may send it to, and the header it rides (default `x-api-key`) |

How each type reads in the app:

- `int`, `enum` (the choice's index), `toggle` (0 or 1), `number` (scaled by
  10^decimals) and `multi` (a bitmask, choice *i* is bit *i*) with `orb_cfg_get`.
- `text` and `place` with `orb_cfg_get_str`, at most 31 characters. A `place` is
  picked on the hub from the city list or typed as coordinates, and stored
  `Name|lat,lon` with two decimals.
- `secret` never: see [Secrets](#secrets).

On the dial, an app's page lists the settings shown there in manifest order. A press
edits a number or a choice with the knob; a multi opens into its choices, each one a
press to switch. If any setting is hub-only, the last row reads "More on Orb Manager".
`show: "none"` keeps a setting out of both, for a bench switch an owner should never
see. Whichever side changes a setting, `orb_cfg_gen` moves, so re-read your settings
when it does.

An app built against an SDK runs on that OS release and every later one, as long as
the imports it uses are still bound. The loader refuses a module with an import the
running firmware does not bind, so moving `sdk.txt` forward is how an app picks up
new calls.

From OS v1.2.8 that refusal names the missing call on the app's row ("needs orb.x, which
this firmware lacks: update the firmware"), in the orb's Apps menu, `/api/v1/status` and the
hub alike, however the app arrived (hub, or a file copied over USB and installed from the
menu); older firmware refuses the same way but says only "failed". An app that needs a newer OS than its
imports alone show (the flight tracker needs v1.2.8's theme pool and app-picked frames)
names the oldest release in `app.json`: `"os_min": "v1.2.8"`. The SDK writes it into the
stamp in the app's editor region, so it needs an `"editor"`, and Orb Manager refuses to
install the app on older firmware, saying to update the firmware first.

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

## The knob

The orb has one input, a knob that turns and presses. Every app uses it the same way,
so someone who has learned one app has learned them all. The rules in full are
[docs/app-design-rules.md](../docs/app-design-rules.md); this section is how to follow them.

| Gesture | Means | Who handles it |
|---|---|---|
| Turn | move along the app's **one dial** | the app |
| Press | the app's **one action** | the app |
| Press-and-turn | the app's **second axis**, if it has one (radar's zoom) | the app, if it exports `orb_push` |
| Hold | leave the app for the app list | the OS, always |

**Design your app as one dial centred on a home view.** Home is what someone glancing at
the orb wants to see: the weather now, the radar. Put one kind of thing clockwise of home
and another anticlockwise, so whatever is wanted most is a few clicks either side of home
and never at the far end of a line. Weather puts the hours clockwise and the days
anticlockwise; Flights puts the aircraft nearest-first clockwise and farthest-first
anticlockwise.

- **Turning never needs a press first.** No modes, no "press to edit, then turn". If the
  app has two things to scrub, it has two sides of the dial.
- **A press does one thing, always the same thing**, wherever the dial stands: the next
  place, start or pause. Keep the dial's position across it, so the press compares
  rather than resets.
- **The ends stop.** Nothing wraps, so an overshoot never jumps to the other side.
- **Home is sticky.** A fast spin stops on home, so one flick back from anywhere is home.
- **Idle goes home.** After 20 s without a turn the app shows home again, and so does
  coming back to the app.
- **Never use the hold.** It is the way out of every app, and it must always work.
- **Clockwise is forward**: later, more, next, closer. Radar's dial turns clockwise into
  the forecast, and its zoom clockwise closer.
- **A dial that changes a value is a value dial**, not a view: a timer's remaining time.
  Its change stays, nothing returns on idle, and the ends are the value's own limits. An
  app has one dial, a view dial or a value dial.
- **Settings are declared, not drawn.** Anything that is not a dial or its one action is
  a setting in app.json, and the OS draws it ([Settings](#settings)), never the app.
- **A click must show at once.** The OS draws a frame straight after every knob event, so
  redraw in `orb_input` and the change is on the glass in about 40 ms.

**In code.** Call `orb_knob_dial()` once from `orb_init`: from then on the app owns
every turn and press while it shows, with no arming. Turns arrive accelerated, so a slow
click is `±1` and a quick spin is `±2` or `±4` a click. `sdk/orb_dial.h` keeps the dial
for you: the stops, the sticky home and the idle return.

```c
#include "orb_dial.h"

static orb_dial_t s_dial = { .lo = -5, .hi = 23, .idle_ms = ORB_DIAL_IDLE_MS };
static int64_t up_ms(void) { return orb_uptime_us() / 1000; }

ORB_EXPORT("orb_init") int32_t orb_init(void) { orb_knob_dial(); /* ... */ return ORB_OK; }
ORB_EXPORT("orb_activate") void orb_activate(void) { orb_dial_home(&s_dial); }
ORB_EXPORT("orb_tick") void orb_tick(uint32_t now) { if (orb_dial_idle(&s_dial, up_ms())) redraw(); }
ORB_EXPORT("orb_set_armed") int32_t orb_set_armed(int32_t armed)
{
    if (!armed) orb_dial_home(&s_dial);        /* left: come back home */
    return armed;
}
ORB_EXPORT("orb_input") void orb_input(int32_t delta, int32_t pressed)
{
    if (pressed) next_place();                 /* the one action */
    if (delta) orb_dial_turn(&s_dial, delta, up_ms());
    redraw();                                  /* s_dial.pos: 0 home, >0 clockwise, <0 anticlockwise */
}
```

When the dial's ends depend on data (how many hours are left, how many aircraft are
drawn), call `orb_dial_range(&s_dial, lo, hi)` before turning; it keeps the position
inside them. When what sits at a position can move (Flights re-sorts its aircraft every
poll), keep the selection by identity and set `s_dial.pos` from it before each turn, as
`apps/flights/main.c` does in `dial_sync()`. `orb_knob_dial` is an OS v1.2.11 import: an
older OS refuses an app that calls it. The template (`orbsdk.py new`) starts as a dial.

**Press-and-turn.** Export `orb_push` and the OS sends it every click turned while the
knob is held down, one by one and unaccelerated, instead of to `orb_input`; exporting it is
the opt-in, with no manifest key. Show the choice as it moves and land it on the let-go,
`orb_push(0)` (from OS v1.2.27, 400 ms after the knob comes up). A press or a plain turn
while choosing lands it too. `apps/radar/main.c` (`orb_push`, `zoom_step`) is the model.

```c
ORB_EXPORT("orb_push") void orb_push(int32_t delta)
{
    if (!delta) { choice_lands(); redraw(); return; }    /* let go */
    s_want = clamp(s_want + delta, 0, N - 1);           /* clockwise: next, closer */
    redraw();                                           /* show the choice in the header */
}
```

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
| knob | `orb_knob_dial`, and [sdk/orb_dial.h](sdk/orb_dial.h) (header only) | free (OS v1.2.11) |
| menu | `orb_armed` | free |
| JSON | `orb_json_begin/next/skip` | free |
| network | `orb_net_request/info/read/header/free`, `orb_net_cred`, `orb_net_get` | `net` |
| sound | `orb_sound_play/stop/active` | `sound` |
| location | `orb_home`, `orb_home_gen` | `location` |
| units | `orb_units`, `orb_units_gen`, and the `orb_*_from_*` and `orb_*_label` helpers | free (OS v1.2.10) |
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
- `orb_input`: the knob. With `orb_knob_dial` (see [The knob](#the-knob)) every
  turn and press arrives here while the app shows. `delta > 0` is clockwise;
- `orb_set_armed`: `0` when the app is left (a hold, or another app shown); an app
  using `orb_knob_dial` goes home here. Without `orb_knob_dial` it is the older
  model: a press arms the app, and only then does `orb_input` hear the knob.

There are no options hooks: the app's menu page is the OS's, drawn from its declared
settings ([Settings](#settings)). Themes are picked in the OS Themes menu (D49).

### Off the glass

An app off the glass does nothing for the screen. It's off the glass from
`orb_suspend` until `orb_activate` or `orb_resume`. That covers another app being
shown, the menu, and the app carousel opened over it (from OS v1.6.1 the carousel
suspends the app under it and resumes it if the owner comes back to it). Off the
glass the app's display isn't visible, and every pixel it draws, every map it renders
and every frame it fetches for the screen costs the app on the glass. Map renders
read the SD card, and while the card is read the display lends it its DMA memory and
drops frames.

Best practice, which the shipped apps follow:

- **Keep a visible flag.** Set it in `orb_activate` / `orb_resume`, clear it in
  `orb_suspend`, and gate all display work on it in `orb_service`.
- **Draw nothing off the glass.** No `orb_cv_*`, `orb_ui_*` or `orb_stage_*` calls
  from `orb_service` while the flag is clear. Mark what changed (a dirty flag) and
  draw it in `orb_resume`.
- **Open no maps off the glass.** From OS v1.6.1, `orb_map_open` answers
  `ORB_MAP_OFF_GLASS` to an app off the glass. Treat that as "not now", not as a
  failure: open the view again in `orb_resume`. The OS also frees an app's map
  views when it leaves the glass.
- **Fetch only what the background needs.** Data for a notification, an alarm or a
  chime keeps flowing (`orb_net_*` from `orb_service`). Data only the screen shows
  (a radar frame, a picture) waits until the app is back. The radar app's
  `net_step(s_visible)` is the model.
- **Keep sounds and alerts in `orb_service`.** `orb_tick` runs only on the glass. A
  chime, a strike or an `orb_attention` that must happen whatever the owner is looking
  at belongs in `orb_service`, with the on-glass path in `orb_tick` (the clock does
  both, and its service stands down while it is on the glass).
- **Expect a quick return.** A carousel the owner opens and closes is a suspend
  and a resume a few seconds apart. Keep state across `orb_suspend` (don't free your
  data), so `orb_resume` redraws what you had rather than starting over.

### Secrets

A `secret` setting (for example an API key) is declared with the one host it may
be sent to:

```json
{"id": "key", "type": "secret", "label": "API key", "host": "api.example.com"}
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

### Pacing an arm (OS v1.6.1)

The OS paces a theme's sweep (a radar's arm, a tracker's) no faster than its live layer's
steps cost: on a storm, frames come every ~160 ms instead of every 66. `orb_frame_ms(0)` is
the period now and `orb_frame_ms(1)` the theme's own. Turn the arm by its speed times
own / now, integrating the angle rather than computing it from the clock, so a change of pace
never makes it jump: it then moves as far a frame as at the theme's pace, which looks
smoother and leaves less of the glass to repaint.

### Kept bodies on the card (OS v1.6.1)

`orb_net_keep(slot)` moves a landed body into one of 24 OS-owned slots, 768 KB of PSRAM
an app, and `orb_cv_png8` draws from that. When an app keeps more than fits, such as a
radar loop on a rainy day, it can park bodies on the SD card and load them back before it
draws them:

- `orb_blob_park(slot)` hands the body to the card. Its PSRAM goes once the card holds it.
  It answers `ORB_ERR_NOT_SUPPORTED` with no usable card (none, or shared over USB), and the
  body stays in PSRAM. Keep the old behaviour for that case.
- `orb_blob_load(slot)` brings a parked body back. It answers `ORB_ERR_NO_MEM` past the
  768 KB. Park something else first.
- `orb_blob_state(slot)` is `ORB_BLOB_EMPTY`, `_RAM`, `_CARD` or `_MOVING`. Only `_RAM` can
  be drawn: `orb_cv_png8` answers `ORB_ERR_NOT_FOUND` for the rest. `orb_blob_len` answers
  in every state.

Best practice:

- **Keep a small window in PSRAM**: what is on the glass and what comes next. Park the rest
  as it lands, and ask for loads a step or two before you need them. A 200 KB body loads in
  about 0.3 s.
- **Wait, don't skip**: if the next body is still `_MOVING`, hold the picture you have.
- **Expect parks to be slow**. A park is the card's last priority: it stands aside while a
  carousel is open, an app is switching, a theme copies, or an upload runs, and a 200 KB
  write takes about a second. Nothing on the glass depends on it.
- **Bodies are cache, not storage**. They go 15 minutes after the app leaves the glass, at
  uninstall, and at every boot, card copies included.

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
