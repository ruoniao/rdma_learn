#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

if [ $# -lt 3 ]; then
  echo "Usage: $0 <listen_ip> <port> <output_dir>"
  exit 1
fi

LISTEN_IP="$1"
PORT="$2"
OUT_DIR="$3"

echo "[run_receiver] ip=${LISTEN_IP} port=${PORT} out_dir=${OUT_DIR}"
./bin/receiver "${LISTEN_IP}" "${PORT}" "${OUT_DIR}"
