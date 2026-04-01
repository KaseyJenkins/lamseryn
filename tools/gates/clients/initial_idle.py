#!/usr/bin/env python3

from __future__ import annotations

import argparse
import socket
import time


def main() -> int:
    ap = argparse.ArgumentParser(description="Initial idle: connect and send nothing")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--wait-ms", type=int, default=1500, help="How long to stay idle before probing")
    ap.add_argument("--max-seconds", type=float, default=10.0)
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.25)
    t0 = time.time()
    s.connect((args.host, args.port))

    # Stay idle, then probe by attempting a write.
    time.sleep(args.wait_ms / 1000.0)

    # Observe close without writing. Note that sending may still succeed after
    # a peer FIN, so write-probing is not a reliable indicator.
    deadline = time.time() + max(0.5, (args.max_seconds - (time.time() - t0)))
    while time.time() < deadline:
        try:
            chunk = s.recv(1)
            if not chunk:
                print("EOF")
                return 0
        except socket.timeout:
            pass
        except ConnectionResetError:
            print("RST")
            return 0

    print("STILL_OPEN")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
