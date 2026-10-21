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
  // #onboard is a dialog over the fleet, not a view: an old bookmark lands on
  // the list with the dialog open, which is where it belongs.
  if (v === "onboard") { show("manage"); openNewOrbDialog(); return; }
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
//
// The third field here, "notOrb", keeps those two verdicts honest about a board
// that has been reflashed out of gorb-os. orbSeen is built from history, and the
// history is keyed on a MAC the reflash cannot change, so left alone it holds a
// board shut in both doors at once. The hub clears it wherever it holds a paid
// verdict that the chip is running something else, and says so with notOrb so the
// label below can name what is actually on the lead.
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
    o.textContent = blankLabel(entry);
    sel.appendChild(o);
  }
  renderBlankGate();
  renderIdentity(allPorts);
  fillConsolePorts();
}

// renderBlankGate is the button and the three honest states of the New Orb
// dialog: one blank (a line of text — a dropdown with one option asks
// nothing), several (pick a cable), none ("no orb is plugged in"). Ports
// enumerate themselves on the hub's own poll, so no state offers a refresh
// door and none needs one.
function renderBlankGate() {
  $("#mn-add").hidden = blanks.length === 0;
  $("#ob-blankpick").hidden = blanks.length < 2;
  $("#ob-blankone").hidden = blanks.length !== 1;
  $("#ob-blanknone").hidden = blanks.length !== 0;
  if (blanks.length === 1) {
    $("#ob-blankone").textContent = "Flashing the ESP32-S3 on " + blankLabel(blanks[0]);
  }
  gateFlashButton();
}

// gateFlashButton is the name requirement made physical: no word, no flash.
// The hub enforces the same rule server-side; this is the same truth at zero
// cost, and the button's title says so.
function gateFlashButton() {
  $("#ob-go").disabled = blanks.length === 0 || !($("#ob-name").value || "").trim();
}
$("#ob-name").addEventListener("input", gateFlashButton);

// openNewOrbDialog is the whole New Orb flow now: a frame on the page that is
// already showing the fleet, not a second page about the same bench. The name
// box is always empty on open — the last orb's name is a fact about the last
// orb, and pre-filling it is how two boards end up called "Gold".
function openNewOrbDialog() {
  renderBlankGate();
  $("#ob-name").value = "";
  $("#ob-blankstate").textContent = "";
  newOrbStep("form");
  $("#no-mask").hidden = false;
  $("#ob-name").focus();
}
$("#mn-add").onclick = (e) => { e.preventDefault(); openNewOrbDialog(); };
$("#no-close").onclick = closeNewOrbDialog;
$("#no-mask").addEventListener("click", (e) => { if (e.target === $("#no-mask")) closeNewOrbDialog(); });
function closeNewOrbDialog() {
  // A running flash keeps going even when the dialog closes — the hub owns the
  // wire, not this frame — but Cancel is the door that stops it. Closing while
  // one runs is allowed and says so below the bar.
  $("#no-mask").hidden = true;
  newOrbStep("form");
}

// newOrbStep moves the dialog between its form (choose, name) and its progress
// (watch). The dialog never swaps out for another page mid-flash: this is the
// frame the button opened, still open, with the bar in it.
function newOrbStep(step) {
  $("#nf-form").hidden = step !== "form";
  $("#nf-run").hidden = step !== "run";
  $("#ob-go").hidden = step !== "form";
  $("#no-foot .ghost").textContent = step === "run" ? "Close" : "Cancel";
}

// blankLabel is all the New Orb panel says about a candidate board: the port
// it sits on and the MAC the host reads off the USB descriptor for free — the
// whole of what a blank JTAG board reveals. Whether the chip once was an orb
// changes nothing about the flash, so nothing says so here.
//
// One thing DOES change the flash, and it gets a word: a board the hub has proof
// is running firmware that is not gorb-os. The flash erases it either way, but the
// operator choosing this lead is choosing to overwrite something specific, and a
// picker that called it blank beside a genuinely blank board was describing the
// bench rather than what is on it.
function blankLabel(entry) {
  const who = entry.usbMac ? entry.port + "  —  " + entry.usbMac : entry.port;
  if (entry.notOrb === true) {
    return who + "  ·  runs " + (entry.project || "other firmware") + ", not gorb-os";
  }
  if (entry.lonely === true) return who + "  ·  left lonely";
  return who;
}

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
  // The free cable answer, filed and never paid for with a reboot. It says the chip is
  // an orb and nothing else, so the label stops there: no version, no verdict, and not
  // the "not identified" a bare descriptor still earns.
  if (entry.cableProof && entry.isOrb) {
    return p + "  \u2014  " + (entry.usbMac || entry.mac || "orb") + "  \u00b7  an orb, answered over the cable";
  }
  if (entry.usbMac) { return p + "  —  " + entry.usbMac + "  ·  not identified"; }
  if (entry.hint) { return p + "  —  " + entry.hint + "  ·  not identified"; }
  return p;
}

// renderIdentity shows the cached verdict for the cable the manage view is
// looking at. It never probes: the whole point of the cache is that
// identifying an orb costs a reboot, and the hub is the one that decides when to pay
// it. This renders what that reboot already bought.
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
  // The cost belongs on the card in every state, not only before the first answer: the
  // hub reboots this board on its own schedule, so "identified 20:31" without that
  // context reads like a fact the hub simply knew.
  state.textContent = (chosen.probedAt ? "identified " + esc(chosen.probedAt) + " \u2014 " : "")
    + "the hub reboots this board to read it, about 10 seconds";
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
    el.innerHTML = "<b>Firmware " + who + "</b> - " + (b.source === "built-in" ? "built into this app" : "your file: " + esc(b.source)) +
      (b.foreign ? ' <span class="tag err">not gorb-os</span>' : "") +
      (b.merged ? ' <span class="tag err">whole-flash</span>' : "") +
      '<br><span class="hint">' + imgs.length + " image(s) - " +
      imgs.map((i) => esc(i.offset) + " " + esc(i.name) + " (" + (Number(i.bytes) || 0) + " B)").join(" - ") +
      (b.sha256 ? " - sha256 " + esc(String(b.sha256).slice(0, 16)) + "\u2026" : "") + "</span>";
    hubFirmware = b;
    gateFwButtons();
  } catch (e) { el.textContent = e.message; }
}

// hubFirmware is the last /api/build payload, kept so the flash modal can label its
// own first choice without asking again on every open. It is a cache of a server
// answer, never a substitute for one: refreshBuild is what runs after any change to
// what the hub carries.
let hubFirmware = null;

// ---- the flash modal ---------------------------------------------------------
// One dialog answers "what should this board be running", and it offers only what
// the connection supports. The split is not a preference. A cable writes the whole
// manifest whatever the board ran before, so every direction is safe and every file
// is eligible; Wi-Fi can only push this hub's app slot into the spare partition. A
// foreign blob appears on the cable branch alone, which is what lets it exist with
// no warning paragraph attached to it.
let ffPicked = null;   // the probed file on the disk branch, cleared on every open

function openFlashModal() {
  const port = current.port, ip = current.ip;
  const sub = [];
  if (port) sub.push("on cable " + port);
  if (ip) sub.push("at " + ip);
  $("#ff-sub").textContent = sub.length ? sub.join(" \u00b7 ") : "not reachable from here";
  $("#ff-none").hidden = !!(port || ip);
  $("#ff-choices").hidden = !(port || ip);
  if (!port && !ip) {
    $("#ff-none-msg").textContent = "This orb is neither on a cable of this computer nor answering on the LAN. Plug a cable in to flash it.";
  }
  // Disk is a cable-only choice. It could be pushed over Wi-Fi in principle, and
  // there it would mean an unverified blob through a door the orb's rollback guards,
  // for nothing the hub's own bundle does not already supply.
  $("#ff-disk-row").hidden = !port;
  $("#ff-wipe-row").hidden = !port;
  $("#ff-filebox").hidden = true;
  $("#ff-file").textContent = "";
  $("#ff-wipe").checked = false;
  ffPicked = null;
  ffForced = false;
  document.querySelectorAll('input[name="ff-src"]').forEach((r) => { r.checked = r.value === "hub"; });
  $("#ff-hub-label").textContent = "This hub \u2014 " + (hubFirmware && hubFirmware.version
    ? hubFirmware.project + " " + hubFirmware.version : (hubFirmware && hubFirmware.source) || "loading\u2026");
  $("#ff-state").textContent = "";
  ffNote();
  $("#ff-mask").hidden = false;
  ffGate();
}

function ffSource() {
  const r = document.querySelector('input[name="ff-src"]:checked');
  return r ? r.value : "hub";
}

