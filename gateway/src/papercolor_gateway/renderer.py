"""Deterministic PaperColor card renderer."""

from __future__ import annotations

from dataclasses import dataclass
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

from .models import CardRequest


WIDTH = 400
HEIGHT = 600
EINK_PALETTE: tuple[tuple[int, int, int], ...] = (
    (255, 255, 255),
    (0, 0, 0),
    (215, 38, 45),
    (244, 196, 48),
    (28, 126, 74),
    (34, 89, 173),
)


@dataclass(frozen=True, slots=True)
class TextLayout:
    font: ImageFont.FreeTypeFont
    lines: tuple[str, ...]
    line_height: int
    height: int
    truncated: bool


@dataclass(frozen=True, slots=True)
class StyleLayout:
    background: tuple[int, int, int]
    accent: tuple[int, int, int]
    image_box: tuple[int, int, int, int]
    title_box: tuple[int, int, int, int]
    body_box: tuple[int, int, int, int]


STYLE_LAYOUTS: dict[str, StyleLayout] = {
    "note": StyleLayout(
        (255, 255, 255),
        (244, 196, 48),
        (30, 54, 370, 192),
        (30, 215, 370, 305),
        (30, 326, 370, 535),
    ),
    "brief": StyleLayout(
        (255, 255, 255),
        (34, 89, 173),
        (30, 54, 370, 174),
        (30, 197, 370, 280),
        (30, 302, 370, 535),
    ),
    "quote": StyleLayout(
        (255, 255, 255),
        (215, 38, 45),
        (30, 54, 370, 150),
        (54, 180, 370, 268),
        (54, 292, 360, 535),
    ),
    "postcard": StyleLayout(
        (255, 255, 255),
        (28, 126, 74),
        (30, 54, 370, 260),
        (30, 282, 370, 362),
        (30, 382, 370, 535),
    ),
}


def _line_height(draw: ImageDraw.ImageDraw, font: ImageFont.FreeTypeFont) -> int:
    box = draw.textbbox((0, 0), "Ag国", font=font)
    return max(1, box[3] - box[1])


def _wrap_measured(
    draw: ImageDraw.ImageDraw,
    text: str,
    font: ImageFont.FreeTypeFont,
    max_width: int,
) -> list[str]:
    lines: list[str] = []
    for paragraph in text.split("\n"):
        if not paragraph:
            lines.append("")
            continue
        current = ""
        for character in paragraph:
            candidate = current + character
            if current and draw.textlength(candidate, font=font) > max_width:
                lines.append(current.rstrip())
                current = character.lstrip()
            else:
                current = candidate
        if current or not lines:
            lines.append(current.rstrip())
    return lines


def _ellipsize(
    draw: ImageDraw.ImageDraw,
    line: str,
    font: ImageFont.FreeTypeFont,
    max_width: int,
) -> str:
    ellipsis = "…"
    candidate = line.rstrip()
    while candidate and draw.textlength(candidate + ellipsis, font=font) > max_width:
        candidate = candidate[:-1].rstrip()
    return candidate + ellipsis


