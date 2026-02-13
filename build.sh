#!/usr/bin/env bash
set -euo pipefail

echo "[build] clean old binaries"
rm -f sender receiver

echo "[build] build sender"
gcc -Wall -O2 -o sender sender.c rdma_sim.c -lrdmacm -libverbs

echo "[build] build receiver"
gcc -Wall -O2 -o receiver receiver.c rdma_sim.c -lrdmacm -libverbs

echo "[build] done"
