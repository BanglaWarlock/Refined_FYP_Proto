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
    
# ── Float sensor sanity: wetness must be monotonic from bottom up ──────────
# bit0=1ft, bit1=2ft, bit2=3ft. A submerged high sensor implies the lower
# ones. Anything else = stuck/failing float → publish a float_anomaly event.

VALID_FLOAT_BITS = {0b000, 0b001, 0b011, 0b111}

def check_float_anomaly(node_id: str, village: str, float_bits):
    try:
        bits = int(float_bits) & 0x07
    except (TypeError, ValueError):
        return 0            # unreadable — don't crash the pipeline over it
    if bits in VALID_FLOAT_BITS:
        return bits

    wet     = [n for n in (1, 2, 3) if bits & (1 << (n - 1))]
    dry     = [n for n in (1, 2, 3) if not bits & (1 << (n - 1))]
    detail  = f"sensors {wet} wet but {dry} dry"
    log.warning("FLOAT ANOMALY node=%s bits=0b%03b — %s", node_id, bits, detail)

    data = {"node_id": node_id, "village": village,
            "float_bits": bits, "detail": detail}
    emit("float_anomaly", data)
    try:
        db.events.insert_one({"ts": now(), "type": "float_anomaly",
                              "node_id": node_id, "village": village,
                              "data": data})
    except pymongo.errors.PyMongoError as e:
        log.warning("float_anomaly DB write failed: %s", e)
    return bits


# ── Alert freshness: (node_id, type) -> last accepted seq (PROTOCOL §7) ────
last_alert_seq: dict[tuple[str, str], int] = {}
# seq==0 fallback cooldown per (node_id, type) — seconds
ALERT_COOLDOWN_S = 0
last_alert_ts: dict[tuple[str, str], float] = {}


def now() -> datetime:
    return datetime.now(timezone.utc)


def water_level_of(float_bits: int) -> int:
    if float_bits & 0x04: return 3
    if float_bits & 0x02: return 2
    if float_bits & 0x01: return 1
    return 0


KNOWN_KINDS = {"heartbeat", "alert", "announce", "topology", "nodes", "master"}

def parse_topic(topic: str):
    """floodwatch/[deploy/]village/rest... -> (deploy, village, [rest...]) or None."""
    parts = topic.split("/")
    if len(parts) < 3 or parts[0] != "floodwatch":
        return None
    if parts[1] in KNOWN_KINDS:              # floodwatch/village/type[/node]  (no deploy)
        return "", parts[1], parts[2:]
    if len(parts) == 3:                      # floodwatch/deploy/village — too short
        return None
    return parts[1], parts[2], parts[3:]     # floodwatch/deploy/village/rest...


def emit(event_type: str, data: dict):
    try:
        rds.publish("sse_events", json.dumps({"type": event_type, "data": data, "ts": now().isoformat()}))
    except redis.RedisError as e:
        log.warning("redis publish failed (%s): %s", event_type, e)


def store_failed(topic: str, payload: str, reason: str):
    db.failed_messages.insert_one({"ts": now(), "topic": topic, "payload": payload[:2000], "reason": reason})


# ── Handlers ────────────────────────────────────────────────────────────────

def handle_heartbeat(deploy, village, node_id, payload):
    # SSE event goes out BEFORE the DB writes — the stream must not wait on
    # Atlas round-trips (DB state catches up moments later)
    bits = check_float_anomaly(node_id, village, payload.get("float_bits", 0))
    emit("heartbeat", {"node_id": node_id, "village": village, "deploy": deploy,
                       "water_level": water_level_of(bits), "float_bits": bits,
                       "bat": payload.get("bat"), "gps_fix": payload.get("gps_fix"),
                       "lat": payload.get("lat"), "lng": payload.get("lng")})
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
    if atype == "flood":
        check_float_anomaly(node_id, village, payload.get("float_bits", 0))
        emit("flood_level", {k: doc.get(k) for k in
             ("node_id", "village", "level", "float_bits", "lat", "lng")})
    elif atype == "node_lost":
        emit("node_lost", {"node_id": payload.get("lost"), "reported_by": node_id,
                           "village": village})
    else:
        emit(atype, {k: doc.get(k) for k in
             ("node_id", "village", "bat", "level", "dist", "lat", "lng")})
    db.alerts.insert_one(doc)
    if atype == "flood":
        db.river_nodes.update_one({"node_id": node_id},
                                  {"$set": {"water_level": payload.get("level", 0),
                                            "float_bits": payload.get("float_bits", 0),
                                            "last_seen": now()}})
    elif atype == "node_lost":
        # master already applied the suppression rule; trust its decision
        db.events.insert_one({"ts": now(), "type": "node_lost",
                              "node_id": payload.get("lost"), "village": village,
                              "data": payload})
    else:
        db.events.insert_one({"ts": now(), "type": atype, "node_id": node_id,
                              "village": village, "data": payload})


