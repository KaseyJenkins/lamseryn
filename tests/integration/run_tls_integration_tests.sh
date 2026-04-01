#!/usr/bin/env bash
set -euo pipefail

SERVER_BIN=${1:-build/lamseryn_itest}
PORT=${2:-18443}
THREADS=${3:-1}

HOST=${HOST:-127.0.0.1}
DOCROOT_SRC=${DOCROOT:-tests/integration/wwwroot}
OPENSSL_PREFIX=${OPENSSL_PREFIX:-}
SNI_DEFAULT=${SNI_DEFAULT:-tls-itest-default.local}
SNI_ALT=${SNI_ALT:-tls-itest-alt.local}
SNI_UNKNOWN=${SNI_UNKNOWN:-tls-itest-unknown.local}

if [[ -z "$OPENSSL_PREFIX" ]] && [[ -x "third_party/openssl/_install/bin/openssl" ]]; then
  OPENSSL_PREFIX="third_party/openssl/_install"
fi

OPENSSL_BIN=""
if [[ -n "$OPENSSL_PREFIX" ]] && [[ -x "$OPENSSL_PREFIX/bin/openssl" ]]; then
  OPENSSL_BIN="$OPENSSL_PREFIX/bin/openssl"
elif command -v openssl >/dev/null 2>&1; then
  OPENSSL_BIN="$(command -v openssl)"
fi

OPENSSL_CNF=""
if [[ -n "$OPENSSL_PREFIX" ]] && [[ -f "$OPENSSL_PREFIX/ssl/openssl.cnf" ]]; then
  OPENSSL_CNF="$OPENSSL_PREFIX/ssl/openssl.cnf"
elif [[ -f "/etc/ssl/openssl.cnf" ]]; then
  OPENSSL_CNF="/etc/ssl/openssl.cnf"
fi

run_openssl() {
  if [[ -n "$OPENSSL_PREFIX" ]] && [[ -d "$OPENSSL_PREFIX/lib" ]]; then
    if [[ -n "$OPENSSL_CNF" ]]; then
      OPENSSL_CONF="$OPENSSL_CNF" \
        LD_LIBRARY_PATH="$OPENSSL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$OPENSSL_BIN" "$@"
    else
      LD_LIBRARY_PATH="$OPENSSL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$OPENSSL_BIN" "$@"
    fi
    return
  fi
  "$OPENSSL_BIN" "$@"
}

if [[ ! -x "$SERVER_BIN" ]]; then
  echo "tls-itest: server binary not found/executable: $SERVER_BIN" >&2
  exit 2
fi

if [[ -z "$OPENSSL_BIN" ]]; then
  echo "tls-itest: openssl CLI is required" >&2
  exit 2
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "tls-itest: python3 is required" >&2
  exit 2
fi

if [[ ! -f "$DOCROOT_SRC/index.html" ]]; then
  echo "tls-itest: docroot source missing index.html: $DOCROOT_SRC" >&2
  exit 2
fi

DOCROOT_TMP=$(mktemp -d -t lamseryn_tls_itest_docroot.XXXXXX)
CERT_DIR=$(mktemp -d -t lamseryn_tls_itest_cert.XXXXXX)
ITEST_INI=$(mktemp -t lamseryn_tls_itest.XXXXXX.ini)
LOG_FILE=$(mktemp -t lamseryn_tls_itest.XXXXXX.log)

DOCROOT="$DOCROOT_TMP"
CERT_FILE="$CERT_DIR/server.crt"
KEY_FILE="$CERT_DIR/server.key"
CERT_FILE_ALT="$CERT_DIR/server_alt.crt"
KEY_FILE_ALT="$CERT_DIR/server_alt.key"
CERT_FILE_ROT="$CERT_DIR/server_rotated.crt"
KEY_FILE_ROT="$CERT_DIR/server_rotated.key"
CERT_FILE_OLD="$CERT_DIR/server_old.crt"

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

cleanup() {
  stop_server
  rm -rf "$DOCROOT_TMP" "$CERT_DIR" || true
  rm -f "$ITEST_INI" "$LOG_FILE" || true
}

trap cleanup EXIT

wait_for_listen() {
  local deadline=$((SECONDS + 5))
  while (( SECONDS < deadline )); do
    if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
      exec 3>&-
      exec 3<&-
      return 0
    fi
    sleep 0.05
  done
  return 1
}

cp "$DOCROOT_SRC/index.html" "$DOCROOT/index.html"

python3 - "$DOCROOT/large.txt" <<'PY'
import pathlib, sys

