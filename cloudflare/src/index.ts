export interface Env {
  ORIGIN_MCP_URL: string;
  CLIENT_TOKEN: string;
  ORIGIN_ADMIN_TOKEN: string;
  ACCESS_CLIENT_ID: string;
  ACCESS_CLIENT_SECRET: string;
}

const ALLOWED_TOOLS = new Set([
  "papercolor_show_card",
  "papercolor_update_pulse",
  "get_environment",
  "papercolor_list_ideas",
]);

const MAX_BODY_BYTES = 64 * 1024;

type RpcObject = {
  jsonrpc?: unknown;
  id?: unknown;
  method?: unknown;
  params?: unknown;
  result?: unknown;
  error?: unknown;
};

function json(body: unknown, status = 200): Response {
  return Response.json(body, {
    status,
    headers: {
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
    },
  });
}

function rpcError(id: unknown, code: number, message: string, status = 200): Response {
  return json({ jsonrpc: "2.0", id: id ?? null, error: { code, message } }, status);
}

function bearer(request: Request): string | null {
  const match = /^Bearer ([^\s,]+)$/i.exec(request.headers.get("Authorization") ?? "");
  return match?.[1] ?? null;
}

function safeEqual(left: string | null, right: string): boolean {
  if (left === null || left.length !== right.length) return false;
  let mismatch = 0;
  for (let index = 0; index < left.length; index += 1) {
    mismatch |= left.charCodeAt(index) ^ right.charCodeAt(index);
  }
  return mismatch === 0;
}

function validOrigin(url: string): boolean {
  try {
    const parsed = new URL(url);
    return parsed.protocol === "https:" && parsed.username === "" && parsed.password === "";
  } catch {
    return false;
  }
}

function requestedTool(payload: RpcObject): string | null {
  if (payload.method !== "tools/call" || typeof payload.params !== "object" || payload.params === null) {
    return null;
  }
  const name = (payload.params as Record<string, unknown>).name;
  return typeof name === "string" ? name : null;
}

function filterTools(payload: RpcObject): RpcObject {
  if (typeof payload.result !== "object" || payload.result === null) return payload;
  const result = payload.result as Record<string, unknown>;
  if (!Array.isArray(result.tools)) return payload;
  return {
    ...payload,
    result: {
      ...result,
      tools: result.tools.filter(
        (tool) =>
          typeof tool === "object" &&
          tool !== null &&
          typeof (tool as Record<string, unknown>).name === "string" &&
          ALLOWED_TOOLS.has((tool as Record<string, unknown>).name as string),
      ),
    },
  };
}

export async function handleRequest(
  request: Request,
  env: Env,
  fetcher: typeof fetch = fetch,
): Promise<Response> {
  const url = new URL(request.url);
  if (url.pathname === "/health" && request.method === "GET") {
    return json({ ok: true, service: "papercolor-pulse-worker" });
  }
  if (url.pathname !== "/mcp") return json({ error: "not_found" }, 404);
  if (request.method !== "POST") return json({ error: "method_not_allowed" }, 405);
  if (!env.CLIENT_TOKEN || !safeEqual(bearer(request), env.CLIENT_TOKEN)) {
    return json({ error: "unauthorized" }, 401);
  }
  if (!validOrigin(env.ORIGIN_MCP_URL)) {
    return json({ error: "origin_not_configured" }, 503);
  }
  if (!env.ORIGIN_ADMIN_TOKEN || !env.ACCESS_CLIENT_ID || !env.ACCESS_CLIENT_SECRET) {
    return json({ error: "origin_credentials_not_configured" }, 503);
  }

  const contentLength = Number(request.headers.get("Content-Length") ?? "0");
  if (Number.isFinite(contentLength) && contentLength > MAX_BODY_BYTES) {
    return json({ error: "request_too_large" }, 413);
  }
  const body = await request.arrayBuffer();
  if (body.byteLength > MAX_BODY_BYTES) return json({ error: "request_too_large" }, 413);

  let payload: RpcObject;
  try {
    payload = JSON.parse(new TextDecoder().decode(body)) as RpcObject;
  } catch {
    return rpcError(null, -32700, "Parse error", 400);
  }
  if (typeof payload !== "object" || payload === null || Array.isArray(payload)) {
    return rpcError(null, -32600, "Invalid Request", 400);
  }
  const tool = requestedTool(payload);
  if (tool !== null && !ALLOWED_TOOLS.has(tool)) {
    return rpcError(payload.id, -32602, "Tool is not allowed");
  }

  const upstream = await fetcher(env.ORIGIN_MCP_URL, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${env.ORIGIN_ADMIN_TOKEN}`,
      "CF-Access-Client-Id": env.ACCESS_CLIENT_ID,
      "CF-Access-Client-Secret": env.ACCESS_CLIENT_SECRET,
      "Content-Type": "application/json",
      Accept: "application/json",
    },
    body,
    redirect: "manual",
  });
  if (upstream.status < 200 || upstream.status >= 300) {
    return json({ error: "origin_error", status: upstream.status }, 502);
  }

  let result: RpcObject;
  try {
    result = (await upstream.json()) as RpcObject;
  } catch {
    return json({ error: "invalid_origin_response" }, 502);
  }
  return json(payload.method === "tools/list" ? filterTools(result) : result);
}

export default {
  fetch(request: Request, env: Env): Promise<Response> {
    return handleRequest(request, env);
  },
} satisfies ExportedHandler<Env>;
