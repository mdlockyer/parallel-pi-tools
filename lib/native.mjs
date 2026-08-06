// lib/native.mjs — Wrapper for the native C binary.
// Spawns the binary as a subprocess, reads stdout.

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
 * @param {string} apiKey
 * @param {string} objective
 * @returns {Promise<string>}
 */
export async function nativeSearch(apiBase, apiKey, objective) {
  const { stdout } = await execFileAsync(BINARY, [
    "search", apiBase, apiKey, objective,
  ], { timeout: 30_000 });
  return stdout;
}

/**
 * Extract via native binary.
 * @param {string} apiBase
 * @param {string} apiKey
 * @param {string[]} urls
 * @param {string} [objective]
 * @returns {Promise<string>}
 */
export async function nativeExtract(apiBase, apiKey, urls, objective) {
  const args = ["extract", apiBase, apiKey, urls.join(",")];
  if (objective) args.push(objective);
  const { stdout } = await execFileAsync(BINARY, args, { timeout: 30_000 });
  return stdout;
}
