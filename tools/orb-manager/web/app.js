// Orb Manager front-end. Talks only to same-origin endpoints (/api/*, /orb/*).
// The server is the only thing that reaches an orb; CORS never enters into it.
//
// The page is three views over one truth. Manage is the landing and the whole
// first screen: one card per board the hub has met as an orb, Forget only where
// the hub's own verdict says the board cannot come straight back, and nothing
// at all for a cable with no orb on it — a blank ESP32-S3 is a New Orb
// candidate, not an orb card, and it never reaches this list. Onboard is the
// single decision a blank chip gets: a name, and the firmware this app already
// carries. Orb is one board's manage state: firmware, name, Wi-Fi, time zone.
// A newly flashed board lands there the moment its firmware boots, which is the
// one place the two flash doors converge.
"use strict";

const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];

// ORB_TOKEN arrives inlined into index.html by the hub at serve time, one value per
// launch. The hub demands it on every request that changes anything, alongside the
// HttpOnly cookie the browser attaches by itself: the cookie stops a cross-site form
// POST (SameSite=Strict withholds it), and this header is the half a scripted agent
// can supply and a cross-site page cannot forge. A page that never loaded from this
// hub has neither.
const ORB_TOKEN = (window.__ORB_TOKEN__ === "__ORB_TOKEN__" || !window.__ORB_TOKEN__)
  ? "" : String(window.__ORB_TOKEN__);

// hubFetch is fetch() with the launch token attached, for every call that can alter
// something — a flash, a provision, a device route. GETs of /api/ports and the like
// use plain fetch, so a read never carries the credential it does not need.
async function hubFetch(url, opts = {}) {
  const h = Object.assign({}, opts.headers || {});
  if (ORB_TOKEN) h["X-Orb-Token"] = ORB_TOKEN;
  return fetch(url, Object.assign({}, opts, { headers: h }));
}

async function getJSON(url) {
  const r = await fetch(url);
  const j = await r.json();
  if (!j.ok) throw new Error(j.error || ("GET " + url + " failed"));
  return j.data;
}

// readDeviceJSON parses a /orb/<ip>/api/v1/... reply as the orb device API's
// JSON. It never lets a raw JSON.parse failure escape as a cryptic "is not
// valid JSON": a hub proxy error arrives as {"error":...} and is raised with
// that message, and any non-JSON body (a captive portal, an HTML 502 page) is
// reported as its own text instead.
async function readDeviceJSON(r) {
  const text = await r.text();
  let d = null;
  try { d = JSON.parse(text); } catch (e) { d = null; }
  if (d === null || typeof d !== "object") {
    throw new Error((text.trim() || ("HTTP " + r.status)));
  }
  if (!r.ok) throw new Error(d.error || ("HTTP " + r.status));
  return d;
}

// esc keeps a server or device string from closing the markup it is interpolated
// into. Rows built with createElement/textContent do not need it and must not
// reach for innerHTML at all; this is for the panel templates that remain.
function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"]/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

// ---- views ---------------------------------------------------------------
// The fleet is the landing. #onboard is reached only through the New Orb button,
// #orb only by opening one of the fleet's cards (or by the New Orb flow
// arriving at its manage state on its own). A pasted hash with no board loaded
// returns to the fleet rather than guessing at which board the visitor meant.
function show(view) {
  if (view && !document.getElementById(view)) return false;
  $$(".view").forEach((v) => (v.hidden = v.id !== view));
  if (view) location.hash = view;
  return true;
}

document.addEventListener("click", (e) => {
  const card = e.target.closest("[data-view]");
  if (card) {
    // Intercept only a control naming a real in-page view. The Theme Editor link
    // is a separate page: cancelling its navigation is the bug this guard exists
    // for, so an unknown view name lets the click through untouched.
    if (show(card.dataset.view)) e.preventDefault();
    return;
  }
  if (e.target.closest("[data-back]")) { show("manage"); location.hash = ""; }
});
window.addEventListener("hashchange", route);
function route() {
  const v = location.hash.replace("#", "");
  if (v === "onboard") { openOnboard(); return; }
  if (v === "orb" && current.mac) { show("orb"); return; }
  show("manage");
}

// ---- meta ------------------------------------------------------------------
(async () => {
  try {
    const m = await getJSON("/api/status");
    $("#meta").textContent = `${m.platform} · firmware ${m.firmware || "none"}`;
  } catch (e) { $("#meta").textContent = e.message; }
})();

// ---- ports, and the blank gate ---------------------------------------------
// The New Orb button is the only place a cable with no orb on it appears, and it
// opens on two verdicts the hub answers without touching the chip: "espressif"
// says a genuine Espressif device sits behind this tty, and "orbSeen" says the
// hub has evidence this MAC has been an orb — a name, an address, a stamp, an
// announcement, a chip that answered. A port that is Espressif and has no orb
// evidence is a blank candidate, and it is a candidate only here, never on the
// fleet list. The fleet's own filter (isBlankRow) reads the row twin of this
// same verdict, which is the point: the two floors are complements, so a board
// is always visible through exactly one door. Reading row existence instead —
// the inFleet field this gate first used — closed the door on any board ever
// plugged in, because a cable sighting alone writes a row, and a wiped orb then
// appeared nowhere on the page at all.
let blanks = [];   // port entries a New Orb flash may target
let allPorts = []; // the last /api/ports payload, for labels and cached verdicts

function isBlankPort(e) {
  if (!e || typeof e !== "object") return false;
  if (!e.espressif) return false;
  if (e.orbSeen === true) return false;
  if (e.probed && e.isOrb) return false;
  return true;
}

async function refreshPorts() {
  try {
    const { ports } = await getJSON("/api/ports");
    allPorts = Array.isArray(ports) ? ports : [];
  } catch (e) { allPorts = []; }
  blanks = allPorts.filter(isBlankPort);
  const sel = $("#ob-port");
  sel.innerHTML = "";
  for (const entry of blanks) {
    const o = document.createElement("option");
    o.value = entry.port;
    o.textContent = portLabel(entry.port, entry);
    sel.appendChild(o);
  }
  renderBlankGate();
  renderIdentity(allPorts);
  fillConsolePorts();
}

// renderBlankGate is the button and the three honest states of the New Orb
// card: one blank (chosen for you, named), several (pick a cable), none
// (nothing to flash; plug one in). The button hides rather than lies, so a
// page opened on a bench with no spare chip shows exactly the fleet and
// nothing else.
function renderBlankGate() {
  $("#mn-add").hidden = blanks.length === 0;
  $("#ob-blankpick").hidden = blanks.length < 2;
  $("#ob-blankone").hidden = blanks.length !== 1;
  $("#ob-blanknone").hidden = blanks.length !== 0;
  $("#ob-go").disabled = blanks.length === 0;
  if (blanks.length === 1) {
    $("#ob-blankone").textContent = blanks[0].port +
      " is blank — an ESP32-S3 on a cable with no orb on it. Give it a name and flash it.";
  }
}

function openOnboard() {
  renderBlankGate();
  show("onboard");
}
$("#mn-add").onclick = (e) => { e.preventDefault(); openOnboard(); };

// portLabel spells a port out. The MAC is free — the host reads it off the USB
// descriptor without opening the port — so two cables on the bench are tellable
// apart before anyone pays for a probe. The version only appears once one has
// been identified, and is never guessed at.
function portLabel(p, entry) {
  if (typeof entry === "string" || !entry) { return p; }
  if (entry.probed) {
    return entry.isOrb
      ? p + "  —  " + (entry.mac || entry.usbMac || "orb") + "  ·  " + (entry.version || "version unknown")
      : p + "  —  " + (entry.reason || "not an orb");
  }
  if (entry.usbMac) { return p + "  —  " + entry.usbMac + "  ·  not identified"; }
  if (entry.hint) { return p + "  —  " + entry.hint + "  ·  not identified"; }
  return p;
}

// renderIdentity shows the cached verdict for the cable the manage view is
// looking at. It never probes: the whole point of the cache is that
// identifying an orb costs a reboot and the operator decides when to pay it.
function renderIdentity(list) {
  const card = $("#ob-idcard"), state = $("#ob-idstate");
  const chosen = (list || allPorts).find((e) => e && e.port === (current.port || ""));
  if (!chosen || !chosen.probed) { card.hidden = true; return; }
  card.hidden = false;
  const rows = [];
  if (chosen.isOrb) {
    rows.push("<b>Orb " + esc(chosen.mac || "MAC unknown") + "</b>");
    rows.push("running " + esc(chosen.project || "gorb-os") + " " + esc(chosen.version || "?") +
      (chosen.compileTime ? ' <span class="hint">built ' + esc(chosen.compileTime) + "</span>" : ""));
    if (chosen.ip) rows.push('on the LAN at <code>' + esc(chosen.ip) + "</code>");
    rows.push('<span class="verdict v-' + esc(chosen.verdict || "unknown") + '">' + verdictText(chosen.verdict) + "</span>");
  } else {
    rows.push("<b>Not an orb</b> — " + esc(chosen.reason || "could not identify"));
  }
  card.innerHTML = rows.join("<br>");
  state.textContent = chosen.probedAt ? "identified " + esc(chosen.probedAt) : "";
}

