# FloodWatch LoRa Mesh Protocol v2

This document is the contract between river nodes, master nodes, and the cloud.
The firmware in `hardware/river_node` and `hardware/master_node` implements
exactly what is written here — keep the doc and the code in sync.

## 1. Topology & roles

- **Master node** — one per village. ESP32 + SX1278, WiFi backhaul to MQTT.
  Owns node registration, heartbeat liveness, topology building, and MQTT
  publishing. Depth 0.
- **River node** — ESP32 + SX1278 + solar/battery + GPS + 3 float switches
  (1/2/3 ft, active HIGH). discovers a parent, registers, relays for its own
  descendants. Depth ≥ 1.
- The mesh is a **tree**: every river node has exactly one parent at a time
  and may relay for a bounded number of children (`max_children`, NVS).
  Nodes never route sideways; upstream/downstream only.

## 2. Radio layer

| Param  | Demo (indoor) | Field (river) |
|--------|---------------|----------------|
| Freq   | 433 MHz       | 433 MHz        |
| SF     | **7**         | 10 (NVS `lora_sf`) |
| BW     | 125 kHz       | 125 kHz        |
| CR     | 4/5           | 4/5            |
| TX pwr | 17 dBm        | 17 dBm         |
| CRC    | on            | on             |

Airtime at SF7/125 kHz for a max-size 200-byte frame is ≈ 65 ms; at SF10 it is
≈ 528 ms. **All timing constants below assume the SF7 profile.** Switching a
node to SF10 by provisioning does not change the protocol, only how long each
exchange takes — the retries/ACKs are event-driven, not scheduled.

### Half-duplex discipline

1. **CAD before every transmit.** `channelActivityDetection()` scans for an
   existing preamble (~2 ms at SF7). Busy → random backoff (`CAD_BACKOFF_MS`
   + jitter), re-scan, up to `CAD_MAX_TRIES`, then transmit anyway (fail open:
   availability beats politeness; the ACK/retry layer recovers losses).
2. **Blocking TX.** `endPacket()` returns on TX-done, so the post-TX state is
   known exactly. Radio returns to RX immediately.
3. **Post-TX guard** `POST_TX_LISTEN_MS` (100 ms) keeps the radio on RX after
   a send so an immediate ACK/response can be received.
4. **Jitter everywhere.** Every retry and every DISC_RESP is jittered so that
   simultaneous wake-ups do not synchronise into permanent collisions.

## 3. Frame format

Text, NUL-free ASCII:

```
<msg_id:16-hex>|<src>|<dst>|<type:u8>|<payload>
```

- `src` is always the **original sender** — relays forward without rewriting
  it, so the master sees the true origin.
- `dst` is the next hop's node id, or `ALL` for broadcast.
- `payload` is `k=v` pairs separated by `,`.
- `msg_id` is freshly random per transmission (retransmissions get a new id);
  delivery is guaranteed by **ACK + seq**, not by id dedup (the dedup buffer
  is only a local echo filter).

### Freshness: `seq`

Every alert payload carries `seq=<u32>`: a per-origin monotonic counter,
seeded from `esp_random()` at boot and incremented per alert instance. The
random base guarantees a reboot never replays lower seqs than a pre-reboot
alert. Rule everywhere downstream:

> **Latest-wins.** A pending (un-ACKed) alert entry is replaced only by a
> payload whose `seq` is greater, or whose semantic content (flood `level`)
> differs. Equal or older `seq` retransmissions are absorbed (ACKed, not
> forwarded again).

Heartbeats carry their own `seq` but HB freshness is handled purely by
ACK-retry (a stale HB is harmless).

## 4. Message types

