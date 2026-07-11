import asyncio
from io import BytesIO
import socket

from PIL import Image
import pytest

from papercolor_gateway.fetcher import (
    HttpResponse,
    ImageFetchError,
    ImageFetcher,
    decode_image,
    is_public_address,
    resolve_public_addresses,
    validate_remote_url,
)


@pytest.mark.parametrize(
    "address",
    [
        "127.0.0.1",
        "10.0.0.1",
        "100.64.0.1",
        "169.254.169.254",
        "192.168.1.1",
        "224.0.0.1",
        "0.0.0.0",
        "::1",
        "fc00::1",
        "fe80::1",
    ],
)
def test_non_public_addresses_are_rejected(address: str) -> None:
    assert not is_public_address(address)


def test_public_addresses_are_accepted() -> None:
    assert is_public_address("93.184.216.34")
    assert is_public_address("2606:2800:220:1:248:1893:25c8:1946")


@pytest.mark.parametrize(
    "url",
    [
        "http://example.com/image.png",
        "https://127.0.0.1/image.png",
        "https://224.0.0.1/image.png",
        "https://[::1]/image.png",
        "https://user:pass@example.com/image.png",
        "https://example.com:8443/image.png",
        "https://localhost/image.png",
        "https://example.com/image.png#fragment",
    ],
)
def test_remote_url_validation_rejects_unsafe_targets(url: str) -> None:
    with pytest.raises(ImageFetchError):
        validate_remote_url(url)


def test_dns_resolution_rejects_mixed_public_and_private_answers() -> None:
    async def fake_getaddrinfo(*args, **kwargs):
        return [
            (socket.AF_INET, socket.SOCK_STREAM, 6, "", ("93.184.216.34", 443)),
            (socket.AF_INET, socket.SOCK_STREAM, 6, "", ("10.0.0.4", 443)),
        ]

    with pytest.raises(ImageFetchError, match="non-public"):
        asyncio.run(
            resolve_public_addresses("example.com", 443, getaddrinfo=fake_getaddrinfo)
        )


def test_literal_multicast_address_is_rejected_during_resolution() -> None:
    with pytest.raises(ImageFetchError, match="not public"):
        asyncio.run(resolve_public_addresses("224.0.0.1"))


def test_redirect_target_is_revalidated_before_second_request() -> None:
    calls: list[str] = []

    async def fake_request(url: str) -> HttpResponse:
        calls.append(url)
        return HttpResponse(
            status=302,
            content_type="text/plain",
            location="https://127.0.0.1/private.png",
            body=b"",
        )

    with pytest.raises(ImageFetchError):
        asyncio.run(
            ImageFetcher(request_once=fake_request).fetch("https://example.com/a")
        )
    assert calls == ["https://example.com/a"]


def _png_bytes(size: tuple[int, int] = (20, 20)) -> bytes:
    output = BytesIO()
    Image.new("RGB", size, "red").save(output, format="PNG")
    return output.getvalue()


def test_fetch_rejects_response_larger_than_byte_limit() -> None:
    async def fake_request(url: str) -> HttpResponse:
        return HttpResponse(200, "image/png", None, b"x" * 101)

    with pytest.raises(ImageFetchError, match="byte limit"):
        asyncio.run(
            ImageFetcher(max_bytes=100, request_once=fake_request).fetch(
                "https://example.com/a.png"
            )
        )


def test_fetch_enforces_one_total_timeout() -> None:
    async def slow_request(url: str) -> HttpResponse:
        await asyncio.sleep(0.05)
        return HttpResponse(200, "image/png", None, _png_bytes())

    with pytest.raises(ImageFetchError, match="timed out"):
        asyncio.run(
            ImageFetcher(total_timeout=0.01, request_once=slow_request).fetch(
                "https://example.com/a.png"
            )
        )


def test_decode_rejects_mime_format_mismatch_and_pixel_limit() -> None:
    payload = _png_bytes((20, 20))
    with pytest.raises(ImageFetchError, match="does not match"):
        decode_image(payload, "image/jpeg")
    with pytest.raises(ImageFetchError, match="pixel limit"):
        decode_image(payload, "image/png", max_pixels=399)


def test_fetch_returns_loaded_rgb_image_for_valid_png() -> None:
    payload = _png_bytes()

    async def fake_request(url: str) -> HttpResponse:
        return HttpResponse(200, "image/png; charset=binary", None, payload)

    fetched = asyncio.run(
        ImageFetcher(request_once=fake_request).fetch("https://example.com/a.png")
    )
    assert fetched.mime_type == "image/png"
    assert fetched.image.mode == "RGB"
    assert fetched.image.size == (20, 20)
