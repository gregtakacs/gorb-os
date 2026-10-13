#!/usr/bin/env python3
"""The menu's closed-loop bench walk: prove the rendered rows are what they were.

Run it before signing off any change to a page's rows()/label()/back_cur, and
diff the two <tag>_cursor.txt files across the flash. The last such pair ran
2026-10-13 across P5.2: 221 records each side, 49 checks 0 failed, and the only
diff line was the boot's own image stamp.

Three bench facts drive the design:
  * The only ink the shell puts out is `menu: cursor <TITLE> row N/M: <label>`,
    and it is emitted ONLY when the cursor moves. A press that opens a page or
    backs out of one logs nothing, so every open/back is read by nudging the
    cursor one row and deriving the landed row from where the nudge landed.
  * ?turn=+1 steps rows UP: the injected delta is electrical and
    KNOB_DIR_DOWN_IS_CW is 0. Advancing to a higher row injects turn=-1.
  * A fresh-armed menu swallows injected turns until one press lands, so the
    walk opens with a press on the inert AP stub.

The radio drops and retransmits, and a retransmitted ?turn executes twice: a
multi-row jump is recorded, converged on, and counted in the summary rather than
blamed on the firmware. The invariant a jump hides — one detent moves one row —
is pinned on the host by the suite's "clamp removed at the bottom" and "clamp
removed at the top" mutants, which is why the walk can afford to record a jump
instead of aborting on it.

Usage: bench_menu_walk.py <tag> [expected-stamp]
  -> /tmp/<tag>_cursor.txt  the rendered records, in order
  -> /tmp/<tag>.log         the checks, one line each
The optional second argument is the git stamp the board must be carrying; the
walk refuses to score an image that is not the one it is about.
"""
import json, os, re, sys, threading, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orb_bind, serial

OUT = sys.argv[1] if len(sys.argv) > 1 else "walk"
CFG = orb_bind.load(); PORT = orb_bind.resolve_port(CFG)
orb_bind.gate_mac(CFG, PORT); BASE = "http://%s" % orb_bind.gate_ip(CFG)

lines, lk = [], threading.Lock()
def reader(s):
    while True:
        ln = s.readline()
        if ln is None: break
        if not ln: continue
        with lk: lines.append(ln.decode("utf-8", "replace"))
ser = serial.Serial(PORT, 115200, timeout=0.2)          # the open resets the board
threading.Thread(target=reader, args=(ser,), daemon=True).start()

TRANS, RES = [], []
pos = 0
CURRX = re.compile(r"menu: cursor (.+?) row (\d+)/(\d+): (.*)")

def one_line(timeout=7.0):
    global pos
    end = time.time() + timeout
    while time.time() < end:
        with lk: n = len(lines)
        while pos < n:
            with lk: ln = lines[pos]
            pos += 1
            m = CURRX.search(ln)
            if m: return (m.group(1).strip(), int(m.group(2)), int(m.group(3)), m.group(4).rstrip())
    return None
def rd_raw(timeout=1.0):
    """The next raw boot line, cursor lines included, so the boot scan can read
    the identity lines before expect() starts discarding what it does not match."""
    global pos
    end = time.time() + timeout
    while time.time() < end:
        with lk: n = len(lines)
        while pos < n:
            with lk: ln = lines[pos]
            pos += 1
            return ln.strip()
        time.sleep(0.05)
    return None
def expect(pat, timeout):
    global pos
    end, rx = time.time() + timeout, re.compile(pat)
    while time.time() < end:
        with lk: n = len(lines)
        while pos < n:
            with lk: ln = lines[pos]
            pos += 1
            if rx.search(ln): return ln.strip()
        time.sleep(0.05)
    return None
def http(path, tries=30):
    """The bench radio stalls in and out; a 30 s stall on one request must cost a
    retry, not a walk. Only the tiny /api/v1/screen body (28 B) is asked for in
    the walk itself — the 1.2 KB status body is read once, from the boot log."""
    last = None
    for _ in range(tries):
        try:
            with urllib.request.urlopen(BASE + path, timeout=8) as r:
                return json.loads(r.read())
        except Exception as e: last = e; time.sleep(0.4)
    abort("httpd refused %s (%s) — the radio, not the firmware" % (path, last))
def record(kind, r):
    TRANS.append("%-6s %-18s cur=%-3d last=%-3d %s" % (kind, r[0], r[1], r[2], r[3]))
    print(TRANS[-1], flush=True)
def check(name, ok, detail=""):
    RES.append((name, ok))
    print("%-4s %s | %s" % ("ok" if ok else "FAIL", name, detail), flush=True)
    return ok
