#!/usr/bin/env python3
"""Turn session: drive the knob by hand, prove what the ISR actually counted.

Relocated from /tmp/turnsession.py (2026-10-11) so the device binding applies:
the orb IP comes from this clone's .orb-target and ip-neigh-gated, and the
serial port comes from the same binding behind an esptool read_mac gate. The
LAN half holds no port, so run this while the orb is otherwise idle.

Usage: python3 tools/turnsession.py
"""
import json, os, sys, time, urllib.request

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orb_bind

cfg = orb_bind.load()
IP = orb_bind.gate_ip(cfg)
BASE = "http://%s/api/v1" % IP

def get(p):
    with urllib.request.urlopen(BASE + p, timeout=6) as f:
        return json.loads(f.read())

# Wait for the radio, do not sleep a fixed time.
end = time.time() + 45
st = None
while time.time() < end:
    try:
        st = get("/status")
        if st.get("state") == "connected":
            break
    except Exception:
        pass
    time.sleep(1)
print("ready. baseline knob:", json.dumps(st["knob"]), flush=True)
print("", flush=True)
print(">>> Turn the knob about 10 notches in ONE direction, slowly, one click at a", flush=True)
print(">>> time. Then about 10 back. Take roughly 20 seconds total.", flush=True)

port_path = orb_bind.resolve_port(cfg)
orb_bind.gate_mac(cfg, port_path)
port = serial.Serial(port_path, 115200, timeout=0.3)
port.reset_input_buffer()
log = []
t_end = time.time() + 25
while time.time() < t_end:
    d = port.read(4096)
    if d:
        log.append(d.decode("utf-8", "replace"))
text = "".join(log)
open("/tmp/turnsession.log", "w").write(text)

after = get("/status")
print("", flush=True)
print("after the session:", json.dumps(after["knob"]), flush=True)
k = after["knob"]
print("", flush=True)
print("=== settle / valley lines from the serial log ===", flush=True)
for l in text.splitlines():
    if "knob" in l.lower() and "ready" not in l and "running" not in l:
        print(l, flush=True)
print("", flush=True)
if k["det"] == 0:
    print("VERDICT: no detents were seen at all — the encoder is not reaching the ISR.", flush=True)
else:
    ratio = k["raw"] / float(k["det"]) if k["det"] else 0
    print("VERDICT: %d detents from %d raw steps = %.1f raw per detent." %
          (k["det"], k["raw"], ratio), flush=True)
    print("spd10=%d (want ~40), valley hits=%d, settle hits=%d (want ~0)"
          % (k["spd10"], k["vhits"], k["shits"]), flush=True)
    if k["shits"] > max(1, k["vhits"] // 4):
        print(">> mostly settle hits: edges are being LOST between the pin and the ISR.", flush=True)
    print(">> rows moved should equal |det| = %d" % abs(k["det"]), flush=True)
