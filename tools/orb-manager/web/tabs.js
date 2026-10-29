// tabs.js — the orb view's six faces, dormant until opened.
//
// app.js owns the page's bones; this file owns the tab SHAPE: the router, the
// one-request-per-tab-per-board discipline, the /timezone picker ported to the
// manager, the apps ledger with its sub-tabs, the card file manager, and the
// LAN arm of the reset dialog. It loads after app.js and rebinds the few
// handlers whose old single-tab behavior it replaces (the reset button, the
// Wi-Fi face's availability). Every request this file makes answers to a tab the
// operator opened for THIS board — no face prefetches another's data.
"use strict";

let tabNow = "orb";
const tabLoaded = new Set();
let fwStamp = "";              // status's fw line, for the doors that gate on it

// tabOffered is the smaller question tabPaneVisible never answers: does this
// board have the means to serve this face at all? Wi-Fi is the only face with
// an outward requirement — with no address there is nothing to point a scan or
// a join at, and the button is withdrawn rather than opened onto a dead picker.
// A cable-only board is provisioned from the New Orb flow over the lead, which
// is a different door on a different face, and it stays reachable there.
function tabOffered(name) {
  if (name === "wifi") return !!current.ip;
  return true;
}

// showTab is the whole router: it withdraws unofferable buttons, keeps the
// operator's choice when it can be honored, and falls back to the Orb face when
// it cannot (the common arrival is a board losing its lease under an open Wi-Fi
// face, which orbTabsStatus reports here on the next status read).
function showTab(name) {
  if (!tabOffered(name)) name = "orb";
  tabNow = name;
  document.querySelectorAll("#orb .tabpane").forEach((p) => {
    p.hidden = !tabPaneVisible(p, name);
  });
  document.querySelectorAll("#orb-tabs .tab").forEach((b) => {
    b.hidden = !tabOffered(b.dataset.tab);
    b.classList.toggle("on", b.dataset.tab === name);
  });
  // The press itself is news to the hub's read-ahead pass, even when the face
  // needs no fetch here to draw it (the tab was already loaded for this board).
  // The POST carries which face the hand just chose; the pass reorders its
  // remaining card reads around it at its next dial. Fire and forget at face
  // value: a lost beat means one pass keeps its old order, never a broken face.
  if (current.ip) {
    themePost("/api/theme/warm", { ip: current.ip, face: name }, 8000).catch(() => {});
  }
  if (name === "apps") lazyTab("apps", loadApps);
  else if (name === "themes") lazyTab("themes", themeOpen);
  else if (name === "files") lazyTab("files", fmOpen);
  else if (name === "wifi") lazyTab("wifi", wifiOpen);
  else if (name === "orb") lazyTab("tz", loadZonesTable);
}

// wifiOpen is the Wi-Fi face's first request, and it asks for the cached table
// rather than fresh: the board runs a scan at boot and holds the result until
// someone re-arms it, so opening the face costs one cheap read, and the Rescan
// button is what pays for a new sweep of the air.
function wifiOpen() {
  return loadNetworks(false);
}

// themeOpen is the Themes face's first request. The listing is the device's own
// (the hub's /api/theme/list wraps GET /api/v1/sd), and app.js owns the card:
// previews, the store press, the refusal witnesses. This face only decides WHEN
// the read happens — on a press, never on the Apps face's coattails.
function themeOpen() {
  return loadThemes(current.ip);
}

function tabPaneVisible(p, name) {
  return p.dataset.tab === name;
}

function lazyTab(key, fn) {
  if (tabLoaded.has(key) || !current.ip) return;
  tabLoaded.add(key);
  Promise.resolve().then(fn).catch(() => tabLoaded.delete(key));
}

// tabRearm is the answer to "the orb just came back". A board that went silent
// and now answered was rebooted by somebody — an update the hub staged, a
// factory reset, a radio that dropped. Every face loaded across that silence
// holds facts about a boot that has ended, so app.js's return-from-wake clears
// the ledger here and re-fires the face under the operator's hand. The other
// faces are simply un-loaded: they fetch fresh the moment they are pressed.
function tabRearm() {
  tabLoaded.clear();
  showTab(tabNow);
}

// orbTabsReset re-arms dormancy whenever a DIFFERENT board opens: last orb's
// apps, zones, files and themes are this orb's facts, and a stale paint is a
// wrong paint. app.js calls it from openOrbByIP before the first loadOrb.
function orbTabsReset() {
  tabLoaded.clear();
  stopZoneWake();                 // last orb's zone wait is not this orb's promise
  themeFaceReset();
  appList = null; appsSub = "clock"; cardApps = []; appData = null;
  fmPath = "";
  fwStamp = "";
  tabNow = "orb";
  // A typed SSID from the last board is that board's intent, not this one's.
  $("#j-manual").value = "";
  document.querySelectorAll("#orb .tabpane").forEach((p) => { p.hidden = true; });
  document.querySelectorAll("#orb-tabs .tab").forEach((b) => {
    b.hidden = !tabOffered(b.dataset.tab);
    b.classList.toggle("on", b.dataset.tab === "orb");
  });
}

// orbTabsStatus is loadOrb's hook: the stamp gates the LAN reset door, the
// open faces refresh themselves against the truth this read carries, and the
// shape re-asserts — a board that lost its address loses the Wi-Fi face out
// from under whoever was standing on it, and lands back on Orb.
function orbTabsStatus(st) {
  fwStamp = (st && st.fw) || "";
  showTab(tabNow);   // re-assert the shape; the faces themselves refresh on press
}

document.querySelectorAll("#orb-tabs .tab").forEach((b) => {
  b.onclick = () => showTab(b.dataset.tab);
});

// ---- face: Orb — the board's name, writable here now ---------------------------
$("#mn-name-go").onclick = async () => {
  const note = $("#mn-name-note"), btn = $("#mn-name-go");
  const name = $("#mn-name").value.trim();
  if (!current.ip) { note.textContent = "no address to write to"; return; }
  if (!name) { note.textContent = "the board will not carry an empty name"; return; }
  btn.disabled = true;
  note.textContent = "writing to the board\u2026";
  try {
    const r = await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/name", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name }),
    });
    const d = await readDeviceJSON(r);
    if (!r.ok || d.error) throw new Error(d.error || ("HTTP " + r.status));
    orbAdoptName(d.name || name);
    paintOrbHead();   // the headline follows the write, not just the field below it
    note.textContent = "saved \u2014 the board answers to " + current.name;
    pullFound();
  } catch (e) {
    note.textContent = "the board refused it: " + e.message;
  } finally {
    btn.disabled = false;
  }
};

