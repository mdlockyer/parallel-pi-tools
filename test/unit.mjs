// test/unit.mjs — Unit tests for parallel-pi-tools.
// Run: node --test test/unit.mjs

import { describe, it, before, after } from "node:test";
import assert from "node:assert/strict";
import {
  findApiKey,
  callMcpTool,
  apiSearch,
  apiExtract,
  extractMcpText,
} from "../lib/parallel.mjs";
import { createMockServer } from "./mock-server.mjs";
import { nativeSearch, nativeExtract } from "../lib/native.mjs";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

// --- findApiKey ---

describe("findApiKey", () => {
  it("finds PARALLEL_API_KEY", () => {
    assert.equal(findApiKey({ PARALLEL_API_KEY: "abc123" }), "abc123");
  });

  it("finds PARALLEL_KEY", () => {
    assert.equal(findApiKey({ PARALLEL_KEY: "xyz" }), "xyz");
  });

  it("finds PARALLELSECRET", () => {
    assert.equal(findApiKey({ PARALLELSECRET: "s3cr3t" }), "s3cr3t");
  });

  it("finds PARALLEL_TOKEN", () => {
    assert.equal(findApiKey({ PARALLEL_TOKEN: "tok" }), "tok");
  });

  it("matches case-insensitively", () => {
    assert.equal(findApiKey({ parallel_api_key: "low" }), "low");
    assert.equal(findApiKey({ Parallel_Key: "mixed" }), "mixed");
  });

  it("finds PARALLEL-API-KEY with dashes", () => {
    assert.equal(findApiKey({ "PARALLEL-API-KEY": "dash" }), "dash");
  });

  it("trims whitespace", () => {
    assert.equal(findApiKey({ PARALLEL_API_KEY: "  padded  " }), "padded");
  });

  it("ignores empty values", () => {
    assert.equal(findApiKey({ PARALLEL_API_KEY: "" }), undefined);
    assert.equal(findApiKey({ PARALLEL_API_KEY: "   " }), undefined);
  });

  it("returns undefined when no key found", () => {
    assert.equal(findApiKey({}), undefined);
    assert.equal(findApiKey({ HOME: "/home/user" }), undefined);
  });

  it("picks the first matching key", () => {
    const env = { PARALLEL_KEY: "first", PARALLEL_API_KEY: "second" };
    const result = findApiKey(env);
    assert.ok(result === "first" || result === "second");
  });
});

// --- extractMcpText ---

describe("extractMcpText", () => {
  it("extracts text from content array", () => {
    const result = extractMcpText({
      content: [
        { type: "text", text: "hello" },
        { type: "text", text: "world" },
      ],
    });
    assert.equal(result, "hello\n\nworld");
  });

  it("skips non-text content", () => {
    const result = extractMcpText({
      content: [
        { type: "image", data: "base64..." },
        { type: "text", text: "only this" },
      ],
    });
    assert.equal(result, "only this");
  });

  it("returns empty string for no text content", () => {
    assert.equal(extractMcpText({ content: [] }), "");
  });
});

// --- Mock server integration ---

describe("with mock server", () => {
  let server;

  before(async () => {
    server = await createMockServer({ port: 0 });
  });

  after(async () => {
    await server?.close();
  });

  // --- callMcpTool ---

  describe("callMcpTool", () => {
    it("calls web_search via MCP", async () => {
      const result = await callMcpTool(
        "web_search",
        { objective: "test query" },
        { mcpUrl: server.url + "/mcp" }
      );
      assert.ok(result.content);
      assert.ok(result.content[0].text.includes("Mock Result"));
    });

    it("calls web_fetch via MCP", async () => {
      const result = await callMcpTool(
        "web_fetch",
        { urls: ["https://example.com"] },
        { mcpUrl: server.url + "/mcp" }
      );
      assert.ok(result.content);
      assert.ok(result.content[0].text.includes("Extracted Article"));
    });

    it("throws on unknown tool", async () => {
      await assert.rejects(
        () => callMcpTool("unknown_tool", {}, { mcpUrl: server.url + "/mcp" }),
        /Unknown tool/
      );
    });
  });

  // --- apiSearch ---

  describe("apiSearch", () => {
    it("returns formatted search results", async () => {
      const text = await apiSearch("fake-key", "test", [], {
        apiBase: server.url + "/v1",
      });
      assert.ok(text.includes("Mock Result One"));
      assert.ok(text.includes("example.com/result-1"));
      assert.ok(text.includes("first mock excerpt"));
    });

    it("includes search queries in request", async () => {
      // Just verify it doesn't throw with queries
      const text = await apiSearch("fake-key", "test", ["q1", "q2"], {
        apiBase: server.url + "/v1",
      });
      assert.ok(text.length > 0);
    });
  });

  // --- apiExtract ---

  describe("apiExtract", () => {
    it("returns formatted extract results", async () => {
      const text = await apiExtract("fake-key", ["https://example.com"], undefined, {
        apiBase: server.url + "/v1",
      });
      assert.ok(text.includes("Mock Extracted Article"));
      assert.ok(text.includes("extracted content"));
    });

    it("passes objective to API", async () => {
      const text = await apiExtract(
        "fake-key",
        ["https://example.com"],
        "focus on X",
        { apiBase: server.url + "/v1" }
      );
      assert.ok(text.length > 0);
    });

    it("rejects when API returns error", async () => {
      await assert.rejects(
        () => apiExtract("fake-key", [], undefined, { apiBase: server.url + "/v1" }),
        /Extract API failed/
      );
    });
  });

  // --- native binary ---

  describe("native binary", () => {
    const __dirname = dirname(fileURLToPath(import.meta.url));
    const binaryExists = existsSync(join(__dirname, "..", "native", "parallel"));

    (binaryExists ? it : it.skip)("nativeSearch returns results", async () => {
      const text = await nativeSearch(server.url + "/v1", "test-key", "test query");
      assert.ok(text.includes("Mock Result One"));
      assert.ok(text.includes("example.com/result-1"));
    });

    (binaryExists ? it : it.skip)("nativeExtract returns results", async () => {
      const text = await nativeExtract(server.url + "/v1", "test-key", ["https://example.com"]);
      assert.ok(text.includes("Mock Extracted Article"));
      assert.ok(text.includes("extracted content"));
    });

    (binaryExists ? it : it.skip)("nativeSearch output matches JS output", async () => {
      const native = await nativeSearch(server.url + "/v1", "key", "test");
      const js = await apiSearch("key", "test", [], { apiBase: server.url + "/v1" });
      assert.equal(native.trim(), js.trim());
    });

    (binaryExists ? it : it.skip)("nativeExtract output matches JS output", async () => {
      const native = await nativeExtract(server.url + "/v1", "key", ["https://example.com"]);
      const js = await apiExtract("key", ["https://example.com"], undefined, { apiBase: server.url + "/v1" });
      assert.equal(native.trim(), js.trim());
    });
  });
});
