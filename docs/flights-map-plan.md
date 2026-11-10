# A map under the flights scope (and the weather's, later), and a Sectional look

The owner, 2026-11-08: "a map as a background to flight tracker. We don't want a real full
fledged map, just main roads and airports and water bodies — not creeks, just rivers and lakes
and bigger. A hint, not the show. The theme editor should allow changing the intensity, and
the drafting one should draw it like it was on draft paper with dashed lines. Some may want a
diamond for the airports, some the actual strips. City names yes/no, city dots yes/no. Helps
people who know an area orient themselves without being overbearing on the signal. Maybe the
same for the weather app too, optional. And a new theme for all three apps, reminiscent of a
flight map's magentas, blues and yellows, with a matching clock."

## 0. Rulings

1. The map is the OS's map of home (radar-plan §2.4): the same `.orbmap` levels Orb Manager
   makes when home is set, drawn by the OS into a live layer. Flights ships no map data.
2. A hint: one faint layer over the scope's ground and under the arm, the aircraft and the
   words, cut at the outer range ring so the bezel stays the theme's.
3. What is drawn is the theme's (its editor's Map step), never an app setting.
4. Off in every design saved before the map; on in the new starts.
5. An airport is never drawn as nothing (owner, 2026-11-10). Where a theme draws runways
   (Airports: "Runways at the nearest range"), an airport whose runways the map data does not
   carry gets the airport symbol the theme chose instead. The OS decides per airport at draw
   time: no RUNWAY pixel near the mark means no runways (`ORB_MAP_MARK_BARE`, OS v1.6.9).

## 1. What the OS adds (v1.2.38)

The view (`orb_map_view_t`, still 44 B) grows meaning in bytes that were zero, so an older app
draws exactly as before:

| byte | was | now |
|---|---|---|
| `clip` (was `pad`) | 0 | half the radius, px, past which the view is the ground |
| `width[CITY]`, `width[AIRPORT]` | 0 (ignored) | the mark's look, `ORB_MAP_LOOK(shape, r)`: dot, ring, diamond, diamond outline, square, square outline, ring with four ticks; 0 the old 5 px dot and square |
| `dash[CITY]`, `dash[AIRPORT]` | 0 | the rank cap: marks ranked past it not drawn (an IATA airport ranks 1) |
| `dash[WATER/URBAN/LAKE]` | 0 | the floor: a piece under 2× that many px both ways not drawn (ponds) |
| a line role's `width`, bits 3-7 | 0 | the line floor in 8 px steps: a piece under it both ways not drawn (the creeks the tiles call rivers); the width is bits 0-2 |

The renderer holds a floored line's points in the scratch until its box is known; a line
longer than the scratch is drawn whole. `maps@1`'s JS renderer is held to the C's nibble for
nibble (`tools/host_orbmap_js_test.py`, 22 mutants); the C suite has 26.

`maps@1` also carries what an editor's preview needs: `GorbMaps.orbHome(ip)` (home and the
orb's distance unit) and `GorbMaps.fetchLevel(lat, lon, z)` (one level built from the Map
tiles source through the hub, the Orb tab's tile cache shared). The radar's editor moved onto
them.

## 2. Flights v1.5.0 (os_min v1.2.38)

### 2.1 What each range draws

The scope's four ranges (km 20/40/80/140, mi 10/25/50/90, nm 10/20/40/80) draw from the level
nearest their radius (at DFW: z11, z9, z8, z7). Measured on the owner's home map, z9's window
carries every small river and z11's every residential block, so:

- **water:** lakes and the sea filled, a lake under 12 px left out (Big lakes only: 32 px;
  Every pond: none); the coast and the rivers drawn, a river piece under 48 px left out;
- **roads:** motorways at every range, the main roads at the nearest only;
- **airports:** the runways at the nearest range and a symbol wider out (or a symbol at every
  range, or none); the airline airports only (rank 1) or every airfield;
- **towns:** a dot and a name (cities, or cities and towns), built-up areas a tint (a piece
  under 20 px left out), state and country lines.

### 2.2 The app

