"""Strict model-facing input validation."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import UTC, datetime
import time
import unicodedata
from collections.abc import Mapping
from urllib.parse import urlsplit


ALLOWED_STYLES = frozenset({"note", "brief", "quote", "postcard"})
MAX_TITLE_CHARACTERS = 80
MAX_BODY_CHARACTERS = 1_000
MAX_IMAGE_URL_CHARACTERS = 2_048
_ALLOWED_FIELDS = frozenset(
    {"title", "body", "style", "image_url", "display_at", "expires_at"}
)


class ValidationError(ValueError):
    """Raised when a tool argument does not satisfy the public contract."""


def _normalize_text(value: object, field: str, maximum: int) -> str:
    if not isinstance(value, str):
        raise ValidationError(f"{field} must be a string")
    normalized = value.replace("\r\n", "\n").replace("\r", "\n").strip()
    if not normalized:
        raise ValidationError(f"{field} must not be empty")
    if len(normalized) > maximum:
        label = f"{maximum:,}"
        raise ValidationError(f"{field} must be at most {label} characters")
    for character in normalized:
        if unicodedata.category(character) == "Cc" and character not in {"\n", "\t"}:
            raise ValidationError(f"{field} contains a forbidden control character")
    if field == "title" and ("\n" in normalized or "\t" in normalized):
        raise ValidationError("title must be a single line")
    return normalized.replace("\t", "    ")


def _optional_string(value: object, field: str) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str):
        raise ValidationError(f"{field} must be a string")
    normalized = value.strip()
    if not normalized:
        raise ValidationError(f"{field} must not be empty")
    return normalized


def parse_iso_timestamp(value: object, field: str) -> int | None:
    """Parse a bounded timezone-aware ISO-8601 string into UTC epoch seconds."""
    normalized = _optional_string(value, field)
    if normalized is None:
        return None
    if len(normalized) > 64:
        raise ValidationError(f"{field} is too long")
    try:
        parsed = datetime.fromisoformat(normalized.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ValidationError(f"{field} must be a valid ISO-8601 timestamp") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise ValidationError(f"{field} must include a timezone")
    try:
        return int(parsed.astimezone(UTC).timestamp())
    except (OverflowError, OSError, ValueError) as exc:
        raise ValidationError(f"{field} is outside the supported time range") from exc


def _validate_image_url(value: object) -> str | None:
    normalized = _optional_string(value, "image_url")
    if normalized is None:
        return None
    if len(normalized) > MAX_IMAGE_URL_CHARACTERS:
        raise ValidationError(
            f"image_url must be at most {MAX_IMAGE_URL_CHARACTERS:,} characters"
        )
    try:
        parsed = urlsplit(normalized)
        port = parsed.port
    except ValueError as exc:
        raise ValidationError("image_url is malformed") from exc
    if parsed.scheme.lower() != "https":
        raise ValidationError("image_url must use HTTPS")
    if not parsed.hostname:
        raise ValidationError("image_url must include a hostname")
    if parsed.username is not None or parsed.password is not None:
        raise ValidationError("image_url must not include credentials")
    if parsed.fragment:
        raise ValidationError("image_url must not include a fragment")
    if port not in (None, 443):
        raise ValidationError("image_url must use the standard HTTPS port")
    return normalized


@dataclass(frozen=True, slots=True)
class CardRequest:
    title: str
    body: str
    style: str = "note"
    image_url: str | None = None
    display_at: int | None = None
    expires_at: int | None = None

    @classmethod
    def from_mapping(
        cls, payload: Mapping[str, object], *, now: int | None = None
    ) -> CardRequest:
        if not isinstance(payload, Mapping):
            raise ValidationError("arguments must be an object")
        unknown = sorted(set(payload) - _ALLOWED_FIELDS)
        if unknown:
            raise ValidationError(f"Unknown field(s): {', '.join(unknown)}")
        if "title" not in payload or "body" not in payload:
            raise ValidationError("title and body are required")

        current_time = int(time.time()) if now is None else now
        title = _normalize_text(payload["title"], "title", MAX_TITLE_CHARACTERS)
        body = _normalize_text(payload["body"], "body", MAX_BODY_CHARACTERS)

        style_value = payload.get("style", "note")
        if not isinstance(style_value, str) or style_value not in ALLOWED_STYLES:
            raise ValidationError("style must be one of: note, brief, quote, postcard")

        display_at = parse_iso_timestamp(payload.get("display_at"), "display_at")
        expires_at = parse_iso_timestamp(payload.get("expires_at"), "expires_at")
        if display_at is not None and display_at <= current_time:
            raise ValidationError("display_at must be in the future")
        expiration_floor = display_at if display_at is not None else current_time
        if expires_at is not None and expires_at <= expiration_floor:
            raise ValidationError(
                "expires_at must be after display_at and the current time"
            )

        return cls(
            title=title,
            body=body,
            style=style_value,
            image_url=_validate_image_url(payload.get("image_url")),
            display_at=display_at,
            expires_at=expires_at,
        )
