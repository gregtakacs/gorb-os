"""Prototype audition pack (docs/sound-plan.md §7.1): per-dial, cute and futuristic ticks and chimes,
built on sound_synth_proto.py. Retires with it when the JS port lands (S2).

Writes each sound as .wav (16-bit LE mono, 22,050 Hz) into <outdir>/<group>/; loops are written
three times over so the seam is heard twice:  python3 tools/sound_pack_proto.py <outdir>
"""
import sys, wave
from pathlib import Path
import numpy as np
import sound_synth_proto as S

SR = S.SR
rng = np.random.default_rng(11)
TAU = 2 * np.pi


def T(d):
    return np.arange(int(d * SR)) / SR


def note(n):
    return S.note(n)


def partials(f0, parts, dur, attack=0.002, detune=0.0):
    """parts: (ratio, amp, decay_s). detune > 0 doubles each partial into a beating pair."""
    t = T(dur)
    out = np.zeros_like(t)
    for r, a, dec in parts:
        f = f0 * r
        if f >= SR * 0.45:
            continue
        pairs = ((1.0, 1.0),) if not detune else ((1.0, 0.5), (1.0 + detune * r ** 0.3, 0.5))
        for d, g in pairs:
            out += g * a * np.sin(TAU * f * d * t + rng.uniform(0, TAU)) * np.exp(-t / dec)
    if attack > 0:
        out *= np.minimum(1.0, t / attack)
    return out


def click(dur=0.004, f=3000, q=2, gain=0.3):
    n = int(dur * SR)
    x = np.zeros(int(0.03 * SR))
    x[:n] = rng.standard_normal(n) * np.exp(-np.arange(n) / (n / 3))
    y = S.resonator(x, f, q)
    return gain * y / (np.max(np.abs(y)) + 1e-9)


def mix(*parts):
    n = max(len(p) for p in parts)
    out = np.zeros(n)
    for p in parts:
        out[:len(p)] += p
    return out


def seq(events, tail=1.0):
    end = max(s + len(sig) / SR for s, sig in events)
    return S.seq(events, end + tail)


def circ(events, loop_s=8.0):
    n = int(round(loop_s * SR))
    out = np.zeros(n)
    for s, sig in events:
        idx = (int(round(s * SR)) + np.arange(len(sig))) % n
        np.add.at(out, idx, sig)
    return out


def periodic(period, voice, jitter_ms=0.5, amp_db=0.8, loop_s=8.0):
    nb = int(round(loop_s / period))
    ph = rng.uniform(0, TAU)
    ev = []
    for b in range(nb):
        g = 10 ** ((amp_db * 0.5 * np.sin(TAU * 2 * b / nb + ph) + amp_db * 0.5 * rng.standard_normal()) / 20)
        ev.append((b * period + rng.normal(0, jitter_ms / 1000), g * voice(b, nb)))
    return circ(ev, loop_s)


def env_ar(dur, attack, release):
    t = T(dur)
    e = np.ones_like(t)
    a = int(attack * SR); r = int(release * SR)
    if a: e[:a] = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, a))
    if r: e[-r:] *= 0.5 + 0.5 * np.cos(np.linspace(0, np.pi, r))
    return e


def bandnoise(dur, f, q):
    x = rng.standard_normal(int(dur * SR))
    y = S.resonator(x, f, q)
    return y / (np.sqrt(np.mean(y ** 2)) + 1e-9)


def wobble(n, depth, rates=(3.1, 4.7, 6.3)):
    t = np.arange(n) / SR
    return 1 + depth * sum(np.sin(TAU * r * t + rng.uniform(0, TAU)) for r in rates) / len(rates)


def osc(fcurve, harm):
    ph = TAU * np.cumsum(fcurve) / SR
    f0 = float(np.max(fcurve))
    return sum(a * np.sin(k * ph) for k, a in harm if f0 * k < SR * 0.45)


# ---------------- instruments ----------------

def bar(f, kind="vib", dur=2.5):
    P = {"vib": [(1, 1, 1.6), (4.0, 0.35, 0.4), (10.0, 0.08, 0.12)],
         "marimba": [(1, 1, 0.45), (3.93, 0.4, 0.08), (9.2, 0.1, 0.03)],
         "glock": [(1, 1, 1.2), (2.76, 0.5, 0.3), (5.40, 0.25, 0.1)]}[kind]
    x = partials(f, P, dur)
    if kind == "vib":
        x *= 1 + 0.25 * np.sin(TAU * 5.5 * T(dur))
    return mix(x, click(f=f * 3, gain=0.15))


