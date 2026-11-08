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
- A theme with the map is 165 KB larger (the names' font with its accents): Modern 1.16 MB.
- `apps/flights/editor/_selftest/bake_check.py`: 35 themes, five of them the map's variants
  (off, Chart with symbols, Drafting with towns and codes, nothing drawn, saved before).

## 3. Owed

- The weather's map (§0's "maybe"): the shown place's own map, faint, under the reading.
- The Sectional look for flights, radar, weather and the clock.
- Gold has not run a map under the scope.