// ffNote states what the write costs, once, in the tense of the choice actually
// made. It is the only sentence in the dialog, because it is the only fact an
// operator cannot read off the row they just clicked.
function ffNote() {
  const src = ffSource(), el = $("#ff-note");
  if (src === "disk") {
    el.textContent = ffPicked
      ? "Written over the cable. This replaces the bootloader, the partition table and both app slots."
      : "Pick a .zip release bundle or a whole-flash .bin.";
    return;
  }
  if (current.port) { el.textContent = "Written over the cable: bootloader, partition table and app slots."; return; }
  el.textContent = "Pushed to the spare slot over Wi-Fi. The bootloader rolls back if the new image never marks itself valid.";
}

// ffProbe asks the hub what a file is before anything is adopted, and the answer
// comes back as fields rather than as a sentence to parse. The button then names
// the project, which is the only warning shape that tells an operator something
// they can act on: a label that says brookesia is a decision made with information.
async function ffProbe(file) {
  const form = new FormData();
  form.append("firmware", file, file.name);
  form.append("allow_foreign", "1");
  form.append("probe", "1");
  const res = await hubFetch("/api/firmware", { method: "POST", body: form });
  const j = await res.json().catch(() => ({}));
  if (!res.ok || j.ok === false) {
    return { error: j.error || ("HTTP " + res.status) };
  }
  const d = j.data || {};
  return { file, name: file.name, project: d.project || "", version: d.version || "",
           foreign: !!d.foreign, merged: !!d.merged };
}

function ffGate() {
  const go = $("#ff-go");
  go.disabled = $("#ff-choices").hidden || (ffSource() === "disk" && !ffPicked);
  go.textContent = ffForced ? "Flash anyway" : "Flash";
}

// gateFwButtons lights the card's single Flash button. It is reachable whenever the
// board can be flashed at all: a cable, or an address to push to. The amber badge is
// the hub's own verdict from the pushed row, never a version comparison run here, so
// the badge and the flash gate cannot disagree about what "newer" means.
function gateFwButtons() {
  const btn = $("#mn-flashbtn");
  if (!btn) return;
  const reachable = !!(current.port || current.ip);
  btn.disabled = !reachable || !hubFirmware || !hubFirmware.present;
  const reset = $("#mn-resetbtn");
  if (reset) reset.disabled = !current.port;
  const badge = $("#mn-updatebadge");
  if (badge) badge.hidden = !(current.row && current.row.update === true);
}

$("#mn-flashbtn").onclick = openFlashModal;
$("#ff-cancel").onclick = () => { $("#ff-mask").hidden = true; };
$("#ff-mask").addEventListener("click", (e) => { if (e.target === $("#ff-mask")) $("#ff-mask").hidden = true; });
$("#ff-pick").onclick = () => $("#ob-fwfile").click();
document.querySelectorAll('input[name="ff-src"]').forEach((r) => {
  r.onchange = () => {
    $("#ff-filebox").hidden = ffSource() !== "disk";
    ffNote(); ffGate();
  };
});
$("#ob-fwfile").onchange = async (e) => {
  const f = e.target.files && e.target.files[0];
  e.target.value = "";            // let the same file be picked again
  if (!f) return;
  $("#ff-file").textContent = "reading " + f.name + "\u2026";
  const r = await ffProbe(f);
  if (r.error) {
    ffPicked = null;
    $("#ff-file").textContent = "";
    $("#ff-state").textContent = r.error;
    ffNote(); ffGate();
    return;
  }
  ffPicked = r;
  $("#ff-filebox").hidden = false;
  // The row names the project the hub read, which is the one thing an operator
  // needs to spot a wrong file before pressing Flash rather than after.
  $("#ff-file").textContent = r.name + (r.foreign ? " \u2014 " + r.project + ", not gorb-os"
    : (r.version ? " \u2014 " + r.version : ""));
  $("#ff-state").textContent = "";
  ffNote(); ffGate();
};

// ffGo commits. A disk pick was already parsed and rejected-or-accepted by the hub
// during ffProbe, so the second upload adopts bytes the hub has already read once;
// it is the same file, and the hub is still the thing that decides.
$("#ff-go").onclick = async () => {
  const go = $("#ff-go"), state = $("#ff-state");
  if (go.disabled) return;
  const src = ffSource();
  go.disabled = true;
  try {
    if (src === "disk") {
      state.textContent = "loading " + ffPicked.name + "\u2026";
      const form = new FormData();
      form.append("firmware", ffPicked.file, ffPicked.name);
      form.append("allow_foreign", "1");
      const res = await hubFetch("/api/firmware", { method: "POST", body: form });
      const j = await res.json().catch(() => ({}));
      if (!res.ok || j.ok === false) throw new Error(j.error || ("HTTP " + res.status));
      await refreshBuild();
    } else if (hubFirmware && hubFirmware.source && hubFirmware.source !== "built-in") {
      // Choosing "this hub" while the hub still carries a previous disk pick means
      // the operator asked for the shipped release, not for whatever is loaded.
      await hubFetch("/api/firmware/reset", { method: "POST" });
      await refreshBuild();
    }
    if (current.port) {
      // The cable hands off to the progress panel and then the join dialog, so the
      // modal steps out of the way before either appears.
      $("#ff-mask").hidden = true;
      await cableFlash($("#ff-wipe").checked);
    } else {
      // The wireless path keeps the dialog open. A downgrade comes back asking for a
      // second press, and a confirm written into a hidden dialog is the dead end this
      // whole rewrite exists to remove. wifiUpdate closes it on success.
      await wifiUpdate();
    }
  } catch (e) {
    state.textContent = e.message;
  } finally {
    ffGate();
  }
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



// streamFlash drives one POST /api/flash or /api/lonely and renders its NDJSON
// events into the shared flash panel: the bar is the story, the log is the
// detail, and the same Cancel door reaches both. It returns {mac, ok}: mac is
// the eFuse MAC the ROM reported, ok is whether the run reached its "done"
// event — the caller only runs onboarding after a real success, so a refused or
// mid-flash error never dumps the user into a Wi-Fi form for an orb that was
// not actually rewritten.
//
// The bar is ONE bar for the WHOLE flash: the hub weights the plan by bytes and sends
// the running percentage, so a 4 MB app moves the bar and a 12 kB otadata does not, and
// the count of images rides beside it as detail rather than as the story. Two rules keep
// the panel from twitching while it fills, both of them learned the hard way: nothing
// that changes length writes to the element the seconds counter writes to (two writers on
// one line alternated every two seconds), and nothing that grows the log writes to the
// log at all (the panel is anchored to the bottom of the screen, so every appended
// progress line pushed the bar itself upward). The depth behind a percentage lives in the
// bar's title, where wanting it costs a hover and nothing costs the layout.
// streamFlash drives one flash stream into WHATEVER ui is handed it, so the
// New Orb dialog and the manage view's panel share one reader and two frames.
// It returns the outcome rather than navigating anywhere: the caller decides
// what opening a dialog next means, and this function must never yank the page
// out from under a bar someone is watching. The panel the bar lives in must be
// visible BEFORE the call — a progress bar nobody sees is the bug this rewrite
// exists to kill.
async function streamFlash(url, payload, ui) {
  const log = ui.log, bar = ui.bar, opEl = ui.op, cancelBtn = ui.cancel;
  const liveEl = ui.live || opEl;
  log.textContent = ""; bar.style.width = "0%"; bar.title = ""; opEl.textContent = "";
  let mac = null;
  let ok = false;
  const port = payload.port || "";
  const loneling = url === "/api/lonely";
  ui.doing.textContent = loneling ? "freeing " + port : "flashing " + port;
  cancelBtn.hidden = false;
  cancelBtn.disabled = false;
  cancelBtn.onclick = async () => {
    cancelBtn.disabled = true;
    liveEl.textContent = "cancelling the write on " + port +
      " — it stops at the next block boundary; the chip stays in its bootloader, and one flash of either image finishes cleanly";
    try { await hubFetch("/api/flash/cancel", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ port }) }); }
    catch (e) { /* the stream's own log line says what happened */ }
  };
  const tick = setInterval(() => showLiveOp(port, liveEl), 2000);
  showLiveOp(port, liveEl);
  try {
    await streamPost(url, payload, (ev) => {
      if (ev.log) log.textContent += ev.log + "\n";
      else if (ev.error) log.textContent += "\n\u2717 " + ev.error + "\n";
      else if (ev.done) { log.textContent += "\n\u2713 " + ev.done + "\n"; ok = true; }
      else if (ev.mac) mac = ev.mac;
      else if (ev.progress) {
        const p = JSON.parse(ev.progress);
        const pct = Number.isFinite(p.pct) ? p.pct
          : (p.total ? Math.round((p.written / p.total) * 100) : 0);
        bar.style.width = pct + "%";
        bar.title = "image " + (p.image + 1) + "/" + p.of + " at " + p.offset +
          " \u00b7 " + p.written + "/" + p.total + " B \u00b7 " + pct + "% of " + p.bytes + " B";
        opEl.textContent = pct + "% \u00b7 " + (p.image + 1) + "/" + p.of;
      }
      log.scrollTop = log.scrollHeight;
    });
  } finally {
    clearInterval(tick);
    cancelBtn.hidden = true;
  }
  return { mac, ok };
}

