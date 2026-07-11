"""SQLite job state and content-addressed PNG assets."""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import sqlite3
import tempfile
import threading
import unicodedata
import uuid
from datetime import UTC, datetime

from .models import CardRequest
from .pulse_renderer import PulseBundle
from .schedule import is_exact_delivery_guaranteed, next_regular_sync


PROTOCOL_VERSION = 1
MANIFEST_HORIZON_SECONDS = 24 * 60 * 60
MAX_MANIFEST_JOBS = 16
MAX_MANIFEST_ASSET_BYTES = 4 * 1024 * 1024
MAX_ACK_MESSAGE_CHARACTERS = 240
_SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
_TERMINAL_STATES = frozenset({"displayed", "failed"})
_CAPTURE_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$")


class StoreError(RuntimeError):
    """Base class for persistent state errors."""


class StoreValidationError(StoreError):
    """Raised for invalid store input."""


class NotFound(StoreError):
    """Raised when a device, job, or asset does not exist."""


class InvalidTransition(StoreError):
    """Raised when an acknowledgement attempts to rewrite a terminal state."""


@dataclass(frozen=True, slots=True)
class AssetRecord:
    sha256: str
    size: int
    mime_type: str
    path: Path


@dataclass(frozen=True, slots=True)
class JobRecord:
    id: str
    created_at: int
    display_at: int
    expires_at: int | None
    guaranteed: bool
    asset_sha256: str
    asset_size: int
    asset_path: Path


@dataclass(frozen=True, slots=True)
class AckResult:
    created: bool
    state: str


@dataclass(frozen=True, slots=True)
class PulseAssetRecord:
    index: int
    name: str
    sha256: str
    size: int
    path: Path


@dataclass(frozen=True, slots=True)
class PulseBundleRecord:
    id: int
    generated_at: int
    pages: tuple[PulseAssetRecord, ...]


@dataclass(frozen=True, slots=True)
class VoiceCaptureRecord:
    capture_id: str
    device_id: str
    sha256: str
    size: int
    state: str
    path: Path
    created: bool


@dataclass(frozen=True, slots=True)
class PendingVoiceCapture:
    capture_id: str
    path: Path


