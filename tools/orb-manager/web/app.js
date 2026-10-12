// Orb Manager front-end. Talks only to same-origin endpoints (/api/*, /orb/*).
// The server is the only thing that reaches an orb; CORS never enters into it.
"use strict";

const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];

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

function show(view) {
  // A card can name a view that is not in this document: the Theme Editor is a
  // separate page. With nothing to reveal, switching only hides the hub and
  // re-shows it a hash-change later, so the click looks dead. Refuse instead and
  // let the caller follow the card's href.
  if (view && !document.getElementById(view)) return false;
  $$(".view").forEach((v) => (v.hidden = v.id !== view));
  $("#hub").hidden = view !== null;
  if (view) location.hash = view;
  return true;
}

document.addEventListener("click", (e) => {
  const card = e.target.closest("[data-view]");
  if (card) {
    // Intercept only a card naming a real in-page view. The Theme Editor card
    // has an href to /composer/, a separate page: preventDefault there used to
    // cancel that navigation, and the click did nothing at all.
    if (show(card.dataset.view)) e.preventDefault();
    return;
  }
  if (e.target.closest("[data-back]")) { show(null); location.hash = ""; }
});
window.addEventListener("hashchange", route);
function route() {
  const v = location.hash.replace("#", "");
  if (v === "onboard" || v === "manage") show(v);
  else show(null);
}

// ---- meta ------------------------------------------------------------------
(async () => {
  try {
    const m = await getJSON("/api/status");
    $("#meta").textContent = `${m.platform} · build ${m.buildDir || "—"}`;
  } catch (e) { $("#meta").textContent = e.message; }
})();

// ---- onboarding ------------------------------------------------------------
async function refreshPorts() {
  const sel = $("#ob-port");
  sel.innerHTML = "";
  try {
    const { ports } = await getJSON("/api/ports");
    // Never trust the payload to be an array: taking .length on undefined threw
    // here once and the picker blamed the browser instead of saying "no ports".
    const list = Array.isArray(ports) ? ports : [];
    if (!list.length) { sel.innerHTML = "<option>(no serial ports found)</option>"; return; }
    for (const p of list) {
      const o = document.createElement("option");
      o.value = p;
      o.textContent = p;
      sel.appendChild(o);
    }
  } catch (e) { sel.innerHTML = `<option>(${e.message})</option>`; }
}

async function refreshBuild() {
  const el = $("#ob-build");
  try {
    const b = await getJSON("/api/build");
    if (!b.present) { el.textContent = "no build detected — run idf.py build, or start with --build <dir>"; return; }
    el.innerHTML = "<b>" + b.images.length + " image(s) from " + b.buildDir + ":</b><br>" +
      b.images.map((i) => `${i.offset}  ${i.name}  (${i.bytes} B)`).join("<br>");
  } catch (e) { el.textContent = e.message; }
}

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
    r = await fetch(url, {
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

// streamFlash drives one POST /api/flash and renders its NDJSON events into the
// log/bar. It returns {mac, ok}: mac is the eFuse MAC the ROM reported (null if
// the flash died before the MAC read), ok is whether the flash reached its
// "done" event — the caller only runs onboarding after a real success, so a
// refused or mid-flash error never dumps the user into a Wi-Fi form for an orb
// that was not actually rewritten.
async function streamFlash(url, payload, log, bar) {
  log.textContent = ""; bar.style.width = "0%";
  let mac = null;
  let ok = false;
  await streamPost(url, payload, (ev) => {
    if (ev.log) log.textContent += ev.log + "\n";
    else if (ev.error) log.textContent += "\n\u2717 " + ev.error + "\n";
    else if (ev.done) { log.textContent += "\n\u2713 " + ev.done + "\n"; ok = true; }
    else if (ev.mac) mac = ev.mac;
    else if (ev.progress) {
      const p = JSON.parse(ev.progress);
      const pct = p.total ? Math.round((p.written / p.total) * 100) : 0;
      bar.style.width = pct + "%";
      log.textContent += `\r[${p.image + 1}/${p.of}] ${p.offset} ${p.written}/${p.total} (${pct}%)`;
    }
    log.scrollTop = log.scrollHeight;
  });
  return { mac, ok };
}

$("#ob-refresh").onclick = refreshPorts;

$("#ob-go").onclick = async () => {
  const port = $("#ob-port").value;
  const mode = $("#ob-mode").value;
  const wipe = $("#ob-wipe").checked;
  const log = $("#ob-log");
  const bar = $("#ob-bar > div");
  const go = $("#ob-go");
  go.disabled = true;
  try {
    // Onboarding runs after ANY successful flash, not just full: an app-mode
    // flash of an unprovisioned/wiped orb still opens the provisioning AP, and
    // the boot console the flash already captured decides AP vs connected — the
    // firmware, not the flash mode, picks the branch. A flash that errored
    // (ok=false) never opens the Wi-Fi form for an orb that wasn't rewritten.
    const { mac, ok } = await streamFlash("/api/flash", { port, mode, wipe }, log, bar);
    if (ok) startJoin(port, mac);
    else $("#ob-join").hidden = true;
  } finally {
    go.disabled = false;
  }
};

// ---- post-flash onboarding ---------------------------------------------------
// The firmware decides the branch, not the UI and not the flash mode: a flash
// that leaves stored NVS lets the orb self-join in ~2.5 s, while a fresh or
// wiped orb opens its own open provisioning AP the moment boot finds NVS empty
// — regardless of whether the flash was full or app-only. The flash session
// reads the boot console while it still owns the port and records that verdict;
// /api/watch consumes it. A connected orb's address is known only from what the
// hub read over USB — the boot banner the flash logged, or the lease the serial
// door read after PROV-ACK — never from an ARP table or a subnet sweep.
// Credentials always travel over HTTP — scan + join through the same-origin
// proxy, the same pair the orb's own config page uses.
let orbIP = "";
let provPort = ""; // the serial port the wired-provision door writes to
let provMac = "";  // the flashed orb's MAC, the key back to its boot verdict

function startJoin(port, mac) {
  const join = $("#ob-join");
  join.hidden = false;
  $("#ob-watch").textContent = "waiting for the orb to boot…";
  $("#ob-console").textContent = "";
  $("#ob-ap").hidden = true;
  $("#ob-pick").hidden = true;
  $("#ob-done").hidden = true;
  $("#j-log").textContent = "";
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
      // A transport fault (manager restarted mid-probe, etc.) — say so on the
      // watch line instead of leaving it stuck on "waiting for the orb to boot".
      $("#ob-watch").textContent = ev.error;
    } else if (ev.result) onWatchResult(ev.result);
  });
}

