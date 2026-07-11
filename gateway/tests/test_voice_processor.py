from pathlib import Path

from papercolor_gateway.store import JobStore
from papercolor_gateway.voice_processor import VoiceProcessor, idea_title_from_transcript


NOW = 1_800_000_000


def _wav() -> bytes:
    return b"RIFF" + (40).to_bytes(4, "little") + b"WAVE" + b"\0" * 36


def test_voice_processor_completes_capture_and_exposes_idea(tmp_path: Path) -> None:
    store = JobStore(tmp_path / "gateway.sqlite3", tmp_path / "assets")
    store.register_device("papercolor", "d" * 32, now=NOW)
    store.store_voice_capture("papercolor", "capture-1", _wav(), now=NOW)
    processor = VoiceProcessor(store, lambda _path: "把 PaperColor 做成灵感终端。后面继续完善。")

    assert processor.process_one(now=NOW + 1) == "capture-1"
    assert processor.process_one(now=NOW + 2) is None
    assert store.voice_idea_items() == [
        {
            "title": "把 PaperColor 做成灵感终端",
            "state": "已转写",
            "captured_at": "2027-01-15T08:00:00+00:00",
        }
    ]


def test_idea_title_is_bounded_and_deterministic() -> None:
    assert idea_title_from_transcript("  一个想法！ 后续内容 ") == "一个想法"
    assert len(idea_title_from_transcript("很长" * 40)) == 48
