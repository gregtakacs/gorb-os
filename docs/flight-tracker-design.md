# Flight tracker — design and viability bench

Status: built on branch `feat/flights-bench` (rebased on `main` 404c480),
2026-11-01. The OS half (HTTPS client, feed service, parser, the scheduler's
handshake gate) and the app (`flights.orbapp`, behaviour kind 4) run on
orb-A; §6.5 records what was verified there.

## 0. What the owner asked for

- A flight-tracker app that does what the orb-os one does: nearby aircraft
  from a public ADS-B feed, drawn around the orb's home position.
- **Normal operation is a standalone orb.** No hub, no card activity, the clock
  on the glass. The feed polls in the background during that, and the clock
  must stay smooth while it does: no held beats, no hiccups.
- The feed is HTTPS. orb-os polls `http://api.adsb.lol`, which now answers
  `301 → https` (checked 2026-10-31), so the HTTP path is gone.
- No hub dependency for flight data. The hub is only on during maintenance.
- v1 draws with OS primitives: text, dots and direction markers. Sprites,
  themes and the tracker's own theme editor come later (§11).

## 1. What orb-os learned (and what carries over)

`../orb-os/src/adsb_client.cpp` and `config.h` are a record of every way this
feed fails on an ESP32-S3. The lessons, with what this design does about them:

| orb-os lesson | Here |
|---|---|
| A TLS handshake needs two ~16 KB internal buffers; the board couldn't give them (`-32512`), so it **dropped to plain HTTP**. | Not available any more (HTTP is 301). §4 is how TLS fits without the internal pool. |
| Parse while the body streams; never hold it. | `orb_readsb.c`, a byte-at-a-time state machine. Body size costs time, not memory. |
| Keep the connection alive; a fresh socket per poll exhausted lwIP's PCBs (TIME_WAIT). | `orb_https` keeps one connection alive and reconnects once if the server dropped it while idle. |
| Keep the nearest N, gated before any string allocation. | The sink keeps the nearest 24 as the body passes. |
| 4xx is policy, not a local fault: back off, never reboot over it. | Counted per poll; no watchdog reboot exists (§5). |
| adsb.lol's edges flap and rate-limit per edge; airplanes.live answers 403. | Two providers (adsb.lol, adsb.fi), selectable. A learned edge pool is a follow-up (§8). |
| Poll only when someone can see it. | Owner wants a background poll while the clock shows (§5 policy). |

## 2. The feed

| Provider | URL | Format | Notes (2026-10-31) |
|---|---|---|---|
| adsb.lol | `https://api.adsb.lol/v2/point/{lat}/{lon}/{nm}` | readsb, list under `"ac"` | HTTP is 301. Let's Encrypt ECDSA P-256 chain. |
| adsb.fi | `https://opendata.adsb.fi/api/v2/lat/{lat}/lon/{lon}/dist/{nm}` | readsb, list under `"aircraft"` | Google Trust ECDSA chain. Faster TLS from here. |
| airplanes.live | — | — | 403 to every request; asks you to email them first. |

Body sizes over Phoenix: 10 nm 9.8 KB, 20 nm 13 KB, 40 nm 16 KB, 80 nm 24 KB.
LAX at 100 nm is 57-66 KB (~120 aircraft). Gzip would cut 40 nm to 3.3 KB,
but the server then sends it chunked and inflating it needs a 32 KB window.
That's an option for later (in PSRAM), not v1.