// The manage view's own flash frame — the fixed bottom panel. fpOpen shows it
// the instant a door there is pressed; fpClose hides it once the work is over.
function fpUI() {
  return { log: $("#ob-log"), bar: $("#ob-bar > div"), doing: $("#ob-doing"),
           op: $("#ob-op"), live: $("#ob-liveop"), cancel: $("#ob-cancel") };
}
function fpOpen() { $("#ob-flashpanel").hidden = false; }
$("#fp-close").onclick = () => { $("#ob-flashpanel").hidden = true; };

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
  $('#ob-identify').hidden = !current.port;
  gateFwButtons();
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
    gateFwButtons();
    loadThemes(current.ip);
    startPolling(current.ip);
  } catch (e) {
    $("#mn-curfw").textContent = "no orb answering at " + current.ip + " \u2014 " + e.message;
    gateFwButtons();
  }
}


// cableFlash is the one cable write, for the hub's own firmware and for a file from
// disk alike. The ROM conversation does not care which bytes it is carrying, and the
// UI should not either: both converge on the join dialog a fresh flash converges on,
// so every successful flash ends the same way. It writes the whole manifest, which is
// why no version comparison is consulted here: bootloader, partition table, otadata
// and both app slots are rewritten whatever the board ran before, so a downgrade over
// a cable is not a special case and needs no confirmation of its own.
async function cableFlash(wipe) {
  const port = current.port;
  if (!port) return;
  fpOpen();
  const { mac, ok } = await streamFlash("/api/flash",
    { port, mode: "full", wipe: !!wipe }, fpUI());
  if (ok) {
    await refreshPorts();
    if (!current.mac && mac) current.mac = mac;
    openJoinDialog(port, mac || current.mac, "");
  }
}

// wifiUpdate is the OTA door, and the one place a version verdict is acted on.
// A downgrade is offered rather than refused: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
// means the bootloader reverts an image that never marks itself valid, so the
// hardware holds the safety a page cannot infer from a stamp. The confirm exists so
// stepping backwards is a deliberate press rather than a second click on an upgrade.
// ffForced is set only by a downgrade refusal from the hub, and cleared every
// time the modal opens, so a press that forces an upgrade can never leak into the
// next board opened in the same page.
let ffForced = false;
async function wifiUpdate() {
  if (!current.ip) return;
  const state = $("#ff-state"), go = $("#ff-go");
  state.textContent = "uploading to " + current.ip + "\u2026";
  try {
    const r = await hubFetch("/api/orb/update", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ip: current.ip, force: ffForced }),
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) {
      // A downgrade refusal is not an error to display, it is a second click
      // waiting to happen. The hub named the verdict, so the button changes and
      // says what the extra press costs; the rollback flag in the bootloader is
      // what makes the press recoverable rather than hopeful.
      if (j.overridable && j.verdict === "downgrade" && !ffForced) {
        ffForced = true;
        go.textContent = "Flash anyway";
        go.disabled = false;
        state.textContent = j.error + " The bootloader rolls back if the new image never boots.";
        return;
      }
      throw new Error(j.error || ("HTTP " + r.status));
    }
    const d = j.data || {};
    $("#mn-curfw").textContent = "staged and rebooting: " + d.from + " \u2192 " + d.to;
    $("#mn-verdict").textContent = "";
    $("#ff-mask").hidden = true;   // the card carries the rest; it is not a dead end
    pullFound();
  } catch (e) {
    state.textContent = e.message;
    go.disabled = false;
  }
}



// ---- New Orb: the one flash a blank board gets -------------------------------
// A fresh flash is the one flash with nothing to preserve: full manifest, NVS
// cleared, exactly as a board leaves the factory. The name is mandatory — the
// hub refuses a nameless fresh flash in the request itself, so no client is
// trusted with that rule — and the hub writes it over the cable the moment the
// app boots, before the stream reports done. The dialog stays open with its
// bar live through the whole write; when done lands, the dialog closes and the
// join dialog opens. The converged path for every flash door runs through
// openJoinDialog, and nothing else navigates.
$("#ob-go").onclick = async () => {
  const port = blanks.length === 1 ? blanks[0].port : $("#ob-port").value;
  if (!port || !blanks.some((b) => b.port === port)) {
    $("#ob-blankstate").textContent = "that cable no longer reads as a blank board — the list follows the physical ports";
    return;
  }
  const name = ($("#ob-name").value || "").trim();
  if (!name) { $("#ob-blankstate").textContent = "the orb needs a name before it can be flashed"; return; }
  newOrbStep("run");
  $("#ob-blankstate").textContent = "";
  const go = $("#ob-go"), close = $("#no-close");
  go.disabled = true; close.disabled = true;
  let mac = null, ok = false;
  const ui = { log: $("#nf-log"), bar: $("#nf-bar > div"), doing: $("#nf-doing"),
               op: $("#nf-op"), live: $("#nf-liveop"), cancel: $("#nf-cancel") };
  try {
    const r = await streamFlash("/api/flash", { port, mode: "full", wipe: true, name, fresh: true }, ui);
    mac = r.mac; ok = r.ok;
    if (ok) {
      await refreshPorts();
      pullFound();
      $("#no-mask").hidden = true;
      newOrbStep("form");
      // The third argument is the /api/watch skip_ap flag, NOT the orb's name. A
      // name here is truthy, and a truthy skip_ap drops Phase A — the only phase
      // that returns the boot verdict and the boot scan — so a freshly flashed
      // board answered with mode no_ip and an empty picker: "the scan produced
      // nothing, it came up with nothing" (bench, 2026-10-15). A fresh flash is
      // precisely the case that MUST run Phase A: its NVS is empty, so the
      // provisioning AP and its scan are the whole story.
      openJoinDialog(port, mac, "");
    } else {
      $("#ob-blankstate").textContent = "the flash stopped before its end — the log under the bar says why";
    }
  } finally {
    go.disabled = false; close.disabled = false;
    gateFlashButton();
  }
};


// ---- Erase flash completely: the full end of orb life this page offers ------
// One class of act lives here: the board stops being an orb. A plain chip erase
// is not offered — a blank S3 boot-loops, flaps its port, and reads as a broken
// cable. The base-image write does the same ending honestly: the board boots
// once, holds enumeration, wipes its own name, time zone and credentials, and
// shows it is waiting to be reflashed. The hub forgets the board on success and
// remembers the act, so the cable's MAC-shaped descriptor cannot rebuild the row
// (New Orb is the door back). The door is still served by /api/lonely under the
// hood; that name is internal and the page never shows it.
// ---- the reset modal ---------------------------------------------------------
// Two scopes, chosen independently, because "reset" named two different acts and the
// old card only offered the destructive one. Settings alone keeps the firmware and
// returns the board to its provisioning AP; firmware alone also happens to clear
// settings, because the base image is what re-provisions, so the two boxes are not
// four states and the modal says so rather than pretending they are.
function openResetModal() {
  const port = current.port;
  $("#rt-sub").textContent = port ? "on cable " + port : "not on a cable of this computer";
  $("#rt-choices").hidden = !port;
  $("#rt-settings").checked = true;
  $("#rt-firmware").checked = false;
  $("#rt-state").textContent = "";
  rtGate();
  $("#rt-mask").hidden = false;
}

function rtGate() {
  const go = $("#rt-go");
  go.disabled = $("#rt-choices").hidden ||
    (!$("#rt-settings").checked && !$("#rt-firmware").checked);
}

$("#mn-resetbtn").onclick = openResetModal;
$("#rt-cancel").onclick = () => { $("#rt-mask").hidden = true; };
$("#rt-mask").addEventListener("click", (e) => { if (e.target === $("#rt-mask")) $("#rt-mask").hidden = true; });
$("#rt-settings").onchange = () => {
  // Erasing the firmware takes the settings with it, so the settings box is implied
  // rather than independently uncheckable while firmware is selected.
  if ($("#rt-firmware").checked) $("#rt-settings").checked = true;
  $("#rt-settings").disabled = $("#rt-firmware").checked;
  rtGate();
};
$("#rt-firmware").onchange = () => {
  if ($("#rt-firmware").checked) $("#rt-settings").checked = true;
  $("#rt-settings").disabled = $("#rt-firmware").checked;
  rtGate();
};

