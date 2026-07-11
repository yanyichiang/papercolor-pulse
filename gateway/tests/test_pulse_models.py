from __future__ import annotations

from datetime import UTC, datetime

import pytest

from papercolor_gateway.models import ValidationError
from papercolor_gateway.pulse_models import AssistantUpdate, PulseView, SourceUpdate


NOW = "2026-07-11T01:17:00+08:00"


def test_assistant_update_accepts_only_note_and_focus_summary() -> None:
    update = AssistantUpdate.from_mapping(
        {"assistant_note": "下午有阵雨，出门记得带伞。", "focus_summary": "先完成 Pulse V1。"}
    )

    assert update.assistant_note == "下午有阵雨，出门记得带伞。"
    assert update.focus_summary == "先完成 Pulse V1。"


def test_assistant_update_rejects_factual_fields() -> None:
    with pytest.raises(ValidationError, match="Unknown field"):
        AssistantUpdate.from_mapping(
            {"assistant_note": "带伞。", "temperature_c": 28.5}
        )


def test_pulse_view_accepts_partial_factual_sources() -> None:
    view = PulseView.from_mapping(
        {"schema_version": 1, "generated_at": NOW, "now": {}}
    )

    assert view.generated_at == int(
        datetime.fromisoformat(NOW).astimezone(UTC).timestamp()
    )
    assert view.now.temperature_c is None
    assert view.now.assistant_note is None
    assert view.work.sessions == ()
    assert view.ideas.items == ()


def test_pulse_view_rejects_naive_timestamp() -> None:
    with pytest.raises(ValidationError, match="timezone"):
        PulseView.from_mapping(
            {"schema_version": 1, "generated_at": "2026-07-11T01:17:00", "now": {}}
        )


def test_pulse_view_rejects_more_than_three_sessions() -> None:
    sessions = [
        {"title": f"Session {index}", "priority": "P1", "status": "active"}
        for index in range(4)
    ]
    with pytest.raises(ValidationError, match="at most 3"):
        PulseView.from_mapping(
            {
                "schema_version": 1,
                "generated_at": NOW,
                "now": {},
                "work": {"sessions": sessions},
            }
        )


def test_pulse_view_rejects_out_of_range_usage() -> None:
    with pytest.raises(ValidationError, match="between 0 and 100"):
        PulseView.from_mapping(
            {
                "schema_version": 1,
                "generated_at": NOW,
                "now": {},
                "work": {
                    "quota_usage": {
                        "five_hour_pct": 101,
                        "weekly_pct": 41,
                        "observed_at": NOW,
                    }
                },
            }
        )


def test_device_source_update_validates_telemetry() -> None:
    update = SourceUpdate.from_mapping(
        "device",
        {
            "observed_at": NOW,
            "temperature_c": 28.5,
            "humidity_pct": 62.0,
            "sd_state": "ready",
        },
    )

    assert update.source == "device"
    assert update.payload["sd_state"] == "ready"


def test_device_source_update_rejects_model_text() -> None:
    with pytest.raises(ValidationError, match="Unknown field"):
        SourceUpdate.from_mapping(
            "device", {"observed_at": NOW, "assistant_note": "我觉得有点热"}
        )
