#!/usr/bin/env bash
set -euo pipefail

SERVER_BIN=${1:-build/lamseryn}
PROTECTED_PORT=${2:-18081}
PUBLIC_PORT=${3:-18082}
HOST=${HOST:-127.0.0.1}

if [[ ! -x "$SERVER_BIN" ]]; then
  echo "server binary not found/executable: $SERVER_BIN" >&2
  exit 2
fi
if ! command -v openssl >/dev/null 2>&1; then
  echo "openssl is required for auth integration tests" >&2
  exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required for auth integration tests" >&2
  exit 2
fi

TMP_ROOT=$(mktemp -d -t lamseryn_auth_itest.XXXXXX)
PROTECTED_ROOT="$TMP_ROOT/protected"
PUBLIC_ROOT="$TMP_ROOT/public"
HTPASSWD="$TMP_ROOT/test.htpasswd"
ITEST_INI="$TMP_ROOT/auth.ini"
LOG_FILE="$TMP_ROOT/server.log"
HDR_FILE="$TMP_ROOT/headers.txt"
BODY_FILE="$TMP_ROOT/body.txt"
server_pid=""

cleanup() {
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
  rm -rf "$TMP_ROOT"
}

trap cleanup EXIT

mkdir -p "$PROTECTED_ROOT" "$PUBLIC_ROOT"
printf 'protected auth ok\n' > "$PROTECTED_ROOT/index.html"
printf 'public auth bypass ok\n' > "$PUBLIC_ROOT/index.html"

PASS_HASH=$(openssl passwd -6 "hunter2")
printf 'alice:%s\n' "$PASS_HASH" > "$HTPASSWD"

cat >"$ITEST_INI" <<EOF
[globals]
log_level = info
log_categories = all
workers = 1
queue_depth = 1024
initial_idle_timeout_ms = 5000
keepalive_idle_close_ms = 5000
header_timeout_ms = 5000
body_timeout_ms = 5000
write_timeout_ms = 5000
drain_timeout_ms = 5000
accept_backoff_ms = 10
default_max_header_fields = 100

[vhost protected]
bind = $HOST
port = $PROTECTED_PORT
docroot = $PROTECTED_ROOT
static = true
auth = true
auth_basic_file = $HTPASSWD
auth_realm = Admin Area

[vhost public]
bind = $HOST
port = $PUBLIC_PORT
docroot = $PUBLIC_ROOT
static = true
auth = false
EOF

wait_for_port() {
  local port=$1
  local deadline=$((SECONDS + 5))
  while (( SECONDS < deadline )); do
    if (exec 3<>"/dev/tcp/$HOST/$port") 2>/dev/null; then
      exec 3>&-
      exec 3<&-
      return 0
    fi
    sleep 0.05
  done
  return 1
}

request_status() {
  local port=$1
  local credentials=${2:-}
  : > "$HDR_FILE"
  : > "$BODY_FILE"
  python3 - "$HOST" "$port" "$credentials" "$HDR_FILE" "$BODY_FILE" <<'PY'
import base64
import http.client
import sys

host = sys.argv[1]
port = int(sys.argv[2])
credentials = sys.argv[3]
headers_path = sys.argv[4]
body_path = sys.argv[5]

headers = {}
if credentials:
    token = base64.b64encode(credentials.encode("utf-8")).decode("ascii")
    headers["Authorization"] = f"Basic {token}"

conn = http.client.HTTPConnection(host, port, timeout=5)
conn.request("GET", "/", headers=headers)
resp = conn.getresponse()
body = resp.read()

with open(headers_path, "wb") as fp:
    for key, value in resp.getheaders():
        fp.write(f"{key}: {value}\r\n".encode("iso-8859-1", "replace"))

with open(body_path, "wb") as fp:
    fp.write(body)

print(resp.status, end="")
PY
}

echo "[auth-itest] starting server: $SERVER_BIN" >&2
SERVER_CONFIG="$ITEST_INI" "$SERVER_BIN" >"$LOG_FILE" 2>&1 &
server_pid=$!

if ! wait_for_port "$PROTECTED_PORT" || ! wait_for_port "$PUBLIC_PORT"; then
  echo "[auth-itest] server did not start listening" >&2
  tail -n 200 "$LOG_FILE" >&2 || true
  exit 1
fi

echo "[auth-itest] protected vhost without credentials -> 401" >&2
status=$(request_status "$PROTECTED_PORT")
[[ "$status" == "401" ]]
grep -iq '^WWW-Authenticate: Basic realm="Admin Area"' "$HDR_FILE"

echo "[auth-itest] protected vhost with wrong credentials -> 401" >&2
status=$(request_status "$PROTECTED_PORT" 'alice:wrong')
[[ "$status" == "401" ]]

echo "[auth-itest] protected vhost with valid credentials -> 200" >&2
status=$(request_status "$PROTECTED_PORT" 'alice:hunter2')
[[ "$status" == "200" ]]
grep -q 'protected auth ok' "$BODY_FILE"

echo "[auth-itest] public vhost without credentials -> 200" >&2
status=$(request_status "$PUBLIC_PORT")
[[ "$status" == "200" ]]
grep -q 'public auth bypass ok' "$BODY_FILE"

echo "[auth-itest] success" >&2