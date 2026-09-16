# FloodWatch (Refined)

Early flood monitoring IoT system: LoRa mesh river nodes deployed on rivers,
gateway master nodes per village, and a cloud layer (MQTT → parser → DB/Redis
→ API/SSE) behind it. This repo supersedes the original FYP folders — the
firmware protocol is v2 (see [docs/PROTOCOL.md](docs/PROTOCOL.md)).

## Layout

```
docs/            PROTOCOL.md (LoRa mesh spec), DEPLOYMENT.md (droplet + CI/CD)
hardware/
  river_node/    ESP32 + SX1278 + GPS + float sensors + solar/battery
  master_node/   ESP32 + SX1278 village gateway (WiFi → MQTT)   [in progress]
  provisioning/  one-shot NVS provisioning sketches             [in progress]
iot-service/     parser, API, deployment scripts, GH Actions workflows
website/         showcase dashboard
```

## Firmware (Arduino IDE / arduino-cli, ESP32 core)

1. Install libraries: `LoRa` (sandeepmistry, **GitHub master — needs
   onCadDone/CAD**), `TinyGPSPlus`.
2. Flash `hardware/provisioning` once per node (identity, pins, radio SF).
3. Flash the node sketch. NVS keys are stable — v1 provisioning still works.

## Protocol v2 in one paragraph

Half-duplex LoRa with CAD-before-transmit and post-TX listen guard; per-hop
ACK reliability with jittered retries; every alert carries a per-origin
monotonic `seq` so latest-wins at every hop (a level-2 alert always overtakes
an in-flight level-1); relays cancel their own `node_lost` reports when the
missing child is heard re-announcing through them, and the master suppresses
offline reports overtaken by newer knowledge. Timing constants target the
indoor demo profile (SF7); field nodes provision `lora_sf=10` without code
changes.