def tine(f, dur=1.8, parts=((1, 1, 1.4), (6.27, 0.3, 0.15), (17.55, 0.05, 0.04))):
    return mix(partials(f, parts, dur, attack=0.001), click(f=min(f * 4, 8000), gain=0.08))


def celesta(f, dur=1.6):
    return mix(partials(f, [(1, 1, 0.9), (2, 0.2, 0.4), (3, 0.05, 0.2)], dur, attack=0.002),
               click(f=f * 2, gain=0.1))


def tube(f, dur=4.0, g=1.0):
    return g * partials(f, [(1, 1, 3.0), (2.756, 0.6, 1.2), (5.404, 0.35, 0.45), (8.933, 0.15, 0.2)], dur)


def bowl(f, dur=7.0):
    return mix(partials(f, [(1, 1, 6), (2.71, 0.5, 3), (5.12, 0.25, 1.5), (8.0, 0.1, 0.8)], dur,
                        attack=0.004, detune=0.002), click(f=f * 2, gain=0.15))


def pipe(f, dur, attack=0.05, release=0.1, harm=((1, 1), (2, 0.25), (3, 0.1)), slide=0.0,
         slide_t=0.08, breath=0.1, vib=0.0, droop=0.0):
    t = T(dur)
    fc = f * (1 - slide * np.exp(-t / slide_t))
    if droop:
        fc *= 1 - droop * np.clip((t - (dur - release)) / max(release, 1e-3), 0, 1)
    if vib:
        fc *= wobble(len(t), vib)
    tone = osc(fc, harm)
    tone /= np.sqrt(np.mean(tone ** 2)) + 1e-9
    return (tone + breath * bandnoise(dur, f, 12)) * env_ar(dur, attack, release)


def brass(f, dur, attack=0.06, release=0.15):
    t = T(dur)
    fc = f * wobble(len(t), 0.002)
    harm = [(k, 1.0 / k) for k in range(1, 14)]
    x = osc(fc, harm) * (1 + 0.05 * np.sin(TAU * 31 * t))
    return x * env_ar(dur, attack, release)


def fm(fc, ratio, index, dur, idx_decay=0.6, amp_decay=1.2, attack=0.002):
    t = T(dur)
    I = index * np.exp(-t / idx_decay)
    x = np.sin(TAU * fc * t + I * np.sin(TAU * fc * ratio * t)) * np.exp(-t / amp_decay)
    return x * np.minimum(1, t / attack)


def sine_tone(f, dur, attack=0.004, release=0.004, harm=((1, 1),)):
    return osc(np.full(int(dur * SR), f), harm) * env_ar(dur, attack, release)


def square(f, dur, duty=0.5, attack=0.003, release=0.01):
    harm = [(k, np.sin(np.pi * k * duty) / k) for k in range(1, 40)]
    return osc(np.full(int(dur * SR), f), harm) * env_ar(dur, attack, release)


def saw_pluck(f, dur=0.5):
    t = T(dur)
    x = np.zeros_like(t)
    for k in range(1, 40):
        if f * k >= SR * 0.45:
            break
        bright = np.exp(-(k - 1) / (1 + 10 * np.exp(-t / 0.12)))
        x += np.sin(TAU * f * k * t) / k * bright
    return x * np.exp(-t / 0.25) * np.minimum(1, t / 0.003)


def echo(x, delay, gain, n=2):
    d = int(delay * SR)
    out = np.zeros(len(x) + d * n)
    for i in range(n + 1):
        out[i * d:i * d + len(x)] += x * gain ** i
    return out


def reverb(x, tail=1.2, mix_=0.25):
    ir = rng.standard_normal(int(tail * SR)) * np.exp(-T(tail) / (tail / 5))
    ir /= np.sqrt(np.sum(ir ** 2))
    n = len(x) + len(ir)
    wet = np.fft.irfft(np.fft.rfft(x, n) * np.fft.rfft(ir, n), n)
    return mix(x, mix_ * wet / (np.max(np.abs(wet)) + 1e-9) * np.max(np.abs(x)))


