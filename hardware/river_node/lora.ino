// Radio layer: frame codec, CAD-before-TX half-duplex discipline, RX/TX/proc
// tasks, reliable alert queue with seq freshness. Spec: docs/PROTOCOL.md §2-§7.

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

// ── Radio state tracking (debug visibility + discovery window sync)
#define RADIO_RX  0
#define RADIO_TX  1
#define RADIO_CAD 2
volatile uint8_t  radio_state    = RADIO_RX;
volatile uint64_t last_tx_msg_id = 0;   // msg_id of the most recently transmitted frame

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

// node ids: alphanumeric + - _ .
static bool id_valid(const char *id) {
    for (const char *p = id; *p; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
    }
    return id[0] != '\0';
}

void format_packet(char *out, size_t len, uint64_t id,
                   const char *src, const char *dst, uint8_t type, const char *payload) {
    snprintf(out, len, "%016llX|%s|%s|%u|%s", id, src, dst, type, payload);
}

// Fetch "name=value" from a k=v payload (first field needs no leading comma)
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

// Map payload type= back to ALERT_KEY_* for relay dedup
static uint8_t parse_alert_key(const char *payload) {
    char t[16];
    if (!get_field(payload, "type", t, sizeof(t))) return 0;
    if (!strcmp(t, "flood"))           return ALERT_KEY_FLOOD;
    if (!strcmp(t, "battery"))         return ALERT_KEY_BATTERY;
    if (!strcmp(t, "gps_moved"))       return ALERT_KEY_GPS_MOVED;
    if (!strcmp(t, "gps_signal_lost") || !strcmp(t, "gps_restored")) return ALERT_KEY_GPS_SIGNAL;
    if (!strcmp(t, "node_lost"))       return ALERT_KEY_NODE_LOST;
    return 0;
}

void enqueueLora(const String& pkt) {
    xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
    lora_tx_list.push_back(pkt);
    xSemaphoreGive(lora_tx_mutex);
    xSemaphoreGive(lora_tx_sem);
}

// Queue a reliable own alert. Same (key, tag) pending ⇒ replace payload and
// stamp a fresh seq — LATEST WINS (PROTOCOL §7 scenario A).
void enqueueAlert(uint8_t type, uint8_t key, const char *tag, const char *payload) {
    xSemaphoreTake(alert_mutex, portMAX_DELAY);
    uint32_t seq = ++alert_seq;
    for (auto &a : alert_list) {
        if (a.key == key && a.relay_src[0] == '\0' && strcmp(a.tag, tag) == 0) {
            strlcpy(a.payload, payload, sizeof(a.payload));
            a.seq = seq;
            a.last_sent_ms = 0;
            a.sent_msg_id  = 0;   // old in-flight id is stale — ACKs for it match nothing
            xSemaphoreGive(alert_mutex);
            xSemaphoreGive(lora_tx_sem);
            Serial.printf("[ALERT] Replaced key=%u tag=%s seq=%lu\n", key, tag, (unsigned long)seq);
            return;
        }
    }
    pending_alert_t a = {};
    a.type = type;
    a.key  = key;
    a.seq  = seq;
    strlcpy(a.tag, tag, sizeof(a.tag));
    strlcpy(a.payload, payload, sizeof(a.payload));
    alert_list.push_back(a);
    xSemaphoreGive(alert_mutex);
    xSemaphoreGive(lora_tx_sem);
    Serial.printf("[ALERT] Queued key=%u tag=%s seq=%lu pending=%u\n",
                  key, tag, (unsigned long)seq, (unsigned)alert_list.size());
}

// A missing child was heard from again (announce/heartbeat relayed through us)
// — drop our pending node_lost so the master is not told stale state (§7B).
static void cancel_offline_alerts_for(const char *src_id) {
    xSemaphoreTake(alert_mutex, portMAX_DELAY);
    for (auto it = alert_list.begin(); it != alert_list.end(); ++it) {
        if (it->key == ALERT_KEY_NODE_LOST && strcmp(it->tag, src_id) == 0) {
            Serial.printf("[ALERT] node_lost %s superseded — %s announced again\n", src_id, src_id);
            alert_list.erase(it);
            break;
        }
    }
    xSemaphoreGive(alert_mutex);
}

// True while a crash alert is still un-ACKed upstream — blocks advertising
// as a relay parent until the report reaches the master.
static bool is_crash_pending() {
    xSemaphoreTake(alert_mutex, portMAX_DELAY);
    for (const auto &a : alert_list) {
        if (a.key == ALERT_KEY_CRASH) { xSemaphoreGive(alert_mutex); return true; }
    }
    xSemaphoreGive(alert_mutex);
    return false;
}

