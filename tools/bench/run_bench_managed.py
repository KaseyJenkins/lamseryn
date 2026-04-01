#!/usr/bin/env python3
"""
Managed bench sweep: build server/client, launch server with a temp ini/docroot,
run bench_client scenarios, then tear everything down.
"""
from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Reuse helpers from the sweep script
sys.path.append(str(Path(__file__).parent))  # allow sibling import
from run_bench_sweep import (  # type: ignore
    DEFAULT_SCENARIOS,
    Scenario,
    build_bench,
    parse_summary,
    run_bench,
)


def wait_for_listen(host: str, port: int, timeout_s: float = 5.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def write_ini(docroot: Path, port: int, workers: int) -> str:
    ini = f"""[globals]
log_level = info
log_categories = core,accept,timer,http,io
workers = {workers}
initial_idle_timeout_ms = 1000
keepalive_idle_close_ms = 10000
header_timeout_ms = 5000
body_timeout_ms = 5000
write_timeout_ms = 1500

[vhost default]
bind = 0.0.0.0
port = {port}
docroot = {docroot}
static = true
"""
    return ini


def build_server(repo: Path) -> Path:
    make = os.environ.get("MAKE", "make")
    cmd = [make, "gates"]
    proc = subprocess.run(cmd, cwd=repo, text=True, capture_output=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"gates build failed: {' '.join(cmd)}")
    return repo / "build" / "lamseryn_gates"


def start_server(server_bin: Path, threads: int, ini_path: Path, env: dict[str, str]) -> tuple[subprocess.Popen, Path]:
    env = dict(env)
    env.setdefault("LOG_LEVEL", "info")
    env.setdefault("LOG_CATS", "core,http,io")
    env["SERVER_CONFIG"] = str(ini_path)
    log_path = ini_path.parent / "server.log"
    logf = open(log_path, "w", encoding="utf-8")
    p = subprocess.Popen([str(server_bin)], stdout=logf, stderr=subprocess.STDOUT, env=env, text=True)
    return p, log_path


def stop_server(p: subprocess.Popen, grace_s: float = 3.0) -> None:
    if p.poll() is not None:
        return
    p.terminate()
    try:
        p.wait(timeout=grace_s)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait(timeout=grace_s)


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    default_server = repo / "build" / "lamseryn_gates"
    default_client = repo / "build" / "bench_client"

    # Extend scenarios with a big-file fetch.
    extra_scenarios = [Scenario(name="get_big", path="/big.bin", pipeline=1)]

    ap = argparse.ArgumentParser(description="Managed bench sweep (build, launch server, run bench_client)")
    ap.add_argument("--host", default="127.0.0.1", help="Host for bench_client to hit")
    ap.add_argument("--port", type=int, default=18081, help="Port for server and bench_client")
    ap.add_argument("--threads", type=int, default=1, help="Server threads")
    ap.add_argument("--conns", type=int, default=64, help="Connections for bench_client")
    ap.add_argument("--duration", type=int, default=20, help="Seconds per scenario")
    ap.add_argument("--warmup-seconds", type=int, default=1, help="Sleep before running scenarios")
    ap.add_argument("--path", default="/", help="Default path for scenarios that do not override")
    ap.add_argument("--scenarios", default="all", help="Comma list or 'all'")
    ap.add_argument("--allow-errors", action="store_true", help="Do not fail if errors > 0")
    ap.add_argument("--build", action="store_true", help="Run make gates/bench if binaries are missing")
    ap.add_argument("--keep-temp", action="store_true", help="Keep temp dir (docroot/logs) for debugging")
    ap.add_argument("--tail-on-error", action="store_true", default=True, help="Print server log tail when errors occur")
    ap.add_argument("--small-bytes", type=int, default=4096, help="Size of small index.html payload")
    ap.add_argument("--large-bytes", type=int, default=4 * 1024 * 1024, help="Size of large big.bin payload")
    ap.add_argument("--server-bin", default=str(default_server), help="Path to server binary")
    ap.add_argument("--bench-bin", default=str(default_client), help="Path to bench_client binary")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[], help="Extra args passed to bench_client")
    args = ap.parse_args()

    repo_env = os.environ
    server_bin = Path(args.server_bin)
    bench_bin = Path(args.bench_bin)

    if args.build:
        if not server_bin.exists():
            server_bin = build_server(repo)
        if not bench_bin.exists():
            build_bench(repo)
    if not server_bin.exists():
        raise SystemExit(f"server binary not found: {server_bin}")
    if not bench_bin.exists():
        raise SystemExit(f"bench_client not found: {bench_bin}")

    scenario_map = {s.name: s for s in DEFAULT_SCENARIOS + extra_scenarios}
    if args.scenarios != "all":
        wanted = [x.strip() for x in args.scenarios.split(",") if x.strip()]
        missing = [w for w in wanted if w not in scenario_map]
        if missing:
            raise SystemExit(f"unknown scenarios: {', '.join(missing)}")
        scenarios = [scenario_map[w] for w in wanted]
    else:
        scenarios = list(scenario_map.values())

    results = []
    rc_overall = 0

    tmp_ctx = tempfile.TemporaryDirectory(prefix="bench-managed-") if not args.keep_temp else None
    tmpdir = Path(tmp_ctx.name if tmp_ctx else tempfile.mkdtemp(prefix="bench-managed-"))
    docroot = tmpdir / "docroot"
    docroot.mkdir(parents=True, exist_ok=True)
    (docroot / "index.html").write_bytes(b"A" * max(1, args.small_bytes))
    with open(docroot / "big.bin", "wb") as f:
        f.truncate(args.large_bytes)

    ini_path = tmpdir / "server.ini"
    ini_path.write_text(write_ini(docroot, args.port, args.threads), encoding="utf-8")

    p, log_path = start_server(server_bin, args.threads, ini_path, repo_env)
    try:
        print(f"[bench] tempdir={tmpdir}")
        print(f"[bench] starting server {server_bin} threads={args.threads} port={args.port}")
        if not wait_for_listen(args.host, args.port, timeout_s=5.0):
            stop_server(p)
            if log_path.exists():
                sys.stderr.write(log_path.read_text(encoding="utf-8", errors="replace"))
            raise SystemExit(f"server did not start listening on {args.host}:{args.port}")

        if args.warmup_seconds > 0:
            print(f"[bench] warmup sleeping {args.warmup_seconds}s before scenarios")
            time.sleep(args.warmup_seconds)

        for scen in scenarios:
            path = scen.path if scen.path != "/" else args.path
            run_scen = Scenario(name=scen.name, method=scen.method, path=path, pipeline=scen.pipeline, body_bytes=scen.body_bytes)
            extra = list(args.extra)
            if args.warmup_seconds > 0:
                # pass a small initial stagger to reduce mid-write closes on fresh conns
                extra.extend(["-S", "2"])
            print(f"[bench] running {run_scen.name} method={run_scen.method} path={run_scen.path} pipeline={run_scen.pipeline} duration={args.duration}s conns={args.conns}")
            code, out, err = run_bench(bench_bin, args.host, args.port, args.conns, args.duration, run_scen, extra)
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
    finally:
        stop_server(p)

    if rc_overall != 0 and args.tail_on_error and log_path.exists():
        print("[bench] server log tail:")
        lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        for line in lines[-80:]:
            print(line)

    print("scenario,rps,done,errors,p50_ms,p95_ms,p99_ms")
    for name, summary, raw in results:
        if summary is None:
            print(f"{name},ERROR,?,?", end="")
            if raw:
                print(f" ({raw})")
            else:
                print()
            continue
        print(f"{name},{summary['rps']:.2f},{summary['done']},{summary['errors']},{summary['p50_ms']:.2f},{summary['p95_ms']:.2f},{summary['p99_ms']:.2f}")

    if tmp_ctx:
        tmp_ctx.cleanup()

    return rc_overall


if __name__ == "__main__":
    raise SystemExit(main())
