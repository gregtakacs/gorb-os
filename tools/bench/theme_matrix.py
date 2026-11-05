#!/usr/bin/env python3
"""theme_matrix.py — every baked template through the dial bench (plan §49.3).

For each <preset>.orbtheme in BAKES (tools/bench/bake_presets.py writes them): store it
on the bound orb (upload, automatic reboot onto the slot), clear the clock's reset
strikes, put the clock on the glass, and run the arms below through
GET /api/v1/debug/dial. Beside the device's figures it records estimate3's own price of
the same bytes and the inputs that price is built from (node, the shipped baker), so the
fit can be redone from the JSON alone.

Arms, 8 s each:
  shipped/box      the theme's own beat, swept-box marks, baked shadows (today's dial)
  shipped/spans    the same beat, ink spans
  shipped/nearest  the same beat, ink spans, nearest shadows
  max/box          a forced 30 fps beat with the seconds sweeping truly, box marks
  max/nearest      the same, ink spans and nearest shadows

Usage: tools/bench/theme_matrix.py BAKES OUT.json [preset ...]
Appends per theme as it goes, so an interrupted run keeps what it measured.
"""
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import orb_bind  # noqa: E402  (this clone's bound orb: .orb-target, never a literal address)
IP = orb_bind.gate_ip(orb_bind.load())
NODE = shutil.which("node") or os.path.expanduser("~/.local/share/nodeenv-gorb/bin/node")
ARMS = [
    ("shipped/box",     dict(spans=0, rate=0,  sweep=0, shadow="baked")),
    ("shipped/spans",   dict(spans=1, rate=0,  sweep=0, shadow="baked")),
    ("shipped/nearest", dict(spans=1, rate=0,  sweep=0, shadow="nearest")),
    ("max/box",         dict(spans=0, rate=30, sweep=1, shadow="baked")),
    ("max/nearest",     dict(spans=1, rate=30, sweep=1, shadow="nearest")),
]
ESTIMATE_JS = r"""
const fs = require('fs'), vm = require('vm');
const ctx = { window: {}, console, Uint8Array, Uint16Array, Uint32Array, Int16Array, Int32Array,
              DataView, ArrayBuffer, Math, Date, TextEncoder, TextDecoder, Error, JSON };
ctx.self = ctx.window; vm.createContext(ctx);
vm.runInContext(fs.readFileSync(process.argv[2], 'utf8'), ctx);
const G = ctx.window.GorbBake;
const sec = G.verifyBundle3(new Uint8Array(fs.readFileSync(process.argv[3]))).sections[0];
const e = G.estimate3(sec);
const sc = sec.blob.scalars;
e.scalars = { bg_mode: sc.bg_mode, gif_frames: sc.gif_frames, bg_frames: sc.bg_frames, bg_fps: sc.bg_fps };
e.rows = sec.blob.elements.filter((r) => r['class'] === 8).length;
process.stdout.write(JSON.stringify(e));
"""


def get(path, timeout=40):
    with urllib.request.urlopen("http://%s%s" % (IP, path), timeout=timeout) as r:
        return json.loads(r.read())


def post(path, data=None, timeout=120):
    req = urllib.request.Request("http://%s%s" % (IP, path), data=data or b"", method="POST",
                                 headers={"Content-Type": "application/octet-stream"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def wait_up(name, limit=150):
    t0 = time.time()
    time.sleep(8)
    while time.time() - t0 < limit:
        try:
            sd = get("/api/v1/sd", timeout=4)
            if sd.get("source") == "slot" and sd.get("active") == name and not sd.get("job_busy"):
                return True
        except Exception:
            pass
        time.sleep(3)
    return False


def estimate(path):
    js = os.path.join(os.path.dirname(os.path.abspath(path)), "_est.js")
    open(js, "w").write(ESTIMATE_JS)
    baker = os.path.join(ROOT, "tools", "orb-manager", "web", "runtime", "baker@1", "orbtheme-baker.js")
    return json.loads(subprocess.run([NODE, js, baker, path], capture_output=True, text=True, check=True).stdout)


def theme_name(path):
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import orbbundle as ob
    return ob.parse_header3(open(path, "rb").read())["name"]


def main():
    bakes, out = sys.argv[1], sys.argv[2]
    ids = sys.argv[3:] or sorted(f[:-9] for f in os.listdir(bakes) if f.endswith(".orbtheme"))
    res = json.load(open(out)) if os.path.exists(out) else {}
    for pid in ids:
        path = os.path.join(bakes, pid + ".orbtheme")
        name = theme_name(path)
        rec = {"preset": pid, "name": name, "estimate": estimate(path),
               "bake_status": open(os.path.join(bakes, pid + ".status")).read(), "arms": {}}
        up = post("/api/v1/sd/theme?overwrite=1", open(path, "rb").read())
        if up.get("state") not in ("stored", "unchanged") and "stored" not in json.dumps(up):
            print("%-10s upload refused: %s" % (pid, up), flush=True)
            continue
        if not wait_up(name):
            print("%-10s never came up on its slot" % pid, flush=True)
            continue
        try:
            post("/api/v1/apps/clear?id=clock", timeout=10)
        except Exception:
            pass
        time.sleep(12)
        get("/api/v1/screen?to=clock", timeout=10)
        time.sleep(5)
        try:
            for label, q in ARMS:
                qs = "&".join("%s=%s" % kv for kv in q.items())
                r = get("/api/v1/debug/dial?reset=1&%s&settle=2500&ms=8000" % qs)
                rec["arms"][label] = r
                p = r["per_commit"]
                print("%-10s %-16s l4 %d  %5.2f fps  hook %5.1f  dma %4.1f  te %4.1f  max %5.1f ms  restored %6.0f"
                      % (pid, label, r["l4"], r["commit_fps"], p["hook_us"] / 1e3, p["dma_us"] / 1e3,
                         p["te_us"] / 1e3, r["max_commit_us"] / 1e3, p["restored_px"]), flush=True)
        finally:
            get("/api/v1/debug/dial?reset=1&settle=0&ms=0")
        e = rec["estimate"]
        print("%-10s estimate3: est %s fps rung %s flush %s ms  box %s ink %s aa %s car %s glass %.2f"
              % (pid, e["est_fps"], e["rung"], e["flush_ms"], e["box_kpx"], e["ink_kpx"], e["aa_kpx"],
                 e["car_kpx"], e["glass_cov"]), flush=True)
        res[pid] = rec
        json.dump(res, open(out, "w"), indent=1)


if __name__ == "__main__":
    main()
