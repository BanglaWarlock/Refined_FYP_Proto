// provision_master.ino
// Writes all permanent config for a master node to NVS (Preferences).
// Flash ONCE on a fresh ESP32, then flash hardware/master_node.
// New in v2: "deploy" (topic prefix slug for public brokers) and "lora_sf".

#include <Preferences.h>
#include "credentials.h"  // copy credentials.h.example → credentials.h and fill in

// # edit before flashing ─────────────────────────────────────────────────────

const char* village_code = "SUTS";       // 2-6 chars, uppercase — node_id becomes "M-SUTS"
const char* deploy_slug  = "suts-demo";  // topic prefix: floodwatch/<deploy>/<village>/...
                                         // keep unique on public brokers; "" = no prefix level
const int   max_children = 8;            // how many depth-1 nodes this master may accept
const int   lora_sf      = 7;            // demo 7; field deployments use 10

const int pin_lora_ss    = 5;
const int pin_lora_rst   = 12;
const int pin_lora_dio0  = 13;
const int pin_led_status = 2;

// # end of config ────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("\n[prov] master provisioning start");

    char node_id[16];
    snprintf(node_id, sizeof(node_id), "M-%s", village_code);   // e.g. M-SUTS

    Preferences prefs;
    prefs.begin("floodwatch", false);

    prefs.putString("node_id",     node_id);
    prefs.putString("village",     village_code);

    prefs.putString("wifi_ssid",   wifi_ssid);
    prefs.putString("wifi_pass",   wifi_pass);
    prefs.putString("mqtt_host",   mqtt_host);
    prefs.putInt(   "mqtt_port",   mqtt_port);
    prefs.putString("deploy",      deploy_slug);
    prefs.putInt(   "lora_sf",     lora_sf);

    prefs.putInt("pin_lora_ss",    pin_lora_ss);
    prefs.putInt("pin_lora_rst",   pin_lora_rst);
    prefs.putInt("pin_lora_dio0",  pin_lora_dio0);
    prefs.putInt("pin_led_status", pin_led_status);
    prefs.putInt("max_children",   max_children);

    prefs.end();

    prefs.begin("floodwatch", true);
    Serial.println("[prov] written and verified:");
    Serial.printf("  node_id    = %s\n", prefs.getString("node_id",   "MISSING").c_str());
    Serial.printf("  village    = %s\n", prefs.getString("village",   "MISSING").c_str());
    Serial.printf("  deploy     = %s\n", prefs.getString("deploy",     "(none)").c_str());
    Serial.printf("  wifi_ssid  = %s\n", prefs.getString("wifi_ssid", "MISSING").c_str());
    Serial.printf("  mqtt_host  = %s:%d\n", prefs.getString("mqtt_host", "MISSING").c_str(), prefs.getInt("mqtt_port", -1));
    Serial.printf("  lora       = SS:%d RST:%d DIO0:%d  sf=%d\n",
        prefs.getInt("pin_lora_ss",   -1),
        prefs.getInt("pin_lora_rst",  -1),
        prefs.getInt("pin_lora_dio0", -1),
        prefs.getInt("lora_sf", -1));
    Serial.printf("  led        = status:GPIO%d\n", prefs.getInt("pin_led_status", -1));
    Serial.printf("  max_children = %d\n", prefs.getInt("max_children", -1));
    prefs.end();

    Serial.println("\n[prov] done. flash master_node firmware now.");
}

void loop() {
    static bool led_state = false;
    digitalWrite(2, led_state = !led_state);
    delay(300);
}
