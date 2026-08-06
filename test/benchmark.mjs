// test/benchmark.mjs — Measures overhead introduced by the tool code layer.
// Compares: raw fetch | JS lib | MCP path | subprocess | FFI
// Run: node test/benchmark.mjs

import { execFile } from "node:child_process";
import { writeFileSync, readFileSync, unlinkSync } from "node:fs";
import { promisify } from "node:util";
import {
  callMcpTool,
  apiSearch,
  apiExtract,
} from "../lib/parallel.mjs";
import { nativeSearch, nativeExtract } from "../lib/native.mjs";

const execFileAsync = promisify(execFile);
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
  };
}

function fmt(ms) {
  return ms < 1 ? `${(ms * 1000).toFixed(0)}µs` : `${ms.toFixed(2)}ms`;
}

async function bench(name, fn, iterations = ITERATIONS) {
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

async function startMockServer() {
  const proc = execFile("node", ["test/mock-server.mjs", "0"], {
    cwd: new URL("..", import.meta.url).pathname,
  });

  return new Promise((resolve, reject) => {
    let output = "";
    const timeout = setTimeout(() => reject(new Error("mock server timeout")), 5000);

    proc.stdout.on("data", (data) => {
      output += data.toString();
      const match = output.match(/Mock server listening on (http:\/\/[^\s]+)/);
      if (match) {
        clearTimeout(timeout);
        resolve({ url: match[1], proc });
      }
    });

    proc.stderr.on("data", (data) => {
      output += data.toString();
    });
  });
}

async function main() {
  console.log(`\nparallel-pi-tools benchmark`);
  console.log(`iterations: ${ITERATIONS}  |  warmup: ${WARMUP}`);

  // Start mock server as separate process (needed for FFI + subprocess)
  const mock = await startMockServer();
  console.log(`mock server: ${mock.url} (pid ${mock.proc.pid})\n`);

  try {
    // ── baselines ──

    const baselineSearch = await bench(
      "baseline: raw fetch /v1/search",
      () => rawFetch(`${mock.url}/v1/search`, { objective: "test" })
    );

    const baselineExtract = await bench(
      "baseline: raw fetch /v1/extract",
      () => rawFetch(`${mock.url}/v1/extract`, { urls: ["https://example.com"] })
    );

    const baselineMcp = await bench(
      "baseline: raw fetch /mcp (search)",
      () =>
        rawFetch(`${mock.url}/mcp`, {
          jsonrpc: "2.0", id: 1, method: "tools/call",
          params: { name: "web_search", arguments: { objective: "test" } },
        })
    );

    // ── JS lib wrappers ──

    const toolSearch = await bench(
      "tool: apiSearch (JS lib)",
      () => apiSearch("bench-key", "test", [], { apiBase: mock.url + "/v1" })
    );

    const toolExtract = await bench(
      "tool: apiExtract (JS lib)",
      () => apiExtract("bench-key", ["https://example.com"], undefined, { apiBase: mock.url + "/v1" })
    );

    const toolMcpSearch = await bench(
      "tool: callMcpTool web_search (MCP)",
      () => callMcpTool("web_search", { objective: "test" }, { mcpUrl: mock.url + "/mcp" })
    );

    // ── subprocess (C binary) ──

    const subSearch = await bench(
      "native: C subprocess search",
      () => nativeSearch(mock.url + "/v1", "bench-key", "test")
    );

    const subExtract = await bench(
      "native: C subprocess extract",
      () => nativeExtract(mock.url + "/v1", "bench-key", ["https://example.com"])
    );

    // ── FFI (C in-process) ──

    let ffiSearchStats, ffiExtractStats;
    try {
      const { ffiSearch, ffiExtract, ffiSearchAuto, ffiExtractAuto, ffiFindKey } = await import("../lib/ffi.mjs");

      ffiSearchStats = await bench(
        "native: C FFI search (in-process)",
        () => ffiSearch(mock.url + "/v1", "bench-key", "test")
      );

      ffiExtractStats = await bench(
        "native: C FFI extract (in-process)",
        () => ffiExtract(mock.url + "/v1", "bench-key", ["https://example.com"])
      );

      // Key detection overhead
      await bench(
        "native: C key detection (getenv scan)",
        () => ffiFindKey(),
        1000
      );

      // Full pipeline: key detection + search in one FFI call
      await bench(
        "native: C FFI search_auto (full pipeline)",
        () => ffiSearchAuto(mock.url + "/v1", "test")
      );

      await bench(
        "native: C FFI extract_auto (full pipeline)",
        () => ffiExtractAuto(mock.url + "/v1", ["https://example.com"])
      );
    } catch (err) {
      console.log(`\n  ⚠ FFI not available: ${err.message}`);
    }

    // ── summary ──

    const line = "═".repeat(58);
    const thin = "─".repeat(58);

    console.log(`\n${line}`);
    console.log(`  OVERHEAD SUMMARY (avg)`);
    console.log(`${line}`);
    console.log(`  Search (JS):     ${fmt(toolSearch.avg - baselineSearch.avg)} overhead  vs baseline`);
    console.log(`  Extract (JS):    ${fmt(toolExtract.avg - baselineExtract.avg)} overhead  vs baseline`);
    console.log(`  Search (MCP):    ${fmt(toolMcpSearch.avg - baselineMcp.avg)} overhead  vs baseline`);
    console.log(`${thin}`);
    console.log(`  Search (sub):    ${fmt(subSearch.avg)} avg  (+${fmt(subSearch.avg - baselineSearch.avg)} vs baseline)`);
    console.log(`  Extract (sub):   ${fmt(subExtract.avg)} avg  (+${fmt(subExtract.avg - baselineExtract.avg)} vs baseline)`);

    if (ffiSearchStats) {
      console.log(`${thin}`);
      console.log(`  Search (FFI):    ${fmt(ffiSearchStats.avg)} avg  (+${fmt(ffiSearchStats.avg - baselineSearch.avg)} vs baseline)`);
      console.log(`  Extract (FFI):   ${fmt(ffiExtractStats.avg)} avg  (+${fmt(ffiExtractStats.avg - baselineExtract.avg)} vs baseline)`);
    }

    console.log(`${thin}`);
    console.log(`  JS vs subprocess:  JS faster by ${fmt(subSearch.avg - toolSearch.avg)}`);
    if (ffiSearchStats) {
      const ffiVsJs = toolSearch.avg - ffiSearchStats.avg;
      if (ffiVsJs > 0) {
        console.log(`  FFI vs JS:         FFI faster by ${fmt(ffiVsJs)}`);
      } else {
        console.log(`  FFI vs JS:         JS faster by ${fmt(Math.abs(ffiVsJs))}`);
      }
      console.log(`  FFI vs subprocess: FFI faster by ${fmt(subSearch.avg - ffiSearchStats.avg)}`);
    }
    console.log(`${line}\n`);
  } finally {
    mock.proc.kill();
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
