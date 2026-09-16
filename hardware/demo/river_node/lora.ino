// Radio layer — deliberately simple for the 1:1 demo:
//   RX  : poll parsePacket() every 3 ms (no DIO0, no interrupts)
//   TX  : blocking send, back in RX immediately
//   CDC : none — collisions are near-impossible at 1:1, and the ACK/retry
//         layer recovers the rest
// Frame harvesting + hard validation still guard against FIFO residue.

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
    tok = strtok_r(buf, "|", &sp);                       // msg_id: 16 hex chars
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

static uint8_t flood_level_of(const char *payload) {
    char b[4];
    if (!get_field(payload, "level", b, sizeof(b))) return 0;
    return (uint8_t)atoi(b);
}

void enqueueLora(const String& pkt) {
    xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
    lora_tx_list.push_back(pkt);
    xSemaphoreGive(lora_tx_mutex);
    xSemaphoreGive(lora_tx_sem);
}

// Queue a reliable alert. Same key pending ⇒ replace payload + fresh seq
// (LATEST WINS: a level-2 alert always overtakes an in-flight level-1).
void enqueueAlert(uint8_t type, uint8_t key, const char *payload) {
    xSemaphoreTake(alert_mutex, portMAX_DELAY);
    uint32_t seq = ++alert_seq;
    for (auto &a : alert_list) {
        if (a.key == key) {
            strlcpy(a.payload, payload, sizeof(a.payload));
            a.seq = seq;
            a.sent_msg_id = 0;   // old in-flight id is stale — its ACK matches nothing
            xSemaphoreGive(alert_mutex);
            xSemaphoreGive(lora_tx_sem);
            Serial.printf("[ALERT] Replaced key=%u seq=%lu\n", key, (unsigned long)seq);
            return;
        }
    }
    pending_alert_t a = {};
    a.type = type;
    a.key  = key;
    a.seq  = seq;
    strlcpy(a.payload, payload, sizeof(a.payload));
    alert_list.push_back(a);
    xSemaphoreGive(alert_mutex);
    xSemaphoreGive(lora_tx_sem);
    Serial.printf("[ALERT] Queued key=%u seq=%lu pending=%u\n",
                  key, (unsigned long)seq, (unsigned)alert_list.size());
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
    LoRa.receive();                  // RX continuous — polled, no DIO0 needed
    radio_set(RADIO_RX);
    Serial.println("[LORA] OK");
}

static void lora_send_locked(const char *raw) {
    radio_set(RADIO_TX);
    LoRa.beginPacket();
    LoRa.print(raw);
    LoRa.endPacket();                // blocking — returns on TX-done
    LoRa.receive();                  // straight back to RX continuous
    radio_set(RADIO_RX);
}

// Frame harvesting — FIFO reads may span historical writes with stray bytes;
// frames are self-describing (16-hex msg_id + '|'), so scan boundaries and
// hard-validate each candidate. Ghosts of our own TX are dropped in proc.
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

// ── loraRxTask — pure parsePacket() polling. The RxDone flag waits in the
// chip's IRQ register until we read it (3 ms poll vs 65 ms airtime), so no
// interrupt is needed and a dead/flapping DIO0 is irrelevant.
void loraRxTask(void *pv) {
    char raw[PACKET_MAX_LEN];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3));
        xSemaphoreTake(lora_mutex, portMAX_DELAY);
        int sz = LoRa.parsePacket();
        if (sz <= 0) {
            xSemaphoreGive(lora_mutex);
            continue;
        }
        if (sz > PACKET_MAX_LEN - 1) sz = PACKET_MAX_LEN - 1;
        int n = 0;
        while (n < sz) {
            int b = LoRa.read();
            if (b < 0) break;
            raw[n++] = (char)b;
        }
        float snr = LoRa.packetSnr(); int rssi = LoRa.packetRssi();
        LoRa.receive();              // parsePacket left RX mode — re-arm
        radio_set(RADIO_RX);
        xSemaphoreGive(lora_mutex);
        if (n < 22) continue;
        raw[n] = '\0';
        Serial.printf("[LORA RX] rssi=%d snr=%.1f  %s\n", rssi, snr, raw);
        harvest_frames(raw, n, rssi, snr);
    }
}

