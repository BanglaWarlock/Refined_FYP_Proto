// Application handlers: discovery/registration (reconnect-friendly), live
// child tracking, reliable seq-deduped alerts, topology, health timeouts.

// Topic prefix: floodwatch/<deploy>/<village> or floodwatch/<village>
// (topic_base lives in lora.ino)

void publish_node_status(const char *node_id, bool online) {
    char base[64], topic[96], pl[160];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/nodes/%s/status", base, node_id);
    snprintf(pl, sizeof(pl),
             "{\"node_id\":\"%s\",\"village\":\"%s\",\"online\":%s}",
             node_id, VILLAGE, online ? "true" : "false");
    enqueueMqtt(String(topic), String(pl));
    Serial.printf("[STATUS] %s %s\n", node_id, online ? "ONLINE" : "OFFLINE");
}

registered_node *find_node(const char *id) {
    for (uint8_t i = 0; i < node_count; i++)
        if (strcmp(node_registry[i].node_id, id) == 0) return &node_registry[i];
    return nullptr;
}

// Any frame from a child proves it alive — flips it online and clears the
// offline clock. This is what makes reconnects "just work": registration,
// announce, heartbeat and alerts all funnel through here.
// Returns true if the node transitioned OFFLINE → ONLINE with this frame.
static bool mark_alive(registered_node *n) {
    bool was_online = n->is_online;
    n->last_seen_ms = millis();
    n->is_online    = true;
    return !was_online;
}

static registered_node *get_or_create(const char *id) {
    registered_node *n = find_node(id);
    if (n) return n;
    if (node_count >= MAX_NODES) return nullptr;
    n = &node_registry[node_count++];
    memset(n, 0, sizeof(*n));
    strlcpy(n->node_id, id, NODE_ID_MAX_LEN);
    return n;
}

void send_ack(const lora_packet *pkt) {
    char ack_pl[28], ack[PACKET_MAX_LEN];
    snprintf(ack_pl, sizeof(ack_pl), "ack_id=%016llX", (unsigned long long)pkt->msg_id);
    format_packet(ack, sizeof(ack), new_msg_id(), NODE_ID, pkt->src_id, MSG_ACK, ack_pl);
    enqueueLora(String(ack));
}

void handle_discover(const lora_packet *pkt) {
    vTaskDelay(pdMS_TO_TICKS(50 + esp_random() % 300));   // spread responders
    char payload[64], raw[PACKET_MAX_LEN];
    snprintf(payload, sizeof(payload), "id=%s,depth=0", NODE_ID);
    format_packet(raw, sizeof(raw), new_msg_id(), NODE_ID, pkt->src_id, MSG_DISC_RESP, payload);
    enqueueLora(String(raw));
    Serial.printf("[DISC] DISC_RESP → %s\n", pkt->src_id);
}

void handle_reg_req(const lora_packet *pkt) {
    char claimed[NODE_ID_MAX_LEN];
    if (!get_field(pkt->payload, "id", claimed, sizeof(claimed)) || !id_valid(claimed)) {
        Serial.printf("[REG] Dropped — bad id from %s\n", pkt->src_id);
        return;
    }

    bool registered = false;
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *n = get_or_create(claimed);
    if (n) {
        n->is_online = false;   // flips online on next announce/heartbeat
        n->depth     = 1;
        registered   = true;
    }
    xSemaphoreGive(registry_mutex);

    if (!registered) {
        Serial.printf("[REG] Dropped %s — registry full\n", claimed);
        return;
    }

    char payload[80], raw[PACKET_MAX_LEN];
    snprintf(payload, sizeof(payload), "id=%s,parent=%s,depth=1", claimed, NODE_ID);
    format_packet(raw, sizeof(raw), new_msg_id(), NODE_ID, pkt->src_id, MSG_REG_ACK, payload);
    enqueueLora(String(raw));
    Serial.printf("[REG] ACK %s\n", claimed);
}

