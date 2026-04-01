# Logging and Instrumentation Knobs

Last updated: 2026-03-20

## Purpose

This document explains how logging and counter instrumentation are controlled:

- which knobs are compile-time vs runtime,
- precedence rules,
- and how to use convenience build targets.

## Big Picture

There are two independent systems:

1. Logging
- controls what log lines can exist in the binary and what is emitted at runtime.

2. Instrumentation counters
- controls which counters exist in the worker stats struct and which increments compile to no-op.

Both are designed as two-stage controls:

- Compile-time ceiling/level: determines what is compiled in.
- Runtime selection: determines what is emitted/used from what is compiled in.

## Logging Architecture

### Compile-time logging ceiling

Primary knob:

- `LOG_LEVEL_CEILING` in top-level `Makefile`.

How it is wired:

- The Makefile passes the ceiling as a compile definition.
- Any log call above the compile ceiling is compiled out entirely.

Implication:

- If ceiling is `LOG_INFO`, debug and trace log statements do not survive into the binary.

### Diagnostic stats coupling

Some internal diagnostic counters (request arena stats, connection freelist stats) are
automatically enabled when the compile ceiling is `LOG_DEBUG` or higher, and disabled
for lower ceilings. This keeps production builds lean without manual flag management.

Explicit overrides remain available via `-DREQ_ARENA_STATS=0|1` and
`-DCONN_FREELIST_STATS=0|1` if needed.

### Runtime log selection

At runtime, the log level and category mask can be set from two sources:

1. Config file: `[globals] log_level` and `[globals] log_categories` in `server.ini`.
2. Environment variables: `LOG_LEVEL` and `LOG_CATS`.

Environment variables override config file values if both are present.

Effective log emission requires both the compile ceiling and the runtime filter to allow the message.

### Log levels

Available levels, from most to least severe:

- `LOGE` — errors requiring operator attention
- `LOGW` — warnings for degraded but non-fatal conditions
- `LOGI` — important lifecycle milestones (startup, shutdown, config changes)
- `LOGD` — development diagnostics
- `LOGT` — high-volume trace output

Rate-limited variants (`*_RL`, `*_EVERY_N`) are available for high-frequency paths
to prevent log storms.

## Counter Instrumentation Architecture

### Compile-time instrumentation level

Primary knob:

- `INSTRUMENTATION_LEVEL` in top-level `Makefile`.

Levels:

- `0` = none — no counters compiled in
- `1` = ops — production-relevant counters (request rates, timeout classes, error counts)
- `2` = dev — all counters including detailed diagnostic internals

Counters at inactive levels are compiled out entirely — they add zero runtime cost
and do not exist in the stats struct.

## Quick Commands

Build with ops-only instrumentation:

```bash
make instr-ops
```

Build with dev instrumentation:

```bash
make instr-dev
```

Build with trace compile ceiling:

```bash
make trace
```
