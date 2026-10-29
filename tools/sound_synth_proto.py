"""Prototype clock-sound generator (docs/sound-plan.md §6). Retires when the JS port lands (S2).

Writes ticks, tick loops, bells and Westminster chimes as 16-bit LE mono PCM at 22,050 Hz
(.pcm, headerless) plus .wav for audition:  python3 tools/sound_synth_proto.py <outdir>
"""
import sys, wave
from pathlib import Path
import numpy as np

SR = 22050          # ES8311 rate the Waveshare BSP defaults to
SPEAKER_HP = 350    # 2 W micro speaker on the NS4150B: little output below ~300 Hz
rng = np.random.default_rng(7)


def t(dur):
    return np.arange(int(dur * SR)) / SR


def resonator(x, freq, q):
    """Two-pole resonant bandpass (biquad), run sample by sample; sounds are short."""
    w = 2 * np.pi * freq / SR
    alpha = np.sin(w) / (2 * q)
    b0, b2 = alpha, -alpha
    a0, a1, a2 = 1 + alpha, -2 * np.cos(w), 1 - alpha
    b0, b2, a1, a2 = b0 / a0, b2 / a0, a1 / a0, a2 / a0
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b0 * v + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def tick(dur=0.06, modes=((3200, 18, 1.0), (5100, 25, 0.6), (1800, 12, 0.5)),
         click_ms=1.5, thump_hz=0, thump_gain=0.0, decay_ms=12):
    """Escapement tick: a sharp noise impulse ringing a few metal modes, plus optional case thump."""
    n = int(dur * SR)
    exc = np.zeros(n)
    k = int(click_ms / 1000 * SR)
    exc[:k] = rng.standard_normal(k) * np.exp(-np.arange(k) / (k / 3))
    out = sum(g * resonator(exc, f, q) for f, q, g in modes)
    out *= np.exp(-t(dur) / (decay_ms / 1000))
    if thump_hz:
        tt = t(dur)
        out += thump_gain * np.sin(2 * np.pi * thump_hz * tt) * np.exp(-tt / 0.008)
    return out


# Bell partials (ratio to nominal-ish strike tone, relative amp, decay seconds at 1x)
BELL = [(0.5, 0.35, 1.0), (1.0, 1.0, 0.55), (1.19, 0.45, 0.4), (1.5, 0.3, 0.3),
        (2.0, 0.5, 0.22), (2.51, 0.2, 0.15), (2.66, 0.15, 0.13), (3.01, 0.12, 0.1),
        (4.07, 0.08, 0.07)]


def bell(f0, dur=3.0, brightness=1.0, ring=1.0):
    tt = t(dur)
    out = np.zeros_like(tt)
    for ratio, amp, dec in BELL:
        f = f0 * ratio
        if f > SR * 0.45:
            continue
        a = amp * (brightness ** (ratio - 1))
        d = dec * ring * 6.0 * (440 / f0) ** 0.5
        # pairs of slightly detuned partials give the slow "warble" of a real bell
        for det in (1.0, 1.0 + 0.0015 * ratio):
            out += 0.5 * a * np.sin(2 * np.pi * f * det * tt + rng.uniform(0, 6.28)) * np.exp(-tt / d)
    strike = rng.standard_normal(len(tt)) * np.exp(-tt / 0.002) * 0.15
    return out + resonator(strike, f0 * 3, 4)


def chime_rod(f0, dur=2.5):
    """Mantel-clock gong rod: clamped-free bar partials (1, 6.27, 17.55)."""
    tt = t(dur)
    out = sum(a * np.sin(2 * np.pi * f0 * r * tt) * np.exp(-tt / d)
              for r, a, d in ((1, 1.0, 1.2), (2.76, 0.35, 0.5), (5.40, 0.15, 0.2), (8.93, 0.06, 0.08))
              if f0 * r < SR * 0.45)
    return out * (1 - np.exp(-tt / 0.001))


def seq(events, dur):
    """events: (start_seconds, signal)"""
    out = np.zeros(int(dur * SR))
    for s, sig in events:
        i = int(s * SR)
        m = min(len(sig), len(out) - i)
        out[i:i + m] += sig[:m]
    return out


def note(name):
    names = {'C': -9, 'D': -7, 'E': -5, 'F': -4, 'G': -2, 'A': 0, 'B': 2}
    semi = names[name[0]] + (1 if '#' in name else -1 if 'b' in name[1:-1] else 0)
    return 440 * 2 ** ((semi + 12 * (int(name[-1]) - 4)) / 12)


def highpass(x, fc=SPEAKER_HP, order=2):
    """Zero-phase Butterworth-magnitude high-pass in the frequency domain (offline, so FFT is fine)."""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    f[0] = 1e-6
    return np.fft.irfft(X / np.sqrt(1 + (fc / f) ** (2 * order)), len(x))


