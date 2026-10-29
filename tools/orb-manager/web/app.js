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
    // Intercept only a control naming a real in-page view: cancelling the
    // navigation of anything else is how a real link gets broken. The header once
    // carried a global Theme Editor link here; D37 retired it, because the editor
    // belongs to an app and opens inside the Themes pane, not beside the fleet.
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
  // A board that answers its own network door has said the thing a reboot is bought to
  // hear, for one GET and no blank glass. It is not a probe and not a PING: the version
  // is the board's own quote, and the build stamp and ELF digest are facts this door
  // does not carry, so they are not printed as blanks. It sits above the cable branch
  // because a door answer outranks a console echo and must not be labelled "not
  // identified" — and it stays below the probe branch, which knows more.
  if (entry.lanProof && entry.isOrb) {
    return p + "  \u2014  " + (entry.usbMac || entry.mac || "orb") + "  \u00b7  " +
      (entry.version || "an orb") + ", answering on the LAN";
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
  // Idle wording on every open, whatever the last upload left behind.
  ffUploadOpen = false;
  ffCancelLabel();
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
  if (current.port) {
    // The channel beats the radio choice: a board on a lead is written by the
    // cable no matter which source is picked, and a cable write runs esptool
    // INSTEAD of the app — the glass cannot report a transfer the firmware is
    // not executing. Say so here, where the operator can still unplug.
    el.textContent = "Written over the cable: bootloader, partition table and app slots. " +
      "The dial shows nothing through a cable write: the chip is in its bootloader, and the updating overlay lives in the firmware the cable overwrites.";
    return;
  }
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
  // The button names the channel before the press, not after. A board on a
  // lead takes the cable whatever else the dialog says; the label says so.
  // ffForced keeps its own wording ("Flash anyway over Wi-Fi"), set by the
  // downgrade branch of wifiUpdate and untouched by the ffGate that the
  // press's finally block runs afterwards.
  if (!ffForced) go.textContent = current.port ? "Flash over cable" : "Update over Wi-Fi";
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
// ffDismiss leaves the flash dialog. It stops nothing. The hub has no abort door
// for a Wi-Fi push (only the cable's /api/flash/cancel exists), so while bytes are
// in flight this button says Close: a label spelling Cancel would promise a stop
// nobody can perform, and the upload runs to its end either way.
let ffUploadOpen = false;
function ffDismiss() { $("#ff-mask").hidden = true; }
function ffCancelLabel() { $("#ff-cancel").textContent = ffUploadOpen ? "Close" : "Cancel"; }
$("#ff-cancel").onclick = ffDismiss;
$("#ff-mask").addEventListener("click", (e) => { if (e.target === $("#ff-mask")) ffDismiss(); });
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

// The manage view's own flash frame: a dialog centered on the page mask for as
// long as a board is being written. fpOpen shows it the instant a door there is
// pressed; fpClose hides it once the work is over. Close hides the panel alone:
// the hub owns the wire, and a flash keeps going whether anyone is watching.
function fpUI() {
  return { log: $("#ob-log"), bar: $("#ob-bar > div"), doing: $("#ob-doing"),
           op: $("#ob-op"), live: $("#ob-liveop"), cancel: $("#ob-cancel") };
}
function fpOpen() { $("#ob-flashmask").hidden = false; }
function fpClose() { $("#ob-flashmask").hidden = true; }
$("#fp-close").onclick = fpClose;

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

// paintOrbHead draws the card's headline: the name the board answers to, and only
// while nothing has said it, the most specific address on hand. It exists as a
// function because the name arrives late — a board opened straight from a flash
// has no row to read a name off, so the first paint is an address and the truth
// walks in with the status reply. Every door that learns the name paints through
// here (openOrbByIP, loadOrb, the Orb face's Save), which is what keeps the
// headline from going stale on a mac while the name field below it already
// reads "Kitchen" (bench 2026-10-27, the flash-to-manage path's one rough edge).
function paintOrbHead() {
  const el = $("#mn-orbname");
  const inst = (current.row && current.row.instance) || "";
  if (el) el.textContent = current.name || inst || current.mac || current.ip || "orb";
}

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
  paintOrbHead();                           // an address until the board says its name
  $("#mn-orbident").textContent = [current.mac && ("mac " + current.mac), current.ip && ("at " + current.ip),
    current.port && ("on cable " + current.port)].filter(Boolean).join("  \u00b7  ");
  $("#mn-name").value = current.name;
  $('#ob-identify').hidden = !current.port;
  gateFwButtons();
  $("#mn-curfw").textContent = "loading\u2026";
  $("#mn-verdict").textContent = "";
  $("#ob-idcard").hidden = true;
  renderIdentity(allPorts);
  show("orb");
  // Dormancy: opening a board resets the faces (nothing carries over from the
  // last orb) and loads none of them beyond the status read loadOrb makes.
  // The wifi picker starts empty; Rescan is what fills it, on this board, now.
  // The pending scan retry goes with it: a timer armed for the last board would
  // paint THIS board's picker without the operator having opened the face.
  clearTimeout(scanTimer);
  orbTabsReset();
  loadOrb();
}

// loadOrb asks the board who it is. The reply is the manage view's headline and
// the input to the version gate; a board that answers nothing is said so on the
// firmware card, and the cable doors stay lit, because a cable answers to
// nothing on a DHCP lease.
//
// A board that does not answer on the first try is often a board mid-reboot, and
// the hub itself is the likeliest author of that reboot: the launch cable probe
// resets every attributable lead, and the reset that costs a dial its lease for a
// beat is the same event that made this card open saying "no orb answering" while
// the card behind it was simply booting. So a miss is not the end of the read:
// it re-dials on the boot's clock — a bounded run of slow retries, retired the
// moment the card opens another orb, this one answers, or the retries run out.
// The mDNS stream is the fast path (paintFound re-dials on the frame that says
// the board is back); this is the path for every board the stream does not know.
let orbWakeTmr = 0;
let orbWakeTok = "";
let orbWakeN = 0;
let orbWaking = false;                // the next loadOrb is a wake retry, not a fresh read
const ORB_WAKE_MAX = 30;

function stopOrbWake() {
  if (orbWakeTmr) { clearTimeout(orbWakeTmr); orbWakeTmr = 0; }
}

async function loadOrb(returned) {
  stopOrbWake();                      // one wake-wait at a time, whatever started it
  // wasBack is the page's one true word for "this answer is the board coming
  // back": the read is a wake retry, or the caller says it is re-reading a
  // board it had just written off (an update's boot wait, an mDNS return).
  // It decides below whether the dormant faces re-arm.
  const wasBack = orbWaking || returned === true;
  if (!current.ip) {
    $("#mn-curfw").textContent = "not on the LAN: the doors that work over the cable stay lit";
    return;
  }
  const tok = current.mac || current.ip;
  if (!orbWaking) { orbWakeTok = tok; orbWakeN = 0; }   // a fresh read starts a new wait
  orbWaking = false;
  if (tok !== orbWakeTok) { orbWakeTok = tok; orbWakeN = 0; }
  try {
    await hubFetch("/api/orb/allow", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ ip: current.ip }) });
  } catch (e) { /* the status read below reports the real failure */ }
  try {
    const st = await readDeviceJSON(await hubFetch("/orb/" + encodeURIComponent(current.ip) + "/api/v1/status"));
    current.name = st.name || current.name;
    paintOrbHead();   // the name the board just gave is the headline's, too
    $("#mn-curfw").textContent = "running " + (st.fw || "version unknown") +
      " \u00b7 state " + (st.state || "?") + (st.rssi ? " \u00b7 rssi " + st.rssi : "");
    // No tz paint here: the zone display is the Orb face's picker's own read
    // (its /api/v1/config fetch carries the truth), and the join form sends
    // no zone, so a status tick has nothing on this page it needs to draw.
    if (st.name) $("#mn-name").value = st.name;
    gateFwButtons();
    // The themes are the Apps face's clock sub-tab now: no face prefetches them.
    // What a fresh status does instead is hand the stamp to the tab gate and let
    // the router re-assert the shape (a board that lost its address loses wifi).
    orbTabsStatus(st);
    // Across a reboot, every face already loaded is a report about a board
    // that no longer exists, and every face that failed its fetch while the
    // dial was dark is owed a retry. tabRearm (tabs.js) clears the ledger and
    // re-fires the face on screen; the rest load fresh on the next press.
    if (wasBack) tabRearm();
    startPolling(current.ip);
  } catch (e) {
    $("#mn-curfw").textContent = "no orb answering at " + current.ip + " \u2014 " + e.message +
      "; it may be mid-reboot, re-reading as it comes back";
    // The Firmware face owns the detailed line, but it is a tab now: the ident
    // row, always visible under the name, carries the failure too, so an open
    // but silent board says so on the face the operator is looking at.
    $("#mn-orbident").textContent = "no orb answering at " + current.ip + " \u2014 " + e.message;
    gateFwButtons();
    orbWakeTmr = setTimeout(() => {
      orbWakeTmr = 0;
      if ((current.mac || current.ip) !== orbWakeTok) return;   // a different card is open now
      if (++orbWakeN > ORB_WAKE_MAX) {
        $("#mn-curfw").textContent = "no orb answering at " + current.ip +
          " \u2014 press Re-read when it is back";
        return;
      }
      orbWaking = true;               // keep the count this wait has already spent
      loadOrb();
    }, 5000);
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
    // The card's headline names the channel this stamp arrived on, until the
    // board's own next answer overwrites it with the truth of the boot.
    $("#mn-curfw").textContent = "flashed over the cable on " + port;
    await refreshPorts();
    if (!current.mac && mac) current.mac = mac;
    // A reflash that kept its settings usually lands back on the network it was
    // already on, and that is the ending worth reporting. The join form exists to
    // give a board a network; asking for one again after an update that lost
    // nothing made a finished update read as one that had not finished.
    if (!wipe && await flashJoinedAgain(port, mac)) return;
    openJoinDialog(port, mac || current.mac, "");
  }
}

