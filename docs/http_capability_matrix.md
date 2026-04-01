# HTTP Capability Matrix (Current vs Planned)

Last updated: 2026-03-20

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
| `Range` | Feature-stored | `CFG_FEAT_RANGE` | Captured only; no 206/partial-content response path yet | Captured, semantics pending |
| `If-Range` | Feature-stored | `CFG_FEAT_RANGE` | Captured only; no range validator decision path yet | Captured, semantics pending |
| `If-Modified-Since` | Feature-stored | `CFG_FEAT_CONDITIONAL` | Captured only; no 304 evaluator yet | Captured, semantics pending |
| `If-None-Match` | Feature-stored | `CFG_FEAT_CONDITIONAL` | Captured only; no ETag/304 evaluator yet | Captured, semantics pending |
| `Accept-Encoding` | Feature-stored | `CFG_FEAT_COMPRESSION` | Captured only; no content-encoding selection/compression pipeline yet | Captured, semantics pending |
| `Authorization` | Feature-stored | `CFG_FEAT_AUTH` | Captured only; no auth challenge/allow/deny policy engine yet | Captured, semantics pending |
| `Cookie` | Feature-stored | `CFG_FEAT_AUTH` | Captured only; no auth/session policy evaluator yet | Captured, semantics pending |

## 3) Feature Flags: What They Include Today

### `range`

Included headers:

- `Range`
- `If-Range`

Current effect:

- Header capture only.

Not yet implemented:

- Byte-range parser and satisfiable-range checks.
- `206 Partial Content` and `416 Range Not Satisfiable` behavior.
- `Content-Range` generation and conditional range semantics with validators.

### `conditional`

Included headers:

- `If-Modified-Since`
- `If-None-Match`

Current effect:

- Header capture only.

Not yet implemented:

- Validator model (mtime and/or ETag source of truth).
- Precondition evaluation and `304 Not Modified` flow.
- Strong/weak ETag policy and precedence rules.

### `compression`

Included headers:

- `Accept-Encoding`

Current effect:

- Header capture only.

Not yet implemented:

- Encoding negotiation (`gzip`/`br`/etc.) decision engine.
- Actual compression or precompressed-asset selection path.
- `Content-Encoding` and `Vary: Accept-Encoding` response behavior.

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

- Range semantics (`206`/`416` + `Content-Range`).
- Conditional GET semantics (`304` + validator policy).
- Compression semantics (encoding negotiation + response encoding path).
- Auth semantics (challenge, authn/authz decisions).
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
