# PaperColor Gateway

The gateway is an authenticated aiohttp service that renders deterministic
400 x 600 images, stores jobs and Pulse generations in SQLite, and serves a
pull manifest to a LAN-only PaperColor device.

## Configuration

Copy `config/example.env` to `.env`. Required values:

| Variable | Purpose |
|---|---|
| `PAPERCOLOR_DEVICE_ID` | Stable device identifier using letters, numbers, dot, underscore, or hyphen |
| `PAPERCOLOR_DEVICE_TOKEN` | Device bearer, 32-512 non-whitespace characters |
| `PAPERCOLOR_ADMIN_TOKEN` | Different admin/MCP bearer, 32-512 characters |
| `PAPERCOLOR_*_FONT_PATH` | Existing font files used by the deterministic renderer |
| `PAPERCOLOR_TIMEZONE` | IANA timezone, for example `Asia/Shanghai` |
| `PAPERCOLOR_DATA_DIR` | Persistent SQLite and asset directory |

## Docker

```bash
cp config/example.env .env
# Edit .env first.
docker compose up -d --build
curl http://127.0.0.1:8767/health
```

## Native Python

Requires Python 3.12+ and Noto CJK fonts:

```bash
uv sync --extra test
set -a; . config/example.env; set +a
uv run papercolor-gateway
```

## API boundaries

- `/device/v1/*` requires the device bearer.
- `/admin/v1/*` and `/mcp` require the admin bearer.
- Device and admin tokens must be different.
- `PUT /admin/v1/pulse/sources/ha` accepts weather facts.
- `PUT /admin/v1/pulse/sources/mac` accepts optional work/idea facts.
- Device telemetry accepts only device-owned sensor/storage facts.

The stateless MCP endpoint supports `initialize`, `tools/list`, and
`tools/call`, exposing exactly:

- `papercolor_show_card`
- `papercolor_update_pulse`
- `get_environment`
- `papercolor_list_ideas`

`papercolor_update_pulse` accepts only `assistant_note` and optional
`focus_summary`. Sensor, weather, quota, task, storage, and timestamp facts are
rejected from that model-facing write tool.

## Home Assistant

See `../integrations/home-assistant/papercolor_pulse.yaml` and
`../docs/home-assistant.md`.

## Tests

```bash
uv sync --extra test
uv run pytest
```
