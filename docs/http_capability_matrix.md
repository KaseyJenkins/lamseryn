# HTTP Capability Matrix (Current vs Planned)

Last updated: 2026-04-29

## Overview

This document is the inventory of:

- what HTTP behavior is implemented and enforced,
- which request headers are captured but not yet acted on,
- what each feature flag currently includes,
- and known gaps planned for future work.

## 1) Current HTTP Handling Baseline

### Parser and framing (implemented)

- Request parsing via llhttp with strict policy hooks.
- `Content-Length` syntax/duplication validation.
- `Transfer-Encoding` and `Content-Length` conflict rejection.
- Unsupported transfer-coding rejection.
- Header byte and header field-count caps.
- Body size cap.
- `Expect` policy:
: `Expect: 100-continue` accepted without interim 100 response;
: unsupported `Expect` values rejected.

### Request routing/static serving (implemented)

- Static routing for `GET`/`HEAD` to configured docroot.
- Path normalization and traversal-safe resolution.
- Response mode selection (`HEAD`, buffered send, sendfile).

### Deadlines and resilience (implemented)

- Initial/header/body/keepalive/write/drain timeout classes.
- Timed-out requests receive a `408` response; slow readers are forcibly disconnected; error paths drain cleanly before closing.

## 2) Header Matrix: Observed vs Acted On

Legend:

- `Always stored`: captured regardless of feature flags.
- `Feature-stored`: captured only when a feature bit is enabled.
- `Enforced (not stored)`: parsed and used for correctness/security, but not in `req_hdrs` storage path.

| Header | Capture class | Capture gate | Runtime behavior today | Status |
|---|---|---|---|---|
| `Host` | Always stored | none | Used for normal HTTP/1.1 request handling; available in request echo/debug paths | Implemented |
| `Connection` | Always stored | none | Used to derive keepalive/close behavior | Implemented |
| `Content-Length` | Enforced (not stored) | parser policy | Syntax/duplicate/conflict checks; body length framing | Implemented |
| `Transfer-Encoding` | Enforced (not stored) | parser policy | TE parsing, unsupported coding reject, TE+CL reject | Implemented |
| `Expect` | Enforced (not stored) | parser policy | `100-continue` accepted (no interim 100), unsupported values reject | Implemented (phase policy) |
| `Range` | Feature-stored | `CFG_FEAT_RANGE` | Single byte-range parsed and evaluated; satisfiable range -> `206 Partial Content` with `Content-Range`; unsatisfiable -> `416`; multi-range and invalid syntax fall back to `200` full response; `Accept-Ranges: bytes` advertised on all static responses | Implemented |
| `If-Range` | Feature-stored | `CFG_FEAT_RANGE` | ETag comparison gates range response; mismatch falls back to `200` full response | Implemented |
| `If-Modified-Since` | Feature-stored | `CFG_FEAT_CONDITIONAL` | Compared against file mtime; match -> `304 Not Modified`; evaluated only when `If-None-Match` is absent (RFC 7232 precedence) | Implemented |
| `If-None-Match` | Feature-stored | `CFG_FEAT_CONDITIONAL` | Weak ETag list comparison including wildcard `*`; match -> `304 Not Modified`; takes precedence over `If-Modified-Since` | Implemented |
| `Accept-Encoding` | Feature-stored | `CFG_FEAT_COMPRESSION` | Precompressed sibling selection (`.br`/`.gz`) for compressible static assets with `Content-Encoding` emission; `Vary: Accept-Encoding` emitted for both encoded and identity responses on compressible types; range requests bypass compressed variant selection; on-the-fly gzip (always) and brotli (when built with `HAVE_BROTLI`) compression for assets without a precompressed sibling when `compression_dynamic` is enabled | Implemented |
| `Authorization` | Feature-stored | `CFG_FEAT_AUTH` | Captured only; no auth challenge/allow/deny policy engine yet | Captured, semantics pending |
| `Cookie` | Feature-stored | `CFG_FEAT_AUTH` | Captured only; no auth/session policy evaluator yet | Captured, semantics pending |

## 3) Feature Flags: What They Include Today

### `range`

Included headers:

