// Mesh layer: alert builders, discovery/registration FSM, heartbeat
// liveness, beacons, child eviction → node_lost. Spec: docs/PROTOCOL.md §5-§7.

void send_announce() {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "id=%s,village=%s,depth=%u,parent=%s,lat=%.6f,lng=%.6f",
             own_node_id, village, own_depth, active_parent_id, gps_lat, gps_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_ANNOUNCE, "", pl);
    Serial.println("[ANNOUNCE] queued (reliable)");
}

void send_heartbeat() {
    static uint32_t seq = 0;
    char pl[PACKET_MAX_LEN], raw[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "id=%s,bat=%.2f,float_bits=%u,depth=%u,parent=%s,lat=%.6f,lng=%.6f,gps_fix=%u,seq=%lu",
             own_node_id, battery_voltage, float_bits, own_depth, active_parent_id, gps_lat, gps_lng,
             (uint8_t)gps_fix_valid, seq++);
    uint64_t mid = new_msg_id();
    format_packet(raw, sizeof(raw), mid, own_node_id, active_parent_id, MSG_HB, pl);
    pending_hb.sent_msg_id  = mid;
    pending_hb.retries++;
    enqueueLora(String(raw));
}

void send_alert_flood(uint8_t level) {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=flood,id=%s,level=%u,float_bits=0x%02X,bat=%.2f,lat=%.6f,lng=%.6f",
             own_node_id, level, float_bits, battery_voltage, gps_lat, gps_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_FLOOD, "", pl);
    Serial.printf("[ALERT] Flood level=%u queued\n", level);
}

void send_alert_gps_signal_lost() {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=gps_signal_lost,id=%s,bat=%.2f,last_lat=%.6f,last_lng=%.6f",
             own_node_id, battery_voltage, gps_lat, gps_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_GPS_SIGNAL, "", pl);
    Serial.println("[ALERT] GPS signal lost queued");
}

void send_alert_gps_fix_restored() {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=gps_restored,id=%s,bat=%.2f,lat=%.6f,lng=%.6f",
             own_node_id, battery_voltage, gps_lat, gps_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_GPS_SIGNAL, "", pl);
    Serial.println("[ALERT] GPS fix restored queued");
}

void send_alert_gps_moved(double dist) {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl),
             "type=gps_moved,id=%s,bat=%.2f,dist=%.1f,lat=%.6f,lng=%.6f,home_lat=%.6f,home_lng=%.6f",
             own_node_id, battery_voltage, (float)dist, gps_lat, gps_lng, gps_home_lat, gps_home_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_GPS_MOVED, "", pl);
    Serial.printf("[ALERT] GPS moved %.1fm queued\n", (float)dist);
}

void send_alert_battery() {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=battery,id=%s,bat=%.2f", own_node_id, battery_voltage);
    enqueueAlert(MSG_ALERT, ALERT_KEY_BATTERY, "", pl);
    Serial.println("[ALERT] Battery low queued");
}

void send_alert_crash(const char *reason) {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=crash,id=%s,reason=%s,bat=%.2f",
             own_node_id, reason, battery_voltage);
    enqueueAlert(MSG_ALERT, ALERT_KEY_CRASH, "", pl);
    Serial.printf("[CRASH] Alert queued: %s\n", reason);
}

// Relay lost its child — reliable report upstream; tag = child id so two
// simultaneous losses don't overwrite each other (PROTOCOL §7).
void send_alert_node_lost(const char *child, uint32_t down_s) {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=node_lost,id=%s,lost=%s,down_s=%lu,bat=%.2f",
             own_node_id, child, (unsigned long)down_s, battery_voltage);
    enqueueAlert(MSG_ALERT, ALERT_KEY_NODE_LOST, child, pl);
    Serial.printf("[ALERT] node_lost %s (down %.0fs) queued\n", child, (float)down_s);
}

