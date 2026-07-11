from pathlib import Path

import pytest


@pytest.fixture(scope="session")
def font_path() -> Path:
    candidates = (
        Path("/System/Library/Fonts/Supplemental/Arial Unicode.ttf"),
        Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    pytest.skip("no test font available")


@pytest.fixture(scope="session")
def cjk_serif_font_path() -> Path:
    candidates = (
        Path.home() / "Library/Fonts/NotoSerifSC-Regular.otf",
        Path("/System/Library/Fonts/Supplemental/Songti.ttc"),
        Path("/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    pytest.skip("no CJK serif test font available")


@pytest.fixture(scope="session")
def latin_serif_font_path() -> Path:
    candidates = (
        Path.home() / "Library/Fonts/AnthropicSerif-Roman.ttf",
        Path("/System/Library/Fonts/Supplemental/Georgia.ttf"),
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    pytest.skip("no Latin serif test font available")
