"""Authenticated aiohttp device and stateless MCP APIs."""

from __future__ import annotations

import asyncio
from collections.abc import Callable, Mapping
import hmac
import json
import re
import time
from typing import Any
from datetime import UTC, datetime
from zoneinfo import ZoneInfo

from aiohttp import web

from .config import Settings
from .fetcher import ImageFetchError, ImageFetcher
from .models import CardRequest, ValidationError
from .pulse_models import AssistantUpdate, SourceUpdate
from .pulse_renderer import PulseRenderer
from .pulse_service import PulseRenderError, PulseService
from .renderer import HEIGHT, WIDTH, CardRenderer
from .schedule import next_regular_sync
from .store import (
    InvalidTransition,
    JobStore,
    NotFound,
    StoreValidationError,
)


MCP_PROTOCOL_VERSION = "2025-03-26"
MAX_JSON_BODY_BYTES = 16 * 1024
MAX_VOICE_CAPTURE_BYTES = 2 * 1024 * 1024
_ASSET_PATTERN = re.compile(r"^[0-9a-f]{64}$")
_BEARER_PATTERN = re.compile(r"^Bearer ([^\s,]+)$", re.IGNORECASE)
_ACK_FIELDS = frozenset({"device_id", "job_id", "state", "message"})

STORE_KEY: web.AppKey[JobStore] = web.AppKey("store", JobStore)
SETTINGS_KEY: web.AppKey[Settings] = web.AppKey("settings", Settings)
RENDERER_KEY: web.AppKey[CardRenderer] = web.AppKey("renderer", CardRenderer)
PULSE_SERVICE_KEY: web.AppKey[PulseService] = web.AppKey(
    "pulse_service", PulseService
)
FETCHER_KEY: web.AppKey[ImageFetcher] = web.AppKey("fetcher", ImageFetcher)
CLOCK_KEY: web.AppKey[Callable[[], float]] = web.AppKey("clock", Callable)


PAPERCOLOR_TOOL = {
    "name": "papercolor_show_card",
    "description": "Render and queue one card for the PaperColor display.",
    "inputSchema": {
        "type": "object",
        "additionalProperties": False,
        "required": ["title", "body"],
        "properties": {
            "title": {"type": "string", "minLength": 1, "maxLength": 80},
            "body": {"type": "string", "minLength": 1, "maxLength": 1000},
            "style": {
                "type": "string",
                "enum": ["note", "brief", "quote", "postcard"],
                "default": "note",
            },
            "image_url": {"type": "string", "format": "uri", "maxLength": 2048},
            "display_at": {"type": "string", "format": "date-time", "maxLength": 64},
            "expires_at": {"type": "string", "format": "date-time", "maxLength": 64},
        },
    },
}

PAPERCOLOR_PULSE_TOOL = {
    "name": "papercolor_update_pulse",
    "description": (
        "Update the assistant's short note on the factual three-page PaperColor Pulse. "
        "This tool cannot write sensor, weather, usage, task, storage, or timestamp facts."
    ),
    "inputSchema": {
        "type": "object",
        "additionalProperties": False,
        "required": ["assistant_note"],
        "properties": {
            "assistant_note": {"type": "string", "minLength": 1, "maxLength": 160},
            "focus_summary": {"type": "string", "minLength": 1, "maxLength": 96},
        },
    },
}

PAPERCOLOR_ENVIRONMENT_TOOL = {
    "name": "get_environment",
    "description": (
        "Read factual PaperColor time, indoor climate, HA weather, source timestamps, "
        "and freshness. Use this before writing the assistant's care note."
    ),
    "inputSchema": {"type": "object", "additionalProperties": False},
}

PAPERCOLOR_IDEAS_TOOL = {
    "name": "papercolor_list_ideas",
    "description": "List recent voice captures with transcription state and text.",
    "inputSchema": {
        "type": "object",
        "additionalProperties": False,
        "properties": {"limit": {"type": "integer", "minimum": 1, "maximum": 20}},
    },
}


