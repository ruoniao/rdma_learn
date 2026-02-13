#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 3 ]; then
  echo "Usage: $0 <receiver_ip> <port> <file_path>"
  exit 1
fi

IP="$1"
PORT="$2"
FILE="$3"

echo "[run_sender] ip=${IP} port=${PORT} file=${FILE}"
./sender "${IP}" "${PORT}" "${FILE}"