// verdictText words the comparison for someone who did not build either image.
// An unknown verdict is said plainly: guessing "newer" here would push a user to
// replace a working orb on no evidence.
function verdictText(v) {
  switch (v) {
    case "upgrade": return "The firmware ready to flash is NEWER than this orb's — safe to load.";
    case "downgrade": return "The firmware ready to flash is OLDER than this orb's — loading it is a downgrade.";
    case "same": return "This orb already runs the firmware ready to flash.";
    case "same-version": return "Same version, different build — loading it replaces nothing meaningful.";
    default: return "Cannot tell which is newer: one of the builds has no usable timestamp.";
  }
}

// ---- the firmware this hub carries ------------------------------------------
// One line on every card that could flash: what the bundle is, where it came
// from, and the door to replace it with a file. The New Orb flow and the cable
// Update button both flash what this panel describes, so it belongs to the hub,
// not to either button.
async function refreshBuild() {
  const el = $("#ob-build");
  try {
    const b = await getJSON("/api/build");
    if (!b.present) { el.textContent = "no firmware is loaded — restart orb-manager"; return; }
    // Same discipline as the port picker: .length and .map on a value the server
    // could hand back as null or undefined throws, and the catch below then
    // blames the browser for what is a payload-shape bug.
    const imgs = Array.isArray(b.images) ? b.images : [];
    // project and version are bytes out of the image, read from an esp_app_desc_t
    // in a file the operator picked off disk. They are not trusted text: esc them
    // like every other server string, and say so at the source of the value so the
    // next field added here is not added raw.
    const who = b.version ? esc((b.project || "gorb-os") + " " + b.version) : esc(b.source || "unknown firmware");
    if (!imgs.length) { el.textContent = "firmware " + who + " lists no images"; return; }
    el.innerHTML = "<b>Firmware " + who + "</b> — " + (b.source === "built-in" ? "built into this app" : "your file: " + esc(b.source)) + "<br>" +
      imgs.map((i) => esc(i.offset) + "  " + esc(i.name) + "  (" + (Number(i.bytes) || 0) + " B)").join("<br>") +
      (b.sha256 ? '<br><span class="hint">sha256 ' + esc(String(b.sha256).slice(0, 16)) + "… · " + imgs.length + " image(s)</span>" : "");
    const reset = $("#ob-fwreset"), pick = $("#ob-fwpick");
    const custom = b.source && b.source !== "built-in";
    reset.hidden = !custom;
    pick.textContent = custom ? "Pick a different firmware…" : "Use my own firmware…";
  } catch (e) { el.textContent = e.message; }
}

// pickFirmware uploads the chosen bundle. The hub validates it and only then makes
// it active, so a wrong pick leaves the built-in firmware in place rather than
// leaving the Update buttons pointing at a file that was never accepted.
async function pickFirmware(file) {
  const state = $("#ob-fwstate");
  state.textContent = "checking " + file.name + "…";
  const form = new FormData();
  form.append("firmware", file, file.name);
  try {
    const res = await hubFetch("/api/firmware", { method: "POST", body: form });
    const j = await res.json().catch(() => ({}));
    if (!res.ok || j.ok === false) {
      state.textContent = (j.error || ("HTTP " + res.status)) + " — still flashing the built-in firmware";
      return;
    }
    state.textContent = "this hub now carries " + file.name;
    await refreshBuild();
  } catch (e) {
    state.textContent = e.message + " — still flashing the built-in firmware";
  }
}

$("#ob-fwpick").onclick = () => $("#ob-fwfile").click();
$("#ob-fwfile").onchange = async (e) => {
  const f = e.target.files && e.target.files[0];
  if (f) await pickFirmware(f);
  e.target.value = ""; // let the same file be picked again after a reset
};
$("#ob-fwreset").onclick = async () => {
  const state = $("#ob-fwstate");
  state.textContent = "restoring the built-in firmware…";
  try {
    const res = await hubFetch("/api/firmware/reset", { method: "POST" });
    const j = await res.json().catch(() => ({}));
    state.textContent = (res.ok && j.ok) ? "using the firmware built into this app" : (j.error || "HTTP " + res.status);
  } catch (e) { state.textContent = e.message; }
  await refreshBuild();
};

// ---- the streaming core ------------------------------------------------------
// streamPost POSTs payload to url and feeds each newline-delimited JSON event
// to onEvent. Its whole reason for existing is to keep a transport failure from
// surfacing as a bare "TypeError: Failed to fetch": fetch() rejecting (the
// manager was restarted, the page moved mid-request, the port is gone) and a
// non-2xx reply (the hub's http.Error paths answer plain text, not NDJSON) are
// BOTH turned into an {error:...} event the caller already knows how to render.
// Returns true only when the stream opened and drained without a transport
// fault; the caller uses that to decide whether a "done"/result was reached.
async function streamPost(url, payload, onEvent) {
  let r;
  try {
    r = await hubFetch(url, {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
  } catch (e) {
    onEvent({ error: "cannot reach the orb-manager (" + e.message + ") — it may have restarted. Reload this page and retry." });
    return false;
  }
  if (!r.ok || !r.body) {
    const t = await r.text().catch(() => "");
    onEvent({ error: "orb-manager answered HTTP " + r.status + (t.trim() ? ": " + t.trim() : " — retry, or reload if it persists") });
    return false;
  }
  const reader = r.body.getReader();
  const dec = new TextDecoder();
  let buf = "";
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let nl;
    while ((nl = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, nl).trim(); buf = buf.slice(nl + 1);
      if (!line) continue;
      let ev;
      try { ev = JSON.parse(line); } catch (e) { onEvent({ error: "malformed reply: " + line }); continue; }
      onEvent(ev);
    }
  }
  return true;
}



// streamFlash drives one POST /api/flash or /api/wipe and renders its NDJSON
// events into the shared flash panel: the bar is the story, the log is the
// detail, and the same Cancel door reaches both. It returns {mac, ok}: mac is
// the eFuse MAC the ROM reported, ok is whether the run reached its "done"
// event — the caller only runs onboarding after a real success, so a refused or
// mid-flash error never dumps the user into a Wi-Fi form for an orb that was
// not actually rewritten.
async function streamFlash(url, payload, log, bar) {
  log.textContent = ""; bar.style.width = "0%";
  let mac = null;
  let ok = false;
  const opEl = $("#ob-op"), cancelBtn = $("#ob-cancel");
  const port = payload.port || "";
  const wiping = url === "/api/wipe";
  $("#ob-doing").textContent = wiping ? "erasing " + port : "flashing " + port;
  cancelBtn.hidden = false;
  cancelBtn.disabled = false;
  cancelBtn.onclick = async () => {
    cancelBtn.disabled = true;
    opEl.textContent = wiping
      ? "cancelling the wipe on " + port +
        " — the erase stops at the next command boundary; the chip is left partly erased and will not boot until it is flashed"
      : "cancelling the flash on " + port +
        " — it stops at the next block boundary, and the orb stays in its bootloader";
    try { await hubFetch("/api/flash/cancel", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ port }) }); }
    catch (e) { /* the stream's own log line says what happened */ }
  };
  const tick = setInterval(() => showLiveOp(port, opEl), 2000);
  showLiveOp(port, opEl);
  try {
    await streamPost(url, payload, (ev) => {
      if (ev.log) log.textContent += ev.log + "\n";
      else if (ev.error) log.textContent += "\n\u2717 " + ev.error + "\n";
      else if (ev.done) { log.textContent += "\n\u2713 " + ev.done + "\n"; ok = true; }
      else if (ev.mac) mac = ev.mac;
      else if (ev.progress) {
        const p = JSON.parse(ev.progress);
        const pct = p.total ? Math.round((p.written / p.total) * 100) : 0;
        bar.style.width = pct + "%";
        opEl.textContent = (p.image + 1) + "/" + p.of + " \u00b7 " + pct + "%";
        log.textContent += `\r[${p.image + 1}/${p.of}] ${p.offset} ${p.written}/${p.total} (${pct}%)`;
      }
      log.scrollTop = log.scrollHeight;
    });
  } finally {
    clearInterval(tick);
    cancelBtn.hidden = true;
  }
  return { mac, ok };
}

// showLiveOp reports what the hub says is running on this port, so the panel
// states the truth about the cable rather than the client's guess about its own
// request — and a flash started in a second tab or by orbflash on the bench
// shows up too.
async function showLiveOp(port, el) {
  try {
    const { ops } = await getJSON("/api/ops");
    const mine = (ops || []).filter((o) => !port || o.port === port || o.asked === port);
    el.textContent = mine.length
      ? mine.map((o) => o.kind + " running on " + o.port + " (" + o.seconds + "s)").join("; ")
      : "";
  } catch (e) { /* the flash bar is the primary voice here */ }
}

