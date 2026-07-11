#!/usr/bin/env python3
"""Push a local image to a PaperColor running this firmware."""

from __future__ import annotations

import argparse
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path


def detect_content_type(data: bytes) -> str:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if data.startswith(b"BM"):
        return "image/bmp"
    raise ValueError("unsupported image type; use PNG, JPEG, or BMP")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="PNG/JPEG/BMP image to display")
    parser.add_argument(
        "--url",
        default=os.environ.get("PAPERCOLOR_PUSH_URL", "http://papercolor.local/api/push/display"),
        help="Push endpoint URL",
    )
    parser.add_argument(
        "--token",
        default=os.environ.get("PAPERCOLOR_API_TOKEN", ""),
        help="Bearer token; defaults to PAPERCOLOR_API_TOKEN",
    )
    args = parser.parse_args()

    if not args.token:
        print("error: set PAPERCOLOR_API_TOKEN or pass --token", file=sys.stderr)
        return 2

    data = args.image.read_bytes()
    content_type = detect_content_type(data)
    req = urllib.request.Request(
        args.url,
        data=data,
        method="POST",
        headers={
            "Authorization": f"Bearer {args.token}",
            "Content-Type": content_type,
        },
    )

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            print(resp.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as exc:
        print(exc.read().decode("utf-8", errors="replace"), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