void handle_announce(const lora_packet *pkt) {
    char buf[32];
    double lat = 0.0, lng = 0.0;
    uint8_t depth = 1;

    bool went_online = false; 


    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *n = get_or_create(pkt->src_id);
    if (n) {
        went_online = mark_alive(n);
        n->last_announce_ms = millis();
        n->snr = pkt->snr; n->rssi = pkt->rssi;
        if (get_field(pkt->payload, "depth",  buf, sizeof(buf))) n->depth = depth = (uint8_t)atoi(buf);
        if (get_field(pkt->payload, "lat",    buf, sizeof(buf))) n->lat = lat = atof(buf);
        if (get_field(pkt->payload, "lng",    buf, sizeof(buf))) n->lng = lng = atof(buf);
    }
    xSemaphoreGive(registry_mutex);

    if (went_online) publish_node_status(pkt->src_id, true);

    send_ack(pkt);   // reliable — the child retries until this lands

    char base[64], topic[96], pl[320];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/announce/%s", base, pkt->src_id);
    snprintf(pl, sizeof(pl),
             "{\"node_id\":\"%s\",\"village\":\"%s\",\"depth\":%u,\"parent\":\"%s\","
             "\"lat\":%.6f,\"lng\":%.6f,\"snr\":%.1f,\"rssi\":%d}",
             pkt->src_id, VILLAGE, depth, NODE_ID, lat, lng, pkt->snr, pkt->rssi);
    enqueueMqtt(String(topic), String(pl));
    Serial.printf("[ANNOUNCE] %s depth=%u lat=%.6f lng=%.6f\n", pkt->src_id, depth, lat, lng);
    publishTopology();
}

void handle_hb(const lora_packet *pkt) {
    char buf[32];
    float bat = 0.0f, link_snr = pkt->snr;
    uint8_t fb = 0, depth = 1;
    int link_rssi = pkt->rssi;
    double lat = 0.0, lng = 0.0;
    bool gps_fix = false;

        bool went_online = false; 


    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *n = get_or_create(pkt->src_id);
    if (n) {
        went_online = mark_alive(n);
        if (get_field(pkt->payload, "bat",        buf, sizeof(buf))) n->battery_voltage = atof(buf);
        if (get_field(pkt->payload, "float_bits", buf, sizeof(buf))) n->float_bits = (uint8_t)atoi(buf);
        if (get_field(pkt->payload, "depth",      buf, sizeof(buf))) n->depth = depth = (uint8_t)atoi(buf);
        if (get_field(pkt->payload, "lat",        buf, sizeof(buf))) n->lat = lat = atof(buf);
        if (get_field(pkt->payload, "lng",        buf, sizeof(buf))) n->lng = lng = atof(buf);
        if (get_field(pkt->payload, "gps_fix",    buf, sizeof(buf))) n->gps_fix = atoi(buf) != 0;
        gps_fix = n->gps_fix;
        bat = n->battery_voltage; fb = n->float_bits;
        link_snr = n->snr = pkt->snr; link_rssi = n->rssi = pkt->rssi;
    }
    xSemaphoreGive(registry_mutex);

        if (went_online) publish_node_status(pkt->src_id, true);


    char base[64], topic[96], pl[320];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/heartbeat/%s", base, pkt->src_id);
    snprintf(pl, sizeof(pl),
             "{\"node_id\":\"%s\",\"village\":\"%s\",\"bat\":%.2f,\"float_bits\":%u,"
             "\"depth\":%u,\"parent\":\"%s\",\"lat\":%.6f,\"lng\":%.6f,\"gps_fix\":%s,"
             "\"snr\":%.1f,\"rssi\":%d}",
             pkt->src_id, VILLAGE, bat, fb, depth, NODE_ID, lat, lng,
             gps_fix ? "true" : "false", link_snr, link_rssi);
    enqueueMqtt(String(topic), String(pl));
}

