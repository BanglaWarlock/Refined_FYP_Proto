// WiFi/MQTT backhaul: connect, LWT, subscribe, publish queue drain.

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

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    Serial.printf("[MQTT RX] topic=%s  payload=%s\n", topic, msg.c_str());
    xSemaphoreTake(cmd_mutex, portMAX_DELAY);
    cmd_list.push_back(msg);
    xSemaphoreGive(cmd_mutex);
    xSemaphoreGive(cmd_sem);
}

void setupWiFi() {
    WiFi.begin(wifi_ssid, wifi_pass);
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
    char base[64], status_topic[96], lwt_payload[128], sub_topic[96], client_id[32];
    topic_base(base, sizeof(base));
    snprintf(status_topic, sizeof(status_topic), "%s/master/status", base);
    snprintf(lwt_payload,  sizeof(lwt_payload),  "{\"status\":\"offline\",\"node_id\":\"%s\"}", own_node_id);
    snprintf(sub_topic,    sizeof(sub_topic),    "%s/cmd", base);
    snprintf(client_id,    sizeof(client_id),    "master-%s", village);

    mqtt.setServer(mqtt_broker, mqtt_port);
    mqtt.setCallback(onMqttMessage);
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

    mqtt.subscribe(sub_topic);

    char online_payload[160];
    if (master_crash_reason[0]) {
        snprintf(online_payload, sizeof(online_payload),
                 "{\"status\":\"online\",\"node_id\":\"%s\",\"crash_reason\":\"%s\"}",
                 own_node_id, master_crash_reason);
        master_crash_reason[0] = '\0';   // reported once
    } else {
        snprintf(online_payload, sizeof(online_payload),
                 "{\"status\":\"online\",\"node_id\":\"%s\"}", own_node_id);
    }
    mqtt.publish(status_topic, online_payload, true);
    Serial.println("[MQTT] OK");
}

void ensureConnections() {
    if (WiFi.status() != WL_CONNECTED) { setupWiFi(); setupMQTT(); return; }
    if (!mqtt.connected()) setupMQTT();
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

void connTask(void *pv) {
    for (;;) {
        xSemaphoreTake(mqtt_mutex, portMAX_DELAY);
        ensureConnections();
        mqtt.loop();
        xSemaphoreGive(mqtt_mutex);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void cmdHandlerTask(void *pv) {
    for (;;) {
        if (xSemaphoreTake(cmd_sem, portMAX_DELAY) != pdTRUE) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        xSemaphoreTake(cmd_mutex, portMAX_DELAY);
        if (cmd_list.empty()) { xSemaphoreGive(cmd_mutex); continue; }
        String msg = cmd_list.front();
        cmd_list.erase(cmd_list.begin());
        xSemaphoreGive(cmd_mutex);

        Serial.printf("[CMD] %s\n", msg.c_str());
        processCmd(msg);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
