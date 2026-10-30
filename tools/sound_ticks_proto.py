"""Prototype rate grid (docs/sound-plan.md §6.1): every tick voice as a seamless 8 s loop at
each rate a second hand can step (1, 2, 4, 8 per second). 8 s holds a whole number of beats at all four, so one loop length
serves the grid. A voice is (b, nb, period) -> one beat; fit() cuts any tail that would run
into the next beat. Retires with sound_synth_proto.py at S2:
    python3 tools/sound_ticks_proto.py <outdir>
"""
import sys, wave
from pathlib import Path
import numpy as np
import sound_synth_proto as S
import sound_pack_proto as P

SR = S.SR
TAU = 2 * np.pi
rng = np.random.default_rng(23)
P.rng = rng
S.rng = rng
RATES = (1, 2, 4, 8)


def fit(sig, period):
    """Keep a beat inside 85% of its period, cosine-faded over the last 20% it keeps."""
    keep = int(0.85 * period * SR)
    if len(sig) <= keep:
        return sig
    sig = sig[:keep].copy()
    f = max(8, keep // 5)
    sig[-f:] *= 0.5 * (1 + np.cos(np.pi * np.arange(f) / f))
    return sig


def pair(a, b):
    """Tick/tock: even beats voice a, odd beats voice b."""
    return lambda i, nb, p: (a if i % 2 == 0 else b)()


def voices():
    B = S.beat
    v = {}
    v["mantel"] = (pair(lambda: B(((3200, 18, 1.0), (5100, 25, 0.6), (1800, 12, 0.5)), 12, S.CLOCK_CLICKS, 0.08),
                        lambda: B(((2600, 16, 1.0), (4300, 22, 0.5), (1500, 10, 0.6)), 12, S.CLOCK_CLICKS, 0.08)), 6, 1.0, 1.0)
    v["grandfather"] = (pair(lambda: B(((1900, 20, 1.0), (3100, 25, 0.5), (900, 10, 0.5)), 30, S.CLOCK_CLICKS, 0.15, 380, 0.25),
                             lambda: B(((1600, 20, 1.0), (2700, 25, 0.5), (800, 10, 0.5)), 30, S.CLOCK_CLICKS, 0.15, 340, 0.3)), 8, 1.5, 1.0)
    soft = lambda: B(((2200, 8, 1.0), (1200, 6, 0.6)), 8, S.CLOCK_CLICKS, 0.06)
    v["soft"] = (pair(soft, soft), 0, 1.0, 1.5)
    v["pocket"] = (pair(lambda: B(((4200, 25, 1.0), (6800, 30, 0.5), (2600, 15, 0.4)), 6, S.POCKET_CLICKS, 0.04),
                        lambda: B(((4000, 25, 1.0), (6500, 30, 0.5), (2500, 15, 0.4)), 6, S.POCKET_CLICKS, 0.04)), 1.5, 0.3, 1.0)
    v["wrist"] = (pair(lambda: B(((5500, 30, 1.0), (8200, 30, 0.5), (3500, 20, 0.3)), 4, S.WRIST_CLICKS, 0.03, var=0.7),
                       lambda: B(((5300, 30, 1.0), (7900, 30, 0.5), (3400, 20, 0.3)), 4, S.WRIST_CLICKS, 0.03, var=0.7)), 0.6, 0.15, 0.8)
    classic = lambda: B(((3000, 20, 1.0), (4800, 25, 0.5), (1900, 12, 0.5)), 8, S.POCKET_CLICKS, 0.05)
    v["classic"] = (pair(classic, classic), 1.0, 0.3, 0.8)

    def tourb(i, nb, p):
        sw = 1 + 0.03 * np.sin(TAU * i / nb)
        return (1 + 0.15 * np.sin(TAU * i / nb)) * B(((3400 * sw, 22, 1.0), (5600 * sw, 28, 0.5), (2200 * sw, 14, 0.4)),
                                                    7, S.POCKET_CLICKS, 0.05)
    v["tourbillon"] = (tourb, 0, 0.3, 0.8)
    relay = lambda: B(((1400, 8, 1.0), (2600, 10, 0.6), (5200, 12, 0.2)), 10, ((0, 1.0, 0.8), (38, 0.35, 0.6)), 0.12, var=0.5)
    v["station_relay"] = (pair(relay, relay), 0, 0.4, 0.8)
    reg = lambda: B(((2400, 14, 1.0), (3900, 20, 0.5), (520, 6, 0.8)), 25, S.CLOCK_CLICKS, 0.15, var=0.6)
    v["regulator"] = (pair(reg, reg), 0, 0.3, 0.5)
    wood = lambda k: lambda: B(tuple((f * k, q, a) for f, q, a in ((1100, 30, 1.0), (2650, 25, 0.5), (4000, 20, 0.25))),
                               15, ((0, 1.0, 0.8),), 0.08, var=0.5)
    v["woodblock"] = (pair(wood(1.0), wood(0.9)), 0, 0.6, 0.8)

    def buzz(f):
        t = P.T(0.035)
        x = sum(np.sin(TAU * f * k * t) / k for k in range(2, 30) if f * k < SR * 0.45)
        return x * np.exp(-t / 0.012) * np.minimum(1, t / 0.002)
    v["neon_buzz"] = (pair(lambda: buzz(240), lambda: buzz(200)), 0, 0.5, 0.8)
    t6 = P.T(0.06)
    drop = lambda f0: np.sin(TAU * np.cumsum(f0 * (1 + 1.2 * (1 - np.exp(-t6 / 0.01)))) / SR) * np.exp(-t6 / 0.02) * np.minimum(1, t6 / 0.001)
    v["waterdrop"] = (lambda i, nb, p: drop(900 * (1 + 0.08 * rng.standard_normal())), 0, 3.0, 0.8)
    hann = lambda f, d: np.sin(TAU * f * P.T(d)) * np.sin(np.pi * P.T(d) / d) ** 2
    v["sine_blip"] = (lambda i, nb, p: hann((1600 if i % 2 == 0 else 1200) * (1 + 0.005 * rng.standard_normal()), 0.006), 0, 0.2, 0.8)

    def glitch(i, nb, p):
        n = int(0.01 * SR)
        x = rng.standard_normal(n)
        x = np.repeat(x[::3], 3)[:n]
        x = np.round(x * 2) / 2 * np.exp(-np.arange(n) / (n / 3))
        return S.resonator(np.concatenate([x, np.zeros(n)]), rng.uniform(1500, 4000), 4)
    v["glitch"] = (glitch, 0, 1.0, 2.0)

    def dpip(i, nb, p):
        f = 2200 if i % 2 == 0 else 1900
        return P.seq([(0, hann(f, 0.012)), (0.03, 0.7 * hann(f, 0.012))], tail=0)
    v["double_pip"] = (dpip, 0, 0.2, 0.8)

    def patter(i, nb, p):
        span = min(0.07, 0.5 * p)
        return P.seq([(rng.uniform(0, span), rng.uniform(0.3, 1) * hann(rng.uniform(2000, 6000), 0.008))
                      for _ in range(rng.integers(3, 6))], tail=0.01)
    v["data_patter"] = (patter, 0, 0.5, 1.5)
    return v


def loop(voice, rate, beat_error_ms, jitter_ms, amp_db, loop_s=8.0):
    period = 1.0 / rate
    nb = int(round(loop_s * rate))
    n = int(round(loop_s * SR))
    out = np.zeros(n)
    ph = rng.uniform(0, TAU)
    for i in range(nb):
        g = 10 ** ((amp_db * 0.5 * np.sin(TAU * 2 * i / nb + ph) + amp_db * 0.5 * rng.standard_normal()) / 20)
        start = i * period + (beat_error_ms / 1000 if i % 2 else 0) + rng.normal(0, jitter_ms / 1000)
        sig = g * fit(voice(i, nb, period), period)
        idx = (int(round(start * SR)) + np.arange(len(sig))) % n
        np.add.at(out, idx, sig)
    return out


def seam_ok(pcm):
    x = pcm.astype(float)
    d2 = np.abs(np.roll(x, -1) - 2 * x + np.roll(x, 1))
    return max(d2[0], d2[-1]) <= np.percentile(d2, 99.99)


def main(outdir):
    out = Path(outdir)
    bad = 0
    for name, (voice, be, jit, amp) in voices().items():
        d = out / name
        d.mkdir(parents=True, exist_ok=True)
        for r in RATES:
            # beat error is a fraction of the period on a real escapement; scale it with the rate
            pcm = S.finish_loop(loop(voice, r, be / r, jit, amp))
            ok = seam_ok(pcm)
            bad += not ok
            with wave.open(str(d / f"{name}_{r}hz.wav"), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
                w.writeframes(np.tile(pcm, 3).tobytes())
            print(f"{name:14s} {r}/s  {pcm.nbytes} B  seam {'ok' if ok else 'BAD'}")
    print("seams bad:", bad)


if __name__ == "__main__":
    main(sys.argv[1])