`main.c` reads the theme's values (layout 1, `fl_values_t`, 120 B): the 16 colours, widths
and dashes as the view takes them, each range's roles, marks and name count, the names'
kinds, the layer's depth, the clip (92: the outer ring) and the names' size. One view a
range, rendered on the map task (~520-590 ms on Bronze) and drawn once into live layer 0; a
range press clears the layer until its view is ready, so the map never stands at the wrong
scale under the aircraft. The names are six label rows at slots 32-37 (font 4), placed as
the radar places its own: under their mark, off home's dot, off the range figure and off each
other, inside the rings.

The theme is read again at the first frame after every arrival (the OS binds the stage after
`orb_activate` and after `orb_show`; a theme sent from the editor binds while the app is away).
Never through `orb_stage_open` from inside a frame: it binds the stage again, and the stage
stopped (Bronze, 2026-11-08: the glass held its plate).

### 2.3 The editor's Map step

Map on or off; a **Look** (Hint: the family's ink, thin and quiet; Chart: blue water, yellow
towns, magenta airports; Drafting: one pen, dashed roads, dotted rivers, outlined marks), then
**Strength**, one slider over every opacity (0.5 is the look's own). What is drawn (water,
lakes, roads, airports and their symbol, which airfields, town dots, names, towns, airport
codes, built-up areas, borders), each colour and opacity, each line's style and weight, the
town dot, the names' face and size. The preview is the owner's own home map at the range the
toolbar picks, drawn by maps@1 exactly as the orb draws it. The fit counts the six names
against the 64 rows an app seats.

Families start: Modern, Silver, Brass and Celestial on Hint in their own ink, Blueprint on
Drafting, Paper on Chart; Blank with no map.

### 2.4 As built (2026-11-08, Bronze, OS v1.2.38, flights v1.5.0)

