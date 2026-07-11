"""Environment-backed gateway configuration."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
import os
from pathlib import Path
import re


_DEVICE_ID_PATTERN = re.compile(r"^[A-Za-z0-9._-]{1,128}$")


class ConfigurationError(ValueError):
    """Raised when required service configuration is unsafe or incomplete."""


@dataclass(frozen=True, slots=True)
class Settings:
    data_directory: Path
    font_path: Path
    device_id: str
    device_token: str
    admin_token: str
    cjk_serif_font_path: Path | None = None
    latin_serif_font_path: Path | None = None
    timezone_name: str = "UTC"
    host: str = "0.0.0.0"
    port: int = 8767

    def __post_init__(self) -> None:
        object.__setattr__(self, "data_directory", Path(self.data_directory))
        object.__setattr__(self, "font_path", Path(self.font_path))
        object.__setattr__(
            self,
            "cjk_serif_font_path",
            Path(self.cjk_serif_font_path or self.font_path),
        )
        object.__setattr__(
            self,
            "latin_serif_font_path",
            Path(self.latin_serif_font_path or self.font_path),
        )
        if not _DEVICE_ID_PATTERN.fullmatch(self.device_id):
            raise ConfigurationError(
                "device_id must use 1-128 letters, digits, dots, underscores, or hyphens"
            )
        for field, token in (
            ("device_token", self.device_token),
            ("admin_token", self.admin_token),
        ):
            if not isinstance(token, str) or not 32 <= len(token) <= 512:
                raise ConfigurationError(f"{field} must contain 32-512 characters")
            if any(character.isspace() for character in token):
                raise ConfigurationError(f"{field} must not contain whitespace")
        if self.device_token == self.admin_token:
            raise ConfigurationError("device_token and admin_token must be distinct")
        if not self.font_path.is_file():
            raise ConfigurationError(
                f"configured font does not exist: {self.font_path}"
            )
        for field, path in (
            ("cjk_serif_font_path", self.cjk_serif_font_path),
            ("latin_serif_font_path", self.latin_serif_font_path),
        ):
            if path is None or not path.is_file():
                raise ConfigurationError(f"configured {field} does not exist: {path}")
        if not self.timezone_name:
            raise ConfigurationError("timezone_name must not be empty")
        if not self.host:
            raise ConfigurationError("host must not be empty")
        if not 1 <= self.port <= 65_535:
            raise ConfigurationError("port must be between 1 and 65535")

    @property
    def database_path(self) -> Path:
        return self.data_directory / "gateway.sqlite3"

    @property
    def asset_directory(self) -> Path:
        return self.data_directory / "assets"

    @classmethod
    def from_env(cls, environment: Mapping[str, str] | None = None) -> Settings:
        env = os.environ if environment is None else environment

        def required(name: str) -> str:
            value = env.get(name)
            if not value:
                raise ConfigurationError(f"{name} is required")
            return value

        raw_port = env.get("PAPERCOLOR_PORT", "8767")
        try:
            port = int(raw_port)
        except ValueError as exc:
            raise ConfigurationError("PAPERCOLOR_PORT must be an integer") from exc
        return cls(
            data_directory=Path(
                env.get("PAPERCOLOR_DATA_DIR", "/var/lib/papercolor-gateway")
            ),
            font_path=Path(required("PAPERCOLOR_FONT_PATH")),
            device_id=required("PAPERCOLOR_DEVICE_ID"),
            device_token=required("PAPERCOLOR_DEVICE_TOKEN"),
            admin_token=required("PAPERCOLOR_ADMIN_TOKEN"),
            cjk_serif_font_path=Path(required("PAPERCOLOR_CJK_SERIF_FONT_PATH")),
            latin_serif_font_path=Path(required("PAPERCOLOR_LATIN_SERIF_FONT_PATH")),
            timezone_name=env.get("PAPERCOLOR_TIMEZONE", "UTC"),
            host=env.get("PAPERCOLOR_HOST", "0.0.0.0"),
            port=port,
        )
