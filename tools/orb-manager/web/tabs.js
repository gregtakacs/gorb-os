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
  // it FAILED, because a quarantined row is the one built-in state an owner can act on:
  // Clear fault. The reader still reports the ledger whole; only this face filters.
  const rows = Array.isArray(d.installed) ? d.installed : [];
  appList = rows.filter((a) => !a.compiled || a.failed);
  cardApps = Array.isArray(d.apps) ? d.apps : [];
  const hidden = rows.length - appList.length;
  state.textContent = appList.length + " app" + (appList.length === 1 ? "" : "s")
    + " on the dial" + (cardApps.length ? ", " + cardApps.length + " waiting on the card" : "")
    + (hidden ? ", and " + hidden + " built-in" + (hidden === 1 ? "" : "s") + " not listed" : "");
  if (!appList.some((a) => a.slug === appsSub))
    appsSub = appList.some((a) => a.slug === "clock") ? "clock" : ((appList[0] && appList[0].slug) || "");
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
  const tabs = $("#mn-apps-tabs"), body = $("#mn-apps-body");
  tabs.innerHTML = "";
  body.innerHTML = "";
  if (!appList) return;
  for (const a of appList) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "tab sub" + (a.slug === appsSub ? " on" : "");
    b.textContent = APP_TITLES[a.slug] || a.title || a.slug;
    b.onclick = () => { appsSub = a.slug; renderAppsTab(); };
    tabs.appendChild(b);
  }
  const a = appList.find((x) => x.slug === appsSub);
  if (a) appFaceBody(a, body);
  // S6: the library rows and the drop zone share the card's foot, in that order — the
  // files the hub holds come first because they are the answer to "what goes on a bare
  // board", and the zone is where a file from anywhere else joins them.
  renderLibrary();
  appFaceCardZone(body);
}

// The hub library (S6 §1, §7). One fetch of /api/library, one section per kind, one
// refusal list — and the resolved directory printed above all of it, because a hub on a
// second machine that installed the clone's copy instead of the NAS volume is a debugging
// session nobody wants to have.
let libCache = null;

async function renderLibrary() {
  const state = $("#lib-state"), list = $("#lib-list");
  if (!list) return;
  try {
    libCache = await getJSON("/api/library");
  } catch (e) {
    state.textContent = "the hub would not answer for its library: "
      + (e && e.message ? e.message : e);
    list.innerHTML = "";
    return;
  }
  list.innerHTML = "";
  const apps = libCache.apps || [], themes = libCache.themes || [];
  const refused = libCache.refused || [];
  const dirLine = document.createElement("p");
  dirLine.className = "hint";
  dirLine.textContent = "apps " + (libCache.appsDir || "(none)")
    + " \u00b7 themes " + (libCache.themesDir || "(none)")
    + " \u2014 resolved from " + (libCache.appsSource || "?");
  list.appendChild(dirLine);
  for (const a of apps) list.appendChild(libRow(a));
  for (const t of themes) list.appendChild(libThemeRow(t));
  if (!apps.length && !themes.length && !refused.length) {
    state.textContent = "the library is empty \u2014 drop files into "
      + (libCache.appsDir || "an apps directory") + " to install them onto an orb.";
    return;
  }
  // Refusals are listed and not hidden, on the device's rule: a card that silently omitted
  // an app it could not parse would leave an operator staring at a directory that plainly
  // contains the file.
  for (const r of refused) {
    const row = document.createElement("p");
    row.className = "hint";
    row.textContent = "\u2717 " + r.file + " \u2014 " + r.why;
    list.appendChild(row);
  }
  state.textContent = apps.length + " app" + (apps.length === 1 ? "" : "s") + ", "
    + themes.length + " theme" + (themes.length === 1 ? "" : "s")
    + (refused.length ? ", " + refused.length + " refused" : "") + ".";
}

// libVerdict remembers the sentence the last install press earned, keyed by slug, so the
// ledger refetch that follows can re-draw the row WITHOUT losing it. The first cut of this
// handler skipped the refetch precisely to protect the label, and that is what left an
// operator staring at "installed on the orb in hand" above a dial whose button matrix had
// never heard of the app — no row, no sub-tab, no Uninstall door. The verdict survives; the
// face tells the truth.
let libVerdict = null;

