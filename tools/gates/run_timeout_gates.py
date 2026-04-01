#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from lib.log_parse import parse_thread_counters, get_int
from lib.profile import get_profile


def _run(cmd: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(cwd) if cwd else None, env=env, check=False, text=True, capture_output=True)


def _build(repo: Path, profile_name: str, profiles_path: Path) -> Path:
    profile = get_profile(profiles_path, profile_name)

    # Apply build-time knobs from profile values.
    defs = []
    for k in (
        "INITIAL_IDLE_TIMEOUT_MS",
        "HEADER_TIMEOUT_MS",
        "BODY_TIMEOUT_MS",
        "IDLE_CLOSE_MS",
        "WRITE_TIMEOUT_MS",
        "ACCEPT_BACKOFF_MS",
    ):
        if k in profile.values:
            defs.append(f"-D{k}={profile.values[k]}")

    make = os.environ.get("MAKE", "make")
    # Pass APP_DEFS via environment to avoid argv/whitespace parsing issues.
    env = dict(os.environ)
    env["APP_DEFS"] = " ".join(defs)
    cmd = [make, "gates"]
    p = _run(cmd, cwd=repo, env=env)
    if p.returncode != 0:
        sys.stderr.write(p.stdout)
        sys.stderr.write(p.stderr)
        raise SystemExit(f"build failed: {' '.join(cmd)}")

    return repo / "build" / "lamseryn_gates"


def _write_ini(profile, port: int, workers: int, docroot_dir: Path, ini_path: Path) -> None:
    vals = profile.values

    def v(name: str, default: int) -> int:
        return int(vals.get(name, default))

    # Minimal INI with globals and one static-serving vhost.
    ini = f"""[globals]
log_level = info
log_categories = core,accept,timer,http,io
workers = {workers}
initial_idle_timeout_ms = {v("INITIAL_IDLE_TIMEOUT_MS", 1000)}
keepalive_idle_close_ms = {v("IDLE_CLOSE_MS", 5000)}
header_timeout_ms = {v("HEADER_TIMEOUT_MS", 30000)}
body_timeout_ms = {v("BODY_TIMEOUT_MS", 30000)}
write_timeout_ms = {v("WRITE_TIMEOUT_MS", 10000)}
drain_timeout_ms = {v("DRAIN_TIMEOUT_MS", 2000)}
accept_backoff_ms = {v("ACCEPT_BACKOFF_MS", 5)}

[vhost default]
bind = 0.0.0.0
port = {port}
docroot = {docroot_dir}
static = true
"""
    ini_path.write_text(ini, encoding="utf-8")


def _start_server(server: Path, *, threads: int, log_path: Path, ini_path: Path) -> tuple[subprocess.Popen, object]:
    env = dict(os.environ)
    env.setdefault("LOG_LEVEL", "info")
    env.setdefault("LOG_CATS", "core,accept,timer,http,io")
    # Keep accept() immediate so INITIAL_IDLE timing remains deterministic.
    env.setdefault("TCP_DEFER_ACCEPT_SEC", "0")
    env["SERVER_CONFIG"] = str(ini_path)

    # Route server stdout/stderr into one file for parsing.
    logf = open(log_path, "w", encoding="utf-8")
    p = subprocess.Popen(
        [str(server)],
        stdout=logf,
        stderr=subprocess.STDOUT,
        env=env,
        text=True,
    )

    deadline = time.time() + 3.0
    while time.time() < deadline:
        time.sleep(0.05)
        if p.poll() is not None:
            raise SystemExit(f"server exited early (rc={p.returncode}). See {log_path}")
        try:
            txt = log_path.read_text(encoding="utf-8", errors="replace")
            if "listening on" in txt:
                return p, logf
        except FileNotFoundError:
            pass

    return p, logf


def _stop_server(p: subprocess.Popen, *, timeout_s: float = 3.0) -> None:
    if p.poll() is not None:
        return
    p.send_signal(signal.SIGINT)
    try:
        p.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait(timeout=timeout_s)


def _parse_probe_rst_ms(output: str) -> tuple[bool, int | None]:
    """
    Return (saw_rst, ms) parsed from slow_reader_probe output.
    Accepts lines like:
      RESULT: RST after 6078 ms
      RESULT: RST/CLOSE after 6060 ms
    """
    out = output.strip()
    saw_rst = "RST" in out.upper()
    m = re.search(r'after\s+(\d+)\s*ms', out, re.IGNORECASE)
    ms = int(m.group(1)) if m else None
    return saw_rst, ms


