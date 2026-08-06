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
 * @returns {Promise<{ url: string, port: number, close: () => Promise<void> }>}
 */
export function createMockServer({ port = 0, delayMs = 0 } = {}) {
  return new Promise((resolve, reject) => {
    const server = createServer(async (req, res) => {
      if (delayMs > 0) {
        await new Promise((r) => setTimeout(r, delayMs));
      }

      const url = new URL(req.url, `http://localhost`);

      // --- Parallel REST API ---
      if (req.method === "POST" && url.pathname === "/v1/search") {
        const body = JSON.parse(await readBody(req));
        if (!body.objective) return json(res, 400, { error: "missing objective" });
        return json(res, 200, MOCK_SEARCH_RESPONSE);
      }

      if (req.method === "POST" && url.pathname === "/v1/extract") {
        const body = JSON.parse(await readBody(req));
        if (!body.urls?.length) return json(res, 400, { error: "missing urls" });
        return json(res, 200, MOCK_EXTRACT_RESPONSE);
      }

      // --- MCP endpoint ---
      if (req.method === "POST" && url.pathname === "/mcp") {
        const body = JSON.parse(await readBody(req));
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
