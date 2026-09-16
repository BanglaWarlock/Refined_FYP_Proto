// NVS configuration — Provision_Master keys, plus new optional "deploy"
// (topic prefix slug) and "lora_sf" (radio profile).

void loadConfig() {
    Preferences prefs;
    prefs.begin("floodwatch", true);
    String nid  = prefs.getString("node_id",   "");
    String vil  = prefs.getString("village",   "");
    String ssid = prefs.getString("wifi_ssid", "");
    String pass = prefs.getString("wifi_pass", "");
    String host = prefs.getString("mqtt_host", "");
    String dep  = prefs.getString("deploy",    "");
    int    port = prefs.getInt(   "mqtt_port", 1883);
    int    ss   = prefs.getInt("pin_lora_ss",    -1);
    int    rst  = prefs.getInt("pin_lora_rst",   -1);
    int    dio0 = prefs.getInt("pin_lora_dio0",  -1);
    int    led  = prefs.getInt("pin_led_status", -1);
    int    mc   = prefs.getInt("max_children",    8);
    int    sf   = prefs.getInt("lora_sf",    LORA_SF);
    prefs.end();

    if (nid.isEmpty() || vil.isEmpty() || ssid.isEmpty() || host.isEmpty() || ss < 0) {
        Serial.println("[BOOT] NVS not provisioned — flash Provision_Master first!");
        for (uint8_t i = 5; i > 0; i--) {
            Serial.printf("[BOOT] Restarting in %u...\n", i);
            delay(1000);
        }
        esp_restart();
    }

    strlcpy(own_node_id, nid.c_str(),  sizeof(own_node_id));
    strlcpy(village,     vil.c_str(),  sizeof(village));
    strlcpy(wifi_ssid,   ssid.c_str(), sizeof(wifi_ssid));
    strlcpy(wifi_pass,   pass.c_str(), sizeof(wifi_pass));
    strlcpy(mqtt_broker, host.c_str(), sizeof(mqtt_broker));
    if (dep.length()) strlcpy(deploy, dep.c_str(), sizeof(deploy));
    mqtt_port = port;
    LORA_SS   = ss;
    LORA_RST  = rst;
    LORA_DIO0 = dio0;
    if (led >= 0) LED_PIN = led;
    if (mc  > 0) max_children = (uint8_t)mc;
    if (sf >= 7 && sf <= 12) lora_sf = (uint8_t)sf;

    Serial.printf("[BOOT] NVS: id=%s  village=%s  mqtt=%s:%d  lora=SS:%d RST:%d DIO0:%d  max_children=%u\n",
                  own_node_id, village, mqtt_broker, mqtt_port, LORA_SS, LORA_RST, LORA_DIO0, max_children);
}
