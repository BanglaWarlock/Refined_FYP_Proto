// Health timeouts + serial status dump + LED.

void healthTask(void *pv) {
    char ids[MAX_NODES][NODE_ID_MAX_LEN];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
        uint32_t now = millis();
        uint8_t cnt = 0;

        xSemaphoreTake(registry_mutex, portMAX_DELAY);
        for (uint8_t i = 0; i < node_count; i++) {
            registered_node &n = node_registry[i];
            if (n.is_online && n.last_seen_ms > 0 &&
                (now - n.last_seen_ms > NODE_OFFLINE_TIMEOUT_MS)) {
                n.is_online = false;
                Serial.printf("[HEALTH] %s timed out\n", n.node_id);
                strlcpy(ids[cnt++], n.node_id, NODE_ID_MAX_LEN);
            }
        }
        xSemaphoreGive(registry_mutex);

        for (uint8_t i = 0; i < cnt; i++) {
            publish_node_status(ids[i], false);
            publishTopology();
        }
    }
}

void statusTask(void *pv) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(STATUS_INTERVAL_MS));

        xSemaphoreTake(mqtt_mutex, portMAX_DELAY);
        bool mqtt_up = mqtt.connected();
        xSemaphoreGive(mqtt_mutex);

        xSemaphoreTake(registry_mutex, portMAX_DELAY);
        Serial.printf("\n--- STATUS  wifi=%s  mqtt=%s  nodes=%u ---\n",
                      WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
                      mqtt_up ? "UP" : "DOWN", node_count);
        for (uint8_t i = 0; i < node_count; i++) {
            registered_node &n = node_registry[i];
            Serial.printf("  %-20s  d=%u  %-7s  bat=%.2fV  float=0x%02X  fix=%s  rssi=%d  snr=%.1f\n",
                          n.node_id, n.depth,
                          n.is_online ? "ONLINE" : "OFFLINE",
                          n.battery_voltage, n.float_bits, n.gps_fix ? "YES" : "NO",
                          n.rssi, n.snr);
        }
        xSemaphoreGive(registry_mutex);

        // LED: blink when WiFi/MQTT down, steady off when connected
        if (!mqtt_up || WiFi.status() != WL_CONNECTED) {
            digitalWrite(PIN_LED, HIGH); vTaskDelay(pdMS_TO_TICKS(100));
            digitalWrite(PIN_LED, LOW);
        } else {
            digitalWrite(PIN_LED, LOW);
        }
    }
}
