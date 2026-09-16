#include <SPI.h>
#include <LoRa.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <vector>
#include "protocol.h"

// FloodWatch river node v2 — self-healing LoRa mesh with CAD channel
// discipline, per-hop ACK delivery, and seq-based alert freshness.
// Protocol spec: docs/PROTOCOL.md. Provision NVS first (provisioning sketch),
// keys are unchanged from v1; optional "lora_sf" selects the radio profile.

// ── Hardware pins — loaded from NVS by loadConfig()
int LORA_SS    = 5;
int LORA_RST   = 12;
int LORA_DIO0  = 13;
int FLOAT1_PIN = 25;
int FLOAT2_PIN = 26;
int FLOAT3_PIN = 27;
int BAT_PIN    = 35;
int GPS_RX_PIN = 16;
int GPS_TX_PIN = 17;
int LED_PIN    = 2;

// ── Identity — loaded from NVS by loadConfig()
char own_node_id[NODE_ID_MAX_LEN] = "";
char village[VILLAGE_MAX_LEN]     = "";

uint8_t lora_sf = LORA_SF;   // NVS "lora_sf" override (7 demo / 10 field)

// ── ADC / battery — two cascading 1:5 dividers ≈ ×25, hence the cal factor
#define WARMUP_READS 10
#define SAMPLE_COUNT 50
const float ADC_MAX   = 4095.0f;
const float ADC_REF   = 1.1f;
const float BAT_SCALE = 25.0f;
const float BAT_CAL   = 1.256f;

// ── LED patterns (N blinks → 2 s pause → repeat; 0 = steady off)
#define LED_OK               0
#define LED_DISCOVERING      1
#define LED_REGISTERING      2
#define LED_LOST_PARENT      3
#define LED_NO_GPS           4
#define LED_LORA_FAIL        5
#define LED_GPS_NO_NMEA      6
#define LED_GPS_CALIBRATING  7

// ── Mesh state
volatile node_state_t node_state            = NODE_DISCOVERING;
char     active_parent_id[NODE_ID_MAX_LEN]  = "";
uint32_t last_parent_seen_ms                = 0;
uint8_t  own_depth                          = 1;
pending_hb_t pending_hb                    = {};

// Alert freshness counter — random per-boot base so a reboot never replays
// lower seqs than pre-reboot alerts (docs/PROTOCOL.md §3). Seeded in setup().
uint32_t alert_seq = 1;

// ── Crash reporter — filled in setup() if last reboot was abnormal
char pending_crash_reason[16] = "";

// ── Hardware
TinyGPSPlus    gps;
HardwareSerial gps_serial(2);

// ── Sensor state
volatile uint8_t float_bits      = 0;
volatile uint8_t old_float_bits  = 0;
float            battery_voltage = 0.0f;
bool             battery_low_sent      = false;
bool             gps_fix_valid         = false;
double           gps_lat               = 0.0;
double           gps_lng               = 0.0;
uint32_t         last_nmea_ms          = 0;
uint32_t         last_gps_fix_ms       = 0;
uint16_t         gps_cal_samples       = 200;
float            gps_move_thr_m        = 10.0f;
bool             gps_calibrated        = false;
double           gps_home_lat          = 0.0;
double           gps_home_lng          = 0.0;
bool             gps_moved_sent        = false;
bool             gps_signal_lost_sent  = false;

// ── Discovery
disc_candidate_t  candidates[8];
uint8_t           candidate_count = 0;
SemaphoreHandle_t candidates_mutex;
SemaphoreHandle_t reg_ack_sem;

// ── Children (relay role)
uint8_t                   max_children = 1;   // loaded from NVS
std::vector<child_reg_t>  child_regs;
SemaphoreHandle_t         children_mutex;

// ── Dedup (local echo filter only — reliability is ACK + seq)
uint64_t dedup_buf[DEDUP_SIZE] = {};
uint8_t  dedup_idx             = 0;

// ── FreeRTOS: LoRa
SemaphoreHandle_t         lora_mutex;
SemaphoreHandle_t         lora_rx_sem;
SemaphoreHandle_t         lora_rx_mutex;
SemaphoreHandle_t         lora_proc_sem;
std::vector<lora_packet>  lora_rx_list;

