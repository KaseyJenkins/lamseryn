#!/usr/bin/env python3

from __future__ import annotations

import argparse
import socket
import time


def _recv_exact(s: socket.socket, n: int, *, deadline: float) -> bytes:
    out = bytearray()
    while len(out) < n:
        now = time.time()
        if now >= deadline:
            raise TimeoutError("deadline while reading body")
        s.settimeout(max(0.05, min(0.25, deadline - now)))
        chunk = s.recv(n - len(out))
        if not chunk:
            raise ConnectionError("EOF while reading body")
        out += chunk
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Keep-alive idle gate: read full response, then expect server close after IDLE_CLOSE_MS")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--idle-ms", type=int, required=True, help="How long to wait for server-side keepalive idle close")
    ap.add_argument("--max-seconds", type=float, default=10.0)
    args = ap.parse_args()

    t0 = time.time()
    deadline = t0 + args.max_seconds

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    s.connect((args.host, args.port))

    req = b"GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"
    s.sendall(req)

    # Read headers
    buf = bytearray()
    while b"\r\n\r\n" not in buf:
        if time.time() >= deadline:
            print("TIMEOUT waiting headers")
            return 2
        s.settimeout(0.25)
        chunk = s.recv(4096)
        if not chunk:
            print("EOF")
            return 1
        buf += chunk

    head, rest = bytes(buf).split(b"\r\n\r\n", 1)
    # Parse Content-Length if present
    content_length = None
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            try:
                content_length = int(line.split(b":", 1)[1].strip())
            except ValueError:
                content_length = None

    # Drain body fully if we can; otherwise just drain what arrives quickly.
    if content_length is not None:
        if len(rest) < content_length:
            try:
                _recv_exact(s, content_length - len(rest), deadline=deadline)
            except Exception:
                print("TIMEOUT reading body")
                return 2

    # Now we are idle; expect server to close within idle-ms (+small slack).
    idle_deadline = time.time() + (args.idle_ms / 1000.0) + 0.75
    while True:
        now = time.time()
        if now >= idle_deadline:
            print("STILL_OPEN")
            return 1
        s.settimeout(max(0.05, min(0.25, idle_deadline - now)))
        try:
            chunk = s.recv(1)
            if chunk == b"":
                print("EOF")
                return 0
            # If server sends unexpected data after the response, treat as failure.
            print("DATA")
            return 1
        except socket.timeout:
            continue
        except ConnectionResetError:
            print("RST")
            return 0


if __name__ == "__main__":
    raise SystemExit(main())
