#!/usr/bin/env python3
"""
slow_reader_probe.py
Python replacement for the C slow_reader_probe. It:
- Opens a TCP connection, GETs a large path, and intentionally does not read,
  creating write-side backpressure on the server.
- Detects abortive close via SO_ERROR polling and/or by forcing a write.
- Optionally attempts a second request on the same socket (try-reuse) to surface RST.
Outputs one line like:
  RESULT: RST after <ms> ms
or
  RESULT: NO RESPONSE
"""

from __future__ import annotations

import argparse
import errno
import socket
import sys
import time


def _now_ms() -> int:
    return int(time.monotonic() * 1000)


def _make_request(host: str, path: str, keepalive: bool = True) -> bytes:
    conn_hdr = "keep-alive" if keepalive else "close"
    return (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"Connection: {conn_hdr}\r\n"
        f"\r\n"
    ).encode("ascii")


def probe(
    host: str,
    port: int,
    path: str,
    rcvbuf: int,
    max_seconds: int,
    try_reuse: bool,
    reuse_wait_ms: int,
    soerr_window_ms: int,
    poll_interval_ms: int,
    force_surface: bool,
) -> None:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        # Small receive buffer to hit backpressure faster (disables autotune)
        if rcvbuf > 0:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        # Disable SIGPIPE delivery (Python already turns into BrokenPipeError)
        # Keep default blocking mode
        s.connect((host, port))

        # Send initial request
        req = _make_request(host, path, keepalive=True)
        s.sendall(req)

        t0 = _now_ms()
        deadline = t0 + max_seconds * 1000
        reuse_deadline = t0 + reuse_wait_ms if try_reuse and reuse_wait_ms > 0 else None
        did_reuse_try = False

        # Backpressure: do not read. Just poll SO_ERROR and optionally poke writes.
        while _now_ms() < deadline:
            now = _now_ms()

            # 1) Check SO_ERROR for pending RST
            err = s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if err:
                if err == errno.ECONNRESET:
                    print(f"RESULT: RST after {now - t0} ms")
                    return
                # Other errors -> treat as close/error, but keep trying to surface RST via write
                # fall through

            # 2) Optionally force-surface by writing a byte
            if force_surface:
                try:
                    s.send(b"X")
                except (BrokenPipeError, ConnectionResetError, OSError) as e:
                    # ECONNRESET/EPIPE both indicate the peer is gone (abortive/graceful)
                    print(f"RESULT: RST after {now - t0} ms")
                    return

            # 3) Optional reuse attempt: send a second request on same socket
            if try_reuse and reuse_deadline and not did_reuse_try and now >= reuse_deadline:
                try:
                    s.sendall(_make_request(host, path, keepalive=True))
                    did_reuse_try = True
                except (BrokenPipeError, ConnectionResetError, OSError):
                    print(f"RESULT: RST after {now - t0} ms")
                    return

            # 4) Passive wait, then loop
            time.sleep(max(poll_interval_ms, 1) / 1000.0)

        print("RESULT: NO RESPONSE")
    finally:
        try:
            s.close()
        except Exception:
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description="Slow reader probe (Python)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--path", default="/index_big.html")
    ap.add_argument("--rcvbuf", type=int, default=512)
    ap.add_argument("--max-seconds", type=int, default=8)
    ap.add_argument("--try-reuse", action="store_true")
    ap.add_argument("--reuse-wait-ms", type=int, default=5000)
    ap.add_argument("--soerr-window-ms", type=int, default=1500)  # kept for CLI parity
    ap.add_argument("--poll-interval-ms", type=int, default=100)
    ap.add_argument("--force-surface", action="store_true")
    args = ap.parse_args()

    probe(
        host=args.host,
        port=args.port,
        path=args.path,
        rcvbuf=args.rcvbuf,
        max_seconds=args.max_seconds,
        try_reuse=args.try_reuse,
        reuse_wait_ms=args.reuse_wait_ms,
        soerr_window_ms=args.soerr_window_ms,
        poll_interval_ms=args.poll_interval_ms,
        force_surface=args.force_surface,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
