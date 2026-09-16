#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <vector>
#include "protocol.h"

// FloodWatch master node v2 — village gateway. LoRa mesh ↔ WiFi/MQTT.
// v2: CAD channel discipline (matches river v2), seq-based alert dedup,
// node_lost alerts with offline suppression, deployment-slug topic prefix.

// ── Hardware pins — loaded from NVS by loadConfig()
int LORA_SS   = 5;
int LORA_RST  = 12;
int LORA_DIO0 = 13;
int LED_PIN   = 2;

#define LED_LORA_FAIL 5   // fatal — LoRa hardware not found at boot

// ── Network — loaded from NVS
char wifi_ssid[64]   = "";
char wifi_pass[64]   = "";
char mqtt_broker[64] = "";
int  mqtt_port       = 1883;

// ── Identity — loaded from NVS
char own_node_id[NODE_ID_MAX_LEN] = "";
char village[VILLAGE_MAX_LEN]     = "";

// Deployment slug: topic prefix floodwatch/<deploy>/<village>/...
// keeps demos isolated on public brokers; empty = floodwatch/<village>/...
char deploy[24] = "";
uint8_t lora_sf = LORA_SF;   // NVS "lora_sf" override

// ── Crash reporter — included in first MQTT online publish, then cleared
char master_crash_reason[16] = "";

WiFiClient   wifi_client;
PubSubClient mqtt(wifi_client);

// ── Node registry
uint8_t           max_children = 1;
registered_node   node_registry[MAX_REGISTERED_NODES];
uint8_t           node_count  = 0;
SemaphoreHandle_t registry_mutex;

// ── Dedup ring buffer (local echo filter — reliability is ACK + seq)
uint64_t dedup_buf[DEDUP_SIZE] = {};
uint8_t  dedup_idx             = 0;

// ── Pending downstream commands
std::vector<pending_cmd_t> pending_cmds;
SemaphoreHandle_t          pending_cmd_mutex;

// ── LoRa queues
SemaphoreHandle_t         lora_mutex;
SemaphoreHandle_t         lora_rx_mutex;
SemaphoreHandle_t         lora_proc_sem;
std::vector<lora_packet>  lora_rx_list;
SemaphoreHandle_t         lora_tx_mutex;
SemaphoreHandle_t         lora_tx_sem;
std::vector<String>       lora_tx_list;

// ── CAD
volatile bool cad_done_flag = false;   // set by CAD ISR (LoRa lib spinlock)
volatile bool cad_activity  = false;   // true = preamble detected

// ── MQTT queues
SemaphoreHandle_t       mqtt_mutex;
struct mqtt_out_t { String topic; String payload; };
std::vector<mqtt_out_t> send_list;
SemaphoreHandle_t       send_mutex;
SemaphoreHandle_t       send_sem;
std::vector<String>     cmd_list;
SemaphoreHandle_t       cmd_mutex;
SemaphoreHandle_t       cmd_sem;

// send_list cap — bounds heap growth during long MQTT outages
#define SEND_LIST_MAX 150

// ── Task handles (watermark monitoring)
TaskHandle_t h_lora_rx     = nullptr;
TaskHandle_t h_lora_tx     = nullptr;
TaskHandle_t h_lora_proc   = nullptr;
TaskHandle_t h_beacon      = nullptr;
TaskHandle_t h_health      = nullptr;
TaskHandle_t h_cmd_retry   = nullptr;
TaskHandle_t h_conn        = nullptr;
TaskHandle_t h_mqtt_send   = nullptr;
TaskHandle_t h_cmd_handler = nullptr;
TaskHandle_t h_status      = nullptr;

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

    loadConfig();
    Serial.printf("[BOOT] node_id=%s  village=%s  deploy=%s  sf=%u\n",
                  own_node_id, village, deploy[0] ? deploy : "(none)", lora_sf);
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
    setupLoRa();
    setupWiFi();
    setupMQTT();

    registry_mutex    = xSemaphoreCreateMutex();
    pending_cmd_mutex = xSemaphoreCreateMutex();
    lora_mutex     = xSemaphoreCreateMutex();
    lora_rx_mutex  = xSemaphoreCreateMutex();
    lora_proc_sem  = xSemaphoreCreateCounting(50, 0);
    lora_tx_mutex  = xSemaphoreCreateMutex();
    lora_tx_sem    = xSemaphoreCreateCounting(50, 0);
    mqtt_mutex     = xSemaphoreCreateMutex();
    cmd_mutex      = xSemaphoreCreateMutex();
    cmd_sem        = xSemaphoreCreateCounting(50, 0);
    send_mutex     = xSemaphoreCreateMutex();
    send_sem       = xSemaphoreCreateCounting(50, 0);

    xTaskCreatePinnedToCore(loraRxTask,     "lora_rx",     4096, nullptr, 3, &h_lora_rx,     1);
    xTaskCreatePinnedToCore(loraTxTask,     "lora_tx",     4096, nullptr, 3, &h_lora_tx,     1);
    xTaskCreatePinnedToCore(loraProcTask,   "lora_proc",   6144, nullptr, 2, &h_lora_proc,   1);
    xTaskCreatePinnedToCore(beaconTask,     "beacon",      3072, nullptr, 1, &h_beacon,      1);
    xTaskCreatePinnedToCore(healthTask,     "health",      4096, nullptr, 1, &h_health,      0);
    xTaskCreatePinnedToCore(cmdRetryTask,   "cmd_retry",   3072, nullptr, 1, &h_cmd_retry,   0);
    xTaskCreatePinnedToCore(connTask,       "conn",        8192, nullptr, 1, &h_conn,        0);
    xTaskCreatePinnedToCore(mqttSendTask,   "mqtt_send",   4096, nullptr, 2, &h_mqtt_send,   0);
    xTaskCreatePinnedToCore(cmdHandlerTask, "cmd_handler", 4096, nullptr, 2, &h_cmd_handler, 0);
    xTaskCreatePinnedToCore(statusTask,     "status",      4096, nullptr, 1, &h_status,      0);

    Serial.printf("[BOOT] Master ready — %s\n", own_node_id);
    vTaskDelete(NULL);
}

void loop() {}