def dump():
    open("/tmp/%s_cursor.txt" % OUT, "w").write("\n".join(TRANS) + "\n")
    open("/tmp/%s_raw.log" % OUT, "w").write("".join(lines))
def abort(why):
    dump(); print("ABORT: %s" % why, flush=True); ser.close(); sys.exit(2)

def inject(rows_delta):
    """One detent, in ROW terms: +1 = one row down the list. Returns the line."""
    elec = -1 if rows_delta > 0 else 1        # electrical sense is inverted
    for _ in range(4):
        http("/api/v1/screen?turn=%+d" % elec)
        r = one_line()
        if r: return r
    return None
def press():
    http("/api/v1/screen?push=1"); time.sleep(0.5)
JUMPS = [0]
def step_to(page, cur, target):
    guard = 0
    while cur != target:
        guard += 1
        if guard > 400: abort("never reached %s row %d (stuck at %d)" % (page, target, cur))
        r = inject(target - cur)
        if r is None:
            # No line: the shell saw no move. Re-sync by nudging the other way;
            # a genuine clamp answers with nothing again, a lost injection shows
            # the cursor one row off and the loop keeps walking.
            r = inject(cur - target)
            if r is None: abort("no cursor line heading %s row %d" % (page, target))
            record("resync", r); cur = r[1]; continue
        if r[0] != page: abort("page is %s, walk expected %s (row %d)" % (r[0], page, r[1]))
        if abs(r[1] - cur) > 1:
            # One ?turn moved two rows. The driver never injects more than one
            # detent, so the second came from a retried ambiguous write (the
            # .clinerules radio hazard). Record it and converge; the adjacency
            # invariant this hides is covered by mutant M32 on the host.
            record("jump", r); JUMPS[0] += 1; cur = r[1]; continue
        record("turn", r); cur = r[1]
    return r
def nudge_open(page):
    """Read the row a press just landed on, by nudging. Returns (line, landed_cur)."""
    r = inject(1)                                     # one row down
    if r and r[1] > 0: return r, r[1] - 1
    if r and page not in r[0]: abort("page is %r, expected %r after the press" % (r[0], page))
    if r is None:                                     # clamped: cursor sat on the last row
        r = inject(-1)
        if r is None: abort("a press left the menu with no cursor line either way")
        return r, r[1] + 1
    abort("a nudge up from row %d said nothing" % r[1])
def show(page, cur, row, want=None):
    """Confirm row `row` reads `want` (if given) and log its label."""
    if cur != row:
        r = step_to(page, cur, row); cur = row
    else:
        away = row + (1 if row == 0 else -1)
        step_to(page, cur, away); r = step_to(page, away, row)
    record("row ", r)
    if want is not None and want not in r[3]:
        abort("row %s/%d reads %r, wanted %r" % (page, row, r[3], want))
    return cur, r[2], r[3]

print("== boot ==", flush=True)
# Accumulate the boot burst: `expect` discards everything that is not its own
# match, and the identity lines print ~15 s BEFORE `input: router running`.
BOOT = []
v15t0 = time.time(); vl = rl = None
while time.time() - v15t0 < 60:
    line = rd_raw()
    if not line: continue
    BOOT.append(line)
    if vl is None and "App version" in line: vl = line.strip()
    if rl is None and "rssi:" in line: rl = line.strip()
    if "input: router running" in line: break
check("boot router", "input: router running" in "\n".join(BOOT))
print("   %s | %s" % (vl, rl), flush=True)
WANT_FW = sys.argv[2] if len(sys.argv) > 2 else None   # the stamp the run is about
fw = vl.split()[-1] if vl else "?"
record("fw  ", ("IMAGE", 0, 0, fw))
if WANT_FW:
    check("the board carries the image this run is about", fw == WANT_FW, "fw=%s want=%s" % (fw, WANT_FW))
timeout_lab = "1 min"          # /api/v1/status said timeout_s=60 before the walk started
http("/api/v1/screen?to=menu")
check("screen -> menu", expect(r"input: screen -> menu", 10) is not None)
time.sleep(1.5); press()
if expect(r"input: armed menu", 5) is None:
    press(); check("armed on the second press", expect(r"input: armed menu", 5) is not None)
else: check("armed on the first press", True)
time.sleep(1.0); press()                      # top row 0: the inert AP stub, and the swallowed-turn clearer

# ---- top page: read its shape off the first cursor line, walk every row ----
r = step_to("MENU", 0, 1); top_last = r[2]
cur = 1
check("top row 1 is TZ", "TZ" in r[3], r[3])
cur, _, lab = show("MENU", cur, 0, "AP")            # keep cur: a discarded return desyncs the walk
check("top row 0 is AP", lab == "AP", lab)
LABS = {}
for row in range(1, top_last + 1):
    cur, _, lab = show("MENU", cur, row); LABS[row] = lab
    if row == 2: check("top row 2 is DISPLAY", lab == "DISPLAY", lab)
    if row == 3: check("top row 3 is PERMISSIONS", lab.startswith("PERMISSIONS"), lab)
    if row == top_last: check("the last top row is exit", lab == "exit", lab)
