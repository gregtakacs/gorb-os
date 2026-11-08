# Radar: rain over a map, for home and the three places

Status: **designed 2026-11-07 (owner, §0); R0 measured, R1, R1b and R2 built and committed,
R3 committed, R3b built 2026-11-07, uncommitted (§6).** Four pieces of work:

- the places move out of the weather app into the OS, beside home (§2.1);
- **the OS serves a map for home and each place** to any app that holds `location`, drawn
  on the orb from OpenStreetMap vectors that a tool in the hub fetches (§2.4);
- the OS gains a small raster surface an app can draw radar with (§2.3);
- a new app, `apps/radar`, with its own theme editor (§2.5, §2.9).

Weather adopts the OS places in the same release (§2.8). Every figure marked *estimate* is
owed a measurement before the slice that depends on it (§4).

## 0. Owner rulings, 2026-11-07

1. **The radar is its own app**, not a view inside Weather. Weather stays as simple as it
   is.
2. **Home and the three places are OS settings.** They live in the OS config beside home,
   and every app that wants them reads them there; no app holds another app's places.
   This supersedes weather-plan.md §0 ruling 8 *for the places* (an app's own settings
   still belong on its App settings page).
3. **The map belongs to the location, not to the radar.** The OS serves each place's map
   with the rest of its location facts, to any app. Flights is the next reader: the map as
   a coloured, transparent layer of landmarks under the aircraft.
4. **The radar has a theme editor**, in the app (apps are atomic). What goes on it is open;
   §2.9 is the proposal.
5. **Clean slate for the places.** Weather's `place1..3` are not carried into the OS. The
   owner re-enters the places on the hub once the release lands.
6. **Flights' use of the map is out of scope.** This work prepares the ability: the map
   must reach flights' ranges (10 nm / 10 mi / 20 km at the tightest), but flights showing
   it is a later plan of its own.
7. **OpenStreetMap for the close steps, and we ship the tool, never the data.** The hub
   fetches what a place needs when the owner saves it; no map data is in a release.
8. **The public LibreWXR instance, for now.** The owner is ready to run their own if it
   is ever needed.
9. **The map credit goes on gorb-os's ABOUT page**, not on the map. *Revised 2026-11-07*
   after OSMF's guidelines (§4 item 11): **and, once a boot, on the glass for five
   seconds** the first time an app draws a map, then gone (owner's choice of the three
   offered).
