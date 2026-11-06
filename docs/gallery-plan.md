# The gallery: photos from the card, cropped for the round glass

Status: **designed 2026-11-06 (owner, §0). Not started: nothing here is built.** A new
app, `apps/gallery`, and the OS surface it needs. The firmware work comes first (§3,
G1–G3); the app and its editor ride on it. Every figure marked *estimate* is owed a
measurement on Bronze (§4) before the slice that depends on it starts.

## 0. Owner rulings, 2026-11-05/06

1. **No photo is ever theme content.** The 8 MB `assets` pool is shared by every
   theme-aware app (theme-pool-plan §0 ruling 1); a gallery baked as a theme's picked
   frames would crowd out the clock's and the tracker's themes. Photos live on the card
   and the OS draws them; the app never holds a pixel.
2. **Photos arrive over USB, as they are.** The owner copies files onto the card in MSC
   mode; the orb converts each one into its own display format and saves that. The hub
   is not the way in.
3. **Originals are kept.** The orb never rewrites or deletes a file the owner copied on.
   Its display copies are a cache beside them, rebuildable at any time.
4. **Photos survive an uninstall.** Uninstalling the gallery wipes its settings as every
   uninstall does (app-platform-plan §43), but only **offers** to delete the photos and
   the cache, defaulting to keep. A reinstall finds them.
5. **No captions, for now.** Just photos.
6. **Any photo can be re-cropped.** Zoom, scroll and slide the rendered area and save a
   new crop, as often as needed.
7. **The card is a runtime read path for the gallery** (2026-11-06). This retires
   `orb_usb_msc.h`'s "the card is an install source, never a runtime read path" for
   this app: the gallery reads the card while it is on the glass, and the OS handles a
   missing card and the USB window as ordinary states (§2.3).
8. **The card-listing cap goes up** (2026-11-06). `orb_file_src_t.max_entries` widens
   from `uint8_t` to `uint16_t`, so one source can list more than 255 files (§2.2).

## 1. What is there today (read 2026-11-06, OS v1.2.8)

- **No image path for an app.** `apps/sdk/binds.json` binds no image import and no file
  read; the canvas (`cv_*`) is primitives and `cv_pixel`, far too slow for 217 k pixels
  through the interpreter. An app's linear memory is 64 or 128 KB; one 466×466 RGB565
  frame is 434 KB.
- **`sd_read` is declared and unbound.** app-platform-plan §3 lists it ("read inside its
  own `/sd/apps/<id>/` only"); §43 moved the Themes page into the OS and the clock dropped
  the cap, and nothing binds it.
- **A JPEG decoder in ROM.** `esp_rom/esp32s3/include/esp32s3/rom/tjpgd.h` (IDF v5.4):
  baseline only, no progressive, output scaled by 1/1, 1/2, 1/4 or 1/8, a few KB of
  work area. The ROM build's `JD_FORMAT` is 0, RGB888; the output callback packs to
  RGB565. No encoder anywhere.
- **Card files are the OS's** (`main/sd/orb_files.h`). An app declares a source (one
  directory, one extension, a name prefix); the OS mounts, locks, walks and deletes, and
  the app never calls `open()`. `max_entries` is a `uint8_t`: one walk returns at most
  255 files (raised by §0 ruling 8).
- **The card is an install source, never a runtime read path**
  (`main/usb/orb_usb_msc.h`; retired for the gallery by §0 ruling 7). In the USB window `/sd` is unmounted and the host owns
  every sector; what it writes is seen only after the reboot that ends the window.
- **The card bus** is SDSPI at `CONFIG_ORB_SD_SPI_MAX_KHZ=20000`, falling back to 10 and
  4 MHz (`orb_sd.c`). *Estimate:* 1.5–2 MB/s sustained.
- **Input is the knob alone:** `orb_input(delta, pressed)` while armed, a press arms, a
  hold disarms. Bronze has no touch layer.
- **Octal PSRAM**, malloc-backed (`CONFIG_SPIRAM_USE_MALLOC`). What is free with the
  clock's and the tracker's planes up is not yet measured (§4).

## 2. Design

### 2.1 On the card

```
/sd/Photos/                     the owner's: copied on over USB, never written by the orb
/sd/apps/gallery/
    cache/<key>.565             one display copy per original, 466×466 RGB565, 434 KB
    cache/<key>.th              one thumbnail, 96×96 RGB565, 18 KB
    crops.json                  the crops the owner saved (§2.5)
    index.bin                   the photo list the worker keeps (§2.2)
```