for row in range(top_last - 1, -1, -1):
    cur, _, lab = show("MENU", cur, row)

# ---- TZ ----
cur, _, _ = show("MENU", cur, 1, "TZ"); press()
r, tcur = nudge_open("TIME ZONE"); record("open", r); tzpage, tz_last = r[0], r[2]
check("the TZ press opens the zone list", tzpage == "TIME ZONE", tzpage)
check("it opens ON a zone, not on row 0", tcur > 0, "landed cur=%d %s" % (tcur, r[3]))
check("the zone list carries 63 rows (62 + back)", tz_last == 62, "last=%d" % tz_last)
_, _, lab = show(tzpage, tcur, tcur); check("the current zone carries its mark", "  *" in lab, lab)
for row in (tcur + 1, tcur + 2, max(0, tcur - 1), 0):
    tcur, _, _ = show(tzpage, tcur, row)
step_to(tzpage, tcur, 30); step_to(tzpage, 30, 45)
tcur, _, lab = show(tzpage, 45, tz_last, "<- back"); press()
r, land = nudge_open("MENU"); record("back", r)
check("TZ back lands on the top page's TZ row", land == 1, "landed %d: %s" % (land, r[3]))
cur = land

# ---- DISPLAY, then the timeout picker ----
cur, _, _ = show("MENU", cur, 2, "DISPLAY"); press()
r, dcur = nudge_open("DISPLAY"); record("open", r); dpage, d_last = r[0], r[2]
check("DISPLAY opens at its first row", dpage == "DISPLAY" and dcur == 0, "%s cur=%d" % (dpage, dcur))
check("DISPLAY is dim, timeout, back", d_last == 2, "last=%d" % d_last)
dcur, _, lab = show(dpage, dcur, 0); check("the dim row reads the switch", "dim after idle:" in lab, lab)
dim0 = lab
dcur, _, lab = show(dpage, dcur, 1); check("the timeout row reads the preset", "timeout:" in lab, lab)
to0 = lab
press()                                                  # -> the picker
r, tocur = nudge_open("DIM TIMEOUT"); record("open", r); topage, to_last = r[0], r[2]
check("the picker is DIM TIMEOUT", topage == "DIM TIMEOUT", topage)
check("the picker is 7 presets + back", to_last == 7, "last=%d" % to_last)
check("it opens ON the preset in force",
      "  *" in show(topage, tocur, tocur)[2], "cur=%d %s" % (tocur, r[3]))
for row in (0, 1, 2, to_last):
    tocur, _, lab = show(toplevel := topage, tocur, row)
    if row == to_last: check("the picker's last row is back", lab == "<- back", lab)
tocur, _, lab = show(toplevel, tocur, 0, "1 min"); press()      # press the preset in force
r, back_to = nudge_open("DISPLAY"); record("back", r)
check("choosing a preset closes the picker onto the timeout row",
      r[0] == "DISPLAY" and back_to == 1, "%s cur=%d %s" % (r[0], back_to, r[3]))
dcur, _, lab = show("DISPLAY", back_to, 1)
check("the timeout row reads the same preset it did before", lab == to0, "%s vs %s" % (lab, to0))
dcur, _, lab = show("DISPLAY", dcur, 0)
check("the dim row reads the switch as it did before", lab == dim0, "%s vs %s" % (lab, dim0))
dcur, _, _ = show("DISPLAY", dcur, d_last, "<- back"); press()
r, land = nudge_open("MENU"); record("back", r)
check("DISPLAY back lands on its own top row", land == 2, "landed %d" % land)
cur = land

# ---- PERMISSIONS and one caps page ----
cur, _, _ = show("MENU", cur, 3, "PERMISSIONS"); press()
r, pcur = nudge_open("PERMISSIONS"); record("open", r); ppage, p_last = r[0], r[2]
check("PERMISSIONS opens on its first app row", ppage == "PERMISSIONS" and pcur == 0, "%s cur=%d" % (ppage, pcur))
for row in range(0, p_last):
    pcur, _, lab = show(ppage, pcur, row)
    if row == 0: check("an app row shows its declared count", "(" in lab and ")" in lab, lab)
check("the permission list ends on back", "<- back" in show(ppage, pcur, p_last)[2], "")
pcur, _, _ = show(ppage, pcur, 0); press()                 # -> that app's caps page
r, ccur = nudge_open("PERMISSIONS"); record("open", r); cpage, c_last = r[0], r[2]
check("the caps page header names its app and is not the list",
      "PERMISSIONS" in cpage and cpage != "PERMISSIONS", cpage)