// flashJoinedAgain asks the one question an updated board answers: is it back on
// the LAN. It runs /api/watch with skip_ap, so the hub dials the address it
// learned over the USB console during the flash instead of waiting on a
// provisioning AP, and it takes the firmware's own word: a status call answering
// state "connected". On that answer the update is over, the panel says so in its
// headline, and no form opens. Every other outcome (no address yet, the AP open
// instead, a refused passphrase, a budget that ran out) returns false, and the
// caller opens the join form, which re-runs this same watch and shows the
// operator what it found. A settings-wipe flash never asks: it took the
// credentials, so the form is its next step by definition.
async function flashJoinedAgain(port, mac) {
  const doing = $("#ob-doing");
  doing.textContent = "waiting for the orb to restart";
  let joined = null;
  await streamPost("/api/watch", { port, mac: mac || "", skip_ap: true, ip: "" }, (ev) => {
    if (ev.log) doing.textContent = ev.log;
    const r = ev.result;
    if (r && r.mode === "lan" && r.status && r.status.state === "connected") joined = r;
  });
  if (!joined) return false;
  doing.textContent = "back on Wi-Fi at " + joined.ip;
  $("#mn-curfw").textContent = "flashed over the cable, back on Wi-Fi at " + joined.ip;
  pullFound();
  fpClose();
  return true;
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

// ffPoll drives the Wi-Fi update's progress bar. The hub's POST is one blocking
// upload the browser cannot watch — the bytes cross hub-to-orb, not page-to-hub
// — so the page polls /api/orb/update/progress while the POST is open, and the
// hub answers from the byte counter its HTTP writer keeps. The board itself is
// silent through the whole upload (h_ota holds the one httpd task), which is
// exactly why the hub has to be the one that knows.
let ffTimer = 0;
function ffWatchStop() { if (ffTimer) { clearInterval(ffTimer); ffTimer = 0; } }
function ffWatchStart(ip) {
  ffWatchStop();
  const bar = $("#ff-bar > div"), barBox = $("#ff-bar");
  if (barBox) barBox.hidden = false;
  if (bar) bar.style.width = "0%";
  ffTimer = setInterval(async () => {
    if (document.hidden) return;                    // a background tab owes nobody a dial
    try {
      // hubFetch, not getJSON: the route is behind the control gate, and getJSON
      // sends no token — a page whose polls died in a silent catch is exactly how
      // this bar shipped blank once. The envelope is {ok,data:{...}} in one layer.
      const r = await hubFetch("/api/orb/update/progress?ip=" + encodeURIComponent(ip));
      const j = await r.json().catch(() => ({}));
      if (!r.ok || !j || j.ok !== true) return;      // a hub mid-restart or a gate that refused
      const p = j.data || {};
      if (p.phase === "idle") return;               // the POST has not opened its tracker yet
      const fill = $("#ff-bar > div");
      if (fill && typeof p.pct === "number") fill.style.width = p.pct + "%";
      const kb = (n) => Math.round((n || 0) / 1024);
      const line = p.phase === "uploading"
        ? "sending " + kb(p.sent) + " of " + kb(p.total) + " KB \u2014 " + p.pct + "%"
        : p.phase === "staged" ? "staged in the spare slot; asking the orb to reboot\u2026"
        : p.phase === "confirming" ? "the reboot answer died on the wire; waiting for the orb to come back on the new stamp"
        : p.phase === "staged, no reboot ack" ? "staged, but the orb never answered the reboot; it boots the new image at its next restart"
        : p.phase === "rebooting" ? "rebooting into the new image; it leaves the wire for one boot"
        : p.phase === "failed" ? "the orb refused the upload"
        : p.phase || "";
      if (line) $("#ff-state").textContent = line;
    } catch (e) { /* a hub mid-restart: the POST's own outcome will land */ }
  }, 700);
}

async function wifiUpdate() {
  if (!current.ip) return;
  const state = $("#ff-state"), go = $("#ff-go");
  state.textContent = "uploading to " + current.ip + "\u2026";
  ffUploadOpen = true; ffCancelLabel();
  ffWatchStart(current.ip);
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
        go.textContent = "Flash anyway over Wi-Fi";
        go.disabled = false;
        $("#ff-bar").hidden = true;
        state.textContent = j.error + " The bootloader rolls back if the new image never boots.";
        return;
      }
      throw new Error(j.error || ("HTTP " + r.status));
    }
    const d = j.data || {};
    $("#mn-curfw").textContent = "staged and rebooting over Wi-Fi: " + d.from + " \u2192 " + d.to;
    $("#mn-verdict").textContent = "";
    $("#ff-mask").hidden = true;   // the card carries the rest; it is not a dead end
    pullFound();
    // The board is mid-reboot; its stamp (and this card's whole headline) only
    // becomes true again once it answers. Re-read on the boot's own clock, and
    // say so — the read is a return, so the faces re-arm with it.
    setTimeout(() => { loadOrb(true); pullFound(); }, 8000);
  } catch (e) {
    state.textContent = e.message;
    go.disabled = false;
  } finally {
    ffWatchStop();
    $("#ff-bar").hidden = true;
    // Out of flight and back to idle wording: the dialog is a form again, and
    // its ghost button means leave, which is what Cancel says.
    ffUploadOpen = false; ffCancelLabel();
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
                                  : "\n\u2717 The orb didn't accept the reset" +
                                    (ev.result.crash ? " — it crashed taking it: " + ev.result.crash : "") + "\n");
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
  // S6 restores the join-time install S5 deleted, narrowed to the clock and gated on the
  // ledger answering zero card rows: a board an owner emptied gets emptied again only if
  // they press Install. Everything else in apps/ waits for a press, so a hub directory full
  // of experiments cannot refill a shelf somebody cleared. The hub holds the real gate
  // (firmware floor, mounted card, port claim); this call is the moment, not the decision.
  autoInstall(ip);
  pullFound();
}

// One install attempt per joined address per page load. A board that drops off WiFi and
// answers again is the same board, and re-installing a card the owner has been working on
// because the DHCP lease moved is the failure this map exists to prevent.
const installTried = new Set();

async function autoInstall(ip) {
  if (installTried.has(ip)) return;
  installTried.add(ip);
  const watch = $("#ob-watch");
  // The gate is the LEDGER, read from the dial that owns it. It used to read
  // /api/fleet's per-row `apps` count, and commit 9f56b7e turned the fleet store into an
  // address book that no longer carries that field — so the line became `if (undefined)
  // return`, never fired, and a board whose owner had deliberately emptied the shelf got
  // refilled on the next join. A guard that reads a field nobody sends is a guard that is
  // off, silently, which is the worse of the two ways to be wrong here.
  //
  // Either tier counts as the owner's doing: a container on the card, or a row in a slot.
  // An unreadable ledger falls through and lets the hub decide, the same way it always did.
  try {
    const d = await readDeviceJSON(await hubFetch(
      "/orb/" + encodeURIComponent(ip) + "/api/v1/apps"));
    if (d && !d.error) {
      // A compiled row is not the owner's doing — the firmware put it there and they cannot
      // take it off — and h_apps_list's installed tier CARRIES it (apps_send_installed
      // emits the shell at slot 0 with "compiled":true). Counting it would mean no dial
      // ever read as empty, the join-time seed would never run again, and a board out of
      // the box would sit uninstalled forever. Same predicate the Apps face filters on.
      const rows = (Array.isArray(d.installed) ? d.installed : [])
        .filter((a) => !a.compiled);
      const onCard = Array.isArray(d.apps) ? d.apps.length : 0;
      if (rows.length || onCard) return; // the owner put something here: leave the shelf alone
    }
  } catch (e) { /* unreadable ledger: fall through and let the hub decide */ }
  if (watch) watch.textContent = "installing the clock from the hub library\u2026";
  const done = await streamInstall(ip, "clock", true);
  if (watch && done) watch.textContent = "clock installed from the hub library.";
  else if (watch) watch.textContent = "the clock did not install on join \u2014 see the Library card.";
}

// streamInstall POSTs one install run and reads the hub's NDJSON steps, returning whether
// the run said done. It is the same shape watchOrbs uses for the same reason: an install
// crosses WiFi and takes minutes, and a spinner with no lines cannot be told apart from a
// hang.
//
// The optional frame is the shared flash panel's elements (fpUI()). A press from the Library
// card hands one in, and every step then lands there as it happens rather than in a `<pre>`
// under the fold that filled in only after the whole run: a 660 KB container takes long
// enough that the operator needs the panel's headline to change and the bar to move to know
// the hub is writing bytes and not stuck. Without a frame the card's own log is still
// written, which is what the automatic join-time run does — it has the join dialog as its
// frame already and must not steal it mid-provision.
// The optional force (D38) rides the query the hub's Update press sends: it
// tells installRun the shelf's copy must cross again even where the card and
// the dial already hold readable copies of their own — the press was made
// because those copies are older.
async function streamInstall(ip, only, auto, frame, force, card) {
  let q = "/api/library/install?ip=" + encodeURIComponent(ip);
  if (only) q += "&only=" + encodeURIComponent(only);
  if (auto) q += "&auto=1";
  if (force) q += "&force=1";
  if (card) q += "&card=1";
  return streamSteps(q, frame, ip);
}

// streamSteps is streamInstall's body, split out because D38's Add-to-card press runs the
// SAME kind of run — one streamed upload that answers with per-chunk facts and a final
// verdict — so it drives the same progress panel rather than a second, thinner dialect.
// q is the whole hub route with its query already assembled.
async function streamSteps(q, frame, ip) {
  const out = $("#lib-install-log");
  const line = function (t) {
    if (frame) {
      frame.log.textContent += (frame.log.textContent ? "\n" : "") + t;
      frame.log.scrollTop = frame.log.scrollHeight;
      return;
    }
    if (!out) return;
    out.textContent += (out.textContent ? "\n" : "") + t;
    out.scrollTop = out.scrollHeight;
  };
  // The bar is driven by the hub's `uploading` steps, whose note is "N of M KB" — the only
  // true statement available mid-transfer, since the device answers once at the end.
  const setBar = function (note) {
    if (!frame) return;
    const m = /(\d+) of (\d+) KB/.exec(note || "");
    if (!m) return;
    const pct = Math.min(100, Math.round(Number(m[1]) * 100 / Number(m[2])));
    frame.bar.style.width = pct + "%";
    frame.bar.title = note;
    frame.op.textContent = pct + "%";
  };
  const headline = function (j) {
    if (!frame) return;
    frame.doing.textContent = "working on " + ip + " \u2014 " + describeStep(j);
  };
  try {
    const r = await hubFetch(q, { method: "POST", headers: { Accept: "application/x-ndjson" } });
    if (!r.ok || !r.body) { line("refused: HTTP " + r.status + " — " + (await r.text().catch(() => ""))); return false; }
    const rd = r.body.getReader(), dec = new TextDecoder();
    let buf = "", ok = false, refusal = "";
    for (;;) {
      const got = await rd.read();
      if (got.done) break;
      buf += dec.decode(got.value, { stream: true });
      let nl;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const raw = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (!raw) continue;
        let j;
        try { j = JSON.parse(raw); } catch (e) { continue; }
        const t = j.step + ": " + j.action + (j.note ? " \u2014 " + j.note : "");
        line(t);
        headline(j);
        if (j.action === "uploading") setBar(j.note);
        if (j.action === "done") ok = true;
        if (j.action === "refused") refusal = j.note || "the hub refused the run";
      }
    }
    if (frame) {
      frame.doing.textContent = ok ? "done on " + ip : "the hub refused it";
      if (ok) frame.bar.style.width = "100%";
      // A refusal leaves its own line as the panel's last word, and the panel open: the
      // rules for this panel say an operation never closes its own completion row.
      if (!ok && refusal) line("\u2717 " + refusal);
    }
    return ok;
  } catch (e) {
    line("the hub did not answer: " + (e && e.message ? e.message : e));
    return false;
  }
}

