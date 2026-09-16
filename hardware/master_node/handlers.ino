// Registry handlers: discovery, registration, announce, heartbeat, alerts
// (with seq dedup + node_lost suppression), commands, topology.

// Topic prefix: floodwatch/<deploy>/<village> or floodwatch/<village>
void topic_base(char *out, size_t len) {
    if (deploy[0]) snprintf(out, len, "floodwatch/%s/%s", deploy, village);
    else           snprintf(out, len, "floodwatch/%s", village);
}

void publish_node_status(const char *node_id, bool online) {
    char base[64], topic[96], pl[160];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/nodes/%s/status", base, node_id);
    snprintf(pl, sizeof(pl),
             "{\"node_id\":\"%s\",\"village\":\"%s\",\"online\":%s}",
             node_id, village, online ? "true" : "false");
    enqueueMqtt(String(topic), String(pl));
    Serial.printf("[STATUS] %s %s\n", node_id, online ? "ONLINE" : "OFFLINE");
}

registered_node *find_node(const char *id) {
    for (uint8_t i = 0; i < node_count; i++)
        if (strcmp(node_registry[i].node_id, id) == 0) return &node_registry[i];
    return nullptr;
}

// Any packet from a node proves it alive — clear any in-flight liveness probe
static void mark_alive(registered_node *n) {
    n->last_seen_ms  = millis();
    n->probe_sent_ms = 0;
    n->probe_tries   = 0;
}

// Liveness probe for a timed-out child: reuse the command path — the target
// ACKs MSG_CMD (data=ping), and CMD_ACK clears the probe (PROTOCOL §5).
void send_cmd_ping(const char *target) {
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *rn = find_node(target);
    char next_hop[NODE_ID_MAX_LEN] = "";
    if (rn) strlcpy(next_hop, rn->depth == 1 ? rn->node_id : rn->parent_id, NODE_ID_MAX_LEN);
    xSemaphoreGive(registry_mutex);
    if (!next_hop[0]) return;
    char pload[PACKET_MAX_LEN], raw[PACKET_MAX_LEN];
    snprintf(pload, sizeof(pload), "target=%s,data=ping", target);
    format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, next_hop, MSG_CMD, pload);
    enqueueLora(String(raw));
    Serial.printf("[PROBE] ping → %s via %s\n", target, next_hop);
}

static registered_node *get_or_create(const char *id) {
    registered_node *n = find_node(id);
    if (n) return n;
    if (node_count >= MAX_REGISTERED_NODES) return nullptr;
    n = &node_registry[node_count++];
    memset(n, 0, sizeof(*n));
    strlcpy(n->node_id, id, NODE_ID_MAX_LEN);
    return n;
}

void handle_discover(const lora_packet *pkt) {
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    uint8_t other_online  = 0;
    bool    is_known_child = false;
    for (uint8_t i = 0; i < node_count; i++) {
        if (strcmp(node_registry[i].parent_id, own_node_id) != 0) continue;
        if (strcmp(node_registry[i].node_id, pkt->src_id)   == 0) { is_known_child = true; continue; }
        if (node_registry[i].is_online) other_online++;
    }
    xSemaphoreGive(registry_mutex);
    if (!is_known_child && other_online >= max_children) return;
    vTaskDelay(pdMS_TO_TICKS(50 + esp_random() % 200));
    char payload[64], raw[PACKET_MAX_LEN];
    snprintf(payload, sizeof(payload), "id=%s,depth=0,cur=%u,max=%u",
             own_node_id, other_online, max_children);
    format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, pkt->src_id, MSG_DISC_RESP, payload);
    enqueueLora(String(raw));
    Serial.printf("[DISC] DISC_RESP → %s\n", pkt->src_id);
}

