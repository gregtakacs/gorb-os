#!/usr/bin/env python3
"""C5 flick bench: does a burst of N detents cost ONE activation?

docs/app-platform-plan.md 12.6 owes that number. The observable is the serial log:
the carousel prints one "peek" line per detent named and input_router prints one
"switch" line per activation, so the two counts against each other are the claim.

Detents must arrive as separate HTTP requests. One request carrying turn=3 does NOT
make a burst: poll_timer_cb drains the whole pending injection into a single tick
(input_router.c:202), so a request is one detent by construction. Spacing requests
below the 400 ms settle window is what a hand flicking the knob looks like to the
router.

The UART is held by the capture only for the window being measured, and the requests
are fired while it is held, because the drive path is the LAN and the evidence path
is the serial port.
"""
import json, re, subprocess, sys, time
import socket
import urllib.request

CAP = "/home/greg/.espressif/python_env/idf5.4_py3.14_env/bin/python"


def get(base, path, timeout=5, tries=2):
    """Tolerant GET. One httpd worker behind -73 dBm wifi answers some requests
    late enough to time out, and a home request that fails must not take the whole
    trial down with it: the burst then simply fails the setup precondition, which
    names the real cause instead of hiding it behind a stack trace."""
    for i in range(tries):
        try:
            with urllib.request.urlopen(base + path, timeout=timeout) as r:
                return json.loads(r.read())
        except Exception as e:
            last = e
            time.sleep(0.3)
    print(f"  (get {path} failed after {tries} tries: {last})")
    return {}


POLL_MS      = 10    # input_router's poll period; one injected detent is consumed per poll
DETENT_SPACING = 0.045


def flick(base, n, spacing=DETENT_SPACING):
    """n detents, ~45 ms apart, each fire-and-forget: one flick.

    The spacing is the whole design, and both ends of it are load-bearing:

    Too tight and it is NOT a burst. poll_timer_cb drains the pending injection whole
    (input_router.c:202) and the roaming branch takes exactly ONE step per poll, so n
    requests accepted inside the same 10 ms poll walk one screen, not n. Firing them
    simultaneously therefore emulates `?turn=n`, not a flick — measured: 6 parallel
    requests produced a single `peek ... (1 hop)`.

    Too loose and it is not ONE flick either: past the 400 ms settle the router
    correctly activates each detent separately, which is the behaviour the carousel
    exists to replace. Measured spacing on this bench at -73 dBm runs 40-120 ms.

    Requests are written and abandoned rather than read back: waiting on each response
    puts the httpd worker's own stalls in the spacing, and a commit in flight can hold
    it for hundreds of ms. verdict() re-checks the real arrival spacing from the device
    log, so a burst that failed to land tight is reported as a bad drive, never as a
    broken carousel.
    """
    hp = base.split("//")[1]
    host, _, prt = hp.partition(":")
    port = int(prt) if prt else 80
    wire = ("GET /api/v1/screen?turn=1 HTTP/1.1\r\nHost: %s\r\n"
            "Connection: close\r\n\r\n" % host).encode()
    for i in range(n):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(2.0)
            s.connect((host, port))
            s.sendall(wire)
            s.close()                     # response deliberately unread: see docstring
        except OSError:
            pass                          # a lost detent shows up in the counts
        time.sleep(spacing)


RING = 3   # walkable entries today: clock, color_picker, menu (orb_app.c:185-215)

TRIALS       = (1, 2, 3, 4, 5, 6)
BOOT_WAIT    = 10.0   # the port open resets the board: wait out wifi and the dial
SETTLE_TAIL  = 2.0    # let a burst's activate and one dial second reach the log


