"""Console entry point for papercolor-gateway."""

from __future__ import annotations

import logging
import os

from aiohttp import web

from .app import create_app
from .config import ConfigurationError, Settings


def main() -> None:
    level_name = os.environ.get("PAPERCOLOR_LOG_LEVEL", "INFO").upper()
    level = getattr(logging, level_name, logging.INFO)
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        settings = Settings.from_env()
    except ConfigurationError as exc:
        raise SystemExit(f"configuration error: {exc}") from exc
    web.run_app(
        create_app(settings),
        host=settings.host,
        port=settings.port,
        print=None,
    )


if __name__ == "__main__":
    main()