void handle_reg_req(const lora_packet *pkt) {
    char claimed[NODE_ID_MAX_LEN];
    // hard-validate the claimed id — FIFO residue can glue garbage into the
    // payload mid-transport; a corrupt id must never enter the registry
    if (!get_field(pkt->payload, "id", claimed, sizeof(claimed)) || !id_valid(claimed)) {
        Serial.printf("[REG] Dropped — bad id from %s\n", pkt->src_id);
        return;
    }

    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *existing = find_node(claimed);
    bool is_known_child = (existing && strcmp(existing->parent_id, own_node_id) == 0);
    uint8_t other_online = 0;
    for (uint8_t i = 0; i < node_count; i++) {
        if (strcmp(node_registry[i].parent_id, own_node_id) != 0) continue;
        if (strcmp(node_registry[i].node_id,   claimed)      == 0) continue;
        if (node_registry[i].is_online) other_online++;
    }
    if (!is_known_child && other_online >= max_children) {
        xSemaphoreGive(registry_mutex);
        Serial.printf("[REG] Dropped %s — at capacity\n", claimed);
        return;
    }
    registered_node *n = existing;
    if (!n && node_count < MAX_REGISTERED_NODES) {
        n = &node_registry[node_count++];
        memset(n, 0, sizeof(*n));
        strlcpy(n->node_id, claimed, NODE_ID_MAX_LEN);
    }
    if (n) {
        strlcpy(n->parent_id, own_node_id, NODE_ID_MAX_LEN);
        n->depth = 1; n->is_online = false;
        n->probe_sent_ms = 0; n->probe_tries = 0;   // fresh registration — cancel probes
    }
    xSemaphoreGive(registry_mutex);

    if (!n) {
        Serial.printf("[REG] Dropped %s — registry full\n", claimed);
        return;
    }

    char payload[80], raw[PACKET_MAX_LEN];
    snprintf(payload, sizeof(payload), "id=%s,parent=%s,depth=1", claimed, own_node_id);
    format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, pkt->src_id, MSG_REG_ACK, payload);
    enqueueLora(String(raw));
    Serial.printf("[REG] ACK %s for %s\n", claimed, pkt->src_id);
}

void handle_announce(const lora_packet *pkt) {
    uint8_t depth = 0; char parent[NODE_ID_MAX_LEN] = "";
    double lat = 0.0, lng = 0.0;
    bool came_online = false;

    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *n = get_or_create(pkt->src_id);
    if (n) {
        came_online = !n->is_online;
        mark_alive(n);
        n->last_announce_ms = millis();
        n->is_online = true;
        n->snr = pkt->snr; n->rssi = pkt->rssi;
        char buf[32];
        if (get_field(pkt->payload, "depth",  buf, sizeof(buf))) n->depth = (uint8_t)atoi(buf);
        if (get_field(pkt->payload, "parent", buf, sizeof(buf))) strlcpy(n->parent_id, buf, NODE_ID_MAX_LEN);
        if (get_field(pkt->payload, "lat",    buf, sizeof(buf))) n->lat = atof(buf);
        if (get_field(pkt->payload, "lng",    buf, sizeof(buf))) n->lng = atof(buf);
        depth = n->depth; strlcpy(parent, n->parent_id, sizeof(parent));
        lat = n->lat; lng = n->lng;
    }
    xSemaphoreGive(registry_mutex);

    // ACK the announce so the sender stops retrying
    char ack_pl[28], ack[PACKET_MAX_LEN];
    snprintf(ack_pl, sizeof(ack_pl), "ack_id=%016llX", (unsigned long long)pkt->msg_id);
    format_packet(ack, sizeof(ack), new_msg_id(), own_node_id, pkt->src_id, MSG_ACK, ack_pl);
    enqueueLora(String(ack));

    if (came_online) publish_node_status(pkt->src_id, true);
    publishTopology();   // reparents also change topology

    char base[64], topic[96], pl[320];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/announce/%s", base, pkt->src_id);
    snprintf(pl, sizeof(pl),
             "{\"node_id\":\"%s\",\"village\":\"%s\",\"depth\":%u,\"parent\":\"%s\","
             "\"lat\":%.6f,\"lng\":%.6f,\"snr\":%.1f,\"rssi\":%d}",
             pkt->src_id, village, depth, parent, lat, lng, pkt->snr, pkt->rssi);
    enqueueMqtt(String(topic), String(pl));
    Serial.printf("[ANNOUNCE] %s depth=%u parent=%s lat=%.6f lng=%.6f\n",
                  pkt->src_id, depth, parent, lat, lng);
}

