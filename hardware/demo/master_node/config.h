// ─────────────────────────────────────────────────────────────────────────────
//  FloodWatch DEMO master node — per-node configuration.
//  Edit this file, flash, done. No provisioning sketch, no NVS.
// ─────────────────────────────────────────────────────────────────────────────

// Identity
#define NODE_ID        "M-SUTS"
#define VILLAGE        "SUTS"
#define DEPLOY         "suts-demo"    // MQTT topic prefix: floodwatch/<deploy>/<village>/...
                                      // keep unique on public brokers; "" = no prefix level

// WiFi backhaul
#define WIFI_SSID      "Lab@IOT"
#define WIFI_PASS      "P@ss1234"

// MQTT broker (public EMQX for the demo; your droplet Mosquitto later)
#define MQTT_HOST      "broker.emqx.io"
#define MQTT_PORT      1883

// Radio — must match the river nodes
#define LORA_SF        7
#define LORA_TX_PWR    20

// Pin wiring
#define PIN_LORA_SS     5
#define PIN_LORA_RST   12
#define PIN_LORA_DIO0  13
#define PIN_LED         2

// Liveness: a child silent this long is published offline; its next
// registration/announce/heartbeat flips it straight back online.
#define NODE_OFFLINE_TIMEOUT_MS  185000