$("#rt-go").onclick = async () => {
  const port = current.port, state = $("#rt-state"), go = $("#rt-go");
  if (!port || go.disabled) return;
  go.disabled = true;
  try {
    if ($("#rt-firmware").checked) {
      fpOpen();
      const { ok } = await streamFlash("/api/lonely", { port, confirm: "LONELY" }, fpUI());
      if (ok) {
        await refreshPorts();
        pullFound();
        $("#rt-mask").hidden = true;
        show("manage"); location.hash = "";
        // The row is gone and the picker calls the lead blank, which is the whole
        // true answer. The card re-rendered itself, so the note is cleared rather
        // than left holding the previous action's words.
        $("#mn-find-note").textContent = "";
      }
    } else {
      state.textContent = "clearing settings\u2026";
      const r = await hubFetch("/api/orb/cmd", {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ port, mac: current.mac, command: "reset", operand: "ALL" }),
      });
      const j = await r.json().catch(() => ({}));
      if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
      state.textContent = "settings cleared — the orb is ready to set up again";
      pullFound();
    }
  } catch (e) {
    state.textContent = e.message;
  } finally {
    go.disabled = false;
    rtGate();
  }
};




// ---- the forget dialog: three bodies, one row's current verdict --------------
// The dialog is what Forget opens, everywhere and always: no path on this card
// answers with a native browser popup any more. The body it shows is the hub's verdict
// on the row in the frame that just landed, never the frame that opened the
// dialog, so an orb unplugged with the dialog already on screen loses its cable
// endings on the next push instead of going on naming a lead the hub no longer
// sees. The three bodies:
//
//   • cable — the board sits on a lead of this hub, and deleting its row alone
//     would only hide it until the next sweep. Two endings actually end it:
//     /api/orb/cmd drives RESET ALL (firmware stays, every setting goes, the
//     board comes back as its own open provisioning AP, and the card takes the
//     default name "The Orb" when the ack lands — the dialog calls this Reset
//     settings), or /api/lonely writes the base image (the board stops being an
//     orb, wipes its name, time zone and credentials, boots, and holds its port
//     for an easy reflash later; the hub marks the chip, so it reads as a blank
//     Espressif board adoptable from New Orb — the dialog calls this Erase flash
//     completely). The /api/lonely name is internal and never surfaces in the UI.
//   • drop — no lead, and the hub will not rebuild this row. The press is the
//     plain /api/fleet/forget the popup used to gate.
//   • wait — the hub refused and has no lead to act over. The hub's own refusal
//     is the body, and there is nothing to press.
let fjRow = null;      // the <li> the dialog is acting on
let fjBusy = false;    // one action at a time, and no closing mid-write
let fjMode = "";       // the body on screen, so a repaint tells a mode change from a re-word

// forgetMode reads the row as it stands now. livePort is the hub's answer to "is this
// board on a lead of this machine at this instant", and it is the only port the cable
// body may name: the row's own port list is deliberately not consulted, so a lead that
// went quiet cannot reappear in the dialog as a stale string.
function forgetMode(li) {
  if (li._livePort) return "cable";
  if (li._forgettable) return "drop";
  return "wait";
}

// paintForgetDialog writes the whole dialog from the row's current verdicts. It runs on
// open and from paintFound on every frame that touches the row the dialog is showing —
// the reason an unplug moves an open dialog rather than leaving it as it was. A write in
// flight owns the dialog, so the paint stands down while fjBusy holds.
function paintForgetDialog() {
  const li = fjRow;
  if (!li || fjBusy) return;
  const mode = forgetMode(li);
  const shown = li._shown || li.dataset.mac || "this orb";
  const why = li._forgetWhy || "";
  const log = $("#fj-log"), note = $("#fj-note");
  if (mode !== fjMode) { log.hidden = true; log.textContent = ""; }
  fjMode = mode;
  $("#fj-title").textContent = mode === "cable" ? "What do you want to do?"
    : mode === "drop" ? "Remove this orb?" : "This orb can't be removed yet";
  $("#fj-cable").hidden = mode !== "cable";
  $("#fj-gone").hidden = mode !== "drop";
  $("#fj-wait").hidden = mode !== "wait";
  $("#fj-lonely").disabled = $("#fj-reset").disabled = mode !== "cable";
  $("#fj-forget").disabled = mode !== "drop";
  $("#fj-cancel").disabled = false;
  if (mode === "cable") {
    $("#fj-who").textContent = "This orb is plugged in and still yours to use, so hiding it from the list won't stick. Pick what to do with it instead.";
    note.textContent = "Your choice happens right away.";
    return;
  }
  if (mode === "drop") {
    $("#fj-who").textContent = shown + " isn't plugged in and hasn't been seen for a while, so this just removes it from your list.";
    $("#fj-gone-what").textContent = "Nothing is sent to the orb. It keeps its software, name and settings, and appears again the next time you plug it in or it comes online.";
    note.textContent = "You can add it back later if you change your mind.";
    return;
  }
  $("#fj-who").textContent = shown + " is connected right now, so this page waits for your next move.";
  $("#fj-wait").textContent = why || "The orb is still connected somewhere. Unplug it first, then try again.";
  note.textContent = "There's nothing to click yet. This card updates on its own, and Remove lights up when it's safe.";
}

function openForgetDialog(li) {
  if (fjBusy || !li.dataset.mac) return;   // no chip to act on; the row's own line says why
  fjRow = li;
  fjMode = "";                             // force the paint and clear the last row's log
  paintForgetDialog();
  $("#fj-mask").hidden = false;
}

function closeForgetDialog() {
  if (fjBusy) return;
  $("#fj-mask").hidden = true;
  fjRow = null;
  fjMode = "";
}
$("#fj-cancel").onclick = closeForgetDialog;
// The same three exits the join dialog answers to: the named button, the backdrop,
// and Escape. All three stand down while a write is in flight, which is what
// closeForgetDialog's own guard is for.
$("#fj-mask").addEventListener("click", (e) => { if (e.target === $("#fj-mask")) closeForgetDialog(); });
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !$("#fj-mask").hidden) closeForgetDialog();
});

// The buttons act on the press. Reaching this dialog already took two deliberate
// clicks — Forget, then the named ending — and a typed word after that punished the
// operator for the care they had just shown (bench, 2026-10-15). The port each press
// writes to is read off the row at press time rather than from what the dialog said
// when it opened: a board that left the lead mid-read gets a return, not a write to a
// socket the hub no longer holds. One press cannot re-fire while a write is in flight.
$("#fj-lonely").onclick = async () => {
  if (fjBusy || !fjRow) return;
  const port = fjRow._livePort;
  if (!port) { paintForgetDialog(); return; }   // the lead went quiet since the last frame
  fjBusy = true;
  $("#fj-lonely").disabled = true; $("#fj-reset").disabled = true;
  const log = $("#fj-log"); log.hidden = false; log.textContent = "Erasing this orb completely…\n";
  let ok = false;
  try {
    await streamPost("/api/lonely", { port, confirm: "LONELY" }, (ev) => {
      if (ev.log) log.textContent += ev.log + "\n";
      else if (ev.error) log.textContent += "\n\u2717 " + ev.error + "\n";
      else if (ev.done) { log.textContent += "\n\u2713 " + ev.done + "\n"; ok = true; }
      log.scrollTop = log.scrollHeight;
    });
  } finally {
    fjBusy = false;
    $("#fj-cancel").disabled = false;
  }
  if (ok) {
    await refreshPorts();
    pullFound();
    closeForgetDialog();
    // Nothing to add: this dialog's own log just showed the write land, and the card
    // it re-renders onto lost the row. A sentence narrating the board's screen after
    // that only repeats a fact the list already states by being empty.
    $("#mn-find-note").textContent = "";
  }
};

$("#fj-reset").onclick = async () => {
  if (fjBusy || !fjRow) return;
  const port = fjRow._livePort, mac = fjRow.dataset.mac || "";
  if (!port) { paintForgetDialog(); return; }   // the lead went quiet since the last frame
  fjBusy = true;
  $("#fj-lonely").disabled = true; $("#fj-reset").disabled = true;
  const log = $("#fj-log"); log.hidden = false; log.textContent = "Clearing the orb's settings…\n";
  let acked = false;
  try {
    await streamPost("/api/orb/cmd", { port, mac, command: "reset", operand: "ALL" }, (ev) => {
      if (ev.log) log.textContent += ev.log + "\n";
      else if (ev.error) log.textContent += "\n\u2717 " + ev.error + "\n";
      else if (ev.result) {
        acked = ev.result.ack === true;
        log.textContent += (acked ? "\n\u2713 Done — the orb is ready to set up again\n"
                                  : "\n\u2717 The orb didn't accept the reset\n");
      }
      log.scrollTop = log.scrollHeight;
    });
  } finally {
    fjBusy = false;
  }
  if (acked) {
    pullFound();
    closeForgetDialog();
    $("#mn-find-note").textContent = "Settings cleared — this orb is ready to set up again.";
  }
};