| Type | Name        | Direction | Purpose |
|------|-------------|-----------|---------|
| 5    | `HB`        | up        | heartbeat; ACK-tracked parent liveness |
| 6    | `SENSOR`    | up        | reserved for dense readings |
| 7    | `ALERT`     | up        | reliable event (see 7) |
| 8    | `CMD`       | down      | command from master, routed by id prefix |
| 9    | `CMD_ACK`   | up        | node confirms command |
| 10   | `BEACON`    | down      | parent announces presence to children |
| 11   | `ANNOUNCE`  | up        | "I am operational at (lat,lng)" — reliable |
| 12/13| `DISCOVER`/`DISC_RESP` | down/up | parent discovery |
| 14/15| `REG_REQ`/`REG_ACK`    | up/down | registration |
| 16   | `TOPO_REQ`  | down      | ask all nodes to re-announce |
| 17   | `ACK`       | down      | per-hop delivery confirmation (`ack_id=<msg_id>`) |
| 18   | `RELAY_REQ` | —         | unused/reserved |

### ACK semantics

- `ACK` clears **exactly one in-flight transmission** at the receiving relay
  by matching `ack_id` against the entry's `sent_msg_id`.
- For alert relays, ownership is per hop: C→B ACKed by B, B→A ACKed by A, and
  so on up to the master. A retransmission at any hop only re-floods that hop.
- Because entries are cleared by `sent_msg_id`, an old ACK can never clear a
  newer payload: when content is replaced, `sent_msg_id` is reset to 0, so the
  stale ACK matches nothing and the newer payload keeps retrying.

## 5. Membership (discovery → registration → operation)

River node FSM: `DISCOVERING → REGISTERING → OPERATIONAL ⇄ LOST_PARENT`.

1. **DISCOVER** (broadcast) → nodes that can accept children answer
   **DISC_RESP** (`id,depth,cur,max`, jittered 50–250 ms). The requester picks
   the best candidate by SNR → RSSI → lowest depth, skipping full nodes and
   its own parent.
2. **REG_REQ** (unicast) → **REG_ACK** (`id,parent,depth`). On ACK the node
   goes OPERATIONAL and immediately sends **ANNOUNCE** (reliable, includes GPS
   home position once calibrated).
3. Parent liveness: the child's HB is ACK-tracked
   (`HB_RETRY_MS × HB_MAX_RETRIES`); on exhaustion → `LOST_PARENT` → back to
   DISCOVERING (children cleared, pending alerts re-armed for the new parent).
4. Child liveness: a parent evicts a silent child after
   `CHILD_OFFLINE_TIMEOUT_MS` and — new in v2 — queues a **`node_lost`**
   alert (see §7) before freeing the slot.
5. A relay never advertises itself as a parent while its own crash report is
   still un-ACKed (`is_crash_pending()` gate), so the report gets priority.

## 6. Heartbeats

- Interval `HEARTBEAT_INTERVAL_MS`, ACK-tracked like alerts. The HB payload
  carries battery, float bits, depth, parent, GPS, fix flag, link quality.
- The **first relay** on the path injects `lsnr=/lrssi=` (link-layer quality
  of leaf→relay) if not already present — the master stores leaf→relay
  quality, which is the measurement that actually matters, and deep chains
  cannot grow the payload unboundedly.

## 7. Alerts, guarantee, and freshness (the core of v2)

Alert types and their payloads (`seq` added by the queue, not the caller):

| `type=`           | Trigger                          | Extra fields |
|-------------------|----------------------------------|--------------|
| `flood`           | float state change (incl. down)  | `level`, `float_bits` |
| `battery`         | V < 10.5 (hysteresis 11.5)       | `bat` |
| `gps_signal_lost` | fix expired after calibration    | `last_lat`, `last_lng` |
| `gps_restored`    | fix returned after loss          | `lat`, `lng` |
| `gps_moved`       | moved > `gps_move_thr` from home | `dist`, `home_lat`, `home_lng` |
| `crash`           | abnormal reset detected at boot  | `reason` |
| `node_lost`       | *new* — relay lost its child     | `lost`, `down_s` |

Guarantees, and the two scenarios that motivated v2:

**Scenario A — level change overtakes an old alert.** C reports `flood level=1`
(seq 5) through B. Before B's upstream copy is ACKed, C reports `level=2`
(seq 6). B replaces its pending entry with seq 6 and re-arms it immediately.
If an ACK for the *old* in-flight id (seq 5) arrives afterwards, it matches
nothing (`sent_msg_id` was reset) and cannot suppress the newer alert. The
master therefore converges on level 2 even when level-1 packets are still in
flight. Same rule at every hop and at the master (parser dedups by
`(node_id, type, seq)`).

