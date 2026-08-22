// test/install.mjs — Installation regression tests.

import { after, before, describe, it } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  appendFileSync,
  mkdtempSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  rmSync,
  existsSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const testDir = dirname(fileURLToPath(import.meta.url));
const repoDir = join(testDir, "..");
const libraryName = process.platform === "darwin" ? "libparallel.dylib" : "libparallel.so";
const libRelDir = "lib/web-search";

describe("native library installation", () => {
  let extensionsDir;

  before(() => {
    extensionsDir = mkdtempSync(join(tmpdir(), "parallel-pi-install-"));
    mkdirSync(join(extensionsDir, "node_modules", "koffi"), { recursive: true });
  });

  after(() => {
    rmSync(extensionsDir, { recursive: true, force: true });
  });

  it("installs the extension + lib directory", () => {
    const makeArgs = ["install", `PI_EXTENSIONS_DIR=${extensionsDir}`];
    execFileSync("make", makeArgs, { cwd: repoDir, stdio: "pipe" });

    assert.ok(existsSync(join(extensionsDir, "web-search.ts")));
    assert.ok(existsSync(join(extensionsDir, libRelDir, "parallel.mjs")));
    assert.ok(existsSync(join(extensionsDir, libRelDir, "ffi.mjs")));
    assert.ok(existsSync(join(extensionsDir, libRelDir, libraryName)));
  });

  it("replaces the installed library wholesale on every install", () => {
    const makeArgs = ["install", `PI_EXTENSIONS_DIR=${extensionsDir}`];
    const installedLibrary = join(extensionsDir, libRelDir, libraryName);
    const sourceLibrary = join(repoDir, "native", libraryName);

    execFileSync("make", makeArgs, { cwd: repoDir, stdio: "pipe" });

    // Corrupt the installed copy so the next install has to genuinely
    // replace the file rather than touch it in place.
    appendFileSync(installedLibrary, Buffer.from("\n-- CORRUPTION --\n"));

    execFileSync("make", makeArgs, { cwd: repoDir, stdio: "pipe" });

    // Installed content restored byte-for-byte.
    assert.deepEqual(readFileSync(installedLibrary), readFileSync(sourceLibrary));
    // Install uses atomic rename into place, so no temp files survive.
    const leftovers = readdirSync(join(extensionsDir, libRelDir)).filter((f) =>
      f.startsWith(`.${libraryName}.`)
    );
    assert.deepEqual(leftovers, []);
  });
});