def expected(n):
    """What a burst of n detents from the dial must print, wrap included.

    A whole lap names the entry already on the glass, and step() cancels rather
    than paint a title over its own screen (orb_carousel.c:164), so every R-th
    detent prints no peek at all. A burst that ends exactly on the lap boundary
    therefore costs ZERO activations, which beats the one the C5 claim asks for:
    the user asked to go round the ring and ended where they started.
    """
    peeks   = n - (n // RING)
    settles = 0 if n % RING == 0 else 1
    return peeks, settles


def count(txt):
    """Every carousel/router signal in one capture, with boot-ms timestamps.

    The ms comes from the ESP log tag, not the harness wall clock: it is the
    device's own time, so a stalled host cannot fake a settle window.
    """
    def ms(pat):
        return [int(m) for m in re.findall(r"I \((\d+)\) " + pat, txt)]
    peeked = re.findall(r"I \((\d+)\) carousel: peek (\S+)", txt)
    return dict(
        turns   = ms(r"orb_net: synthetic turn "),
        peeks   = [int(t) for t, _ in peeked],
        slug    = [s for _, s in peeked],
        settles = re.findall(r"carousel: settle (\S+) after (\d+) hop[^\n]*", txt),
        set_ms  = ms(r"carousel: settle "),
        sw_ms   = ms(r"input:\s+switch \S+ sync"),
        commits = [(int(t), n) for t, n in
                   re.findall(r"I \((\d+)\) input: switch \S+ -> glass \+\d+ ms: "
                              r"(\d+) commit", txt)],
        cancels = ms(r"carousel: cancelled on "),
        stat    = re.findall(r"I \((\d+)\) clock: clock (\d+) fps", txt),
    )


FIRST_PEEK = "color_picker"   # +1 from the dial, per the table order in orb_app.c:185-215


def verdict(n, c, alive=None):
    """(pass, reason) for one burst. alive is the dial's fps after returning home."""
    exp_p, exp_s = expected(n)

    # Precondition the DRIVE path before judging the device at all. A burst is a
    # burst only if its detents arrive inside one settle window. When the LAN delivers
    # them late the router correctly sees separate gestures, and charging the carousel
    # for that is the false failure that made the first version of this bench useless.
    turns = c["turns"]
    if len(turns) != n:
        return False, f"drive: {len(turns)}/{n} turns landed in the window"
    gaps = [b - a for a, b in zip(turns, turns[1:])]
    if gaps and max(gaps) >= 380:
        return False, f"drive: turns {max(gaps)} ms apart, so not one flick"
    if gaps and min(gaps) < POLL_MS:
        return False, (f"drive: turns {min(gaps)} ms apart, inside one {POLL_MS} ms poll — "
                       f"the router drained them as a single step, not a burst")
    g = turns[0]

    # Judge only what THIS burst caused: the capture window can straddle the tail of
    # the previous trial's activate, and counting that settle made the hold time come
    # out negative in the first hardware run.
    def since(lst):
        return [t for t in lst if t >= g]
    peeks   = since(c["peeks"])
    set_ms  = since(c["set_ms"])
    sws     = since(c["sw_ms"])
    cancels = since(c["cancels"])
    slugs   = [s for t, s in zip(c["peeks"], c["slug"]) if t >= g]
    settles = [x for x, t in zip(c["settles"], c["set_ms"]) if t >= g]
    commits = [m for t, m in c["commits"] if t >= g]

    if slugs and slugs[0] != FIRST_PEEK:
        return False, f"setup: burst did not start at the dial (first peek {slugs[0]})"
    if (len(peeks) != exp_p or len(settles) != exp_s or len(sws) != exp_s
            or commits.count("1") != exp_s):
        return False, "counts"
    if exp_s:
        if int(settles[0][1]) != n % RING:
            return False, "hops"
        if not (380 <= set_ms[0] - peeks[-1] <= 600):
            return False, "window"
        # Beat-leak check for a burst that ACTIVATES. The clock's stats printer is the
        # beat callback itself, which returns early when the clock is not on the glass
        # (orb_sched.c:471), so no stat line can exist in this capture once the picker
        # holds the screen. The evidence is the dial's tempo AFTER coming home instead:
        # commit() hands the park over rather than releasing it, so a handover that
        # leaked would leave the dial dead on its return, and alive would be 0.
        if alive is None or alive < 5:
            return False, "beat leak (handover)"
    else:
        # Beat-leak check for a burst that CANCELS: the glass never left the clock, so
        # the beat's own stat line is observable inside this capture. A cancel() that
        # resumed nothing would freeze the dial at 0 fps forever.
        gate  = max(peeks[-1] if peeks else 0, cancels[-1] if cancels else 0)
        after = [int(f) for t, f in c["stat"] if int(t) > gate]
        if not after or max(after) < 10:
            return False, "beat leak (cancel)"
    return True, ""


def selftest():
    """Controls proving the classifier discriminates, in both directions.

    Without these, a firmware with no carousel at all could pass the suite by the
    counts being ignored, and a check that always failed would look identical to one
    that always caught the bug. The pre-carousel log below is exactly what the router
    printed before C5: every detent activates, so a 5-detent flick pays 5 activates
    and 5 full-page commits.
    """
    old = "".join(f"I ({400000 + i * 100}) orb_net: synthetic turn +1 detent(s)\n"
                  f"I ({400050 + i * 100}) input: switch menu sync 0 ms\n"
                  f"I ({400080 + i * 100}) input: switch menu -> glass +64 ms: "
                  f"1 commit(s), avg 57000 us\n"
                  for i in range(5))
    burst = ("I (499900) orb_net: synthetic turn +1 detent(s)\n"
             "I (499950) orb_net: synthetic turn +1 detent(s)\n"
             "I (500000) carousel: peek color_picker (1 hop)\n"
             "I (500100) carousel: peek menu (2 hops)\n"
             "I (500510) carousel: settle menu after 2 hops\n"
             "I (500510) input: switch menu sync 0 ms\n"
             "I (500580) input: switch menu -> glass +70 ms: 1 commit(s), avg 60000 us\n")
    lap = ("I (599900) orb_net: synthetic turn +1 detent(s)\n"
           "I (599950) orb_net: synthetic turn +1 detent(s)\n"
           "I (600000) orb_net: synthetic turn +1 detent(s)\n"
           "I (600050) carousel: peek color_picker (1 hop)\n"
           "I (600100) carousel: peek menu (2 hops)\n"
           "I (600200) carousel: cancelled on menu — no switch\n")
    ticks = "".join(f"I ({t}) clock: clock 15 fps | flush frames 15\n"
                    for t in (600400, 601400, 602400))
    # Same burst, but the two turns land 500 ms apart: two gestures, not one flick.
    spaced  = burst.replace("I (499950) orb_net", "I (500400) orb_net")
    # Same burst, but the first name is not the dial's neighbour: it did not start home.
    unhomed = burst.replace("peek color_picker", "peek menu")

    checks = [
        # n   log                     alive  must pass?  what it is
        (5,   old,                    15,    False, "pre-carousel log: 5 detents cost 5 activates"),
        (2,   burst,                  0,     False, "leaked handover: dial frozen at 0 fps on return"),
        (2,   burst,                  15,    True,  "healthy 2-detent flick: 1 activate, 1 commit"),
        (3,   lap,                    None,  False, "leaked cancel park: beat never resumed"),
        (3,   lap + ticks,            None,  True,  "whole lap cancels and the beat survives it"),
        (2,   spaced,                 15,    False, "turns 500 ms apart are two gestures, not a flick"),
        (2,   unhomed,                15,    False, "burst that did not start at the dial"),
    ]
    fails = 0
    for n, txt, alive, want, label in checks:
        got, why = verdict(n, count(txt), alive=alive)
        ok = (got == want)
        if not ok:
            fails += 1
        print(f"  {'ok  ' if ok else 'BAD ' }[{label}] got={got} ({why or 'clean'})")
    print(f"selftest: {'ALL CONTROLS BEHAVE' if not fails else str(fails) + ' CONTROL(S) WRONG'}")
    return 0 if not fails else 1


def parse_rows(txt):
    """Wall instant, device ms, and the original log text, for each stamped line."""
    rows = []
    for line in txt.splitlines():
        m = re.match(r"^(\d+\.\d+) (.*I \((\d+)\).*)$", line)
        if m:
            rows.append((float(m.group(1)), int(m.group(3)), m.group(2)))
    return rows


def window(rows, a, b):
    """The log lines the capture received between two wall instants."""
    return "\n".join(l for w, _, l in rows if a <= w < b)


def judge(txt, plan):
    """Segment one continuous capture by the instants the harness fired each burst.

    plan is [(n, t_start, t_end)] in drive order. Trial boundaries come from the drive
    side and never from gaps in the log: at -73 dBm the gaps stretch unpredictably, and
    the first heuristic (turns within 1.5 s belong together) merged two trials into one
    burst and reported [2, 3, 3, 5, 6] for a run that fired [1, 2, 3, 4, 5, 6].
    """
    print(f"{'detents':>8} {'peek':>5} {'settle':>7} {'switch':>7} "
          f"{'hops':>5} {'settle_ms':>10} {'commits':>8} {'fps':>5}  verdict")

    # capf.py prefixes every line with the wall instant it arrived, which is what makes
    # the slicing above possible; keep the original log text after the prefix.
    rows = parse_rows(txt)
    if not rows:
        print("INVALID RUN: no wall-stamped log lines — wrong capture file, or capf.py "
              "is the unstamped version. Re-run.")
        return 2

    # Was the board stable while it was being driven? Opening the port MIGHT reset it
    # (observed both ways) and the reset can land mid-line, gluing the tail of the old
    # session onto the first line of the new one. Everything ahead of the LAST reset is
    # dropped, because those stale lines carry ms values LARGER than the fresh session's
    # and would otherwise be judged as part of the final trial. Any reset left after
    # that cut happened underneath a burst, which invalidates it. Compare adjacent
    # lines only, not a running maximum: max() never comes back down, so one real reset
    # counted 297 times in the first version of this check.
    boots = len(re.findall(r"cpu_start: Pro cpu start", txt))
    dev = [d for _, d, _ in rows]
    cut = 0
    for i, (a, b) in enumerate(zip(dev, dev[1:])):
        if b < a - 1000:
            cut = i + 1
    rows  = rows[cut:]
    dev   = [d for _, d, _ in rows]
    leaked = sum(1 for a, b in zip(dev, dev[1:]) if b < a - 1000)
    if boots > 1 or leaked:
        print(f"INVALID RUN: {boots} boot banner(s) and {leaked} clock rewind(s) inside "
              f"the driven window, so those bursts are not one continuous session. Re-run.")
        return 2
    if not any("carousel:" in l for _, _, l in rows):
        print("INVALID RUN: the capture holds no carousel lines at all — the port was "
              "busy, or the board never came up. Re-run.")
        return 2

    fails = 0
    for n, b0, b1, h0, h1 in plan:
        body = window(rows, b0, b1)
        c    = count(body)
        g    = c["turns"][0] if c["turns"] else 0
        peeks  = [t for t in c["peeks"] if t >= g]
        set_ms = [t for t in c["set_ms"] if t >= g]
        settles = [x for x, t in zip(c["settles"], c["set_ms"]) if t >= g]

        # A burst that activates leaves the dial off the glass, and the clock's beat
        # callback returns early when it is not on the glass (orb_sched.c:471), so no
        # stat line exists during that stretch. The harness homes the device after
        # every burst precisely so the dial's own tempo becomes observable again, and
        # samples that window separately: it is what a leaked handover would zero out.
        alive = None
        if expected(n)[1]:
            hb   = count(window(rows, h0, h1))
            rate = [int(f) for _, f in hb["stat"]]
            alive = max(rate) if rate else 0

        ok, why = verdict(n, c, alive=alive)
        # Held-still measured from the LAST detent, which is what the window is
        # timed from. There is no "parked N ms" log field, so this is derived from
        # the device's own timestamps rather than trusted from a printed number.
        last_hold = (set_ms[0] - peeks[-1]) if (settles and peeks) else -1
        gate  = max(set_ms[0] if set_ms else 0,
                    peeks[-1] if peeks else 0,
                    max([t for t in c["cancels"] if t >= g], default=0))
        after = [int(f) for t, f in c["stat"] if int(t) > gate]
        beat  = alive if alive is not None else (max(after) if after else -1)
        hops  = settles[0][1] if settles else "0"
        if not ok:
            fails += 1
        sws   = [t for t in c["sw_ms"] if t >= g]
        print(f"{n:>8} {len(peeks):>5} {len(settles):>7} {len(sws):>7} "
              f"{hops:>5} {last_hold:>10} "
              f"{','.join(m for t, m in c['commits'] if t >= g) or '-':>8} "
              f"{beat:>5}  {'PASS' if ok else 'FAIL ' + why}")
        if not ok:
            for l in body.splitlines():
                if "carousel" in l or "switch" in l or "stall" in l or "turn" in l:
                    print("   |", l.strip())

    print(f"\n{'ALL TRIALS PASS' if not fails else str(fails) + ' TRIAL(S) FAIL'}")
    print("note: the fps check covers 12.6's 'no beat leak' automatically. Chip "
          "visibility over the live dial is the one claim left that needs an eye.")
    return 0 if not fails else 1


PLAN = "/tmp/flick_plan.json"


def drive(ip, gap):
    """Hold the port open once, fire every burst, judge the single capture."""

    base = f"http://{ip}"

    st = get(base, "/api/v1/status")
    print(f"target {base}  bursts fired as {len(TRIALS)} parallel connections; "
          f"400 ms settle window  (state={st.get('state', '?')} "
          f"rssi={st.get('rssi', '?')})")

    # ONE capture for the whole run. Opening the S3 USB-CDC port can hard-reset the
    # board, so a capture per trial rebooted the device underneath several bursts and
    # the verdicts described a screen that had just come back from power-on. One open
    # for the run, and the log is sliced by the wall instants recorded here.
    log = "/tmp/flick_all.txt"
    # Long ceiling: the run ends by terminating the capture, so a slow httpd worker that
    # stretches the trials cannot truncate the log out from under the slicing.
    p = subprocess.Popen([CAP, "/tmp/capf.py", "600", log, "--no-reset"])
    time.sleep(BOOT_WAIT)                    # out of reset, through wifi, onto the dial

    plan = []
    for n in TRIALS:
        for attempt in range(3):
            # Home through the registry so every burst starts from the dial and a peek
            # left over from the last trial is cancelled rather than inherited.
            get(base, "/api/v1/screen?to=clock")
            time.sleep(1.2)
            b0 = time.time() - 0.3           # margin for serial read latency
            flick(base, n)
            time.sleep(SETTLE_TAIL)          # let the activate and one dial second land
            b1 = time.time()
            get(base, "/api/v1/screen?to=clock")  # home: the handover's beat sample
            h0 = time.time() - 0.3
            time.sleep(1.8)
            h1 = time.time()
            # capf.py writes each line as it arrives, so the burst can be checked while
            # the capture is still running and re-fired if the drive path misbehaved.
            now = parse_rows(open(log).read())
            why = verdict(n, count(window(now, b0, b1)),
                          alive=(max([int(f) for _, f in
                                      count(window(now, h0, h1))["stat"]], default=0)
                                 if expected(n)[1] else None))[1]
            if not why.startswith("drive:"):
                break
            print(f"  {n} detents, attempt {attempt + 1}: {why}")
        plan.append((n, b0, b1, h0, h1))
    time.sleep(1.0)
    p.terminate()
    p.wait()
    with open(PLAN, "w") as f:               # so --replay re-judges the same slices
        json.dump(plan, f)
    return judge(open(log).read(), plan)


def main():
    ip   = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.32"
    if ip == "--selftest":
        return selftest()
    if ip == "--replay":
        with open(PLAN) as f:
            return judge(open(sys.argv[2]).read(), json.load(f))
    gap = float(sys.argv[2]) if len(sys.argv) > 2 else 0.06
    return drive(ip, gap)


if __name__ == "__main__":
    sys.exit(main())
