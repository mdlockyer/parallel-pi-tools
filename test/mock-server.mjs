// test/mock-server.mjs — Lightweight mock of Parallel API + MCP endpoint.
// Returns realistic payloads with configurable latency.

import { createServer } from "node:http";

const MOCK_SEARCH_RESPONSE = {
  search_id: "search_mock_001",
  results: [
    {
      url: "https://example.com/result-1",
      title: "Mock Result One",
      publish_date: null,
      excerpts: [
        "This is the first mock excerpt from example.com. It contains relevant information about the search query.",
      ],
    },
    {
      url: "https://example.com/result-2",
      title: "Mock Result Two",
      publish_date: null,
      excerpts: [
        "This is the second mock excerpt. It provides additional context and details about the topic.",
      ],
    },
    {
      url: "https://example.com/result-3",
      title: "Mock Result Three",
      publish_date: null,
      excerpts: [
        "Third mock excerpt with supplementary information and related content for the query.",
      ],
    },
  ],
  warnings: null,
};

const MOCK_EXTRACT_RESPONSE = {
  extract_id: "extract_mock_001",
  results: [
    {
      url: "https://example.com/article",
      title: "Mock Extracted Article",
      publish_date: "2026-01-15",
      excerpts: [
        "This is the extracted content from the mock article. It simulates clean markdown output from the Parallel Extract API.",
        "A second excerpt provides additional detail about the article's content structure.",
      ],
    },
  ],
};

const MOCK_MCP_SEARCH_RESULT = {
  content: [
    {
      type: "text",
      text: MOCK_SEARCH_RESPONSE.results
        .map((r) => `### ${r.title}\nURL: ${r.url}\n\n${r.excerpts.map((e) => `> ${e}`).join("\n\n")}`)
        .join("\n\n---\n\n"),
    },
  ],
};

const MOCK_MCP_EXTRACT_RESULT = {
  content: [
    {
      type: "text",
      text: MOCK_EXTRACT_RESPONSE.results
        .map((r) => `### ${r.title}\nURL: ${r.url}\n\n${r.excerpts.join("\n\n")}`)
        .join("\n\n---\n\n"),
    },
  ],
};

const MOCK_SESSION_ID = "mock-session-0123456789abcdef";

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => resolve(Buffer.concat(chunks).toString()));
    req.on("error", reject);
  });
}

function json(res, status, data) {
  res.writeHead(status, { "Content-Type": "application/json" });
  res.end(JSON.stringify(data));
}

/**
 * Create and start a mock server.
 * @param {object} opts
 * @param {number} opts.port - Port to listen on (0 for random)
 * @param {number} opts.delayMs - Artificial delay in ms (default 0)
 * @param {number|null} opts.searchErrorStatus - When set, /v1/search fails
 *   with this HTTP status (body carries a JSON error message) so callers can
 *   exercise error propagation.
 * @returns {Promise<{ url: string, port: number, close: () => Promise<void> }>}
 */
export function createMockServer({ port = 0, delayMs = 0, searchErrorStatus = null } = {}) {
  return new Promise((resolve, reject) => {
    // Mutable so tests can toggle failure mode between calls.
    const state = { searchErrorStatus };
    const server = createServer(async (req, res) => {
      if (delayMs > 0) {
        await new Promise((r) => setTimeout(r, delayMs));
      }

      const url = new URL(req.url, `http://localhost`);

      // --- Parallel REST API ---
      if (req.method === "POST" && url.pathname === "/v1/search") {
        const body = JSON.parse(await readBody(req));
        if (!body.objective) return json(res, 400, { error: "missing objective" });
        if (state.searchErrorStatus !== null) {
          return json(res, state.searchErrorStatus, {
            error: { code: -32000, message: `mock failure ${state.searchErrorStatus}` },
          });
        }
        return json(res, 200, MOCK_SEARCH_RESPONSE);
      }

      if (req.method === "POST" && url.pathname === "/v1/extract") {
        const body = JSON.parse(await readBody(req));
        if (!body.urls?.length) return json(res, 400, { error: "missing urls" });
        return json(res, 200, MOCK_EXTRACT_RESPONSE);
      }

      // --- MCP endpoint (streamable HTTP; speaks the initialize handshake
      // but also tolerates stateless direct tools/call) ---
      if (req.method === "POST" && url.pathname === "/mcp") {
        const body = JSON.parse(await readBody(req));

        if (body.method === "initialize") {
          res.writeHead(200, {
            "Content-Type": "application/json",
            "Mcp-Session-Id": MOCK_SESSION_ID,
          });
          res.end(
            JSON.stringify({
              jsonrpc: "2.0",
              id: body.id,
              result: {
                protocolVersion: body.params?.protocolVersion ?? "2025-06-18",
                capabilities: {},
                serverInfo: { name: "mock-mcp", version: "1.0.0" },
              },
            })
          );
          return;
        }

        // Notifications carry no id; acknowledge without a body per spec.
        if (body.method?.startsWith("notifications/")) {
          res.writeHead(202);
          res.end();
          return;
        }

        if (body.method !== "tools/call") {
          return json(res, 200, {
            jsonrpc: "2.0",
            id: body.id,
            error: { code: -32601, message: "Method not found" },
          });
        }

        const toolName = body.params?.name;
        let result;

        if (toolName === "web_search") {
          result = MOCK_MCP_SEARCH_RESULT;
        } else if (toolName === "web_fetch") {
          result = MOCK_MCP_EXTRACT_RESULT;
        } else {
          return json(res, 200, {
            jsonrpc: "2.0",
            id: body.id,
            error: { code: -32602, message: `Unknown tool: ${toolName}` },
          });
        }

        // Test hooks driven by magic markers in the arguments:
        // __mcp_error__ -> JSON-RPC error response
        // __sse__       -> event-stream response whose FIRST data event is a
        //                  result with no usable text (regression test for the
        //                  parse_sse_result double-free), followed by the real
        //                  result.
        const argsStr = JSON.stringify(body.params?.arguments ?? {});
        if (argsStr.includes("__mcp_error__")) {
          return json(res, 200, {
            jsonrpc: "2.0",
            id: body.id,
            error: { code: -32000, message: "mock mcp failure" },
          });
        }
        if (argsStr.includes("__sse__")) {
          res.writeHead(200, { "Content-Type": "text/event-stream" });
          const evt = (payload) => `event: message\ndata: ${JSON.stringify(payload)}\n\n`;
          res.end(
            evt({ jsonrpc: "2.0", id: body.id, result: { content: [{ type: "text", text: "" }] } }) +
            evt({ jsonrpc: "2.0", id: body.id, result })
          );
          return;
        }

        return json(res, 200, {
          jsonrpc: "2.0",
          id: body.id,
          result,
        });
      }

      json(res, 404, { error: "not found" });
    });

    server.listen(port, "127.0.0.1", () => {
      const addr = server.address();
      resolve({
        url: `http://127.0.0.1:${addr.port}`,
        port: addr.port,
        setSearchErrorStatus(v) { state.searchErrorStatus = v; },
        close: () => new Promise((r) => server.close(r)),
      });
    });

    server.on("error", reject);
  });
}

// Run standalone if executed directly
if (process.argv[1] === import.meta.filename) {
  const port = parseInt(process.argv[2] || "0", 10);
  const server = await createMockServer({ port });
  console.log(`Mock server listening on ${server.url}`);
  console.log("Press Ctrl+C to stop.");
}
