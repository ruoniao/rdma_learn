#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

if [ $# -lt 3 ]; then
  echo "Usage: $0 <receiver_ip> <port> <file_path>"
  exit 1
fi

IP="$1"
PORT="$2"
FILE="$3"

echo "[run_sender] ip=${IP} port=${PORT} file=${FILE}"
./bin/sender "${IP}" "${PORT}" "${FILE}"