Both servers negotiate TLS 1.2 ECDHE-ECDSA, which the current mbedTLS build
supports. Both chains end in roots that are in the bundled certificate store
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL`), so certificates are
verified. orb-os never verified them (`setInsecure()`).

## 3. Parsing — `main/net/orb_readsb.c`

A streaming reader fed in whatever pieces the socket hands over. It tracks
container depth so the nested arrays inside each aircraft (`mlat`, `tisb`,
`nav_modes`) are skipped, takes the dozen fields the tracker uses from the
aircraft object's own level, and emits one `orb_ac_t` per aircraft that
carried `hex`, `lat` and `lon`. It holds no buffer beyond one key and one
scalar, and has no IDF include.

`tools/host_readsb_test.py` compiles the shipped file and checks it against
Python's `json` reading of three real captures plus a synthetic edge fixture.
Each is fed whole, a byte at a time, and in 7- and 1024-byte pieces, plus two
truncations that must never report done. Result: 24 checks pass, and 6/6
mutations are killed (track/true_heading precedence, callsign padding, nested
fields, adsb.fi's key, string escapes, truncation reading as done).

## 4. Making TLS fit — the memory decisions

The internal pool is the constraint the whole design turns on. The numbers
below were measured on orb-A, idle, on the clock, before any of this:

| | free | largest block |
|---|---|---|
| internal, idle on the clock | 11,027 B | 3,968 B |
| internal, at the boot ladder's low point (post-wifi_start) | 2,163 B | 1,536 B |
| PSRAM | 7.5 MB | — |

TLS wants a 16 KB record buffer, a 4 KB send buffer and the handshake's
working set. Three decisions make it fit, and each was forced by a
measurement.

**4.1 TLS memory in PSRAM, chosen per task (`main/net/orb_tls_mem.c`).**
The first attempt used `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, which moves every
mbedTLS allocation to PSRAM. On the bench that broke the radio's own crypto.
The WPA supplicant's contexts moved too, and at boot the AES engine logged
`esp-aes: Failed to allocate memory` while bouncing them through internal DMA
RAM at the instant the pool bottoms out. The same squeeze then made the serial
door's driver install `abort()` in a lock init: the board crash-looped twice,
then came up without the door. So the policy became per task:
`CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC` with an allocator that gives PSRAM only to
tasks that registered (the feed's) and keeps everyone else, including the
supplicant, exactly as the shipped image had it.

A trap in that option, found by the first long run: under CUSTOM, IDF's
`esp_config.h` binds *nothing*, and mbedTLS falls back to libc `calloc`
until `mbedtls_platform_set_calloc_free()` is called. The allocator was dead
code. Everything up to 16 KB went to the internal pool: the 4 KB send buffer,
the certificate chain and the session state. A kept connection held 63 blocks
/ 15.5 KB internal (an internal-heap walk diffed before and after one poll),
and idle internal fell from ~26 KB to ~6-8 KB while the feed ran.
`orb_tls_mem_init()`, called in `app_main` right after the panic recorder,
installs the allocator. After that a kept connection holds 14 internal
blocks / 2.5 KB (`esp_tls_t` at 1,764 B plus lwIP's socket pieces), and a
handshake dips the internal pool by ~1 KB instead of ~14 KB.

**4.2 Statics that only the CPU touches, moved to PSRAM.** The TLS layer links
about 730 B of internal statics (mbedTLS's SSL tables and the crypto modules
it pulls in, plus the service's own). That alone took the boot low point from
2,163 B to 1,439 B, enough for the crash above even with 4.1 in place.
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` plus `EXT_RAM_BSS_ATTR` on
`orb_net.c`'s two httpd JSON buffers (11 KB + 6 KB) repays it. Those buffers
are CPU-only and copied into lwIP by the send, and their handlers run one at
a time. The same option also lets ESP-IDF place lwIP's and the Wi-Fi
libraries' zero-initialised statics in PSRAM, a placement it documents as
supported. Result:

| at post-wifi_start | free | largest |
|---|---|---|
| shipped v1.1.43 | 2,163 B | 1,536 B |
| this build | **32,711 B** | **17,408 B** |

Static DIRAM went from 247,235 B to 215,547 B. As a side effect, the
first-attempt `auth-fail (reason 202)` that every shipped boot shows (the
supplicant starved at that same instant) is gone, and the first association
succeeds. The extra headroom also widens the margin behind v1.1.43's
display-ring refund (`RING_NET_RESERVE`) for hub replies.

Risk carried: Wi-Fi/lwIP statics in PSRAM is the same family as failed fix 4,
which flipped `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` and lost DHCP. That option moves
the radio's *buffers*; this one moves *zeroed statics*, and the TX/RX buffers
stay internal. The bench counts lease losses and disconnects per arm (§6). One
`no lease` occurred on a boot of this build, on the `3a:98:b5:f3:e2:2d` mesh
node, which has done the same on shipped v1.1.41 boots. The long run is what
separates the two.

**4.3 The client — `main/net/orb_https.c`.** One `esp_http_client` per
caller, keep-alive, with certificates checked against the bundle. The body
streams to the caller's sink in 1 KB pieces off the caller's (PSRAM) stack.
Its own rx/tx buffers are 512 B because `malloc` under
`SPIRAM_MALLOC_ALWAYSINTERNAL` (16 KB) lands in the internal pool. Every call
samples the internal pool while it runs, so each poll reports what it cost.
This is what `orb_svc_t.http_get` (frozen in §4.3 of the platform plan,
stubbed `ESP_ERR_NOT_SUPPORTED`) will sit on when an app's `net` capability
is wired.

## 5. The feed service — `main/app/orb_flights.c`

- **Task:** `orb_flights`, priority 2, below lvgl (5), orb_net/dns (4) and
  the jobs worker (3), so a poll only ever runs on CPU time nobody drawing
  wanted. 8 KB stack and both aircraft tables in PSRAM. The task is created
  on first enable: a dial that never tracks pays nothing at boot.
- **Gates (a poll becomes a counted skip):** network down; a job running
  (install, store); the card lock held within the last 2 s (the display
  refunds its tile ring up to 1.5 s after the card is released); internal
  largest block under 1,536 B.
- **Poll:** GET, stream through `orb_readsb`, keep the nearest 24 by
  equirectangular distance, then swap the working table into the published
  one under a mutex. A failed poll leaves the last good picture published, and
  its age is part of the snapshot.
- **Readout:** `orb_flights_snapshot()` copies up to N aircraft plus the data
  age. The view consumes it; nothing else does.
- **Bench door:** `GET/POST /api/v1/debug/flights` (debug-gated, like
  `/api/v1/debug/panic`) for config, stats, the dial's tick-gap histogram and
  the snapshot. The shipped tracker configures itself through its app
  settings (§6.3), not through this.
- **Cadence (owner, 2026-11-01: avoid being throttled, but don't miss a
  close or interesting pass):**
  - *on the glass:* every 15 s;
  - *background:* every 60 s by default (1,440 requests a day);
  - *boost:* while the last picture holds something interesting, the
    background rate rises to 20 s, and it drops back once the contact leaves.
    Interesting means within 5 nm, or under 3,000 ft within 10 nm and closing
    on home, or an emergency squawk.

  A 60 s base doesn't miss approaches: a 250 kt aircraft covers ~7.7 km a
  minute, so one entering a 40 nm (74 km) circle is sampled ~9 times before
  it reaches home, and the boost then catches the close pass.
- **Refusals (built):** a 4xx rests that provider, starting at 1 minute and
  doubling to 15, and polls go to the other provider meanwhile. If both are
  resting the poll is a counted skip. A good answer forgives the provider.
  Transport failures don't rest anything: they're the network's, and the
  next poll is the retry. The owner has seen adsb.lol's own site 403 a page
  left open until it was reloaded. adsb.lol's API sends no rate-limit headers,
  and its terms say an API key (got by feeding) will be required "in the
  future"; §6.3 has a slot for one.
- **No reboot watchdog:** a feed that is down shows its age, and the poll
  retries.
- **Aging (from orb-os, v1 view):** merge each poll into a persistent table
  keyed by `hex`. A contact missing from one poll keeps its last position for
  60 s, drawn dimmed, then drops. Without this a routine missed poll makes
  aircraft blink out.

## 6. The app

### 6.1 How it fits the app model

A compiled behaviour module, `ORB_BEHAVIOUR_FLIGHTS`, like the colour picker:
an LVGL-drawn `orb_app_iface_t` behind a seed `.orbapp` (D36). The manifest
declares `net` (D19's install-time consent: an app that declared nothing never
opens a socket), `config` for its settings, and `time`. The wasm tier is the
wrong home for v1: it has one import today and no draw primitives.

The feed service is OS-owned, because the poll must run while another app (the
clock) has the glass. The app owns the settings and the view. Its `activate`
and `suspend` move the service between the visible rate and the background
rate.

### 6.2 The view (v1: text, dots, direction markers)

466 px round screen, north up, home at the centre.

- **Range rings:** two thin circles at ½ and full range, with a small range
  label on the outer one ("40 nm"). A north tick at 12 o'clock.
- **Each aircraft:** a 7 px filled dot coloured by altitude band (ground grey,
  <3k ft red, <10k amber, <20k lime, <30k green, above cyan, orb-os's legend)
  with a direction marker. The marker is a line from the dot along `track`,
  its length scaled by ground speed (6-22 px), so the screen reads as moving
  traffic. With no track the dot has no marker. Emergency squawks
  (7500/7600/7700) get a red ring.
- **Labels:** none on the radar (owner ruling 2026-11-01): the dots, their
  track lines and colours carry the picture, and the one aircraft named is the
  selected one, in the detail card at the knob's info level.
- **Header and footer text:** "12 aircraft" at the top. Data age at the bottom
  ("now", "25 s", "feed down 3 min" in amber).
- **Motion between polls:** dead-reckon each dot along track at its ground
  speed at 1 Hz. It reads as live, costs a few small invalidations a second,
  and the next poll snaps it back. A 10 s gap is visibly stale without it.
- **Knob** (owner ruling 2026-11-01): turning flips between apps, as on any
  screen. A push enters *zoom*: turning steps the range through 10/20/40/80 nm
  (the next poll uses it, and the rings rescale at once). A second push enters
  *info*: the nearest aircraft's detail card opens (callsign, type, altitude,
  ground speed, vertical rate, distance/bearing, its dot ringed), and turning
  steps the card through the drawn aircraft, wrapping at both ends. A third
  push goes back to flipping between apps. The header names the level
  ("zoom  40 nm", "3 of 24", in green). A long press is the shell's.

Drawing cost: at most 24 × (dot + line + optional label) LVGL objects, moved at
1 Hz, each invalidating a small area. That's well under what the clock's dial
pays every frame. The view draws nothing while suspended.

### 6.3 Settings (manifest-declared, C9 types)

| id | type | default | note |
|---|---|---|---|
| (home) | — | — | Not an app setting: the OS's home location (§6.4), read through a `location` capability. |
| `range_nm` | ENUM | 40 | 10/20/40/80 |
| `source` | ENUM | adsb.lol | adsb.lol / adsb.fi |
| `bg_poll_s` | ENUM | 60 | 0 (off) / 60 / 120 / 300: the background base rate; the boost (§5) rides on top |
| `boost` | TOGGLE | on | the 20 s rate while something interesting is near |
| `api_key` | STRING | "" | for when adsb.lol starts requiring one (declared as `key`; not sent yet) |
| `hide_ground` | TOGGLE | on | |
| `units` | ENUM | ft/kt | ft/kt or m/km/h (designed; not built) |

As shipped the container declares `range`, `source`, `bg`, `boost`, `ground`, `key`, and the caps `net, location`. The two `lat`/`lon` strings the first build carried went away with §6.4.

### 6.4 Home location replaces the time-zone pick (owner ruling, 2026-11-01)

The orb gets one **home location**, an OS setting rather than an app's. It
drives both the time zone and every app that needs a position, so the tracker
has nothing of its own to configure. Apps read it through a `location`
capability, declared at install like `net`: a position is personal, and D19's
consent rule applies.

- **Stored:** lat/lon (1e-6°, two int32), a short label, and the resolved zone
  (the IANA key and the POSIX string the clock already uses), in NVS beside
  today's tz key. Today's zone pick becomes an override under it, for the case
  where the derived zone is wrong.
- **From the hub (the main path):** the orb page offers *Use this browser's
  location* (Geolocation API) or *Pin on a map*. The hub turns the point into
  a zone. A browser fix comes with the browser's own zone
  (`Intl.DateTimeFormat().resolvedOptions().timeZone`), right when the
  computer is local. A map pin needs a point-to-zone lookup in the hub (an
  embedded boundary set, e.g. the `tzf` Go library). The hub sends lat/lon,
  label and zone to the orb in one config write. Existing zone-table rows keep
  their keys (the frozen NVS contract in `host_tz_test.py`); the hub sends the
  POSIX string for zones outside the table.
- **On the orb alone (menu):** *built, then removed (owner ruling
  2026-11-01: a character-per-press editor is a poor way to say where you
  live, and owners set it from the hub). The design as it stood:* two entry
  forms, digit by digit on the knob:
  - *decimal coordinates;*
  - *a full plus code* (8+2 characters). Open Location Code decodes offline
    in ~100 lines of C. Short codes ("XXXX+XX City") need a reference point
    the orb doesn't have, so they're refused with that reason.

  Zone and city then come from a small embedded city list: cities of 500k+
  people, ~1,200 rows of name, lat/lon and zone index, about 25 KB of flash.
  The nearest one names the place ("near Phoenix") and proposes its zone. The
  menu shows both for confirmation, because a border can make the nearest big
  city's zone wrong. That's the efficient way to put a city on it without a
  geocoder.
- **Not in v1 of the tracker:** the tracker ships reading home from the OS. If
  the location work lands later, the tracker's options page holds a temporary
  lat/lon until it does.

### 6.4.1 Home location as built (orb-A, 2026-11-01)

- **Record:** `main/net/orb_home.c`, one blob in `storage`/`orb.home`: lat/lon at 1e-6°, the place, the IANA zone it set and the exact tz string it wrote. "The zone follows home" is the comparison of that string with the zone in force, so a zone written past the home (`POST /api/v1/config`, the orb's own `/timezone` page) shows as not following it without a flag to drift.
- **Zone from a point:** the nearest row of `main/net/orb_cities.c`. That is 1,055 places of 500k+ plus 185 correction rows: places of 100k+ whose zone *rule* differs from their nearest big city's. Without them, Boise was "555 km E of Portland" in Pacific time; now it is "Boise", in Mountain time. The rules are derived from zoneinfo for 2027 and checked through glibc from 2026-11-01 for 800 days. This machine's tzdata (2026e) has BC, Alberta and Manitoba on permanent daylight time, and the table follows it. Asia/Jerusalem is the one approximate rule (standard time only), flagged `"exact":false` on the wire. Tables in the zone list keep their keys: a city whose zone is in the 62-row table stores that key ("US/Arizona"), any other stores the rule.
- **Hub (`GET/POST /api/v1/home`, Orb tab), two steps (owner ruling 2026-11-01).** There is no zone picker on the hub; the location drives the zone. Every source only *proposes* a point:
  - a city from the orb's own list, searched by name (`web/cities.json`, baked with `orb_cities.c` from the same TSV, so a city the hub offers is a city the orb names);
  - *Use this browser's location* (only on localhost or https);
  - a click on the map (Leaflet 1.9.4, vendored, BSD-2, over OpenStreetMap tiles; always shown);
  - typed coordinates or a full plus code.

  The orb previews it (`GET ?lat=&lon=` or `?code=`, which answers with the point, the place and the zone), the map pins that point, and the zone shows beside *Set home* with today's offset. A plus code is decoded by the orb, so the pin is the point Set stores. *Set home* posts the point; the orb takes the nearest city's zone with it. The browser's own zone is no longer sent. No `tzf`: the hub keeps its one dependency. Verified in headless Chrome against orb-A: city search, plus code, short-code refusal, coordinates, a map click past the antimeridian, Set, and a restore of the home that was there.
- **Dial:** nothing to set (owner ruling 2026-11-01). The HOME page (a character-per-press editor for coordinates or a plus code) and the zone list it overrode were both removed from the menu; the top page is AP, DISPLAY, PERMISSIONS, APPS, ABOUT. ABOUT still names the zone in force. The tracker's footer says "set home in the hub (Orb tab)" until one is set. The menu's saved position moved to NVS key `pg2`, because the page ids after the zone page shifted down one; an old record is ignored once.
- **Tracker:** reads home through `svc->home` (cap `location`), re-reads when `orb_home_gen()` moves, and drops its contacts on a move: their offsets are from the old home. Bench: knob-typed `8559CWXG+9C` set Phoenix and US/Arizona; an API move to LAX re-centred within one poll; back to Phoenix cleared the screen at once and refilled on the next poll.
- **Cost:** +35 KB of image (city table ~23 KB). The 2 MB slot now has **35 KB free**. Internal RAM is unchanged: the record and the editor's state are in PSRAM, and only the mutex is internal.

### 6.5 As built (orb-A, 2026-11-01)

- **Module:** `main/flights/flights_view.c`, behaviour kind 4 in `orb_app.c`.
  The seed container `tools/orb-manager/apps/flights.orbapp` (4,096 B) comes
  from `make_factory_apps.py`, declares `net` and the eight settings of §6.3
  (home as `lat`/`lon` STRINGs until §6.4 lands), and ships in the hub's
  library beside the clock and the picker.
- **Drawing:** one 466×466 RGB565 canvas in PSRAM. Rings, dots, markers and
  emergency/selection rings are written straight into its pixels; only the
  range figure goes through LVGL's draw layer. Four
  label objects (header, footer, detail card) plus the canvas: the LVGL pool
  (22 KB, ~20 KB peak) has no room for a widget per aircraft.
- **Labels:** the first build labelled the nearest six airborne contacts,
  placed clear of each other on the round glass. Removed on 2026-11-01 (owner
  ruling) with the placement code (`fm_place_label`): the radar is dots only,
  and the info level's card names the selected aircraft.
- **Merging:** a contact table of 32 in PSRAM. Each poll is merged by `hex`,
  a contact the latest poll missed is drawn dimmed, and one missing for 60 s
  drops. Dead reckoning runs at 1 Hz, capped at 90 s.
- **Knob:** turn flips apps; push → zoom (turn steps 10/20/40/80 nm, saved
  at disarm) → push → info (turn steps the card through the drawn aircraft by
  `hex`, nearest first, wrapping; `fm_sel_step`) → push → back to flipping
  apps. The view disarms itself on the third push; the router reads
  `flights_view_armed()` each poll. Bench on orb-A through `/api/v1/screen`:
  every level and the turn after the third push landing on the next app.
  The long press is the shell's. The options
  page cycles Range, Source, Background, Boost and Ground on a press and shows
  Home read-only. That page was unreachable until 2026-11-01: the menu lists
  an app's page only when its iface declares `opt_arm`, and the tracker's did
  not, so no Flights row ever appeared. Fixed (the hook re-reads the settings
  on open), seen on orb-A's glass, and `host_app_registry_test.py` now fails
  any iface that offers rows without the hook.
- **Feed control:** `activate`/`resume` set the visible rate (15 s);
  `suspend` drops to the background rate (60 s, 20 s boost). The feed only
  runs when home is set and `net` is granted.
- **Settings from outside:** `POST /api/v1/apps/cfg?id=&key=&value=` writes
  one declared setting through the service table's own gate (type, range,
  quota, revocation). The row's `orb_svc_cfg_gen` bumps, and the view
  re-reads on its service hook. The hub allows and fakes the door, and
  `/api/v1/debug/flights` is on the contract test's refused list.
- **Verified on orb-A** (screen captures through `/api/v1/fb`, knob through
  `/api/v1/screen`):
  - install from the card, then a reboot to initialize the row;
  - background polling with the clock on the glass, the boost engaging over
    Phoenix;
  - the view with labels, dimmed contacts, the detail card (type, altitude,
    vertical rate, speed, track, distance, bearing) and the 80 nm zoom;
  - the settings door's refusals (undeclared key 404, out-of-range 400);
  - resume-to-clock across a reboot.
- **Handshake gate bench** (`flights_bench.sh … gate`): 159 handshakes in 20
  min, 0 rung moves, 33 report-seconds forgiven, 159/159 polls ok, radio
  clean.
- **Seen, not caused by the tracker:** after any app visit (the menu does it
  too) the clock re-seeds its rung and settles at 7-8 fps instead of a fresh
  boot's 10, because a promote needs 25% slack it never shows. That's the
  scheduler's re-seed path, outside this work; noted for whoever owns
  `orb_sched`.

### 6.6 Route on the info card (2026-11-01)

- **Source:** adsbdb.com, `GET https://api.adsbdb.com/v0/callsign/<cs>`, free,
  no key, ~700 B, ~0.15 s on a kept connection. The feed's own providers
  carry no route, and adsb.lol's `/api/0/routeset` answered every request
  that day with an empty 201.
- **When:** only at the info level, only for the selected aircraft, and only
  for an airline callsign (`fm_route_callsign_ok`: three letters, a digit, at
  most seven characters). A registration has nothing to look up, and adsbdb
  took 7.7 s to say so on the bench.
- **How:** `orb_flights_route()` never blocks. It queues an unseen callsign in
  a 16-entry cache (PSRAM), and the feed task looks it up between polls on its
  own kept connection, under the same gate as a poll (network, jobs, card,
  internal heap). A wake for a route does not trigger a feed poll. A 404 is
  "unknown"; another 4xx rests route lookups from a minute doubling to 15 min,
  as the feed rests a provider. The connection closes when the tracker leaves
  the glass. `/api/v1/debug/flights` reports `route` counters.
- **Plausibility (`fm_route_plausible`):** adsbdb answers with a schedule, not
  a flight plan, and over DFW (American's hub) most answers are another
  flight under the same number. The card shows the route only when the
  aircraft is near an end, or within max(150 km, 15% of the route) of the
  great circle and not past either end; and, with a track, heading the right
  way: leaving the origin (away, or climbing), arriving at the destination
  (toward, or descending), toward the destination between them. Bench over
  Fort Worth, 2026-11-01: 68 airline callsigns in 40 nm, adsbdb knew 57, 12
  passed (MEX-DFW descending, PVR-ORD overhead at FL370, DFW-LHR climbing
  out, UPS SDF-DFW descending). Those hidden included "CLE-DFW" on a jet
  climbing east out of DFW, and LGA-ORD at 6,000 ft over Dallas.
- **Card:** a line "PHL → PBI" (IATA, ICAO when there is none) under the
  callsign, absent while the lookup runs and whenever the route is unknown
  or implausible.
- **Cost:** ~4.5 KB of flash (25.2 KB left in the slot); one more kept TLS
  connection while the tracker is on the glass and a card was opened.

### 6.7 adsb.lol API key and the ban (2026-11-01)

- **What happened:** api.adsb.lol answered `403 banned` to the owner's public
  IP (the website still loaded in a browser). Likely cause: a day of bench
  probing from the dev machine on top of the orb's 15-20 s polls; the orb and
  the machine share the address. adsb.lol's README: limits are dynamic, "if
  you get 4xx errors, you are doing something wrong", keys are coming (earned
  by feeding).
- **Back-off:** a 403 now rests a provider 1 h doubling to 12 h (a 429 or
  other 4xx keeps 1 min to 15 min); any good answer forgives it.
  `/api/v1/debug/flights` reports `rest_s` per provider, so the ban is watched
  from the orb, never by probing adsb.lol. The orb's source was switched to
  adsb.fi; adsb.lol is the fallback.
- **The key:** an OS credential, not an app setting (the container's
  `key` STRING tops out at 31 characters and is still unused). NVS
  `storage`/"orb.flights" (`lol_key`, `lol_hdr`), read on first use, never
  read back. `GET/POST /api/v1/flights/key` (`{"key":..,"header":..}` or
  `{"clear":true}`; GET answers set, header, last four), allowed and faked in
  the hub, with a form on the Orb tab. Sent only on the adsb.lol client, as
  one header, default `x-api-key`: adsb.lol has not published the header
  name, so the owner names it when the key arrives. Setting a key forgives
  adsb.lol's rest so the next fallback (or a switch of Source back) tries it.
  Bench on orb-A: refusals, set, header override, survival across a reboot,
  clear. Not yet seen on the wire: that needs adsb.lol, and the key.

## 7. Later in the design chain

- **Theming:** planned in §11. Nothing in v1 blocks it: the view's colours
  and sizes come from one table that a resolved theme can later fill.
- **Alerts over the clock:** an emergency squawk or a low overflight could
  raise a complication on the clock face. The feed already runs during the
  clock. That needs a policy ruling first.
- **Gzip** (§2) and a **learned edge pool** with per-edge cool-down (orb-os
  §1).

## 8. Open risks

- **Rate limits and edge flaps** (orb-os's 2026-08 measurements). v1 has two
  providers and backs off on 4xx. An edge pool is the follow-up if adsb.lol
  flaps here too.
- **Certificate store aging:** the bundle is in the firmware. A root
  rotation means an OTA, which is the normal update path anyway.
- **Keep-alive idle close:** the servers close idle connections, so some
  polls reconnect (a full handshake). Each one is counted, and their cost is
  in §9.
- **Internal RAM the client keeps:** `esp_http_client`'s structures come from
  `malloc` and so from the internal pool: ~2.3 KB while the client lives, plus
  ~2.5 KB while a kept connection is open (measured, §4.1). The feed frees
  both when it is turned off.
- **Host reachability after a boot:** on the shake-out the host could not
  reach the orb for ~4 min after a boot that touched one mesh node before
  settling on another, while the orb was associated and serving. That's
  network-side; the bench driver retries.

## 9. Viability bench

`tools/bench/flights_bench.sh` runs one serial capture from a fresh boot,
standalone (no hub), Aviator theme on the glass. It switches timed arms over
the debug door and takes a `/api/v1/debug/flights` snapshot at every boundary.
`tools/bench/flights_report.py` prices each arm:

- **dial:** the per-second `clock N fps` line (mean, p5, min); RUNG moves; and
  the scheduler's tick-gap histogram (`orb_sched_jank_snapshot`), as the share
  of ticks whose start-to-start gap exceeded 1.5×, 2× and 3× the beat.
- **feed:** polls ok/fail/skip, handshakes, open and total ms, bytes, and the
  internal heap low-water each poll saw. Also the dial's gaps counted *during*
  polls, so a cost the feed causes can't hide in an arm-wide average.
- **radio:** disconnects, no-lease drops, `esp-aes` failures, aborts.

Arms: `off` 15 min → `ka10` (keep-alive, 10 s, 40 nm) 20 min → `hs10` (a full
handshake every poll) 15 min → `fi10` (adsb.fi) 5 min → `stress5` (LAX 100 nm,
~57 KB, a handshake every 5 s) 10 min → `off2` 10 min.

### 9.1 Results — orb-A, Aviator, standalone, two full runs (2026-10-31)

Run 2 is on the final build (allocator installed, §4.1). Run 1 measured the
build where mbedTLS still allocated from the internal pool; its dial and radio
columns stand, and they were taken under *more* memory pressure.

| arm (run 2) | min | dial fps mean / p5 | ticks >1.5x beat | rung moves | polls ok | handshakes | poll total p50 / max | body | internal min during polls |
|---|---|---|---|---|---|---|---|---|---|
| off | 15 | 10.40 / 10 | 0 of 9,001 | 0 | — | — | — | — | — |
| ka10 (keep-alive, 10 s) | 20 | 10.39 / 10 | 0 of 12,008 | 0 | 114/114 | 2 | 459 / 6,369 ms | 7.4 KB | 18,987 B |
| hs10 (handshake every poll) | 15 | 9.84 / 9 | 0 of 8,211 | **1** (10→9) | 71/71 | 71 | 2,540 / 5,296 ms | 7.6 KB | 18,859 B |
| fi10 (adsb.fi, keep-alive) | 5 | 9.79 / 9 | 0 of 2,704 | 0 | 30/30 | 1 | 248 / 1,342 ms | 11.0 KB | 12,027 B |
| stress5 (LAX 100 nm, handshake every 5 s) | 10 | 9.77 / 9 | 0 of 5,405 | 0 | 69/69 | 69 | 3,425 / 9,107 ms | 34.6 KB | 11,783 B |
| off2 | 10 | 9.77 / 9 | 0 of 5,411 | 0 | — | — | — | — | — |

- **The dial never held a beat.** Across both runs, 0 ticks out of ~86,000
  ran past 1.5x the beat, during polls or between them, in every arm. That
  includes 140 handshakes and 57 KB bodies every 5 s.
- **Every poll succeeded.** 284/284 in run 2 and 277/277 in run 1, all 200,
  no skips. **Radio clean** in both: no disconnect, no lease loss, no
  `esp-aes` failure, no abort in 160 minutes of polling.
- **Keep-alive is the design.** On a kept connection a poll is ~250-460 ms
  (adsb.fi ~250, adsb.lol ~460). A cold handshake is ~2.2 s of background
  time. adsb.lol kept the connection across 10 s polls (2 handshakes in 20
  min).
- **Internal RAM:** idle on the clock ~26 KB free with the feed off, ~21 KB
  with it on (the client plus a kept connection, §4.1). Lowest seen during a
  normal poll 18.9 KB; during the 57 KB stress 11.8 KB. The pool's low-water
  since boot over the whole run was 6.9 KB. Before this work, the idle pool
  was ~11 KB with no feed at all.

**The one effect: a handshake can cost one rung.** In each run the scheduler
demoted the dial 10→9 fps once. Both times it was during a TLS handshake,
in the second the certificate was validated, with the flush average creeping
from ~80 to ~100 ms over the seconds before. The poll task runs below the dial
(2 vs 5), so this is not CPU priority. The likely mechanism is the two cores
contending for PSRAM: the handshake's maths runs on PSRAM buffers, and the
dial's paint reads PSRAM. The demotion then persists, because a promote needs
25% slack the dial never shows. Keep-alive arms (3 handshakes in 25 min)
caused none; handshake-every-poll arms (140) caused one per run. It's a step
in the steady frame rate, not a hiccup: tick gaps stayed under 1.5x the beat
throughout.

### 9.2 Verdict and what goes into the build

**Viable.** A standalone orb can poll the HTTPS feed in the background with
the clock on the glass: no held beats, every poll good, the radio
undisturbed, and more internal RAM to spare than the shipped image had idle.
The design commits to:

1. keep-alive always, with handshakes counted (the bench's `hs10` and
   `stress5` are worst cases, not the plan);
2. a background rate no faster than 30 s, so a server that does drop idle
   connections costs a handshake per 30 s at most;
3. the scheduler rides out handshakes (owner ruling 2026-11-01; built). While
   `orb_https` runs a TLS handshake, and for 1.5 s after, report-seconds under
   the rung don't count toward a demote (`orb_sched_bg_burst`). With
   keep-alive a handshake is rare, and the frame rate is back the moment one
   ends, so nothing needs to stay lowered. `host_sched_rung_test.py` lints the
   gate's order. Bench: §9.3.

### 9.3 Hub soak with the feed running (orb-A, 2026-11-01, `53efde5`)

The feed was pinned at a 10 s background poll through the debug door, six
times the shipped rate, while the hub-side tools ran against it.

| test | result |
|---|---|
| `store_check quiet` | store 8,093 ms, no panic, nothing staged |
| `store_check status` | 8,236 ms, clean |
| `store_check previews` | 8,477 ms and 8,480 ms (two runs), clean |
| `scan_preview_stress`, 25 rounds, five conditions | no stalls, every `/status` answered |

v1.1.43 shipped at 8.1 s for the same store. During each 8 s slot write the
feed's gate turned polls into skips ("a job is running", "the card is
busy"): no TLS crossed the radio while the slot was written, and polls
resumed after the reboot. In `previews` mode a full handshake poll ran while
the preview stream was going.

The five clean stress conditions: fresh boot with the feed off; feed at
10 s; feed at 10 s plus a second client polling every 5 s; straight after a
store; the menu on the glass. Each gave quick 503s while scanning (the
v1.1.42 refusal) and whole 107,652 B previews in 0.8-3.7 s. `main` at
404c480 (no tracker, no TLS) gave the same picture on the same orb.

**Open: one degraded run, not reproduced.** The first stress of the day ran
after three store reboots, with the feed pinned and the tracker visited
just before. Six of its 25 previews hung for the full 40 s, `/status` went
unanswered in four rounds, and internal free swung between 38 and 78 KB.
The state persisted into the next run (the feed switched off, no reboot) and
cleared on reboot. It did not come back in the five runs above, each built
to recreate one ingredient. The 78 KB rounds are the display's tile ring on
loan, which the menu run reproduces without stalls, so the ring explains the
memory pattern but not the hang. The trigger is unknown, and it is not shown
to be the tracker's: `main` was sampled once, the branch five times. Next
step: an overnight soak under serial capture, hub-like traffic, the feed at
its shipped rate, and reboots in between, to catch it with a log.

## 10. Owner rulings, 2026-11-01

1. **Handshakes:** ignore the drop during them; built (§9.2 item 3).
2. **Wi-Fi/lwIP statics in PSRAM:** this isn't fix 4.
   - *Fix 4* was `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, which moves the radio's and
     lwIP's *packet buffers* to PSRAM. It was tried twice (plan §17 on
     2026-10-14, and §41 fix 4 on 2026-10-31) and failed both times: httpd
     wouldn't fit, then lost leases and AES/SHA bounce failures.
   - *This one* is `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`, which moves those
     libraries' *zero-initialised statics* and leaves every packet buffer
     internal.

   It is already on `main` as `aaa577e` (adopted from this branch), and both
   77-minute runs here ran on it with the radio clean.
3. **Cadence:** a lower background rate with a boost for close passes, plus
   4xx backoff and failover (§5).
4. **Home location** replaces the zone pick and feeds the tracker (§6.4).

## 11. Theming plan (2026-11-03)

The tracker gets themes: drawn planes, a radar arm and a background, set in
an editor of its own. This section is the plan. Nothing in it is built yet.

### 11.1 Owner rulings, 2026-11-03

1. **Apps are atomic.** An app knows nothing of any other app, so that someone
   can write a new app without entanglement with the existing ones. The
   tracker's editor is **not** a tab in the clock's Watch Composer. It lives
   in `apps/flights/editor/` and ships inside `flights.orbapp`, the way the
   clock's ships in `clock.orbapp` (`"editor"` in `app.json`, built by
   `tools/make_factory_apps.py`). Anything the two editors share comes from
   the OS or the SDK, never from one app reaching into the other.
2. **The editor is small.** A handful of elements and little variance, so it
   is a fraction of the Watch Composer, not a copy of it.
3. **The order is art, then firmware, then editor** (§11.4).
4. **Altitude colour is baked: one plane copy per band.** The sprites are
   small and cheap to bake (7 shapes × 7 bands at ~40 × 40 px is under
   250 KB a family against D29's 4 MB), so the stage needs no tint.
5. **The wedge's trail is baked into the arm sprite.** Baking costs the
   editor nothing: it renders the trail at whatever length, spread and
   colour the author set, and the bake freezes that. `make_art.py`'s
   `arm_wedge()` takes them as parameters for the same reason.
6. **Two of three: shape, colour or detail.** A plane at 37–42 px carries two
   of them, never all three; all three is busy and illegible. Each family
   says which two it carries (§11.3).
7. **No coloured rims.** A band-coloured rim round the metal planes was tried
   and rejected: round an icon this small it is a blur. The metal families
   take the band as a light tint instead, at 30% (approved: "the perfect
   hint/hue change").
8. **Altitude colour can be turned off.** Then every plane is drawn in its
   family's own colour (the metals untinted) and ground traffic is drawn at
   45% opacity so it recedes; with the app's Ground setting off there is no
   ground traffic to dim. It is a theme option, set in the editor, since it
   changes what is baked: two copies per shape (airborne, ground) instead of
   one per band.
9. **Every family has two forms.** A plane per category, which carries
   shape, or one detailed plane for every category, which gives up shape and
   spends the budget on detail (shading, a soft shadow, flap and elevator
   lines, intakes, a lit edge). Paper's two forms came first: the paper
   fleet and the shaded paper dart. A shape per aircraft model under each
   category (an A380 apart from a 737) is an option for later, not part of
   either form.

### 11.2 What the firmware has to offer

- The app SDK canvas (`orb.h`) draws fill, pixel, ring, disc, line and text.
  It has no image call, which is why v1 is dots and heading lines.
- The stage (`orb_stage.h`, plan D50) is the OS's renderer for any app that
  ships a theme: a full-screen plate plus rows drawn as rotated sprites, each
  posed per frame with `orb_stage_pose(i, ang_10, ox, oy)`. The tracker maps
  onto it as: background → plate; radar arm → one rotating row; planes → a
  pool of rows, each posed at a contact's position and track. The model keeps
  32 contacts (`FM_MAX_TRK`); the pool is likely 12–16.
- The parser keeps the ICAO type designator (`orb_ac_t.type`) but not the
  ADS-B emitter category (`category`, A1 light … A5 heavy, A7 rotorcraft).
  Picking a silhouette per aircraft wants the category parsed: one short
  field, against a type-designator lookup table.

**What the code answers (read 2026-11-03).** Step 2 is firmware work, not
only app work: four of these need the OS to change.

1. **One live theme on the device.** The store keeps a single slot with a
   single section (`orb_theme.c`: "single-slot boot", `sole_sec()`), and
   `orb_app_wasm.c` gives the stage only to the app whose slug matches
   `orb_theme_live_slug()`. Any other app's `stage_open` answers `base = 1`.
   So installing a flights theme today would take the clock's theme off
   the glass. D49 (themes picked per app) and D50 (the stage opens the
   calling app's own theme) say each theme-aware app has its own; the
   residency does not do that yet. Step 2 needs a theme resident per
   theme-aware app, or the tracker cannot have a theme while the clock
   keeps one.
2. **A row is one fixed image.** `clk_row_t` carries one `clk_sprite_t`,
   and `stage_pose(i, ang_10, ox, oy)` sets only angle and offset. Only the
   carriage row (`car_row`, `CLK_TOURB_MAX` frames) animates through
   frames, and it is the one row of its kind. With baked band copies (7
   shapes × 7 bands), a plane row has to switch image as its contact
   changes category or band. That needs a new call, a frame index per
   row (`stage_frame(i, f)` or a frame argument on `stage_pose`), over a
   bank of frames like the carriage's.
3. **Rows can share pixels.** A row names its image by `frame0`, an index
   into the section's sprite catalogue (`orb_clock3.c`, `fill_sprite`).
   The reader checks the catalogue is contiguous and every row's plane is
   inside it; nothing read refuses two rows naming the same plane. So a
   pool of plane rows can share one bank of images, and the bake stores
   each image once.
4. **24 rows in all.** `CLK_ROW_MAX` is 24 (a `theme_schema` constant, so
   an ABI number). Arm plus plane pool fits: 1 + up to 16, with room left.
   Raising it is not needed.
5. **The app's canvas would hide the stage.** The stage builds its layers
   on the app's own screen (`s_scr = orb_ui_screen(...)`) in `wasm_init`,
   before `orb_init` runs, so the app's widgets are created after them and
   sit above. The SDK canvas is opaque RGB565 (`orb_ui_cv_create`), so
   today's full-screen canvas covers the scope. With a theme the app drops
   the canvas: labels stay `ORB_UI_LABEL` widgets, and range rings become
   `ORB_UI_BOX` widgets with `ORB_RADIUS_CIRCLE`, a border and no fill.
   **Measured on Bronze (theme-pool-plan §4, P4):** the direct compositor
   (L4) paints the theme's pixels into the panel mirror for every box it
   redraws, whatever LVGL's stacking says, so the canvas must go, and the
   card and header labels over the theme have to be checked against L4's
   boxes when the tracker moves onto the stage.

**The wedge.** It is the largest sprite the stage would turn every frame
(~120 × 184 px at a 40° trail; a clock hand is long but thin), and the
owner has seen a large rotating sweep slow the frame before. Rotation cost
follows the sprite's area, so the trail's spread is the lever: the editor
caps it, and the bench measures frame time at the cap before any theme
ships it. `arm_wedge()` already takes the spread as a parameter.

### 11.3 The art

Original SVGs, drawn for gorb-os. The orb-os public theme library (snapshot
2026-11-03, 48 plane images, 7 image arms) is reference for style only;
nothing is traced from it, and many of its themes carry no licence.

- **Planes**, one family per style, each with: light single, turboprop,
  business jet, airliner, heavy, helicopter, and a generic fallback. They
  draw at 37–42 px on the 466 px glass (§11.5), so: bold silhouettes, a firm
  outline, no fine panel lines. Nose up, centred, pivot at the centre.
- **Radar arms**: a phosphor wedge with a fading trail, a needle with its
  arbor, a brass pointer, a drafting line. Each records its pivot. A trail
  is baked into its arm (§11.1 ruling 5).
- **Backgrounds**: phosphor scope, aviation chart with compass rose, brass
  instrument face, blueprint grid. Range rings are not baked in: their
  spacing follows the Range setting, so they stay drawn live.

**Families** (owner's picks, 2026-11-03: modern, modern silver, vintage
brass, blueprint wireframe, paper). A plane is drawn from named parts
(body, tail, engines, props, canopy, markings); a family is a shape set
plus a renderer, and under ruling 6 each renderer carries two of shape,
colour and detail. Each family has a pairing, a background and arm it is
shown with first:

| Family | Carries | Shapes | Band colour | Pairs with |
|---|---|---|---|---|
| Modern | shape, colour | modern | a flat silhouette in the band | scope, wedge |
| Modern silver | shape, material | modern | a 30% tint over polished metal | scope, needle |
| Vintage brass | shape, material | vintage: elliptical-wing fighter, twin radial transport, twin-boom early jet, four-engine propliner, four-engine bomber, autogyro, compass pointer | a 30% tint over polished brass | brass, pointer |
| Blueprint | shape, colour | modern | a wireframe outline; the background shows through | blueprint, drafting |
| Paper | shape, colour | paper planes: broad glider, square-wing, slim dart with winglets, classic dart, wide notched delta, whirligig, folded chevron | the paper itself, flat, one faint crease | chart, needle |

Each family's second form, one detailed plane for every category (ruling 9):

| Family | Carries | The one plane |
|---|---|---|
| Modern | colour, detail | a shaded airliner in the band colour |
| Modern silver | material, detail | a polished four-engine jet, 30% tint |
| Vintage brass | material, detail | a twin radial transport, props spinning, cowls ringed, 30% tint |
| Blueprint | colour, detail | the airliner as a full technical outline in the band colour |
| Paper | colour, detail | the paper dart: a soft shadow, far wing and folds in shade, the near edge lit |

How the band was carried changed twice on review at true size. Tail,
engine and roundel markings alone could not be read; a rim round the
whole plane was a blur (ruling 7). Set one's grey engines and canopy went
too: modern is now a flat silhouette. The canopy and markings geometry is
still in `make_art.py` for a family that trades colour for detail.

Sources live in `apps/flights/art/`: `make_art.py` holds the geometry and
writes the SVGs, `art.json` (sizes, pivots) and `review.html`, a scope with
simulated traffic that draws at the orb's true size (§11.5).

### 11.4 Order

1. **The art.** One plane family, two arms, two backgrounds first, to settle
   the look before it is multiplied; then the rest of §11.3. **Done,
   approved by the owner 2026-11-03:** five families in two forms, four
   arms, four backgrounds, in `apps/flights/art/`.
2. **Firmware.** **Done 2026-11-04** (the OS side in theme-pool-plan P1–P5; the app and
   the bake in §11.6). The tracker on the stage. The OS side first (§11.2): a
   theme resident per theme-aware app, so the clock keeps its own; a frame
   index per row, so a plane row switches image by category and band.
   Then the app side: the plane pool and arm row, the category parse, rings
   as box widgets in place of the canvas, the card's fourth line gone
   (§11.5). Bench-checked on orb-A with hand-placed assets before any
   editor exists, the wedge's frame time measured at the editor's cap.
3. **The editor** in `apps/flights/editor/`, once the format step 2 reads is
   fixed (D28: the firmware's resolver is the source of truth). Panels:
   background (image or colour; ring count, colour, opacity), radar arm
   (image, pivot, speed, trail), planes (family, form: per category or one
   detailed plane, size, category → shape),
   altitude colours (on or off, ruling 8; the band table `main.c` holds
   today), labels (card font
   and colours). The preview runs **simulated traffic**, a dozen contacts at
   mixed tracks, altitudes and categories crossing the scope under a turning
   arm, so every shape and band is seen without waiting for real traffic.

### 11.5 Legibility at true size (owner, 2026-11-03)

The panel is a 1.75 in round, 466 px across: about 266 ppi. `review.html`
draws the face at that physical size on a monitor (its dpi set from a
bank-card outline; the browser's pixel ratio is divided out), and the owner
judged the first set there.

- **Planes: 3.5–4.0 mm, 37–42 px.** The review's default is 38 px. At
  30 px light and turboprop were too close to tell apart.
- **14 px is the smallest text**: the footer's size. The app's text is "as
  small as we'd ever want to go". Baked numerals were 11 px, a speck at
  true size; they are 15 px bold now.
- **The info card drops its last line** (distance and bearing): the map
  already shows both. It is four lines in the review. `main.c` still
  prints five; that change goes in with step 2.
- **Text colour belongs to the theme.** The card's white is unreadable on
  the paper chart, so header, card, footer and selection-ring colours are
  theme properties per background, not constants in `main.c`.
- **Nothing baked sits where the card draws.** The card covers the bottom of
  the range ring in info mode, so neither background puts a numeral at
  south. Planes still pass under the card; where the card sits, and
  whether it gets a backing, is a step-2 layout question.
- The look of set one is approved: the modern family, the two arms, the two
  backgrounds.

### 11.6 The tracker on its theme, as built (2026-11-04)

- **The bake** (`apps/flights/art/bake_theme.py`). The art is rasterised with cairosvg from
  `make_art.py`'s renderers; blueprint's outline is built from rasters (the silhouette
  stroked wide less the silhouette), because cairo ignores SVG masks. Each theme is
  assembled by the OS's own stage bake (baker@1's `GorbBake.stage3`, under node):
  - the plate;
  - the arm: slot 0, declared a 15 fps sweeping seconds row, so the stage runs at that
    rate; the app poses it itself;
  - a pool of **15** plane rows (slots 1–15) naming one bank of 49 frames, 7 shapes × 7
    bands, in `make_art.BANDS` order (`--colour off` gives the family's own colour, with
    the ground at 45%);
  - a 232 px preview.

  It bakes one theme per family with its pairing (`--form single`, `--trail` too) into
  `apps/flights/build/themes/`, about 0.8 MB each.
- **The app** (flights v1.2.3). With a theme of its own, the tracker draws through the
  stage and opens no canvas:
  - the arm turns once in 4 s;
  - the nearest 15 contacts ride the pool, each row's frame its shape × 7 + band, turned
    to its track and offset to its position; unused rows are hidden;
  - rings, the home dot, the selection and emergency rings and the range figure are
    box and label widgets;
  - the stage ticks it every frame, so it sets no beat, and labels change only when
    their text does.

  The shape comes from the ADS-B emitter category, now parsed: A1 light, A2 turboprop by
  ICAO type else business jet, A3/A4 airliner, A5 heavy, A6 business jet, A7 rotorcraft,
  B1/B4 light, else generic. The card lost distance and bearing (§11.5). With no theme,
  the canvas as before.
- **Fifteen planes, not sixteen.** An app's layer table holds 16 rows (`ORB_LAYER_MAX`).
  Raising it would cost the internal heap the commit's job array. The claim also stopped
  counting the preview, which every theme carried into it and which is never drawn.
- **L4 and the app's widgets.** L4 writes the theme into the panel mirror over whatever
  LVGL drew in the boxes it redraws. The sweep marks a large box every frame, and on
  Bronze the header, rings and footer were painted over. An app with widgets of its own
  over its theme now renders through LVGL, which stacks them (`orb_sched_set_l4_veto`,
  set by the stage on the way onto the glass); the clock keeps L4. On the LVGL path a
  row's widget must start hidden, or a row the app never showed stands at its P0.
- **Benched on Bronze, `v1.2.7+f9e12db`, Flights Modern stored through the hub's send.**
  At 80 nm: 11–12 live aircraft drawn with their shapes and bands, turned to their tracks,
  under the sweeping wedge, with the header, rings, range figure and footer on top;
  **12–14 fps** through LVGL with the 30° wedge, no panic, no fault. The owner's range
  (10 nm) and the clock on the glass were put back after.
- **Owed.** The text and ring colours are the app's fixed ones, not the theme's (§11.5);
  a stale contact is not dimmed on the theme path (frames cannot dim); and L4 for a themed
  tracker would need the app's widgets drawn into the L4 composite.