// describeStep words one NDJSON step for a panel headline. It names what is happening, not
// the protocol's own field names, because the headline is the one line an operator reads
// while they wait.
function describeStep(j) {
  switch (j.action) {
    case "uploading": return "sending " + j.step + " (" + (j.note || "") + ")";
    case "uploaded": return j.step + " sent; asking the orb to install it";
    case "installing": return "the orb is writing " + j.step + " to its app slot";
    case "installed": return j.step + " installed";
    case "enabled": return j.step + " enabled";
    case "storing": return "the orb is storing the theme";
    case "stored": return "theme stored";
    case "rebooting": return "the orb is rebooting after the store";
    case "skipped": return j.step + " was already there" + (j.note ? " (" + j.note + ")" : "");
    case "refused": return "refused: " + (j.note || "no reason given");
    case "done": return "done";
    default: return j.step + " \u2014 " + j.action;
  }
}


$("#ob-rejoin").onclick = () => {
  $("#ob-done").hidden = true;
  $("#ob-join").hidden = false;
  $("#ob-watch").textContent = "pick a network and connect again — the form writes over the cable.";
};
$("#ob-manage").onclick = () => {
  closeJoinDialog();
  $("#ob-flashpanel").hidden = true;
  // Pass the name this join was just given as the row, so the headline reads
  // "Kitchen" on the first frame rather than a mac until the status reply
  // arrives a beat later. loadOrb confirms it against the board regardless.
  openOrbByIP(orbIP, provMac, { name: ($("ob-name").value || "").trim() });
};
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

// The picker's standing answer, identical to the New Orb flow's: the scan shows
// what the radio could hear, and "Other — type the network name" is the way to a
// network that hides its SSID — the device's scan asks the driver not to return
// hidden ones, so no sweep will ever list it.
function applyWifiChoice() {
  $("#j-manual").hidden = $("#j-ssid").value !== "__manual__";
}
$("#j-ssid").onchange = applyWifiChoice;

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
    const prev = sel.value;
    sel.innerHTML = "";
    for (const n of list) {
      const o = document.createElement("option");
      o.value = n.ssid;
      o.textContent = `${n.ssid}  (${n.rssi} dBm${n.open ? ", open" : ""})`;
      sel.appendChild(o);
    }
    const manual = document.createElement("option");
    manual.value = "__manual__";
    manual.textContent = "Other — type the network name";
    sel.appendChild(manual);
    // A scan that saw nothing still leaves the typed name as a way out, so the
    // picker is never the dead end it once was (same doctrine as the dialog's).
    if (list.some((n) => n.ssid === prev)) sel.value = prev;
    else if (list.length) sel.selectedIndex = 0;
    else sel.value = "__manual__";
    applyWifiChoice();
    jlog.textContent = list.length
      ? list.length + " network(s). Pick one, enter the password."
      : "no networks found — a hidden network can still be typed; or try Rescan";
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
  const picked = $("#j-ssid").value;
  // The sentinel is never the answer sent: under "Other" the name is what the
  // operator typed, exactly as the New Orb flow reads its own write-in.
  const ssid = (picked === "__manual__" ? $("#j-manual").value.trim() : picked).trim();
  if (!ip) { log.textContent = "enter the orb's LAN IP"; return; }
  if (!ssid) { log.textContent = "pick or type a network first (Rescan if the list is empty)"; return; }
  orbIP = ip;
  // No tz key at all: the zone is the Orb face's picker's door, and the
  // firmware reads an absent key as "keep the stored one".
  const body = { ssid, pass: $("#j-pass").value };
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
  // The open manage card reads its amber Update badge off current.row, and
  // that object is a snapshot of the frame the card was opened from. Without
  // this resync a pushed fold — a re-announce, a sweep answer, or the note the
  // update handler writes itself — repaints the tile and leaves the card below
  // it holding the stale frame: the board came back on the new firmware and
  // the card still promises the update it just took. When this frame is the
  // open board's, the card adopts it.
  if (current.mac && o.mac && o.mac === current.mac) {
    const wasWaiting = orbWakeTmr !== 0 && current.ip === o.ip;
    current.row = o;
    current.ip = o.ip || current.ip;
    // Adopt a name this frame carries — it came from the board, so it outranks
    // an address, and it is the cheapest word that the board is answering again.
    // The name input below is left alone: unsaved typing is the operator's.
    if (o.name) current.name = o.name;
    paintOrbHead();
    gateFwButtons();
    // The stream says this board is answering again. If the open card is sitting
    // in its wake-wait for the same address, do not let the operator watch a
    // five-second timer for a fact the packet just proved: re-dial now.
    if (wasWaiting && o.connected === true) { stopOrbWake(); loadOrb(true); }
  }
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
// The Themes face draws one app's rows at a time. Under D37 the chooser is the
// LEDGER's answer, not the listing's: an app that can take themes gets a pane the
// moment it is installed and enabled, whether or not the card holds one yet.
// themeAppsRows carries that enumeration; themeRowsAll carries the rows currently
// on the card. themeSub "" draws every row; tabs.js resets the face through
// themeFaceReset so a tab opened for the next board never paints the last board's
// choice of app.
let themeSub = "";
let themeRowsAll = [];
let themeAppsRows = [];
let themeAppsErr = "";
function themeFaceReset() {
  themeSub = ""; themeRowsAll = []; themeAppsRows = []; themeAppsErr = "";
  // A different board means a different card and a different set of containers, so
  // an editor frame left open on the last one would be offering to write themes
  // that this board has never heard of.
  closeThemeEditor();
}

// themeAppTitle labels a pane with the app's own ledger title when the hub sent
// one. The old version mapped three hard-coded slugs and fell through to the raw
// slug, which is how a fourth app would have arrived labelled "thermo"; the
// capability table lives on the device, so its labels come from there too.
function themeAppTitle(a) {
  const row = themeAppsRows.find((x) => x.slug === a);
  if (row && row.title) return row.title;
  return a;
}

// loadThemeApps is D37's inversion: ask the orb which of its apps can hold themes,
// rather than inferring it from the themes already filed there. The two reads it
// composes server-side are the app ledger and the card listing, so an installed,
// enabled, theme-capable app with an empty card still gets a pane.
async function loadThemeApps(ip) {
  themeAppsRows = [];
  themeAppsErr = "";
  try {
    // 45 s, the same daylight the list read gets: this route rides the same lane
    // behind the same cold card walk, and a hub that waits out its 30 s ride must
    // be allowed to report it rather than be aborted mid-wait. See
    // themeListWithRetry for the ordering this preserves.
    const d = await themeGet("/api/theme/apps?ip=" + encodeURIComponent(ip), 45000);
    themeAppsRows = Array.isArray(d.apps) ? d.apps : [];
    themeAppsErr = d.app_error || d.theme_error || "";
  } catch (e) {
    themeAppsErr = orbWaitWords(e);
  }
  if (themeSub && !themeAppsRows.some((a) => a.slug === themeSub)) themeSub = "";
}

// waitRanOut names the one error kind that says nothing about the orb: Chrome's
// AbortSignal.timeout arrives as a TimeoutError whose message reads "signal timed
// out", and it means only that this page stopped listening before the hub
// answered. The call behind it may be succeeding, failing, or still riding the
// lane — which is why a caller that has a witness to follow must follow it after
// one, instead of reporting a refusal it never received.
function waitRanOut(e) {
  return !!(e && (e.name === "TimeoutError" ||
    String((e && e.message) || e).indexOf("signal timed out") >= 0));
}

// orbWaitWords turns a failed hub call into words that name the real failure.
// waitRanOut's error is rewritten into the budget disagreement it actually is;
// every other error already carries the hub's or the device's own sentence, and
// that sentence is what the operator should read.
function orbWaitWords(e) {
  if (waitRanOut(e)) {
    return "the hub was still waiting on the orb when this page's own wait ran out; the card walk is slow, not lost";
  }
  return String((e && e.message) || e);
}

// themeGet is the GET twin of themePost: same envelope, same error discipline,
// same 20s cap on a hub that accepts a request and then stops answering.
// themeGet is the GET twin of themePost: same envelope, same error discipline,
// same cap on a hub that accepts a request and then stops answering. The cap is
// the caller's to raise, exactly as themePost's is: a route that rides the orb's
// one-lane transport needs the same daylight its hub-side budget promises.
async function themeGet(path, timeoutMs) {
  const opts = { method: "GET" };
  if (typeof AbortSignal !== "undefined" && AbortSignal.timeout) opts.signal = AbortSignal.timeout(timeoutMs || 20000);
  const r = await hubFetch(path, opts);
  const j = await r.json().catch(() => ({}));
  if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
  return j.data;
}

// renderThemeTabs is the app chooser. It draws whenever the orb names at least one
// theme-capable app. Below two it once drew NOTHING — which is how a one-app dial showed
// an unlabelled pile of themes and left its owner unable to say whose themes those were.
// The chooser's job is naming the pane, not offering a choice, so a single app gets a
// single tab that says so; the choice only appears when there is one. A theme-aware app
// with zero themes on the card still gets its pane, showing that empty state and its Edit
// button, because that is the exact moment an author needs the door.
function renderThemeTabs() {
  const nav = $("#mn-themes-tabs");
  if (!nav) return;
  nav.innerHTML = "";
  const apps = themeAppsRows.slice();
  if (!apps.length) return;
  apps.sort((x, y) => (x.slug < y.slug ? -1 : 1));
  const mk = (slug, label, on) => {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "tab sub" + (on ? " on" : "");
    b.textContent = label;
    b.onclick = () => { themeSub = slug; loadThemes(themeIP); };
    nav.appendChild(b);
  };
  // One themeable app: name the pane, and mark it current — with one app in scope the
  // unfiltered view IS that app's view (see themeFaceScope), so an "on" tab is a true
  // statement rather than a pressed button.
  if (apps.length === 1) {
    const n = apps[0].count | 0;
    mk(apps[0].slug, themeAppTitle(apps[0].slug) + " themes"
      + (n ? " (" + n + ")" : " (none)"), !themeSub || themeSub === apps[0].slug);
    return;
  }
  const mk2 = (slug, label) => mk(slug, label, themeSub === slug);
  mk2("", "All");
  for (const a of apps) {
    const n = a.count | 0;
    mk2(a.slug, themeAppTitle(a.slug) + (n ? " (" + n + ")" : " (none)"));
  }
}

