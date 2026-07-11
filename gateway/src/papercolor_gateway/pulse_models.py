"""Strict semantic models for deterministic PaperColor Pulse pages."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from types import MappingProxyType
import math
import unicodedata

from .models import ValidationError, parse_iso_timestamp


MAX_VISIBLE_ITEMS = 3


def _object(value: object, field: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise ValidationError(f"{field} must be an object")
    return value


def _reject_unknown(
    payload: Mapping[str, object], allowed: frozenset[str], field: str
) -> None:
    unknown = sorted(set(payload) - allowed)
    if unknown:
        raise ValidationError(f"Unknown field(s) in {field}: {', '.join(unknown)}")


def _text(value: object, field: str, maximum: int, *, required: bool = True) -> str | None:
    if value is None and not required:
        return None
    if not isinstance(value, str):
        raise ValidationError(f"{field} must be a string")
    normalized = value.replace("\r\n", "\n").replace("\r", "\n").strip()
    if not normalized:
        raise ValidationError(f"{field} must not be empty")
    if len(normalized) > maximum:
        raise ValidationError(f"{field} must be at most {maximum} characters")
    if any(
        unicodedata.category(character) == "Cc" and character not in {"\n", "\t"}
        for character in normalized
    ):
        raise ValidationError(f"{field} contains a forbidden control character")
    return normalized.replace("\t", "    ")


def _timestamp(value: object, field: str) -> int:
    parsed = parse_iso_timestamp(value, field)
    if parsed is None:
        raise ValidationError(f"{field} is required")
    return parsed


def _number(
    value: object, field: str, minimum: float, maximum: float
) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValidationError(f"{field} must be a number")
    normalized = float(value)
    if not math.isfinite(normalized) or normalized < minimum or normalized > maximum:
        raise ValidationError(f"{field} must be between {minimum:g} and {maximum:g}")
    return normalized


def _percentage(value: object, field: str) -> int:
    normalized = _number(value, field, 0, 100)
    if not normalized.is_integer():
        raise ValidationError(f"{field} must be a whole percentage")
    return int(normalized)


def _items(value: object, field: str) -> Sequence[object]:
    if isinstance(value, (str, bytes, bytearray)) or not isinstance(value, Sequence):
        raise ValidationError(f"{field} must be an array")
    if len(value) > MAX_VISIBLE_ITEMS:
        raise ValidationError(f"{field} must contain at most {MAX_VISIBLE_ITEMS} items")
    return value


@dataclass(frozen=True, slots=True)
class ObservedNumber:
    value: float
    observed_at: int

    @classmethod
    def from_mapping(
        cls,
        payload: object,
        field: str,
        *,
        minimum: float,
        maximum: float,
    ) -> ObservedNumber:
        data = _object(payload, field)
        _reject_unknown(data, frozenset({"value", "observed_at"}), field)
        if "value" not in data or "observed_at" not in data:
            raise ValidationError(f"{field}.value and {field}.observed_at are required")
        return cls(
            _number(data["value"], f"{field}.value", minimum, maximum),
            _timestamp(data["observed_at"], f"{field}.observed_at"),
        )


@dataclass(frozen=True, slots=True)
class Weather:
    condition: str
    low_c: float
    high_c: float
    observed_at: int

    @classmethod
    def from_mapping(cls, payload: object) -> Weather:
        data = _object(payload, "now.weather")
        _reject_unknown(
            data,
            frozenset({"condition", "low_c", "high_c", "observed_at"}),
            "now.weather",
        )
        condition = _text(data.get("condition"), "now.weather.condition", 20)
        low = _number(data.get("low_c"), "now.weather.low_c", -60, 70)
        high = _number(data.get("high_c"), "now.weather.high_c", -60, 70)
        if low > high:
            raise ValidationError("now.weather.low_c must not exceed high_c")
        return cls(
            condition=condition or "",
            low_c=low,
            high_c=high,
            observed_at=_timestamp(data.get("observed_at"), "now.weather.observed_at"),
        )


@dataclass(frozen=True, slots=True)
class Focus:
    title: str
    summary: str
    progress_pct: int | None
    observed_at: int

    @classmethod
    def from_mapping(cls, payload: object) -> Focus:
        data = _object(payload, "now.focus")
        _reject_unknown(
            data,
            frozenset({"title", "summary", "progress_pct", "observed_at"}),
            "now.focus",
        )
        progress = data.get("progress_pct")
        return cls(
            title=_text(data.get("title"), "now.focus.title", 48) or "",
            summary=_text(data.get("summary"), "now.focus.summary", 96) or "",
            progress_pct=(
                None if progress is None else _percentage(progress, "now.focus.progress_pct")
            ),
            observed_at=_timestamp(data.get("observed_at"), "now.focus.observed_at"),
        )


@dataclass(frozen=True, slots=True)
class LatestResult:
    text: str
    verified_at: int

    @classmethod
    def from_mapping(cls, payload: object) -> LatestResult:
        data = _object(payload, "now.latest_result")
        _reject_unknown(data, frozenset({"text", "verified_at"}), "now.latest_result")
        return cls(
            text=_text(data.get("text"), "now.latest_result.text", 100) or "",
            verified_at=_timestamp(
                data.get("verified_at"), "now.latest_result.verified_at"
            ),
        )


@dataclass(frozen=True, slots=True)
class NowView:
    temperature_c: ObservedNumber | None = None
    humidity_pct: ObservedNumber | None = None
    weather: Weather | None = None
    assistant_note: str | None = None
    focus: Focus | None = None
    latest_result: LatestResult | None = None

    @classmethod
    def from_mapping(cls, payload: object) -> NowView:
        data = _object(payload, "now")
        _reject_unknown(
            data,
            frozenset(
                {
                    "temperature_c",
                    "humidity_pct",
                    "weather",
                    "assistant_note",
                    "focus",
                    "latest_result",
                }
            ),
            "now",
        )
        return cls(
            temperature_c=(
                None
                if data.get("temperature_c") is None
                else ObservedNumber.from_mapping(
                    data["temperature_c"],
                    "now.temperature_c",
                    minimum=-40,
                    maximum=125,
                )
            ),
            humidity_pct=(
                None
                if data.get("humidity_pct") is None
                else ObservedNumber.from_mapping(
                    data["humidity_pct"],
                    "now.humidity_pct",
                    minimum=0,
                    maximum=100,
                )
            ),
            weather=None if data.get("weather") is None else Weather.from_mapping(data["weather"]),
            assistant_note=_text(
                data.get("assistant_note"), "now.assistant_note", 160, required=False
            ),
            focus=None if data.get("focus") is None else Focus.from_mapping(data["focus"]),
            latest_result=(
                None
                if data.get("latest_result") is None
                else LatestResult.from_mapping(data["latest_result"])
            ),
        )


@dataclass(frozen=True, slots=True)
class QuotaUsage:
    five_hour_pct: int
    weekly_pct: int
    observed_at: int

    @classmethod
    def from_mapping(cls, payload: object) -> QuotaUsage:
        data = _object(payload, "work.quota_usage")
        _reject_unknown(
            data,
            frozenset({"five_hour_pct", "weekly_pct", "observed_at"}),
            "work.quota_usage",
        )
        return cls(
            _percentage(data.get("five_hour_pct"), "work.quota_usage.five_hour_pct"),
            _percentage(data.get("weekly_pct"), "work.quota_usage.weekly_pct"),
            _timestamp(data.get("observed_at"), "work.quota_usage.observed_at"),
        )


@dataclass(frozen=True, slots=True)
class SessionItem:
    title: str
    priority: str
    status: str

    @classmethod
    def from_mapping(cls, payload: object, index: int) -> SessionItem:
        field = f"work.sessions[{index}]"
        data = _object(payload, field)
        _reject_unknown(data, frozenset({"title", "priority", "status"}), field)
        return cls(
            _text(data.get("title"), f"{field}.title", 48) or "",
            _text(data.get("priority"), f"{field}.priority", 8) or "",
            _text(data.get("status"), f"{field}.status", 72) or "",
        )


@dataclass(frozen=True, slots=True)
class WorkView:
    quota_usage: QuotaUsage | None = None
    sessions: tuple[SessionItem, ...] = ()

    @classmethod
    def from_mapping(cls, payload: object) -> WorkView:
        data = _object(payload, "work")
        _reject_unknown(data, frozenset({"quota_usage", "sessions"}), "work")
        raw_sessions = _items(data.get("sessions", ()), "work.sessions")
        return cls(
            quota_usage=(
                None
                if data.get("quota_usage") is None
                else QuotaUsage.from_mapping(data["quota_usage"])
            ),
            sessions=tuple(
                SessionItem.from_mapping(item, index)
                for index, item in enumerate(raw_sessions)
            ),
        )


@dataclass(frozen=True, slots=True)
class IdeaItem:
    title: str
    state: str
    captured_at: int

    @classmethod
    def from_mapping(cls, payload: object, index: int) -> IdeaItem:
        field = f"ideas.items[{index}]"
        data = _object(payload, field)
        _reject_unknown(data, frozenset({"title", "state", "captured_at"}), field)
        return cls(
            _text(data.get("title"), f"{field}.title", 72) or "",
            _text(data.get("state"), f"{field}.state", 16) or "",
            _timestamp(data.get("captured_at"), f"{field}.captured_at"),
        )


@dataclass(frozen=True, slots=True)
class IdeasView:
    items: tuple[IdeaItem, ...] = ()
    sd_state: str | None = None

    @classmethod
    def from_mapping(cls, payload: object) -> IdeasView:
        data = _object(payload, "ideas")
        _reject_unknown(data, frozenset({"items", "sd_state"}), "ideas")
        raw_items = _items(data.get("items", ()), "ideas.items")
        sd_state = data.get("sd_state")
        if sd_state is not None and sd_state not in {"ready", "missing", "error"}:
            raise ValidationError("ideas.sd_state must be ready, missing, or error")
        return cls(
            items=tuple(
                IdeaItem.from_mapping(item, index)
                for index, item in enumerate(raw_items)
            ),
            sd_state=sd_state,
        )


@dataclass(frozen=True, slots=True)
class PulseView:
    schema_version: int
    generated_at: int
    now: NowView
    work: WorkView
    ideas: IdeasView

    @classmethod
    def from_mapping(cls, payload: Mapping[str, object]) -> PulseView:
        data = _object(payload, "PulseView")
        _reject_unknown(
            data,
            frozenset({"schema_version", "generated_at", "now", "work", "ideas"}),
            "PulseView",
        )
        if data.get("schema_version") != 1:
            raise ValidationError("schema_version must be 1")
        return cls(
            schema_version=1,
            generated_at=_timestamp(data.get("generated_at"), "generated_at"),
            now=NowView.from_mapping(data.get("now", {})),
            work=WorkView.from_mapping(data.get("work", {})),
            ideas=IdeasView.from_mapping(data.get("ideas", {})),
        )


@dataclass(frozen=True, slots=True)
class AssistantUpdate:
    assistant_note: str
    focus_summary: str | None = None

    @classmethod
    def from_mapping(cls, payload: Mapping[str, object]) -> AssistantUpdate:
        data = _object(payload, "AssistantUpdate")
        _reject_unknown(data, frozenset({"assistant_note", "focus_summary"}), "AssistantUpdate")
        return cls(
            assistant_note=_text(data.get("assistant_note"), "assistant_note", 160) or "",
            focus_summary=_text(
                data.get("focus_summary"), "focus_summary", 96, required=False
            ),
        )


@dataclass(frozen=True, slots=True)
class SourceUpdate:
    source: str
    observed_at: int
    payload: Mapping[str, object]

    @classmethod
    def from_mapping(
        cls, source: str, payload: Mapping[str, object]
    ) -> SourceUpdate:
        data = _object(payload, f"source.{source}")
        allowed: dict[str, frozenset[str]] = {
            "device": frozenset(
                {"observed_at", "temperature_c", "humidity_pct", "sd_state"}
            ),
            "ha": frozenset({"observed_at", "weather"}),
            "mac": frozenset(
                {
                    "observed_at",
                    "quota_usage",
                    "sessions",
                    "focus",
                    "latest_result",
                    "ideas",
                }
            ),
        }
        if source not in allowed:
            raise ValidationError("source must be device, ha, or mac")
        _reject_unknown(data, allowed[source], f"source.{source}")
        observed_at = _timestamp(data.get("observed_at"), f"source.{source}.observed_at")
        normalized: dict[str, object] = {"observed_at": observed_at}

        if source == "device":
            if "temperature_c" in data:
                normalized["temperature_c"] = _number(
                    data["temperature_c"], "temperature_c", -40, 125
                )
            if "humidity_pct" in data:
                normalized["humidity_pct"] = _number(
                    data["humidity_pct"], "humidity_pct", 0, 100
                )
            if "sd_state" in data:
                if data["sd_state"] not in {"ready", "missing", "error"}:
                    raise ValidationError("sd_state must be ready, missing, or error")
                normalized["sd_state"] = data["sd_state"]
        else:
            for key, value in data.items():
                if key != "observed_at":
                    normalized[key] = value

        return cls(source, observed_at, MappingProxyType(normalized))
