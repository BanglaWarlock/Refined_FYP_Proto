/* FloodWatch showcase frontend.
 * Same-origin: nginx serves this site and proxies /api/* to the FastAPI app.
 * Live data arrives via SSE (/api/v1/events/stream) so alerts appear
 * without polling. */

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
const EVENT_ICON = {
  flood_level: "🌊", node_lost: "📡", node_offline: "📴", node_online: "✅",
  battery: "🔋", gps_signal_lost: "🛰️", gps_restored: "🛰️", node_announce: "📍",
  master_online: "🖥️", master_offline: "🖥️", topology: "🕸️", heartbeat: "💧",
};

/* ── Tabs ─────────────────────────────────────────────────────────── */
document.querySelectorAll(".tab").forEach((btn) => {
  btn.onclick = () => {
    document.querySelectorAll(".tab, .panel").forEach((x) => x.classList.remove("active"));
    btn.classList.add("active");
    $(`#${btn.dataset.tab}`).classList.add("active");
    if (btn.dataset.tab === "map") setTimeout(resizeMap, 50);
  };
});

/* ── Dashboard: stats + nodes + masters/topology ──────────────────── */
async function refreshStats() {
  try {
    const s = await (await fetch("/api/v1/stats")).json();
    $("#st-nodes").textContent   = s.nodes;
    $("#st-online").textContent  = s.nodes_online;
    $("#st-alerts").textContent  = s.alerts_24h;
    $("#st-hb").textContent      = s.heartbeats_1h;
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
  const nodes = await (await fetch("/api/v1/nodes")).json();
  const tb = $("#nodes-table tbody");
  tb.innerHTML = "";
  for (const n of nodes) {
    const lvl = n.water_level ?? 0;
    tb.append(el("tr", "", `
      <td><b>${esc(n.node_id)}</b></td>
      <td>${esc(n.village)}</td>
      <td><span class="pill ${n.online ? "on" : "off"}">${n.online ? "online" : "offline"}</span></td>
      <td><span class="pill L${lvl}">L${lvl} ${LEVEL_NAME[lvl]}</span></td>
      <td>${n.bat != null ? n.bat.toFixed(2) + " V" : "–"}</td>
      <td>${n.rssi != null ? `${n.rssi} dBm / ${Math.round(n.snr ?? 0)} dB` : "–"}</td>
      <td>${seenAgo(n.last_seen?.$date ?? n.last_seen)}</td>`));
  }
  updateMapMarkers(nodes);
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
  const topo = $("#topology");
  topo.textContent = villages
    .map((v) => v.topology ? renderTree(v.topology, 0) : "")
    .filter(Boolean).join("\n") || "No topology received yet.\n(The master publishes it on demand and on node changes.)";
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
    .filter(([k]) => !["deploy"].includes(k))
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
  es.onopen = () => setConn(true, "live");
  es.onerror = () => setConn(false, "reconnecting…");
  es.onmessage = (m) => {
    const e = JSON.parse(m.data);
    addEvent(e.type ?? "message", e.data ?? {}, e.ts);
    if (["heartbeat", "flood_level", "node_online", "node_offline"].includes(e.type)) {
      refreshStats(); refreshNodes();
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

/* ── Map ──────────────────────────────────────────────────────────── */
// Default view: Swinburne Sarawak — first fix recentres the map.
const map = L.map("map-canvas", { center: [1.5346, 110.3573], zoom: 15 });
L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
  { attribution: "© OpenStreetMap" }).addTo(map);

function resizeMap() { map.invalidateSize(); }

function updateMapMarkers(nodes) {
  map.eachLayer((l) => { if (l instanceof L.CircleMarker) map.removeLayer(l); });
  let any = false;
  for (const n of nodes) {
    // gps_fix=false means lat/lng are stale or 0,0 — never display (protocol rule)
    if (!n.gps_fix || n.lat == null || (n.lat === 0 && n.lng === 0)) continue;
    any = true;
    const lvl = n.water_level ?? 0;
    const color = lvl >= 3 ? "#f55d5d" : lvl === 2 ? "#f5883d" : lvl === 1 ? "#f5b83d" : "#35c48d";
    L.circleMarker([n.lat, n.lng], {
      radius: 9, color, fillColor: color, fillOpacity: .85, weight: 2,
    }).addTo(map)
      .bindPopup(`<b>${esc(n.node_id)}</b><br>${n.online ? "online" : "offline"} · level ${lvl} (${LEVEL_NAME[lvl]})<br>${n.bat?.toFixed(2)} V`);
  }
  if (any) map.fitBounds(L.featureGroup(Object.values(map._layers).filter(l => l instanceof L.CircleMarker)).getBounds(), { maxZoom: 16, padding: [40, 40] });
}

/* ── Boot ─────────────────────────────────────────────────────────── */
$("#foot-time").textContent = new Date().toDateString();
refreshStats(); refreshNodes(); refreshMasters();
connectSSE();
setInterval(refreshStats, 15000);
setInterval(() => { refreshNodes(); }, 20000);
