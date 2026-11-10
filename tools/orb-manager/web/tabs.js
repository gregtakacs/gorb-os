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
let badgesFor = "";   // the board warmBadges last counted for
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
  // Leaving the Themes face retires its preview queue. A queued plane is a page
  // dial the hub may not cut, so a queue left streaming under the Wi-Fi face
  // holds the lane against the scan the operator just asked for, and the scan
  // comes back "orb busy" (bench, 2026-10-31). Un-loading the face makes the
  // way back a fresh read; the planes already fetched answer from the hub's
  // cache, so the return costs the list read and nothing more.
  if (tabNow === "themes" && name !== "themes") {
    clearPreviewQueue();
    tabLoaded.delete("themes");
  }
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
  else if (name === "orb") lazyTab("home", () => {
    loadSecrets().catch(() => {});
    loadSound().catch(() => {});
    loadUnits().catch(() => {});
    loadPlaces().catch(() => {});
    loadMaps().catch(() => {});
    return loadHome();
  });
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

// cardChanged is the aftertaste of every flow that moves bytes off or onto the
// card. The Apps and Themes faces both draw the card's own contents, so a file
// erased under the Files face — or an .orbapp pulled off the dial — leaves their
// ledgers describing a card that no longer exists, and the operator watches a
// stale "Remove from card" button until a page reload says otherwise. Un-load
// both faces here, the same ledger tabRearm clears across a reboot: the next
// press pays one fresh read and shows the truth. The face already on screen is
// refreshed by its own flow, which calls this beside that refresh.
function cardChanged() {
  tabLoaded.delete("apps");
  tabLoaded.delete("themes");
}

// tabRearm is the answer to "the orb just came back". A board that went silent
// and now answered was rebooted by somebody — an update the hub staged, a
// factory reset, a radio that dropped. Every face loaded across that silence
// holds facts about a boot that has ended, so app.js's return-from-wake clears
// the ledger here and re-fires the face under the operator's hand. The other
// faces are simply un-loaded: they fetch fresh the moment they are pressed.
function tabRearm() {
  tabLoaded.clear();
  badgesFor = "";
  showTab(tabNow);
}

// orbTabsReset re-arms dormancy whenever a DIFFERENT board opens: last orb's
// apps, zones, files and themes are this orb's facts, and a stale paint is a
// wrong paint. app.js calls it from openOrbByIP before the first loadOrb.
function orbTabsReset() {
  tabLoaded.clear();
  badgesFor = "";
  stopHomeWake();                 // last orb's home wait is not this orb's promise
  homeClearPend();                // nor is a point proposed to it
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
  warmBadges();
}

// warmBadges counts the Apps and Themes updates once per opened board, so the tabs
// carry their amber numbers before either face is pressed (owner, 2026-11-09). The
// Apps face is cheap and loads whole; Themes counts from data and paints nothing,
// since its face queues previews that must not run under another face.
function warmBadges() {
  if (!current.ip || badgesFor === current.ip) return;
  badgesFor = current.ip;
  lazyTab("apps", loadApps);
  if (tabNow !== "themes" && typeof themeBadgePass === "function") themeBadgePass(current.ip).catch(() => {});
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

// ---- face: Orb — sound: the level and quiet hours (docs/knob-edit-plan.md) ----
// The orb's /api/v1/sound, through the hub's allowlisted door. Quiet hours off is
// quiet_on false; the hours then show the ones they come back with (quiet_last_*),
// and the pickers are disabled so Save cannot pretend to set them.
const soundURL = () => "/orb/" + encodeURIComponent(current.ip) + "/api/v1/sound";
// Times in half-hour steps (owner, 2026-11-04): a picker's value is the minute of the day,
// and the orb takes the hour and its half (quiet_from + quiet_from_m). A board older than
// the half hours reads only the hour, and the line after a save shows what it kept.
const hh = (h, m) => String("0" + h).slice(-2) + ":" + (m >= 30 ? "30" : "00");

(function soundPickers() {
  for (const id of ["#snd-qfrom", "#snd-qto"]) {
    const sel = $(id);
    for (let t = 0; t < 1440; t += 30) sel.add(new Option(hh(t / 60 | 0, t % 60), String(t)));
  }
})();

function soundLine(d) {
  if (!d.ready) return "this board has no speaker";
  const lvl = d.volume ? "volume " + d.volume : "volume off";
  return lvl + " · " + (d.quiet_on ? "quiet " + hh(d.quiet_from, d.quiet_from_m) + "–" + hh(d.quiet_to, d.quiet_to_m)
                                   : "quiet hours off (" + hh(d.quiet_last_from, d.quiet_last_from_m) + "–" + hh(d.quiet_last_to, d.quiet_last_to_m) + " when on)");
}

function soundShow(d) {
  $("#snd-cur").textContent = soundLine(d);
  $("#snd-vol").value = String(d.volume);
  $("#snd-volv").textContent = d.volume ? String(d.volume) : "off";
  $("#snd-quiet").checked = !!d.quiet_on;
  const at = (h, m) => String(h * 60 + (m >= 30 ? 30 : 0));
  $("#snd-qfrom").value = d.quiet_on ? at(d.quiet_from, d.quiet_from_m) : at(d.quiet_last_from, d.quiet_last_from_m);
  $("#snd-qto").value = d.quiet_on ? at(d.quiet_to, d.quiet_to_m) : at(d.quiet_last_to, d.quiet_last_to_m);
  soundPickersOn();
}

function soundPickersOn() {
  const on = $("#snd-quiet").checked;
  $("#snd-qfrom").disabled = !on;
  $("#snd-qto").disabled = !on;
}

async function loadSound() {
  if (!current.ip) return;
  try {
    soundShow(await readDeviceJSON(await hubFetch(soundURL())));
  } catch (e) {
    $("#snd-cur").textContent = "sound did not load: " + (e && e.message ? e.message : String(e));
    throw e;
  }
}

$("#snd-vol").oninput = () => {
  const v = +$("#snd-vol").value;
  $("#snd-volv").textContent = v ? String(v) : "off";
};
$("#snd-quiet").onchange = soundPickersOn;

$("#snd-go").onclick = async () => {
  const msg = $("#snd-msg"), btn = $("#snd-go");
  if (!current.ip) { msg.textContent = "no address to write to"; return; }
  const on = $("#snd-quiet").checked;
  const from = +$("#snd-qfrom").value, to = +$("#snd-qto").value;
  if (on && from === to) { msg.textContent = "quiet hours need two different times"; return; }
  const body = { volume: +$("#snd-vol").value, quiet_on: on };
  if (on) {
    body.quiet_from = from / 60 | 0; body.quiet_from_m = from % 60;
    body.quiet_to = to / 60 | 0; body.quiet_to_m = to % 60;
  }
  btn.disabled = true;
  msg.textContent = "writing to the board\u2026";
  try {
    const r = await hubFetch(soundURL(), {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
    });
    const d = await readDeviceJSON(r);
    if (!r.ok || d.error) throw new Error(d.error || ("HTTP " + r.status));
    soundShow(d);
    msg.textContent = "saved";
  } catch (e) {
    msg.textContent = "the board refused it: " + e.message;
  } finally {
    btn.disabled = false;
  }
};

// ---- face: Orb — units: temperature, speed, distance (weather-plan §2.1) ----
// The orb's /api/v1/units, through the hub's allowlisted door. A board older than the
// units (OS v1.2.10) answers 404, and the card says so rather than offering a save.
const unitsURL = () => "/orb/" + encodeURIComponent(current.ip) + "/api/v1/units";
const UNIT_TEXT = { c: "\u00b0C", f: "\u00b0F", kmh: "km/h", mph: "mph", kn: "knots", km: "km", mi: "miles", nm: "nautical miles",
  "24h": "24-hour", "12h": "12-hour", hpa: "hPa", inhg: "inHg", mmhg: "mmHg" };

// The time format joined the units in OS v1.2.16 and pressure in v1.2.17: an older board
// names none, and the choice is hidden rather than offered to a board that would not keep it.
function unitsShow(d) {
  $("#un-cur").textContent = [d.temp, d.speed, d.dist, d.time, d.pressure].filter(Boolean).map((k) => UNIT_TEXT[k] || k).join(" \u00b7 ");
  $("#un-temp").value = d.temp;
  $("#un-speed").value = d.speed;
  $("#un-dist").value = d.dist;
  $("#un-time-l").hidden = !d.time;
  if (d.time) $("#un-time").value = d.time;
  $("#un-pressure-l").hidden = !d.pressure;
  if (d.pressure) $("#un-pressure").value = d.pressure;
  $("#un-go").disabled = false;
}

async function loadUnits() {
  if (!current.ip) return;
  try {
    const r = await hubFetch(unitsURL());
    if (r.status === 404) {
      $("#un-cur").textContent = "this firmware has no units setting: update it to v1.2.10 or later";
      $("#un-go").disabled = true;
      return;
    }
    unitsShow(await readDeviceJSON(r));
  } catch (e) {
    $("#un-cur").textContent = "units did not load: " + (e && e.message ? e.message : String(e));
    throw e;
  }
}

$("#un-go").onclick = async () => {
  const msg = $("#un-msg"), btn = $("#un-go");
  if (!current.ip) { msg.textContent = "no address to write to"; return; }
  const body = { temp: $("#un-temp").value, speed: $("#un-speed").value, dist: $("#un-dist").value };
  if (!$("#un-time-l").hidden) body.time = $("#un-time").value;
  if (!$("#un-pressure-l").hidden) body.pressure = $("#un-pressure").value;
  btn.disabled = true;
  msg.textContent = "writing to the board\u2026";
  try {
    const r = await hubFetch(unitsURL(), {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
    });
    const d = await readDeviceJSON(r);
    if (!r.ok || d.error) throw new Error(d.error || ("HTTP " + r.status));
    unitsShow(d);
    msg.textContent = "saved";
    mapsZooms().catch(() => {});                  /* the preview's scales are in the orb's unit */
  } catch (e) {
    msg.textContent = "the board refused it: " + e.message;
  } finally {
    btn.disabled = false;
  }
};

// ---- face: Orb — the home location (flight-tracker-design §6.4) --------------
// One OS setting: where the orb is. It sets the zone, and every app that needs a
// position reads it (the flight tracker through its location cap). There is no
// zone picker here any more (owner ruling 2026-11-01): the location drives the
// zone.
//
// Two steps. Every source only PROPOSES a point:
//   * a city from the orb's own list (web/cities.json, baked by
//     tools/make_cities.py from the rows orb_cities.c is baked from),
//   * this browser's location,
//   * a click on the map,
//   * typed coordinates or a full plus code.
// The orb previews it (GET /api/v1/home?lat=&lon= or ?code=): the place and zone
// it would store, and the point itself, which is what the map pins — a plus code
// is decoded by the orb, not here, so the pin is the point Set home stores. Set
// home then posts that point, and the orb takes the zone with it.
const homeURL = (q) => "/orb/" + encodeURIComponent(current.ip) + "/api/v1/home" + (q || "");
let homeMap = null;
let homeMark = null;
let homePend = null;          // the orb's preview of the proposed point, or null
let homeAsk = 0;              // the newest preview asked for; older answers are dropped
let homeCities = null;

// The zone's offset now, from this browser's IANA data: the orb's newlib has
// none, and every desktop browser has all of it.
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
function tzHm(m) {
  if (m == null || isNaN(m)) return "GMT?";
  const s = m < 0 ? "-" : "+";
  m = Math.abs(m);
  return "GMT" + s + String("0" + Math.floor(m / 60)).slice(-2) + ":" + String("0" + (m % 60)).slice(-2);
}
const homeZone = (zone, off) => zone + " (" + tzHm(off) + " now)";
const homeAt = (lat, lon) => (+lat).toFixed(6) + ", " + (+lon).toFixed(6);

function homeLine(d) {
  const h = d && d.home;
  if (!h || !h.set) return "No location set. The clock shows " + homeZone(d && d.zone_label || "UTC", d && d.tz_offset);
  return h.place + " · " + homeAt(h.lat, h.lon) + " · " + homeZone(d.zone_label || d.tz, d.tz_offset) +
    (h.follows ? "" : " — set apart from the location; Set location again to make it follow");
}

// The home read can die the way every slow read of a booting board dies: the
// dial mid-boot or mid-theme-draw, its httpd accepting and answering nothing
// until the 8 s device budget gives up (owner, 2026-10-28: connected onto an orb
// that had just rebooted onto a stored theme; status answered, this read timed
// out). app.js's orbWake only re-dials when the STATUS read fails, so a board
// that answers status and starves this read would leave the card parked on a
// promise no mechanism owned. This ladder is that mechanism — 5 s a beat,
// retired the moment the card opens another orb or the read lands.
let homeWakeTmr = 0;
let homeWakeTok = "";
let homeWakeN = 0;
const HOME_WAKE_MAX = 24;   // ≈ a boot, SNTP and a big theme draw, at 5 s a beat

function stopHomeWake() {
  if (homeWakeTmr) { clearTimeout(homeWakeTmr); homeWakeTmr = 0; }
}

async function loadHome(retry) {
  if (!current.ip) return;
  if (!retry) { stopHomeWake(); homeWakeN = 0; }   // a fresh read starts a new wait
  const cur = $("#home-cur");
  try {
    const d = await readDeviceJSON(await hubFetch(homeURL()));
    cur.textContent = homeLine(d);
    tabLoaded.add("home");   // a wake re-dial that lands restores the mark lazyTab dropped
    stopHomeWake();
    homeShow(d);
    return d;
  } catch (e) {
    // Name the failure, re-dial on the boot's clock, and re-throw so lazyTab
    // drops its mark and a press works even mid-ladder.
    cur.textContent = "the location did not load: " + (e && e.message ? e.message : String(e)) +
      " — the orb may be mid-boot; re-reading it every 5 s";
    const tok = current.mac || current.ip;
    if (tok !== homeWakeTok) { homeWakeTok = tok; homeWakeN = 0; }   // a different card resets the wait
    if (homeWakeN < HOME_WAKE_MAX && !homeWakeTmr) {
      homeWakeTmr = setTimeout(() => {
        homeWakeTmr = 0;
        if ((current.mac || current.ip) !== homeWakeTok) return;     // a different card is open now
        if (++homeWakeN > HOME_WAKE_MAX) {
          cur.textContent = "the location stopped re-reading — press the Orb face when the board answers";
          return;
        }
        loadHome(true).catch(() => {});
      }, 5000);
    }
    throw e;
  }
}

// Leaflet (BSD-2, vendored under web/vendor/leaflet) loads with the face, so a
// hub that never opens it never parses it. Tiles are OpenStreetMap's, fetched by
// this browser; the attribution is theirs to require.
function homeLeaflet() {
  if (window.L) return Promise.resolve(window.L);
  return new Promise((ok, bad) => {
    const css = document.createElement("link");
    css.rel = "stylesheet"; css.href = "/web/vendor/leaflet/leaflet.css";
    document.head.appendChild(css);
    const js = document.createElement("script");
    js.src = "/web/vendor/leaflet/leaflet.js";
    js.onload = () => ok(window.L);
    js.onerror = () => bad(new Error("the map library did not load"));
    document.head.appendChild(js);
  });
}

async function homeMapReady() {
  if (homeMap) { homeMap.invalidateSize(); return homeMap; }
  const L = await homeLeaflet();
  if (homeMap) return homeMap;     // two callers raced the first load
  L.Icon.Default.imagePath = "/web/vendor/leaflet/images/";
  homeMap = L.map($("#home-map")).setView([20, 0], 2);
  L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
    maxZoom: 18,
    attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
  }).addTo(homeMap);
  homeMap.on("click", (ev) => homePropose("?lat=" + (+ev.latlng.lat.toFixed(6)) +
    "&lon=" + (+(((ev.latlng.lng + 540) % 360) - 180).toFixed(6)), "the pin", false));
  return homeMap;
}

