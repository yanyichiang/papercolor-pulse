import asyncio
import json
from io import BytesIO

from aiohttp.test_utils import TestClient, TestServer
from PIL import Image

from papercolor_gateway.app import PULSE_SERVICE_KEY, STORE_KEY, create_app
from papercolor_gateway.config import Settings
from papercolor_gateway.models import CardRequest


NOW = 1_800_000_000
DEVICE_ID = "papercolor-001"
DEVICE_TOKEN = "d" * 32
ADMIN_TOKEN = "a" * 32


def _png(color: str = "red") -> bytes:
    output = BytesIO()
    Image.new("RGB", (400, 600), color).save(output, format="PNG")
    return output.getvalue()


def _settings(tmp_path, font_path) -> Settings:
    return Settings(
        data_directory=tmp_path,
        font_path=font_path,
        device_id=DEVICE_ID,
        device_token=DEVICE_TOKEN,
        admin_token=ADMIN_TOKEN,
    )


async def _with_client(app, scenario):
    async with TestClient(TestServer(app)) as client:
        await scenario(client)


def test_device_manifest_requires_matching_bearer_and_exact_contract(
    tmp_path, font_path
) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    job = app[STORE_KEY].create_job(CardRequest("Now", "Body"), _png(), now=NOW)

    async def scenario(client: TestClient) -> None:
        anonymous = await client.get(f"/device/v1/manifest?device_id={DEVICE_ID}")
        wrong = await client.get(
            f"/device/v1/manifest?device_id={DEVICE_ID}",
            headers={"Authorization": "Bearer wrong"},
        )
        response = await client.get(
            f"/device/v1/manifest?device_id={DEVICE_ID}",
            headers={"Authorization": f"Bearer {DEVICE_TOKEN}"},
        )

        assert anonymous.status == 401
        assert wrong.status == 401
        assert response.status == 200
        payload = await response.json()
        assert set(payload) == {
            "protocol_version",
            "server_time",
            "manifest_version",
            "next_sync_at",
            "jobs",
        }
        assert payload["protocol_version"] == 1
        assert len(payload["jobs"]) == 1
        manifest_job = payload["jobs"][0]
        assert set(manifest_job) == {
            "id",
            "display_at",
            "expires_at",
            "asset_path",
            "asset_sha256",
            "asset_size",
            "mime_type",
        }
        assert manifest_job["id"] == job.id
        assert manifest_job["asset_path"] == (
            f"/device/v1/assets/{job.asset_sha256}.png"
        )

    asyncio.run(_with_client(app, scenario))


def test_asset_uses_same_device_bearer(tmp_path, font_path) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    job = app[STORE_KEY].create_job(CardRequest("Now", "Body"), _png(), now=NOW)
    path = f"/device/v1/assets/{job.asset_sha256}.png"

    async def scenario(client: TestClient) -> None:
        anonymous = await client.get(path)
        response = await client.get(
            path, headers={"Authorization": f"Bearer {DEVICE_TOKEN}"}
        )
        assert anonymous.status == 401
        assert response.status == 200
        assert response.headers["Content-Type"] == "image/png"
        assert response.headers["ETag"] == f'"{job.asset_sha256}"'
        assert await response.read() == _png()

    asyncio.run(_with_client(app, scenario))


def test_voice_capture_upload_is_authenticated_bounded_and_idempotent(
    tmp_path, font_path
) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    capture_id = "pc-1800000000-a1b2c3d4"
    wav = b"RIFF" + (40).to_bytes(4, "little") + b"WAVE" + b"\0" * 36

    async def scenario(client: TestClient) -> None:
        anonymous = await client.put(
            f"/device/v1/voice-captures/{capture_id}",
            data=wav,
            headers={"Content-Type": "audio/wav"},
        )
        first = await client.put(
            f"/device/v1/voice-captures/{capture_id}",
            data=wav,
            headers={
                "Authorization": f"Bearer {DEVICE_TOKEN}",
                "Content-Type": "audio/wav",
            },
        )
        repeated = await client.put(
            f"/device/v1/voice-captures/{capture_id}",
            data=wav,
            headers={
                "Authorization": f"Bearer {DEVICE_TOKEN}",
                "Content-Type": "audio/wav",
            },
        )
        conflict = await client.put(
            f"/device/v1/voice-captures/{capture_id}",
            data=wav + b"different",
            headers={
                "Authorization": f"Bearer {DEVICE_TOKEN}",
                "Content-Type": "audio/wav",
            },
        )

        assert anonymous.status == 401
        assert first.status == 201
        assert await first.json() == {
            "ok": True,
            "capture_id": capture_id,
            "state": "pending",
            "duplicate": False,
        }
        assert repeated.status == 200
        assert (await repeated.json())["duplicate"] is True
        assert conflict.status == 409

    asyncio.run(_with_client(app, scenario))