// The drop body: the row the hub will not rebuild, so the press is the plain fleet
// delete. It still re-reads the port-free verdict at press time, and it still expects a
// refusal — the door re-prices the gate on the way in, and a board that came back between
// the frame and the click is exactly the case the rule is about. A refusal stays in the
// dialog, which repaints from the row's new verdict on the way out; it does not close and
// it does not pretend.
$("#fj-forget").onclick = async () => {
  if (fjBusy || !fjRow) return;
  const li = fjRow, mac = li.dataset.mac || "";
  if (!mac || li._livePort || !li._forgettable) { paintForgetDialog(); return; }
  fjBusy = true;
  const go = $("#fj-forget"); go.disabled = true;
  const log = $("#fj-log"); log.hidden = false; log.textContent = "dropping the row for " + mac + "\n";
  let dropped = false, refusal = "";
  try {
    const r = await hubFetch("/api/fleet/forget", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mac: li.dataset.mac }),
    });
    const j = await r.json().catch(() => ({}));
    dropped = r.ok && j.ok !== false;
    if (!dropped) refusal = j.error || ("the hub refused with HTTP " + r.status);
  } catch (e) { refusal = e.message; }
  finally { fjBusy = false; }
  await pullFound();                       // the row leaves the list, and that closes this
  if (dropped) {
    if (fjRow === li) closeForgetDialog();
    $("#mn-find-note").textContent = "Removed from your list. The orb itself wasn't changed.";
    return;
  }
  log.textContent += "\n\u2717 " + refusal + "\n";
  if (fjRow === li) { paintForgetDialog(); $("#fj-note").textContent = "\u2717 the hub refused this press"; }
};


// ---- post-flash onboarding: the join dialog ----------------------------------
// The firmware decides the branch, not the UI and not the flash mode: a flash
// that leaves stored NVS lets the orb self-join in ~2.5 s, while a fresh or
// wiped orb opens its own open provisioning AP the moment boot finds NVS empty.
// The flash session reads the boot console while it still owns the port and
// records that verdict; /api/watch consumes it. One dialog, one set of outcomes
// — the name already reached the hub inside the flash, so the converged path
// after ANY flash (fresh or reflash) is this frame.
let orbIP = "";
let provPort = ""; // the serial port the wired-provision door writes to
let provMac = "";  // the flashed orb's MAC, the key back to its boot verdict
let provApSsid = ""; // the provisioning AP the boot verdict named, for the AP line
let zoneList = null;
// Watch is a read stream, so a closed dialog may drop its events but never the
// hub's view of the port: a generation counter, not an aborted fetch (the one
// abort allowed in this file belongs to the discovery watchdog). Stale events
// land on a frame the hub can no longer answer, and the guard drops them at
// the door — the hub-side tail ends on its own once the boot verdict lands.
let watchGen = 0;

function watchLog(text) {
  const el = $("#ob-console");
  el.textContent += text + "\n";
  el.scrollTop = el.scrollHeight;
  // Mirror the newest line onto the lock while the lock is up. The locked page
  // carries a spinner and one hardcoded sentence, and that sentence stays true
  // for a minute at a time — so a live wait behind it looked like a hang. The
  // newest hub line is the difference between "waiting" and "apparently stuck".
  const lock = $("#wj-lock");
  if (!lock.hidden) $("#wj-lock-msg").textContent = text;
}

// The sentence the markup ships with, taken once at load. watchLog mirrors each
// hub line onto the lock, which replaces it, and the next join must not inherit
// the last join's last word.
const lockIntro = $("#wj-lock-msg").textContent;

// openJoinDialog is the one destination for every converged flash path. SkipAp
// carries the /api/watch skip flag: after a provision the boot already showed a
// PROV-ACK, and re-announcing that open AP to a joining orb wastes the wait.
function openJoinDialog(port, mac, skipAp) {
  provPort = port;
  provMac = mac || "";
  orbIP = "";
  const gen = ++watchGen;
  $("#ob-watch").textContent = "waiting for the orb to boot…";
  $("#ob-console").textContent = "";
  $("#wj-consoledetails").hidden = false;
  provApSsid = "";
  $("#sp-manual").value = "";
  $("#wj-apmsg").hidden = true;
  $("#ob-done").hidden = true;
  $("#sp-log").textContent = "";
  $("#sp-log").hidden = true;
  showTzRow(false);
  joinLock(true);
  $("#wj-lock-msg").textContent = lockIntro;
  $("#wj-mask").hidden = false;
  loadZones();
  streamWatch(gen, port, mac, skipAp, "");
}

// joinLock is the boot phase of the join dialog. Until the orb's own scan is in
// there is nothing honest to show — an empty picker is the bug this replaces —
// so the dialog carries a spinner instead of a form, and the spinner is gone
// the moment the form has an answer. The wait keeps its exits: backdrop click
// and Escape, since Cancel lives inside the form.
function joinLock(on, msg) {
  $("#wj-lock").hidden = !on;
  $("#ob-join").hidden = on;
  if (!on) showTzRow(true);
  if (on && msg) $("#wj-lock-msg").textContent = msg;
}

function closeJoinDialog() {
  // Invalidate first: an answer arriving after the close belongs to a frame
  // nobody is looking at, and the orb keeps booting without an audience.
  watchGen++;
  $("#wj-mask").hidden = true;
}

async function streamWatch(gen, port, mac, skipAp, ip) {
  await streamPost("/api/watch", { port, mac, skip_ap: !!skipAp, ip: ip || "" }, (ev) => {
    if (gen !== watchGen) return;
    if (ev.log) watchLog(ev.log);
    else if (ev.error) {
      // A watch that cannot answer is still an answer: the orb is on its own
      // AP or off the cable. The dropdown still carries its typed-name and
      // AP-mode answers, so unlocking onto one never shows a dead picker.
      $("#ob-watch").textContent = ev.error;
      populateSerialNetworks([]);
      joinLock(false);
    }
    else if (ev.result) onWatchResult(ev.result);
  });
}

function onWatchResult(res) {
  joinLock(false);
  const watch = $("#ob-watch");
  if (res.mode === "lan") {
    if (res.status && res.status.state === "connected") {
      watch.textContent = "orb is on the LAN at " + res.ip + ", already connected.";
      joinDone(res.ip, res.status);
    } else {
      // Reachable but NOT connected — wrong passphrase after a router change —
      // is the case the form is for: wired door, boot-scan list, no AP hop.
      populateSerialNetworks(res.networks || []);
      watch.textContent = "orb answered at " + res.ip +
        " but is not on Wi-Fi (state: " + (res.status ? res.status.state : "unknown") + ") — the form writes over the cable.";
    }
  } else if (res.mode === "serial") {
    populateSerialNetworks(res.networks || []);
    watch.textContent = "the boot scan read " + (res.networks || []).length +
      " network" + ((res.networks || []).length === 1 ? "" : "s") + " off the console — wired door, the form writes over the cable.";
    if (!(res.networks || []).length) serialRescan(true); // the boot window closed before the scan was read
  } else if (res.mode === "ap") {
    provApSsid = res.ap_ssid || "";
    populateSerialNetworks(res.networks || []);
    watch.textContent = "orb opened its own provisioning AP — the hub can still write over the cable.";
    if (!(res.networks || []).length) serialRescan(true); // the flash missed the boot rows; drive a fresh scan now
  } else {
    // no_ip / unknown: the hub has no address for it. With a cable bound that is
    // never a dead end — the serial door answers in every state, including
    // joined-and-hidden — so re-drive the scan rather than leaving the picker
    // showing whatever the last round happened to leave, and say what the cable
    // can still do instead of pointing only at an address nobody has.
    if (provPort) {
      watch.textContent = "no address yet: " + (res.reason || "it has not announced itself") +
        " — the cable is still a door; Rescan re-reads its network list and the form writes over the wire.";
      populateSerialNetworks([]);
      serialRescan(true);
    } else {
      watch.textContent = "orb did not announce itself: " +
        (res.reason || "it is flashed and holding — add it by address once you know where it lives") + ".";
    }
  }
}

