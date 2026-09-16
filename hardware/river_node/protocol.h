#pragma once
#include <stdint.h>

// ── Wire format ────────────────────────────────────────────────────────────
//   <msg_id:16hex>|<src>|<dst>|<type:u8>|<payload>      (payload = k=v pairs)
// src is the original sender — relays forward it untouched. Delivery is
// guaranteed by per-hop ACK + payload seq (latest-wins), not by msg_id dedup.
// See docs/PROTOCOL.md.

#define NODE_ID_MAX_LEN  20
#define VILLAGE_MAX_LEN  16
#define PACKET_MAX_LEN  200
#define DEDUP_SIZE       32

typedef enum : uint8_t {
    MSG_HB        = 5,  MSG_SENSOR    = 6,  MSG_ALERT     = 7,
    MSG_CMD       = 8,  MSG_CMD_ACK   = 9,  MSG_BEACON    = 10,
    MSG_ANNOUNCE  = 11, MSG_DISCOVER  = 12, MSG_DISC_RESP = 13,
    MSG_REG_REQ   = 14, MSG_REG_ACK   = 15, MSG_TOPO_REQ  = 16,
    MSG_ACK       = 17, MSG_RELAY_REQ = 18
} msg_type_t;

typedef enum { NODE_DISCOVERING, NODE_REGISTERING, NODE_OPERATIONAL, NODE_LOST_PARENT } node_state_t;

typedef struct {
    uint64_t msg_id;
    char     src_id[NODE_ID_MAX_LEN];
    char     dst_id[NODE_ID_MAX_LEN];
    uint8_t  type;
    char     payload[PACKET_MAX_LEN];
    int      rssi;                      // link-layer quality of the last hop
    float    snr;
} lora_packet;

typedef struct {
    char    id[NODE_ID_MAX_LEN];
    uint8_t depth;
    uint8_t cur_children;
    uint8_t max_children;
    float   snr;
    int     rssi;
} disc_candidate_t;

// ACK-tracked heartbeat: HB_MAX_RETRIES misses ⇒ parent lost
typedef struct {
    uint64_t sent_msg_id;               // 0 = none in flight
    uint32_t last_sent_ms;
    uint8_t  retries;
} pending_hb_t;

typedef struct {
    char     id[NODE_ID_MAX_LEN];
    uint32_t last_seen_ms;
    uint32_t probe_sent_ms;       // non-zero = probing before node_lost (§5)
    uint8_t  probe_tries;
} child_reg_t;

// Reliable alert entry. Retried every ALERT_RETRY_MS(+jitter) until an
// upstream ACK matches sent_msg_id. (key, tag, relay_src) identifies the
// entry for freshness replacement; tag disambiguates node_lost per child.
// seq: per-origin monotonic counter (random boot base) — latest-wins.
typedef struct {
    uint8_t  type;
    uint8_t  key;
    char     tag[NODE_ID_MAX_LEN];
    char     relay_src[NODE_ID_MAX_LEN]; // "" = own alert, else relayed
    uint32_t seq;
    char     payload[PACKET_MAX_LEN];
    uint32_t last_sent_ms;
    uint64_t sent_msg_id;                // 0 = new content, nothing in flight
} pending_alert_t;

#define ALERT_KEY_FLOOD      1
#define ALERT_KEY_BATTERY    2
#define ALERT_KEY_GPS_MOVED  3
#define ALERT_KEY_GPS_SIGNAL 4
#define ALERT_KEY_CRASH      5
#define ALERT_KEY_ANNOUNCE   6
#define ALERT_KEY_NODE_LOST  7

// ── Radio — demo profile. Field nodes: provision lora_sf=10 in NVS. ────────
#define LORA_FREQ_HZ  433E6
#define LORA_SF       7
#define LORA_BW       125E3
#define LORA_CR       5
#define LORA_TX_PWR   17

// ── Timing (demo profile; field values in docs/PROTOCOL.md §9) ─────────────
#define HEARTBEAT_INTERVAL_MS     10000
#define HB_RETRY_MS                2000
#define HB_MAX_RETRIES                5
#define ALERT_RETRY_MS             1000
#define ALERT_JITTER_MS             300
#define CAD_TIMEOUT_MS              100
#define CAD_BACKOFF_MS               40
#define CAD_MAX_TRIES                 5
#define POST_TX_LISTEN_MS           100
#define BEACON_INTERVAL_MS        10000
#define DISC_WINDOW_MS              800
#define DISC_INTERVAL_MS          10000
#define REG_TIMEOUT_MS             6000
#define CHILD_OFFLINE_TIMEOUT_MS  45000  // passive silence → start probing
#define PROBE_INTERVAL_MS          3000  // between liveness probes
#define PROBE_MAX_TRIES               3  // silent through all → node_lost
#define STATUS_INTERVAL_MS        10000
#define FLOAT_DEBOUNCE_MS            50
#define GPS_FIX_TIMEOUT_MS        45000
#define GPS_NMEA_TIMEOUT_MS       20000
