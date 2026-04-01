#!/usr/bin/env python3

from __future__ import annotations

import argparse
import socket
import time


def main() -> int:
    ap = argparse.ArgumentParser(description="Slowloris headers: drip bytes until server closes or replies")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--drip-ms", type=int, default=250, help="Delay between single-byte sends")
    ap.add_argument("--max-seconds", type=float, default=30.0)
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    t0 = time.time()
    s.connect((args.host, args.port))

    pre = b"GET / HTTP/1.1\r\nHost: x\r\n"
    s.sendall(pre)

    # Now drip bytes forever (\r, \n, header bytes) until close.
    payload = b"X"

    # Switch to small recv timeout so we can observe 408.
    s.settimeout(0.25)

    got = b""
    while True:
        if time.time() - t0 > args.max_seconds:
            print("TIMEOUT max-seconds")
            return 2

        try:
            chunk = s.recv(4096)
            if chunk:
                got += chunk
                # If we see a full status line, print it and exit.
                if b"\r\n" in got:
                    line = got.split(b"\r\n", 1)[0]
                    print(line.decode("latin-1", errors="replace"))
                    return 0
            else:
                print("EOF")
                return 0
        except socket.timeout:
            pass
        except ConnectionResetError:
            print("RST")
            return 0

        # Drip one byte
        try:
            s.sendall(payload)
        except (BrokenPipeError, ConnectionResetError):
            print("CLOSED")
            return 0

        time.sleep(args.drip_ms / 1000.0)


if __name__ == "__main__":
    raise SystemExit(main())
