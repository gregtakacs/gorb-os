#!/usr/bin/env python3
"""D39: put sounds into a clock theme (docs/history/sound-plan.md §3).

A clock sound is a SOUND element row (theme_schema.CLASS_SOUND) naming one catalogue
row whose plane is bare 16-bit mono PCM at 22,050 Hz: w samples, h 1, two bytes each.
The row's own columns carry its facts (theme_schema.check_clock_sounds), so the
device can refuse a bad one at the door before the bank is resident.

Planes go at the HEAD of the catalogue. The reader refuses a sound at an odd offset
(a 16-bit load from the mapped bank would fault), and pixel planes priced w*h*3 can
leave the bank's tail odd; at the head every sound sits at an even offset because
every sound before it is an even number of bytes. Inserting there shifts every later
plane by the same even amount, which keeps the two scalars that hold bank offsets
(plate_off, bg_key_off) even and true.

    python3 tools/orbsound.py add IN.orbtheme OUT.orbtheme \\
        --sound tick_loop=tick.wav --beat 100 \\
        --sound chime_hour=dingdong.wav --sound jump_minute=clunk.wav --pattern none
    python3 tools/orbsound.py list IN.orbtheme
    python3 tools/orbsound.py strip IN.orbtheme OUT.orbtheme

WAVs must already be 22,050 Hz mono 16-bit: the composer's Sound tab resamples, and
this tool refuses rather than guess.
"""
import argparse
import os
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbbundle as ob          # noqa: E402
import theme_schema as ts       # noqa: E402

OFF_SCALARS = ("plate_off", "bg_key_off")   # the clock scalars that are bank offsets


def _is_sound(e):
    return int(e["class"]) == ts.CLASS_SOUND


def _shift(sec, at, delta, frames_from, frames_delta):
    """Move every plane at or past byte `at` by `delta` bytes, every element frame
    index >= frames_from by frames_delta, and the offset scalars with the planes."""
    for sp in sec["sprites"]:
        if int(sp["off"]) >= at:
            sp["off"] = int(sp["off"]) + delta
    for e in sec["elements"]:
        if int(e["frame0"]) >= frames_from:
            e["frame0"] = int(e["frame0"]) + frames_delta
    for k in OFF_SCALARS:
        if k in sec["scalars"] and int(sec["scalars"][k]) >= at:
            sec["scalars"][k] = int(sec["scalars"][k]) + delta


def strip_sounds(sec):
    """Remove every SOUND row and its plane, closing the gaps."""
    dead = sorted({int(e["frame0"]) for e in sec["elements"] if _is_sound(e)}, reverse=True)
    sec["elements"][:] = [e for e in sec["elements"] if not _is_sound(e)]   # in place: callers alias it
    for i in dead:
        sp = sec["sprites"][i]
        off, size = int(sp["off"]), int(sp["size"])
        del sec["art"][off:off + size]
        del sec["sprites"][i]
        _shift(sec, off + size, -size, i + 1, -1)


def add_sounds(sec, sounds, pattern="none", check=True):
    """sounds: list of dicts {slot, pcm (bytes, int16 LE), beat_chz=0, first_beat=0}.
    Replaces any sound the section already carries. `sec` is a mutable section dict:
    elements (list of row dicts), scalars (dict), sprites (list of dicts), art
    (bytearray). check=False is the suite's door for forging a table the device must
    refuse; nothing that writes a theme passes it."""
    strip_sounds(sec)
    sounds = sorted(sounds, key=lambda s: ts.CLOCK_SOUND_SLOTS.index(s["slot"])
                    if isinstance(s["slot"], str) else s["slot"])
    head = bytearray()
    rows = []
    v4 = bool(sec["sprites"]) and "diffx" in sec["sprites"][0]
    for k, s in enumerate(sounds):
        pcm = bytes(s["pcm"])
        if len(pcm) % 2 or len(pcm) < 4:
            raise ValueError("sound %r: %d bytes is not a run of 16-bit samples"
                             % (s["slot"], len(pcm)))
        n = len(pcm) // 2
        sp = {"off": len(head), "size": len(pcm), "w": n, "h": 1}
        if v4:
            sp.update(diffx=0, diffy=0)
        rows.append(sp)
        head.extend(pcm)
    _shift(sec, 0, len(head), 0, len(rows))
    sec["sprites"][0:0] = rows
    sec["art"][0:0] = head
    tag = max([int(e["n"]) for e in sec["elements"]] + [0]) + 1
    for k, s in enumerate(sounds):
        rec = ts.sound_row(s["slot"], k, pattern, s.get("beat_chz", 0),
                           s.get("first_beat", 0), n=tag + k)
        if v4:
            rec.setdefault("parent", ts.PARENT_NONE)
            rec.setdefault("delay", 0)
        sec["elements"].append(rec)
    if not check:
        return None
    sprites = [(int(p["off"]), int(p["size"]), int(p["w"]), int(p["h"]))
               for p in sec["sprites"]]
    # D42: the beat the loop is held against is the rows', derived as the dial derives it.
    sweep, rate = ts.clock_tempo(sec["elements"], sec["scalars"])
    return ts.check_clock_sounds(sec["elements"], sprites, sweep, rate)


