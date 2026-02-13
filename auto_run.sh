#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# auto start receiver -> send file -> cleanup

if [ $# -lt 5 ]; then
  echo "Usage: $0 <receiver_ip> <port> <file_path> <output_dir> <receiver_ssh_user>"
  echo "Example: $0 192.168.153.131 18500 /app/source/rdma-learn/test.txt /app/source/rdma-recv root"
  exit 1
fi

RECV_IP="$1"
PORT="$2"
FILE="$3"
OUT_DIR="$4"
USER="$5"

LOG="/tmp/rdma_receiver.log"

echo "[auto] start receiver: ${USER}@${RECV_IP}:${PORT} -> ${OUT_DIR}"
ssh "${USER}@${RECV_IP}" "mkdir -p '${OUT_DIR}' && cd /app/source/rdma-learn && nohup ./run_receiver.sh '${RECV_IP}' '${PORT}' '${OUT_DIR}' > '${LOG}' 2>&1 & echo \$! > /tmp/rdma_receiver.pid"

echo "[auto] send file: ${FILE}"
./run_sender.sh "${RECV_IP}" "${PORT}" "${FILE}"

echo "[auto] cleanup receiver and log"
ssh "${USER}@${RECV_IP}" "if [ -f /tmp/rdma_receiver.pid ]; then kill \$(cat /tmp/rdma_receiver.pid) >/dev/null 2>&1 || true; rm -f /tmp/rdma_receiver.pid; fi; rm -f '${LOG}'"

echo "[auto] done"
