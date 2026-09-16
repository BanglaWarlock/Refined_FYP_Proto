#!/usr/bin/env python3
"""
FloodWatch Parser v3 — MQTT -> MongoDB + Redis event bus.

Topic contract (matches hardware firmware v2, docs/PROTOCOL.md):
  floodwatch/[deploy/]village/heartbeat/<node>   sensor readings
  floodwatch/[deploy/]village/alert/<node>       reliable alerts (seq-deduped)
  floodwatch/[deploy/]village/announce/<node>    topology + install position
  floodwatch/[deploy/]village/nodes/<id>/status  online/offline transitions
  floodwatch/[deploy/]village/topology           full mesh tree
  floodwatch/[deploy/]village/master/status      master LWT / online

Collections:
  river_nodes    live state, one doc per node (upsert)
  heartbeats     time-series readings, TTL 30 days
  alerts         alert events, seq-deduped, TTL 90 days
  events         online/offline/announce log, TTL 30 days
  villages       per-village master state + topology
  master_nodes   master gateway live state
  failed_messages malformed/unroutable MQTT messages (debug)

Redis: every parsed event is published on channel "sse_events" as
  {"type": "...", "data": {...}}  — the API fans this out over SSE.
"""

import json
import logging
import os
import time
from datetime import datetime, timezone, timedelta

import paho.mqtt.client as mqtt
import pymongo
import redis
from dotenv import load_dotenv

load_dotenv()

MONGO_URI   = os.getenv("MONGO_URI", "mongodb://localhost:27017")
MQTT_BROKER = os.getenv("MQTT_BROKER", "broker.emqx.io")
MQTT_PORT   = int(os.getenv("MQTT_PORT", "1883"))
MQTT_DEPLOY = os.getenv("MQTT_DEPLOY", "")   # only process this deploy slug; empty = all
REDIS_URL   = os.getenv("REDIS_URL", "redis://localhost:6379")

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("parser")

mongo = pymongo.MongoClient(MONGO_URI)
db    = mongo.get_default_database()

rds = redis.Redis.from_url(REDIS_URL, decode_responses=True)

TTL_30D = timedelta(days=30)
TTL_90D = timedelta(days=90)


def ensure_indexes():
    db.heartbeats.create_index([("ts", pymongo.ASCENDING)], expireAfterSeconds=int(TTL_30D.total_seconds()))
    db.alerts.create_index([("ts", pymongo.ASCENDING)], expireAfterSeconds=int(TTL_90D.total_seconds()))
    db.alerts.create_index([("node_id", pymongo.ASCENDING), ("type", pymongo.ASCENDING), ("seq", pymongo.ASCENDING)])
    db.events.create_index([("ts", pymongo.ASCENDING)], expireAfterSeconds=int(TTL_30D.total_seconds()))
    db.river_nodes.create_index([("node_id", pymongo.ASCENDING)], unique=True)
    db.master_nodes.create_index([("village", pymongo.ASCENDING)], unique=True)
    db.villages.create_index([("village", pymongo.ASCENDING)], unique=True)
    log.info("indexes ensured")


# ── Alert freshness: (node_id, type) -> last accepted seq (PROTOCOL §7) ────
last_alert_seq: dict[tuple[str, str], int] = {}
# seq==0 fallback cooldown per (node_id, type) — seconds
ALERT_COOLDOWN_S = 30
last_alert_ts: dict[tuple[str, str], float] = {}


def now() -> datetime:
    return datetime.now(timezone.utc)


def water_level_of(float_bits: int) -> int:
    if float_bits & 0x04: return 3
    if float_bits & 0x02: return 2
    if float_bits & 0x01: return 1
    return 0


