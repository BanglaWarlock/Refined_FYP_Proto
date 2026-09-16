// Radio layer — same discipline as the demo river node (and the mesh build):
// CAD via register polling, dual-path RX, frame harvesting, hard validation.

volatile int lora_rx_size  = 0;
volatile bool lora_rx_ready = false;

void IRAM_ATTR onLoRaReceive(int packetSize) {
    if (packetSize <= 0 || packetSize > PACKET_MAX_LEN - 1) return;
    lora_rx_size  = packetSize;
    lora_rx_ready = true;
}

void IRAM_ATTR onLoraCadDone(bool activity) {
    cad_activity  = activity;
    cad_done_flag = true;
}

uint64_t new_msg_id() { return (uint64_t)esp_random() << 32 | esp_random(); }

bool is_duplicate(uint64_t id) {
    for (uint8_t i = 0; i < DEDUP_SIZE; i++) if (dedup_buf[i] == id) return true;
    return false;
}
void add_to_dedup(uint64_t id) { dedup_buf[dedup_idx] = id; dedup_idx = (dedup_idx + 1) % DEDUP_SIZE; }

static bool id_valid(const char *id) {
    for (const char *p = id; *p; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return id[0] != '\0';
}

bool parse_packet(const char *raw, lora_packet *pkt) {
    char buf[PACKET_MAX_LEN]; strlcpy(buf, raw, sizeof(buf));
    char *sp, *tok;
    tok = strtok_r(buf, "|", &sp);
    if (!tok || strlen(tok) != 16) return false;
    for (const char *h = tok; *h; h++) if (!isxdigit((unsigned char)*h)) return false;
    pkt->msg_id = strtoull(tok, nullptr, 16);
    tok = strtok_r(nullptr, "|", &sp);
    if (!tok || strlen(tok) >= NODE_ID_MAX_LEN || !id_valid(tok)) return false;
    strlcpy(pkt->src_id, tok, NODE_ID_MAX_LEN);
    tok = strtok_r(nullptr, "|", &sp);
    if (!tok || strlen(tok) >= NODE_ID_MAX_LEN ||
        (strcmp(tok, "ALL") != 0 && !id_valid(tok))) return false;
    strlcpy(pkt->dst_id, tok, NODE_ID_MAX_LEN);
    tok = strtok_r(nullptr, "|", &sp);
    if (!tok || atoi(tok) > 17) return false;
    pkt->type = (uint8_t)atoi(tok);
    strlcpy(pkt->payload, sp ? sp : "", sizeof(pkt->payload));
    return true;
}

void format_packet(char *out, size_t len, uint64_t id,
                   const char *src, const char *dst, uint8_t type, const char *payload) {
    snprintf(out, len, "%016llX|%s|%s|%u|%s", id, src, dst, type, payload);
}

// Fetch "name=value" from a k=v payload (boundary-safe: "lat=" must not match
// inside "last_lat=")
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

// Topic prefix: floodwatch/<deploy>/<village> or floodwatch/<village>
void topic_base(char *out, size_t len) {
    if (DEPLOY[0]) snprintf(out, len, "floodwatch/%s/%s", DEPLOY, VILLAGE);
    else           snprintf(out, len, "floodwatch/%s", VILLAGE);
}

void enqueueLora(const String& pkt) {
    xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
    lora_tx_list.push_back(pkt);
    xSemaphoreGive(lora_tx_mutex);
    xSemaphoreGive(lora_tx_sem);
}

void setupLoRa() {
    SPI.begin();
    LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);
    if (!LoRa.begin(433E6)) {
        Serial.println("[LORA] FATAL — hardware not found");
        for (uint8_t i = 0; i < LED_LORA_FAIL; i++) {
            digitalWrite(PIN_LED, HIGH); delay(200);
            digitalWrite(PIN_LED, LOW);  delay(200);
        }
        delay(2000);
        esp_restart();
    }
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.setTxPower(LORA_TX_PWR);
    LoRa.enableCrc();
    LoRa.onReceive(onLoRaReceive);
    LoRa.onCadDone(onLoraCadDone);
    LoRa.receive();
    radio_set(RADIO_RX);
    Serial.println("[LORA] OK");
}

static bool channel_idle(TickType_t timeout) {
    cad_activity    = false;
    cad_done_flag   = false;
    cad_in_progress = true;
    radio_set(RADIO_CAD);
    LoRa.channelActivityDetection();
    TickType_t waited = 0;
    int result = -1;
    while (waited < timeout) {
        if (cad_done_flag) { result = cad_activity ? 1 : 0; break; }
        int r = LoRa.cadResult();
        if (r >= 0) { result = r; break; }
        vTaskDelay(pdMS_TO_TICKS(2));
        waited += 2;
    }
    LoRa.receive();
    radio_set(RADIO_RX);
    cad_in_progress = false;
    if (result < 0) return true;     // fail open
    return result == 0;
}