// probePort asks the hub to identify the cable the manage view stands on. It
// reboots the orb, so it only ever happens on this click, and the button is
// disabled while one runs so a double-click cannot queue a second reset.
async function probePort() {
  const port = current.port;
  const state = $("#ob-idstate"), btn = $("#ob-probe"), card = $("#ob-idcard");
  if (!port) { state.textContent = "this orb is not on a cable right now"; return; }
  btn.disabled = true;
  card.hidden = true;
  state.textContent = "restarting the orb to read what it runs — about 10 seconds";
  try {
    const res = await hubFetch("/api/ports/probe", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ port }),
    });
    const j = await res.json().catch(() => ({}));
    if (!res.ok || j.ok === false) {
      state.textContent = (j.error || ("HTTP " + res.status)) + " — nothing was changed on the orb";
    } else {
      state.textContent = j.data.isOrb ? "identified" : (j.data.reason || "not an orb");
      await refreshPorts();
    }
  } catch (e) {
    state.textContent = e.message + " — nothing was changed on the orb";
  } finally { btn.disabled = false; }
}

$("#ob-probe").onclick = probePort;
$("#ob-refresh").onclick = async () => { await refreshPorts(); renderBlankGate(); };


// ---- the orb under management ----------------------------------------------
// One board at a time lives in view: its row from the pushed card, the cable it
// is on if any, the address the hub dials if it has one. Everything the manage
// view writes is keyed on the MAC — the address is a lease, and a name write
// that trusted one would rename whoever holds it tomorrow.
let current = { mac: "", ip: "", port: "", name: "", row: null };

// isBlankRow is the fleet's doorkeeper, and it is the exact complement of the
// New Orb gate: the hub's own `orb` verdict decides, so a board the hub cannot
// yet call an orb is never a card here and is always a flash candidate over on
// the cable side. Reading the verdict rather than re-deriving it from fields is
// what keeps the two doors from disagreeing the way name/ip/fw/state-versus-
// inFleet once did — that pair left a wiped board on a lead invisible in both
// directions, which is how "plug in two orbs, see one" happened on this bench.
function isBlankRow(o) {
  if (!o || typeof o !== "object") return true;
  return o.orb !== true;
}

// openOrbByIP is the one door into the manage view. The ip may be empty — a
// board on a cable with no address is still openable, and its card is exactly
// where the cable-only truth earns its buttons.
function openOrbByIP(ip, mac, row) {
  current = {
    mac: mac || "", ip: ip || "", name: (row && row.name) || "", row: row || null,
    port: (row && Array.isArray(row.ports) && row.ports[0]) || "",
  };
  orbIP = current.ip;                       // the scan/join doors dial this, one source
  $("#mn-orbname").textContent = current.name || (row && row.instance) || current.mac || current.ip || "orb";
  $("#mn-orbident").textContent = [current.mac && ("mac " + current.mac), current.ip && ("at " + current.ip),
    current.port && ("on cable " + current.port)].filter(Boolean).join("  \u00b7  ");
  $("#mn-name").value = current.name;
  $("#mn-cablerow").hidden = !current.port;
  $("#ob-identify").hidden = !current.port;
  $("#ob-chipwiperow").hidden = !current.port;
  $("#mn-wificard").hidden = !current.ip;
  $("#mn-curfw").textContent = "loading\u2026";
  $("#mn-verdict").textContent = "";
  $("#ob-idcard").hidden = true;
  renderIdentity(allPorts);
  show("orb");
  loadOrb();
  if (current.ip) loadNetworks(false);
}

// loadOrb asks the board who it is. The reply is the manage view's headline and
// the input to the version gate; a board that answers nothing is said so on the
// firmware card, and the cable doors stay lit, because a cable answers to
// nothing on a DHCP lease.
async function loadOrb() {
  if (!current.ip) {
    $("#mn-curfw").textContent = "not on the LAN: the doors that work over the cable stay lit";
    return;
  }
  try {
    await hubFetch("/api/orb/allow", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ ip: current.ip }) });
  } catch (e) { /* the status read below reports the real failure */ }
  try {
    const st = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/status"));
    current.name = st.name || current.name;
    $("#mn-curfw").textContent = "running " + (st.fw || "version unknown") +
      " \u00b7 state " + (st.state || "?") + (st.rssi ? " \u00b7 rssi " + st.rssi : "") + (st.tz ? " \u00b7 " + st.tz : "");
    $("#mn-tz").value = st.tz || "";
    $("#j-tz").value = st.tz || "";
    if (st.name) $("#mn-name").value = st.name;
    setUpdateGate(st.fw);
    loadThemes(current.ip);
    startPolling(current.ip);
  } catch (e) {
    $("#mn-curfw").textContent = "no orb answering at " + current.ip + " \u2014 " + e.message;
    setUpdateGate("");
  }
}


// setUpdateGate is the amber alert's other half: the OTA button lights only when
// the hub's compare says upgrade — the row flag from the pushed card, or the
// fresh status read, which outranks a stale flag the moment a board updates out
// from under the page.
let buildVersion = "";
function setUpdateGate(statusFw) {
  const rowUp = !!(current.row && current.row.update === true);
  const can = (rowUp || !!statusFw) && !!current.ip;
  $("#mn-update").disabled = !can;
  $("#mn-updatenote").textContent = can
    ? "this hub carries " + (buildVersion || "newer firmware") + " — the orb stages it, verifies it, and reboots once"
    : (statusFw ? "this orb runs the firmware this hub carries: nothing to load"
                : "no address to update over — the cable doors work without one");
}

// The OTA door. One POST does the whole staging: the hub reads its own bundle,
// the device validates the image in the spare slot, and the reboot is the hub's
// second request inside the same handler, so nobody is left holding a staged
// slot they forgot about. The reply names both stamps.
$("#mn-update").onclick = async () => {
  if (!current.ip) return;
  const btn = $("#mn-update"), note = $("#mn-updatenote");
  btn.disabled = true;
  note.textContent = "uploading to " + current.ip + " — the orb writes the spare slot with its cache off; this takes a minute";
  try {
    const r = await hubFetch("/api/orb/update", {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ ip: current.ip }),
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
    const d = j.data || {};
    note.textContent = "staged and rebooting: " + d.from + " → " + d.to + " — the card will show the new stamp in a moment";
    $("#mn-curfw").textContent = "off the wire for one boot; the next check-in carries the new stamp";
    pullFound();
  } catch (e) {
    note.textContent = "update refused: " + e.message;
    btn.disabled = false;
  }
};

// The cable door for an orb that has been an orb for a while: the firmware this
// hub carries, full manifest, NVS preserved unless the factory-reset box says
// otherwise. It reuses the flash machinery because below the button it is the
// same ROM conversation — the difference is only what it starts after.
$("#mn-cable").onclick = async () => {
  if (!current.port) return;
  const go = $("#mn-cable");
  const log = $("#ob-log"), bar = $("#ob-bar > div");
  go.disabled = true;
  try {
    const { mac, ok } = await streamFlash("/api/flash",
      { port: current.port, mode: "full", wipe: $("#ob-wipe").checked }, log, bar);
    if (ok) {
      await refreshPorts();
      if (!current.mac && mac) current.mac = mac;
      startJoin(current.port, mac || current.mac);
    }
  } finally { go.disabled = false; }
};

// ---- New Orb: the one flash a blank board gets -------------------------------
// A fresh flash is the one flash with nothing to preserve: full manifest, NVS
// cleared, exactly as a board leaves the factory. There is no second choice
// here — the firmware is the one this app carries, and the firmware card's file
// door lets anyone replace what "built in" means. The name is typed here and
// written the moment the board has an address to write it to.
let pendingName = "";

$("#ob-go").onclick = async () => {
  const port = blanks.length === 1 ? blanks[0].port : $("#ob-port").value;
  if (!port || !blanks.some((b) => b.port === port)) {
    $("#ob-blankstate").textContent = "no blank board on that cable — the list follows the physical ports";
    return;
  }
  pendingName = ($("#ob-name").value || "").trim();
  const go = $("#ob-go"), log = $("#ob-log"), bar = $("#ob-bar > div");
  go.disabled = true;
  let mac = null, ok = false;
  try {
    const r = await streamFlash("/api/flash", { port, mode: "full", wipe: true }, log, bar);
    mac = r.mac; ok = r.ok;
    if (ok) startJoin(port, mac);
    else $("#ob-blankstate").textContent = "the flash stopped before its end — the log under the bar says why";
  } finally { go.disabled = false; }
};


// ---- whole-chip wipe -----------------------------------------------------------
// The one control with no undo, behind a typed word rather than a checkbox: the
// checkbox above means "also clear the config" on a re-flash, and this means
// "clear the chip". It reuses streamFlash because the stream is the same
// contract — log, mac, progress, error, done, and the same Cancel door — with
// the difference that what it writes is nothing. It ends on a blank chip: the
// hub dropped the row, the name, and the grant the moment the erase completed
// (forgetWipedOrb, server-side), and the board reappears as nothing but the
// New Orb button waiting for the BOOT/RESET recovery.
function wipeArm() {
  const port = current.port;
  if (!port) {
    $("#ob-chipwipe-hint").textContent = "this orb is not on a cable right now — the wipe takes exactly the board on that lead";
    return;
  }
  $("#ob-chipwipe").hidden = true;
  $("#ob-chipwipe-hint").hidden = true;
  $("#ob-chipwipe-confirm").hidden = false;
  $("#ob-chipwipe-port").textContent = port;
  $("#ob-chipwipe-go").disabled = true;
  $("#ob-chipwipe-word").value = "";
  $("#ob-chipwipe-word").focus();
}

