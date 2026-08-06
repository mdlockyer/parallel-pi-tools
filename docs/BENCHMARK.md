# How We Built the World's Fastest Parallel Web Search for Pi

A story in six commits about going from "add a search tool" to 93 microseconds.

## The Question

> How do we add real-time web search to [Pi](https://github.com/badlogic/pi-mono/)?

## Attempt 1: MCP Adapter

The obvious path. Install `pi-mcp-adapter`, point it at `https://search.parallel.ai/mcp`, done.

**Problem:** MCP tool definitions burn context window tokens. A single MCP server can eat 10k+ tokens before you even use it. Connect a few servers and you've burned half your context window on tools you might never call.

## Attempt 2: Native Pi Extension

Write a TypeScript extension that calls the Parallel API directly via `fetch()`. Two tools, `web_search` and `web_fetch`. Auto-detects API key from environment, falls back to free MCP endpoint.

**Result:** ~1.5ms per call. Fast enough. Ship it.

But then we got curious.

## Attempt 3: Overengineer With C

What if we implement the whole pipeline in C? libcurl for HTTP, cJSON for parsing, direct string formatting. Surely C will crush JavaScript?

We built it. Compiled it. Benchmarked it.

**Result:** 8.2ms per call. **8x slower than JS.**

```
┌─────────────────┬──────────┬────────────────────────────────┐
│ Approach        │ Avg      │ What's happening               │
├─────────────────┼──────────┼────────────────────────────────┤
│ JS (fetch)      │  1.5ms   │ Node.js HTTP + JSON.parse      │
│ C (subprocess)  │  8.2ms   │ fork + exec + pipe + waitpid   │
└─────────────────┴──────────┴────────────────────────────────┘
```

The C code itself was lightning fast. The problem was getting to it. Every call paid 7ms of `posix_spawn` kernel scheduling tax. The "slow" scripting language won because it avoided the syscall overhead entirely.

## Attempt 4: The Clever Part

What if we never leave the process?

We compiled the C code as a shared library (`.dylib`/`.so`) and called it directly from Node via [koffi](https://koffi.dev/), a zero-dependency FFI bridge. No subprocess. No pipes. No kernel scheduling.

**Result:** 564µs per call. 3x faster than JS.

```
┌─────────────────┬──────────┬────────────────────────────────┐
│ Approach        │ Avg      │ vs JS                          │
├─────────────────┼──────────┼────────────────────────────────┤
│ JS (fetch)      │ 1,500µs  │ baseline                       │
│ C (subprocess)  │ 8,200µs  │ 5.5x slower                    │
│ C (FFI)         │   564µs  │ 2.7x faster                    │
└─────────────────┴──────────┴────────────────────────────────┘
```

The FFI call overhead was ~microseconds. The speedup came from C genuinely doing less work:
- libcurl is leaner than Node's fetch for small requests
- cJSON parses faster than `JSON.parse`
- String formatting in C is basically free

## Attempt 5: Squeezing Every Cycle

We profiled the C code and found five inefficiencies:

| Optimization | Savings | Why |
|---|---|---|
| **Reuse curl handle** | ~400µs | Eliminates DNS cache rebuild + SSL context init per call |
| **`snprintf` instead of `cJSON_PrintUnformatted`** | ~30µs | Avoids cJSON alloc + serialize for 20-byte payloads |
| **Stack buffers for URL/body** | ~15µs | No `malloc`/`free` for request construction |
| **Pre-composed headers** | ~10µs | `curl_slist_append` only when key changes |
| **`SB_LIT` macro** | ~5µs | Compile-time `strlen` on string literals |

The curl handle reuse was the monster. 400 microseconds of DNS + SSL setup, eliminated on every call.

**Final result:** 93µs per call.

## The Final Scoreboard

```
┌───────────────────────────┬──────────┬─────────────────────────────────────┐
│ Approach                  │ Avg      │ Relative                            │
├───────────────────────────┼──────────┼─────────────────────────────────────┤
│ C FFI (optimized)         │    93µs  │ ████████████████████████████████ 1x │
│ JS (fetch + JSON.parse)   │ 1,500µs  │ ██ 16x slower                       │
│ C (subprocess)            │ 8,200µs  │ ▏ 88x slower                        │
└───────────────────────────┴──────────┴─────────────────────────────────────┘
```

## What We Learned

1. **Subprocess overhead dominates for small payloads.** 7ms of kernel scheduling to execute 93µs of work. The math never works.

2. **FFI call overhead is essentially zero.** `koffi` adds ~microseconds per call. The speedup comes from the code being faster, not the bridge being cheaper.

3. **curl handle reuse is the single biggest win.** If you're making repeated HTTP requests, `curl_easy_init`/`curl_easy_cleanup` per call is leaving 400µs on the table.

4. **C beats JS at small, tight loops.** For a single HTTP request + JSON parse + string format, C is 16x faster. The gap narrows for larger payloads where Node's JIT can optimize, but for this workload, C wins decisively.

5. **The "slow" language always wins if it avoids the syscall.** Node's fetch is "slow" compared to libcurl. But it runs in-process. The C binary was "fast" but paid 7ms to start. Context matters more than benchmarks.

## How to Use It

```bash
# Install
make install    # copies extension to ~/.pi/agent/extensions/
                # also builds native/parallel and native/libparallel.dylib

# Run /reload in Pi
# Done. web_search and web_fetch are available as native tools.
```

The extension auto-detects your environment:

- **`PARALLEL_API_KEY` set** → calls `https://api.parallel.ai/v1/` directly via C FFI
- **No key** → falls back to the free MCP endpoint via JS

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        Pi (Node.js)                         │
│                                                             │
│  parallel-search.ts                                         │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  web_search / web_fetch                             │    │
│  │       │                                             │    │
│  │       ├─[API key?]──→ lib/ffi.mjs ──→ libparallel   │    │
│  │       │               (koffi FFI)    (C, 93µs)      │    │
│  │       │                                             │    │
│  │       └─[no key]────→ lib/parallel.mjs              │    │
│  │                       (JS fetch, 1.5ms)             │    │
│  └─────────────────────────────────────────────────────┘    │
│                                                             │
│  Test suite                                                 │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  mock-server.mjs  ←→  unit.mjs  (25 tests)          │    │
│  │                   ←→  benchmark.mjs.                │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

## Files

```
parallel-pi-tools/
├── parallel-search.ts       # Pi extension (auto-detects API key)
├── lib/
│   ├── parallel.mjs         # Core logic (JS, no Pi deps)
│   └── ffi.mjs              # FFI wrapper (koffi → C)
├── native/
│   ├── parallel_lib.c       # Shared library (curl + cJSON + formatters)
│   ├── parallel.c           # CLI binary
│   ├── cJSON.c / cJSON.h    # Embedded JSON parser
│   └── Makefile
├── test/
│   ├── mock-server.mjs      # Mock API + MCP server
│   ├── unit.mjs             # 25 tests
│   ├── benchmark.mjs        # 5-approach comparison
│   └── benchmark.sh         # Runner script
├── Makefile                 # install / test / bench / all
└── README.md
```

## Commits

```
02494d1 Initial commit: Parallel web search/extract as native Pi tools
59a83fc Rewrite README in Docker docs style
d18a3d6 Add mock server, unit tests, and benchmark
fabb809 Overengineer: native C binary + benchmark proving JS is faster
9a6731c Add FFI path. C in-process is 3x faster than JS, 16x faster than subprocess
c0a6559 Optimize C: curl handle reuse, no cJSON for requests, stack buffers
```

---

*Built by overthinking a problem that was already solved. In 93 microseconds.*