def finish(x, peak_db=-1.0, fade_ms=20):
    x = highpass(x - np.mean(x))
    # trim trailing silence below -60 dB
    thr = np.max(np.abs(x)) * 1e-3
    last = np.nonzero(np.abs(x) > thr)[0][-1] + 1
    x = x[:last]
    f = min(len(x), int(fade_ms / 1000 * SR))
    x[-f:] *= np.linspace(1, 0, f)
    x *= 10 ** (peak_db / 20) / np.max(np.abs(x))
    return np.round(x * 32767).astype('<i2')


def write(outdir, name, pcm):
    (outdir / f"{name}.pcm").write_bytes(pcm.tobytes())
    with wave.open(str(outdir / f"{name}.wav"), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"{name:24s} {len(pcm)/SR:6.3f}s {len(pcm)*2:7d} bytes")


def beat(modes, decay_ms, clicks, dur, thump_hz=0, thump_gain=0.0, var=1.0):
    """One escapement beat: unlock / impulse / drop sub-clicks, each with jittered resonances.
    clicks: (offset_ms, gain, click_ms). var scales all the randomness."""
    out = np.zeros(int(dur * SR))
    for off, g, cms in clicks:
        off = max(0.0, off * (1 + 0.15 * var * rng.standard_normal()))
        m = tuple((f * (1 + 0.02 * var * rng.standard_normal()),
                   q * (1 + 0.1 * var * rng.standard_normal()),
                   a * 10 ** (2 * var * rng.standard_normal() / 20))
                  for f, q, a in modes)
        th = thump_gain if g == max(c[1] for c in clicks) else 0.0
        c = tick(dur=dur, modes=m, click_ms=cms, decay_ms=decay_ms)
        # a few samples of noise into high-Q modes swings ~±6 dB on its own; pin the energy so
        # loudness variance comes only from tick_loop's amp_db, and timbre carries the variety
        c /= np.sqrt(np.mean(c ** 2))
        if th:
            tt = t(dur)
            c += th * 3 * np.sin(2 * np.pi * thump_hz * tt) * np.exp(-tt / 0.008)
        i = int(off / 1000 * SR)
        out[i:] += g * c[:len(out) - i]
    return out


def tick_loop(period, voices, beat_error_ms=0.0, jitter_ms=0.5, amp_db=1.0, loop_s=8.0):
    """Seamless loop: beats every `period` s cycling through `voices` (tick, tock, ...).
    Every beat is generated fresh, its tail wraps circularly past the end, and the slow
    amplitude wander completes whole cycles, so sample N-1 runs straight into sample 0."""
    n = int(round(loop_s * SR))
    nbeats = int(round(loop_s / period))
    assert abs(nbeats * period - loop_s) < 1e-9 and nbeats % len(voices) == 0
    out = np.zeros(n)
    phase = rng.uniform(0, 2 * np.pi)
    for b in range(nbeats):
        v = voices[b % len(voices)]
        start = b * period + (beat_error_ms / 1000 if b % 2 else 0) + rng.normal(0, jitter_ms / 1000)
        wander = np.sin(2 * np.pi * 2 * b / nbeats + phase)  # two full cycles per loop
        gain = 10 ** ((amp_db * 0.5 * wander + amp_db * 0.5 * rng.standard_normal()) / 20)
        sig = gain * v()
        idx = (int(round(start * SR)) + np.arange(len(sig))) % n
        np.add.at(out, idx, sig)
    return out


def finish_loop(x, peak_db=-1.0):
    """finish() without trim/fade; the FFT high-pass is circular, so the loop stays seamless."""
    x = highpass(x - np.mean(x))
    x *= 10 ** (peak_db / 20) / np.max(np.abs(x))
    return np.round(x * 32767).astype('<i2')


# (offset_ms, gain, click_ms) for unlock, impulse, drop
CLOCK_CLICKS = ((0, 0.35, 1.0), (3.5, 0.6, 1.5), (9.0, 1.0, 1.5))
POCKET_CLICKS = ((0, 0.4, 0.6), (1.8, 0.7, 0.6), (4.5, 1.0, 0.8))
WRIST_CLICKS = ((0, 0.4, 0.4), (1.0, 0.7, 0.4), (2.6, 1.0, 0.5))