function onWatchResult(res) {
  const watch = $("#ob-watch");
  if (res.mode === "lan") {
    // A full flash preserves NVS, so a provisioned orb returns to its network
    // on its own — pushing a scan/join form at a connected orb is noise.
    // The picker is for the orb that is reachable but NOT connected (wrong
    // passphrase after a router change), which is the case that needs it.
    if (res.status && res.status.state === "connected") {
      watch.textContent = "orb is on the LAN at " + res.ip + ", already connected.";
      doneWithOrb(res.ip, res.status);
    } else {
      const st = res.status ? res.status.state : "unknown state";
      watch.textContent = "orb is reachable at " + res.ip + " but its state is \"" + st + "\".";
      showPicker(res.ip);
    }
  } else if (res.mode === "ap") {
    watch.textContent = "orb has no stored network — it opened its own provisioning AP.";
    $("#ob-ap-ssid").textContent = res.ap_ssid;
    populateSerialNetworks(res.networks || []);
    $("#ob-ap").hidden = false;
  } else {
    watch.textContent = "orb did not announce itself: " +
      (res.reason || "enter its IP or join its AP and continue here") + ".";
    showPicker("");
  }
}

function showPicker(ip) {
  $("#ob-pick").hidden = false;
  $("#ob-ap").hidden = true;
  $("#ob-done").hidden = true;
  if (ip) { orbIP = ip; $("#j-ip").value = ip; loadNetworks(); }
}

function doneWithOrb(ip, st) {
  orbIP = ip;
  $("#ob-done-msg").textContent =
    "orb is connected at " + ip +
    " (" + st.rssi + " dBm, fw " + st.fw +
    (st.tz ? ", " + st.tz : "") + "). Nothing left to configure.";
  $("#ob-done").hidden = false;
  $("#ob-pick").hidden = true;
  $("#ob-ap").hidden = true;
}

$("#ob-rejoin").onclick = () => { $("#ob-done").hidden = true; showPicker(orbIP); };
$("#ob-manage").onclick = () => { show("manage"); $("#mn-ip").value = orbIP; scan(); };

$("#ob-ap-here").onclick = () => showPicker($("#j-ip").value.trim() || "192.168.4.1");
$("#j-rescan").onclick = () => loadNetworks(true);

// ---- wired (serial) provisioning door ---------------------------------------
// The boot scan the flash captured off the console arrives here as res.networks;
// it is the ONLY network list available when the hub is wired-only and cannot
// reach the AP's 192.168.4.1. "Provision over USB" reboots the orb into its app
// and writes one PROV line on the held console (server-side /api/provision); the
// firmware writes its own NVS keys and joins. The orb then comes up as a station
// and the normal LAN phase takes over.
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
    o.textContent = `${n.ssid}  (ch ${n.ch}, ${n.rssi} dBm${n.open ? ", open" : ""})`;
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

