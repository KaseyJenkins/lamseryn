#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  --sample N          access_log_sample value (default: 1)
  --workers N         workers in server config (default: 1)
  --conns N           benchmark client connections (default: 32)
  --duration N        per-scenario duration seconds (default: 4)
  --scenarios LIST    comma-separated scenarios (default: get_basic,get_pipeline4,get_pipeline16)
  --abba              run ABBA ordering instead of single A/B pass
  --repeats N         ABBA repeat count (default: 1)
  --port-base N       base port for file/null pair (default: 18196)
  --prefix PATH       output prefix path (default: /tmp/accesslog_fix)
  --server-cpu LIST   pin server process with taskset -c LIST (optional)
  --bench-cpu LIST    pin benchmark client with taskset -c LIST (optional)
  -h, --help          show this help

Outputs:
  "/tmp" files (or --prefix path) for both file/null runs:
  *_base.ini *_alog.ini *_base.out *_alog.out *_delta.csv *_base.log *_alog.log
  ABBA mode additionally emits: *_abba_raw.csv *_abba_summary.csv
EOF
}

sample=1
workers=1
conns=32
duration=4
scenarios="get_basic,get_pipeline4,get_pipeline16"
abba=0
repeats=1
port_base=18196
prefix="/tmp/accesslog_fix"
server_cpu=""
bench_cpu=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sample)
      sample="$2"
      shift 2
      ;;
    --workers)
      workers="$2"
      shift 2
      ;;
    --conns)
      conns="$2"
      shift 2
      ;;
    --duration)
      duration="$2"
      shift 2
      ;;
    --scenarios)
      scenarios="$2"
      shift 2
      ;;
    --abba)
      abba=1
      shift
      ;;
    --repeats)
      repeats="$2"
      shift 2
      ;;
    --port-base)
      port_base="$2"
      shift 2
      ;;
    --prefix)
      prefix="$2"
      shift 2
      ;;
    --server-cpu)
      server_cpu="$2"
      shift 2
      ;;
    --bench-cpu)
      bench_cpu="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"

if [[ -n "$server_cpu" || -n "$bench_cpu" ]]; then
  if ! command -v taskset >/dev/null 2>&1; then
    echo "taskset is required for --server-cpu/--bench-cpu but was not found" >&2
    exit 1
  fi
fi