def test_ack_contract_is_authenticated_strict_and_idempotent(
    tmp_path, font_path
) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    job = app[STORE_KEY].create_job(CardRequest("Now", "Body"), _png(), now=NOW)
    body = {
        "device_id": DEVICE_ID,
        "job_id": job.id,
        "state": "displayed",
        "message": "shown",
    }

    async def scenario(client: TestClient) -> None:
        anonymous = await client.post("/device/v1/ack", json=body)
        first = await client.post(
            "/device/v1/ack",
            json=body,
            headers={"Authorization": f"Bearer {DEVICE_TOKEN}"},
        )
        repeated = await client.post(
            "/device/v1/ack",
            json=body,
            headers={"Authorization": f"Bearer {DEVICE_TOKEN}"},
        )
        cached = await client.post(
            "/device/v1/ack",
            json={**body, "state": "cached"},
            headers={"Authorization": f"Bearer {DEVICE_TOKEN}"},
        )
        extra = await client.post(
            "/device/v1/ack",
            json={**body, "extra": True},
            headers={"Authorization": f"Bearer {DEVICE_TOKEN}"},
        )

        assert anonymous.status == 401
        assert await first.json() == {"ok": True, "duplicate": False}
        assert await repeated.json() == {"ok": True, "duplicate": True}
        assert cached.status == 400
        assert extra.status == 400

    asyncio.run(_with_client(app, scenario))


def _rpc(method: str, request_id: int = 1, params=None) -> dict[str, object]:
    request: dict[str, object] = {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": method,
    }
    if params is not None:
        request["params"] = params
    return request


def test_mcp_initialize_and_tools_list_are_stateless_and_narrow(
    tmp_path, font_path
) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    headers = {"Authorization": f"Bearer {ADMIN_TOKEN}"}

    async def scenario(client: TestClient) -> None:
        anonymous = await client.post("/mcp", json=_rpc("initialize"))
        initialized = await client.post(
            "/mcp",
            json=_rpc(
                "initialize",
                params={
                    "protocolVersion": "2025-03-26",
                    "capabilities": {},
                    "clientInfo": {"name": "test", "version": "1"},
                },
            ),
            headers=headers,
        )
        listed = await client.post(
            "/mcp", json=_rpc("tools/list", request_id=2), headers=headers
        )

        assert anonymous.status == 401
        assert initialized.status == 200
        init_result = (await initialized.json())["result"]
        assert init_result["protocolVersion"] == "2025-03-26"
        assert "Mcp-Session-Id" not in initialized.headers
        tools = (await listed.json())["result"]["tools"]
        assert [tool["name"] for tool in tools] == [
            "papercolor_show_card",
            "papercolor_update_pulse",
            "get_environment",
            "papercolor_list_ideas",
        ]
        assert tools[0]["inputSchema"]["additionalProperties"] is False
        assert tools[1]["inputSchema"]["additionalProperties"] is False

    asyncio.run(_with_client(app, scenario))


def test_mcp_tool_call_queues_render_and_reports_guarantee(tmp_path, font_path) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    headers = {"Authorization": f"Bearer {ADMIN_TOKEN}"}

    async def scenario(client: TestClient) -> None:
        response = await client.post(
            "/mcp",
            json=_rpc(
                "tools/call",
                params={
                    "name": "papercolor_show_card",
                    "arguments": {
                        "title": "Scheduled",
                        "body": "This should be cached before display.",
                        "style": "brief",
                        "display_at": "2027-01-15T08:20:00Z",
                    },
                },
            ),
            headers=headers,
        )

        assert response.status == 200
        rpc = await response.json()
        result = rpc["result"]
        assert result["isError"] is False
        structured = result["structuredContent"]
        assert structured["render_dimensions"] == {"width": 400, "height": 600}
        assert structured["requested_display_at"] == NOW + 1_200
        assert structured["exact_delivery_guaranteed"] is True
        assert structured["estimated_next_sync_at"] == NOW + 900
        assert app[STORE_KEY].job_status(structured["job_id"]) == "pending"
        assert json.loads(result["content"][0]["text"]) == structured

    asyncio.run(_with_client(app, scenario))