// themeFaceScope answers the question every part of this face keeps having to ask: which
// app's themes is the operator looking at? It is the pressed tab when there is one, the
// orb's only theme-capable app when there is exactly one of those, and "" only when the
// pane really spans several apps. Returning "" for a one-app orb is what made the state
// line read "3 themes on the card" with no owner named, and made two different readers
// re-derive the same answer separately.
function themeFaceScope() {
  if (themeSub) return themeSub;
  if (themeAppsRows.length === 1) return themeAppsRows[0].slug;
  return "";
}


async function themePost(path, payload, timeoutMs) {
  // AbortSignal.timeout so a hub that accepts the request and then stops
  // answering (a process mid-exit, a proxy stalled on a device) always ends
  // this wait with a visible error instead of leaving the panel frozen on
  // its loading line — the failure shape the owner saw at 10:22. The device
  // path itself is already capped by the hub (main.go deviceTimeout); this
  // line covers the hub's own half, and a caller that knows its route waits
  // longer than the default hands in the number. Older engines without the
  // static method get today's behavior, not a broken call.
  const opts = {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload),
  };
  if (typeof AbortSignal !== "undefined" && AbortSignal.timeout) opts.signal = AbortSignal.timeout(timeoutMs || 20000);
  const r = await hubFetch(path, opts);
  const j = await r.json().catch(() => ({}));
  if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
  return j.data;
}

// The Theme card's preview unpack, mirroring the themes page on the orb itself:
// one GET per row, and the row answers itself by drawing or staying out of the
// way. The body is the device's own wire shape — [u16 w LE][u16 h LE][RGB565 LE]
// — which the hub relays without touching (docs/theme-preview-plan.md §4). A
// theme that carries NO plane (the device's own 404) keeps the design's quiet
// answer. Everything else — timeout, mid-body stall, refusal, unreachable orb —
// is a FAILED READ, and the row now says so with a "?" chip whose title names
// the reason. The silent hide is what made a stalled route indistinguishable
// from an empty card: "every thumbnail is a black square" was the page keeping
// a secret it could have told.
async function themePreviewPlane(el, t, mark) {
  const fail = (why) => {
    if (!mark) return;
    mark.className = "pvq";            /* stop the spinner: this read ended */
    mark.textContent = "?";
    mark.title = why + " \u2014 click to try again";
    mark.hidden = false;
    // The chip is the retry: a dial that timed out once usually lands on the
    // second try, and the hub's ten-minute cache makes a retried GOOD plane
    // free. Requeue rather than call directly so a retry never runs beside the
    // queue's own in-flight read — the device takes one preview at a time.
    mark.onclick = () => { mark.onclick = null; queuePreview(el, t, mark); };
  };
  // The in-flight read carries its own controller so clearPreviewQueue can
  // actually END it, not just ignore it: the hub holds the orb's lane for the
  // whole relay, so a zombie fetch from a retired render parks every control
  // call — including the redrawn page's own list read — behind a stream that
  // exists only because the page stopped caring about it.
  const ctrl = new AbortController();
  previewCtrl = ctrl;
  let r, b;
  try {
    r = await hubFetch("/api/theme/preview?ip=" + encodeURIComponent(themeIP) + "&id=" + encodeURIComponent(t.id), { signal: ctrl.signal });
    if (r.status === 404) {
      // Two different 404s wear this one status, and the device tells them apart
      // in the body: "this theme carries no preview" is the design's quiet answer,
      // while "no theme on the card answers that id" is h_sd_preview reporting
      // ORB_SD_MOUNTED FALSE — true for the whole window a remount takes, and after
      // a card swap. Reading the second as the first removes the row's preview
      // rectangle entirely, which is the 2026-10-28 report of themes vanishing
      // rather than showing a "?": the page mistook "I cannot look" for "there is
      // nothing to see". A 404 with no readable reason stays the quiet answer, so a
      // hub that speaks the old shape keeps the old behavior.
      const why = await r.json().catch(() => ({}));
      const msg = (why && why.error) ? String(why.error) : "";
      if (msg && !/carries no preview/i.test(msg)) {
        return fail("preview read failed: " + msg);
      }
      if (mark) { mark.hidden = true; mark.onclick = null; }
      return;
    }
    if (!r.ok) {
      let why = "HTTP " + r.status;
      try { const j = await r.json(); if (j && j.error) why = j.error; } catch (e) {}
      return fail("preview read failed: " + why);
    }
    b = await r.arrayBuffer().catch(() => null);
  } catch (e) {
    if (e && e.name === "AbortError") return;   // the page moved on; this read no longer belongs to anyone
    return fail("preview read failed: " + (e && e.message ? e.message : String(e)));
  } finally {
    if (previewCtrl === ctrl) previewCtrl = null;
  }
  if (!b || b.byteLength < 4) return fail("preview came back empty");
  const dv = new DataView(b);
  const w = dv.getUint16(0, true), h = dv.getUint16(2, true);
  if (w < 1 || h < 1) return fail("preview named no plane");
  if (dv.byteLength < 4 + w * h * 2) return fail("preview came short: " + dv.byteLength + " of " + (4 + w * h * 2) + " B");
  el.width = w;
  el.height = h;
  const cx = el.getContext("2d");
  const im = cx.createImageData(w, h), p = im.data;
  for (let i = 0, n = w * h; i < n; i++) {
    const v = dv.getUint16(4 + i * 2, true);
    p[i * 4] = (v >> 11) << 3;
    p[i * 4 + 1] = ((v >> 5) & 63) << 2;
    p[i * 4 + 2] = (v & 31) << 3;
    p[i * 4 + 3] = 255;
  }
  cx.putImageData(im, 0, 0);
  el.title = "how this bake poses its dial";
  el.hidden = false;
  if (mark) { mark.hidden = true; mark.onclick = null; }
}

// The preview queue: one dial in flight, always. The device answers a preview on
// its single-task httpd and holds that task for the whole stream — up to 20 s on
// a cold card — so N rows fetched at once do not draw N thumbnails faster; they
// queue behind each other on three sockets and starve every other route on the
// board while the hub's liveness sweeper calls the stall dead and reboots it.
// The card of 2026-10-26 measured exactly that: the page stuck on loading and an
// orb mid-reboot with nothing but thumbnails asked of it. One at a time is the
// only honest concurrency against a one-task server. The generation counter
// retires every queued or in-flight job when loadThemes redraws the list, so a
// stale plane can never paint onto a row that no longer exists; the retry chip in
// themePreviewPlane re-enters through queuePreview and obeys the same rule.
let previewGen = 0;
let previewRunning = false;
let previewCtrl = null;            // the one in-flight preview's controller; see themePreviewPlane
const previewQueue = [];

function queuePreview(el, t, mark) {
  const gen = previewGen;
  if (mark) { mark.className = "pvq spin"; mark.textContent = ""; mark.title = "waiting its turn for the card"; mark.hidden = false; }
  previewQueue.push({ el, t, mark, gen });
  if (!previewRunning) runPreviewQueue();
}

function clearPreviewQueue() {
  previewGen++;
  previewQueue.length = 0;
  // Retire the stream too, not just the queue: the hub's lane for this orb
  // stays held by the relay until the socket closes, and a redraw that leaves
  // it open starves the fresh page's own list read behind a dead render's
  // thumbnail. The AbortError lands in themePreviewPlane's catch and is silent.
  if (previewCtrl) previewCtrl.abort();
}

function runPreviewQueue() {
  previewRunning = true;
  const next = () => {
    let job;
    while (previewQueue.length) {
      job = previewQueue.shift();
      if (job.gen === previewGen) break;
      job = null;
    }
    if (!job) { previewRunning = false; return; }
    if (job.mark) { job.mark.className = "pvq spin"; job.mark.textContent = ""; job.mark.title = "reading this plane"; }
    const done = () => next();
    Promise.resolve(themePreviewPlane(job.el, job.t, job.mark)).then(done, done);
  };
  next();
}

// themeListWithRetry rides out the lane, because "orb busy" is a wait with a
// name, not a failure. The hub's control budget (1.5 s) deliberately expires
// before one honest preview stream does (up to ~4 s cold), so a list read that
// lands mid-stream is told "retry shortly" in 503 — and the right answer is to
// take the hint quietly instead of painting it on the card as if the orb had
// died. Three waits of 2.3 s covers the worst stream plus a boot; anything
// still busy after that is worth telling the person about.
// themeListWithRetry is the page's own patience on the card read: three tries,
// 2.3 s apart, at a 45 s per-request budget.
//
// The 45 s is the load-bearing number and it is not a guess. The hub rides a busy
// lane for up to pageRideBudget (30 s) before it reports the wait — that bound is
// priced over a cold FatFs walk of a big card, which is where the 30 s goes — and
// a read that takes the full ride must still arrive inside this fetch's window.
// Otherwise Chrome aborts first and the card prints the browser's "signal timed
// out" where the orb's real reason belongs: the failure this ordering kills.
// Fifteen seconds of daylight covers the hub's marshaling and a slow link.
//
// The rule, not the number: every hub-side wait must be smaller than the fetch
// budget of the route that carries it. TestTheHubAnswersBeforeThePageGivesUp
// checks the two files against each other so neither drifts alone.
async function themeListWithRetry(ip) {
  let last = null;
  for (let i = 0; i < 3; i++) {
    try { return await themePost("/api/theme/list", { ip }, 45000); }
    catch (e) {
      last = e;
      if (!e || !laneWaitMessage(e)) throw e;
      await new Promise((r) => setTimeout(r, 2300));
    }
  }
  throw last;
}

// laneWaitMessage says whether a failed call's words name a condition a second
// dial can clear. Three shapes qualify: the hub's busy refusal (the lane was
// held), a wait that ran out the hub's own budget, and a card walk that timed
// out part-way — the board was cold, the card was mid-mount, or the one httpd
// task was serving a plane, and the next dial usually lands. A card that is not
// mounted, an orb that refused the write, and a hub that is gone do not qualify,
// and the page must not retry those into a third failure.
function laneWaitMessage(e) {
  const m = String((e && e.message) || e);
  return m.indexOf("orb busy") >= 0
    || m.indexOf("budget for the lane") >= 0
    || m.indexOf("timed out") >= 0
    || m.indexOf("deadline") >= 0
    || m.indexOf("connection refused") >= 0
    || m.indexOf("no route to host") >= 0;
}

