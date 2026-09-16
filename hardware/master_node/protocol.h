#pragma once
#include <stdint.h>

// ── Wire format — MUST match hardware/river_node/protocol.h ───────────────
//   <msg_id:16hex>|<src>|<dst>|<type:u8>|<payload>      (payload = k=v pairs)
// Reliability = per-hop ACK + payload seq (latest-wins). See docs/PROTOCOL.md.

#define NODE_ID_MAX_LEN       20
#define VILLAGE_MAX_LEN       16
#define PACKET_MAX_LEN       200
#define MAX_REGISTERED_NODES  100
#define DEDUP_SIZE            32

typedef enum : uint8_t {
    MSG_HB        = 5,  MSG_SENSOR    = 6,  MSG_ALERT     = 7,
    MSG_CMD       = 8,  MSG_CMD_ACK   = 9,  MSG_BEACON    = 10,
    MSG_ANNOUNCE  = 11, MSG_DISCOVER  = 12, MSG_DISC_RESP = 13,
    MSG_REG_REQ   = 14, MSG_REG_ACK   = 15, MSG_TOPO_REQ  = 16,
    MSG_ACK       = 17, MSG_RELAY_REQ = 18
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

// ── Node registry entry
typedef struct {
    char     node_id[NODE_ID_MAX_LEN];
    char     parent_id[NODE_ID_MAX_LEN];
    uint8_t  depth;
    bool     is_online;
    uint32_t last_seen_ms;
    uint32_t last_announce_ms;    // millis() of last ANNOUNCE; 0 = never
    uint32_t last_alert_seq;      // newest alert seq accepted — older = stale
    float    battery_voltage;
    uint8_t  float_bits;
    float    snr;                 // leaf→relay (injected) when available
    int      rssi;
    uint32_t last_seq;            // HB seq — packet-loss accounting
    uint32_t pkt_rx;
    uint32_t pkt_lost;
    double   lat;
    double   lng;
    bool     gps_fix;
} registered_node;

// ── Downstream command retry
typedef struct {
    char     target[NODE_ID_MAX_LEN];
    char     next_hop[NODE_ID_MAX_LEN];
    char     pload[PACKET_MAX_LEN];
    uint32_t last_sent_ms;
    uint8_t  attempts;
} pending_cmd_t;

// ── Radio — must match the river nodes' profile (demo SF7; field nodes
//    provision lora_sf=10 — masters follow via their own NVS key)
#define LORA_FREQ_HZ  433E6
#define LORA_SF       7
#define LORA_BW       125E3
#define LORA_CR       5
#define LORA_TX_PWR   17

// ── Timing (demo profile — docs/PROTOCOL.md §9)
#define BEACON_INTERVAL_MS        10000
#define NODE_OFFLINE_TIMEOUT_MS   45000  // matches river CHILD_OFFLINE_TIMEOUT
#define STATUS_PRINT_INTERVAL_MS  10000
#define CMD_RETRY_INTERVAL_MS      3000
#define POST_TX_LISTEN_MS           100
#define CAD_TIMEOUT_MS              100
#define CAD_BACKOFF_MS               40
#define CAD_MAX_TRIES                 5