def read_wav(path, loop_seconds=None):
    with wave.open(path, "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (ts.SOUND_RATE, 1, 2):
            raise ValueError("%s is %d Hz, %d channel(s), %d-bit: a clock sound is %d Hz "
                             "mono 16-bit" % (path, w.getframerate(), w.getnchannels(),
                                              8 * w.getsampwidth(), ts.SOUND_RATE))
        n = w.getnframes()
        if loop_seconds:
            n = min(n, int(loop_seconds * ts.SOUND_RATE))
        return w.readframes(n)


def load(path):
    hdr, secs = ob.read_bundle(path)
    sec = [s for s in secs if s["blob"]["slug"] == "clock"]
    if not sec:
        raise ValueError("%s carries no clock section" % path)
    s = sec[0]
    return hdr, s, {
        "elements": [dict(e) for e in s["blob"]["elements"]],
        "scalars": dict(s["blob"]["scalars"]),
        "sprites": [dict(p) for p in s["blob"]["sprites"]],
        "art": bytearray(s["art"]),
    }


def save(path, hdr, s, sec):
    ob.write_bundle(path, hdr["name"],
                    [{"slug": "clock", "elements": sec["elements"], "scalars": sec["scalars"],
                      "sprites": sec["sprites"], "art": bytes(sec["art"]),
                      "scr_w": s["rec"]["scr_w"], "scr_h": s["rec"]["scr_h"],
                      "abi_min": s["rec"]["abi_min"], "abi_max": s["rec"]["abi_max"]}],
                    serial=hdr.get("serial", 0) + 1, baked_ms=hdr.get("baked_ms", 0))


def cmd_list(a):
    _hdr, _s, sec = load(a.theme)
    sweep, rate = ts.clock_tempo(sec["elements"], sec["scalars"])
    print("beat: %s %d a second (from the rows)" % ("sweep" if sweep else "step", rate))
    any_ = False
    for e in sec["elements"]:
        if not _is_sound(e):
            continue
        any_ = True
        sp = sec["sprites"][int(e["frame0"])]
        print("%-12s %7d samples %6.2f s  pattern %-11s%s" % (
            ts.CLOCK_SOUND_SLOTS[int(e["slot"])], sp["w"], sp["w"] / ts.SOUND_RATE,
            ts.HOUR_PATTERNS[int(e["kind"])],
            "  loop, beat %d/100 s, first %d" % (e["vmax"], e["vmin"])
            if int(e["flags"]) & ts.EF_LOOP else ""))
    if not any_:
        print("no sounds")


def cmd_add(a):
    hdr, s, sec = load(a.theme)
    sounds = []
    for spec in a.sound:
        slot, _, path = spec.partition("=")
        if slot not in ts.CLOCK_SOUND_SLOTS:
            raise SystemExit("unknown slot %r (%s)" % (slot, ", ".join(ts.CLOCK_SOUND_SLOTS)))
        tick = slot == "tick_loop"
        pcm = read_wav(path, a.loop_seconds if tick else None)
        d = {"slot": slot, "pcm": pcm}
        if tick:
            d["beat_chz"] = a.beat
            d["first_beat"] = a.first_beat
        sounds.append(d)
    pat = add_sounds(sec, sounds, a.pattern)
    save(a.out, hdr, s, sec)
    print("wrote %s: %d sound(s), pattern %s" % (a.out, len(sounds),
                                                  ts.HOUR_PATTERNS[pat] if pat is not None else "-"))


def cmd_strip(a):
    hdr, s, sec = load(a.theme)
    strip_sounds(sec)
    save(a.out, hdr, s, sec)
    print("wrote %s with no sounds" % a.out)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    q = sub.add_parser("list"); q.add_argument("theme"); q.set_defaults(fn=cmd_list)
    q = sub.add_parser("strip"); q.add_argument("theme"); q.add_argument("out")
    q.set_defaults(fn=cmd_strip)
    q = sub.add_parser("add"); q.add_argument("theme"); q.add_argument("out")
    q.add_argument("--sound", action="append", default=[], metavar="SLOT=FILE.wav")
    q.add_argument("--pattern", default="none", choices=ts.HOUR_PATTERNS)
    q.add_argument("--beat", type=int, default=100,
                   help="the tick loop's beats per 100 s (100 = 1/s, 400 = 4/s)")
    q.add_argument("--first-beat", type=int, default=0)
    q.add_argument("--loop-seconds", type=float, default=8.0,
                   help="keep only this much of the tick loop's WAV (an audition copy "
                        "repeats it)")
    q.set_defaults(fn=cmd_add)
    a = p.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
