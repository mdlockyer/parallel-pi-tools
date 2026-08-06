# parallel-pi-tools

This extension adds real-time web search and URL content extraction to [Pi](https://github.com/badlogic/pi-mono/) as native tools.

## Overview

The extension registers two tools:

| Tool | Description |
|------|-------------|
| `web_search` | Search the web with a natural language objective. Returns ranked URLs and LLM-optimized excerpts. |
| `web_fetch` | Extract clean markdown from one or more URLs. Supports JS-heavy pages and PDFs. |

## Prerequisites

- You have [Pi](https://github.com/badlogic/pi-mono/) installed.
- (Optional) You have a [Parallel API key](https://platform.parallel.ai) for higher rate limits and direct API access.

## Install

Clone this repository and run `make install`:

```bash
git clone https://github.com/<your-username>/parallel-pi-tools.git
cd parallel-pi-tools
make install
```

This copies `parallel-search.ts` to `~/.pi/agent/extensions/`. Run `/reload` in Pi to activate.

## Configuration

The extension auto-detects your environment:

- **API key found** — Calls `https://api.parallel.ai/v1/` directly using your key.
- **No API key** — Falls back to the free MCP endpoint at `https://search.parallel.ai/mcp`.

To use direct API mode, set any of the following environment variables:

```bash
export PARALLEL_API_KEY="your-key-here"
```

The extension matches common variations: `PARALLEL_API_KEY`, `PARALLEL_KEY`, `PARALLELSECRET`, `PARALLEL_TOKEN`, and case-insensitive variants.

## Usage

Once installed, Pi has two new tools. Call them directly:

```bash
# Search the web
web_search --objective "What's new in Node.js 22?"

# Fetch content from a URL
web_fetch --urls '["https://example.com/article"]'
```

## Uninstall

```bash
make uninstall
```

Then restart Pi or run `/reload` to remove the tools from your session.