`Photos/` sits at the card's root because it is the folder the owner finds on a
computer; `/sd/apps/gallery/` follows the per-app rule. `<key>` is derived from the
original's name and size, so a replaced file of the same name gets a new key and its
stale copy is pruned.

**Why raw RGB565 and not JPEG.** The orb has no encoder, and raw is also the fast format
to show: a cached photo is one 434 KB read (*estimate:* ~0.25 s at 20 MHz) and no decode.
A 32 GB card holds tens of thousands.

### 2.2 The worker: converting what was copied on

An OS task, low priority, never the UI task (an entry point that loops is a frozen dial,
`orb.h`). After every mount it walks `Photos/` against `index.bin` and converts what is
new, in this order per file:

1. **Read** the original into a PSRAM buffer under the card lock, then **release the lock
   before decoding**, so a slow decode never holds the card (`orb_files.h`'s threading
   rule).
2. **EXIF orientation:** parse APP1's orientation tag. tjpgd ignores it, and half of all
   phone portraits would show sideways.
3. **Decode** at the smallest tjpgd scale whose cropped region still spans ≥ 466 px. An
   auto crop of a 4000×3000 photo decodes at 1/4.
4. **Crop and shrink while streaming:** the output callback discards blocks outside the
   crop and box-filters the rest straight into one 466×466 plane. No full-size
   intermediate, so memory is the same at any zoom.
5. **Write** `<key>.565` and `<key>.th` to a temporary name and rename over the old one,
   so a power cut mid-write leaves the previous copy showing.

**The auto crop** is the centre square, the short side filling the circle.

**What it cannot read** — progressive JPEG (needs the whole image's coefficients in
memory, tens of MB for a phone photo), HEIC (no HEVC decoder), PNG and anything else —
is listed as "can't read here" with the reason, never shown as a broken photo. The
user guide tells iPhone owners to set the camera to Most Compatible.

*Estimate:* 2–5 s per typical 3–6 MB phone JPEG, about half of it the SPI read; 200 new
photos take 10–15 minutes after the reboot. A photo opened before the worker reaches it
converts on the spot, with a progress ring.

**The 255 cap (§0 ruling 8).** `max_entries` widens to `uint16_t` in G1, and every
source's declared cap and every caller that counts against it (`orb_files.c`'s walk,
`orb_app_scan.c`, `orb_theme.c`'s clock source) is checked for an 8-bit assumption in
the same change. The cap stays per source; the existing sources keep their values. The
gallery declares its own, sized in G0 from what one walk of `Photos/` costs.

### 2.3 The OS surface: the `photos` cap

A new cap, `photos`, declared in `app.json` and revocable on the dial (D19). New
imports, all gated on it:

```c
int32_t orb_photo_count(void);              /* photos ready to show */
int32_t orb_photo_gen(void);                /* moves when the list changes */
int32_t orb_photo_show(int32_t i);          /* queue photo i; answers at once */
int32_t orb_photo_ready(void);              /* the index on the glass, -1 while loading */
```

`show` reads `<key>.565` on the worker into a PSRAM plane and swaps it onto the app's
page at the next frame. **Two planes** (868 KB): one on the glass, the next prefetched,
so a knob turn is instant. If §4 finds PSRAM tight, one plane and a short wait.

**The card is read at runtime (§0 ruling 7).** `orb_usb_msc.h`'s comment ("never a
runtime read path") is rewritten in G1 to name the gallery as the reader it allows, and
why giving the card away in the USB window is still acceptable: the dial keeps drawing
from flash and only the gallery goes dark. With no card, or in the USB window, the
gallery shows "Photos are on the computer" and recovers on the next mount; a card
pulled mid-read fails that read, keeps the photo already on the glass, and never stalls
the UI task.

### 2.4 The app (`apps/gallery`)

Small wasm, 64 KB memory, caps `photos`, `period_ms: 1000`.

- **Knob:** turn steps photo to photo; press toggles slideshow and hold.
- **Settings:** `interval` (10 s, 30 s, 1 min, 5 min, 15 min), `order` (in turn, or
  shuffled without repeats until every photo has shown), `resume` (start on the last
  photo).