class JobStore:
    def __init__(self, database_path: str | Path, asset_directory: str | Path):
        self.database_path = Path(database_path)
        self.asset_directory = Path(asset_directory)
        self.database_path.parent.mkdir(parents=True, exist_ok=True, mode=0o750)
        self.asset_directory.mkdir(parents=True, exist_ok=True, mode=0o750)
        self.voice_directory = self.asset_directory.parent / "voice-captures"
        self.voice_directory.mkdir(parents=True, exist_ok=True, mode=0o750)
        self._lock = threading.RLock()
        self._connection = sqlite3.connect(
            self.database_path,
            isolation_level=None,
            check_same_thread=False,
        )
        self._connection.row_factory = sqlite3.Row
        self._connection.execute("PRAGMA foreign_keys = ON")
        self._connection.execute("PRAGMA journal_mode = WAL")
        self._connection.execute("PRAGMA synchronous = FULL")
        self._connection.execute("PRAGMA busy_timeout = 5000")
        self._initialize_schema()

    def _initialize_schema(self) -> None:
        with self._lock:
            self._connection.executescript(
                """
                CREATE TABLE IF NOT EXISTS metadata (
                    key TEXT PRIMARY KEY,
                    value INTEGER NOT NULL
                );
                INSERT OR IGNORE INTO metadata(key, value)
                VALUES ('manifest_version', 0);

                CREATE TABLE IF NOT EXISTS devices (
                    id TEXT PRIMARY KEY,
                    token_hash TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    last_seen_at INTEGER
                );

                CREATE TABLE IF NOT EXISTS assets (
                    sha256 TEXT PRIMARY KEY,
                    size INTEGER NOT NULL CHECK(size > 0),
                    mime_type TEXT NOT NULL CHECK(mime_type = 'image/png'),
                    created_at INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS jobs (
                    id TEXT PRIMARY KEY,
                    title TEXT NOT NULL,
                    body TEXT NOT NULL,
                    style TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    display_at INTEGER NOT NULL,
                    expires_at INTEGER,
                    is_immediate INTEGER NOT NULL CHECK(is_immediate IN (0, 1)),
                    guaranteed INTEGER NOT NULL CHECK(guaranteed IN (0, 1)),
                    status TEXT NOT NULL CHECK(
                        status IN ('pending', 'superseded', 'displayed', 'failed', 'expired')
                    ),
                    asset_sha256 TEXT NOT NULL REFERENCES assets(sha256)
                );
                CREATE INDEX IF NOT EXISTS jobs_manifest_idx
                ON jobs(status, is_immediate, display_at, expires_at);

                CREATE TABLE IF NOT EXISTS acknowledgements (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT NOT NULL REFERENCES devices(id),
                    job_id TEXT NOT NULL REFERENCES jobs(id),
                    state TEXT NOT NULL CHECK(state IN ('displayed', 'failed')),
                    message TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    UNIQUE(device_id, job_id, state)
                );

                CREATE TABLE IF NOT EXISTS pulse_sources (
                    source TEXT PRIMARY KEY,
                    observed_at INTEGER NOT NULL,
                    payload_json TEXT NOT NULL,
                    updated_at INTEGER NOT NULL
                );

                CREATE TABLE IF NOT EXISTS pulse_generations (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    generated_at INTEGER NOT NULL,
                    active INTEGER NOT NULL CHECK(active IN (0, 1))
                );
                CREATE UNIQUE INDEX IF NOT EXISTS pulse_one_active_generation
                ON pulse_generations(active) WHERE active = 1;

                CREATE TABLE IF NOT EXISTS pulse_generation_pages (
                    generation_id INTEGER NOT NULL REFERENCES pulse_generations(id),
                    page_index INTEGER NOT NULL CHECK(page_index BETWEEN 0 AND 2),
                    name TEXT NOT NULL,
                    asset_sha256 TEXT NOT NULL REFERENCES assets(sha256),
                    PRIMARY KEY(generation_id, page_index),
                    UNIQUE(generation_id, name)
                );

                CREATE TABLE IF NOT EXISTS voice_captures (
                    capture_id TEXT PRIMARY KEY,
                    device_id TEXT NOT NULL REFERENCES devices(id),
                    sha256 TEXT NOT NULL,
                    size INTEGER NOT NULL CHECK(size > 0),
                    state TEXT NOT NULL CHECK(
                        state IN ('pending', 'processing', 'completed', 'failed')
                    ),
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL,
                    transcript TEXT,
                    idea_title TEXT,
                    error TEXT
                );
                CREATE INDEX IF NOT EXISTS voice_captures_state_idx
                ON voice_captures(state, created_at);
                """
            )

    def store_voice_capture(
        self,
        device_id: str,
        capture_id: str,
        payload: bytes,
        *,
        now: int,
    ) -> VoiceCaptureRecord:
        if not _CAPTURE_ID_PATTERN.fullmatch(capture_id):
            raise StoreValidationError("invalid capture_id")
        if len(payload) < 44 or payload[:4] != b"RIFF" or payload[8:12] != b"WAVE":
            raise StoreValidationError("voice capture must be a WAV file")
        digest = hashlib.sha256(payload).hexdigest()
        path = self.voice_directory / f"{capture_id}.wav"
        with self._transaction() as connection:
            if connection.execute(
                "SELECT 1 FROM devices WHERE id = ?", (device_id,)
            ).fetchone() is None:
                raise NotFound("device not found")
            existing = connection.execute(
                "SELECT device_id, sha256, size, state FROM voice_captures WHERE capture_id = ?",
                (capture_id,),
            ).fetchone()
            if existing is not None:
                if existing["device_id"] != device_id or not hmac.compare_digest(
                    existing["sha256"], digest
                ):
                    raise InvalidTransition("capture_id already contains different audio")
                return VoiceCaptureRecord(
                    capture_id,
                    device_id,
                    digest,
                    existing["size"],
                    existing["state"],
                    path,
                    False,
                )

            file_descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{capture_id}.", suffix=".tmp", dir=self.voice_directory
            )
            try:
                with os.fdopen(file_descriptor, "wb") as stream:
                    stream.write(payload)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(temporary_name, path)
                connection.execute(
                    """
                    INSERT INTO voice_captures(
                        capture_id, device_id, sha256, size, state, created_at, updated_at
                    ) VALUES (?, ?, ?, ?, 'pending', ?, ?)
                    """,
                    (capture_id, device_id, digest, len(payload), now, now),
                )
            except Exception:
                Path(temporary_name).unlink(missing_ok=True)
                path.unlink(missing_ok=True)
                raise
        return VoiceCaptureRecord(
            capture_id, device_id, digest, len(payload), "pending", path, True
        )

    def claim_pending_voice_capture(self, *, now: int) -> PendingVoiceCapture | None:
        with self._transaction() as connection:
            row = connection.execute(
                """
                SELECT capture_id FROM voice_captures
                WHERE state = 'pending' ORDER BY created_at, capture_id LIMIT 1
                """
            ).fetchone()
            if row is None:
                return None
            connection.execute(
                "UPDATE voice_captures SET state = 'processing', updated_at = ? WHERE capture_id = ?",
                (now, row["capture_id"]),
            )
        return PendingVoiceCapture(
            row["capture_id"], self.voice_directory / f"{row['capture_id']}.wav"
        )

    def finish_voice_capture(
        self,
        capture_id: str,
        *,
        transcript: str | None,
        idea_title: str | None,
        error: str | None,
        now: int,
    ) -> None:
        state = "failed" if error else "completed"
        with self._transaction() as connection:
            result = connection.execute(
                """
                UPDATE voice_captures
                SET state = ?, transcript = ?, idea_title = ?, error = ?, updated_at = ?
                WHERE capture_id = ? AND state = 'processing'
                """,
                (state, transcript, idea_title, error, now, capture_id),
            )
            if result.rowcount != 1:
                raise InvalidTransition("voice capture is not processing")

    def voice_idea_items(self, *, limit: int = 3) -> list[dict[str, object]]:
        with self._lock:
            rows = self._connection.execute(
                """
                SELECT capture_id, state, created_at, idea_title
                FROM voice_captures ORDER BY created_at DESC, capture_id DESC LIMIT ?
                """,
                (limit,),
            ).fetchall()
        state_labels = {
            "pending": "待转写",
            "processing": "转写中",
            "completed": "已转写",
            "failed": "转写失败",
        }
        return [
            {
                "title": row["idea_title"] or "语音灵感",
                "state": state_labels[row["state"]],
                "captured_at": datetime.fromtimestamp(
                    row["created_at"], UTC
                ).isoformat(),
            }
            for row in rows
        ]

    def voice_capture_summaries(self, *, limit: int = 10) -> list[dict[str, object]]:
        with self._lock:
            rows = self._connection.execute(
                """
                SELECT capture_id, state, created_at, updated_at, transcript, idea_title, error
                FROM voice_captures ORDER BY created_at DESC, capture_id DESC LIMIT ?
                """,
                (max(1, min(limit, 50)),),
            ).fetchall()
        return [dict(row) for row in rows]

    @contextmanager
    def _transaction(self) -> Iterator[sqlite3.Connection]:
        with self._lock:
            self._connection.execute("BEGIN IMMEDIATE")
            try:
                yield self._connection
            except Exception:
                self._connection.execute("ROLLBACK")
                raise
            else:
                self._connection.execute("COMMIT")

    @staticmethod
    def _token_hash(token: str) -> str:
        return hashlib.sha256(token.encode("utf-8")).hexdigest()

    def register_device(self, device_id: str, token: str, *, now: int) -> None:
        if not device_id or len(device_id) > 128:
            raise StoreValidationError("device_id must contain 1 to 128 characters")
        if not token:
            raise StoreValidationError("device token must not be empty")
        token_hash = self._token_hash(token)
        with self._transaction() as connection:
            connection.execute(
                """
                INSERT INTO devices(id, token_hash, created_at, last_seen_at)
                VALUES (?, ?, ?, NULL)
                ON CONFLICT(id) DO UPDATE SET token_hash = excluded.token_hash
                """,
                (device_id, token_hash, now),
            )

    def authenticate_device(self, device_id: str, token: str, *, now: int) -> bool:
        candidate = self._token_hash(token)
        with self._lock:
            row = self._connection.execute(
                "SELECT token_hash FROM devices WHERE id = ?", (device_id,)
            ).fetchone()
            authenticated = row is not None and hmac.compare_digest(
                row["token_hash"], candidate
            )
            if authenticated:
                self._connection.execute(
                    "UPDATE devices SET last_seen_at = ? WHERE id = ?", (now, device_id)
                )
            return authenticated

    def authenticate_any_device(self, token: str, *, now: int) -> str | None:
        candidate = self._token_hash(token)
        matched_id: str | None = None
        with self._lock:
            rows = self._connection.execute(
                "SELECT id, token_hash FROM devices ORDER BY id"
            ).fetchall()
            for row in rows:
                if hmac.compare_digest(row["token_hash"], candidate):
                    matched_id = row["id"]
            if matched_id is not None:
                self._connection.execute(
                    "UPDATE devices SET last_seen_at = ? WHERE id = ?",
                    (now, matched_id),
                )
        return matched_id

    def put_asset(self, png_bytes: bytes, *, now: int) -> AssetRecord:
        if not png_bytes.startswith(b"\x89PNG\r\n\x1a\n"):
            raise StoreValidationError("asset must be a PNG")
        if len(png_bytes) > MAX_MANIFEST_ASSET_BYTES:
            raise StoreValidationError("asset exceeds the 4 MiB manifest byte limit")
        digest = hashlib.sha256(png_bytes).hexdigest()
        path = self.asset_directory / f"{digest}.png"
        if not path.exists():
            temporary_path: str | None = None
            try:
                with tempfile.NamedTemporaryFile(
                    dir=self.asset_directory,
                    prefix=".asset-",
                    suffix=".tmp",
                    delete=False,
                ) as temporary:
                    temporary_path = temporary.name
                    temporary.write(png_bytes)
                    temporary.flush()
                    os.fsync(temporary.fileno())
                os.chmod(temporary_path, 0o640)
                os.replace(temporary_path, path)
                temporary_path = None
            finally:
                if temporary_path is not None:
                    Path(temporary_path).unlink(missing_ok=True)
        else:
            existing = path.read_bytes()
            if len(existing) != len(png_bytes) or not hmac.compare_digest(
                hashlib.sha256(existing).hexdigest(), digest
            ):
                raise StoreError("content-addressed asset failed integrity validation")

        with self._transaction() as connection:
            connection.execute(
                """
                INSERT OR IGNORE INTO assets(sha256, size, mime_type, created_at)
                VALUES (?, ?, 'image/png', ?)
                """,
                (digest, len(png_bytes), now),
            )
            row = connection.execute(
                "SELECT size, mime_type FROM assets WHERE sha256 = ?", (digest,)
            ).fetchone()
            assert row is not None
            if row["size"] != len(png_bytes) or row["mime_type"] != "image/png":
                raise StoreError("asset metadata conflicts with existing content")
        return AssetRecord(digest, len(png_bytes), "image/png", path)

    def upsert_pulse_source(
        self,
        source: str,
        observed_at: int,
        payload: Mapping[str, object],
        *,
        now: int,
    ) -> None:
        if not source or len(source) > 32:
            raise StoreValidationError("pulse source must contain 1 to 32 characters")
        payload_json = json.dumps(
            dict(payload), ensure_ascii=False, separators=(",", ":"), sort_keys=True
        )
        with self._transaction() as connection:
            existing = connection.execute(
                "SELECT observed_at FROM pulse_sources WHERE source = ?", (source,)
            ).fetchone()
            if existing is not None and observed_at < existing["observed_at"]:
                raise StoreValidationError("pulse source update is older than stored data")
            connection.execute(
                """
                INSERT INTO pulse_sources(source, observed_at, payload_json, updated_at)
                VALUES (?, ?, ?, ?)
                ON CONFLICT(source) DO UPDATE SET
                    observed_at = excluded.observed_at,
                    payload_json = excluded.payload_json,
                    updated_at = excluded.updated_at
                """,
                (source, observed_at, payload_json, now),
            )

    def get_pulse_sources(self) -> dict[str, dict[str, object]]:
        with self._lock:
            rows = self._connection.execute(
                "SELECT source, observed_at, payload_json FROM pulse_sources ORDER BY source"
            ).fetchall()
        sources: dict[str, dict[str, object]] = {}
        for row in rows:
            payload = json.loads(row["payload_json"])
            if not isinstance(payload, dict):
                raise StoreError("stored pulse source payload is not an object")
            payload["observed_at"] = row["observed_at"]
            sources[row["source"]] = payload
        return sources

    def activate_pulse_bundle(
        self, bundle: PulseBundle, *, now: int
    ) -> PulseBundleRecord:
        expected_names = ("01-now", "02-work", "03-ideas")
        if len(bundle.pages) != 3 or tuple(page.name for page in bundle.pages) != expected_names:
            raise StoreValidationError("pulse bundle must contain the three ordered pages")
        assets = [self.put_asset(page.png_bytes, now=now) for page in bundle.pages]
        total_size = sum(asset.size for asset in assets)
        if total_size > MAX_MANIFEST_ASSET_BYTES:
            raise StoreValidationError("pulse bundle exceeds the 4 MiB manifest byte limit")

        with self._transaction() as connection:
            connection.execute("UPDATE pulse_generations SET active = 0 WHERE active = 1")
            cursor = connection.execute(
                "INSERT INTO pulse_generations(generated_at, active) VALUES (?, 1)",
                (bundle.generated_at,),
            )
            generation_id = int(cursor.lastrowid)
            for page, asset in zip(bundle.pages, assets, strict=True):
                connection.execute(
                    """
                    INSERT INTO pulse_generation_pages(
                        generation_id, page_index, name, asset_sha256
                    ) VALUES (?, ?, ?, ?)
                    """,
                    (generation_id, page.index, page.name, asset.sha256),
                )
            self._bump_manifest_version(connection)
        return self._pulse_bundle_record(generation_id)

    def get_active_pulse_pages(self) -> tuple[PulseAssetRecord, ...]:
        with self._lock:
            row = self._connection.execute(
                "SELECT id FROM pulse_generations WHERE active = 1"
            ).fetchone()
        if row is None:
            return ()
        return self._pulse_bundle_record(row["id"]).pages

    def _pulse_bundle_record(self, generation_id: int) -> PulseBundleRecord:
        with self._lock:
            generation = self._connection.execute(
                "SELECT generated_at FROM pulse_generations WHERE id = ?",
                (generation_id,),
            ).fetchone()
            rows = self._connection.execute(
                """
                SELECT p.page_index, p.name, a.sha256, a.size
                FROM pulse_generation_pages p
                JOIN assets a ON a.sha256 = p.asset_sha256
                WHERE p.generation_id = ?
                ORDER BY p.page_index
                """,
                (generation_id,),
            ).fetchall()
        if generation is None:
            raise NotFound("pulse generation not found")
        pages = tuple(
            PulseAssetRecord(
                index=row["page_index"],
                name=row["name"],
                sha256=row["sha256"],
                size=row["size"],
                path=self.asset_directory / f"{row['sha256']}.png",
            )
            for row in rows
        )
        return PulseBundleRecord(generation_id, generation["generated_at"], pages)

    def create_job(
        self, request: CardRequest, png_bytes: bytes, *, now: int
    ) -> JobRecord:
        asset = self.put_asset(png_bytes, now=now)
        job_id = str(uuid.uuid4())
        is_immediate = request.display_at is None
        display_at = now if is_immediate else request.display_at
        assert display_at is not None
        guaranteed = is_exact_delivery_guaranteed(request.display_at, now)
        with self._transaction() as connection:
            if is_immediate:
                connection.execute(
                    """
                    UPDATE jobs SET status = 'superseded'
                    WHERE is_immediate = 1 AND status = 'pending'
                    """
                )
            connection.execute(
                """
                INSERT INTO jobs(
                    id, title, body, style, created_at, display_at, expires_at,
                    is_immediate, guaranteed, status, asset_sha256
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'pending', ?)
                """,
                (
                    job_id,
                    request.title,
                    request.body,
                    request.style,
                    now,
                    display_at,
                    request.expires_at,
                    int(is_immediate),
                    int(guaranteed),
                    asset.sha256,
                ),
            )
            self._bump_manifest_version(connection)
        return JobRecord(
            id=job_id,
            created_at=now,
            display_at=display_at,
            expires_at=request.expires_at,
            guaranteed=guaranteed,
            asset_sha256=asset.sha256,
            asset_size=asset.size,
            asset_path=asset.path,
        )

    @staticmethod
    def _bump_manifest_version(connection: sqlite3.Connection) -> None:
        connection.execute(
            "UPDATE metadata SET value = value + 1 WHERE key = 'manifest_version'"
        )

    def _expire_jobs(self, now: int) -> None:
        with self._transaction() as connection:
            cursor = connection.execute(
                """
                UPDATE jobs SET status = 'expired'
                WHERE status = 'pending'
                  AND expires_at IS NOT NULL
                  AND expires_at <= ?
                """,
                (now,),
            )
            if cursor.rowcount:
                self._bump_manifest_version(connection)

    def build_manifest(
        self,
        device_id: str,
        *,
        now: int,
        limit: int = MAX_MANIFEST_JOBS,
    ) -> dict[str, object]:
        if limit <= 0 or limit > MAX_MANIFEST_JOBS:
            raise StoreValidationError(
                f"manifest limit must be between 1 and {MAX_MANIFEST_JOBS}"
            )
        with self._lock:
            device = self._connection.execute(
                "SELECT 1 FROM devices WHERE id = ?", (device_id,)
            ).fetchone()
        if device is None:
            raise NotFound("device not found")

        self._expire_jobs(now)
        columns = """
            j.id, j.display_at, j.expires_at,
            a.sha256 AS asset_sha256, a.size AS asset_size, a.mime_type
        """
        with self._lock:
            immediate = self._connection.execute(
                f"""
                SELECT {columns}
                FROM jobs j JOIN assets a ON a.sha256 = j.asset_sha256
                WHERE j.status = 'pending' AND j.is_immediate = 1
                ORDER BY j.created_at DESC, j.id DESC
                LIMIT 1
                """
            ).fetchone()
            scheduled = self._connection.execute(
                f"""
                SELECT {columns}
                FROM jobs j JOIN assets a ON a.sha256 = j.asset_sha256
                WHERE j.status = 'pending'
                  AND j.is_immediate = 0
                  AND j.display_at <= ?
                ORDER BY j.display_at ASC, j.created_at ASC, j.id ASC
                """,
                (now + MANIFEST_HORIZON_SECONDS,),
            )
            rows = []
            total_asset_bytes = 0
            if immediate is not None:
                rows.append(immediate)
                total_asset_bytes = immediate["asset_size"]
            for row in scheduled:
                if len(rows) >= limit:
                    break
                if total_asset_bytes + row["asset_size"] > MAX_MANIFEST_ASSET_BYTES:
                    continue
                rows.append(row)
                total_asset_bytes += row["asset_size"]
            manifest_version = self._connection.execute(
                "SELECT value FROM metadata WHERE key = 'manifest_version'"
            ).fetchone()["value"]
            pulse_rows = self._connection.execute(
                """
                SELECT p.page_index, p.name, a.sha256, a.size
                FROM pulse_generations g
                JOIN pulse_generation_pages p ON p.generation_id = g.id
                JOIN assets a ON a.sha256 = p.asset_sha256
                WHERE g.active = 1
                ORDER BY p.page_index
                """
            ).fetchall()

        jobs = [self._manifest_job(row) for row in rows]
        manifest: dict[str, object] = {
            "protocol_version": PROTOCOL_VERSION,
            "server_time": now,
            "manifest_version": manifest_version,
            "next_sync_at": next_regular_sync(now),
            "jobs": jobs,
        }
        if pulse_rows:
            manifest["pulse_pages"] = [
                {
                    "index": row["page_index"],
                    "name": row["name"],
                    "asset_path": f"/device/v1/assets/{row['sha256']}.png",
                    "asset_sha256": row["sha256"],
                    "asset_size": row["size"],
                }
                for row in pulse_rows
            ]
        return manifest

    @staticmethod
    def _manifest_job(row: sqlite3.Row) -> dict[str, object]:
        sha256 = row["asset_sha256"]
        return {
            "id": row["id"],
            "display_at": row["display_at"],
            "expires_at": row["expires_at"] or 0,
            "asset_path": f"/device/v1/assets/{sha256}.png",
            "asset_sha256": sha256,
            "asset_size": row["asset_size"],
            "mime_type": row["mime_type"],
        }

    @staticmethod
    def _validate_ack_message(message: object) -> str:
        if not isinstance(message, str):
            raise StoreValidationError("message must be a string")
        if len(message) > MAX_ACK_MESSAGE_CHARACTERS:
            raise StoreValidationError(
                f"message must be at most {MAX_ACK_MESSAGE_CHARACTERS} characters"
            )
        for character in message:
            if unicodedata.category(character) == "Cc" and character not in {
                "\n",
                "\t",
            }:
                raise StoreValidationError(
                    "message contains a forbidden control character"
                )
        return message

    def acknowledge(
        self,
        device_id: str,
        job_id: str,
        state: str,
        message: object,
        *,
        now: int,
    ) -> AckResult:
        if state not in _TERMINAL_STATES:
            raise StoreValidationError("state must be displayed or failed")
        normalized_message = self._validate_ack_message(message)
        with self._transaction() as connection:
            if (
                connection.execute(
                    "SELECT 1 FROM devices WHERE id = ?", (device_id,)
                ).fetchone()
                is None
            ):
                raise NotFound("device not found")
            job = connection.execute(
                "SELECT status FROM jobs WHERE id = ?", (job_id,)
            ).fetchone()
            if job is None:
                raise NotFound("job not found")
            existing = connection.execute(
                """
                SELECT 1 FROM acknowledgements
                WHERE device_id = ? AND job_id = ? AND state = ?
                """,
                (device_id, job_id, state),
            ).fetchone()
            if existing is not None:
                return AckResult(created=False, state=state)
            if job["status"] in _TERMINAL_STATES or job["status"] == "expired":
                raise InvalidTransition(
                    f"cannot change terminal job state {job['status']} to {state}"
                )
            connection.execute(
                """
                INSERT INTO acknowledgements(device_id, job_id, state, message, created_at)
                VALUES (?, ?, ?, ?, ?)
                """,
                (device_id, job_id, state, normalized_message, now),
            )
            connection.execute(
                "UPDATE jobs SET status = ? WHERE id = ?", (state, job_id)
            )
            self._bump_manifest_version(connection)
            return AckResult(created=True, state=state)

    def get_asset(self, sha256: str) -> AssetRecord:
        if not _SHA256_PATTERN.fullmatch(sha256):
            raise NotFound("asset not found")
        with self._lock:
            row = self._connection.execute(
                "SELECT size, mime_type FROM assets WHERE sha256 = ?", (sha256,)
            ).fetchone()
        path = self.asset_directory / f"{sha256}.png"
        if row is None or not path.is_file():
            raise NotFound("asset not found")
        payload = path.read_bytes()
        if len(payload) != row["size"] or not hmac.compare_digest(
            hashlib.sha256(payload).hexdigest(), sha256
        ):
            raise NotFound("asset not found")
        return AssetRecord(sha256, row["size"], row["mime_type"], path)

    def job_status(self, job_id: str) -> str:
        with self._lock:
            row = self._connection.execute(
                "SELECT status FROM jobs WHERE id = ?", (job_id,)
            ).fetchone()
        if row is None:
            raise NotFound("job not found")
        return row["status"]

    def close(self) -> None:
        with self._lock:
            self._connection.close()
