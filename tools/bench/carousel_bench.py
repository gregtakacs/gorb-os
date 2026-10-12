#!/usr/bin/env python3
"""carousel_bench.py — the ruling-C5 coalescing claim, tested on the board.

What it decides
---------------
main/app/orb_carousel.c promises that N detents inside the settle window cost ONE
full-page commit, not N. host_carousel_test.py proves that against a synthetic
clock and stubbed leaves. This proves it against the shipping build, on the real
LVGL task, with the real registry and the real display, and it reads the commit
count out of the router's own M1 probe (`input: switch X -> glass +N ms: N
commit(s)`).

Both directions are asserted, because only one of them is a test:
  coalesce   N tight detents  -> exactly 1 activation, 1 commit, hops == N
  control    N detents spaced beyond the READ window -> N activations, N commits
A suite that checked only `coalesce` would also pass if the window were infinite
(nothing would ever switch) and if it were zero (every detent switches, which is
the pre-carousel behaviour). The control is what makes the number mean something.
Since §12.9 the module carries TWO windows (in-burst settle / read hold), so the
control spacing is taken beyond the read one — detents spaced between the two
would coalesce legitimately, and a control that coalesces is not a control.

Three cases the first bench pass produced by accident, kept because they are the
interesting ones:
  wrap       N == walkable count lands back on the entry already on the glass,
             and orb_carousel.c CANCELS rather than commit a phantom switch:
             0 activations, 1 "cancelled" line.
  hold       long-press from a non-home screen returns to the clock (C8).
  press      a lone detent deep inside the read window, then push=1: the press
             must switch NOW ("that one, now", input_router.c), not wait out
             the read window — and must land INSIDE the app (armed HSB editor /
             menu list, owner ruling 2026-10-12). Timed: the activation must
             land well before the auto-commit could, and the same press is the
             only thing allowed to arm (the auto-commit cases assert no arm).

Where it is honest about its own limits
---------------------------------------
Detents are fired over `GET /api/v1/screen?turn=+1`, which reaches the router
through input_router_inject() — the same fold the encoder's own detent uses
(input_router.c:202), so from there down the path is the shipping one. It is NOT
a finger: injection enters downstream of knob.c, and `/api/v1/status` proves it
(raw/det/spd10 stay 0 through injected runs while the walk visibly happens).
This suite decides whether the windows COALESCE and whether the press commits.
The gap distribution of a human hand is measured separately and recorded in
§12.9 — it is not this tool's, and injected 25 ms gaps must never be quoted as
human timing.

It also refuses to trust its own spacing. HTTP round-trip at bench rssi adds
60-300 ms of jitter per request, so the x-axis is taken from the DEVICE's serial
timestamps for each `synthetic turn`, not from the requested gap. If two turns
arrive inside one 10 ms poll they fold into a single +/-1 step (the delta is a
saturating ADD, not a queue), and the case is reported VOID rather than passed —
a hop count below N means the burst never existed as N detents.

Usage: carousel_bench.py            # full run, ~90 s incl. the boot it triggers
Exit:  0 all assertions held; 1 a named assertion failed; 2 a gate/env refusal.
"""
import json, os, re, subprocess, sys, threading, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import orb_bind

_CFG = orb_bind.load()
IP = orb_bind.gate_ip(_CFG, sys.argv[1] if len(sys.argv) > 1 else None)
BASE = "http://%s" % IP
SER = os.path.join(os.path.dirname(HERE), "orb_watch_ser.py")
LOG = "/tmp/carousel_bench_ser.log"
STOP = "/tmp/carousel_bench.stop"
TURN_GAP = 0.025          # > POLL_PERIOD_MS (10 ms) so each turn is its own detent
TAIL = 1.6                # settle window + commit + log drain, per case

class Unreachable(Exception):
    """The link died, which is NOT the same fact as an assertion failing. Kept
    distinct so a flaky bench radio can never be reported as a carousel bug:
    this exits 2 (environment), while a real failure exits 1."""