// serialRescan drives the firmware's serial SCAN verb over the bound cable: the
// orb runs its radio again, mirrors PROVSCAN rows to the console, and closes
// with PROVSCAN-END (the hub's own wait). It works in every state — fresh
// provisioning or joined-and-hidden — because the command door is armed for the
// life of the app. `auto` marks the silent retry the boot phase triggers when
// the flash captured no rows; a manual press always reports out loud.
let provScanBusy = false;
async function serialRescan(auto) {
  if (!provPort || provScanBusy) return;
  provScanBusy = true;
  const btn = $("#sp-rescan");
  const spin = $("#sp-scanspin");
  const gen = watchGen;
  btn.disabled = true;
  spin.hidden = false;
  try {
    await streamPost("/api/orb/cmd", { port: provPort, command: "scan" }, (ev) => {
      if (gen !== watchGen) return;
      if (ev.log) watchLog(ev.log);
      else if (ev.error) {
        const jlog = $("#sp-log");
        jlog.hidden = false;
        jlog.textContent += "rescan: " + ev.error + "\n";
        if (!auto) $("#ob-watch").textContent = "the rescan did not complete — the console says why below; the typed SSID still works.";
      }
      else if (ev.result) {
        const nets = ev.result.networks || [];
        populateSerialNetworks(nets);
        $("#ob-watch").textContent = "the orb's own scan read " + nets.length +
          " network" + (nets.length === 1 ? "" : "s") + " over the cable.";
      }
    });
  } finally {
    provScanBusy = false;
    btn.disabled = false;
    spin.hidden = true;
  }
}
$("#sp-rescan").onclick = () => serialRescan(false);

function joinDone(ip, st) {
  orbIP = ip;
  $("#ob-done-msg").textContent =
    "connected at " + ip + " — " + (st.rssi || "?") + " dBm, fw " + (st.fw || "?") +
    (st.tz ? ", " + st.tz : "") + ".";
  $("#ob-join").hidden = true;
  $("#ob-done").hidden = false;
  pullFound();
}

$("#ob-rejoin").onclick = () => {
  $("#ob-done").hidden = true;
  $("#ob-join").hidden = false;
  $("#ob-watch").textContent = "pick a network and connect again — the form writes over the cable.";
};
$("#ob-manage").onclick = () => { closeJoinDialog(); $("#ob-flashpanel").hidden = true; openOrbByIP(orbIP, provMac, null); };
// The third answer on a finished flash: stay here. A cable flash that worked and
// joined is a completed job, and the two buttons beside this one both move the
// operator somewhere — onto the orb's page, or back into the network form. The
// wait and the form keep their backdrop and Escape exits; the done panel covered
// the whole dialog in buttons, so closing it had to be one of them.
$("#ob-close").onclick = () => { closeJoinDialog(); $("#ob-flashpanel").hidden = true; pullFound(); };

// populateSerialNetworks fills the wired-provision door's scan list and the
// two standing answers that ride beside it in the same dropdown: a typed name
// and AP mode. A scan that saw nothing still offers both ways out, so the
// picker is never the dead end it once was. Firmware sorts strong→weak, so the
// first real network is the best one.
function populateSerialNetworks(nets) {
  const sel = $("#sp-ssid");
  const prev = sel.value;
  sel.innerHTML = "";
  nets.forEach((n) => {
    const o = document.createElement("option");
    o.value = n.ssid;
    o.textContent = n.ssid + "  (ch " + n.ch + ", " + n.rssi + " dBm" + (n.open ? ", open" : "") + ")";
    sel.appendChild(o);
  });
  const manual = document.createElement("option");
  manual.value = "__manual__";
  manual.textContent = "Other — type the network name";
  sel.appendChild(manual);
  const ap = document.createElement("option");
  ap.value = "__ap__";
  ap.textContent = "No Wi-Fi — run on the orb's own AP";
  sel.appendChild(ap);
  if (nets.some((n) => n.ssid === prev)) sel.value = prev;
  else if (nets.length) sel.selectedIndex = 0;
  else sel.value = "__manual__";
  applySsidChoice();
}

// applySsidChoice draws the consequences of the dropdown's current answer:
// the typed box only under "Other", the one-line AP door only under AP mode.
function applySsidChoice() {
  const v = $("#sp-ssid").value;
  $("#sp-manual").hidden = v !== "__manual__";
  const msg = $("#wj-apmsg");
  msg.hidden = v !== "__ap__";
  if (v === "__ap__") {
    msg.textContent = "The orb answers on its own open access point" +
      (provApSsid ? " " + provApSsid : "") +
      " — connect to it with your phone and configure the orb there, at http://192.168.4.1/.";
  }
}
$("#sp-ssid").onchange = applySsidChoice;

// ---- the time zone the join form writes ---------------------------------------
// The list is the firmware's OWN table, served by /api/tz from the same source
// the parity test pins: an orb that knows every name it can be given cannot be
// told a name it will reject. Each row arrives as {name, posix, iana, label};
// the picker shows the city, posts the device key, and hands the IANA name to
// the browser when it is asked where it is. UTC is not a guess at home; the
// browser may know better, and when it does the form says so in one line.
async function loadZones() {
  const sel = $("#sp-tz");
  const keep = sel.value;
  try {
    const r = await hubFetch("/api/tz");
    const d = await readDeviceJSON(r);
    const rows = (d && d.ok && d.data && d.data.zones) || [];
    if (!rows.length) throw new Error("empty zone table");
    zoneList = rows;
    sel.innerHTML = "";
    for (const z of zoneList) {
      const o = document.createElement("option");
      o.value = z.name;
      o.textContent = (z.label && z.label !== z.name) ? z.label + "  (" + z.name + ")" : z.name;
      sel.appendChild(o);
    }
    sel.value = zoneList.some((z) => z.name === keep) ? keep : "UTC";
    $("#sp-tzstate").textContent = "";
  } catch (e) {
    zoneList = null;
    sel.innerHTML = "<option>UTC</option>";
    $("#sp-tzstate").textContent = "the orb's own zone list did not load — UTC and a typed name are all this form trusts";
  }
}

// zoneOffsetMin asks the browser's own IANA data what a zone's UTC offset is,
// in minutes, at the given instant (default now). longOffset gives
// "GMT+05:30"; plain "GMT" is zero.
function zoneOffsetMin(iana, at) {
  try {
    const part = new Intl.DateTimeFormat("en-US", { timeZone: iana, timeZoneName: "longOffset" })
      .formatToParts(at || new Date()).find((p) => p.type === "timeZoneName");
    if (!part) return null;
    const m = /^GMT(?:([+-])(\d{2})(?::(\d{2}))?)?$/.exec(part.value);
    if (!m) return null;
    if (!m[1]) return 0;
    return (m[1] === "-" ? -1 : 1) * (parseInt(m[2], 10) * 60 + (m[3] ? parseInt(m[3], 10) : 0));
  } catch (e) {
    return null; // a browser without that zone (rare; these are all real ones)
  }
}

// zoneFromLocation names the orb-table row that best covers a longitude. The
// target is the solar offset (longitude over 15, snapped to the 15-minute
// grid); a row's score is the smaller of its distance at the January offset
// and at the July offset — the browser's own IANA data, read in both
// hemispheres' calm, so a daylight line and its winter twin compete on the
// zone's own rule and not on the accident of today's date. Within 90 minutes
// it is a match; beyond that the form would be naming a stranger's clock, and
// it says so instead.
function zoneFromLocation(longitude) {
  if (!zoneList) return null;
  const solarMin = Math.round(longitude / 3.75) * 15; // minutes east of UTC, on the 15-minute grid
  const jan = new Date(Date.UTC(2026, 0, 15));
  const jul = new Date(Date.UTC(2026, 6, 15));
  let best = null, bestD = 91;
  for (const z of zoneList) {
    if (!z.iana) continue;
    const oJan = zoneOffsetMin(z.iana, jan);
    const oJul = zoneOffsetMin(z.iana, jul);
    if (oJan === null && oJul === null) continue;
    const ds = [oJan, oJul].filter((o) => o !== null)
      .map((o) => Math.abs(o - solarMin)).sort((a, b) => a - b);
    if (ds[0] < bestD) { bestD = ds[0]; best = z; }
  }
  return best;
}

// The join dialog's own doors, wired here rather than inline: Cancel abandons
// this orb to its open AP — reachable at 192.168.4.1 from any device that
// joins it — and the next flash reopens this same frame for it or for any
// other. The boot wait keeps its exits through the backdrop and Escape.
$("#wj-cancel").onclick = closeJoinDialog;
$("#wj-mask").addEventListener("click", (e) => { if (e.target === $("#wj-mask")) closeJoinDialog(); });
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !$("#wj-mask").hidden) closeJoinDialog();
});

// showTzRow is one lever, pulled by joinLock. The zone row is a join field
// like the other two: a row that hid itself whenever the scan came back empty
// is how this form once read as "the picker is dead".
function showTzRow(on) { $("#sp-tzrow").hidden = !on; }

