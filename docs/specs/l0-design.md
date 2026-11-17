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

*Revised 2026-11-17 on the owner's steer ("can the data in the theme actually be compressed?"),
after hub step 1 had shipped a PCM-only writer; §3.1's file is now the orb's own encoding.*

From gorb-os: the orb plays only 22,050 Hz mono, as PCM16 or its own blocked IMA ADPCM
(`main/audio/orb_sound.h` `ORB_SOUND_RATE 22050u`; `main/audio/orb_adpcm.h`: 256 samples in
132 bytes a block, each block `int16 sample0, u8 step index, u8 0, 128 B of nibbles low first`).
The bakers store a clip as ADPCM unless its round trip falls under `ADPCM_MIN_SNR_DB = 20`
(`tools/theme_schema.py` `sound_encode`, ~759–774; the baker's `soundEncode`, one choice on one
sum, pinned by `tools/host_adpcm_test.py`). An app's voice bank holds at most 1 MB
(`main/art/orb_voice.h` `ORB_VOICE_CAP`). The editors' own clip rules: a clock loop is at most
8 s (`sound-engine.js` `LOOP_S`), a pomodoro upload at most 4.0 s (`pomodoro-bake.js`
`UPLOAD_MAX_S`), and the clock calls anything past 10 s "a long sound" (`SND_LONG_S`).

| Bound | Value | Where it comes from |
|---|---|---|
| format | RIFF/WAVE holding either PCM16 (format tag 1, 16 bits, block align 2) or the orb's ADPCM (tag 0x11, 4 bits, block align 132, 256 samples a block, a `fact` sample count), 1 channel | the two encodings the orb plays |
| sample rate | exactly 22,050 Hz | `ORB_SOUND_RATE`, the engine's one rate |
| length | at most 20.0 s = 441,000 samples | **answered (owner, 2026-11-17)** — the longer line §5 question 1 offered; the editors' own clip rules (the clock's 8 s loops, the pomodoro's 4 s fit) stay inside it |
| byte size | at most 1 MB = 1,048,576 bytes (the whole file) | `ORB_VOICE_CAP`: one sound can never be bigger than the bank that has to hold it |

## 3. The contract both repos share

### 3.1 The file an editor sends: the orb's own bytes, in a WAV header

`data:audio/wav;base64,<standard padded base64>`. The body is exactly what `GorbBake.soundEncode`
returns for the samples (ADPCM unless under the SNR floor, so never PCM when ADPCM qualifies),
wrapped so the site can bound it from the header. All integers little-endian.

PCM16 (`enc` 0), 44-byte header:

```
"RIFF" u32 36+2n "WAVE"
"fmt " u32 16  u16 1  u16 1  u32 22050  u32 44100  u16 2  u16 16
"data" u32 2n  <soundEncode's bytes: n int16 samples>
```

ADPCM (`enc` 1), 60-byte header, B = ceil(n/256) blocks:

```
"RIFF" u32 52+132B "WAVE"
"fmt " u32 20  u16 0x0011  u16 1  u32 22050  u32 11370  u16 132  u16 4  u16 2  u16 256
"fact" u32 4   u32 n
"data" u32 132B  <soundEncode's bytes: B orb ADPCM blocks>
```

(11370 = round(22050 × 132 / 256). `samplesPerBlock` 256 is the orb's layout, one sample fewer
than the standard IMA-WAV block of that size: owner question 3.)

- **One encoder.** The file's body is `soundEncode`'s output; nothing re-implements ADPCM.
- **Encoded once.** An upload is encoded when it is taken, a pre-L0 inline array when it is
  migrated. After that the editor keeps the file string it read or made and writes it back
  verbatim: an unchanged sound keeps its bytes and its sha256 across every save, open, remix and
  bake (no re-encode generations).