// ---- face: Orb — the /timezone page's picker, ported ----------------------------
// The zone table comes from the board itself (GET /api/v1/config), exactly what
// the orb's own page reads; the GMT labels come from this browser, because the
// orb's newlib has no zoneinfo and every desktop browser has the full IANA
// data. The phone row synthesizes POSIX rules for a zone the table does not
// carry — the orb page's own bisect and two-year cross-check, ported unchanged
// in behavior — and every write is verified by the device's echoed measured
// offset, never by the word the page sent.
let tzRows = null;
let tzCur = "UTC";
let tzPhone = "";
let tzWant = "";
const _tzoc = {};
function tzOffAt(zn, ms) {
  const k = zn + "|" + Math.floor(ms / 864e5), c = _tzoc[k];
  if (c !== undefined) return c;
  let o = null;
  try {
    const a = new Intl.DateTimeFormat("en-US", { timeZone: zn, hourCycle: "h23", year: "numeric",
      month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit"
    }).formatToParts(new Date(ms));
    const p = {};
    for (const x of a) p[x.type] = x.value;
    let h = parseInt(p.hour, 10); if (h === 24) h = 0;
    o = Math.round((Date.UTC(+p.year, p.month - 1, +p.day, h, +p.minute, +p.second) - ms) / 60000);
  } catch (e) { o = null; }
  _tzoc[k] = o;
  return o;
}
const tzOffMin = (zn) => tzOffAt(zn, Date.now());
function tzHm(m) {
  if (m == null || isNaN(m)) return "GMT?";
  const s = m < 0 ? "-" : "+";
  m = Math.abs(m);
  return "GMT" + s + String("0" + Math.floor(m / 60)).slice(-2) + ":" + String("0" + (m % 60)).slice(-2);
}
const tzName = (z) => z.iana || z.name;
const tzLbl = (z) => tzHm(tzOffMin(tzName(z))) + " - " + (z.label || z.name);
function tzPhoneZone() {
  try { return Intl.DateTimeFormat().resolvedOptions().timeZone || ""; } catch (e) { return ""; }
}
// Name first (exact IANA); offset second, and only when it names exactly one
// zone — guessing between Denver and Phoenix is how clocks get quietly wrong.
function tzMatchPhone() {
  if (!tzPhone || !tzRows) return null;
  for (const z of tzRows) if (z.iana === tzPhone || z.name === tzPhone) return z;
  const po = -new Date().getTimezoneOffset();
  let hit = null, n = 0;
  for (const z of tzRows) {
    const o = tzOffMin(tzName(z));
    if (o != null && o === po) { n++; hit = z; }
  }
  return n === 1 ? hit : null;
}
// Synthesis for a zone the table does not carry: bisect each monthly window to
// the minute by offset, encode last-weekday-of-month as the .5 C libraries read
// as "last", cross-check the next two years, and refuse (null) anything whose
// pattern moves — the fixed-offset row is that zone's honest fallback.
function tzFoff(e) {
  const w = -e, x = w < 0 ? "-" : "", a = Math.abs(w);
  const h = Math.floor(a / 60), r = a % 60;
  return x + h + (r ? ":" + ("0" + r).slice(-2) : "");
}
function tzTransY(zn, y) {
  const out = [];
  for (let m = 0; m < 12; m++) {
    const a0 = Date.UTC(y, m, 1) / 6e4;
    const a1 = (m === 11 ? Date.UTC(y + 1, 0, 1) : Date.UTC(y, m + 1, 1)) / 6e4;
    const o0 = tzOffAt(zn, a0 * 6e4), o1 = tzOffAt(zn, a1 * 6e4);
    if (o0 == null || o1 == null) return null;
    if (o0 === o1) continue;
    let lo = a0, hi = a1;
    while (hi - lo > 1) {
      const mid = lo + ((hi - lo) >> 1);
      if (tzOffAt(zn, mid * 6e4) === o0) lo = mid; else hi = mid;
    }
    out.push([hi * 6e4, o0, o1]);
  }
  return out;
}
function tzMrule(t, pre) {
  const d = new Date(t + pre * 6e4);
  const day = d.getUTCDate();
  const dim = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth() + 1, 0)).getUTCDate();
  const wd = d.getUTCDay(), wk = (day + 7 > dim) ? 5 : Math.floor((day - 1) / 7) + 1;
  const hh = d.getUTCHours(), mm = d.getUTCMinutes(), ss = d.getUTCSeconds();
  const tt = (hh === 2 && mm === 0 && ss === 0) ? ""
    : (hh === 0 && mm === 0 && ss === 0) ? "/0"
    : "/" + hh + (mm || ss ? ":" + ("0" + mm).slice(-2) : "") + (ss ? ":" + ("0" + ss).slice(-2) : "");
  return "M" + (d.getUTCMonth() + 1) + "." + wk + "." + wd + tt;
}
function tzSym(zn) {
  const c = String(zn).split("/").pop().replace(/[^A-Za-z0-9]/g, "").toUpperCase();
  return (c || "X").slice(0, 5);
}
function tzGmtRule(m) {
  const s = m >= 0 ? "-" : "+";
  m = Math.abs(m);
  const h = Math.floor(m / 60), r = m % 60;
  return "GMT" + s + h + (r ? ":" + ("0" + r).slice(-2) : "");
}
function tzSynth(zn) {
  const y = new Date().getUTCFullYear(), ts = tzTransY(zn, y);
  if (!ts || ts.length !== 2) return null;
  const a = ts[0], b = ts[1];
  const std = a[1] < b[1] ? a[1] : b[1], dst = a[1] < b[1] ? b[1] : a[1];
  if (std === dst) return null;
  const td = a[2] === dst ? a : b, tsd = a[2] === dst ? b : a, nm = tzSym(zn);
  const parts = [nm + tzFoff(std) + nm + "D" + tzFoff(dst), tzMrule(td[0], td[1]), tzMrule(tsd[0], tsd[1])];
  for (let k = 1; k <= 2; k++) {
    const o = tzTransY(zn, y + k);
    if (!o || o.length !== 2) return null;
    const p0 = o[0][2] > o[0][1]
      ? [tzMrule(o[0][0], o[0][1]), tzMrule(o[1][0], o[1][1])]
      : [tzMrule(o[1][0], o[1][1]), tzMrule(o[0][0], o[0][1])];
    if (p0[0] !== parts[1] || p0[1] !== parts[2]) return null;
  }
  const r = parts[0] + "," + parts[1] + "," + parts[2];
  return r.length > 64 ? null : r;
}
// The zone table comes from the board itself, so it can die exactly the way the
// board's other slow reads die: dialing while the dial is mid-boot or mid-theme-
// draw, its single-task httpd accepting the connection and answering nothing
// until the 8 s device budget gives up (owner, 2026-10-28: connected onto an orb
// that had just rebooted onto a stored theme; status answered, this dial timed
// out). app.js's orbWake only re-dials when the STATUS read fails, so a board
// that answers status and starves config left this card parked on the old
// sentence's false promise of a reload that no mechanism owned. This ladder is
// that missing mechanism — same shape, same 5 s beat, retired the moment the
// card opens another orb or the table lands.
let zoneWakeTmr = 0;
let zoneWakeTok = "";
let zoneWakeN = 0;
const ZONE_WAKE_MAX = 24;   // ≈ a boot, SNTP and a big theme draw, at 5 s a beat

function stopZoneWake() {
  if (zoneWakeTmr) { clearTimeout(zoneWakeTmr); zoneWakeTmr = 0; }
}

