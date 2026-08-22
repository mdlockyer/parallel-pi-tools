import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { Type } from "typebox";
import {
  truncateHead,
  DEFAULT_MAX_BYTES,
  DEFAULT_MAX_LINES,
  formatSize,
} from "@earendil-works/pi-coding-agent";
import {
  findApiKey,
  apiSearch,
  apiExtract,
  mcpSearch,
  mcpFetch,
} from "./lib/web-search/parallel.mjs";

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

export default function (pi: ExtensionAPI) {
  pi.registerTool({
    name: "web_search",
    label: "Web Search",
    description: `Search the web using Parallel Search. Returns ranked URLs with LLM-optimized excerpts. Supports natural language objectives. Uses the direct API when a PARALLEL_* key is set in the environment, otherwise the free MCP fallback. Native C FFI.`,
    promptSnippet: "Search the web for real-time information",
    promptGuidelines: [
      "Use web_search when you need current information from the web that is not in your training data.",
      "Use web_search to verify facts, find recent news, or research topics.",
    ],
    parameters: Type.Object({
      objective: Type.String({
        description: "Natural language description of what you are looking for",
      }),
      search_queries: Type.Optional(
        Type.Array(Type.String(), {
          description: "Optional specific search queries to supplement the objective",
        })
      ),
    }),
    async execute(_toolCallId, params) {
      try {
        // Resolve the key per call so env changes after extension load are
        // picked up without a /reload (the scan is ~microseconds).
        const apiKey = findApiKey();
        const queries = params.search_queries?.length ? params.search_queries : [params.objective];
        const text = apiKey
          ? await apiSearch(apiKey, params.objective, queries)
          : await mcpSearch(params.objective, queries);

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
    description: `Extract content from one or more URLs using Parallel Extract. Returns clean, LLM-optimized markdown. Uses the direct API when a PARALLEL_* key is set in the environment, otherwise the free MCP fallback. Native C FFI.`,
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
          description: "Optional objective to focus extraction on specific information",
        })
      ),
    }),
    async execute(_toolCallId, params) {
      try {
        const apiKey = findApiKey();
        const text = apiKey
          ? await apiExtract(apiKey, params.urls, params.objective)
          : await mcpFetch(params.urls, params.objective);

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