void handle_alert(const lora_packet *pkt) {
    char origin[NODE_ID_MAX_LEN];
    if (!get_field(pkt->payload, "id", origin, sizeof(origin)) || !id_valid(origin))
        strlcpy(origin, pkt->src_id, sizeof(origin));
    char atype[16] = "unknown";
    get_field(pkt->payload, "type", atype, sizeof(atype));
    uint32_t aseq = payload_seq(pkt->payload);

    bool stale = false;
    bool origin_gps_fix = false;
        bool went_online = false; 

    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    registered_node *orig = get_or_create(origin);
    if (orig) {
        went_online = mark_alive(orig);
        origin_gps_fix = orig->gps_fix;
        if (aseq) {
            if (orig->last_alert_seq && aseq <= orig->last_alert_seq) stale = true;
            else orig->last_alert_seq = aseq;   // FIFO TX ⇒ per-origin seqs arrive in order
        }
    }
    xSemaphoreGive(registry_mutex);

        if (went_online) publish_node_status(pkt->src_id, true);


    send_ack(pkt);   // ACK the child whether fresh or stale — stop its retries

    if (stale) {
        Serial.printf("[ALERT] stale seq=%lu from %s type=%s — absorbed\n",
                      (unsigned long)aseq, origin, atype);
        return;
    }

    char buf[24];
    uint8_t level = 0, fb = 0;
    float bat = 0.0f, dist = 0.0f;
    double lat = 0.0, lng = 0.0, home_lat = 0.0, home_lng = 0.0;
    if (get_field(pkt->payload, "level",      buf, sizeof(buf))) level    = (uint8_t)atoi(buf);
    if (get_field(pkt->payload, "float_bits", buf, sizeof(buf))) fb       = (uint8_t)strtol(buf, nullptr, 0);
    if (get_field(pkt->payload, "bat",        buf, sizeof(buf))) bat      = atof(buf);
    if (get_field(pkt->payload, "dist",       buf, sizeof(buf))) dist     = atof(buf);
    if (get_field(pkt->payload, "lat",        buf, sizeof(buf))) lat      = atof(buf);
    if (get_field(pkt->payload, "lng",        buf, sizeof(buf))) lng      = atof(buf);
    if (get_field(pkt->payload, "home_lat",   buf, sizeof(buf))) home_lat = atof(buf);
    if (get_field(pkt->payload, "home_lng",   buf, sizeof(buf))) home_lng = atof(buf);

    char base[64], topic[96], pl[448];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/alert/%s", base, origin);
    snprintf(pl, sizeof(pl),
             "{\"node_id\":\"%s\",\"village\":\"%s\",\"type\":\"%s\",\"seq\":%lu,\"level\":%u,"
             "\"float_bits\":%u,\"bat\":%.2f,\"dist\":%.1f,\"lat\":%.6f,\"lng\":%.6f,"
             "\"home_lat\":%.6f,\"home_lng\":%.6f,\"gps_fix\":%s,\"snr\":%.1f,\"rssi\":%d}",
             origin, VILLAGE, atype, (unsigned long)aseq, level, fb, bat, dist, lat, lng,
             home_lat, home_lng, origin_gps_fix ? "true" : "false", pkt->snr, pkt->rssi);
    enqueueMqtt(String(topic), String(pl));
    Serial.printf("[ALERT] %s type=%s seq=%lu level=%u\n", origin, atype, (unsigned long)aseq, level);
}

// Called with registry_mutex held; flat topology — all online children hang
// directly off the master in the demo build.
static int buildTopoNode(char *buf, size_t buf_len, int pos, const char *node_id) {
    pos += snprintf(buf + pos, buf_len - pos, "{");
    bool first = true;
    for (uint8_t i = 0; i < node_count; i++) {
        if (!node_registry[i].is_online) continue;
        pos += snprintf(buf + pos, buf_len - pos, "%s\"%s\":{}",
                        first ? "" : ",", node_registry[i].node_id);
        first = false;
    }
    pos += snprintf(buf + pos, buf_len - pos, "}");
    return pos;
}

void publishTopology() {
    size_t buf_len = node_count * 60 + 256;
    char *buf = (char *)malloc(buf_len);
    if (!buf) { Serial.println("[TOPO] malloc failed"); return; }

    int pos = snprintf(buf, buf_len, "{\"%s\":", NODE_ID);
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    pos = buildTopoNode(buf, buf_len, pos, NODE_ID);
    xSemaphoreGive(registry_mutex);
    snprintf(buf + pos, buf_len - pos, "}");

    char base[64], topic[96];
    topic_base(base, sizeof(base));
    snprintf(topic, sizeof(topic), "%s/topology", base);
    enqueueMqtt(String(topic), String(buf));
    free(buf);
    Serial.println("[TOPO] queued");
}
