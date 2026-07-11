from io import BytesIO

from PIL import Image
import pytest

from papercolor_gateway.models import CardRequest
from papercolor_gateway.store import (
    MAX_MANIFEST_ASSET_BYTES,
    MAX_MANIFEST_JOBS,
    InvalidTransition,
    JobStore,
    NotFound,
    StoreValidationError,
)


NOW = 1_800_000_000


def _png(color: str = "red") -> bytes:
    output = BytesIO()
    Image.new("RGB", (400, 600), color).save(output, format="PNG")
    return output.getvalue()


def _large_png(color: str) -> bytes:
    output = BytesIO()
    Image.new("RGB", (400, 600), color).save(output, format="PNG", compress_level=0)
    return output.getvalue()


@pytest.fixture
def store(tmp_path):
    opened = JobStore(tmp_path / "gateway.sqlite3", tmp_path / "assets")
    opened.register_device("papercolor-001", "d" * 32, now=NOW)
    yield opened
    opened.close()


def test_device_registration_authenticates_without_storing_plaintext(store) -> None:
    assert store.authenticate_device("papercolor-001", "d" * 32, now=NOW + 1)
    assert not store.authenticate_device("papercolor-001", "wrong", now=NOW + 1)
    assert store.authenticate_any_device("d" * 32, now=NOW + 1) == "papercolor-001"
    assert "d" * 32 not in store.database_path.read_text(errors="ignore")


def test_assets_are_content_addressed_and_deduplicated(store) -> None:
    first = store.create_job(CardRequest("First", "Body"), _png(), now=NOW)
    second = store.create_job(CardRequest("Second", "Body"), _png(), now=NOW + 1)

    assert first.asset_sha256 == second.asset_sha256
    assert first.asset_path == second.asset_path
    assert first.asset_path.read_bytes() == _png()
    assert len(list(store.asset_directory.glob("*.png"))) == 1


def test_same_size_asset_corruption_is_rejected(store) -> None:
    job = store.create_job(CardRequest("First", "Body"), _png(), now=NOW)
    original = job.asset_path.read_bytes()
    job.asset_path.write_bytes(bytes([original[0] ^ 0xFF]) + original[1:])

    with pytest.raises(NotFound, match="asset"):
        store.get_asset(job.asset_sha256)


def test_new_immediate_job_supersedes_old_one_in_manifest(store) -> None:
    old = store.create_job(CardRequest("Old", "Body"), _png("red"), now=NOW)
    newest = store.create_job(CardRequest("New", "Body"), _png("blue"), now=NOW + 10)

    manifest = store.build_manifest("papercolor-001", now=NOW + 20)

    assert [job["id"] for job in manifest["jobs"]] == [newest.id]
    assert manifest == {
        "protocol_version": 1,
        "server_time": NOW + 20,
        "manifest_version": 2,
        "next_sync_at": 1_800_000_900,
        "jobs": [
            {
                "id": newest.id,
                "display_at": NOW + 10,
                "expires_at": 0,
                "asset_path": f"/device/v1/assets/{newest.asset_sha256}.png",
                "asset_sha256": newest.asset_sha256,
                "asset_size": newest.asset_size,
                "mime_type": "image/png",
            }
        ],
    }
    assert store.job_status(old.id) == "superseded"


def test_manifest_includes_only_active_24_hour_jobs_and_caps_count(store) -> None:
    assert MAX_MANIFEST_JOBS == 16
    immediate = store.create_job(CardRequest("Now", "Body"), _png(), now=NOW)
    scheduled_ids: list[str] = []
    for index in range(MAX_MANIFEST_JOBS + 5):
        job = store.create_job(
            CardRequest(
                f"Scheduled {index}",
                "Body",
                display_at=NOW + 60 + index,
                expires_at=NOW + 3_600,
            ),
            _png("green"),
            now=NOW,
        )
        scheduled_ids.append(job.id)
    store.create_job(
        CardRequest("Beyond", "Body", display_at=NOW + 86_401),
        _png("yellow"),
        now=NOW,
    )

    jobs = store.build_manifest("papercolor-001", now=NOW)["jobs"]

    assert len(jobs) == MAX_MANIFEST_JOBS
    assert immediate.id in {job["id"] for job in jobs}
    assert [job["id"] for job in jobs if job["id"] != immediate.id] == scheduled_ids[
        : MAX_MANIFEST_JOBS - 1
    ]


def test_manifest_caps_total_asset_bytes_at_four_mib(store) -> None:
    created = []
    for index, color in enumerate(
        ("red", "green", "blue", "yellow", "purple", "orange", "black", "white")
    ):
        created.append(
            store.create_job(
                CardRequest(
                    f"Large {index}",
                    "Body",
                    display_at=NOW + 60 + index,
                ),
                _large_png(color),
                now=NOW,
            )
        )

    jobs = store.build_manifest("papercolor-001", now=NOW)["jobs"]

    assert sum(job["asset_size"] for job in jobs) <= MAX_MANIFEST_ASSET_BYTES
    expected_ids = []
    expected_size = 0
    for job in created:
        if expected_size + job.asset_size <= MAX_MANIFEST_ASSET_BYTES:
            expected_ids.append(job.id)
            expected_size += job.asset_size
    assert [job["id"] for job in jobs] == expected_ids


def test_ack_is_idempotent_and_terminal(store) -> None:
    job = store.create_job(CardRequest("Now", "Body"), _png(), now=NOW)

    first = store.acknowledge(
        "papercolor-001", job.id, "displayed", "ok", now=NOW + 1
    )
    repeated = store.acknowledge(
        "papercolor-001", job.id, "displayed", "ignored", now=NOW + 2
    )

    assert first.created is True
    assert repeated.created is False
    assert store.job_status(job.id) == "displayed"
    assert store.build_manifest("papercolor-001", now=NOW + 3)["jobs"] == []
    with pytest.raises(InvalidTransition):
        store.acknowledge(
            "papercolor-001", job.id, "failed", "late failure", now=NOW + 4
        )


@pytest.mark.parametrize("state", ["cached", "done", ""])
def test_ack_rejects_unknown_state(store, state: str) -> None:
    job = store.create_job(CardRequest("Now", "Body"), _png(), now=NOW)
    with pytest.raises(StoreValidationError, match="state"):
        store.acknowledge("papercolor-001", job.id, state, "", now=NOW)


def test_ack_message_is_bounded(store) -> None:
    job = store.create_job(CardRequest("Now", "Body"), _png(), now=NOW)
    with pytest.raises(StoreValidationError, match="message"):
        store.acknowledge("papercolor-001", job.id, "failed", "x" * 241, now=NOW)


def test_expired_jobs_leave_manifest_and_increment_version(store) -> None:
    job = store.create_job(
        CardRequest("Short", "Body", expires_at=NOW + 5), _png(), now=NOW
    )
    assert store.build_manifest("papercolor-001", now=NOW)["manifest_version"] == 1

    manifest = store.build_manifest("papercolor-001", now=NOW + 5)

    assert manifest["jobs"] == []
    assert manifest["manifest_version"] == 2
    assert store.job_status(job.id) == "expired"
