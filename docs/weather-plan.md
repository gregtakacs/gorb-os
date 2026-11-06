# Weather: home and three places, now, by the hour, five days

Status: **designed 2026-11-06 (owner, §0); W1–W5 built 2026-11-06 on `feat/weather` and run on
Bronze; not committed (§6).** Two
pieces of work: a system-wide **units** setting in the OS, which the flight tracker
adopts, and a new app, `apps/weather`, that reads it. Units go first (§3, W1–W2). Every
figure marked *estimate* is owed a measurement before the slice that depends on it.

## 0. Owner rulings, 2026-11-06

1. **Home plus at most three places.**
2. **Current conditions, hour by hour, and five days.**
3. **Units are a system setting, not an app's.** Temperature, speed and distance, set
   once for the orb. The flight tracker reads distance from it too.
4. **Open-Meteo, to start.** Other sources can be added later behind a `source` setting,
   as flights does. §1.2 records why OpenSprinkler's service was ruled out.
5. **Six starter themes, all kept** (2026-11-06, from the design mockups): Scope,
   Vintage brass, Blueprint, Paper, Celestial and Station, each matched to a flights or
   clock look (§2.4).
6. **Every theme has its own icon pack.** Packs are never shared between themes.
7. **The weather app gets a theme editor**, as the clock and flights have (§2.6).
8. **An app's settings belong on its App settings page, never in its theme editor**
   (2026-11-06). The places moved there (§2.6).
9. **The knob is one dial centred on Now** (2026-11-06, app-platform-plan.md §51): the
   hours clockwise, the days anticlockwise, a press for the next place, no arming (§2.4).

## 1. What is there today (read 2026-11-06, OS v1.2.8)

### 1.1 The orb

- **Networking** (`apps/sdk/orb.h`, D51): one request at a time per app, run off the UI
  task. The body lands in an OS buffer of up to 256 KB (`ORB_APP_NET_BODY_MAX`: a longer
  body fails, it is never cut short). The app tokenizes it in place with
  `orb_json_begin(NULL, 0)`, one event at a time (key up to 32 B, string up to 96 B), so
  the app's own memory holds only what it keeps. `orb_net_info_t.retry_after_s` carries
  the server's Retry-After.
- **Home** comes from the `location` cap: `orb_home()` gives latitude, longitude and a
  place name, and `orb_home_gen()` moves when it changes. It is set from the hub only;
  the dial has no location or time-zone UI (`main/net/orb_home.c`, NVS `orb.home` on
  `storage`).
- **Settings**: up to 16 per app, `int`, `enum`, `toggle`, `string` or `secret`.
- **The knob**, by the convention flights uses (`apps/flights/main.c` `orb_set_armed`,
  `orb_input`): unarmed, turning moves between apps; a press arms the app; armed, turns
  and presses are the app's; a hold, or the app's own disarm, hands the knob back. Ruled
  out for Weather on 2026-11-06 (§0 ruling 9, §2.4).
- **No units anywhere.** Flights writes `nm` straight into its range label and option
  value (`RANGES_NM[] = { 10, 20, 40, 80 }`, `main.c:37`, used at :632, :676, :688,
  :932). The OS has no units setting.
- **System settings** live on the dial's menu pages (`main/menu/menu_view.c`, e.g.
  `PG_DISPLAY`) and on the orb's own routes (`/api/v1/config` holds the time zone,
  `/api/v1/home` the home).

### 1.2 Why not OpenSprinkler's weather service

Read from the OpenSprinkler-Weather source (commit `5f1f205`, 2026-10-27):

- Its forecast response (`WeatherData` in `src/types.ts`) is current conditions plus
  **days only**: `WeatherDataForecast` is min, max, rain, date, icon. There is no hourly
  field.
- Its Apple provider asks WeatherKit for `currentWeather,forecastDaily` for display
  (`Apple.ts:345`). It asks for `forecastHourly` only over a past window
  (`Apple.ts:251`), to work out watering, and never returns those hours.
