"""Chrome DevTools Protocol client for the Orb Manager window.

The hub's control token lives in an HttpOnly cookie and is inlined into the page it
serves, so the only way to call a control door as the page is to ask the page. The hub
chooses a random fallback port when 3779 is taken, so the tab is matched by the port
argument, not by a hardcoded one. No websocket library is installed here, so the frame
coding is done by hand: masked text out, unmasked in.

  python3 orb_cdp.py 'ORB_TOKEN'                  evaluate in the live tab
  python3 orb_cdp.py reload                       hard-reload it, bypassing cache
  python3 orb_cdp.py 'document.title' 3779        name the hub port explicitly

The DevTools port comes from the browser profile the hub itself picks (window.go:
`<user-data-dir>/orb-manager/browser-profile/DevToolsActivePort`). Under WSL that is a
Windows path, so it is read through /mnt/c; override it with ORB_DEVTOOLS_PORT_FILE when
the profile lives elsewhere. `make cdp EXPR=…` and `make reload` wrap this script.
"""
import base64
import glob
import json
import os
import re
import socket
import struct
import sys
import urllib.request


def port_file():
    """The newest DevToolsActivePort whose browser actually answers.

    A port file outlives its browser: chrome.exe is single-instance per profile, so a
    hand-off launch leaves the previous instance's file behind, and its port is dead. The
    hub's own watcher discards exactly that file (awaitOpen), so this does too — otherwise
    the one thing that can tell you the window is unwatchable is the tool you just ran.
    """
    if env := os.environ.get("ORB_DEVTOOLS_PORT_FILE"):
        return env
    cands = [os.path.expanduser("~/.config/orb-manager/browser-profile/DevToolsActivePort")]
    cands += glob.glob("/mnt/c/Users/*/AppData/Local/orb-manager/browser-profile/DevToolsActivePort")
    cands += glob.glob("/mnt/c/Users/*/AppData/Roaming/orb-manager/browser-profile/DevToolsActivePort")
    live = []
    for c in cands:
        try:
            port = open(c).readline().strip()
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/version", timeout=1):
                live.append((os.path.getmtime(c), c))
        except Exception:
            continue
    if not live:
        raise SystemExit("no live DevTools endpoint for an Orb Manager window - a hub that "
                         "logged 'still looking' is blind for the same reason: close every "
                         "Chrome on that profile and reopen the window, or pass "
                         "ORB_DEVTOOLS_PORT_FILE")
    return max(live)[1]



def tabs(dpp):
    with urllib.request.urlopen(f"http://127.0.0.1:{dpp}/json", timeout=4) as r:
        return json.load(r)


def find_tab(dpp, hub_port):
    for t in tabs(dpp):
        if t.get("type") == "page" and f":{hub_port}" in t.get("url", ""):
            return t
    raise SystemExit(f"no Orb Manager window on port {hub_port}")


def connect(dpp, hub_port):
    tab = find_tab(dpp, hub_port)
    s = socket.create_connection(("127.0.0.1", int(dpp)), timeout=120)
    key = base64.b64encode(os.urandom(16)).decode()
    path = tab["webSocketDebuggerUrl"].split(f":{dpp}")[1]
    s.sendall(
        (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{dpp}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        ).encode()
    )
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(4096)
        if not chunk:
            raise SystemExit("devtools closed the handshake")
        resp += chunk
    if b"101" not in resp.split(b"\r\n")[0]:
        raise SystemExit("devtools refused the upgrade")
    return s


def send(s, obj):
    payload = json.dumps(obj).encode()
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    n = len(payload)
    head = bytes([0x81])
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    s.sendall(head + mask + masked)


def read_frame(s):
    def need(k):
        buf = b""
        while len(buf) < k:
            ch = s.recv(k - len(buf))
            if not ch:
                raise SystemExit("socket closed mid-frame")
            buf += ch
        return buf

    ln = need(2)[1] & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", need(2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", need(8))[0]
    try:
        return json.loads(need(ln).decode("utf-8", "replace")) if ln else None
    except Exception:
        return None


def call(s, mid, method, params=None):
    send(s, {"id": mid, "method": method, "params": params or {}})
    while True:
        m = read_frame(s)
        if m is None:
            continue
        if m.get("id") == mid:
            return m


def hub_port():
    for line in os.popen("ss -ltnp 2>/dev/null").read().splitlines():
        if "orb-manager" in line:
            m = re.search(r"127\.0\.0\.1:(\d+)", line)
            if m:
                return m.group(1)
    raise SystemExit("no orb-manager is listening")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    dpp = open(port_file()).readline().strip()
    expr = sys.argv[1]
    port = sys.argv[2] if len(sys.argv) > 2 else hub_port()
    s = connect(dpp, port)
    if expr == "reload":
        call(s, 3, "Page.reload", {"ignoreCache": True})
        print(f"reloaded the window on {port}")
        return 0
    call(s, 1, "Runtime.enable")
    out = call(s, 2, "Runtime.evaluate", {
        "expression": expr,
        "awaitPromise": True,
        "returnByValue": True,
        "timeout": 120000,
    })
    res = out.get("result", {})
    if res.get("exceptionDetails"):
        print("page threw:", json.dumps(res["exceptionDetails"])[:600])
        return 1
    print(json.dumps(res.get("result", {}).get("value"), indent=1)[:8000])
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