- On the glass at 50 mi over DFW: motorways, lakes, Denton, Plano, Fort Worth, Dallas; the
  view 592 ms at z8 1.17×; 19-20 fps with the traffic (the scope's own 20); internal
  31 KB free (largest 11.8 KB), PSRAM 1.18 MB, the glass heap 2.38 MB free, no fallbacks.
- The names first had a font of their own (+165 KB with its accents), which pushed the
  shelf's four defaults past the art pool's ~8 MB (the radar's budget check failed). They now
  default to the words' face and size, whose glyphs the bake shares: the map costs Modern
  +94 KB (the six name rows' panels), 1.09 MB in all; another face or size says what it costs.
- `apps/flights/editor/_selftest/bake_check.py`: 35 themes, five of them the map's variants
  (off, Chart with symbols, Drafting with towns and codes, nothing drawn, saved before).

## 3. Owed

- Gold has not run a map under the scope or the reading.
- The shelf's art pool is nearly full (Skeleton 3.48 MB, Modern 1.09, Scope 1.31,
  Radarscope 1.91): a fifth default, or a heavier one, needs room made first.

## 4. Sectional (2026-11-08)

A flight chart's look, made up (no chart data): chart paper with soft terrain tints, a
graticule ticked in minutes, a dashed magenta isogonic line, the magenta band a chart shades
inside an airspace's edge (here inside the rim), a blue rose, blue rings, magenta for
airports and the arm, Barlow Condensed in chart navy.

- **Flights** (start `sectional`): its own ground (`sectional`), the Modern silhouettes in
  chart inks by altitude (magenta under 3,000 ft, orange, green, blue, navy over 30,000), the
  Chart map on (blue water, yellow towns, magenta ticked rings at the airports). On Bronze over
  DFW: 16-18 fps with 32 aircraft, internal 31 KB, PSRAM 0.99 MB, the glass heap untouched.
- **Radar** (starter `sectional`): the same map colours on chart paper, airports as ticked
  rings (`map.look`, the view's v1.2.38 mark bytes; an older OS draws the old square), the rain
  ramp kept off yellow and blue (green, orange, red, magenta, purple).
- **Weather** (preset `sectional`): the chart plate (graticule, isogonic line, the faded
  magenta band, a blue rose), the Station pictograms in navy and magenta. Weather v1.5.2.
- **Clock** (template `sectional`): a chart-paper dial, a blue railway track and figures,
  a magenta and a blue airport ring, a magenta reporting point, SECTIONAL and CLASS E 700
  printed, royal-blue pencil hands, a magenta seconds hand. Clock v1.2.30. Estimated 4 fps
  still, the same as Phase, which shares its hands.

## 5. The weather's map (2026-11-08, Bronze, weather v1.5.2)

The owner's "maybe the same for the weather too, so the background is not so sterile; again
optional". The shown place's own map (home, or the place a press steps to: each has its own
levels on the card), faint, behind the reading, cut inside the bezel's ticks (r 192).

- `main.c` reads the theme's values (layout 1, `wx_values_t`, 108 B): the colours, widths and
  dashes, what is drawn, the clip, the layer's depth (over the ground, under every row) and the
  reach (20, 40, 80 or 150 km to the glass's edge). One view a place; a press clears the layer
  until the next place's view is ready. The values are read again at the first frame after an
  arrival, as flights reads its theme.
- The editor's Map step, between Ground and Icons: on or off, the three looks and Strength, the
  reach, water, roads (motorways, or main roads too), airports (off at the start: round a city
  even the airline fields are a handful of marks), town dots, built-up areas, borders, colours,
  lines. `wx-map.js` is the weather's own copy of the tracker's module, cut down (no names, no
  ranges): apps share no code.
- Every preset starts on (Scope, Brass, Celestial and Station on Hint, Blueprint on Drafting,
  Paper and Sectional on Chart); Blank and every design saved before it, off. The values cost
  a theme ~100 B.
- On Bronze over DFW at 40 km: the view 1,323 ms at z10 0.75×; 20 fps; the shelf's Scope
  re-baked with it.

## 6. One look set (2026-11-08)

The owner: parity of look and feel across weather, radar and flights. Every app now starts
from the same eight looks, under the same names, in the same order: **Scope, Paper, Brass,
Blueprint, Station, Celestial, Silver, Sectional** (radar-plan §6.26).

- **Flights** gains **Station** (black silhouettes on white, a red arm, Jost); "Modern" is
  **Scope** and "Vintage brass" **Brass**; **Silver** wears graphite under a silver bezel.
  Its picture draws the map (maps@1's made-up place) under the planes.
- **Weather** gains **Silver** (graphite, a turned bezel, the engraved icons in silver);
  "Vintage brass" is **Brass**. Its picture draws the map behind the reading.
- **Radar** gains **Celestial** and **Silver**; "Radarscope" is **Scope**, "Vintage" **Brass**.
- The shelf files are the app and the look: `flights-scope.orbtheme`, `weather-scope.orbtheme`,
  `radar-scope.orbtheme` (three apps have a Scope and the shelf is one directory; the hub's
  library test holds a file to its own name, the app first allowed).

## 7. Range and scale (owner, 2026-11-10)

1. **The range steps are radar's zoom ladder, its four closest**: Close, City, Area, Local,
   each twice as far as the one before (the glass's edge at map zoom z11 to z8, radar.c
   `rd_radius_m`), so the map draws at its own scale and matches the radar at the same name.
   Built in flights v1.6.3 (`main.c` `RANGE_NAME`, `range_km_at`).
2. **A press-and-turn chooses the range as radar's zoom does**: the choice is shown large on
   the theme's pill (slot 38) while it changes and lands on the let-go; the `range` setting is
   where the app opens, and a turn never writes it (flights v1.6.3).
3. **Every distance the glass shows is a round number**, as on the radar: the scale's round
   length (radar.c `rd_scale`: 1, 2, 5, 10, 20, 50, 100 … in the orb's unit, the longest that
   fits), never the odd radius of the outer ring. The pill says the step's name and that round
   length ("Area  20 mi").
4. **The scale is the theme's, as on the radar** (radar-plan's `layout.scale`): **Ring**, a
   ring drawn at the round length and labelled on it, up and to the right; or **Bar**, a bar at
   the foot with its length under it. With its look as the radar's: bar ink, edge, thickness,
   end ticks, longest bar, and the label's face, size and colour. The scope's own baked rings
   stay as the plate's decoration and carry no figure.
5. **The pill wears the theme, not grey**: its words in the theme's accent tone (the scope's
   green), on a backing in the card's frame look (its fill and line). (Claude's call on the
   owner's "grey looking even though everything is green"; the owner may change it.)