function wipeDisarm() {
  $("#ob-chipwipe-confirm").hidden = true;
  $("#ob-chipwipe").hidden = false;
  $("#ob-chipwipe-hint").hidden = false;
  $("#ob-chipwipe-hint").textContent = "Erase the chip back to factory blank — firmware, Wi-Fi credentials and name all go. No undo. The blank chip then boot-loops, so the port flaps until you hold BOOT and tap RESET.";
}

$("#ob-chipwipe").onclick = wipeArm;
$("#ob-chipwipe-no").onclick = wipeDisarm;
// The button unlocks on the exact word, matched after trimming so a trailing space
// from an intent-to-paste does not read as a refusal.
$("#ob-chipwipe-word").oninput = () => {
  $("#ob-chipwipe-go").disabled = $("#ob-chipwipe-word").value.trim() !== "WIPE";
};

$("#ob-chipwipe-go").onclick = async () => {
  const port = current.port;
  if (!port || $("#ob-chipwipe-go").disabled) return;
  const log = $("#ob-log"), bar = $("#ob-bar > div");
  $("#mn-cable").disabled = true;
  $("#ob-chipwipe-go").disabled = true;
  let wiped = false;
  try {
    const { ok } = await streamFlash("/api/wipe", { port, confirm: "WIPE" }, log, bar);
    wiped = ok;
    if (ok) {
      // The hub dropped this board's row, name and address on success; the port
      // list and the fleet both have to be re-read for that to show, or the
      // page keeps an orb the wire says is gone.
      await refreshPorts();
      pullFound();
      show("manage"); location.hash = "";
      $("#mn-find-note").textContent = "erased — the blank chip boot-loops, so that port comes and goes. " +
        "Hold BOOT, tap RESET, release BOOT to force download mode, then flash it from New Orb.";
    }
  } finally {
    $("#mn-cable").disabled = false;
    wipeDisarm();
    if (!wiped) $("#ob-chipwipe-go").disabled = false;
  }
};


// ---- post-flash onboarding ----------------------------------------------------
// The firmware decides the branch, not the UI and not the flash mode: a flash
// that leaves stored NVS lets the orb self-join in ~2.5 s, while a fresh or
// wiped orb opens its own open provisioning AP the moment boot finds NVS empty.
// The flash session reads the boot console while it still owns the port and
// records that verdict; /api/watch consumes it. A connected orb's address is
// known only from what the hub read over USB — the boot banner the flash logged,
// or the lease the serial door read after PROV-ACK — never from an ARP table.
let orbIP = "";
let provPort = ""; // the serial port the wired-provision door writes to
let provMac = "";  // the flashed orb's MAC, the key back to its boot verdict

function startJoin(port, mac) {
  $("#ob-flashpanel").hidden = false;
  $("#ob-join").hidden = false;
  $("#ob-watch").textContent = "waiting for the orb to boot…";
  $("#ob-console").textContent = "";
  $("#ob-ap").hidden = true;
  $("#ob-done").hidden = true;
  $("#sp-log").textContent = "";
  orbIP = "";
  provPort = port;
  provMac = mac;
  streamWatch(port, mac);
}

async function streamWatch(port, mac, skipAp, ip) {
  await streamPost("/api/watch", { port, mac, skip_ap: !!skipAp, ip: ip || "" }, (ev) => {
    if (ev.log) {
      const el = $("#ob-console");
      el.textContent += ev.log + "\n";
      el.scrollTop = el.scrollHeight;
    } else if (ev.error) {
      $("#ob-watch").textContent = ev.error;
    } else if (ev.result) onWatchResult(ev.result);
  });
}

function onWatchResult(res) {
  const watch = $("#ob-watch");
  if (res.mode === "lan") {
    // A full flash preserves NVS, so a provisioned orb returns to its network
    // on its own — pushing a scan/join form at a connected orb is noise. The
    // form is for the orb that is reachable but NOT connected (wrong
    // passphrase after a router change), which is the case that needs it.
    if (res.status && res.status.state === "connected") {
      watch.textContent = "orb is on the LAN at " + res.ip + ", already connected.";
      doneWithOrb(res.ip, res.status);
    } else {
      const st = res.status ? res.status.state : "unknown state";
      watch.textContent = "orb is reachable at " + res.ip + " but its state is \"" + st + "\" — its Wi-Fi card is ready.";
      showPicker(res.ip);
    }
  } else if (res.mode === "ap") {
    watch.textContent = "orb has no stored network — it opened its own provisioning AP.";
    $("#ob-ap-ssid").textContent = res.ap_ssid;
    populateSerialNetworks(res.networks || []);
    $("#ob-ap").hidden = false;
  } else {
    watch.textContent = "orb did not announce itself: " +
      (res.reason || "it is flashed and holding — add it by address once you know where it lives") + ".";
  }
}

// showPicker hands a reachable-but-unconnected orb to its own manage view, whose
// Wi-Fi card is the one join form on this page. Two join forms drift; one does
// not, and the address is already in the state the card dials through.
function showPicker(ip) {
  $("#ob-ap").hidden = true;
  $("#ob-done").hidden = true;
  if (ip) openOrbByIP(ip, provMac, null);
}

function doneWithOrb(ip, st) {
  orbIP = ip;
  $("#ob-done-msg").textContent =
    "orb is connected at " + ip +
    " (" + st.rssi + " dBm, fw " + st.fw +
    (st.tz ? ", " + st.tz : "") + "). Nothing left to configure.";
  $("#ob-done").hidden = false;
  $("#ob-ap").hidden = true;
  if (pendingName && provMac) {
    hubFetch("/api/fleet/set", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mac: provMac, name: pendingName }),
    }).then(() => { pullFound(); }).catch(() => { /* the name card says so if it matters */ });
    pendingName = "";
  }
  pullFound();
}

$("#ob-rejoin").onclick = () => { $("#ob-done").hidden = true; showPicker(orbIP); };
$("#ob-manage").onclick = () => { $("#ob-flashpanel").hidden = true; openOrbByIP(orbIP, provMac, null); };
$("#ob-ap-here").onclick = () => { window.open("http://192.168.4.1/", "_blank"); };
$("#j-rescan").onclick = () => loadNetworks(true);

// populateSerialNetworks fills the wired-provision door's scan list: the boot
// scan the flash captured off the console. It is the ONLY network list
// available when the hub is wired-only and cannot reach the AP's 192.168.4.1.
function populateSerialNetworks(nets) {
  const sel = $("#sp-ssid");
  sel.innerHTML = "";
  const manual = document.createElement("option");
  manual.value = "";
  manual.textContent = nets.length ? "— or type it below —" : "— scan saw nothing; type it below —";
  sel.appendChild(manual);
  nets.forEach((n) => {
    const o = document.createElement("option");
    o.value = n.ssid;
    o.textContent = n.ssid + "  (ch " + n.ch + ", " + n.rssi + " dBm" + (n.open ? ", open" : "") + ")";
    sel.appendChild(o);
  });
  if (nets.length) sel.selectedIndex = 1; // best (first, firmware sorts strong→weak)
}

$("#sp-go").onclick = async () => {
  const jlog = $("#sp-log");
  const ssid = ($("#sp-manual").value.trim() || $("#sp-ssid").value || "").trim();
  if (!ssid) { jlog.textContent = "pick or type a network first"; return; }
  if (!provPort) { jlog.textContent = "no bound serial port — flash first"; return; }
  const pass = $("#sp-pass").value;
  const tz = $("#sp-tz").value.trim();
  const go = $("#sp-go");
  go.disabled = true;
  jlog.textContent = "";
  try {
    await streamPost("/api/provision", { port: provPort, ssid, pass, tz }, (ev) => {
      if (ev.log) { jlog.textContent += ev.log + "\n"; jlog.scrollTop = jlog.scrollHeight; }
      else if (ev.error) { jlog.textContent += "error: " + ev.error + "\n"; }
      else if (ev.result) {
        const n = ev.result;
        if (n.ack && n.ip) {
          jlog.textContent += `accepted "${ssid}" — orb joined and took lease ${n.ip} over USB.\n`;
        } else {
          jlog.textContent += (n.ack
            ? `accepted "${ssid}" — the orb is joining; watch for it on the LAN.`
            : `no PROV-ACK for "${ssid}" — check the passphrase, or the orb may already be configured (use its AP page).`) + "\n";
        }
        if (n.ack) streamWatch(provPort, provMac, true, n.ip || ""); // confirm the USB lease, else re-probe the LAN
      }
    });
  } finally {
    go.disabled = false;
  }
};