void handle_hb_sensor(const lora_packet *pkt) {
    uint8_t depth = 0; char parent[NODE_ID_MAX_LEN] = "";
    float bat = 0.0f; uint8_t fb = 0; double lat = 0.0, lng = 0.0; bool gps_fix = false;
    float link_snr = pkt->snr; int link_rssi = pkt->rssi;
    bool came_online = false;

    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *n = get_or_create(pkt->src_id);
    if (n) {
        came_online = !n->is_online;
        mark_alive(n);
        n->is_online = true;
        char buf[32];
        if (get_field(pkt->payload, "lsnr",  buf, sizeof(buf))) n->snr  = atof(buf);
        if (get_field(pkt->payload, "lrssi", buf, sizeof(buf))) n->rssi = atoi(buf);
        if (get_field(pkt->payload, "bat",        buf, sizeof(buf))) n->battery_voltage = atof(buf);
        if (get_field(pkt->payload, "float_bits", buf, sizeof(buf))) n->float_bits = (uint8_t)atoi(buf);
        if (get_field(pkt->payload, "seq",        buf, sizeof(buf))) {
            uint32_t seq = (uint32_t)strtoul(buf, nullptr, 10);
            if (n->pkt_rx == 0) n->last_seq = seq;
            else if (seq > n->last_seq) n->pkt_lost += seq - n->last_seq - 1;
            n->last_seq = seq; n->pkt_rx++;
        }
        // depth/parent only before first announce — announce is authoritative
        if (n->last_announce_ms == 0) {
            if (get_field(pkt->payload, "depth",  buf, sizeof(buf))) n->depth = (uint8_t)atoi(buf);
            if (get_field(pkt->payload, "parent", buf, sizeof(buf))) strlcpy(n->parent_id, buf, NODE_ID_MAX_LEN);
        }
        if (get_field(pkt->payload, "lat",     buf, sizeof(buf))) n->lat = atof(buf);
        if (get_field(pkt->payload, "lng",     buf, sizeof(buf))) n->lng = atof(buf);
        if (get_field(pkt->payload, "gps_fix", buf, sizeof(buf))) n->gps_fix = atoi(buf) != 0;
        depth = n->depth; strlcpy(parent, n->parent_id, sizeof(parent));
        bat = n->battery_voltage; fb = n->float_bits;
        lat = n->lat; lng = n->lng; gps_fix = n->gps_fix;
        link_snr = n->snr; link_rssi = n->rssi;
    }
    xSemaphoreGive(registry_mutex);

    // ACK the heartbeat — confirms the master is alive to the child/relay
    char ack_pl[28], ack[PACKET_MAX_LEN];
    snprintf(ack_pl, sizeof(ack_pl), "ack_id=%016llX", (unsigned long long)pkt->msg_id);
    format_packet(ack, sizeof(ack), new_msg_id(), own_node_id, pkt->src_id, MSG_ACK, ack_pl);
    enqueueLora(String(ack));

    char base[64], topic[96], pl[320];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/heartbeat/%s", base, pkt->src_id);
    snprintf(pl, sizeof(pl),
             "{\"node_id\":\"%s\",\"village\":\"%s\",\"bat\":%.2f,\"float_bits\":%u,"
             "\"depth\":%u,\"parent\":\"%s\",\"lat\":%.6f,\"lng\":%.6f,\"gps_fix\":%s,"
             "\"snr\":%.1f,\"rssi\":%d}",
             pkt->src_id, village, bat, fb, depth, parent, lat, lng,
             gps_fix ? "true" : "false", link_snr, link_rssi);
    enqueueMqtt(String(topic), String(pl));

    if (came_online) {
        publish_node_status(pkt->src_id, true);
        publishTopology();
    }
}

