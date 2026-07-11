from __future__ import annotations

from io import BytesIO
import json
from pathlib import Path

from PIL import Image

from papercolor_gateway.pulse_models import PulseView
from papercolor_gateway.pulse_renderer import PulseRenderer
from papercolor_gateway.renderer import EINK_PALETTE


FIXTURE = Path(__file__).parent / "fixtures/pulse-full.json"


def test_pulse_renderer_is_deterministic_three_page_six_color(
    cjk_serif_font_path: Path, latin_serif_font_path: Path
) -> None:
    view = PulseView.from_mapping(json.loads(FIXTURE.read_text()))
    renderer = PulseRenderer(cjk_serif_font_path, latin_serif_font_path)

    first = renderer.render(view)
    second = renderer.render(view)

    assert first == second
    assert [page.name for page in first.pages] == ["01-now", "02-work", "03-ideas"]
    assert len({page.sha256 for page in first.pages}) == 3
    for page in first.pages:
        image = Image.open(BytesIO(page.png_bytes))
        assert image.size == (400, 600)
        assert image.mode == "RGB"
        assert set(image.getdata()) <= set(EINK_PALETTE)


def test_now_page_uses_white_inline_assistant_region_and_bounded_accent(
    cjk_serif_font_path: Path, latin_serif_font_path: Path
) -> None:
    view = PulseView.from_mapping(json.loads(FIXTURE.read_text()))
    page = PulseRenderer(cjk_serif_font_path, latin_serif_font_path).render(view).pages[0]
    image = Image.open(BytesIO(page.png_bytes))

    assistant_region = image.crop((24, 206, 376, 326))
    colors = assistant_region.getcolors(maxcolors=assistant_region.width * assistant_region.height) or []
    white_pixels = next((count for count, color in colors if color == (255, 255, 255)), 0)
    assert white_pixels / (assistant_region.width * assistant_region.height) > 0.70

    all_colors = image.getcolors(maxcolors=400 * 600) or []
    accent_pixels = sum(
        count for count, color in all_colors if color in {(215, 38, 45), (244, 196, 48)}
    )
    assert 20 < accent_pixels < 400 * 600 * 0.15


def test_partial_view_renders_unavailable_facts_without_zeroes(
    cjk_serif_font_path: Path, latin_serif_font_path: Path
) -> None:
    view = PulseView.from_mapping(
        {"schema_version": 1, "generated_at": "2026-07-11T01:17:00+08:00", "now": {}}
    )

    bundle = PulseRenderer(cjk_serif_font_path, latin_serif_font_path).render(view)

    assert len(bundle.pages) == 3
    assert all(page.png_bytes.startswith(b"\x89PNG\r\n\x1a\n") for page in bundle.pages)


def test_ideas_page_centers_microsd_status_block(
    cjk_serif_font_path: Path, latin_serif_font_path: Path
) -> None:
    view = PulseView.from_mapping(json.loads(FIXTURE.read_text()))
    page = PulseRenderer(cjk_serif_font_path, latin_serif_font_path).render(view).pages[2]
    image = Image.open(BytesIO(page.png_bytes)).convert("RGB")

    region = image.crop((24, 392, 376, 446))
    nonwhite = Image.new("1", region.size)
    pixels = nonwhite.load()
    for y in range(region.height):
        for x in range(region.width):
            pixels[x, y] = region.getpixel((x, y)) != (255, 255, 255)
    bbox = nonwhite.getbbox()

    assert bbox is not None
    center_x = 24 + (bbox[0] + bbox[2]) / 2
    assert abs(center_x - 200) <= 4