# ---------------- the pack ----------------

def station():
    s = {}
    relay = lambda b, nb: S.beat(((1400, 8, 1.0), (2600, 10, 0.6), (5200, 12, 0.2)), 10,
                                 ((0, 1.0, 0.8), (38, 0.35, 0.6)), 0.12, var=0.5)
    s["loop_station_relay_1hz"] = ("loop", periodic(1.0, relay, jitter_ms=0.4))
    rattle = seq([(0.008 * i + rng.uniform(0, 0.004), click(f=rng.uniform(1500, 3500), q=6, gain=0.4 * 0.7 ** i))
                  for i in range(7)], tail=0.05)
    clunk = S.tick(dur=0.25, modes=((700, 6, 1.0), (1300, 8, 0.6), (2900, 12, 0.3)), click_ms=3, decay_ms=45)
    s["station_minute_clunk"] = mix(clunk / np.max(np.abs(clunk)), rattle)
    s["station_dingdong"] = seq([(0, bar(note("D6"))), (0.6, bar(note("A5")))])
    s["station_dingdong_3tone"] = seq([(0, bar(note("A5"))), (0.45, bar(note("C#6"))), (0.9, bar(note("E6")))])
    def whistle(dur):
        hiss = bandnoise(dur, 5000, 1.5) * env_ar(dur, 0.1, 0.3) * 0.08
        chord = [pipe(f, dur, attack=0.12 + 0.03 * i, release=0.3, slide=0.06, slide_t=0.12,
                      breath=0.25, vib=0.003, droop=0.03) for i, f in enumerate((440.0, 523.25, 659.26))]
        return mix(*chord, hiss)
    s["steam_whistle"] = whistle(1.8)
    s["steam_whistle_long_long"] = seq([(0, whistle(1.4)), (1.9, whistle(1.6))], tail=0.4)
    s["diesel_horn"] = mix(*[brass(note(n), 1.6) for n in ("D#5", "F#5", "G#5", "B5", "D#6")])
    s["departure_handbell"] = seq([(0.17 * i, (1.0 if i % 2 else 0.8) * S.bell(note("C6") * (1.0 if i % 2 else 1.003),
                                                                               dur=1.5, brightness=1.2, ring=0.4))
                                   for i in range(10)], tail=1.0)
    return s


def regulator():
    s = {}
    reg = lambda b, nb: S.beat(((2400, 14, 1.0), (3900, 20, 0.5), (520, 6, 0.8)), 25, S.CLOCK_CLICKS, 0.15, var=0.6)
    s["loop_regulator_1hz"] = ("loop", periodic(1.0, reg, jitter_ms=0.3, amp_db=0.5))
    ting, tang, low = S.chime_rod(note("G5"), 2.5), S.chime_rod(note("E5"), 2.5), S.chime_rod(note("G4"), 3.5)
    def pairs(n, t0=0.0):
        return [(t0 + i * 1.1, ting) for i in range(n)] + [(t0 + i * 1.1 + 0.42, tang) for i in range(n)]
    s["regulator_tingtang_q2"] = seq(pairs(2), tail=2.0)
    s["regulator_hour_3"] = seq(pairs(4) + [(4 * 1.1 + 0.8 + i * 2.0, low) for i in range(3)], tail=3.0)
    return s


def perpetual():
    lo, hi = S.chime_rod(note("A5"), 1.5), S.chime_rod(note("E6"), 1.5)
    ev, t = [], 0.0
    for _ in range(10):
        ev.append((t, lo)); t += 0.38
    t += 0.8
    for _ in range(3):
        ev += [(t, hi), (t + 0.2, lo)]; t += 0.75
    t += 0.6
    for _ in range(2):
        ev.append((t, hi)); t += 0.4
    return {"minute_repeater_10h47": seq(ev, tail=1.5)}


def ships_bells():
    b = lambda: S.bell(note("D5") * (1 + 0.002 * rng.standard_normal()), dur=3.0, brightness=1.15, ring=0.7)
    def pattern(n):
        ev, t = [], 0.0
        for i in range(n):
            ev.append((t, b())); t += 0.35 if i % 2 == 0 else 1.1
        return seq(ev, tail=2.5)
    return {"ships_bell_single": b(), "ships_bells_3": pattern(3), "ships_bells_8": pattern(8)}