void handle_alert(const lora_packet *pkt) {
    // id= in payload is the originating node; src_id is the delivering hop
    char origin[NODE_ID_MAX_LEN];
    if (!get_field(pkt->payload, "id", origin, sizeof(origin)) || !id_valid(origin))
        strlcpy(origin, pkt->src_id, sizeof(origin));
    char atype[16] = "unknown";
    get_field(pkt->payload, "type", atype, sizeof(atype));
    uint32_t aseq = payload_seq(pkt->payload);

    bool stale = false;
    bool origin_gps_fix = false;
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *relay = find_node(pkt->src_id);
    if (relay) mark_alive(relay);
    registered_node *orig = find_node(origin);
    if (orig) {
        mark_alive(orig);
        origin_gps_fix = orig->gps_fix;
        if (aseq) {
            if (orig->last_alert_seq && aseq <= orig->last_alert_seq) stale = true;
            else orig->last_alert_seq = aseq;   // per-origin FIFO TX ⇒ seqs arrive in order
        }
    }
    xSemaphoreGive(registry_mutex);

    // ACK the delivering hop regardless — its retry must stop at this hop
    char ack_pl[28], ack[PACKET_MAX_LEN];
    snprintf(ack_pl, sizeof(ack_pl), "ack_id=%016llX", (unsigned long long)pkt->msg_id);
    format_packet(ack, sizeof(ack), new_msg_id(), own_node_id, pkt->src_id, MSG_ACK, ack_pl);
    enqueueLora(String(ack));

    if (stale) {
        Serial.printf("[ALERT] stale seq=%lu from %s type=%s — absorbed\n",
                      (unsigned long)aseq, origin, atype);
        return;
    }

    // node_lost suppression (PROTOCOL §7B): if the master has seen the lost
    // node more recently than the relay detected the loss, skip the offline
    // status publish — the node re-announced through another branch.
    char   lost_id[NODE_ID_MAX_LEN] = "";
    uint32_t down_s = 0;
    bool   suppress_offline = false;
    if (strcmp(atype, "node_lost") == 0) {
        char ds[12] = "";
        get_field(pkt->payload, "lost",   lost_id, sizeof(lost_id));
        if (get_field(pkt->payload, "down_s", ds, sizeof(ds))) down_s = strtoul(ds, nullptr, 10);
        xSemaphoreTake(registry_mutex, portMAX_DELAY);
        registered_node *lost = find_node(lost_id);
        uint32_t detect_ms = millis() - down_s * 1000UL;
        if (lost && lost->is_online && lost->last_seen_ms + 2000 >= detect_ms)
            suppress_offline = true;
        xSemaphoreGive(registry_mutex);
    }

    // fields (boundary-safe parsing; lat= would otherwise match last_lat=)
    uint8_t level = 0, fb = 0;
    float bat = 0.0f, dist = 0.0f;
    double lat = 0.0, lng = 0.0, home_lat = 0.0, home_lng = 0.0;
    char buf[24];
    if (get_field(pkt->payload, "level",      buf, sizeof(buf))) level    = (uint8_t)atoi(buf);
    if (get_field(pkt->payload, "float_bits", buf, sizeof(buf))) fb       = (uint8_t)strtol(buf, nullptr, 0);
    if (get_field(pkt->payload, "bat",        buf, sizeof(buf))) bat      = atof(buf);
    if (get_field(pkt->payload, "dist",       buf, sizeof(buf))) dist     = atof(buf);
    if (get_field(pkt->payload, "lat",        buf, sizeof(buf))) lat      = atof(buf);
    if (get_field(pkt->payload, "lng",        buf, sizeof(buf))) lng      = atof(buf);
    if (get_field(pkt->payload, "home_lat",   buf, sizeof(buf))) home_lat = atof(buf);
    if (get_field(pkt->payload, "home_lng",   buf, sizeof(buf))) home_lng = atof(buf);

    // prefer injected leaf→relay link quality
    float link_snr = pkt->snr; int link_rssi = pkt->rssi;
    if (get_field(pkt->payload, "lsnr",  buf, sizeof(buf))) link_snr  = atof(buf);
    if (get_field(pkt->payload, "lrssi", buf, sizeof(buf))) link_rssi = atoi(buf);

    char extra[96] = "";
    if (strcmp(atype, "node_lost") == 0)
        snprintf(extra, sizeof(extra), ",\"lost\":\"%s\",\"down_s\":%lu", lost_id, (unsigned long)down_s);

    char base[64], topic[96], pl[448];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/alert/%s", base, origin);
    snprintf(pl, sizeof(pl),
             "{\"node_id\":\"%s\",\"village\":\"%s\",\"type\":\"%s\",\"seq\":%lu,\"level\":%u,"
             "\"float_bits\":%u,\"bat\":%.2f,\"dist\":%.1f,\"lat\":%.6f,\"lng\":%.6f,"
             "\"home_lat\":%.6f,\"home_lng\":%.6f,\"gps_fix\":%s,\"snr\":%.1f,\"rssi\":%d%s}",
             origin, village, atype, (unsigned long)aseq, level, fb, bat, dist, lat, lng,
             home_lat, home_lng, origin_gps_fix ? "true" : "false", link_snr, link_rssi, extra);
    enqueueMqtt(String(topic), String(pl));

    if (strcmp(atype, "node_lost") == 0) {
        if (suppress_offline)
            Serial.printf("[ALERT] node_lost %s suppressed — seen more recently\n", lost_id);
        else if (lost_id[0])
            publish_node_status(lost_id, false);
    }
    Serial.printf("[ALERT] %s (via %s) type=%s seq=%lu level=%u\n",
                  origin, pkt->src_id, atype, (unsigned long)aseq, level);
}