function libRow(a) {
  const row = document.createElement("div");
  row.className = "row";
  const txt = document.createElement("span");
  txt.className = "hint";
  // Which orb this row is judged against, and what that orb runs of it. Every button
  // below keys off the same two lookups, so a row cannot say "on the dial" in its
  // sentence while still offering a plain Install.
  const onDial = (Array.isArray(appList) && appList.find((x) => x.slug === a.slug)) || null;
  const onCard = Array.isArray(cardApps) && cardApps.some((c) => c.stem === a.slug);
  // The version leads the detail line: it is the fact the whole Format 3 change exists to
  // carry, and an unstamped file says so rather than showing a blank where a number goes.
  txt.textContent = (a.title || a.slug) + " \u00b7 " + (a.version || "unstamped")
    + " \u00b7 " + Math.round(a.bytes / 1024) + " KB"
    + (a.caps && a.caps.length ? " \u00b7 may use: " + a.caps.join(", ") : " \u00b7 may use nothing")
    + (a.has_editor ? " \u00b7 carries an editor" : "");
  // The ledger is the authority on whether this container stands on the dial, so a row the
  // refetch found in `installed` says so on its own — the verdict is a memory of a press,
  // and this is the fact under it. The dial's own version rides the sentence, because the
  // pair of numbers is the thing an operator is deciding about.
  if (libVerdict && libVerdict.slug === a.slug) txt.textContent = libVerdict.text;
  else if (onDial)
    txt.textContent += " \u00b7 on the dial at " + (onDial.version || "unstamped")
      + (onDial.enabled === false ? ", switched off" : "")
      + (onCard ? ", and on the card" : "");
  row.appendChild(txt);
  // The button set IS the state, and there are four of them:
  //
  //   not on the dial            Install | Remove from hub library
  //   on the dial, switched off  Install | Remove from hub library
  //   on the dial, same release  Remove from hub library
  //   on the dial, library ahead Upgrade | Remove from hub library
  //
  // The third line is the yield. Once the hub's copy and the dial's copy name the same
  // release AND the row stands on the dial, an Install button only offers to move bytes
  // the board already runs — and it is not a no-op, because the device writes the
  // container to the card, re-installs it and reboots onto it. So for an app that is
  // already current the library stops being an install door and becomes what it actually
  // is: a file on a shelf, which the operator may delete. Upgrade covers the fourth case
  // through the same door with newer bytes. A switched-off row keeps its door: P4a lands
  // an install disabled, a card-scan re-adopts a file into a slot with the enable word
  // off, and a yield on THAT row would strand the board with no UI door back on — the
  // toggle is gone by ruling, so this button is the only way the hub writes enable, and
  // installOneApp asks the ledger first: an off row costs one enable POST, no rewrite.
  const behind = onDial ? verCompare(onDial.version, a.version) : null;
  if (!onDial || onDial.enabled === false) row.appendChild(installLibButton(a, txt, "Install"));
  else if (behind !== null && behind < 0) row.appendChild(installLibButton(a, txt, "Upgrade"));
  row.appendChild(removeFromLibraryButton(a, txt));
  return row;
}