void setupLoRa() {
    SPI.begin();
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
    if (!LoRa.begin(LORA_FREQ_HZ)) {
        Serial.println("[LORA] FATAL — hardware not found (check SPI wiring / CS / RST / DIO0)");
        pinMode(LED_PIN, OUTPUT);
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

// One CAD scan. Leaves the radio back in RX. Fails open on CAD timeout —
// availability beats politeness; ACK/retry recovers any resulting loss.
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

// CAD + random backoff; caller must hold lora_mutex across scan + transmit.
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
    LoRa.endPacket();   // returns on TX-done — post-TX state is exact
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

// ── loraRxTask (core 1, pri 5) — dual-path RX.
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

// ── loraProcTask (core 1, pri 4) — full mesh dispatch
void loraProcTask(void *pv) {
    for (;;) {
        if (xSemaphoreTake(lora_proc_sem, portMAX_DELAY) != pdTRUE) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        xSemaphoreTake(lora_rx_mutex, portMAX_DELAY);
        lora_packet pkt = lora_rx_list.front();
        lora_rx_list.erase(lora_rx_list.begin());
        xSemaphoreGive(lora_rx_mutex);

        if (is_duplicate(pkt.msg_id)) continue;
        add_to_dedup(pkt.msg_id);

        // a node can never legitimately hear itself — ghosts of our own TX
        // that survive FIFO harvest are dropped here
        if (strcmp(pkt.src_id, own_node_id) == 0) continue;

        bool for_us = (strcmp(pkt.dst_id, "ALL") == 0 || strcmp(pkt.dst_id, own_node_id) == 0);
        if (!for_us) {
            // downstream relay: forward to registered child
            if (node_state == NODE_OPERATIONAL) {
                xSemaphoreTake(children_mutex, portMAX_DELAY);
                bool for_child = false;
                for (const auto &r : child_regs) {
                    if (strcmp(r.id, pkt.dst_id) == 0) { for_child = true; break; }
                }
                xSemaphoreGive(children_mutex);
                if (for_child) {
                    char relay[PACKET_MAX_LEN];
                    format_packet(relay, sizeof(relay), pkt.msg_id, pkt.src_id, pkt.dst_id, pkt.type, pkt.payload);
                    enqueueLora(String(relay));
                    Serial.printf("[RELAY DN] type=%u → %s\n", pkt.type, pkt.dst_id);
                }
            }
            continue;
        }

        // upstream packets from our parent count as keepalive
        if (active_parent_id[0] && strcmp(pkt.src_id, active_parent_id) == 0 &&
            (pkt.type == MSG_BEACON  || pkt.type == MSG_REG_ACK ||
             pkt.type == MSG_ACK     || pkt.type == MSG_CMD)) {
            last_parent_seen_ms = millis();
        }

        // update child last-seen for timeout tracking
        xSemaphoreTake(children_mutex, portMAX_DELAY);
        for (auto &r : child_regs) {
            if (strcmp(r.id, pkt.src_id) == 0) { r.last_seen_ms = millis(); break; }
        }
        xSemaphoreGive(children_mutex);

        Serial.printf("[PROC] type=%u src=%s dst=%s\n", pkt.type, pkt.src_id, pkt.dst_id);

        switch (pkt.type) {

        case MSG_BEACON:
            break; // keepalive handled above

        case MSG_ACK: {
            const char *ap = strstr(pkt.payload, "ack_id=");
            if (!ap) break;
            uint64_t ack_id = strtoull(ap + 7, nullptr, 16);
            xSemaphoreTake(alert_mutex, portMAX_DELAY);
            for (auto it = alert_list.begin(); it != alert_list.end(); ++it) {
                if (it->sent_msg_id == ack_id && ack_id != 0) {
                    Serial.printf("[ALERT] ACK from %s — cleared key=%u relay_src=%s\n",
                                  pkt.src_id, it->key,
                                  it->relay_src[0] ? it->relay_src : "(own)");
                    alert_list.erase(it);
                    break;
                }
            }
            xSemaphoreGive(alert_mutex);
            if (ack_id != 0 && pending_hb.sent_msg_id == ack_id) {
                Serial.printf("[HB] ACK from %s — parent alive\n", pkt.src_id);
                pending_hb = {};
            }
            break;
        }

        case MSG_DISC_RESP:
            if (node_state == NODE_DISCOVERING) handle_disc_resp(&pkt);
            break;

        case MSG_REG_ACK:
            if (node_state == NODE_REGISTERING) handle_reg_ack(&pkt);
            break;

        case MSG_DISCOVER:
            if (node_state == NODE_OPERATIONAL) handle_child_discover(&pkt);
            break;

        case MSG_REG_REQ:
            if (node_state == NODE_OPERATIONAL) handle_child_reg_req(&pkt);
            break;

        case MSG_HB:
            if (node_state == NODE_OPERATIONAL && active_parent_id[0] &&
                strcmp(pkt.src_id, active_parent_id) != 0) {
                bool is_child = false;
                xSemaphoreTake(children_mutex, portMAX_DELAY);
                for (const auto &r : child_regs)
                    if (strcmp(r.id, pkt.src_id) == 0) { is_child = true; break; }
                xSemaphoreGive(children_mutex);
                if (is_child) {
                    char ack_pl[28], ack[PACKET_MAX_LEN];
                    snprintf(ack_pl, sizeof(ack_pl), "ack_id=%016llX", (unsigned long long)pkt.msg_id);
                    format_packet(ack, sizeof(ack), new_msg_id(), own_node_id, pkt.src_id, MSG_ACK, ack_pl);
                    enqueueLora(String(ack));
                }
                // relay upstream; a re-announced child's HB cancels our node_lost (§7B)
                cancel_offline_alerts_for(pkt.src_id);
                char relay_pl[PACKET_MAX_LEN], relay[PACKET_MAX_LEN];
                // first relay on the path injects leaf→relay link quality
                if (strstr(pkt.payload, "lsnr=") == nullptr) {
                    snprintf(relay_pl, sizeof(relay_pl), "%s,lsnr=%.1f,lrssi=%d",
                             pkt.payload, pkt.snr, pkt.rssi);
                    relay_pl[sizeof(relay_pl) - 1] = '\0';
                } else {
                    strlcpy(relay_pl, pkt.payload, sizeof(relay_pl));
                }
                format_packet(relay, sizeof(relay), new_msg_id(), pkt.src_id, active_parent_id, MSG_HB, relay_pl);
                enqueueLora(String(relay));
            }
            break;

        case MSG_SENSOR:
            if (node_state == NODE_OPERATIONAL && active_parent_id[0] &&
                strcmp(pkt.src_id, active_parent_id) != 0) {
                char relay[PACKET_MAX_LEN];
                format_packet(relay, sizeof(relay), new_msg_id(), pkt.src_id, active_parent_id, pkt.type, pkt.payload);
                enqueueLora(String(relay));
            }
            break;

        case MSG_ANNOUNCE:
            if (node_state == NODE_OPERATIONAL && active_parent_id[0] &&
                strcmp(pkt.src_id, active_parent_id) != 0) {
                char ack_pl[28], ack[PACKET_MAX_LEN];
                snprintf(ack_pl, sizeof(ack_pl), "ack_id=%016llX", (unsigned long long)pkt.msg_id);
                format_packet(ack, sizeof(ack), new_msg_id(), own_node_id, pkt.src_id, MSG_ACK, ack_pl);
                enqueueLora(String(ack));
                cancel_offline_alerts_for(pkt.src_id);   // §7B
                char relay[PACKET_MAX_LEN];
                format_packet(relay, sizeof(relay), new_msg_id(), pkt.src_id, active_parent_id, MSG_ANNOUNCE, pkt.payload);
                enqueueLora(String(relay));
                Serial.printf("[RELAY ANNOUNCE] ACK + relay → %s\n", pkt.src_id);
            }
            break;

        case MSG_ALERT: {
            if (!(node_state == NODE_OPERATIONAL && active_parent_id[0] &&
                  strcmp(pkt.src_id, active_parent_id) != 0)) break;
            // ACK child — delivery ownership transfers to us for this hop
            char ack_pl[28], ack[PACKET_MAX_LEN];
            snprintf(ack_pl, sizeof(ack_pl), "ack_id=%016llX", (unsigned long long)pkt.msg_id);
            format_packet(ack, sizeof(ack), new_msg_id(), own_node_id, pkt.src_id, MSG_ACK, ack_pl);
            enqueueLora(String(ack));

            // first relay injects leaf→relay link quality (unbounded-growth guard)
            char injected[PACKET_MAX_LEN];
            if (strstr(pkt.payload, "lsnr=") == nullptr) {
                snprintf(injected, sizeof(injected), "%s,lsnr=%.1f,lrssi=%d",
                         pkt.payload, pkt.snr, pkt.rssi);
                injected[sizeof(injected) - 1] = '\0';
            } else {
                strlcpy(injected, pkt.payload, sizeof(injected));
            }

            uint8_t  rkey = parse_alert_key(pkt.payload);
            char     rtag[NODE_ID_MAX_LEN] = "";
            if (rkey == ALERT_KEY_NODE_LOST) get_field(pkt.payload, "lost", rtag, sizeof(rtag));
            uint32_t rseq = payload_seq(pkt.payload);

            xSemaphoreTake(alert_mutex, portMAX_DELAY);
            pending_alert_t *slot = nullptr;
            for (auto &a : alert_list) {
                if (a.relay_src[0] && strcmp(a.relay_src, pkt.src_id) == 0 &&
                    a.key == rkey && strcmp(a.tag, rtag) == 0) { slot = &a; break; }
            }
            if (slot) {
                bool newer         = (rseq > slot->seq);
                bool level_changed = (rkey == ALERT_KEY_FLOOD &&
                                      flood_level_of(pkt.payload) != flood_level_of(slot->payload));
                if (newer || level_changed) {
                    strlcpy(slot->payload, injected, sizeof(slot->payload));
                    if (rseq) slot->seq = rseq;
                    slot->last_sent_ms = 0;   // content changed — send immediately
                    slot->sent_msg_id  = 0;   // old ACKs can no longer clear it
                    Serial.printf("[RELAY ALERT] updated %s key=%u seq=%lu\n",
                                  pkt.src_id, rkey, (unsigned long)rseq);
                }
                // equal-seq retransmission: keep in-flight id, master sees one copy
            } else {
                pending_alert_t a = {};
                a.type = pkt.type;
                a.key  = rkey;
                a.seq  = rseq;
                strlcpy(a.relay_src, pkt.src_id, sizeof(a.relay_src));
                strlcpy(a.tag, rtag, sizeof(a.tag));
                strlcpy(a.payload, injected, sizeof(a.payload));
                alert_list.push_back(a);
                Serial.printf("[RELAY ALERT] queued %s key=%u seq=%lu\n",
                              pkt.src_id, rkey, (unsigned long)rseq);
            }
            xSemaphoreGive(alert_mutex);
            xSemaphoreGive(lora_tx_sem);
            break;
        }

        case MSG_CMD: {
            const char *tp = strstr(pkt.payload, "target=");
            if (!tp) break;
            char target[NODE_ID_MAX_LEN]; strlcpy(target, tp + 7, sizeof(target));
            char *cm = strchr(target, ','); if (cm) *cm = '\0';
            if (strcmp(target, own_node_id) == 0) {
                Serial.printf("[CMD] For us: %s\n", pkt.payload);
                char ack[PACKET_MAX_LEN];
                format_packet(ack, sizeof(ack), new_msg_id(), own_node_id, active_parent_id, MSG_CMD_ACK, pkt.payload);
                enqueueLora(String(ack));
                break;
            }
            xSemaphoreTake(children_mutex, portMAX_DELAY);
            char next[NODE_ID_MAX_LEN] = "";
            for (const auto &r : child_regs) {
                size_t clen = strlen(r.id);
                if (strncmp(target, r.id, clen) == 0 &&
                    (target[clen] == '\0' || target[clen] == '-')) {
                    strlcpy(next, r.id, sizeof(next)); break;
                }
            }
            xSemaphoreGive(children_mutex);
            if (next[0]) {
                char relay[PACKET_MAX_LEN];
                format_packet(relay, sizeof(relay), new_msg_id(), pkt.src_id, next, MSG_CMD, pkt.payload);
                enqueueLora(String(relay));
                Serial.printf("[CMD] Relayed → %s\n", next);
            }
            break;
        }

        case MSG_CMD_ACK:
            // a child's CMD_ACK answers our liveness probe — clear probe state
            xSemaphoreTake(children_mutex, portMAX_DELAY);
            for (auto &r : child_regs) {
                if (strcmp(r.id, pkt.src_id) == 0) {
                    r.probe_sent_ms = 0;
                    r.probe_tries   = 0;
                    break;
                }
            }
            xSemaphoreGive(children_mutex);
            if (node_state == NODE_OPERATIONAL && active_parent_id[0]) {
                char relay[PACKET_MAX_LEN];
                format_packet(relay, sizeof(relay), new_msg_id(), pkt.src_id, active_parent_id, MSG_CMD_ACK, pkt.payload);
                enqueueLora(String(relay));
            }
            break;

        case MSG_TOPO_REQ:
            if (node_state == NODE_OPERATIONAL) {
                send_announce();
                xSemaphoreTake(children_mutex, portMAX_DELAY);
                bool has_ch = !child_regs.empty();
                xSemaphoreGive(children_mutex);
                if (has_ch) {
                    char relay[PACKET_MAX_LEN];
                    format_packet(relay, sizeof(relay), new_msg_id(), own_node_id, "ALL", MSG_TOPO_REQ, "");
                    enqueueLora(String(relay));
                }
            }
            break;

        default: break;
        }
    }
}

// ── loraTxTask (core 1, pri 5) — alerts first, then normal FIFO.
// Every transmission is CAD-gated (PROTOCOL §2).
void loraTxTask(void *pv) {

    for (;;) {
        // 1. Reliable alert queue
        if (node_state == NODE_OPERATIONAL && active_parent_id[0]) {
            xSemaphoreTake(alert_mutex, portMAX_DELAY);
            if (!alert_list.empty()) {
                pending_alert_t &a = alert_list.front();
                uint32_t now = millis();
                if (a.last_sent_ms == 0 ||
                    now - a.last_sent_ms >= ALERT_RETRY_MS + esp_random() % ALERT_JITTER_MS) {
                    char raw[PACKET_MAX_LEN];
                    uint64_t mid = new_msg_id();
                    a.last_sent_ms = now;          // also covers a deferred attempt
                    a.sent_msg_id  = mid;
                    format_packet(raw, sizeof(raw), mid, own_node_id, active_parent_id, a.type, a.payload);
                    xSemaphoreGive(alert_mutex);
                    xSemaphoreTake(lora_mutex, portMAX_DELAY);
                    bool sent = channel_wait_idle();
                    if (sent) {
                        lora_send_locked(raw);
                        last_tx_msg_id = mid;      // discovery window sync
                        Serial.printf("[ALERT TX] %s\n", raw);
                    }
                    xSemaphoreGive(lora_mutex);
                    if (!sent) {
                        // never actually transmitted — clear in-flight mark so an
                        // ACK (impossible) can never clear it, retry fires normally
                        xSemaphoreTake(alert_mutex, portMAX_DELAY);
                        for (auto &e : alert_list)
                            if (e.sent_msg_id == mid) { e.sent_msg_id = 0; break; }
                        xSemaphoreGive(alert_mutex);
                        Serial.println("[ALERT TX] channel busy — deferred");
                    }
                    vTaskDelay(pdMS_TO_TICKS(POST_TX_LISTEN_MS));
                } else {
                    xSemaphoreGive(alert_mutex);
                }
            } else {
                xSemaphoreGive(alert_mutex);
            }
        }

        // 2. Normal queue (block until item or alert-retry timeout)
        if (xSemaphoreTake(lora_tx_sem, pdMS_TO_TICKS(ALERT_RETRY_MS)) == pdTRUE) {
            xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
            if (!lora_tx_list.empty()) {
                String pkt = lora_tx_list.front();
                lora_tx_list.erase(lora_tx_list.begin());
                xSemaphoreGive(lora_tx_mutex);
                xSemaphoreTake(lora_mutex, portMAX_DELAY);
                if (channel_wait_idle()) {
                    lora_send_locked(pkt.c_str());
                    last_tx_msg_id = strtoull(pkt.c_str(), nullptr, 16);
                    Serial.printf("[LORA TX] %s\n", pkt.c_str());
                    // anchor HB retry window to actual TX time
                    if (pending_hb.sent_msg_id != 0) {
                        uint64_t tx_mid = strtoull(pkt.c_str(), nullptr, 16);
                        if (tx_mid == pending_hb.sent_msg_id)
                            pending_hb.last_sent_ms = millis();
                    }
                } else {
                    xSemaphoreTake(lora_tx_mutex, portMAX_DELAY);
                    lora_tx_list.insert(lora_tx_list.begin(), pkt);   // re-queue at front
                    xSemaphoreGive(lora_tx_mutex);
                    Serial.println("[LORA TX] channel busy — re-queued");
                }
                xSemaphoreGive(lora_mutex);
                vTaskDelay(pdMS_TO_TICKS(POST_TX_LISTEN_MS));
            } else {
                xSemaphoreGive(lora_tx_mutex);   // spurious wake
            }
        }
    }
}
