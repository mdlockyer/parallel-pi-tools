import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { Type } from "typebox";
import {
  truncateHead,
  DEFAULT_MAX_BYTES,
  DEFAULT_MAX_LINES,
  formatSize,
} from "@earendil-works/pi-coding-agent";

// --- Configuration ---

const MCP_URL = "https://search.parallel.ai/mcp";
const API_BASE = "https://api.parallel.ai/v1";

// Regex to match common Parallel API key env var names
// Matches: PARALLEL_API_KEY, PARALLEL_KEY, PARALLELSECRET, PARALLEL_TOKEN, etc.
const API_KEY_PATTERN = /^PARALLEL[_-]?(?:API[_-]?)?(?:KEY|SECRET|TOKEN)$/i;

// --- Types ---

interface McpToolResult {
  content: Array<{ type: string; text?: string; [key: string]: unknown }>;
  isError?: boolean;
}

interface McpJsonRpcResponse {
  jsonrpc: string;
  id: number;
  result?: McpToolResult;
  error?: { code: number; message: string; data?: unknown };
}

interface SearchResult {
  url: string;
  title?: string;
  publish_date?: string;
  excerpts: string[];
}

interface SearchResponse {
  search_id: string;
  results: SearchResult[];
  warnings?: unknown;
}

interface ExtractResult {
  url: string;
  title?: string;
  publish_date?: string;
  excerpts: string[];
  full_content?: string;
}

interface ExtractResponse {
  extract_id: string;
  results: ExtractResult[];
}

// --- API Key Detection ---

function findApiKey(): string | undefined {
  for (const [key, value] of Object.entries(process.env)) {
    if (API_KEY_PATTERN.test(key) && value && value.trim()) {
      return value.trim();
    }
  }
  return undefined;
}

// --- MCP Transport (fallback) ---

async function callMcpTool(
  toolName: string,
  args: Record<string, unknown>
): Promise<McpToolResult> {
  const body = {
    jsonrpc: "2.0",
    id: 1,
    method: "tools/call",
    params: {
      name: toolName,
      arguments: args,
    },
  };

  const res = await fetch(MCP_URL, {
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

  // Streamable HTTP can return SSE or direct JSON
  if (contentType.includes("text/event-stream")) {
    const raw = await res.text();
    const lines = raw.split("\n");
    for (const line of lines) {
      if (line.startsWith("data: ")) {
        try {
          const parsed = JSON.parse(line.slice(6)) as McpJsonRpcResponse;
          if (parsed.result) return parsed.result;
          if (parsed.error) throw new Error(`MCP error: ${parsed.error.message}`);
        } catch {
          // skip non-JSON data lines
        }
      }
    }
    throw new Error("No valid MCP response found in SSE stream");
  }

  const parsed = (await res.json()) as McpJsonRpcResponse;
  if (parsed.error) throw new Error(`MCP error: ${parsed.error.message}`);
  if (!parsed.result) throw new Error("Empty MCP response");
  return parsed.result;
}

function extractMcpText(result: McpToolResult): string {
  return result.content
    .filter((c) => c.type === "text" && c.text)
    .map((c) => c.text as string)
    .join("\n\n");
}

// --- Direct REST API ---

async function apiSearch(
  apiKey: string,
  objective: string,
  searchQueries?: string[]
): Promise<string> {
  const body: Record<string, unknown> = { objective };
  if (searchQueries?.length) body.search_queries = searchQueries;

  const res = await fetch(`${API_BASE}/search`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "x-api-key": apiKey,
    },
    body: JSON.stringify(body),
  });

  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(`Search API failed (${res.status}): ${text.slice(0, 200)}`);
  }

  const data = (await res.json()) as SearchResponse;
  return data.results
    .map((r) => {
      const title = r.title ? `### ${r.title}\n` : "";
      const url = `URL: ${r.url}\n`;
      const excerpts = r.excerpts.map((e) => `> ${e}`).join("\n\n");
      return `${title}${url}\n${excerpts}`;
    })
    .join("\n\n---\n\n");
}

