#!/usr/bin/env bash
set -euo pipefail

# Simple smoke runner for bench_client. Assumes server is already running.
# Usage: ./tools/bench/run_smoke.sh [host] [port] [path]

HOST=${1:-127.0.0.1}
PORT=${2:-8080}
PATH_REQ=${3:-/}

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
BIN="$ROOT/build/bench_client"

if [ ! -x "$BIN" ]; then
  echo "bench_client not built; run: make bench" >&2
  exit 1
fi

echo "Running smoke: host=$HOST port=$PORT path=$PATH_REQ conns=64 duration=20s"
"$BIN" -h "$HOST" -p "$PORT" -c 64 -d 20 "$PATH_REQ"

echo "Running smoke (pipeline=4, small body)"
"$BIN" -h "$HOST" -p "$PORT" -c 64 -d 20 -P 4 -B 128 "$PATH_REQ"
