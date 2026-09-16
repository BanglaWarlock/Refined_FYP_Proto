// ─────────────────────────────────────────────────────────────────────────────
//  FloodWatch DEMO river node — per-node configuration.
//  Edit this file, flash, done. No provisioning sketch, no NVS.
//  (The chaining/mesh research build lives in hardware/mesh-chaining/.)
// ─────────────────────────────────────────────────────────────────────────────

// Identity — must be unique per node, and VILLAGE must match its master
#define NODE_ID        "SUTS-001"
#define VILLAGE        "SUTS"

// Radio — 433 MHz SX1278. SF7 = short range / fast (indoor demo);
// use SF10 for outdoor range tests.
#define LORA_SF        7
#define LORA_TX_PWR    17

// Pin wiring (this node's physical build)
#define PIN_LORA_SS     5
#define PIN_LORA_RST   12
#define PIN_LORA_DIO0  13
#define PIN_FLOAT_1FT  25    // active HIGH, GPIO25=DAC1
#define PIN_FLOAT_2FT  26    // active HIGH, GPIO26=DAC2
#define PIN_FLOAT_3FT  27    // active HIGH
#define PIN_BAT_ADC    35
#define PIN_GPS_RX     16    // GPS TX → ESP RX
#define PIN_GPS_TX     17    // GPS RX ← ESP TX
#define PIN_LED         2

// Battery voltage divider: two cascading 1:5 dividers ≈ ×25, times cal factor
#define BAT_SCALE      25.0f
#define BAT_CAL        1.256f

// GPS
#define GPS_CAL_SAMPLES 100  // fixes averaged for the home position
#define GPS_MOVE_THR_M  10.0f // movement alert threshold (metres)