let scanTimer = null;
async function loadNetworks(fresh) {
  const jlog = $("#j-log");
  if (!orbIP) { jlog.textContent = "no orb selected yet"; return; }
  const sel = $("#j-ssid");
  jlog.textContent = "scanning " + orbIP + "…";
  clearTimeout(scanTimer);
  try {
    const r = await hubFetch(`/orb/${orbIP}/api/v1/wifi/scan` + (fresh ? "?fresh=1" : ""));
    const d = await readDeviceJSON(r);
    if (d.status === "scanning") {
      scanTimer = setTimeout(() => loadNetworks(false), 1200);
      return;
    }
    const list = d.networks || [];
    if (!list.length) { jlog.textContent = "no networks found — try Rescan"; return; }
    const prev = sel.value;
    sel.innerHTML = "";
    for (const n of list) {
      const o = document.createElement("option");
      o.value = n.ssid;
      o.textContent = `${n.ssid}  (${n.rssi} dBm${n.open ? ", open" : ""})`;
      sel.appendChild(o);
    }
    if (list.some((n) => n.ssid === prev)) sel.value = prev;
    jlog.textContent = list.length + " network(s). Pick one, enter the password.";
  } catch (e) {
    let msg = "\u2717 scan failed: " + e.message + " — check the IP; Rescan to retry.";
    // 192.168.4.1 is the orb's own AP; a hub wired to the LAN has no route to
    // it, so the honest instruction is to reach the orb's config page from a
    // machine that joined the AP — not to keep hammering an unreachable IP.
    if (orbIP === "192.168.4.1") {
      msg += "\nThis hub is wired to the LAN and cannot reach the orb's AP at 192.168.4.1. Join the orb's open AP from this laptop, or enter the orb's LAN IP once it is on your network, then Rescan.";
    }
    jlog.textContent = msg;
  }
}


$("#j-go").onclick = async () => {
  const log = $("#j-log");
  const ip = orbIP;   // the Wi-Fi card belongs to the orb under management; there is no other target
  const ssid = $("#j-ssid").value;
  if (!ip) { log.textContent = "enter the orb's LAN IP"; return; }
  if (!ssid) { log.textContent = "pick a network first (Rescan if the list is empty)"; return; }
  orbIP = ip;
  const body = { ssid, pass: $("#j-pass").value };
  // Empty tz must be omitted, not sent as "": the firmware reads {"tz":""} as
  // "clear the zone" and an absent key as "keep the stored one".
  const tz = $("#j-tz").value.trim();
  if (tz) body.tz = tz;
  log.textContent = "→ joining " + ssid + "…";
  try {
    const r = await hubFetch(`/orb/${ip}/api/v1/wifi/join`, {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
    });
    log.textContent += "\n← " + (await r.text());
  } catch (e) { log.textContent += "\n\u2717 " + e.message; }
  // The reply is {"state":"joining"} sent before the radio moves: joining
  // drops the very TCP connection it arrives on, so confirmation is a status
  // poll, exactly like the orb's own page does it.
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    await new Promise((res) => setTimeout(res, 1500));
    try {
      const r = await hubFetch(`/orb/${ip}/api/v1/status`);
      const s = await r.json();
      if (s.state === "connected") {
        log.textContent += "\n\u2713 connected — the orb is on " + ssid + " at " + ip + ".";
        doneWithOrb(ip, s);
        return;
      }
      log.textContent += "\n" + s.state + (s.wifi_reason_name ? " (" + s.wifi_reason_name + ")" : "");
      if (s.wifi_reason_name && /auth|wrong/.test(s.wifi_reason_name) && s.assoc_fails > 2) {
        log.textContent += "\n\u2717 the network refused the credentials — check the password and retry.";
        return;
      }
    } catch (e) { /* the join drops requests mid-flight; the next tick recovers */ }
  }
  log.textContent += "\nstill not connected after 30 s — check the Orbs list in a moment.";
};

async function postOrb(ip, path, body, out) {
  out.hidden = false; out.textContent = "\u2192 POST " + path;
  try {
    const r = await hubFetch("/orb/" + ip + path, {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
    });
    out.textContent += "\n\u2190 " + (await r.text());
  } catch (e) { out.textContent += "\n\u2717 " + e.message; }
}

// ---- discovered orbs --------------------------------------------------------------
//
// The hub listens for _orb._tcp check-ins and the hub alone: the page reads a server-side
// list and never touches 5353, because a browser cannot and a page must not pick its own
// targets. The list arrives pushed. /api/orbs/stream answers the moment the page opens and
// again whenever a check-in changes what the card says, so a row lands within a round-trip of
// the orb announcing itself instead of at the next tick of a timer, and there is no button to
// press for a fact the hub already has. A frame body is the same envelope GET /api/orbs
// answers, and applyFound renders either one: nothing on this card renders a row by a second
// path.
//
// What the list may not do is make any of them reachable. Choosing a row fills the IP box the
// operator then loads, so the click that authorises a dial stays a human one, exactly as it
// was when the only way to get an address was to type it.
//
// Rows are built with createElement/textContent, never interpolated markup: an instance name
// and a fw stamp arrive over UDP from a device on the LAN, which is the same trust level as a
// theme name off an SD card, and the same rule applies.

// foundRows keys the DOM by mac, which is the row's identity in the hub's own terms: an orb
// that took a new lease patches the same <li> rather than trading places with itself, and a
// row nobody announced any more is removed rather than left to age on screen.
const foundRows = new Map();
let refusedTail = "";
let orbAbort = null;
let orbBytesAt = 0;

// applyFound renders one snapshot of the card, from a stream frame or from the GET pull
// underneath it. Rows already on screen are patched in place, so a check-in cannot steal
// focus, wipe a half-typed name, or scroll a list the operator is reading.
function applyFound(d) {
  const list = $("#mn-find"), state = $("#mn-find-state");
  const orbs = (Array.isArray(d.orbs) ? d.orbs : []).filter((o) => !isBlankRow(o));
  $("#mn-empty").hidden = orbs.length !== 0;
  const fresh = orbs.filter((o) => o.update === true).length;
  if (fresh) $("#mn-find-note").textContent = fresh + " orb(s) carry older firmware than this hub; the amber tag names them.";
  const live = new Set();
  for (const o of orbs) {
    // Keyed on the roster key, not the mac: a cable the host cannot attribute has no
    // mac, and two of them must not fight over one <li>.
    const k = o.key || o.mac;
    live.add(k);
    let li = foundRows.get(k);
    if (!li) {
      li = foundRow(o);
      foundRows.set(k, li);
      // Above the refusal tail, which is not a row and is rebuilt wholesale; below the
      // rows already there, which stay exactly where the operator left them.
      const tail = list.querySelector("li.refused");
      if (tail) list.insertBefore(li, tail); else list.appendChild(li);
    }
    paintFound(li, o);
  }
  for (const entry of foundRows) {
    if (!live.has(entry[0])) { entry[1].remove(); foundRows.delete(entry[0]); }
  }
  drawRefused(list, (d.refused || []).slice(-3).reverse());

  // The header describes what the list is a view of. A deaf mDNS listener no longer
  // empties it: rows on cables are as real as rows from packets, and the old card that
  // cleared itself on a bind failure is the bug the merge exists to fix.
  const beat = "check-ins every " + (d.checkinS || 60) + " s";
  const onCable = orbs.filter((o) => o.onCable).length;
  const lanN = orbs.filter((o) => (o.via || []).indexOf("lan") >= 0).length;
  const parts = [];
  if (lanN) parts.push(lanN + " by LAN");
  if (onCable) parts.push(onCable + " on cable");
  state.textContent = (!d.listening
    ? "\u2717 no LAN listener: " + (d.error || "unknown reason")
    : orbs.length
      ? orbs.length + " orb" + (orbs.length === 1 ? "" : "s") + " \u2014 " + parts.join(" + ") + " (" + beat + ")"
      : "no orb seen: none announced, and none on a cable")
    + (d.cableError ? "  \u00b7  port list failed: " + d.cableError : "");
}

// drawRefused replaces the refused tail, and only when its content moved. Those lines are the
// hub explaining why a packet did not become a row, and rebuilding them under an operator who
// was reading one is the mistake the old timer made to the rows.
function drawRefused(list, tail) {
  const sig = tail.map((f) => (f.name || "") + "|" + f.reason).join(";");
  if (sig === refusedTail) return;
  refusedTail = sig;
  for (const li of Array.prototype.slice.call(list.querySelectorAll("li.refused"))) li.remove();
  for (const f of tail) {
    const li = document.createElement("li");
    li.className = "found refused";
    li.textContent = "refused " + (f.name || "a packet") + ": " + f.reason;
    list.appendChild(li);
  }
}

// pullFound is the one place this card asks, and it asks once: after a rename lands or a
// Forget, where the change came from the page itself and the hub has no reason to announce
// it. It is not a poll and it is not a button coming back — it is a write reading its result.
async function pullFound() {
  const state = $("#mn-find-state");
  try {
    applyFound(await getJSON("/api/orbs"));
  } catch (e) {
    state.textContent = "\u2717 discovery unread: " + e.message;
  }
}


