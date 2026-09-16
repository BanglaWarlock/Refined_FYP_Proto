// Housekeeping: beacon, node health (offline marking), command retry,
// serial status dump + LED.

#define STACK_WARN_BYTES 512

void beaconTask(void *pv) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(BEACON_INTERVAL_MS));
        char raw[PACKET_MAX_LEN];
        format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, "ALL", MSG_BEACON, "");
        enqueueLora(String(raw));
        Serial.println("[BEACON] sent");
    }
}

// Mark timed-out nodes offline and publish status. Nodes stay in the
// registry (topology/history survive an outage; re-registration reuses them).
void healthTask(void *pv) {
    char ids[8][NODE_ID_MAX_LEN];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PRINT_INTERVAL_MS));
        uint32_t now = millis();
        uint8_t cnt = 0;
        xSemaphoreTake(registry_mutex, portMAX_DELAY);
        for (uint8_t i = 0; i < node_count; i++) {
            registered_node &n = node_registry[i];
            if (n.is_online && n.last_seen_ms > 0 &&
                (now - n.last_seen_ms > NODE_OFFLINE_TIMEOUT_MS)) {
                n.is_online = false;
                Serial.printf("[HEALTH] %s timed out\n", n.node_id);
                if (cnt < 8) {
                    strlcpy(ids[cnt], n.node_id, NODE_ID_MAX_LEN);
                    cnt++;
                }
            }
        }
        xSemaphoreGive(registry_mutex);
        for (uint8_t i = 0; i < cnt; i++) {
            publish_node_status(ids[i], false);
            publishTopology();
        }
    }
}

void cmdRetryTask(void *pv) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CMD_RETRY_INTERVAL_MS));

        xSemaphoreTake(pending_cmd_mutex, portMAX_DELAY);
        for (int i = (int)pending_cmds.size() - 1; i >= 0; i--) {
            pending_cmd_t &cmd = pending_cmds[i];

            xSemaphoreTake(registry_mutex, portMAX_DELAY);
            registered_node *n = find_node(cmd.target);
            bool offline = (n != nullptr && !n->is_online);
            xSemaphoreGive(registry_mutex);

            if (offline) {
                Serial.printf("[CMD] %s offline — command abandoned after %u attempt(s)\n",
                              cmd.target, cmd.attempts);
                pending_cmds.erase(pending_cmds.begin() + i);
            } else {
                cmd.attempts++;
                cmd.last_sent_ms = millis();
                char raw[PACKET_MAX_LEN];
                format_packet(raw, sizeof(raw), new_msg_id(), own_node_id, cmd.next_hop, MSG_CMD, cmd.pload);
                enqueueLora(String(raw));
                Serial.printf("[CMD] Retry %u → %s via %s\n",
                              cmd.attempts, cmd.target, cmd.next_hop);
            }
        }
        xSemaphoreGive(pending_cmd_mutex);
    }
}

static void _wm(const char *name, TaskHandle_t h, bool &any_low) {
    UBaseType_t wm = uxTaskGetStackHighWaterMark(h);
    Serial.printf("  %-14s free=%u bytes%s\n", name, (unsigned)wm,
                  wm < STACK_WARN_BYTES ? "  *** LOW ***" : "");
    if (wm < STACK_WARN_BYTES) any_low = true;
}

void statusTask(void *pv) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PRINT_INTERVAL_MS));

        xSemaphoreTake(mqtt_mutex, portMAX_DELAY);
        bool mqtt_up = mqtt.connected();
        xSemaphoreGive(mqtt_mutex);

        xSemaphoreTake(pending_cmd_mutex, portMAX_DELAY);
        size_t pending = pending_cmds.size();
        xSemaphoreGive(pending_cmd_mutex);

        xSemaphoreTake(registry_mutex, portMAX_DELAY);
        Serial.printf("\n--- STATUS  wifi=%s  mqtt=%s  pending=%u  nodes=%u ---\n",
                      WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
                      mqtt_up ? "UP" : "DOWN", (unsigned)pending, node_count);
        for (uint8_t i = 0; i < node_count; i++) {
            registered_node &n = node_registry[i];
            Serial.printf("  %-20s  parent=%-20s  d=%u  %-7s  bat=%.2fV  rssi=%d  snr=%.1f  rx=%lu  lost=%lu\n",
                          n.node_id, n.parent_id, n.depth,
                          n.is_online ? "ONLINE" : "OFFLINE",
                          n.battery_voltage, n.rssi, n.snr,
                          (unsigned long)n.pkt_rx, (unsigned long)n.pkt_lost);
        }
        xSemaphoreGive(registry_mutex);

        Serial.println("[STACK watermarks]");
        bool any_low = false;
        _wm("lora_rx",     h_lora_rx,     any_low);
        _wm("lora_tx",     h_lora_tx,     any_low);
        _wm("lora_proc",   h_lora_proc,   any_low);
        _wm("beacon",      h_beacon,      any_low);
        _wm("health",      h_health,      any_low);
        _wm("cmd_retry",   h_cmd_retry,   any_low);
        _wm("conn",        h_conn,        any_low);
        _wm("mqtt_send",   h_mqtt_send,   any_low);
        _wm("cmd_handler", h_cmd_handler, any_low);
        _wm("status",      h_status,      any_low);

        if (any_low) {
            char base[64], topic[96], pl[128];
            topic_base(base, sizeof(base));
            snprintf(topic, sizeof(topic), "%s/master/health", base);
            snprintf(pl, sizeof(pl),
                     "{\"node_id\":\"%s\",\"warning\":\"low_stack\"}", own_node_id);
            enqueueMqtt(String(topic), String(pl));
        }

        Serial.println("------------------------------------------------------");

        static uint32_t last_topo_ms = 0;
        if (millis() - last_topo_ms >= 60000UL) {
            last_topo_ms = millis();
            publishTopology();
        }

        // LED: blink when WiFi/MQTT down, steady off when connected
        if (!mqtt_up || WiFi.status() != WL_CONNECTED) {
            digitalWrite(LED_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(100));
            digitalWrite(LED_PIN, LOW);
        } else {
            digitalWrite(LED_PIN, LOW);
        }
    }
}