def loops():
    mantel_tick = lambda: beat(((3200, 18, 1.0), (5100, 25, 0.6), (1800, 12, 0.5)), 12, CLOCK_CLICKS, 0.08)
    mantel_tock = lambda: beat(((2600, 16, 1.0), (4300, 22, 0.5), (1500, 10, 0.6)), 12, CLOCK_CLICKS, 0.08)
    gf_tick = lambda: beat(((1900, 20, 1.0), (3100, 25, 0.5), (900, 10, 0.5)), 30, CLOCK_CLICKS, 0.15,
                           thump_hz=380, thump_gain=0.25)
    gf_tock = lambda: beat(((1600, 20, 1.0), (2700, 25, 0.5), (800, 10, 0.5)), 30, CLOCK_CLICKS, 0.15,
                           thump_hz=340, thump_gain=0.3)
    soft = lambda: beat(((2200, 8, 1.0), (1200, 6, 0.6)), 8, CLOCK_CLICKS, 0.06)
    pocket_a = lambda: beat(((4200, 25, 1.0), (6800, 30, 0.5), (2600, 15, 0.4)), 6, POCKET_CLICKS, 0.04)
    pocket_b = lambda: beat(((4000, 25, 1.0), (6500, 30, 0.5), (2500, 15, 0.4)), 6, POCKET_CLICKS, 0.04)
    wrist_a = lambda: beat(((5500, 30, 1.0), (8200, 30, 0.5), (3500, 20, 0.3)), 4, WRIST_CLICKS, 0.03, var=0.7)
    wrist_b = lambda: beat(((5300, 30, 1.0), (7900, 30, 0.5), (3400, 20, 0.3)), 4, WRIST_CLICKS, 0.03, var=0.7)
    return {
        "loop_mantel_1hz": tick_loop(1.0, (mantel_tick, mantel_tock), beat_error_ms=6, jitter_ms=1.0),
        "loop_grandfather_1hz": tick_loop(1.0, (gf_tick, gf_tock), beat_error_ms=8, jitter_ms=1.5),
        "loop_soft_1hz": tick_loop(1.0, (soft, soft), jitter_ms=1.0, amp_db=1.5),
        # 14400 bph: classic pocket watch
        "loop_pocketwatch_4hz": tick_loop(0.25, (pocket_a, pocket_b), beat_error_ms=1.5, jitter_ms=0.3),
        # 28800 bph: modern high-beat movement
        "loop_watch_8hz": tick_loop(0.125, (wrist_a, wrist_b), beat_error_ms=0.6, jitter_ms=0.15, amp_db=0.8),
    }


def main(outdir):
    outdir = Path(outdir); outdir.mkdir(parents=True, exist_ok=True)
    s = {}
    # --- ticks ---
    s["tick_mantel"] = tick()
    s["tock_mantel"] = tick(modes=((2600, 16, 1.0), (4300, 22, 0.5), (1500, 10, 0.6)))
    s["tick_soft"] = tick(modes=((2200, 8, 1.0), (1200, 6, 0.6)), click_ms=2.5, decay_ms=8)
    s["tick_grandfather"] = tick(dur=0.12, modes=((1900, 20, 1.0), (3100, 25, 0.5), (900, 10, 0.5)),
                                 thump_hz=380, thump_gain=0.25, decay_ms=30)
    s["tock_grandfather"] = tick(dur=0.12, modes=((1600, 20, 1.0), (2700, 25, 0.5), (800, 10, 0.5)),
                                 thump_hz=340, thump_gain=0.3, decay_ms=30)
    s["tick_watch"] = tick(dur=0.03, modes=((6000, 30, 1.0), (8200, 30, 0.5)), click_ms=0.6, decay_ms=4)
    # --- single strikes ---
    s["bell_hour"] = bell(note("E4"), dur=5.0)               # one hour stroke; play N times
    s["bell_small"] = bell(note("E5"), dur=2.5, brightness=1.2)
    s["rod_hour"] = chime_rod(note("G4"), dur=3.0)
    s["ding"] = bell(note("A5"), dur=1.5, brightness=0.8, ring=0.6)
    # --- Westminster quarters (1793 tune, public domain) ---
    ch = {n: bell(note(n), dur=3.5, brightness=0.9) for n in ("G#5", "F#5", "E5", "B4")}
    gap = 0.62
    phrases = {1: "G#5 F#5 E5 B4", 2: "E5 G#5 F#5 B4", 3: "E5 F#5 G#5 E5",
               4: "G#5 E5 F#5 B4", 5: "B4 F#5 G#5 E5"}
    def changes(ids):
        ev, pos = [], 0.0
        for k in ids:
            for nm in phrases[k].split():
                ev.append((pos, ch[nm])); pos += gap
            pos += gap  # rest between changes
        return seq(ev, pos + 3.5)
    s["westminster_q1"] = changes([1])
    s["westminster_q2"] = changes([2, 3])
    s["westminster_q3"] = changes([4, 5, 1])
    s["westminster_q4"] = changes([2, 3, 4, 5])
    for k, v in s.items():
        write(outdir, k, finish(v))
    for k, v in loops().items():
        write(outdir, k, finish_loop(v))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out")