async function loadZonesTable(retry) {
  if (!current.ip) return;
  if (!retry) { stopZoneWake(); zoneWakeN = 0; }   // a fresh read starts a new wait
  const cur = $("#tz-cur");
  cur.textContent = "reading the board's zone table\u2026";
  try {
    const d = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/config"));
    if (!d || !d.zones || !d.zones.length) throw new Error("the board answered no zones");
    tzRows = d.zones;
    tzCur = d.tz || "UTC";
    tzPhone = tzPhoneZone();
    cur.textContent = "the orb now shows " + tzCur + " \u00b7 " + d.zones.length + " zones it knows by name";
    renderTzList();
    tabLoaded.add("tz");   // a wake re-dial that lands restores the mark lazyTab dropped
    stopZoneWake();
  } catch (e) {
    // A card parked on "reading…" forever is what the owner saw during an
    // update: the dial was mid-reboot, the fetch died, and the silence wore
    // the costume of a page still working. Name the failure, re-dial on the
    // boot's clock, and re-throw so lazyTab drops its mark and a press works
    // even mid-ladder.
    cur.textContent = "the zone table did not load: " + (e && e.message ? e.message : String(e)) +
      " \u2014 the orb may be mid-boot; re-reading it every 5 s";
    const tok = current.mac || current.ip;
    if (tok !== zoneWakeTok) { zoneWakeTok = tok; zoneWakeN = 0; }   // a different card resets the wait
    if (zoneWakeN < ZONE_WAKE_MAX && !zoneWakeTmr) {
      zoneWakeTmr = setTimeout(() => {
        zoneWakeTmr = 0;
        if ((current.mac || current.ip) !== zoneWakeTok) return;     // a different card is open now
        if (++zoneWakeN > ZONE_WAKE_MAX) {
          cur.textContent = "the zone table stopped re-reading \u2014 press the Orb face when the board answers";
          return;
        }
        loadZonesTable(true).catch(() => {});
      }, 5000);
    }
    throw e;
  }
}

function renderTzList() {
  const box = $("#tz-list");
  box.innerHTML = "";
  if (!tzRows) return;
  const f = (tzWant || "").toLowerCase();
  const list = tzRows.filter((z) => !f
    || (z.label || "").toLowerCase().includes(f)
    || z.name.toLowerCase().includes(f)
    || (z.iana || "").toLowerCase().includes(f));
  list.sort((a, b) => {
    let x = tzOffMin(tzName(a)), y = tzOffMin(tzName(b));
    if (x == null) x = -9999;
    if (y == null) y = -9999;
    return x - y || String(a.label || a.name).localeCompare(String(b.label || a.name));
  });
  for (const z of list)
    box.appendChild(tzRowEl(tzLbl(z), () => tzSet(z.name, z.label || z.name), z.name === tzCur ? "cur" : ""));
  if (tzPhone && !tzMatchPhone()) {
    const po = -new Date().getTimezoneOffset(), city = tzSym(tzPhone), r = tzSynth(tzPhone);
    if (r) box.appendChild(tzRowEl(tzHm(po) + " - " + city + " (this browser's zone, follows its clock changes)",
      () => tzSet(r, city + " from this browser", po), "phone"));
    else box.appendChild(tzRowEl(tzHm(po) + " - Same as this browser (does not follow DST automatically)",
      () => tzSet(tzGmtRule(po), "browser time", po), "phone"));
  }
}

function tzRowEl(txt, on, cls) {
  const b = document.createElement("button");
  b.type = "button";
  b.textContent = txt;
  b.className = "tzrow" + (cls ? " " + cls : "");
  b.onclick = on;
  return b;
}

async function tzSet(v, txt, want) {
  const msg = $("#tz-msg");
  msg.textContent = "setting " + txt + "\u2026";
  try {
    const d = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/config", {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ tz: v }),
    }));
    if (!d || d.error) throw new Error((d && d.error) || "the orb refused it");
    tzCur = d.tz;
    renderTzList();
    if (want != null && d.tz_offset === want)
      msg.textContent = "orb now shows " + tzHm(want) + " \u2014 matches this browser";
    else if (want != null)
      msg.textContent = "orb shows " + tzHm(d.tz_offset) + ", not the " + tzHm(want) + " asked for \u2014 that zone did not stick";
    else msg.textContent = "orb now shows " + tzHm(d.tz_offset);
    pullFound();
  } catch (e) {
    msg.textContent = "the orb refused it: " + e.message;
  }
}

$("#tz-q").addEventListener("input", function () { tzWant = this.value; renderTzList(); });
$("#tz-loc").onclick = () => {
  const m = tzMatchPhone();
  if (m) { tzSet(m.name, m.label || m.name, -new Date().getTimezoneOffset()); return; }
  renderTzList();   // makes the phone row visible: it is the answer to press
  $("#tz-msg").textContent = tzPhone
    ? "this browser says " + tzPhone + " \u2014 press the row at the end of the list"
    : "this browser will not say where it is";
};

// ---- face: Apps — one sub-tab per app the LEDGER carries --------------------
// GET /api/v1/apps answers with two arrays, and the first cut of this face read
// the wrong one. `installed` is what stands on the dial — every used slot, the
// built-in shell rows included; `apps` is only the /sd/apps drop zone, and it is
// empty precisely when the dial is full. Reading `apps` as the ledger is why a
// board running clock, HSB and menu reported "0 installed apps" to its owner.
// The button matrix mirrors the device's own /apps page: a compiled row is never
// togglable or uninstallable, a failed row gets Clear fault, the face grows a hub
// Install row for the starter pair (D36), and the drop zone is the only place an
// Install button lives.
// Every verb is the device's own door; the face renders and asks, and the door's
// answer is the last word.
let appList = null;   // the installed tier, in ledger order
let cardApps = [];    // the drop-zone candidates sitting on the card
let appData = null;   // the whole answer, re-read after every press
let appsSub = "clock";
// D36 §7: `menu` left this table when the shell stopped being an app the face offers.
// A label table for rows the operator can never act on was the second half of the lie —
// it made the shell look like a fourth app of the same kind as the other three.
const APP_TITLES = { clock: "Clock", color_picker: "HSB" };

