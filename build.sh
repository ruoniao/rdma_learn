#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

mkdir -p bin

echo "[build] clean old binaries"
rm -f bin/sender bin/receiver

echo "[build] build sender"
gcc -Wall -O2 -Iinclude -o bin/sender src/sender.c src/rdma_sim.c -lrdmacm -libverbs

echo "[build] build receiver"
gcc -Wall -O2 -Iinclude -o bin/receiver src/receiver.c src/rdma_sim.c -lrdmacm -libverbs

echo "[build] done"
