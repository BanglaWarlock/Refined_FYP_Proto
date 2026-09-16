# Deployment — DigitalOcean droplet (showcase setup)

Everything runs on **one droplet**: Mosquitto, parser, Redis, API. MongoDB
stays on Atlas (managed, free tier). The master nodes publish to the public
EMQX broker until the droplet broker is reachable from the demo site.

## 1. Create the droplet

1. DigitalOcean → Droplets → Create.
2. Region: **Singapore (SGP1)** — closest to Malaysia; pick the region you demo from.
3. Image: **Ubuntu 24.04 LTS**.
4. Size: **Basic / Regular / 2 vCPU 2 GB ($12/mo)** is comfortable; 1 GB works
   for the showcase but MongoDB-atlas + 4 containers on 1 GB is tight.
5. Authentication: your SSH key.
6. **Advanced → Add Initialization scripts (User data)** — paste the entire
   contents of [`iot-service/scripts/cloud-init.sh`](../iot-service/scripts/cloud-init.sh).
   That installs Docker and prepares `/opt/floodwatch` on first boot.
7. Create, note the public IP.

## 2. Verify bootstrap

```bash
ssh root@<droplet-ip>
docker --version          # should print a version
cat /var/log/cloud-init-output.log | tail -20   # "Bootstrap complete"
```

## 3. Broker choice

- **Showcase (now):** `broker.emqx.io` — public, no auth, no TLS. Fine for a
  demo. Topic namespace MUST be unique (see below) since anyone can subscribe.
- **Later / production:** Mosquitto on the droplet (config already exists in
  the old repo `mosquitto/`), point master nodes at `<droplet-ip>:1883`, add
  per-node credentials + TLS before any field use.

Topic scheme for the public broker — one slug per deployment so unrelated
systems never collide:

```
floodwatch/<deployment-slug>/<village>/sensor/<node_id>
floodwatch/<deployment-slug>/<village>/alert/<node_id>
...
```

e.g. `floodwatch/suts-demo/SUTS/sensor/SUTS-001`.

## 4. CI/CD (wired up with iot-service)

GitHub Actions deploys on push to `main`:

- Repo secrets: `SSH_HOST` (droplet IP), `SSH_USER` (`root`), `SSH_KEY`
  (private key), plus env secrets (`MONGO_URI`, `MQTT_BROKER`, `REDIS_URL`…).
- Workflow builds the parser/API images, SSHes to the droplet, and restarts
  the compose stack in `/opt/floodwatch`.

Detailed steps come with the `iot-service` workflows in this repo.

## 5. Cost note

Droplet billed hourly — for the showcase, power it on the day before and
destroy or snapshot afterwards. Same bootstrap script works on any Ubuntu
cloud host (AWS EC2, DO resize/rebuild) since it is plain cloud-init.
