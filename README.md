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

Auto-detects your environment:

- **`PARALLEL_API_KEY` set** → calls `https://api.parallel.ai/v1/` directly via C FFI
- **No key** → falls back to the free MCP endpoint via JS

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

Run `/reload` in Pi. Done.

## Test

```bash
make test    # 25 tests
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
├── parallel-search.ts       # Pi extension
├── lib/
│   ├── parallel.mjs         # Core logic (JS)
│   └── ffi.mjs              # FFI bridge (koffi → C)
├── native/
│   ├── parallel_lib.c       # Shared library (libcurl + cJSON)
│   └── parallel.c           # CLI binary
├── test/
│   ├── mock-server.mjs      # Mock API server
│   ├── unit.mjs             # 25 tests
│   └── benchmark.mjs        # Performance comparison
├── docs/
│   └── index.html           # Landing page
└── Makefile
```

## Docs

- [Benchmark writeup](docs/BENCHMARK.md) — the full optimization story
- [Landing page](https://mdlockyer.github.io/parallel-pi-tools/) — 93µs, with pulse rings

## License

ISC
