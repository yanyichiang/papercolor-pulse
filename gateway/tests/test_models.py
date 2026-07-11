from datetime import UTC, datetime

import pytest

from papercolor_gateway.models import CardRequest, ValidationError
from papercolor_gateway.schedule import is_exact_delivery_guaranteed, next_regular_sync


NOW = int(datetime(2026, 7, 10, 4, 0, tzinfo=UTC).timestamp())


def test_card_request_normalizes_valid_input() -> None:
    request = CardRequest.from_mapping(
        {
            "title": "  午后提醒  ",
            "body": "第一行\r\n第二行",
            "style": "brief",
            "image_url": "https://images.example.com/card.png",
            "display_at": "2026-07-10T12:30:00+08:00",
            "expires_at": "2026-07-10T13:30:00+08:00",
        },
        now=NOW,
    )

    assert request.title == "午后提醒"
    assert request.body == "第一行\n第二行"
    assert request.display_at == NOW + 30 * 60
    assert request.expires_at == NOW + 90 * 60


@pytest.mark.parametrize(
    ("overrides", "message"),
    [
        ({"title": ""}, "title"),
        ({"title": "题" * 81}, "80"),
        ({"body": "文" * 1001}, "1,000"),
        ({"body": "bad\x00body"}, "control"),
        ({"style": "loud"}, "style"),
        ({"image_url": "http://example.com/a.png"}, "HTTPS"),
        ({"display_at": "2026-07-10T12:30:00"}, "timezone"),
        ({"display_at": "2026-07-10T11:59:59+08:00"}, "future"),
        (
            {
                "display_at": "2026-07-10T12:30:00+08:00",
                "expires_at": "2026-07-10T12:30:00+08:00",
            },
            "after",
        ),
        ({"extra": True}, "Unknown"),
    ],
)
def test_card_request_rejects_invalid_input(
    overrides: dict[str, object], message: str
) -> None:
    payload: dict[str, object] = {"title": "Title", "body": "Body"}
    payload.update(overrides)

    with pytest.raises(ValidationError, match=message):
        CardRequest.from_mapping(payload, now=NOW)


def test_guarantee_threshold_is_exactly_twenty_minutes() -> None:
    assert not is_exact_delivery_guaranteed(None, NOW)
    assert not is_exact_delivery_guaranteed(NOW + 1199, NOW)
    assert is_exact_delivery_guaranteed(NOW + 1200, NOW)


def test_next_regular_sync_uses_next_epoch_aligned_interval() -> None:
    assert next_regular_sync(899, 900) == 900
    assert next_regular_sync(900, 900) == 1800
