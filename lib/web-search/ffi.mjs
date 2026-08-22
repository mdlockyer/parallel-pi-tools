// lib/ffi.mjs — FFI wrapper for the native shared library.
// Calls C functions in-process via koffi async FFI (worker thread), so the C
// HTTP call never blocks the Node event loop — the extension and the Pi TUI
// stay responsive while libcurl runs.
//
// Degrades to null rather than throwing when koffi or the shared library is
// missing, so the extension can fall back to the JS path.

import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { existsSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));

const require = createRequire(import.meta.url);
let koffi = null;
try {
  koffi = require("koffi");
} catch {
  // not installed — native unavailable
}

const libCandidates = [
  join(__dirname, "libparallel.dylib"),
  join(__dirname, "libparallel.so"),
  join(__dirname, "..", "..", "native", "libparallel.dylib"),
  join(__dirname, "..", "..", "native", "libparallel.so"),
];
const libPath = libCandidates.find((p) => existsSync(p));

let lib = null;
if (koffi && libPath) {
  try {
    lib = koffi.load(libPath);
  } catch (err) {
    console.error(`[parallel] failed to load native lib: ${err.message}`);
  }
}

// C functions returning malloc'd char* are wrapped in a disposable type so
// koffi decodes them into JS strings and releases the buffer via parallel_free
// — no leaks on repeated calls.
const parallel_free = lib ? lib.func("void parallel_free(char *ptr)") : null;
const DStr = lib
  ? koffi.disposable(koffi.pointer(koffi.types.char), (ptr) => parallel_free(ptr))
  : null;

function makeAsync(fn) {
  if (!fn) return null;
  return (...args) =>
    new Promise((resolve, reject) => {
      fn.async(...args, (err, result) => {
        if (err) {
          reject(err instanceof Error ? err : new Error(String(err)));
          return;
        }
        resolve(result);
      });
    });
}

// Full-surface signatures: JSON arrays are passed as pre-stringified strings
// from JS (JSON.stringify), avoiding any per-element marshal or CSV ambiguity
// and eliminating the comma-in-URL bug entirely.
//
// Error convention: on failure, C functions return a malloc'd string starting
// with "PARALLEL_ERROR: " (HTTP status, JSON-RPC error, transport failure).
// lib/web-search/parallel.mjs unwraps that prefix into thrown Errors.
const _findKey = makeAsync(lib && DStr ? lib.func("parallel_find_key", DStr, []) : null);
const _search = makeAsync(
  lib && DStr ? lib.func("parallel_search", DStr, ["char *", "char *", "char *", "char *"]) : null,
);
const _extract = makeAsync(
  lib && DStr ? lib.func("parallel_extract", DStr, ["char *", "char *", "char *", "char *"]) : null,
);

// MCP fallback signatures (free tier, no API key required).
const _mcpSearch = makeAsync(
  lib && DStr ? lib.func("parallel_mcp_search", DStr, ["char *", "char *", "char *"]) : null,
);
const _mcpFetch = makeAsync(
  lib && DStr ? lib.func("parallel_mcp_fetch", DStr, ["char *", "char *", "char *"]) : null,
);

function requireNative() {
  if (!lib) {
    throw new Error("Native library not available (run make in native/ and install koffi)");
  }
}

/** Find API key from environment (C-side scan). Resolves null if not found. */
export async function ffiFindKey() {
  if (!lib) return null;
  return _findKey();
}

/**
 * Search via async FFI (in-process C call on a worker thread).
 * @param {string} apiBase
 * @param {string} apiKey
 * @param {string} objective
 * @param {string[]} [searchQueries] - optional array; when omitted defaults to [objective]
 */
export async function ffiSearch(apiBase, apiKey, objective, searchQueries) {
  requireNative();
  const queriesJson = Array.isArray(searchQueries) ? JSON.stringify(searchQueries) : null;
  const result = await _search(apiBase, apiKey, objective, queriesJson);
  if (!result) throw new Error("ffiSearch returned null");
  return result;
}

/**
 * Extract via async FFI (in-process C call on a worker thread).
 * @param {string} apiBase
 * @param {string} apiKey
 * @param {string[]} urls
 * @param {string} [objective]
 */
export async function ffiExtract(apiBase, apiKey, urls, objective) {
  requireNative();
  const urlsJson = JSON.stringify(urls);
  const result = await _extract(apiBase, apiKey, urlsJson, objective || null);
  if (!result) throw new Error("ffiExtract returned null");
  return result;
}

/**
 * Availability + direct C entry points (all async) for callers that dispatch
 * on it. `native` is null when the library or koffi is unavailable.
 *
 * `search` expects a pre-stringified JSON array for search_queries (or null
 * to default to [objective]). `extract` expects a pre-stringified JSON array
 * for urls. This keeps the JS→C boundary to a single cheap pointer per array
 * and lets the caller handle JSON escaping via JSON.stringify (≈1µs for
 * typical payloads).
 *
 * `mcpSearch` and `mcpFetch` are the free MCP fallback (no API key). They
 * take the same shapes but route through the MCP endpoint via C.
 */
export const native = lib
  ? {
      isAvailable: true,
      findKey: () => _findKey(),
      search: (apiBase, apiKey, objective, searchQueriesJson) =>
        _search(apiBase, apiKey, objective, searchQueriesJson ?? null),
      extract: (apiBase, apiKey, urlsJson, objective) =>
        _extract(apiBase, apiKey, urlsJson, objective ?? null),
      mcpSearch: (objective, searchQueriesJson, mcpUrl) =>
        _mcpSearch(objective, searchQueriesJson ?? null, mcpUrl ?? null),
      mcpFetch: (urlsJson, objective, mcpUrl) =>
        _mcpFetch(urlsJson, objective ?? null, mcpUrl ?? null),
    }
  : null;
