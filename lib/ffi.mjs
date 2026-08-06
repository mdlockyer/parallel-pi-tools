// lib/ffi.mjs — FFI wrapper for the native shared library.
// Calls C functions in-process, zero spawn overhead.

import koffi from "koffi";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { existsSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));

// Find the shared library
const libCandidates = [
  join(__dirname, "..", "native", "libparallel.dylib"),
  join(__dirname, "..", "native", "libparallel.so"),
];

const libPath = libCandidates.find((p) => existsSync(p));
if (!libPath) {
  throw new Error(
    `Native library not found. Run 'make' in native/ first.\nLooked in: ${libCandidates.join(", ")}`
  );
}

// Load and define the C functions
const lib = koffi.load(libPath);

const parallel_init = lib.func("void parallel_init()");
const parallel_find_key = lib.func("char *parallel_find_key()");
const parallel_search = lib.func("char *parallel_search(char *api_base, char *api_key, char *objective)");
const parallel_extract = lib.func("char *parallel_extract(char *api_base, char *api_key, char *urls_csv, char *objective)");
const parallel_search_auto = lib.func("char *parallel_search_auto(char *api_base, char *objective)");
const parallel_extract_auto = lib.func("char *parallel_extract_auto(char *api_base, char *urls_csv, char *objective)");

// Initialize curl on module load
parallel_init();

/**
 * Find API key from environment (C-side regex scan).
 * @returns {string|null}
 */
export function ffiFindKey() {
  return parallel_find_key();
}

/**
 * Search via FFI (in-process C call).
 * @param {string} apiBase
 * @param {string} apiKey
 * @param {string} objective
 * @returns {string}
 */
export function ffiSearch(apiBase, apiKey, objective) {
  const result = parallel_search(apiBase, apiKey, objective);
  if (!result) throw new Error("ffiSearch returned null");
  return result;
}

/**
 * Extract via FFI (in-process C call).
 * @param {string} apiBase
 * @param {string} apiKey
 * @param {string[]} urls
 * @param {string} [objective]
 * @returns {string}
 */
export function ffiExtract(apiBase, apiKey, urls, objective) {
  const result = parallel_extract(apiBase, apiKey, urls.join(","), objective || "");
  if (!result) throw new Error("ffiExtract returned null");
  return result;
}

/**
 * Search with auto-detected API key (full C pipeline).
 * @param {string} apiBase
 * @param {string} objective
 * @returns {string}
 */
export function ffiSearchAuto(apiBase, objective) {
  const result = parallel_search_auto(apiBase, objective);
  if (!result) throw new Error("ffiSearchAuto returned null");
  return result;
}

/**
 * Extract with auto-detected API key (full C pipeline).
 * @param {string} apiBase
 * @param {string[]} urls
 * @param {string} [objective]
 * @returns {string}
 */
export function ffiExtractAuto(apiBase, urls, objective) {
  const result = parallel_extract_auto(apiBase, urls.join(","), objective || "");
  if (!result) throw new Error("ffiExtractAuto returned null");
  return result;
}
