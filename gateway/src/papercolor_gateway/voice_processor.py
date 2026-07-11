"""Process persisted PaperColor voice captures with a local transcriber."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Sequence
from pathlib import Path
import re
import time

from .store import JobStore


def idea_title_from_transcript(transcript: str) -> str:
    normalized = re.sub(r"\s+", " ", transcript).strip()
    if not normalized:
        return "语音灵感"
    sentence = re.split(r"[。！？!?\n]", normalized, maxsplit=1)[0].strip()
    return (sentence or normalized)[:48]


class VoiceProcessor:
    def __init__(self, store: JobStore, transcribe: Callable[[Path], str]):
        self.store = store
        self.transcribe = transcribe

    def process_one(self, *, now: int) -> str | None:
        capture = self.store.claim_pending_voice_capture(now=now)
        if capture is None:
            return None
        try:
            transcript = self.transcribe(capture.path).strip()
            if not transcript:
                raise ValueError("transcriber returned empty text")
            self.store.finish_voice_capture(
                capture.capture_id,
                transcript=transcript[:4000],
                idea_title=idea_title_from_transcript(transcript),
                error=None,
                now=now,
            )
        except Exception as exc:
            self.store.finish_voice_capture(
                capture.capture_id,
                transcript=None,
                idea_title=None,
                error=str(exc)[:500],
                now=now,
            )
        return capture.capture_id


def _local_whisper(model_name: str) -> Callable[[Path], str]:
    try:
        from faster_whisper import WhisperModel
    except ImportError as exc:
        raise RuntimeError("install the gateway voice extra") from exc
    model = WhisperModel(model_name, device="cpu", compute_type="int8")

    def transcribe(path: Path) -> str:
        segments, _ = model.transcribe(
            str(path), language="zh", vad_filter=True, beam_size=5
        )
        return "".join(segment.text for segment in segments).strip()

    return transcribe


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--model", default="small")
    parser.add_argument("--max-items", type=int, default=4)
    parser.add_argument("--watch", action="store_true")
    parser.add_argument("--poll-seconds", type=float, default=10.0)
    args = parser.parse_args(argv)
    store = JobStore(args.data_dir / "gateway.sqlite3", args.data_dir / "assets")
    try:
        processor = VoiceProcessor(store, _local_whisper(args.model))
        while True:
            processed = 0
            while processed < max(1, args.max_items):
                capture_id = processor.process_one(now=int(time.time()))
                if capture_id is None:
                    break
                print(capture_id, flush=True)
                processed += 1
            if not args.watch:
                break
            time.sleep(max(1.0, args.poll_seconds))
    finally:
        store.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