def fit_text(
    draw: ImageDraw.ImageDraw,
    text: str,
    font_path: str | Path,
    *,
    max_size: int,
    min_size: int,
    max_width: int,
    max_height: int,
    spacing: int,
) -> TextLayout:
    """Fit text using measured glyph widths, reducing then visibly truncating."""
    if min_size <= 0 or max_size < min_size:
        raise ValueError("invalid font size range")
    if max_width <= 0 or max_height <= 0:
        raise ValueError("text bounds must be positive")

    for size in range(max_size, min_size - 1, -1):
        font = ImageFont.truetype(str(font_path), size)
        line_height = _line_height(draw, font)
        lines = _wrap_measured(draw, text, font, max_width)
        height = len(lines) * line_height + max(0, len(lines) - 1) * spacing
        if height <= max_height:
            return TextLayout(font, tuple(lines), line_height, height, False)

    font = ImageFont.truetype(str(font_path), min_size)
    line_height = _line_height(draw, font)
    lines = _wrap_measured(draw, text, font, max_width)
    max_lines = max(1, (max_height + spacing) // (line_height + spacing))
    visible = lines[:max_lines]
    truncated = len(lines) > max_lines
    if truncated:
        visible[-1] = _ellipsize(draw, visible[-1], font, max_width)
    height = len(visible) * line_height + max(0, len(visible) - 1) * spacing
    return TextLayout(font, tuple(visible), line_height, height, truncated)


def _palette_image() -> Image.Image:
    palette = Image.new("P", (1, 1))
    flat = [component for color in EINK_PALETTE for component in color]
    palette.putpalette(flat + [0] * (768 - len(flat)))
    return palette


class CardRenderer:
    """Render normalized cards to stable 400 x 600 six-color RGB PNGs."""

    def __init__(self, font_path: str | Path):
        self.font_path = Path(font_path)
        if not self.font_path.is_file():
            raise FileNotFoundError(f"font not found: {self.font_path}")

    def render(
        self, request: CardRequest, source_image: Image.Image | None = None
    ) -> bytes:
        layout = STYLE_LAYOUTS[request.style]
        canvas = Image.new("RGB", (WIDTH, HEIGHT), layout.background)
        draw = ImageDraw.Draw(canvas)

        draw.rectangle((0, 0, WIDTH - 1, 13), fill=layout.accent)
        draw.rectangle((0, HEIGHT - 9, WIDTH - 1, HEIGHT - 1), fill=layout.accent)
        if request.style == "quote":
            draw.rectangle((30, 178, 40, 535), fill=layout.accent)

        image_box = layout.image_box
        draw.rectangle(image_box, outline=(0, 0, 0), width=2)
        if source_image is not None:
            normalized = ImageOps.exif_transpose(source_image).convert("RGB")
            fitted = ImageOps.fit(
                normalized,
                (image_box[2] - image_box[0] - 4, image_box[3] - image_box[1] - 4),
                method=Image.Resampling.LANCZOS,
                centering=(0.5, 0.5),
            )
            canvas.paste(fitted, (image_box[0] + 2, image_box[1] + 2))
        else:
            draw.rectangle(
                (
                    image_box[0] + 2,
                    image_box[1] + 2,
                    image_box[2] - 2,
                    image_box[3] - 2,
                ),
                fill=layout.accent,
            )

        title_box = layout.title_box
        title = fit_text(
            draw,
            request.title,
            self.font_path,
            max_size=38,
            min_size=24,
            max_width=title_box[2] - title_box[0],
            max_height=title_box[3] - title_box[1],
            spacing=4,
        )
        self._draw_lines(draw, title, title_box[0], title_box[1], 4)

        body_box = layout.body_box
        body = fit_text(
            draw,
            request.body,
            self.font_path,
            max_size=27,
            min_size=18,
            max_width=body_box[2] - body_box[0],
            max_height=body_box[3] - body_box[1],
            spacing=7,
        )
        self._draw_lines(draw, body, body_box[0], body_box[1], 7)

        footer_font = ImageFont.truetype(str(self.font_path), 15)
        footer = f"PAPERCOLOR  {request.style.upper()}"
        draw.text((30, 562), footer, font=footer_font, fill=(0, 0, 0))

        quantized = canvas.quantize(
            palette=_palette_image(),
            dither=Image.Dither.FLOYDSTEINBERG,
        ).convert("RGB")
        output = BytesIO()
        quantized.save(output, format="PNG", optimize=False, compress_level=9)
        return output.getvalue()

    @staticmethod
    def _draw_lines(
        draw: ImageDraw.ImageDraw,
        layout: TextLayout,
        x: int,
        y: int,
        spacing: int,
    ) -> None:
        cursor_y = y
        for line in layout.lines:
            draw.text((x, cursor_y), line, font=layout.font, fill=(0, 0, 0))
            cursor_y += layout.line_height + spacing