**Scenario B — offline report overtaken by a re-announce.** B's child A goes
silent; B queues `node_lost lost=A`. If A re-appeared elsewhere and its
ANNOUNCE/HB is now being relayed *through* B (A re-parented under B's
descendant), B cancels its pending `node_lost` for A the moment it relays A's
traffic — the master never hears a false offline. If A re-appeared on a branch
B cannot hear, B's `node_lost` still arrives, and the master applies the
final rule:

> The master suppresses a `node_lost` for X if it has received any packet
> (announce/HB/alert) from X **after** the offline was detected
> (`last_seen` ordering). Offline is published only if it is still the newest
> knowledge about X. (Enforced master-side / parser-side; firmware's job is
> to cancel what it can hear.)

`node_lost` entries are keyed by `(relay_src, key, tag)` where `tag` is the
missing child's id — two children of the same relay can be missing
simultaneously without overwriting each other's report.

## 8. Commands (downstream)

`CMD` payload carries `target=<node_id>,data=<command>`. Each relay forwards
to the registered child whose id prefix matches the remaining path (ids are
hierarchically prefixed, e.g. `SUTS-001-…`). The target ACKs with `CMD_ACK`,
relayed back up. The master retries un-ACKed commands every
`CMD_RETRY_INTERVAL_MS` until ACK or the target is offline.

## 9. Timing budget (demo profile, SF7)

| Constant                  | Value | Rationale |
|---------------------------|-------|-----------|
| `HEARTBEAT_INTERVAL_MS`   | 10 s  | demo-visible liveness; field ≈ 60 s |
| `HB_RETRY_MS`             | 2 s   | > max airtime + turnaround at SF7 |
| `HB_MAX_RETRIES`          | 5     | parent declared lost ≈ 10 s |
| `ALERT_RETRY_MS`          | 1 s (+0–300 ms jitter) | fast demo propagation |
| `CAD_TIMEOUT_MS`          | 100 ms | scan itself is ~2 ms; generous |
| `CAD_MAX_TRIES`           | 5     | then fail open |
| `POST_TX_LISTEN_MS`       | 100 ms | half-duplex turnaround guard |
| `DISC_WINDOW_MS`          | 800 ms | SF7 responses land well within |
| `DISC_INTERVAL_MS`        | 10 s  | |
| `REG_TIMEOUT_MS`          | 6 s   | |
| `CHILD_OFFLINE_TIMEOUT_MS`| 45 s  | 4 missed HBs; field ≈ 180 s |
| `BEACON_INTERVAL_MS`      | 10 s  | only sent when the node has children |

Worst-case demo alert latency (leaf → master, 2 hops, channel idle):
`TX + 2×(ACK + relay) ≈ 0.3–0.8 s`. With channel contention, retries bound it
at a few seconds.

## 10. Invariants

1. Every un-ACKed transmission is retried forever until ACKed **or** the
   link it needs is declared dead (then re-armed for the new parent).
2. For a given `(origin, alert key/tag)` the master eventually receives the
   **highest seq** payload; it may briefly see older ones, never miss newer.
3. A node never claims another node offline unless it was that node's parent
   and the report can be superseded by newer knowledge.
4. Relays never alter `src`; only the first relay injects `lsnr/lrssi`.
5. All timers are event-driven (ACK/seq), not scheduled sync — no global
   timebase is required anywhere in the mesh.

## 11. Known gaps / next steps

- Master node firmware must implement: `node_lost` ACK + publish, seq-aware
  alert dedup before MQTT, offline-suppression rule (§7 scenario B).
- Parser: dedup alerts by `(node_id, type, seq)` with cooldown; apply §7
  suppression using `last_seen` ordering.
- `msg_id` dedup window (`DEDUP_SIZE=32`) is small; it is a local echo filter,
  not the reliability mechanism — keep it that way.
- ANNOUNCE/`node_lost` interplay across branches relies on master-side
  suppression when B cannot overhear A's new path.
