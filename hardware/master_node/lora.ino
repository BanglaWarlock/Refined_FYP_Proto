// Radio layer: frame codec, CAD-before-TX half-duplex discipline, RX/TX/proc
// tasks. Same discipline as the river nodes (docs/PROTOCOL.md §2-§3).

// ── DIO0 ISRs (dispatched by IRQ flags inside the library).
// RX has two complementary paths: if DIO0 works, the ISR flags the packet
// (µs latency); if DIO0 is dead/miswired, the RX task's parsePacket() poll
// catches the same IRQ flag over SPI instead. Only one path services a given
// packet (the ISR clears the flag first when it fires); duplicates fall to
// msg_id dedup. Callbacks run inside the library's SPI spinlock — flags only.
volatile int      lora_rx_size    = 0;
volatile bool     lora_rx_ready   = false;
volatile bool     cad_in_progress = false;   // RX task must not parsePacket mid-CAD

void IRAM_ATTR onLoRaReceive(int packetSize) {
    if (packetSize <= 0 || packetSize > PACKET_MAX_LEN - 1) return;
    lora_rx_size  = packetSize;
    lora_rx_ready = true;
}

void IRAM_ATTR onLoraCadDone(bool activity) {
    cad_activity  = activity;
    cad_done_flag = true;
}

// ── Radio state tracking (debug visibility)
#define RADIO_RX  0
#define RADIO_TX  1
#define RADIO_CAD 2
volatile uint8_t radio_state = RADIO_RX;

static void radio_set(uint8_t s) {
    if (radio_state != s) {
        static const char *nm[] = {"RX", "TX", "CAD"};
        radio_state = s;
        Serial.printf("[RADIO] → %s\n", nm[s]);
    }
}

uint64_t new_msg_id() { return (uint64_t)esp_random() << 32 | esp_random(); }

bool is_duplicate(uint64_t id) {
    for (uint8_t i = 0; i < DEDUP_SIZE; i++) if (dedup_buf[i] == id) return true;
    return false;
}
void add_to_dedup(uint64_t id) { dedup_buf[dedup_idx] = id; dedup_idx = (dedup_idx + 1) % DEDUP_SIZE; }

// node ids: alphanumeric + - _ .
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
    // msg_id: exactly 16 hex chars
    tok = strtok_r(buf, "|", &sp);
    if (!tok || strlen(tok) != 16) return false;
    for (const char *h = tok; *h; h++) if (!isxdigit((unsigned char)*h)) return false;
    pkt->msg_id = strtoull(tok, nullptr, 16);
    // ids: printable, bounded
    tok = strtok_r(nullptr, "|", &sp);
    if (!tok || strlen(tok) >= NODE_ID_MAX_LEN || !id_valid(tok)) return false;
    strlcpy(pkt->src_id, tok, NODE_ID_MAX_LEN);
    tok = strtok_r(nullptr, "|", &sp);
    if (!tok || strlen(tok) >= NODE_ID_MAX_LEN ||
        (strcmp(tok, "ALL") != 0 && !id_valid(tok))) return false;
    strlcpy(pkt->dst_id, tok, NODE_ID_MAX_LEN);
    tok = strtok_r(nullptr, "|", &sp);
    if (!tok || atoi(tok) > MSG_RELAY_REQ) return false;
    pkt->type = (uint8_t)atoi(tok);
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
    LoRa.onReceive(onLoRaReceive);   // fast path when DIO0 works
    LoRa.onCadDone(onLoraCadDone);
    LoRa.receive();
    radio_set(RADIO_RX);
    Serial.println("[LORA] OK");
}

// One CAD scan; leaves the radio back in RX. Fails open on timeout.
// Called with lora_mutex held; cad_in_progress keeps the RX poller out.
// Completion is detected via the cadResult() register poll — the DIO0 ISR
// (cad_done_flag) is just a faster path when the interrupt works.
static bool channel_idle(TickType_t timeout) {
    cad_activity    = false;
    cad_done_flag   = false;
    cad_in_progress = true;
    radio_set(RADIO_CAD);
    LoRa.channelActivityDetection();
    TickType_t waited = 0;
    int result = -1;                   // -1 pending, 0 idle, 1 activity
    while (waited < timeout) {
        if (cad_done_flag) { result = cad_activity ? 1 : 0; break; }
        int r = LoRa.cadResult();
        if (r >= 0) { result = r; break; }
        vTaskDelay(pdMS_TO_TICKS(2));
        waited += 2;
    }
    LoRa.receive();                    // CAD leaves the chip in standby
    radio_set(RADIO_RX);
    cad_in_progress = false;
    if (result < 0) return true;       // fail open
    return result == 0;
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
    radio_set(RADIO_TX);
    LoRa.beginPacket();
    LoRa.print(raw);
    LoRa.endPacket();   // returns on TX-done
    LoRa.receive();
    radio_set(RADIO_RX);
}

// Frame harvesting: FIFO state across TX/CAD churn is not always trustworthy
// — reads may span several historical FIFO writes with stray bytes between
// fragments. Frames are self-describing (16-hex msg_id + '|'), so scan for
// boundaries, extract each candidate, and let parse_packet's hard validation
// decide. Valid ghosts of our own traffic are dropped in loraProcTask.
static void harvest_frames(char *buf, int n, int rssi, float snr) {
    const int MIN_FRAME = 22;                 // 16 id + 4 '|' + src + dst + type
    int i = 0;
    while (i + MIN_FRAME <= n) {
        bool hex16 = isxdigit((unsigned char)buf[i]);
        for (int k = 1; hex16 && k < 16; k++) hex16 = isxdigit((unsigned char)buf[i + k]);
        if (!hex16 || buf[i + 16] != '|') { i++; continue; }
        int end = n;                          // frame runs to next boundary or end
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

// ── loraRxTask (core 1, pri 3) — dual-path RX.
// ISR path (DIO0 alive): flag + byte count delivered at RxDone.
// Poll path (DIO0 dead): parsePacket() reads the IRQ flag over SPI.
// Whatever the byte count, harvest_frames() recovers every complete frame.
void loraRxTask(void *pv) {
    char raw[PACKET_MAX_LEN];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3));
        if (cad_in_progress) continue;            // CAD owns the radio
        bool isr = lora_rx_ready;
        if (isr) { lora_rx_ready = false; }

        xSemaphoreTake(lora_mutex, portMAX_DELAY);
        int sz;
        if (isr) {
            sz = lora_rx_size;                    // FIFO ptr already set by ISR
        } else {
            sz = LoRa.parsePacket();              // IRQ-flag poll, no DIO0 needed
            if (sz > PACKET_MAX_LEN - 1) sz = PACKET_MAX_LEN - 1;
        }
        if (sz <= 0) {
            xSemaphoreGive(lora_mutex);
            continue;
        }
        int n = 0;
        while (n < sz) {
            int b = LoRa.read();
            if (b < 0) break;                     // FIFO exhausted
            raw[n++] = (char)b;
        }
        float snr = LoRa.packetSnr(); int rssi = LoRa.packetRssi();
        if (!isr) { LoRa.receive(); radio_set(RADIO_RX); }   // parsePacket left RX mode — re-arm
        xSemaphoreGive(lora_mutex);
        if (n < 22) continue;
        raw[n] = '\0';
        Serial.printf("[LORA RX] rssi=%d snr=%.1f  %s\n", rssi, snr, raw);
        harvest_frames(raw, n, rssi, snr);
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

        // a master can never legitimately hear itself — ghosts of our own TX
        // that survive FIFO harvest are dropped here
        if (strcmp(pkt.src_id, own_node_id) == 0) continue;

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
