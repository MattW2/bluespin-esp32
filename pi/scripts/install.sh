#!/usr/bin/env bash
set -euo pipefail

# Idempotent Raspberry Pi OS (Bookworm / Trixie) setup script for BlueSpin Pi 3B.
# Run as root (sudo bash pi/scripts/install.sh).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
INSTALL_DIR="/opt/bluespin"
DATA_DIR="/var/lib/bluespin"
ENV_DIR="/etc/bluespin"

echo "==> Installing system dependencies..."
apt-get update
apt-get install -y --no-install-recommends \
  bluez \
  pipewire \
  pipewire-audio \
  pipewire-pulse \
  wireplumber \
  libspa-0.2-bluetooth \
  python3 \
  python3-venv \
  python3-dev \
  i2c-tools \
  fonts-dejavu-core \
  libavcodec-dev \
  libavformat-dev \
  libavutil-dev \
  libswresample-dev

echo "==> Enabling I2C interface for optional SSD1306 OLED..."
if command -v raspi-config >/dev/null 2>&1; then
  raspi-config nonint do_i2c 0 || true
fi

echo "==> Configuring BlueZ for A2DP Sink + Just Works SSP..."
mkdir -p /etc/bluetooth
cat >/etc/bluetooth/bluespin-override.conf <<'EOF'
# Managed by BlueSpin Pi
[General]
Class = 0x200414
DiscoverableTimeout = 0
PairableTimeout = 0
EOF
if ! grep -q "Class = 0x200414" /etc/bluetooth/main.conf 2>/dev/null; then
  sed -i 's/^#\?Class =.*/Class = 0x200414/' /etc/bluetooth/main.conf || true
fi

echo "==> Installing BlueSpin Pi into ${INSTALL_DIR}..."
mkdir -p "${INSTALL_DIR}" "${DATA_DIR}" "${ENV_DIR}"
rm -rf "${INSTALL_DIR}/app"
cp -r "${PI_DIR}" "${INSTALL_DIR}/app"

if [ ! -d "${INSTALL_DIR}/venv" ]; then
  python3 -m venv "${INSTALL_DIR}/venv"
fi
"${INSTALL_DIR}/venv/bin/pip" install --upgrade pip
"${INSTALL_DIR}/venv/bin/pip" install --no-cache-dir "${INSTALL_DIR}/app"

if [ ! -f "${ENV_DIR}/bluespin.env" ]; then
  cat >"${ENV_DIR}/bluespin.env" <<'EOF'
BLUESPIN_NAME=BlueSpin Pi
BLUESPIN_WEB_PORT=8080
BLUESPIN_SENDSPIN_PORT=8928
BLUESPIN_CODEC=opus
BLUESPIN_OLED_ENABLED=true
BLUESPIN_UNPAIRED_ACCESS=false
BLUESPIN_DATA_DIR=/var/lib/bluespin
EOF
fi

echo "==> Installing and starting bluespin.service..."
cp "${PI_DIR}/systemd/bluespin.service" /etc/systemd/system/bluespin.service
systemctl daemon-reload
systemctl enable bluetooth bluespin
systemctl restart bluetooth
systemctl restart bluespin

echo "==> Done! Check status with: systemctl status bluespin"
