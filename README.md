# parallel-pi-tools

Parallel web search and content extraction as native Pi tools — no MCP adapter needed.

## What it gives you

| Tool | What it does |
|------|-------------|
| `web_search` | Natural language web search, returns ranked URLs with LLM-optimized excerpts |
| `web_fetch` | Extract clean markdown content from specific URLs |

## How it works

The extension auto-detects your setup:

- **API key found** (`PARALLEL_API_KEY` env var) → calls `https://api.parallel.ai/v1/` directly
- **No key** → falls back to the free MCP endpoint at `https://search.parallel.ai/mcp`

The regex covers common variations: `PARALLEL_API_KEY`, `PARALLEL_KEY`, `PARALLELSECRET`, `PARALLEL_TOKEN`, etc.

## Install

```bash
make install
```

Then run `/reload` in Pi (or restart Pi).

## Uninstall

```bash
make uninstall
```

## Requirements

- [Pi](https://github.com/badlogic/pi-mono/) coding agent
- Optional: [Parallel API key](https://platform.parallel.ai) for higher rate limits and direct API access