async function loadApps() {
  const state = $("#mn-appstate");
  if (!current.ip) { state.textContent = "no address to read"; return; }
  state.textContent = "reading the app ledger\u2026";
  const d = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/apps"));
  if (!d || d.error) throw new Error((d && d.error) || "the app list did not answer");
  appData = d;
  // §7: the shell leaves the button matrix. D35 left ONE compiled row and it is always on,
  // never togglable and never uninstallable, so a sub-tab for it could only ever hold the
  // words "built into the firmware" — a row that answers no question. It stays visible when
  // it FAILED, because a quarantined row is the one firmware state an owner can act on:
  // Clear fault. The reader still reports the ledger whole; only this face filters.
  const rows = Array.isArray(d.installed) ? d.installed : [];
  appList = rows.filter((a) => !a.compiled || a.failed);
  cardApps = Array.isArray(d.apps) ? d.apps : [];
  // The one honest sentence the header gets: what the dial runs and what the
  // card holds. Built-ins are not counted, directories are not named, and the
  // hub's shelf inventory has no business being half of a status line.
  state.textContent = appList.length + " app" + (appList.length === 1 ? "" : "s")
    + " on the dial" + (cardApps.length ? ", " + cardApps.length + " on the card" : "");
  renderAppsTab();
  // The library rows judge themselves against `appList` — that is what decides Install
  // versus Upgrade versus shelf-delete — so a ledger read that leaves the shelf face
  // holding the previous answer shows a button for a state the dial no longer has. The
  // first load of the two is order-free, and every later one re-synchronises here.
  if (libCache && typeof renderLibrary === "function") {
    try { await renderLibrary(); } catch (e) { /* renderLibrary reports its own failure */ }
  }
}

// appPost is the one leg every apps mutation walks: the proxy's orb path, an
// empty JSON body (the device reads only the query), and the device's own
// sentence as the thrown error — the face never re-cuts a refusal.
async function appPost(path) {
  const r = await hubFetch("/orb/" + encodeURIComponent(current.ip) + path, {
    method: "POST", headers: { "Content-Type": "application/json" }, body: "{}",
  });
  const d = await readDeviceJSON(r);
  if (!d || d.error) throw new Error((d && d.error) || ("HTTP " + r.status));
  return d;
}

function renderAppsTab() {
  const body = $("#mn-apps-body");
  $("#mn-apps-tabs").innerHTML = "";
  body.innerHTML = "";
  if (!appList) return;
  // D38: one list, every app the hub can see in any of its three homes — the
  // dial's ledger, the card's folder, the hub's shelf. The section labels are
  // gone because they were the same fact three times: an app is one thing, and
  // the row says where it stands and what the press would do.
  const seen = new Map();
  for (const a of appList)
    seen.set(a.slug, { slug: a.slug, title: a.title || APP_TITLES[a.slug] || a.slug, installed: a });
  for (const c of cardApps) {
    const k = c.slug || c.stem;
    const e = seen.get(k) || { slug: k, title: c.title || c.stem };
    e.card = c;
    seen.set(k, e);
  }
  for (const f of (libCache && libCache.apps) || []) {
    const e = seen.get(f.slug) || { slug: f.slug, title: f.title || f.slug };
    e.lib = f;
    seen.set(f.slug, e);
  }
  const rows = Array.from(seen.values()).sort((x, y) =>
    String(x.title).localeCompare(String(y.title)));
  if (!rows.length) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = "nothing to install \u2014 the dial has no apps, its card holds none, "
      + "and the hub's shelf is empty. Import one below.";
    body.appendChild(p);
  }
  for (const e of rows) body.appendChild(appRow(e));
  // Refused shelf files and the resolved directory lines print under the list:
  // they are the hub's own inventory report, not an app the operator can act on.
  renderLibrary();
}

// updateGate is D38's Update test, and it is strict by rule: the press is owed
// only when the shelf names a release and every copy already present (a card
// file's stamp, the dial slot's stamp) is older than it. A copy that states no
// stamp is silence, not agreement: it does not block the press, because the
// press overwrites it. No copy at all is NOT an update \u2014 that row's button is
// Install, and the face never shows two verbs for one fact.
function updateGate(e) {
  if (!e.lib || !e.lib.version) return false;
  if (!e.card && !e.installed) return false;
  if (e.card && e.card.version && verCompare(e.card.version, e.lib.version) >= 0) return false;
  if (e.installed && e.installed.version && verCompare(e.installed.version, e.lib.version) >= 0) return false;
  return true;
}

function appStateSentence(e) {
  const a = e.installed, c = e.card, f = e.lib;
  let t = (e.title || e.slug) + " \u00b7 " + e.slug;
  if (a) {
    if (a.compiled) t += a.failed ? " \u00b7 built in, quarantined at boot" : " \u00b7 built into the firmware";
    else if (a.failed) t += " \u00b7 failed: " + (a.status || "the dial did not say why");
    // A slot the dial holds is an INSTALLED app — on the rolodex, pressable.
    // The ledger's enabled word is the dial's own switch and not a hub state:
    // this face stopped offering a switch in D38, so nothing here turns a row
    // off, and a row that reads off on a board is a row this press will fix.
    else t += " \u00b7 installed" + (a.version ? " at " + a.version + libUpdateSuffix(a) : " at an unstamped release");
    if (!a.compiled && a.caps) t += " \u00b7 may use: " + a.caps;
  }
  if (c) t += c.ok
    ? " \u00b7 on the card" + (c.version ? " at " + c.version : "")
    : " \u00b7 the card refuses it: " + (c.why || "no reason given");
  if (f) t += " \u00b7 in the hub library at " + (f.version || "no stamp");
  if (updateGate(e)) t += " \u00b7 update available (" + f.version + ")";
  return t;
}

function appRow(e) {
  const a = e.installed, c = e.card, f = e.lib;
  const row = document.createElement("div");
  row.className = "row";
  const txt = document.createElement("span");
  txt.className = "hint";
  txt.textContent = appStateSentence(e);
  row.appendChild(txt);
  const named = e.title || e.slug;
  // The button set IS the state, and Install and Uninstall are mutually
  // exclusive BY CONSTRUCTION: the dial holds a slot or it does not.
  //   Install          no slot, and the shelf or a readable card file can make one
  //   Update           a copy exists and the shelf provably holds a newer release
  //   Uninstall        a slot the dial holds (installed means on the rolodex —
  //                    the hub never keeps an installed-but-off state, D38)
  //   Clear fault      a quarantined row, built-in tier included
  //   Add to card      the shelf holds it and the card does not
  //   Remove from card the card holds it (and uninstalls first if the dial runs it)
  //   Remove from hub library the shelf holds it; no dial or card is touched
  // When the Update gate is true the row already offers to bring this app onto
  // the dial with the shelf's newer bytes, so Install stands down: one verb
  // per fact, never two buttons for one question.
  if (!a && (f || (c && c.ok)) && !updateGate(e))
    row.appendChild(installButton(e, txt, "Install", false));
  if (updateGate(e))
    row.appendChild(installButton(e, txt, "Update", true));
  if (a && !a.compiled)
    row.appendChild(uninstallButton(e, txt));
  if (a && a.failed)
    row.appendChild(clearFaultButton(e, txt));
  if (f && !c)
    row.appendChild(addToCardButton(e, txt));
  if (c)
    row.appendChild(eraseCardButton(e, txt));
  if (f)
    row.appendChild(removeFromLibraryButton(f, txt));
  return row;
}