out = pathlib.Path(sys.argv[1])
chunk = b"TLS_LARGE_BODY_PATTERN_0123456789abcdef\n"
target = 256 * 1024
parts = []
size = 0
while size < target:
  parts.append(chunk)
  size += len(chunk)
out.write_bytes(b"".join(parts)[:target])
PY

run_openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
  -subj "/CN=$SNI_DEFAULT" \
  -keyout "$KEY_FILE" \
  -out "$CERT_FILE" >/dev/null 2>&1

run_openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
  -subj "/CN=$SNI_ALT" \
  -keyout "$KEY_FILE_ALT" \
  -out "$CERT_FILE_ALT" >/dev/null 2>&1

run_openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
  -subj "/CN=$SNI_DEFAULT" \
  -keyout "$KEY_FILE_ROT" \
  -out "$CERT_FILE_ROT" >/dev/null 2>&1

echo "[tls-itest] openssl cli: $OPENSSL_BIN" >&2
run_openssl version >&2

cat >"$ITEST_INI" <<EOF
[globals]
log_level = info
log_categories = all
workers = $THREADS
queue_depth = 4096
initial_idle_timeout_ms = 1000
keepalive_idle_close_ms = 2000
header_timeout_ms = 500
body_timeout_ms = 500
write_timeout_ms = 2000
drain_timeout_ms = 200

[vhost $SNI_DEFAULT]
bind = $HOST
port = $PORT
docroot = $DOCROOT
static = true
tls = true
tls_cert_file = $CERT_FILE
tls_key_file = $KEY_FILE
tls_min_version = tls1.2

[vhost $SNI_ALT]
bind = $HOST
port = $PORT
docroot = $DOCROOT
static = true
tls = true
tls_cert_file = $CERT_FILE_ALT
tls_key_file = $KEY_FILE_ALT
tls_min_version = tls1.2
EOF

echo "[tls-itest] starting server: $SERVER_BIN (workers=$THREADS port=$PORT config=$ITEST_INI)" >&2
SERVER_CONFIG="$ITEST_INI" "$SERVER_BIN" >"$LOG_FILE" 2>&1 &
server_pid=$!

if ! wait_for_listen; then
  echo "[tls-itest] server did not start listening on $HOST:$PORT" >&2
  tail -n 200 "$LOG_FILE" >&2 || true
  exit 1
fi

echo "[tls-itest] TLS handshake succeeds (openssl s_client)" >&2
if ! run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_DEFAULT" \
  -CAfile "$CERT_FILE" \
  -verify_return_error \
  -brief \
  </dev/null >/dev/null 2>&1; then
  echo "tls-itest: openssl s_client handshake check failed" >&2
  exit 1
fi

echo "[tls-itest] SNI selects alternate vhost certificate" >&2
if ! run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_ALT" \
  -CAfile "$CERT_FILE_ALT" \
  -verify_return_error \
  -brief \
  </dev/null >/dev/null 2>&1; then
  echo "tls-itest: expected SNI alt cert handshake to succeed" >&2
  exit 1
fi

echo "[tls-itest] unmatched SNI deterministically falls back to default cert" >&2
if ! run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_UNKNOWN" \
  -CAfile "$CERT_FILE" \
  -verify_return_error \
  -brief \
  </dev/null >/dev/null 2>&1; then
  echo "tls-itest: expected unmatched SNI to fall back to default cert" >&2
  exit 1
fi

if run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_UNKNOWN" \
  -CAfile "$CERT_FILE_ALT" \
  -verify_return_error \
  -brief \
  </dev/null >/dev/null 2>&1; then
  echo "tls-itest: unmatched SNI unexpectedly validated against alt cert" >&2
  exit 1
fi

echo "[tls-itest] SIGHUP reload swaps default certificate for new handshakes" >&2
if ! run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_DEFAULT" \
  -CAfile "$CERT_FILE" \
  -verify_return_error \
  -brief \
  </dev/null >/dev/null 2>&1; then
  echo "tls-itest: expected pre-reload default cert to validate" >&2
  exit 1
fi

if run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_DEFAULT" \
  -CAfile "$CERT_FILE_ROT" \
  -verify_return_error \
  -brief \
  </dev/null >/dev/null 2>&1; then
  echo "tls-itest: rotated cert unexpectedly validated before reload" >&2
  exit 1
fi