run_pair() {
  label="$1"
  sink="$2"
  port="$3"

  docroot="${prefix}_${label}_docroot"
  base_ini="${prefix}_${label}_base.ini"
  alog_ini="${prefix}_${label}_alog.ini"
  base_out="${prefix}_${label}_base.out"
  alog_out="${prefix}_${label}_alog.out"
  delta="${prefix}_${label}_delta.csv"
  base_log="${prefix}_${label}_base.log"
  alog_log="${prefix}_${label}_alog.log"
  abba_raw="${prefix}_${label}_abba_raw.csv"
  abba_summary="${prefix}_${label}_abba_summary.csv"

  mkdir -p "$docroot"
  printf 'ok' > "$docroot/index.html"

  cat > "$base_ini" <<EOF
[globals]
log_level = info
workers = $workers
queue_depth = 4096
initial_idle_timeout_ms = 5000
keepalive_idle_close_ms = 10000
header_timeout_ms = 5000
body_timeout_ms = 5000
write_timeout_ms = 5000
drain_timeout_ms = 200
accept_backoff_ms = 10
access_log_enabled = false

[vhost default]
bind = 127.0.0.1
port = $port
docroot = $docroot
static = true
EOF

  cat > "$alog_ini" <<EOF
[globals]
log_level = info
workers = $workers
queue_depth = 4096
initial_idle_timeout_ms = 5000
keepalive_idle_close_ms = 10000
header_timeout_ms = 5000
body_timeout_ms = 5000
write_timeout_ms = 5000
drain_timeout_ms = 200
accept_backoff_ms = 10
access_log_enabled = true
access_log_path = $sink
access_log_format = text
access_log_sample = $sample
access_log_min_status = 100

[vhost default]
bind = 127.0.0.1
port = $port
docroot = $docroot
static = true
EOF

  run_case() {
    ini="$1"
    out="$2"
    slog="$3"

    if [[ -n "$server_cpu" ]]; then
      SERVER_CONFIG="$ini" taskset -c "$server_cpu" ./build/lamseryn >"$slog" 2>&1 &
    else
      SERVER_CONFIG="$ini" ./build/lamseryn >"$slog" 2>&1 &
    fi
    spid=$!

    for _ in $(seq 1 140); do
      if (exec 3<>/dev/tcp/127.0.0.1/$port) 2>/dev/null; then
        exec 3>&-
        exec 3<&-
        break
      fi
      sleep 0.05
    done

    if [[ -n "$bench_cpu" ]]; then
      taskset -c "$bench_cpu" python3 tools/bench/run_bench_sweep.py \
        --host 127.0.0.1 \
        --port "$port" \
        --conns "$conns" \
        --duration "$duration" \
        --scenarios "$scenarios" > "$out"
    else
      python3 tools/bench/run_bench_sweep.py \
        --host 127.0.0.1 \
        --port "$port" \
        --conns "$conns" \
        --duration "$duration" \
        --scenarios "$scenarios" > "$out"
    fi

    kill -TERM "$spid" 2>/dev/null || true
    wait "$spid" 2>/dev/null || true
  }

  append_case_rows() {
    mode="$1"
    run_idx="$2"
    out_csv="$3"
    emitted="$4"
    dropped="$5"
    write_err="$6"

    python3 - <<'PY' "$out_csv" "$abba_raw" "$mode" "$run_idx" "$emitted" "$dropped" "$write_err"
import sys
bench, raw, mode, run_idx, emitted, dropped, write_err = sys.argv[1:8]

with open(raw, 'a', encoding='utf-8') as dst:
    with open(bench, 'r', encoding='utf-8', errors='replace') as src:
        for line in src:
            if not line.startswith('get_'):
                continue
            n, rps, _done, _errs, p50, p95, p99 = line.strip().split(',')
            dst.write(f"{run_idx},{mode},{n},{rps},{p50},{p95},{p99},{emitted},{dropped},{write_err}\n")
PY
  }

  if [[ "$abba" == "1" ]]; then
    if ! [[ "$repeats" =~ ^[0-9]+$ ]] || [[ "$repeats" -lt 1 ]]; then
      echo "--repeats must be a positive integer" >&2
      exit 1
    fi

    echo 'run,mode,scenario,rps,p50,p95,p99,emitted,dropped,write_err' > "$abba_raw"

    run_abba_case() {
      mode="$1"
      run_idx="$2"
      out_file="${prefix}_${label}_run${run_idx}_${mode}.out"
      log_file="${prefix}_${label}_run${run_idx}_${mode}.log"

      if [[ "$mode" == "A" ]]; then
        run_case "$base_ini" "$out_file" "$log_file"
        append_case_rows "$mode" "$run_idx" "$out_file" 0 0 0
      else
        run_case "$alog_ini" "$out_file" "$log_file"
        em=$(grep 'access log counters:' "$log_file" | sed -E 's/.*emitted=([0-9]+).*/\1/' | tail -n 1 || true)
        dr=$(grep 'access log counters:' "$log_file" | sed -E 's/.*dropped=([0-9]+).*/\1/' | tail -n 1 || true)
        we=$(grep 'access log counters:' "$log_file" | sed -E 's/.*write_err=([0-9]+).*/\1/' | tail -n 1 || true)
        em=${em:-0}
        dr=${dr:-0}
        we=${we:-0}
        append_case_rows "$mode" "$run_idx" "$out_file" "$em" "$dr" "$we"
      fi
    }

    for r in $(seq 1 "$repeats"); do
      run_abba_case A "$r"
      run_abba_case B "$r"
      run_abba_case B "$r"
      run_abba_case A "$r"
    done

    python3 - <<'PY' "$abba_raw" "$abba_summary" "$scenarios"
import csv
import math
import sys

raw_path, summary_path, scenarios = sys.argv[1], sys.argv[2], sys.argv[3]
scenario_order = [s.strip() for s in scenarios.split(',') if s.strip()]
rows = list(csv.DictReader(open(raw_path, 'r', encoding='utf-8', errors='replace')))

def stats(vals):
    n = len(vals)
    if n == 0:
        return 0.0, 0.0
    mean = sum(vals) / n
    var = sum((x - mean) ** 2 for x in vals) / (n - 1) if n > 1 else 0.0
    return mean, math.sqrt(var)

def pct(a, b):
    return 0.0 if a == 0 else (b - a) / a * 100.0

with open(summary_path, 'w', encoding='utf-8') as f:
    f.write('scenario,base_mean_rps,base_std_rps,alog_mean_rps,alog_std_rps,rps_delta_pct,base_mean_p50,alog_mean_p50,p50_delta_pct,base_mean_p95,alog_mean_p95,p95_delta_pct,base_mean_p99,alog_mean_p99,p99_delta_pct,alog_emitted_sum,alog_dropped_sum,alog_write_err_sum\n')
    for s in scenario_order:
        base = [r for r in rows if r['scenario'] == s and r['mode'] == 'A']
        alog = [r for r in rows if r['scenario'] == s and r['mode'] == 'B']
        if not base or not alog:
            continue

        brps = [float(r['rps']) for r in base]
        arps = [float(r['rps']) for r in alog]
        bp50 = [float(r['p50']) for r in base]
        ap50 = [float(r['p50']) for r in alog]
        bp95 = [float(r['p95']) for r in base]
        ap95 = [float(r['p95']) for r in alog]
        bp99 = [float(r['p99']) for r in base]
        ap99 = [float(r['p99']) for r in alog]

        brm, brs = stats(brps)
        arm, ars = stats(arps)
        b50m, _ = stats(bp50)
        a50m, _ = stats(ap50)
        b95m, _ = stats(bp95)
        a95m, _ = stats(ap95)
        b99m, _ = stats(bp99)
        a99m, _ = stats(ap99)

        em = sum(int(r['emitted']) for r in alog)
        dr = sum(int(r['dropped']) for r in alog)
        we = sum(int(r['write_err']) for r in alog)

        f.write(
            f"{s},{brm:.2f},{brs:.2f},{arm:.2f},{ars:.2f},{pct(brm, arm):.2f},"
            f"{b50m:.2f},{a50m:.2f},{pct(b50m, a50m):.2f},"
            f"{b95m:.2f},{a95m:.2f},{pct(b95m, a95m):.2f},"
            f"{b99m:.2f},{a99m:.2f},{pct(b99m, a99m):.2f},"
            f"{em},{dr},{we}\n"
        )

print(open(summary_path, 'r', encoding='utf-8').read())
PY

    echo "--- ${label} ABBA raw ---"
    echo "$abba_raw"
    return
  fi

  run_case "$base_ini" "$base_out" "$base_log"
  run_case "$alog_ini" "$alog_out" "$alog_log"

  python3 - <<'PY' "$base_out" "$alog_out" "$delta" "$scenarios"
import sys
base, alog, out, scenarios = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

scenario_order = [s.strip() for s in scenarios.split(',') if s.strip()]

def parse(path):
    rows = {}
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('get_'):
                n, rps, done, errs, p50, p95, p99 = line.strip().split(',')
                rows[n] = {
                    'rps': float(rps),
                    'p50': float(p50),
                    'p95': float(p95),
                    'p99': float(p99),
                }
    return rows

b = parse(base)
a = parse(alog)

def pct(x, y):
    return 0.0 if x == 0 else (y - x) / x * 100.0

with open(out, 'w', encoding='utf-8') as f:
    f.write('scenario,base_rps,alog_rps,rps_delta_pct,base_p50,alog_p50,p50_delta_pct,base_p95,alog_p95,p95_delta_pct,base_p99,alog_p99,p99_delta_pct\n')
    for s in scenario_order:
        if s not in b or s not in a:
            continue
        br, ar = b[s], a[s]
        f.write(
            f"{s},{br['rps']:.2f},{ar['rps']:.2f},{pct(br['rps'], ar['rps']):.2f},"
            f"{br['p50']:.2f},{ar['p50']:.2f},{pct(br['p50'], ar['p50']):.2f},"
            f"{br['p95']:.2f},{ar['p95']:.2f},{pct(br['p95'], ar['p95']):.2f},"
            f"{br['p99']:.2f},{ar['p99']:.2f},{pct(br['p99'], ar['p99']):.2f}\n"
        )

print(open(out, 'r', encoding='utf-8').read())
PY

  echo "--- ${label} counters ---"
  grep 'access log counters:' "$alog_log" | tail -n 1 || true
}

run_pair file "${prefix}_file_sink.log" "$port_base"
run_pair null /dev/null "$((port_base + 1))"