// watchOrbs holds the pushed card open, and reconnects: on loss after a beat, and on silence
// through the watchdog below. Silence matters because a hub that went away mid-connection —
// upgraded, rebooted, a laptop asleep — can leave the socket open with nothing ever arriving,
// and a card that keeps showing last night's fleet is worse than one that admits it stopped
// talking to the hub.
async function watchOrbs() {
  const ctrl = new AbortController();
  orbAbort = ctrl;
  const state = $("#mn-find-state");
  try {
    const r = await fetch("/api/orbs/stream", {
      signal: ctrl.signal, headers: { Accept: "application/x-ndjson" },
    });
    if (!r.ok || !r.body) throw new Error("HTTP " + r.status);
    const rd = r.body.getReader(), dec = new TextDecoder();
    let buf = "";
    for (;;) {
      const got = await rd.read();
      if (got.done) break;
      orbBytesAt = Date.now();
      buf += dec.decode(got.value, { stream: true });
      let nl;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (!line || line.charAt(0) === ":") continue;
        let j;
        try { j = JSON.parse(line); } catch (e) { continue; }
        if (!j.ok) { state.textContent = "\u2717 " + (j.error || "the hub refused the stream"); continue; }
        applyFound(j.data);
      }
    }
  } catch (e) {
    if (!ctrl.signal.aborted) {
      // A lost stream is not a quiet LAN: the hub itself went away, and with it every
      // fact this card renders — packets and cable sightings alike. Clear the rows
      // rather than leaving last night's bench on screen, which is what the pin on
      // foundRows.clear() below guards.
      for (const entry of foundRows) entry[1].remove();
      foundRows.clear();
      state.textContent = "\u2717 discovery stream lost: " + e.message;
    }
  }
  if (orbAbort === ctrl) {
    orbAbort = null;
    setTimeout(watchOrbs, 3000);
  }
}

// The watchdog. The hub's keepalive is 25 s, so three beats of no bytes at all is a dead pipe
// rather than a quiet LAN. Aborting is the reconnect: the read above rejects, the catch stays
// quiet because this side asked for it, and the tail reopens the stream.
setInterval(() => {
  if (!orbAbort || !orbBytesAt) return;
  if (Date.now() - orbBytesAt < 90000) return;
  $("#mn-find-state").textContent = "\u2717 discovery stream went quiet \u2014 reopening";
  orbAbort.abort();
}, 10000);

// foundRow builds one row's shell; paintFound writes onto it. The split is the point: the
// controls are wired to the row's mac rather than to a snapshot of the packet, so a frame that
// lands while the operator is mid-word cannot strand a handler on a stale address, and the
// patch has somewhere to put a new age without rebuilding the row around their cursor.
function foundRow(o) {
  const li = document.createElement("li");
  li.className = "found";
  li.dataset.key = o.key || o.mac;
  li.dataset.mac = o.mac || "";

  const label = document.createElement("button");
  label.className = "ghost found-pick";
  const badge = document.createElement("span");
  badge.className = "found-via";

  const name = document.createElement("input");
  name.placeholder = "name"; name.maxLength = 32;
  // "stored on the orb" is the sentence that changed. The word goes onto the board
  // first and the card keeps a copy of it, so a reflash or a state file lost in a move
  // costs the orb its label again only if the write never landed.
  name.title = "the word you use for this orb; stored on the orb itself and keyed on the mac";
  // The hub remembers an orb's ADDRESS the moment it learns one, so there is nothing to
  // press to add a row: the name field IS the save, and it saves to the board. A change
  // commits itself on blur or Enter — one keystroke path, no dead button in it.
  // There is deliberately no room or label field: the address book stores where a board
  // is reachable and nothing about where it sits, because a word the hub kept about a
  // board was a word that could disagree with the board.
  const commit = () => {
    if (!li.dataset.mac) return;
    hubFetch("/api/fleet/set", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mac: li.dataset.mac, name: name.value }),
    }).then(async (r) => {
      const j = await r.json().catch(() => ({}));
      if (j.error) {
        $("#mn-find-note").textContent = "\u2717 " + j.error;
        pullFound();   // the store kept its answer; show that, not the typed word
      } else {
        $("#mn-find-note").textContent = "";
      }
    });
  };
  name.onchange = commit;
  for (const el of [name]) {
    el.onkeydown = (e) => { if (e.key === "Enter") { e.preventDefault(); el.blur(); } };
  }

  const forget = document.createElement("button");
  forget.className = "ghost"; forget.textContent = "Forget";
  // Forget is the one removal this list offers: the hub remembers every board it has
  // seen and never needs a press to add one. The confirm is not ceremony — it drops the
  // name the operator chose and revokes any address the hub was willing to dial.
  //
  // It is also only ever worth pressing on a board that cannot come straight back, and
  // the hub is the one who knows that: it can see the cable sweep, the last check-in and
  // whether the remembered address still answers. So the row carries the verdict and this
  // renders it. A Forget that deletes a row an announcing orb rebuilds in sixty seconds
  // teaches the operator that the button does nothing, which is worse than no button.
  forget.onclick = () => {
    if (forget.disabled) return;   // the hub said no; the title says what to do instead
    const where = li._ip ? "? The hub stops dialing " + li._ip + "." : "; the hub will stop listing it.";
    if (!confirm("Forget " + (name.value || li.dataset.mac) + where)) return;
    hubFetch("/api/fleet/forget", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mac: li.dataset.mac }),
    }).then(async (r) => {
      const j = await r.json().catch(() => ({}));
      // The door can still refuse: the card's verdict is cached, and a board plugged
      // back in or heard from between this frame and the click is exactly the case the
      // rule is about. Say so on the card instead of redrawing it as if it worked.
      if (!r.ok || j.ok === false) $("#mn-find-note").textContent = "\u2717 " + (j.error || "the hub refused");
      else $("#mn-find-note").textContent = "";
      pullFound();
    });
  };

  // A cable the host cannot attribute is a row with a tty and nothing else, and the one
  // action that changes that is the probe: it costs a reboot, so the hub offers it here
  // and pays for it on the Onboard card, where the port is already the thing under test.
  const identify = document.createElement("button");
  identify.className = "ghost"; identify.textContent = "Identify"; identify.hidden = true;
  identify.title = "open this board and read the chip once; costs a reboot";
  identify.onclick = () => {
    openOrbByIP(o.ip || "", o.mac, o);
  };

  const ident = document.createElement("span");
  ident.className = "row";
  // The Forget refusal, spelled out on the row whenever the button is dead. Created here
  // and filled by the paint, so a row that has not been annotated yet shows the reason
  // rather than a mystery.
  const fwhy = document.createElement("span");
  fwhy.className = "fwhy"; fwhy.hidden = true;
  ident.append(name, forget, identify, fwhy);
  const upd = document.createElement("span");
  upd.className = "tag update"; upd.textContent = "update"; upd.hidden = true;
  upd.title = "this hub carries newer firmware than this orb runs — open it to load";
  li._update = upd;
  label.append(upd);
  li.append(label, badge, ident);
  li._label = label; li._badge = badge; li._name = name;
  li._forget = forget; li._identify = identify; li._fwhy = fwhy;
  return li;
}

// viaBadge spells the access paths out in the words the hub means: how it could reach this
// board if asked, right now. "on LAN" and "on cable" are not synonyms — one is a packet
// from a network, the other is a fact about this machine's USB ports.
function viaBadge(o) {
  const via = Array.isArray(o.via) ? o.via : [];
  const lan = via.indexOf("lan") >= 0, usb = via.indexOf("usb") >= 0;
  const access = lan && usb ? "LAN + cable" : usb ? "on cable" : lan ? "on LAN" : "not seen now";
  const where = usb && o.ports && o.ports.length ? " (" + o.ports.join(", ") + ")" : "";
  const idn = o.identity === "confirmed" ? "mac confirmed"
    : o.identity === "descriptor" ? "mac from USB descriptor"
    : o.identity === "unknown" ? "mac unreadable: identify it"
    : "mac announced by the orb";
  return access + where + " \u00b7 " + idn;
}

