// Status LED patterns and periodic serial status dump.

void led_set_pattern(uint8_t p) { if (h_led) xTaskNotify(h_led, p, eSetValueWithOverwrite); }

void ledTask(void *pv) {
    uint8_t pattern = LED_DISCOVERING;
    for (;;) {
        if (pattern == LED_OK) {
            digitalWrite(LED_PIN, LOW);
            uint32_t np; xTaskNotifyWait(0, 0xFFFFFFFF, &np, portMAX_DELAY);
            pattern = (uint8_t)np; continue;
        }
        for (uint8_t i = 0; i < pattern; i++) {
            digitalWrite(LED_PIN, HIGH); vTaskDelay(pdMS_TO_TICKS(200));
            digitalWrite(LED_PIN, LOW);
            if (i < pattern - 1) vTaskDelay(pdMS_TO_TICKS(200));
        }
        uint32_t np;
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &np, pdMS_TO_TICKS(2000)) == pdTRUE) pattern = (uint8_t)np;
    }
}

static void _wm(const char *name, TaskHandle_t h) {
    UBaseType_t wm = uxTaskGetStackHighWaterMark(h);
    Serial.printf("  %-12s free=%u%s\n", name, (unsigned)wm, wm < 512 ? " *** LOW ***" : "");
}

void statusTask(void *pv) {
    TickType_t last = xTaskGetTickCount();
    const char *ss[] = {"DISCOVERING", "REGISTERING", "OPERATIONAL", "LOST_PARENT"};
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(STATUS_INTERVAL_MS));
        xSemaphoreTake(alert_mutex, portMAX_DELAY);
        size_t pending = alert_list.size();
        xSemaphoreGive(alert_mutex);
        xSemaphoreTake(children_mutex, portMAX_DELAY);
        std::vector<child_reg_t> snap = child_regs;
        xSemaphoreGive(children_mutex);
        uint8_t cc = (uint8_t)snap.size();
        uint32_t now_ms = millis();
        Serial.printf("\n--- STATUS  state=%-12s  id=%s  parent=%s  depth=%u  bat=%.2fV  float=0x%02X  fix=%s  alerts=%u  children=%u ---\n",
                      ss[(int)node_state], own_node_id,
                      active_parent_id[0] ? active_parent_id : "(none)",
                      own_depth, battery_voltage, float_bits,
                      gps_fix_valid ? "YES" : "NO", (unsigned)pending, cc);
        for (auto &r : snap) {
            uint32_t age = (r.last_seen_ms > 0) ? (now_ms - r.last_seen_ms) : 0;
            bool stale = (r.last_seen_ms > 0 && age > CHILD_OFFLINE_TIMEOUT_MS);
            Serial.printf("  child: %-12s  last_seen=%lums ago  %s\n",
                          r.id, (unsigned long)age, stale ? "STALE" : "OK");
        }
        if (gps_fix_valid) Serial.printf("    gps: %.6f, %.6f\n", gps_lat, gps_lng);

        Serial.println("[STACK watermarks (bytes free)]");
        _wm("lora_rx",  h_lora_rx);
        _wm("lora_tx",  h_lora_tx);
        _wm("lora_proc",h_lora_proc);
        _wm("disc",     h_disc);
        _wm("gps",      h_gps);
        _wm("float",    h_float);
        _wm("hb",       h_hb);
        _wm("beacon",   h_beacon);
        _wm("status",   h_status);

        Serial.println("------------------------------------------------------");
    }
}
