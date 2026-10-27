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
    $("#mn-orbname").textContent = current.name;
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
async function loadZonesTable() {
  if (!current.ip) return;
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
  } catch (e) {
    // A card parked on "reading\u2026" forever is what the owner saw during an
    // update: the dial was mid-reboot, the fetch died, and the silence wore
    // the costume of a page still working. Name the failure and the way out;
    // re-throw so lazyTab drops its mark and the next press (or the wake
    // retry's tabRearm, app.js) loads the table again by itself.
    cur.textContent = "the zone table did not load: " + (e && e.message ? e.message : String(e)) +
      " \u2014 the orb may be mid-reboot; it reloads when the board answers again";
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
// togglable or uninstallable, a failed row gets Clear fault, a missing factory
// app gets Restore, and the drop zone is the only place an Install button lives.
// Every verb is the device's own door; the face renders and asks, and the door's
// answer is the last word.
let appList = null;   // the installed tier, in ledger order
let cardApps = [];    // the drop-zone candidates sitting on the card
let appData = null;   // the whole answer — factory_missing rides at the top level
let appsSub = "clock";
const APP_TITLES = { clock: "Clock", color_picker: "HSB", menu: "Menu" };

async function loadApps() {
  const state = $("#mn-appstate");
  if (!current.ip) { state.textContent = "no address to read"; return; }
  state.textContent = "reading the app ledger\u2026";
  const d = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/apps"));
  if (!d || d.error) throw new Error((d && d.error) || "the app list did not answer");
  appData = d;
  appList = Array.isArray(d.installed) ? d.installed : [];
  cardApps = Array.isArray(d.apps) ? d.apps : [];
  state.textContent = appList.length + " app" + (appList.length === 1 ? "" : "s")
    + " on the dial" + (cardApps.length ? ", " + cardApps.length + " waiting on the card" : "");
  if (!appList.some((a) => a.slug === appsSub))
    appsSub = appList.some((a) => a.slug === "clock") ? "clock" : ((appList[0] && appList[0].slug) || "");
  renderAppsTab();
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
  appFaceCardZone(body);
  if (appData && appData.factory_missing) appFaceRestore(body);
}

function appFaceBody(a, body) {
  const tier = a.compiled ? "built into the firmware"
    : a.factory ? "factory app, slot " + a.slot : "slot " + a.slot;
  const sub = document.createElement("p");
  sub.className = "hint";
  sub.textContent = a.slug + " \u00b7 " + tier
    + (a.caps ? " \u00b7 may use: " + a.caps : " \u00b7 may use nothing")
    + (a.period_ms ? " \u00b7 every " + (a.period_ms / 1000) + " s" : "")
    + (a.n_settings ? " \u00b7 " + a.n_settings + " settings" : "")
    + (a.on_glass ? " \u00b7 on the glass now" : "");
  body.appendChild(sub);
  const row = document.createElement("div");
  row.className = "row";
  const note = document.createElement("span");
  note.className = "hint";
  if (!a.compiled) {
    const sw = document.createElement("button");
    sw.type = "button";
    sw.className = "ghost";
    sw.textContent = a.enabled ? "On the carousel \u2014 turn off" : "Turned off \u2014 turn on";
    sw.onclick = async () => {
      sw.disabled = true;
      note.textContent = "writing\u2026";
      try {
        const d = await appPost("/api/v1/apps/enable?id=" + encodeURIComponent(a.slug)
          + "&on=" + (a.enabled ? "0" : "1"));
        note.textContent = d.state || "answered";
        await loadApps();
      } catch (e) {
        note.textContent = "the orb refused it: " + e.message;
        sw.disabled = false;
      }
    };
    row.appendChild(sw);
    const un = document.createElement("button");
    un.type = "button";
    un.className = "ghost";
    un.textContent = "Uninstall";
    un.onclick = async () => {
      const named = APP_TITLES[a.slug] || a.title || a.slug;
      const warn = a.factory
        ? " This one ships inside the firmware: it stays gone until you press Restore the factory apps."
        : " The file stays on your card.";
      if (!confirm("Uninstall " + named + "? The dial erases its slot and forgets its permissions." + warn)) return;
      un.disabled = true;
      note.textContent = "uninstalling\u2026";
      try { await appPost("/api/v1/apps/uninstall?id=" + encodeURIComponent(a.slug)); note.textContent = "uninstalled \u2014 the slot is free again"; await loadApps(); }
      catch (e) { note.textContent = "the orb refused it: " + e.message; un.disabled = false; }
    };
    row.appendChild(un);
  } else {
    note.textContent = a.failed ? "quarantined at boot" : "built into the firmware \u2014 always on";
  }
  if (a.failed) {
    const cf = document.createElement("button");
    cf.type = "button";
    cf.className = "ghost";
    cf.textContent = "Clear fault";
    cf.onclick = async () => {
      if (!confirm("Clear the fault record for " + (a.title || a.slug) + "? A reboot brings it back; Wi-Fi and the card are untouched.")) return;
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
        if (!confirm("Install " + named + " from the card onto the dial? The copy takes a few seconds and the app stays off until you turn it on.")) return;
        go.disabled = true;
        lbl.textContent = "installing\u2026";
        try { await appPost("/api/v1/apps/install?id=" + encodeURIComponent(c.stem)); await loadApps(); }
        catch (e) { lbl.textContent = "the orb refused it: " + e.message; go.disabled = false; }
      };
      row.appendChild(go);
    }
    body.appendChild(row);
  }
}

function appFaceRestore(body) {
  const row = document.createElement("div");
  row.className = "row";
  const rb = document.createElement("button");
  rb.type = "button";
  rb.className = "ghost";
  rb.textContent = "Restore the factory apps";
  rb.onclick = async () => {
    if (!confirm("Restore the clock and HSB? Anything already installed keeps its state; anything missing comes back enabled.")) return;
    rb.disabled = true;
    try { await appPost("/api/v1/apps/factory"); $("#mn-appstate").textContent = "restore queued \u2014 re-reading"; await loadApps(); }
    catch (e) { $("#mn-appstate").textContent = "the orb refused it: " + e.message; rb.disabled = false; }
  };
  row.appendChild(rb);
  const hint = document.createElement("span");
  hint.className = "hint";
  hint.textContent = "a factory app is missing from its slot";
  row.appendChild(hint);
  body.appendChild(row);
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
  const child = e.dir ? "directory (with everything inside it)" : "file";
  if (!confirm("Delete this " + child + " from the card:\n/" + (fmPath ? fmPath + "/" : "") + e.name)) return;
  const rel = fmPath ? fmPath + "/" + e.name : e.name;
  btn.disabled = true;
  const st = $("#fm-state");
  st.hidden = false;
  try {
    let r = await fmRm(rel, false);
    if (r.status === 409 && r.body && /not empty/.test(r.body.error || "")) {
      if (!confirm("That directory is not empty. Delete it WITH its contents?")) { btn.disabled = false; return; }
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
showTab("orb");






