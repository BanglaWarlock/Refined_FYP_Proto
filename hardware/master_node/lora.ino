// Radio layer: frame codec, CAD-before-TX half-duplex discipline, RX/TX/proc
// tasks. Same discipline as the river nodes (docs/PROTOCOL.md §2-§3).

// ── ISRs (DIO0) — dispatched by IRQ flags inside the library
void IRAM_ATTR onLoRaReceive(int packetSize) {
    if (packetSize == 0) return;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(lora_rx_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void IRAM_ATTR onLoraCadDone(bool activity) {
    cad_activity = activity;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(cad_done_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

uint64_t new_msg_id() { return (uint64_t)esp_random() << 32 | esp_random(); }

bool is_duplicate(uint64_t id) {
    for (uint8_t i = 0; i < DEDUP_SIZE; i++) if (dedup_buf[i] == id) return true;
    return false;
}
void add_to_dedup(uint64_t id) { dedup_buf[dedup_idx] = id; dedup_idx = (dedup_idx + 1) % DEDUP_SIZE; }

bool parse_packet(const char *raw, lora_packet *pkt) {
    char buf[PACKET_MAX_LEN]; strlcpy(buf, raw, sizeof(buf));
    char *sp, *tok;
    tok = strtok_r(buf, "|", &sp); if (!tok) return false;
    pkt->msg_id = strtoull(tok, nullptr, 16);
    tok = strtok_r(nullptr, "|", &sp); if (!tok) return false; strlcpy(pkt->src_id, tok, NODE_ID_MAX_LEN);
    tok = strtok_r(nullptr, "|", &sp); if (!tok) return false; strlcpy(pkt->dst_id, tok, NODE_ID_MAX_LEN);
    tok = strtok_r(nullptr, "|", &sp); if (!tok) return false; pkt->type = (uint8_t)atoi(tok);
    strlcpy(pkt->payload, sp ? sp : "", sizeof(pkt->payload));
    return true;
}

void format_packet(char *out, size_t len, uint64_t id,
                   const char *src, const char *dst, uint8_t type, const char *payload) {
    snprintf(out, len, "%016llX|%s|%s|%u|%s", id, src, dst, type, payload);
}

// Fetch "name=value" from a k=v payload (first field needs no leading comma).
// Boundary-safe, unlike bare strstr — e.g. "lat=" would otherwise match inside
// "last_lat=".
static bool get_field(const char *payload, const char *name, char *out, size_t outlen) {
    char pat[24]; size_t plen;
    snprintf(pat, sizeof(pat), ",%s=", name);
    const char *p = strstr(payload, pat);
    if (p) {
        p += (plen = strlen(pat));
    } else {
        snprintf(pat, sizeof(pat), "%s=", name);
        plen = strlen(pat);
        if (strncmp(payload, pat, plen) != 0) return false;
        p = payload + plen;
    }
    const char *e = strchr(p, ',');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n); out[n] = '\0';
    return true;
}

static uint32_t payload_seq(const char *payload) {
    char b[12];
    if (!get_field(payload, "seq", b, sizeof(b))) return 0;
    return (uint32_t)strtoul(b, nullptr, 10);
}

void enqueueLora(const String& pkt) {
    xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
    lora_tx_list.push_back(pkt);
    xSemaphoreGive(lora_tx_mutex);
    xSemaphoreGive(lora_tx_sem);
}

void setupLoRa() {
    SPI.begin();
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
    if (!LoRa.begin(LORA_FREQ_HZ)) {
        Serial.println("[LORA] FATAL — hardware not found (check SPI wiring / CS / RST / DIO0)");
        for (uint8_t i = 0; i < LED_LORA_FAIL; i++) {
            digitalWrite(LED_PIN, HIGH); delay(200);
            digitalWrite(LED_PIN, LOW);  delay(200);
        }
        delay(2000);
        esp_restart();
    }
    LoRa.setSpreadingFactor(lora_sf);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setTxPower(LORA_TX_PWR);
    LoRa.enableCrc();
    LoRa.onReceive(onLoRaReceive);
    LoRa.onCadDone(onLoraCadDone);
    LoRa.receive();
    Serial.println("[LORA] OK");
}

// One CAD scan; leaves the radio back in RX. Fails open on timeout.
static bool channel_idle(TickType_t timeout) {
    cad_activity = false;
    xSemaphoreTake(cad_done_sem, 0);   // drain stale token
    LoRa.channelActivityDetection();
    if (xSemaphoreTake(cad_done_sem, timeout) != pdTRUE) {
        LoRa.receive();
        return true;
    }
    LoRa.receive();                    // CAD leaves the chip in standby
    return !cad_activity;
}

// CAD + random backoff; caller holds lora_mutex across scan + transmit.
static bool channel_wait_idle() {
    for (uint8_t i = 0; i < CAD_MAX_TRIES; i++) {
        if (channel_idle(pdMS_TO_TICKS(CAD_TIMEOUT_MS))) return true;
        vTaskDelay(pdMS_TO_TICKS(CAD_BACKOFF_MS + esp_random() % (4 * CAD_BACKOFF_MS)));
    }
    return false;
}

// Blocking transmit + return to RX. Caller holds lora_mutex.
static void lora_send_locked(const char *raw) {
    LoRa.beginPacket();
    LoRa.print(raw);
    LoRa.endPacket();   // returns on TX-done
    LoRa.receive();
}

// ── loraRxTask (core 1, pri 3) — ISR driven
void loraRxTask(void *pv) {
    char raw[PACKET_MAX_LEN];
    for (;;) {
        xSemaphoreTake(lora_rx_sem, portMAX_DELAY);
        xSemaphoreTake(lora_mutex, portMAX_DELAY);
        int n = 0;
        while (LoRa.available() && n < PACKET_MAX_LEN - 1) raw[n++] = (char)LoRa.read();
        raw[n] = '\0';
        float snr = LoRa.packetSnr(); int rssi = LoRa.packetRssi();
        xSemaphoreGive(lora_mutex);
        if (n == 0) continue;
        Serial.printf("[LORA RX] rssi=%d snr=%.1f  %s\n", rssi, snr, raw);
        lora_packet pkt;
        if (!parse_packet(raw, &pkt)) continue;
        pkt.rssi = rssi; pkt.snr = snr;
        xSemaphoreTake(lora_rx_mutex, portMAX_DELAY);
        lora_rx_list.push_back(pkt);
        xSemaphoreGive(lora_rx_mutex);
        xSemaphoreGive(lora_proc_sem);
    }
}

// ── loraProcTask (core 1, pri 2) — dedup + dispatch to handlers
void loraProcTask(void *pv) {
    for (;;) {
        if (xSemaphoreTake(lora_proc_sem, portMAX_DELAY) != pdTRUE) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        xSemaphoreTake(lora_rx_mutex, portMAX_DELAY);
        lora_packet pkt = lora_rx_list.front();
        lora_rx_list.erase(lora_rx_list.begin());
        xSemaphoreGive(lora_rx_mutex);

        bool for_us = (strcmp(pkt.dst_id, own_node_id) == 0 || strcmp(pkt.dst_id, "ALL") == 0);
        if (!for_us || is_duplicate(pkt.msg_id)) continue;
        add_to_dedup(pkt.msg_id);

        Serial.printf("[PROC] type=%u src=%s dst=%s\n", pkt.type, pkt.src_id, pkt.dst_id);
        switch (pkt.type) {
            case MSG_DISCOVER:  handle_discover(&pkt);  break;
            case MSG_REG_REQ:   handle_reg_req(&pkt);   break;
            case MSG_ANNOUNCE:  handle_announce(&pkt);  break;
            case MSG_HB:        handle_hb_sensor(&pkt); break;
            case MSG_ALERT:     handle_alert(&pkt);     break;
            case MSG_CMD_ACK:   handle_cmd_ack(&pkt);   break;
            case MSG_RELAY_REQ: Serial.printf("[RELAY] MSG_RELAY_REQ from %s (not handled)\n", pkt.src_id); break;
            default: break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── loraTxTask (core 1, pri 3) — FIFO, every TX CAD-gated
void loraTxTask(void *pv) {
    for (;;) {
        if (xSemaphoreTake(lora_tx_sem, portMAX_DELAY) != pdTRUE) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
        if (lora_tx_list.empty()) { xSemaphoreGive(lora_tx_mutex); continue; }
        String pkt = lora_tx_list.front();
        lora_tx_list.erase(lora_tx_list.begin());
        xSemaphoreGive(lora_tx_mutex);

        xSemaphoreTake(lora_mutex, portMAX_DELAY);
        if (channel_wait_idle()) {
            lora_send_locked(pkt.c_str());
            Serial.printf("[LORA TX] %s\n", pkt.c_str());
        } else {
            xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
            lora_tx_list.insert(lora_tx_list.begin(), pkt);   // re-queue at front
            xSemaphoreGive(lora_tx_mutex);
            Serial.println("[LORA TX] channel busy — re-queued");
        }
        xSemaphoreGive(lora_mutex);
        vTaskDelay(pdMS_TO_TICKS(POST_TX_LISTEN_MS));
    }
}