void handle_cmd_ack(const lora_packet *pkt) {
    // a CMD_ACK (including our liveness pings) proves the node alive
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *n = find_node(pkt->src_id);
    if (n) mark_alive(n);
    xSemaphoreGive(registry_mutex);

    xSemaphoreTake(pending_cmd_mutex, portMAX_DELAY);
    for (int i = (int)pending_cmds.size() - 1; i >= 0; i--) {
        if (strcmp(pending_cmds[i].target, pkt->src_id) == 0) {
            Serial.printf("[CMD] ACK from %s after %u attempt(s)\n",
                          pkt->src_id, pending_cmds[i].attempts);
            pending_cmds.erase(pending_cmds.begin() + i);
            break;
        }
    }
    xSemaphoreGive(pending_cmd_mutex);

    char base[64], topic[96], pl[256];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/cmd_ack/%s", base, pkt->src_id);
    snprintf(pl, sizeof(pl),
             "{\"node_id\":\"%s\",\"village\":\"%s\",\"data\":\"%s\"}",
             pkt->src_id, village, pkt->payload);
    enqueueMqtt(String(topic), String(pl));
}

// Called with registry_mutex held; recurses to build the topology tree JSON.
static int buildTopoNode(char *buf, size_t buf_len, int pos, const char *node_id) {
    pos += snprintf(buf + pos, buf_len - pos, "{");
    bool first = true;
    for (uint8_t i = 0; i < node_count; i++) {
        if (strcmp(node_registry[i].parent_id, node_id) != 0) continue;
        if (!node_registry[i].is_online) continue;
        pos += snprintf(buf + pos, buf_len - pos, "%s\"%s\":",
                        first ? "" : ",", node_registry[i].node_id);
        first = false;
        pos = buildTopoNode(buf, buf_len, pos, node_registry[i].node_id);
    }
    pos += snprintf(buf + pos, buf_len - pos, "}");
    return pos;
}

void publishTopology() {
    size_t buf_len = node_count * 60 + 256;
    char *buf = (char *)malloc(buf_len);
    if (!buf) { Serial.println("[TOPO] malloc failed"); return; }

    int pos = snprintf(buf, buf_len, "{\"%s\":", own_node_id);
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    pos = buildTopoNode(buf, buf_len, pos, own_node_id);
    xSemaphoreGive(registry_mutex);
    snprintf(buf + pos, buf_len - pos, "}");

    char base[64], topic[96];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/topology", base);
    enqueueMqtt(String(topic), String(buf));
    free(buf);
    Serial.println("[TOPO] queued");
}

void processCmd(const String& msg) {
    if (msg.indexOf("topology") >= 0) {
        publishTopology();
        char raw[PACKET_MAX_LEN];
        format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, "ALL", MSG_TOPO_REQ, "");
        enqueueLora(String(raw));
        return;
    }

    // {"cmd":"send","target":"VILLAGE-001","data":"recalibrate_gps"}
    int ti = msg.indexOf("\"target\"");
    int di = msg.indexOf("\"data\"");
    if (ti < 0 || di < 0) return;
    String target = "", data = "";
    int qs, qe;
    qs = msg.indexOf('"', ti + 9); qe = msg.indexOf('"', qs + 1);
    if (qs >= 0 && qe > qs) target = msg.substring(qs + 1, qe);
    qs = msg.indexOf('"', di + 7); qe = msg.indexOf('"', qs + 1);
    if (qs >= 0 && qe > qs) data = msg.substring(qs + 1, qe);
    if (!target.length() || !data.length()) return;

    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *rn = find_node(target.c_str());
    char next_hop[NODE_ID_MAX_LEN] = "";
    if (rn) strlcpy(next_hop, rn->depth == 1 ? rn->node_id : rn->parent_id, NODE_ID_MAX_LEN);
    xSemaphoreGive(registry_mutex);

    if (!next_hop[0]) { Serial.printf("[CMD] %s not found\n", target.c_str()); return; }

    pending_cmd_t cmd = {};
    strlcpy(cmd.target,   target.c_str(), sizeof(cmd.target));
    strlcpy(cmd.next_hop, next_hop,       sizeof(cmd.next_hop));
    snprintf(cmd.pload, sizeof(cmd.pload), "target=%s,data=%s", target.c_str(), data.c_str());
    cmd.last_sent_ms = millis();
    cmd.attempts     = 1;

    xSemaphoreTake(pending_cmd_mutex, portMAX_DELAY);
    pending_cmds.push_back(cmd);
    xSemaphoreGive(pending_cmd_mutex);

    char raw[PACKET_MAX_LEN];
    format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, next_hop, MSG_CMD, cmd.pload);
    enqueueLora(String(raw));
    Serial.printf("[CMD] attempt 1 → %s via %s\n", target.c_str(), next_hop);
}
