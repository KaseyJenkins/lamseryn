#!/usr/bin/env bash
set -euo pipefail

SERVER_BIN=${1:-build/lamseryn}
CLIENT_BIN=${2:-build/pipeline_test}
PORT=${3:-18080}
THREADS=${4:-1}

HOST=${HOST:-127.0.0.1}
HOST_DEFAULT="$HOST"
TIMEOUT_MS=${TIMEOUT_MS:-5000}
PIPELINE_N=${PIPELINE_N:-50}
FRAG_DELAY_MS=${FRAG_DELAY_MS:-10}
VERBOSE=${VERBOSE:-0}
ENABLE_EXTRA_ITESTS=${ENABLE_EXTRA_ITESTS:-0}
ENABLE_SHUTDOWN_BASELINE_ITESTS=${ENABLE_SHUTDOWN_BASELINE_ITESTS:-0}
ENABLE_SHUTDOWN_ITESTS=${ENABLE_SHUTDOWN_ITESTS:-1}
ENABLE_ACCESS_LOG_ITESTS=${ENABLE_ACCESS_LOG_ITESTS:-1}
ENABLE_ACCESS_LOG_SATURATION_ITESTS=${ENABLE_ACCESS_LOG_SATURATION_ITESTS:-0}
ENABLE_ACCESS_LOG_IPV6_ITESTS=${ENABLE_ACCESS_LOG_IPV6_ITESTS:-1}
DOCROOT_SRC=${DOCROOT:-tests/integration/wwwroot}
ITEST_INI=$(mktemp -t lamseryn_itest.XXXXXX.ini)

# Use a temporary docroot so tests can create large files without leaving
# untracked artifacts in the repo.
DOCROOT_TMP=$(mktemp -d -t lamseryn_itest_docroot.XXXXXX)
DOCROOT="$DOCROOT_TMP"
ACCESS_LOG_FILE=""
ACCESS_LOG_ROTATED_FILE=""
ACCESS_LOG_FAIL_DIR=""
ACCESS_LOG_TEMP_FILES=()

register_access_log_temp_file() {
  local p=${1:-}
  if [[ -n "$p" ]]; then
    ACCESS_LOG_TEMP_FILES+=("$p")
  fi
}

cleanup() {
  stop_server
  if [[ -n "${DOCROOT_TMP:-}" ]] && [[ -d "$DOCROOT_TMP" ]]; then
    rm -rf "$DOCROOT_TMP" || true
  fi
  if [[ -n "${ITEST_INI:-}" ]] && [[ -f "$ITEST_INI" ]]; then
    rm -f "$ITEST_INI" || true
  fi
  if [[ -n "${ACCESS_LOG_FILE:-}" ]] && [[ -f "$ACCESS_LOG_FILE" ]]; then
    rm -f "$ACCESS_LOG_FILE" || true
  fi
  if [[ -n "${ACCESS_LOG_ROTATED_FILE:-}" ]] && [[ -f "$ACCESS_LOG_ROTATED_FILE" ]]; then
    rm -f "$ACCESS_LOG_ROTATED_FILE" || true
  fi
  if [[ -n "${ACCESS_LOG_FAIL_DIR:-}" ]] && [[ -d "$ACCESS_LOG_FAIL_DIR" ]]; then
    chmod 0755 "$ACCESS_LOG_FAIL_DIR" 2>/dev/null || true
    rm -rf "$ACCESS_LOG_FAIL_DIR" || true
  fi
  for p in "${ACCESS_LOG_TEMP_FILES[@]:-}"; do
    if [[ -n "$p" ]] && [[ -f "$p" ]]; then
      rm -f "$p" || true
    fi
  done
}

if [[ ! -x "$SERVER_BIN" ]]; then
  echo "server binary not found/executable: $SERVER_BIN" >&2
  exit 2
fi
if [[ ! -x "$CLIENT_BIN" ]]; then
  echo "client binary not found/executable: $CLIENT_BIN" >&2
  exit 2
fi

log_file=$(mktemp -t lamseryn_itest.XXXXXX.log)

server_pid=""

