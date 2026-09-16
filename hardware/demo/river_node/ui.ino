// Status LED patterns and periodic serial status dump.

void led_set_pattern(uint8_t p) { if (h_led) xTaskNotify(h_led, p, eSetValueWithOverwrite); }

void ledTask(void *pv) {
    uint8_t pattern = LED_DISCOVERING;
    for (;;) {
        if (pattern == LED_OK) {
            digitalWrite(PIN_LED, LOW);
            uint32_t np; xTaskNotifyWait(0, 0xFFFFFFFF, &np, portMAX_DELAY);
            pattern = (uint8_t)np; continue;
        }
        for (uint8_t i = 0; i < pattern; i++) {
            digitalWrite(PIN_LED, HIGH); vTaskDelay(pdMS_TO_TICKS(200));
            digitalWrite(PIN_LED, LOW);
            if (i < pattern - 1) vTaskDelay(pdMS_TO_TICKS(200));
        }
        uint32_t np;
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &np, pdMS_TO_TICKS(2000)) == pdTRUE) pattern = (uint8_t)np;
    }
}

void statusTask(void *pv) {
    TickType_t last = xTaskGetTickCount();
    const char *ss[] = {"DISCOVERING", "REGISTERING", "OPERATIONAL"};
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(STATUS_INTERVAL_MS));
        xSemaphoreTake(alert_mutex, portMAX_DELAY);
        size_t pending = alert_list.size();
        xSemaphoreGive(alert_mutex);
        Serial.printf("\n--- STATUS  state=%-12s  id=%s  parent=%s  bat=%.2fV  float=0x%02X  fix=%s  alerts=%u ---\n",
                      ss[(int)node_state], NODE_ID,
                      active_parent_id[0] ? active_parent_id : "(none)",
                      battery_voltage, float_bits, gps_fix_valid ? "YES" : "NO", (unsigned)pending);
        if (gps_fix_valid) Serial.printf("    gps: %.6f, %.6f\n", gps_lat, gps_lng);
        Serial.println("------------------------------------------------------");
    }
}
