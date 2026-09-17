// Application layer: registration (discover → register → announce), reliable
// alerts, fire-and-forget heartbeats, link-loss re-registration.

void send_announce() {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "id=%s,village=%s,depth=%u,parent=%s,lat=%.6f,lng=%.6f",
             NODE_ID, VILLAGE, own_depth, active_parent_id, gps_lat, gps_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_ANNOUNCE, pl);
    Serial.println("[ANNOUNCE] queued (reliable)");
}

void send_heartbeat() {
    static uint32_t seq = 0;
    char pl[PACKET_MAX_LEN], raw[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl),
             "id=%s,bat=%.2f,float_bits=%u,depth=%u,parent=%s,lat=%.6f,lng=%.6f,gps_fix=%u,seq=%lu",
             NODE_ID, battery_voltage, float_bits, own_depth, active_parent_id,
             gps_lat, gps_lng, (uint8_t)gps_fix_valid, seq++);
    uint64_t mid = new_msg_id();
    format_packet(raw, sizeof(raw), mid, NODE_ID, active_parent_id, MSG_HB, pl);
    enqueueLora(String(raw));
}

void send_alert_flood(uint8_t level) {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=flood,id=%s,level=%u,float_bits=0x%02X,bat=%.2f,lat=%.6f,lng=%.6f",
             NODE_ID, level, float_bits, battery_voltage, gps_lat, gps_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_FLOOD, pl);
    Serial.printf("[ALERT] Flood level=%u queued\n", level);
}

void send_alert_battery() {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=battery,id=%s,bat=%.2f", NODE_ID, battery_voltage);
    enqueueAlert(MSG_ALERT, ALERT_KEY_BATTERY, pl);
    Serial.println("[ALERT] Battery low queued");
}

void send_alert_gps_signal_lost() {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=gps_signal_lost,id=%s,bat=%.2f,last_lat=%.6f,last_lng=%.6f",
             NODE_ID, battery_voltage, gps_lat, gps_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_GPS_SIGNAL, pl);
    Serial.println("[ALERT] GPS signal lost queued");
}

void send_alert_gps_fix_restored() {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=gps_restored,id=%s,bat=%.2f,lat=%.6f,lng=%.6f",
             NODE_ID, battery_voltage, gps_lat, gps_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_GPS_SIGNAL, pl);
    Serial.println("[ALERT] GPS fix restored queued");
}

void send_alert_gps_moved(double dist) {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl),
             "type=gps_moved,id=%s,bat=%.2f,dist=%.1f,lat=%.6f,lng=%.6f,home_lat=%.6f,home_lng=%.6f",
             NODE_ID, battery_voltage, (float)dist, gps_lat, gps_lng, gps_home_lat, gps_home_lng);
    enqueueAlert(MSG_ALERT, ALERT_KEY_GPS_MOVED, pl);
    Serial.printf("[ALERT] GPS moved %.1fm queued\n", (float)dist);
}

void send_alert_crash(const char *reason) {
    char pl[PACKET_MAX_LEN];
    snprintf(pl, sizeof(pl), "type=crash,id=%s,reason=%s,bat=%.2f", NODE_ID, reason, battery_voltage);
    enqueueAlert(MSG_ALERT, ALERT_KEY_CRASH, pl);
    Serial.printf("[CRASH] Alert queued: %s\n", reason);
}

// ── Registration handlers

void handle_disc_resp(const lora_packet *pkt) {
    disc_candidate_t c = {};
    strlcpy(c.id, pkt->src_id, sizeof(c.id));
    c.rssi = pkt->rssi; c.snr = pkt->snr;
    const char *p;
    if ((p = strstr(pkt->payload, "depth=")) != nullptr) c.depth = (uint8_t)atoi(p + 6);
    if (c.depth != 0) return;   // demo: pair directly with the master only
    xSemaphoreTake(candidates_mutex, portMAX_DELAY);
    if (candidate_count < 8) candidates[candidate_count++] = c;
    xSemaphoreGive(candidates_mutex);
    Serial.printf("[DISC] Candidate: %s depth=%u rssi=%d\n", c.id, c.depth, c.rssi);
}

void handle_reg_ack(const lora_packet *pkt) {
    const char *p = strstr(pkt->payload, "parent=");
    if (p) {
        char cand[NODE_ID_MAX_LEN];
        strlcpy(cand, p + 7, NODE_ID_MAX_LEN);
        char *c = strchr(cand, ','); if (c) *c = '\0';
        strlcpy(active_parent_id, cand, NODE_ID_MAX_LEN);
    }
    last_parent_heartbeat();   // re-arm pending alerts for the fresh link
    node_state = NODE_OPERATIONAL;
    led_set_pattern(LED_OK);
    xSemaphoreGive(reg_ack_sem);
    Serial.printf("[REG] Registered — parent=%s depth=%u\n", active_parent_id, own_depth);
    send_announce();
}

