# Changelog

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
