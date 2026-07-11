"""Deterministic three-page Absolutely renderer for PaperColor Pulse."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
import hashlib
from io import BytesIO
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from PIL import Image, ImageDraw, ImageFont

from .pulse_models import PulseView
from .renderer import EINK_PALETTE, HEIGHT, WIDTH, _palette_image, fit_text


PAPER = (255, 255, 255)
INK = (0, 0, 0)
GRAY_TARGET = (198, 198, 194)
ACCENT_TARGET = (204, 125, 94)
ACCENT_SOLID = (215, 38, 45)


@dataclass(frozen=True, slots=True)
class PulsePage:
    index: int
    name: str
    png_bytes: bytes
    sha256: str


@dataclass(frozen=True, slots=True)
class PulseBundle:
    generated_at: int
    pages: tuple[PulsePage, PulsePage, PulsePage]


class PulseRenderer:
    """Render one semantic Pulse snapshot into three stable e-ink pages."""

    def __init__(
        self,
        cjk_font_path: str | Path,
        latin_font_path: str | Path,
        *,
        timezone_name: str = "Asia/Shanghai",
    ) -> None:
        self.cjk_font_path = Path(cjk_font_path)
        self.latin_font_path = Path(latin_font_path)
        for path in (self.cjk_font_path, self.latin_font_path):
            if not path.is_file():
                raise FileNotFoundError(f"font not found: {path}")
        try:
            self.timezone = ZoneInfo(timezone_name)
        except ZoneInfoNotFoundError as exc:
            raise ValueError(f"unknown timezone: {timezone_name}") from exc

    def render(self, view: PulseView) -> PulseBundle:
        rendered = (
            ("01-now", self._render_now(view)),
            ("02-work", self._render_work(view)),
            ("03-ideas", self._render_ideas(view)),
        )
        pages = tuple(
            PulsePage(index, name, png, hashlib.sha256(png).hexdigest())
            for index, (name, png) in enumerate(rendered)
        )
        return PulseBundle(view.generated_at, pages)  # type: ignore[arg-type]

    def _font(self, size: int, *, latin: bool = False) -> ImageFont.FreeTypeFont:
        path = self.latin_font_path if latin else self.cjk_font_path
        return ImageFont.truetype(str(path), size)

    def _canvas(self) -> tuple[Image.Image, ImageDraw.ImageDraw]:
        canvas = Image.new("RGB", (WIDTH, HEIGHT), PAPER)
        return canvas, ImageDraw.Draw(canvas)

    @staticmethod
    def _line(draw: ImageDraw.ImageDraw, y: int) -> None:
        draw.line((24, y, 376, y), fill=GRAY_TARGET, width=1)

    def _label(self, draw: ImageDraw.ImageDraw, text: str, x: int, y: int) -> None:
        draw.text((x, y), text, font=self._font(13, latin=True), fill=ACCENT_SOLID)

    def _text_box(
        self,
        draw: ImageDraw.ImageDraw,
        text: str,
        box: tuple[int, int, int, int],
        *,
        max_size: int,
        min_size: int,
        spacing: int = 4,
        fill: tuple[int, int, int] = INK,
        latin: bool = False,
        align: str = "left",
    ) -> None:
        if align not in {"left", "center", "right"}:
            raise ValueError(f"unknown text alignment: {align}")
        latin = latin or text.isascii()
        font_path = self.latin_font_path if latin else self.cjk_font_path
        layout = fit_text(
            draw,
            text,
            font_path,
            max_size=max_size,
            min_size=min_size,
            max_width=box[2] - box[0],
            max_height=box[3] - box[1],
            spacing=spacing,
        )
        y = box[1]
        for line in layout.lines:
            line_width = draw.textlength(line, font=layout.font)
            if align == "center":
                x = box[0] + ((box[2] - box[0]) - line_width) / 2
            elif align == "right":
                x = box[2] - line_width
            else:
                x = box[0]
            draw.text((round(x), y), line, font=layout.font, fill=fill)
            y += layout.line_height + spacing

    def _footer(self, draw: ImageDraw.ImageDraw, active: int) -> None:
        self._line(draw, 559)
        font = self._font(11, latin=True)
        draw.text((24, 570), "B  Previous", font=font, fill=INK)
        draw.text((318, 570), "A  Next", font=font, fill=INK)
        for index, x in enumerate((187, 200, 213)):
            fill = ACCENT_SOLID if index == active else PAPER
            outline = ACCENT_SOLID if index == active else INK
            draw.ellipse((x, 573, x + 6, 579), fill=fill, outline=outline, width=1)

    def _finish(self, canvas: Image.Image) -> bytes:
        quantized = canvas.quantize(
            palette=_palette_image(), dither=Image.Dither.FLOYDSTEINBERG
        ).convert("RGB")
        output = BytesIO()
        quantized.save(output, format="PNG", optimize=False, compress_level=9)
        return output.getvalue()

    def _render_now(self, view: PulseView) -> bytes:
        canvas, draw = self._canvas()
        current = datetime.fromtimestamp(view.generated_at, self.timezone)
        draw.text((24, 19), current.strftime("%H:%M"), font=self._font(48, latin=True), fill=INK)
        weekday = "一二三四五六日"[current.weekday()]
        draw.text(
            (24, 68),
            f"{current.month} 月 {current.day} 日 · 周{weekday}",
            font=self._font(13),
            fill=INK,
        )
        weather = view.now.weather
        if weather is not None:
            self._text_box(draw, weather.condition, (285, 22, 376, 52), max_size=24, min_size=18)
            draw.text(
                (293, 60),
                f"{weather.low_c:g}–{weather.high_c:g}°C",
                font=self._font(14, latin=True),
                fill=INK,
            )
        else:
            draw.text((315, 40), "—", font=self._font(24, latin=True), fill=INK)
        self._line(draw, 92)

        draw.text((24, 108), "室内温度", font=self._font(13), fill=INK)
        draw.text((208, 108), "相对湿度", font=self._font(13), fill=INK)
        temperature = view.now.temperature_c
        humidity = view.now.humidity_pct
        temp_text = "不可用" if temperature is None else f"{temperature.value:.1f}°"
        humi_text = "不可用" if humidity is None else f"{humidity.value:.0f}%"
        draw.text((24, 137), temp_text, font=self._font(32, latin=True), fill=INK)
        draw.text((208, 137), humi_text, font=self._font(32, latin=True), fill=INK)
        draw.line((196, 105, 196, 181), fill=GRAY_TARGET, width=1)
        self._line(draw, 194)

        if view.now.assistant_note:
            draw.text((24, 211), "Assistant", font=self._font(14, latin=True), fill=ACCENT_SOLID)
            self._text_box(
                draw,
                view.now.assistant_note,
                (24, 237, 376, 304),
                max_size=18,
                min_size=15,
                spacing=5,
            )
        else:
            draw.text((24, 248), "Assistant 暂时没有留下旁注。", font=self._font(15), fill=INK)
        self._line(draw, 318)

        focus = view.now.focus
        self._label(draw, "CURRENT FOCUS", 24, 335)
        if focus is not None:
            self._text_box(draw, focus.title, (24, 359, 376, 395), max_size=23, min_size=18)
            self._text_box(draw, focus.summary, (24, 402, 376, 434), max_size=15, min_size=13)
            if focus.progress_pct is not None:
                draw.rectangle((24, 446, 376, 449), fill=GRAY_TARGET)
                progress_x = 24 + round(352 * focus.progress_pct / 100)
                draw.rectangle((24, 446, progress_x, 449), fill=ACCENT_TARGET)
        else:
            draw.text((24, 370), "当前关注不可用", font=self._font(18), fill=INK)

        result = view.now.latest_result
        if result is not None:
            draw.text((24, 478), "·", font=self._font(30), fill=ACCENT_SOLID)
            draw.text((43, 478), "刚刚完成", font=self._font(14), fill=INK)
            self._text_box(draw, result.text, (43, 504, 376, 545), max_size=15, min_size=13)
        self._footer(draw, 0)
        return self._finish(canvas)

    def _render_work(self, view: PulseView) -> bytes:
        canvas, draw = self._canvas()
        draw.text((24, 22), "工作脉搏", font=self._font(32), fill=INK)
        self._line(draw, 72)
        self._label(draw, "QUOTA REMAINING", 24, 91)
        usage = view.work.quota_usage
        if usage is not None:
            for y, label, value in (
                (121, "5h window", usage.five_hour_pct),
                (163, "Weekly", usage.weekly_pct),
            ):
                draw.text((24, y), label, font=self._font(14, latin=True), fill=INK)
                draw.text((333, y), f"{value}%", font=self._font(14, latin=True), fill=INK)
                draw.rectangle((24, y + 25, 376, y + 28), fill=GRAY_TARGET)
                draw.rectangle((24, y + 25, 24 + round(352 * value / 100), y + 28), fill=ACCENT_TARGET)
        else:
            draw.text((24, 128), "当前额度不可用", font=self._font(17), fill=INK)
        self._line(draw, 211)

        self._label(draw, "RECENT SESSIONS", 24, 229)
        y = 256
        if view.work.sessions:
            for session in view.work.sessions:
                draw.text((24, y), session.title, font=self._font(17), fill=INK)
                self._text_box(
                    draw,
                    f"{session.priority} · {session.status}",
                    (24, y + 25, 376, y + 47),
                    max_size=13,
                    min_size=11,
                )
                draw.line((24, y + 58, 376, y + 58), fill=GRAY_TARGET, width=1)
                y += 68
        else:
            draw.text((24, y), "近期主线不可用", font=self._font(17), fill=INK)

        result = view.now.latest_result
        if result is not None:
            draw.text((24, 486), "·", font=self._font(30), fill=ACCENT_SOLID)
            draw.text((43, 486), "最新成果", font=self._font(14), fill=INK)
            self._text_box(draw, result.text, (43, 512, 376, 548), max_size=14, min_size=12)
        self._footer(draw, 1)
        return self._finish(canvas)

    def _render_ideas(self, view: PulseView) -> bytes:
        canvas, draw = self._canvas()
        draw.text((24, 22), "灵感簿", font=self._font(32), fill=INK)
        self._line(draw, 72)
        y = 92
        if view.ideas.items:
            for item in view.ideas.items:
                self._text_box(draw, item.title, (24, y, 376, y + 48), max_size=18, min_size=14)
                captured = datetime.fromtimestamp(item.captured_at, self.timezone)
                date_text = captured.strftime("%m-%d %H:%M")
                date_font = self._font(12, latin=True)
                draw.text((24, y + 53), date_text, font=date_font, fill=INK)
                state_x = 24 + round(draw.textlength(date_text, font=date_font)) + 8
                draw.text((state_x, y + 53), f"· {item.state}", font=self._font(12), fill=INK)
                draw.line((24, y + 76, 376, y + 76), fill=GRAY_TARGET, width=1)
                y += 90
        else:
            draw.text((24, y), "还没有捕捉到灵感。", font=self._font(17), fill=INK)

        self._line(draw, 382)
        self._text_box(
            draw,
            "microSD 本地灵感簿",
            (24, 397, 376, 418),
            max_size=14,
            min_size=14,
            align="center",
        )
        state = {
            "ready": "READY",
            "missing": "MISSING",
            "error": "ERROR",
            None: "UNKNOWN",
        }[view.ideas.sd_state]
        self._text_box(
            draw,
            state,
            (24, 422, 376, 441),
            max_size=13,
            min_size=13,
            fill=ACCENT_SOLID,
            latin=True,
            align="center",
        )

        draw.ellipse((187, 450, 213, 476), outline=ACCENT_SOLID, width=1)
        draw.ellipse((194, 457, 206, 469), fill=ACCENT_SOLID)
        self._text_box(
            draw,
            "按住顶部 C 说出灵感",
            (78, 489, 322, 516),
            max_size=17,
            min_size=14,
            align="center",
        )
        self._text_box(
            draw,
            "松手存入 microSD · 联网后转写整理",
            (52, 522, 348, 547),
            max_size=12,
            min_size=10,
            align="center",
        )
        self._footer(draw, 2)
        return self._finish(canvas)
