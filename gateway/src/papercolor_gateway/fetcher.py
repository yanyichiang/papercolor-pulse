"""Bounded public-HTTPS image retrieval with SSRF protections."""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from io import BytesIO
import ipaddress
import socket
from typing import Any
from urllib.parse import SplitResult, urljoin, urlsplit
import warnings

import aiohttp
from PIL import Image, ImageOps, UnidentifiedImageError


MAX_REMOTE_BYTES = 5 * 1024 * 1024
MAX_REMOTE_PIXELS = 12_000_000
MAX_REDIRECTS = 3
MAX_TOTAL_SECONDS = 10.0
MAX_URL_CHARACTERS = 2_048
ALLOWED_MIME_FORMATS = {
    "image/png": "PNG",
    "image/jpeg": "JPEG",
    "image/webp": "WEBP",
}


class ImageFetchError(ValueError):
    """Raised when a remote image is unsafe, invalid, or outside limits."""


@dataclass(frozen=True, slots=True)
class HttpResponse:
    status: int
    content_type: str
    location: str | None
    body: bytes


@dataclass(frozen=True, slots=True)
class FetchedImage:
    image: Image.Image
    mime_type: str


RequestOnce = Callable[[str], Awaitable[HttpResponse]]
GetAddrInfo = Callable[..., Awaitable[list[tuple[Any, ...]]]]


def is_public_address(address: str) -> bool:
    try:
        parsed = ipaddress.ip_address(address)
    except ValueError:
        return False
    return parsed.is_global and not any(
        (
            parsed.is_link_local,
            parsed.is_loopback,
            parsed.is_multicast,
            parsed.is_private,
            parsed.is_reserved,
            parsed.is_unspecified,
        )
    )


def validate_remote_url(url: str) -> SplitResult:
    if not isinstance(url, str) or not url or len(url) > MAX_URL_CHARACTERS:
        raise ImageFetchError("image URL is empty or too long")
    if any(ord(character) < 32 for character in url):
        raise ImageFetchError("image URL contains a control character")
    try:
        parsed = urlsplit(url)
        port = parsed.port
    except ValueError as exc:
        raise ImageFetchError("image URL is malformed") from exc
    if parsed.scheme.lower() != "https":
        raise ImageFetchError("remote images must use HTTPS")
    if not parsed.hostname:
        raise ImageFetchError("remote image URL has no hostname")
    if parsed.username is not None or parsed.password is not None:
        raise ImageFetchError("remote image URL must not include credentials")
    if parsed.fragment:
        raise ImageFetchError("remote image URL must not include a fragment")
    if port not in (None, 443):
        raise ImageFetchError("remote images must use HTTPS port 443")

    hostname = parsed.hostname.rstrip(".").lower()
    if hostname == "localhost" or hostname.endswith((".localhost", ".local")):
        raise ImageFetchError("local hostnames are forbidden")
    try:
        address = ipaddress.ip_address(hostname)
    except ValueError:
        pass
    else:
        if not is_public_address(str(address)):
            raise ImageFetchError("remote image address is not public")
    return parsed


async def resolve_public_addresses(
    hostname: str,
    port: int = 443,
    *,
    getaddrinfo: GetAddrInfo | None = None,
    timeout: float = 2.0,
) -> tuple[str, ...]:
    """Resolve a host and reject the entire answer set if any IP is non-public."""
    try:
        literal = ipaddress.ip_address(hostname.rstrip("."))
    except ValueError:
        literal = None
    if literal is not None:
        if not is_public_address(str(literal)):
            raise ImageFetchError("remote image address is not public")
        return (str(literal),)

    lookup = getaddrinfo or asyncio.get_running_loop().getaddrinfo
    try:
        answers = await asyncio.wait_for(
            lookup(
                hostname,
                port,
                family=socket.AF_UNSPEC,
                type=socket.SOCK_STREAM,
                proto=socket.IPPROTO_TCP,
            ),
            timeout=timeout,
        )
    except (TimeoutError, OSError, socket.gaierror) as exc:
        raise ImageFetchError("remote image hostname could not be resolved") from exc

    addresses = tuple(sorted({str(answer[4][0]) for answer in answers}))
    if not addresses:
        raise ImageFetchError("remote image hostname returned no addresses")
    if any(not is_public_address(address) for address in addresses):
        raise ImageFetchError("DNS response contains a non-public address")
    return addresses


class _PinnedResolver(aiohttp.abc.AbstractResolver):
    def __init__(self, hostname: str, addresses: tuple[str, ...]):
        self.hostname = hostname.rstrip(".").lower()
        self.addresses = addresses

    async def resolve(
        self, host: str, port: int = 0, family: socket.AddressFamily = socket.AF_INET
    ) -> list[dict[str, Any]]:
        if host.rstrip(".").lower() != self.hostname:
            raise OSError("unexpected hostname for pinned resolver")
        return [
            {
                "hostname": host,
                "host": address,
                "port": port,
                "family": socket.AF_INET6 if ":" in address else socket.AF_INET,
                "proto": socket.IPPROTO_TCP,
                "flags": 0,
            }
            for address in self.addresses
        ]

    async def close(self) -> None:
        return None


