from io import BytesIO

from PIL import Image, ImageDraw

from papercolor_gateway.models import CardRequest
from papercolor_gateway.renderer import EINK_PALETTE, CardRenderer, fit_text


def test_fit_text_wraps_cjk_by_measured_width_and_ellipsizes(font_path) -> None:
    image = Image.new("RGB", (400, 600), "white")
    draw = ImageDraw.Draw(image)

    layout = fit_text(
        draw,
        "这是一段需要按照真实字形宽度换行的中文内容。" * 20,
        font_path,
        max_size=28,
        min_size=18,
        max_width=300,
        max_height=100,
        spacing=4,
    )

    assert layout.truncated is True
    assert layout.lines[-1].endswith("…")
    assert all(draw.textlength(line, font=layout.font) <= 300 for line in layout.lines)
    assert layout.height <= 100


def test_renderer_is_deterministic_nonblank_rgb_400x600(font_path) -> None:
    renderer = CardRenderer(font_path)
    request = CardRequest.from_mapping(
        {"title": "今日计划", "body": "完成网关测试，然后喝一杯茶。", "style": "note"},
        now=1_700_000_000,
    )

    first = renderer.render(request)
    second = renderer.render(request)
    rendered = Image.open(BytesIO(first))

    assert first == second
    assert rendered.size == (400, 600)
    assert rendered.mode == "RGB"
    colors = {pixel for _, pixel in rendered.getcolors(maxcolors=400 * 600) or []}
    assert colors <= set(EINK_PALETTE)
    assert len(colors) > 1


def test_all_styles_render_with_bounded_remote_image(font_path) -> None:
    renderer = CardRenderer(font_path)
    source = Image.new("RGB", (1200, 300), (18, 120, 210))

    for style in ("note", "brief", "quote", "postcard"):
        request = CardRequest.from_mapping(
            {"title": "A title", "body": "A body", "style": style},
            now=1_700_000_000,
        )
        rendered = Image.open(BytesIO(renderer.render(request, source)))
        assert rendered.size == (400, 600)
