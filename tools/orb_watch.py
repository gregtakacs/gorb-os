#!/usr/bin/env python3
"""orb_watch.py - live feedback for a theme/app transfer or install, over HTTP only.

Run this while orb-manager (or curl, or the page) pushes a theme. It answers the one
question every other tool here leaves open: is anything actually happening? One line
per second shows installed-art state, card transfer state, free space and heap, and it
SHOUTS when the board reboots underneath the transfer, naming the reset that ended the
previous boot.

Why HTTP only and never the port: opening the bound ttyACM asserts DTR/RTS and resets
the S3 (capf.py exists to do that on purpose). A watcher that grabbed the port would
cause the failure it reports. The holder is read from /proc instead, and dmesg is
scanned for the usbip ECONNRESET signature: on a WSL bench a dropped serial tunnel
resets the chip, and /api/v1/status reports that boot as rst_name "usb". An upload
killed by that reset leaves no trace on the card - no .part, no refusal row. Without
this tool the owner sees silence and blames the network.
"""
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orb_bind  # noqa: E402  refuses an unbound clone and a crossed LAN entry

RESET_WHY = {
    "usb": "the USB-Serial-JTAG peripheral pulsed the chip into reset. The hub's own "
           "identity probe does this on purpose: launchprobe.go states 'this costs each "
           "board on a lead one reboot per hub launch', and it enters download mode by "
           "pulsing DTR/RTS (internal/esprom/reset.go). If a theme upload was in flight, "
           "the probe landed mid-write. Nothing to unplug: fix it by not letting the hub "
           "probe a cable that is receiving an install.",
    "sw": "a software restart: an install reboot or the RESET verb.",
    "panic": "a firmware panic. Read the console.",
    "task-wdt": "the task watchdog. Read the console.",
    "int-wdt": "the interrupt watchdog. Read the console.",
    "brownout": "a brownout. The supply cannot carry WiFi and SD writes together.",
    "poweron": "the board lost power.",
}