def decode_image(
    payload: bytes,
    mime_type: str,
    *,
    max_pixels: int = MAX_REMOTE_PIXELS,
) -> Image.Image:
    if not payload:
        raise ImageFetchError("remote image is empty")
    expected_format = ALLOWED_MIME_FORMATS.get(mime_type)
    if expected_format is None:
        raise ImageFetchError("remote image MIME type is not accepted")
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            image = Image.open(BytesIO(payload))
            if image.format != expected_format:
                raise ImageFetchError(
                    "remote image MIME type does not match its format"
                )
            width, height = image.size
            if width <= 0 or height <= 0 or width * height > max_pixels:
                raise ImageFetchError("remote image exceeds the pixel limit")
            if (
                getattr(image, "is_animated", False)
                and getattr(image, "n_frames", 1) > 1
            ):
                raise ImageFetchError("animated remote images are not accepted")
            image.load()
    except ImageFetchError:
        raise
    except (
        UnidentifiedImageError,
        OSError,
        ValueError,
        Image.DecompressionBombError,
    ) as exc:
        raise ImageFetchError("remote image could not be decoded safely") from exc
    return ImageOps.exif_transpose(image).convert("RGB")


class ImageFetcher:
    def __init__(
        self,
        *,
        max_bytes: int = MAX_REMOTE_BYTES,
        max_pixels: int = MAX_REMOTE_PIXELS,
        total_timeout: float = MAX_TOTAL_SECONDS,
        request_once: RequestOnce | None = None,
    ):
        if max_bytes <= 0 or max_pixels <= 0 or total_timeout <= 0:
            raise ValueError("fetch limits must be positive")
        self.max_bytes = max_bytes
        self.max_pixels = max_pixels
        self.total_timeout = total_timeout
        self._request_once_override = request_once

    async def fetch(self, url: str) -> FetchedImage:
        current_url = url
        request_once = self._request_once_override or self._request_once
        try:
            async with asyncio.timeout(self.total_timeout):
                for redirect_count in range(MAX_REDIRECTS + 1):
                    validate_remote_url(current_url)
                    response = await request_once(current_url)
                    if response.status in {301, 302, 303, 307, 308}:
                        if not response.location:
                            raise ImageFetchError(
                                "remote image redirect has no location"
                            )
                        if redirect_count == MAX_REDIRECTS:
                            raise ImageFetchError(
                                "remote image exceeded the redirect limit"
                            )
                        current_url = urljoin(current_url, response.location)
                        continue
                    if response.status != 200:
                        raise ImageFetchError(
                            f"remote image returned HTTP status {response.status}"
                        )
                    if len(response.body) > self.max_bytes:
                        raise ImageFetchError("remote image exceeds the byte limit")
                    mime_type = response.content_type.split(";", 1)[0].strip().lower()
                    image = decode_image(
                        response.body, mime_type, max_pixels=self.max_pixels
                    )
                    return FetchedImage(image=image, mime_type=mime_type)
        except TimeoutError as exc:
            raise ImageFetchError("remote image fetch timed out") from exc
        except aiohttp.ClientError as exc:
            raise ImageFetchError("remote image fetch failed") from exc
        raise ImageFetchError("remote image fetch did not complete")

    async def _request_once(self, url: str) -> HttpResponse:
        parsed = validate_remote_url(url)
        hostname = parsed.hostname
        assert hostname is not None
        addresses = await resolve_public_addresses(hostname, 443)
        resolver = _PinnedResolver(hostname, addresses)
        connector = aiohttp.TCPConnector(
            resolver=resolver,
            use_dns_cache=False,
            limit=1,
            ssl=True,
        )
        timeout = aiohttp.ClientTimeout(
            total=self.total_timeout,
            connect=min(3.0, self.total_timeout),
            sock_read=min(5.0, self.total_timeout),
        )
        headers = {"Accept": ", ".join(ALLOWED_MIME_FORMATS)}
        async with aiohttp.ClientSession(
            connector=connector, timeout=timeout
        ) as session:
            async with session.get(
                url, allow_redirects=False, headers=headers
            ) as response:
                location = response.headers.get("Location")
                content_type = response.headers.get("Content-Type", "")
                if response.status in {301, 302, 303, 307, 308}:
                    return HttpResponse(response.status, content_type, location, b"")
                if response.status != 200:
                    return HttpResponse(response.status, content_type, None, b"")

                content_length = response.headers.get("Content-Length")
                if content_length is not None:
                    try:
                        declared_size = int(content_length)
                    except ValueError as exc:
                        raise ImageFetchError(
                            "invalid remote image Content-Length"
                        ) from exc
                    if declared_size < 0 or declared_size > self.max_bytes:
                        raise ImageFetchError("remote image exceeds the byte limit")

                chunks: list[bytes] = []
                size = 0
                async for chunk in response.content.iter_chunked(64 * 1024):
                    size += len(chunk)
                    if size > self.max_bytes:
                        raise ImageFetchError("remote image exceeds the byte limit")
                    chunks.append(chunk)
                return HttpResponse(200, content_type, None, b"".join(chunks))