def handle_announce(deploy, village, node_id, payload):
    emit("node_announce", {"node_id": node_id, "village": village, **payload})
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


def handle_node_status(deploy, village, target_id, payload):
    online = bool(payload.get("online", False))
    emit("node_online" if online else "node_offline",
         {"node_id": target_id, "village": village, "deploy": deploy})
    db.river_nodes.update_one({"node_id": target_id},
                              {"$set": {"village": village, "deploy": deploy,
                                        "online": online, "last_status_change": now()}},
                              upsert=True)
    db.events.insert_one({"ts": now(), "type": "node_online" if online else "node_offline",
                          "node_id": target_id, "village": village})


def handle_topology(deploy, village, node_id, payload):
    emit("topology", {"village": village, "topology": payload})
    db.villages.update_one(
        {"village": village},
        {"$set": {"village": village, "deploy": deploy,
                  "topology": payload, "topology_ts": now()}},
        upsert=True,
    )


# Villages whose master is currently offline. While a village is in this set,
# its river-node traffic is frozen: no online flips, no state updates.
offline_villages: set[str] = set()


def handle_master_status(deploy, village, payload):
    online = payload.get("status") == "online"
    emit("master_online" if online else "master_offline",
         {"village": village, "deploy": deploy})

    if not online:
        offline_villages.add(village)
        # cascade: every node in this village goes offline
        cursor = db.river_nodes.find({"village": village, "online": True},
                                     {"node_id": 1})
        affected = [doc["node_id"] for doc in cursor]
        if affected:
            db.river_nodes.update_many(
                {"village": village, "online": True},
                {"$set": {"online": False, "last_status_change": now()}})
            for nid in affected:
                emit("node_offline", {"node_id": nid, "village": village,
                                      "deploy": deploy, "reason": "master_offline"})
        log.warning("master %s OFFLINE — %d nodes marked offline", village, len(affected))
    else:
        offline_villages.discard(village)
        log.info("master %s ONLINE — nodes resume via their own traffic", village)
        # do NOT mass-flip nodes online here — let real heartbeats/announces do it,
        # so the dashboard only shows nodes that are genuinely back.

    db.master_nodes.update_one(
        {"village": village},
        {"$set": {"village": village, "deploy": deploy, "online": online,
                  "node_id": payload.get("node_id"), "last_seen": now()}},
        upsert=True)
    db.villages.update_one({"village": village},
                           {"$set": {"village": village, "deploy": deploy,
                                     "online": online, "last_seen": now()}},
                           upsert=True)
    db.events.insert_one({"ts": now(),
                          "type": "master_online" if online else "master_offline",
                          "village": village})


HANDLERS = {
    "heartbeat": lambda d, v, n, p: handle_heartbeat(d, v, n, p),
    "alert":     lambda d, v, n, p: handle_alert(d, v, n, p),
    "announce":  lambda d, v, n, p: handle_announce(d, v, n, p),
    "topology":  lambda d, v, n, p: handle_topology(d, v, n, p),
}

def on_message(client, userdata, msg):
    topic, raw = msg.topic, msg.payload.decode("utf-8", errors="replace")
    try:
        return _dispatch(client, topic, raw)
    except Exception as e:
        log.exception("handler crash on %s", topic)
        try:
            store_failed(topic, raw, f"handler crash: {e}")
        except Exception:
            log.exception("failed_messages write also failed")

def _dispatch(client, userdata, msg):
    parsed = parse_topic(topic)
    if not parsed:
        return store_failed(topic, raw, "unrecognized topic shape")
    deploy, village, rest = parsed
    if MQTT_DEPLOY and deploy and deploy != MQTT_DEPLOY:
        return
    kind = rest[0]
    
     # Frozen village: master is down — only master status / topology may update state
    if village in offline_villages and kind not in ("master", "nodes", "topology"):
        log.info("dropping %s from offline village %s", topic, village)
        return store_failed(topic, raw, "village frozen (master offline)")


    try:
        if kind == "master" and len(rest) >= 2 and rest[1] == "status":
            return handle_master_status(deploy, village, json.loads(raw))
        if kind == "nodes" and len(rest) >= 3 and rest[-1] == "status":
            return handle_node_status(deploy, village, rest[1], json.loads(raw))

        node_id = rest[-1]
        payload = json.loads(raw)
        if kind != "topology" and node_id != payload.get("node_id"):
            payload["node_id"] = node_id
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
def seed_offline_villages():
    for doc in db.master_nodes.find({"online": False}, {"village": 1}):
        offline_villages.add(doc["village"])
    if offline_villages:
        log.info("seeded offline villages from DB: %s", offline_villages)

def main():
    ensure_indexes()
    seed_offline_villages() 
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="floodwatch-parser")
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    log.info("parser running")
    client.loop_forever()


if __name__ == "__main__":
    main()