10. **The hub's Go side forwards the tile requests** and sets the User-Agent
    OpenFreeMap's offline-download terms ask for: `gorb-os/<version>
    (+https://github.com/gregtakacs/gorb-os)`. It only forwards (D33).
11. **The map is vectors, drawn on the orb.** The hub filters and encodes the vector data;
    the OS renders it at whatever radius, rotation and line widths an app asks for.
    Rendered vectors give more freedom in how the data is shown than finished pixels do.
    The extra card space (~2 MB a place against ~560 KB of pictures) does not matter.
12. **Press-and-turn is an OS gesture, and the radar's zoom.** Turning while the knob is
    held down is a third control beside turn and press; double press stays out (§51: it
    would delay every press). The owner felt it on Bronze's test build (2026-11-07) and
    kept it. The hold must not fire under a slow first turn (§2.10).
13. **The look of the mock-up is approved** (2026-11-07,
    https://claude.ai/artifact/NULqoinYyg8zVQWmtrYbjC), with these changes:
    - **The map has presets:** levels of detail and colour schemes that match the themes,
      and switches that turn things off easily, such as minor roads, or fewer city
      names (§2.9).
    - **Five starters:** Vintage and Blueprint join Scope, Paper and Station, so the radar
      matches every look the orb has. Blueprint draws thin and dashed lines, so the map
      renderer draws dashes (§2.4).
14. **Every radar setting is set at theme-edit time and carried in the baked theme.** The
    map's colours, opacities, widths and dashes, the detail switches, the rain ramp, the
    forecast style, the overlay and the life all live in the theme. Only the starting zoom
    and the radar source stay app settings (§2.5). The theme format needs a section of
    values for this (§2.11).
15. **The card is a runtime read path for the OS's maps**: essential, and fine. The OS
    schedules every card read and write, so the maps never block the hub's writes of apps,
    themes and maps, and the hub's writes never corrupt a map being read (§2.12).
16. **The stage gains a live layer** (2026-11-07): the radar draws its map and rain into a
    full-glass element the theme places in its stack, not an LVGL canvas (§2.9, R3c).
17. **A theme's values are locked to the theme** (2026-11-07). They exist because the map
    and the rain are rendered live by the OS, which must know the theme's colours; they
    are not settings. Nothing at runtime shows or changes them: no app setting, no options
    row, no hub page, no route. The only way to change them is the theme editor and a new
    bake (§2.11).

Everything else below is design, open to the owner until it is built.

## 1. What is there today (read 2026-11-07, OS v1.2.16)

### 1.1 The orb

- **An app's memory is 64 or 128 KB** (`apps/sdk/orbsdk.py:126`, D27). One decoded radar
  frame at 466×466 is 108 KB at 4 bits a pixel and 217 KB at 8, so **frames cannot live
  inside the app.** They must be OS memory the app names by handle.
- **Networking** (`apps/sdk/orb.h`, D51): one request at a time per app, the body in an OS
  buffer of up to 256 KB, read in pieces with `orb_net_read`. There is no way to keep a
  body once the next request starts.
- **The canvas** (`main/app/orb_ui.c:188`): an OS-owned RGB565 surface in PSRAM, up to
  1024×1024, drawn with OS primitives (`cv_fill`, `cv_line`, `cv_disc`, `cv_text`…).
  `cv_pixel` exists, but an interpreted pixel loop over 217,156 pixels is far too slow.
- **No image decoder in the image.** `CONFIG_LV_USE_LODEPNG`, `LIBPNG`, `TJPGD` and
  `LIBWEBP` are all off (`sdkconfig`). The ESP32-S3's ROM carries miniz's `tinfl`
  inflater, so a PNG reader costs only its filter and palette code.
- **No runtime flash writes for apps** (D18, and §14.4's lesson). A flash cache of maps,
  which the reference project uses (§1.3), is not open to us.
- **The card** is mounted on both bench orbs (Bronze: `SD32G SDHC 29.1GB @20000kHz`). The
  hub can write to it through `POST /api/v1/sd/write` and `/api/v1/sd/mkdir`
  (`main/net/orb_net.c:8098-8100`). At runtime the card is an install source; the gallery's
  §0 ruling 7 opens it as a runtime read path for the gallery alone, and that work (G1) is
  not built.
- **Home** is `main/net/orb_home.c`: one record in NVS `orb.home` on `storage`, read by an
  app through the `location` cap (`orb_home()`, `orb_home_gen()`), set from the hub only.
  **The three places** are Weather's own string settings `place1..3`, drawn as place rows
  on the hub's App settings page (weather-plan §2.6, W4).
- **Memory, read from Bronze's `/api/v1/status` on 2026-11-07 (v1.2.15):** PSRAM free
  3,625,416 B; internal heap free 23,379 B, largest block 7,168 B. Internal RAM stays the
  constraint, so everything below lives in PSRAM.
- **The carousel** (app-platform-plan §54) shows each app by its theme's 232 px preview
  plane, so the radar needs a theme to appear there (§2.9).
- **Flights draws on the stage** (theme rows, `orb_stage_*`), not on a canvas. How the map
  reaches flights' glass is flights' later plan (§0 ruling 6). Its range steps
  (`apps/flights/main.c:42`) are km 20, 40, 80, 140; mi 10, 25, 50, 90; nm 10, 20, 40, 80:
  a radius from 16 km (10 mi) to 148 km (80 nm).

### 1.2 The hub

- **It is on only for maintenance** (flight-tracker-design §1). Nothing at runtime may
  depend on it: the orb fetches radar itself, and reads maps the hub made earlier.
- **The browser holds the product logic** (D33). Anything the hub makes, such as the maps
  in §2.4, is browser JS on the hub's page, never Go.

### 1.3 The reference: `TheMonkeyz/esp32-s3-weather` (MIT, read 2026-11-07)

A standalone firmware for a very similar round 466 px S3 board. Its radar
(`main/radar.c`, 1,362 lines) does:

- Environment Canada GeoMet WMS `GetMap` for each frame: a 466×466 **RGBA** PNG, decoded
  on the device (~140 ms each), then reduced to palette indices.
- 15 frames at 12-minute steps, each 217 KB in PSRAM permanently, plus two 434 KB RGB565
  maps: about **4.1 MB of PSRAM held all the time.**
- An OpenStreetMap basemap fetched from `tile.openstreetmap.org`, **preloaded for seven
  zoom levels in the background**, and kept as raw RGB565 in a **4 MB `mapcache` flash
  partition** (seven 512 KB slots).
- Zooms 4 to 10, with lightning marks from a second GeoMet layer.

**Ideas we take:**

- Decode a row at a time (its `png_rows.c`), never a whole RGBA image.
- When the frame list shifts by one, reuse the buffers the frames already hold
  (`plan_frames`), so a refresh is one request, not fifteen.
- Fetch the newest frame first and show it straight away, then fill in the past.
- A few frames a second is enough for the loop.
- Its Web Mercator view maths (`view_origin`, `radius_at`).

**What we leave behind:**

- **The flash map cache.** We have no flash to give (Layout C's rows end at exactly
  16 MB) and apps may not write flash.
- **4 MB of PSRAM held forever.** We share PSRAM with every other app, the carousel's
  preview planes and the tile ring.
- **RGBA frames decoded and coloured on the device.** A raw-value image with the palette
  on the orb is a tenth of the bytes, and the theme picks the colours.
- **OSM's tile servers.** The OSMF tile usage policy, as read for this plan, forbids
  distributing an app that uses `tile.openstreetmap.org` without permission, and forbids
  bulk prefetching, which the background preload is. A firmware anyone can flash is that
  distributed app. (Not yet checked against the current policy text, §4.)
- **Canada only**, and threading built for a firmware that owns the whole device.

### 1.4 Radar sources (surveyed and measured 2026-11-07)

- **LibreWXR** (`github.com/JoshuaKimsey/LibreWXR`, AGPL-3.0 server; the public API's data
  is CC-BY-4.0, with no key). It keeps RainViewer's v2 API and fills it from public
  national radar (US MRMS and NEXRAD, Canada GeoMet, EUMETNET OPERA for Europe, Italy's
  DPC, JMA, Taiwan, Malaysia, the Philippines, El Salvador), NOAA RRQPE satellite rain from
  60°S to 70°N, and ECMWF IFS through Open-Meteo for the rest. Measured against
  `api.librewxr.net`:
  - `/public/weather-maps.json`: **12 past frames 600 s apart, plus 6 nowcast**, each a
    `time` and a `path`.
  - A point image, centred on a lat/lon:
    `{path}/512/{z}/{lat}/{lon}/255/0_0.png`. **Colour 255 ("raw") is an 8-bit indexed
    PNG, 512×512.** London at z7: 6,540 B (WebP 3,992 B); New York at z6: 11,400 B; London
    at z5: 31,895 B; DFW, light rain to the south-west: z6 2,253 B, z7 1,529 B. The RGBA
    scheme 2 for the London frame: 31,319 B.
  - The values: index 0 is no echo. Indices 1–68 map to a grey ramp of 84–151 in the PNG's
    palette (New York z6). **The indices are the encoder's, frame by frame** (R3c's bench,
    2026-11-07: Seattle's index 1 is grey 89), so a pixel's value is its palette entry's
    grey, never its index; `orb_cv_png8` colours values (§2.3). **The grey is LibreWXR's pixel value, and dBZ = value / 2 − 32**
    (read from its source, 2026-11-07: `pixel = (dBZ + 32) * 2`; scheme 255 is an identity
    table). Echoes under 10 dBZ (value 84, `noise_floor_dbz`) are dropped, which is why the
    lowest grey seen is 84. **The raw image carries no snow:** scheme 255's snow table is the
    same identity.
  - **A 512 px point image at z has the scale of zoom z+1** in the usual 256 px pyramid: it
    is placed in a world of 2^z tiles of 512 px (`window_origin(lat, lon, z, 512)`), centred
    to the nearest whole pixel. Measured over Seattle's rain (2026-11-07): the z6 512 px
    image equals the same zoom's tiles stitched and cropped at that origin, pixel for pixel,
    and its centre 256 px equals the z7 256 px image, all 3,581 echo pixels.
  - ~~Point images stop at z7.~~ *Wrong, corrected 2026-11-07:* Seattle answers z8 and z9
    with data. DFW's empty z8 was an empty sky; the server's default cap is z12.
  - `/v2/coverage/0/512/{z}/{lat}/{lon}/0/0_0.png`: an indexed PNG with two values, 0 and
    1 (DFW z6 1,312 B; London z7 1,302 B). It says covered or not; it does not tell radar
    from satellite or model.
  - `Cache-Control: public, max-age=300`; the resolved time comes back in
    `X-Frame-Timestamp`.
  - **The risk:** one maintainer's server, hosted on donations, with no stated rate
    limits. The server is published as a Docker image (linux/amd64 and arm64), so an owner
    can run their own and point the orb at it.
- **RainViewer**: since 2026-01-01 the free tier is capped at zoom 7, one colour scheme,
  no nowcast, personal and educational use only. Its paths changed again in March 2026.
  Not open, and not stable.
- **National services** (GeoMet, MRMS, DWD, KNMI…): open, but each covers one country.
- **OpenWeather and the other commercial layers**: keys and terms, and not open.

### 1.5 The map, rendered (2026-11-07, DFW)

A first render of §2.4's map from Natural Earth 1:10m, centred on DFW (32.8998,
−97.0403), in three candidate themes with the live LibreWXR frame over it, and as a
flights-style transparent layer. Made by a scratch script outside the tree (Python and
Pillow, standing in for the hub's browser JS), not committed.

| step | Mercator z | radius at DFW | metres a pixel | 4-bit role PNG | landmarks |
|---|---|---|---|---|---|
| Wide | 6 | 479 km | 2,054 | 19,715 B | 20 |
| Region | 7 | 239 km | 1,027 | 13,044 B | 8 |
| Local | 8 | 120 km | 513 | 9,862 B | 9 |

- **A step's radius depends on latitude.** The same steps at 50°N are about 366, 183 and
  92 km.
- **1:10m is right for Wide and Region, and thin at Local.** At 513 m a pixel, only the
  interstates, the largest lakes and the urban outline show. Lewisville and Grapevine lakes
  are absent. Flights' tighter ranges would be thinner still, which is why the map moved
  to OpenStreetMap data (§1.6, §0 ruling 7).
- **Labels collide at Wide.** DFW, Dallas, Fort Worth and FTW sit within a few pixels of
  each other, so the landmark list needs ranks, and whoever draws names needs to drop the
  lower-ranked ones that collide (§2.4).
- **The data's size:** the nine layers used (land, lakes, urban areas, rivers, roads, state
  and country lines, populated places, airports) are 3,229,292 vertices and 140 MB as
  GeoJSON. Urban areas (1.15 M vertices) and roads (0.71 M) are most of it. Delta-coded at
  about 3 B a vertex, that is ~10 MB before any simplification. Shipping that is what §0
  ruling 7 rules out.

### 1.6 Map data the tool can fetch (checked 2026-11-07)

- **OpenFreeMap** (`openfreemap.org`): OpenStreetMap as vector tiles (MVT) in the
  OpenMapTiles schema, z0–z14, free, no key, no registration, commercial use allowed.
  - TileJSON `https://tiles.openfreemap.org/planet` names a dated build
    (`planet/20261101_080013_pt/{z}/{x}/{y}.pbf`).
  - Layers include `water`, `waterway`, `landuse`, `landcover`, `park`, `transportation`,
    `boundary`, `place`, `aerodrome_label` and `aeroway` (runways and aprons).
  - Tiles over DFW: z8 118,211 B, z10 135,663 B, z12 111,441 B, z14 76,315 B. Dense
    urban tiles, so these are near the top of the range.
  - `access-control-allow-origin: *`, so the hub's page can fetch them directly.
  - **Terms:** attribution is required ("OpenFreeMap © OpenMapTiles Data from
    OpenStreetMap"). An offline download is allowed up to 3,000 tiles or 50 MB, with at
    most 4 requests in parallel and a unique User-Agent. A browser cannot set its
    User-Agent (§4).
  - The server is open source and the planet is published weekly, so a self-hosted
    OpenMapTiles source is the same tool pointed elsewhere.
  - At low zoom, OpenMapTiles itself draws its water and borders from Natural Earth, so
    one source covers every step.
- **Natural Earth** stays the reference for §1.5's render, but it is no longer needed as a
  source.

### 1.7 The map from OpenStreetMap, rendered (2026-11-07, DFW)

*This is the raster draft the owner reviewed before §0 ruling 11 moved the map to vectors.
It stays as a record of the measurements, and its columns C and D are the look the vector
renderer aims for.*

The tool's fetch, run for DFW: **81 tiles, 8,801,137 B**, z6–z11 (9 to 16 tiles a zoom,
enough for a 660 px map at each), 4 at a time, with a `gorb-os-map-preview` User-Agent.
The tiles were decoded by a 120-line MVT reader and drawn by a scratch script (Python,
standing in for the hub's JS), not committed.

| step | z | radius at DFW | m/px | 466 px PNG | 660 px PNG | landmarks |
|---|---|---|---|---|---|---|
| 0 | 6 | 479 km | 2,054 | 24,281 B | 40,035 B | 88 |
| 1 | 6.5 | 338 km | 1,452 | 22,084 B | 38,092 B | 55 |
| 2 | 7 | 239 km | 1,027 | 24,881 B | 46,861 B | 100 |
| 3 | 7.5 | 169 km | 726 | 22,821 B | 38,519 B | 52 |
| 4 | 8 | 120 km | 513 | 23,529 B | 39,080 B | 80 |
| 5 | 8.5 | 85 km | 363 | 24,021 B | 37,543 B | 45 |
| 6 | 9 | 60 km | 257 | 42,411 B | 65,131 B | 86 |
| 7 | 9.5 | 42 km | 182 | 42,262 B | 67,963 B | 71 |
| 8 | 10 | 30 km | 128 | 40,717 B | 72,591 B | 108 |
| 9 | 10.5 | 21 km | 91 | 34,711 B | 65,332 B | 63 |
| 10 | 11 | 15 km | 64 | 34,395 B | 65,591 B | 30 |

- **Close range is now worth having.** From step 6, the lakes (Lewisville, Grapevine, Ray
  Hubbard), the suburbs and minor roads are there. From step 8, DFW's seven runways are
  drawn to their real headings, and Love Field, Alliance and Meacham show as airports.
- **A correction to the first draft of §2.4.** A step that *covers* a radius has to be
  *enlarged* to fill the glass. To *shrink* instead, the finer step must be drawn larger
  than the glass. So the maps are **660 px** (√2 × 466), and the 466 px window inside them
  is what the radar shows.
- **Flights' mile ranges three ways** (10, 25, 50 and 90 mi; the sheet `osm-shrink.png`):
  - A: the coarser step enlarged (×1.05–1.31, nearest pixel). Readable, but it carries the
    coarser step's detail, and at ×1.31 it turns blocky.
  - B: the finer step shrunk (÷1.08–1.35), nearest pixel. Finer detail, but thin lines
    lose pixels and break up.
  - C: the same shrink, taking the highest-priority role in each glass pixel. No breaks;
    roads and rivers read bolder, about 2 px at ÷1.35. The owner judges whether that weight
    is right; a theme can lighten it through the roads' colours.
  - D: C as a flights-style transparent layer with names from the landmark list.
- **The cost of 660 px:** 37–73 KB a map, about 560 KB a place and 2.2 MB for four, on a
  29 GB card.

### 1.8 The same map as vectors (2026-11-07, DFW)

The vertices the drawn roles carry within each zoom's ±330 px window, counted from the same
81 tiles. This is the input to §0 ruling 11.

| z | vertices | at 2.5 B a vertex | largest layers |
|---|---|---|---|
| 6 | 119,788 | ~292 KB | water 48,664; roads 31,205; rivers 21,193 |
| 7 | 125,654 | ~307 KB | water 49,040; roads 31,393; urban 27,299 |
| 8 | 93,870 | ~229 KB | urban 35,792; water 34,297; roads 14,288 |
| 9 | 174,677 | ~426 KB | roads 70,957; urban 57,468; rivers 27,049 |
| 10 | 175,560 | ~429 KB | urban 71,772; roads 70,349; water 17,486 |
| 11 | 110,947 | ~271 KB | roads 46,738; urban 42,440; water 14,322 |

- **About 800,000 vertices and ~2 MB a place, before simplification.** The tiles carry
  coordinates at 4,096 units per 256 px, 16 times finer than a glass pixel, so
  simplifying to a quarter pixel should cut that several times (*estimate* 400–600 KB, §4).
- **The level of detail still matters.** A wide view drawn from z10's data would carry
  roughly ten times the vertices and show nothing more, so the OS keeps one file per whole
  zoom and picks the nearest (§2.4).

### 1.9 The vector renderer on Bronze (R0, 2026-11-07)

`main/map/orb_map_raster.c` (plain C, host-tested by `tools/host_map_raster_test.py`)
drawing DFW's six `.orbmap` files (`tools/orbmap.py`, FORMAT 1) from Bronze's card,
through the bench-only `GET /api/v1/debug/map_bench` on the httpd task. Best of three,
240 MHz, octal PSRAM at 80 MHz, the clock app on the glass.

| level | file | card read | render, own scale | 1.41x | 0.71x | rot 30 | dashed | fills / lines at 1x |
|---|---|---|---|---|---|---|---|---|
| z6 | 126,952 B | 197 ms | 165 ms | 144 | 191 | 167 | 169 | 59 / 103 ms |
| z7 | 145,164 B | 224 ms | 173 ms | 160 | 214 | 185 | 176 | 71 / 99 ms |
| z8 | 123,091 B | 189 ms | 159 ms | 161 | 170 | 169 | 161 | 97 / 61 ms |
| z9 | 326,386 B | 500 ms | 421 ms | 428 | 422 | 451 | 487 | 168 / 251 ms |
| z10 | 354,349 B | 527 ms | 446 ms | 396 | 484 | 501 | 510 | 224 / 219 ms |
| z11 | 230,666 B | 352 ms | 284 ms | 252 | 339 | 315 | 334 | 144 / 138 ms |

- **The files are larger than estimated:** 1.31 MB for DFW's six levels at a quarter
  pixel (2.7 B a vertex; the simplification about halves the tiles' vertices). §1.8's
  400–600 KB estimate was low.
- **The card reads at about 650 KB/s.** A 32 KB piece holds the card lock 48–57 ms, so a
  hub write waits at most that long behind a map read (§2.12). CRC: 8–24 ms.
- **One bug found and fixed on the bench.** An insertion sort of each fill's edges was
  quadratic on a metro's urban outline in PSRAM: z8 took 2.95 s. A counting sort by row
  brought it to 159 ms. A solid-line fast path cut the lines by about 25 %.
- **It is plain CPU, not memory.** Decoding alone (`roles=0`) is 24 ms at z6 and 61 ms at
  z10, about 90x this host, the same ratio as drawing. A tiled raster (8x8 pixels, one
  32-byte cache line a tile) left the times unchanged, and is kept since it is
  pixel-identical and only 2.8 KB larger.
- **The verdict: vectors stay** (§0 ruling 11). A view opens in 0.35–1.0 s with its read,
  and re-renders in 0.15–0.5 s from a file already in PSRAM. That holds with three rules
  (§2.4): views render on the worker while the old one stays on the glass; the app's
  current levels stay in PSRAM; and the radar keeps its three levels (~395 KB at DFW)
  while it is on the glass, so a zoom costs only the render (~0.16–0.2 s).
- **Room left, not needed yet:** a coarser simplification, dropping rings under a pixel,
  and inlining `floorf` and `ceilf` in the inner loops.

## 2. Design

### 2.1 Places in the OS

**Where they live:** a small OS module, `main/net/orb_places.c`, beside `orb_home.c`,
with its own NVS namespace `orb.places` on `storage`. That survives a reflash and an OTA
(the `partitions.csv` retention rule) and is never one of `nvs.net80211`'s. Three slots,
each the home record's shape: `lat_e6`, `lon_e6`, a name of up to 39 characters. An empty
slot is no place. A slot keeps its position when another is cleared, so "place 2" stays
place 2.

**How an app reads them:** under the `location` cap, the same consent as home:

```c
typedef struct {
    int32_t  lat_e6, lon_e6;
    uint32_t gen;            /* the places' gen, as orb_places_gen() */
    char     place[40];      /* "" for an empty slot */
} orb_place_t;

int32_t  orb_place(uint32_t i, orb_place_t *out);   /* i = 0..2; ORB_OK, or an error for an empty slot */
uint32_t orb_places_gen(void);                      /* moves on every change to any place */
```

Home stays `orb_home()`. Where an API below takes a place, **0 is home and 1–3 are the
places**. An app checks `orb_places_gen()` on its beat and re-reads when it moves, as it
does `orb_home_gen()`.

**Where they are set:** only from the hub (§1.1: the dial has no location UI, by ruling).
`GET /api/v1/places` and `POST /api/v1/places` with
`{"i":1,"lat":38.7223,"lon":-9.1393,"name":"Lisbon"}`, or `{"i":1,"clear":true}`, checked
and echoed as `/api/v1/home` is. **On Orb Manager the place rows move from Weather's App
settings page to the Orb tab, under Home**, as a Places card: three rows with the same
searcher W4 built (the city list, coordinates or a plus code, "Use this browser's
location", the name editable before Save, Clear), saved through the new route rather than
`/api/v1/apps/cfg`. The hub's allowlist (`security.go`) and the fake orb grow the route.

**No carry** (§0 ruling 5). A fresh `orb.places` starts empty; Weather's old strings are
dropped with its settings (§2.8).

**Units and time** are unchanged: a place has no zone of its own in the OS. Weather keeps
getting each place's offset from Open-Meteo, and the radar labels frames relative to now
("−40 min"), which needs no zone.

### 2.2 The source: LibreWXR, behind a `source` setting

- **A `source` string setting**, the base URL, defaulting to `https://api.librewxr.net`. An
  owner running their own LibreWXR (on a NAS, say) points it there; the API is the same.
- **Per refresh:** one `weather-maps.json`, then only the frames the app does not hold. A
  path is just the frame's time (`/v2/radar/<unix time>`), and **the same path serves the
  forecast for that time and, once it has passed, the observation** (polled 2026-11-07:
  `/v2/radar/1794083400` was a nowcast frame for three polls, then a past one). So a frame
  is keyed by its time, its kind (past or nowcast), the place and the zoom: a frame that
  moves from nowcast to past is fetched again, and the six nowcast frames are re-asked each
  cycle with `If-None-Match` and the `ETag` the server sends (`max-age=300`), so an unchanged
  one costs a 304. Normally a refresh is one new past frame plus six conditional requests.
- **The request:** `{source}{path}/512/{z}/{lat}/{lon}/255/0_0.png`, coordinates to four
  decimals, z at most 7 (§1.4). PNG, not WebP: one decoder covers the radar and the map
  (§2.3), and WebP's lossless coder is much more code than a PNG filter on top of ROM
  `tinfl`.
- **Order:** the newest past frame first, which is drawn at once; then the nowcast; then
  the older past frames, newest to oldest.
- **When:** only while the radar is on the glass, every 10 minutes (the cadence the frame
  list moves at). A place change or a zoom change fetches at once. `retry_after_s` is
  obeyed; failures back off 1, 2, 5, then 10 minutes. A frame list older than 30 minutes
  stays shown with a quiet "updated 40 min ago".
- **Coverage:** one coverage image per place and zoom, fetched with the first frame and
  kept for the session (~1.3 KB). Where it says 0, the glass dims the area rather than
  showing it as dry. *Not built (R5, §6.8):* LibreWXR's coverage image marks where a
  frame's data is non-zero (its renderer: `mask = values > 0`), an echo mask rather than
  where radar reaches: 2.6 % of DFW at z6, all of it under NEXRAD. It cannot tell dry from
  unwatched, so the radar shows no such state.
- **Attribution**, in the app's settings help, as Weather credits Open-Meteo: "Radar:
  LibreWXR (CC BY 4.0) from national radar services, NOAA and ECMWF via Open-Meteo." Italy's
  DPC data is CC-BY-SA; the help names it. The map's credit (OpenStreetMap) is the OS's
  and sits with the Places card (§2.4).
- **The public instance, for now** (§0 ruling 8).

### 2.3 The OS surface: kept bodies and an indexed-PNG draw

Two additions, named for what they do, not for radar. They serve the radar frames and the coverage image; the map is vectors (§2.4).

**Kept bodies** (cap `net`). A landed response can be kept as an OS buffer in PSRAM,
which the app names by a small slot number:

```c
int32_t  orb_net_keep(uint32_t slot);     /* move the landed body into slot; frees the net buffer */
uint32_t orb_blob_len(uint32_t slot);     /* 0 = empty (never filled, freed, or reclaimed) */
void     orb_blob_free(uint32_t slot);
```

- 24 slots and **768 KB per app**, both *estimates* to be fixed in R3 from a stormy day's
  frame sizes. A keep past the budget is refused, and the app drops its oldest past frame
  first.
- **Lifetime:** the blobs live while the app is on the glass and for 15 minutes after it
  leaves, so a glance at the clock and back is instant. Then the OS frees them, and the
  app sees `orb_blob_len() == 0` and fetches again. The OS may also free them early when
  PSRAM runs short. Uninstall frees them at once.

**An indexed-PNG draw** (free; it only reads the app's own blobs and draws on its own
canvas):

```c
typedef struct {
    uint32_t slot;              /* a kept body: an 8-bit-or-less indexed PNG */
    int32_t  sx, sy;            /* the source window's top-left */
    int32_t  dx, dy, w, h;      /* where on the canvas, and how much */
    int32_t  scale;             /* 1; 2 doubles each source pixel (kept for an app that wants it) */
    const uint32_t *pal;        /* 256 ARGB entries in app memory; alpha 0 = leave the canvas */
} orb_png_draw_t;

int32_t orb_cv_png8(int32_t cv, const orb_png_draw_t *d);
```

- It reads colour type 3 at bit depths 1, 2, 4 and 8, non-interlaced, and refuses
  anything else with its reason. Rows are inflated through ROM `tinfl` into PSRAM
  (`tinfl_decompressor` and the 32 KB window are PSRAM, the two row buffers are 512 B
  each). The draw is native and row by row, with no whole-image buffer.
- The app's palette colours it, indexed by each pixel's **value**: the grey the PNG's own
  palette names for its index (`orb_png8_plte`), since LibreWXR numbers its greys afresh
  in every frame (§1.4). R3 shipped this sentence as "the app's palette, not the PNG's",
  indexing by the index; R3c's bench drew nothing with a dBZ ramp and corrected it. Alpha
  blends over the canvas (0 leaves it as it was), so the radar draws over the map, and the
  map over a plain fill.
- *Estimate:* 20–40 ms for a 512×512 radar frame, synchronous inside the guest call
  (`orb_service`'s budget is 250 ms wall). If R3 measures it over ~30 ms, or the UI
  stutters, the draw moves to the jobs worker with an `orb_cv_ready()`, the shape the
  photos cap uses.
- A host test lifts the reader into gcc, as `tools/host_sd_test.py` does the theme reader:
  good PNGs from Pillow at every depth, the refusals (RGBA, interlaced, a short `IDAT`, a
  bad CRC, a window outside the image), and mutants that prove the checks bite.

### 2.4 The map: an OS location fact, kept as vectors and drawn on the orb

**What it is:** for home and each place, the OS holds the map as **vectors, one level of
detail for each whole Mercator zoom, z6 to z11**, centred on the place, plus a **landmark
list**. The OS draws it on the orb at whatever radius, rotation and line widths an app
asks for (§0 ruling 11). An app colours it with its own palette, any part transparent, so
the radar paints a full map and a later flights a faint layer of landmarks from the same
data (§1.7's sheets, columns C and D, are the look to match).

**The roles** are what each feature is. They are unchanged from the raster draft, and are
also the drawing order and the priority:

| index | role | kind | | index | role | kind |
|---|---|---|---|---|---|---|
| 0 | water | fill | | 7 | state or province line | line |
| 1 | land | ground | | 8 | country border | line |
| 2 | urban area | fill | | 9 | coast | line |
| 3 | lake | fill | | 10 | city dot | mark |
| 4 | river | line | | 11 | airport mark | mark |
| 5 | minor road (FORMAT 2: primary) | line | | 12 | runway | line |
| 6 | major road (FORMAT 2: motorway, trunk) | line | | 13 | secondary road (FORMAT 2) | line |
| | | | | 14 | tertiary road (FORMAT 2) | line |

*FORMAT 2 (§6.9)* split the roads into four roles and draws them minor under major; index
15 is spare. A new role appends; a role's number never changes. Land is the
ground every map starts from, so it has no features.

**Levels of detail:** one file per whole zoom. For a radius r, the OS takes the zoom whose
own radius (233 px at that zoom) is nearest to r, so it draws at between 0.71× and 1.41×
that zoom's scale. Each level covers ±330 px around the place at its zoom: the 660 px
window §1.7 measured, which the drawing never leaves at 1.41×. A rotated map needs only the
glass's circle, so it fits too. Level z6 at DFW reaches 479 km × 1.41; level z11 is good to
about 11 km. Flights' whole range, 16–148 km, falls inside.

| level | z | radius at DFW | at 50°N | vertices at DFW (§1.8) |
|---|---|---|---|---|
| 0 | 6 | 479 km | 366 km | 119,788 |
| 1 | 7 | 239 km | 183 km | 125,654 |
| 2 | 8 | 120 km | 92 km | 93,870 |
| 3 | 9 | 60 km | 46 km | 174,677 |
| 4 | 10 | 30 km | 23 km | 175,560 |
| 5 | 11 | 15 km | 11 km | 110,947 |

**The file, `.orbmap`** (FORMAT 1), one per place and level. Its reader is pinned by a host
test with mutants, as the theme and app containers are.

- **Header:** magic `ORBM`, format, the place's `lat_e6` and `lon_e6`, the zoom, the
  feature and landmark counts, and a CRC32 over the body.
- **Features**, in role order: a role, a kind (fill or line), a ring count, and each ring's
  vertices. Coordinates are relative to the place, in 1/16 px at the level's zoom, as
  zigzag varint deltas. ±330 px is ±5,280 units, so a delta is one or two bytes.
- **Landmarks:** a kind (city, town, airport), a rank (OpenMapTiles' `rank`, lower is more
  important; an airport with an IATA code ranks 1), a position in the same units, and a
  name of up to 23 characters (an airport's IATA code, or its ICAO code when it has none).

The hub writes it already clipped to the window and simplified to a quarter pixel at its
zoom (§4: the tolerance and the size).

**How an app draws it** (under `location`; a map of a place says no more than the place
does). Drawing is split into a *view*, rendered once on the worker, and a *draw*, which is
cheap and happens every frame:

```c
typedef struct {
    int32_t  radius_m;       /* centre to the glass's edge */
    int32_t  rot_cdeg;       /* rotation, hundredths of a degree clockwise; 0 = north up */
    uint8_t  width[16];      /* line width per role, 0-4 px; 0 = not drawn; fills and marks ignore it */
    uint8_t  dash[16];       /* per line role: 0 solid, else a dash pattern id (§2.11's table) */
    uint8_t  detail;         /* the theme's detail preset: which roles and how many names */
} orb_map_view_t;

typedef struct {
    int16_t  x, y;           /* glass pixel in the view; may be off the glass */
    uint8_t  kind, rank;     /* ORB_MARK_CITY, ORB_MARK_TOWN, ORB_MARK_AIRPORT */
    char     name[24];
} orb_map_mark_t;

int32_t  orb_map_open(uint32_t place, const orb_map_view_t *v);  /* a view handle; renders on the worker */
int32_t  orb_map_ready(int32_t h);                               /* 1 ready, 0 rendering, <0 absent / no card / failed */
int32_t  orb_map_draw(int32_t cv, int32_t h, const uint32_t pal[16], int32_t dx, int32_t dy);
int32_t  orb_map_mark(int32_t h, uint32_t i, orb_map_mark_t *out);
int32_t  orb_map_project(int32_t h, int32_t lat_e6, int32_t lon_e6, int32_t *x, int32_t *y);
void     orb_map_close(int32_t h);
uint32_t orb_maps_gen(void);                                     /* moves when the hub remakes any place's maps */
```

- **`orb_map_open`** reads the level's file from the card into PSRAM, renders a 466×466
  4-bit role raster (108 KB, PSRAM) with the view's radius, rotation and widths, then frees
  the vectors. Fills are scanline-filled even-odd, so holes and islands come out right.
  Lines are drawn at their role's width and dash pattern (Blueprint's thin dashed lines),
  with the dash phase carried along each line so a pattern does not restart at every
  vertex, and marks last. Measured on Bronze (§1.9): 145–510 ms a render, plus
  190–530 ms to read the level if it is not already in PSRAM. **Each app keeps the
  levels it is using in PSRAM** (the radar all three of its own, ~395 KB at DFW), so
  only a new place or a new level pays the read. Opening a new view (a
  range or a rotation change) renders again; the old view draws until the new one is ready.
- **`orb_map_draw`** blends the view's role raster onto the canvas through the palette: 16
  ARGB entries, one per role, and alpha 0 leaves the canvas as it was. A palette change
  costs no render.
- **`orb_map_project`** puts a lat/lon on the view's pixels, rotation included, so an
  aircraft or a storm cell lines up with the map by construction.
- **`orb_map_mark`** gives the view's landmarks in glass pixels, best rank first. Names are
  never in the pixels: an app draws them in its own theme's font, and drops lower-ranked
  names that collide (§1.5: DFW, Dallas, Fort Worth and FTW overlap at Wide).
- **Views per app:** two (the one shown and the one rendering), freed when the app leaves
  the glass.

**How a theme colours it: an indexed layer with alpha.** The role raster holds what each
pixel is. The theme sets a colour and an opacity per role, which the app passes to
`orb_map_draw`, and a line width per role, which it passes in the view. The same map can
be:
- a solid ground: every role opaque, as the radar's map;
- a transparent overlay: land, water and urban at 0, roads thin at 30–50 %, runways and
  airports at full, over a plate or another app's own drawing (§1.7's column D).

The radar's rain works the same way: raw values 0–255 through a 256-entry ARGB ramp from
the theme, with 0 ("no echo") transparent.

**Later, not now:** drawing the vectors straight onto the canvas in colour with
antialiasing, which a role raster cannot do. It would need a cached RGB565 view (434 KB)
instead of the 108 KB role raster. The API above does not change for it.

**Where it is kept:** on the card, in an OS folder:
`/System/Maps/<place>-<lat_e4>_<lon_e4>-z<z>.orbmap`, six files a place. A moved place has
new names, so an old map is never drawn for it. **No card, or the USB window:** a view
answers absent, and apps draw without a map. The radar shows its rings and the place's dot,
with "No map yet" in quiet text. **This makes the card a runtime read path for the OS's
maps**, beside the gallery's photos (gallery §0 ruling 7). The reasoning is the gallery's:
the dial keeps drawing from flash, and only the map goes dark in the USB window.

**Who makes it: a tool in the hub, never shipped data** (§0 ruling 7). The tool runs in
the browser (D33), on the Orb tab. **Saving home or a place fetches what that place needs
and writes its six files.** It writes them through `/api/v1/sd/mkdir` and
`/api/v1/sd/write`, then tells the OS (`POST /api/v1/places/maps`), which moves
`orb_maps_gen`. The page compares the card's `/System/Maps` listing against home and the
places, and shows **"Maps out of date — Make maps"** in loud amber with a tab badge when one
is missing or stale (a hub from before the maps, a card swapped, a failed fetch, an older
FORMAT). It deletes the files no place uses. No app's settings page is involved.

- **The source:** a **Map tiles** row on the Orb tab, a TileJSON URL defaulting to
  `https://tiles.openfreemap.org/planet` (§1.6). Any OpenMapTiles-schema source works, so a
  self-hosted planet is the same tool pointed elsewhere. The tool reads the TileJSON each
  time, so OpenFreeMap's weekly builds need nothing from us.
- **The fetch:** vector tiles at z6–z11, the 3×3 or 4×4 block that covers ±330 px at each.
  **Measured at DFW: 81 tiles, 8.8 MB** (§1.7), 4 requests at a time: inside OpenFreeMap's
  3,000-tile and 50 MB offline limits for each save. Tiles are cached in the browser's
  Cache Storage, so rewriting the files after a lost card fetches nothing.
- **Through the hub's forwarder** (§0 ruling 10): the page asks the hub, the hub asks the
  tile host with `gorb-os/<version> (+https://github.com/gregtakacs/gorb-os)`, and the
  bytes come back untouched. It forwards only to the host the Map tiles row names, so it
  is never an open proxy. It caps itself at 4 requests in flight, and `security.go`'s
  allowlist and the fake orb grow the route.
- **The decoder:** MVT is protobuf. The tool carries a small decoder in the hub's vendor
  directory (`@mapbox/vector-tile` and `pbf`, BSD-3, a few tens of KB), or one of its own
  (§1.7's preview used a 120-line one). Its licence is the only code shipped.
- **Classes to roles**, one table:
  - `water` → water (`ocean`) or lake; `waterway` river and canal → river; `landuse`
    residential, commercial, industrial and retail → urban area;
  - `transportation` motorway, trunk and primary → major road; secondary and tertiary →
    minor road, from z9;
  - `boundary` admin levels 2 and 4 (not maritime) → border and state line; `aeroway`
    runway → runway.

  Landmarks come from `place` (city and town, villages from z10) and `aerodrome_label`.
- **The encoding:** tile coordinates to place-centred 1/16 px, clipped to the window,
  simplified (Douglas–Peucker at a quarter pixel), then written in role order. Pieces of
  one feature cut at tile edges stay separate pieces; fills overlap harmlessly where tiles'
  buffers do.
- **A preview on the Places card:** the same rasteriser in JS draws the place's map at a
  chosen radius, so the owner sees what the orb will draw. A host test holds the C and JS
  rasterisers to the same pixels on a set of fixture files (the pixdiff discipline).
- **Sizes:** measured at DFW, about 800,000 vertices a place before simplification, ~2 MB
  at 2.5 B a vertex (§1.8). The 400–600 KB estimate after it was low: *measured (R4,
  §6.6)* 0.84 MB for Austin, 1.19 MB for DFW and 1.92 MB for London, six levels each.
  Still nothing on a 29 GB card (owner, 2026-11-07).
- **Attribution** (§0 ruling 9, revised): the first map an app draws after a boot puts
  "Map data: OpenStreetMap" on the glass for five seconds (the OS does it, so every map app
  gets it); ABOUT carries "OpenStreetMap contributors", openstreetmap.org/copyright,
  "OpenMapTiles, OpenFreeMap", LibreWXR and Open-Meteo. The Places card credits
  OpenStreetMap and names the source in its Map tiles row.

### 2.5 The app (`apps/radar`)

**Manifest:**

```json
{
  "slug": "radar",
  "title": "Radar",
  "caps": ["net", "location"],
  "period_ms": 250,
  "memory_kb": 64,
  "settings": [
    {"id": "zoom",   "type": "enum",   "min": 0, "max": 2, "default": 1},
    {"id": "source", "type": "string"}
  ],
  "shelf": true,
  "os_min": "<R3's release>",
  "editor": "editor"
}
```

`zoom` is 0 wide, 1 region, 2 local. An empty `source` means the default URL (§2.2).
These two are the radar's only settings, and its options page carries only the zoom: every
colour, width, dash, preset and timing is the theme's (§0 ruling 17).

**What it keeps** in its 64 KB: the frame list (18 × time, path and blob slot, about
1 KB), the palettes (1 KB for rain, 64 B for the map), the place on the glass and its zoom.
The frames are blobs (§2.3); the map is the OS's (§2.4). *As built (§6.8):* a stormy day's
frames are 40–70 KB, so 18 can pass the OS's 768 KB; when a frame will not fit, the older
past frames go unfetched for that list and the loop starts later.

**Zoom:** *(six steps since §6.9: Wide, Region, Local, Area, City, Close, the map's z6 to
z11; frames stop at Local's z7 and the closer steps magnify them 2, 4 and 8 times, smoothed
on the orb.)* Three steps, the map's z6, z7 and z8: 479, 239 and 120 km from the centre to the
edge at DFW, and 366, 183 and 92 km at 50°N. **Each asks LibreWXR for its 512 px image one
zoom lower** (z5, z6, z7), which is the same scale (§1.4). The app opens a map view at
exactly that radius (`orb_map_open`) and shifts the frame by the server's rounding of its
centre (under half a pixel, computed from `window_origin`'s formula), so the map and the
rain line up. No step doubles a frame. The step is changed by
press-and-turn (§0 ruling 12), and the `zoom` setting is where it starts. Each step's
frames are fetched separately, and a step change opens a new map view.

**The knob** (app-platform-plan §51: one dial per app, centred on its home, no arming):

- **The dial is time, centred on Now:** anticlockwise steps back through the past in
  10-minute steps (to −110 min); clockwise steps through the nowcast (to +60 min). The
  accelerated spin and the idle return to Now are §51's.
- **Untouched**, the loop plays from the oldest past frame to Now at 4 frames a second,
  holds Now for 2 s, and repeats. A turn stops it; the idle return starts it again.
- **A press** moves to the next place (Home → 1 → 2 → 3 → Home, skipping empty ones), at
  the same step, as Weather's does.
- **Press-and-turn** zooms (§2.10): clockwise closer (Wide → Region → Local),
  anticlockwise wider; the ends stop. The rim shows the three steps while it is used.
- **A hold** is the OS's app list and menu.

**On the glass**, back to front (*as built:* no canvas: the map in live layer 0, drawn
once a view, the rain in live layer 1, cleared and drawn alone each step, §6.5; the
overlay is the theme's rows and glass):

1. The ground fill, then the map (`orb_map_draw` of the open view, with the theme's role
   palette; the view carries the theme's line widths).
2. Areas with no radar dimmed, from the coverage image.
3. The radar frame, its raw values through the theme's rain palette (§4: dBZ), with alpha 0
   for "no echo" so the map shows through.
4. The overlay: the place's dot, the range ring labelled in the orb's distance unit
   (`orb_units`), the landmark names the theme shows, the place name, and the time
   ("−40 min", "Now", "+30 min"). Nowcast frames are marked as a forecast (§2.9), so a
   forecast is never read as an observation.

*Estimate:* one step costs the map's blend from its role raster, the radar decode and the
overlay, 30–60 ms, inside 250 ms at 4 fps. The map's render (§2.4) happens once a view, not
once a step. *Measured (§6.8):* 85–90 ms a step on Bronze (the frame's decode and blend;
the map is not redrawn), four a second while the loop plays, which `orb_burst` holds up:
a theme with no moving rows ticks its app once a second.

### 2.6 Budget

| | radar, on the glass | off the glass |
|---|---|---|
| Flash, image | ~15–25 KB: the PNG reader, the `.orbmap` reader and rasteriser, places, the routes (*estimate*) | — |
| Flash, `apps` ledger | one slot; *estimate* 20–30 KB of wasm | — |
| New partition | none | — |
| PSRAM | canvas 434,312 B + 18 frames at 1.5–32 KB (measured range, *estimate* ~180 KB on a quiet day) + the map view's role raster 108,578 B (two while a new one renders) + coverage ~1.3 KB: **~0.73 MB** steady; while a view renders, its level's vectors too (*estimate* 100–430 KB, freed after) | the blobs for 15 min, then nothing |
| Internal RAM | nothing new beyond the HTTPS request Weather already makes (*to confirm*, §4) | — |
| Card | the OS's maps, ~2 MB a place unsimplified (measured at DFW, §1.8), so up to ~8 MB for home and three places | the same |
| Network | ~10–80 KB per 10 min (the new past frame plus six nowcast); a cold start ~200 KB in 20 requests | nothing |
| NVS | `orb.places`: three ~48 B records | — |

The reference holds 4.1 MB of PSRAM and 4 MB of flash all the time (§1.3). Off the glass,
the radar costs the other apps nothing but its ledger slot.

### 2.7 What the other apps see

- **Weather** reads the places from the OS (§2.8).
- **Flights** can open map views, read the landmarks and use `orb_map_project` from the
  release R3 lands in. Any range radius in any unit works, and the view can rotate. Using
  them is a later flights plan (§0 ruling 6); nothing in flights changes here.
- **The clock, the picker:** nothing.
- **PSRAM:** the radar's ~0.73 MB is in use only while it is on the glass, when no other
  app's page is. The carousel's preview planes are unaffected.

### 2.8 Weather adopts the OS places

- Weather **v1.4.0** (v1.3.0 shipped first, with pressure and lightning): `place1..3` leave `app.json` and `settings.json`; `load_places()`
  (`apps/weather/main.c:112`) reads `orb_place(0..2)`; the redraw rule watches
  `orb_places_gen()` beside `orb_home_gen()`. `os_min` names R1's release.
- Its old place strings go with the settings it no longer declares (§0 ruling 5). Until
  the owner sets places on the Orb tab, Weather shows Home alone.
- Its settings help keeps the Open-Meteo credit and points at the Orb tab for the places.
- Bump `version.txt` and re-bake its shelf theme (`tools/rebake_shelf.py`).

### 2.9 The theme editor (`apps/radar/editor`): a proposal

Built on the kit (theme-editor-kit-plan: start sheet, screen calibration, colour picker,
CSS), with Weather's editor as the model, in steps:

1. **Start:** the five starter themes as tiles, plus Blank, or one from the hub library:
   **Scope** (phosphor green on black, flights' Scope), **Paper** (a printed road atlas),
   **Station** (the TV weather map: slate land, white borders, the broadcast rain ramp),
   **Vintage** and **Blueprint** (§0 ruling 13, below). The mock-up shows the first three.
2. **Ground:** the plate (colour, gradient or a picture), the rim, and the dim applied
   where there is no radar coverage.
3. **Map:** a detail preset, a names preset and a colour scheme (below), then for each
   role a colour, an opacity and an off switch, and for the line roles a width (1–4 px) and
   a dash pattern. Previewed on the owner's own home map at all three zoom
   steps: the editor reads the `.orbmap` files from the card and draws them with the hub's
   JS rasteriser, which matches the orb's pixels (§2.4). Also which landmark kinds show
   names, and from which rank down.
4. **Rain:** the ramp's stops over the raw values, with the same lightness check as
   Weather's Temperature step (stops must differ in lightness, not hue alone, so the ramp
   still reads to colour-blind eyes and on a dim glass). The stops are set in dBZ (10 to 75),
   since the raw value is dBZ (§1.4). No snow ramp: the raw frames carry no precipitation
   type. The **forecast treatment**: dimmed, desaturated, hatched or outlined, so the
   nowcast never looks observed.
5. **Overlay:** the place marker, the range rings (how many, the line, the label), the
   bearing ticks on the rim, and the **time track**: an arc on the rim from −2 h to +1 h,
   one tick a frame, a mark at Now and a pointer on the frame shown. Since the dial is
   time, this arc is the dial's scale.
6. **Life:** an optional sweep (a rotating line with a fading trail, Scope's signature),
   the loop's speed, and how long Now is held.
7. **Words:** fonts and tones for the place name, the time, the range label and the
   landmark names, as the kit's other editors do.
8. **Render:** all three steps at true size (177 dpi), the carousel's 232 px preview, the
   pool size of the bake, then Send to orb.

**Presets in the Map step** (§0 ruling 13): a theme starts from a preset and its switches,
then its colours can be edited one by one.
- **Detail:** Minimal (major roads, borders, lakes, cities), Standard (adds rivers, urban
  areas, state lines, runways) and Detailed (adds minor roads and towns). Each feature then
  has its own on/off switch.
- **Names:** Off, Few (the top-ranked handful), Normal and Many, plus which kinds are named
  (cities, towns, airports).
- **Colour schemes:** one per starter (Scope, Paper, Station, Vintage, Blueprint), applied
  to all thirteen roles at once. Blueprint's lines are thin and dashed (state lines and
  minor roads dashed, borders long-dashed).

**Five starters** (§0 ruling 13): Scope, Paper and Station as in the mock-up; **Vintage**
(the brass and parchment of the clock's vintage looks, sepia map, a muted rain ramp);
**Blueprint** (cyan lines on blueprint blue, thin and dashed, the rain as white-to-cyan
contours). Each matches the same-named looks in Weather and Flights.

**Canvas and theme: a live layer on the stage** (proposed 2026-11-07, R0). The stage draws
a theme's plate, rows and text on the dial's own render path; an LVGL canvas over it forces
the slower LVGL path, and would hide the theme's rows under it. So the radar draws no
canvas. **The stage gains a live layer:** a full-glass plane stacked among the theme's rows,
whose pixels are the app's, held by the OS in PSRAM (RGB565 plus an alpha plane, always,
so a later Flights can lay a map over its plate).
- *As built (R3c):* its depth is a number on the rows' scale (0-63) that the theme carries
  in its values (§2.11) and the app passes to `orb_stage_live(layer, order, argb)`; the
  theme format gains no element for it, so no reader, writer or bake changed. The editor's
  row for it is the radar editor's (R7), writing that number.
- **Two layers an app** (owner, 2026-11-07, §6.5): the radar keeps its map in one, drawn
  once a view, and redraws only the rain in the other. Each has its own depth, so a theme
  can put rows (range rings, the place marker) between map and rain. Flights needs one.
- `orb_map_draw` and `orb_cv_png8` take a layer (`ORB_CV_LIVE0`, `ORB_CV_LIVE1`) as their
  target as well as a canvas, and each draw repaints the box it covered; there is no
  separate damage call (the earlier `orb_stage_layer_damage` was never built).
- The theme's own rows sit below it (the plate) and above it (the rim's time track, the
  rings, the place marker, the labels in the theme's fonts), all still on the fast path.
- To the compositor it is a sprite whose pixels live outside the theme bank; the editor
  shows it as a row it can move in the stack. Approved (§0 ruling 16); its own slice
  (R3c); the radar's glass (§2.5) is drawn this way, not with a canvas.

### 2.10 Press-and-turn, in the OS (§0 ruling 12)

**In the knob layer** (`main/knob/knob.c`, as benched on Bronze 2026-11-07):

- A click committed while the switch is down, and before the hold has fired, goes to its
  own stream (`knob_take_push_delta()`), never to `knob_take_delta()`. The first one
  makes the press a press-and-turn: the hold never fires, and the release is spent.
- **Intent holds the hold back.** Once the shaft has moved half a click since the press,
  the hold waits, however slowly the first click arrives. The first bench build fired
  the hold under a slow start and opened the app list (owner, 2026-11-07).
- **A release before the first click:** under the hold time it is a plain press, so a
  wobbly press is never lost. Past the hold time with intent seen, it is spent, so an
  abandoned turn does not open the app list.
- A hold without movement fires at 700 ms while down, as before. `/api/v1/status` counts
  press-and-turns as `knob.push`.

**To an app:** an optional export, `void orb_push(int32_t delta)`, signed like
`orb_input`'s turns. Having it is the opt-in, so no manifest key is needed. An app without
it gets the clicks as ordinary turns, and the release fires nothing. The carousel discards
them. The SDK's `orb_rt.c`, `binds.json` and the template learn the export, and
`orb_dial.h` gains nothing: zoom is the app's own state.

**Tests:** `tools/host_knob_test.py` grows cases driven through the switch and
quadrature sequences: a click while down, intent then release under and over the hold
time, a hold with no movement, and a click after the hold has fired. There are mutants
for each gate.

### 2.11 A theme's values (§0 ruling 14)

Themes today carry rows, sprites, fonts and sounds (`apps/sdk/orb_stage.h`). Weather's
colours live in its baked sprites. The radar's settings are plain values, so the theme
format gains **a values section**: a short, versioned, app-defined blob baked by the
app's editor and read by the app at `orb_stage_open` time.

- **The OS reads nothing in it.** It checks the section's size and CRC and hands the bytes
  to the app (`orb_stage_values(void *buf, uint32_t cap)`).
- **Read-only, and theme-locked** (§0 ruling 17). The bytes sit in the theme's bank,
  mapped from flash; `orb_stage_values` copies them out to the theme's own app and nothing
  writes them. No route, hub page, app setting or options row reads or edits them. An app
  may not offer any of them as a setting: what a theme sets, only a new bake changes. The app owns the layout and
  its version, so a new radar setting never changes the OS.
- **The radar's layout, version 1** (*estimate* ~1.6 KB): the map's 16 role colours
  (ARGB), widths and dash ids; the detail preset and the per-role switches; the names
  preset and kinds; the rain ramp as 256 ARGB entries and the snow ramp; the forecast
  style; the overlay's styles; the sweep and loop timing.
- **Dash patterns** are a small fixed table in the OS: solid, short, long, dot, dash-dot.
  A theme names one by id, so the renderer never parses a pattern.
- **One format change, made together:** the reader, `theme_schema`, the bakers
  (`baker@1`) and the host suites move as one change, as the sparse fonts did (§56).
  An older OS refuses a theme that carries the section, by `os_min`.

### 2.12 The card, scheduled (§0 ruling 15)

The card has one mutex (`orb_sd_lock`, `main/sd/orb_sd.h`), held for each whole operation.
Map reads join the hub's uploads and installs, the theme store and the gallery, so they
follow rules that keep everyone moving:

- **Reads happen on the worker, never on the UI task or an httpd worker.** A view renders
  only after its file is in PSRAM.
- **Bounded holds.** A map is read in 32 KB pieces, releasing the lock between pieces, so
  a hub write waits for at most one piece: 48–59 ms measured (the card reads ~650 KB/s).
  The header's CRC is checked once the whole file is in, and a file that changed
  underneath is read again.
- **Writes win, and it has to be built.** Every card writer marks the card as written once
  a chunk (`orb_sd_note_write()`: the file manager, the theme upload, the app upload), and
  a map read stands aside while `orb_sd_write_idle_ms()` is under a second; the shown view
  keeps drawing. A map is never worth slowing an install. Measured (R0, Bronze, a 3 MB
  write): alone 12.5–17.8 s; beside a reader looping without the rule 30.9–44.0 s, since
  the upload takes the lock per chunk and the reader slips in between; beside the same
  reader with the rule 14.3–16.6 s, the reader standing aside 32.5 s.
- **Atomic maps.** The hub writes each `.orbmap` to a temporary name and renames it into
  place, so a reader sees the old file or the new one, never half of one.
  `POST /api/v1/places/maps` moves `orb_maps_gen` only after the last rename.
- **The USB window** gives the card away whole: map reads answer "no card" until it
  returns, as the gallery's do.
- **Measured in R0**: above, through `map_bench`'s background reader (`bg=`, `yield=`).

## 3. Slices

| slice | what | depends on |
|---|---|---|
| R0 | §4's checks that change the design. **First, the vector render's time:** a host rasteriser in C over DFW's densest level, then the same code in a test build on Bronze, rendering from the card. Then the point image's centre and scale against a map, the raw value → dBZ mapping, nowcast path stability, canvas plus theme, and the card lock's longest hold with a map read beside a theme upload | — |
| R1b | press-and-turn in the OS (§2.10): the knob layer as benched, the `orb_push` export, the router and the SDK, the host cases | — |
| R1 | places in the OS: `orb_places.c`, NVS, the imports, `binds --write`, the routes, the hub's allowlist and fake orb, the Places card on the Orb tab; a host test with mutants | — |
| R2 | Weather v1.4.0 reads the OS places; the place rows leave its App settings page | R1 |
| R3 | the OS surface: kept bodies and `orb_cv_png8`; the maps: the `.orbmap` FORMAT 1 reader, the rasteriser, views on the worker, the map imports, `/System/Maps`, the gen route; host tests with mutants for the PNG reader, the `.orbmap` reader and the rasteriser; dash patterns, the card's scheduling (§2.12), the render and draw times measured on Bronze | R0, R1 |
| R3c | the stage's live layer (§2.9): its depth, its PSRAM planes, damage, `orb_map_draw`/`orb_cv_png8` targeting it, host and bench checks (the editor's row moved to R7; §6.4) | R0 |
| R3b | a theme's values section (§2.11): the reader, `orb_stage_values`, `theme_schema`, `baker@1`, the host suites, `os_min` | R0 |
| R4 | the hub's map tool: the Map tiles row, the Go forwarder with its User-Agent, the tile fetch and cache, the MVT decoder, the class-to-role table, clipping, simplifying and the `.orbmap` writer for six levels with their landmarks, the JS rasteriser and the Places card preview, the C/JS pixel parity test, the writes and clean-up, the amber "Maps out of date" state | R0, R1 |
| R5 | `apps/radar`: the frame list, fetch order, refresh rules, the knob, the glass, the no-map and no-coverage states, one baked starter on the shelf | R3, R3b, R3c, R4 |
| R6 | bench on Bronze: a stormy day and a quiet one, PSRAM and internal low points from `/status` across a cold start and an hour on the glass, the step time at 4 fps, a card pulled mid-read | R5 |
| R7 | `apps/radar/editor`: §2.9's steps, the map presets, the five starters | R5, R3b, R0's canvas-plus-theme answer |

## 4. Owed

1. ~~Alignment~~ **Answered 2026-11-07** (§1.4): a 512 px image at z is zoom z+1's scale,
   centred to the nearest pixel; checked pixel for pixel against the server's own tiles.
2. ~~The raw values~~ **Answered 2026-11-07** (§1.4): dBZ = value / 2 − 32, floor 10 dBZ,
   no snow in raw.
3. ~~Nowcast paths~~ **Answered 2026-11-07** (§2.2): paths are times and stay put; a time's
   path changes meaning from forecast to observation. Owed: whether a nowcast frame's ETag
   changes between cycles (the conditional request copes either way).
4. **Frame sizes on a stormy day** at each zoom, to fix the blob budget. Seattle on
   2026-11-07, 6–10 % of the image raining: 18.6 KB (Wide), 24.7 KB (Region), 10.9 KB
   (Local), so 18 frames ≈ 0.45 MB. A widespread storm is still owed; past the 768 KB
   budget the app drops its oldest past frames.
5. **The times on Bronze:** ~~the vector render of a view at the densest level~~
   *measured 2026-11-07* (§1.9: 145–510 ms, reads 190–530 ms); `orb_cv_png8` and `orb_map_draw`, and
   whether a synchronous draw shows in the UI (§2.3). If the render is far slower, the
   fallback is §1.7's hub-drawn pictures behind the same imports.
6. **Internal RAM** during the cold start's 20 HTTPS requests in a row: does the OS client
   keep the connection alive, and what is the internal low point? Weather makes one request
   per 30 minutes; this makes twenty in a few seconds. *Found in R3c (§6.4):* with the
   clock on the glass Bronze has ~23 KB internal free, the largest block 7.2-7.4 KB, and
   R3's map task taking 6 KB of it was enough to stop Wi-Fi transmits. The radar's own
   headroom on the glass is R6's to measure, against this number.
7. ~~The map tool's cost~~ **Measured 2026-11-07** (§6.6): 74–85 tiles a place; Austin
   and London together 23 MB fetched and written in 25 s, Austin alone 9.5 s from the
   page; 0.84–1.92 MB on the card a place. The quarter-pixel tolerance stands; how lines
   look against it is R7's to judge in the editor.
8. ~~Canvas plus theme~~ **Ruled 2026-11-07** (§0 ruling 16): the live layer, R3c.
9. **The OSMF tile policy's current text**, to confirm §1.3's reading before anything is
   said about it in a public doc or issue.
10. ~~OpenFreeMap's User-Agent clause~~ **Settled by §0 ruling 10.** Their terms (read
    2026-11-07) ask an offline download for "a unique User-Agent identifying your app".
    Chrome does not let a page set one, and the hub page's Referer is a LAN address, so
    the Go side forwards with its own.
11. ~~OpenStreetMap's attribution guidelines~~ **Read 2026-11-07** (OSMF, adopted
    2021-06-25, page as edited 2026-09-10): attribution "must be placed in the vicinity of
    the produced work" and must not need an interaction to be seen; on an interactive map
    it may fade "automatically after five seconds", after which an "About" option is
    where it lives; a splash need not repeat every time; the licence link must be "in an
    easily locatable part" of the application; "Map data from OpenStreetMap" is a named
    form. ABOUT alone fell short, so ruling 9 was revised (once a boot, five seconds on the
    glass); built in §6.7.
12. ~~The priority shrink's weight~~ **Gone with §0 ruling 11:** vectors are drawn at the
    view's scale, so nothing is shrunk, and line weight is the theme's choice per role.
13. **The classic path rounds the live layer differently** (R3c, §6.4): over a pixel the
    layer half covers, LVGL's blend lands one RGB565 step under L4's (land at alpha 160:
    L4 2/8/5, LVGL 1/7/4, the exact blend 1.63/7.63/4.5). `orb_lv_blend.c`'s rounding
    hooks claim this blend, so some branch for an unturned RGB565A8 image is not hooked.
    One step, on the fallback path; which branch is owed.
14. **A theme label recomposed under a plan** probably goes stale the way the live layer
    did before `gfx_prep_stale` (§6.4): its plane changes under a pose that does not, and
    pre-paint keys on the pose. Not benched; the same call at `label_compose` would cover
    it.
15. **Pre-paint's exactness check (`prep=2`) cannot see a stale slow job**: with the stale
    call removed it still counted 0 differing pixels while the glass kept the old layer.
    It compares against planes built from the same stale state.

## 5. Open

- **Flights' map** is a later plan (§0 ruling 6). How a map gets under stage rows is
  answered: the live layer (§2.9, R3c), at a depth under the aircraft rows.
- **Running LibreWXR on the NAS**: the owner is ready to (§0 ruling 8). The `source`
  setting exists from R5.
- **Lightning** (`/v2/lightning`, GeoJSON by point and radius) as marks on the radar, after
  R6. JSON through the OS tokenizer, no new surface.
- **More than three places**: the OS record has room, but Weather's 32 rows and one-request
  URL (weather-plan §6.1: 389 B of 480) were sized for four.

## 6. As built

### 6.1 R0, R1, R1b, R2 (2026-11-07, Bronze, OS v1.2.19, uncommitted)

- **R0.** §1.4, §1.9 and §2.12 hold the measurements. The bench pieces stay in the tree:
  `tools/orbmap.py` (the FORMAT 1 writer and reader), `tools/mvt.py`,
  `main/map/orb_map_raster.c` with `tools/host_map_raster_test.py`, and the debug-only
  `GET /api/v1/debug/map_bench` (`bg=`, `yield=`, `roles=`, `dump=`), exempt from the hub's
  allowlist like the other bench doors. `orb_sd_note_write()` / `orb_sd_write_idle_ms()`
  are in `orb_sd.c`, called by the file manager, the theme upload and the app upload.
- **R1, places.** `main/net/orb_places.c` (NVS `orb.places`, keys `p0`–`p2`), the broker's
  `place` entry behind `location`, the imports `place` and `places_gen` (72 in
  `binds.json`), `GET`/`POST /api/v1/places` (58 routes of 64), the hub's allowlist, fake
  orb (`internal/orbfake/places.go`) and the Places card on the Orb tab, which reuses W4's
  searcher with an encoder that keeps the name whole. Benched: a point without a name is
  named from the city list ("London"), slot 3, a latitude off the globe and an escape are
  refused, a cleared slot leaves the others' numbers, and the places survive a reboot.
  `tools/host_places_test.py`: the checks, the 52 B record against the import, 7 of 7
  mutants. `host_svc_test.py`: the entry is wired and refused before the read.
- **R1b, press-and-turn to apps.** The rules moved into four pure functions in `knob.c`
  (`kg_press`, `kg_click`, `kg_poll`, `kg_release`), which the ISR, the commit and
  `knob_poll` call. `host_knob_test.py` lifts them and drives 25 cases, 10 of 10 mutants.
  The iface gains `push`; the wasm module calls a guest's `orb_push` export (appended as
  `W_PUSH`, entry 16) and the router hands the clicks to it at the armed step, or folds them
  into the turn when nobody takes them. The carousel discards them.
- **R2, Weather v1.4.0** (os_min v1.2.19): `load_places()` reads `orb_place(0..2)` and
  watches `orb_places_gen()`; `place1..3` left `app.json`, and `settings.json` is empty.
  Scope re-baked on the shelf (v1.4.0). Benched: setting a place on the orb refetched the
  forecast for it at once (`WX ok 200 places=3`). **The Open-Meteo credit** lived in the
  place rows' help, which are gone: it moves to gorb-os's ABOUT page with the map credits
  (owed with the ABOUT page's change).

### 6.2 R3, the OS surface (2026-11-07, Bronze, OS v1.2.20, uncommitted)

- **Kept bodies.** `main/app/orb_app_blob.c`: 24 slots and 768 KB an app, freed 15 min off
  the glass (`orb_blob_note_glass`, on the service pass) and at release.
  `orb_app_net_take()` hands the landed body over without a copy. Imports `net_keep`
  (cap net; refused past the budget with the body left in place), `blob_len`, `blob_free`.
- **`orb_cv_png8`.** `main/gfx/orb_png8.c` reads colour type 3 at 1, 2, 4 and 8 bits over
  the ROM's `tinfl` (`esp_rom/include/miniz.h`), a 32 KB window and two rows: 46,836 B of
  scratch, shared by every draw on the LVGL task. `orb_ui_cv_png8` blends the window
  through the app's 256 ARGB words, optionally doubled. **On Bronze: 33–35 ms** for a real
  512x512 LibreWXR frame (Seattle's rain). `tools/host_png8_test.py` builds the reader
  against a zlib stand-in for `tinfl` that also checks every write lands where the ROM's
  window needs it; every filter type and depth, a 512x512 frame split into 997-byte
  IDATs, a 1-byte IDAT, and the refusals; 11 of 11 mutants.
- **Maps.** `main/map/orb_map.c`: two views an app, rendered on a task of their own
  (priority 2, under the LVGL task), the card read in 32 KB pieces standing aside while
  the card is written, the CRC checked and a changed file read again, three level files an
  app cached in PSRAM, a busy mutex so a release never frees memory a render is using.
  **The view as built** (`orb_map_view_t`, 44 B) carries `role_on` and `mark_on` masks
  where §2.4 said "detail": the app turns its theme's preset into them. Files are named
  `<place>-<lat_e4>_<lon_e4>-z<z>.orbmap` under `/System/Maps` (`orb_map_file_name`;
  `tools/orbmap.py build … --place N` names them the same way). Imports `map_open`,
  `map_ready`, `map_draw`, `map_mark`, `map_project`, `map_close`, `maps_gen` (cap
  location; 83 in `binds.json`), and `POST /api/v1/places/maps` moves the gen and drops
  every cached file (allowlisted, faked).
- **On Bronze, home near Fort Worth, maps from `orbmap.py`:** a view opens to READY in
  360–890 ms with its card read, and re-renders from the cache in ~190 ms (rotated 30°,
  dashed); home projects to (233, 233) at every range, and DFW airport 11.3 km east lands
  6 px out at Wide and 88 px at 30 km, as the scales say. The device's raster for
  r = 120 km equals the host renderer's for the same file and scale: 0 of 217,156
  pixels differ. An unset place answers ABSENT.
- **Bench doors** (debug builds only, exempt from the hub's allowlist): `map_view`,
  `png_bench`, beside R0's `map_bench`.
- **Owed to R5:** the time of `orb_map_draw` (a 466x466 palette blend) and of
  `orb_cv_png8` onto a real canvas, measured in the app at 4 fps.

### 6.3 R3b, a theme's values (2026-11-07, OS v1.2.21, uncommitted)

- **The class.** `values` is class 11, appended without moving the ABI word (the AUX,
  PREVIEW and SOUND precedent): `orb_assets.h` (`ORB_ASSET_VALUES`, priced w*h),
  `orb_clock3.c`, `orb_elems.c`, `theme_schema.py` (`CLASS_VALUES`, `VALUES_MAX` 4096),
  `orbapp.py`'s asset classes and `baker@1`, each with the same rules in the same words:
  one row at slot 0, one plane of 1 to 4096 bytes by 1, placing nothing, the app's layout
  version in `src` (1-255; `src` is a u8, so §2.11's layout version is too).
- **The way in and out.** An editor calls `B.values(bytes, version)` on `GorbBake.stage3`;
  the projection records where the bytes sit (`out_values_*`, kept in the theme's
  geometry), the stage never draws the row, and an app reads them with
  `orb_stage_values(buf, cap, &ver)` (84 imports). No values, or no theme bound, answers
  0 and the app's own defaults stand.
- **Tests.** `host_clock3_test.py`: an accepted 40-byte blob found at its plane with its
  version, a theme with none answering (0, 0, 0), seven refusals the device and the writer
  word alike, four guards each caught by its mutant, and a mutant pricing the plane as
  pixels caught on the accept side. The h == 1 rule has no mutant: with the walk's w*h
  price and size == w, a two-row plane is refused either way. `headless-stage.test.js`:
  a blob baked, read back through the card's reader byte for byte, and four refusals.
  `make check-editor` now runs here (Node 22 LTS under `~/.local/opt`).
- **On Bronze:** the clock's Wave and Weather's Scope still project and draw; no faults.
  The first theme that carries values is the radar's (R5/R7), which exercises
  `orb_stage_values` on the glass.
- **Found, not fixed:** `python3 tools/theme_schema.py`'s self-test stops at its "seats 32
  moving rows" case, on HEAD too: the row ceiling rose to 64 (§53) and that case was not
  moved with it. `make check-host` does not run the self-test.

### 6.4 R3c, the live layer (2026-11-07, Bronze, OS v1.2.22, uncommitted)

- **The layer.** `stage_live_open(order, argb)` (`main/stage/orb_stage.c`) allocates one
  PSRAM plane, 16 + 466x466x3 B (651 KB, RGB565 then A8), fills it, and stacks it at
  `depth_key(order, false)`: above the rows at that depth, under the glass. Again at the
  same order it only fills; another order re-stacks; -1 frees it, as does release. In
  the L4 table it is a row like the glass overlay (`live_desc`: a plate-class row whose
  plane is the stage's), picked into `clock_layers_build`'s depth sort, so a re-stack is
  a rebuild of the table (L4's z is table order). On the classic path it is an image
  placed by `live_restack`: every widget `stage_init` builds now carries its depth key
  + 1 as user data, and the image goes before the first one deeper than it; it counts as
  the stage's own child, so it never trips the L4 veto meant for an app's widgets.
- **The way in.** One import, `orb_stage_live(order, argb)` (85 imports), behind the
  stage's binding like every `stage_*` call. `orb_cv_png8` and `orb_map_draw` take
  `ORB_CV_LIVE` (-1) as their canvas and repaint the box they drew (`stage_live_damage`:
  row marks under L4, an invalidate on the classic path). The draws were generalised over
  a target (`orb_ui_tgt_t`): a canvas is opaque as before; the layer's alpha plane takes
  src-over with the coverage added up and the colours weighted by what each contributes.
- **Pre-paint.** A slow job whose pixels change under an unmoved pose was invisible to
  pre-paint's planes, so the damage also calls `gfx_prep_stale` (new in `orb_gfx.c`),
  which rebuilds the box from the new pixels. Proved on Bronze under the clock with
  `prep=1`: a same-order redraw that adds rain showed the old layer without the call and
  the rain with it (owed 15: `prep=2` saw neither as a mismatch).
- **PNG values.** The bench found §2.3's draw indexing the app's palette by the PNG's
  index, which LibreWXR assigns per frame; `orb_png8_plte` reads the palette's first
  channel and the draw indexes by that value (§1.4 and §2.3 corrected in place).
- **The map task's stack** moved to PSRAM (`orb_map.c`, the `appnet` pattern; the task
  reads the card and never touches flash). In internal RAM it took the block Wi-Fi's
  transmit buffers come from: under the clock (23 KB free, largest 7.4 KB), once the task
  existed, any reply longer than about a segment stopped after its first send (status's
  1.6 KB, config's 4.5 KB, a framebuffer capture), while short ones still went. Before
  the move, the bench's map + PNG + layer stalled status by the second poll; after, 24
  polls over three minutes and six cycles of re-stack, re-render and frame swaps with
  status, config and a 652 KB capture each time all came through whole.
- **Tests.** `tools/host_live_test.py` lifts `over`, `depth_key` and `live_restack` into
  gcc: the canvas blend unchanged, a clear pixel taking the source whole, coverage adding
  up (half over half = 191), forty faint strokes building to opaque; the layer's place
  among a plate, rows at keys 3 and 5, a comp at 21, the glass and an app widget, up and
  down; 7/7 mutants (two equivalent edits left out, said why in the file).
  `host_png8_test.py` gains the palette read (a non-grey palette proves the first channel
  is read; no palette, 256 entries, a bad CRC, a ragged length, 257 entries): 17/17.
- **On Bronze** (`/api/v1/debug/live`, debug builds only: open at an order, draw the
  home map at a radius and a card PNG through a rain ramp, timed on the stage's frame;
  `veto=1` forces the classic path):
  - Weather (32 rows + the layer = 33, L4): open 18-25 ms (alloc, fill, table rebuild),
    the map's blend into it 60-65 ms, a 512x512 frame 47 ms with no echo in the window
    and 63-66 ms with Seattle's. At order 10 the map covers Weather's rows; at 0 they
    stand on it; the classic path stacks the same at both.
  - The clock (8 rows + 1): a commit's paint 25.0 ms without the layer, 29.6 ms with it.
  - A redraw of map and rain is ~125 ms on the LVGL task, and with one layer the loop
    pays it every step: the layer holds the two composited, so a new frame needs the map
    under it again. *Answered by §6.5: two layers, a rain step ~65 ms.*
- **Not built here:** the editor's row (R7: it writes the order into the values).
  Classic-path rounding is owed (13).

### 6.5 R3c, a second layer (2026-11-07, Bronze, OS v1.2.23, uncommitted)

The owner's call after §6.4: keep the map parked and redraw only the rain.

- **Two layers an app** (`STAGE_LIVE_N`). `orb_stage_live(layer, order, argb)` takes the
  layer first (the import was `i(ii)` in v1.2.22, never pushed and never used by an app;
  it is now `i(iii)`); the canvas handles are `ORB_CV_LIVE0` (-1) and `ORB_CV_LIVE1` (-2).
  Each layer has its own depth and plane. In the L4 table both are picked into the depth
  sort, layer 0 first, so at one depth layer 0 is under layer 1. On the classic path
  `live_restack` places both at once: it lifts them to the end of the page, finds each
  one's place among the stage's own widgets, and puts the lower in first. Re-stacking one
  at a time read the other's old place (the host test's same-depth case caught it).
- **A clear is cheap:** a fill with alpha 0 writes only the alpha plane (a draw over a
  clear pixel takes its own colour whole): 6 ms a step. (The 11 ms of a first open is
  the allocation and the table rebuild; a coloured fill was not timed.)
- **On Bronze, Weather on the glass:** map into layer 0 once (open 11 ms, blend 64 ms);
  each rain step on layer 1 is a clear (6 ms) and a frame (58-60 ms), **~65 ms against
  ~125 ms** with one layer, and the map is left alone. The frames swap cleanly with the old
  rain gone. Under the clock with pre-paint, a layer-1 redraw shows (the stale path holds
  for either layer); on the classic path, map at 40 over rain at 0 stacks as the depths
  say. Twelve rain steps under the clock, with status (1.6 KB) and config (4.5 KB) read
  after each, all whole (§6.4's Wi-Fi finding holds).
- **The memory:** the second layer is 651 KB more PSRAM. Under the clock with both layers,
  the map's level files and pre-paint's planes, 1.18 MB of PSRAM was left; closing both
  gave back 1.3 MB. The radar's own low point on the glass is R6's to measure.
- **Tests.** `host_live_test.py` adds the two-layer stacks: each order pair both ways, both
  at one depth (layer 0 under 1), both under the glass; 9/9 mutants (a third equivalent
  edit, the explicit stop at the lifted layers, left out and said why).

### 6.6 R4, the hub's map tool (2026-11-07, Bronze, hub only, uncommitted)

- **`maps@1`** (`tools/orb-manager/web/runtime/maps@1/orbmap.js`, a runtime kit so the
  radar editor can use it in R7): an MVT reader (`tools/mvt.py`'s port), the `.orbmap`
  builder and writer (`tools/orbmap.py`'s), and the role rasteriser
  (`orb_map_raster.c`'s, every float step rounded to 32 bits in the C's order), plus the
  level picker, the file name, a header reader and a palette painter for previews.
  DOM-free; the page and node run the same file.
- **One rounding rule** for both writers: half up, `floor(v + 0.5)`. `orbmap.py` used
  Python's `round`, which rounds halves to even and which JS cannot copy, and read tiles in
  file-name order; both now match the JS (by column, then row). R0's six DFW files from
  the old writer are byte for byte the new writer's, so the files on Bronze stand.
- **`tools/host_orbmap_js_test.py`** (run by `make check-editor`; skipped without node):
  seeded synthetic vector tiles from a small MVT encoder in the test, every layer the tool
  reads (holes, lines across the window, maritime borders, places outside it, long and
  non-ASCII names), at three places in three hemispheres. The JS writes `orbmap.py`'s bytes
  for all 18 levels; 52 renders (scales 0.71–1.41, rotated, widths 1–4, every dash) are the
  C's raster nibble for nibble; the level picker and names are the C's. 12/12 mutants
  (`Math.round` is no mutant: it also rounds halves up). Outside the suite, on R0's real
  DFW tiles the JS wrote the same six files byte for byte, and on Bronze the device's
  London raster at 15 km equalled the JS's on all 217,156 pixels.
- **The forwarder** (`maps.go`): `GET/POST /api/maps/source` keeps the TileJSON in the
  hub's state file (absent for OpenFreeMap's default); `GET /api/maps/fetch?u=` fetches
  only that TileJSON and the tile hosts its `tiles` templates name (learned when the
  TileJSON is read through it, forgotten when the source changes), as
  `gorb-os/<hub version> (+https://github.com/gregtakacs/gorb-os)`, four at a time, 8 MB at
  most, under the launch token. `maps_test.go`: the default and its persistence, refused
  sources, a tile host refused before its TileJSON is read and after the source changes,
  other hosts and schemes refused, the token required, twelve asks reaching the host at
  most four at a time.
- **The Places card**: a Maps table, one row a place (home and the set places, with KB on
  the card) and its state in a fixed column; out of date (a place without its six levels,
  or moved, since a moved place has new names) is an amber tag, an amber "Maps out of date
  — Make maps" press and a count on the Orb tab. Make maps fetches the tiles (this
  browser's Cache Storage first, then the hub), builds each level, writes it with
  `/api/v1/sd/write?overwrite=1` (the orb's own `.part`-and-rename, so a reader never sees
  half a file), removes the `.orbmap` files no place names, and posts
  `/api/v1/places/maps`; "Step n of m" as it goes and DONE at the end. A Preview draws the
  level the orb would use at a chosen radius, from the tiles, never reading the card. The
  Map tiles row sets the source. Saving a place re-reads the maps.
- **On Bronze, through a hub run from this tree:** Austin and London's twelve levels made
  in 25 s (159 tiles, 23 MB), the OS's maps gen moved, and both drawn by the OS with their
  landmarks (AUS, Waco, Killeen; LHR, LTN, City of London). Then the page itself, in
  headless Chrome over the DevTools protocol: with Austin's files removed and a stray file
  planted, Austin's row read "no maps yet" in amber with a badge of 1; Make maps made only
  Austin (six steps, 74 tiles, 9.5 s), removed the stray, and the rows went green and the
  badge went away; Preview drew London at 30 km (z10 at 0.74x, 305 landmarks).
- **Not built here, closed in §6.7:** an older FORMAT on the card (by a naming rule) and
  the credits (ABOUT and the glass, after §4 item 11).

### 6.7 The credits and the format rule (2026-11-07, Bronze, OS v1.2.24, uncommitted)

- **On the glass** (`main/app/orb_map_credit.c`): the first `orb_map_draw` after a boot by
  the app on the glass raises "Map data: OpenStreetMap" (Montserrat 14, white on a black
  pill, 200x24 at the foot of the glass, inside the circle and clear of the carousel
  chip's band) on LVGL's top layer for five seconds; switching apps takes it down. On
  Bronze over Weather's live map: up with the map, gone five seconds later with the map
  whole beneath, and not again on the next draw.
- **The keep-out became two slots** (`GFX_KO_CHIP`, `GFX_KO_CREDIT`), as its own comment
  said a second chrome would need, and its cut was fixed: a row crossing the rect skipped
  its whole segment (min and max swapped in the side runs), harmless while the chip
  parked the beat, not under a map being redrawn. `tools/host_keepout_test.py` lifts the
  slots and the cut: both sides of a rect painted, only its pixels kept, two chromes at
  once, a clear re-marking its rect and leaving the other standing; 6/6 mutants.
  `host_carousel_test.py` follows the slot argument.
- **ABOUT** gains five rows after its five facts: "maps  OpenStreetMap contributors",
  "openstreetmap.org/copyright", "tiles  OpenMapTiles, OpenFreeMap", "radar  LibreWXR (CC BY
  4.0)", "weather  Open-Meteo (CC BY 4.0)" (the Open-Meteo credit weather-plan owed). Read
  on Bronze through the screen door: every row fits.
- **An older FORMAT on the card:** closed by a rule rather than a reader. A file's name
  is its FORMAT's (`orb_map.h`, `orbmap.py`, `maps@1` say so): a later FORMAT names its
  files apart, so the OS never opens an older one, and the Places card's sweep of names no
  place uses removes them on the next Make maps.

### 6.8 R5, the radar app (2026-11-07, Bronze, radar v1.0.0 on OS v1.2.25, uncommitted)

- **`apps/radar`** (24.8 KB of wasm, 46 imports, on the hub's shelf; `os_min` v1.2.24).
  `radar.c` is the model, free of imports: LibreWXR's frame list read token by token, the
  frame and list URLs, the image's draw window, the zoom ladder, the words, and
  `rd_values_t` (layout 1, 1,128 B: the map's palette, widths, dashes, roles, marks and
  names shown, the two layers' depths, the rain as 256 ARGB words) with Scope's defaults.
  `main.c` is the app:
  - **places:** home and the OS's three, a press for the next, the step kept;
  - **frames:** the list every 10 minutes; the newest past frame first, then the nowcast
    (a held one asked again with `If-None-Match`; Bronze answered 304 for the five it held
    and 200 for the new one), then
    the older past ones newest first; one request at a time; a frame that turns from
    nowcast to past is fetched afresh; failures back off 1, 2, 5, 10 minutes, a 404 (the
    server no longer keeps it) does not; past 30 minutes the age is shown;
  - **the glass:** the map in live layer 0 at the theme's depth, opened per place and zoom
    at the step's exact radius and drawn once it is ready; the rain in layer 1, cleared
    and drawn through the theme's ramp at the window `rd_window` gives; the dot, the time
    cursor (1.5 degrees a minute from Now), the place, the time ("Now", "-40 min", the
    nowcast's in the theme's forecast colour), the range in the orb's unit, a status line
    ("No map yet", "Loading radar", the zoom's name for 2 s, the age) and up to six
    landmark names kept off the dot, off each other and off the labels' bands;
  - **the knob:** the dial is time, centred on Now; untouched, the past loops at four a
    second and holds Now 2 s; a turn stops it; the idle return starts it again;
    press-and-turn zooms a step a click (Wide, Region, Local; the ends stop).
  The stage is read afresh at every arrival on the glass: at a boot the theme was not yet
  bound while the app built, and the first bench run drew only the plate and glass.
- **The image against the map:** the server centres a point image on the URL's four
  decimals with `round(px - 256)` (Python's round, its y clamped to the world); the app
  draws the window from `round(place_px - origin - 233)`, so the rain sits within half a
  pixel of the map. The guest's libm has no log, so the model carries its own for the
  Mercator y (`rd_ln`, libm's to 1e-14).
- **The theme** (`apps/radar/editor/radar-bake.js`, in the app's editor region, which R5
  ships because `os_min` rides it; its page says the editor is R7's): the plate, the glass
  (two range rings, a dotted crosshair, the time track's 18 ticks, the nowcast's in the
  forecast colour), the dot (white with an amber ring: the airports are amber), the cursor,
  three fonts, ten labels, the values and the preview. The shelf's starter is
  **Radarscope** (`radarscope.orbtheme`, 1.13 MB; a shelf file is its theme's name, and
  Weather's is Scope), checked by `apps/radar/editor/_selftest/bake_check.py` in Chrome
  and re-baked by `tools/rebake_shelf.py radar`.
- **Tests.** `tools/host_radar_test.py` compiles `radar.c` beside the OS's tokenizer: the
  list (whole, torn, out of order, gaps, a path too long), the URLs, `rd_ln`, the window
  against the server's formula at 10 places x 3 zooms including both poles' clamps,
  Python's round, the ladder against `pick_level`, the words, and the C defaults against
  the baker's Scope values byte for byte (with node); 11/11 mutants (one equivalent edit
  left out, said why).
- **On Bronze, DFW with a storm to the south-west and Austin under it:** the map, the rain
  over it with the roads through it, the names (Wichita Falls, Sherman, Dallas, Tyler,
  Waco), the loop playing (-60, then -10), a turn stopping it at -30 min, a press to
  Austin with its own rain, press-and-turn to Local (z7 frames, 30–37 KB) and two clicks
  out to Wide (z5, 25 KB). A Region step costs 85–90 ms on the LVGL task. The OS's
  "Map data: OpenStreetMap" showed with the first map after each boot.
- **Found and fixed on the way:** Austin's Region frames (51–69 KB) filled the 768 KB of
  kept bodies, and the app read it as a network failure and backed off; it now stops
  fetching older frames instead. The bench door gained `push_turn=N` (a press-and-turn,
  debug builds, OS v1.2.25) to drive the zoom without a hand.
- **For R6:** the internal RAM under the radar with both layers and the map (Bronze showed
  33 KB free, the largest block 13 KB, and the display's tile ring re-funded short once);
  an hour on the glass; a card pulled mid-read.

### 6.9 After the owner's hand test (2026-11-07, Bronze, OS v1.2.26, radar v1.1.0, uncommitted)

The owner's verdict on R5: the time dial and the press for the next place feel right; the
press-and-turn felt wrong ("one zoom then it reverts to time scrub"), zooming was slow,
three steps were not close enough, and the roads far too dense at DFW and London.

- **The press-and-turn** (`main/knob/knob.c`): the switch ISR took a release at its first
  edge, and a push switch held under a turning shaft opens for a moment; the gesture ended
  after one click, the re-close fell inside the press debounce and was dropped, and every
  later click was an ordinary turn. A release now counts once the switch has stayed open
  60 ms (`kg_up`, `kg_down`, `kg_settle`, in the gesture's pure half); a close inside the
  settle is the same press; a hold is not fired while a release settles. `host_knob_test.py`
  adds the cases; 14/14 gesture mutants. Not provable from the bench door: the owner's hand
  is the test.
- **The zoom lands once a turn rests** (350 ms after the last click), however many clicks
  it took; the status line and the range follow every click at once. The old map and rain
  stay up until the new view is ready instead of going blank.
- **Six steps**: Wide, Region, Local, Area, City, Close (479, 239, 120, 60, 30, 15 km at
  DFW), the map's z6 to z11. **Frames stop at Local's z7** (~0.6 km a pixel, finer than the
  ~1 km radar): Area, City and Close draw the same frames magnified 2, 4 and 8 times, so
  zooming among the four fetches nothing, only the map view changes. LibreWXR's own
  smoothing (`1_0` in the URL) was tried first: it is bilinear plus a blur and looks right,
  but comes back as RGBA (colour type 6) at ~100 KB a frame, which the reader refuses and
  the body budget could hold seven of.
- **A smoothed, magnified draw** (`orb_cv_png8` with `scale | ORB_PNG_SMOOTH`, OS v1.2.26):
  each glass pixel samples the source at its own centre and blends the four values around
  it bilinearly before the palette colours them (values, not colours: green-to-red never
  passes through orange that was not there), exact in 1/(2k) of a source pixel, row by row
  as the decoder streams with one row kept. `rd_window_k` places the window so the place
  lands on the glass's centre within half a glass pixel (0.496 at worst over 30 cases).
  `host_live_test.py` holds it to a double-precision bilinear reference at 1, 2, 4 and 8
  times, across the source's right edge and with negative positions; 15/15 mutants. A step
  at 4 or 8 times costs 100–130 ms on Bronze; the source's 1 km cells still show as soft
  steps at City and Close, which no interpolation removes.
- **Roads: map FORMAT 2.** Major road is motorway and trunk; primary moved to role 5;
  secondary and tertiary are roles 13 and 14; lines draw minor under major (a table, not the
  role number). Each road class is written only from the level it can first be shown at
  (OpenMapTiles itself leaves secondary and tertiary out of the wide tiles, so the files
  barely shrink: what was dense at Region was trunks and primaries). The density is the
  view's: the radar's values (layout 2) carry the first zoom step each role is drawn at,
  and Scope draws motorways and trunks everywhere, primary from Local, secondary from Area,
  tertiary from City. The files are named `-f2` (§2.4's rule), so the Places card showed
  every place amber until Make maps; on Bronze it made all three (18 levels, 3.9 MB) and
  swept the 18 FORMAT 1 files. JS and Python still write the same bytes (R0's DFW tiles and
  the seeded suite, 14/14 mutants).
- **Not yet:** the step cost at the close zooms (half the display task at four steps a
  second) is R6's to measure over time; the editor's road presets are R7's.

### 6.10 The second hand test (2026-11-07, Bronze, OS v1.2.26, radar v1.1.0, uncommitted)

The owner: zooming was fine for a while, then very slow; City (30 km) at DFW said "No
map yet"; "Radar draws on its theme: install one from the hub" stood in the middle; the
zoom's word was lost in the map; still too many roads.

- **One cause behind the first three:** PSRAM. Bronze showed 613 KB free. Beside the
  radar's two live layers, kept frames and cached levels, a map would not open (read as
  "No map yet") and the theme would not bind, so the radar made its fallback widget, and a
  widget on the page takes the dial off its fast path for good (the slowness). Fixed:
  - **pre-paint gives its four planes (1.7 MB) back** when it turns off
    (`gfx_prep_mode(0)`, `orb_gfx.c`); they stayed for the rest of the boot once any plan
    had played;
  - **the radar makes no widget:** without a bound theme it asks again every 2 s;
  - **a map that fails for memory or a read is tried again in 3 s**, the old view kept up;
    only a map missing from the card reads "No map yet".
  On Bronze afterwards: 1.04 MB free at City, the same after two rounds of Wide to City, on
  the fast path (`l4` true), a step 100–120 ms. The 613 KB was not reproduced (the clock
  played no plan this time), so pre-paint is the likely cause, not a proven one.
- **The zoom, large:** label 18, a fourth, 34 px font on a dark rounded backing, centred
  below the dot, "City  30 km", shown while the zoom changes and 2 s after. The status line
  has a backing too and says "Loading map" and "Loading radar" while they come.
- **Roads, further:** motorways and trunks to Local, primary from Area, secondary from
  City, tertiary at Close alone.