$("#sp-go").onclick = async () => {
  const jlog = $("#sp-log");
  jlog.hidden = false;
  const picked = $("#sp-ssid").value;
  if (picked === "__ap__") { applySsidChoice(); return; } // the option's own line is the answer
  const ssid = (picked === "__manual__" ? $("#sp-manual").value.trim() : picked).trim();
  if (!ssid) { jlog.textContent = "pick a network or type its name first"; return; }
  if (!provPort) { jlog.textContent = "no bound serial port — flash first"; return; }
  const tz = $("#sp-tz").value.trim() || "UTC";
  const go = $("#sp-go");
  go.disabled = true;
  jlog.textContent = "";
  try {
    await streamPost("/api/provision", { port: provPort, ssid, pass: $("#sp-pass").value, tz }, (ev) => {
      if (ev.log) watchLog(ev.log);
      else if (ev.error) jlog.textContent += "error: " + ev.error + "\n";
      else if (ev.result) {
        const n = ev.result;
        if (n.ack && n.ip) {
          jlog.textContent += `accepted "${ssid}" — joined, lease ${n.ip} read over USB.\n`;
        } else {
          jlog.textContent += (n.ack
            ? `accepted "${ssid}" — the orb is joining; watch for it on the LAN.`
            : `no PROV-ACK for "${ssid}" — check the passphrase, or the orb may already be configured (use its AP page).`) + "\n";
        }
        if (n.ack) streamWatch(watchGen, provPort, provMac, true, n.ip || ""); // confirm the USB lease, else re-probe the LAN
      }
      jlog.scrollTop = jlog.scrollHeight;
    });
  } finally {
    go.disabled = false;
  }
};

$("#sp-geo").onclick = () => {
  const state = $("#sp-tzstate");
  if (!navigator.geolocation) { state.textContent = "this browser does not do location — pick the zone"; return; }
  state.textContent = "asking the browser for your location…";
  navigator.geolocation.getCurrentPosition((p) => {
    const z = zoneFromLocation(p.coords.longitude);
    if (z) {
      $("#sp-tz").value = z.name;
      state.textContent = "location says " + (z.label || z.name) + " (" + z.name + ") — fix it if that is wrong";
    } else {
      state.textContent = "no zone in the orb's own list covers that position — pick one";
    }
  }, (e) => {
    state.textContent = "no location (" + (e.code === 1 ? "not allowed" : "no fix") + ") — pick the zone";
  }, { timeout: 8000, maximumAge: 600000 });
};

// ---- passphrase reveal ---------------------------------------------------------
// Every password field on this page gets a show/hide: the join form's whole
// failure mode is one mistyped character, and the one that costs a re-provision
// is the one nobody can see. Shared wiring, identical behaviour on both fields.
for (const pair of [["#sp-pass", "#sp-pass-show"], ["#j-pass", "#j-pass-show"]]) {
  const inp = $(pair[0]), btn = $(pair[1]);
  if (!inp || !btn) continue;
  btn.onclick = () => {
    const show = inp.type === "password";
    inp.type = show ? "text" : "password";
    btn.textContent = show ? "hide" : "show";
    btn.setAttribute("aria-label", (show ? "Hide" : "Show") + " the passphrase");
    inp.focus();
  };
}

