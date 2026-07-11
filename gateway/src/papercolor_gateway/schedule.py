"""UTC scheduling helpers shared by the API and store."""

from __future__ import annotations


GUARANTEE_WINDOW_SECONDS = 20 * 60
DEFAULT_POLL_INTERVAL_SECONDS = 15 * 60


def is_exact_delivery_guaranteed(display_at: int | None, now: int) -> bool:
    """Return whether a scheduled job meets the documented cache window."""
    return display_at is not None and display_at - now >= GUARANTEE_WINDOW_SECONDS


def next_regular_sync(
    now: int, poll_interval: int = DEFAULT_POLL_INTERVAL_SECONDS
) -> int:
    """Return the next UTC epoch-aligned regular synchronization time."""
    if poll_interval <= 0:
        raise ValueError("poll_interval must be positive")
    return ((now // poll_interval) + 1) * poll_interval
