import { describe, expect, it, vi } from "vitest";
import { handleRequest, type Env } from "../src/index";

const env: Env = {
  ORIGIN_MCP_URL: "https://papercolor-origin.example.com/mcp",
  CLIENT_TOKEN: "client-secret-value",
  ORIGIN_ADMIN_TOKEN: "origin-admin-secret",
  ACCESS_CLIENT_ID: "access-client-id",
  ACCESS_CLIENT_SECRET: "access-client-secret",
};

function rpc(body: object, token = env.CLIENT_TOKEN): Request {
  return new Request("https://worker.example.com/mcp", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify(body),
  });
}

describe("PaperColor Worker", () => {
  it("rejects anonymous MCP calls", async () => {
    const response = await handleRequest(
      new Request("https://worker.example.com/mcp", { method: "POST" }),
      env,
    );
    expect(response.status).toBe(401);
  });

  it("filters tools/list to the explicit allowlist", async () => {
    const fetcher = vi.fn(async () =>
      Response.json({
        jsonrpc: "2.0",
        id: 1,
        result: {
          tools: [
            { name: "papercolor_show_card" },
            { name: "get_environment" },
            { name: "dangerous_debug_shell" },
          ],
        },
      }),
    );
    const response = await handleRequest(
      rpc({ jsonrpc: "2.0", id: 1, method: "tools/list", params: {} }),
      env,
      fetcher as unknown as typeof fetch,
    );
    const payload = await response.json() as { result: { tools: Array<{ name: string }> } };
    expect(payload.result.tools.map((tool) => tool.name)).toEqual([
      "papercolor_show_card",
      "get_environment",
    ]);
  });

  it("blocks an unlisted tools/call before the origin", async () => {
    const fetcher = vi.fn();
    const response = await handleRequest(
      rpc({
        jsonrpc: "2.0",
        id: 2,
        method: "tools/call",
        params: { name: "dangerous_debug_shell", arguments: {} },
      }),
      env,
      fetcher as unknown as typeof fetch,
    );
    const payload = await response.json() as { error: { code: number } };
    expect(payload.error.code).toBe(-32602);
    expect(fetcher).not.toHaveBeenCalled();
  });

  it("injects origin auth without forwarding the client bearer", async () => {
    const fetcher = vi.fn(async (_url: string | URL | Request, init?: RequestInit) => {
      const headers = new Headers(init?.headers);
      expect(headers.get("Authorization")).toBe("Bearer origin-admin-secret");
      expect(headers.get("CF-Access-Client-Id")).toBe("access-client-id");
      return Response.json({ jsonrpc: "2.0", id: 3, result: { tools: [] } });
    });
    const response = await handleRequest(
      rpc({ jsonrpc: "2.0", id: 3, method: "tools/list", params: {} }),
      env,
      fetcher as unknown as typeof fetch,
    );
    expect(response.status).toBe(200);
  });
});