$("#j-rescan").onclick = () => loadNetworks(true);

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
        // This door serves the join dialog when it is the one that knocked, and
        // the orb view's own card otherwise; the branch reads the open frame,
        // not the caller, so both roads land on the same sentence.
        if (!$("#wj-mask").hidden) joinDone(ip, s);
        orbIP = ip;
        pullFound();
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
  const raw = Array.isArray(d.orbs) ? d.orbs : [];
  const orbs = raw.filter((o) => !isBlankRow(o));
  // A board the hub can see on a lead and cannot prove is an orb is not a card, and
  // it is not nothing either. "None on a cable" while one sits there is the sentence
  // that sends an operator to wiggle the wire instead of reading the row's refusal.
  const unproven = raw.filter((o) => isBlankRow(o) && o && o.onCable).length;
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
    if (!live.has(entry[0])) {
      // The row left the hub's view — forgotten, aged out, or never a board at all — so
      // the dialog has no subject left to act on and closes with it.
      if (entry[1] === fjRow) closeForgetDialog();
      entry[1].remove(); foundRows.delete(entry[0]);
    }
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
      : unproven
        // On the lead and silent, so not a card: the chip did not answer the one
        // question that costs it nothing. The line under the empty list names the
        // door that fits a board running no orb, which is the New Orb flash.
        ? unproven + " board" + (unproven === 1 ? "" : "s") + " on a cable, silent when asked: none announced, and none proven an orb"
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
      // Every fact this card renders came off that stream, including the one the forget
      // dialog is acting on. A dialog over a fleet it can no longer see is a button with
      // no hub behind it, so it goes with the rows.
      closeForgetDialog();
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
// controls are wired to the row's mac rather than to a snapshot of the packet, so a frame
// that lands while the operator is reading the list cannot strand a handler on a stale
// address, and the patch has somewhere to put a new age without rebuilding the row under
// the eyes of whoever is on the page.
function foundRow(o) {
  const li = document.createElement("li");
  li.className = "found";
  li.dataset.key = o.key || o.mac;
  li.dataset.mac = o.mac || "";

  // The tile IS the door, so the tile carries the name and the hover. The button holds the
  // title line and is what a keyboard reaches; the row's own click listener below is what
  // makes a press anywhere on the tile open the orb.
  const label = document.createElement("button");
  label.className = "ghost found-pick";
  const title = document.createElement("span");
  title.className = "found-name";
  const upd = document.createElement("span");
  upd.className = "tag update"; upd.textContent = "update"; upd.hidden = true;
  upd.title = "this hub carries newer firmware than this orb runs — open it to load";
  label.append(title, upd);

  // What the board is, in one muted line under the name: where to reach it, its mac,
  // what it runs, how fresh the fact is, and the exceptions that change what a press
  // does. Where the hub LEARNED each field is deliberately absent: the tile answers
  // the operator's question, it does not audit itself.
  const detail = document.createElement("span");
  detail.className = "found-detail";

  const badge = document.createElement("span");
  badge.className = "found-status";

  // There is no name field on a tile. The board owns its word: a rename goes onto the
  // device, and the hub only keeps a copy of what the device agreed to. The editing door
  // is the orb's own network page, and a box here could only repeat the title above it.

  // The tile is the door, so the row takes the press wherever it lands inside it. A click
  // on one of the row's own controls is that control's and does not open anything, and the
  // title button above stays the keyboard handle for the same action. The paint writes
  // li._open with the address of the frame that produced it, so a row that has not been
  // painted yet has no door to open and says so by doing nothing.
  li.addEventListener("click", (e) => {
    if (e.target.closest("button")) return;
    // A drag that ends inside the tile arrives here as a click too, and an operator who
    // just selected a MAC to copy it did not press the door.
    const sel = document.getSelection && document.getSelection();
    if (sel && String(sel).length) return;
    if (li._open) li._open();
  });

  const forget = document.createElement("button");
  forget.className = "ghost"; forget.textContent = "Forget";
  // Forget is the one removal this list offers: the hub remembers every board it has
  // seen and never needs a press to add one. The press never answers with a browser
  // popup — it opens the page's own forget dialog, which says what the hub will accept
  // for THIS row and carries the action. Dropping a row costs the name the operator
  // chose and revokes any address the hub was willing to dial, so the dialog names both
  // before the press, and a row the hub will rebuild says that instead of a button.
  forget.onclick = () => {
    if (forget.disabled) return;   // the hub said no; the row's own line says what to do
    openForgetDialog(li);
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
  ident.append(forget, identify, fwhy);
  li.append(label, detail, badge, ident);
  li._label = label; li._badge = badge; li._title = title; li._detail = detail;
  li._update = upd;
  li._forget = forget; li._identify = identify; li._fwhy = fwhy;
  return li;
}

// statusBadge spells the one verdict the tile makes about the present tense: is
// this board reachable right now, by the hub's own accounting. It deliberately says
// nothing about WHERE the hub's facts came from — announce, dial, console or cable
// descriptor are the hub's business, not the operator's, and the row that recited
// them was the tile arguing its provenance instead of answering the one question a
// fleet list is for. The port rides along on a lead row because it names where the
// cable actions run, which is an action, not a source.
function statusBadge(o) {
  if (o.connected !== true) {
    if (o.onCable) {
      const where = o.ports && o.ports.length ? " (" + o.ports.join(", ") + ")" : "";
      return "on cable" + where;
    }
    return o.reachable === "unknown" ? "asking" : "disconnected";
  }
  if (o.onCable && (!Array.isArray(o.via) || o.via.indexOf("lan") < 0)) {
    const where = o.ports && o.ports.length ? " (" + o.ports.join(", ") + ")" : "";
    return "on cable" + where;
  }
  return "connected";
}

// paintFound writes what the hub currently believes onto a row that already exists. It runs on
// every frame, so it patches text rather than rebuilding the row: a rebuild would drop the
// focus of whoever has a tile's title button under the cursor, and a frame whose address moved
// cannot be allowed to leave the old one behind in the confirm text.
function paintFound(li, o) {
  const label = li._label;
  const softAP = !!o.provisioning && String(o.ip).indexOf("192.168.4.") === 0;
  const shown = o.name || o.instance || o.mac || o.key || "?";
  const addr = o.ip ? o.ip + ":" + o.port : "no address";
  li.dataset.mac = o.mac || "";
  li._port = (o.ports && o.ports.length) ? o.ports[0] : "";
  // Every text write on this row is conditional. The frame arrives on whichever packet
  // moved, which for a fleet at rest means every row is repainted with the words it already
  // carried, and a textContent assignment kills a text selection wherever it lands. An
  // operator copying a MAC out of a tile to paste it somewhere should be able to finish the
  // copy before the next advertisement wipes it.
  if (li._shown !== shown) {
    li._shown = shown;
    li._title.textContent = shown;
  }
  const detailText = [addr, o.mac ? "mac " + o.mac : "no mac", o.fw ? "fw " + o.fw : "",
    typeof o.ageS === "number" ? o.ageS + "s" : "",
    // Only the exceptions ride this line. The orb's own TXT state is "normal" whenever it
    // is not provisioning (main/net/orb_mdns.c sends one or the other), so a token every
    // row carries is noise; provisioning, a contested address and a shared name are the
    // three that change what a press on this tile does. Whether the hub's word for the
    // address came from a packet, a dial or a cable is not on this list at all and not
    // on the tile anywhere: it is the hub's accounting, and the operator's one question
    // is answered by the status badge above it.
    o.state && o.state !== "normal" ? o.state.toUpperCase() : "",
    o.contended ? "CONTESTED" : "",
    o.shared ? "shared name" : ""].filter(Boolean).join("  \u00b7  ");
  if (li._detailText !== detailText) {
    li._detailText = detailText;
    li._detail.textContent = detailText;
  }
  const statusText = statusBadge(o);
  if (li._statusText !== statusText) {
    li._statusText = statusText;
    li._badge.textContent = statusText;
    li._badge.classList.toggle("ok", o.connected === true);
    li._badge.classList.toggle("bad", o.connected !== true && o.reachable !== "unknown");
  }
  // The verdict is the hub’s, and read fail-closed like Forget’s: the field is
  // pinned non-omitempty server-side precisely so an absent one means no.
  li._update.hidden = o.update !== true;
  // Four separate reasons a row cannot be dialed, and the button says which one it is rather
  // than letting a click discover it: the board has no address at all, its address is a SoftAP
  // the hub cannot route to, two boards both claim it, or it was never allowed.
  label.disabled = !!o.contended;   // a cable row opens too: its cable doors are the manage state
  // The tile is the click target, so the row itself carries whether a press does anything.
  li.classList.toggle("noclick", !!o.contended);
  label.title = !o.ip ? (o.refusal || "opens over the cable it is on")
    : softAP ? "reachable only from its own network; the hub cannot dial a SoftAP address"
    : o.contended ? "two boards both announce this address, so the hub will not dial either"
    : (o.refusal || (o.dialable ? "already an address this hub dials"
      : "fills the Orb IP box; the hub dials it only once you load it"));
  // The frame carries the address, so the row's door is rewritten with it: a handler left
  // on the snapshot the row was built from dials whatever lease the board held then.
  li._open = () => { openOrbByIP(o.ip || "", o.mac, o); };
  label.onclick = () => { li._open(); };
  // Identity is what the naming controls key on. A cable with no mac has no row anywhere to
  // write a name against, so the Forget control goes dark and the Identify button takes its
  // place; the tile stays a tile.
  const nameable = !!o.mac;
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
  const forgettable = nameable && o.forgettable === true;
  const liveForget = nameable && o.liveForget === true;
  li._forgettable = forgettable;
  li._liveForget = liveForget;
  li._livePort = typeof o.livePort === "string" ? o.livePort : "";
  li._forgetWhy = why;
  // A forgettable row drops itself from the dialog's drop body; a refused row opens the
  // body that says why. Only a row with neither — no mac, or a hub that sent no verdict
  // at all — is a dead button, and it says that too.
  forget.disabled = !(forgettable || liveForget);
  forget.title = !nameable ? "Nothing to remove yet — this orb hasn't finished setting up."
    : forgettable ? "Remove this orb from the list. Nothing is sent to it."
    : liveForget ? (li._livePort
        ? "This orb is plugged in, so choose Reset settings or Erase flash completely."
        : "This orb is still connected, so it can't be removed yet. Open Forget to see why.")
    : (why || "This orb can't be removed right now.");
  // The reason goes on the row as well as in the title. A greyed button with no text is a
  // fault to diagnose rather than a rule to learn, and the operator should not have to
  // hover every row to find out which of the three facts is holding the button back.
  const fwhy = li._fwhy;
  fwhy.hidden = forgettable || liveForget || !nameable;
  fwhy.textContent = !nameable ? "Nothing to remove yet — this orb hasn't finished setting up."
    : (forgettable || liveForget) ? ""
    : (why || "This orb can't be removed right now. Reload the page.");
  li._identify.hidden = o.identity !== "unknown";
  li._ip = o.ip;
  // The dialog reads its row, so the row's new verdict is the dialog's new body. This is
  // the whole of the unplug fix: a board that left the lead while its dialog was open
  // loses the cable endings on the frame that noticed, rather than keeping a promise
  // about a port the hub no longer holds.
  if (fjRow === li && !fjBusy) paintForgetDialog();
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
  use.textContent = "Store on the dial";
  use.disabled = !t.ok || t.active;
  // Store is the firmware's FIRST press; the door answers once the copy is queued,
  // seconds before the row's slot witness exists. So the press is not over when the
  // POST returns: watchThemeJob follows the row to its outcome, and the state line
  // says running, stored, or refused — never a silent no-op.
  use.onclick = async () => {
    use.disabled = true;
    state.textContent = "storing " + t.name + " \u2014 queued; the copy takes a few seconds";
    try {
      await themePost("/api/theme/install", { ip: themeIP, id: t.id });
      await watchThemeJob(state, t, "storing");
    } catch (e) { state.textContent = "store refused: " + e.message; }
    use.disabled = false;
  };
  li.appendChild(use);
  // The second press of the two-stage form: the flip, offered on exactly the rows the
  // firmware offers it — one whose stored copy holds a slot and which is not already
  // on the glass. The flip reboots the orb, so the wait is for the device to come
  // back with the new face drawn.
  if (t.ok && t.slot && !t.active) {
    const flip = document.createElement("button");
    flip.className = "ghost";
    flip.textContent = "Put it on the dial";
    flip.onclick = async () => {
      flip.disabled = true;
      state.textContent = "flipping " + t.name + " \u2014 the orb reboots onto it";
      try {
        await themePost("/api/theme/flip", { ip: themeIP, id: t.id });
        await watchThemeJob(state, t, "flipping");
      } catch (e) { state.textContent = "flip refused: " + e.message; }
      flip.disabled = false;
    };
    li.appendChild(flip);
  }
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

// watchThemeJob follows one row after a store or a flip, from the queue ack to the
// witness the worker leaves behind. The store door and the flip door both answer as
// soon as the job is queued; the device then spends seconds copying (store) or goes
// away for a reboot (flip). A press without this follow-up is the silent button this
// card shipped with: the row never changed on screen and the operator could not tell
// a running copy from a dead button. The outcome always names the firmware's own
// reason: the row's slot for a stored theme, the list's try witness for a refusal,
// the drawn name for a flip, and a re-read prompt when the orb stays quiet past two
// minutes.
async function watchThemeJob(state, t, kind) {
  const busy = kind === "storing" ? "storing " : "flipping ";
  for (let n = 0; n < 100; n++) {
    await new Promise((res) => setTimeout(res, n < 60 ? 1200 : 3000));
    let d;
    try { d = await themePost("/api/theme/list", { ip: themeIP }); }
    catch (e) { state.textContent = busy + t.name + " \u2014 the orb is mid-reboot; waiting"; continue; }
    if (d.job_busy) { state.textContent = busy + t.name + " \u2014 the copy is running, " + n + " s"; continue; }
    if (d.try_id === t.id && d.try_why) {
      state.textContent = "could not " + (kind === "storing" ? "store" : "flip") + " " + t.name + ": " + d.try_why;
      return;
    }
    const row = (d.themes || []).find((x) => x.id === t.id);
    if (kind === "storing" && row && row.slot) {
      state.textContent = "stored " + t.name + " in slot " + row.slot + " \u2014 press Put it on the dial";
      await loadThemes(themeIP);
      return;
    }
    if (kind === "flipping" && d.active === t.name) {
      state.textContent = "the dial is drawing " + t.name;
      await loadThemes(themeIP);
      return;
    }
    state.textContent = busy + t.name + " finished without its witness \u2014 the rows were re-read";
    await loadThemes(themeIP);
    return;
  }
  state.textContent = busy + t.name + " still going after two minutes \u2014 re-read the card";
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