// paintFound writes what the hub currently believes onto a row that already exists. It runs on
// every frame, so the two boxes the operator types into are left alone while they hold the
// caret, and a row whose address moved cannot leave a stale one behind in the confirm text.
function paintFound(li, o) {
  const label = li._label;
  const softAP = !!o.provisioning && String(o.ip).indexOf("192.168.4.") === 0;
  const shown = o.name || o.instance || o.mac || o.key || "?";
  const addr = o.ip ? o.ip + ":" + o.port : "no address";
  li.dataset.mac = o.mac || "";
  li._port = (o.ports && o.ports.length) ? o.ports[0] : "";
  label.textContent = [shown, addr, o.mac ? "mac " + o.mac : "no mac", o.fw ? "fw " + o.fw : "",
    o.state === "provisioning" ? "PROVISIONING" : (o.state || ""),
    o.contended ? "CONTESTED" : "",
    o.shared ? "shared name" : "", o.bound ? "" : "unverified",
    typeof o.ageS === "number" ? o.ageS + "s" : "",
    // A row with no via is an address-book entry: the hub has an address and no sighting
    // this run. Say which of remembering it is, because the dial verdict is the only thing
    // that distinguishes a board in a drawer from a board whose lease moved.
    (!o.via || !o.via.length) ? ("remembered" + (o.reachable === "dead" ? ", not answering"
      : o.reachable === "alive" ? ", answering" : ", asking now")) : ""].filter(Boolean).join("  \u00b7  ");
  li._badge.textContent = viaBadge(o);
  // The verdict is the hub’s, and read fail-closed like Forget’s: the field is
  // pinned non-omitempty server-side precisely so an absent one means no.
  li._update.hidden = o.update !== true;
  // Four separate reasons a row cannot be dialed, and the button says which one it is rather
  // than letting a click discover it: the board has no address at all, its address is a SoftAP
  // the hub cannot route to, two boards both claim it, or it was never allowed.
  label.disabled = !!o.contended;   // a cable row opens too: its cable doors are the manage state
  label.title = !o.ip ? (o.refusal || "opens over the cable it is on")
    : softAP ? "reachable only from its own network; the hub cannot dial a SoftAP address"
    : o.contended ? "two boards both announce this address, so the hub will not dial either"
    : (o.refusal || (o.dialable ? "already an address this hub dials"
      : "fills the Orb IP box; the hub dials it only once you load it"));
  label.onclick = () => { openOrbByIP(o.ip || "", o.mac, o); };
  // Identity is what the naming controls key on. A cable with no mac has no row anywhere to
  // write a name against — offering the box anyway would promise a save that cannot happen,
  // so the controls go dark and the Identify button takes their place.
  const nameable = !!o.mac;
  li._name.disabled = !nameable;
  // Forget is disabled on its own axis: a row can have a mac and still be one the hub
  // will rebuild, and the reason has to be readable without a click that does nothing.
  //
  // The test is "literally true", not "not false". The verdict is the hub's answer, and a
  // hub that did not answer is not a hub that said yes: an old build, a truncated frame or
  // a row shape this page has never seen all read as an absent field, and reading those as
  // permission is how a gate ends up guarding nothing. Fail closed, and say that is what
  // happened, so a silent field is a visible state rather than a live button.
  const forget = li._forget;
  const why = typeof o.forgetWhy === "string" ? o.forgetWhy : "";
  forget.disabled = !nameable || o.forgettable !== true;
  forget.title = !nameable ? "no mac yet, so there is no fleet entry to forget"
    : forget.disabled ? (why || "the hub sent no Forget verdict for this row")
    : "drop this board from the fleet and revoke the address the hub was dialing";
  // The reason goes on the row as well as in the title. A greyed button with no text is a
  // fault to diagnose rather than a rule to learn, and the operator should not have to
  // hover every row to find out which of the three facts is holding the button back.
  const fwhy = li._fwhy;
  fwhy.hidden = nameable && !forget.disabled;
  fwhy.textContent = forget.disabled
    ? (why || (nameable ? "the hub sent no Forget verdict: reload the page"
                       : "no mac yet, so there is no fleet entry to forget"))
    : "";
  li._identify.hidden = o.identity !== "unknown";
  li._ip = o.ip;
  if (document.activeElement !== li._name) li._name.value = o.name || "";

}


// ---- themes -----------------------------------------------------------------
// Everything here posts to a /api/theme/* endpoint; none of it builds a device URL.
// The one place the device theme API is spoken is internal/orbnet/themes.go on the
// server, which is what keeps the FORMAT-2 contract — address a theme by the id the orb
// issued for it — in one file instead of in six buttons.
let themeIP = "";

async function themePost(path, payload) {
  const r = await hubFetch(path, {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload),
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
  return j.data;
}

async function loadThemes(ip) {
  const state = $("#mn-themestate"), list = $("#mn-themes"), card = $("#mn-themecard");
  if (!ip) { card.hidden = true; return; }
  themeIP = ip;
  card.hidden = false;
  state.textContent = "reading the orb's theme list\u2026";
  list.innerHTML = "";
  let d;
  try {
    d = await themePost("/api/theme/list", { ip });
  } catch (e) {
    state.textContent = "no theme list from " + ip + ": " + e.message;
    return;
  }
  if (!d.mounted) {
    state.textContent = "no SD card mounted on this orb — themes live on the card";
    return;
  }
  const rows = Array.isArray(d.themes) ? d.themes : [];
  state.textContent = rows.length + " theme(s) on " + (d.card || "the card") +
    ", drawing \"" + (d.active || "?") + "\"" + (d.free_mb ? ", " + d.free_mb + " MB free" : "");
  for (const t of rows) list.appendChild(themeRow(t, state, d));
  if (!rows.length) {
    const empty = document.createElement("li");
    empty.className = "hint";
    empty.textContent = "the card holds no themes yet";
    list.appendChild(empty);
  }
}

// themeRow builds one row with DOM APIs rather than a template string: theme names
// are bytes off the orb's card and must never be parsed as markup.
//
// FORMAT 2 gives the row two halves of one identity, and the card shows both because
// they answer different questions. The name is what the owner calls the theme — the
// string inside the container, the only thing a human types or reads. The id is
// theme_id(name), the 12 hex the orb filed it under, and it is the ONLY handle install
// and delete accept: the device has no route that takes a name, so a button that sent
// one would be refused by every orb on the market. The id goes on the row for the same
// reason try_id and damage_id match on it — when a theme fails to load, that is the
// string the orb and the operator can both point at.
function themeRow(t, state, list) {
  const li = document.createElement("li");
  const label = document.createElement("b");
  label.textContent = t.name;
  li.appendChild(label);
  const id = document.createElement("span");
  id.className = "tag";
  id.textContent = t.id;
  id.title = "the orb files this theme under sha256(name)[:12]; install and delete name it this way";
  li.appendChild(id);
  if (t.active) {
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = "active";
    li.appendChild(tag);
  }
  // An overwrite leaves the name, and so the id, alone: the row can read active while
  // the card holds newer bytes than the glass is drawing. Saying so is the difference
  // between an operator who re-installs and one who waits for a face that never comes.
  if (t.changed) {
    const stale = document.createElement("span");
    stale.className = "tag warn";
    stale.textContent = "card is newer than what it is drawing";
    stale.title = "the installed slot holds an older copy of this theme; use it again to draw this one";
    li.appendChild(stale);
  }
  if (!t.ok) {
    const bad = document.createElement("span");
    bad.className = "tag err";
    bad.textContent = "not loadable: " + (t.why || "unknown");
    li.appendChild(bad);
  }
  // The orb remembers the last install it refused and the copy boot could not load,
  // keyed by id. Without them on the row a press that did nothing looks like a press
  // that worked.
  if (list && list.try_id === t.id && list.try_why) {
    const why = document.createElement("span");
    why.className = "tag err";
    why.textContent = "last try failed: " + list.try_why;
    li.appendChild(why);
  }
  if (list && list.damage_id === t.id && list.damage_why) {
    const why = document.createElement("span");
    why.className = "tag err";
    why.textContent = "the dial would not load it: " + list.damage_why;
    li.appendChild(why);
  }
  const use = document.createElement("button");
  use.className = "ghost";
  use.textContent = "Use";
  use.disabled = !t.ok || t.active;
  use.onclick = async () => {
    use.disabled = true;
    state.textContent = "installing " + t.name + " — the orb copies it and restarts itself";
    try { await themePost("/api/theme/install", { ip: themeIP, id: t.id }); }
    catch (e) { state.textContent = "install refused: " + e.message; }
    use.disabled = false;
  };
  li.appendChild(use);
  // Delete is the one theme action that cannot be undone from here, so it asks
  // first: the firmware's own delete is a plain unlink on the card. The confirm names
  // the theme the way the operator knows it and quotes the id being removed, because
  // two rows can share a name across a re-bake and never share an id.
  const del = document.createElement("button");
  del.className = "ghost";
  del.textContent = "Delete";
  del.onclick = async () => {
    if (!confirm('Delete theme "' + t.name + '" (' + t.id + ") from the orb SD card? This cannot be undone from here.")) return;
    del.disabled = true;
    state.textContent = "deleting " + t.name + "\u2026";
    try { await themePost("/api/theme/delete", { ip: themeIP, id: t.id }); await loadThemes(themeIP); }
    catch (e) { state.textContent = "delete refused: " + e.message; del.disabled = false; }
  };
  li.appendChild(del);
  return li;
}

$("#mn-default").onclick = async () => {
  if (!themeIP) return;
  const state = $("#mn-themestate");
  state.textContent = "restoring the built-in face — the orb restarts itself";
  try { await themePost("/api/theme/default", { ip: themeIP }); }
  catch (e) { state.textContent = "refused: " + e.message; }
};
$("#mn-rescan").onclick = async () => {
  if (!themeIP) return;
  const state = $("#mn-themestate");
  state.textContent = "asking the orb to re-read its card\u2026";
  try { await themePost("/api/theme/rescan", { ip: themeIP }); await loadThemes(themeIP); }
  catch (e) { state.textContent = "refused: " + e.message; }
};

// A .orbtheme exported from the Theme Editor goes straight to the orb. The hub never
// writes it to disk, so there is no dropped-file state to reason about afterwards.
//
// There is no name to type. FORMAT 2 files an upload under the name baked INSIDE the
// container — the orb reads it out of the header and names the card file sha256(name)
// [:12] — so a name entered here would either be ignored by the device or, worse, be
// shown to the operator as the theme's name while the card filed it as something else.
// The hub reads the real name back out of the file and reports it in the reply, which is
// what this says after a send.
$("#mn-up-btn").onclick = () => $("#mn-fw-file").click();
$("#mn-fw-file").onchange = async (e) => {
  const f = e.target.files && e.target.files[0];
  const st = $("#mn-up-state");
  e.target.value = "";
  if (!f) return;
  if (!themeIP) { st.textContent = "load an orb first"; return; }
  st.textContent = "sending " + f.size + " B\u2026";
  try {
    const r = await hubFetch("/api/theme/upload?ip=" + encodeURIComponent(themeIP),
      { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: f });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) {
      // The orb holds a DIFFERENT theme under this name. It is the one decision on an
      // upload the device hands back to the operator, so ask rather than report an
      // error, and re-send with the flag the firmware asks for. The prompt quotes the
      // orb's own sentence: it knows the name, and this page only ever read it off the
      // file, so guessing at it here would put a name on screen the card may not hold.
      const why = j.error || ("HTTP " + r.status);
      if (why.indexOf("already on the card") >= 0 &&
          confirm("The orb refused this upload: " + why + "\n\nReplace it with this file?")) {
        return uploadTheme(f, true, st);
      }
      throw new Error(why);
    }
    const d = j.data || {};
    st.textContent = "stored as \"" + (d.name || "?") + "\" (" + (d.id || "?") +
      ") \u2014 the orb verifies it before it will draw it";
    await loadThemes(themeIP);
  } catch (err) { st.textContent = "upload failed: " + err.message; }
};