def cuckoo():
    def one():
        whoosh = bandnoise(0.12, 1200, 2) * env_ar(0.12, 0.04, 0.06) * 0.15
        cu = pipe(note("G5"), 0.28, attack=0.02, release=0.06, harm=((1, 1), (2, 0.12), (3, 0.04)), breath=0.15)
        koo = pipe(note("E5"), 0.45, attack=0.02, release=0.15, harm=((1, 1), (2, 0.12), (3, 0.04)), breath=0.15)
        return seq([(0, whoosh), (0.04, cu), (0.36, koo)], tail=0.2)
    return {"cuckoo_once": one(), "cuckoo_x3": seq([(1.1 * i, one()) for i in range(3)], tail=0.3)}


def windchimes(dur, rate, name):
    notes = [note(n) for n in ("C6", "D6", "E6", "G6", "A6")]
    ev, t = [], 0.0
    while t < dur:
        ev.append((t, tube(notes[rng.integers(len(notes))], g=rng.uniform(0.3, 1.0))))
        t += rng.exponential(1 / rate)
    return {name: seq(ev, tail=1.0)}


def phase_dial():
    def clink():
        return mix(partials(3200 * rng.uniform(0.97, 1.03),
                            [(1, 1, 0.25), (1.41, 0.6, 0.18), (1.93, 0.4, 0.12), (2.6, 0.2, 0.08)], 0.6, attack=0.0005),
                   click(f=6000, gain=0.2))
    def harmonica(f, dur):
        return pipe(f, dur, attack=0.3, release=0.5, harm=((1, 1), (2, 0.15)), breath=0.02, vib=0.003)
    return {"teacup_clink": seq([(0, clink()), (0.13, 0.7 * clink())], tail=0.3),
            "glass_harmonica": seq([(0.0, harmonica(note("A5"), 2.0)), (0.5, harmonica(note("C#6"), 1.8)),
                                    (1.0, harmonica(note("E6"), 1.6)), (1.5, harmonica(note("A6"), 1.8))], tail=0.2),
            "glass_bell": S.bell(note("A6"), dur=3.0, brightness=1.3, ring=0.6)}


def luna():
    return {"singing_bowl": bowl(note("C5")),
            "celesta_lullaby": seq([(0.42 * i, celesta(note(n))) for i, n in enumerate(("G5", "E6", "D6", "B5", "G5"))]),
            "small_bowl": bowl(note("G5"), dur=4.0)}


def neon():
    def buzz(f):
        t = T(0.035)
        x = sum(np.sin(TAU * f * k * t) / k for k in range(2, 30) if f * k < SR * 0.45)
        return x * np.exp(-t / 0.012) * np.minimum(1, t / 0.002)
    s = {"loop_neon_buzz_1hz": ("loop", periodic(1.0, lambda b, nb: buzz(240 if b % 2 == 0 else 200), jitter_ms=0.5))}
    arp = [(0.13 * i, saw_pluck(note(n))) for i, n in enumerate(("A4", "C5", "E5", "A5", "C6", "E6"))]
    s["synthwave_arp"] = echo(seq(arp, tail=0.2), 0.26, 0.35, 2)
    t = T(4.5)
    pad = sum(osc(np.full(len(t), note(n) * d), [(1, 1), (3, 1 / 9), (5, 1 / 25)])
              for n in ("C5", "E5", "G5", "B5") for d in (0.9977, 1.0023)) * env_ar(4.5, 1.5, 1.5)
    s["synth_pad_bell"] = mix(pad / np.max(np.abs(pad)) * 0.5, seq([(1.6, 0.5 * fm(note("E6"), 3.5, 4, 2.5))], tail=0))
    s["synth_bell"] = fm(note("A5"), 3.5, 5, 3.0)
    return s