def _now(request: web.Request) -> int:
    return int(request.app[CLOCK_KEY]())


def _bearer_token(request: web.Request) -> str | None:
    authorization = request.headers.get("Authorization", "")
    match = _BEARER_PATTERN.fullmatch(authorization)
    return match.group(1) if match else None


def _unauthorized() -> web.Response:
    return web.json_response(
        {"error": "unauthorized"},
        status=401,
        headers={"WWW-Authenticate": "Bearer"},
    )


def _admin_authenticated(request: web.Request) -> bool:
    token = _bearer_token(request)
    return token is not None and hmac.compare_digest(
        token, request.app[SETTINGS_KEY].admin_token
    )


async def _read_json_object(request: web.Request) -> dict[str, Any]:
    if request.content_type != "application/json":
        raise web.HTTPBadRequest(
            text='{"error":"Content-Type must be application/json"}',
            content_type="application/json",
        )
    try:
        payload = await request.json(loads=json.loads)
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise web.HTTPBadRequest(
            text='{"error":"invalid JSON"}', content_type="application/json"
        ) from exc
    if not isinstance(payload, dict):
        raise web.HTTPBadRequest(
            text='{"error":"JSON body must be an object"}',
            content_type="application/json",
        )
    return payload


async def health(request: web.Request) -> web.Response:
    return web.json_response({"ok": True})


async def device_manifest(request: web.Request) -> web.Response:
    device_id = request.query.get("device_id", "")
    token = _bearer_token(request)
    if token is None or not request.app[STORE_KEY].authenticate_device(
        device_id, token, now=_now(request)
    ):
        return _unauthorized()
    manifest = request.app[STORE_KEY].build_manifest(device_id, now=_now(request))
    return web.json_response(manifest, headers={"Cache-Control": "no-store"})


async def device_asset(request: web.Request) -> web.StreamResponse:
    token = _bearer_token(request)
    if (
        token is None
        or request.app[STORE_KEY].authenticate_any_device(token, now=_now(request))
        is None
    ):
        return _unauthorized()
    sha256 = request.match_info["sha256"]
    if not _ASSET_PATTERN.fullmatch(sha256):
        raise web.HTTPNotFound()
    try:
        asset = request.app[STORE_KEY].get_asset(sha256)
    except NotFound as exc:
        raise web.HTTPNotFound() from exc
    return web.Response(
        body=asset.path.read_bytes(),
        headers={
            "Content-Type": "image/png",
            "Content-Length": str(asset.size),
            "Cache-Control": "private, max-age=86400, immutable",
            "ETag": f'"{asset.sha256}"',
            "X-Content-Type-Options": "nosniff",
        },
    )


async def device_ack(request: web.Request) -> web.Response:
    payload = await _read_json_object(request)
    if set(payload) != _ACK_FIELDS:
        return web.json_response(
            {
                "error": "ack body must contain only device_id, job_id, state, and message"
            },
            status=400,
        )
    device_id = payload.get("device_id")
    token = _bearer_token(request)
    if (
        not isinstance(device_id, str)
        or token is None
        or not request.app[STORE_KEY].authenticate_device(
            device_id, token, now=_now(request)
        )
    ):
        return _unauthorized()
    job_id = payload.get("job_id")
    state = payload.get("state")
    if not isinstance(job_id, str) or not isinstance(state, str):
        return web.json_response({"error": "invalid ack fields"}, status=400)
    try:
        result = request.app[STORE_KEY].acknowledge(
            device_id,
            job_id,
            state,
            payload.get("message"),
            now=_now(request),
        )
    except StoreValidationError as exc:
        return web.json_response({"error": str(exc)}, status=400)
    except NotFound as exc:
        return web.json_response({"error": str(exc)}, status=404)
    except InvalidTransition as exc:
        return web.json_response({"error": str(exc)}, status=409)
    return web.json_response({"ok": True, "duplicate": not result.created})