// One marker, always the point in question: the proposal while there is one,
// else the stored home. A map that shows anything else is lying about what Set
// home would do.
async function homePin(lat, lon, recentre) {
  let map;
  try { map = await homeMapReady(); } catch (e) { $("#home-msg").textContent = e.message; return; }
  if (lat == null) {
    if (homeMark) { homeMark.remove(); homeMark = null; }
    return;
  }
  if (homeMark) homeMark.setLatLng([lat, lon]); else homeMark = window.L.marker([lat, lon]).addTo(map);
  if (recentre) map.setView([lat, lon], Math.max(map.getZoom(), 9));
}

function homeShow(d) {
  if (homePend) return;            // the proposal owns the map until it is set or replaced
  const h = d && d.home;
  homePin(h && h.set ? h.lat : null, h && h.set ? h.lon : null, true);
}

function homeClearPend() {
  homePend = null;
  homeAsk++;
  $("#home-next").hidden = true;
}

// Step one, for every source: ask the orb what this point would become, pin
// that point, and show it beside the Set button with its zone and the zone's
// offset today.
// name: a city picked from the hub's list keeps its own name; the orb's table only knows
// the big ones and would call a town "200 km N of Grand Rapids"
async function homePropose(query, what, recentre, name) {
  const ask = ++homeAsk;
  const msg = $("#home-msg");
  msg.textContent = "asking the orb about " + what + "…";
  try {
    const d = await readDeviceJSON(await hubFetch(homeURL(query)));
    if (ask !== homeAsk) return;   // a newer proposal won
    const p = d && d.proposal;
    if (!p) throw new Error("the orb sent no preview");
    if (name) { p.place = fitUtf8(String(name).replace(/["\\]/g, ""), 39); p.named = true; }
    homePend = p;
    await homePin(p.lat, p.lon, recentre);
    $("#home-pend").textContent = p.place + " · " + homeAt(p.lat, p.lon) + " · zone " +
      homeZone(p.zone, tzOffAt(p.zone, Date.now())) +
      (p.exact ? "" : " · the orb only approximates this zone's clock changes");
    $("#home-next").hidden = false;
    msg.textContent = "";
  } catch (e) {
    if (ask === homeAsk) msg.textContent = "the orb could not place " + what + ": " + e.message;
  }
}

// Step two: store the previewed point. The zone comes with it.
$("#home-set").onclick = async () => {
  const p = homePend;
  if (!p) return;
  const msg = $("#home-msg");
  msg.textContent = "setting " + p.place + "…";
  try {
    const d = await readDeviceJSON(await hubFetch(homeURL(), {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify(p.named ? { lat: p.lat, lon: p.lon, place: p.place } : { lat: p.lat, lon: p.lon }),
    }));
    homeClearPend();
    $("#home-cur").textContent = homeLine(d);
    homeShow(d);
    msg.textContent = "location set: " + d.home.place + ", clock on " + homeZone(d.zone_label || d.tz, d.tz_offset);
    pullFound();
    loadMaps().catch(() => {});                   /* home's old maps go; its new ones are owed */
  } catch (e) {
    msg.textContent = "the orb refused it: " + e.message;
  }
};

$("#home-clear").onclick = async () => {
  const msg = $("#home-msg");
  try {
    const d = await readDeviceJSON(await hubFetch(homeURL(), {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ clear: true }),
    }));
    homeClearPend();
    $("#home-cur").textContent = homeLine(d);
    homeShow(d);
    msg.textContent = "location forgotten; the clock keeps its zone";
    loadMaps().catch(() => {});
  } catch (e) {
    msg.textContent = "the orb refused it: " + e.message;
  }
};

$("#home-loc").onclick = () => {
  const msg = $("#home-msg");
  if (!navigator.geolocation || !window.isSecureContext) {
    msg.textContent = "this browser will only share its location with a page on localhost or https " +
      "— open the hub at http://localhost, or search or pin the map instead";
    return;
  }
  msg.textContent = "asking this browser where it is…";
  navigator.geolocation.getCurrentPosition(
    (p) => homePropose("?lat=" + (+p.coords.latitude.toFixed(6)) + "&lon=" + (+p.coords.longitude.toFixed(6)),
      "this browser's location", true),
    (e) => { msg.textContent = "the browser did not say (" + (e.message || "denied") + ") — search or pin the map instead"; },
    { enableHighAccuracy: false, timeout: 15000, maximumAge: 600000 });
};

// ---- app settings: one page per installed app (owner, 2026-11-04) --------------
// The app declares its settings in app.json and the orb answers them whole: GET
// /api/v1/apps/settings?id=<slug> is the schema (each setting's type, label, help, links,
// where it shows, its range, step, unit, decimals and choices) and every value now
// (app-platform-plan §57). This page draws each setting shown on the hub from that answer
// alone, in the manifest's order, and names no app and no service itself: a number field
// and slider, a dropdown, on/off buttons, checkboxes, a line of text, the place picker, the
// secret row. Writes go through POST /api/v1/apps/cfg, the orb's one settings door.
// A place is stored "Name|lat,lon" (two decimals, the 31 characters a string setting
// holds), picked from the hub's city list or typed as coordinates. A secret answers its
// host, whether it is set, its header and its last four characters, never its value;
// POST {"value","header"} writes it and an empty value clears it.
const cfgURL = (slug, id) => "/orb/" + encodeURIComponent(current.ip) + "/api/v1/apps/cfg?id=" +
  encodeURIComponent(slug) + "&key=" + encodeURIComponent(id);
let apsApps = [];     // the installed apps, in title order
let apsSub = "";      // the app whose page is open
function secretLine(d) {
  return d && d.set ? "set (\u2026" + d.tail + ", sent as " + d.header + " to " + d.host + ")"
                    : "not set: sent only to " + (d && d.host ? d.host : "the host the app declared");
}
function secretRow(app, id, help) {
  const box = document.createElement("div");
  box.className = "apsset";
  const cur = document.createElement("span"), msg = document.createElement("span");
  cur.className = msg.className = "hint";
  const name = document.createElement("strong");
  name.textContent = help.label || id;
  const head = document.createElement("div");
  head.className = "apshead";
  head.append(name, cur);
  box.appendChild(head);
  if (help.help) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = help.help;
    box.appendChild(p);
  }
  const links = Array.isArray(help.links) ? help.links.filter((l) => l && /^https:\/\//.test(l.href || "")) : [];
  if (links.length) {
    const p = document.createElement("p");
    p.className = "apslinks";
    for (const l of links) {
      const a = document.createElement("a");
      a.href = l.href; a.target = "_blank"; a.rel = "noopener noreferrer";
      a.textContent = (l.text || l.href) + " \u2197";
      p.appendChild(a);
    }
    box.appendChild(p);
  }
  const form = document.createElement("div");
  form.className = "row";
  const key = document.createElement("input"), hdr = document.createElement("input");
  key.type = "password"; key.size = 34; key.autocomplete = "off"; key.placeholder = "paste the " + (help.label || id);
  key.setAttribute("aria-label", (app.title || app.slug) + " " + (help.label || id));
  hdr.size = 14; hdr.autocomplete = "off"; hdr.placeholder = help.header || "x-api-key";
  hdr.setAttribute("aria-label", "Header the value is sent in");
  hdr.title = "The HTTP header the service wants the value in. Leave empty for " + (help.header || "x-api-key") + ".";
  const set = document.createElement("button"), clr = document.createElement("button");
  set.className = "primary"; clr.className = "ghost";
  set.type = clr.type = "button";
  set.textContent = "Save"; clr.textContent = "Remove";
  form.append(key, hdr, set, clr, msg);
  box.appendChild(form);
  const url = cfgURL(app.slug, id);
  const post = async (body, what) => {
    msg.textContent = what + "\u2026";
    try {
      await readDeviceJSON(await hubFetch(url, {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
      }));
      const d = await readDeviceJSON(await hubFetch(url));
      cur.textContent = secretLine(d);
      key.value = "";
      msg.textContent = d.set ? "saved; the orb sends it on the app's next request" : "removed";
    } catch (e) {
      msg.textContent = "the orb refused it: " + e.message;
    }
  };
  set.onclick = () => {
    const v = key.value.trim(), h = hdr.value.trim() || help.header || "";
    if (!v) { msg.textContent = "paste the value first"; return; }
    post(h ? { value: v, header: h } : { value: v }, "saving");
  };
  clr.onclick = () => post({ value: "" }, "removing");
  hubFetch(url).then(readDeviceJSON).then((d) => { cur.textContent = secretLine(d); })
    .catch((e) => { cur.textContent = "unreadable: " + e.message; });
  return box;
}
// A name cut to fit `max` bytes of UTF-8 without splitting a letter: the orb keeps bytes,
// and Győr's ő is two of them (radar-plan §6.22: names in their own letters, OS v1.2.37)
function fitUtf8(s, max) {
  const enc = new TextEncoder();
  let out = "";
  for (const ch of String(s)) {
    if (enc.encode(out + ch).length > max) break;
    out += ch;
  }
  return out;
}
// a place setting's stored form: "Name|lat,lon", the name giving way to the coordinates
const PLACE_MAX = 31;
function placeValue(name, lat, lon) {
  const c = "|" + (+lat).toFixed(2) + "," + (+lon).toFixed(2);
  const nm = String(name || "").replace(/[|"\\]/g, "").trim() || "Place";
  return fitUtf8(nm, Math.max(1, PLACE_MAX - c.length)) + c;
}
function placeParse(v) {
  const m = /^([^|]+)\|(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)$/.exec(v || "");
  return m ? { name: m[1], lat: +m[2], lon: +m[3] } : null;
}
function placeLine(v) {
  const p = placeParse(v);
  return p ? p.name + " (" + p.lat.toFixed(2) + ", " + p.lon.toFixed(2) + ")" : "not set";
}
// one string setting: a place, or a line of text
function stringRow(app, id, help) {
  const box = document.createElement("div");
  box.className = "apsset";
  const cur = document.createElement("span"), msg = document.createElement("span");
  cur.className = msg.className = "hint";
  const name = document.createElement("strong");
  name.textContent = help.label || id;
  const head = document.createElement("div");
  head.className = "apshead";
  head.append(name, cur);
  box.appendChild(head);
  if (help.help) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = help.help;
    box.appendChild(p);
  }
  const place = help.kind === "place";
  const url = cfgURL(app.slug, id);
  const show = (v) => { cur.textContent = place ? placeLine(v) : (v ? "\u201c" + v + "\u201d" : "not set"); };
  const write = async (v, what) => {
    msg.textContent = what + "\u2026";
    try {
      await readDeviceJSON(await hubFetch(url + "&value=" + encodeURIComponent(v), { method: "POST" }));
      const d = await readDeviceJSON(await hubFetch(url));
      show(d.value);
      msg.textContent = v ? "saved; the app reads it at once" : "cleared";
      return true;
    } catch (e) {
      msg.textContent = "the orb refused it: " + e.message;
      return false;
    }
  };
  if (place) box.appendChild(placeSearch(app, id, help, write, msg));
  else {
    const form = document.createElement("div");
    form.className = "row";
    const q = document.createElement("input"), save = document.createElement("button"), clr = document.createElement("button");
    q.size = 30; q.autocomplete = "off"; q.placeholder = help.placeholder || "";
    q.setAttribute("aria-label", (app.title || app.slug) + " " + (help.label || id));
    save.type = clr.type = "button";
    save.className = "primary"; clr.className = "ghost";
    save.textContent = "Save"; clr.textContent = "Clear";
    form.append(q, save, clr, msg);
    save.onclick = () => write(q.value.trim(), "saving").then((ok) => { if (ok) q.value = ""; });
    clr.onclick = () => write("", "clearing");
    box.appendChild(form);
  }
  hubFetch(url).then(readDeviceJSON).then((d) => show(d.value))
    .catch((e) => { cur.textContent = "unreadable: " + e.message; });
  return box;
}
// A place setting's search: the Home card's searcher without its map (owner, 2026-11-06).
// Typing lists the matching cities at once; Enter takes the first, or reads coordinates
// or a full plus code, which the orb names the way it names a home (GET /api/v1/home's
// proposal, which stores nothing); this browser's location works the same. A pick waits
// beside its Save with its name editable, as Home's waits beside Set location.
function placeSearch(app, id, help, write, msg, encode) {
  encode = encode || placeValue;
  const side = document.createElement("div");
  side.className = "locside";
  const q = document.createElement("input");
  q.placeholder = "a city, 33.4484, -112.074, or 8559CWXG+9C";
  q.autocomplete = "off";
  q.setAttribute("aria-label", (app.title || app.slug) + " " + (help.label || id) + ": find a city, or type coordinates or a full plus code");
  const hits = document.createElement("div");
  hits.className = "tzlist"; hits.setAttribute("role", "listbox"); hits.hidden = true;
  const btns = document.createElement("div");
  btns.className = "locbtns";
  const here = document.createElement("button"), clr = document.createElement("button");
  here.type = clr.type = "button";
  here.className = clr.className = "ghost";
  here.textContent = "Use this browser's location"; here.title = "Ask this browser where it is";
  clr.textContent = "Clear"; clr.title = "Forget this place";
  btns.append(here, clr);
  const next = document.createElement("div");
  next.className = "locnext"; next.hidden = true;
  const pendAt = document.createElement("span"), nm = document.createElement("input"), save = document.createElement("button");
  nm.size = 16; nm.autocomplete = "off"; nm.style.width = "12em";
  nm.setAttribute("aria-label", "The name the orb shows for this place");
  nm.title = "The name the orb shows. The orb keeps " + PLACE_MAX + " characters with the position, so a long name is cut.";
  save.type = "button"; save.className = "primary"; save.textContent = "Save";
  next.append(nm, pendAt, save);
  side.append(q, hits, btns, next, msg);
  let pend = null, ask = 0;
  const show = (list) => {
    hits.innerHTML = "";
    hits.hidden = !list.length;
    for (const c of list) {
      const b = document.createElement("button");
      b.type = "button"; b.className = "tzrow"; b.setAttribute("role", "option");
      b.textContent = cityLabel(c);
      b.onclick = () => { q.value = cityLabel(c); show([]); propose(c[2], c[3], c[0]); };
      hits.appendChild(b);
    }
  };
  const propose = (lat, lon, name) => {
    pend = { lat: +lat, lon: +lon };
    nm.value = name;
    pendAt.textContent = pend.lat.toFixed(2) + ", " + pend.lon.toFixed(2);   /* what the orb keeps */
    next.hidden = false;
    msg.textContent = "";
    nm.focus();
  };
  // coordinates or a plus code: the orb's proposal names the point ("near Lansing")
  const ask_orb = async (query, what) => {
    const my = ++ask;
    msg.textContent = "asking the orb about " + what + "\u2026";
    try {
      const d = await readDeviceJSON(await hubFetch(homeURL(query)));
      if (my !== ask) return;
      const p = d && d.proposal;
      if (!p) throw new Error("the orb sent no preview");
      propose(p.lat, p.lon, p.place);
    } catch (e) {
      if (my === ask) msg.textContent = "the orb could not place " + what + ": " + e.message;
    }
  };
  q.addEventListener("input", async function () {
    const v = this.value.trim();
    if (v.length < 2 || v.includes("+") || HOME_COORDS.test(v) || /^[-+\d.,\s]+$/.test(v)) { show([]); return; }
    try { await homeCityList(); } catch (e) { msg.textContent = e.message; return; }
    if (this.value.trim() === v) show(homeFind(v));
  });
  q.addEventListener("keydown", function (ev) {
    if (ev.key === "Escape") { show([]); return; }
    if (ev.key !== "Enter") return;
    ev.preventDefault();
    const v = this.value.trim();
    if (!v) return;
    if (v.includes("+")) { show([]); ask_orb("?code=" + encodeURIComponent(v.split(/\s+/)[0]), v); return; }
    const m = v.match(HOME_COORDS);
    if (m) { show([]); ask_orb("?lat=" + encodeURIComponent(m[1]) + "&lon=" + encodeURIComponent(m[2]), v); return; }
    const first = hits.firstChild;
    if (first) first.click();
    else msg.textContent = "no city by that name in the orb's list: try a bigger one nearby, or type its coordinates";
  });
  here.onclick = () => {
    if (!navigator.geolocation) { msg.textContent = "this browser cannot say where it is"; return; }
    msg.textContent = "asking this browser where it is\u2026";
    navigator.geolocation.getCurrentPosition(
      (pos) => ask_orb("?lat=" + pos.coords.latitude.toFixed(5) + "&lon=" + pos.coords.longitude.toFixed(5), "this browser's location"),
      (e) => { msg.textContent = "the browser did not say: " + e.message; },
      { timeout: 15000 });
  };
  save.onclick = () => {
    if (!pend) return;
    write(encode(nm.value.trim() || "Place", pend.lat, pend.lon), "saving").then((ok) => {
      if (ok) { pend = null; next.hidden = true; q.value = ""; }
    });
  };
  nm.addEventListener("keydown", (ev) => { if (ev.key === "Enter") { ev.preventDefault(); save.click(); } });
  clr.onclick = () => { pend = null; next.hidden = true; show([]); write("", "clearing"); };
  return side;
}
// The OS's three places beside home (radar-plan §2.1): POST /api/v1/places with
// {"i", "lat", "lon", "name"} or {"i", "clear": 1}. The orb keeps 39 bytes of name (UTF-8)
// on its own, so the row passes the searcher an encoder that keeps the name whole.
const placesURL = () => "/orb/" + encodeURIComponent(current.ip) + "/api/v1/places";
// a set place, its position and its zone (OS v1.2.38 answers each place's zone: the one
// named, else its nearest city's), the zone marked when the orb's rule only approximates it
function placesLine(p) {
  if (!p || !p.set) return "not set";
  const zone = p.zone ? " · " + p.zone + (p.exact === false ? " (approximate)" : "") : "";
  return p.name + " (" + (+p.lat).toFixed(2) + ", " + (+p.lon).toFixed(2) + ")" + zone;
}
async function loadPlaces() {
  if (!current.ip) return;
  const box = $("#places");
  const d = await readDeviceJSON(await hubFetch(placesURL()));
  box.innerHTML = "";
  for (const p of d.places || []) {
    const row = document.createElement("div");
    row.className = "apsset";
    const head = document.createElement("div"), name = document.createElement("strong"), cur = document.createElement("span"), msg = document.createElement("span");
    head.className = "apshead";
    cur.className = msg.className = "hint";
    name.textContent = "Place " + (p.i + 1);
    cur.textContent = placesLine(p);
    head.append(name, cur);
    row.appendChild(head);
    const write = async (v, what) => {
      msg.textContent = what + "\u2026";
      const body = v ? { i: p.i, lat: v.lat, lon: v.lon, name: v.name } : { i: p.i, clear: 1 };
      try {
        const r = await readDeviceJSON(await hubFetch(placesURL(), {
          method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }));
        cur.textContent = placesLine((r.places || [])[p.i]);
        msg.textContent = v ? "saved; every app reads it at once" : "cleared";
        loadMaps().catch(() => {});                 /* a moved place needs new maps */
        return true;
      } catch (e) {
        msg.textContent = "the orb refused it: " + e.message;
        return false;
      }
    };
    const encode = (nm, lat, lon) => ({ name: fitUtf8(String(nm).replace(/["\\]/g, "").trim(), 39) || "Place",
                                        lat: +(+lat).toFixed(4), lon: +(+lon).toFixed(4) });
    row.appendChild(placeSearch({ title: "Place " + (p.i + 1), slug: "orb" }, "place" + p.i, { label: "Place " + (p.i + 1) },
                                write, msg, encode));
    box.appendChild(row);
  }
}
// ---- radar-plan §2.4 (R4): the OS maps -------------------------------------------------
//
// Home (place 0) and each place (1-3) get six levels, z6-z11, as /System/Maps/<place>-
// <lat_e4>_<lon_e4>-z<z>.orbmap on the orb's card. They are made here: vector tiles from
// the Map tiles source, fetched by the hub as gorb-os (/api/maps/fetch), decoded, clipped,
// simplified and written by maps@1 (the same bytes tools/orbmap.py writes), then sent with
// the card's own write door. A moved place has new names, so its old maps are never drawn
// for it; the files no place names are removed. The OS hears POST /api/v1/places/maps.
const MAPS_DIR = "System/Maps";
const MAPS_CACHE = "gorb-maps-tiles-v1";
let mapsKit = null;
let mapsBusy = false;
let mapsState = null;
function loadMapsKit() {
  if (mapsKit) return mapsKit;
  mapsKit = new Promise((ok, no) => {
    if (window.GorbMaps) return ok(window.GorbMaps);
    const el = document.createElement("script");
    el.src = "/runtime/maps@1/orbmap.js";
    el.onload = () => (window.GorbMaps ? ok(window.GorbMaps) : no(new Error("the map kit did not load")));
    el.onerror = () => { mapsKit = null; no(new Error("the hub did not serve the map kit")); };
    document.head.appendChild(el);
  });
  return mapsKit;
}
const orbURL = (path, ip) => "/orb/" + encodeURIComponent(ip || current.ip) + path;
async function hubData(url, opts) {
  const r = await hubFetch(url, opts);
  const j = await r.json();
  if (!j.ok) throw new Error(j.error || ("the hub refused " + url));
  return j.data;
}
// Home and the places as the OS names their maps: [{place, label, latE6, lonE6}] (set ones).
async function mapsPlaces(ip) {
  const [h, p] = await Promise.all([readDeviceJSON(await hubFetch(orbURL("/api/v1/home", ip))),
                                    readDeviceJSON(await hubFetch(orbURL("/api/v1/places", ip)))]);
  const out = [];
  const e6 = (v) => Math.round(+v * 1e6);
  if (h.home && h.home.set) out.push({ place: 0, label: "Home", latE6: e6(h.home.lat), lonE6: e6(h.home.lon) });
  for (const q of p.places || [])
    if (q.set) out.push({ place: q.i + 1, label: q.name || "Place " + (q.i + 1), latE6: e6(q.lat), lonE6: e6(q.lon) });
  return out;
}
async function mapsListing(ip) {
  const r = await hubFetch(orbURL("/api/v1/sd/ls?path=" + encodeURIComponent(MAPS_DIR), ip));
  const d = await r.json().catch(() => ({}));
  if (!r.ok) {
    if (/no such directory/.test(d.error || "")) return {};
    throw new Error(d.error || ("the orb's card answered " + r.status));
  }
  const files = {};
  for (const e of d.entries || []) if (!e.dir) files[e.name] = e.size;
  return files;
}
// What the card holds against what home and the places need.
async function mapsCompute(ip) {
  const G = await loadMapsKit();
  const [places, files] = await Promise.all([mapsPlaces(ip), mapsListing(ip)]);
  const want = {};
  for (const pl of places) {
    pl.names = G.ZOOMS.map((z) => G.mapName(pl.place, pl.latE6, pl.lonE6, z));
    pl.have = pl.names.filter((n) => files[n]).length;
    pl.bytes = pl.names.reduce((a, n) => a + (files[n] || 0), 0);
    pl.names.forEach((n) => { want[n] = 1; });
  }
  const stale = Object.keys(files).filter((n) => /\.orbmap$/.test(n) && !want[n]);
  return { places, stale, need: places.filter((pl) => pl.have < pl.names.length) };
}
async function loadMaps() {
  if (!current.ip) return;
  const rows = $("#maps-rows"), msg = $("#maps-msg"), make = $("#maps-make");
  try {
    mapsState = await mapsCompute();
  } catch (e) {
    rows.innerHTML = "";
    msg.textContent = "the maps could not be read: " + e.message;
    return;
  }
  const st = mapsState;
  /* Maps no place uses go as soon as they are seen (owner, 2026-11-08: a moved or cleared
   * place kept its old maps on the card until the next Make maps). */
  if (st.stale.length && !mapsBusy) {
    try {
      for (const name of st.stale)
        await readDeviceJSON(await hubFetch(orbURL("/api/v1/sd/rm?path=" + encodeURIComponent(MAPS_DIR + "/" + name)), { method: "POST" }));
      await readDeviceJSON(await hubFetch(orbURL("/api/v1/places/maps"), { method: "POST" }));
      msg.textContent = st.stale.length + " old map file" + (st.stale.length === 1 ? "" : "s") + " no place uses removed";
      st.stale = [];
    } catch (e) {
      msg.textContent = "old maps could not be removed: " + e.message;
    }
  }
  rows.innerHTML = "";
  for (const pl of st.places) {
    const row = document.createElement("div");
    const name = document.createElement("strong");
    const where = document.createElement("span");
    const tag = document.createElement("span");
    row.className = "maprow";
    where.className = "hint";
    name.textContent = pl.label;
    where.textContent = (pl.latE6 / 1e6).toFixed(4) + ", " + (pl.lonE6 / 1e6).toFixed(4) +
      (pl.have ? " · " + Math.round(pl.bytes / 1024) + " KB on the card" : "");
    const done = pl.have === pl.names.length;
    tag.className = "tag " + (done ? "on" : "upd");
    tag.textContent = done ? "maps made" : pl.have ? "maps out of date" : "no maps yet";
    row.append(name, where, tag);
    rows.appendChild(row);
  }
  if (!st.places.length) rows.innerHTML = '<div class="hint">set home on the Location card to make maps</div>';
  const out = st.need.length > 0;
  make.className = out ? "upd" : "";
  make.textContent = out ? "Maps out of date — Make maps" : "Make maps again";
  make.disabled = mapsBusy || !st.places.length;
  tabBadge("orb", st.need.length, st.need.length + " place" + (st.need.length === 1 ? "" : "s") + " without maps");
  if (!mapsBusy && !st.stale.length && !/removed$/.test(msg.textContent))
    msg.textContent = st.need.length ? "" : "every place has its maps";
  const sel = $("#maps-place"), keep = sel.value;
  sel.innerHTML = "";
  for (const pl of st.places) {
    const o = document.createElement("option");
    o.value = String(pl.place); o.textContent = pl.label;
    sel.appendChild(o);
  }
  if (keep && st.places.some((pl) => String(pl.place) === keep)) sel.value = keep;
  await mapsZooms().catch(() => {});
  try {
    const src = await hubData("/api/maps/source");
    if (document.activeElement !== $("#maps-src")) $("#maps-src").value = src.tilejson;
    $("#maps-src").placeholder = src.default;
  } catch (e) { $("#maps-srcmsg").textContent = e.message; }
}
// A tile, from this browser's cache or through the hub; null for a tile the host has not
// got (open ocean at some sources is a 204 or 404). Gzipped bodies are unwrapped.
async function mapsTile(url) {
  const cache = window.caches ? await caches.open(MAPS_CACHE).catch(() => null) : null;
  let r = cache ? await cache.match(url) : null;
  if (!r) {
    r = await hubFetch("/api/maps/fetch?u=" + encodeURIComponent(url));
    if (r.status === 204 || r.status === 404) return null;
    if (!r.ok) throw new Error((await r.text()).trim() || "the tile host answered " + r.status);
    if (cache) await cache.put(url, r.clone()).catch(() => {});
  }
  let b = new Uint8Array(await r.arrayBuffer());
  if (b.length > 1 && b[0] === 0x1f && b[1] === 0x8b)
    b = new Uint8Array(await new Response(new Blob([b]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
  return b;
}
async function mapsTemplate() {
  const src = (await hubData("/api/maps/source")).tilejson;
  const r = await hubFetch("/api/maps/fetch?u=" + encodeURIComponent(src));
  if (!r.ok) throw new Error((await r.text()).trim() || "the Map tiles source answered " + r.status);
  const tj = await r.json();
  if (!tj.tiles || !tj.tiles.length) throw new Error("the Map tiles source names no tiles (is it a TileJSON?)");
  return { url: tj.tiles[0], maxzoom: tj.maxzoom === undefined ? 14 : +tj.maxzoom };
}
// One level of one place: its tiles, four at a time, then the builder.
async function mapsLevel(G, tmpl, pl, z, onTile) {
  const lat = pl.latE6 / 1e6, lon = pl.lonE6 / 1e6;
  if (z > tmpl.maxzoom) throw new Error("the Map tiles source stops at z" + tmpl.maxzoom + "; the maps need z11");
  const want = G.tilesFor(lat, lon, z), tiles = [];
  let next = 0;
  const worker = async () => {
    while (next < want.length) {
      const t = want[next++];
      const url = tmpl.url.replace("{z}", t.z).replace("{x}", t.x).replace("{y}", t.y);
      const b = await mapsTile(url);
      if (b) tiles.push({ x: t.ox, y: t.y, layers: G.readTile(b) });
      onTile();
    }
  };
  await Promise.all([worker(), worker(), worker(), worker()]);
  return G.build(tiles, lat, lon, z).bytes;
}
// buildPlaceMaps makes the maps the orb at ip lacks (every place's again when none is
// missing, or always with all), writes them to its card, removes the ones no place names and
// tells the OS. say(text) hears each step. It answers null when the orb has no home or place
// set, else what it did. The Orb face's Make maps and the setup journey both run it (owner,
// 2026-11-09: "the initial install should also make the home maps").
async function buildPlaceMaps(ip, say, all) {
  const G = await loadMapsKit();
  const st = await mapsCompute(ip);
  if (!st.places.length) return null;
  const todo = all || !st.need.length ? st.places : st.need;   /* nothing missing: remake all */
  const n = todo.length * G.ZOOMS.length;
  let step = 0, tiles = 0;
  say("reading the Map tiles source…");
  const tmpl = await mapsTemplate();
  await readDeviceJSON(await hubFetch(orbURL("/api/v1/sd/mkdir?path=" + encodeURIComponent(MAPS_DIR), ip), { method: "POST" }));
  for (const pl of todo) {
    for (const z of G.ZOOMS) {
      step++;
      const tell = () => say("Step " + step + " of " + n + ": " + pl.label + ", level z" + z + " (" + tiles + " tiles fetched)");
      tell();
      const bytes = await mapsLevel(G, tmpl, pl, z, () => { tiles++; tell(); });
      const name = G.mapName(pl.place, pl.latE6, pl.lonE6, z);
      say("Step " + step + " of " + n + ": writing " + name + " (" + Math.round(bytes.length / 1024) + " KB)");
      await readDeviceJSON(await hubFetch(orbURL("/api/v1/sd/write?overwrite=1&path=" +
        encodeURIComponent(MAPS_DIR + "/" + name), ip), { method: "POST", body: bytes }));
    }
  }
  for (const name of st.stale)
    await readDeviceJSON(await hubFetch(orbURL("/api/v1/sd/rm?path=" + encodeURIComponent(MAPS_DIR + "/" + name), ip), { method: "POST" }));
  await readDeviceJSON(await hubFetch(orbURL("/api/v1/places/maps", ip), { method: "POST" }));
  return { places: todo.length, levels: n, tiles, removed: st.stale.length };
}
async function makeMaps() {
  if (mapsBusy || !mapsState) return;
  const msg = $("#maps-msg"), make = $("#maps-make");
  mapsBusy = true; make.disabled = true;
  try {
    const r = await buildPlaceMaps(current.ip, (t) => { msg.textContent = t; });
    if (r) msg.textContent = "DONE: " + r.places + " place" + (r.places === 1 ? "" : "s") + ", " + r.levels + " levels, " + r.tiles +
      " tiles" + (r.removed ? "; " + r.removed + " old file" + (r.removed === 1 ? "" : "s") + " removed" : "") +
      ". Apps redraw their maps now.";
  } catch (e) {
    msg.textContent = "the maps were not made: " + e.message;
  } finally {
    mapsBusy = false;
    await loadMaps().catch(() => {});
  }
}
// The preview draws the level the orb would use at the chosen radius, built from the
// tiles (this browser's cache first), so it never reads the card.
// by role (FORMAT 2): primary road 5, motorway 6, secondary 13, tertiary 14
const MAPS_PREVIEW_PAL = [0xff0b2a3c, 0xff1b2228, 0xff2c3138, 0xff123a52, 0xff2f6f99, 0xff8a929c, 0xffc9ced6,
  0xff8a7fb0, 0xffd0d0d0, 0xff3d7fa8, 0xffffffff, 0xffffc040, 0xffe0e0e0, 0xff5f6771, 0xff4a5059, 0];
// The radar's six zooms (apps/radar/radar.c rd_radius_m, rd_scale), named as on the dial and
// each with the scale bar's round length in the orb's own unit, so the preview and the orb
// agree (owner, 2026-11-08: the list was always km, and its radii were not the app's).
const MAPS_ZOOMS = ["Wide", "Region", "Local", "Area", "City", "Close"];
const MAPS_UNIT_M = { km: 1000, mi: 1609.344, nm: 1852 };
const mapsRadius = (step, latE6) => Math.floor(156543.03392 * Math.cos(latE6 / 1e6 * Math.PI / 180) / Math.pow(2, 6 + step) * 233 + 0.5);
function mapsScale(radiusM, unit, maxPx) {
  const u = MAPS_UNIT_M[unit] || 1000, mpp = radiusM / 233;
  let best = 1;
  for (const n of [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000]) if (n * u / mpp <= maxPx) best = n;
  return { len: best, px: Math.floor(best * u / mpp + 0.5) };
}
let mapsDist = "km";
async function mapsZooms() {
  try { mapsDist = (await readDeviceJSON(await hubFetch(unitsURL()))).dist || "km"; } catch (e) { /* km */ }
  const sel = $("#maps-radius"), keep = sel.value || "3";
  const pl = mapsState && mapsState.places.find((q) => String(q.place) === $("#maps-place").value);
  const lat = pl ? pl.latE6 : 0;
  sel.innerHTML = "";
  MAPS_ZOOMS.forEach((nm, i) => {
    const o = document.createElement("option"), sc = mapsScale(mapsRadius(i, lat), mapsDist, 150);
    o.value = String(i); o.textContent = nm + "  " + sc.len + " " + mapsDist;
    sel.appendChild(o);
  });
  sel.value = keep;
}
$("#maps-place").addEventListener("change", () => { mapsZooms().catch(() => {}); });
async function mapsPreview() {
  const pm = $("#maps-prevmsg");
  const pl = mapsState && mapsState.places.find((q) => String(q.place) === $("#maps-place").value);
  if (!pl) { pm.textContent = "set home or a place first"; return; }
  try {
    const G = await loadMapsKit();
    const step = +$("#maps-radius").value || 0, radius = mapsRadius(step, pl.latE6), lv = G.pickLevel(pl.latE6, radius);
    pm.textContent = "fetching level z" + lv.z + "…";
    const tmpl = await mapsTemplate();
    const bytes = await mapsLevel(G, tmpl, pl, lv.z, () => {});
    const st = { scale: lv.scale, rot_rad: 0, role_on: 0xffff, mark_on: 0x0b,
                 width: [0, 0, 0, 0, 1, 1, 2, 1, 2, 1, 0, 0, 2, 1, 1, 0], dash: new Array(16).fill(0) };
    const r = G.render(bytes, st);
    const cv = $("#maps-canvas"), ctx = cv.getContext("2d"), img = ctx.createImageData(G.W, G.W);
    G.paint(r.raster, MAPS_PREVIEW_PAL, img.data);
    ctx.putImageData(img, 0, 0);
    const sc = mapsScale(radius, mapsDist, 150);              /* the orb's scale bar, as it draws it */
    const x0 = Math.round(233 - sc.px / 2);
    ctx.fillStyle = "#05090d"; ctx.fillRect(x0 - 2, 439, sc.px + 4, 4); ctx.fillRect(x0 - 2, 430, 4, 13); ctx.fillRect(x0 + sc.px - 2, 430, 4, 13);
    ctx.fillStyle = "#d8f4ee"; ctx.fillRect(x0, 440, sc.px, 2); ctx.fillRect(x0, 431, 2, 11); ctx.fillRect(x0 + sc.px - 2, 431, 2, 11);
    ctx.font = "26px 'IBM Plex Sans', sans-serif"; ctx.textAlign = "center"; ctx.textBaseline = "middle";
    ctx.lineWidth = 4; ctx.strokeStyle = "#05090d"; ctx.strokeText(sc.len + " " + mapsDist, 233, 414);
    ctx.fillText(sc.len + " " + mapsDist, 233, 414);
    pm.textContent = pl.label + " at " + MAPS_ZOOMS[step] + " (scale bar " + sc.len + " " + mapsDist + "): level z" + lv.z +
      " drawn at " + lv.scale.toFixed(2) + "x, " + Math.round(bytes.length / 1024) + " KB, " + r.marks.length + " landmarks";
  } catch (e) {
    pm.textContent = "no preview: " + e.message;
  }
}
$("#maps-make").onclick = () => { makeMaps(); };
$("#maps-preview").onclick = () => { mapsPreview(); };
$("#maps-src-save").onclick = async () => {
  const m = $("#maps-srcmsg");
  try {
    const d = await hubData("/api/maps/source", { method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ tilejson: $("#maps-src").value.trim() }) });
    $("#maps-src").value = d.tilejson;
    m.textContent = d.tilejson === d.default ? "OpenFreeMap's planet (the default)" : "saved; Make maps uses it";
  } catch (e) { m.textContent = e.message; }
};
// The parts every row shares: its name, what it is now, its help and its links.
function setHead(box, label, help) {
  const cur = document.createElement("span"), name = document.createElement("strong");
  cur.className = "hint";
  name.textContent = label;
  const head = document.createElement("div");
  head.className = "apshead";
  head.append(name, cur);
  box.appendChild(head);
  if (help && help.help) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = help.help;
    box.appendChild(p);
  }
  const links = help && Array.isArray(help.links) ? help.links.filter((l) => l && /^https?:\/\//.test(l.href || "")) : [];
  if (links.length) {
    const p = document.createElement("p");
    p.className = "apslinks";
    for (const l of links) {
      const a = document.createElement("a");
      a.href = l.href; a.target = "_blank"; a.rel = "noopener noreferrer";
      a.textContent = (l.text || l.href) + " ↗";
      p.appendChild(a);
    }
    box.appendChild(p);
  }
  return cur;
}
// a choice's label in the orb's distance unit, when it gives one per unit
const choiceText = (c, dist) => (c && typeof c === "object" ? c[dist] || c.km : String(c));
// a stored number as the owner reads it: its decimals and its unit, as the dial draws it
function numText(s, v) {
  const d = s.decimals || 0, u = s.unit || "";
  const sp = u && u !== "%" && u !== "°" ? " " : "";
  return (d ? (v / Math.pow(10, d)).toFixed(d) : String(v)) + sp + u;
}
function valueText(s, v, dist) {
  if (s.type === "int" || s.type === "number") return numText(s, v);
  if (s.type === "enum" || s.type === "toggle") return choiceText((s.choices || [])[v], dist) || String(v);
  if (s.type === "multi") {
    const on = (s.choices || []).filter((_c, i) => (v >> i) & 1).map((c) => choiceText(c, dist));
    return on.length ? on.join(", ") : "none";
  }
  return String(v);
}
// One setting the knob also edits (int, number, enum, multi, toggle), drawn for a page.
function valueRow(app, s, value, dist) {
  const box = document.createElement("div");
  box.className = "apsset";
  const cur = setHead(box, s.label || s.id, s);
  const msg = document.createElement("span");
  msg.className = "hint";
  const url = cfgURL(app.slug, s.id);
  let now = value;
  const show = () => { cur.textContent = valueText(s, now, dist); };
  const write = async (v) => {
    msg.textContent = "saving…";
    try {
      await readDeviceJSON(await hubFetch(url + "&value=" + encodeURIComponent(String(v)), { method: "POST" }));
      now = v; show();
      msg.textContent = "saved; the app reads it at once";
    } catch (e) {
      msg.textContent = "the orb refused it: " + e.message;
    }
  };
  const form = document.createElement("div");
  form.className = "row";
  if (s.type === "int" || s.type === "number") {
    const k = Math.pow(10, s.decimals || 0), step = (s.step || 1) / k;
    const range = document.createElement("input"), num = document.createElement("input");
    range.type = "range";
    num.type = "number";
    for (const el of [range, num]) { el.min = s.min / k; el.max = s.max / k; el.step = step; el.value = now / k; }
    range.style.width = "14em"; num.style.width = "6em";
    range.setAttribute("aria-label", s.label); num.setAttribute("aria-label", s.label);
    range.oninput = () => { num.value = range.value; };
    num.oninput = () => { range.value = num.value; };
    const commit = (el) => {
      const v = Math.round(+el.value * k);
      if (!isFinite(v) || v < s.min || v > s.max) { msg.textContent = "between " + numText(s, s.min) + " and " + numText(s, s.max); return; }
      if (v !== now) write(v);
    };
    range.onchange = () => commit(range);
    num.onchange = () => commit(num);
    const unit = document.createElement("span");
    unit.className = "hint"; unit.textContent = s.unit || "";
    form.append(range, num, unit, msg);
  } else if (s.type === "enum") {
    const sel = document.createElement("select");
    sel.setAttribute("aria-label", s.label);
    (s.choices || []).forEach((c, i) => {
      const o = document.createElement("option");
      o.value = String(i); o.textContent = choiceText(c, dist);
      sel.appendChild(o);
    });
    sel.value = String(now);
    sel.onchange = () => write(+sel.value);
    form.append(sel, msg);
  } else if (s.type === "toggle") {
    const seg = document.createElement("span");
    seg.className = "seg";
    const btns = (s.choices || ["off", "on"]).map((c, i) => {
      const b = document.createElement("button");
      b.type = "button"; b.className = "ghost"; b.textContent = choiceText(c, dist);
      b.onclick = () => { if (i !== now) write(i).then(mark); };
      seg.appendChild(b);
      return b;
    });
    const mark = () => btns.forEach((b, i) => b.classList.toggle("on", i === now));
    mark();
    form.append(seg, msg);
  } else if (s.type === "multi") {
    (s.choices || []).forEach((c, i) => {
      const lab = document.createElement("label"), cb = document.createElement("input");
      lab.className = "inline";
      cb.type = "checkbox"; cb.checked = !!((now >> i) & 1);
      cb.onchange = () => write(cb.checked ? now | (1 << i) : now & ~(1 << i));
      lab.append(cb, " " + choiceText(c, dist));
      form.appendChild(lab);
    });
    form.appendChild(msg);
  }
  box.appendChild(form);
  show();
  return box;
}
function apsTabs() {
  const nav = $("#aps-tabs");
  nav.innerHTML = "";
  for (const a of apsApps) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "tab sub" + (a.slug === apsSub ? " on" : "");
    b.textContent = a.title || a.slug;
    b.onclick = () => { apsSub = a.slug; apsTabs(); apsPage().catch(() => {}); };
    nav.appendChild(b);
  }
}
async function apsPage() {
  const box = $("#secrets"), app = apsApps.find((a) => a.slug === apsSub);
  box.innerHTML = "";
  if (!app) return;
  let d;
  try {
    d = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) +
                                            "/api/v1/apps/settings?id=" + encodeURIComponent(app.slug)));
  } catch (e) {
    box.innerHTML = '<p class="hint"></p>';
    box.firstChild.textContent = "the orb did not answer " + (app.title || app.slug) + "'s settings: " + e.message +
      ". An orb older than v1.2.19 cannot describe them; update its firmware.";
    return;
  }
  if (app.slug !== apsSub) return;   // another tab was pressed while the page was read
  box.innerHTML = "";
  const shown = (d.settings || []).filter((s) => s.show === "hub" || s.show === "both");
  if (!shown.length) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = (app.title || app.slug) + " has no settings to change here." +
      ((d.settings || []).length ? " Its choices live on the orb's own menu." : "");
    box.appendChild(p);
    return;
  }
  const values = d.values || {};
  for (const s of shown) {
    if (s.type === "secret") box.appendChild(secretRow(app, s.id, { label: s.label, help: s.help, links: s.links, header: s.header }));
    else if (s.type === "text" || s.type === "place")
      box.appendChild(stringRow(app, s.id, { kind: s.type, label: s.label, help: s.help }));
    else box.appendChild(valueRow(app, s, values[s.id], d.dist || "km"));
  }
}
async function loadSecrets() {
  if (!current.ip) return;
  const d = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/apps"));
  apsApps = (d.installed || []).filter((a) => !a.compiled)
    .map((a) => Object.assign({}, a, { title: a.title || APP_TITLES[a.slug] || a.slug }))
    .sort((x, y) => String(x.title).localeCompare(String(y.title)));
  if (!apsApps.some((a) => a.slug === apsSub))
    apsSub = (apsApps.find((a) => (a.secrets || []).length || a.n_settings) || apsApps[0] || {}).slug || "";
  apsTabs();
  if (!apsApps.length) {
    $("#secrets").innerHTML = '<p class="hint">no apps installed on this orb</p>';
    return;
  }
  await apsPage();
}

// ---- the search box: a city, coordinates, or a full plus code ----
const HOME_COORDS = /^\s*([-+]?\d*\.?\d+)\s*[, ]\s*([-+]?\d*\.?\d+)\s*$/;

async function homeCityList() {
  if (homeCities) return homeCities;
  const r = await fetch("/web/cities.json");
  if (!r.ok) throw new Error("the city list did not load (" + r.status + ")");
  homeCities = (await r.json()).cities;
  return homeCities;
}

// A row is [name, country, lat, lon, admin1]: "Traverse City, MI, US".
const cityLabel = (c) => c[0] + (c[4] && /^[A-Z]{2,3}$/.test(c[4]) ? ", " + c[4] : "") + ", " + c[1];

// Names that start with the words typed first, then names that contain them;
// biggest first within each, which is the file's order. A trailing ", XX" narrows to
// a country or a state: "Portland, US", "Springfield, MO", "Lansing, MI".
function homeFind(q) {
  let f = q.toLowerCase(), within = "";
  const m = f.match(/^(.*?)\s*,\s*([a-z]{2,3})$/);
  if (m) { f = m[1]; within = m[2].toUpperCase(); }
  const head = [], rest = [];
  for (const c of homeCities) {
    if (within && c[1] !== within && c[4] !== within) continue;
    const n = c[0].toLowerCase();
    if (n.startsWith(f)) head.push(c); else if (n.includes(f) && rest.length < 8) rest.push(c);
    if (head.length >= 8) break;
  }
  return head.concat(rest).slice(0, 8);
}

function homeHits(list) {
  const box = $("#home-hits");
  box.innerHTML = "";
  box.hidden = !list.length;
  for (const c of list) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "tzrow";
    b.setAttribute("role", "option");
    b.textContent = cityLabel(c);
    b.onclick = () => homeCity(c);
    box.appendChild(b);
  }
}

function homeCity(c) {
  $("#home-q").value = cityLabel(c);
  homeHits([]);
  homePropose("?lat=" + c[2] + "&lon=" + c[3], c[0], true, c[0]);
}

$("#home-q").addEventListener("input", async function () {
  const v = this.value.trim();
  if (v.length < 2 || v.includes("+") || HOME_COORDS.test(v) || /^[-+\d.,\s]+$/.test(v)) { homeHits([]); return; }
  try { await homeCityList(); } catch (e) { $("#home-msg").textContent = e.message; return; }
  if (this.value.trim() === v) homeHits(homeFind(v));
});

$("#home-q").addEventListener("keydown", function (ev) {
  if (ev.key === "Escape") { homeHits([]); return; }
  if (ev.key !== "Enter") return;
  ev.preventDefault();
  const v = this.value.trim();
  if (!v) return;
  if (v.includes("+")) { homeHits([]); homePropose("?code=" + encodeURIComponent(v.split(/\s+/)[0]), v, true); return; }
  const m = v.match(HOME_COORDS);
  if (m) { homeHits([]); homePropose("?lat=" + encodeURIComponent(m[1]) + "&lon=" + encodeURIComponent(m[2]), v, true); return; }
  const first = $("#home-hits").firstChild;
  if (first) first.click();
  else $("#home-msg").textContent = "no city by that name in the orb's list — try a bigger one nearby, or pin the map";
});

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
  // The shelf is read BEFORE the rows are built, not after. renderAppsTab merges
  // libCache into the row set, and a cache that is still null on the first paint
  // drops every "in the hub library" row -- the face showed a shelf it had not
  // read yet, and an operator who knew color_picker.orbapp was on the disk saw a
  // tab that quietly denied it (bench, owner, 2026-10-29). The same ordering rule
  // the Themes face already obeys: enumeration first, rows second.
  if (typeof renderLibrary === "function") {
    try { await renderLibrary(); } catch (e) { /* renderLibrary reports its own failure */ }
  }
  await loadCarousel();
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

// ---- §59: the carousel is the app list -------------------------------------------------
// The list IS the carousel (owner, 2026-11-08: "the app list itself draggable, then select
// the ones with a checkbox that we want in the auto rotation"): the apps on the dial come
// first, in the order the knob walks them, each moved by dragging its grip; the apps only
// on the card or in the hub library follow. Rotation is one switch and an interval above
// the list, and while it is on every app on the dial carries a checkbox for whether the
// rotation stops on it. The order is set only here (owner, 2026-11-07); the rotation is
// also on the dial. Every change posts and redraws from the orb's answer, so the face
// never shows an order the orb did not take.
const ROT_CHOICES = [[15, "15 seconds"], [30, "30 seconds"], [60, "1 minute"], [120, "2 minutes"],
  [300, "5 minutes"], [600, "10 minutes"], [900, "15 minutes"], [1800, "30 minutes"], [3600, "1 hour"]];
let carData = null;
let carFocus = "";          // the slug whose grip keeps the keyboard's focus across a redraw

async function loadCarousel() {
  carData = null;
  if (!current.ip || fwBelow("v1.2.20")) return;
  try {
    carData = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/carousel"));
  } catch (e) { carData = { error: String(e && e.message || e) }; }
}

async function carouselPost(form, say) {
  const state = $("#mn-rot-state");
  if (state) state.textContent = "saving…";
  let msg;
  try {
    const r = await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/carousel", {
      method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" },
      // Spelled by hand, not URLSearchParams: that encodes the commas as %2C, and the
      // device's form reader decodes nothing, so "a%2Cb" was one unknown slug and every
      // reorder and every second rotation box was silently ignored (Gold, 2026-11-08).
      // Slugs are [a-z0-9_] and the numbers are digits: nothing here needs escaping.
      body: Object.entries(form).map(([k, v]) => k + "=" + v).join("&"),
    });
    const d = await readDeviceJSON(r);
    if (!d || d.error) throw new Error((d && d.error) || ("HTTP " + r.status));
    carData = d;
    msg = say || "saved";
  } catch (e) {
    msg = "the orb refused: " + (e && e.message || e);
  }
  // A refusal redraws too: the rows go back to the order the orb still holds.
  renderAppsTab();
  const st = $("#mn-rot-state");
  if (st) st.textContent = msg;
}

