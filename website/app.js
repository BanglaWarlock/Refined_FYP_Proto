/* FloodWatch showcase frontend.
 * Served by Caddy (static + /api proxy, same origin — no CORS).
 * Map is the default tab; clicking a node opens its live stats panel.
 * Edges connect each node to its parent where both have GPS fixes
 * (the master has no GPS, so child→master links are not drawn). */

const $  = (s) => document.querySelector(s);
const el = (tag, cls, html) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (html !== undefined) e.innerHTML = html;
  return e;
};
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) =>
  ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

const LEVEL_NAME = ["dry", "1 ft", "2 ft", "3 ft"];
const LEVEL_COLOR = ["#94a3b8", "#f5b83d", "#f5883d", "#f55d5d"];
const EVENT_ICON = {
  flood_level: "🌊", node_lost: "📡", node_offline: "📴", node_online: "✅",
  battery: "🔋", gps_signal_lost: "🛰️", gps_restored: "🛰️", node_announce: "📍",
  master_online: "🖥️", master_offline: "🖥️", topology: "🕸️", heartbeat: "💧",
};

/* ── Theme ────────────────────────────────────────────────────────── */
const themeBtn = $("#theme-btn");
function applyTheme(t) {
  document.body.classList.toggle("dark", t === "dark");
  themeBtn.textContent = t === "dark" ? "☀️" : "🌙";
  localStorage.setItem("fw-theme", t);
}
applyTheme(localStorage.getItem("fw-theme") || "light");
themeBtn.onclick = () =>
  applyTheme(document.body.classList.contains("dark") ? "light" : "dark");

/* ── Tabs ─────────────────────────────────────────────────────────── */
document.querySelectorAll(".tab").forEach((btn) => {
  btn.onclick = () => {
    document.querySelectorAll(".tab, .panel").forEach((x) => x.classList.remove("active"));
    btn.classList.add("active");
    $(`#${btn.dataset.tab}`).classList.add("active");
    if (btn.dataset.tab === "map") setTimeout(() => map.invalidateSize(), 50);
  };
});

/* ── Map: markers, parent edges, node selection ───────────────────── */
// Default view: Swinburne Sarawak — first fix recentres.
const map = L.map("map-canvas", { center: [1.5346, 110.3573], zoom: 15 });
L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
  { attribution: "© OpenStreetMap" }).addTo(map);

const edgeLayer  = L.layerGroup().addTo(map);
const nodeLayer  = L.layerGroup().addTo(map);   // added after edges → drawn on top
const markers    = new Map();                   // node_id -> marker
let   nodesCache = [];
let   selectedId = null;

const hasPos = (n) => n.gps_fix && n.lat != null && !(n.lat === 0 && n.lng === 0);

function markerColor(n) {
  return n.online ? LEVEL_COLOR[n.water_level ?? 0] : "#94a3b8";
}

function redrawMap() {
  nodeLayer.clearLayers(); markers.clear();
  edgeLayer.clearLayers();
  const byId = new Map(nodesCache.map((n) => [n.node_id, n]));

  // edges: child → parent (only where both ends have a GPS fix; the master
  // has no GPS module so child→master links stay undrawn by design)
  for (const n of nodesCache) {
    if (!hasPos(n) || !n.parent) continue;
    const p = byId.get(n.parent);
    if (!p || !hasPos(p)) continue;
    L.polyline([[n.lat, n.lng], [p.lat, p.lng]], {
      color: n.online ? "#1670d8" : "#94a3b8",
      weight: 2, opacity: .65, dashArray: n.online ? null : "6 6",
    }).addTo(edgeLayer);
  }

  for (const n of nodesCache) {
    if (!hasPos(n)) continue;
    const sel = n.node_id === selectedId;
    const m = L.circleMarker([n.lat, n.lng], {
      radius: sel ? 12 : 9, color: sel ? "#ffffff" : markerColor(n),
      fillColor: markerColor(n), fillOpacity: .9, weight: sel ? 4 : 2,
    }).addTo(nodeLayer);
    m.bindTooltip(`${n.node_id} · L${n.water_level ?? 0}`);
    m.on("click", () => selectNode(n.node_id));
    markers.set(n.node_id, m);
  }
}

function fitMap() {
  const pts = nodesCache.filter(hasPos).map((n) => [n.lat, n.lng]);
  if (pts.length) map.fitBounds(L.latLngBounds(pts).pad(.25), { maxZoom: 16 });
}