// installButton is the run that puts bytes on the dial: the shared progress
// panel opens BEFORE the first byte leaves, the hub streams NDJSON into it, and
// the press walks installRun's own doors. Update is the same run with force —
// the shelf's bytes cross again over older copies because the press was earned
// by the shelf being newer. A card-resident Install press is the same run with
// card=1: the hub skips the upload because the bytes already sit on the card,
// and the run still streams installing / installed / enabled so the owner sees
// the dial write and the roll-up rather than a spinner over a silent POST.
// EVERY press opens the progress panel BEFORE the first byte leaves: an
// install runs minutes, and "it looks stuck" is not an acceptable answer to
// a press that is working.
function installButton(e, txt, name, force) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  btn.textContent = name;
  btn.title = force
    ? "Push the hub's newer release onto the orb in hand, over the copies the card and the dial hold."
    : "Put this app on the orb in hand, through the device's own doors.";
  btn.onclick = async function () {
    if (!orbIP) { txt.textContent = "no orb in hand \u2014 connect one first."; return; }
    btn.disabled = true;
    const was = btn.textContent;
    fpOpen();
    const ui = fpUI();
    ui.doing.textContent = name.toLowerCase() + "ing " + (e.title || e.slug) + " on " + orbIP;
    ui.log.textContent = ""; ui.bar.style.width = "0%"; ui.bar.title = ""; ui.op.textContent = "";
    ui.live.textContent = "";
    const fromCard = !force && !!(e.card && e.card.ok);
    const ok = await streamInstall(orbIP, e.slug, false, ui, force, fromCard);
    btn.disabled = false;
    btn.textContent = ok ? name + "ed" : was;
    if (ok) fpClose();
    libVerdict = { slug: e.slug, text: ok
      ? (e.title || e.slug) + " \u00b7 " + (force ? "updated" : "installed") + " on the orb in hand"
      : (e.title || e.slug) + " \u00b7 " + name.toLowerCase() + " refused \u2014 read the progress panel" };
    // Re-read the ledger NOW: the run moved the state this face prints, and a
    // row that still shows the pre-press truth is a row whose buttons lie.
    try { await loadApps(); }
    catch (err) {
      const st = $("#mn-appstate");
      if (st) st.textContent = libVerdict.text + " \u2014 but the ledger would not re-read: " + err.message;
    }
  };
  return btn;
}

// uninstallButton is D38's honest verb: the door homes a shown app and erases
// the slot, and the confirm says so when the ledger says the row is on the
// glass. The card copy stays — Remove from card is the row's other half, and
// the row offers it beside this one whenever both are true.
function uninstallButton(e, txt) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  btn.textContent = "Uninstall";
  btn.title = "Erase the app's slot on the orb in hand. Its card file stays.";
  btn.onclick = async () => {
    if (!await askConfirm({
      title: "Uninstall this app?",
      body: (e.installed && e.installed.on_glass)
        ? (e.title || e.slug) + " is on the glass \u2014 the dial will return to its menu and erase the slot."
        : "The dial erases " + (e.title || e.slug) + "'s slot and forgets the permissions it was granted.",
      detail: "The .orbapp stays on your card, so Install can bring it back. Remove from card "
            + "erases that too.",
      yes: "Uninstall",
    })) return;
    btn.disabled = true;
    txt.textContent = (e.title || e.slug) + " \u00b7 uninstalling\u2026";
    try {
      await appPost("/api/v1/apps/uninstall?id=" + encodeURIComponent(e.slug));
      libVerdict = null;   // this row is about to change homes: do not remember it
      await loadApps();
    } catch (err) {
      txt.textContent = (e.title || e.slug) + " \u00b7 the orb refused it: " + err.message;
      btn.disabled = false;
    }
  };
  return btn;
}

function clearFaultButton(e, txt) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  btn.textContent = "Clear fault";
  btn.onclick = async () => {
    if (!await askConfirm({
      title: "Clear the fault record?",
      body: "The hub forgets why " + (e.title || e.slug) + " was quarantined.",
      detail: "A reboot tries the same load and brings the record straight back. "
            + "Wi-Fi, the card and the app's files are untouched.",
      yes: "Clear the record",
      safe: true,
    })) return;
    btn.disabled = true;
    txt.textContent = (e.title || e.slug) + " \u00b7 clearing\u2026";
    try { await appPost("/api/v1/apps/clear?id=" + encodeURIComponent(e.slug)); txt.textContent = (e.title || e.slug) + " \u00b7 fault cleared \u2014 reboot the dial to rebuild it"; await loadApps(); }
    catch (err) { txt.textContent = "the orb refused it: " + err.message; btn.disabled = false; }
  };
  return btn;
}

// addToCardButton is the filing press: /api/library/push streams the shelf's
// own file to the device's upload door and stops. No slot, no reboot, no
// enable word \u2014 Install remains the only verb that moves bytes onto the dial.
function addToCardButton(e, txt) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  btn.textContent = "Add to card";
  btn.title = "Write the hub library's " + e.slug + ".orbapp to the orb in hand's card. Nothing is installed.";
  btn.onclick = async () => {
    if (!orbIP) { txt.textContent = "no orb in hand \u2014 connect one first."; return; }
    btn.disabled = true;
    // The push is a real transfer over the room Wi-Fi — a 660 KB container runs
    // seconds — so it gets the same progress panel an install gets, driven by
    // the hub's own streaming steps. No more one-line "adding\u2026" with nothing
    // behind it.
    fpOpen();
    const ui = fpUI();
    ui.doing.textContent = "adding " + (e.title || e.slug) + " to the card on " + orbIP;
    ui.log.textContent = ""; ui.bar.style.width = "0%"; ui.bar.title = ""; ui.op.textContent = "";
    ui.live.textContent = "";
    const ok = await streamSteps("/api/library/push?kind=app&name="
      + encodeURIComponent(e.slug) + "&ip=" + encodeURIComponent(orbIP), ui, orbIP);
    btn.disabled = false;
    if (ok) {
      fpClose();
      txt.textContent = (e.title || e.slug) + " \u00b7 added to the card";
      await loadApps();
    } else {
      txt.textContent = (e.title || e.slug) + " \u00b7 the hub refused it \u2014 read the progress panel";
    }
  };
  return btn;
}

// removeFromLibraryButton deletes the hub's copy of a container. It is deliberately not the
// same verb as Remove from card, and the confirm says what it does NOT do: it touches no
// dial and no card. A library delete that quietly uninstalled a running app would be the
// worst surprise on the least reversible button in this face.
function removeFromLibraryButton(a, txt) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  btn.textContent = "Remove from hub library";
  btn.title = "Delete " + a.file + " from this hub's library. No orb and no SD card is touched.";
  btn.onclick = async () => {
    if (!await askConfirm({
      title: "Delete it from the hub library?",
      body: (a.title || a.slug) + " is removed from " + ((libCache && libCache.appsDir) || "the library") + ".",
      detail: "A dial already running it keeps running it, and a copy still on a card stays "
            + "installable from there. This removes only the hub's copy.",
      yes: "Delete from library",
    })) return;
    btn.disabled = true;
    const r = await hubFetch("/api/library/rm?kind=app&name=" + encodeURIComponent(a.slug),
      { method: "POST" });
    const d = await r.json().catch(() => null);
    if (!d || !d.ok) {
      btn.disabled = false;
      txt.textContent = (a.title || a.slug) + " \u00b7 the hub refused: "
        + ((d && d.error) || ("HTTP " + r.status));
      return;
    }
    txt.textContent = (a.title || a.slug) + " \u00b7 removed from the hub library";
    // Re-render both halves: the shelf row is gone, and the row above has lost
    // its Update/Install verdict. renderAppsTab re-reads libCache, which just
    // changed, and the ledger, which did not.
    await renderLibrary();
    renderAppsTab();
  };
  return btn;
}

