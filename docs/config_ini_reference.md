# INI Configuration Reference

Last updated: 2026-04-30

This document is the authoritative reference for `server.ini` keys accepted by Lamseryn.

## Load Rules

- Config path is read from `SERVER_CONFIG` env var.
- If `SERVER_CONFIG` is unset/empty, the server loads `server.ini` from repo root.
- At least one `[vhost ...]` section must exist.
- Maximum vhosts: `32`.
- Vhost section names must be short enaough for parser and internal buffers.

Accepted vhost section forms:

- `[vhost default]`
- `[vhost:default]`
- `[vhost.default]`
- `[vhost]` (name defaults to `default`)

## Value Parsing Rules

Booleans (`true`/`false` style keys):

- true values: `true`, `1`, `yes`, `on`
- false values: `false`, `0`, `no`, `off`

Integer keys:

- `port` and `max_header_fields` are `0..65535`.
- most global numeric keys are parsed as unsigned 32-bit.

Unknown keys:

- unknown `[globals]` keys are ignored.
- unknown `[vhost ...]` keys log a warning and are ignored.

Invalid values:

- invalid booleans for vhost keys log warning and key is ignored.
- invalid numeric values for strict keys (for example `port`) fail parse.

## Section: `[globals]`

These keys are optional. If not present, runtime defaults are used.

| Key | Type | Default if absent | Notes |
|---|---|---|---|
| `log_level` | enum | runtime logger default | `error`, `warn`, `info`, `debug`, `trace` |
| `log_categories` | csv enum | runtime logger default | tokens: `all`, `core`, `accept`, `io`, `http`, `buf`, `timer`, `poll` |
| `queue_depth` | u32 | computed fallback (>= `IOURING_QUEUE_DEPTH`) | See queue-depth behavior below |
| `pre_accepts` | u32 | `CONFIG_PRE_ACCEPTS` (`1`) | initial accepts per worker |
| `workers` | u32 (`>0`) | `2` | worker thread count (clamped to `1..128` at startup) |
| `initial_idle_timeout_ms` | u32 | `INITIAL_IDLE_TIMEOUT_MS` (`1000`) | connection idle before first request |
| `keepalive_idle_close_ms` | u32 | `IDLE_CLOSE_MS` (`5000`) | keepalive idle timeout |
| `header_timeout_ms` | u32 | `HEADER_TIMEOUT_MS` (`30000`) | request header deadline |
| `body_timeout_ms` | u32 | `BODY_TIMEOUT_MS` (`30000`) | request body deadline |
| `write_timeout_ms` | u32 | `WRITE_TIMEOUT_MS` (`10000`) | response write deadline |
| `drain_timeout_ms` | u32 | `DRAIN_TIMEOUT_MS` (`2000`) | post-error drain window |
| `accept_backoff_ms` | u32 | `ACCEPT_BACKOFF_MS` (`5`) | EMFILE/ENFILE backoff |
| `shutdown_grace_ms` | u32 (`>0`) | `5000` | global graceful-shutdown bound for first-signal drain and force-timeout fallback |
| `wake_pipe_mode` | enum | `shared` | wake signaling topology: `shared` (lower FD usage) or `per-worker` |
| `access_log_enabled` | bool | `false` | enables per-request access-log emission |
| `access_log_path` | string | empty | sink path; supports `stdout` and `stderr` literals; file sinks support reopen on `SIGHUP` for external rotation |
| `access_log_format` | enum | `text` | currently accepted: `text` |
| `access_log_sample` | u32 (`>0`) | `1` | 1/N sampling factor |
| `access_log_min_status` | u32 (`100..599`) | `100` | status threshold for emit/filter |
| `default_max_header_fields` | u32 (`<=65535`) | `100` | applied when creating vhosts that do not set `max_header_fields` |
| `tls` | bool | disabled unless vhost/global enables | global default for vhosts |
| `tls_cert_file` | string | empty | global fallback for TLS vhosts |
| `tls_key_file` | string | empty | global fallback for TLS vhosts |
| `tls_min_version` | string | implementation default | accepted by TLS layer: `tls1.2` or `tls1.3` |
| `tls_ciphers` | string | OpenSSL default | optional cipher string |
| `tls_ciphersuites` | string | OpenSSL default | optional TLS1.3 suites string |
| `tls_session_tickets` | bool | implementation default | global fallback for vhosts |
| `tls_session_cache` | bool | implementation default | global fallback for vhosts |

