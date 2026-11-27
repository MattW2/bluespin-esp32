#!/usr/bin/env bash
set -euo pipefail

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PI_DIR="$(cd "${DEPLOY_DIR}/.." && pwd)"
SECRETS_FILE="${DEPLOY_DIR}/secrets.env"

if [ ! -f "${SECRETS_FILE}" ]; then
  echo "Missing ${SECRETS_FILE}. Copy secrets.env.example to secrets.env first." >&2
  exit 1
fi

set -a
# shellcheck source=/dev/null
source "${SECRETS_FILE}"
set +a

PI_PORT="${PI_PORT:-22}"
PI_USER="${PI_USER:-pi}"
SSH_OPTS=(-p "${PI_PORT}" -o StrictHostKeyChecking=accept-new)
SCP_OPTS=(-P "${PI_PORT}" -o StrictHostKeyChecking=accept-new)
if [ -n "${PI_SSH_KEY:-}" ]; then
  SSH_OPTS+=(-i "${PI_SSH_KEY}")
  SCP_OPTS+=(-i "${PI_SSH_KEY}")
fi

TARGET="${PI_USER}@${PI_HOST}"
ssh "${SSH_OPTS[@]}" "${TARGET}" "rm -rf ~/bluespin-pi && mkdir -p ~/bluespin-pi"
scp "${SCP_OPTS[@]}" -r "${PI_DIR}/." "${TARGET}:~/bluespin-pi/"
ssh "${SSH_OPTS[@]}" "${TARGET}" "chmod +x ~/bluespin-pi/scripts/install.sh && sudo bash ~/bluespin-pi/scripts/install.sh"
