#!/usr/bin/env python3
"""
Automated sweep for bench_client scenarios.
Runs a small matrix of HTTP/1.1 requests against a running server and
prints a compact summary table (rps, p50/p95/p99, errors).
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List


@dataclass
class Scenario:
    name: str
    method: str = "GET"
    path: str = "/"
    pipeline: int = 1
    body_bytes: int = 0


DEFAULT_SCENARIOS: List[Scenario] = [
    Scenario(name="get_basic", pipeline=1),
    Scenario(name="get_pipeline4", pipeline=4),
    Scenario(name="get_pipeline16", pipeline=16),
]


def parse_summary(output: str) -> dict | None:
    sent_line = re.search(r"sent=(\d+)\s+done=(\d+)\s+errors=(\d+)\s+bytes=(\d+)", output)
    rps_line = re.search(r"rps=([0-9.]+)\s+p50=([0-9.]+)ms\s+p95=([0-9.]+)ms\s+p99=([0-9.]+)ms", output)
    if not sent_line or not rps_line:
        return None
    return {
        "sent": int(sent_line.group(1)),
        "done": int(sent_line.group(2)),
        "errors": int(sent_line.group(3)),
        "bytes": int(sent_line.group(4)),
        "rps": float(rps_line.group(1)),
        "p50_ms": float(rps_line.group(2)),
        "p95_ms": float(rps_line.group(3)),
        "p99_ms": float(rps_line.group(4)),
    }


def run_bench(bin_path: Path, host: str, port: int, conns: int, duration: int, scenario: Scenario, extra_args: list[str]) -> tuple[int, str, str]:
    cmd = [
        str(bin_path),
        "-h",
        host,
        "-p",
        str(port),
        "-c",
        str(conns),
        "-d",
        str(duration),
        "-M",
        scenario.method,
        "-P",
        str(scenario.pipeline),
        "-B",
        str(scenario.body_bytes),
        scenario.path,
    ]
    cmd.extend(extra_args)
    proc = subprocess.run(cmd, text=True, capture_output=True)
    return proc.returncode, proc.stdout, proc.stderr


def build_bench(repo: Path) -> None:
    make = os.environ.get("MAKE", "make")
    cmd = [make, "bench"]
    proc = subprocess.run(cmd, cwd=repo, text=True, capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"bench build failed: {' '.join(cmd)}")


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    bin_path = repo / "build" / "bench_client"

    ap = argparse.ArgumentParser(description="Run a sweep of bench_client scenarios")
    ap.add_argument("--host", default="127.0.0.1", help="Target host")
    ap.add_argument("--port", type=int, default=8080, help="Target port")
    ap.add_argument("--path", default="/", help="Request path (applied to scenarios without overrides)")
    ap.add_argument("--conns", type=int, default=64, help="Connections")
    ap.add_argument("--duration", type=int, default=20, help="Seconds per scenario")
    ap.add_argument("--scenarios", default="all", help="Comma list of scenario names or 'all'")
    ap.add_argument("--allow-errors", action="store_true", help="Do not fail if errors > 0")
    ap.add_argument("--build", action="store_true", help="Run 'make bench' if bench_client is missing")
    ap.add_argument("--bench-bin", default=str(bin_path), help="Path to bench_client binary")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[], help="Extra args passed through to bench_client")
    args = ap.parse_args()

    bin_path = Path(args.bench_bin)
    if not bin_path.exists():
        if args.build:
            build_bench(repo)
        else:
            raise SystemExit(f"bench_client not found at {bin_path}; build it or use --build")

    scenario_map = {s.name: s for s in DEFAULT_SCENARIOS}
    if args.scenarios != "all":
        wanted = {name.strip() for name in args.scenarios.split(",") if name.strip()}
        missing = [w for w in wanted if w not in scenario_map]
        if missing:
            raise SystemExit(f"unknown scenarios: {', '.join(sorted(missing))}")
        scenarios: Iterable[Scenario] = [scenario_map[w] for w in wanted]
    else:
        scenarios = DEFAULT_SCENARIOS

    results = []
    rc_overall = 0

    for scen in scenarios:
        path = scen.path if scen.path != "/" else args.path
        run_scen = Scenario(
            name=scen.name,
            method=scen.method,
            path=path,
            pipeline=scen.pipeline,
            body_bytes=scen.body_bytes,
        )
        print(f"[bench] running {run_scen.name} method={run_scen.method} path={run_scen.path} pipeline={run_scen.pipeline} duration={args.duration}s conns={args.conns}")
        code, out, err = run_bench(bin_path, args.host, args.port, args.conns, args.duration, run_scen, args.extra)
        combined = out + err
        summary = parse_summary(combined)
        if summary is None:
            rc_overall = rc_overall or 1
            results.append((run_scen.name, None, combined.strip()))
            if combined.strip():
                print(f"[bench] {run_scen.name} output:\n{combined.strip()}")
            continue
        results.append((run_scen.name, summary, combined.strip()))
        if code != 0:
            rc_overall = rc_overall or code
        if not args.allow_errors and summary["errors"] > 0:
            rc_overall = rc_overall or 1
        if summary["errors"] > 0:
            print(f"[bench] {run_scen.name} reported errors={summary['errors']}")
            if combined.strip():
                print(f"[bench] {run_scen.name} bench output:\n{combined.strip()}")

    print("scenario,rps,done,errors,p50_ms,p95_ms,p99_ms")
    for name, summary, raw in results:
        if summary is None:
            print(f"{name},ERROR,?,?", end="")
            if raw:
                print(f" ({raw})")
            else:
                print()
            continue
        print(
            f"{name},{summary['rps']:.2f},{summary['done']},{summary['errors']}"
            f",{summary['p50_ms']:.2f},{summary['p95_ms']:.2f},{summary['p99_ms']:.2f}"
        )
    return rc_overall


if __name__ == "__main__":
    raise SystemExit(main())