- The service is OpenSprinkler's, run on their Apple account for their controllers.

So it cannot feed the hourly view, and it is not ours to call. Open-Meteo is one of its
supported providers, so the owner can set the sprinkler to Open-Meteo if the two should
agree.

## 2. Design

### 2.1 Units: an OS setting

**What is set:**

| quantity | choices | first-boot default |
|---|---|---|
| temperature | °C, °F | °C |
| speed | km/h, mph, kn | km/h |
| distance | km, mi, nm | km |

Defaults are open (§5). The flight tracker shows `nm` today; on its first boot after W2
it uses whatever distance is set.

**Where it lives:** a small OS module, `main/net/orb_units.c` (beside `orb_home.c`), with its own NVS
namespace `orb.units` on `storage`, beside `orb.home`. That namespace survives a
reflash and an OTA (the `partitions.csv` retention rule) and is never one of
`nvs.net80211`'s.

**Where it is set:**

- **On the dial:** a UNITS page in the system menu, one row per quantity, the knob edits
  in place (the same knob-edit pattern as the other system rows).
- **From the hub:** `GET /api/v1/units` and `POST /api/v1/units` with
  `{"temp":"c","speed":"kmh","dist":"km"}`, and a Units row on Orb Manager's system
  settings. Values are checked against the fixed list and echoed back, as
  `/api/v1/config` does for the zone.

**How an app reads it:** two free imports, no cap (a unit is not sensitive):

```c
typedef struct {
    uint8_t temp;     /* ORB_UNIT_C, ORB_UNIT_F */
    uint8_t speed;    /* ORB_UNIT_KMH, ORB_UNIT_MPH, ORB_UNIT_KN */
    uint8_t dist;     /* ORB_UNIT_KM, ORB_UNIT_MI, ORB_UNIT_NM */
    uint8_t _pad;
    uint32_t gen;     /* moves on every change */
} orb_units_t;

int32_t  orb_units(orb_units_t *out);
uint32_t orb_units_gen(void);
```

An app checks `orb_units_gen()` on its beat and redraws when it moves, as it already
does with `orb_cfg_gen()` and `orb_home_gen()`. The SDK gets `binds.json` updated
(`orbsdk.py binds --write`) and a small helper in `orb_rt.c` for the conversions and
the unit labels ("°C", "km/h", "nm"), so each app doesn't write its own.

**Apps that use it** name the release in `os_min`, so Orb Manager refuses to install them
on older firmware. The release is **OS v1.2.10**, after v1.2.9 (the plans and rings work).

### 2.2 The flight tracker adopts it

- **Range** stays four steps, but each step is a round number in the set unit, not a
  converted nautical mile: km 20, 40, 80, 140; mi 10, 25, 50, 90; nm 10, 20, 40, 80.
  No outer step is wider than 80 nm (148 km, 92 mi): a wider radius is a bigger body,
  and the 21 KB body at 80 nm is what the parse budget at `main.c:264-267` was measured
  on (*to be confirmed in W2*).
