#!/usr/bin/env node
// L0 hub step 2 (docs/specs/l0-design.md §3.1, §2; amends step 1's PCM-only kit): the file an
// owner's sound travels in is the ORB'S OWN encoding. GorbBake.soundFile encodes the samples once
// with soundEncode — ADPCM unless the round trip falls under the SNR floor — behind §3.1's header,
// soundFileUrl wraps that file in the data URL a design carries, and soundRead takes either layout
// back: its `body` is the data chunk as stored and its `pcm` the samples the orb will play. One
// encoder, so the site's stored bytes, a download and the theme's voice bank are one sound, and the
// baker's sound stage puts those stored bytes in the bank as they are instead of encoding again. The
// bounds (10.0 s, 1 MiB) are exported for the doors that enforce them — the reader does not, so a
// chime someone recorded before L0 still opens. The last checks read the committed schema-1 fixtures:
// they are what the unchanged editors write, and the site's pre-L0 tests reuse them.
//
//   node headless-wav.test.js
'use strict';
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
let fails = 0, checked = 0;
const ok = (c, n, x) => { checked++; console.log((c ? 'PASS ' : 'FAIL ') + n + (x === undefined || x === null || x === '' ? '' : '  [' + x + ']')); if (!c) fails++; };
const refuses = (fn, want, name) => {
  try { fn(); ok(false, name, 'accepted'); } catch (e) { ok(String(e.message).indexOf(want) >= 0, name, String(e.message).slice(0, 170)); }
};
const G = require(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'));
const SR = G.SOUND_RATE;
const ROOT = path.join(__dirname, '..', '..', '..', '..', '..');
const N = 2205;                                        // the fixture's sound: 0.1 s of a 660 Hz sine
const SINE = (() => { const p = new Int16Array(N); for (let i = 0; i < N; i++) p[i] = Math.round(8000 * Math.sin(2 * Math.PI * 660 * i / SR)); return p; })();
// the clip soundEncode keeps lossless: Skeleton's wood tick, whose 3.2 kHz attack 4-bit ADPCM
// smears to 17.4 dB, under theme_schema.ADPCM_MIN_SNR_DB's 20, so the encoder writes it as PCM16
const CLICK = G.synth({ name: 'tick', label: 'Wood tick', dur: 0.4,
  events: [{ t: 0, kind: 'click', f: 3200, q: 2, dur: 0.35, gain: 1.0 }] });
const same = (a, b) => !!a && a.length === b.length && a.every((v, i) => v === b[i]);
const hex = (b) => Array.from(b).map((x) => x.toString(16).padStart(2, '0')).join(' ');
const eq = (a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b)) === 0;   // bytes, not identity
const urlOf = (bytes) => G.SOUND_WAV_PREFIX + ',' + Buffer.from(bytes).toString('base64');
// §3.1's two files, written here so the reader is answered by something other than the code under it
const chunk = (id, body) => {
  const b = new Uint8Array(8 + body.length + (body.length & 1)), dv = new DataView(b.buffer);
  for (let i = 0; i < 4; i++) b[i] = id.charCodeAt(i);
  dv.setUint32(4, body.length, true); b.set(body, 8); return b;
};
const join = (list) => { let n = 0; list.forEach((b) => { n += b.length; }); const out = new Uint8Array(n); let o = 0; list.forEach((b) => { out.set(b, o); o += b.length; }); return out; };
const riff = (body) => {
  const sz = new Uint8Array(4); new DataView(sz.buffer).setUint32(0, body.length + 4, true);
  return join([new Uint8Array([0x52, 0x49, 0x46, 0x46]), sz, new Uint8Array([0x57, 0x41, 0x56, 0x45]), body]);
};
const fmtBody = (o) => {                                            // a fmt chunk's body, §3.1's fields
  const b = new Uint8Array(o.size === undefined ? 16 : o.size), dv = new DataView(b.buffer);
  const rate = o.rate === undefined ? SR : o.rate, bits = o.bits === undefined ? 16 : o.bits;
  const align = o.align === undefined ? 2 : o.align;
  dv.setUint16(0, o.tag === undefined ? 1 : o.tag, true); dv.setUint16(2, o.ch === undefined ? 1 : o.ch, true);
  dv.setUint32(4, rate, true);
  dv.setUint32(8, o.bps === undefined ? Math.round(rate * align / (bits / 8)) : o.bps, true);
  dv.setUint16(12, align, true); dv.setUint16(14, bits, true);
  if (b.length >= 20) { dv.setUint16(16, o.cb === undefined ? 2 : o.cb, true); dv.setUint16(18, o.spb === undefined ? 256 : o.spb, true); }
  return b;
};
const factBody = (n) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, n, true); return b; };
const pcmBytes = (p) => {                                            // a clip's PCM16 bytes, little-endian
  const b = new Uint8Array(p.length * 2), dv = new DataView(b.buffer);
  for (let i = 0; i < p.length; i++) dv.setInt16(2 * i, p[i], true);
  return b;
};
const PC = (o) => {                                                   // a PCM16 file, §3.1's layout
  o = o || {};
  return riff(join([chunk('fmt ', fmtBody(o)), chunk('data', o.data === undefined ? pcmBytes(SINE) : o.data)].filter((x) => x)));
};
const AD = (o) => {                                                   // the orb's ADPCM file, §3.1's layout
  o = Object.assign({ tag: 0x11, size: 20, align: 132, bits: 4, bps: 11370 }, o || {});
  const n = o.n === undefined ? N : o.n, body = o.data === undefined ? new Uint8Array(132 * Math.ceil(n / 256)) : o.data;
  return riff(join([chunk('fmt ', fmtBody(o)), o.fact === false ? null : chunk('fact', factBody(n)), chunk('data', body)].filter((x) => x)));
};