stop_server() {
  if [[ -n "${server_pid:-}" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
    for _ in {1..40}; do
      if ! kill -0 "$server_pid" 2>/dev/null; then
        break
      fi
      sleep 0.05
    done
    if kill -0 "$server_pid" 2>/dev/null; then
      kill -KILL "$server_pid" 2>/dev/null || true
    fi
  fi
  server_pid=""
}

trap cleanup EXIT

# Populate temp docroot (index.html + a large file to trigger streaming).
if [[ ! -f "$DOCROOT_SRC/index.html" ]]; then
  echo "docroot source missing index.html: $DOCROOT_SRC" >&2
  exit 2
fi

cp "$DOCROOT_SRC/index.html" "$DOCROOT/index.html"

BIG_BYTES=524288
dd if=/dev/zero of="$DOCROOT/big.bin" bs=1 count="$BIG_BYTES" status=none

# Compression test fixtures: CSS with fake precompressed siblings, plus a
# PNG with a fake .gz sibling to verify non-compressible MIME bypass.
printf 'body{color:red}\n' > "$DOCROOT/comp.css"
printf 'GZIPPED_CONTENT\n' > "$DOCROOT/comp.css.gz"
printf 'BROTLI_CONTENT_\n' > "$DOCROOT/comp.css.br"
printf 'image_data'        > "$DOCROOT/nocomp.png"
printf 'GZIPPED_PNG\n'     > "$DOCROOT/nocomp.png.gz"

# Backward compatibility: legacy env name forces shutdown lane on.
if [[ "$ENABLE_SHUTDOWN_BASELINE_ITESTS" == "1" ]]; then
  ENABLE_SHUTDOWN_ITESTS=1
fi

if [[ "$ENABLE_SHUTDOWN_ITESTS" == "1" ]]; then
  # Larger artifact to exercise in-flight drain behavior deterministically on loopback.
  HUGE_BYTES=16777216
  dd if=/dev/zero of="$DOCROOT/huge.bin" bs=1 count="$HUGE_BYTES" status=none
fi

THRESHOLD_BYTES=262145
dd if=/dev/zero of="$DOCROOT/big_threshold.bin" bs=1 count="$THRESHOLD_BYTES" status=none

start_server() {
  local features=${1:-}
  local access_log_enabled=${2:-false}
  local access_log_path=${3:-stderr}
  local compression_dynamic=${4:-false}
  local static=true
  local range=false
  local conditional=false
  local compression=false
  local auth=false

  if [[ "$features" == "all" ]]; then
    range=true
    conditional=true
    compression=true
    auth=true
  fi

  cat >"$ITEST_INI" <<EOF
[globals]
log_level = info
log_categories = all
workers = $THREADS
queue_depth = 4096
initial_idle_timeout_ms = 5000
keepalive_idle_close_ms = 10000
header_timeout_ms = 5000
body_timeout_ms = 200
write_timeout_ms = 10000
drain_timeout_ms = 200
accept_backoff_ms = 10
default_max_header_fields = 100
access_log_enabled = $access_log_enabled
access_log_path = $access_log_path
access_log_format = text
access_log_sample = 1
access_log_min_status = 100

[vhost itest]
bind = $HOST
port = $PORT
docroot = $DOCROOT
static = $static
range = $range
conditional = $conditional
compression = $compression
auth = $auth
compression_dynamic = $compression_dynamic
max_header_fields = 100
EOF

  echo "[itest] starting server: $SERVER_BIN (workers=$THREADS port=$PORT features=${features:-default} config=$ITEST_INI)" >&2
  if [[ -n "$features" ]]; then
    DOCROOT="$DOCROOT" SERVER_CONFIG="$ITEST_INI" SERVER_FEATURES="$features" "$SERVER_BIN" >"$log_file" 2>&1 &
  else
    DOCROOT="$DOCROOT" SERVER_CONFIG="$ITEST_INI" "$SERVER_BIN" >"$log_file" 2>&1 &
  fi
  server_pid=$!

  if ! wait_for_listen; then
    echo "[itest] server did not start listening on $HOST:$PORT" >&2
    echo "[itest] server log (tail):" >&2
    tail -n 200 "$log_file" >&2 || true
    exit 1
  fi
}

wait_for_listen() {
  local deadline=$((SECONDS + 5))
  while (( SECONDS < deadline )); do
    # bash TCP connect probe
    if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
      exec 3>&-
      exec 3<&-
      return 0
    fi
    sleep 0.05
  done
  return 1
}

ipv6_loopback_available() {
  if ! command -v python3 >/dev/null 2>&1; then
    return 1
  fi

  python3 - <<'PY' >/dev/null 2>&1
import socket

s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
try:
    s.bind(("::1", 0))
except OSError:
    raise SystemExit(1)
finally:
    s.close()
PY
}

run_client() {
  local mode=$1
  shift
  local extra=()
  if [[ "$VERBOSE" == "1" ]]; then
    extra+=("-v")
  fi
  ITEST_SERVER_PID="${server_pid:-}" "$CLIENT_BIN" "$mode" -H "$HOST" -P "$PORT" -t "$TIMEOUT_MS" "${extra[@]}" "$@"
}

start_server ""

echo "[itest] running static index (DOCROOT=$DOCROOT)" >&2
run_client static-index --nodelay

echo "[itest] running static HEAD index (expect headers only)" >&2
run_client static-head-index --nodelay

echo "[itest] running static large file (expect 200 + correct Content-Length)" >&2
run_client static-large-file --nodelay

echo "[itest] running static sendfile threshold file (expect sendfile path)" >&2
run_client static-sendfile-threshold --nodelay

echo "[itest] running method not allowed (expect 405)" >&2
run_client method-not-allowed --nodelay

echo "[itest] running pipeline coalesce" >&2
run_client pipeline -n "$PIPELINE_N" --nodelay

echo "[itest] running fragment CRLF" >&2
run_client fragment -d "$FRAG_DELAY_MS"

echo "[itest] running body pipeline (Content-Length)" >&2
run_client body-pipeline --nodelay

echo "[itest] running body pipeline (chunked)" >&2
run_client chunked-pipeline --nodelay --expected-status 200

echo "[itest] running chunked separate (pos==endp)" >&2
run_client chunked-separate --nodelay --expected-status 200

echo "[itest] running chunked split-first-byte (stash boundary)" >&2
run_client chunked-split-first-byte --nodelay --expected-status 200

echo "[itest] running TE+CL conflict (expect 400)" >&2
run_client te-cl-conflict --nodelay

echo "[itest] running duplicate Content-Length (expect 400)" >&2
run_client dup-cl-reject --nodelay

echo "[itest] running too many header fields (expect 431)" >&2
run_client too-many-headers --nodelay

echo "[itest] running echo request fields (expect 200 + echoed fields)" >&2
run_client echo-fields --nodelay

echo "[itest] running Transfer-Encoding: trailers (expect 501)" >&2
run_client te-trailers-reject --nodelay

echo "[itest] running Content-Length too large (expect 413)" >&2
run_client body-too-large-cl --nodelay

echo "[itest] running Expect: 100-continue accepted path (expect 200)" >&2
run_client expect-100-continue-ok --nodelay

echo "[itest] running unsupported Expect rejection (expect 400)" >&2
run_client expect-unsupported-reject --nodelay

echo "[itest] running invalid Content-Length syntax rejection (expect 400)" >&2
run_client cl-plus-invalid-reject --nodelay

echo "[itest] running encoded slash path rejection (expect 400)" >&2
run_client path-encoded-slash-reject --nodelay

echo "[itest] running malformed TE separator rejection (expect 400)" >&2
run_client te-double-comma-reject --nodelay

echo "[itest] running dot-segment normalization path handling (expect 404)" >&2
run_client path-dot-segments-normalize --nodelay

if [[ "$ENABLE_ACCESS_LOG_ITESTS" == "1" ]]; then
  stop_server
  ACCESS_LOG_FILE=$(mktemp -t lamseryn_access_itest.XXXXXX.log)
  register_access_log_temp_file "$ACCESS_LOG_FILE"
  ACCESS_LOG_ROTATED_FILE="${ACCESS_LOG_FILE}.1"
  register_access_log_temp_file "$ACCESS_LOG_ROTATED_FILE"
  start_server "" "true" "$ACCESS_LOG_FILE"

  echo "[itest] running access-log emission checks" >&2
  run_client static-index --nodelay
  run_client static-head-index --nodelay
  run_client static-large-file --nodelay
  run_client static-sendfile-threshold --nodelay
  run_client sendfile-keepalive-bytes-regression --nodelay
  run_client method-not-allowed --nodelay
  run_client te-cl-conflict --nodelay
  run_client too-many-headers --nodelay
  run_client te-trailers-reject --nodelay
  run_client body-too-large-cl --nodelay
  run_client body-timeout --nodelay
  run_client expect-100-continue-timeout --nodelay

  # Force deterministic flush behavior under batched direct logging.
  for _ in $(seq 1 70); do
    run_client static-index --nodelay
  done

  # Allow the worker to flush line writes before assertions.
  sleep 0.1

  if [[ ! -f "$ACCESS_LOG_FILE" ]]; then
    echo "[itest] access log file missing: $ACCESS_LOG_FILE" >&2
    exit 1
  fi

  log_lines=$(wc -l <"$ACCESS_LOG_FILE" | tr -d '[:space:]')
  if [[ "$log_lines" -lt "64" ]]; then
    echo "[itest] expected at least 64 access log lines after threshold flush, got $log_lines" >&2
    echo "[itest] access log contents:" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi

  if ! grep -q "method=GET" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected method=GET in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -q "method=HEAD" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected method=HEAD in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -q "status=200" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=200 in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -q "status=405" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=405 in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -q "status=400" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=400 in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -q "status=408" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=408 in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -q "status=413" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=413 in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -q "status=431" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=431 in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -q "status=501" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=501 in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -Eq '(^|[[:space:]])ip=' "$ACCESS_LOG_FILE"; then
    echo "[itest] expected ip= field in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if ! grep -Eq '(^|[[:space:]])port=' "$ACCESS_LOG_FILE"; then
    echo "[itest] expected port= field in access log" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi

  if [[ "$HOST" == "127.0.0.1" ]]; then
    if ! grep -q "ip=127.0.0.1" "$ACCESS_LOG_FILE"; then
      echo "[itest] expected ip=127.0.0.1 in access log for localhost IPv4 tests" >&2
      cat "$ACCESS_LOG_FILE" >&2 || true
      exit 1
    fi
    if ! grep -Eq '(^|[[:space:]])port=[1-9][0-9]*([[:space:]]|$)' "$ACCESS_LOG_FILE"; then
      echo "[itest] expected non-zero port= in access log for localhost IPv4 tests" >&2
      cat "$ACCESS_LOG_FILE" >&2 || true
      exit 1
    fi
  fi

  # --- bytes= field validation ---
  # Contract: bytes = Content-Length header value from the response (intent).
  index_bytes=$(wc -c < "$DOCROOT/index.html" | tr -d '[:space:]')
  if ! grep -q "status=200 bytes=${index_bytes} " "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=200 with bytes=$index_bytes (index.html size) in access log" >&2
    grep 'status=200' "$ACCESS_LOG_FILE" | head -3 >&2 || true
    exit 1
  fi
  if ! grep -q "status=405 bytes=0 " "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=405 with bytes=0 in access log" >&2
    grep 'status=405' "$ACCESS_LOG_FILE" | head -3 >&2 || true
    exit 1
  fi
  if ! grep -q "status=408 bytes=0 " "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=408 with bytes=0 in access log" >&2
    grep 'status=408' "$ACCESS_LOG_FILE" | head -3 >&2 || true
    exit 1
  fi
  if ! grep -q "status=413 bytes=0 " "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=413 with bytes=0 in access log" >&2
    grep 'status=413' "$ACCESS_LOG_FILE" | head -3 >&2 || true
    exit 1
  fi
  if ! grep -q "status=431 bytes=0 " "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=431 with bytes=0 in access log" >&2
    grep 'status=431' "$ACCESS_LOG_FILE" | head -3 >&2 || true
    exit 1
  fi
  if ! grep -q "status=501 bytes=0 " "$ACCESS_LOG_FILE"; then
    echo "[itest] expected status=501 with bytes=0 in access log" >&2
    grep 'status=501' "$ACCESS_LOG_FILE" | head -3 >&2 || true
    exit 1
  fi
  if ! grep -q "target=/big.bin status=200 bytes=524288 " "$ACCESS_LOG_FILE"; then
    echo "[itest] expected /big.bin access log line with bytes=524288" >&2
    grep 'target=/big.bin' "$ACCESS_LOG_FILE" | head -3 >&2 || true
    exit 1
  fi
  if ! grep -q "target=/big_threshold.bin status=200 bytes=262145 " "$ACCESS_LOG_FILE"; then
    echo "[itest] expected /big_threshold.bin access log line with bytes=262145" >&2
    grep 'target=/big_threshold.bin' "$ACCESS_LOG_FILE" | head -3 >&2 || true
    exit 1
  fi

  # --- stale-hint keep-alive regression ---
  # The sendfile-keepalive-bytes-regression test sends GET /big.bin (keep-alive)
  # then DELETE /__stale_hint_probe on the same connection. Match the exact
  # signature to avoid confusion with other DELETE-producing tests.
  ka_delete_sig='method=DELETE target=/__stale_hint_probe status=405 bytes=0 '
  if ! grep -q "$ka_delete_sig" "$ACCESS_LOG_FILE"; then
    echo "[itest] stale-hint regression: expected exact DELETE signature missing" >&2
    grep 'method=DELETE' "$ACCESS_LOG_FILE" | head -5 >&2 || true
    exit 1
  fi

  echo "[itest] running access-log reopen on SIGHUP checks" >&2
  sleep 0.4
  pre_lines=$(wc -l <"$ACCESS_LOG_FILE" | tr -d '[:space:]')
  if [[ "$pre_lines" -lt "3" ]]; then
    echo "[itest] expected at least 3 pre-rotation access log lines, got $pre_lines" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi

  mv "$ACCESS_LOG_FILE" "$ACCESS_LOG_ROTATED_FILE"
  : > "$ACCESS_LOG_FILE"

  if [[ -z "${server_pid:-}" ]] || ! kill -0 "$server_pid" 2>/dev/null; then
    echo "[itest] server pid missing before SIGHUP reopen check" >&2
    exit 1
  fi

  kill -HUP "$server_pid"
  reopen_seen=0
  for _ in $(seq 1 100); do
    if grep -q "access log: reopened path=$ACCESS_LOG_FILE" "$log_file"; then
      reopen_seen=1
      break
    fi
    sleep 0.02
  done
  if [[ "$reopen_seen" != "1" ]]; then
    echo "[itest] expected reopen success log line for path=$ACCESS_LOG_FILE" >&2
    tail -n 200 "$log_file" >&2 || true
    exit 1
  fi

  # Anchor old-file line count after reopen is observed to avoid race with
  # buffered pre-HUP writes that can legally land in the rotated inode.
  old_before_post=$(wc -l <"$ACCESS_LOG_ROTATED_FILE" | tr -d '[:space:]')

  # Force a deterministic post-reopen flush using threshold batching.
  for _ in $(seq 1 70); do
    run_client static-index --nodelay
  done
  sleep 0.1

  old_after_post=$(wc -l <"$ACCESS_LOG_ROTATED_FILE" | tr -d '[:space:]')
  new_after_post=$(wc -l <"$ACCESS_LOG_FILE" | tr -d '[:space:]')

  if [[ "$new_after_post" -lt "64" ]]; then
    echo "[itest] expected at least 64 post-reopen lines in new access log, got $new_after_post" >&2
    echo "[itest] new access log contents:" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi

  if [[ "$HOST" == "127.0.0.1" ]] && ! grep -q "ip=127.0.0.1" "$ACCESS_LOG_FILE"; then
    echo "[itest] expected post-reopen access log lines to include ip=127.0.0.1" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi
  if [[ "$HOST" == "127.0.0.1" ]] && ! grep -Eq '(^|[[:space:]])port=[1-9][0-9]*([[:space:]]|$)' "$ACCESS_LOG_FILE"; then
    echo "[itest] expected post-reopen access log lines to include non-zero port=" >&2
    cat "$ACCESS_LOG_FILE" >&2 || true
    exit 1
  fi

  if [[ "$old_after_post" != "$old_before_post" ]]; then
    echo "[itest] rotated access log changed after reopen: before=$old_before_post after=$old_after_post" >&2
    echo "[itest] rotated access log contents:" >&2
    cat "$ACCESS_LOG_ROTATED_FILE" >&2 || true
    exit 1
  fi

  stop_server
  start_server "" "true" "stderr"

  echo "[itest] running access-log reopen no-op checks (stderr sink)" >&2
  if [[ -z "${server_pid:-}" ]] || ! kill -0 "$server_pid" 2>/dev/null; then
    echo "[itest] server pid missing before stderr SIGHUP reopen check" >&2
    exit 1
  fi
  kill -HUP "$server_pid"
  sleep 0.1
  run_client static-index --nodelay
  if grep -q "access log reopen failed on SIGHUP" "$log_file"; then
    echo "[itest] stderr reopen unexpectedly reported failure" >&2
    tail -n 200 "$log_file" >&2 || true
    exit 1
  fi

  stop_server
  ACCESS_LOG_FAIL_DIR=$(mktemp -d -t lamseryn_access_fail.XXXXXX)
  ACCESS_LOG_FILE="$ACCESS_LOG_FAIL_DIR/access.log"
  start_server "" "true" "$ACCESS_LOG_FILE"

  echo "[itest] running access-log reopen failure-path checks" >&2
  run_client static-index --nodelay
  run_client static-head-index --nodelay

  # Deterministic reopen failure: remove parent directory so open(path)
  # fails with ENOENT regardless of runner privileges.
  rm -rf "$ACCESS_LOG_FAIL_DIR"

  if [[ -z "${server_pid:-}" ]] || ! kill -0 "$server_pid" 2>/dev/null; then
    echo "[itest] server pid missing before failure-path SIGHUP reopen check" >&2
    exit 1
  fi
  kill -HUP "$server_pid"
  sleep 0.2

  # Reopen failure must not fail request handling.
  run_client static-index --nodelay
  if ! grep -q "access log reopen failed on SIGHUP" "$log_file"; then
    echo "[itest] expected reopen failure log line on SIGHUP" >&2
    tail -n 200 "$log_file" >&2 || true
    exit 1
  fi

  if [[ "$ENABLE_ACCESS_LOG_IPV6_ITESTS" == "1" ]]; then
    if ! command -v python3 >/dev/null 2>&1; then
      echo "[itest] IPv6 access-log checks are enabled but python3 is unavailable" >&2
      exit 1
    fi
    if ! ipv6_loopback_available; then
      echo "[itest] IPv6 access-log checks are enabled but IPv6 loopback (::1) is unavailable" >&2
      exit 1
    fi

    stop_server
    HOST="::1"
    ACCESS_LOG_FILE=$(mktemp -t lamseryn_access_itest_v6.XXXXXX.log)
    register_access_log_temp_file "$ACCESS_LOG_FILE"
    ACCESS_LOG_ROTATED_FILE="${ACCESS_LOG_FILE}.1"
    register_access_log_temp_file "$ACCESS_LOG_ROTATED_FILE"
    start_server "" "true" "$ACCESS_LOG_FILE"

    echo "[itest] running access-log IPv6 loopback checks" >&2
    run_client static-index --nodelay
    run_client static-head-index --nodelay
    run_client method-not-allowed --nodelay
    for _ in $(seq 1 70); do
      run_client static-index --nodelay
    done
    sleep 0.1

    if ! grep -Eq '(^|[[:space:]])ip=::1([[:space:]]|$)' "$ACCESS_LOG_FILE"; then
      echo "[itest] expected ip=::1 in access log for localhost IPv6 tests" >&2
      cat "$ACCESS_LOG_FILE" >&2 || true
      exit 1
    fi
    if ! grep -Eq '(^|[[:space:]])port=[1-9][0-9]*([[:space:]]|$)' "$ACCESS_LOG_FILE"; then
      echo "[itest] expected non-zero port= in access log for localhost IPv6 tests" >&2
      cat "$ACCESS_LOG_FILE" >&2 || true
      exit 1
    fi

    stop_server
    HOST="$HOST_DEFAULT"
    ACCESS_LOG_FILE=$(mktemp -t lamseryn_access_itest.XXXXXX.log)
    register_access_log_temp_file "$ACCESS_LOG_FILE"
    ACCESS_LOG_ROTATED_FILE="${ACCESS_LOG_FILE}.1"
    register_access_log_temp_file "$ACCESS_LOG_ROTATED_FILE"
    start_server "" "true" "$ACCESS_LOG_FILE"
  fi

  echo "[itest] running access-log forced format-failure accounting checks" >&2
  stop_server
  old_threads="$THREADS"
  THREADS=2
  ACCESS_LOG_FILE=$(mktemp -t lamseryn_access_itest_forcefail.XXXXXX.log)
  register_access_log_temp_file "$ACCESS_LOG_FILE"
  ACCESS_LOG_ROTATED_FILE="${ACCESS_LOG_FILE}.1"
  register_access_log_temp_file "$ACCESS_LOG_ROTATED_FILE"
  export ACCESS_LOG_FORCE_ASYNC_WRITER=1
  export ACCESS_LOG_TEST_FORCE_FORMAT_FAIL=1
  start_server "" "true" "$ACCESS_LOG_FILE"
  unset ACCESS_LOG_FORCE_ASYNC_WRITER
  unset ACCESS_LOG_TEST_FORCE_FORMAT_FAIL
  run_client static-index --nodelay
  run_client static-head-index --nodelay
  sleep 0.1
  stop_server
  THREADS="$old_threads"

  counters_line=$(grep 'access log counters:' "$log_file" | tail -n 1 || true)
  if [[ -z "$counters_line" ]]; then
    echo "[itest] expected access log counters line after forced format-failure run" >&2
    tail -n 200 "$log_file" >&2 || true
    exit 1
  fi
  dropped_count=$(echo "$counters_line" | sed -E 's/.* dropped=([0-9]+).*/\1/' || true)
  if [[ ! "$dropped_count" =~ ^[0-9]+$ ]] || [[ "$dropped_count" -le 0 ]]; then
    echo "[itest] expected dropped>0 after forced format-failure run; got dropped=$dropped_count" >&2
    echo "$counters_line" >&2
    exit 1
  fi

  ACCESS_LOG_FILE=$(mktemp -t lamseryn_access_itest.XXXXXX.log)
  register_access_log_temp_file "$ACCESS_LOG_FILE"
  ACCESS_LOG_ROTATED_FILE="${ACCESS_LOG_FILE}.1"
  register_access_log_temp_file "$ACCESS_LOG_ROTATED_FILE"
  start_server "" "true" "$ACCESS_LOG_FILE"

fi

if [[ "$ENABLE_ACCESS_LOG_SATURATION_ITESTS" == "1" ]]; then
  stop_server
  start_server "" "true" "/dev/full"

  echo "[itest] running access-log sink-failure resilience checks" >&2
  run_client static-index --nodelay
  run_client static-head-index --nodelay
  run_client method-not-allowed --nodelay

  # Give the writer thread a chance to drain/retry before shutdown summary logs.
  sleep 0.1
  stop_server
fi

if [[ "$ENABLE_EXTRA_ITESTS" == "1" ]]; then
  echo "[itest] running chunked body too large (expect 413)" >&2
  run_client body-too-large-chunked --nodelay

  echo "[itest] running body timeout (expect 408)" >&2
  run_client body-timeout --nodelay

  echo "[itest] running Expect:100-continue body timeout (expect 408)" >&2
  run_client expect-100-continue-timeout --nodelay
fi

if [[ "$ENABLE_SHUTDOWN_ITESTS" == "1" ]]; then
  echo "[itest] running shutdown drain: inflight response completes after SIGTERM" >&2
  run_client shutdown-inflight-drains-complete --nodelay

  # First shutdown mode signals the server; restart for next mode.
  stop_server
  start_server ""

  echo "[itest] running shutdown drain: keepalive pipeline completes after SIGTERM" >&2
  run_client shutdown-keepalive-drains-pipeline --nodelay

  # Next shutdown mode also signals the server; restart before continuing.
  stop_server
  start_server ""

  echo "[itest] running shutdown force: grace timeout forces bounded exit" >&2
  run_client shutdown-grace-timeout-forces-exit --nodelay

  # Next shutdown mode also signals the server; restart before continuing.
  stop_server
  start_server ""

  echo "[itest] running shutdown force: second SIGTERM forces immediate exit" >&2
  run_client shutdown-second-signal-forces-immediate --nodelay

  # Final shutdown mode also signals the server; keep runner state clean before next section.
  stop_server
  start_server ""
fi

# Restart server with all features enabled and validate header storage policy end-to-end.
stop_server
start_server "all"

echo "[itest] running echo feature headers (SERVER_FEATURES=all)" >&2
run_client echo-features-all --nodelay

echo "[itest] running conditional 304 Not Modified (SERVER_FEATURES=all)" >&2
run_client conditional-304 --nodelay

echo "[itest] running range requests 206/416 (SERVER_FEATURES=all)" >&2
run_client range-requests --nodelay

echo "[itest] running precompressed sibling serving (SERVER_FEATURES=all)" >&2
run_client precompressed --nodelay

stop_server
start_server "all" "false" "stderr" "true"
echo "[itest] running dynamic compression (compression_dynamic=true)" >&2
DOCROOT="$DOCROOT" run_client dynamic-compression --nodelay

echo "[itest] OK" >&2