// eraseCardButton is the verb that finishes a removal. The device splits the two
// halves and this face honours the split: Uninstall frees the slot and leaves the
// container on the card; erasing the file is the second door (POST /api/v1/sd/rm).
// One button walks both, uninstall first, because "gone from the dial AND the
// card" is one decision. The path is the card file's own stem, which can differ
// from the slug when a file arrived under another name — the row erases what the
// listing named.
function eraseCardButton(e, note) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  const stem = (e.card && e.card.stem) || e.slug;
  btn.textContent = "Remove from card";
  btn.title = "Delete apps/" + stem + ".orbapp from the card. If the app is installed, this uninstalls it first.";
  btn.onclick = async () => {
    const installed = !!e.installed && !e.installed.compiled;
    const named = e.title || e.slug;
    if (!await askConfirm({
      title: installed ? "Uninstall and erase from the card?" : "Erase it from the card?",
      body: installed
        ? named + " is uninstalled from the dial and its file is deleted from the card."
        : "The .orbapp for " + named + " is deleted from the card.",
      detail: "Nothing in the hub library is touched, so the app stays installable from there.",
      yes: "Remove from card",
    })) return;
    btn.disabled = true;
    note.textContent = named + " \u00b7 " + (installed ? "uninstalling\u2026" : "erasing\u2026");
    try {
      if (installed) {
        await appPost("/api/v1/apps/uninstall?id=" + encodeURIComponent(e.slug));
      }
      await appPost("/api/v1/sd/rm?path=" + encodeURIComponent("apps/" + stem + ".orbapp"));
      libVerdict = null;   // this row is about to disappear: do not remember it as installed
      await loadApps();
    } catch (err) {
      note.textContent = named + " \u00b7 the orb refused it: " + err.message;
      btn.disabled = false;
    }
  };
  return btn;
}

// The hub's own inventory report (D38): the resolved directories and the files
// the shelf could not parse. The app rows themselves are in the face's list
// above; this is the half of /api/library that is NOT an actionable app.
let libCache = null;

async function renderLibrary() {
  const state = $("#lib-state"), list = $("#lib-list");
  if (!list) return;
  try {
    libCache = await getJSON("/api/library");
  } catch (e) {
    state.hidden = false;
    state.textContent = "the hub would not answer for its library: "
      + (e && e.message ? e.message : e);
    list.innerHTML = "";
    return;
  }
  list.innerHTML = "";
  const refused = libCache.refused || [];
  // The directories and the count are gone from this face (owner, 2026-10-28):
  // a path the operator cannot press and a number that restates the list above
  // are noise. What stays is only what asks for action — files the shelf could
  // not parse, each with the reader's own refusal.
  for (const r of refused) {
    const row = document.createElement("p");
    row.className = "hint";
    row.textContent = "\u2717 " + r.file + " \u2014 " + r.why;
    list.appendChild(row);
  }
  if (refused.length) {
    state.hidden = false;
    state.textContent = refused.length + " file" + (refused.length === 1 ? "" : "s")
      + " the shelf would not read.";
  } else {
    state.hidden = true;
    state.textContent = "";
  }
}

// libVerdict remembers the sentence the last install press earned, keyed by slug, so the
// ledger refetch that follows can re-draw the row WITHOUT losing it. The first cut of this
// handler skipped the refetch precisely to protect the label, and that is what left an
// operator staring at "installed on the orb in hand" above a dial whose button matrix had
// never heard of the app — no row, no sub-tab, no Uninstall door. The verdict survives; the
// face tells the truth.
let libVerdict = null;

// libEntry finds the library's row for a slug, or null.
function libEntry(slug) {
  const apps = (libCache && libCache.apps) || [];
  for (const f of apps) if (f.slug === slug) return f;
  return null;
}

// verCompare mirrors the hub's orbnet.AppVersionCompare on the face side, clause for
// clause, and the two must stay in step: the row's sentence and its button come from this
// copy while the editor's source comes from the Go one, and a divergence would show an
// "update available" row whose Update button the hub then refused as a downgrade.
//
// Negative when a is behind, 0 for the same release, positive when a is ahead, and null
// when either side is unstamped or outside the grammar. Null is not zero: an unstamped
// file states nothing about its own age, and a face that read that silence as agreement
// would tell an operator their dial was current when the truth is that nobody knows. The
// leading v is mandatory because the device's grammar says so — a bare "1.2.3" is refused
// here and by AppVersionOK, rather than being quietly accepted on one side. A prerelease
// tail is OLDER than the release it leads to, which is the clause a raw string compare
// gets backwards.
function verCompare(a, b) {
  const parse = (s) => {
    const m = /^v(\d+)\.(\d+)\.(\d+)(?:[-+](.*))?$/.exec(s || "");
    if (!m) return null;
    return { n: (+m[1]) * 10000 + (+m[2]) * 100 + (+m[3]), tail: m[4] || "" };
  };
  const pa = parse(a), pb = parse(b);
  if (!pa || !pb) return null;
  if (pa.n !== pb.n) return pa.n - pb.n;
  if (pa.tail === pb.tail) return 0;
  if (pa.tail === "") return 1;
  if (pb.tail === "") return -1;
  return pa.tail < pb.tail ? -1 : 1;
}

// libUpdateSuffix is the shelf's verdict on a dial row's version line. Note the
// ordering: it is the LIBRARY that is ahead. A dial running newer than the shelf is
// not out of date, and the face does not say it is — it names the dial as ahead of the
// shelf, because that is the operator's own problem to notice and the sentence that
// lets them act on it.
function libUpdateSuffix(a) {
  const f = libEntry(a.slug);
  if (!f) return "";
  const cmp = verCompare(a.version, f.version);
  if (cmp === null) return "";
  if (cmp < 0) return " \u00b7 update available (" + f.version + ")";
  if (cmp > 0) return " \u00b7 ahead of the hub library";
  return "";
}