def parse_topic(topic: str):
    """floodwatch/[deploy/]village/rest... -> (deploy, village, [rest...]) or None."""
    parts = topic.split("/")
    if len(parts) < 3 or parts[0] != "floodwatch":
        return None
    if len(parts) == 3:                      # floodwatch/village (bad)
        return None
    if len(parts) == 4:                      # floodwatch/village/type/node
        return "", parts[1], parts[2:]
    return parts[1], parts[2], parts[3:]     # floodwatch/deploy/village/...


def emit(event_type: str, data: dict):
    rds.publish("sse_events", json.dumps({"type": event_type, "data": data, "ts": now().isoformat()}))


def store_failed(topic: str, payload: str, reason: str):
    db.failed_messages.insert_one({"ts": now(), "topic": topic, "payload": payload[:2000], "reason": reason})


# ── Handlers ────────────────────────────────────────────────────────────────

def handle_heartbeat(deploy, village, node_id, payload):
    db.river_nodes.update_one(
        {"node_id": node_id},
        {"$set": {
            "village": village, "deploy": deploy, "online": True,
            "bat": payload.get("bat", 0.0),
            "float_bits": payload.get("float_bits", 0),
            "water_level": water_level_of(payload.get("float_bits", 0)),
            "depth": payload.get("depth"), "parent": payload.get("parent"),
            "lat": payload.get("lat"), "lng": payload.get("lng"),
            "gps_fix": payload.get("gps_fix", False),
            "snr": payload.get("snr"), "rssi": payload.get("rssi"),
            "last_seen": now(),
        }, "$setOnInsert": {"first_seen": now()}},
        upsert=True,
    )
    db.heartbeats.insert_one({"ts": now(), "node_id": node_id, "village": village, **payload})
    emit("heartbeat", {"node_id": node_id, "village": village, "deploy": deploy,
                       "water_level": water_level_of(payload.get("float_bits", 0)),
                       "bat": payload.get("bat"), "gps_fix": payload.get("gps_fix"),
                       "lat": payload.get("lat"), "lng": payload.get("lng")})


def handle_alert(deploy, village, node_id, payload):
    atype = payload.get("type", "unknown")
    seq   = int(payload.get("seq", 0) or 0)
    key   = (node_id, atype)

    if seq:
        prev = last_alert_seq.get(key)
        if prev is not None and seq <= prev:
            log.info("stale alert %s/%s seq=%s (last=%s) — absorbed", node_id, atype, seq, prev)
            return
        last_alert_seq[key] = seq
    else:
        prev_ts = last_alert_ts.get(key, 0.0)
        if time.time() - prev_ts < ALERT_COOLDOWN_S:
            log.info("alert %s/%s within cooldown — absorbed", node_id, atype)
            return
    last_alert_ts[key] = time.time()

    doc = {"ts": now(), "node_id": node_id, "village": village, "deploy": deploy, **payload}
    db.alerts.insert_one(doc)
    if atype == "flood":
        db.river_nodes.update_one({"node_id": node_id},
                                  {"$set": {"water_level": payload.get("level", 0),
                                            "float_bits": payload.get("float_bits", 0),
                                            "last_seen": now()}})
        emit("flood_level", {k: doc.get(k) for k in
             ("node_id", "village", "level", "float_bits", "lat", "lng")})
    elif atype == "node_lost":
        # master already applied the suppression rule; trust its decision
        db.events.insert_one({"ts": now(), "type": "node_lost",
                              "node_id": payload.get("lost"), "village": village,
                              "data": payload})
        emit("node_lost", {"node_id": payload.get("lost"), "reported_by": node_id,
                           "village": village})
    else:
        db.events.insert_one({"ts": now(), "type": atype, "node_id": node_id,
                              "village": village, "data": payload})
        emit(atype, {k: doc.get(k) for k in
             ("node_id", "village", "bat", "level", "dist", "lat", "lng")})


