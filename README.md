# Lamseryn

[![Release](https://img.shields.io/github/release/KaseyJenkins/lamseryn.svg)](https://github.com/KaseyJenkins/lamseryn/releases)
[![Phase1 Gate](https://github.com/KaseyJenkins/lamseryn/actions/workflows/phase1-gate.yml/badge.svg)](https://github.com/KaseyJenkins/lamseryn/actions/workflows/phase1-gate.yml)
[![Nightly Latest Deps](https://github.com/KaseyJenkins/lamseryn/actions/workflows/nightly-latest.yml/badge.svg)](https://github.com/KaseyJenkins/lamseryn/actions/workflows/nightly-latest.yml)

A lightweight, io_uring-first HTTP/1.1 server written in C for Linux.

Most Linux servers still use `epoll` as their I/O backbone — Lamseryn takes a different path.
Every network operation (accept, read, write, close, timeout sweep) is submitted through
`io_uring`, so multiple operations can be batched into a single kernel transition instead of
requiring one system call per event. Each worker thread owns its own ring, listener set, and
connection store, so threads never contend with each other on the I/O path.

The result is a server that is simple to reason about, straightforward to operate, and
well-suited for exploring what an io_uring-native networking stack looks like in practice.

## Project Aims

- Build a correct, auditable HTTP/1.1 server using io_uring as the sole I/O runtime.
- Provide a clear, modular C codebase that can serve as a reference for io_uring networking patterns.
- Evaluate the practical performance and operational trade-offs of an io_uring-first design.

## Current Capabilities

- **HTTP/1.1 parsing and pipelining** — full request parsing via llhttp with
  Content-Length/Transfer-Encoding conflict rejection, header count and size caps,
  body size enforcement, and request smuggling defenses.
- **Six-class timeout architecture** — independent deadlines for initial idle,
  header receive, body receive, keep-alive idle, response write, and post-error
  drain, each with configurable thresholds. Timed-out requests receive a `408`
  response, slow readers are forcibly disconnected, and error paths drain cleanly
  before closing.
- **Static file serving** — GET/HEAD with path-traversal-safe resolution,
  automatic sendfile selection for large responses, precompressed sibling
  selection (`.br`/`.gz`) from `Accept-Encoding`, and per-vhost docroot
  configuration.
- **Dynamic on-the-fly compression** — gzip (always available) and brotli
  (when built with brotli support) applied at response time for compressible
  MIME types; configurable size window, compression level, and per-vhost
  enable/disable. Precompressed siblings take priority over dynamic compression.
- **Multi-vhost INI configuration** — workers, timeouts, ports, docroots, TLS
  settings, and logging knobs are all driven by a single `server.ini` file.
- **Optional TLS** — build-time and runtime OpenSSL integration with per-vhost
  certificate/key paths, minimum protocol version, and cipher suite controls.
- **Overload resilience** — when the system runs out of file descriptors, new
  accepts are paused briefly and retried automatically. When a client is too slow
  to receive data, the server waits for the socket to become writable rather than
  buffering indefinitely, and enforces a write timeout to reclaim stuck connections.
- **Gate-style validation** — unit tests, integration tests, and timeout
  gates run locally and in CI via a single `make phase1-gate` command.

## Requirements

- Linux kernel with io_uring support
- gcc
- make
- python3

**zlib** (required for dynamic gzip compression):
- If `third_party/zlib/` contains the zlib source tree, `make` builds it
  automatically with no system dependency.
- Otherwise, `zlib1g-dev` (or equivalent) must be installed system-wide.
  Absence causes a hard compile failure.

**brotli** (optional — absence silently disables brotli compression):
- If `third_party/brotli/` contains the brotli source tree, `make` builds it
  automatically and the binary gains brotli support (`HAVE_BROTLI`).
- Otherwise, the build probes `pkg-config libbrotlienc`. If found, brotli is
  enabled via system libraries.
- If neither is present, the binary is built without brotli. Dynamic
  compression still works with gzip; brotli requests fall back to gzip or
  identity. **No warning is emitted at build time.**

Optional:
- OpenSSL (for TLS-enabled build/runtime paths).

## Build

Standard build:

```bash
make -j$(nproc)
```

AddressSanitizer build:

```bash
make asan
```

TLS-enabled build:

```bash
make ENABLE_TLS=1
```

## Run

Default configuration:

```bash
./build/lamseryn
```

Custom configuration path:

```bash
SERVER_CONFIG=/path/to/server.ini ./build/lamseryn
```

Quick sanity check:

```bash
curl -i http://127.0.0.1:8080/
```

## Configuration

Config source order:

1. SERVER_CONFIG environment variable (if set)
2. server.ini in repository root

Reference docs:

- docs/config_ini_reference.md
- server.ini

## Test and Validation

Unit tests:

```bash
make -C tests test
```

Integration tests:

```bash
make itest
```

Canonical gate command (used in CI):

```bash
make phase1-gate
```

CI workflow:

- .github/workflows/phase1-gate.yml

## Contributing

Before opening a PR, run:

```bash
make phase1-gate
```
