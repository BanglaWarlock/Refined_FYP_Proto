#!/usr/bin/env python3
"""
FloodWatch API v3 — FastAPI REST + SSE over MongoDB/Redis.

Reads what parser v3 writes (river_nodes, heartbeats, alerts, events,
villages, master_nodes). No direct DB writes — the parser is the only
writer, end users only read through here (docs/PROTOCOL.md architecture).

Run: uvicorn main:app --host 0.0.0.0 --port 8000
"""

import asyncio
import json
import os
from datetime import datetime, timezone

import pymongo
import redis.asyncio as aioredis
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse

load_dotenv()

MONGO_URI = os.getenv("MONGO_URI", "mongodb://localhost:27017")
REDIS_URL = os.getenv("REDIS_URL", "redis://localhost:6379")

mongo = pymongo.MongoClient(MONGO_URI)
db    = mongo.get_default_database()
rds   = aioredis.from_url(REDIS_URL, decode_responses=True)

app = FastAPI(title="FloodWatch API", version="3.0")
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])

SSE_CHANNEL = "sse_events"


def now():
    return datetime.now(timezone.utc)


def since_from(from_iso: str | None, default_h: int) -> dict:
    if from_iso:
        try:
            return {"ts": {"$gte": datetime.fromisoformat(from_iso.replace("Z", "+00:00"))}}
        except ValueError:
            raise HTTPException(400, "invalid 'from' — use ISO 8601")
    return {"ts": {"$gte": datetime.fromtimestamp(
        now().timestamp() - default_h * 3600, tz=timezone.utc)}}


@app.get("/")
def root():
    return {"service": "floodwatch-api", "status": "ok", "time": now().isoformat()}


@app.get("/api/v1/nodes")
def list_nodes(village: str | None = None, status: str | None = None):
    q = {}
    if village: q["village"] = village
    if status == "online":  q["online"] = True
    if status == "offline": q["online"] = False
    return list(db.river_nodes.find(q, {"_id": 0}).sort("node_id", 1))


@app.get("/api/v1/nodes/{node_id}")
def get_node(node_id: str):
    n = db.river_nodes.find_one({"node_id": node_id}, {"_id": 0})
    if not n:
        raise HTTPException(404, "node not found")
    return n


@app.get("/api/v1/nodes/{node_id}/readings")
def node_readings(node_id: str, from_iso: str | None = Query(None, alias="from"), limit: int = 500):
    limit = max(1, min(limit, 2000))
    q = {"node_id": node_id, **since_from(from_iso, 24)}
    cur = db.heartbeats.find(q, {"_id": 0}).sort("ts", -1).limit(min(limit, 2000))
    return list(cur)[::-1]   # oldest -> newest for charting


@app.get("/api/v1/alerts")
def list_alerts(village: str | None = None, node_id: str | None = None,
                alert_type: str | None = None, from_iso: str | None = Query(None, alias="from"),
                limit: int = 200):
    limit = max(1, min(limit, 1000))
    q = since_from(from_iso, 72)
    if village:     q["village"] = village
    if node_id:     q["node_id"] = node_id
    if alert_type:  q["type"] = alert_type
    cur = db.alerts.find(q, {"_id": 0}).sort("ts", -1).limit(min(limit, 1000))
    return list(cur)


@app.get("/api/v1/events")
def list_events(event_type: str | None = None, village: str | None = None,
                node_id: str | None = None, from_iso: str | None = Query(None, alias="from"),
                limit: int = 200):
    limit = max(1, min(limit, 1000))
    q = since_from(from_iso, 72)
    if event_type: q["type"] = event_type
    if village:    q["village"] = village
    if node_id:    q["node_id"] = node_id
    cur = db.events.find(q, {"_id": 0}).sort("ts", -1).limit(min(limit, 1000))
    return list(cur)


@app.get("/api/v1/villages")
def list_villages():
    return list(db.villages.find({}, {"_id": 0, "topology": 0}).sort("village", 1))


@app.get("/api/v1/villages/{village}")
def get_village(village: str):
    v = db.villages.find_one({"village": village}, {"_id": 0})
    if not v:
        raise HTTPException(404, "village not found")
    return v


@app.get("/api/v1/masters")
def list_masters():
    return list(db.master_nodes.find({}, {"_id": 0}).sort("village", 1))


@app.get("/api/v1/stats")
def stats():
    return {
        "nodes": db.river_nodes.count_documents({}),
        "nodes_online": db.river_nodes.count_documents({"online": True}),
        "alerts_24h": db.alerts.count_documents(
            {"ts": {"$gte": datetime.fromtimestamp(now().timestamp() - 86400, tz=timezone.utc)}}),
        "heartbeats_1h": db.heartbeats.count_documents(
            {"ts": {"$gte": datetime.fromtimestamp(now().timestamp() - 3600, tz=timezone.utc)}}),
        "villages": db.villages.count_documents({}),
    }


@app.get("/api/v1/events/stream")
async def sse_stream(types: str | None = None):
    """SSE: every event parser publishes on Redis 'sse_events'.
    ?types=heartbeat,flood_level filters event types."""
    want = set(types.split(",")) if types else None

    async def gen():
        pubsub = rds.pubsub()
        await pubsub.subscribe(SSE_CHANNEL)
        try:
            # keepalive comment every 15 s so proxies don't idle out
            while True:
                msg = await pubsub.get_message(ignore_subscribe_messages=True, timeout=15.0)
                if msg is None:
                    yield ": keepalive\n\n"
                    continue
                evt = json.loads(msg["data"])
                if want and evt.get("type") not in want:
                    continue
                payload = {**evt["data"], "_ts": evt.get("ts")}
                yield f"event: {evt.get('type', 'message')}\ndata: {json.dumps(payload)}\n\n"
        finally:
            await pubsub.unsubscribe(SSE_CHANNEL)
            await pubsub.close()

    return StreamingResponse(gen(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})