def handle_announce(deploy, village, node_id, payload):
    db.river_nodes.update_one(
        {"node_id": node_id},
        {"$set": {
            "village": village, "deploy": deploy, "online": True,
            "depth": payload.get("depth"), "parent": payload.get("parent"),
            "lat": payload.get("lat"), "lng": payload.get("lng"),
            "snr": payload.get("snr"), "rssi": payload.get("rssi"),
            "last_seen": now(),
        }, "$setOnInsert": {"first_seen": now()}},
        upsert=True,
    )
    db.events.insert_one({"ts": now(), "type": "announce", "node_id": node_id,
                          "village": village, "data": payload})
    emit("node_announce", {"node_id": node_id, "village": village, **payload})


def handle_node_status(deploy, village, target_id, payload):
    online = bool(payload.get("online", False))
    db.river_nodes.update_one({"node_id": target_id},
                              {"$set": {"village": village, "deploy": deploy,
                                        "online": online, "last_status_change": now()}},
                              upsert=True)
    db.events.insert_one({"ts": now(), "type": "node_online" if online else "node_offline",
                          "node_id": target_id, "village": village})
    emit("node_online" if online else "node_offline",
         {"node_id": target_id, "village": village, "deploy": deploy})


def handle_topology(deploy, village, payload):
    db.villages.update_one(
        {"village": village},
        {"$set": {"village": village, "deploy": deploy,
                  "topology": payload, "topology_ts": now()}},
        upsert=True,
    )
    emit("topology", {"village": village, "topology": payload})


def handle_master_status(deploy, village, payload):
    online = payload.get("status") == "online"
    db.master_nodes.update_one(
        {"village": village},
        {"$set": {"village": village, "deploy": deploy, "online": online,
                  "node_id": payload.get("node_id"), "last_seen": now()}},
        upsert=True,
    )
    db.villages.update_one({"village": village},
                           {"$set": {"village": village, "deploy": deploy,
                                     "online": online, "last_seen": now()}},
                           upsert=True)
    db.events.insert_one({"ts": now(), "type": "master_online" if online else "master_offline",
                          "village": village})
    emit("master_online" if online else "master_offline", {"village": village, "deploy": deploy})


HANDLERS = {
    "heartbeat": lambda d, v, n, p: handle_heartbeat(d, v, n, p),
    "alert":     lambda d, v, n, p: handle_alert(d, v, n, p),
    "announce":  lambda d, v, n, p: handle_announce(d, v, n, p),
    "topology":  lambda d, v, n, p: handle_topology(d, v, n, p),
}


def on_message(client, userdata, msg):
    topic, raw = msg.topic, msg.payload.decode("utf-8", errors="replace")
    parsed = parse_topic(topic)
    if not parsed:
        return store_failed(topic, raw, "unrecognized topic shape")
    deploy, village, rest = parsed
    if MQTT_DEPLOY and deploy and deploy != MQTT_DEPLOY:
        return
    kind = rest[0]

    try:
        if kind == "master" and len(rest) >= 2 and rest[1] == "status":
            return handle_master_status(deploy, village, json.loads(raw))
        if kind == "nodes" and len(rest) >= 2 and rest[1] == "status":
            return handle_node_status(deploy, village, rest[-1], json.loads(raw))
        node_id = rest[-1]
        payload = json.loads(raw)
        if node_id != payload.get("node_id"):
            payload["node_id"] = node_id   # topic is authoritative
        fn = HANDLERS.get(kind)
        if fn:
            return fn(deploy, village, node_id, payload)
        store_failed(topic, raw, f"no handler for '{kind}'")
    except (json.JSONDecodeError, KeyError, ValueError) as e:
        log.warning("failed to handle %s: %s", topic, e)
        store_failed(topic, raw, str(e))


def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        log.info("connected to %s:%s (deploy filter: %s)", MQTT_BROKER, MQTT_PORT, MQTT_DEPLOY or "all")
        client.subscribe("floodwatch/#", qos=1)
    else:
        log.error("connect failed rc=%s", rc)


def main():
    ensure_indexes()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="floodwatch-parser")
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    log.info("parser running")
    client.loop_forever()


if __name__ == "__main__":
    main()
