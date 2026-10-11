#!/usr/bin/env python3
"""Watch the idle dim from the LAN, and prove the wake path without a hand.

Polls the idle block of /api/v1/status, so the countdown is visible rather than
inferred from a stopwatch: since_s climbs to timeout_s, dimmed flips, and the
level it returns to is lit_pct. When the dim lands it injects one rotary detent
over the same HTTP path the bench always used, and checks the face comes back —
the half of the feature that no log line can speak for.

It reports and never asserts:
  - the instant the dim fired, and how long the orb had actually been untouched
    then, because a knob turned by somebody else resets the countdown and would
    otherwise look like a dim that fired early;
  - the wake its own injection caused, and how long the lift took;
  - wakes and worst_mg, the two numbers ORB_IDLE_MOTION_MG is set from. Read
    them while the face is LIT and still and you have this board's floor; shake
    the orb and you have a pick-up. The threshold belongs between the two.

Usage: idle_watch.py [orb-ip] [seconds] [wake-after-secs|0] [trace]
  seconds      how long to watch (default: timeout + 60)
  wake-after   inject the detent this long after the dim lands; 0 = never inject
  trace        print one line per poll, not just the events

The trace is what tells the two failures of a never-dimming dial apart. If
since_s climbs steadily and stamps stays put while dimmed never goes true, the
policy task is not reaching its decision; if since_s keeps snapping back and
stamps climbs, something is stamping the clock and the firmware now says who.
"""
import json, sys, time, urllib.request

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.32"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
WAKE_AFTER = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
TRACE = len(sys.argv) > 4 and sys.argv[4] == "trace"
BASE = "http://%s" % IP
EVERY = 2.0

t0 = time.time()


def note(msg):
    print("%7.1f  %s" % (time.time() - t0, msg), flush=True)


def status():
    """One status read, retried: the orb is on a shared 2.4 GHz link and a lost
    GET here is a blip, not the finding the run is about."""
    err = None
    for _ in range(3):
        try:
            with urllib.request.urlopen(BASE + "/api/v1/status", timeout=8) as r:
                return json.load(r)
        except Exception as e:
            err = e
            time.sleep(1.0)
    note("status unreachable: %s" % err)
    sys.exit(2)


d = status()
idle = d.get("idle")
if idle is None:
    note("no idle block in status - this build predates it, reflash first")
    sys.exit(1)
if not idle.get("on"):
    note("idle dim compiled OUT (ORB_IDLE_DIM_ENABLE=n): nothing to watch")
    sys.exit(0)

timeout_s = idle["timeout_s"]
if not SECS:
    SECS = timeout_s + 60
note("armed: dims to %u%% at %u s | now %u s since last touch, wakes=%u, "
     "worst=%u mg | leave the orb alone from here"
     % (idle["dim_pct"], timeout_s, idle["since_s"], idle["wakes"],
        idle["worst_mg"]))

dimmed_at = woke_at = None
last_since = idle["since_s"]
resets = 0
i = idle
while time.time() - t0 < SECS:
    i = status()["idle"]
    # A countdown that jumps back up with no injection of ours is somebody else's
    # touch. Say so, or the dim that eventually lands looks late for no reason.
    if i["since_s"] < last_since - int(EVERY) and dimmed_at is None:
        resets += 1
        note("countdown reset by an outside touch (%u -> %u s); the clock "
             "restarts from here" % (last_since, i["since_s"]))
    last_since = i["since_s"]
    if TRACE:
        print("       since=%-4s dimmed=%-5s wakes=%-3s stamps=%-4s "
              "worst=%s/%s mg" % (i["since_s"], i["dimmed"], i["wakes"],
                                  i.get("stamps"), i["worst_mg"],
                                  i.get("worst_dark_mg")), flush=True)

    if i["dimmed"] and dimmed_at is None:
        dimmed_at = time.time()
        note("DIMMED at since_s=%u (timeout_s=%u) | wakes=%u worst=%u mg"
             % (i["since_s"], timeout_s, i["wakes"], i["worst_mg"]))
        if not WAKE_AFTER:
            note("not injecting a wake (wake-after=0); turn the knob to test it")
    elif woke_at is None and dimmed_at and not i["dimmed"]:
        woke_at = time.time()
        note("WAKE confirmed: back to lit_pct=%u%%, %.1f s after the dim"
             % (i["lit_pct"], woke_at - dimmed_at))

    if dimmed_at and woke_at is None and WAKE_AFTER \
            and time.time() - dimmed_at >= WAKE_AFTER:
        note("injecting one detent over HTTP (?turn=1)")
        with urllib.request.urlopen(BASE + "/api/v1/screen?turn=1", timeout=8):
            pass
    time.sleep(EVERY)

if dimmed_at is None:
    note("NEVER DIMMED in %.0f s of watching (timeout_s=%u, final since_s=%u, "
         "outside resets=%d)" % (SECS, timeout_s, last_since, resets))
    sys.exit(2)
if woke_at is None and WAKE_AFTER:
    note("dimmed, but the injected detent did not bring it back inside the "
         "window - the wake path is the suspect")
    sys.exit(3)
note("watch over: wakes=%u worst=%u mg" % (i["wakes"], i["worst_mg"]))