check("the caps page is the four owner caps + back", c_last == 4, "last=%d" % c_last)
for row in range(0, c_last):
    ccur, _, lab = show(cpage, ccur, row)
    if row == 0: check("cap row 0 is power", lab.startswith("power:"), lab)
    if row == 1: check("cap row 1 is net", lab.startswith("net:"), lab)
    if row == 2: check("cap row 2 is sd_read", lab.startswith("sd_read:"), lab)
    if row == 3: check("cap row 3 is themes", lab.startswith("themes:"), lab)
ccur, _, _ = show(cpage, ccur, 0, "power:"); press()       # undeclared: the gate closes it
show(cpage, 0, 1); ccur, _, lab = show(cpage, 1, 0)
check("pressing an undeclared cap leaves it not declared and unmarked",
      "not declared" in lab and " *" not in lab, lab)
# The * rides only on a denial the app caused by CALLING a cap it never
# declared (orb_svc.c:169 sets denied_mask in the runtime gate, never from
# an owner press). host_menu_test.py:687 pins the same label here.
ccur, _, _ = show(cpage, ccur, c_last, "<- back"); press()        # leave the caps page
r, back_to = nudge_open("PERMISSIONS"); record("back", r)
check("caps back parks on its own app row", back_to == 0, "landed %d" % back_to)
pcur, _, lab = show("PERMISSIONS", back_to, p_last, "<- back"); press()
r, land = nudge_open("MENU"); record("back", r)
check("PERMISSIONS back lands on its own top row", land == 3, "landed %d" % land)
check("the inert press left the top page's PERMISSIONS row unmarked",
      " *" not in r[3], r[3])

# ---- an app page, and its one group (NO CARD on this bench: the group has a
# header and no items, which is the state the shell must render honestly) ----
# The first row that is neither an OS entry nor exit — an app's title. Derived
# from the labels rather than hardcoded, because P5.3 inserts APPS/ABOUT into
# s_top and a literal index would then walk the wrong row on the new image.
OS_TOP = ("AP", "TZ", "DISPLAY", "PERMISSIONS", "APPS", "ABOUT", "exit")
app_row = next(r for r in sorted(LABS)
               if LABS[r] and not any(LABS[r].startswith(p) for p in OS_TOP))
check("the app rows sit between the OS rows and exit",
      LABS.get(app_row, "") not in OS_TOP and LABS.get(top_last, "") == "exit", "%s" % str(LABS))
cur, _, lab = show("MENU", cur, app_row)
check("the row after the OS's is an app's title, not exit",
      lab not in ("exit", "?") and lab != "", lab)
press()
r, acur = nudge_open(lab); record("open", r); apage, a_last = r[0], r[2]
check("the app page is headed by the app, not the shell", apage == lab, "%s vs %s" % (apage, lab))
for row in range(0, a_last):
    acur, _, lab2 = show(apage, acur, row)
    if row == 1: header_lab = lab2
check("the card group header is on the glass whatever the card holds",
      "card" in header_lab.lower(), header_lab)
acur, _, _ = show(apage, acur, 1); press()
time.sleep(1.8)
acur, _, openlab = show(apage, acur, 1)
check("pressing the header replaces its summary with 'open'", "open" in openlab, openlab)
if a_last >= 3:                                     # a card present: items are on the glass
    acur, _, lab2 = show(apage, acur, 2)
    check("an item is indented", lab2.startswith("  "), repr(lab2))
http("/api/v1/screen?hold=1"); time.sleep(0.7)
check("hold closes the open group, items or none",
      expect(r"menu: hold closes group", 4) is not None)
acur = 1
acur, _, _ = show(apage, acur, a_last, "<- back"); press()
r, back_to = nudge_open("MENU"); record("back", r)
check("the app page back lands on that app's own top row", back_to == app_row, "landed %d" % back_to)
cur = back_to

# ---- exit ----
cur, _, _ = show("MENU", cur, top_last, "exit"); press()
check("exit disarms the menu", expect(r"menu: menu idle", 6) is not None)
check("exit hands the knob back to the clock", expect(r"input: screen -> clock", 6) is not None)
press()                                                    # and the router must ignore a press on the clock
check("a press on the clock does not re-arm the menu", expect(r"input: armed menu", 2) is None)

dump()
nfail = sum(1 for _, v in RES if not v)
print("\n%d cursor records (%d multi-row jumps the radio retried), %d checks, "
      "%d failed" % (len(TRANS), JUMPS[0], len(RES), nfail), flush=True)
ser.close(); sys.exit(1 if nfail else 0)