// ── FreeRTOS: normal TX
SemaphoreHandle_t   lora_tx_mutex;
SemaphoreHandle_t   lora_tx_sem;
std::vector<String> lora_tx_list;

// ── FreeRTOS: reliable alert queue
SemaphoreHandle_t            alert_mutex;
std::vector<pending_alert_t> alert_list;

// ── Other handles
SemaphoreHandle_t float_change_sem;
SemaphoreHandle_t cad_done_sem;          // CAD scan finished (from ISR)
volatile bool     cad_activity = false;  // true = preamble detected
TaskHandle_t      h_led = nullptr;

// ── Task handles (watermark monitoring)
TaskHandle_t h_lora_rx   = nullptr;
TaskHandle_t h_lora_tx   = nullptr;
TaskHandle_t h_lora_proc = nullptr;
TaskHandle_t h_disc      = nullptr;
TaskHandle_t h_gps       = nullptr;
TaskHandle_t h_float     = nullptr;
TaskHandle_t h_hb        = nullptr;
TaskHandle_t h_beacon    = nullptr;
TaskHandle_t h_status    = nullptr;

void setup() {
    Serial.begin(115200);

    pinMode(Vext);

    // Detect abnormal reboot before anything else runs
    {
        esp_reset_reason_t rr = esp_reset_reason();
        if      (rr == ESP_RST_PANIC)                         strlcpy(pending_crash_reason, "panic",    sizeof(pending_crash_reason));
        else if (rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT) strlcpy(pending_crash_reason, "watchdog", sizeof(pending_crash_reason));
        else if (rr == ESP_RST_BROWNOUT)                      strlcpy(pending_crash_reason, "brownout", sizeof(pending_crash_reason));
        if (pending_crash_reason[0])
            Serial.printf("[CRASH] Prior crash detected: %s — will report once operational\n", pending_crash_reason);
    }

    loadConfig();
    Serial.printf("[BOOT] node_id=%s  village=%s  sf=%u\n", own_node_id, village, lora_sf);

    alert_seq = esp_random() | 1;   // non-zero random base (PROTOCOL §3)
    setupVoltage();
    setupLoRa();
    setupGPS();
    setupFloatSensors();
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);

    lora_mutex       = xSemaphoreCreateMutex();
    lora_rx_sem      = xSemaphoreCreateBinary();
    lora_rx_mutex    = xSemaphoreCreateMutex();
    lora_proc_sem    = xSemaphoreCreateCounting(50, 0);
    lora_tx_mutex    = xSemaphoreCreateMutex();
    lora_tx_sem      = xSemaphoreCreateCounting(50, 0);
    alert_mutex      = xSemaphoreCreateMutex();
    candidates_mutex = xSemaphoreCreateMutex();
    reg_ack_sem      = xSemaphoreCreateBinary();
    children_mutex   = xSemaphoreCreateMutex();
    float_change_sem = xSemaphoreCreateCounting(10, 0);
    cad_done_sem     = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(loraRxTask,    "lora_rx",   6144, nullptr, 5, &h_lora_rx,   1);
    xTaskCreatePinnedToCore(loraTxTask,    "lora_tx",   4096, nullptr, 5, &h_lora_tx,   1);
    xTaskCreatePinnedToCore(loraProcTask,  "lora_proc", 6144, nullptr, 4, &h_lora_proc, 1);
    xTaskCreatePinnedToCore(discoveryTask, "disc",      4096, nullptr, 4, &h_disc,      0);
    xTaskCreatePinnedToCore(gpsTask,       "gps",       6144, nullptr, 3, &h_gps,       0);
    xTaskCreatePinnedToCore(floatTask,     "float",     3072, nullptr, 3, &h_float,     0);
    xTaskCreatePinnedToCore(heartbeatTask, "hb",        3072, nullptr, 3, &h_hb,        0);
    xTaskCreatePinnedToCore(beaconTask,    "beacon",    3072, nullptr, 3, &h_beacon,    0);
    xTaskCreatePinnedToCore(ledTask,       "led",       1536, nullptr, 1, &h_led,       0);
    xTaskCreatePinnedToCore(statusTask,    "status",    4096, nullptr, 2, &h_status,    0);

    Serial.printf("[BOOT] River node ready — %s\n", own_node_id);
    vTaskDelete(NULL);
}

void loop() {}