def get(path, timeout=8, tries=3):
    last = None
    for i in range(tries):
        try:
            with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
                return r.read().decode("utf-8", "replace")
        except Exception as e:            # timeouts, refused connections, partials
            last = e
            time.sleep(0.4 * (i + 1))     # back off; the httpd worker can be busy
    raise Unreachable("%s after %d tries: %s" % (path, tries, last))


def start_capture(secs):
    """orb_watch_ser owns the port and its gates; opening it resets the board,
    so the run always begins from home and the capture spans the boot too."""
    for p in (LOG, STOP):
        if os.path.exists(p):
            os.remove(p)
    return subprocess.Popen([sys.executable, SER, str(secs), LOG, "--stop-file", STOP],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def rows():
    out = []
    if not os.path.exists(LOG):
        return out
    for raw in open(LOG, errors="replace"):
        m = re.match(r"^(\d+\.\d+)\s+(.*)$", raw.rstrip("\n"))
        if m:
            out.append((float(m.group(1)), m.group(2)))
    return out


def wait_up(want_fw, secs=150):
    t0 = time.time()
    while time.time() - t0 < secs:
        # Polled with a bare fetch, not get(): while the board is booting every
        # request is expected to fail, and that is waiting, not an error.
        try:
            with urllib.request.urlopen(BASE + "/api/v1/status", timeout=4) as r:
                s = json.loads(r.read().decode("utf-8", "replace"))
            if s.get("state") == "connected":
                return s
        except Exception:
            pass
        time.sleep(2)
    raise SystemExit("orb at %s never reported connected (fw wanted %s)" % (IP, want_fw))


def consts(rs):
    """Take the constants under test FROM the device, not from this file: if
    the carousel's windows change, the control case must move with them or the
    suite is testing windows the board does not use."""
    settle, read, burst, walkable = 400, 2000, 300, 3
    for _, x in rs:
        m = re.search(r"carousel: chip ready \(settle (\d+) ms", x)
        if m:
            settle = int(m.group(1))
        m = re.search(r"read (\d+) ms, burst gap (\d+) ms", x)
        if m:
            read, burst = int(m.group(1)), int(m.group(2))
        m = re.search(r"router running \(poll (\d+) ms, (\d+) walkable", x)
        if m:
            walkable = int(m.group(2))
    return settle, read, burst, walkable


def fire(n, gap):
    """N separate +1 requests, each its own poll. Returns the device-side
    timestamps so the caller can measure the gap the board actually saw."""
    seen = []
    for i in range(n):
        t = time.time()
        get("/api/v1/screen?turn=+1")
        seen.append(t)
        if i < n - 1:
            time.sleep(max(0.0, gap - 0.001))
    return seen


def home():
    get("/api/v1/screen?to=clock")
    time.sleep(1.2)


def slice_win(rs, t0, t1):
    return [x for t, x in rs if t0 <= t <= t1]


def measure(rs, t0, t1):
    """Everything the verdict needs, read from the device's own log lines."""
    w = slice_win(rs, t0, t1)
    acts = [re.search(r"input: screen -> (\S+)", x).group(1)
            for x in w if re.search(r"input: screen -> (\S+)", x)]
    peeks = len([1 for x in w if "carousel: peek" in x])
    hops = [int(m.group(1)) for x in w if (m := re.search(r"settle .+? after (\d+) hop", x))]
    canc = len([1 for x in w if "carousel: cancelled on" in x])
    arms = [re.search(r"input: armed (\S+)", x).group(1)
            for x in w if re.search(r"input: armed (\S+)", x)]
    coms = [int(m.group(1)) for x in w if (m := re.search(r"-> glass \+(\d+) ms: (\d+) commit", x))]
    glms = [int(m.group(1)) for x in w if (m := re.search(r"-> glass \+(\d+) ms: (\d+) commit", x))]
    turns = [t for t, x in rs if t0 <= t <= t1 and "synthetic turn" in x]
    gaps = [int(round((turns[i + 1] - turns[i]) * 1000)) for i in range(len(turns) - 1)]
    act_ts = [t for t, x in rs if t0 <= t <= t1
              and re.search(r"input: screen -> (\S+)", x)]
    return dict(acts=acts, peeks=peeks, hops=hops[-1] if hops else 0, canc=canc,
                arms=arms,
                commits=coms, glass=glms, gaps=gaps, turns=turns, act_ts=act_ts)


def main():
    fw = os.environ.get("ORB_EXPECT_FW")
    proc = start_capture(420)
    try:
        st = wait_up(fw or "?")
        print("orb %s  fw=%s  rssi=%s  slot=%s  art_banks=%s" %
              (IP, st.get("fw"), st.get("rssi"), st.get("slot"), st.get("art_banks")))
        time.sleep(2.5)                      # let the boot log drain into the file
        rs = rows()
        settle, read, burst, W = consts(rs)
        print("device says: settle %d ms, read %d ms, burst gap %d ms, "
              "%d walkable, poll 10 ms\n" % (settle, read, burst, W))

        fails, voids = [], []
        plan = [
            ("coalesce-2",     2,     TURN_GAP,             "acts==1 and hops==2"),
            ("coalesce-w+1",   W + 1, TURN_GAP,             "acts==1 (wrap-cancel then re-name)"),
            ("wrap-w",         W,     TURN_GAP,             "acts==0, cancelled>=1"),
            # beyond the READ window, not the settle one: detents spaced between
            # the two coalesce legitimately now, and a coalesced control tests
            # nothing. The VOID below catches HTTP jitter that fails to clear it.
            ("control-2",      2,     (read + 300) / 1000.0, "acts==2"),
        ]
        for name, n, gap, want in plan:
            home()
            t_pre = time.time() - 0.05
            fire(n, gap)
            t_post = time.time() + read / 1000.0 + 1.3
            while time.time() < t_post:
                time.sleep(0.05)
            rs = rows()
            m = measure(rs, t_pre, t_post)
            seen_gap = max(m["gaps"]) if m["gaps"] else 0
            tight = bool(m["gaps"]) and max(m["gaps"]) < burst / 2000.0
            verdict = "ok"
            if name.startswith("coalesce") or name == "wrap-w":
                # a tight burst must stay tight AND land whole: the requests
                # log one line each, but the router walks one step per poll, so
                # HTTP arrival jitter that clusters requests into one poll
                # window silently drops the extra step — a step shows up as a
                # peek or (on the wrap-around step) a cancel; fewer than n
                # means the board never received the burst the case claims
                # to test. Never pass or FAIL on a burst that did not land.
                if tight and (seen_gap > burst * 0.9 or m["peeks"] + m["canc"] != n):
                    verdict = "VOID (HTTP jitter: gaps %s, peeks=%d canc=%d want %d steps)" % (
                        m["gaps"], m["peeks"], m["canc"], n)
                    voids.append(name)
                elif name == "wrap-w":
                    ok = len(m["acts"]) == 0 and m["canc"] >= 1
                    verdict = "ok" if ok else "FAIL acts=%d cancels=%d" % (
                        len(m["acts"]), m["canc"])
                else:
                    ok = len(m["acts"]) == 1 and m["hops"] in (n, m["peeks"])
                    if name == "coalesce-w+1":
                        ok = len(m["acts"]) == 1
                    verdict = "ok" if ok else "FAIL acts=%d hops=%d peeks=%d" % (
                        len(m["acts"]), m["hops"], m["peeks"])
            else:
                # The control's own VOID: jitter that kept the spaced detents
                # INSIDE the read window coalesces them, and counting that as a
                # carousel bug would blame the board for the bench link.
                if seen_gap and seen_gap < read * 0.9:
                    verdict = "VOID (detents only %d ms apart, want >%d)" % (
                        seen_gap, int(read * 0.9))
                    voids.append(name)
                else:
                    verdict = "ok" if len(m["acts"]) == n else "FAIL acts=%d want %d" % (
                        len(m["acts"]), n)
                # the press is what arms (owner ruling 2026-10-12); a window
                # expiry that also armed would make every auto-switch a trap
                # the user cannot turn the knob out of
                if verdict == "ok" and m["arms"]:
                    verdict = "FAIL auto-commit armed %s (press must be the only arm)" % m["arms"]
            bad = verdict.startswith("FAIL")
            if bad:
                fails.append("%s: %s (want %s)" % (name, verdict, want))
            print("%-14s n=%d gap<=%3dms seen=%4dms | peeks=%d acts=%d hops=%d canc=%d "
                  "glass=%s | %s" %
                  (name, n, gap * 1000, seen_gap, m["peeks"], len(m["acts"]), m["hops"],
                   m["canc"], "/".join("%d" % g for g in m["glass"]) or "-", verdict),
                  flush=True)

        home()
        get("/api/v1/screen?to=color_picker")
        time.sleep(1.3)
        t0 = time.time() - 0.05
        get("/api/v1/screen?hold=1")
        t1 = time.time() + 1.8
        while time.time() < t1:
            time.sleep(0.05)
        m = measure(rows(), t0, t1)
        landed = m["acts"][-1] if m["acts"] else "NOTHING"
        v = "ok" if landed == "clock" else "FAIL landed on %s" % landed
        if landed != "clock":
            fails.append("hold-homes: landed on %s, want clock (C8)" % landed)
        print("%-14s long-press from color_picker -> %s | %s" % ("hold-c8", landed, v),
              flush=True)

        # press-to-commit (the B half of §12.9's ruling): one lone detent is a
        # stop-to-read under the adaptive rule, so the read window is open; the
        # press must switch NOW rather than let that window run out. Counting
        # acts alone cannot see the difference — the auto-commit also produces
        # exactly one act — so the assertion is TIMED: the activation must land
        # clearly before the auto-commit deadline, or the press path silently
        # fell back to waiting and the user is still holding a dead button.
        home()
        t_pre = time.time() - 0.05
        fire(1, 0)
        time.sleep(0.6)                       # deep inside READ, nowhere near it
        get("/api/v1/screen?push=1")
        t_post = time.time() + read / 1000.0 + 1.3
        while time.time() < t_post:
            time.sleep(0.05)
        m = measure(rows(), t_pre, t_post)
        p_verdict = "ok"
        if len(m["acts"]) != 1 or not m["act_ts"] or not m["turns"]:
            p_verdict = "FAIL acts=%d (want 1, with turn+act timestamps)" % len(m["acts"])
        else:
            delay = int(round((min(m["act_ts"]) - min(m["turns"])) * 1000))
            if not (0 <= delay < read - 400):
                p_verdict = ("FAIL press did not pre-empt: act +%d ms (auto-commit ~%d)"
                             % (delay, read))
            elif m["arms"] != [m["acts"][0]]:
                # rotate-press-enters (owner ruling 2026-10-12): the same press
                # that committed the peek must find the user INSIDE the named
                # app — the armed view (HSB editor / menu list), not its face.
                p_verdict = "FAIL press committed but did not enter: armed=%s" % m["arms"]
        print("%-14s 1 detent + press | acts=%d arms=%s | %s" %
              ("press-1", len(m["acts"]), m["arms"] or ["-"], p_verdict), flush=True)
        if p_verdict.startswith("FAIL"):
            fails.append("press-1: %s" % p_verdict)

        print("\nHand ratification of the windows is NOT this run's evidence: it is")
        print("the owner's 16-detent sample recorded in plan §12.9 (flick gaps")
        print("33-157 ms, read pauses 510-1936 ms). Injection here enters")
        print("downstream of knob.c (raw/det/spd10 stay 0), so injected 25 ms")
        print("gaps are not human timing and must never be quoted as such.")
        if voids:
            print("\nVOID cases (re-run, or shorten the burst): %s" % ", ".join(voids))
        if fails:
            print("\n%d FAILED:" % len(fails))
            for f in fails:
                print("  " + f)
            return 1
        print("\nall carousel bench assertions held")
        return 0
    except Unreachable as e:
        print("ENV: %s — that is the bench link, not the carousel" % e)
        return 2
    finally:
        open(STOP, "w").close()
        try:
            proc.wait(timeout=20)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())

