# Radar: rain over a map, for home and the three places

Status: **designed 2026-11-07 (owner, §0). Not started: nothing here is built.** Four
pieces of work:

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
9. **The map credit goes on gorb-os's ABOUT page**, not on the map.
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
    palette (New York z6). The mapping to dBZ is not documented (§4).
  - **Point images stop at z7.** At DFW, z8 answered an empty 1,096 B RGBA image for both
    the radar and the coverage, though DFW is inside radar cover. London z9 and z11 gave the
    same 1,096 B.
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
  frame is keyed by its `time`, the place and the zoom. Normally that is one new past frame
  and the six nowcast frames, which are re-issued each cycle (§4: whether a nowcast frame's
  path changes when it is re-issued).
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
  showing it as dry.
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
    int32_t  scale;             /* 1, or 2 to double each source pixel (the radar's Local step) */
    const uint32_t *pal;        /* 256 ARGB entries in app memory; alpha 0 = leave the canvas */
} orb_png_draw_t;

int32_t orb_cv_png8(int32_t cv, const orb_png_draw_t *d);
```

- It reads colour type 3 at bit depths 1, 2, 4 and 8, non-interlaced, and refuses
  anything else with its reason. Rows are inflated through ROM `tinfl` into PSRAM
  (`tinfl_decompressor` and the 32 KB window are PSRAM, the two row buffers are 512 B
  each). The draw is native and row by row, with no whole-image buffer.
- The app's palette, not the PNG's, colours it. Alpha blends over the canvas (0 leaves it
  as it was), so the radar draws over the map, and the map over a plain fill.
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
| 5 | minor road | line | | 12 | runway | line |
| 6 | major road | line | | | | |

Indices 13–15 are spare. A new role appends; a role's number never changes. Land is the
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
  Lines are drawn at their role's width, and marks last. *Estimate* 100–300 ms for the
  densest level (§4); R0 measures it before anything else is built. Opening a new view (a
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
  at 2.5 B a vertex (§1.8). *Estimate* 400–600 KB after it. Either is nothing on a 29 GB
  card (owner, 2026-11-07).
- **Attribution** (§0 ruling 9): "© OpenStreetMap contributors · OpenMapTiles ·
  OpenFreeMap" on gorb-os's ABOUT page, beside the other credits, never on the map. The
  Places card names the source in its Map tiles row.

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

**What it keeps** in its 64 KB: the frame list (18 × time, path and blob slot, about
1 KB), the palettes (1 KB for rain, 64 B for the map), the place on the glass and its zoom.
The frames are blobs (§2.3); the map is the OS's (§2.4).

**Zoom:** three steps at the radar frames' own scales, z6, z7 and z8: 479, 239 and 120 km
from the centre to the edge at DFW, and 366, 183 and 92 km at 50°N. The app opens a map
view at exactly that radius (`orb_map_open`), so the map and the frame line up pixel for
pixel. **Local draws the z7 radar frame doubled** (`scale = 2`), since point images stop
at z7 (§1.4) and the radar's data is about 1 km a pixel anyway. The step is changed by
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

**On the glass**, drawn into one 466×466 canvas, back to front:

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
once a step.

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

1. **Start:** the starter themes as tiles, plus Blank, or one from the hub library.
   Proposed starters, matched to looks the orb already has: **Scope** (phosphor green on
   black, flights' Scope), **Paper** (a printed road atlas), **Station** (the TV weather
   map: slate land, white borders, the broadcast rain ramp). §1.5's render is a first
   sketch of all three.
2. **Ground:** the plate (colour, gradient or a picture), the rim, and the dim applied
   where there is no radar coverage.
3. **Map:** for each of the twelve roles, a colour, an opacity and an off switch, and for
   the line roles a width (1–4 px). Previewed on the owner's own home map at all three zoom
   steps: the editor reads the `.orbmap` files from the card and draws them with the hub's
   JS rasteriser, which matches the orb's pixels (§2.4). Also which landmark kinds show
   names, and from which rank down.
4. **Rain:** the ramp's stops over the raw values, with the same lightness check as
   Weather's Temperature step (stops must differ in lightness, not hue alone, so the ramp
   still reads to colour-blind eyes and on a dim glass). A second ramp for snow if
   LibreWXR's snow flag is used. The **forecast treatment**: dimmed, desaturated, or
   outlined only, so the nowcast never looks observed.
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

**The open question it raises:** the radar draws on a canvas, while theme text and art go
through the stage (`orb_stage_*`; theme-text-plan's rule that app words are theme labels).
How a canvas app's theme supplies fonts, a plate and the rim's track, and whether the
canvas sits as one stage row under theme rows, is owed before R7 (§4).

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

## 3. Slices

| slice | what | depends on |
|---|---|---|
| R0 | §4's checks that change the design. **First, the vector render's time:** a host rasteriser in C over DFW's densest level, then the same code in a test build on Bronze, rendering from the card. Then the point image's centre and scale against a map, the raw value → dBZ mapping, nowcast path stability, and canvas plus theme | — |
| R1b | press-and-turn in the OS (§2.10): the knob layer as benched, the `orb_push` export, the router and the SDK, the host cases | — |
| R1 | places in the OS: `orb_places.c`, NVS, the imports, `binds --write`, the routes, the hub's allowlist and fake orb, the Places card on the Orb tab; a host test with mutants | — |
| R2 | Weather v1.4.0 reads the OS places; the place rows leave its App settings page | R1 |
| R3 | the OS surface: kept bodies and `orb_cv_png8`; the maps: the `.orbmap` FORMAT 1 reader, the rasteriser, views on the worker, the map imports, `/System/Maps`, the gen route; host tests with mutants for the PNG reader, the `.orbmap` reader and the rasteriser; the render and draw times measured on Bronze | R0, R1 |
| R4 | the hub's map tool: the Map tiles row, the Go forwarder with its User-Agent, the tile fetch and cache, the MVT decoder, the class-to-role table, clipping, simplifying and the `.orbmap` writer for six levels with their landmarks, the JS rasteriser and the Places card preview, the C/JS pixel parity test, the writes and clean-up, the amber "Maps out of date" state | R0, R1 |
| R5 | `apps/radar`: the frame list, fetch order, refresh rules, the knob, the glass, the no-map and no-coverage states, one baked starter on the shelf | R3, R4 |
| R6 | bench on Bronze: a stormy day and a quiet one, PSRAM and internal low points from `/status` across a cold start and an hour on the glass, the step time at 4 fps, a card pulled mid-read | R5 |
| R7 | `apps/radar/editor`: §2.9's steps and the starters | R5, R0's canvas-plus-theme answer |

## 4. Owed

1. **Alignment.** Is the 512 px point image centred on the given lat/lon to the pixel, and
   drawn at zoom z's scale? Check by drawing R4's map under a frame with rain on a coast
   or a lake shore. §1.5's render assumed both.
2. **The raw values.** The mapping from the raw grey values to dBZ, read from LibreWXR's
   source. The rain palette's stops depend on it. Also whether `smooth` and `snow` change
   the values.
3. **Nowcast paths**: whether a re-issued nowcast frame keeps its path, which decides how
   much a refresh fetches.
4. **Frame sizes on a stormy day** at each zoom, to fix the blob budget. London's z5 frame
   at 31.9 KB suggests wide views of a big system cost several times a quiet z7.
5. **The times on Bronze:** the vector render of a view at the densest level (§2.4,
   *estimate* 100–300 ms, R0 measures it first); `orb_cv_png8` and `orb_map_draw`, and
   whether a synchronous draw shows in the UI (§2.3). If the render is far slower, the
   fallback is §1.7's hub-drawn pictures behind the same imports.
6. **Internal RAM** during the cold start's 20 HTTPS requests in a row: does the OS client
   keep the connection alive, and what is the internal low point? Weather makes one request
   per 30 minutes; this makes twenty in a few seconds.
7. **The map tool's cost:** bytes fetched and time to encode six levels for one place, at
   DFW and at a sparse place. The simplification tolerance (a quarter pixel, *estimate*)
   against how lines look, and the `.orbmap` sizes it gives.
8. **Canvas plus theme** (§2.9): how a canvas app takes its theme's fonts, plate and rim.
9. **The OSMF tile policy's current text**, to confirm §1.3's reading before anything is
   said about it in a public doc or issue.
10. ~~OpenFreeMap's User-Agent clause~~ **Settled by §0 ruling 10.** Their terms (read
    2026-11-07) ask an offline download for "a unique User-Agent identifying your app".
    Chrome does not let a page set one, and the hub page's Referer is a LAN address, so
    the Go side forwards with its own.
11. **OpenStreetMap's attribution guidelines** against §0 ruling 9 (credit on ABOUT, not on
    the map): confirm before R4 ships in a public release.
12. ~~The priority shrink's weight~~ **Gone with §0 ruling 11:** vectors are drawn at the
    view's scale, so nothing is shrunk, and line weight is the theme's choice per role.

## 5. Open

- **Flights' map** is a later plan (§0 ruling 6). It will have to answer how a map gets
  under stage rows: a stage plane the OS fills from `orb_map_draw`, or a canvas row under
  the theme.
- **Running LibreWXR on the NAS**: the owner is ready to (§0 ruling 8). The `source`
  setting exists from R5.
- **Lightning** (`/v2/lightning`, GeoJSON by point and radius) as marks on the radar, after
  R6. JSON through the OS tokenizer, no new surface.
- **More than three places**: the OS record has room, but Weather's 32 rows and one-request
  URL (weather-plan §6.1: 389 B of 480) were sized for four.