/* ── Node side panel ──────────────────────────────────────────────── */
async function selectNode(nodeId) {
  selectedId = nodeId;
  redrawMap();
  const panel = $("#node-panel");
  panel.classList.remove("hidden");
  $("#np-title").textContent = nodeId;
  $("#np-body").innerHTML = `<span class="hint">loading…</span>`;

  let n, readings;
  try {
    [n, readings] = await Promise.all([
      (await fetch(`/api/v1/nodes/${encodeURIComponent(nodeId)}`)).json(),
      (await fetch(`/api/v1/nodes/${encodeURIComponent(nodeId)}/readings?limit=120`)).json(),
    ]);
  } catch { $("#np-body").innerHTML = `<span class="hint">failed to load</span>`; return; }
  if (selectedId !== nodeId) return;   // user picked another node meanwhile

  const lvl = n.water_level ?? 0;
  const row = (k, v) => `<div class="np-row"><span class="k">${k}</span><span>${v}</span></div>`;
  $("#np-body").innerHTML = `
    <div class="np-row"><span class="k">status</span>
      <span class="pill ${n.online ? "on" : "off"}">${n.online ? "online" : "offline"}</span></div>
    <div class="np-row"><span class="k">water level</span>
      <span class="pill L${lvl}">L${lvl} ${LEVEL_NAME[lvl]}</span></div>
    ${row("battery", n.bat != null ? n.bat.toFixed(2) + " V" : "–")}
    ${row("link (leaf→parent)", n.rssi != null ? `${n.rssi} dBm · SNR ${Math.round(n.snr ?? 0)} dB` : "–")}
    ${row("mesh depth", n.depth ?? "–")}
    ${row("parent", esc(n.parent ?? "–"))}
    ${row("last seen", seenAgo(n.last_seen?.$date ?? n.last_seen))}
    ${row("position", hasPos(n) ? `${n.lat.toFixed(5)}, ${n.lng.toFixed(5)}` : "no fix")}
    <div class="np-sec">battery (last readings)</div>
    <canvas class="spark" id="spark-bat"></canvas>
    <div class="np-sec">water level (last readings)</div>
    <canvas class="spark" id="spark-lvl"></canvas>`;

  drawSpark("spark-bat", readings.map((r) => r.bat).filter((v) => v != null),
            { color: "#149663", unit: " V", min: 10, max: 13 });
  drawSpark("spark-lvl", readings.map((r) => r.water_level ?? waterLevelOfBits(r.float_bits)),
            { color: "#d43a3a", step: true, max: 3 });
}

function deselectNode() {
  selectedId = null;
  $("#node-panel").classList.add("hidden");
  redrawMap();
}
$("#np-close").onclick = deselectNode;

const waterLevelOfBits = (bits) =>
  (bits & 4) ? 3 : (bits & 2) ? 2 : (bits & 1) ? 1 : 0;

/* Tiny sparkline on a canvas — battery as a line, level as steps. */
function drawSpark(id, values, { color, step = false, unit = "", min, max }) {
  const cv = document.getElementById(id);
  if (!cv) return;
  const W = cv.width = cv.offsetWidth * 2, H = cv.height = 112;
  const ctx = cv.getContext("2d");
  ctx.scale(1, 1);
  const lo = min ?? Math.min(...values), hi = max ?? Math.max(...values);
  const pad = (hi - lo) * 0.1 || 0.5;
  const y = (v) => H - 8 - ((v - lo) / (hi - lo + pad)) * (H - 16);
  ctx.clearRect(0, 0, W, H);
  if (!values.length) {
    ctx.fillStyle = "#8294b4"; ctx.font = "20px sans-serif";
    ctx.fillText("no data", 8, H / 2); return;
  }
  const x = (i) => (i / Math.max(1, values.length - 1)) * (W - 8) + 4;
  ctx.strokeStyle = color; ctx.lineWidth = 3; ctx.beginPath();
  if (step) {
    values.forEach((v, i) => {
      if (i === 0) ctx.moveTo(x(0), y(v));
      else { ctx.lineTo(x(i), y(values[i - 1])); ctx.lineTo(x(i), y(v)); }
    });
  } else {
    values.forEach((v, i) => (i ? ctx.lineTo(x(i), y(v)) : ctx.moveTo(x(0), y(v))));
  }
  ctx.stroke();
  ctx.fillStyle = "#8294b4"; ctx.font = "18px sans-serif";
  ctx.fillText(`${values[values.length - 1]}${unit}`, W - 70, 20);
}

/* ── Dashboard: stats + nodes + masters/topology ──────────────────── */
async function refreshStats() {
  try {
    const s = await (await fetch("/api/v1/stats")).json();
    $("#st-nodes").textContent    = s.nodes;
    $("#st-online").textContent   = s.nodes_online;
    $("#st-alerts").textContent   = s.alerts_24h;
    $("#st-hb").textContent       = s.heartbeats_1h;
    $("#st-villages").textContent = s.villages;
  } catch { setConn(false, "API unreachable"); }
}