Queue-depth behavior:

1. Use `[globals].queue_depth` if set.
2. Else use env `QUEUE_DEPTH` if set and valid.
3. Else compute from pre-accept heuristic with lower bound `IOURING_QUEUE_DEPTH` (`2048`).

Wake-pipe mode behavior:

1. Use `[globals].wake_pipe_mode` if set.
2. Else default to `shared`.

## Section: `[vhost <name>]`

Each vhost should declare bind/port/docroot and feature toggles.

| Key | Type | Default if absent | Notes |
|---|---|---|---|
| `bind` | string | empty | host/IP is normalized to numeric form at load time; wildcard allowed |
| `port` | u16 | `0` | vhost is unusable without a port |
| `docroot` | string | empty | opened as directory at startup; failures log warning |
| `max_header_fields` | u16 | `default_max_header_fields` or `100` | enforced by HTTP limits |
| `static` | bool | false | enables static routing behavior |
| `range` | bool | false | enables capture of `Range` and `If-Range` headers |
| `conditional` | bool | false | enables capture of conditional headers |
| `compression` | bool | false | enables `Accept-Encoding`-driven precompressed sibling selection (`.br`/`.gz`) and `Content-Encoding`/`Vary` response behavior for compressible static assets |
| `compression_dynamic` | bool | false | enables on-the-fly compression (gzip always; brotli when built with `HAVE_BROTLI`) for compressible assets that have no precompressed sibling; requires `compression = true`; response buffered in memory before send |
| `compression_dynamic_min_bytes` | u32 | `256` | files smaller than this threshold are served uncompressed even when `compression_dynamic` is enabled |
| `compression_dynamic_max_bytes` | u32 | `1048576` (1 MiB) | files larger than this threshold are served uncompressed (sendfile path); prevents unbounded memory use |
| `compression_dynamic_effort` | u32 | `1` | compression effort `1`–`9` applied to all codecs: for gzip maps to zlib level 1–9; for brotli maps to brotli quality 1–9; `1` is fastest; `9` is smallest output |
| `header_set` | string (repeatable) | none | emitted verbatim as a response header on all static responses (`200`, `206`, `HEAD`, `304`); value must be a valid `Header-Name: value` line; up to 16 per vhost; max 1024 bytes per entry; total emitted bytes across all entries must not exceed 1536 bytes (excess entries are warned and ignored at startup) |
| `auth` | bool | false | enables capture of `Authorization` and `Cookie` |
| `tls` | bool | inherit from globals or false | explicit vhost override |
| `tls_cert_file` | string | inherit from globals or empty | required when effective TLS is enabled |
| `tls_key_file` | string | inherit from globals or empty | required when effective TLS is enabled |
| `tls_min_version` | string | inherit from globals | validated by TLS init |
| `tls_ciphers` | string | inherit from globals | optional |
| `tls_ciphersuites` | string | inherit from globals | optional |
| `tls_session_tickets` | bool | inherit from globals | optional override |
| `tls_session_cache` | bool | inherit from globals | optional override |

Feature-toggle reality:

- `static` affects serving path.
- `range`, `conditional`, and `compression` drive both header capture and static-serving behavior.
- `compression_dynamic` requires `compression = true`; has no effect if `compression` is disabled.
- `auth` currently drives header capture for future auth semantics.
- See `docs/http_capability_matrix.md` for implemented vs planned semantics.

## TLS Inheritance and Validation

Effective TLS settings are built per vhost as:

1. Use vhost key if set.
2. Else use corresponding global key if present.
3. Else use runtime/TLS library defaults.

Validation:

- If effective `tls=true`, both cert and key paths must be non-empty after inheritance.
- Missing cert/key on a TLS-enabled vhost fails config load.

## Bind Resolution and Warnings

At config load:

- non-wildcard binds are resolved to numeric addresses;
- resolution errors fail startup.

Warnings (non-fatal):

- link-local IPv6 bind without zone id,
- multiple wildcard vhosts on the same port (first-match selection behavior).

## Minimal Example

```ini
[globals]
workers = 2
queue_depth = 4096
initial_idle_timeout_ms = 5000
header_timeout_ms = 5000
default_max_header_fields = 100

[vhost default]
bind = 0.0.0.0
port = 8080
docroot = tests/integration/wwwroot
static = true
range = true
conditional = true
compression = false
auth = false
max_header_fields = 100
```
