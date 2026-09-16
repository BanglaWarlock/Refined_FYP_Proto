#!/bin/bash
# =============================================================
#  FloodWatch — droplet/instance bootstrap (User Data script)
#
#  Paste into the "User data" field when creating a DigitalOcean
#  Droplet (or AWS EC2 Launch Template). Runs ONCE on first boot.
#  Idempotent — safe to re-run:
#      bash /var/lib/cloud/instance/scripts/*
#  After this, the instance is Docker-ready and GitHub Actions
#  can SSH in to start the actual containers.
#
#  NOTE: DPkg::Lock::Timeout waits out unattended-upgrades, which
#  holds the apt lock during first boot and otherwise kills this
#  script under `set -e`.
# =============================================================
set -euo pipefail
APT_LOCK="-o DPkg::Lock::Timeout=600"

# ── Docker ────────────────────────────────────────────────────
if ! command -v docker &>/dev/null; then
    apt-get $APT_LOCK update -qq
    apt-get $APT_LOCK install -y -qq ca-certificates curl gnupg

    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
        | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
    chmod a+r /etc/apt/keyrings/docker.gpg

    echo \
      "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
      https://download.docker.com/linux/ubuntu \
      $(. /etc/os-release && echo "$VERSION_CODENAME") stable" \
      | tee /etc/apt/sources.list.d/docker.list > /dev/null

    apt-get $APT_LOCK update -qq
    apt-get $APT_LOCK install -y -qq docker-ce docker-ce-cli containerd.io \
        docker-buildx-plugin docker-compose-plugin
fi

systemctl enable --now docker

# ── Add default users to docker group ────────────────────────
# DO default is "root"; EC2 defaults are "ubuntu"/"ec2-user"
for USER in root ubuntu ec2-user; do
    if id "$USER" &>/dev/null; then
        usermod -aG docker "$USER" 2>/dev/null || true
    fi
done

# ── App directory ─────────────────────────────────────────────
mkdir -p /opt/floodwatch
chmod 755 /opt/floodwatch

echo "Bootstrap complete — Docker $(docker --version)"