// ---- the writer: the orb's own encoding under §3.1's header ---------------------------------------
{
  const f = G.soundFile(SINE), head = f.bytes.subarray(0, 60);
  // §3.1's ADPCM header for n = 2205: RIFF 52+132·9, a 20-byte fmt of tag 0x11/1ch/22050/11370/132/4
  // bits/cbSize 2/256 samples a block, a fact of 2205, and data of 9 blocks — the literal bytes
  const HEAD = '52 49 46 46 d8 04 00 00 57 41 56 45 66 6d 74 20 14 00 00 00 11 00 01 00 22 56 00 00'
    + ' 6a 2c 00 00 84 00 04 00 02 00 00 01 66 61 63 74 04 00 00 00 9d 08 00 00 64 61 74 61 a4 04 00 00';
  ok(f.enc === 1 && f.n === N && hex(head) === HEAD && eq(f.body, G.soundEncode(SINE).bytes)
    && f.bytes.length === 60 + f.body.length && eq(f.bytes.subarray(60), f.body),
    "soundFile writes the orb's ADPCM when it qualifies",
    'enc ' + f.enc + ', ' + f.bytes.length + ' B behind a 60-byte header, ' + f.url.length + ' chars of URL');
  const pay = f.url.slice(G.SOUND_WAV_PREFIX.length + 1);
  ok(f.url.slice(0, G.SOUND_WAV_PREFIX.length + 1) === G.SOUND_WAV_PREFIX + ','
    && /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(pay) && Buffer.from(pay, 'base64').length === f.bytes.length,
    'the url is the design’s prefix plus standard padded base64 of the whole file', pay.length + ' chars');
  ok(G.soundFileUrl(SINE) === f.url && same(G.soundRead(f.url).pcm, G.adpcmDecode(f.body, N)),
    'soundFileUrl is soundFile’s url, and it decodes to what the orb will play');
  const c = G.soundFile(CLICK), chead = c.bytes.subarray(0, 44), cn = c.n;
  // §3.1's PCM16 header for the click: RIFF 36+2n, a 16-byte fmt of tag 1/1ch/22050/44100/2/16, data 2n
  const CHEAD = '52 49 46 46 ' + hex(new Uint8Array([0x0c, 0x45, 0x00, 0x00])) + ' 57 41 56 45 66 6d 74 20'
    + ' 10 00 00 00 01 00 01 00 22 56 00 00 44 ac 00 00 02 00 10 00 64 61 74 61 '
    + hex(new Uint8Array([0xe8, 0x44, 0x00, 0x00]));
  ok(cn === CLICK.length && c.enc === 0 && G.soundEncode(CLICK).enc === 0 && hex(chead) === CHEAD
    && c.bytes.length === 44 + 2 * cn && eq(c.bytes.subarray(44), pcmBytes(CLICK)) && eq(c.body, pcmBytes(CLICK)),
    "soundFile writes PCM16 only under the SNR floor",
    'enc ' + c.enc + ', ' + cn + ' samples (' + (cn / SR).toFixed(2) + ' s), a 44-byte header and the samples');
  refuses(() => G.soundFile(new Int16Array(1)), 'a sound is at least two samples',
    'soundFile refuses a clip a sound plane cannot hold');
  refuses(() => G.soundFile(new Int16Array(0)), 'a sound is at least two samples',
    'soundFile refuses no samples at all, and does not write an empty file');
}
// ---- one encoder: the baker's JS and tools/theme_schema.py's Python give one answer --------------
{
  const py = 'import base64, json, sys\n'
    + 'sys.path.insert(0, "tools")\n'
    + 'import theme_schema as ts\n'
    + 'for line in sys.stdin:\n'
    + '    enc, body = ts.sound_encode(json.loads(line))\n'
    + '    print(enc, base64.b64encode(bytes(body)).decode())\n';
  const run = spawnSync('python3', ['-c', py], { cwd: ROOT, encoding: 'utf8',
    input: JSON.stringify(Array.from(SINE)) + '\n' + JSON.stringify(Array.from(CLICK)) + '\n' });
  if (run.error || run.status !== 0) {
    console.log("SKIP the body is theme_schema.sound_encode's bytes  [" +
      (run.error ? String(run.error.message) : String(run.stderr).slice(0, 140)) + ']');
  } else {
    const want = (i) => { const w = String(run.stdout).trim().split('\n')[i].split(' '); return { enc: +w[0], body: new Uint8Array(Buffer.from(w[1], 'base64')) }; };
    const a = G.soundFile(SINE), b = G.soundFile(CLICK), wa = want(0), wb = want(1);
    ok(a.enc === wa.enc && eq(a.body, wa.body) && b.enc === wb.enc && eq(b.body, wb.body),
      "the body is theme_schema.sound_encode's bytes",
      'the sine as enc ' + a.enc + ' in ' + a.body.length + ' B and the tick as enc ' + b.enc + ' in ' + b.body.length + ' B, both bodies byte-equal');
  }
}
// ---- the reader, both layouts -------------------------------------------------------------------
{
  const f = G.soundFile(SINE), r = G.soundRead(f.bytes), c = G.soundFile(CLICK), rc = G.soundRead(c.url);
  ok(r && r.enc === 1 && r.n === N && eq(r.body, f.body) && same(r.pcm, G.adpcmDecode(f.body, N))
    && rc && rc.enc === 0 && rc.n === c.n && eq(rc.body, c.body) && same(rc.pcm, CLICK),
    'soundRead round-trips both encodings',
    r && rc ? 'enc ' + r.enc + ', ' + r.n + ' samples back from ' + r.body.length + ' B; enc ' + rc.enc + ', ' + rc.n + ' samples byte-exact' : 'null');
  ok(r.body instanceof Uint8Array && r.pcm instanceof Int16Array
    && same(G.soundRead(f.bytes.buffer).pcm, r.pcm) && same(G.soundRead(new Uint8Array(f.bytes)).pcm, r.pcm),
    'soundRead takes the bytes, the ArrayBuffer and the URL alike');
  ok(eq(G.soundRead(f.bytes).body, G.soundEncode(SINE).bytes) && eq(G.soundRead(c.bytes).body, G.soundEncode(CLICK).bytes),
    "soundRead's body is soundEncode's bytes, so a bake needs no second encode");
  ok(G.soundRead(f.bytes.subarray(0, 40)) === null && G.soundRead(new Uint8Array(0)) === null,
    'a fragment of a header, and no bytes at all, read as nothing');
}