// installLibButton is the library's write door, labelled by the state libRow worked out.
// Install and Upgrade are one run — stream the hub's container to the orb in hand through
// the device's own doors — and only one label is ever true at a time, so the row's
// sentence carries the distinction and the button carries the verb.
function installLibButton(a, txt, name) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  btn.textContent = name;
  btn.title = "Put this release on the orb in hand, through the device's own doors.";
  btn.onclick = async function () {
    if (!orbIP) { txt.textContent = "no orb in hand \u2014 connect one first."; return; }
    btn.disabled = true;
    // The press opens the shared progress panel BEFORE the first byte leaves, and the run
    // streams into it. The panel is the one surface every long-running operation here
    // reports on, and an upload of this size is exactly the wait that must not be silent.
    fpOpen();
    const ui = fpUI();
    ui.doing.textContent = name.toLowerCase() + "ing " + (a.title || a.slug) + " on " + orbIP;
    ui.log.textContent = ""; ui.bar.style.width = "0%"; ui.bar.title = ""; ui.op.textContent = "";
    ui.live.textContent = "";
    const ok = await streamInstall(orbIP, a.slug, false, ui);
    btn.disabled = false;
    btn.textContent = ok ? name + "ed" : name;
    // Success closes the panel after the row says it; a refusal leaves it open with the
    // reason on screen, because Close hides the panel and the operator still needs to read
    // what the orb said.
    if (ok) fpClose();
    libVerdict = { slug: a.slug, text: ok
      ? (a.title || a.slug) + " \u00b7 " + (name === "Upgrade" ? "upgraded" : "installed")
        + " on the orb in hand"
      : (a.title || a.slug) + " \u00b7 " + name.toLowerCase() + " refused \u2014 read the progress panel" };
    // Re-read the ledger NOW. The install moved the dial's own state, and a face that shows
    // the state as it was before the press is a face an operator cannot use: the installed
    // app's row, its Uninstall door and its card entry all come out of this read.
    try { await loadApps(); }
    catch (e) {
      const st = $("#mn-appstate");
      if (st) st.textContent = libVerdict.text + " \u2014 but the ledger would not re-read: " + e.message;
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
    await renderLibrary();
  };
  return btn;
}

function libThemeRow(t) {
  const row = document.createElement("div");
  row.className = "row";
  const txt = document.createElement("span");
  txt.className = "hint";
  // The app leads the kind: a theme is a theme OF something, and a row that names only
  // the owner's title makes the operator guess — or go read the file — to find out which.
  txt.textContent = (t.title || t.slug)
    + (t.app ? " \u00b7 theme for " + t.app : " \u00b7 theme")
    + " \u00b7 " + Math.round(t.bytes / 1024) + " KB";
  row.appendChild(txt);
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  btn.textContent = "Install theme";
  btn.title = "Store this theme on the orb in hand. The store reboots the board.";
  btn.onclick = async function () {
    if (!orbIP) { txt.textContent = "no orb in hand \u2014 connect one first."; return; }
    btn.disabled = true;
    fpOpen();
    const ui = fpUI();
    ui.doing.textContent = "installing " + (t.title || t.slug) + " on " + orbIP;
    ui.log.textContent = ""; ui.bar.style.width = "0%"; ui.bar.title = ""; ui.op.textContent = "";
    ui.live.textContent = "";
    // The theme rides the full run so the clock-rides-with-it rule applies here too.
    const ok = await streamInstall(orbIP, "", false, ui);
    btn.disabled = false;
    btn.textContent = ok ? "Installed" : "Install theme";
    if (ok) fpClose();
    txt.textContent = ok ? (t.title || t.slug) + " \u00b7 stored on the orb in hand"
      : (t.title || t.slug) + " \u00b7 install refused \u2014 read the progress panel";
    // The Themes face holds this theme's rows, and a store reboots the board it lives on.
    // Refetch through the one loader that reads the enumeration and the card together, so
    // the theme appears under its own app rather than waiting for a tab press.
    if (typeof loadThemes === "function" && orbIP) {
      try { await loadThemes(orbIP); } catch (e) { /* the Themes face reports its own failure */ }
    }
  };
  row.appendChild(btn);
  // The shelf's own delete, on the theme side too. The rule is the app rule: this removes
  // the hub's file and leaves every dial alone, so a theme an orb is currently drawing
  // keeps drawing it. Drop keys themes by FILENAME rather than slug, because a .orbtheme
  // carries its name inside and the device files it by that name, not by the stem on disk.
  const rm = document.createElement("button");
  rm.type = "button";
  rm.className = "ghost";
  rm.textContent = "Remove from hub library";
  rm.title = "Delete " + t.file + " from this hub's library. No orb and no SD card is touched.";
  rm.onclick = async () => {
    if (!await askConfirm({
      title: "Delete it from the hub library?",
      body: (t.title || t.slug) + " is removed from " + ((libCache && libCache.themesDir) || "the library") + ".",
      detail: "An orb already drawing it keeps drawing it. This removes only the hub's copy.",
      yes: "Delete from library",
    })) return;
    rm.disabled = true;
    const r = await hubFetch("/api/library/rm?kind=theme&name="
      + encodeURIComponent((t.file || "").replace(/\.orbtheme$/i, "")), { method: "POST" });
    const d = await r.json().catch(() => null);
    if (!d || !d.ok) {
      rm.disabled = false;
      txt.textContent = (t.title || t.slug) + " \u00b7 the hub refused: "
        + ((d && d.error) || ("HTTP " + r.status));
      return;
    }
    txt.textContent = (t.title || t.slug) + " \u00b7 removed from the hub library";
    await renderLibrary();
  };
  row.appendChild(rm);
  return row;
}