def main() -> int:
    ap = argparse.ArgumentParser(description="Run a small subset of ship-room timeout gates")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument("--profiles", default=str(Path(__file__).with_name("profiles.yaml")))
    ap.add_argument("--profile", default="ci-fast")
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--docroot", default=str(Path("tests/integration/wwwroot")))
    ap.add_argument("--drip-ms", type=int, default=250)
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    profiles_path = Path(args.profiles).resolve()

    profile = get_profile(profiles_path, args.profile)

    server = _build(repo, args.profile, profiles_path)
    if not server.exists():
        raise SystemExit(f"server binary not found: {server}")

    with tempfile.TemporaryDirectory(prefix="gates-") as td:
        td_path = Path(td)
        log_path = td_path / "server.log"
        docroot_dir = td_path / "docroot"
        docroot_dir.mkdir(parents=True, exist_ok=True)

        # Small index for the keep-alive idle gate.
        (docroot_dir / "index.html").write_text(
            "<html><body>ok</body></html>\n",
            encoding="utf-8",
        )

        # Large file to trigger TCP backpressure reliably.
        big_path = docroot_dir / "index_big.html"
        big_size = int(profile.values.get("SLOW_READER_FILE_BYTES", 32 * 1024 * 1024))
        with big_path.open("wb") as f:
            f.truncate(big_size)

        # Generate INI for the requested port.
        ini_path = td_path / "server.ini"
        _write_ini(profile, args.port, args.threads, docroot_dir, ini_path)

        p, logf = _start_server(server, threads=args.threads, log_path=log_path, ini_path=ini_path)
        try:
            # Gate 1: initial idle.
            wait_ms = int(profile.values.get("INITIAL_IDLE_TIMEOUT_MS", 1000)) + 500
            c1 = _run([
                sys.executable,
                str(repo / "tools/gates/clients/initial_idle.py"),
                "--port", str(args.port),
                "--wait-ms", str(wait_ms),
                "--max-seconds", "5",
            ])

            # Gate 2: keep-alive idle.
            idle_ms = int(profile.values.get("IDLE_CLOSE_MS", 5000))
            cka = _run([
                sys.executable,
                str(repo / "tools/gates/clients/keepalive_idle.py"),
                "--port", str(args.port),
                "--idle-ms", str(idle_ms),
                "--max-seconds", "15",
            ])

            # Gate 3: slowloris headers.
            c2 = _run([
                sys.executable,
                str(repo / "tools/gates/clients/slowloris_headers.py"),
                "--port", str(args.port),
                "--drip-ms", str(args.drip_ms),
                "--max-seconds", "15",
            ])

            # Gate 4: slow reader/write-side backpressure (assert RST delivery).
            write_ms = int(profile.values.get("WRITE_TIMEOUT_MS", 10000))
            c3 = _run([
                sys.executable,
                str(repo / "tools/gates/clients/slow_reader_probe.py"),
                "--port", str(args.port),
                "--path", "/index_big.html",
                "--rcvbuf", "512",
                "--max-seconds", str(int((write_ms / 1000.0) + 5.0)),
            ])

        finally:
            _stop_server(p)
            try:
                logf.flush()
            finally:
                logf.close()

        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        counters = parse_thread_counters(log_text)

        # Print client outputs.
        print("initial_idle:", c1.stdout.strip() or c1.stderr.strip())
        print("keepalive_idle:", cka.stdout.strip() or cka.stderr.strip())
        print("slowloris_headers:", c2.stdout.strip() or c2.stderr.strip())
        print("slow_reader:", c3.stdout.strip() or c3.stderr.strip())

        # Counters used by gate checks.
        cnt_408 = get_int(counters, "408", 0)
        sqe_starve_close = get_int(counters, "sqe_starve_close", 0)
        accept_ok = get_int(counters, "accept_ok", 0)
        accept_err = get_int(counters, "accept_err", 0)
        accept_emfile = get_int(counters, "emfile", 0)
        accept_enfile = get_int(counters, "enfile", 0)

        print(f"counters: 408={cnt_408} sqe_starve_close={sqe_starve_close}")
        print(f"counters: accept_ok={accept_ok} accept_err={accept_err} emfile={accept_emfile} enfile={accept_enfile}")

        # Hard CI-style failures, kept minimal to avoid flakiness.
        if c1.returncode != 0:
            print("FAIL: initial_idle client timed out", file=sys.stderr)
            return 1
        if cka.returncode != 0:
            print("FAIL: keepalive_idle did not close on schedule", file=sys.stderr)
            return 1
        if c2.returncode != 0:
            print("FAIL: slowloris client timed out", file=sys.stderr)
            return 1

        # Require an actual RST observed by the probe.
        sr_out = (c3.stdout or "") + (c3.stderr or "")
        saw_rst, rst_ms = _parse_probe_rst_ms(sr_out)
        if not saw_rst:
            print("FAIL: slow_reader_probe did not observe an RST (abortive close)", file=sys.stderr)
            return 1

        # Jitter tolerances can be overridden in the profile.
        tol_neg = int(profile.values.get("WRITE_TIMEOUT_TOL_NEG_MS", 1000))
        tol_pos = int(profile.values.get("WRITE_TIMEOUT_TOL_POS_MS", 3000))
        if rst_ms is not None:
            lo = max(0, write_ms - tol_neg)
            hi = write_ms + tol_pos
            if not (lo <= rst_ms <= hi):
                print(
                    f"FAIL: RST after {rst_ms} ms outside expected window [{lo}, {hi}] "
                    f"(WRITE_TIMEOUT_MS={write_ms})",
                    file=sys.stderr,
                )
                return 1

        # Under benign load, expect at least one 408 if timeout wiring is active.
        if cnt_408 <= 0:
            print("FAIL: expected cnt_408 >= 1 (header/body timeout staging)", file=sys.stderr)
            return 1

        return 0


if __name__ == "__main__":
    raise SystemExit(main())