// ---- canonical: one sound is one file, and a stored file is never refreshed ----------------------
{
  const a = G.soundFile(SINE), b = G.soundFile(SINE);
  ok(a.url === b.url && eq(a.bytes, b.bytes) && G.soundFileUrl(Array.from(SINE)) === a.url,
    'the same samples give the same file', 'two calls and a plain array give one ' + a.url.length + '-char URL');
  const r = G.soundRead(a.url), head = a.bytes.subarray(0, a.bytes.length - r.body.length);
  ok(r.enc === a.enc && r.n === a.n && eq(r.body, a.body) && urlOf(join([head, r.body])) === a.url
    && eq(G.soundRead(a.bytes).body, a.body),
    'an unchanged file keeps its bytes',
    'read from the URL and from the file: the same ' + r.body.length + ' B, and its own URL again, so its sha256 holds across every save');
  ok(!/soundFile\([^)]{0,24}soundRead\(/.test(fs.readFileSync(path.join(__dirname, '..', 'baker@1', 'orbtheme-baker.js'), 'utf8')),
    'nothing in the kit re-encodes a file it has just read (that would be a second generation)');
}
// ---- what the orb does not play, and what a cut or mislabelled file is ---------------------------
{
  // a chunk whose header names more bytes than the file holds: the reader must refuse it, not trust it
  const bigData = (n) => { const b = new Uint8Array(8 + 8); for (let i = 0; i < 4; i++) b[i] = 'data'.charCodeAt(i); new DataView(b.buffer).setUint32(4, n, true); return b; };
  const cases = [
    ['stereo', () => PC({ ch: 2, align: 4 })],
    ['8-bit', () => PC({ bits: 8, align: 1, data: new Uint8Array(N) })],
    ['44,100 Hz', () => PC({ rate: 44100 })],
    ['format tag 3 (IEEE float)', () => PC({ tag: 3 })],
    ['a standard IMA-WAV block of 257 samples', () => AD({ spb: 257 })],
    ['ADPCM with the standard 257-byte block align', () => AD({ align: 257 })],
    ['ADPCM in a 16-byte fmt', () => AD({ size: 16 })],
    ['ADPCM with cbSize 0', () => AD({ cb: 0 })],
    ['ADPCM with no fact chunk', () => AD({ fact: false })],
    ['ADPCM with a fact naming no samples', () => AD({ n: 0, data: new Uint8Array(0) })],
    ['ADPCM data one block short of its fact', () => AD({ n: 300, data: new Uint8Array(132) })],
    ['ADPCM data one block longer than its fact', () => AD({ data: new Uint8Array(132 * 10) })],
    ['PCM data declared past the bytes', () => riff(join([chunk('fmt ', fmtBody({})), bigData(8000)]))],
    ['ADPCM with fact after data', () => riff(join([chunk('fmt ', fmtBody({ tag: 0x11, size: 20, align: 132, bits: 4, bps: 11370 })), chunk('data', new Uint8Array(1188)), chunk('fact', factBody(N))]))],
    ['cut inside the header', () => PC().subarray(0, 30)],
    ['cut inside the data', () => AD().subarray(0, 100)],
    ['data before fmt', () => riff(join([chunk('data', pcmBytes(SINE)), chunk('fmt ', fmtBody({}))]))],
    ['not RIFF', () => join([new Uint8Array([0x58, 0x4d, 0x4c, 0x3c, 0, 0, 0, 40, 0, 0, 0, 0]), PC().subarray(12)])],
    ['RIFF but not WAVE', () => riff(join([chunk('fmt ', fmtBody({})), chunk('data', pcmBytes(SINE))])).map((x, i) => i === 11 ? 0x41 : x)],
    ['an odd PCM data size at 22,050 Hz', () => PC({ data: new Uint8Array(4001) })],
    ['a header with no chunks after it', () => new Uint8Array([0x52, 0x49, 0x46, 0x46, 4, 0, 0, 0, 0x57, 0x41, 0x56, 0x45])],
  ];
  const bad = [];
  for (const [name, bytes] of cases) {
    let v;
    try { v = G.soundRead(bytes()); } catch (e) { bad.push(name + ' threw ' + e.message); continue; }
    if (v !== null) bad.push(name + ' read as ' + v.n + ' samples of enc ' + v.enc);
  }
  [['bad base64', G.SOUND_WAV_PREFIX + ',@@@ not base64 @@@'],
   ['an mp3’s data URL', 'data:audio/mpeg;base64,' + Buffer.from([0x49, 0x44, 0x33, 3, 0, 0, 0, 0]).toString('base64')],
   ['a plain string', 'RIFF'], ['undefined', undefined], ['a number', 22050], ['an object', {}]]
    .forEach(([name, v]) => {
      try { if (G.soundRead(v) !== null) bad.push(name + ' read as a sound'); }
      catch (e) { bad.push(name + ' threw ' + e.message); }
    });
  ok(!bad.length, 'soundRead refuses what the orb does not play',
    bad.join('; ') || (cases.length + 6) + ' cases, each null and none throwing');

  // an odd LIST chunk (7 bytes, padded by the byte a reader has to step over) between fmt and data
  const list = chunk('LIST', new Uint8Array([0x49, 0x4e, 0x46, 0x4f, 0x00, 0x00, 0x01]));
  const p = G.soundRead(riff(join([chunk('fmt ', fmtBody({})), list, chunk('data', pcmBytes(SINE))])));
  const f = G.soundFile(SINE);
  const a = G.soundRead(riff(join([chunk('fmt ', fmtBody({ tag: 0x11, size: 20, align: 132, bits: 4, bps: 11370 })),
    list, chunk('fact', factBody(N)), chunk('data', f.body)])));
  ok(same(p && p.pcm, SINE) && p.enc === 0 && same(a && a.pcm, G.adpcmDecode(f.body, N)) && eq(a.body, f.body),
    'soundRead skips unknown chunks',
    'a 7-byte LIST between the chunks it needs, in both layouts: ' + (p ? p.n : 0) + ' and ' + (a ? a.n : 0) + ' samples back');
  const mp3 = G.soundRead(riff(join([chunk('LIST', list.subarray(8, 12)), chunk('fmt ', fmtBody({})), chunk('data', pcmBytes(SINE))])));
  ok(same(mp3 && mp3.pcm, SINE), 'soundRead walks a real-world WAV’s extra chunks before the ones that matter');
}
// ---- the one normaliser: every form a design has ever carried its samples in --------------------
{
  const obj = {}; for (let i = 0; i < N; i++) obj[String(i)] = SINE[i];
  const backwards = {}; Object.keys(obj).reverse().forEach((k) => { backwards[k] = obj[k]; });
  const bad = [];
  const want = (name, v, exp) => { const got = G.soundPcm(v); if (!same(got, exp)) bad.push(name + ' gave ' + (got ? got.length + ' samples' : String(got))); };
  want('an Int16Array', SINE, SINE);
  want('a plain array of samples', Array.from(SINE), SINE);
  want('the {"0": …} object JSON.stringify makes of a typed array', obj, SINE);
  want('that object with its keys written backwards', backwards, SINE);
  want('a §3.1 ADPCM data URL', G.soundFileUrl(SINE), G.adpcmDecode(G.soundEncode(SINE).bytes, N));
  want('a §3.1 PCM16 data URL', G.soundFileUrl(CLICK), CLICK);
  want('a short array', [1, -2, 30000], Int16Array.from([1, -2, 30000]));
  [['undefined', undefined], ['null', null], ['a number', 8000], ['true', true], ['a plain string', 'x'],
   ['an object with no numeric keys', { name: 'l0-chime.wav' }], ['a video URL', 'data:video/mp4;base64,AAAA'],
   ['an audio URL whose base64 is broken', G.SOUND_WAV_PREFIX + ',@@@'],
   ['an mp3 URL', 'data:audio/mpeg;base64,' + Buffer.from([0x49, 0x44, 0x33, 3, 0, 0, 0, 0]).toString('base64')]]
    .forEach(([name, v]) => { if (G.soundPcm(v) !== null) bad.push(name + ' gave a sound'); });
  ok(!bad.length, 'soundPcm takes every legacy form',
    bad.join('; ') || 'typed array, array, {"0":…} in any key order, and §3.1’s ADPCM and PCM16 urls in; null for the other nine');
  ok(G.soundPcm(SINE) === SINE, 'soundPcm hands back the Int16Array it was given, not a copy of it');
  ok(same(G.soundPcm(G.soundFileUrl(SINE)), G.soundRead(G.soundFileUrl(SINE)).pcm),
    'a URL through soundPcm is the same answer as through soundRead — one reader, not two');
}

// ---- the bounds (§2): the numbers, and who is allowed to enforce them ----------------------------
{
  ok(G.SOUND_MAX_S === 10 && G.SOUND_MAX_SAMPLES === 220500 && G.SOUND_MAX_BYTES === 1048576
    && G.SOUND_WAV_PREFIX === 'data:audio/wav;base64',
    "the bounds are the design's numbers",
    G.SOUND_MAX_S + ' s · ' + G.SOUND_MAX_SAMPLES + ' samples · ' + G.SOUND_MAX_BYTES + ' B · ' + G.SOUND_WAV_PREFIX);
  ok(G.SOUND_MAX_SAMPLES === G.SOUND_MAX_S * SR && G.SOUND_MAX_BYTES > G.SOUND_MAX_S * 2 * SR,
    'the sample bound is the second bound at the one rate, and the byte bound never bites inside it',
    G.SOUND_MAX_S + ' s × ' + SR + ' Hz; the worst ' + G.SOUND_MAX_SAMPLES + ' samples are ' + (44 + 2 * G.SOUND_MAX_SAMPLES) + ' B');
  // the doors enforce; soundRead never does, so an old recording still opens
  const long = new Int16Array(G.SOUND_MAX_SAMPLES + 2);
  for (let i = 0; i < long.length; i++) long[i] = ((i * 37) % 65536) - 32768;
  const over = G.soundRead(riff(join([chunk('fmt ', fmtBody({})), chunk('data', pcmBytes(long))])));
  const huge = G.soundRead(PC({ data: new Uint8Array(2 * 600000) }));
  ok(over && over.n === long.length && huge && huge.n === 600000,
    'soundRead enforces neither bound: a longer clip and a bigger file both open',
    (over ? over.n : 0) + ' samples (' + ((over ? over.n : 0) / SR).toFixed(2) + ' s) and ' + (huge ? huge.n : 0) +
    ' samples in ' + (44 + 2 * 600000) + ' B, past both');
  ok(G.soundRead(G.soundFile(long).url).n === long.length,
    'a file the site would refuse at the door still round-trips through the reader that opens a design');
}

// ---- the baker bakes a stored sound as it is ----------------------------------------------------
{
  // A stored clip whose encoding is NOT what soundEncode would choose for its samples today: the
  // wood tick stays PCM16 under the SNR floor, so a §3.1 file that carries it as ADPCM — an older
  // encoder's answer, or an owner's file that reached the site already compressed — is a sound the
  // encoder would not write now. The plane must be those bytes exactly, at that enc: the site's
  // file and the bank are one sound, with no second encode and no generation loss (§3.1).
  const body = G.adpcmEncode(CLICK), n = CLICK.length;
  const pcm = CLICK;
  ok(G.soundEncode(pcm).enc === 0 && G.soundEncode(pcm).bytes.length === 2 * n,
    'a stored clip really can disagree with what encoding the samples now would give', 'soundEncode picks enc 0, the file says enc 1');
  const B = G.stage3({});
  const ans = B.sounds([{ slot: 3, pcm, coded: { enc: 1, body }, name: 'stored' }], { pattern: 'none' });
  const sp = B.bank.sprites[0];
  const P = G.stage3({});
  const pans = P.sounds([{ slot: 1, pcm: SINE.slice() }], { pattern: 'none' });   // the sine, uncoded
  const psp = P.bank.sprites[0];
  ok(ans.bytes === body.length && sp.size === body.length && sp.w === n && sp.h === 1
    && ans.enc[0].enc === 1 && ans.enc[0].n === n
    && eq(B.bank.art.subarray(sp.off, sp.off + sp.size), body)
    && pans.enc[0].enc === 1 && eq(P.bank.art.subarray(psp.off, psp.off + psp.size), G.soundEncode(SINE).bytes),
    'the baker bakes a stored sound as it is',
    'a ' + body.length + ' B plane of ' + n + ' samples at enc 1 — the stored bytes, ' + (2 * n - body.length)
    + ' B smaller than a re-encode; and a pcm-only item still gets soundEncode’s own answer');

  // two stored slots naming one file share one plane; a stored clip never shares with the same
  // samples straight from the encoder, which is a different answer on the bank
  const C = G.stage3({});
  const c2 = C.sounds([{ slot: 1, pcm, coded: { enc: 1, body } }, { slot: 2, pcm: pcm.slice(), coded: { enc: 1, body: body.slice() } },
    { slot: 3, pcm }], { pattern: 'none' });
  ok(C.bank.sprites.length === 2 && c2.rows === 3 && c2.bytes === body.length + 2 * n
    && C.bank.sprites[0].off === 0 && C.bank.sprites[1].off === body.length,
    'stored sounds share a plane by their bytes, and never with a re-encoded one',
    [c2.rows, C.bank.sprites.length, c2.bytes].join(' · '));

  // the clock's own layer: o.sounds items carry their sound the same way, and refuse by slot name
  const W = 466;
  const flat = (w, h, rgb) => { const r = new Uint8Array(w * h * 4); for (let i = 0; i < w * h; i++) r.set([rgb[0], rgb[1], rgb[2], 255], i * 4); return { w, h, rgba: r }; };
  const hand = Object.assign(flat(9, 120, [220, 200, 120]), { slot: 0, x: 229, y: 123, pivot_x: 4, pivot_y: 110 });
  const base = { name: 'Stored', serial: 1, bakedMs: 1, plate: flat(W, W, [20, 30, 40]), hands: [hand], order: [0],
    scalars: { second_sweep: 0, tick_rate: 1 } };
  const clk = G.verifyBundle3(G.bakeClock3(Object.assign({}, base, {
    sounds: [{ slot: 'chime_hour', pcm, coded: { enc: 1, body } }] })).bytes).sections[0];
  const crow = clk.blob.elements.find((e) => e['class'] === G.classId('sound')), csp = clk.blob.sprites[crow.frame0];
  ok(crow.src === 1 && csp.size === body.length && eq(clk.art.subarray(csp.off, csp.off + csp.size), body)
    && clk.blob.scalars.plate_off === body.length && (csp.off & 1) === 0,
    'the clock’s layer puts the same stored bytes at the head of its bank',
    'row src ' + crow.src + ', plane ' + csp.size + ' B at offset ' + csp.off + ', plate_off ' + clk.blob.scalars.plate_off
    + ' — and bakeClock3 ran checkClockSounds on it, the dial’s own refusals');

  // and the refusal, from both layers, before the bank moves: a body that is not the length its
  // encoding gives for its samples is not that sound
  const D = G.stage3({});
  const before = D.bank.size;
  const SENT = 'sound slot 5 carries a stored sound whose bytes do not match its length';
  const rejs = [
    ['an ADPCM body one block short', 5, pcm, { enc: 1, body: body.subarray(0, body.length - 132) }, SENT],
    ['a PCM16 body one sample short', 6, SINE, { enc: 0, body: pcmBytes(SINE).subarray(0, 2 * N - 2) }, 'sound slot 6 carries a stored sound whose bytes do not match its length'],
    ['an encoding the orb does not play', 7, pcm, { enc: 2, body }, 'sound slot 7 carries a stored sound whose bytes do not match its length'],
    ['no bytes at all', 8, pcm, { enc: 1 }, 'sound slot 8 carries a stored sound whose bytes do not match its length'],
  ];
  const why = [];
  for (const [name, slot, p, cd, want] of rejs) {
    try { D.sounds([{ slot, pcm: p, coded: cd }]); why.push(name + ': accepted'); }
    catch (e) { if (e.message !== want) why.push(name + ': ' + e.message); }
  }
  ok(!why.length && D.bank.size === before && D.bank.sprites.length === 0,
    'a stored sound with the wrong byte count is refused',
    why.join('; ') || 'all four, in the design’s own sentence, with the bank still at ' + before + ' B');
  refuses(() => G.bakeClock3(Object.assign({}, base, {
    sounds: [{ slot: 'chime_hour', pcm, coded: { enc: 1, body: body.subarray(0, 100) } }] })), SENT.replace(' 5', ' chime_hour'),
    'the clock’s own layer refuses it too, naming the slot the author can see');
}

// ---- the committed pre-L0 fixtures --------------------------------------------------------------
{
  const read = (p) => { try { return JSON.parse(fs.readFileSync(path.join(ROOT, p), 'utf8')); } catch (e) { return null; } };
  const c = read('apps/clock/editor/_fixtures/schema-1.orbdesign');
  const up = c && c.sounds && c.sounds.up && c.sounds.up.chime_hour;
  const cp = up && up.pcm;
  ok(!!c && c.schema === 1 && c.app === 'clock' && !!up && up.name === 'l0-chime.wav' && same(G.soundPcm(cp), SINE),
    'the clock fixture carries inline samples',
    c ? 'schema ' + c.schema + ', ' + (G.soundPcm(cp) ? G.soundPcm(cp).length + ' samples' : 'no sound') + ' on chime_hour' : 'unreadable');
  ok(!!c && c.sounds.slots.chime_hour === 'upload' && c.art === true && Array.isArray(c.assets) && !c.assets.length,
    'the clock fixture names its hour chime as the owner’s own upload', c && c.sounds.slots.chime_hour);

  const p = read('apps/pomodoro/editor/_fixtures/schema-1.orbdesign');
  const ups = (p && p.design && Array.isArray(p.design.uploads)) ? p.design.uploads : [];
  const hits = ups.filter((u) => u && u.name === 'l0-chime.wav' && u.pcm !== undefined);
  ok(!!p && p.schema === 1 && p.app === 'pomodoro' && hits.length === 1 && same(G.soundPcm(hits[0].pcm), SINE),
    'the pomodoro fixture carries inline samples',
    p ? 'schema ' + p.schema + ', ' + hits.length + ' upload named l0-chime.wav of '
      + (hits[0] && G.soundPcm(hits[0].pcm) ? G.soundPcm(hits[0].pcm).length : 0) + ' samples' : 'unreadable');
  ok(!!hits[0] && hits[0].samples === N && /^u[0-9]+$/.test(hits[0].ref || '') && Array.isArray(hits[0].pcm)
    && (p.design.chimes || {}).focus && p.design.chimes.focus.upload === ups.indexOf(hits[0]),
    'the pomodoro fixture records the upload the way its editor’s own handler does',
    hits[0] && Object.keys(hits[0]).join('/') + ' · chimes.focus ' + JSON.stringify(p.design.chimes.focus));
  // step 2 changes what a NEW file holds, not what an old design holds: these fixtures stay inline
  // samples, and the same sound through the kit now is one §3.1 file with the samples inside it
  ok(!!hits[0] && G.soundFile(G.soundPcm(hits[0].pcm)).url === G.soundFileUrl(SINE),
    'the same samples give the same file, whether they come from a fixture or from the studio');
}

console.log((fails ? fails + ' FAILED of ' : '') + checked + ' checks');
process.exit(fails ? 1 : 0);