- The request to the ADS-B source stays in nm (its API's unit); only the label and the
  options page change.
- The stored `range` setting stays an index, so a unit change keeps the same step.
- Flights bumps `version.txt`, sets `os_min` to W1's release, and re-bakes its shelf
  theme (tools/rebake_shelf.py).

### 2.3 The weather app (`apps/weather`)

**Manifest:**

```json
{
  "slug": "weather",
  "title": "Weather",
  "caps": ["net", "location"],
  "period_ms": 60000,
  "settings": [
    {"id": "place1", "type": "string"},
    {"id": "place2", "type": "string"},
    {"id": "place3", "type": "string"}
  ],
  "os_min": "<W1's release>",
  "editor": "editor"
}
```

A place is stored as `name|lat,lon` (e.g. `Lisbon|38.7223,-9.1393`); an empty string is
no place. The orb never geocodes (§2.6).

**One request covers every place.** Open-Meteo's forecast endpoint takes comma-separated
lists and answers a JSON array, one object per place:

```
https://api.open-meteo.com/v1/forecast
  ?latitude=<home>,<p1>,<p2>,<p3>&longitude=<home>,<p1>,<p2>,<p3>
  &current=temperature_2m,apparent_temperature,weather_code,is_day,wind_speed_10m,precipitation
  &hourly=temperature_2m,weather_code,precipitation_probability,is_day
  &daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset
  &forecast_hours=24&forecast_days=5
  &timezone=auto&timeformat=unixtime
```

- **Always metric on the wire.** The app converts to the set units when it draws, so a
  units change redraws at once with no new request.
- **`timezone=auto`** gives each place its own local hours and days, so the app never
  works out a zone (home's zone is the orb's own; the others come from the response's
  `utc_offset_seconds`).
- *Estimate:* 10–20 KB for four places, well under 256 KB. The URL's length against the
  OS client's limit is owed (§4).

**What the app keeps,** per place, parsed straight from the tokens:

- now: temperature, feels-like, weather code, day/night, wind, rain;
- 24 hours: temperature, code, rain chance, day/night;
- 5 days: code, high, low, rain chance, sunrise, sunset;
- the UTC offset and when it was fetched.

About 200 B a place, under 1 KB in all, so 64 KB of memory is plenty.

**Refreshing:**

- every 30 minutes on the beat; never sooner than 15 minutes for any reason (the
  adsb.lol lesson: calls stay rare);
- at once when home moves (`orb_home_gen`) or a place changes (`orb_cfg_gen`), still
  inside the 15-minute floor unless no data is held;
- `retry_after_s` is obeyed; failures back off 5, 10, 20, 40, then 60 minutes;
- older than 90 minutes, the data stays shown with a quiet "updated 2 h ago".

### 2.4 On the glass

The knob is one dial centred on Now (owner, 2026-11-06; the OS rules in
app-platform-plan.md §51). There is no arming: the first click is the app's.

```
   anticlockwise                          clockwise
Fri Thu Wed Tue Tomorrow | NOW | 1h 2h 3h ... 24h
        day bars          home      hourly ring
```

- **Clockwise from Now:** the hourly ring, its cursor on the next hour; each click moves
  it an hour, and past the twelfth the ring pages to the next twelve. It stops at the
  24th hour.
- **Anticlockwise from Now:** the five days, tomorrow first, each click the next day,
  the day under the cursor lit. It stops at the fifth day.
- **Now is sticky:** a fast spin always stops on it, so one flick back from anywhere
  lands on Now. Idle for about 20 s also returns to Now.
- **Press:** the next place (Home → place 1 → 2 → 3 → Home), keeping the position on the
  dial, so the same hour or day can be compared between places. The place shown stays
  chosen (owner, 2026-11-06).
- **Hold:** leaves the app (the app list and the system pages).

The 2026-11-06 build (§6) still arms: a press steps places, then views. It moves to the
dial with the OS input work in §51.

**Now:** the place name, a large temperature, the condition icon, feels-like and wind,
today's high and low on the bezel.

**Hourly:** the next 24 hours around the ring like a clock face, starting at the current
hour. Each hour's tick is coloured by temperature, its rain chance as an inner arc;
the centre shows the current hour's numbers, and turning clockwise moves a cursor round
the ring to read any hour.

**Days:** five rows, each a day name, icon, high/low and rain chance. Five is the most
that stays readable on a 1.75 in glass (*to be checked at true size*, 177 dpi).

**Icons:** WMO codes group into ten conditions: clear, clear at night, partly cloudy,
partly cloudy at night, cloudy, fog, drizzle, rain, snow, thunder. They are theme
sprites following the two-of-three rule (shape, colour, detail: never all three at
~40 px). The words are theme fonts and labels, not widgets.

**The six starter themes** (§0 ruling 5). Colours, fonts and bezel details come from the
presets they match (`apps/flights/editor/flights-bake.js` SCOPES, WORDS, FRAMES; the clock
presets in `apps/clock/editor/watch-engine.js`):

| theme | matches | ground | words | icon pack |
|---|---|---|---|---|
| Scope | Flights · Modern | radar green, range rings, 5° ticks | IBM Plex Mono / Sans | **Phosphor:** glowing strokes, radar blips for drizzle, crosshairs for snow |
| Vintage brass | Flights · Vintage brass, Clock · Aviator | dark olive, double brass bezel, lubber | Cormorant Garamond | **Engraved:** brass fills with an engraved inner line, a sixteen-ray sunburst |
| Blueprint | Flights · Blueprint | drafting blue, faint grid, dash-dot cross | IBM Plex Mono | **Drafted:** white linework over dashed construction circles, hexagon snow, arrowed rain |
| Paper | Flights · Paper | chart cream, blue grid, navy ring | IBM Plex Sans | **Folded:** two-tone paper facets with a cast shadow, a pinwheel sun |
| Celestial | Flights · Celestial, near Clock · Orbit | deep violet, galaxy band, stars | Space Grotesk, Michroma labels | **Nebula:** translucent glows, a cratered full moon, meteor rain, star snow |
| Station | Clock · Station | white, railway minute marks | Jost | **Pictogram:** solid black, red only for the sun and the bolt |

Each theme also carries a **temperature ramp** (three stops, cold to hot, differing in
lightness, not hue alone) for the hourly bars and the day bars, a rain colour, and a bar
track colour.

**The theme** is the app's own `.orbtheme`, baked by its editor. Scope is the default on
the shelf, as Modern is for flights; every app version bump re-bakes it
(`tools/rebake_shelf.py`). A theme here is not small: a ground plate (434 KB, full
screen RGB565) plus ten icons at every size a screen draws them (§4), so its size is
reported at bake time against the pool it shares with every other app's theme. The
owner's budget (2026-11-06): the pool is 8 MB, the largest clock theme is about 5 MB,
flights' is small, and a weather theme of about 1 MB sits beside them within budget.

### 2.5 Attribution

Open-Meteo's data is CC BY 4.0. The credit goes in the app's settings help
(`settings.json`) and the editor's About, not on the glass.

### 2.6 The places, and the theme editor (`apps/weather/editor`)

**Places are the app's settings, so they live on the hub's App settings page** (§0 ruling
8), not in the theme editor. The app's `settings.json` names `place1..3` with
`"kind": "place"`, and the page draws a place row for each: the Home card's searcher without
its map (owner, 2026-11-06). Typing lists matching cities from the hub's list at once (no
Find button); Enter takes the first, or reads coordinates or a full plus code, which the
orb names as it names a home (`GET /api/v1/home?lat=&lon=` or `?code=`, a proposal that
stores nothing); "Use this browser's location" works the same. A pick waits with its name
editable beside Save; Clear forgets the place. Written through `/api/v1/apps/cfg`. The stored form is `Name|lat,lon`, two decimals, inside
the 31 characters a string setting holds; the name gives way to the coordinates.

