// lib/parallel.mjs — Core logic, no Pi dependencies.
// Uses the in-process C library via koffi FFI. No JS fallback.
// MCP fallback (free tier, no API key) is also routed through C.

import { native } from "./ffi.mjs";

const DEFAULT_API_BASE = "https://api.parallel.ai/v1";

// The C library returns failures as "PARALLEL_ERROR: <cause>" strings
// (HTTP status + response excerpt, JSON-RPC errors, transport failures).
const ERROR_PREFIX = "PARALLEL_ERROR:";

function unwrap(text, fallbackMessage) {
  if (typeof text === "string" && text.startsWith(ERROR_PREFIX)) {
    const cause = text.slice(ERROR_PREFIX.length).trim();
    throw new Error(cause || fallbackMessage);
  }
  return text;
}

/** Matches: PARALLEL_API_KEY, PARALLEL_KEY, PARALLELSECRET, PARALLEL_TOKEN, etc.
 *  Mirrored by is_parallel_key_name() in native/parallel_lib.c — keep in sync. */
const API_KEY_PATTERN = /^PARALLEL[_-]?(?:API[_-]?)?(?:KEY|SECRET|TOKEN)$/i;

/** True when the in-process C library is loaded. */
export function nativeActive() {
  return native !== null;
}

export function findApiKey(env = process.env) {
  for (const [key, value] of Object.entries(env)) {
    if (API_KEY_PATTERN.test(key) && value && value.trim()) {
      return value.trim();
    }
  }
  return undefined;
}

export async function apiSearch(
  apiKey,
  objective,
  searchQueries,
  { apiBase = DEFAULT_API_BASE } = {}
) {
  if (!native) throw new Error("Native library not available (run make native and install koffi)");
  const queries = searchQueries?.length ? searchQueries : [objective];
  const queriesJson = JSON.stringify(queries);
  const text = await native.search(apiBase, apiKey, objective, queriesJson);
  return unwrap(text, "Search API failed");
}

export async function apiExtract(
  apiKey,
  urls,
  objective,
  { apiBase = DEFAULT_API_BASE } = {}
) {
  if (!native) throw new Error("Native library not available (run make native and install koffi)");
  const urlsJson = JSON.stringify(urls);
  const text = await native.extract(apiBase, apiKey, urlsJson, objective);
  return unwrap(text, "Extract API failed");
}

/* ── MCP fallback (free tier, no API key) ──────────────────────── */

export async function mcpSearch(
  objective,
  searchQueries,
  { mcpUrl } = {}
) {
  if (!native) throw new Error("Native library not available (run make native and install koffi)");
  const queries = searchQueries?.length ? searchQueries : [objective];
  const queriesJson = JSON.stringify(queries);
  const text = await native.mcpSearch(objective, queriesJson, mcpUrl ?? null);
  return unwrap(text, "MCP search failed");
}

export async function mcpFetch(
  urls,
  objective,
  { mcpUrl } = {}
) {
  if (!native) throw new Error("Native library not available (run make native and install koffi)");
  const urlsJson = JSON.stringify(urls);
  const text = await native.mcpFetch(urlsJson, objective ?? null, mcpUrl ?? null);
  return unwrap(text, "MCP fetch failed");
}