function seenAgo(iso) {
  if (!iso) return "–";
  const s = Math.max(0, Math.round((Date.now() - new Date(iso)) / 1000));
  if (s < 60) return `${s}s ago`;
  if (s < 3600) return `${Math.round(s / 60)}m ago`;
  return `${Math.round(s / 3600)}h ago`;
}

async function refreshNodes() {
  nodesCache = await (await fetch("/api/v1/nodes")).json();
  const tb = $("#nodes-table tbody");
  tb.innerHTML = "";
  for (const n of nodesCache) {
    const lvl = n.water_level ?? 0;
    const tr = el("tr", "", `
      <td><b>${esc(n.node_id)}</b></td>
      <td>${esc(n.village)}</td>
      <td><span class="pill ${n.online ? "on" : "off"}">${n.online ? "online" : "offline"}</span></td>
      <td><span class="pill L${lvl}">L${lvl} ${LEVEL_NAME[lvl]}</span></td>
      <td>${n.bat != null ? n.bat.toFixed(2) + " V" : "–"}</td>
      <td>${n.rssi != null ? `${n.rssi} dBm / ${Math.round(n.snr ?? 0)} dB` : "–"}</td>
      <td>${seenAgo(n.last_seen?.$date ?? n.last_seen)}</td>`);
    tr.style.cursor = "pointer";
    tr.onclick = () => { selectNode(n.node_id); document.querySelector('[data-tab="map"]').click(); };
    tb.append(tr);
  }
  redrawMap();
  if (selectedId) {   // keep the side panel fresh with live data
    const n = nodesCache.find((x) => x.node_id === selectedId);
    if (n) $("#np-title").textContent = n.node_id;
  }
}

async function refreshMasters() {
  const masters = await (await fetch("/api/v1/masters")).json();
  $("#masters").innerHTML = masters.length
    ? masters.map((m) => `
      <div class="master-row">
        <span><b>${esc(m.node_id ?? "master")}</b> · ${esc(m.village)}</span>
        <span class="pill ${m.online ? "on" : "off"}">${m.online ? "online" : "offline"}</span>
      </div>`).join("")
    : `<span class="hint">No master has connected yet.</span>`;

  const villages = await (await fetch("/api/v1/villages")).json();
  $("#topology").textContent = villages
    .map((v) => v.topology ? renderTree(v.topology, 0) : "")
    .filter(Boolean).join("\n") || "No topology received yet.\n(The master publishes it on node changes.)";
}

function renderTree(node, depth) {
  return Object.entries(node)
    .map(([id, children]) =>
      "  ".repeat(depth) + (depth ? "└─ " : "") + id +
      (Object.keys(children).length ? "\n" + renderTree(children, depth + 1) : ""))
    .join("\n");
}

/* ── Live event feed (SSE) ────────────────────────────────────────── */
function addEvent(type, data, ts) {
  const feed = $("#feed");
  const when = ts ? new Date(ts).toLocaleTimeString() : new Date().toLocaleTimeString();
  const icon = EVENT_ICON[type] ?? "•";
  const detail = Object.entries(data)
    .filter(([k]) => k !== "deploy")
    .slice(0, 6)
    .map(([k, v]) => `${esc(k)}=${esc(typeof v === "number" ? Math.round(v * 1000) / 1000 : v)}`)
    .join("  ");
  const row = el("div", `evt ${type}`, `
    <div class="t">${icon} ${esc(type)} · ${when}</div>
    <div><b>${esc(data.node_id ?? data.village ?? "")}</b> ${detail ? `<span class="hint">${detail}</span>` : ""}</div>`);
  feed.prepend(row);
  while (feed.children.length > 200) feed.lastChild.remove();
}

function connectSSE() {
  const es = new EventSource("/api/v1/events/stream");
  es.onopen  = () => setConn(true, "live");
  es.onerror = () => setConn(false, "reconnecting…");
  es.onmessage = (m) => {
    const e = JSON.parse(m.data);
    addEvent(e.type ?? "message", e.data ?? {}, e.ts);
    if (["heartbeat", "flood_level", "node_online", "node_offline"].includes(e.type)) {
      refreshStats(); refreshNodes();
      if (e.data?.node_id === selectedId) selectNode(selectedId);
    }
    if (["node_announce", "topology", "node_lost"].includes(e.type)) {
      refreshNodes(); refreshMasters();
    }
  };
}

function setConn(live, text) {
  $("#conn-badge").className = "conn " + (live ? "live" : "dead");
  $("#conn-text").textContent = text;
}

/* ── Boot ─────────────────────────────────────────────────────────── */
$("#foot-time").textContent = new Date().toDateString();
refreshStats(); refreshNodes(); refreshMasters();
connectSSE();
setInterval(refreshStats, 15000);
setInterval(refreshNodes, 20000);