async function apiExtract(
  apiKey: string,
  urls: string[],
  objective?: string
): Promise<string> {
  const body: Record<string, unknown> = { urls };
  if (objective) body.objective = objective;

  const res = await fetch(`${API_BASE}/extract`, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "x-api-key": apiKey,
    },
    body: JSON.stringify(body),
  });

  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(`Extract API failed (${res.status}): ${text.slice(0, 200)}`);
  }

  const data = (await res.json()) as ExtractResponse;
  return data.results
    .map((r) => {
      const title = r.title ? `### ${r.title}\n` : "";
      const url = `URL: ${r.url}\n`;
      const content = r.full_content || r.excerpts.join("\n\n");
      return `${title}${url}\n${content}`;
    })
    .join("\n\n---\n\n");
}

// --- Shared Helpers ---

function applyTruncation(text: string): { content: string; truncated: boolean } {
  const truncation = truncateHead(text, {
    maxLines: DEFAULT_MAX_LINES,
    maxBytes: DEFAULT_MAX_BYTES,
  });

  let output = truncation.content;
  if (truncation.truncated) {
    output += `\n\n[Output truncated: ${truncation.outputLines} of ${truncation.totalLines} lines (${formatSize(truncation.outputBytes)} of ${formatSize(truncation.totalBytes)})]`;
  }

  return { content: output, truncated: truncation.truncated };
}

// --- Extension ---

export default function (pi: ExtensionAPI) {
  const apiKey = findApiKey();
  const mode = apiKey ? "direct API" : "MCP (free, no key)";

  pi.registerTool({
    name: "web_search",
    label: "Web Search",
    description: `Search the web using Parallel Search. Returns ranked URLs with LLM-optimized excerpts. Supports natural language objectives. Mode: ${mode}`,
    promptSnippet: "Search the web for real-time information",
    promptGuidelines: [
      "Use web_search when you need current information from the web that is not in your training data.",
      "Use web_search to verify facts, find recent news, or research topics.",
    ],
    parameters: Type.Object({
      objective: Type.String({
        description:
          "Natural language description of what you are looking for",
      }),
      search_queries: Type.Optional(
        Type.Array(Type.String(), {
          description:
            "Optional specific search queries to supplement the objective",
        })
      ),
    }),
    async execute(_toolCallId, params) {
      const args: Record<string, unknown> = {
        objective: params.objective,
      };
      if (params.search_queries?.length) {
        args.search_queries = params.search_queries;
      }

      try {
        let text: string;

        if (apiKey) {
          text = await apiSearch(
            apiKey,
            params.objective,
            params.search_queries
          );
        } else {
          const result = await callMcpTool("web_search", args);
          text = extractMcpText(result);
          if (result.isError) {
            throw new Error(text || "Search returned an error");
          }
        }

        const { content, truncated } = applyTruncation(text);
        return {
          content: [{ type: "text", text: content }],
          details: { truncated, mode: apiKey ? "api" : "mcp" },
        };
      } catch (err) {
        const msg = err instanceof Error ? err.message : String(err);
        throw new Error(`web_search failed: ${msg}`);
      }
    },
  });

  pi.registerTool({
    name: "web_fetch",
    label: "Web Fetch",
    description: `Extract content from one or more URLs using Parallel Extract. Returns clean, LLM-optimized markdown. Mode: ${mode}`,
    promptSnippet: "Fetch and extract content from specific URLs",
    promptGuidelines: [
      "Use web_fetch to extract readable content from specific web pages when you have URLs.",
      "Use web_fetch after web_search to dive deeper into specific results.",
    ],
    parameters: Type.Object({
      urls: Type.Array(Type.String(), {
        description: "URLs to extract content from",
        minItems: 1,
      }),
      objective: Type.Optional(
        Type.String({
          description:
            "Optional objective to focus extraction on specific information",
        })
      ),
    }),
    async execute(_toolCallId, params) {
      const args: Record<string, unknown> = {
        urls: params.urls,
      };
      if (params.objective) {
        args.objective = params.objective;
      }

      try {
        let text: string;

        if (apiKey) {
          text = await apiExtract(apiKey, params.urls, params.objective);
        } else {
          const result = await callMcpTool("web_fetch", args);
          text = extractMcpText(result);
          if (result.isError) {
            throw new Error(text || "Extract returned an error");
          }
        }

        const { content, truncated } = applyTruncation(text);
        return {
          content: [{ type: "text", text: content }],
          details: { truncated, mode: apiKey ? "api" : "mcp" },
        };
      } catch (err) {
        const msg = err instanceof Error ? err.message : String(err);
        throw new Error(`web_fetch failed: ${msg}`);
      }
    },
  });
}