- **The bake uses the stored bytes.** Where a theme plays an owner sound as it is, the baker puts
  the file's body and encoding in the plane directly, with no `soundEncode`; so the site, a
  download and the bake carry the same bytes. Where an editor derives a new clip from an upload
  (the clock's tick loop made from one tick), it derives from the decoded samples and the baker
  encodes the derived clip as today.
- Playback and readouts in the editor use the decoded samples (`adpcmDecode`), i.e. what the
  orb will play.

### 3.2 Where it goes in a design (schema 2 of both editors)

Each editor raises its design schema 1 → 2, with one pure migration step that turns every
inline sample array into that file. Reading both forms is not enough: an older editor reading a
schema-2 file would see a string where it wants samples and play silence (the bug
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
export const SOUND_TYPE = "a sound in that design is not a WAV the orb plays: a design's sounds are 22,050 Hz mono, as 16-bit PCM or the orb's ADPCM";
export const SOUND_UNREADABLE = "a sound in that design can't be read as a WAV";
export function soundWrongRate(rate: number): string; // `a sound in that design is ${rate.toLocaleString("en-US")} Hz: a design's sounds are 22,050 Hz, the one rate the orb plays`
export function soundTooLong(samples: number): string; // `a sound in that design is ${(Math.ceil(samples / 2205) / 10).toFixed(1)} s long: a design's sounds can be at most 10.0 s`
export function soundTooBig(bytes: number): string;    // `a sound in that design is ${(Math.ceil((bytes * 10) / 1048576) / 10).toFixed(1)} MB: a design's sounds can be at most 1 MB`
```

Refused with the same `AssetError` → `LibraryError(…, 400, "art_refused")` path art uses (the
code word stays `art_refused`: H4's Orb Manager already maps it, and it means "an asset in
that design is refused").

### 3.4 Pre-L0 designs

The site never rewrites a stored design. A schema-1 design with inline samples opens exactly as
stored (the editor's migration step encodes it in the browser); the next save from the newest
editor is schema 2 (M1's save check already demands the newest editor's schema) and carries
`wav` files, which the split stores by hash. An H4 upload of a schema-1 Orb Manager file keeps
its inline samples, as any pre-L0 design does, until its next save on the site.

## 4. Steps

gorb-os (`l0-hub-chain.json`, run from gorb-os `docs/specs/`):

1. `l0-hub-1-wav-kit` — (done) the real pre-L0 fixtures written by the unchanged editors, apps
   repacked and bumped, and a first PCM-only WAV kit.
2. `l0-hub-2-orb-encoding` — the kit rewritten to §3.1: the file is `soundEncode`'s bytes, read
   back with `adpcmDecode`; the baker takes stored bytes as they are; the bounds at 10 s / 1 MB.
3. `l0-hub-3-clock` — clock schema 2: migration step, the file kept verbatim, the bake from the
   stored bytes, the 10 s upload bound, `schema-2` fixture.
4. `l0-hub-4-pomodoro` — the pomodoro the same way, and the full gate with the browser gates.

orb-os-net (`l0-site-chain.json`):

1. `l0-site-1-sound-bounds` — (done) `src/lib/sound-header.ts` and the sound half of
   `design-assets.ts`, PCM16 only at 20 s.
2. `l0-site-1b-orb-adpcm` — the reader and the bound taught §3.1's ADPCM file and the 10 s line.
3. `l0-site-2-stored-once` — the DB-backed proof: stored once, counted once, swept once, refused
   without decoding, served by hash, the pre-L0 fixtures opening and their next save storing
   files, the H4 upload door too.

## 5. Owner questions (L0 picked an answer and carried on)

1. **Longest sound: 10.0 s.** The OS sets no length; the editors' clip rules are the clock's 8 s
   loop, the pomodoro's 4 s and the clock's 10 s "long sound" line. L0 takes the longest of those
   as the site's bound, and the clock now refuses a chime past 10 s at upload (it only warned
   before). Alternatives: 8 s (the longest hard limit today, refusing clock chimes of 8–10 s), or
   a longer line such as 20 s if the owner wants long chimes (1 MB still bounds the bytes).

   **Answered (owner, 2026-11-17):** 20.0 s. The site's bound, the editors' upload doors and the
   baker's `SOUND_MAX_S` are all 20.0 s = 441,000 samples. The editors' own clip rules — the
   clock's 8 s loops and the pomodoro's 4.0 s fit — stay where they are, inside the new line.
2. **Largest sound file: 1 MB**, the voice bank's cap (`ORB_VOICE_CAP`). At 10 s no sound gets near
   it (PCM16 is 441 KB, ADPCM 114 KB); it stops a file padded with extra chunks.

   **Answered (owner, 2026-11-17):** the 1 MB file bound stays. At 20.0 s one clip still fits
   (PCM16 is 882 KB, ADPCM 227 KB), but two long clips together can overrun the bank the orb
   plays them from, so the bake now weighs the whole voice bank as `orb_voice.c` sizes it — head,
   records, even-padded planes — and refuses a theme over 1,024 KB with its numbers (§2's byte
   bound was always one file; this is the bank).
3. **The ADPCM file is the orb's block layout under a standard-looking header** (tag 0x11 with
   `samplesPerBlock` 256, where a standard IMA-WAV block of 132 bytes holds 257). A desktop player
   that honours `samplesPerBlock` plays it with one stray sample per block; one that assumes 257
   may not. The site, the editors and the orb all read it exactly. Recommendation: keep it (the
   bytes are the theme's bytes, which is the point); if a downloadable sound that every player
   plays matters later (L1), the Assets page can offer a PCM export.
4. **A clock chime longer than 10 s saved before L0** opens and plays as before, but its next save
   is refused with `soundTooLong`'s sentence until the owner picks a shorter sound. The
   alternative, trimming it in the migration step, would change a sound behind the owner's back.
5. **Steer after step 1 (2026-11-17).** The first plan stored lossless PCM16 WAVs and bounded them
   at 20 s. On the owner's steer the stored file is the orb's own encoding (§3.1); hub step 1's
   PCM-only writer and site step 1's PCM-only reader are amended by a step each rather than
   redone, and their 20 s bound becomes 10 s.
