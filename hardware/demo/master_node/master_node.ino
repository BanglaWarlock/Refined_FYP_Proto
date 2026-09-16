// FloodWatch DEMO master node — pairs 1:1 (or few:1) with river nodes.
// Simplified from the mesh build: no relay routing, no probes, no commands.
// The master's job: answer discovery/registration, track every child's live
// state, ACK reliable alerts (seq-deduped), and publish everything to MQTT.

#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <vector>
#include "config.h"

// ── Wire format: <msg_id:16hex>|<src>|<dst>|<type:u8>|<payload>
#define NODE_ID_MAX_LEN  20
#define PACKET_MAX_LEN  200
#define MAX_NODES         16
#define DEDUP_SIZE       32

typedef enum : uint8_t {
    MSG_HB = 5, MSG_ALERT = 7, MSG_ANNOUNCE = 11,
    MSG_DISCOVER = 12, MSG_DISC_RESP = 13, MSG_REG_REQ = 14, MSG_REG_ACK = 15,
    MSG_ACK = 17
} msg_type_t;

typedef struct {
    uint64_t msg_id;
    char     src_id[NODE_ID_MAX_LEN];
    char     dst_id[NODE_ID_MAX_LEN];
    uint8_t  type;
    char     payload[PACKET_MAX_LEN];
    int      rssi;
    float    snr;
} lora_packet;

typedef struct {
    char     node_id[NODE_ID_MAX_LEN];
    bool     is_online;
    uint32_t last_seen_ms;
    uint32_t last_announce_ms;
    uint32_t last_alert_seq;    // newest alert seq accepted — older = stale
    uint8_t  depth;
    float    battery_voltage;
    uint8_t  float_bits;
    float    snr;               // leaf→master link quality
    int      rssi;
    double   lat, lng;
    bool     gps_fix;
} registered_node;

// ── Timing (demo)
#define POST_TX_LISTEN_MS        100
#define CAD_TIMEOUT_MS           100
#define CAD_BACKOFF_MS            40
#define CAD_MAX_TRIES              5
#define STATUS_INTERVAL_MS     10000

#define LED_LORA_FAIL 5

// ── Radio state (debug visibility)
#define RADIO_RX  0
#define RADIO_TX  1
#define RADIO_CAD 2
volatile uint8_t radio_state = RADIO_RX;
volatile bool    cad_done_flag   = false;
volatile bool    cad_in_progress = false;
volatile bool    cad_activity    = false;

static void radio_set(uint8_t s) {
    if (radio_state != s) {
        static const char *nm[] = {"RX", "TX", "CAD"};
        radio_state = s;
        Serial.printf("[RADIO] → %s\n", nm[s]);
    }
}

// ── Crash reporter — included in the first MQTT online publish
char master_crash_reason[16] = "";

WiFiClient   wifi_client;
PubSubClient mqtt(wifi_client);

// ── Node registry
registered_node   node_registry[MAX_NODES];
uint8_t           node_count = 0;
SemaphoreHandle_t registry_mutex;

// ── Dedup (echo filter only)
uint64_t dedup_buf[DEDUP_SIZE] = {};
uint8_t  dedup_idx             = 0;

// ── LoRa queues
SemaphoreHandle_t         lora_mutex;
SemaphoreHandle_t         lora_rx_mutex;
SemaphoreHandle_t         lora_proc_sem;
std::vector<lora_packet>  lora_rx_list;
SemaphoreHandle_t         lora_tx_mutex;
SemaphoreHandle_t         lora_tx_sem;
std::vector<String>       lora_tx_list;

// ── MQTT queues
SemaphoreHandle_t       mqtt_mutex;
struct mqtt_out_t { String topic; String payload; };
std::vector<mqtt_out_t> send_list;
SemaphoreHandle_t       send_mutex;
SemaphoreHandle_t       send_sem;

#define SEND_LIST_MAX 150

// ── Task handles
TaskHandle_t h_lora_rx = nullptr, h_lora_tx = nullptr, h_lora_proc = nullptr,
             h_health = nullptr, h_conn = nullptr, h_mqtt_send = nullptr,
             h_status = nullptr;

void setup() {
    Serial.begin(115200);

    {
        esp_reset_reason_t rr = esp_reset_reason();
        if      (rr == ESP_RST_PANIC)                         strlcpy(master_crash_reason, "panic",    sizeof(master_crash_reason));
        else if (rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT) strlcpy(master_crash_reason, "watchdog", sizeof(master_crash_reason));
        else if (rr == ESP_RST_BROWNOUT)                      strlcpy(master_crash_reason, "brownout", sizeof(master_crash_reason));
        if (master_crash_reason[0])
            Serial.printf("[CRASH] Prior crash detected: %s\n", master_crash_reason);
    }

    Serial.printf("[BOOT] node_id=%s  village=%s  deploy=%s  sf=%d\n",
                  NODE_ID, VILLAGE, DEPLOY, LORA_SF);
    pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);
    setupLoRa();
    setupWiFi();
    setupMQTT();

    registry_mutex = xSemaphoreCreateMutex();
    lora_mutex     = xSemaphoreCreateMutex();
    lora_rx_mutex  = xSemaphoreCreateMutex();
    lora_proc_sem  = xSemaphoreCreateCounting(50, 0);
    lora_tx_mutex  = xSemaphoreCreateMutex();
    lora_tx_sem    = xSemaphoreCreateCounting(50, 0);
    mqtt_mutex     = xSemaphoreCreateMutex();
    send_mutex     = xSemaphoreCreateMutex();
    send_sem       = xSemaphoreCreateCounting(50, 0);

    xTaskCreatePinnedToCore(loraRxTask,   "lora_rx",   6144, nullptr, 3, &h_lora_rx,   1);
    xTaskCreatePinnedToCore(loraTxTask,   "lora_tx",   4096, nullptr, 3, &h_lora_tx,   1);
    xTaskCreatePinnedToCore(loraProcTask, "lora_proc", 6144, nullptr, 2, &h_lora_proc, 1);
    xTaskCreatePinnedToCore(healthTask,   "health",    4096, nullptr, 1, &h_health,    0);
    xTaskCreatePinnedToCore(connTask,     "conn",      8192, nullptr, 1, &h_conn,      0);
    xTaskCreatePinnedToCore(mqttSendTask, "mqtt_send", 4096, nullptr, 2, &h_mqtt_send, 0);
    xTaskCreatePinnedToCore(statusTask,   "status",    4096, nullptr, 1, &h_status,    0);

    Serial.printf("[BOOT] Master ready — %s\n", NODE_ID);
    vTaskDelete(NULL);
}

void loop() {}