def test_mcp_tool_validation_errors_do_not_queue_jobs(tmp_path, font_path) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    headers = {"Authorization": f"Bearer {ADMIN_TOKEN}"}

    async def scenario(client: TestClient) -> None:
        response = await client.post(
            "/mcp",
            json=_rpc(
                "tools/call",
                params={
                    "name": "papercolor_show_card",
                    "arguments": {"title": "x" * 81, "body": "Body"},
                },
            ),
            headers=headers,
        )
        rpc = await response.json()
        assert response.status == 200
        assert rpc["result"]["isError"] is True
        assert "80" in rpc["result"]["content"][0]["text"]

    asyncio.run(_with_client(app, scenario))


def test_pulse_mcp_rejects_facts_and_publishes_three_pages(
    tmp_path, font_path
) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    headers = {"Authorization": f"Bearer {ADMIN_TOKEN}"}

    async def scenario(client: TestClient) -> None:
        rejected = await client.post(
            "/mcp",
            json=_rpc(
                "tools/call",
                params={
                    "name": "papercolor_update_pulse",
                    "arguments": {"assistant_note": "带伞。", "temperature_c": 28.5},
                },
            ),
            headers=headers,
        )
        accepted = await client.post(
            "/mcp",
            json=_rpc(
                "tools/call",
                request_id=2,
                params={
                    "name": "papercolor_update_pulse",
                    "arguments": {
                        "assistant_note": "下午可能有阵雨，出门记得带伞。",
                        "focus_summary": "先完成 Pulse V1。",
                    },
                },
            ),
            headers=headers,
        )

        rejected_body = await rejected.json()
        assert rejected_body["result"]["isError"] is True
        assert "Unknown field" in rejected_body["result"]["content"][0]["text"]
        accepted_body = await accepted.json()
        structured = accepted_body["result"]["structuredContent"]
        assert structured["page_count"] == 3
        assert [page["name"] for page in structured["pages"]] == [
            "01-now",
            "02-work",
            "03-ideas",
        ]

    asyncio.run(_with_client(app, scenario))


def test_device_telemetry_is_authenticated_and_cannot_write_model_text(
    tmp_path, font_path
) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    headers = {"Authorization": f"Bearer {DEVICE_TOKEN}"}
    body = {
        "observed_at": "2027-01-15T08:00:00Z",
        "temperature_c": 28.5,
        "humidity_pct": 62,
        "sd_state": "ready",
    }

    async def scenario(client: TestClient) -> None:
        anonymous = await client.post("/device/v1/telemetry", json=body)
        rejected = await client.post(
            "/device/v1/telemetry",
            json={**body, "assistant_note": "我觉得有点热"},
            headers=headers,
        )
        accepted = await client.post(
            "/device/v1/telemetry", json=body, headers=headers
        )

        assert anonymous.status == 401
        assert rejected.status == 400
        assert accepted.status == 200
        payload = await accepted.json()
        assert payload["ok"] is True
        assert payload["device_id"] == DEVICE_ID
        assert payload["page_count"] == 3

    asyncio.run(_with_client(app, scenario))


def test_admin_fact_source_update_and_manifest_pulse_pages(tmp_path, font_path) -> None:
    app = create_app(_settings(tmp_path, font_path), clock=lambda: NOW)
    admin_headers = {"Authorization": f"Bearer {ADMIN_TOKEN}"}
    device_headers = {"Authorization": f"Bearer {DEVICE_TOKEN}"}

    async def scenario(client: TestClient) -> None:
        update = await client.put(
            "/admin/v1/pulse/sources/ha",
            json={
                "observed_at": "2027-01-15T08:00:00Z",
                "weather": {"condition": "多云", "low_c": 24, "high_c": 31},
            },
            headers=admin_headers,
        )
        manifest = await client.get(
            f"/device/v1/manifest?device_id={DEVICE_ID}", headers=device_headers
        )

        assert update.status == 200
        payload = await manifest.json()
        assert payload["protocol_version"] == 1
        assert [page["index"] for page in payload["pulse_pages"]] == [0, 1, 2]
        assert [page["name"] for page in payload["pulse_pages"]] == [
            "01-now",
            "02-work",
            "03-ideas",
        ]
        assert all(page["asset_path"].startswith("/device/v1/assets/") for page in payload["pulse_pages"])

    asyncio.run(_with_client(app, scenario))