async function loadThemes(ip) {
  const state = $("#mn-themestate"), list = $("#mn-themes"), card = $("#mn-themecard");
  if (!ip) { orbThemeListHidden(); return; }
  themeIP = ip;
  orbThemeListShown();          // the sub-tab owns the reveal, not this fetch
  state.textContent = "reading which apps on this orb can take themes\u2026";
  state.classList.add("spin-wait");
  list.innerHTML = "";
  renderThemeEditRow("", false);   // a stale door from the last read edits the last card
  clearPreviewQueue();
  hideOverlay();
  ovRows.length = 0;
  // Enumeration first, rows second, and never the other way round: the chooser's
  // membership is the ledger's answer (D37), so the rows cannot reset it. If the
  // enumeration is the half that failed, the face still lists the themes it can
  // reach and says which read it lost.
  await loadThemeApps(ip);
  let d;
  try {
    d = await themeListWithRetry(ip);
  } catch (e) {
    state.classList.remove("spin-wait");
    state.textContent = "no theme list from " + ip + ": " + orbWaitWords(e)
      + (themeAppsErr ? " (and no app enumeration: " + themeAppsErr + ")" : "");
    return;
  }
  state.classList.remove("spin-wait");
  const rows = Array.isArray(d.themes) ? d.themes : [];
  themeRowsAll = rows;
  renderThemeTabs();
  if (!d.mounted) {
    state.textContent = "no SD card mounted on this orb — themes live on the card"
      + (themeAppsErr ? "; " + themeAppsErr : "");
    themeEmptyPane(list, null);
    return;
  }
  // The scope, not themeSub: a one-app dial has no pressed tab and still has exactly one
  // right answer for "whose themes are these", and every line below has to agree on it.
  const scope = themeFaceScope();
  const shown = scope ? rows.filter((t) => (t.app || "clock") === scope) : rows;
  state.textContent = shown.length + " theme" + (shown.length === 1 ? "" : "s")
    + (scope ? " for " + themeAppTitle(scope) + " \u2014 the only theme-capable app on this orb"
             : (themeAppsRows.length > 1 ? " for any app on this orb" : ""))
    + " on " + (d.card || "the card") +
    ", drawing \"" + (d.active || "?") + "\"" + (d.free_mb ? ", " + d.free_mb + " MB free" : "")
    + (themeAppsErr ? " (" + themeAppsErr + ")" : "");
  state.textContent += (libThemeCount ? " \u00b7 " + libThemeCount + " theme"
    + (libThemeCount === 1 ? "" : "s") + " on the hub shelf" : "");
  for (const t of shown) list.appendChild(themeRow(t, state, d));
  // D38: the hub's own shelf rides the same list. A theme in the folder and not
  // on the card is a row like any other, and its button says Install — which,
  // for a theme, means the whole way onto the dial: the device's upload door
  // queues the store the moment the file verifies, so the board reboots onto
  // it. There is no theme half-step onto a card, and the face does not pretend
  // there is one.
  let shelf = [];
  try { const p = await getJSON("/api/library"); shelf = Array.isArray(p.themes) ? p.themes : []; }
  catch (e) { /* the shelf rows just do not appear; the orb's own rows stand */ }
  libThemeCount = shelf.length;
  const shelfN = themeShelfRows(scope, rows, state, shelf);
  if (!shown.length && !shelfN) themeEmptyPane(list, scope);
  renderThemeEditRow(scope, shown.length > 0);
  if (d.mounted) warmOrbCard(ip);   // the face is painted; the hub can read the rest ahead
}

// warmOrbCard asks the hub to read ahead on this orb: every row's plane, then
// every editable app's editor region, serially, through the same lane the page
// queue obeys. The kick is safe at any time — the hub dials nothing it already
// remembers, and a second press while a pass runs joins it for free. The poll
// exists only to word the state line while the pass lives; the answers reach
// the rows through the ordinary queue fetches, which find the hub's cache.
let warmPollGen = 0;
function warmOrbCard(ip) {
  const gen = ++warmPollGen;
  const state = $("#mn-themestate");
  if (state) { delete state.dataset.base; state.removeAttribute("data-base"); }
  // face "themes": the pass that answers this kick puts every plane's row, and
  // the active theme's plane before them, ahead of the region reads. The face
  // press is also what ends the pass's own plane phase — see the yield note —
  // because this page's queue is about to fetch those same planes through the
  // same caching door, one at a time, in the order the operator sees them.
  themePost("/api/theme/warm", { ip, face: "themes" }).catch(() => {});
  const tick = async () => {
    if (gen !== warmPollGen || themeIP !== ip) return;
    let st = null;
    try {
      const r = await hubFetch("/api/theme/warm-status?ip=" + encodeURIComponent(ip));
      const j = await r.json().catch(() => ({}));
      if (j && j.ok) st = j.data;
      else return;
    } catch (e) { return; }
    if (gen !== warmPollGen || themeIP !== ip) return;
    if (!state) return;
    if (st.phase === "planes" || st.phase === "regions" || st.phase === "listing") {
      if (!state.dataset.base) state.dataset.base = state.textContent;
      state.textContent = state.dataset.base + " \u00b7 reading ahead " + st.done + " of " + st.total + "\u2026";
      setTimeout(tick, 1500);
    } else if (state.dataset.base) {
      // The pass's closing words are the honest end of the line it started: a
      // stand-down in front of this queue, a card that stopped answering, or a
      // quiet done. The next ordinary state write replaces the sentence.
      const tail = (st.note && (st.phase === "done" || st.phase === "failed")) ? " \u00b7 " + st.note : "";
      state.textContent = state.dataset.base + tail;
      delete state.dataset.base;
    }
  };
  setTimeout(tick, 700);
}

// themeEmptyPane is the empty state D37 exists for. Under the old face an app with
// no themes had no pane to hold a message; now the pane is there and this is what
// fills it. The Edit button appears only when the hub reports an editor for the
// app, so a theme-capable app whose author ships nothing else reads as "no themes
// yet", not as a dead button.
function themeEmptyPane(list, sub) {
  // themeFaceScope, not a private guess: the chooser, the state line and this pane all
  // have to name the SAME app, and three copies of "how did we conclude it was clock" is
  // how one of them ends up saying something the other two contradict.
  const slug = sub || themeFaceScope();
  const row = slug ? themeAppsRows.find((a) => a.slug === slug) : null;
  const li = document.createElement("li");
  li.className = "hint";
  // The app is named in the sentence even when there is only one of them and no tab was
  // pressed: an operator reading a bare "this app" cannot tell whether the face knows
  // which app it is standing on.
  li.textContent = row
    ? themeAppTitle(slug) + " takes themes and this orb has none for it yet."
      + (row.has_editor ? " Use Edit below to draw the first one."
                        : " Its container ships no editor, so themes for it arrive as .orbtheme files.")
    : (slug ? "the card holds no themes for " + slug + " yet" : "the card holds no themes yet");
  list.appendChild(li);
  const btn = themeEditButton(slug);
  if (btn) list.appendChild(btn);
}

// themeEditButton builds the pane's per-app Edit door, or returns nothing when
// this app cannot author. The button has always belonged to ONE app — D37's whole
// point is that the editor rides inside the container and the hub is a loader —
// so there is no global "Theme Editor" affordance to build here, and the header
// that once carried one no longer does.
function themeEditButton(sub) {
  const slug = sub || themeFaceScope();
  const row = slug ? themeAppsRows.find((a) => a.slug === slug) : null;
  if (!row || !row.has_editor || !row.editor_url) return null;
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "ghost";
  btn.textContent = "Edit " + themeAppTitle(slug) + " themes\u2026";
  btn.onclick = () => openThemeEditor(row);
  return btn;
}

// renderThemeEditRow keeps the toolbar door honest about what "no editor here"
// means: no slot content at all, not a dead button. An app WITH themes still
// edits — the empty pane is where a first theme gets authored, not the only
// place editing happens.
function renderThemeEditRow(sub, shownThemes) {
  const slot = $("#mn-theme-editrow");
  if (!slot) return;
  slot.innerHTML = "";
  if (shownThemes) slot.appendChild(themeEditButton(sub) || document.createTextNode(""));
}

// openThemeEditor shows one app's editor inside the Themes face (D37 §6.2). The
// frame is same-origin by construction — the hub built the URL from an address the
// hub learned — so the bridge needs no MessageChannel and no transferable port:
// both documents are this hub's, and same-origin is already the strongest claim
// the bridge could ask an unknown child to make.
function openThemeEditor(row) {
  const pane = $("#mn-theme-editor"), frame = $("#mn-theme-editor-frame");
  const title = $("#mn-theme-editor-title"), state = $("#mn-theme-editor-state");
  if (!pane || !frame || !row.editor_url) return;
  // Tell the read-ahead pass which app's region the next dial is about to
  // stream: this pane pays for its own region either way, and the hint is what
  // makes the SECOND editor press cheap — the pass has already queued that
  // app's region to the front of whatever it was warming.
  if (row.app) {
    themePost("/api/theme/warm", { ip: themeIP, face: "edit", slug: row.slug }, 8000).catch(() => {});
  }
  // The veil stands while the archive streams: the editor's source is read off the
  // card over Wi-Fi (measured 6 s cold, 8.7 s warm on a full clock bake), and a white
  // blank frame through all of it reads as a broken editor rather than a slow card.
  // The frame's own load event lifts it — same-origin by construction, and both the
  // archive and the hub's retry page are documents that load, so one listener covers
  // every answer, including the refusal.
  pane.classList.add("loading");
  const base = row.editor_url.replace(/\/$/, "") + "/";
  frame.src = base + "?orb=" + encodeURIComponent(themeIP) + "&app=" + encodeURIComponent(row.slug);
  title.textContent = themeAppTitle(row.slug) + " theme editor";
  state.textContent = "reading the " + row.slug + " editor source from " + themeIP + "\u2026";
  themeEditorApp = row.slug;
  pane.hidden = false;
}
// The veil's one release. A load event means a document arrived — but which one:
// closing sets src to about:blank, and that old document can fire its load AFTER a
// fresh open already set a new src, so frame.src at load time names the NEW editor,
// not the document that loaded. The loaded document answers for itself: the editor
// and the hub's retry page are both same-origin documents with body content (the
// frame is sandboxed to allow-same-origin by construction), while the stray is an
// empty about:blank. An unanswered open leaves the veil standing, which is the truth.
(function edVeil() {
  const frame = $("#mn-theme-editor-frame");
  if (!frame) return;
  frame.addEventListener("load", () => {
    const pane = $("#mn-theme-editor"), state = $("#mn-theme-editor-state");
    let doc = null;
    try { doc = frame.contentDocument; } catch (e) { return; }   // cross-origin: not ours
    if (!doc || !doc.body || !doc.body.hasChildNodes()) return;  // about:blank stray
    if (pane) pane.classList.remove("loading");
    if (state && pane && !pane.hidden) state.textContent = "editing through the hub";
  });
})();