function carApps() {
  return carData && !carData.error && Array.isArray(carData.apps) ? carData.apps : null;
}

function renderRotation() {
  const box = $("#mn-apps-rot");
  if (!box) return;
  box.innerHTML = "";
  if (!appList) return;
  const hint = (t) => { const p = document.createElement("p"); p.className = "hint"; p.textContent = t; box.appendChild(p); };
  if (fwBelow("v1.2.20")) {
    hint("Putting the apps in your own order and rotating them needs firmware v1.2.20 or later: update this orb on the Firmware tab.");
    return;
  }
  if (!carApps()) {
    hint("The orb did not answer its app order" + (carData && carData.error ? ": " + carData.error : "") + ".");
    return;
  }
  const rot = carData.rotate || { on: false, every_s: 60 };
  const bar = document.createElement("div");
  bar.className = "row toolbar";
  const on = document.createElement("label");
  on.className = "check inline";
  const cb = document.createElement("input");
  cb.type = "checkbox"; cb.id = "mn-rot-on"; cb.checked = !!rot.on;
  on.append(cb, document.createTextNode("Rotate apps"));
  const every = document.createElement("label");
  every.className = "inline";
  const sel = document.createElement("select");
  sel.id = "mn-rot-every";
  sel.disabled = !rot.on;
  const choices = ROT_CHOICES.slice();
  if (!choices.some(([s]) => s === rot.every_s)) choices.push([rot.every_s, rot.every_s + " seconds"]);
  for (const [s, t] of choices) {
    const o = document.createElement("option");
    o.value = String(s); o.textContent = t; o.selected = s === rot.every_s;
    sel.appendChild(o);
  }
  every.append(document.createTextNode("every "), sel);
  cb.onchange = () => carouselPost(cb.checked ? { rotate: "1", every_s: sel.value } : { rotate: "0" },
    cb.checked ? "rotation on: tick the apps it stops on" : "rotation off");
  sel.onchange = () => carouselPost({ rotate: "1", every_s: sel.value },
    "the dial moves on after " + sel.options[sel.selectedIndex].textContent + " left alone");
  const state = document.createElement("span");
  state.className = "hint"; state.id = "mn-rot-state";
  state.textContent = rot.on
    ? "Drag \u283f to order the apps. Rotation stops on the ticked ones, once the knob is left alone for the whole interval."
    : "Drag \u283f to set the order the knob and the carousel walk the apps in.";
  bar.append(on, every, state);
  box.appendChild(bar);
}

