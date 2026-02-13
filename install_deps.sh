#!/usr/bin/env bash
set -euo pipefail

ROLE="${1:-all}"

echo "[install] role=${ROLE}"
sudo apt-get update

case "${ROLE}" in
  node1|node2|all)
    sudo apt-get install -y build-essential rdma-core ibverbs-providers libibverbs-dev librdmacm-dev
    ;;
  *)
    echo "Usage: $0 [node1|node2|all]"
    exit 1
    ;;
esac

echo "[install] done"