void discoveryTask(void *pv) {
    for (;;) {
        switch (node_state) {

        case NODE_DISCOVERING: {
            led_set_pattern(LED_DISCOVERING);
            xSemaphoreTake(candidates_mutex, portMAX_DELAY);
            candidate_count = 0;
            xSemaphoreGive(candidates_mutex);
            char raw[PACKET_MAX_LEN];
            format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, "ALL", MSG_DISCOVER, "");
            enqueueLora(String(raw));
            Serial.println("[DISC] Sent DISCOVER");
            vTaskDelay(pdMS_TO_TICKS(DISC_WINDOW_MS));

            xSemaphoreTake(candidates_mutex, portMAX_DELAY);
            uint8_t cnt = candidate_count;
            disc_candidate_t best = {};
            if (cnt > 0) {
                int bi = 0;
                for (uint8_t i = 1; i < cnt; i++) {
                    const disc_candidate_t &c = candidates[i];
                    const disc_candidate_t &b = candidates[bi];
                    if (c.snr   > b.snr)   { bi = i; continue; }
                    if (c.snr   < b.snr)   continue;
                    if (c.rssi  > b.rssi)  { bi = i; continue; }
                    if (c.rssi  < b.rssi)  continue;
                    if (c.depth < b.depth)   bi = i;
                }
                best = candidates[bi];
            }
            xSemaphoreGive(candidates_mutex);

            if (cnt == 0) {
                Serial.println("[DISC] No candidates — retrying");
                vTaskDelay(pdMS_TO_TICKS(DISC_INTERVAL_MS));
            } else {
                char pl[PACKET_MAX_LEN];
                snprintf(pl, sizeof(pl), "id=%s", own_node_id);
                format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, best.id, MSG_REG_REQ, pl);
                enqueueLora(String(raw));
                node_state = NODE_REGISTERING;
                led_set_pattern(LED_REGISTERING);
                Serial.printf("[DISC] REG_REQ → %s (depth=%u rssi=%d)\n", best.id, best.depth, best.rssi);
            }
            break;
        }

        case NODE_REGISTERING:
            if (xSemaphoreTake(reg_ack_sem, pdMS_TO_TICKS(REG_TIMEOUT_MS)) != pdTRUE) {
                Serial.println("[DISC] REG_ACK timeout — re-discovering");
                node_state = NODE_DISCOVERING;
            }
            break;

        case NODE_OPERATIONAL: {
            // one-shot: queue crash report on first tick after becoming operational
            static bool crash_alert_queued = false;
            if (!crash_alert_queued && pending_crash_reason[0]) {
                crash_alert_queued = true;
                send_alert_crash(pending_crash_reason);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            // evict silent children — report node_lost, then free the slot
            uint32_t now = millis();
            xSemaphoreTake(children_mutex, portMAX_DELAY);
            for (auto it = child_regs.begin(); it != child_regs.end(); ) {
                if (it->last_seen_ms > 0 && (now - it->last_seen_ms > CHILD_OFFLINE_TIMEOUT_MS)) {
                    uint32_t down_s = (now - it->last_seen_ms) / 1000;
                    char id[NODE_ID_MAX_LEN];
                    strlcpy(id, it->id, sizeof(id));
                    it = child_regs.erase(it);
                    xSemaphoreGive(children_mutex);
                    send_alert_node_lost(id, down_s);
                    xSemaphoreTake(children_mutex, portMAX_DELAY);
                } else { ++it; }
            }
            xSemaphoreGive(children_mutex);
            break;
        }

        case NODE_LOST_PARENT:
            led_set_pattern(LED_LOST_PARENT);
            xSemaphoreTake(children_mutex, portMAX_DELAY);
            child_regs.clear();
            xSemaphoreGive(children_mutex);
            xSemaphoreTake(alert_mutex, portMAX_DELAY);
            for (auto &a : alert_list) a.last_sent_ms = 0;
            xSemaphoreGive(alert_mutex);
            active_parent_id[0] = '\0';
            last_parent_seen_ms = 0;
            Serial.println("[DISC] Going back to discovering");
            node_state = NODE_DISCOVERING;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void heartbeatTask(void *pv) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ALERT_RETRY_MS));

        if (node_state != NODE_OPERATIONAL) {
            pending_hb     = {};
            last_hb_ack_ms = 0;
            continue;
        }

        battery_voltage = read_battery_voltage();
        if (!battery_low_sent && battery_voltage < 10.5f) {
            send_alert_battery(); battery_low_sent = true;
        } else if (battery_low_sent && battery_voltage >= 11.5f) {
            battery_low_sent = false;
        }

        uint32_t now = millis();
        if (pending_hb.sent_msg_id == 0) {
            // no HB in flight — send when interval elapsed since last ACK
            if (last_hb_ack_ms == 0 || (now - last_hb_ack_ms >= HEARTBEAT_INTERVAL_MS)) {
                pending_hb.retries = 0;
                send_heartbeat();
                Serial.printf("[HB] Sent → %s\n", active_parent_id);
            }
        } else {
            // HB in flight — retry or declare parent lost
            if (now - pending_hb.last_sent_ms >= HB_RETRY_MS) {
                if (pending_hb.retries >= HB_MAX_RETRIES) {
                    Serial.printf("[HB] No ACK after %u tries — parent %s lost\n",
                                  HB_MAX_RETRIES, active_parent_id);
                    pending_hb     = {};
                    last_hb_ack_ms = 0;
                    node_state     = NODE_LOST_PARENT;
                } else {
                    Serial.printf("[HB] Retry %u/%u → %s\n",
                                  pending_hb.retries + 1, HB_MAX_RETRIES, active_parent_id);
                    send_heartbeat();
                }
            }
        }
    }
}

void beaconTask(void *pv) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(BEACON_INTERVAL_MS));
        if (node_state != NODE_OPERATIONAL) continue;
        xSemaphoreTake(children_mutex, portMAX_DELAY);
        bool has_ch = !child_regs.empty();
        xSemaphoreGive(children_mutex);
        if (!has_ch) continue;
        char raw[PACKET_MAX_LEN];
        format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, "ALL", MSG_BEACON, "");
        enqueueLora(String(raw));
        Serial.println("[BEACON] sent");
    }
}