// showPicker revealed the SSID/password form; the AP list comes from the
// orb's own radio through the proxy. The polling shape mirrors the orb's
// built-in config page: /wifi/scan answers {"status":"scanning"} until the
// radio task has drained the driver, then the ready list.
let scanTimer = null;
async function loadNetworks(fresh) {
  const jlog = $("#j-log");
  if (!orbIP) { jlog.textContent = "no orb selected yet"; return; }
  const sel = $("#j-ssid");
  jlog.textContent = "scanning " + orbIP + "…";
  clearTimeout(scanTimer);
  try {
    const r = await fetch(`/orb/${orbIP}/api/v1/wifi/scan` + (fresh ? "?fresh=1" : ""));
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
  const ip = $("#j-ip").value.trim() || orbIP;
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
    const r = await fetch(`/orb/${ip}/api/v1/wifi/join`, {
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
      const r = await fetch(`/orb/${ip}/api/v1/status`);
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
  log.textContent += "\nstill not connected after 30 s — check Manage in a moment.";
};

// ---- manage ----------------------------------------------------------------
// The hub discovers nothing. The operator points it at one orb by the IP that
// USB provisioning reported (or one they look up on their router), and we dial
// that single address through the same-origin proxy — no ARP table, no sweep.
async function scan() {
  const list = $("#mn-list");
  const ip = $("#mn-ip").value.trim();
  if (!ip) { list.innerHTML = "<div class='hint'>enter an orb IP, then Load</div>"; return; }
  list.innerHTML = "<div class='hint'>loading " + ip + "\u2026</div>";
  try {
    const st = await readDeviceJSON(await fetch("/orb/" + encodeURIComponent(ip) + "/api/v1/status"));
    list.innerHTML = "";
    list.appendChild(renderOrb({ ip: ip, status: st }));
  } catch (e) {
    list.innerHTML = "<div class='hint err'>no orb answering at " + ip + " \u2014 " + e.message + "</div>";
  }
}

function renderOrb(o) {
  const node = $("#orb-tpl").content.cloneNode(true);
  const set = (sel, txt) => { const el = $(sel, node); el.textContent = txt; return el; };
  set(".ip", o.ip);
  set(".state", o.status.state);
  const st = o.status;
  const kv = $(".kv", node);
  const rows = [["fw", st.fw], ["idf", st.idf], ["slot", st.slot], ["img", st.img],
    ["rssi", st.rssi], ["tz", st.tz + " (" + st.tz_offset + "m)"], ["ap", st.ap],
    ["art", st.art_ready], ["synced", st.synced]];
  for (const [k, v] of rows) {
    if (v === undefined || v === null || v === "") continue;
    const dt = document.createElement("dt"); dt.textContent = k;
    const dd = document.createElement("dd"); dd.textContent = String(v);
    kv.append(dt, dd);
  }
  const out = $(".out", node);
  const ipEl = $(".ip", node).textContent;
  $(".tz", node).value = st.tz || "";

  $(".set-tz", node).onclick = () => postOrb(ipEl, "/api/v1/config", { tz: $(".tz", node).value }, out);
  $(".ap", node).onclick = () => postOrb(ipEl, "/api/v1/wifi/ap", { on: st.ap ? "off" : "on" }, out);
  $(".reboot", node).onclick = () => postOrb(ipEl, "/api/v1/reboot", {}, out);
  // The /orb proxy returns the device API's raw JSON (no hub {ok,data}
  // envelope), so it goes through readDeviceJSON, not getJSON — getJSON would
  // treat a valid status object as a failure for lacking an "ok" field.
  $(".refresh", node).onclick = async () => {
    try {
      const s = await readDeviceJSON(await fetch("/orb/" + ipEl + "/api/v1/status"));
      out.hidden = false; out.textContent = JSON.stringify(s, null, 2);
    } catch (e) { out.hidden = false; out.textContent = "\u2717 " + e.message; }
  };

  return node;
}

async function postOrb(ip, path, body, out) {
  out.hidden = false; out.textContent = "\u2192 POST " + path;
  try {
    const r = await fetch("/orb/" + ip + path, {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
    });
    out.textContent += "\n\u2190 " + (await r.text());
  } catch (e) { out.textContent += "\n\u2717 " + e.message; }
}

$("#mn-scan").onclick = scan;
$("#mn-ip").addEventListener("keydown", (e) => { if (e.key === "Enter") scan(); });

// ---- boot ------------------------------------------------------------------
route();
refreshPorts();
refreshBuild();