// ── loraProcTask — ACKs and registration only; everything else ignored.
void loraProcTask(void *pv) {
    for (;;) {
        if (xSemaphoreTake(lora_proc_sem, portMAX_DELAY) != pdTRUE) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        xSemaphoreTake(lora_rx_mutex, portMAX_DELAY);
        lora_packet pkt = lora_rx_list.front();
        lora_rx_list.erase(lora_rx_list.begin());
        xSemaphoreGive(lora_rx_mutex);

        if (is_duplicate(pkt.msg_id)) continue;
        add_to_dedup(pkt.msg_id);
        if (strcmp(pkt.src_id, NODE_ID) == 0) continue;   // our own TX ghosts
        if (pkt.dst_id[0] && strcmp(pkt.dst_id, "ALL") != 0 &&
            strcmp(pkt.dst_id, NODE_ID) != 0) continue;   // not for us

        Serial.printf("[PROC] type=%u src=%s dst=%s\n", pkt.type, pkt.src_id, pkt.dst_id);

        switch (pkt.type) {

        case MSG_ACK: {
            const char *ap = strstr(pkt.payload, "ack_id=");
            if (!ap) break;
            uint64_t ack_id = strtoull(ap + 7, nullptr, 16);
            xSemaphoreTake(alert_mutex, portMAX_DELAY);
            for (auto it = alert_list.begin(); it != alert_list.end(); ++it) {
                if (it->sent_msg_id == ack_id && ack_id != 0) {
                    Serial.printf("[ALERT] ACKed key=%u seq=%lu\n",
                                  it->key, (unsigned long)it->seq);
                    alert_list.erase(it);
                    break;
                }
            }
            xSemaphoreGive(alert_mutex);
            break;
        }

        case MSG_DISC_RESP:
            if (node_state == NODE_DISCOVERING) handle_disc_resp(&pkt);
            break;

        case MSG_REG_ACK:
            if (node_state == NODE_REGISTERING) handle_reg_ack(&pkt);
            break;

        default: break;
        }
    }
}

// ── loraTxTask — reliable alerts first, then the normal FIFO.
void loraTxTask(void *pv) {
    for (;;) {
        // 1. Alert queue — retried until ACKed, newest seq wins
        if (node_state == NODE_OPERATIONAL && active_parent_id[0]) {
            xSemaphoreTake(alert_mutex, portMAX_DELAY);
            if (!alert_list.empty()) {
                pending_alert_t &a = alert_list.front();
                uint32_t now = millis();
                if (a.last_sent_ms == 0 ||
                    now - a.last_sent_ms >= ALERT_RETRY_MS + esp_random() % ALERT_JITTER_MS) {
                    char raw[PACKET_MAX_LEN];
                    uint64_t mid = new_msg_id();
                    if (a.first_sent_ms == 0) a.first_sent_ms = now;
                    a.last_sent_ms = now;
                    a.sent_msg_id  = mid;
                    format_packet(raw, sizeof(raw), mid, NODE_ID, active_parent_id, a.type, a.payload);
                    xSemaphoreGive(alert_mutex);
                    xSemaphoreTake(lora_mutex, portMAX_DELAY);
                    lora_send_locked(raw);
                    last_tx_msg_id = mid;
                    xSemaphoreGive(lora_mutex);
                    Serial.printf("[ALERT TX] %s\n", raw);
                    vTaskDelay(pdMS_TO_TICKS(POST_TX_LISTEN_MS));
                } else {
                    xSemaphoreGive(alert_mutex);
                }
            } else {
                xSemaphoreGive(alert_mutex);
            }
        }

        // 2. Normal queue
        if (xSemaphoreTake(lora_tx_sem, pdMS_TO_TICKS(ALERT_RETRY_MS)) == pdTRUE) {
            xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
            if (!lora_tx_list.empty()) {
                String pkt = lora_tx_list.front();
                lora_tx_list.erase(lora_tx_list.begin());
                xSemaphoreGive(lora_tx_mutex);
                xSemaphoreTake(lora_mutex, portMAX_DELAY);
                lora_send_locked(pkt.c_str());
                last_tx_msg_id = strtoull(pkt.c_str(), nullptr, 16);
                xSemaphoreGive(lora_mutex);
                Serial.printf("[LORA TX] %s\n", pkt.c_str());
                vTaskDelay(pdMS_TO_TICKS(POST_TX_LISTEN_MS));
            } else {
                xSemaphoreGive(lora_tx_mutex);
            }
        }
    }
}
