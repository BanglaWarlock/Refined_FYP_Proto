// Sensors: float switches (ISR + debounce), battery ADC, GPS task with
// calibration, movement / fix-lost alerts, GPS-driven LED status.

void IRAM_ATTR onFloat1Change() {
    if (digitalRead(FLOAT1_PIN) == HIGH) float_bits |= 0x01; else float_bits &= ~0x01;
    BaseType_t w = pdFALSE; xSemaphoreGiveFromISR(float_change_sem, &w); if (w) portYIELD_FROM_ISR();
}

void IRAM_ATTR onFloat2Change() {
    if (digitalRead(FLOAT2_PIN) == HIGH) float_bits |= 0x02; else float_bits &= ~0x02;
    BaseType_t w = pdFALSE; xSemaphoreGiveFromISR(float_change_sem, &w); if (w) portYIELD_FROM_ISR();
}
void IRAM_ATTR onFloat3Change() {
    if (digitalRead(FLOAT3_PIN) == HIGH) float_bits |= 0x04; else float_bits &= ~0x04;
    BaseType_t w = pdFALSE; xSemaphoreGiveFromISR(float_change_sem, &w); if (w) portYIELD_FROM_ISR();
}

float read_battery_voltage() {
    for (int i = 0; i < WARMUP_READS; i++) { analogRead(BAT_PIN); vTaskDelay(pdMS_TO_TICKS(2)); }
    long sum = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) { sum += analogRead(BAT_PIN); vTaskDelay(pdMS_TO_TICKS(2)); }
    return (sum / (float)SAMPLE_COUNT / ADC_MAX) * ADC_REF * BAT_SCALE * BAT_CAL;
}

uint8_t getWaterLevel(uint8_t bits) {
    if (bits & 0x04) return 3;
    if (bits & 0x02) return 2;
    if (bits & 0x01) return 1;
    return 0;
}

void setupVoltage() {
    analogReadResolution(12);
    analogSetAttenuation(ADC_0db);
    Serial.println("[ADC] 12-bit 0dB ref=1.1V");
}

void setupFloatSensors() {
    dacDisable(FLOAT1_PIN); dacDisable(FLOAT2_PIN);   // GPIO25/26 are DAC pins
    pinMode(FLOAT1_PIN, INPUT_PULLUP);
    pinMode(FLOAT2_PIN, INPUT_PULLUP);
    pinMode(FLOAT3_PIN, INPUT_PULLUP);
    if (digitalRead(FLOAT1_PIN) == HIGH) float_bits |= 0x01;
    if (digitalRead(FLOAT2_PIN) == HIGH) float_bits |= 0x02;
    if (digitalRead(FLOAT3_PIN) == HIGH) float_bits |= 0x04;
    old_float_bits = float_bits;
    attachInterrupt(FLOAT1_PIN, onFloat1Change, CHANGE);
    attachInterrupt(FLOAT2_PIN, onFloat2Change, CHANGE);
    attachInterrupt(FLOAT3_PIN, onFloat3Change, CHANGE);
    Serial.printf("[FLOAT] init bits=0x%02X  level=%u\n", float_bits, getWaterLevel(float_bits));
}

void setupGPS() {
    gps_serial.setPins(GPS_RX_PIN, GPS_TX_PIN);
    gps_serial.begin(9600);
    Serial.println("[GPS] serial started");
}

void gpsTask(void *pv) {
    bool     nmea_silent  = false;
    uint8_t  last_led_gps = 0xFF;
    uint16_t cal_count    = 0;
    double   cal_lat_sum  = 0.0, cal_lng_sum = 0.0;

    for (;;) {
        while (gps_serial.available()) {
            if (gps.encode(gps_serial.read())) last_nmea_ms = millis();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        uint32_t now = millis();
        bool silent = (now > GPS_NMEA_TIMEOUT_MS) &&
                      (last_nmea_ms == 0 || (now - last_nmea_ms > GPS_NMEA_TIMEOUT_MS));
        if (silent && !nmea_silent) { nmea_silent = true;  Serial.println("[GPS] No NMEA"); }
        if (!silent && nmea_silent) { nmea_silent = false; Serial.println("[GPS] NMEA resumed"); }

        if (gps.location.isValid() && gps.location.isUpdated()) {
            gps_lat = gps.location.lat(); gps_lng = gps.location.lng();
            if (!gps_fix_valid) {
                Serial.println("[GPS] Fix acquired");
                if (gps_calibrated && gps_signal_lost_sent) send_alert_gps_fix_restored();
                gps_signal_lost_sent = false;
            }
            gps_fix_valid = true; last_gps_fix_ms = now;

            if (!gps_calibrated) {
                cal_lat_sum += gps_lat;
                cal_lng_sum += gps_lng;
                if (++cal_count % 20 == 0)
                    Serial.printf("[GPS CAL] %u/%u samples\n", cal_count, gps_cal_samples);
                if (cal_count >= gps_cal_samples) {
                    gps_home_lat  = cal_lat_sum / cal_count;
                    gps_home_lng  = cal_lng_sum / cal_count;
                    gps_calibrated = true;
                    Preferences prefs;
                    prefs.begin("floodwatch", false);
                    prefs.putDouble("install_lat",   gps_home_lat);
                    prefs.putDouble("install_lng",   gps_home_lng);
                    prefs.putBool("install_gps_set", true);
                    prefs.end();
                    Serial.printf("[GPS CAL] Complete — home=%.6f,%.6f\n", gps_home_lat, gps_home_lng);
                    if (node_state == NODE_OPERATIONAL) send_announce();
                }
            } else {
                double dist = TinyGPSPlus::distanceBetween(gps_home_lat, gps_home_lng, gps_lat, gps_lng);
                if (!gps_moved_sent && dist > gps_move_thr_m) {
                    send_alert_gps_moved(dist);
                    gps_moved_sent = true;
                } else if (dist <= gps_move_thr_m) {
                    gps_moved_sent = false;
                }
            }
        }
        if (gps_fix_valid && (now - last_gps_fix_ms > GPS_FIX_TIMEOUT_MS)) {
            gps_fix_valid = false;
            Serial.println("[GPS] Fix expired");
            if (gps_calibrated && !gps_signal_lost_sent) {
                send_alert_gps_signal_lost();
                gps_signal_lost_sent = true;
            }
        }

        if (node_state == NODE_OPERATIONAL) {
            uint8_t want;
            if (nmea_silent)          want = LED_GPS_NO_NMEA;
            else if (!gps_fix_valid)  want = LED_NO_GPS;
            else if (!gps_calibrated) want = LED_GPS_CALIBRATING;
            else                      want = LED_OK;
            if (want != last_led_gps) { led_set_pattern(want); last_led_gps = want; }
        } else {
            last_led_gps = 0xFF;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void floatTask(void *pv) {
    for (;;) {
        xSemaphoreTake(float_change_sem, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(FLOAT_DEBOUNCE_MS));
        while (xSemaphoreTake(float_change_sem, 0) == pdTRUE);
        uint8_t cur = float_bits;
        if (cur != old_float_bits) {
            old_float_bits = cur;
            Serial.printf("[FLOAT] bits=0x%02X  level=%u\n", cur, getWaterLevel(cur));
            send_alert_flood(getWaterLevel(cur));
        }
    }
}
