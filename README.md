# parallel-pi-tools

**93µs** — The world's fastest web search for [Pi](https://github.com/badlogic/pi-mono/).

<p align="center">
  <code>web_search</code> · <code>web_fetch</code> · native C via FFI · no MCP overhead
</p>

---

## What it does

| Tool | Description |
|------|-------------|
| `web_search` | Natural language web search. Ranked URLs with LLM-optimized excerpts. |
| `web_fetch` | Extract clean markdown from any URL. JS-heavy pages, PDFs, the lot. |

## How fast

```
┌───────────────────────────┬──────────┬─────────────────┐
│ Approach                  │ Avg      │ Relative        │
├───────────────────────────┼──────────┼─────────────────┤
│ C FFI (this)              │    93µs  │ 1×              │
│ JS (fetch + JSON.parse)   │ 1,500µs  │ 16× slower      │
│ C (subprocess)            │ 8,200µs  │ 88× slower      │
└───────────────────────────┴──────────┴─────────────────┘
```

## How it works

Uses the native C library in-process via koffi FFI (async, worker thread — the TUI never blocks). No JS fallback for API calls.

- **`PARALLEL_API_KEY` set** → direct API via native C FFI
- **No key** → falls back to the free MCP endpoint via JS

The C path covers the full API surface — multi-query searches, comma-containing URLs, and all escaping.

```bash
# Set your key (optional)
export PARALLEL_API_KEY="your-key-here"
```

## Install

```bash
git clone https://github.com/mdlockyer/parallel-pi-tools.git
cd parallel-pi-tools
make install
```

Builds the native library, copies `web-search.ts` + `lib/web-search/` + the shared library into `~/.pi/agent/extensions/`, and installs `koffi` there on first run.

Run `/reload` in Pi. Done.

## Test

```bash
make test    # 31 tests (incl. native FFI equivalence vs JS)
make bench   # 5-approach benchmark
make all     # both
```

## Uninstall

```bash
make uninstall
```

## Architecture

```
parallel-pi-tools/
├── web-search.ts            # Pi extension
├── lib/
│   ├── parallel.mjs         # Core logic (native FFI only)
│   └── ffi.mjs              # koffi bridge → libparallel (async, leak-free)
├── native/
│   ├── parallel_lib.c       # Shared library (libcurl + cJSON)
│   └── parallel.c           # CLI binary
├── test/
│   ├── mock-server.mjs      # Mock API server
│   ├── unit.mjs             # 31 tests
│   └── benchmark.mjs        # Performance comparison
├── docs/
│   └── index.html           # Landing page
└── Makefile
```

The extension calls `apiSearch`/`apiExtract` which route through the C FFI (async, so Pi stays responsive). MCP tool call is the free fallback when no API key is set.

## Docs

- [Benchmark writeup](docs/BENCHMARK.md) — the full optimization story
- [Landing page](https://mdlockyer.github.io/parallel-pi-tools/) — 93µs, with pulse rings

## License

ISC