- **Burn-in:** the glass is AMOLED. A photo held long drifts 1–2 px every few minutes.
- **Transitions:** hard cuts. A fade blends two full planes every frame, and that waits
  on the render work the shell transitions are shelved behind.
- **A theme** only if one earns its place (a bezel ring, say), and then a tiny one; the
  photos never enter it (§0 ruling 1).

### 2.5 The crop editor (`apps/gallery/editor`)

The gallery ships its own editor, built from the kit's start sheet (theme-editor-kit
plan), reached from Gallery's App settings in Orb Manager.

**The Photos table**, one row per original:

| Thumb | Photo | Status | Crop | |
|---|---|---|---|---|
| ▢ | IMG_2041.jpg | ready | auto | **Edit** |
| ▢ | IMG_2077.jpg | converting… | — | |
| ▢ | beach.jpg | can't read here (progressive) | — | |

**Edit** opens the original inside a true-size round mask (177 dpi by default):

- **slide:** drag the photo under the circle;
- **zoom:** wheel or pinch, from just covering the circle to 4×;
- **rotate 90°:** for the photo whose EXIF tag is wrong;
- **reset to auto;**
- **save:** the orb re-renders and switches to that photo, so the result is checked on
  the glass. The browser preview and the orb's render read the same numbers through
  different decoders; the orb's is the one that counts.

The browser decodes the original itself, so it shows any format at full resolution (it
fetches the file over the LAN; *estimate:* a few seconds per phone photo).

**A crop is numbers, not pixels:**

```json
"IMG_2041.jpg": { "size": 4183552, "cx": 0.62, "cy": 0.41, "zoom": 2.3, "rot": 0 }
```

`cx`, `cy` are the crop's centre on the EXIF-rotated original, `zoom: 1` puts the short
side across the circle, `rot` is the editor's extra quarter-turns. A rebuilt cache
re-renders every crop. The `size` is the identity check: a different file under the same
name drops its crop back to auto.

*Estimate:* a 3× crop decodes at 1/1, 5–10 s per save. Viewing is the cached copy either
way.

**No knob crop on the dial, for now.** It could be done (press cycles zoom, x, y; turn
adjusts), but each step is a seconds-long render and the press is already slideshow and
hold. Revisit if a nudge away from a computer is wanted.

### 2.6 Routes

New, all bound to the gallery's two folders and the same `orb_sd` name validators as
the other card-file routes:

| route | does |
|---|---|
| `GET /api/v1/gallery` | the list: name, size, status, crop |
| `GET /api/v1/gallery/orig?name=` | an original's bytes, for the editor |
| `GET /api/v1/gallery/thumb?name=` | a thumbnail |
| `POST /api/v1/gallery/crop` | save a crop and queue its re-render |
| `POST /api/v1/gallery/delete-all` | the uninstall offer: `Photos/` and the cache |

### 2.7 Uninstall

Orb Manager and the dial both ask, after the usual uninstall: **"Also delete 214 photos
(1.8 GB) from the card"**, unchecked by default. Settings are wiped either way;
`crops.json` and the cache go only with the photos.

## 3. Slices

| slice | what | depends on |
|---|---|---|
| G0 | §4's measurements on Bronze; revise every estimate here in place | — |
| G1 | the worker: walk, EXIF, decode, crop, cache, thumbnails; `max_entries` to `uint16_t` (§0 ruling 8); `orb_usb_msc.h`'s invariant rewritten (§0 ruling 7) | G0 |
| G2 | the `photos` cap and imports, the two planes, the no-card screen; `binds --write` | G1 |
| G3 | the routes (§2.6) | G1 |
| G4 | `apps/gallery`: knob, settings, slideshow, drift; `os_min` set | G2 |
| G5 | `apps/gallery/editor`: Photos table, crop editor, uninstall offer | G3, G4 |

## 4. Owed before G1 (all on Bronze)

1. Free PSRAM with the clock's and the tracker's planes up: decides two planes or one.
2. tjpgd decode time for one real phone JPEG at 1/4 and 1/1.
3. Card read speed for a 434 KB file and a 5 MB file, WiFi busy.
4. Conversion of a 200-photo copy, end to end.

## 5. Open

- Whether the dial needs a knob crop later (§2.5).
- Whether the hub should one day convert what the orb cannot read (progressive, HEIC)
  and write a baseline copy into `Photos/` — the first file anything but the owner
  would put there, so it needs its own ruling against §0 rulings 2 and 3.
