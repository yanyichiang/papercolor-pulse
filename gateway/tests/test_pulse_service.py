from __future__ import annotations

from pathlib import Path

import pytest

from papercolor_gateway.pulse_models import AssistantUpdate, SourceUpdate
from papercolor_gateway.pulse_renderer import PulseRenderer
from papercolor_gateway.pulse_service import PulseRenderError, PulseService
from papercolor_gateway.store import JobStore, StoreValidationError


NOW = 1_783_706_220
NOW_ISO = "2026-07-11T01:17:00+08:00"


def _store(tmp_path: Path) -> JobStore:
    return JobStore(tmp_path / "gateway.sqlite3", tmp_path / "assets")


def test_source_update_renders_and_activates_three_pages(
    tmp_path: Path, cjk_serif_font_path: Path, latin_serif_font_path: Path
) -> None:
    store = _store(tmp_path)
    service = PulseService(
        store, PulseRenderer(cjk_serif_font_path, latin_serif_font_path)
    )

    generation = service.update_source(
        SourceUpdate.from_mapping(
            "device",
            {
                "observed_at": NOW_ISO,
                "temperature_c": 28.5,
                "humidity_pct": 62,
                "sd_state": "ready",
            },
        ),
        now=NOW,
    )

    assert generation.generated_at == NOW
    assert [page.name for page in generation.pages] == ["01-now", "02-work", "03-ideas"]
    assert store.get_active_pulse_pages() == generation.pages
    store.close()


def test_pending_voice_capture_is_renderable_as_an_idea(
    tmp_path: Path, cjk_serif_font_path: Path, latin_serif_font_path: Path
) -> None:
    store = _store(tmp_path)
    store.register_device("papercolor", "d" * 32, now=NOW)
    wav = b"RIFF" + (40).to_bytes(4, "little") + b"WAVE" + b"\0" * 36
    store.store_voice_capture("papercolor", "capture-pending", wav, now=NOW)
    service = PulseService(
        store, PulseRenderer(cjk_serif_font_path, latin_serif_font_path)
    )

    generation = service.update_source(
        SourceUpdate.from_mapping(
            "device", {"observed_at": NOW_ISO, "sd_state": "ready"}
        ),
        now=NOW,
    )

    assert len(generation.pages) == 3
    assert service.compose_view(now=NOW).ideas.items[0].state == "待转写"
    store.close()


def test_older_source_snapshot_is_rejected(
    tmp_path: Path, cjk_serif_font_path: Path, latin_serif_font_path: Path
) -> None:
    store = _store(tmp_path)
    service = PulseService(
        store, PulseRenderer(cjk_serif_font_path, latin_serif_font_path)
    )
    newer = SourceUpdate.from_mapping(
        "device", {"observed_at": NOW_ISO, "sd_state": "ready"}
    )
    older = SourceUpdate.from_mapping(
        "device", {"observed_at": "2026-07-11T01:16:00+08:00", "sd_state": "missing"}
    )
    service.update_source(newer, now=NOW)

    with pytest.raises(StoreValidationError, match="older"):
        service.update_source(older, now=NOW + 1)
    store.close()


def test_failed_render_keeps_previous_active_bundle(
    tmp_path: Path, cjk_serif_font_path: Path, latin_serif_font_path: Path
) -> None:
    store = _store(tmp_path)
    renderer = PulseRenderer(cjk_serif_font_path, latin_serif_font_path)
    service = PulseService(store, renderer)
    first = service.update_source(
        SourceUpdate.from_mapping(
            "device", {"observed_at": NOW_ISO, "sd_state": "ready"}
        ),
        now=NOW,
    )

    class FailingRenderer:
        def render(self, view):
            raise RuntimeError("page 02 failed")

    service.renderer = FailingRenderer()
    with pytest.raises(PulseRenderError, match="page 02 failed"):
        service.update_assistant(
            AssistantUpdate.from_mapping({"assistant_note": "记得带伞。"}),
            now=NOW + 1,
        )

    assert store.get_active_pulse_pages() == first.pages
    store.close()


def test_assistant_update_cannot_replace_device_facts(
    tmp_path: Path, cjk_serif_font_path: Path, latin_serif_font_path: Path
) -> None:
    store = _store(tmp_path)
    service = PulseService(
        store, PulseRenderer(cjk_serif_font_path, latin_serif_font_path)
    )
    service.update_source(
        SourceUpdate.from_mapping(
            "device",
            {
                "observed_at": NOW_ISO,
                "temperature_c": 28.5,
                "humidity_pct": 62,
                "sd_state": "ready",
            },
        ),
        now=NOW,
    )

    service.update_assistant(
        AssistantUpdate.from_mapping({"assistant_note": "室内有点热。"}), now=NOW + 1
    )
    view = service.compose_view(now=NOW + 1)

    assert view.now.temperature_c is not None
    assert view.now.temperature_c.value == 28.5
    assert view.now.assistant_note == "室内有点热。"
    store.close()
