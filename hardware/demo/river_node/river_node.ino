// FloodWatch DEMO river node — pairs 1:1 with a master. No mesh, no relay,
// no parent-liveness state machine: register once, then alerts retry until
// the master ACKs them and heartbeats fire on a timer. If the link dies for
// long enough (every alert un-ACKed past LINK_LOST_MS), the node simply
// re-registers — that IS the recovery. Radio layer (dual-path parsing,
// frame harvest) is shared with the mesh build.

#include <SPI.h>
#include <LoRa.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <vector>
#include "config.h"

// ── Wire format: <msg_id:16hex>|<src>|<dst>|<type:u8>|<payload> (k=v pairs)
#define NODE_ID_MAX_LEN  20
#define PACKET_MAX_LEN  200
#define DEDUP_SIZE       32

typedef enum : uint8_t {
    MSG_HB = 5, MSG_ALERT = 7, MSG_ANNOUNCE = 11,
    MSG_DISCOVER = 12, MSG_DISC_RESP = 13, MSG_REG_REQ = 14, MSG_REG_ACK = 15,
    MSG_ACK = 17
} msg_type_t;

typedef enum { NODE_DISCOVERING, NODE_REGISTERING, NODE_OPERATIONAL } node_state_t;

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
    char    id[NODE_ID_MAX_LEN];
    uint8_t depth;
    float   snr;
    int     rssi;
} disc_candidate_t;

// Reliable alert: retried until the master ACKs sent_msg_id. first_sent_ms
// drives link-loss re-registration; seq drives latest-wins freshness.
typedef struct {
    uint8_t  type;
    uint8_t  key;
    uint32_t seq;
    char     payload[PACKET_MAX_LEN];
    uint32_t first_sent_ms;    // 0 = never transmitted
    uint32_t last_sent_ms;
    uint64_t sent_msg_id;      // 0 = new content, nothing in flight
} pending_alert_t;

#define ALERT_KEY_FLOOD      1
#define ALERT_KEY_BATTERY    2
#define ALERT_KEY_GPS_MOVED  3
#define ALERT_KEY_GPS_SIGNAL 4
#define ALERT_KEY_CRASH      5
#define ALERT_KEY_ANNOUNCE   6

// ── Timing (demo)
#define HB_INTERVAL_MS        60000   // fire-and-forget heartbeat
#define ALERT_RETRY_MS         1000   // + jitter, until ACKed
#define ALERT_JITTER_MS         300
#define LINK_LOST_MS           30000   // all alerts un-ACKed this long → re-register
#define DISC_WINDOW_MS          2000   // starts when DISCOVER leaves the antenna
#define DISC_RETRY_MS           5000   // between discovery cycles
#define REG_TIMEOUT_MS          6000
#define POST_TX_LISTEN_MS        100
#define STATUS_INTERVAL_MS      10000
#define FLOAT_DEBOUNCE_MS         50
#define GPS_FIX_TIMEOUT_MS      45000
#define GPS_NMEA_TIMEOUT_MS     20000

#define LED_OK               0
#define LED_DISCOVERING      1
#define LED_REGISTERING      2
#define LED_NO_GPS           4
#define LED_LORA_FAIL        5
#define LED_GPS_NO_NMEA      6
#define LED_GPS_CALIBRATING  7

// ── Radio state (debug visibility)
#define RADIO_RX  0
#define RADIO_TX  1
volatile uint8_t  radio_state    = RADIO_RX;
volatile uint64_t last_tx_msg_id = 0;    // msg_id of the last transmitted frame

static void radio_set(uint8_t s) {
    if (radio_state != s) {
        static const char *nm[] = {"RX", "TX"};
        radio_state = s;
        Serial.printf("[RADIO] → %s\n", nm[s]);
    }
}

// ── Node state
volatile node_state_t node_state = NODE_DISCOVERING;
char active_parent_id[NODE_ID_MAX_LEN] = "";
uint8_t own_depth = 1;
uint32_t alert_seq = 1;                    // random per-boot base, set in setup()

// ── Hardware
TinyGPSPlus    gps;
HardwareSerial gps_serial(2);