// libEntry finds the library's row for a slug, or null. The Apps face reads the ledger
// and the shelf in two fetches and joins them here, because the pair — what the dial runs
// and what the hub holds — is the whole of "is there an update".
function libEntry(slug) {
  const apps = (libCache && libCache.apps) || [];
  for (const f of apps) if (f.slug === slug) return f;
  return null;
}

// verCompare mirrors the hub's orbnet.AppVersionCompare on the face side, clause for
// clause, and the two must stay in step: the row's sentence and its button come from this
// copy while the editor's source comes from the Go one, and a divergence would show an
// "update available" row whose Upgrade button the hub then refused as a downgrade.
//
// Negative when a is behind, 0 for the same release, positive when a is ahead, and null
// when either side is unstamped or outside the grammar. Null is not zero: an unstamped
// file states nothing about its own age, and a face that read that silence as agreement
// would tell an operator their dial was current when the truth is that nobody knows.
// The leading v is mandatory because the device's grammar says so — a bare "1.2.3" is
// refused here and by AppVersionOK, rather than being quietly accepted on one side.
// A prerelease tail is OLDER than the release it leads to, which is the clause a raw
// string compare gets backwards.
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

// libUpdateSuffix is the shelf's verdict on the row in the header line, and it is the
// only place the face says "update available". Note the ordering: it is the LIBRARY that
// is ahead. A dial running newer than the shelf is not out of date, and the face does not
// say it is — it names the dial as ahead of the library, because that is the operator's
// own problem to notice and the sentence that lets them act on it.
function libUpdateSuffix(a) {
  const f = libEntry(a.slug);
  if (!f) return "";
  const cmp = verCompare(a.version, f.version);
  if (cmp === null) return "";
  if (cmp < 0) return " \u00b7 update available (" + f.version + ")";
  if (cmp > 0) return " \u00b7 ahead of the hub library";
  return "";
}

// eraseCardButton is the one verb that finishes a removal. The device splits the two
// halves and this face has to honour the split: Uninstall frees the slot and leaves the
// container on the card, so Install can put it straight back. Erasing the file is a
// second, separate door (/api/v1/sd/rm) in the device's own file manager. Operators asked
// for the state after Uninstall — gone from the dial AND gone from the card — and one
// button that walks both doors in order, uninstall first, is the honest way to offer it.
// It is shown on an installed row too, not just on a card row, because "installed and
// still on the card" is the ordinary result of an ordinary Uninstall.
function eraseCardButton(slug, note) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  btn.textContent = "Remove from card";
  btn.title = "Delete apps/" + slug + ".orbapp from the card. If the app is installed, this uninstalls it first.";
  btn.onclick = async () => {
    const named = APP_TITLES[slug] || slug;
    const installed = Array.isArray(appList) && appList.some((x) => x.slug === slug);
    if (!await askConfirm({
      title: installed ? "Uninstall and erase from the card?" : "Erase it from the card?",
      body: installed
        ? named + " is uninstalled from the dial and its file is deleted from the card."
        : "The .orbapp for " + named + " is deleted from the card.",
      detail: "Nothing in the hub library is touched, so the app stays installable from there.",
      yes: "Remove from card",
    })) return;
    btn.disabled = true;
    note.textContent = installed ? "uninstalling\u2026" : "erasing\u2026";
    try {
      if (installed) {
        await appPost("/api/v1/apps/uninstall?id=" + encodeURIComponent(slug));
      }
      await appPost("/api/v1/sd/rm?path=" + encodeURIComponent("apps/" + slug + ".orbapp"));
      note.textContent = installed
        ? "uninstalled and erased \u2014 the card holds no copy"
        : "erased \u2014 the card holds no copy";
      await loadApps();
    } catch (e) {
      note.textContent = "the orb refused it: " + e.message;
      btn.disabled = false;
    }
  };
  return btn;
}