// sortable lets a row be dragged by its grip. It is pointer events, not HTML5 drag and
// drop, so a mouse, a pen and a finger behave alike: the row follows the pointer into its
// slot as it crosses the others' midlines (the others slide aside), and a drop that moved
// it posts the whole order once. The grip is a button, so the arrow keys move a row too.
function sortable(list, commit) {
  const rows = () => Array.from(list.querySelectorAll(".arow[data-sort]"));
  const order = () => rows().map((r) => r.dataset.slug).join(",");
  // FLIP: the rows that moved slide from where they were instead of jumping.
  const glide = (fn, skip) => {
    const before = new Map(rows().map((r) => [r, r.getBoundingClientRect().top]));
    fn();
    for (const r of rows()) {
      if (r === skip) continue;
      const dy = before.get(r) - r.getBoundingClientRect().top;
      if (!dy) continue;
      r.style.transition = "none";
      r.style.transform = "translateY(" + dy + "px)";
      requestAnimationFrame(() => { r.style.transition = "transform .15s ease"; r.style.transform = ""; });
    }
  };
  for (const row of rows()) {
    const grip = row.querySelector(".grip");
    grip.addEventListener("pointerdown", (ev) => {
      if (ev.button !== 0) return;
      ev.preventDefault();
      // The window hears the drag, not the grip: moving the row in the document releases
      // the grip's pointer capture, and a grip that stopped hearing never saw the drop.
      const pid = ev.pointerId;
      const from = order();
      const grab = ev.clientY - row.getBoundingClientRect().top;
      row.classList.add("dragging");
      list.classList.add("sorting");
      // The lifted row follows the pointer, held inside the span of the rows it sorts among.
      const follow = (y) => {
        row.style.transform = "";
        const rs = rows(), h = row.getBoundingClientRect().height;
        const lo = rs[0].getBoundingClientRect().top, hi = rs[rs.length - 1].getBoundingClientRect().bottom - h;
        const top = row.getBoundingClientRect().top;
        row.style.transform = "translateY(" + (Math.min(Math.max(y - grab, lo), hi) - top) + "px)";
      };
      const move = (e) => {
        if (e.pointerId !== pid) return;
        const others = rows().filter((r) => r !== row);
        let before = null;
        for (const o of others) {
          const b = o.getBoundingClientRect();
          if (e.clientY < b.top + b.height / 2) { before = o; break; }
        }
        const last = others[others.length - 1];
        const want = before ? row.nextElementSibling !== before : last && last.nextElementSibling !== row;
        if (want) glide(() => { if (before) list.insertBefore(row, before); else last.after(row); }, row);
        follow(e.clientY);
      };
      const end = (e) => {
        if (e.pointerId !== pid) return;
        window.removeEventListener("pointermove", move);
        window.removeEventListener("pointerup", end);
        window.removeEventListener("pointercancel", end);
        row.classList.remove("dragging");
        list.classList.remove("sorting");
        row.style.transition = "transform .12s ease";
        row.style.transform = "";
        const to = order();
        if (to !== from) commit(to, row.dataset.slug);
      };
      window.addEventListener("pointermove", move);
      window.addEventListener("pointerup", end);
      window.addEventListener("pointercancel", end);
    });
    grip.addEventListener("keydown", (ev) => {
      const d = ev.key === "ArrowUp" ? -1 : ev.key === "ArrowDown" ? 1 : 0;
      if (!d) return;
      ev.preventDefault();
      const rs = rows(), i = rs.indexOf(row), j = i + d;
      if (j < 0 || j >= rs.length) return;
      glide(() => { if (d < 0) list.insertBefore(row, rs[j]); else rs[j].after(row); });
      carFocus = row.dataset.slug;
      commit(order(), row.dataset.slug);
    });
  }
}