def sector():
    wood = lambda b, nb: S.beat(tuple((f * (1.0 if b % 2 == 0 else 0.9), q, a)
                                      for f, q, a in ((1100, 30, 1.0), (2650, 25, 0.5), (4000, 20, 0.25))),
                                15, ((0, 1.0, 0.8),), 0.08, var=0.5)
    return {"loop_woodblock_1hz": ("loop", periodic(1.0, wood, jitter_ms=0.6)),
            "marimba_q2": seq([(0, bar(note("C5"), "marimba", 1.2)), (0.22, bar(note("E5"), "marimba", 1.2))]),
            "marimba_hour": seq([(0.22 * i, bar(note(n), "marimba", 1.2)) for i, n in enumerate(("C5", "E5", "G5", "C6"))])}


def modern():
    roll = [(0.06 * i, bar(note(n), "vib", 3.0)) for i, n in enumerate(("C5", "E5", "G5", "B5", "D6"))]
    return {"glass_dingdong": seq([(0, bar(note("B5"))), (0.6, bar(note("G5")))]),
            "vibraphone_chord": seq(roll, tail=0.5)}


def diver():
    t = T(1.2)
    ping = np.sin(TAU * 1500 * (1 - 0.01 * t) * t) * np.exp(-t / 0.35) * np.minimum(1, t / 0.003)
    return {"sonar_ping": reverb(echo(ping, 0.38, 0.35, 2), 1.5, 0.3)}


def aviator():
    tone = lambda n: partials(note(n), [(1, 1, 1.0), (2, 0.15, 0.4), (3, 0.05, 0.2)], 1.8, attack=0.005)
    return {"cabin_ding": tone("C6"),
            "cabin_hilo": seq([(0, tone("C6")), (0.55, tone("A5"))]),
            "cabin_3tone": seq([(0, tone("A5")), (0.45, tone("C#6")), (0.9, tone("E6"))])}


def opentour():
    beep = lambda f, d: square(f, d)
    blip = seq([(0, bandnoise(0.04, 2000, 2) * env_ar(0.04, 0.003, 0.02) * 0.3),
                (0.05, sine_tone(1800, 0.08)), (0.16, sine_tone(1400, 0.08))], tail=0.1)
    return {"pit_radio_blip": blip,
            "race_start": seq([(0, beep(880, 0.15)), (1, beep(880, 0.15)), (2, beep(880, 0.15)),
                               (3, beep(1760, 0.8))], tail=0.2)}


def orbital():
    n = ("C6", "E6", "G6", "B6", "D7")
    return {"orbit_arp": seq([(0.11 * i, celesta(note(x) * (1 + 0.006 * np.sin(np.pi * i / 4))))
                              for i, x in enumerate(n)], tail=0.3)}


def movements():
    classic = lambda b, nb: S.beat(((3000, 20, 1.0), (4800, 25, 0.5), (1900, 12, 0.5)), 8, S.POCKET_CLICKS, 0.05)
    def tourb(b, nb):
        sw = 1 + 0.03 * np.sin(TAU * b / nb)
        return (1 + 0.15 * np.sin(TAU * b / nb)) * S.beat(((3400 * sw, 22, 1.0), (5600 * sw, 28, 0.5),
                                                           (2200 * sw, 14, 0.4)), 7, S.POCKET_CLICKS, 0.05)
    return {"loop_classic_5hz": ("loop", periodic(0.2, classic, jitter_ms=0.3)),
            "loop_tourbillon_6hz": ("loop", periodic(1 / 6, tourb, jitter_ms=0.3))}