// ── Sensors
volatile uint8_t float_bits     = 0;
volatile uint8_t old_float_bits = 0;
float    battery_voltage      = 0.0f;
bool     battery_low_sent     = false;
bool     gps_fix_valid        = false;
double   gps_lat              = 0.0, gps_lng = 0.0;
uint32_t last_nmea_ms         = 0, last_gps_fix_ms = 0;
bool     gps_calibrated       = false;
uint16_t gps_cal_count        = 0;
double   gps_cal_lat_sum      = 0.0, gps_cal_lng_sum = 0.0;
double   gps_home_lat         = 0.0, gps_home_lng = 0.0;
bool     gps_moved_sent       = false, gps_signal_lost_sent = false;
char     pending_crash_reason[16] = "";

// ── Discovery
disc_candidate_t  candidates[8];
uint8_t           candidate_count = 0;
SemaphoreHandle_t candidates_mutex;
SemaphoreHandle_t reg_ack_sem;

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
SemaphoreHandle_t         alert_mutex;
std::vector<pending_alert_t> alert_list;

// ── Other
SemaphoreHandle_t float_change_sem;
TaskHandle_t h_lora_rx = nullptr, h_lora_tx = nullptr, h_lora_proc = nullptr,
             h_disc = nullptr, h_gps = nullptr, h_float = nullptr, h_hb = nullptr,
             h_led = nullptr, h_status = nullptr;

void setup() {
    Serial.begin(115200);

    {
        esp_reset_reason_t rr = esp_reset_reason();
        if      (rr == ESP_RST_PANIC)                         strlcpy(pending_crash_reason, "panic",    sizeof(pending_crash_reason));
        else if (rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT) strlcpy(pending_crash_reason, "watchdog", sizeof(pending_crash_reason));
        else if (rr == ESP_RST_BROWNOUT)                      strlcpy(pending_crash_reason, "brownout", sizeof(pending_crash_reason));
        if (pending_crash_reason[0])
            Serial.printf("[CRASH] Prior crash detected: %s\n", pending_crash_reason);
    }

    Serial.printf("[BOOT] node_id=%s  village=%s  sf=%d\n", NODE_ID, VILLAGE, LORA_SF);
    alert_seq = esp_random() | 1;   // random base — a reboot never replays seqs
    setupVoltage();
    setupLoRa();
    setupGPS();
    setupFloatSensors();
    pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);

    lora_mutex       = xSemaphoreCreateMutex();
    lora_rx_mutex    = xSemaphoreCreateMutex();
    lora_proc_sem    = xSemaphoreCreateCounting(50, 0);
    lora_tx_mutex    = xSemaphoreCreateMutex();
    lora_tx_sem      = xSemaphoreCreateCounting(50, 0);
    alert_mutex      = xSemaphoreCreateMutex();
    candidates_mutex = xSemaphoreCreateMutex();
    reg_ack_sem      = xSemaphoreCreateBinary();
    float_change_sem = xSemaphoreCreateCounting(10, 0);

    xTaskCreatePinnedToCore(loraRxTask,   "lora_rx",   6144, nullptr, 5, &h_lora_rx,   1);
    xTaskCreatePinnedToCore(loraTxTask,   "lora_tx",   4096, nullptr, 5, &h_lora_tx,   1);
    xTaskCreatePinnedToCore(loraProcTask, "lora_proc", 6144, nullptr, 4, &h_lora_proc, 1);
    xTaskCreatePinnedToCore(discoveryTask,"disc",      4096, nullptr, 4, &h_disc,      0);
    xTaskCreatePinnedToCore(gpsTask,      "gps",       6144, nullptr, 3, &h_gps,       0);
    xTaskCreatePinnedToCore(floatTask,    "float",     3072, nullptr, 3, &h_float,     0);
    xTaskCreatePinnedToCore(heartbeatTask,"hb",        3072, nullptr, 3, &h_hb,        0);
    xTaskCreatePinnedToCore(ledTask,      "led",       1536, nullptr, 1, &h_led,       0);
    xTaskCreatePinnedToCore(statusTask,   "status",    4096, nullptr, 2, &h_status,    0);

    Serial.printf("[BOOT] River node ready — %s\n", NODE_ID);
    vTaskDelete(NULL);
}

void loop() {}