function appFaceBody(a, body) {
  const tier = a.compiled ? "built into the firmware" : "slot " + a.slot;
  const sub = document.createElement("p");
  sub.className = "hint";
  sub.textContent = a.slug + " \u00b7 " + tier
    + (a.version ? " \u00b7 " + a.version + libUpdateSuffix(a) : " \u00b7 unstamped")
    + (a.caps ? " \u00b7 may use: " + a.caps : " \u00b7 may use nothing")
    + (a.period_ms ? " \u00b7 every " + (a.period_ms / 1000) + " s" : "")
    + (a.n_settings ? " \u00b7 " + a.n_settings + " settings" : "")
    + (a.on_glass ? " \u00b7 on the glass now" : "");
  body.appendChild(sub);
  const row = document.createElement("div");
  row.className = "row";
  const note = document.createElement("span");
  note.className = "hint";
  // Three states, and no enable toggle among them. The hub used to mirror the device's
  // fourth: a row could sit in a slot and be held off the carousel, and the face offered
  // "turn off"/"turn on" to move it. That state is gone by ruling, not by omission — an
  // orb either RUNS an app (in flash, in a slot, on the carousel: one fact, one word) or
  // does not have it installed at all. A switch that hid a row the operator could not
  // otherwise tell was installed made the face claim a state the product decided not to
  // have, and every "why is my clock gone" started there. The device door still answers
  // /apps/enable for anything that wants it; this face does not ask.
  if (!a.compiled) {
    const un = document.createElement("button");
    un.type = "button";
    un.className = "ghost";
    un.textContent = "Uninstall";
    un.onclick = async () => {
      const named = APP_TITLES[a.slug] || a.title || a.slug;
      if (!await askConfirm({
        title: "Uninstall this app?",
        body: "The dial erases " + named + "'s slot and forgets the permissions it was granted.",
        detail: "The .orbapp stays on your card, so Install can bring it back. Remove from card "
              + "erases that too.",
        yes: "Uninstall",
      })) return;
      un.disabled = true;
      note.textContent = "uninstalling\u2026";
      try { await appPost("/api/v1/apps/uninstall?id=" + encodeURIComponent(a.slug)); note.textContent = "uninstalled \u2014 the slot is free again"; await loadApps(); }
      catch (e) { note.textContent = "the orb refused it: " + e.message; un.disabled = false; }
    };
    row.appendChild(un);
    row.appendChild(eraseCardButton(a.slug, note));
  } else {
    note.textContent = a.failed ? "quarantined at boot" : "built into the firmware \u2014 always on";
  }
  if (a.failed) {
    const cf = document.createElement("button");
    cf.type = "button";
    cf.className = "ghost";
    cf.textContent = "Clear fault";
    cf.onclick = async () => {
      if (!await askConfirm({
        title: "Clear the fault record?",
        body: "The hub forgets why " + (a.title || a.slug) + " was quarantined.",
        detail: "A reboot tries the same load and brings the record straight back. "
              + "Wi-Fi, the card and the app's files are untouched.",
        yes: "Clear the record",
        safe: true,
      })) return;
      cf.disabled = true;
      note.textContent = "clearing\u2026";
      try { await appPost("/api/v1/apps/clear?id=" + encodeURIComponent(a.slug)); note.textContent = "fault cleared \u2014 reboot the dial to rebuild it"; await loadApps(); }
      catch (e) { note.textContent = "the orb refused it: " + e.message; cf.disabled = false; }
    };
    row.appendChild(cf);
  }
  row.appendChild(note);
  body.appendChild(row);
  const p = document.createElement("p");
  p.className = "hint";
  if (a.slug === "clock")
    p.textContent = "The clock's face is its theme \u2014 the Themes tab stores, adds and deletes them.";
  else if (a.slug === "color_picker")
    p.textContent = "HSB takes no settings from here: it runs when the dial opens it. Turning it off hides it from the carousel.";
  else
    p.textContent = "This app exposes no settings the hub can edit.";
  body.appendChild(p);
}

