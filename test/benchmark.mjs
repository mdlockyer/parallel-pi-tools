// test/benchmark.mjs — Measures overhead introduced by the tool code layer.
// Compares direct fetch vs lib wrappers (MCP path and API path).
// Run: node test/benchmark.mjs

import { createMockServer } from "./mock-server.mjs";
import {
  callMcpTool,
  apiSearch,
  apiExtract,
  extractMcpText,
} from "../lib/parallel.mjs";

const ITERATIONS = 100;
const WARMUP = 10;

function percentile(sorted, p) {
  const idx = Math.ceil((p / 100) * sorted.length) - 1;
  return sorted[Math.max(0, idx)];
}

function stats(times) {
  const sorted = [...times].sort((a, b) => a - b);
  const sum = sorted.reduce((a, b) => a + b, 0);
  return {
    min: sorted[0],
    p50: percentile(sorted, 50),
    p95: percentile(sorted, 95),
    p99: percentile(sorted, 99),
    max: sorted[sorted.length - 1],
    avg: sum / sorted.length,
    total: sum,
  };
}

function fmt(ms) {
  return ms < 1 ? `${(ms * 1000).toFixed(0)}µs` : `${ms.toFixed(2)}ms`;
}

async function bench(name, fn, iterations = ITERATIONS) {
  // Warmup
  for (let i = 0; i < WARMUP; i++) await fn();

  const times = [];
  for (let i = 0; i < iterations; i++) {
    const start = performance.now();
    await fn();
    times.push(performance.now() - start);
  }

  const s = stats(times);
  console.log(`\n  ${name}`);
  console.log(`  ${"─".repeat(50)}`);
  console.log(`  avg ${fmt(s.avg)}  |  p50 ${fmt(s.p50)}  |  p95 ${fmt(s.p95)}  |  p99 ${fmt(s.p99)}`);
  console.log(`  min ${fmt(s.min)}  |  max ${fmt(s.max)}  |  ${iterations} iters`);
  return s;
}

async function rawFetch(url, body) {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json", "x-api-key": "bench" },
    body: JSON.stringify(body),
  });
  await res.json();
}

async function main() {
  console.log(`\nparallel-pi-tools benchmark`);
  console.log(`iterations: ${ITERATIONS}  |  warmup: ${WARMUP}\n`);

  const server = await createMockServer({ port: 0 });
  console.log(`mock server: ${server.url}`);

  try {
    // --- Baseline: raw fetch to mock API ---
    const baselineSearch = await bench(
      "baseline: raw fetch /v1/search",
      () => rawFetch(`${server.url}/v1/search`, { objective: "test" })
    );

    const baselineExtract = await bench(
      "baseline: raw fetch /v1/extract",
      () => rawFetch(`${server.url}/v1/extract`, { urls: ["https://example.com"] })
    );

    const baselineMcp = await bench(
      "baseline: raw fetch /mcp (search)",
      () =>
        rawFetch(`${server.url}/mcp`, {
          jsonrpc: "2.0",
          id: 1,
          method: "tools/call",
          params: { name: "web_search", arguments: { objective: "test" } },
        })
    );

    // --- Tool layer: lib wrappers (API path) ---
    const toolSearch = await bench(
      "tool: apiSearch (lib wrapper)",
      () => apiSearch("bench-key", "test", [], { apiBase: server.url + "/v1" })
    );

    const toolExtract = await bench(
      "tool: apiExtract (lib wrapper)",
      () => apiExtract("bench-key", ["https://example.com"], undefined, { apiBase: server.url + "/v1" })
    );

    // --- Tool layer: lib wrappers (MCP path) ---
    const toolMcpSearch = await bench(
      "tool: callMcpTool web_search (MCP path)",
      () => callMcpTool("web_search", { objective: "test" }, { mcpUrl: server.url + "/mcp" })
    );

    const toolMcpExtract = await bench(
      "tool: callMcpTool web_fetch (MCP path)",
      () => callMcpTool("web_fetch", { urls: ["https://example.com"] }, { mcpUrl: server.url + "/mcp" })
    );

    // --- Overhead summary ---
    console.log(`\n${"═".repeat(54)}`);
    console.log(`  OVERHEAD SUMMARY (avg)`);
    console.log(`${"═".repeat(54)}`);

    const searchOverhead = toolSearch.avg - baselineSearch.avg;
    const extractOverhead = toolExtract.avg - baselineExtract.avg;
    const mcpOverhead = toolMcpSearch.avg - baselineMcp.avg;

    console.log(`  Search (API):  ${fmt(searchOverhead)} tool overhead  (${((searchOverhead / baselineSearch.avg) * 100).toFixed(1)}%)`);
    console.log(`  Extract (API): ${fmt(extractOverhead)} tool overhead  (${((extractOverhead / baselineExtract.avg) * 100).toFixed(1)}%)`);
    console.log(`  Search (MCP):  ${fmt(mcpOverhead)} tool overhead  (${((mcpOverhead / baselineMcp.avg) * 100).toFixed(1)}%)`);
    console.log(`${"═".repeat(54)}\n`);
  } finally {
    await server.close();
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