cp -f "$CERT_FILE" "$CERT_FILE_OLD"
echo "[tls-itest] existing TLS connection survives reload on old context" >&2
python3 - "$HOST" "$PORT" "$server_pid" "$CERT_FILE_OLD" "$CERT_FILE" "$KEY_FILE" "$CERT_FILE_ROT" "$KEY_FILE_ROT" <<'PY'
import os
import shutil
import signal
import socket
import ssl
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
server_pid = int(sys.argv[3])
ca_old = sys.argv[4]
live_cert = sys.argv[5]
live_key = sys.argv[6]
rot_cert = sys.argv[7]
rot_key = sys.argv[8]

def recv_headers(tls_sock):
  data = b""
  while b"\r\n\r\n" not in data:
    chunk = tls_sock.recv(4096)
    if not chunk:
      raise SystemExit("connection closed before headers complete")
    data += chunk

  head, body_start = data.split(b"\r\n\r\n", 1)
  lines = head.decode("iso-8859-1", errors="replace").split("\r\n")
  if not lines or not lines[0].startswith("HTTP/1."):
    raise SystemExit(f"invalid status line: {lines[0] if lines else '<empty>'}")
  parts = lines[0].split()
  if len(parts) < 2 or parts[1] != "200":
    raise SystemExit(f"expected HTTP 200, got: {lines[0]}")

  cl = None
  for ln in lines[1:]:
    if ":" not in ln:
      continue
    k, v = ln.split(":", 1)
    if k.strip().lower() == "content-length":
      cl = int(v.strip())
      break
  if cl is None:
    raise SystemExit("missing content-length in response")

  return cl, body_start

def recv_body_remainder(tls_sock, body, expected):
  while len(body) < expected:
    chunk = tls_sock.recv(4096)
    if not chunk:
      raise SystemExit("connection closed before full body")
    body += chunk
  return body[:expected]

ctx = ssl.create_default_context(cafile=ca_old)
ctx.check_hostname = False

with socket.create_connection((host, port), timeout=3.0) as raw:
  raw.settimeout(3.0)
  with ctx.wrap_socket(raw, server_hostname=host) as tls_sock:
    req = f"GET /large.txt HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode("ascii")
    tls_sock.sendall(req)
    content_len, body = recv_headers(tls_sock)
    if content_len != 256 * 1024:
      raise SystemExit(f"unexpected content-length for /large.txt: {content_len}")
    if not body:
      chunk = tls_sock.recv(4096)
      if not chunk:
        raise SystemExit("no body received for /large.txt")
      body += chunk
    if not body.startswith(b"TLS_LARGE_BODY_PATTERN"):
      raise SystemExit("unexpected /large.txt body prefix before reload")

    shutil.copyfile(rot_cert, live_cert)
    shutil.copyfile(rot_key, live_key)
    os.kill(server_pid, signal.SIGHUP)
    time.sleep(0.15)

    full_body = recv_body_remainder(tls_sock, body, content_len)
    if len(full_body) != content_len:
      raise SystemExit("in-flight /large.txt response incomplete after reload")
PY

reload_ok=0
for _ in {1..40}; do
  if run_openssl s_client \
    -connect "$HOST:$PORT" \
    -servername "$SNI_DEFAULT" \
    -CAfile "$CERT_FILE_ROT" \
    -verify_return_error \
    -brief \
    </dev/null >/dev/null 2>&1; then
    reload_ok=1
    break
  fi
  sleep 0.05
done

if [[ "$reload_ok" -ne 1 ]]; then
  echo "tls-itest: rotated cert did not validate after SIGHUP reload" >&2
  tail -n 200 "$LOG_FILE" >&2 || true
  exit 1
fi

if run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_DEFAULT" \
  -CAfile "$CERT_FILE_OLD" \
  -verify_return_error \
  -brief \
  </dev/null >/dev/null 2>&1; then
  echo "tls-itest: old default cert unexpectedly validated after reload" >&2
  exit 1
fi

echo "[tls-itest] HTTPS GET returns expected response" >&2
tls_http_resp=$(printf 'GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n' "$HOST" |
  run_openssl s_client \
    -connect "$HOST:$PORT" \
    -servername "$SNI_DEFAULT" \
    -CAfile "$CERT_FILE" \
    -verify_return_error \
    -quiet 2>/dev/null || true)

if ! grep -q "^HTTP/1\\.[01] 200" <<<"$tls_http_resp"; then
  echo "tls-itest: expected HTTPS status line, got:" >&2
  printf '%s\n' "$tls_http_resp" >&2
  exit 1
fi

if ! grep -q "DEFAULT" <<<"$tls_http_resp"; then
  echo "tls-itest: expected HTTPS body to contain DEFAULT" >&2
  printf '%s\n' "$tls_http_resp" >&2
  exit 1
fi

