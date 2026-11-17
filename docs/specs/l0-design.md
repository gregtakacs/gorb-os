# L0 — sounds stored like art: once, by hash, and bounded

## The ruling

Ruling 41 (`docs/design.md` §0, 2026-11-11) — binding:

> Today an editor's "your own" sounds ride inside the design as raw sample arrays, so every
> version and every remix stores them again, nothing counts them once, and ruling 35 sets them
> no limit.
> - Editors send each user sound as a file (a canonical WAV/ADPCM `data:audio/…;base64` URL),
>   and the site splits it out by sha256 exactly as art (§5, ruling 28): one stored file,
>   referenced by every version and copy that uses it, removed by the sweep only when nothing
>   references it, counted once against the storage limit.
> - Bounds, checked from the header without decoding: the formats the orb plays, a maximum
>   sample rate, length and byte size (L0 sets the numbers from the OS's sound rules); refused
>   with a sentence like art's.
> - Designs saved before L0 keep their inline samples and still open; the next save moves them
>   to files.

design.md §12's gate: *a sound used by two designs and a remix is stored once and counted once;
an over-long or wrong-format sound is refused without decoding; a pre-L0 design with inline
samples still opens and its next save stores files.*

Two repos, like H4. gorb-os (the editors) runs first, because its first step writes the real
pre-L0 fixtures the site's tests then use.

## 1. Where user sounds live today (read 2026-11-17)

Two shelf editors take an owner's own sound; nothing else does (grep `decodeAudioData`):

- **clock** (`apps/clock/editor/clock-logic.js` `sndUpload` ~900, `designContent` ~3405;
  `clock-host.js` `toDoc`/`fromDoc` ~346–361, `pcmOf`/`upsOf` ~121–133). The document carries
  `sounds.up.<slot> = { name, pcm, loop, [rate, first] }`; `pcm` is an `Int16Array` that
  `JSON.stringify` writes as `{"0": v, "1": v, …}`. Tick loops are at most `SoundEngine.LOOP_S`
  (8 s); chimes have no cap, only a warning past `SND_LONG_S` (10 s).
- **pomodoro** (`apps/pomodoro/editor/pomodoro-editor.js` `pcmOf`/`keep` ~35–51, `toDoc`/`fromDoc`
  ~399–420, upload ~101–124). The document carries `design.uploads[i] = { name, ref, samples,
  pcm: [ints] }`; an upload over `PomodoroBake.UPLOAD_MAX_S` (4.0 s) is refused.

Both are design schema 1 (`editor.json` `"schema": 1`). The site stores those arrays as part of
the design JSON.

## 2. The OS's sound rules, and the numbers L0 sets

From gorb-os: `main/audio/orb_sound.h` (`ORB_SOUND_RATE 22050u`), `tools/theme_schema.py`
(`SOUND_RATE = 22050`, "the one rate the engine runs"; encodings PCM16 and the baker's blocked
IMA ADPCM, which only the baker writes, at bake time), `tools/orbsound.py` ("WAVs must already be
22,050 Hz mono 16-bit"). The OS has **no** length or byte cap on a sound.

| Bound | Value | Where it comes from |
|---|---|---|
| format | RIFF/WAVE, format tag 1 (PCM), 1 channel, 16 bits, block align 2 | the orb plays 16-bit mono PCM; orbsound.py's rule |
| sample rate | exactly 22,050 Hz (so at most 22,050) | `ORB_SOUND_RATE`, the engine's one rate |
| length | at most 20.0 s = 441,000 samples | **owner question 1** (the OS sets none) |
| byte size | at most 1 MiB = 1,048,576 bytes (the whole file) | **owner question 2** (the OS sets none) |

A canonical 20.0 s WAV is 882,044 bytes, so for an editor's own file the length binds first;
the byte bound is what stops a file padded with extra chunks.

ADPCM: the ruling says "WAV/ADPCM". The orb's ADPCM is its own 256-sample block layout
(`ADPCM_BLOCK_BYTES = 132`), not the standard WAV IMA layout (format tag 0x11 with a
257-sample block), no editor writes or reads an ADPCM file, and the baker encodes ADPCM from
PCM at bake time anyway. So L0 stores PCM16 WAV only, and a tag-0x11 WAV is refused as
wrong-format (**owner question 3**).

## 3. The contract both repos share

### 3.1 The file an editor sends

`data:audio/wav;base64,<standard padded base64>` of exactly this 44-byte header plus the samples
(all integers little-endian):

```
"RIFF"  u32 36+2n  "WAVE"
"fmt "  u32 16     u16 1 (PCM)  u16 1 (mono)  u32 22050  u32 44100  u16 2  u16 16
"data"  u32 2n     n × int16 samples
```

The same samples always give the same bytes, so the same sound always gives the same sha256:
that is what makes "stored once" true across designs, remixes and re-saves.

### 3.2 Where it goes in a design (schema 2 of both editors)

Each editor raises its design schema 1 → 2, with one pure migration step that turns every
inline sample array into that data URL. Reading both forms is not enough: an older editor
reading a schema-2 file would see a string where it wants samples and play silence (the bug
`clock-host.js:116-120` already documents), and the schema is exactly what stops an editor
opening what it cannot read (ruling 21).

- clock: `sounds.up.<slot> = { name, wav: "data:audio/wav;base64,…", loop, [rate, first] }`
  (`pcm` gone).
- pomodoro: `design.uploads[i] = { name, ref, samples, wav: "data:audio/wav;base64,…" }`
  (`pcm` gone).
- Both readers take `wav` first and still take a legacy `pcm` (array or `{"0":…}` object).

`theme@1` already turns every `data:` URL of at least 512 decoded bytes into an `orb-asset:` ref
with an `orbAssets` sidecar entry, and inflates refs back, without looking at the type; sounds
ride that unchanged. The site's `splitAssets` already walks every string at any depth.

### 3.3 The site's bound and its sentences (exported from `src/lib/design-assets.ts`)

```ts
export const SOUND_PREFIX = "data:audio/wav;base64";
export const SOUND_TYPE = "a sound in that design is not a WAV the orb plays: a design's sounds are 16-bit mono PCM WAV at 22,050 Hz";
export const SOUND_UNREADABLE = "a sound in that design can't be read as a WAV";
export function soundWrongRate(rate: number): string; // `a sound in that design is ${rate.toLocaleString("en-US")} Hz: a design's sounds are 22,050 Hz, the one rate the orb plays`
export function soundTooLong(samples: number): string; // `a sound in that design is ${(Math.ceil(samples / 2205) / 10).toFixed(1)} s long: a design's sounds can be at most 20.0 s`
export function soundTooBig(bytes: number): string;    // `a sound in that design is ${(Math.ceil((bytes * 10) / 1048576) / 10).toFixed(1)} MB: a design's sounds can be at most 1 MB`
```

Refused with the same `AssetError` → `LibraryError(…, 400, "art_refused")` path art uses (the
code word stays `art_refused`: H4's Orb Manager already maps it, and it means "an asset in
that design is refused").

### 3.4 Pre-L0 designs

The site never rewrites a stored design. A schema-1 design with inline samples opens exactly as
stored (the editor's migration step converts it in the browser); the next save from the newest
editor is schema 2 (M1's save check already demands the newest editor's schema) and carries
`wav` URLs, which the split stores as files. An H4 upload of a schema-1 Orb Manager file keeps
its inline samples, as any pre-L0 design does, until its next save on the site.

## 4. Steps

gorb-os (`l0-hub-chain.json`, run from gorb-os `docs/specs/`):

1. `l0-hub-1-wav-kit` — GorbBake's WAV writer/reader and bounds; the real pre-L0 fixtures
   (unchanged editors write them); apps repacked and bumped.
2. `l0-hub-2-clock` — clock schema 2: migration step, `wav` in the file, the 20 s upload bound,
   `schema-2` fixture.
3. `l0-hub-3-pomodoro` — pomodoro schema 2 the same way, the docs, the full gate including the
   live browser gates.

orb-os-net (`l0-site-chain.json`):

1. `l0-site-1-sound-bounds` — `src/lib/sound-header.ts` and the sound half of
   `design-assets.ts`, wired into every place art is asked.
2. `l0-site-2-stored-once` — the DB-backed proof: stored once, counted once, swept once,
   refused without decoding, served by hash, the pre-L0 fixtures opening and their next save
   storing files, the H4 upload door too.

## 5. Owner questions (L0 picked an answer and carried on)

1. **Longest sound: 20.0 s.** The OS has no cap. The clock warns past 10 s ("every second of it
   is room the art cannot use") and loops are at most 8 s; the pomodoro takes 4 s. 20 s leaves
   the clock's warning meaningful and is 882 KB as PCM16. Alternatives: 10 s (the clock's own
   warning line, refusing what it warns about today), or 8 s (the loop cap).
2. **Largest sound file: 1 MB (1,048,576 bytes).** Follows from 20 s of 16-bit mono at 22,050 Hz
   plus room for a WAV's ordinary extra chunks.
3. **ADPCM WAVs refused.** Only PCM16 WAV is stored (§2). Accepting standard IMA ADPCM WAV would
   need a decoder in both editors and a transcode to the orb's block layout at bake time.
4. **A clock chime longer than 20 s saved before L0** opens and plays as before, but its next
   save is refused with `soundTooLong`'s sentence until the owner picks a shorter sound. The
   alternative, trimming it in the migration step, would change a sound behind the owner's back.