// uploadTheme pushes the file, with the firmware's ?overwrite=1 set only when the
// operator has answered the replace prompt above.
async function uploadTheme(f, overwrite, st) {
  st.textContent = "replacing " + f.size + " B\u2026";
  try {
    const r = await hubFetch("/api/theme/upload?ip=" + encodeURIComponent(themeIP) + (overwrite ? "&overwrite=1" : ""),
      { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: f });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
    const d = j.data || {};
    st.textContent = "replaced \"" + (d.name || "?") + "\" (" + (d.id || "?") + ")";
    await loadThemes(themeIP);
  } catch (err) { st.textContent = "upload failed: " + err.message; }
}

// ---- remembered orb ---------------------------------------------------------
// The hub remembers the address it last had an orb at — learned over USB or typed in
// — so the operator does not retype it every launch. It is a pre-fill, not a
// decision: nothing is dialled until Load is pressed, which is the act that registers
// the address with the proxy in the first place.
async function prefillOrbIP() {
  const el = $("#mn-ip");
  if (!el || el.value.trim()) return;
  try {
    const d = await getJSON("/api/orb/known");
    if (d && d.ip && !el.value.trim()) el.value = d.ip;
    const ad = $("#ad-ip");
    if (ad && d && d.ip && !ad.value.trim()) ad.value = d.ip;
  } catch (e) { /* a hub with no history is the normal first run */ }
}

// ---- status polling, with backoff -------------------------------------------
// A fixed poll is rude to a device whose httpd is single-worker and busy, and it is
// also how a loop keeps hammering an orb that has left the network. So: poll while it
// answers, and double the gap on each miss up to a cap, resetting the moment it
// answers again.
let pollTimer = null;
let pollDelay = 15000;

function startPolling(ip) {
  clearTimeout(pollTimer);
  const tick = async () => {
    const row = $("#mn-curfw");
    if (!row) return; // the card was cleared; stop rather than dial unbidden
    try {
      const st = await readDeviceJSON(await hubFetch("/orb/" + ip + "/api/v1/status"));
      row.textContent = "running " + (st.fw || "?") + " \u00b7 state " + (st.state || "?");
      pollDelay = 15000;
    } catch (e) {
      pollDelay = Math.min(pollDelay * 2, 240000);
    }
    pollTimer = setTimeout(tick, pollDelay);
  };
  pollTimer = setTimeout(tick, pollDelay);
}

// ---- USB console ------------------------------------------------------------
// Read-only: the hub opens the port, forwards lines for a bounded window, and closes.
// No reset and nothing written to the orb; the hub registers it as a port operation so
// it cannot overlap a flash and Stop reaches it.
let consoleLive = false;

async function readConsole() {
  const port = $("#mn-con-port").value;
  const log = $("#mn-con-log"), state = $("#mn-con-state");
  if (!port || port.startsWith("(")) { state.textContent = "no port selected"; return; }
  log.textContent = "";
  state.textContent = "listening on " + port + "\u2026";
  $("#mn-con-go").disabled = true;
  $("#mn-con-stop").hidden = false;
  consoleLive = true;
  await streamPost("/api/console", { port }, (ev) => {
    if (!consoleLive) return;
    if (ev.line) log.textContent += ev.line + "\n";
    else if (ev.log) log.textContent += ev.log + "\n";
    else if (ev.done) state.textContent = ev.done;
    else if (ev.error) state.textContent = ev.error;
    log.scrollTop = log.scrollHeight;
  });
  consoleLive = false;
  $("#mn-con-go").disabled = false;
  $("#mn-con-stop").hidden = true;
}

$("#mn-con-go").onclick = readConsole;
$("#mn-con-stop").onclick = () => {
  consoleLive = false;
  $("#mn-con-state").textContent = "stopped";
  $("#mn-con-go").disabled = false;
  $("#mn-con-stop").hidden = true;
};

// The console picker lists the same cables the Onboard card lists, so an operator
// who can flash an orb can always read it.
async function fillConsolePorts() {
  const sel = $("#mn-con-port");
  if (!sel) return;
  try {
    const { ports } = await getJSON("/api/ports");
    const list = Array.isArray(ports) ? ports : [];
    sel.innerHTML = "";
    if (!list.length) { sel.innerHTML = "<option>(no serial ports found)</option>"; return; }
    for (const entry of list) {
      const o = document.createElement("option");
      const p = typeof entry === "string" ? entry : entry.port;
      o.value = p;
      o.textContent = portLabel(p, entry);
      sel.appendChild(o);
    }
  } catch (e) { sel.innerHTML = `<option>(${esc(e.message)})</option>`; }
}


// ---- the manage view's two plain writes ---------------------------------------
$("#mn-name-go").onclick = async () => {
  const note = $("#mn-name-note"), name = $("#mn-name").value;
  if (!current.mac) { note.textContent = "no mac, no fleet row to name"; return; }
  note.textContent = "writing…";
  try {
    const r = await hubFetch("/api/fleet/set", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mac: current.mac, name }),
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
    note.textContent = "the board answers to that name now";
    current.name = name;
    pullFound();
  } catch (e) { note.textContent = "\u2717 " + e.message; }
};

$("#mn-tz-go").onclick = () => {
  if (!current.ip) { $("#mn-tz-note").textContent = "no address to write to"; return; }
  postOrb(current.ip, "/api/v1/config", { tz: $("#mn-tz").value.trim() }, $("#mn-tz-note"));
};

// ---- the Add-by-IP dialog -------------------------------------------------------
// For the orb the hub cannot hear: a network that blocks its broadcasts, a board
// that took a new lease. One POST asks the address who it is; only an orb
// answering earns a row, and the dialog keeps its own counsel until then — a
// wrong typing costs one sentence, Cancel costs nothing, and the fleet is
// untouched either way because the hub learns the grant only from the answer.
function openDialog() {
  $("#ad-err").hidden = true;
  $("#ad-mask").hidden = false;
  $("#ad-ip").focus();
}
function closeDialog() { $("#ad-mask").hidden = true; }
$("#ad-open").onclick = openDialog;
$("#ad-cancel").onclick = closeDialog;
$("#ad-mask").addEventListener("click", (e) => { if (e.target === $("#ad-mask")) closeDialog(); });
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !$("#ad-mask").hidden) closeDialog();
});
$("#ad-check").onclick = async () => {
  const ip = $("#ad-ip").value.trim(), err = $("#ad-err"), go = $("#ad-check");
  if (!ip) { err.textContent = "type the address the orb is answering at"; err.hidden = false; return; }
  go.disabled = true;
  err.hidden = true;
  err.textContent = "";
  try {
    const r = await hubFetch("/api/orb/add", {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ ip }),
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
    const d = j.data || {};
    closeDialog();
    $("#mn-find-note").textContent = "added " + (d.name || d.mac || "") + " at " + ip + " — the hub dials it as an orb.";
    await pullFound();
    openOrbByIP(ip, d.mac, null);
  } catch (e) {
    // The refusal stays in the dialog, which is where the typing happened: the
    // operator either corrects the address or cancels. Nothing else moved.
    err.textContent = (e.message || String(e)) + " — nothing was added. Try another address, or Cancel.";
    err.hidden = false;
    go.disabled = false;
    return;
  }
  go.disabled = false;
};
$("#ad-ip").addEventListener("keydown", (e) => { if (e.key === "Enter") $("#ad-check").click(); });

// ---- boot -----------------------------------------------------------------------
// The stream is the card; the ports poll follows the physical bench, and the
// watchdog below reopens the stream, never the card. A blank board appearing on
// a cable is a port fact, and port facts only change because someone reached
// for a cable, which is exactly what a five-second poll is for.
route();
prefillOrbIP();
fillConsolePorts();
refreshPorts();
refreshBuild();
watchOrbs();
setInterval(() => { refreshPorts(); }, 5000);