def _pulse_generation_result(generation) -> dict[str, object]:
    return {
        "generation_id": generation.id,
        "generated_at": generation.generated_at,
        "page_count": len(generation.pages),
        "render_dimensions": {"width": WIDTH, "height": HEIGHT},
        "pages": [
            {
                "index": page.index,
                "name": page.name,
                "sha256": page.sha256,
                "size": page.size,
            }
            for page in generation.pages
        ],
    }


async def device_telemetry(request: web.Request) -> web.Response:
    token = _bearer_token(request)
    device_id = (
        None
        if token is None
        else request.app[STORE_KEY].authenticate_any_device(token, now=_now(request))
    )
    if device_id is None:
        return _unauthorized()
    try:
        payload = await _read_json_object(request)
        update = SourceUpdate.from_mapping("device", payload)
        generation = await asyncio.to_thread(
            request.app[PULSE_SERVICE_KEY].update_source,
            update,
            now=_now(request),
        )
    except web.HTTPBadRequest:
        raise
    except (ValidationError, StoreValidationError, PulseRenderError) as exc:
        return web.json_response({"error": str(exc)}, status=400)
    return web.json_response(
        {
            "ok": True,
            "device_id": device_id,
            **_pulse_generation_result(generation),
        }
    )


async def device_voice_capture(request: web.Request) -> web.Response:
    token = _bearer_token(request)
    device_id = (
        None
        if token is None
        else request.app[STORE_KEY].authenticate_any_device(token, now=_now(request))
    )
    if device_id is None:
        return _unauthorized()
    if request.content_type not in {"audio/wav", "audio/x-wav"}:
        return web.json_response({"error": "Content-Type must be audio/wav"}, status=415)
    if request.content_length is not None and request.content_length > MAX_VOICE_CAPTURE_BYTES:
        return web.json_response({"error": "voice capture is too large"}, status=413)
    payload = await request.content.read(MAX_VOICE_CAPTURE_BYTES + 1)
    if len(payload) > MAX_VOICE_CAPTURE_BYTES:
        return web.json_response({"error": "voice capture is too large"}, status=413)
    try:
        capture = request.app[STORE_KEY].store_voice_capture(
            device_id,
            request.match_info["capture_id"],
            payload,
            now=_now(request),
        )
    except StoreValidationError as exc:
        return web.json_response({"error": str(exc)}, status=400)
    except InvalidTransition as exc:
        return web.json_response({"error": str(exc)}, status=409)
    return web.json_response(
        {
            "ok": True,
            "capture_id": capture.capture_id,
            "state": capture.state,
            "duplicate": not capture.created,
        },
        status=201 if capture.created else 200,
    )


async def admin_pulse_source(request: web.Request) -> web.Response:
    if not _admin_authenticated(request):
        return _unauthorized()
    source = request.match_info["source"]
    if source not in {"ha", "mac"}:
        return web.json_response({"error": "source must be ha or mac"}, status=400)
    try:
        payload = await _read_json_object(request)
        update = SourceUpdate.from_mapping(source, payload)
        generation = await asyncio.to_thread(
            request.app[PULSE_SERVICE_KEY].update_source,
            update,
            now=_now(request),
        )
    except web.HTTPBadRequest:
        raise
    except (ValidationError, StoreValidationError, PulseRenderError) as exc:
        return web.json_response({"error": str(exc)}, status=400)
    return web.json_response({"ok": True, **_pulse_generation_result(generation)})


def _rpc_result(request_id: object, result: object) -> web.Response:
    return web.json_response({"jsonrpc": "2.0", "id": request_id, "result": result})


def _rpc_error(
    request_id: object, code: int, message: str, *, status: int = 200
) -> web.Response:
    return web.json_response(
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "error": {"code": code, "message": message},
        },
        status=status,
    )


def _tool_error(request_id: object, message: str) -> web.Response:
    return _rpc_result(
        request_id,
        {
            "content": [{"type": "text", "text": message}],
            "isError": True,
        },
    )


