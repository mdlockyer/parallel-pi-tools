// lib/parallel.mjs — Core logic, no Pi dependencies.
// Used by the extension and by tests/benchmarks.

const DEFAULT_MCP_URL = "https://search.parallel.ai/mcp";
const DEFAULT_API_BASE = "https://api.parallel.ai/v1";

// Matches: PARALLEL_API_KEY, PARALLEL_KEY, PARALLELSECRET, PARALLEL_TOKEN, etc.
const API_KEY_PATTERN = /^PARALLEL[_-]?(?:API[_-]?)?(?:KEY|SECRET|TOKEN)$/i;

export function findApiKey(env = process.env) {
  for (const [key, value] of Object.entries(env)) {
    if (API_KEY_PATTERN.test(key) && value && value.trim()) {
      return value.trim();
    }
  }
  return undefined;
}

export async function callMcpTool(
  toolName,
  args,
  { mcpUrl = DEFAULT_MCP_URL, fetchImpl = fetch } = {}
) {
  const body = {
    jsonrpc: "2.0",
    id: 1,
    method: "tools/call",
    params: { name: toolName, arguments: args },
  };

  const res = await fetchImpl(mcpUrl, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Accept: "application/json, text/event-stream",
    },
    body: JSON.stringify(body),
  });

  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(`MCP request failed (${res.status}): ${text.slice(0, 200)}`);
  }

  const contentType = res.headers.get("content-type") || "";

  if (contentType.includes("text/event-stream")) {
    const raw = await res.text();
    for (const line of raw.split("\n")) {
      if (line.startsWith("data: ")) {
        try {
          const parsed = JSON.parse(line.slice(6));
          if (parsed.result) return parsed.result;
          if (parsed.error) throw new Error(`MCP error: ${parsed.error.message}`);
        } catch {
          // skip non-JSON data lines
        }
      }
    }
    throw new Error("No valid MCP response found in SSE stream");
  }

  const parsed = await res.json();
  if (parsed.error) throw new Error(`MCP error: ${parsed.error.message}`);
  if (!parsed.result) throw new Error("Empty MCP response");
  return parsed.result;
}

export async function apiSearch(
  apiKey,
  objective,
  searchQueries,
  { apiBase = DEFAULT_API_BASE, fetchImpl = fetch } = {}
) {
  const body = { objective, search_queries: searchQueries?.length ? searchQueries : [objective] };

  const res = await fetchImpl(`${apiBase}/search`, {
    method: "POST",
    headers: { "Content-Type": "application/json", "x-api-key": apiKey },
    body: JSON.stringify(body),
  });

  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(`Search API failed (${res.status}): ${text.slice(0, 200)}`);
  }

  const data = await res.json();
  return data.results
    .map((r) => {
      const title = r.title ? `### ${r.title}\n` : "";
      const url = `URL: ${r.url}\n`;
      const excerpts = r.excerpts.map((e) => `> ${e}`).join("\n\n");
      return `${title}${url}\n${excerpts}`;
    })
    .join("\n\n---\n\n");
}

export async function apiExtract(
  apiKey,
  urls,
  objective,
  { apiBase = DEFAULT_API_BASE, fetchImpl = fetch } = {}
) {
  const body = { urls };
  if (objective) body.objective = objective;

  const res = await fetchImpl(`${apiBase}/extract`, {
    method: "POST",
    headers: { "Content-Type": "application/json", "x-api-key": apiKey },
    body: JSON.stringify(body),
  });

  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(`Extract API failed (${res.status}): ${text.slice(0, 200)}`);
  }

  const data = await res.json();
  return data.results
    .map((r) => {
      const title = r.title ? `### ${r.title}\n` : "";
      const url = `URL: ${r.url}\n`;
      const content = r.full_content || r.excerpts.join("\n\n");
      return `${title}${url}\n${content}`;
    })
    .join("\n\n---\n\n");
}

export function extractMcpText(result) {
  return result.content
    .filter((c) => c.type === "text" && c.text)
    .map((c) => c.text)
    .join("\n\n");
}