echo "[tls-itest] TLS serves two sequential requests" >&2
tls_http_resp_1=$(printf 'GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n' "$HOST" |
  run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_DEFAULT" \
  -CAfile "$CERT_FILE" \
  -verify_return_error \
  -quiet 2>/dev/null || true)

tls_http_resp_2=$(printf 'GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n' "$HOST" |
  run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_DEFAULT" \
  -CAfile "$CERT_FILE" \
  -verify_return_error \
  -quiet 2>/dev/null || true)

if ! grep -q "^HTTP/1\\.[01] 200" <<<"$tls_http_resp_1"; then
  echo "tls-itest: expected first sequential HTTPS request to return 200" >&2
  printf '%s\n' "$tls_http_resp_1" >&2
  exit 1
fi

if ! grep -q "^HTTP/1\\.[01] 200" <<<"$tls_http_resp_2"; then
  echo "tls-itest: expected second sequential HTTPS request to return 200" >&2
  printf '%s\n' "$tls_http_resp_2" >&2
  exit 1
fi

if ! grep -q "DEFAULT" <<<"$tls_http_resp_1" || ! grep -q "DEFAULT" <<<"$tls_http_resp_2"; then
  echo "tls-itest: expected DEFAULT body in sequential HTTPS responses" >&2
  printf '--- response 1 ---\n%s\n--- response 2 ---\n%s\n' "$tls_http_resp_1" "$tls_http_resp_2" >&2
  exit 1
fi

echo "[tls-itest] TLS large static response is complete" >&2
large_resp_raw=$(mktemp -t lamseryn_tls_large_resp.XXXXXX.bin)
printf 'GET /large.txt HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n' "$HOST" |
  run_openssl s_client \
  -connect "$HOST:$PORT" \
  -servername "$SNI_DEFAULT" \
  -CAfile "$CERT_FILE" \
  -verify_return_error \
  -quiet 2>/dev/null >"$large_resp_raw" || true

python3 - "$large_resp_raw" <<'PY'
import re
import sys

path = sys.argv[1]
expected_len = 256 * 1024
raw = open(path, "rb").read()

if b"\r\n\r\n" not in raw:
  raise SystemExit("missing header/body boundary in /large.txt TLS response")

head, body = raw.split(b"\r\n\r\n", 1)
lines = head.decode("iso-8859-1", errors="replace").split("\r\n")
m = re.match(r"^HTTP/1\.[01]\s+(\d{3})", lines[0])
if not m:
  raise SystemExit(f"invalid status line for /large.txt: {lines[0]!r}")
status = int(m.group(1))
if status != 200:
  raise SystemExit(f"expected 200 for /large.txt, got {status}")

content_length = None
for ln in lines[1:]:
  if ":" not in ln:
    continue
  k, v = ln.split(":", 1)
  if k.strip().lower() == "content-length":
    content_length = int(v.strip())
    break

if content_length is None:
  raise SystemExit("missing content-length for /large.txt")
if content_length != expected_len:
  raise SystemExit(f"unexpected content-length: {content_length}, expected {expected_len}")
if len(body) < expected_len:
  raise SystemExit(f"incomplete /large.txt body: got {len(body)} bytes, expected {expected_len}")
if not body.startswith(b"TLS_LARGE_BODY_PATTERN"):
  raise SystemExit("/large.txt body prefix mismatch")
PY
rm -f "$large_resp_raw"

echo "[tls-itest] plaintext request to TLS port is rejected" >&2
python3 - "$HOST" "$PORT" <<'PY'
import socket, sys, time

host = sys.argv[1]
port = int(sys.argv[2])

with socket.create_connection((host, port), timeout=2.0) as s:
    s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    s.settimeout(2.0)
    deadline = time.time() + 2.0
    observed = b""
    while time.time() < deadline:
        try:
            b = s.recv(256)
        except (ConnectionResetError, BrokenPipeError):
            break
        except socket.timeout:
            continue
        if not b:
            break
        observed += b
        break

if observed.startswith(b"HTTP/"):
    raise SystemExit("plaintext request to TLS port must not get HTTP response")
PY

echo "[tls-itest] handshake idle timeout closes connection" >&2
python3 - "$HOST" "$PORT" <<'PY'
import socket, sys, time

host = sys.argv[1]
port = int(sys.argv[2])

with socket.create_connection((host, port), timeout=2.0) as s:
    time.sleep(1.1)
    s.settimeout(1.5)
    try:
        _ = s.recv(64)
    except (ConnectionResetError, BrokenPipeError):
        pass
    except socket.timeout:
        raise SystemExit("expected idle TLS handshake connection to change state on timeout")
PY

echo "[tls-itest] OK" >&2