**The theme editor** ships in the app (apps are atomic: no Composer tab), built on the kit
(theme-editor-kit-plan: start sheet, screen calibration, colour picker, CSS), with the
clock's and flights' editors as the model.

**Themes** (§0 ruling 7), in steps, as flights' Scope, Arm, Planes, Words, Frames,
Render:

1. **Start:** the six starter themes as tiles, plus Blank (plain greys, nothing
   preset), or one opened from the hub library.
2. **Ground:** the plate (colour, gradient or the author's picture) and its details:
   rings, ticks (step, length, weight), grid, the galaxy band and stars, the lubber.
3. **Icons:** the theme's pack (§0 ruling 6). A pack is a drawing style with named
   colour roles (Phosphor: stroke, glow; Engraved: metal, highlight, engraving; and so
   on), so recolouring keeps the style. Every condition is previewed at every size it is
   drawn. A theme started from Blank picks a pack's style to start from; the baked theme
   carries its own copy, so two themes never share one.
4. **Temperature:** the three ramp stops, the rain colour and the bar track, previewed on
   the hourly ring and the day bars with a lightness check (the stops must differ in
   lightness, not hue alone).
5. **Words:** fonts and the four tones (body, accent, quiet, warning), as the kit's other
   editors do.
6. **Render:** all four screens at true size (177 dpi by default), the pool size of the
   bake, then Send to orb, which saves the design first, as flights' does.

The icon packs live in the editor as drawing code, the way the mockups draw them, and the
bake rasterises them into the theme's sprites. The orb never draws an icon itself.

## 3. Slices

| slice | what | depends on |
|---|---|---|
| W0 | §4's checks | — |
| W1 | units in the OS: `orb_units.c`, NVS, the dial's UNITS page, the routes, the imports, the SDK helper, `binds --write`, Orb Manager's Units row | — |
| W2 | flights adopts units: range steps per unit, labels, `os_min`, version bump, shelf re-bake | W1 |
| W3 | `apps/weather`: request, parse, refresh rules, the three views, `os_min` | W0, W1 |
| W4 | `apps/weather/editor`, places: the geocoding search and the three place rows | W3 |
| W5 | `apps/weather/editor`, themes: the six steps (§2.6), the six packs, the bake, a bake check over the six starters, Scope on the shelf | W3 |

## 4. Owed

1. The URL length (about 450 B with four places) against the OS HTTP client's limit.
2. A real four-place response's size, and the parse time through `orb_json_next`.
3. Five day rows and the 24-hour ring legible at true size.
4. The weather theme's size in the pool. *Estimate:* the plate is 434 KB; icons at the
   five sizes the mockups draw (104, 76, 62, 38 and 30 px) are about 680 KB more at
   3 B a pixel with alpha, so roughly 1.1 MB a theme. Drawing icons at three sizes
   (104, 62, 34) would bring that to about 0.9 MB. Measure on a real bake before W5
   fixes the sizes.

## 5. Open

- **Units defaults** on a fresh orb: metric everywhere, or picked from home's country
  when the hub sets it?
- **Pressure** (hPa, inHg) as a fourth unit, if the app ever shows it.
- **A second source** (Met Norway, or WeatherKit through a proxy the owner runs) behind a
  `source` setting, if Open-Meteo falls short.

## 6. As built (2026-11-06, `feat/weather`, uncommitted)

### 6.1 What landed

- **W1, units.** `main/net/orb_units.c/.h` (NVS `orb.units` on `storage`, gen from 1);
  the free imports `units` and `units_gen` (`orb_app_wasm.c`, `binds.json`, `orb.h` with
  the `orb_temp_from_c`, `orb_speed_from_kmh`, `orb_dist_from_km` and label helpers);
  `GET/POST /api/v1/units` (any subset of `temp`, `speed`, `dist`; an unknown value refused
  whole; 52 of 56 URI handlers); the dial's UNITS page, closing the top menu's OS run so no
  `back_cur` literal moved; the hub's allowlist, fake orb and a Units card on the Orb tab.
  `tools/host_units_test.py` (9 checks, 6 of 6 mutants) and `host_menu_test.py` cover it.
- **The request path.** `orb_app_net.c` held an app's path in 200 B and `orb_https.c`
  built the URL in 320 B; four places need 389 B (measured). The path is 480 B now, which
  still fits esp_http_client's 512 B request line (`http_client_prepare_first_line`).
- **W2, flights v1.2.10** (on v1.2.9's far dots). The range steps per unit (§2.2), `os_min` v1.2.10, Modern
  re-baked onto the shelf.
- **W3, the app.** `apps/weather/main.c` and `wx.c`, 25 KB of wasm in 64 KB.
  `tools/host_weather_test.py` drives `wx.c` through the OS's own tokenizer with
  Open-Meteo-shaped bodies (22 checks, 10 of 10 mutants). A live request for four places
  answered 7 KB, 802 tokens, every place read whole with its own offset, 24 hours and 5 days.
- **W4, the places** (weather v1.0.1): a `kind` in an app's `settings.json` lets the hub's
  App settings page draw a STRING setting (`place`, or plain `text`); the weather's three
  places are the first. Benched on Bronze: a place written the way the page writes it, the
  app's second place on the glass, then cleared.
- **W5, the theme editor.** `apps/weather/editor`: Start (the six themes and Blank),
  Ground, Icons, Temperature, Words, Render; the preview draws all four screens from the
  bake's own frames. `_selftest/bake_check.py` bakes the six and four variants in Chrome and
  checks each against `theme_schema`, `orbbundle` and the shipped `orb_clock3.c`; Scope is on
  the shelf as `scope.orbtheme` (a shipped theme's file is its own name; `rebake_shelf.py` knows it).

### 6.2 Where the build differs from §2

- **Places step = each place's Now**, not the list in the design mockups: the 32 rows had
  no room for a list. Superseded by §2.4's press for the next place, which keeps the view.
- **The hourly ring is a 12-hour clock face.** Twelve bar rows stand at their clock hours
  and show twelve of the 24 hours; turning past the twelfth pages to the next twelve. 24
  bars would have taken 24 of the 32 rows.
- **The rows** (all 32 used): the large icon (0), five small icons (1–5), twelve hour bars
  (6–17), five day bars (18–22), the place dots (23), eight labels (24–31). Banks are
  shared through `stage3.pool`, so the bars and small icons cost one set of frames each.
- **Three fonts, not five.** A font is always the hundred glyphs of `FONT_CHARSET`, so each
  is weighed: body (16 px), quiet (14 px, the titles' face, capitals where the theme writes
  them) and figures (44 px, its cell sized to the digits). The lows and rain columns of the
  days dim by row opacity rather than a font of their own.
- **A label's lines stand one cell apart** (`orb_label.c`; `ease` is refused on a label), so
  the day columns are nine lines, a blank between each day, in a body font whose cell is
  28 px: the rows land 56 px apart.
- **Frames:** bars 8 lengths × 3 rain levels (24), day bars 7 positions (28).

### 6.3 Sizes (measured, all ten bakes)

Scope and Blueprint 1.57 MB, Brass 1.65, Paper 1.69, Station 1.70, Celestial 1.73. The bake
check gates at 1.75 MB: the 8 MB pool less skeleton (5.07 MB) and modern (1.34 MB) leaves
about 1.95 MB. The plate (424 KB), the figures font (258–328 KB) and the large icon
(151 KB) are most of it. A **figures font** (a short charset, the digits and the degree
sign) would save about 250 KB but is a format change: `orb_glyph_next` falls back to glyph
31, past the end of any font shorter than 32, so the reader, `theme_schema` and the bakers
move together. Not done.

### 6.4 Owed

1. **Bench on Bronze (2026-11-06, over Wi-Fi at the owner's go-ahead).** v1.2.10 by OTA;
   weather v1.0.0 and flights v1.2.9 installed; Scope stored (1,588,744 B in the pool beside
   Bold Analog and Modern). Seen on the glass by `fbgrab.py --direct`: Now (a real fetch
   for home, 1.7 KB in 2.2 s, night icon after dark), Hourly (cursor bar lit, turning
   moves it), five days, a units change redrawing °C to °F and km/h to mph with no new
   fetch, flights' range reading "25 mi", the UNITS page, and a knob edit there saving
   through the worker (`gen` 3). Units put back to metric afterwards. Still owed: a look
   at true size for legibility, and the places beyond home (none set on Bronze yet).
2. **The release steps.** `version.txt` says v1.2.10; the hub's embedded firmware bundle
   (`make -C tools/orb-manager firmware`) is not refreshed, because the build is `-dirty`
   until the branch is committed.
3. §4 item 1 is answered by §6.1 and item 4 by §6.3. Item 3 (five day rows and the hour
   ring legible at true size) has the glass's own frames now (item 1) but no owner's eye yet.