// The browser file door (S6 §4). One picked .orbapp, hub-parsed before anything moves,
// filed in the library, listed. It is import and nothing more: the press never installs,
// because moving bytes onto a board is always its own deliberate press on the row that
// results. The state line is the whole reporting surface on purpose — a press that filed
// nothing says so rather than reporting a success the library never got.
function wireLibraryImport() {
  const pick = $("#lib-pick"), file = $("#lib-file"), state = $("#lib-import-state");
  if (!pick || !file || !state) return;
  pick.onclick = () => file.click();
  file.onchange = async () => {
    const f = file.files && file.files[0];
    if (!f) return;
    pick.disabled = true;
    state.textContent = "reading " + f.name + "\u2026";
    let body;
    try { body = await f.arrayBuffer(); }
    catch (e) {
      state.textContent = "the file could not be read: " + (e && e.message ? e.message : e);
      pick.disabled = false; file.value = ""; return;
    }
    let r;
    try {
      r = await hubFetch("/api/apps/import?keep=1",
        { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: body });
    } catch (e) {
      state.textContent = "the hub did not answer: " + (e && e.message ? e.message : e);
      pick.disabled = false; file.value = ""; return;
    }
    let j = null;
    try { j = await r.json(); } catch (e) { /* a non-JSON error body is the sentence below */ }
    const msg = j && (j.error || (j.data && j.data.keep_refused));
    if (!j || !j.ok) {
      state.textContent = "\u2717 " + f.name + " \u2014 " + (msg || ("HTTP " + r.status));
      pick.disabled = false; file.value = ""; return;
    }
    const d = j.data;
    state.textContent = "\u2713 " + (d.title || d.slug) + " \u00b7 " + (d.version || "unstamped")
      + " \u2014 in the library" + (d.kept ? " at " + d.kept : "")
      + (d.overwrote ? " (replaced the copy already there)" : "")
      + (d.keep_refused ? " but the hub refused it: " + d.keep_refused : "");
    file.value = "";
    pick.disabled = false;
    // The listing re-reads rather than appending what this function assumed: the hub
    // resolved the directory, named the file after its header, and is the one that knows
    // whether the row now exists.
    await renderLibrary();
    if (appList) await loadApps();
  };
}

// ---- face: Files — the card as the owner's volume ------------------------------
// Every verb here is the device's own (sd/ls, sd/mkdir, sd/rm, sd/write); this
// face only renders and asks. Deletions confirm once, and a directory that
// refuses as not empty comes back as a SECOND, explicitly-worded press rather
// than a silent recursive retry — "delete with contents" is a decision, so the
// page makes it cost a decision. Upload names are sanitized toward the board's
// own rules here (strange characters become '_'), and the board's refusal
// still says the final word.
let fmPath = "";

async function fmOpen() {
  if (!current.ip) return;
  await fmList(fmPath);
  try {
    const d = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/sd"));
    if (d && d.total_mb)
      $("#fm-space").textContent = d.card + " \u00b7 " + d.free_mb + " of " + d.total_mb + " MB free";
  } catch (e) { $("#fm-space").textContent = ""; }
}

function fmSentence(d, fallback) {
  return (d && d.error) || fallback;
}

async function fmList(rel) {
  const state = $("#fm-state"), list = $("#fm-list");
  state.hidden = false;
  state.textContent = "reading /" + rel + "\u2026";
  try {
    const d = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip)
      + "/api/v1/sd/ls?path=" + encodeURIComponent(rel)));
    if (!d || d.error) throw new Error(fmSentence(d, "the orb refused the listing"));
    fmPath = d.path || "";
    $("#fm-path").textContent = "/" + fmPath;
    $("#fm-up-dir").disabled = fmPath === "";
    list.innerHTML = "";
    const entries = d.entries || [];
    entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
    for (const e of entries) list.appendChild(fmRow(e));
    state.textContent = entries.length + " item(s)" + (d.truncated ? " (listing truncated at 128)" : "");
  } catch (e) {
    list.innerHTML = "";
    state.textContent = "/ " + rel + ": " + e.message;
  }
}

function fmSize(n) {
  if (n < 1024) return n + " B";
  if (n < 1048576) return (n / 1024).toFixed(1) + " KB";
  return (n / 1048576).toFixed(2) + " MB";
}

function fmRow(e) {
  const li = document.createElement("li");
  li.className = "filerow" + (e.dir ? " dir" : "");
  const nm = document.createElement("button");
  nm.type = "button";
  nm.className = "linkish";
  nm.textContent = e.dir ? e.name + "/" : e.name;
  nm.onclick = () => fmList(fmPath ? fmPath + "/" + e.name : e.name);
  const size = document.createElement("span");
  size.className = "hint";
  size.textContent = e.dir ? "directory" : fmSize(e.size);
  const del = document.createElement("button");
  del.type = "button";
  del.className = "ghost small";
  del.textContent = "Delete";
  del.onclick = () => fmDelete(e, del);
  li.appendChild(nm);
  li.appendChild(size);
  li.appendChild(del);
  return li;
}

