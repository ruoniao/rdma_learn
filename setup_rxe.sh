#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Usage: $0 <netdev>"
  echo "Example: $0 ens33"
  exit 1
fi

NETDEV="$1"

echo "[rxe] load kernel module"
sudo modprobe rdma_rxe

echo "[rxe] create rxe0 (ignore if exists)"
sudo rdma link add rxe0 type rxe netdev "${NETDEV}" 2>/dev/null || true

echo "[rxe] rdma link show:"
rdma link show

echo "[rxe] device info:"
if command -v ibv_devices >/dev/null 2>&1; then
  ibv_devices
elif command -v ibv_devinfo >/dev/null 2>&1; then
  ibv_devinfo -l
else
  echo "ibv_devices/ibv_devinfo not found"
fi
