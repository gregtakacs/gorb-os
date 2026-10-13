#!/usr/bin/env python3
"""Browse _orb._tcp with an INDEPENDENT mDNS stack and verify what it reports.

Why an independent stack: the fake in internal/orbfake and the browser in item 5
are both mine. If my encoder and my parser agree, that proves nothing — the exact
self-agreement failure that moved the mDNS advertiser off ladder item 3 in the
first place. zeroconf is a third party's RFC 6762 implementation: if IT can find
the orb, parse the TXT, and reach the address it hands back, the announcement is
real rather than self-consistent.

Usage: browse_mdns.py [--expect-ip 192.168.2.34] [--iface <ip-of-the-net-to-watch>]
Exit 0 when an orb is found whose TXT mac and A record both check out.
"""
import argparse, socket, sys, time
from zeroconf import Zeroconf, ServiceBrowser, ServiceStateChange, InterfaceChoice

ap = argparse.ArgumentParser()
ap.add_argument("--expect-ip", default=None,
                help="assert the A record equals this (the address /api/v1/status self-reports)")
ap.add_argument("--expect-mac", default=None,
                help="assert TXT mac= equals this (esptool read_mac on the bound orb)")
ap.add_argument("--seconds", type=float, default=12.0)
ap.add_argument("--iface", default=None, help="bind only this local address (a specific netif)")
ap.add_argument("--unicast-out", default=None,
                help="send the browse query to this address instead of the group. WSL2 "
                     "mirrored mode does not originate multicast onto the LAN, so a "
                     "plain browse dies in the host's own stack before the orb hears "
                     "it; unicast_out keeps the whole mDNS exchange inside zeroconf "
                     "and changes only the L2 destination. It is a workaround for "
                     "THIS bench, and the item-5 hub must not inherit it.")
args = ap.parse_args()

found = {}
zc_kwargs = {"interfaces": InterfaceChoice.Default}
if args.iface:
    zc_kwargs = {"interfaces": [args.iface]}
if args.unicast_out:
    zc_kwargs["unicast_out"] = [args.unicast_out]
try:
    zc = Zeroconf(**zc_kwargs)
except Exception as e:
    print(f"zeroconf could not bind ({args.iface} / unicast_out {args.unicast_out}): {e}")
    sys.exit(2)

def on_state_change(*args, **kwargs):
    """Tolerant of both zeroconf callback shapes: the historical
    (zeroconf, type, name, state_change) positional form and the keyword form
    this release fires. Getting this wrong looked like "not found" — the browse
    had already matched the announcement before my handler raised."""
    z = kwargs.get("zeroconf", zc)
    state = kwargs.get("state_change")
    name = kwargs.get("name")
    if state is None or name is None:
        for a in args:
            if isinstance(a, ServiceStateChange):
                state = a
            elif isinstance(a, str) and a.endswith("_orb._tcp.local."):
                name = a
        if not args:
            return
        z = args[0] if isinstance(args[0], Zeroconf) else z
    if state is not ServiceStateChange.Added or not name:
        return
    try:
        info = z.get_service_info("_orb._tcp.local.", name, timeout=4.0)
    except Exception as e:
        print(f"  {name}: get_service_info failed: {e}")
        return
    if info is None:
        found[name] = {"error": "no info"}
        return
    addrs = [socket.inet_ntop(socket.AF_INET, bytes(a)) for a in (info.addresses or [])]
    props = {}
    for k, v in (info.properties or {}).items():
        props[k.decode() if isinstance(k, bytes) else k] = (
            v.decode(errors="replace") if isinstance(v, (bytes, bytearray)) else v)
    found[name] = {"addresses": addrs, "port": info.port, "server": info.server,
                   "txt": props, "ttl": getattr(info, "ttl", None)}

browser = ServiceBrowser(zc, "_orb._tcp.local.", [on_state_change])
print(f"browsing _orb._tcp.local for {args.seconds:.0f} s"
      + (f" on {args.iface}" if args.iface else " on all interfaces"))
deadline = time.time() + args.seconds
while time.time() < deadline and not found:
    time.sleep(0.4)
time.sleep(1.0)   # let the SRV/TXT/A follow-ups land
zc.close()

if not found:
    print("NOT FOUND: no _orb._tcp answer arrived")
    sys.exit(1)

rc = 0
for name, d in found.items():
    print(f"SERVICE {name}")
    print(f"  addresses = {d.get('addresses')}  port = {d.get('port')}  server = {d.get('server')}")
    print(f"  txt       = {d.get('txt')}")
    if args.expect_ip:
        if d.get("addresses") and args.expect_ip in d["addresses"]:
            print(f"  OK: A record matches the address the device self-reports ({args.expect_ip})")
        else:
            print(f"  MISMATCH: expected A {args.expect_ip}, got {d.get('addresses')}")
            rc = 1
    if args.expect_mac:
        got = (d.get("txt") or {}).get("mac")
        if got == args.expect_mac:
            print(f"  OK: TXT mac matches esptool read_mac ({args.expect_mac})")
        else:
            print(f"  MISMATCH: TXT mac {got!r} != {args.expect_mac!r}")
            rc = 1
    if d.get("port") != 80:
        print(f"  MISMATCH: port {d.get('port')} is not 80")
        rc = 1
    for key in ("mac", "fw", "state"):
        if key not in (d.get("txt") or {}):
            print(f"  MISSING TXT key {key}")
            rc = 1
sys.exit(rc)
