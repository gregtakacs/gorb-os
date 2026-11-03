# SNTP time sync + user-settable timezone

**Status: APPROVED — plan finalized with owner, not started.**
Self-contained implementation plan; pick it up cold. Repo:
`/home/greg/docker/Projects/gorb-os` (public `gregtakacs/gorb-os`, branch
`main`, repo-local git identity already set to the noreply address).
Build env: ESP-IDF **v5.4** at `~/esp/esp-idf` —
`. $HOME/esp/esp-idf/export.sh && idf.py build`; flash with explicit port
`idf.py -p /dev/ttyACM0 flash` (auto-detect wanders into /dev/ttyS* and
looks like a brick — it isn't). If the tree was moved since the last build,
`idf.py fullclean` first (CMake cache pins absolute paths).

## Why the dial is wrong (verified 2026-10-08, all file:line current)

- `main/clock/clock_view.c:80` — `#define CLOCK_EPOCH 1700000000` (Nov 2023).
  `compose_now()` at **:980-993** is the *only* time source:
  `epoch + (esp_timer_get_time() - s_t0_us) / 1e6` → `localtime_r`. No other
  input exists. The dial shows Nov-2023 UTC forever.
- Nothing anywhere calls `setenv("TZ",…)`/`tzset()` → `localtime_r` renders UTC.
- Two defects, not one: **setting** the time and **interpreting** the zone.
  Both must ship; syncing without a zone gives correct UTC on a dial that looks
  5–8 hours off to the owner.
- `main/net/orb_net.c` already has every hook needed: `on_ip()` at **:190-202**
  (`IP_EVENT_STA_GOT_IP`), NVS handle `s_nvs` opened at **:779**
  (`storage` partition, namespace `"orb.net"`, keys `ssid`/`pass`), and the
  read-until-content_len pattern at **:416-443** (`h_wifi_join`).
- Public surface today: `orb_net_start/is_up/is_provisioning/ip_str`
  (`orb_net.h:38-47`). Whole module body sits in
  `#if CONFIG_ORB_NET_ENABLE` (`orb_net.c:48`) with stubs at **:944-958** —
  new public functions need stubs there too or the `=n` build breaks (this
  exact class of bug already bit once; see commit f5367fd's message).
- `sdkconfig` today: `CONFIG_LWIP_SNTP_MAX_SERVERS=1` (needs 2 servers →
  bump to 3 in `sdkconfig.defaults`; `sdkconfig` itself is gitignored and
  regenerated — everything must come from defaults).
- API: `components/esp_netif/include/esp_netif_sntp.h` —
  `esp_netif_sntp_init(esp_sntp_config_t*)`, `_start()`, `_sync_wait()`.
  Config fields that matter: `.start=false` (we start on lease),
  `.smooth_sync=false` (stepped is honest; slew is for audio),
  `.wait_for_sync=false` (never block; the `sync_cb` drives state),
  `.servers = ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com")`,
  `.num_of_servers=2`. Periodic re-sync already handled by
  `CONFIG_LWIP_SNTP_UPDATE_DELAY=3600000` (hourly, IDF default — don't touch).

## Owner decisions already made (do not relitigate)

1. **TZ is device config, never baked in.** It must be settable from the
   config page now and from the future PWA / on-device options menu later —
   the *endpoint is the contract*, the inline page is just its first client.
2. **No `esp_timezones` now.** `localtime_r` without zoneinfo speaks POSIX
   TZ strings only, so the device carries a named-zone→POSIX table and also
   accepts **free-form POSIX strings** — every zone reachable today without
   a firmware change. When the real PWA or `esp_timezones` lands later, the
   wire format doesn't change.

## Commit A — `net: SNTP sync on lease; the dial switches source when it lands`

**`main/net/orb_net.c`**
- `esp_netif_sntp_init()` with `start=false` during `orb_net_start()`
  bring-up; `esp_netif_sntp_start()` called from `on_ip()` (:190) so sync
  runs only with a lease + DHCP resolver. Idempotent across reconnects
  (verify against the drop test below).
- `static volatile bool s_time_synced`, set in `sync_cb` alongside
  `ESP_LOGI` of the received epoch (second-granularity from the callback is
  fine — the dial's sub-second fraction comes from `esp_timer` regardless).
- New public `bool orb_net_time_synced(void)` + `false` stub under the
  `#else` at :944.
- `/api/v1/status` (`h_status` :210, buffer `char json[512]` at :213 — body
  ~300 B today, new fields ~110 B, still inside; the overflow-→500 guard at
  :255 is the backstop — re-check after edits) gains
  `"synced":true|false,"epoch":N`, epoch from `time(NULL)`. Verification is
  then a curl, no serial cable — same discipline as the OTA work.

**`main/clock/clock_view.c`**
- `compose_now()` (:980): `orb_net_time_synced() ? time(NULL) :
  CLOCK_EPOCH + delta` — the existing fallback stays for the unsynced case
  (router outage keeps ticking from last-known-good, never 1970). Same
  `esp_timer` sub-second fraction either way, so the sweep stays smooth.
- Update the `:79-80` comment ("Replace the base with SNTP sync when WiFi is
  added"): WiFi's in; reword to describe the dual source.
- `#include "orb_net.h"` (already on the path via `main/CMakeLists.txt`
  `INCLUDE_DIRS "net"`).

**`sdkconfig.defaults`**: `CONFIG_LWIP_SNTP_MAX_SERVERS=3`, with a comment
saying why two servers (one hostname down must not cost the clock its time).

## Commit B — `net: timezone becomes device config — NVS, settable from the page`

- **Storage:** `storage`/`"orb.net"` NVS (handle `s_nvs` :779), new key `tz`.
  Inherits the documented survival of `nvs_flash_erase()` (`orb_net.h:19`
  comment — extend it to mention `tz`).
- **Boot order:** load + apply `tz` inside `orb_net_start()` right after the
  NVS open (:779), *before* wifi bring-up, so the first time render is
  already zoned. Unset → UTC, and status says so — never silently wrong.
  Accepted artifact: a few early frames may compose in UTC before net starts;
  not worth reordering main.c for.
- **Apply:** `setenv("TZ", tz, 1); tzset();` — the next `localtime_r` in
  `compose_now()` picks it up; no reboot.
- **Validation:** ≤64 chars, printable ASCII only; reject control chars,
  `"` and `\` (keeps the JSON echo safe). Store only if valid, else 400 with
  reason.
- **API:** `POST /api/v1/config` body `{"tz":"…"}` → validate,
  `nvs_set_str`, `nvs_commit`, apply immediately, echo result.
  `h_wifi_join`'s body gains an **optional `"tz"` field** so a fresh orb can
  be zoned during provisioning, before it ever reaches the LAN. Both follow
  the :416-443 read-until-content_len + 413-on-oversize pattern — a partial
  first read here would store a truncated TZ permanently.
  `json_str()` (the existing quoted-value parser, host-tested) handles the
  body shape.
- **Status gains** `"tz":"<string>","tz_offset":-300` — minutes east of UTC,
  measured at the current instant via a `localtime`/`gmtime` diff through
  `timegm` (newlib's raw `tm_gmtoff` is unreliable for UTC-anchored structs —
  measure now so DST is honored). This echo is the safety valve: a
  malformed-but-printable POSIX string degrades silently to UTC, and the page
  can ask "showing −5:00 — right?" instead of the owner staring at a wrong
  dial wondering.
- **Named-zone table** (compile-time `{name, posix}` pairs): UTC; US Eastern
  `EST5EDT,M3.2.0,M11.1.0`; US Central `CST6CDT,M3.2.0,M11.1.0`; US Mountain
  `MST7MDT,M3.2.0,M11.1.0`; US Pacific `PST8PDT,M3.2.0,M11.1.0`; US Arizona
  `MST7` (no DST); Europe/London `GMT0BST,M3.5.0/1,M10.5.0`; Europe/Central
  `CET-1CEST,M3.5.0,M10.5.0/3`; Japan `JST-9`. POST accepts a table name OR a
  raw POSIX string. `GET /api/v1/config` exposes the table so the future PWA
  renders the picker from the device, not a hardcoded copy.
- **UI:** inline config page gains a `<select>` (table names + "custom…" →
  text input), pre-selected from status's `tz`, POST on change, then display
  the resulting `tz_offset` echo. Provisioning form gains the same control
  (optional field).
- No auth changes: still plain LAN HTTP (documented accepted risk).

## Verification (each commit, before committing — repo rule: every
intermediate builds standalone)

1. `idf.py build` at the commit's exact tree state: `EXIT=0` and
   `grep -cE "main/.*warning:"` == 0. Once per commit, also build with
   `CONFIG_ORB_NET_ENABLE=n` to prove the stub story still compiles (that
   class of bug already bit once — f5367fd's message has the story).
2. `rm sdkconfig` before the final verification build: everything must
   regenerate from `sdkconfig.defaults` (the gitignored-`sdkconfig` trap bit
   the OTA work).
3. `idf.py -p /dev/ttyACM0 flash`, power-cycle, then LAN checks against
   `http://<orb-ip>/` (read the DHCP lease from the serial log or your
   router — never write the literal address into a tracked file):
   - `/api/v1/status`: `"synced":true` within ~2 s of the serial
     `associated` line; epoch within ~2 s of laptop `date +%s`.
   - Dial vs laptop `date` — zone correctness is the one thing curl can't
     prove; `tz_offset` echo is the proxy, a glance at the dial is real.
   - `POST /api/v1/reboot` → `synced` returns within seconds, `tz` persists
     (proves NVS + boot-order apply).
   - Unsynced behavior: reboot with the router's WAN down (or DNS blocked) →
     dial keeps ticking from last-known-good + `esp_timer`, never 1970,
     recovers correct when the network returns.
   - Negative paths, `POST /api/v1/config`: 80-char → rejected; control
     chars/quotes → rejected 400 with reason; valid free-form garbage
     (e.g. `XX3`) → accepted, degrades to UTC, **visible in status** — not a
     silent brick.
   - Provisioning end-to-end once with `tz` in the join body.
4. Once on commit B's final tree: ship it via **OTA**
   (`POST /api/v1/ota` + reboot) — the rollback `img:"valid"` handshake must
   land again (re-proves display health for free).
5. History hygiene: commit style `subsystem: what; detail`, bodies carry the
   measurements; no SSIDs/LAN-IPs/personal email anywhere in messages or
   diffs (this leaked once and forced a history rewrite — keep the record
   clean the first time).

## Explicitly not doing (decided — don't re-open)

- `esp_timezones`/IANA zoneinfo (~470 KB) — table + free-form POSIX covers
  every zone today.
- SNTP server list as user config — two public servers is right for this
  device; revisit with the on-device settings-menu work.
- Manual date/time entry from the phone — invites the 1970 problem on
  outages; SNTP + last-known-good fallback is strictly better.
- `smooth_sync` (slewing) — stepped boot correction is honest; sweep
  smoothness already comes from the `esp_timer` fraction.
- The PWA itself (empty `www` partition) is a separate open decision.

## Environment facts worth not rediscovering

- 1.31 MB OTA upload ≈ 6.7 s; dial dips 13→9-10 fps during, recovers after —
  expected, not a regression.
- `esp_ota_end()` validates structurally only (SHA-256 digest is gated on
  secure boot, `esp_image_format.c:201`).
- WiFi runs `WIFI_PS_NONE` (wall-powered) — no modem-sleep latency tax on
  the NTP exchange.
- `fw` in `/api/v1/status` is the short git hash: if it reports a hash not in
  `git log`, the device wasn't reflashed since the last history change.