static bool channel_wait_idle() {
    for (uint8_t i = 0; i < CAD_MAX_TRIES; i++) {
        if (channel_idle(pdMS_TO_TICKS(CAD_TIMEOUT_MS))) return true;
        vTaskDelay(pdMS_TO_TICKS(CAD_BACKOFF_MS + esp_random() % (4 * CAD_BACKOFF_MS)));
    }
    return false;
}

static void lora_send_locked(const char *raw) {
    radio_set(RADIO_TX);
    LoRa.beginPacket();
    LoRa.print(raw);
    LoRa.endPacket();
    LoRa.receive();
    radio_set(RADIO_RX);
}

static void harvest_frames(char *buf, int n, int rssi, float snr) {
    const int MIN_FRAME = 22;
    int i = 0;
    while (i + MIN_FRAME <= n) {
        bool hex16 = isxdigit((unsigned char)buf[i]);
        for (int k = 1; hex16 && k < 16; k++) hex16 = isxdigit((unsigned char)buf[i + k]);
        if (!hex16 || buf[i + 16] != '|') { i++; continue; }
        int end = n;
        for (int j = i + 17; j + MIN_FRAME <= n; j++) {
            bool h = isxdigit((unsigned char)buf[j]);
            for (int k = 1; h && k < 16; k++) h = isxdigit((unsigned char)buf[j + k]);
            if (h && buf[j + 16] == '|') { end = j; break; }
        }
        char saved = buf[end]; buf[end] = '\0';
        lora_packet pkt;
        if (parse_packet(buf + i, &pkt)) {
            pkt.rssi = rssi; pkt.snr = snr;
            xSemaphoreTake(lora_rx_mutex, portMAX_DELAY);
            lora_rx_list.push_back(pkt);
            xSemaphoreGive(lora_rx_mutex);
            xSemaphoreGive(lora_proc_sem);
            Serial.printf("[FRAME] type=%u src=%s dst=%s\n", pkt.type, pkt.src_id, pkt.dst_id);
        }
        buf[end] = saved;
        i = end;
    }
}

// ── loraRxTask — dual path: ISR flag (DIO0 alive) or parsePacket poll.
void loraRxTask(void *pv) {
    char raw[PACKET_MAX_LEN];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3));
        if (cad_in_progress) continue;
        bool isr = lora_rx_ready;
        if (isr) { lora_rx_ready = false; }

        xSemaphoreTake(lora_mutex, portMAX_DELAY);
        int sz;
        if (isr) {
            sz = lora_rx_size;
        } else {
            sz = LoRa.parsePacket();
            if (sz > PACKET_MAX_LEN - 1) sz = PACKET_MAX_LEN - 1;
        }
        if (sz <= 0) {
            xSemaphoreGive(lora_mutex);
            continue;
        }
        int n = 0;
        while (n < sz) {
            int b = LoRa.read();
            if (b < 0) break;
            raw[n++] = (char)b;
        }
        float snr = LoRa.packetSnr(); int rssi = LoRa.packetRssi();
        if (!isr) { LoRa.receive(); radio_set(RADIO_RX); }
        xSemaphoreGive(lora_mutex);
        if (n < 22) continue;
        raw[n] = '\0';
        Serial.printf("[LORA RX] rssi=%d snr=%.1f  %s\n", rssi, snr, raw);
        harvest_frames(raw, n, rssi, snr);
    }
}

// ── loraProcTask — dedup + dispatch; every frame from a river is for us.
void loraProcTask(void *pv) {
    for (;;) {
        if (xSemaphoreTake(lora_proc_sem, portMAX_DELAY) != pdTRUE) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        xSemaphoreTake(lora_rx_mutex, portMAX_DELAY);
        lora_packet pkt = lora_rx_list.front();
        lora_rx_list.erase(lora_rx_list.begin());
        xSemaphoreGive(lora_rx_mutex);

        bool for_us = (strcmp(pkt.dst_id, NODE_ID) == 0 || strcmp(pkt.dst_id, "ALL") == 0);
        if (!for_us || is_duplicate(pkt.msg_id)) continue;
        add_to_dedup(pkt.msg_id);
        if (strcmp(pkt.src_id, NODE_ID) == 0) continue;   // our own TX ghosts

        Serial.printf("[PROC] type=%u src=%s dst=%s\n", pkt.type, pkt.src_id, pkt.dst_id);
        switch (pkt.type) {
            case MSG_DISCOVER:  handle_discover(&pkt);  break;
            case MSG_REG_REQ:   handle_reg_req(&pkt);   break;
            case MSG_ANNOUNCE:  handle_announce(&pkt);  break;
            case MSG_HB:        handle_hb(&pkt);        break;
            case MSG_ALERT:     handle_alert(&pkt);     break;
            default: break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── loraTxTask — simple FIFO, every TX CAD-gated.
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
