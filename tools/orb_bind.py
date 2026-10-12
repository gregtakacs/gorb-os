#!/usr/bin/env python3
"""Per-clone orb binding for the Python bench tools. The twin of orb_bind.sh.

Every device-facing tool calls load() before anything touches a device: a clone
without .orb-target refuses, an orb that is not attached refuses, and a chip
whose read_mac disagrees with the binding refuses before any port open or POST.
No /dev/ttyACMx fallback exists anywhere here -- a missing by-id path is a hard
error naming the orb it wanted.

The LAN gate trusts this host's ARP table (no mac field on /api/v1/status by
owner decision, 2026-10-11): right for the flat bench LAN, not a defense
against a hostile network.
"""
import os
import re
import subprocess
import sys

ESPTOOL_PY = os.environ.get(
    "ORB_ESPTOOL_PY", "/home/greg/.espressif/python_env/idf5.4_py3.14_env/bin/python")


def _die(msg):
    sys.exit("orb_bind: %s" % msg)


def repo_root():
    """The clone (or worktree) whose tools/ dir this module lives in."""
    here = os.path.dirname(os.path.abspath(__file__))
    r = subprocess.run(["git", "-C", here, "rev-parse", "--show-toplevel"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        _die("orb_bind.py is not inside a git working tree: " + here)
    return r.stdout.strip()


def load():
    """Parse <clone root>/.orb-target into a dict; refuse if bound to nothing."""
    root = repo_root()
    bind = os.path.join(root, ".orb-target")
    if not os.path.isfile(bind):
        _die("no %s -- this clone is unbound. Write ORB_LABEL, ORB_BASE_MAC, "
             "ORB_SERIAL (a /dev/serial/by-id path) and ORB_IP into it first." % bind)
    cfg = {"root": root, "_bind": bind}
    with open(bind) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, _, v = line.partition("=")
            cfg[k.strip()] = v.strip().strip('"')
    for k in ("ORB_LABEL", "ORB_BASE_MAC", "ORB_SERIAL"):
        if not cfg.get(k):
            _die("%s: %s missing" % (bind, k))
    cfg["ORB_BASE_MAC"] = cfg["ORB_BASE_MAC"].lower()
    return cfg


def resolve_port(cfg):
    """The bound orb's by-id path, or a refusal that names the missing device."""
    port = cfg["ORB_SERIAL"]
    if not os.path.exists(port):
        _die("orb %s is not attached: no %s (unplugged? usbipd not attached?). "
             "No other port was touched." % (cfg["ORB_LABEL"], port))
    return port


def gate_mac(cfg, port):
    """The chip itself must report the bound MAC. esptool performs the same
    reset cycle a flash does, so this costs the board a reboot it would take
    anyway -- and it is the only gate that survives a crossed cable."""
    r = subprocess.run([ESPTOOL_PY, "-m", "esptool", "--chip", "esp32s3",
                        "-p", port, "read_mac"], capture_output=True, text=True)
    m = re.search(r"^MAC: *([0-9a-fA-F:]{17})", r.stdout, re.M)
    if r.returncode != 0 or not m:
        _die("read_mac failed on %s:\n%s%s" % (port, r.stdout, r.stderr))
    have = m.group(1).lower()
    if have != cfg["ORB_BASE_MAC"]:
        _die("WRONG ORB: the chip on %s reports MAC %s, but %s is bound to %s. "
             "Crossed cable or stale by-id symlink; nothing was written."
             % (port, have, cfg["ORB_LABEL"], cfg["ORB_BASE_MAC"]))
    print("orb_bind: %s confirmed as %s (MAC %s)" % (port, cfg["ORB_LABEL"], have),
          file=sys.stderr)


def _mac_ok(have, want):
    """Equal, or the same nic's +1 low octet (STA/AP/BT variants)."""
    h, w = have.split(":"), want.split(":")
    if h[:5] != w[:5]:
        return False
    return (int(h[5], 16) - int(w[5], 16)) % 256 <= 1


def gate_ip(cfg, ip=None):
    """Prove ip (default: the binding's ORB_IP) answers as the bound orb per
    ip neigh, after one ping to refresh a stale entry. Returns the ip, or
    refuses naming the MAC that answered -- a DHCP handover to the other orb
    therefore stops the run instead of benching the wrong dial."""
    ip = ip or cfg.get("ORB_IP")
    if not ip:
        _die("%s has no ORB_IP in %s -- provision it onto the LAN first."
             % (cfg["ORB_LABEL"], cfg["_bind"]))
    want = cfg["ORB_BASE_MAC"]
    have = ""
    for _ in range(2):
        r = subprocess.run(["ip", "neigh", "show", ip], capture_output=True, text=True)
        m = re.search(r"lladdr ([0-9a-fA-F:]{17})", r.stdout)
        if m:
            have = m.group(1).lower()
            break
        subprocess.run(["ping", "-c1", "-W1", ip], capture_output=True)
    if not have:
        _die("no ARP answer from %s (%s) in two attempts" % (ip, cfg["ORB_LABEL"]))
    if not _mac_ok(have, want):
        _die("WRONG ORB on the LAN: %s answers as %s but %s is %s (+1 low octet "
             "tolerated). That is likely the OTHER orb's address -- fix ORB_IP "
             "in %s; do not route around this gate."
             % (ip, have, cfg["ORB_LABEL"], want, cfg["_bind"]))
    print("orb_bind: %s confirmed as %s (ARP %s)" % (ip, cfg["ORB_LABEL"], have),
          file=sys.stderr)
    return ip
