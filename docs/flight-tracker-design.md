# Flight tracker — design and viability bench

Status: design + bench, 2026-10-31, branch `feat/flights-bench`. The OS half
(HTTPS client, feed service, parser) is built and measured on orb-A; the app's
view is designed here and not yet built.

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
  themes and a theme editor matching the clock's come later (§7).

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
- **Policy, proposed:** while the tracker is on the glass, poll every 10 s
  (orb-os's cadence, inside adsb.lol's guidance). While it is not, poll at the
  background rate, 30 s by default, set in the app's options. No reboot
  watchdog: a feed that is down shows its age, and the poll retries.
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
- **Labels:** callsign and flight level ("SWA3291 FL035") beside the nearest
  six, placed on the side away from the centre. Not on all 24: at 40 nm over a
  city they collide.
- **Header and footer text:** "12 aircraft" at the top. Data age at the bottom
  ("now", "25 s", "feed down 3 min" in amber).
- **Motion between polls:** dead-reckon each dot along track at its ground
  speed at 1 Hz. It reads as live, costs a few small invalidations a second,
  and the next poll snaps it back. A 10 s gap is visibly stale without it.
- **Knob:** turning steps the range through 10/20/40/80 nm (the next poll uses
  it, and the rings rescale at once). Pressing cycles a detail card through
  the nearest aircraft (callsign, type, altitude, ground speed, vertical rate,
  distance/bearing), highlighting that dot. A long press is the shell's.

Drawing cost: at most 24 × (dot + line + optional label) LVGL objects, moved at
1 Hz, each invalidating a small area. That's well under what the clock's dial
pays every frame. The view draws nothing while suspended.

### 6.3 Settings (manifest-declared, C9 types)

| id | type | default | note |
|---|---|---|---|
| `home_lat`, `home_lon` | STRING | unset | Decimal degrees. Required: the orb only knows its time zone, not where it is. Set from the hub's app editor (a map pick) or the options page. |
| `range_nm` | ENUM | 40 | 10/20/40/80 |
| `source` | ENUM | adsb.lol | adsb.lol / adsb.fi |
| `bg_poll_s` | ENUM | 30 | 0 (off) / 15 / 30 / 60 / 120: the background rate |
| `hide_ground` | TOGGLE | on | |
| `units` | ENUM | ft/kt | ft/kt or m/km/h |

## 7. Later in the design chain

- **Theming:** blip sprites per altitude band, a ring/plate background,
  label fonts, declared as asset classes (D23) so a theme can carry a tracker
  look. The theme editor gains a tracker page matching the clock's. Nothing
  in v1 blocks this: the view's colours and sizes come from one table that a
  resolved theme can later fill.
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
3. a fix for the rung step before the tracker ships, one of: the scheduler
   discounts a report-second during which the feed ran a handshake (the feed
   already knows when), or the promote path re-tries the higher rung after a
   quiet interval instead of requiring 25% slack. The second also helps any
   other one-off burst, so it's the recommendation. Owner's call (§10).

## 10. For the owner

- **The rung step (§9.1):** pick the fix: a handshake-aware demotion gate, or
  a promote that re-tries after a quiet interval (recommended).
- **Wi-Fi/lwIP statics in PSRAM (§4.2):** shipping this is the biggest memory
  change here (+30 KB internal at boot, the reason-202 first-join failure
  gone). Two 77-minute runs were clean. It's the same family as fix 4, so it
  wants a hub soak (`store_check.sh`, the preview stress) before it ships,
  even without the tracker.
- **Background polling:** confirm 30 s default and whether "off" belongs in
  the options (§6.3).
- **Home location:** set from the hub's app editor (a map pick) or typed on
  the options page.
