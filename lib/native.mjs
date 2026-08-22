// lib/native.mjs — Wrapper for the native C binary (subprocess benchmarking).
// Spawns the binary as a subprocess, reads stdout.
// Unlike the FFI path, this incurs ~4ms fork/exec overhead per call.

import { execFile } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);
const __dirname = dirname(fileURLToPath(import.meta.url));
const BINARY = join(__dirname, "..", "native", "parallel");

/**
 * Search via native binary.
 * @param {string} apiBase
 * @param {string} apiKey - passed via PARALLEL_API_KEY env, never argv
 *   (command-line arguments are visible to every local user via `ps`)
 * @param {string} objective
 * @param {string[]} [searchQueries] - optional
 * @returns {Promise<string>}
 */
export async function nativeSearch(apiBase, apiKey, objective, searchQueries) {
  const args = ["search", apiBase, "-", objective];
  if (Array.isArray(searchQueries) && searchQueries.length) {
    args.push(JSON.stringify(searchQueries));
  }
  const env = { ...process.env, PARALLEL_API_KEY: apiKey };
  const { stdout } = await execFileAsync(BINARY, args, { timeout: 30_000, env });
  return stdout;
}

/**
 * Extract via native binary.
 * @param {string} apiBase
 * @param {string} apiKey - passed via PARALLEL_API_KEY env, never argv
 * @param {string[]} urls
 * @param {string} [objective]
 * @returns {Promise<string>}
 */
export async function nativeExtract(apiBase, apiKey, urls, objective) {
  const args = ["extract", apiBase, "-", JSON.stringify(urls)];
  if (objective) args.push(objective);
  const env = { ...process.env, PARALLEL_API_KEY: apiKey };
  const { stdout } = await execFileAsync(BINARY, args, { timeout: 30_000, env });
  return stdout;
}