// appRows is D38's one list: every app the hub can see in any of its three homes — the
// dial's ledger, the card's folder, the hub's shelf — merged by slug. The section labels
// are gone because they were the same fact three times: an app is one thing, and the row
// says where it stands and what the press would do.
function appRows() {
  const seen = new Map();
  for (const a of appList || [])
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
  return Array.from(seen.values()).sort((x, y) => String(x.title).localeCompare(String(y.title)));
}

function renderAppsTab() {
  const body = $("#mn-apps-body");
  $("#mn-apps-tabs").innerHTML = "";
  body.innerHTML = "";
  renderRotation();
  if (!appList) return;
  const rows = appRows();
  if (!rows.length) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = "nothing to install — the dial has no apps, its card holds none, "
      + "and the hub's shelf is empty. Import one below.";
    body.appendChild(p);
  }
  // The dial's apps in the carousel's order, then everything else by name.
  const car = carApps();
  let onDial = [], rest = rows;
  if (car) {
    const by = new Map(rows.map((e) => [e.slug, e]));
    onDial = car.map((c) => Object.assign(by.get(c.slug) || { slug: c.slug, title: c.title || c.slug }, { car: c }));
    const inCar = new Set(car.map((c) => c.slug));
    rest = rows.filter((e) => !inCar.has(e.slug));
  }
  const rotOn = !!(car && carData.rotate && carData.rotate.on);
  const list = document.createElement("div");
  list.className = "applist";
  for (const e of onDial) list.appendChild(appRow(e, { sort: onDial.length > 1, rotOn }));
  if (onDial.length && rest.length) {
    const sep = document.createElement("p");
    sep.className = "hint sep";
    sep.textContent = "Not on this orb";
    list.appendChild(sep);
  }
  for (const e of rest) list.appendChild(appRow(e, { sort: false, rotOn }));
  body.appendChild(list);
  if (onDial.length > 1) {
    sortable(list, (order, slug) => {
      const t = (onDial.find((e) => e.slug === slug) || {}).title || slug;
      carouselPost({ order }, t + " is now number " + (order.split(",").indexOf(slug) + 1) + " in the carousel");
    });
  }
  if (carFocus) {
    const g = list.querySelector('.arow[data-slug="' + carFocus + '"] .grip');
    if (g) g.focus();
    carFocus = "";
  }
  const ups = rows.filter((e) => updateGate(e)).length;
  tabBadge("apps", ups, ups + " app update" + (ups === 1 ? "" : "s") + " in the hub library");
  const st = $("#mn-appstate");
  // The list redraws on every carousel change: one chip, replaced, never one per redraw.
  if (st) st.querySelectorAll(".tag.upd").forEach((t) => t.remove());
  if (st && ups) {
    const u = document.createElement("span");
    u.className = "tag upd";
    u.style.marginLeft = "8px";
    u.textContent = ups + " update" + (ups === 1 ? "" : "s") + " available";
    st.appendChild(u);
  }
  if (typeof paintUpdateAll === "function") paintUpdateAll();
  // Refused shelf files and the resolved directory lines print under the list:
  // they are the hub's own inventory report, not an app the operator can act on.
  renderLibrary();
}

