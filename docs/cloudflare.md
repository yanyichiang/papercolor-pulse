# Cloudflare MCP setup

This is optional. Complete the local-only setup first.

The public route is:

```text
MCP client -> Worker bearer + tool allowlist -> Access Service Token
-> Cloudflare Tunnel -> home gateway admin bearer
```

The PaperColor device remains LAN-only and makes outbound requests to the home
gateway.

## Prerequisites

- A domain on Cloudflare
- Node.js 20+
- A Cloudflare account with Workers, Access, and Tunnel available
- `cloudflared` running on the home server
- The gateway already working on `127.0.0.1:8767`

## 1. Create the tunnel hostname

Create a Cloudflare Tunnel public hostname such as:

```text
papercolor-origin.example.com -> http://127.0.0.1:8767
```

Do not use this origin hostname as the model-facing endpoint.

## 2. Protect the origin with Access

Create a self-hosted Access application for the exact origin hostname and an
Access policy with action `Service Auth`. Create a Service Token and retain its
Client ID and Client Secret.

Test that an anonymous request is rejected before continuing.

## 3. Configure the Worker

```bash
cd cloudflare
npm install
cp wrangler.example.jsonc wrangler.jsonc
```

Edit `ORIGIN_MCP_URL` in `wrangler.jsonc`:

```text
https://papercolor-origin.example.com/mcp
```

Set secrets. Generate `CLIENT_TOKEN` separately from the gateway admin token:

```bash
npx wrangler secret put CLIENT_TOKEN
npx wrangler secret put ORIGIN_ADMIN_TOKEN
npx wrangler secret put ACCESS_CLIENT_ID
npx wrangler secret put ACCESS_CLIENT_SECRET
npx wrangler deploy
```

Secrets are never stored in `wrangler.jsonc`.

## 4. Verify fail-closed behavior

Without a bearer, the Worker must return 401:

```bash
curl -i https://YOUR_WORKER_DOMAIN/mcp \
  -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

With the Worker client bearer, `tools/list` should return exactly four tools:

```bash
curl -sS https://YOUR_WORKER_DOMAIN/mcp \
  -H "Authorization: Bearer $CLIENT_TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

The Worker rejects an unlisted `tools/call` before contacting the origin.

## MCP client configuration

Clients that support custom headers can use:

```json
{
  "mcpServers": {
    "papercolor": {
      "type": "streamable-http",
      "url": "https://YOUR_WORKER_DOMAIN/mcp",
      "headers": {
        "Authorization": "Bearer YOUR_CLIENT_TOKEN"
      }
    }
  }
}
```

If a client cannot send a custom Authorization header, do not place the token
in the URL. Use a client or local adapter that supports headers.

## Security notes

- Access protects the origin; the Worker client bearer protects the model
  endpoint; the gateway admin bearer protects the application API.
- All three credentials should be different.
- Rotate one layer at a time and test the new credential before removing the old.
- Keep the Worker tool allowlist even if the gateway already validates tools.
- Never expose gateway port 8767 with router port forwarding.
