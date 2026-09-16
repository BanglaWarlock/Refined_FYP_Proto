// provision_slave.ino
// Writes all permanent config for a river/slave node to NVS (Preferences).
// Flash ONCE on a fresh ESP32, then flash hardware/river_node.
// Keys are unchanged from v1 (existing nodes stay valid); new optional
// "lora_sf" selects the radio profile.

#include <Preferences.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>

// # edit before flashing ─────────────────────────────────────────────────────

const char* village_code = "SUTS";        // must match the master this node registers with

const char* node_id      = "SUTS-001";    // permanent ID, e.g. "SUTS-001"
const int   max_children    = 1;    // chain topology: each node accepts exactly one child
const int   gps_cal_samples = 100;  // GPS fixes to average for home position calibration
const int   gps_move_thr    = 10;   // movement alert threshold in metres
const int   lora_sf         = 7;    // demo 7; field deployments use 10

// pin assignments — only change if your wiring differs from default
const int pin_lora_ss    = 5;
const int pin_lora_rst   = 12;
const int pin_lora_dio0  = 13;
const int pin_float_1ft  = 25;      // 1ft float (active HIGH, GPIO25=DAC1 needs dacDisable)
const int pin_float_2ft  = 26;      // 2ft float (active HIGH, GPIO26=DAC2 needs dacDisable)
const int pin_float_3ft  = 27;      // 3ft float (active HIGH, no DAC)
const int pin_bat_adc    = 35;      // battery voltage divider → ADC1 ch7
const int pin_gps_rx     = 16;      // GPS TX → ESP RX
const int pin_gps_tx     = 17;      // GPS RX ← ESP TX
const int pin_led_status = 2;

// gps capture mode — true: wait for a fix at provisioning time and store it
// as the install position baseline; false: firmware calibrates on its own.
const bool capture_gps_on_provision = false;
const int  gps_capture_timeout_s    = 120;

// true wipes any existing GPS calibration — firmware recalibrates on boot.
const bool reset_gps_cal = true;

// # end of config ────────────────────────────────────────────────────────────

TinyGPSPlus    gps;
HardwareSerial gps_serial(2);

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[prov] slave provisioning start");

    double install_lat = 0.0, install_lng = 0.0;
    bool   gps_captured = false;

    if (capture_gps_on_provision) {
        Serial.printf("[prov] waiting for GPS fix (timeout %ds)...\n", gps_capture_timeout_s);
        gps_serial.setPins(pin_gps_rx, pin_gps_tx);
        gps_serial.begin(9600);
        unsigned long deadline = millis() + (gps_capture_timeout_s * 1000UL);
        while (millis() < deadline) {
            while (gps_serial.available()) gps.encode(gps_serial.read());
            if (gps.location.isValid() && gps.hdop.isValid() && gps.hdop.hdop() < 5.0) {
                install_lat  = gps.location.lat();
                install_lng  = gps.location.lng();
                gps_captured = true;
                Serial.printf("[prov] GPS fix: %.6f, %.6f  hdop=%.1f  sats=%u\n",
                    install_lat, install_lng,
                    gps.hdop.hdop(), gps.satellites.value());
                break;
            }
            delay(100);
        }
        if (!gps_captured) Serial.println("[prov] GPS timeout — install position not stored");
    }

    Preferences prefs;
    prefs.begin("floodwatch", false);   // read/write

    prefs.putString("village", village_code);
    if (strlen(node_id) == 0) {
        Serial.println("[prov] ERROR: node_id is blank — fill it in before flashing!");
    } else {
        prefs.putString("node_id", node_id);
        prefs.remove("depth");   // node discovers its parent on boot
        Serial.printf("[prov] identity: %s\n", node_id);
    }

    prefs.putInt("pin_lora_ss",    pin_lora_ss);
    prefs.putInt("pin_lora_rst",   pin_lora_rst);
    prefs.putInt("pin_lora_dio0",  pin_lora_dio0);
    prefs.putInt("pin_float_1ft",  pin_float_1ft);
    prefs.putInt("pin_float_2ft",  pin_float_2ft);
    prefs.putInt("pin_float_3ft",  pin_float_3ft);
    prefs.putInt("pin_bat_adc",    pin_bat_adc);
    prefs.putInt("pin_gps_rx",     pin_gps_rx);
    prefs.putInt("pin_gps_tx",     pin_gps_tx);
    prefs.putInt("pin_led_status", pin_led_status);
    prefs.putInt("max_children",    max_children);
    prefs.putInt("gps_cal_samples", gps_cal_samples);
    prefs.putInt("gps_move_thr",    gps_move_thr);
    prefs.putInt("lora_sf",         lora_sf);

    if (reset_gps_cal) {
        prefs.remove("install_gps_set");
        prefs.remove("install_lat");
        prefs.remove("install_lng");
        Serial.println("[prov] GPS calibration cleared — node will recalibrate on next boot");
    } else if (gps_captured) {
        prefs.putDouble("install_lat", install_lat);
        prefs.putDouble("install_lng", install_lng);
        prefs.putBool("install_gps_set", true);
    }

    prefs.end();

    prefs.begin("floodwatch", true);
    Serial.println("[prov] written and verified:");
    Serial.printf("  village      = %s\n",  prefs.getString("village",  "MISSING").c_str());
    Serial.printf("  node_id      = %s\n",  prefs.getString("node_id",  "MISSING — reflash!").c_str());
    Serial.printf("  lora         = SS:%d RST:%d DIO0:%d  sf=%d\n",
        prefs.getInt("pin_lora_ss",   -1),
        prefs.getInt("pin_lora_rst",  -1),
        prefs.getInt("pin_lora_dio0", -1),
        prefs.getInt("lora_sf", -1));
    Serial.printf("  floats       = 1ft:GPIO%d  2ft:GPIO%d  3ft:GPIO%d\n",
        prefs.getInt("pin_float_1ft", -1),
        prefs.getInt("pin_float_2ft", -1),
        prefs.getInt("pin_float_3ft", -1));
    Serial.printf("  bat_adc      = GPIO%d\n", prefs.getInt("pin_bat_adc", -1));
    Serial.printf("  gps          = RX:GPIO%d TX:GPIO%d\n",
        prefs.getInt("pin_gps_rx", -1),
        prefs.getInt("pin_gps_tx", -1));
    Serial.printf("  led          = status:GPIO%d\n", prefs.getInt("pin_led_status", -1));
    Serial.printf("  max_children    = %d\n",   prefs.getInt("max_children",    -1));
    Serial.printf("  gps_cal_samples = %d\n",   prefs.getInt("gps_cal_samples", -1));
    Serial.printf("  gps_move_thr    = %dm\n",  prefs.getInt("gps_move_thr",    -1));
    if (prefs.getBool("install_gps_set", false))
        Serial.printf("  install pos  = %.6f, %.6f\n",
            prefs.getDouble("install_lat", 0.0),
            prefs.getDouble("install_lng", 0.0));
    else
        Serial.println("  install pos  = (not set)");
    prefs.end();

    Serial.println("\n[prov] done. flash river_node firmware now.");
}

void loop() {
    static bool led_state = false;
    digitalWrite(2, led_state = !led_state);
    delay(300);
}