def cute():
    s = {}
    mel = ("E6", "G6", "A6", "G6", "E6", "D6", "C6", "D6", "E6")
    s["music_box"] = seq([(0.28 * i + rng.normal(0, 0.012), tine(note(n) * (1 + 0.002 * rng.standard_normal())))
                          for i, n in enumerate(mel)])
    s["kalimba"] = seq([(0.25 * i, tine(note(n), 1.2, ((1, 1, 0.9), (5.9, 0.25, 0.12))))
                        for i, n in enumerate(("C5", "E5", "G5", "E5", "A5", "G5"))])
    tw = ("G5", "G5", "D6", "D6", "E6", "E6", "D6")
    toy = lambda f: mix(partials(f, [(1, 1, 0.8), (2.76, 0.6, 0.25), (5.4, 0.4, 0.08)], 1.2), click(f=4000, gain=0.25))
    s["toy_piano_twinkle"] = seq([(0.3 * i + (0.3 if i == 6 else 0), toy(note(n))) for i, n in enumerate(tw)])
    s["glockenspiel"] = seq([(0.12 * i, bar(note(n), "glock", 2.0)) for i, n in enumerate(("C6", "E6", "G6", "C7"))])
    t = T(0.06)
    drop = lambda f0: np.sin(TAU * np.cumsum(f0 * (1 + 1.2 * (1 - np.exp(-t / 0.01)))) / SR) * np.exp(-t / 0.02) * np.minimum(1, t / 0.001)
    s["loop_waterdrop_1hz"] = ("loop", periodic(1.0, lambda b, nb: drop(900 * (1 + 0.08 * rng.standard_normal())), jitter_ms=3))
    hoo = lambda d: pipe(470, d, attack=0.08, release=0.2, harm=((1, 1), (2, 0.08)), breath=0.25, droop=0.04)
    s["owl_hoot"] = seq([(0, hoo(0.45)), (0.95, hoo(0.3)), (1.35, hoo(0.5))], tail=0.3)
    def chirp(f1, f2, d):
        tt = T(d)
        f = f1 * (f2 / f1) ** (tt / d)
        return np.sin(TAU * np.cumsum(f) / SR) * np.sin(np.pi * tt / d)
    ev = [(0.25 * i, chirp(5500 * rng.uniform(0.9, 1.1), 3000, 0.06)) for i in range(4)]
    ev += [(1.1 + 0.05 * i, 0.6 * chirp(4500, 3800, 0.035)) for i in range(6)]
    s["dawn_chirp"] = seq(ev, tail=0.2)
    s["temple_bell"] = S.bell(note("A4"), dur=6.0, brightness=0.7, ring=1.5)
    s["tingsha"] = mix(bowl(2900, 5.0), bowl(2900 * 1.004, 5.0))
    return s