// Mark the link freshly established: re-arm pending alerts so they fire at
// the (re)acquired parent right away.
void last_parent_heartbeat() {
    xSemaphoreTake(alert_mutex, portMAX_DELAY);
    for (auto &a : alert_list) { a.first_sent_ms = 0; a.last_sent_ms = 0; }
    xSemaphoreGive(alert_mutex);
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
            uint64_t disc_mid = new_msg_id();
            format_packet(raw, sizeof(raw), disc_mid, NODE_ID, "ALL", MSG_DISCOVER, "");
            enqueueLora(String(raw));
            Serial.println("[DISC] Sent DISCOVER");
            uint32_t waited = 0;   // window opens only when the DISCOVER is on air
            while (last_tx_msg_id != disc_mid && waited < 3000) {
                vTaskDelay(pdMS_TO_TICKS(10));
                waited += 10;
            }
            vTaskDelay(pdMS_TO_TICKS(DISC_WINDOW_MS));

            xSemaphoreTake(candidates_mutex, portMAX_DELAY);
            uint8_t cnt = candidate_count;
            disc_candidate_t best = {};
            if (cnt > 0) {
                int bi = 0;
                for (uint8_t i = 1; i < cnt; i++) {
                    const disc_candidate_t &c = candidates[i];
                    const disc_candidate_t &b = candidates[bi];
                    if (c.snr  > b.snr)  { bi = i; continue; }
                    if (c.snr  < b.snr)  continue;
                    if (c.rssi > b.rssi)   bi = i;
                }
                best = candidates[bi];
            }
            xSemaphoreGive(candidates_mutex);

            if (cnt == 0) {
                vTaskDelay(pdMS_TO_TICKS(DISC_RETRY_MS));
            } else {
                char pl[PACKET_MAX_LEN];
                snprintf(pl, sizeof(pl), "id=%s", NODE_ID);
                format_packet(raw, sizeof(raw), new_msg_id(), NODE_ID, best.id, MSG_REG_REQ, pl);
                enqueueLora(String(raw));
                node_state = NODE_REGISTERING;
                led_set_pattern(LED_REGISTERING);
                Serial.printf("[DISC] REG_REQ → %s (rssi=%d)\n", best.id, best.rssi);
            }
            break;
        }

        case NODE_REGISTERING:
            // success: handle_reg_ack already set NODE_OPERATIONAL, armed the
            // link timers, and queued the announce
            if (xSemaphoreTake(reg_ack_sem, pdMS_TO_TICKS(REG_TIMEOUT_MS)) != pdTRUE) {
                Serial.println("[DISC] REG_ACK timeout — re-discovering");
                node_state = NODE_DISCOVERING;
            }
            break;

        case NODE_OPERATIONAL: {
            static bool crash_queued = false;
            if (!crash_queued && pending_crash_reason[0]) {
                crash_queued = true;
                send_alert_crash(pending_crash_reason);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Link loss = every reliable alert un-ACKed past LINK_LOST_MS.
            // Recovery is simply to re-register — the master re-accepts known
            // children and republishes them online.
            uint32_t now = millis();
            bool link_lost = false;
            xSemaphoreTake(alert_mutex, portMAX_DELAY);
            for (auto &a : alert_list) {
                if (a.first_sent_ms != 0 && now - a.first_sent_ms > LINK_LOST_MS) {
                    link_lost = true;
                    break;
                }
            }
            xSemaphoreGive(alert_mutex);
            if (link_lost) {
                Serial.println("[LINK] Un-ACKed too long — re-registering");
                node_state = NODE_DISCOVERING;
            }
            break;
        }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Heartbeats are fire-and-forget: one every HB_INTERVAL_MS while registered.
// Every 30th heartbeat also re-announces (~5 min) so a rebooted master picks
// up the child's topology/position without waiting for an alert.
void heartbeatTask(void *pv) {
    static uint32_t hb_count = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HB_INTERVAL_MS));
        if (node_state != NODE_OPERATIONAL || !active_parent_id[0]) continue;

        battery_voltage = read_battery_voltage();
        if (!battery_low_sent && battery_voltage < 10.5f) {
            send_alert_battery(); battery_low_sent = true;
        } else if (battery_low_sent && battery_voltage >= 11.5f) {
            battery_low_sent = false;
        }

        send_heartbeat();
        Serial.printf("[HB] Sent → %s\n", active_parent_id);
        if (++hb_count % 30 == 0) {
            Serial.println("[HB] Periodic re-announce");
            send_announce();
        }
    }
}
