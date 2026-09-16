// NVS configuration — keys unchanged from v1 provisioning sketch, so
// Provision_Slave output stays valid. Optional new key: lora_sf.

void loadConfig() {
    Preferences prefs;
    prefs.begin("floodwatch", true);
    String nid  = prefs.getString("node_id",      "");
    String vil  = prefs.getString("village",      "");
    int    ss   = prefs.getInt("pin_lora_ss",    -1);
    int    rst  = prefs.getInt("pin_lora_rst",   -1);
    int    dio0 = prefs.getInt("pin_lora_dio0",  -1);
    int    f1   = prefs.getInt("pin_float_1ft",  -1);
    int    f2   = prefs.getInt("pin_float_2ft",  -1);
    int    f3   = prefs.getInt("pin_float_3ft",  -1);
    int    bat  = prefs.getInt("pin_bat_adc",    -1);
    int    grx  = prefs.getInt("pin_gps_rx",     -1);
    int    gtx  = prefs.getInt("pin_gps_tx",     -1);
    int    led  = prefs.getInt("pin_led_status",  -1);
    int    mc   = prefs.getInt(   "max_children",    1);
    int    cal  = prefs.getInt(   "gps_cal_samples", 200);
    int    thr  = prefs.getInt(   "gps_move_thr",    10);
    int    sf   = prefs.getInt(   "lora_sf",    LORA_SF);
    double hlat = prefs.getDouble("install_lat",     0.0);
    double hlng = prefs.getDouble("install_lng",     0.0);
    bool   hset = prefs.getBool(  "install_gps_set", false);
    prefs.end();

    if (nid.isEmpty() || vil.isEmpty() || ss < 0) {
        Serial.println("[BOOT] NVS not provisioned — flash Provision_Slave first!");
        for (uint8_t i = 5; i > 0; i--) {
            Serial.printf("[BOOT] Restarting in %u...\n", i);
            delay(1000);
        }
        esp_restart();
    }

    strlcpy(own_node_id, nid.c_str(), sizeof(own_node_id));
    strlcpy(village,     vil.c_str(), sizeof(village));
    LORA_SS   = ss;
    LORA_RST  = rst;
    LORA_DIO0 = dio0;
    if (f1  >= 0) FLOAT1_PIN = f1;
    if (f2  >= 0) FLOAT2_PIN = f2;
    if (f3  >= 0) FLOAT3_PIN = f3;
    if (bat >= 0) BAT_PIN    = bat;
    if (grx >= 0) GPS_RX_PIN = grx;
    if (gtx >= 0) GPS_TX_PIN = gtx;
    if (led >= 0) LED_PIN    = led;
    if (mc  > 0) max_children    = (uint8_t)mc;
    if (cal > 0) gps_cal_samples = (uint16_t)cal;
    if (thr > 0) gps_move_thr_m  = (float)thr;
    if (sf  >= 7 && sf <= 12) lora_sf = (uint8_t)sf;
    if (hset && (hlat != 0.0 || hlng != 0.0)) {
        gps_home_lat   = hlat;
        gps_home_lng   = hlng;
        gps_calibrated = true;
    }

    Serial.printf("[BOOT] NVS: id=%s  village=%s  lora=SS:%d RST:%d DIO0:%d  max_children=%u\n",
                  own_node_id, village, LORA_SS, LORA_RST, LORA_DIO0, max_children);
    Serial.printf("[BOOT] Pins: f1=%d f2=%d f3=%d bat=%d gps=%d/%d led=%d\n",
                  FLOAT1_PIN, FLOAT2_PIN, FLOAT3_PIN, BAT_PIN, GPS_RX_PIN, GPS_TX_PIN, LED_PIN);
    Serial.printf("[BOOT] GPS: cal_samples=%u  move_thr=%.0fm  home=%s(%.6f,%.6f)\n",
                  gps_cal_samples, gps_move_thr_m,
                  gps_calibrated ? "" : "NOT SET ", gps_home_lat, gps_home_lng);
}