async function fmRm(rel, recursive) {
  const r = await hubFetch("/orb/" + encodeURIComponent(current.ip)
    + "/api/v1/sd/rm?path=" + encodeURIComponent(rel) + (recursive ? "&recursive=1" : ""),
    { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
  return { status: r.status, body: await r.json().catch(() => null) };
}

async function fmDelete(e, btn) {
  const rel = fmPath ? fmPath + "/" + e.name : e.name;
  const where = "/" + rel;
  if (e.dir) {
    if (!await askConfirm({
      title: "Delete this directory?",
      body: "The orb erases " + where + " from the card.",
      detail: "Everything inside it goes with it. The dial keeps drawing a "
            + "installed theme until another is loaded; nothing else on the "
            + "board changes, and the card stays mounted.",
      yes: "Delete the directory",
    })) return;
  } else if (!await askConfirm({
      title: "Delete this file?",
      body: "The orb erases " + where + " from the card.",
      detail: "This cannot be undone from the page. A USB host can still copy "
            + "what is left, and the dial is untouched.",
      yes: "Delete the file",
    })) return;
  btn.disabled = true;
  const st = $("#fm-state");
  st.hidden = false;
  try {
    let r = await fmRm(rel, false);
    if (r.status === 409 && r.body && /not empty/.test(r.body.error || "")) {
      // A second, separately-worded ask rather than a silent recursive retry:
      // taking a directory's contents is its own decision and costs its own press.
      if (!await askConfirm({
        title: "That directory is not empty",
        body: where + " still holds files. Delete it WITH everything inside it?",
        detail: "Every file and subdirectory under it is erased. This is the "
              + "point of no return for the whole tree.",
        yes: "Delete with contents",
      })) { btn.disabled = false; return; }
      r = await fmRm(rel, true);
    }
    if (r.status !== 200 || !r.body || r.body.error)
      throw new Error(fmSentence(r.body, "HTTP " + r.status));
    st.textContent = "deleted /" + rel + (r.body.removed > 1 ? " (" + r.body.removed + " items)" : "");
    await fmList(fmPath);
  } catch (e2) {
    st.textContent = e2.message;
  }
  btn.disabled = false;
}

$("#fm-refresh").onclick = () => fmList(fmPath);
$("#fm-up-dir").onclick = () => {
  const cut = fmPath.lastIndexOf("/");
  fmList(cut < 0 ? "" : fmPath.slice(0, cut));
};
$("#fm-mkdir-btn").onclick = () => {
  $("#fm-mkdir").hidden = false;
  $("#fm-mkdir-go").hidden = false;
  $("#fm-mkdir-btn").hidden = true;
  $("#fm-mkdir").focus();
};
$("#fm-mkdir-go").onclick = async () => {
  const name = $("#fm-mkdir").value.trim();
  const st = $("#fm-state");
  st.hidden = false;
  const hide = () => {
    $("#fm-mkdir").hidden = true; $("#fm-mkdir-go").hidden = true; $("#fm-mkdir-btn").hidden = false;
  };
  if (!name) { hide(); return; }
  st.textContent = "creating\u2026";
  const r = await hubFetch("/orb/" + encodeURIComponent(current.ip)
    + "/api/v1/sd/mkdir?path=" + encodeURIComponent(fmPath ? fmPath + "/" + name : name),
    { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
  const d = await r.json().catch(() => null);
  st.textContent = r.status === 200 ? "created /" + (fmPath ? fmPath + "/" : "") + name
                                    : fmSentence(d, "HTTP " + r.status);
  hide();
  if (r.status === 200) await fmList(fmPath);
};
$("#fm-upload-btn").onclick = () => $("#fm-file").click();
$("#fm-file").addEventListener("change", async () => {
  const files = Array.from($("#fm-file").files || []);
  $("#fm-file").value = "";
  const st = $("#fm-up-state");
  for (const f of files) {
    const name = f.name.replace(/[^A-Za-z0-9._-]/g, "_").replace(/^\.+/, "") || "upload.bin";
    st.textContent = "sending " + name + " (" + fmSize(f.size) + ")\u2026";
    try {
      const r = await hubFetch("/orb/" + encodeURIComponent(current.ip)
        + "/api/v1/sd/write?path=" + encodeURIComponent(fmPath ? fmPath + "/" + name : name)
        + ($("#fm-overwrite").checked ? "&overwrite=1" : ""),
        { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: f });
      const d = await r.json().catch(() => null);
      if (r.status !== 200 || !d || d.error) throw new Error(fmSentence(d, "HTTP " + r.status));
      st.textContent = "wrote " + name + " \u00b7 " + fmSize(d.bytes) + " landed";
    } catch (e) {
      st.textContent = name + ": " + e.message;
      break;
    }
  }
  await fmList(fmPath);
});

// ---- the Firmware face's reset button, LAN arm included ------------------------
// app.js's modal is cable-only: RESET ALL needs a lead. A v1.1.18 board answers
// the same wipe at /api/v1/factory, so the dialog gains a second reach — and a
// typed ALL, because this is the one LAN verb that can make the orb this page
// is open disappear into its own provisioning AP. The firmware erase stays
// cable-only: lonely is a whole reflash decision no network door should hold.
// These handlers REBIND app.js's (assigned earlier in load order, replaced by
// the later script — the reset modal's cable arms below are carried over as is).
function tabFwAtLeast(major, minor, patch) {
  const m = /v(\d+)\.(\d+)\.(\d+)/.exec(fwStamp || "");
  if (!m) return false;   // an unreadable stamp gates the door closed, as everywhere else
  const v = [Number(m[1]), Number(m[2]), Number(m[3])];
  const w = [major, minor, patch];
  for (let i = 0; i < 3; i++) {
    if (v[i] > w[i]) return true;
    if (v[i] < w[i]) return false;
  }
  return true;
}

function rtAllowed() {
  return !$("#rt-choices").hidden
    && ($("#rt-settings").checked || $("#rt-firmware").checked);
}

function rtLanSettings() {
  return $("#rt-mask").dataset.lan === "1"
    && $("#rt-settings").checked && !$("#rt-firmware").checked;
}

function rtGate() {
  $("#rt-go").disabled = !rtAllowed()
    || (rtLanSettings() && $("#rt-word").value.trim().toUpperCase() !== "ALL");
  $("#rt-wordrow").hidden = !rtLanSettings();
}

$("#mn-resetbtn").onclick = () => {
  const port = current.port;
  const lan = !port && !!current.ip && tabFwAtLeast(1, 1, 18);
  $("#rt-sub").textContent = port ? "on cable " + port
    : lan ? "over the LAN at " + current.ip + " \u2014 settings only; the firmware erase needs the cable"
          : "not on a cable of this computer"
            + (current.ip && !lan ? " \u2014 this board's firmware has no LAN reset door yet" : "");
  $("#rt-choices").hidden = !(port || lan);
  $("#rt-settings").checked = true;
  $("#rt-settings").disabled = false;
  $("#rt-firmware").checked = false;
  $("#rt-firmware").disabled = !port;   // lonely is cable-only, whatever the board runs
  $("#rt-word").value = "";
  $("#rt-state").textContent = "";
  $("#rt-mask").dataset.lan = lan ? "1" : "";
  rtGate();
  $("#rt-mask").hidden = false;
};

for (const id of ["#rt-settings", "#rt-firmware"])
  $(id).onchange = () => {
    if ($("#rt-firmware").checked) $("#rt-settings").checked = true;
    $("#rt-settings").disabled = $("#rt-firmware").checked;
    rtGate();
  };
$("#rt-word").addEventListener("input", rtGate);

$("#rt-go").onclick = async () => {
  const state = $("#rt-state"), go = $("#rt-go");
  if (!rtAllowed() || go.disabled) return;
  const port = current.port, lan = $("#rt-mask").dataset.lan === "1";
  go.disabled = true;
  try {
    if ($("#rt-firmware").checked) {
      fpOpen();
      const { ok } = await streamFlash("/api/lonely", { port, confirm: "LONELY" }, fpUI());
      if (ok) {
        await refreshPorts();
        pullFound();
        $("#rt-mask").hidden = true;
        show("manage");
        location.hash = "";
        $("#mn-find-note").textContent = "";
      }
    } else if (lan) {
      state.textContent = "clearing settings over the LAN\u2026";
      const r = await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/factory?confirm=ALL",
        { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
      const d = await r.json().catch(() => null);
      if (!r.ok || (d && d.error)) throw new Error((d && d.error) || ("HTTP " + r.status));
      state.textContent = "settings cleared \u2014 the orb rebooted as its own setup AP; reach it there to set it up again";
      pullFound();
    } else {
      state.textContent = "clearing settings\u2026";
      const r = await hubFetch("/api/orb/cmd", {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ port, mac: current.mac, command: "reset", operand: "ALL" }),
      });
      const j = await r.json().catch(() => ({}));
      if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
      state.textContent = "settings cleared \u2014 the orb is ready to set up again";
      pullFound();
    }
  } catch (e) {
    state.textContent = e.message;
  } finally {
    go.disabled = false;
    rtGate();
  }
};

// app.js's loadThemes reports through these two instead of writing a flag across
// the file boundary: each classic script owns its own declarations, and the
// jscheck suite enforces exactly that. The Themes face owns its card outright —
// the reveal is the fetch's answer, the hide is the no-board answer's.
function orbThemeListShown() { $("#mn-themecard").hidden = false; }
function orbThemeListHidden() { $("#mn-themecard").hidden = true; }
// The library import lives on the Apps card, whose markup is static, so it binds once
// at load rather than per render — a rebind on every render would stack handlers on the one
// file input and install a picked file twice.
wireLibraryImport();
showTab("orb");