// appFaceCardZone is the drop zone: everything sitting in /sd/apps on the card,
// refused rows shown with the reader's own sentence rather than hidden, because
// "copied nothing" and "copied four, all refused" are different owner problems.
// The browser file door (S6 §4). One picked .orbapp, hub-parsed before anything moves,
// then keep and/or install per the two boxes. The state line is the whole reporting surface
// on purpose: a press that kept one file and failed to install it says exactly that,
// because "installed" over a half-done press would leave an operator trusting a library
// that never received the bytes.
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
  };
}

function appFaceCardZone(body) {
  const h = document.createElement("p");
  h.className = "hint";
  h.textContent = cardApps.length
    ? "On the card, not yet on the dial:"
    : "The card's apps folder holds nothing to install.";
  body.appendChild(h);
  for (const c of cardApps) {
    const row = document.createElement("div");
    row.className = "row";
    const lbl = document.createElement("span");
    lbl.className = "hint";
    const named = c.title || c.stem;
    lbl.textContent = named + " \u00b7 " + Math.round((c.file_size || 0) / 1024) + " KB"
      + (c.ok ? "" : " \u2014 refused: " + (c.why || "the card would not say why"));
    row.appendChild(lbl);
    if (c.ok) {
      const go = document.createElement("button");
      go.type = "button";
      go.className = "ghost";
      go.textContent = "Install";
      go.onclick = async () => {
        if (!await askConfirm({
          title: "Install this app?",
          body: "The dial copies " + named + " from the card into its app slot.",
          detail: "The copy takes a few seconds and the board reboots onto it. Nothing on the "
                + "card changes, so Uninstall leaves this copy behind to install again.",
          yes: "Install",
          safe: true,
        })) return;
        go.disabled = true;
        lbl.textContent = "installing\u2026";
        try { await appPost("/api/v1/apps/install?id=" + encodeURIComponent(c.stem)); await loadApps(); }
        catch (e) { lbl.textContent = "the orb refused it: " + e.message; go.disabled = false; }
      };
      row.appendChild(go);
    }
    // The card's own copy is the operator's file, and the drop zone is the only place this
    // face shows it — so the only place to clear a stale container, make room on the card,
    // or finish an uninstall all the way is here. The verb is the device's own file-manager
    // door (POST /api/v1/sd/rm), the same one the Files face already drives, and the device
    // keeps its policy: it refuses traversal, the roots themselves and non-empty trees in
    // its own words. Erasing this file does not touch a slot that already holds the app.
    const rm = document.createElement("button");
    rm.type = "button";
    rm.className = "ghost";
    rm.textContent = "Remove from card";
    rm.title = "Erase " + c.stem + ".orbapp from the microSD card. A slot already holding the app keeps it.";
    rm.onclick = async () => {
      if (!await askConfirm({
        title: "Erase this app from the card?",
        body: "The orb deletes /sd/apps/" + c.stem + ".orbapp.",
        detail: "An app already installed on the dial keeps drawing until you uninstall it; "
              + "it just cannot be reinstalled from this card. A container the hub library "
              + "holds can be pushed again at any time.",
        yes: "Erase it",
      })) return;
      rm.disabled = true;
      lbl.textContent = "removing\u2026";
      try {
        await appPost("/api/v1/sd/rm?path=" + encodeURIComponent("apps/" + c.stem + ".orbapp"));
        libVerdict = null;   // this row is about to disappear: do not remember it as installed
        await loadApps();
      } catch (e) {
        lbl.textContent = named + " \u00b7 the orb refused the delete: " + e.message;
        rm.disabled = false;
      }
    };
    row.appendChild(rm);
    body.appendChild(row);
  }
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






