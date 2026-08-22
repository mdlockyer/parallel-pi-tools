// test/install.mjs — Installation regression tests.

import { after, before, describe, it } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, mkdirSync, readFileSync, rmSync, statSync, existsSync } from "node:fs";
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

  it("replaces the installed dylib inode on every install", () => {
    const makeArgs = ["install", `PI_EXTENSIONS_DIR=${extensionsDir}`];
    const installedLibrary = join(extensionsDir, libRelDir, libraryName);
    const sourceLibrary = join(repoDir, "native", libraryName);

    execFileSync("make", makeArgs, { cwd: repoDir, stdio: "pipe" });
    const firstInode = statSync(installedLibrary).ino;

    execFileSync("make", makeArgs, { cwd: repoDir, stdio: "pipe" });
    const secondInode = statSync(installedLibrary).ino;

    assert.notEqual(secondInode, firstInode);
    assert.deepEqual(readFileSync(installedLibrary), readFileSync(sourceLibrary));
  });
});
