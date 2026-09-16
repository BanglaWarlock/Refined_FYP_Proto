#!/usr/bin/env python3
"""
One-shot database reset — drops ONLY the collections owned by parser v3:
  river_nodes, heartbeats, alerts, events, villages, master_nodes,
  failed_messages
Anything else in the cluster (other projects, exports) is untouched.

Run manually via the GitHub Actions workflow "Reset database", or directly:
  cd /opt/floodwatch && docker compose run --rm api python reset_db.py
"""

import os

from pymongo import MongoClient

# Explicit allow-list — never a blanket drop
COLLECTIONS = [
    "river_nodes",
    "heartbeats",
    "alerts",
    "events",
    "villages",
    "master_nodes",
    "failed_messages",
]

db = MongoClient(os.environ["MONGO_URI"]).get_default_database()
existing = set(db.list_collection_names())
for name in COLLECTIONS:
    if name in existing:
        db[name].drop()
        print(f"dropped  {name}")
    else:
        print(f"skip     {name} (not present)")
print("done — collections will be recreated with indexes on next parser start")