// themeEditorApp is which app the open frame belongs to, so a message from a frame
// that has since been replaced cannot file a theme under the previous app.
let themeEditorApp = "";

function closeThemeEditor() {
  const pane = $("#mn-theme-editor"), frame = $("#mn-theme-editor-frame");
  if (!pane) return;
  pane.hidden = true;
  pane.classList.remove("loading");
  if (frame) frame.src = "about:blank";
  themeEditorApp = "";
}

// themeBridge is the hub page's half of the editor contract (§7.1). It is eight
// lines and it is the whole mechanism: a child asks to save, the hub dials the
// upload door it already owns, the child is told the answer.
//
// The origin check is not decoration. A page on another origin can postMessage here
// exactly as easily as an editor can, and this handler spends the hub's launch token
// on whatever it is handed — so it answers only a frame this page opened, at the
// origin this page is on, carrying the slug this page asked for.
async function themeBridge(ev) {
  const m = ev.data;
  if (!m || typeof m !== "object" || typeof m.type !== "string" || m.type.indexOf("theme.") !== 0) return;
  const frame = $("#mn-theme-editor-frame");
  if (!frame || ev.source !== frame.contentWindow) return;      // not our editor
  if (ev.origin !== window.location.origin) return;             // not this hub
  const state = $("#mn-theme-editor-state");
  const say = (t) => { if (state) state.textContent = t; };
  const back = (payload) => {
    try { ev.source.postMessage(Object.assign({ type: "theme.status" }, payload), ev.origin); } catch (e) {}
  };
  if (m.type === "theme.ready") { say("editor ready"); back({ state: "ok" }); return; }
  if (m.type === "theme.close") { closeThemeEditor(); return; }
  if (m.type !== "theme.save") return;
  if (!m.container || typeof m.container !== "string") {
    back({ state: "error", message: "the editor sent no container" }); return;
  }
  const name = typeof m.name === "string" ? m.name : "";
  say("storing " + name + "\u2026");
  try {
    // The bytes go to the SAME door the "Add a theme" button uses — the whole point
    // of the bridge: an app's editor buys nothing at the firmware boundary, and
    // D26/D28 verification stays where it already is.
    const bin = Uint8Array.from(atob(m.container), (c) => c.charCodeAt(0));
    const body = new Blob([bin], { type: "application/octet-stream" });
    const r = await hubFetch("/api/theme/upload?ip=" + encodeURIComponent(themeIP) + "&overwrite=1",
      { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
    const d = j.data || {};
    say("stored \u201c" + (d.name || name || "?") + "\u201d (" + (d.id || "?") + ")");
    back({ state: "stored", themeId: d.id || "" });
    await loadThemes(themeIP);
  } catch (e) {
    say("refused: " + e.message);
    back({ state: "error", message: e.message });
  }
}

if (window.addEventListener) window.addEventListener("message", themeBridge);



// themeRow builds one row with DOM APIs rather than a template string: theme names
// are bytes off the orb's card and must never be parsed as markup.
//
// FORMAT 2 gives the row two halves of one identity, and the card shows both because
// they answer different questions. The name is what the owner calls the theme — the
// string inside the container, the only thing a human types or reads. The id is
// theme_id(name), the 12 hex the orb filed it under, and it is the ONLY handle install
// and delete accept: the device has no route that takes a name, so a button that sent
// one would be refused by every orb on the market. The id goes on the row for the same
// reason try_id and staged_id match on it — when a theme fails to load, that is the
// string the orb and the operator can both point at.
function themeRow(t, state, list) {
  const li = document.createElement("li");
  // The ID photo the bake carried: the device unpacks nothing — it streams the
  // plane and this canvas does the work (themePreviewPlane). An absent preview is
  // the design's quiet answer (docs/theme-preview-plan.md §1): the canvas unhooks
  // itself and the row reads exactly as it did before planes existed.
  const pv = document.createElement("canvas");
  pv.className = "pv";
  pv.width = 44;
  pv.height = 44;
  pv.hidden = true;
  li.appendChild(pv);
  // The failure chip: same footprint as the plane, a "?" instead of pixels, and
  // the reason in its title. A theme with no plane shows neither chip nor canvas;
  // a theme whose read FAILED shows this and says why on hover.
  const pvq = document.createElement("span");
  pvq.className = "pvq";
  pvq.hidden = true;
  li.appendChild(pvq);
  queuePreview(pv, t, pvq);
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
  // Migration 1's torn-store witness, keyed by id. In owner words this is ONE
  // fact and nothing else: the last time this theme was put on the wall, the
  // store did not finish, so the wall is not wearing it. The dial draws its
  // base face, and the row's own Activate press completes the store — the
  // firmware re-files the staged copy rather than demanding a re-upload. The
  // face says that, and keeps the firmware's raw internal sentence on hover for
  // the one person who will ever need it.
  if (list && list.staged_id === t.id && list.staged_why) {
    const why = document.createElement("span");
    why.className = "tag warn";
    why.textContent = "last store did not finish \u2014 press Activate again";
    why.title = list.staged_why;
    li.appendChild(why);
  }
  const use = document.createElement("button");
  use.className = "ghost";
  // D38: the verb is activate. The door behind it is still Migration 1's one
  // press — the store of a card file into the slot, and the reboot that draws
  // it — but the row is answering "put this on the wall", which is what
  // activate says and Store, beside an Install button, did not.
  use.textContent = "Activate";
  use.disabled = !t.ok || t.active;
  // Migration 1 made this the ONE press: the door answers once the copy is queued and
  // the orb reboots onto the new face when it verifies, so the press is not over when
  // the POST returns. watchThemeJob follows the row to its witness and the state line
  // says rebooting, stored, or refused — never a silent no-op.
  use.onclick = async () => {
    use.disabled = true;
    state.textContent = "storing " + t.name + " \u2014 queued; the copy takes a few seconds";
    // The POST carries the hub's whole wait: the queue ahead of the dial, the copy
    // of up to 8 MB, and the reboot witness — theme.go gives that 150 s, so the
    // page gives it 165. The order is the invariant (see the same rule on
    // pageRideBudget): when the page gives up first, the row prints Chrome's
    // "signal timed out" over an orb that is storing the theme just fine — owner,
    // 2026-10-28, on exactly this press while the page's own plane relays still
    // filled the dial's single-task httpd. The theme stored and drew; only the
    // sentence lied.
    let postRanOut = false;
    try {
      await themePost("/api/theme/install", { ip: themeIP, id: t.id }, 165000);
    } catch (e) {
      if (!waitRanOut(e)) {
        // The hub answered with its own refusal sentence — no lead held, the dial
        // unreachable at the dial. That sentence is the verdict; show it.
        state.textContent = "store refused: " + orbWaitWords(e);
        use.disabled = false;
        return;
      }
      postRanOut = true;
    }
    // Out of a wait that ran out there is no verdict yet, and the job is the orb's
    // own: follow it to its witness like every other path of this press does.
    if (postRanOut) state.textContent = "storing " + t.name + " \u2014 still following the job";
    await watchThemeJob(state, t);
    use.disabled = false;
  };
  li.appendChild(use);
  // The hover overlay (owner, 2026-10-26): this row is one page of the wheel,
  // and the store button is exactly what a click on the overlay presses.
  const ovEntry = { t, canvas: pv, chip: pvq, save: use };
  ovRows.push(ovEntry);
  pv.addEventListener("mouseenter", () => hoverOverlay(ovEntry));
  // Delete is the one theme action that cannot be undone from here, so it asks
  // first: the firmware's own delete is a plain unlink on the card. The confirm names
  // the theme the way the operator knows it and quotes the id being removed, because
  // two rows can share a name across a re-bake and never share an id.
  const del = document.createElement("button");
  del.className = "ghost";
  del.textContent = "Remove from card";
  // D38 gives the active row's press a second consequence: removing the theme
  // the wall wears also empties the slot (orb_theme_delete's clear tail), and
  // the dial reboots onto its base face. The confirm says so BEFORE the press,
  // the answer's deactivated word drives the wait after it, and the row the
  // re-read paints shows the truth either way.
  del.onclick = async () => {
    const activeNow = !!t.active;
    if (!confirm('Remove "' + t.name + '" (' + t.id + ") from the orb's card?"
      + (activeNow
        ? " It is the theme on the wall \u2014 the dial will clear it and reboot to its no-theme face."
        : " This cannot be undone from here."))) return;
    del.disabled = true;
    state.textContent = "removing " + t.name + "\u2026";
    try {
      const d = await themePost("/api/theme/delete", { ip: themeIP, id: t.id });
      if (d && d.deactivated) {
        state.textContent = "removed " + t.name + " \u2014 the dial is rebooting onto its base face";
        await themeWaitBack(state, t.name);
      }
      await loadThemes(themeIP);
    } catch (e) { state.textContent = "remove refused: " + e.message; del.disabled = false; }
  };
  li.appendChild(del);
  return li;
}

// themeShelfRows draws the hub's themes that the card does not carry, in the
// current app's scope, beside the orb's own rows — one list, every theme the
// hub can see (D38). A shelf theme is matched to a card row by the NAME inside
// its own container (the hub read it when it listed the shelf; the device reads
// it when it lists the card), not by the file's stem on either side: a theme
// that travelled by the drop zone keeps its baked name wherever its file lands.
// Returns the number of rows drawn.
let libThemeCount = 0;   // the state line's count, set by loadThemes with each read

function themeShelfRows(scope, deviceRows, state, shelf) {
  shelf = Array.isArray(shelf) ? shelf : [];
  const list = $("#mn-themes");
  if (!shelf.length || !list) return 0;
  const onCard = {};
  for (const t of deviceRows) onCard[String(t.name || "").toLowerCase()] = true;
  let n = 0;
  for (const f of shelf) {
    const app = f.app || "clock";
    if (scope && app !== scope) continue;
    if (onCard[String(f.title || "").toLowerCase()]) continue;
    n++;
    const li = document.createElement("li");
    const label = document.createElement("b");
    label.textContent = f.title || f.file;
    li.appendChild(label);
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = "in the hub library";
    li.appendChild(tag);
    const size = document.createElement("span");
    size.className = "tag";
    size.textContent = Math.round((f.bytes || 0) / 1024) + " KB";
    li.appendChild(size);
    const inst = document.createElement("button");
    inst.className = "ghost";
    inst.textContent = "Install";
    inst.title = "Send this theme to the orb in hand. The device's theme door stores the file the moment it verifies, and the board reboots onto it.";
    inst.onclick = async () => {
      if (!themeIP) { state.textContent = "no orb in hand \u2014 connect one first."; return; }
      if (typeof askConfirm === "function" && !await askConfirm({
        title: "Install this theme?",
        body: (f.title || f.file) + " goes to " + themeIP + ": the device files the card copy and stores it in the same door, so the board reboots onto the new face.",
        detail: "The hub's copy stays on the shelf. Remove from card later clears the orb's copy, and taking the wall's theme off clears the face too.",
        yes: "Install",
      })) return;
      inst.disabled = true;
      fpOpen();
      const ui = fpUI();
      ui.doing.textContent = "installing " + (f.title || f.file) + " on " + themeIP;
      ui.log.textContent = ""; ui.bar.style.width = "0%"; ui.bar.title = "";
      ui.op.textContent = "pushing"; ui.live.textContent = "";
      state.textContent = "installing " + (f.title || f.file) + " \u2014 the file is crossing";
      // The push streams its steps exactly like an app install: the bar moves
      // with the bytes, and the panel is the single place a refusal is read.
      const okPush = await streamSteps("/api/library/push?kind=theme&name="
        + encodeURIComponent(String(f.file).replace(/\.orbtheme$/i, ""))
        + "&ip=" + encodeURIComponent(themeIP), ui, themeIP);
      if (!okPush) {
        state.textContent = "install refused \u2014 read the progress panel";
        inst.disabled = false;
        return;
      }
      try {
        state.textContent = "installing " + (f.title || f.file) + " \u2014 stored; the board is rebooting onto it";
        await themeWaitBack(state, f.title || f.file);
        fpClose();
        await loadThemes(themeIP);
      } catch (e) {
        ui.doing.textContent = "theme install refused";
        ui.log.textContent = "\u2717 " + e.message;   // the panel stays open with its reason
        state.textContent = "install refused: " + e.message;
        inst.disabled = false;
      }
    };
    li.appendChild(inst);
    const rm = document.createElement("button");
    rm.className = "ghost";
    rm.textContent = "Remove from hub library";
    rm.title = "Delete " + f.file + " from this hub's library. No orb and no SD card is touched.";
    rm.onclick = async () => {
      if (typeof askConfirm === "function" && !await askConfirm({
        title: "Delete it from the hub library?",
        body: (f.title || f.file) + " is removed from the hub\u0027s theme folder.",
        detail: "An orb already drawing it keeps drawing it. This removes only the hub's copy.",
        yes: "Delete from library",
      })) return;
      rm.disabled = true;
      state.textContent = "removing " + (f.title || f.file) + " from the hub library\u2026";
      try {
        const r = await hubFetch("/api/library/rm?kind=theme&name="
          + encodeURIComponent(String(f.file).replace(/\.orbtheme$/i, "")), { method: "POST" });
        const d = await r.json().catch(() => null);
        if (!d || !d.ok) throw new Error((d && d.error) || ("HTTP " + r.status));
        state.textContent = (f.title || f.file) + " \u00b7 removed from the hub library";
        loadThemes(themeIP);   // the re-read brings the shelf too; the row is simply not there
      } catch (e) {
        state.textContent = "the hub refused it: " + e.message;
        rm.disabled = false;
      }
    };
    li.appendChild(rm);
    list.appendChild(li);
  }
  return n;
}

// themeWaitBack waits out a press that reboots the board: the silence IS the
// answer's second half, so the poll just asks for the list until the orb
// answers again. Two and a half minutes, then the caller re-reads anyway and
// the rows say whatever is true.
async function themeWaitBack(state, name) {
  for (let n = 0; n < 100; n++) {
    await new Promise((res) => setTimeout(res, 1500));
    try { await themePost("/api/theme/list", { ip: themeIP }, 45000); return; }
    catch (e) { state.textContent = name + " is away \u2014 the orb is rebooting (" + (n * 1.5 | 0) + " s)"; }
  }
}

// watchThemeJob follows one row after a store, from the queue ack to the witness the
// firmware leaves behind. Migration 1 made store the one press: the door answers as soon
// as the job is queued, the device then spends seconds copying and reboots onto the new
// face. A press without this follow-up is the silent button this card shipped with: the
// row never changed on screen and the operator could not tell a running copy from a dead
// button. The outcome always names the firmware's own reason: the drawn name for a
// landing, the list's try witness for a refusal, the staged witness for a torn store, and
// a re-read prompt when the orb stays quiet past two minutes.
async function watchThemeJob(state, t) {
  for (let n = 0; n < 100; n++) {
    await new Promise((res) => setTimeout(res, n < 60 ? 1200 : 3000));
    let d;
    // 45 s, the same page budget loadThemes carries: this poll rides the same card
    // walk (hub budget 30 s on a cold FatFs cache), and at the old 20 s default a
    // slow walk on a live board read as the mid-reboot miss one line below.
    try { d = await themePost("/api/theme/list", { ip: themeIP }, 45000); }
    catch (e) { state.textContent = "storing " + t.name + " \u2014 the orb is mid-reboot; waiting"; continue; }
    if (d.job_busy) { state.textContent = "storing " + t.name + " \u2014 the copy is running, " + n + " s"; continue; }
    if (d.try_id === t.id && d.try_why) {
      state.textContent = "could not store " + t.name + ": " + d.try_why;
      // Re-read the rows: since §31 the orb marks an undrawable bundle not-loadable
      // at the list itself, so a refusal is very likely already a greyed Store button
      // and a reason tag on this row. An operator who watches only the state line
      // should not have to press Re-read to see the verdict that produced it.
      await loadThemes(themeIP);
      return;
    }
    if (d.staged_id === t.id) {
      state.textContent = "the last store of " + t.name + " did not finish \u2014 press Activate again to complete it";
      return;
    }
    if (d.active === t.name) {
      state.textContent = "the dial is drawing " + t.name;
      await loadThemes(themeIP);
      return;
    }
    const row = (d.themes || []).find((x) => x.id === t.id);
    if (row && row.stored) {
      state.textContent = "stored " + t.name + " \u2014 the orb is rebooting onto it";
      continue;
    }
    state.textContent = "storing " + t.name + " finished without its witness \u2014 the rows were re-read";
    await loadThemes(themeIP);
    return;
  }
  state.textContent = "storing " + t.name + " still going after two minutes \u2014 re-read the card";
}

// The preview overlay: a thumbnail is 44 px and a bake is judged on a dial.
// Hovering a row's plane lifts it to 264 px in a fixed card over the panel,
// captioned with its own gestures: the wheel pages theme by theme through the
// list, a click stores the one on screen by pressing that row's own Store
// button — the same disabled states, the same watchThemeJob follow-up, one
// implementation and never a second copy of the press. It draws the row's own
// decoded canvas, so hovering asks the device for nothing; a plane still in
// the queue shows the overlay's spinner until the row lands its pixels.
const ovRows = [];        // { t, canvas, chip, save } in list order, rebuilt per load
let ovEl = null;
let ovCanvas = null;
let ovCap = null;
let ovIdx = -1;
let ovPoll = 0;

function ensureOverlay() {
  if (ovEl) return;
  ovEl = document.createElement("div");
  ovEl.id = "pvov";
  ovCanvas = document.createElement("canvas");
  const sp = document.createElement("div");
  sp.className = "spin";
  ovCap = document.createElement("div");
  ovCap.className = "cap";
  ovEl.appendChild(ovCanvas);
  ovEl.appendChild(sp);
  ovEl.appendChild(ovCap);
  document.body.appendChild(ovEl);
  ovEl.addEventListener("wheel", (e) => {
    e.preventDefault();
    const d = e.deltaY > 0 ? 1 : e.deltaY < 0 ? -1 : (e.deltaX > 0 ? 1 : e.deltaX < 0 ? -1 : 0);
    if (d) showOverlay(ovIdx + d);
  }, { passive: false });
  ovEl.addEventListener("click", () => {
    const r = ovRows[ovIdx];
    if (r && r.save && !r.save.disabled) r.save.click();
  });
  ovEl.addEventListener("mouseleave", hideOverlay);
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") hideOverlay(); });
}

function showOverlay(i) {
  if (!ovRows.length) return;
  ensureOverlay();
  ovIdx = ((i % ovRows.length) + ovRows.length) % ovRows.length;
  const r = ovRows[ovIdx];
  const ready = r.canvas && !r.canvas.hidden && r.canvas.width > 0;
  const failed = r.chip && !r.chip.hidden && r.chip.textContent.trim() === "?";
  ovEl.classList.add("on");
  ovEl.classList.toggle("waiting", !ready && !failed);
  if (ready) {
    ovCanvas.width = r.canvas.width;
    ovCanvas.height = r.canvas.height;
    ovCanvas.getContext("2d").drawImage(r.canvas, 0, 0);
  }
  const why = r.t.active ? "the dial is already drawing it"
            : !r.t.ok ? "not loadable \u2014 nothing to store"
            : failed ? "this read failed \u2014 retry it on the row"
            : "scroll = previous / next theme \u00b7 click = store it on the dial";
  ovCap.textContent = "";
  const b = document.createElement("b");
  b.textContent = r.t.name;
  const hint = document.createElement("span");
  hint.textContent = (ovIdx + 1) + " of " + ovRows.length + " \u2014 " + why;
  ovCap.appendChild(b);
  ovCap.appendChild(hint);
}

function hideOverlay() {
  ovIdx = -1;
  if (ovPoll) { clearInterval(ovPoll); ovPoll = 0; }
  if (ovEl) { ovEl.classList.remove("on"); ovEl.classList.remove("waiting"); }
}

function hoverOverlay(row) {
  const i = ovRows.indexOf(row);
  if (i < 0) return;
  showOverlay(i);
  if (!ovPoll) ovPoll = setInterval(() => {
    if (ovIdx < 0 || !ovEl || !ovEl.classList.contains("on")) { hideOverlay(); return; }
    const r = ovRows[ovIdx];
    if (r && r.canvas && !r.canvas.hidden) showOverlay(ovIdx);   // the plane landed: stop waiting
  }, 350);
}

$("#mn-rescan").onclick = async () => {
  if (!themeIP) return;
  const state = $("#mn-themestate");
  state.textContent = "asking the orb to re-read its card\u2026";
  // The hub's rescan now rides a lane held by its own streams, and the 45 s
  // window below prices that ride. The outer retry is the belt for the shape
  // the hub cannot wait out: a board that refused the press outright.
  for (let tries = 0; ; tries++) {
    try {
      await themePost("/api/theme/rescan", { ip: themeIP }, 45000);
      break;
    } catch (e) {
      const why = e && e.message ? e.message : String(e);
      if (tries >= 2 || why.indexOf("orb busy") < 0) {
        state.textContent = "refused: " + why;
        return;
      }
      state.textContent = "the card is mid-transfer \u2014 waiting to ask again\u2026";
      await new Promise((r) => setTimeout(r, 2300));
    }
  }
  await loadThemes(themeIP);
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
// §32: the send is watched end to end. The button PRECHECKS the container against the
// card before any bytes dial the orb — a duplicate asks here, inline, not after
// megabytes have crossed the room. A replacement press ships the SAME file the
// precheck just named (no dialog, no re-pick, no browser popup), and while the POST
// blocks, the status line polls /api/theme/upload-status so every phase — hub
// dialing, orb filing, store copying — says its own word. The final sentence quotes
// the hub's store witness: under Migration 1 a stored theme reboots the orb onto
// itself; a torn one boots the base face until one re-store.
let pendingReplace = null;   // {f} — the file the precheck stopped, awaiting the second press
let sendWatchTimer = null;
$("#mn-up-btn").onclick = () => {
  if (pendingReplace) {
    const f = pendingReplace.f;
    const tok = pendingReplace.commit || "";
    pendingReplace = null;
    // The short road when the refusal staged one: the bytes the orb refused are
    // already verified on its card, so the replace spends the token instead of
    // pushing the file again. A refused or stale commit falls back to the full
    // replace inside commitTheme — the press is for the theme, not for a road.
    if (tok) commitTheme(f, tok, $("#mn-up-state"));
    else uploadTheme(f, true, $("#mn-up-state"));
    return;
  }
  $("#mn-fw-file").click();
};
function watchSendState(ip, st, f) {
  stopSendWatch();
  const words = {
    uploading: (d) => "sending to the orb: " + Math.floor((d.sent || 0) * 100 / (d.total || 1)) +
      "% of " + (d.total || 0) + " B",
    queued: () => "on the orb \u2014 store armed, copy next",
    storing: () => "storing into the slot \u2014 progress on the orb's glass; it reboots when the copy lands",
    duplicate: () => "the card holds a different \u201c" + (f && f.name) + "\u201d \u2014 press Upload again to replace",
    rejected: (d) => "the orb refused the upload" + (d.note ? ": " + d.note : "")
  };
  sendWatchTimer = setInterval(async () => {
    try {
      // hubFetch, not getJSON: /api/theme/upload-status is behind the control
      // gate, and a poll that carries no token dies in the catch below exactly
      // like the update bar once did.
      const r = await hubFetch("/api/theme/upload-status?ip=" + encodeURIComponent(ip));
      const j = await r.json().catch(() => ({}));
      if (!r.ok || !j || j.ok !== true) return;
      const d = j.data;
      if (d && words[d.phase]) st.textContent = words[d.phase](d);
    } catch (e) { /* a lost poll is noise; the POST's own answer settles it */ }
  }, 700);
}
function stopSendWatch() { if (sendWatchTimer) { clearInterval(sendWatchTimer); sendWatchTimer = null; } }
function storeVerdictSentence(d, f) {
  if (d.store === "torn")
    return "the copy tore before its header \u2014 the base face boots until one re-store";
  if (d.store === "refused") return "the store refused: " + (d.store_note || "no reason from the orb");
  if (d.store === "lost") return "filed on the card; the store outcome stayed unreported \u2014 re-read the rows";
  return "stored into the slot \u2014 the orb is rebooting onto it";
}
$("#mn-fw-file").onchange = async (e) => {
  const f = e.target.files && e.target.files[0];
  const st = $("#mn-up-state");
  e.target.value = "";
  if (!f) return;
  if (!themeIP) { st.textContent = "load an orb first"; return; }
  pendingReplace = null;
  st.textContent = "asking the orb about \u201c" + f.name + "\u201d\u2026";
  try {
    const pre = await hubFetch("/api/theme/upload?ip=" + encodeURIComponent(themeIP) + "&precheck=1",
      { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: f });
    const pj = await pre.json().catch(() => ({}));
    if (pre.ok && pj.ok && pj.data && pj.data.duplicate) {
      pendingReplace = { f };
      st.textContent = "\u201c" + (pj.data.name || f.name) + "\u201d is already on the card as a different dial" +
        (pj.data.note ? " (" + pj.data.note + ")" : "") + " \u2014 press Upload theme again to replace it";
      return;
    }
  } catch (err) { /* a hub that cannot answer the question does not veto the send; the device still decides */ }
  st.textContent = "sending " + f.size + " B\u2026";
  watchSendState(themeIP, st, f);
  try {
    const r = await hubFetch("/api/theme/upload?ip=" + encodeURIComponent(themeIP),
      { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: f });
    const j = await r.json().catch(() => ({}));
    stopSendWatch();
    if (!r.ok || j.ok === false) {
      // The late refusal the serial-only precheck cannot see (same name, same
      // serial, different payload CRCs). Same inline second press — the prompt
      // that used to be a browser dialog stealing the page. The refusal now
      // usually carries a commit token too: the orb verified these bytes, kept
      // the copy on its card, and offers to file the keep for an empty POST.
      // The token lives on the hold so the second press can take the short road;
      // a refusal without one (older firmware) keeps the re-upload it always was.
      const why = j.error || ("HTTP " + r.status);
      if (why.indexOf("already on the card") >= 0) {
        pendingReplace = { f, commit: j.commit || "" };
        st.textContent = "the orb refused: " + why + " \u2014 press Upload theme again to " +
          (j.commit ? "file the copy already on its card" : "replace it");
        return;
      }
      throw new Error(why);
    }
    const d = j.data || {};
    st.textContent = "stored \u201c" + (d.name || "?") + "\u201d (" + (d.id || "?") + ") \u2014 " + storeVerdictSentence(d, f);
    await loadThemes(themeIP);
  } catch (err) { stopSendWatch(); st.textContent = "upload failed: " + err.message; }
};

// uploadTheme pushes the file, with the firmware's ?overwrite=1 set only on the second
// press of the inline replace prompt above. It watches the same status witness the
// button's first press armed, and reports the hub's store verdict, not a hope.
async function uploadTheme(f, overwrite, st) {
  st.textContent = "replacing " + f.size + " B\u2026";
  watchSendState(themeIP, st, f);
  try {
    const r = await hubFetch("/api/theme/upload?ip=" + encodeURIComponent(themeIP) + (overwrite ? "&overwrite=1" : ""),
      { method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: f });
    const j = await r.json().catch(() => ({}));
    stopSendWatch();
    if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
    const d = j.data || {};
    st.textContent = "replaced \u201c" + (d.name || "?") + "\u201d (" + (d.id || "?") + ") \u2014 " + storeVerdictSentence(d, f);
    await loadThemes(themeIP);
  } catch (err) { stopSendWatch(); st.textContent = "upload failed: " + err.message; }
}

// commitTheme spends a duplicate refusal's one-shot token: POST /api/theme/commit
// with an empty crossing, which files the copy the orb already verified and staged,
// and queues the same store an overwrite upload would. The status line says which
// road is being taken and, when the token turns out stale (410: a reboot, a card
// swap, another upload since), the file ships the long way instead — the operator
// confirmed the replacement, and a dead token must not quietly cancel it.
async function commitTheme(f, tok, st) {
  st.textContent = "filing the copy already on the orb\u2026";
  try {
    const r = await hubFetch("/api/theme/commit", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ip: themeIP, commit: tok })
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok || j.ok === false) throw new Error(j.error || ("HTTP " + r.status));
    const d = j.data || {};
    st.textContent = "replaced \u201c" + f.name + "\u201d (" + (d.id || "?") + ") from the copy on the card \u2014 " +
      (d.reboot === "automatic" ? "the orb is rebooting onto it" : "stored");
    await loadThemes(themeIP);
  } catch (err) {
    st.textContent = "commit refused (" + err.message + ") \u2014 sending the file again\u2026";
    uploadTheme(f, true, st);
  }
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


// (The time zone is set from the ported /timezone picker in tabs.js now; the old
// type-a-string door left with the card it lived on.)

// orbAdoptName is how tabs.js's rename lets the page's own state follow the
// board: `current` is app.js's declaration, and writes to it stay in this file.
function orbAdoptName(n) { current.name = n; }

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

// ---- one themed confirmation, shared by every face ------------------------------
// Replaces the browser's confirm(), which could not be styled, could not name what
// it was about to cost, and landed focus on OK so a stray Enter approved a deletion.
// Here Cancel takes focus, Escape and a scrim click both mean no, and the caller
// supplies the sentence so the thing about to be destroyed is named in the window
// that destroys it. One dialog in the document, resolved through a promise, so a
// face can `if (!(await askConfirm(...))) return;` exactly as it did with confirm().
// Only one ask is ever live: a second call while one is open is refused as a
// cancel rather than stacking a second question the operator cannot see.
let askOpen = null;

function askConfirm(spec) {
  if (askOpen) return Promise.resolve(false);
  const mask = $("#cf-mask"), ok = $("#cf-ok"), no = $("#cf-no");
  if (!mask || !ok || !no) return Promise.resolve(window.confirm(spec.body));
  return new Promise((done) => {
    const back = document.activeElement;
    $("#cf-title").textContent = spec.title || "Are you sure?";
    $("#cf-body").textContent = spec.body;
    const detail = $("#cf-detail");
    detail.textContent = spec.detail || "";
    detail.hidden = !spec.detail;
    ok.textContent = spec.yes || "Confirm";
    ok.className = spec.safe ? "primary" : "danger";
    no.textContent = spec.no || "Cancel";
    const settle = (v) => {
      mask.hidden = true;
      document.removeEventListener("keydown", onKey, true);
      mask.removeEventListener("click", onScrim);
      askOpen = null;
      if (back && back.focus) back.focus();   // home the caret came from
      done(v);
    };
    function onKey(ev) {
      if (ev.key === "Escape") { ev.preventDefault(); settle(false); }
      else if (ev.key === "Enter" && document.activeElement === no) {
        ev.preventDefault(); settle(false);
      }
    }
    function onScrim(ev) { if (ev.target === mask) settle(false); }
    askOpen = settle;
    ok.onclick = () => settle(true);
    no.onclick = () => settle(false);
    document.addEventListener("keydown", onKey, true);
    mask.addEventListener("click", onScrim);
    mask.hidden = false;
    no.focus();
  });
}

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

// The Close button is the frame's only chrome. An editor that wants to be dismissed
// asks through the bridge (theme.close); this is for the operator, who should never
// have to hunt for a tab to close.
// Wired by direct lookup rather than a DOMContentLoaded hook: this file is a
// classic script at the end of the document, so the node already exists, and
// closeThemeEditor runs from themeFaceReset during boot before any event would
// have fired. A missing button is a hidden pane, not an error.
{
  const b = document.getElementById("mn-theme-editor-close");
  if (b) b.onclick = closeThemeEditor;
}
