// Sensors: float switches (ISR + debounce), battery ADC, GPS calibration,
// movement / fix-lost alerts, GPS-driven LED status.

void IRAM_ATTR onFloat1Change() {
    if (digitalRead(PIN_FLOAT_1FT) == HIGH) float_bits |= 0x01; else float_bits &= ~0x01;
    BaseType_t w = pdFALSE; xSemaphoreGiveFromISR(float_change_sem, &w); if (w) portYIELD_FROM_ISR();
}

void IRAM_ATTR onFloat2Change() {
    if (digitalRead(PIN_FLOAT_2FT) == HIGH) float_bits |= 0x02; else float_bits &= ~0x02;
    BaseType_t w = pdFALSE; xSemaphoreGiveFromISR(float_change_sem, &w); if (w) portYIELD_FROM_ISR();
}
void IRAM_ATTR onFloat3Change() {
    if (digitalRead(PIN_FLOAT_3FT) == HIGH) float_bits |= 0x04; else float_bits &= ~0x04;
    BaseType_t w = pdFALSE; xSemaphoreGiveFromISR(float_change_sem, &w); if (w) portYIELD_FROM_ISR();
}

float read_battery_voltage() {
    for (int i = 0; i < 10; i++) { analogRead(PIN_BAT_ADC); vTaskDelay(pdMS_TO_TICKS(2)); }
    long sum = 0;
    for (int i = 0; i < 50; i++) { sum += analogRead(PIN_BAT_ADC); vTaskDelay(pdMS_TO_TICKS(2)); }
    return (sum / 50.0f / 4095.0f) * 1.1f * BAT_SCALE * BAT_CAL;
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
    dacDisable(PIN_FLOAT_1FT); dacDisable(PIN_FLOAT_2FT);   // GPIO25/26 are DAC pins
    pinMode(PIN_FLOAT_1FT, INPUT_PULLUP);
    pinMode(PIN_FLOAT_2FT, INPUT_PULLUP);
    pinMode(PIN_FLOAT_3FT, INPUT_PULLUP);
    if (digitalRead(PIN_FLOAT_1FT) == HIGH) float_bits |= 0x01;
    if (digitalRead(PIN_FLOAT_2FT) == HIGH) float_bits |= 0x02;
    if (digitalRead(PIN_FLOAT_3FT) == HIGH) float_bits |= 0x04;
    old_float_bits = float_bits;
    attachInterrupt(PIN_FLOAT_1FT, onFloat1Change, CHANGE);
    attachInterrupt(PIN_FLOAT_2FT, onFloat2Change, CHANGE);
    attachInterrupt(PIN_FLOAT_3FT, onFloat3Change, CHANGE);
    Serial.printf("[FLOAT] init bits=0x%02X  level=%u\n", float_bits, getWaterLevel(float_bits));
}

void setupGPS() {
    gps_serial.setPins(PIN_GPS_RX, PIN_GPS_TX);
    gps_serial.begin(9600);
    Serial.println("[GPS] serial started");
}

void gpsTask(void *pv) {
    bool     nmea_silent  = false;
    uint8_t  last_led_gps = 0xFF;

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
                gps_cal_lat_sum += gps_lat;
                gps_cal_lng_sum += gps_lng;
                if (++gps_cal_count % 20 == 0)
                    Serial.printf("[GPS CAL] %u/%u samples\n", gps_cal_count, (unsigned)GPS_CAL_SAMPLES);
                if (gps_cal_count >= GPS_CAL_SAMPLES) {
                    gps_home_lat  = gps_cal_lat_sum / gps_cal_count;
                    gps_home_lng  = gps_cal_lng_sum / gps_cal_count;
                    gps_calibrated = true;
                    Serial.printf("[GPS CAL] Complete — home=%.6f,%.6f\n", gps_home_lat, gps_home_lng);
                    if (node_state == NODE_OPERATIONAL) send_announce();
                }
            } else {
                double dist = TinyGPSPlus::distanceBetween(gps_home_lat, gps_home_lng, gps_lat, gps_lng);
                if (!gps_moved_sent && dist > GPS_MOVE_THR_M) {
                    send_alert_gps_moved(dist);
                    gps_moved_sent = true;
                } else if (dist <= GPS_MOVE_THR_M) {
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