// fwBelow: this orb's firmware is older than release v (vX.Y.Z); false when either is unread
function fwBelow(v) {
  const p = (x) => { const m = /v(\d+)\.(\d+)\.(\d+)/.exec(x || ""); return m ? (+m[1]) * 1e6 + (+m[2]) * 1e3 + (+m[3]) : null; };
  const have = p(fwStamp), need = p(v);
  return have != null && need != null && have < need;
}

// tabBadge puts a count on a face's tab (owner, 2026-11-04: updates are seen without
// opening the face): n > 0 shows it, 0 takes it away.
function tabBadge(tab, n, tip) {
  const t = document.querySelector('#orb-tabs .tab[data-tab="' + tab + '"]');
  if (!t) return;
  let b = t.querySelector(".badge");
  if (!n) { if (b) b.remove(); return; }
  if (!b) { b = document.createElement("span"); b.className = "badge"; t.appendChild(b); }
  b.textContent = String(n);
  b.title = tip || "";
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
  // A card copy refused for its FORMAT is a release the firmware moved past: the library's
  // bytes replace it whatever the dial's slot says (owner, 2026-11-08; Gold showed the
  // amber "Update replaces it" chip with no Update to press).
  if (e.card && !e.card.ok && /^app format /.test(e.card.why || "")) return true;
  // Another build of the same release (the +hash tail differs): neither is newer, but the
  // shelf's bytes are not the dial's, and between releases that is how a rebuild reaches it.
  const have = e.installed ? e.installed.version : e.card && e.card.version;
  if (sameRelease(have, e.lib.version)) return true;
  if (e.card && e.card.version && verCompare(e.card.version, e.lib.version) >= 0) return false;
  if (e.installed && e.installed.version && verCompare(e.installed.version, e.lib.version) >= 0) return false;
  return true;
}

