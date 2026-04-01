#!/usr/bin/env python3

from __future__ import annotations

import argparse
from pathlib import Path

from lib.profile import get_profile, defs_for_make


def main() -> int:
    ap = argparse.ArgumentParser(description="Emit -D flags for a gates build profile")
    ap.add_argument("--profiles", default=str(Path(__file__).with_name("profiles.yaml")))
    ap.add_argument("--profile", required=True, help="Profile name (e.g. ci-fast, staging, prod)")
    args = ap.parse_args()

    p = get_profile(Path(args.profiles), args.profile)

    # Only emit flags that are currently wired as build-time macros.
    allowlist = (
        "INITIAL_IDLE_TIMEOUT_MS",
        "HEADER_TIMEOUT_MS",
        "BODY_TIMEOUT_MS",
        "IDLE_CLOSE_MS",
        "WRITE_TIMEOUT_MS",
        "ACCEPT_BACKOFF_MS",
        "MAX_BODY_BYTES",
        "TW_TICK_MS",
        "TW_SLOTS",
    )

    print(defs_for_make(p, allowlist=allowlist))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
