// WiFi/MQTT backhaul: connect, LWT, publish queue drain.

void enqueueMqtt(const String& topic, const String& payload) {
    xSemaphoreTake(send_mutex, portMAX_DELAY);
    if (send_list.size() < SEND_LIST_MAX) {
        send_list.push_back({topic, payload});
        xSemaphoreGive(send_mutex);
        xSemaphoreGive(send_sem);
    } else {
        xSemaphoreGive(send_mutex);
        Serial.printf("[MQTT] send_list full (%u) — drop: %s\n", SEND_LIST_MAX, topic.c_str());
    }
}

void setupWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 15000) {
            Serial.println("\n[WIFI] Timeout — will retry next cycle");
            return;
        }
        Serial.print(".");
        delay(500);
    }
    Serial.println("\n[WIFI] OK");
}

void setupMQTT() {
    char base[64], status_topic[96], lwt_payload[128], client_id[32];
    topic_base(base, sizeof(base));
    snprintf(status_topic, sizeof(status_topic), "%s/master/status", base);
    snprintf(lwt_payload,  sizeof(lwt_payload),  "{\"status\":\"offline\",\"node_id\":\"%s\"}", NODE_ID);
    snprintf(client_id,    sizeof(client_id),    "master-%s", VILLAGE);

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(1024);

    for (int i = 0; i < 10; i++) {
        if (mqtt.connect(client_id, nullptr, nullptr, status_topic, 0, true, lwt_payload)) break;
        Serial.print(".");
        delay(1000);
    }
    if (!mqtt.connected()) {
        Serial.println("\n[MQTT] Failed — connTask will retry");
        return;
    }

    char online_payload[160];
    if (master_crash_reason[0]) {
        snprintf(online_payload, sizeof(online_payload),
                 "{\"status\":\"online\",\"node_id\":\"%s\",\"crash_reason\":\"%s\"}",
                 NODE_ID, master_crash_reason);
        master_crash_reason[0] = '\0';
    } else {
        snprintf(online_payload, sizeof(online_payload),
                 "{\"status\":\"online\",\"node_id\":\"%s\"}", NODE_ID);
    }
    mqtt.publish(status_topic, online_payload, true);
    Serial.println("[MQTT] OK");
}

void connTask(void *pv) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) setupWiFi();   // no mqtt access here
        xSemaphoreTake(mqtt_mutex, portMAX_DELAY);
        if (!mqtt.connected()) setupMQTT();
        mqtt.loop();
        xSemaphoreGive(mqtt_mutex);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void mqttSendTask(void *pv) {
    for (;;) {
        if (xSemaphoreTake(send_sem, portMAX_DELAY) != pdTRUE) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        xSemaphoreTake(send_mutex, portMAX_DELAY);
        if (send_list.empty()) { xSemaphoreGive(send_mutex); continue; }
        mqtt_out_t item = send_list.front();
        send_list.erase(send_list.begin());
        xSemaphoreGive(send_mutex);

        xSemaphoreTake(mqtt_mutex, portMAX_DELAY);
        if (mqtt.connected()) {
            mqtt.publish(item.topic.c_str(), item.payload.c_str());
            Serial.printf("[MQTT TX] %s  %s\n", item.topic.c_str(), item.payload.c_str());
        } else {
            Serial.printf("[MQTT TX] dropped (not connected): %s\n", item.topic.c_str());
        }
        xSemaphoreGive(mqtt_mutex);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