function appStateSentence(e) {
  const a = e.installed, c = e.card, f = e.lib;
  let t = (e.title || e.slug) + " \u00b7 " + e.slug;
  if (a) {
    if (a.compiled) t += a.failed ? " \u00b7 built in, quarantined at boot" : " \u00b7 built into the firmware";
    else if (a.failed) t += " \u00b7 failed: " + (a.reason || a.status || "the dial did not say why");
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
  if (updateGate(e)) {
    // an installed row's own line already names another build (libUpdateSuffix)
    if (!sameRelease(a ? a.version : c && c.version, f.version)) t += " \u00b7 update available (" + f.version + ")";
    else if (!a) t += " \u00b7 the hub library has another build (" + f.version + ")";
  }
  return t;
}

function appRow(e, opt) {
  const a = e.installed, c = e.card, f = e.lib;
  opt = opt || {};
  // The row is the face's table (owner, 2026-11-04): the name over its chips, then the
  // presses in fixed columns, the same verb in the same column on every row. An update is
  // a filled chip and a filled button, not a clause inside a sentence; the whole sentence
  // stays on hover.
  const row = document.createElement("div");
  row.className = "arow";
  row.dataset.slug = e.slug;
  // §59: the grip that orders the carousel, and while rotation is on, whether it stops here.
  // A row off the dial keeps both cells empty so the columns still line up.
  const grip = document.createElement(opt.sort ? "button" : "span");
  grip.className = "grip" + (opt.sort ? "" : " none");
  grip.textContent = "\u283f";
  if (opt.sort) {
    row.dataset.sort = "1";
    grip.type = "button";
    grip.title = "Drag to move " + (e.title || e.slug) + " in the carousel (or focus it and use the arrow keys)";
    grip.setAttribute("aria-label", "Move " + (e.title || e.slug));
  }
  const rc = document.createElement("span");
  rc.className = "rotcell";
  if (opt.rotOn && e.car) {
    const cb = document.createElement("input");
    cb.type = "checkbox"; cb.checked = !!e.car.rotate;
    cb.title = "Rotation stops on " + (e.title || e.slug);
    cb.setAttribute("aria-label", cb.title);
    cb.onchange = () => {
      const inn = carApps().filter((x) => (x.slug === e.slug ? cb.checked : x.rotate)).map((x) => x.slug);
      carouselPost({ apps: inn.join(",") }, (e.title || e.slug) + (cb.checked ? " is in the rotation" : " is out of the rotation"));
    };
    rc.appendChild(cb);
  }
  row.append(grip, rc);
  const info = document.createElement("div");
  info.className = "tinfo";
  info.title = appStateSentence(e);
  const name = document.createElement("b");
  name.textContent = e.title || e.slug;
  const slug = document.createElement("span");
  slug.className = "hint";
  slug.textContent = " " + e.slug;
  const chips = document.createElement("div");
  chips.className = "ttags";
  const chip = (text, cls, tip) => {
    const t = document.createElement("span");
    t.className = "tag" + (cls ? " " + cls : "");
    t.textContent = text;
    if (tip) t.title = tip;
    chips.appendChild(t);
  };
  if (a) {
    if (a.compiled) chip(a.failed ? "built in \u00b7 quarantined at boot" : "built in", a.failed ? "err" : "mut");
    else if (a.failed) chip("failed: " + (a.reason || a.status || "the dial did not say why"), "err");
    else chip("installed " + (a.version || "(unstamped)"), "on");
  }
  // the amber "for older firmware" chip below already says it when the card copy is the only
  // thing behind and the dial runs the library's own release
  const staleOnly = !!(c && !c.ok && /^app format /.test(c.why || "") && f && a && a.version === f.version);
  if (updateGate(e) && !staleOnly) {
    const have = a ? a.version : c && c.version;
    chip(sameRelease(have, f.version) ? "another build in the hub library" : "update available \u2192 " + f.version, "upd",
      "The hub library holds " + f.version + "; Update puts it on this orb");
  } else if (a && !a.compiled && f && verCompare(a.version, f.version) > 0) chip("ahead of the hub library", "mut");
  // A card copy refused only for its FORMAT is an older release the firmware moved past,
  // not damage: amber, and Update (the library's bytes over it) is the fix (owner, 2026-11-08).
  const stale = !!(c && !c.ok && /^app format /.test(c.why || ""));
  if (c) chip(c.ok ? "on the card" + (c.version && !a ? " " + c.version : "")
    : stale ? "for older firmware \u00b7 " + (f ? "Update replaces it" : "the hub library has no newer copy")
    : "the card refuses it: " + (c.why || "no reason given"), c.ok ? "mut" : stale ? "upd" : "err", c.ok ? "" : c.why);
  if (f) chip("in the hub library" + (a || c ? "" : " " + (f.version || "")), "mut");
  if (a && !a.compiled && a.caps) chip("uses " + a.caps, "mut");
  // the app's own floor (app.json os_min): an older OS loads it as a failed row, so say so
  const tooOld = !!(f && f.os_min && fwBelow(f.os_min));
  if (tooOld) chip("needs firmware " + f.os_min + " \u2014 this orb runs " + (fwStamp.split("+")[0] || "older"), "err",
    "Update this orb's firmware on the Firmware tab first; the hub refuses the install until then");
  const txt = document.createElement("div");
  txt.className = "hint";
  const line = document.createElement("div");
  line.append(name, slug);
  info.append(line, chips, txt);
  row.appendChild(info);
  const acts = document.createElement("div");
  acts.className = "acts";
  const slots = [null, null, null, null];
  let extra = null;
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
  // per fact, never two buttons for one question. Columns: [Install | Update |
  // Clear fault] [Uninstall] [Add to card | Remove from card] [Remove from hub library].
  if (!a && (f || (c && c.ok)) && !updateGate(e))
    slots[0] = installButton(e, txt, "Install", false);
  if (updateGate(e)) {
    slots[0] = installButton(e, txt, "Update", true);
    slots[0].className = "upd";
  }
  if (tooOld && slots[0]) slots[0].disabled = true;
  if (a && !a.compiled)
    slots[1] = uninstallButton(e, txt);
  if (a && a.failed) {
    const cf = clearFaultButton(e, txt);
    if (!slots[0]) slots[0] = cf; else extra = cf;
  }
  if (f && !c)
    slots[2] = addToCardButton(e, txt);
  if (c)
    slots[2] = eraseCardButton(e, txt);
  if (f)
    slots[3] = removeFromLibraryButton(f, txt);
  for (const b of slots) {
    if (b) { acts.appendChild(b); continue; }
    const sp = document.createElement("span");
    sp.className = "slot";
    acts.appendChild(sp);
  }
  if (extra) acts.appendChild(extra);
  row.appendChild(acts);
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
      // The run's own reboots routinely outlive this re-read: the board is usually
      // mid-boot, not broken. Say so, wait the tail of the boot out, and try the
      // read once more before printing a failure over an installed app (bench,
      // 2026-10-29 PM: "would not re-read" over a board that answered a minute later).
      const st = $("#mn-appstate");
      if (st) st.textContent = libVerdict.text + " \u2014 the board is not answering the ledger re-read; trying once more as it comes back";
      setTimeout(async () => {
        try { await loadApps(); }
        catch (e2) {
          if (st) st.textContent = libVerdict.text + " \u2014 but the ledger would not re-read: " + e2.message;
        }
      }, 20000);
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
      cardChanged();       // the Themes face drew the same card this press just changed
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
  syncRestoreButton();
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
// The +build tail is the container's content hash and never orders (semver; orbnet's
// AppVersionCompare): one build is equal to itself, and two builds of one release answer
// null — not equal, not ordered — which sameRelease names.
function verCompare(a, b) {
  const parse = (s) => {
    const m = /^v(\d+)\.(\d+)\.(\d+)(?:-([^+]*))?(?:\+(.*))?$/.exec(s || "");
    if (!m) return null;
    return { n: (+m[1]) * 10000 + (+m[2]) * 100 + (+m[3]), tail: m[4] || "", build: m[5] || "" };
  };
  const pa = parse(a), pb = parse(b);
  if (!pa || !pb) return null;
  if (pa.n !== pb.n) return pa.n - pb.n;
  if (pa.tail === pb.tail) return pa.build === pb.build ? 0 : null;
  if (pa.tail === "") return 1;
  if (pb.tail === "") return -1;
  return pa.tail < pb.tail ? -1 : 1;
}

// sameRelease: two stamps of one release in different builds (the +hash differs).
function sameRelease(a, b) {
  if (!a || !b || a === b || verCompare(a, a) !== 0 || verCompare(b, b) !== 0) return false;
  return a.split("+")[0] === b.split("+")[0];
}

// libUpdateSuffix is the shelf's verdict on a dial row's version line. Note the
// ordering: it is the LIBRARY that is ahead. A dial running newer than the shelf is
// not out of date, and the face does not say it is — it names the dial as ahead of the
// shelf, because that is the operator's own problem to notice and the sentence that
// lets them act on it.
function libUpdateSuffix(a) {
  const f = libEntry(a.slug);
  if (!f) return "";
  if (sameRelease(a.version, f.version)) return " \u00b7 the hub library has another build (" + f.version + ")";
  const cmp = verCompare(a.version, f.version);
  if (cmp === null) return "";
  if (cmp < 0) return " \u00b7 update available (" + f.version + ")";
  if (cmp > 0) return " \u00b7 ahead of the hub library";
  return "";
}

// The Restore press (seed.go): the apps compiled into this hub, filed back into its library
// where one is missing or only an older copy is there. /api/library names them, so the button
// is live exactly when there is something to put back and says what that is. An app the
// library holds at the same or a newer version is the owner's and is never offered.
function syncRestoreButton() {
  const btn = $("#lib-restore");
  if (!btn) return;
  const want = (libCache && libCache.restorable) || [];
  btn.disabled = want.length === 0;
  btn.title = want.length
    ? "Put back into the hub library: " + want.map(restoreLabel).join(", ")
      + ". Nothing on an orb or a card is touched."
    : "Every app that ships with this hub is in its library.";
}

function restoreLabel(r) {
  return (r.title || r.slug) + " " + r.version + (r.have ? " (the library has " + r.have + ")" : "");
}

function wireLibraryRestore() {
  const btn = $("#lib-restore"), state = $("#lib-import-state");
  if (!btn || !state) return;
  btn.onclick = async () => {
    const want = (libCache && libCache.restorable) || [];
    if (!want.length) return;
    if (!await askConfirm({
      title: "Restore the bundled apps?",
      body: want.map(restoreLabel).join(", ") + (want.length === 1 ? " goes" : " go")
        + " back into the hub library.",
      detail: "Only missing or older copies are replaced. No orb and no SD card is touched: "
            + "use a row's Install to put an app on a board.",
      yes: "Restore",
      safe: true,
    })) return;
    btn.disabled = true;
    state.textContent = "restoring\u2026";
    let d = null, r;
    try {
      r = await hubFetch("/api/library/restore", { method: "POST" });
      d = await r.json().catch(() => null);
    } catch (e) {
      state.textContent = "the hub did not answer: " + (e && e.message ? e.message : e);
      syncRestoreButton();
      return;
    }
    if (!d || !d.ok) {
      state.textContent = "\u2717 the hub refused: " + ((d && d.error) || ("HTTP " + (r && r.status)));
      syncRestoreButton();
      return;
    }
    const got = (d.data && d.data.restored) || [];
    const named = got.map((slug) => {
      const w = want.find((x) => x.slug === slug);
      return w ? (w.title || slug) + " " + w.version : slug;
    });
    state.textContent = got.length
      ? "\u2713 restored " + named.join(", ") + " to the hub library"
      : "nothing needed restoring";
    await renderLibrary();
    if (appList) await loadApps();
  };
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
    const bad = pickedAs(f, [".orbapp"], "an app");
    if (bad) { state.textContent = bad; file.value = ""; return; }
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
    cardChanged();   // the listing below proves it gone; Apps and Themes owe themselves the same fresh read
    await fmList(fmPath);
  } catch (e2) {
    // A dial that died mid-request during the run's own reboots is not a refusal:
    // the erase may well have landed while the answer was in flight. Wait out the
    // tail of the boot, re-read the listing, and let it report the verdict (bench,
    // 2026-10-29 PM: "i/o timeout" printed over an apps/ directory that was gone).
    if (/unreachable|timeout|network/i.test(e2.message)) {
      st.textContent = "the orb went silent mid-erase — re-reading the card as it comes back";
      cardChanged();   // the erase may well have landed; both card faces read the truth on the next press
      setTimeout(async () => {
        try {
          await fmList(fmPath);
          st.textContent = "the orb is answering again — the listing above is the verdict on /" + rel;
        } catch (e3) {
          st.textContent = "the orb is still not answering: " + e3.message;
        }
      }, 15000);
    } else {
      st.textContent = e2.message;
    }
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
  cardChanged();   // whatever landed, the Apps and Themes faces last read a card without it
  await fmList(fmPath);
});

// ---- the Firmware face's reset button, LAN arm included ------------------------
// The reset modal's opener, gate and press, all of them: app.js keeps only the
// dismissals. RESET ALL over a lead is the cable arm. A v1.1.18 board answers
// the same wipe at /api/v1/factory, so the dialog has a second reach — and a
// typed ALL, because this is the one LAN verb that can make the orb this page
// is open disappear into its own provisioning AP. The firmware erase stays
// cable-only: lonely is a whole reflash decision no network door should hold.
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
        // The progress panel closes with the modal, as every other flash door's does on
        // success; a refused erase never reaches here, so its reason stays on screen.
        fpClose();
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
wireLibraryRestore();
showTab("orb");