def futuristic():
    s = {}
    hann = lambda f, d: np.sin(TAU * f * T(d)) * np.sin(np.pi * T(d) / d) ** 2
    s["loop_sine_blip_1hz"] = ("loop", periodic(1.0, lambda b, nb: hann((1600 if b % 2 == 0 else 1200) * (1 + 0.005 * rng.standard_normal()), 0.006), jitter_ms=0.2))
    def glitch(b, nb):
        n = int(0.01 * SR)
        x = rng.standard_normal(n)
        x = np.repeat(x[::3], 3)[:n]
        x = np.round(x * 2) / 2 * np.exp(-np.arange(n) / (n / 3))
        return S.resonator(np.concatenate([x, np.zeros(n)]), rng.uniform(1500, 4000), 4)
    s["loop_glitch_1hz"] = ("loop", periodic(1.0, glitch, jitter_ms=1.0, amp_db=2.0))
    k = rng.poisson(20)
    s["loop_geiger"] = ("loop", circ([(rng.uniform(0, 8), rng.uniform(0.4, 1.0) * click(0.001, 2800, 1.5, 1.0)) for _ in range(k)]))
    def heart(b, nb):
        t = T(0.2)
        lub = np.sin(TAU * 420 * (1 - 0.05 * t) * t) * np.exp(-t / 0.045)
        dub = 0.6 * np.sin(TAU * 540 * t) * np.exp(-t / 0.035)
        return seq([(0, lub), (0.28, dub)], tail=0.0)
    s["loop_heartbeat_1hz"] = ("loop", periodic(1.0, heart, jitter_ms=8))
    s["loop_double_pip_1hz"] = ("loop", periodic(1.0, lambda b, nb: seq([(0, hann(2200 if b % 2 == 0 else 1900, 0.012)),
                                                                        (0.03, 0.7 * hann(2200 if b % 2 == 0 else 1900, 0.012))], tail=0), jitter_ms=0.2))
    def patter(b, nb):
        return seq([(rng.uniform(0, 0.07), rng.uniform(0.3, 1) * hann(rng.uniform(2000, 6000), 0.008))
                    for _ in range(rng.integers(3, 6))], tail=0.01)
    s["loop_data_patter_4hz"] = ("loop", periodic(0.25, patter, jitter_ms=0.5, amp_db=1.5))
    s["fm_glass_bell"] = fm(note("A5"), 3.5, 5, 3.0)
    pip = lambda d: sine_tone(1000, d, 0.005, 0.005)
    s["time_pips"] = seq([(i, pip(0.1)) for i in range(5)] + [(5, pip(0.5))], tail=0.2)
    def morse(code, f=750, dot=0.06):
        ev, t = [], 0.0
        for ch in code:
            if ch == " ":
                t += dot * 2; continue
            d = dot if ch == "." else dot * 3
            ev.append((t, sine_tone(f, d, 0.004, 0.004))); t += d + dot
        return seq(ev, tail=0.2)
    s["morse_hour_3"] = morse("...--")
    s["morse_hour_12"] = morse(".---- ..---")
    arp = [(0.07 * i, fm(note(n), 2.0, 1.5, 0.35, 0.1, 0.12)) for i, n in enumerate(("C5", "G5", "C6", "E6", "G6", "C7"))]
    s["boot_arpeggio"] = echo(seq(arp, tail=0.2), 0.21, 0.4, 2)
    d = 4.5; t = T(d)
    out = np.zeros_like(t)
    for kk in range(-2, 8):
        f = 120 * 2 ** kk * 2 ** (0.25 * t)
        w = np.exp(-0.5 * ((np.log2(f) - np.log2(1400)) / 1.0) ** 2)
        out += w * np.sin(TAU * np.cumsum(f) / SR) * (f < SR * 0.45)
    s["shepard_swell"] = out * env_ar(d, 0.8, 1.0)
    d = 3.5; n = int(d * SR)
    sh = sum(np.sin(TAU * np.cumsum(1800 * 2 ** (u / 12) * wobble(n, 0.002)) / SR) * wobble(n, 0.4, (0.7, 1.3, 2.1))
             for u in (0, 4, 7, 11, 12, 14, 16, 19))
    s["hologram_shimmer"] = sh * env_ar(d, 1.0, 1.8)
    nn = int(0.6 * SR)
    x = rng.standard_normal(nn); y = np.zeros(nn); y1 = y2 = 0.0
    for i in range(nn):
        f = 600 * (4000 / 600) ** (i / nn); w = TAU * f / SR; al = np.sin(w) / 6
        a0 = 1 + al
        o = (al * x[i] - (-2 * np.cos(w)) * y1 - (1 - al) * y2) / a0
        y2, y1 = y1, o; y[i] = o
    y *= env_ar(0.6, 0.25, 0.3)
    s["door_whoosh_chime"] = mix(y / np.max(np.abs(y)) * 0.6, seq([(0.45, fm(note("E6"), 3.5, 3, 2.0))], tail=0))
    notes = (("C5", .1), ("E5", .1), ("G5", .1), ("C6", .3), ("G5", .1), ("C6", .6))
    ev, tt = [], 0.0
    for nm, dd in notes:
        ev.append((tt, square(note(nm), dd, duty=0.25))); tt += dd + 0.01
    s["chiptune_fanfare"] = seq(ev, tail=0.2)
    return s


def main(outdir):
    out = Path(outdir)
    groups = {"station": station(), "regulator": regulator(), "perpetual": perpetual(),
              "diver": {**diver(), **ships_bells()}, "cuckoo": cuckoo(),
              "kitetour": {**windchimes(2.5, 4, "windchimes_flurry"), **windchimes(5.0, 7, "windchimes_flourish")},
              "phase": phase_dial(), "luna": luna(), "neon": neon(), "sector": sector(), "modern": modern(),
              "aviator": aviator(), "opentour": opentour(), "orbital": orbital(), "movements": movements(),
              "cute": cute(), "futuristic": futuristic()}
    for g, sounds in groups.items():
        d = out / g
        d.mkdir(parents=True, exist_ok=True)
        for name, v in sounds.items():
            if isinstance(v, tuple):
                pcm = S.finish_loop(v[1])
                # audition copy: the loop three times over, so the seam is heard twice
                pcm_play = np.tile(pcm, 3)
                seam = abs(int(pcm[0]) - int(pcm[-1]))
                tag = " loop x3, seam %d LSB" % seam
            else:
                pcm = pcm_play = S.finish(v)
                tag = ""
            with wave.open(str(d / f"{name}.wav"), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
                w.writeframes(pcm_play.tobytes())
            print(f"{g:10s} {name:26s} {len(pcm)/SR:6.2f}s {pcm.nbytes:8d} B{tag}")


if __name__ == "__main__":
    main(sys.argv[1])