- `Range`
- `If-Range`

Current effect:

- Single byte-range parsed and evaluated against file size.
- Satisfiable range -> `206 Partial Content` with `Content-Range: bytes start-end/total` and correct `Content-Length`.
- Unsatisfiable range -> `416 Range Not Satisfiable` with `Content-Range: bytes */total` and zero-length body.
- Multi-range and syntactically invalid `Range` values fall back to `200` full response.
- `If-Range` ETag match gates range response; mismatch falls back to `200` full response.
- `Accept-Ranges: bytes` advertised on all static `200` and `206` responses.
- TLS connections use buffered read path (kernel sendfile bypassed to preserve TLS framing).
- `ETag` included on `206` responses for downstream cache coherence.

### `conditional`

Included headers:

- `If-Modified-Since`
- `If-None-Match`

Current effect:

- ETag derived from inode, size, mtime (nanoseconds); emitted as `ETag` on all static responses.
- `Last-Modified` emitted as IMF-fixdate on all static responses.
- `If-None-Match`: weak ETag list comparison including wildcard `*`; match -> `304 Not Modified` (no body, no `Content-Type`).
- `If-Modified-Since`: file mtime compared against parsed IMF-fixdate; not-modified -> `304`.
- RFC 7232 precedence enforced: `If-None-Match` present -> `If-Modified-Since` ignored.
- `304` responses include `ETag` and `Last-Modified` but omit `Content-Type` and body.

### `compression`

Included headers:

- `Accept-Encoding`

Current effect:

- `Accept-Encoding` parsing drives precompressed static sibling selection.
- Encoded responses emit `Content-Encoding` (`br` or `gzip`) and `Vary: Accept-Encoding`.
- Identity responses for compressible MIME types also emit `Vary: Accept-Encoding`.
- Range requests and non-compressible MIME types bypass precompressed variant serving.

Dynamic (on-the-fly) compression — enabled with `compression_dynamic = true`:

- When no precompressed sibling exists and the client sends `Accept-Encoding: gzip`, the file is compressed in memory before sending.
- File size must be within `compression_dynamic_min_bytes` (default 256 B) and `compression_dynamic_max_bytes` (default 1 MiB) to be eligible; outside that window the file is served uncompressed.
- Compression effort is controlled by `compression_dynamic_effort` (1–9, default 1); applies to all codecs (gzip level, brotli quality).
- If the compressed output is not smaller than the original, the original is sent uncompressed.
- Precompressed siblings take priority over dynamic compression when both would match.
- Brotli (`br`) dynamic compression is guarded by `HAVE_BROTLI` at compile time; gzip is always available.

Not yet implemented:

- Authentication backend/policy hooks (under `auth` flag).

### `auth`

Included headers:

- `Authorization`
- `Cookie`

Current effect:

- Header capture only.

Not yet implemented:

- Authentication backend/policy hooks.
- `401 Unauthorized` and challenge response behavior.
- Authz policy integration for route/docroot access.

## 4) Known Gaps

- Auth semantics (challenge, authn/authz decisions).
- `Cache-Control` / `Expires` response headers (freshness policy; currently clients use heuristic freshness based on `Last-Modified`).
- Structured access-log formats beyond text (for example JSONL).

## 5) Access Logging

### Runtime and emission behavior

- Config keys under `[globals]` enable per-request access-log emission.
- Supported sinks: `stderr`, `stdout`, or file path (append mode).
- One line emitted at response completion for allowed status/sample filters.
- Current format: text key/value line (`access_log_format=text`).
- Sink write path is best-effort and request handling never fails on log sink errors.

### Guardrails and observability

- Min-status filter (`access_log_min_status`) gates low-value statuses.
- 1/N sampling (`access_log_sample`) reduces high-traffic log volume.
- Runtime counters track emitted, dropped, and write-error outcomes.
- Sink failures emit rate-limited warnings to avoid warning storms.

### Current limitations

- Reopen-on-`SIGHUP` for file sinks is implemented; in-process size/time rotation is intentionally deferred in favor of external rotation tooling (for example logrotate).
- Structured formats beyond text (for example JSONL) are pending.