def _structured_tool_result(request_id: object, structured: object) -> web.Response:
    return _rpc_result(
        request_id,
        {
            "content": [
                {
                    "type": "text",
                    "text": json.dumps(
                        structured,
                        ensure_ascii=False,
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                }
            ],
            "structuredContent": structured,
            "isError": False,
        },
    )


def _environment_snapshot(request: web.Request) -> dict[str, object]:
    now = _now(request)
    local = datetime.fromtimestamp(
        now, ZoneInfo(request.app[SETTINGS_KEY].timezone_name)
    )
    sources = request.app[STORE_KEY].get_pulse_sources()
    events: list[dict[str, object]] = []

    def source_meta(source: str, max_age: int) -> tuple[dict[str, object] | None, dict[str, object]]:
        payload = sources.get(source)
        observed = payload.get("observed_at") if payload else None
        age = now - observed if isinstance(observed, int) else None
        return payload, {
            "source": source,
            "observed_at": (
                datetime.fromtimestamp(observed, UTC).isoformat()
                if isinstance(observed, int)
                else None
            ),
            "age_seconds": age,
            "fresh": isinstance(age, int) and -300 <= age <= max_age,
        }

    device, device_meta = source_meta("device", 2 * 60 * 60)
    if device:
        if "temperature_c" in device:
            events.append(
                {"type": "environment.temperature", "value_c": device["temperature_c"], **device_meta}
            )
        if "humidity_pct" in device:
            events.append(
                {"type": "environment.humidity", "value_pct": device["humidity_pct"], **device_meta}
            )
    ha, ha_meta = source_meta("ha", 6 * 60 * 60)
    weather = ha.get("weather") if ha else None
    if isinstance(weather, dict):
        events.append({"type": "weather.snapshot", **weather, **ha_meta})
    return {
        "date": local.strftime("%Y-%m-%d"),
        "time": local.strftime("%H:%M:%S"),
        "timezone": request.app[SETTINGS_KEY].timezone_name,
        "generated_at": local.isoformat(),
        "events": events,
    }


async def mcp(request: web.Request) -> web.Response:
    if not _admin_authenticated(request):
        return _unauthorized()
    try:
        payload = await _read_json_object(request)
    except web.HTTPBadRequest:
        return _rpc_error(None, -32700, "Parse error", status=400)
    request_id = payload.get("id")
    if payload.get("jsonrpc") != "2.0" or not isinstance(payload.get("method"), str):
        return _rpc_error(request_id, -32600, "Invalid Request")
    method = payload["method"]
    if "id" not in payload:
        return web.Response(status=202)
    if isinstance(request_id, bool) or not isinstance(
        request_id, (str, int, type(None))
    ):
        return _rpc_error(None, -32600, "Invalid Request")

    if method == "initialize":
        return _rpc_result(
            request_id,
            {
                "protocolVersion": MCP_PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "papercolor-gateway", "version": "0.1.0"},
            },
        )
    if method == "tools/list":
        return _rpc_result(
            request_id,
            {
                "tools": [
                    PAPERCOLOR_TOOL,
                    PAPERCOLOR_PULSE_TOOL,
                    PAPERCOLOR_ENVIRONMENT_TOOL,
                    PAPERCOLOR_IDEAS_TOOL,
                ]
            },
        )
    if method != "tools/call":
        return _rpc_error(request_id, -32601, "Method not found")

    params = payload.get("params")
    if not isinstance(params, Mapping):
        return _rpc_error(request_id, -32602, "Invalid tools/call params")
    tool_name = params.get("name")
    if tool_name not in {
        "papercolor_show_card",
        "papercolor_update_pulse",
        "get_environment",
        "papercolor_list_ideas",
    }:
        return _rpc_error(request_id, -32602, "Unknown tool")
    arguments = params.get("arguments", {})
    if not isinstance(arguments, Mapping):
        return _rpc_error(request_id, -32602, "Tool arguments must be an object")

    accepted_at = _now(request)
    if tool_name == "get_environment":
        if arguments:
            return _tool_error(request_id, "get_environment takes no arguments")
        return _structured_tool_result(request_id, _environment_snapshot(request))
    if tool_name == "papercolor_list_ideas":
        limit = arguments.get("limit", 10)
        if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 20:
            return _tool_error(request_id, "limit must be an integer from 1 to 20")
        return _structured_tool_result(
            request_id,
            {"ideas": request.app[STORE_KEY].voice_capture_summaries(limit=limit)},
        )
    if tool_name == "papercolor_update_pulse":
        try:
            update = AssistantUpdate.from_mapping(arguments)
            generation = await asyncio.to_thread(
                request.app[PULSE_SERVICE_KEY].update_assistant,
                update,
                now=accepted_at,
            )
        except (ValidationError, StoreValidationError, PulseRenderError) as exc:
            return _tool_error(request_id, str(exc))
        structured = _pulse_generation_result(generation)
        return _rpc_result(
            request_id,
            {
                "content": [
                    {
                        "type": "text",
                        "text": json.dumps(
                            structured,
                            ensure_ascii=False,
                            sort_keys=True,
                            separators=(",", ":"),
                        ),
                    }
                ],
                "structuredContent": structured,
                "isError": False,
            },
        )

    try:
        card = CardRequest.from_mapping(arguments, now=accepted_at)
        source_image = None
        if card.image_url is not None:
            source_image = (await request.app[FETCHER_KEY].fetch(card.image_url)).image
        png_bytes = await asyncio.to_thread(
            request.app[RENDERER_KEY].render, card, source_image
        )
        job = request.app[STORE_KEY].create_job(card, png_bytes, now=accepted_at)
    except (ValidationError, ImageFetchError, StoreValidationError) as exc:
        return _tool_error(request_id, str(exc))

    structured = {
        "job_id": job.id,
        "render_dimensions": {"width": WIDTH, "height": HEIGHT},
        "requested_display_at": card.display_at,
        "exact_delivery_guaranteed": job.guaranteed,
        "estimated_next_sync_at": next_regular_sync(accepted_at),
    }
    return _rpc_result(
        request_id,
        {
            "content": [
                {
                    "type": "text",
                    "text": json.dumps(
                        structured,
                        ensure_ascii=False,
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                }
            ],
            "structuredContent": structured,
            "isError": False,
        },
    )


async def _close_store(app: web.Application) -> None:
    app[STORE_KEY].close()


def create_app(
    settings: Settings,
    *,
    clock: Callable[[], float] = time.time,
    fetcher: ImageFetcher | None = None,
) -> web.Application:
    store = JobStore(settings.database_path, settings.asset_directory)
    store.register_device(settings.device_id, settings.device_token, now=int(clock()))
    app = web.Application(client_max_size=MAX_VOICE_CAPTURE_BYTES + 1024)
    app[SETTINGS_KEY] = settings
    app[STORE_KEY] = store
    app[RENDERER_KEY] = CardRenderer(settings.font_path)
    assert settings.cjk_serif_font_path is not None
    assert settings.latin_serif_font_path is not None
    app[PULSE_SERVICE_KEY] = PulseService(
        store,
        PulseRenderer(
            settings.cjk_serif_font_path,
            settings.latin_serif_font_path,
            timezone_name=settings.timezone_name,
        ),
    )
    app[FETCHER_KEY] = fetcher or ImageFetcher()
    app[CLOCK_KEY] = clock
    app.add_routes(
        [
            web.get("/healthz", health),
            web.get("/device/v1/manifest", device_manifest),
            web.get("/device/v1/assets/{sha256}.png", device_asset),
            web.post("/device/v1/ack", device_ack),
            web.post("/device/v1/telemetry", device_telemetry),
            web.put(
                "/device/v1/voice-captures/{capture_id}", device_voice_capture
            ),
            web.put("/admin/v1/pulse/sources/{source}", admin_pulse_source),
            web.post("/mcp", mcp),
        ]
    )
    app.on_cleanup.append(_close_store)
    return app