def get(base, path, timeout=2.0):
    import urllib.error
    import urllib.request
    try:
        with urllib.request.urlopen(base + path, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except (urllib.error.URLError, OSError, ValueError):
        return None


def port_holder(port_path):
    """PIDs with the bound tty open, from /proc - never by opening the port."""
    try:
        want = os.path.realpath(port_path)
    except OSError:
        return []
    out = []
    for pid in (p for p in os.listdir("/proc") if p.isdigit()):
        try:
            for fd in os.listdir("/proc/%s/fd" % pid):
                if os.path.realpath("/proc/%s/fd/%s" % (pid, fd)) == want:
                    try:
                        with open("/proc/%s/comm" % pid) as f:
                            name = f.read().strip()
                    except OSError:
                        name = "?"
                    out.append((pid, name))
                    break
        except OSError:
            continue                      # exited mid-walk
    return sorted(set(out), key=lambda t: int(t[0]))


def usb_resets(since_wall):
    """Host-side USB resets of any device since since_wall. None means unknown, and
    the caller must never read that as zero."""
    try:
        r = subprocess.run(["dmesg", "--time-format", "iso"],
                           capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    n = 0
    for line in r.stdout.splitlines():
        try:
            wall = time.mktime(time.strptime(line[:19], "%Y-%m-%dT%H:%M:%S"))
        except ValueError:
            continue
        if wall >= since_wall and re.search(
                r"vhci_hcd: urb->status -104|USB disconnect", line, re.I):
            n += 1
    return n


def explain(prev, cur, rst, holders, usbs, rebooted):
    """Plain-language verdict on the prev->cur transition. Returns lines to print."""
    msgs = []
    if prev and cur:
        if cur.get("art_ready") and not prev.get("art_ready"):
            msgs.append("ART LANDED: %s banks, %s pages." % (cur.get("art_banks"),
                                                             cur.get("art_pages")))
        elif prev.get("art_ready") and not cur.get("art_ready"):
            msgs.append("ART GONE: the installed theme is no longer readable. A reboot "
                        "during the install-write leaves the slot without a valid "
                        "bundle header, and migration then clears the stored selection.")
        if rebooted:
            msgs.append("BOARD REBOOTED. Reason that ENDED the previous boot: %s - %s"
                        % (rst, RESET_WHY.get(rst, "no explanation on file.")))
    if holders:
        msgs.append("Serial port is OPEN by %s. Holding the port is harmless on its own "
                    "(measured: a plain open/close and a held DTR neither reset the "
                    "board). What resets it is the hub's identity probe, which pulses "
                    "DTR/RTS into download mode on purpose — see RESET_WHY['usb']."
                    % ", ".join("%s(pid %s)" % (c, p) for p, c in holders))
    if usbs:
        msgs.append("%d host-side USB reset(s) since you started watching: the serial "
                    "tunnel is flapping and that is what kills transfers." % usbs)
    return msgs


def parse_args(argv):
    a = {"secs": 0, "every": 1.0, "expect": None, "port": None}
    i = 0
    while i < len(argv):
        k = argv[i]
        if k in ("--seconds", "-s"):
            a["secs"] = float(argv[i + 1]); i += 2
        elif k in ("--every", "-i"):
            a["every"] = float(argv[i + 1]); i += 2
        elif k == "--expect":
            a["expect"] = argv[i + 1]; i += 2
        elif k == "--port":
            a["port"] = argv[i + 1]; i += 2
        elif k in ("--help", "-h"):
            print(__doc__); sys.exit(0)
        else:
            sys.exit("orb_watch: unknown argument %s" % k)
    return a


def main():
    o = parse_args(sys.argv[1:])
    cfg = orb_bind.load()               # refuses an unbound clone
    ip = orb_bind.gate_ip(cfg)          # refuses the wrong orb on the LAN
    base = "http://%s" % ip
    port = o["port"] or cfg.get("ORB_SERIAL")
    t_start = time.time()
    prev = last_rst = None
    down = 0
    down_run = 0
    landed = False
    print("watching %s (%s) every %ss. Ctrl-C to stop."
          % (cfg.get("ORB_LABEL"), ip, o["every"]), file=sys.stderr)
    try:
        while True:
            st = get(base, "/api/v1/status")
            if st is None:
                down += 1
                if down in (1, 3, 10):
                    print("%s UNREACHABLE (poll %d): board down or radio not associated"
                          % (time.strftime("%T"), down))
            else:
                down_run = down          # a down->up turn is the reboot evidence
                down = 0
                # Reboot proof, any one of three: we could not reach it and now can; it
                # came back unsynced (a boot starts unsynced, and rst_name is fixed for
                # a boot's life so it cannot lie about the cause); or its SNTP clock went
                # backwards. epoch alone is NOT a boot counter -- after a re-sync it keeps
                # rising, which is what an earlier version of this file got wrong.
                rebooted = (down_run > 0
                            or (prev is not None and prev.get("synced")
                                and not st.get("synced"))
                            or (prev is not None
                                and int(st.get("epoch", 0)) < int(prev.get("epoch", 0))))
                if st.get("mac") and st["mac"].replace(":", "").lower() != \
                        cfg["ORB_BASE_MAC"].replace(":", ""):
                    print("%s WRONG ORB: %s answers as mac %s, binding is %s. Stopping."
                          % (time.strftime("%T"), ip, st["mac"], cfg["ORB_BASE_MAC"]))
                    sys.exit(2)
                sd = get(base, "/api/v1/sd") or {}
                rst = st.get("rst_name")
                card = ("NO CARD" if not sd.get("mounted") else
                        "card %s/%sGB" % (sd.get("free_mb"),
                                          round((sd.get("total_mb") or 0) / 1024.0)))
                xfer = "idle"
                if sd.get("uploading"):
                    xfer = "UPLOADING (%sB body)" % sd.get("bundle_bytes")
                elif sd.get("job_busy"):
                    xfer = "JOB BUSY refill %sB/%sms" % (sd.get("refill_bytes"),
                                                         sd.get("refill_ms"))
                elif sd.get("staged_id") or sd.get("try_id"):
                    xfer = "staged=%s try=%s" % (sd.get("staged_id") or "-",
                                                 sd.get("try_id") or "-")
                print("%s | %-9s | art=%s banks=%s pages=%s %sB | %-30s | %s | "
                      "%s | heap_int=%s largest=%s | rssi %s | rst=%s"
                      % (time.strftime("%T"), st.get("state"),
                         "Y" if st.get("art_ready") else "n",
                         st.get("art_banks"), st.get("art_pages"), st.get("art_bytes"),
                         xfer, card, sd.get("active") or "(none)",
                         st.get("heap_internal"), st.get("heap_internal_largest"),
                         st.get("rssi"), rst))
                for m in explain(prev, st, last_rst,
                                 port_holder(port) if port else [], usb_resets(t_start),
                                 rebooted):
                    print("    ^^ " + m)
                if sd.get("uploading"):
                    print("    ^^ UPLOAD IN FLIGHT. The hub's periodic identity probe will "
                          "reset this board mid-write and the card keeps nothing. Do not "
                          "refresh the port list or let the launch sweep run until the "
                          "transfer answers.")
                if sd.get("staged_why") or sd.get("try_why"):
                    print("    ^^ card refused a selection: try=%r staged=%r"
                          % (sd.get("try_why"), sd.get("staged_why")))
                if o["expect"] and sd.get("active") == o["expect"]:
                    landed = True
                    print("    ^^ EXPECTED THEME IS LIVE: %s" % o["expect"])
                last_rst = rst
                prev = st
            if o["secs"] and time.time() - t_start >= o["secs"]:
                break
            time.sleep(o["every"])
    except KeyboardInterrupt:
        print()
    if o["expect"]:
        print("expected theme %r: %s" % (o["expect"], "LIVE" if landed else "NEVER LANDED"))
        sys.exit(0 if landed else 3)


if __name__ == "__main__":
    main()
