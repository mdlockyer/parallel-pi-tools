# Changelog

## [1.0.0] — 2026-08-22

### Fixed

- **Double free / heap-use-after-free in `parse_sse_result`** — a parsed SSE message was deleted twice when a `result` event carried no usable text content. Could crash the host process via the MCP fallback path. Verified with AddressSanitizer.
- **Thread-unsafe shared curl handle** — all FFI entry points drove one global `CURL` easy handle, but koffi async calls run on worker threads; concurrent tool calls could corrupt requests or crash. Each request now uses its own easy handle, global init runs once via `pthread_once`, and a lock-protected `CURLSH` share keeps connection/DNS/TLS-session reuse across calls (verified: sequential calls reuse one pooled TCP connection).
- **Connection reuse preserved under the new model** — the shared cache is guarded with per-data-type mutexes (`CURLSHOPT_LOCKFUNC`/`UNLOCKFUNC`), so pooling is safe for concurrent worker-thread access.
- **Heap overflow on allocation failure** — the string builder doubled `cap` even when `realloc` failed, and callers then wrote past the end of the buffer. Growth failure is now sticky (`oom` flag) and checked before every append.
- **API errors silently swallowed** — HTTP >= 400 responses were discarded, surfacing only "Search API failed". Failures now propagate as `PARALLEL_ERROR: <status + response excerpt>` strings that JS unwraps into thrown errors (401 vs 429 vs 400 are distinguishable), including JSON-RPC errors from MCP endpoints.
- **MCP initialize handshake** — the client now performs `initialize` / `notifications/initialized` and carries the `Mcp-Session-Id` on subsequent calls (spec-compliant servers); fully stateless servers still work as before.
- **Header callback bounds** — `Content-Type`/`Mcp-Session-Id` parsing is now strictly bounded (header lines are not NUL-terminated).
- **Silent header truncation** — `x-api-key` and `Accept` headers are built dynamically instead of fixed 512/256-byte stack buffers.
- **API key no longer passed as an argv to the CLI binary** — pass `-` to read `$PARALLEL_API_KEY` from the environment instead (command-line args are visible to every local user via `ps`).
- **Key-detection drift** — C's `parallel_find_key` now mirrors the JS regex exactly (`PARALLEL[_-]?(API[_-]?)?(KEY|SECRET|TOKEN)`, case-insensitive) by scanning `environ`, replacing a hand-maintained candidate list.
- **CLI/lib code duplication** — `native/parallel.c` is now a thin front-end over `parallel_lib.c`; formatting and error handling can no longer drift between binary and library.
- Trailing slash in `api_base` no longer produces `//search` URLs; overflow guards added to response-buffer growth.

### Changed

- `web_search.ts` resolves the API key per tool call instead of once at registration, so env changes apply without `/reload`.
- CI runs `npm ci` only (no silent fallback to `npm install`); the benchmark job runs on manual dispatch rather than every push.
- Removed non-standard `allowScripts` field from package.json.

## [1.0.0-rc.1] — 2026-08-06

### Added

- **Pi extension** (`parallel-search.ts`) — registers `web_search` and `web_fetch` as native tools
- **Auto-detection** — finds API key from env (`PARALLEL_API_KEY`, `PARALLEL_KEY`, `PARALLELSECRET`, `PARALLEL_TOKEN`, case-insensitive)
- **Dual mode** — direct API via C FFI when key present, free MCP endpoint fallback when absent
- **C shared library** (`native/parallel_lib.c`) — full pipeline: HTTP (libcurl), JSON parsing (cJSON), text formatting
- **FFI bridge** (`lib/ffi.mjs`) — koffi-based, zero spawn overhead
- **CLI binary** (`native/parallel`) — standalone tool for shell pipelines
- **Mock server** (`test/mock-server.mjs`) — simulates Parallel API + MCP endpoint
- **Unit tests** (`test/unit.mjs`) — 25 tests covering key detection, MCP, API, native binary, and output parity
- **Benchmark** (`test/benchmark.mjs`) — 5-approach comparison: raw fetch, JS, MCP, subprocess, FFI
- **GitHub Actions CI** — matrix: ubuntu + macos, node 20 + 22
- **Makefile** — `install`, `uninstall`, `test`, `bench`, `native`, `all`
- **Benchmark writeup** (`docs/BENCHMARK.md`) — the full optimization story

### Performance

| Approach | Avg | vs JS |
|---|---|---|
| C FFI (optimized) | 93µs | 16x faster |
| JS (fetch + JSON.parse) | 1,500µs | baseline |
| C (subprocess) | 8,200µs | 5.5x slower |

### Key optimizations

- Reuse curl handle across calls (~400µs saved per call)
- Build request JSON with `snprintf` instead of cJSON
- Stack-allocated URL and body buffers
- Pre-composed HTTP headers (rebuilt only on key change)
- `SB_LIT` macro for compile-time `strlen` on string literals
- TCP keepalive for connection reuse
