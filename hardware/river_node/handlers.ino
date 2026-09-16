// Discovery / registration handlers (parent side of membership).

void handle_disc_resp(const lora_packet *pkt) {
    disc_candidate_t c = {};
    strlcpy(c.id, pkt->src_id, sizeof(c.id));
    c.rssi = pkt->rssi; c.snr = pkt->snr;
    const char *p;
    if ((p = strstr(pkt->payload, "depth=")) != nullptr) c.depth = (uint8_t)atoi(p + 6);
    if ((p = strstr(pkt->payload, "cur="))   != nullptr) c.cur_children = (uint8_t)atoi(p + 4);
    if ((p = strstr(pkt->payload, "max="))   != nullptr) c.max_children = (uint8_t)atoi(p + 4);
    else c.max_children = 255;   // truncated harvest payload — assume capacity
    if (c.cur_children >= c.max_children) { Serial.printf("[DISC] %s full — skipped\n", c.id); return; }
    xSemaphoreTake(candidates_mutex, portMAX_DELAY);
    if (candidate_count < 8) candidates[candidate_count++] = c;
    xSemaphoreGive(candidates_mutex);
    Serial.printf("[DISC] Candidate: %s depth=%u rssi=%d\n", c.id, c.depth, c.rssi);
}

void handle_reg_ack(const lora_packet *pkt) {
    const char *p;
    if ((p = strstr(pkt->payload, "parent=")) != nullptr) {
        strlcpy(active_parent_id, p + 7, NODE_ID_MAX_LEN);
        char *c = strchr(active_parent_id, ','); if (c) *c = '\0';
    }
    if ((p = strstr(pkt->payload, "depth=")) != nullptr) own_depth = (uint8_t)atoi(p + 6);
    last_parent_seen_ms = millis();
    // hold first HB until the registration burst settles; the master's offline
    // timeout (>> HB interval) makes this safe
    pending_hb = {};
    node_state = NODE_OPERATIONAL;
    // re-arm all pending alerts so they fire immediately to the new parent
    xSemaphoreTake(alert_mutex, portMAX_DELAY);
    for (auto &a : alert_list) a.last_sent_ms = 0;
    xSemaphoreGive(alert_mutex);
    xSemaphoreGive(reg_ack_sem);
    Serial.printf("[REG] Registered — parent=%s depth=%u\n", active_parent_id, own_depth);
    send_announce();
}

void handle_child_discover(const lora_packet *pkt) {
    if (is_crash_pending()) return;   // crash report gets the channel first
    if (active_parent_id[0] && strcmp(pkt->src_id, active_parent_id) == 0) return;  // never parent our own parent
    // count OTHER registered children — requester may reclaim its slot
    xSemaphoreTake(children_mutex, portMAX_DELAY);
    uint8_t other_count = 0;
    for (const auto &r : child_regs)
        if (strcmp(r.id, pkt->src_id) != 0) other_count++;
    xSemaphoreGive(children_mutex);
    if (other_count >= max_children) return;
    // spread responders across the window — multiple relays heard the same
    // DISCOVER and must not transmit on top of each other (SF7 airtime 65 ms)
    vTaskDelay(pdMS_TO_TICKS(50 + esp_random() % 500));
    char pl[PACKET_MAX_LEN], raw[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "id=%s,depth=%u,cur=%u,max=%u", own_node_id, own_depth, other_count, max_children);
    format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, pkt->src_id, MSG_DISC_RESP, pl);
    enqueueLora(String(raw));
    Serial.printf("[RELAY] DISC_RESP → %s\n", pkt->src_id);
}

void handle_child_reg_req(const lora_packet *pkt) {
    if (is_crash_pending()) return;
    const char *id_f = strstr(pkt->payload, "id=");
    if (!id_f) return;
    char claimed[NODE_ID_MAX_LEN]; strlcpy(claimed, id_f + 3, sizeof(claimed));
    char *c = strchr(claimed, ','); if (c) *c = '\0';
    xSemaphoreTake(children_mutex, portMAX_DELAY);
    bool registered = false;
    for (auto &r : child_regs) {
        if (strcmp(r.id, claimed) == 0) {
            r.last_seen_ms = millis();
            registered = true;
            break;
        }
    }
    if (!registered && child_regs.size() < max_children) {
        child_reg_t cr = {};
        strlcpy(cr.id, claimed, NODE_ID_MAX_LEN);
        cr.last_seen_ms = millis();
        child_regs.push_back(cr);
        registered = true;
    }
    xSemaphoreGive(children_mutex);
    if (!registered) {
        Serial.printf("[RELAY] REG_REQ from %s rejected — at capacity\n", claimed);
        return;
    }
    // child is back under us — any pending node_lost for it is stale (§7B)
    cancel_offline_alerts_for(claimed);
    char pl[PACKET_MAX_LEN], raw[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "id=%s,parent=%s,depth=%u", claimed, own_node_id, own_depth + 1);
    format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, pkt->src_id, MSG_REG_ACK, pl);
    enqueueLora(String(raw));
    Serial.printf("[RELAY] REG_ACK %s → %s\n", claimed, pkt->src_id);
}
