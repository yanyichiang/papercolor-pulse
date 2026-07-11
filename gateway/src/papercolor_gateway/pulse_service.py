"""Pulse source composition and atomic bundle publication."""

from __future__ import annotations

from datetime import UTC, datetime
from typing import Protocol

from .pulse_models import AssistantUpdate, PulseView, SourceUpdate
from .pulse_renderer import PulseBundle
from .store import JobStore, PulseBundleRecord


class PulseRenderError(RuntimeError):
    """Raised when a new semantic snapshot cannot produce a complete bundle."""


class PulseRendererProtocol(Protocol):
    def render(self, view: PulseView) -> PulseBundle: ...


def _iso(epoch: int) -> str:
    return datetime.fromtimestamp(epoch, UTC).isoformat()


def _fresh(payload: dict[str, object] | None, now: int, max_age: int) -> bool:
    if payload is None:
        return False
    observed_at = payload.get("observed_at")
    return (
        isinstance(observed_at, int)
        and observed_at <= now + 300
        and now - observed_at <= max_age
    )


class PulseService:
    """Merge isolated source namespaces and publish complete page generations."""

    def __init__(self, store: JobStore, renderer: PulseRendererProtocol) -> None:
        self.store = store
        self.renderer = renderer

    def update_source(
        self, update: SourceUpdate, *, now: int
    ) -> PulseBundleRecord:
        self.store.upsert_pulse_source(
            update.source, update.observed_at, update.payload, now=now
        )
        return self._render_and_activate(now=now)

    def update_assistant(
        self, update: AssistantUpdate, *, now: int
    ) -> PulseBundleRecord:
        payload: dict[str, object] = {
            "observed_at": now,
            "assistant_note": update.assistant_note,
        }
        if update.focus_summary is not None:
            payload["focus_summary"] = update.focus_summary
        self.store.upsert_pulse_source("assistant", now, payload, now=now)
        return self._render_and_activate(now=now)

    def compose_view(self, *, now: int) -> PulseView:
        sources = self.store.get_pulse_sources()
        now_page: dict[str, object] = {}
        work_page: dict[str, object] = {}
        ideas_page: dict[str, object] = {}

        device = sources.get("device")
        if _fresh(device, now, 2 * 60 * 60):
            assert device is not None
            observed_at = int(device["observed_at"])
            if "temperature_c" in device:
                now_page["temperature_c"] = {
                    "value": device["temperature_c"],
                    "observed_at": _iso(observed_at),
                }
            if "humidity_pct" in device:
                now_page["humidity_pct"] = {
                    "value": device["humidity_pct"],
                    "observed_at": _iso(observed_at),
                }
            if "sd_state" in device:
                ideas_page["sd_state"] = device["sd_state"]

        ha = sources.get("ha")
        if _fresh(ha, now, 6 * 60 * 60):
            assert ha is not None
            weather = ha.get("weather")
            if isinstance(weather, dict):
                normalized_weather = dict(weather)
                normalized_weather["observed_at"] = _iso(int(ha["observed_at"]))
                now_page["weather"] = normalized_weather

        mac = sources.get("mac")
        if _fresh(mac, now, 7 * 24 * 60 * 60):
            assert mac is not None
            observed_iso = _iso(int(mac["observed_at"]))
            if isinstance(mac.get("focus"), dict):
                focus = dict(mac["focus"])
                focus["observed_at"] = observed_iso
                now_page["focus"] = focus
            if isinstance(mac.get("latest_result"), dict):
                latest = dict(mac["latest_result"])
                latest.setdefault("verified_at", observed_iso)
                now_page["latest_result"] = latest
            if isinstance(mac.get("quota_usage"), dict):
                usage = dict(mac["quota_usage"])
                usage.setdefault("observed_at", observed_iso)
                work_page["quota_usage"] = usage
            if isinstance(mac.get("sessions"), list):
                work_page["sessions"] = mac["sessions"]
            if isinstance(mac.get("ideas"), list):
                ideas_page["items"] = mac["ideas"]

        voice_items = self.store.voice_idea_items(limit=3)
        if voice_items:
            existing = ideas_page.get("items", [])
            ideas_page["items"] = (voice_items + list(existing))[:3]

        assistant = sources.get("assistant")
        if _fresh(assistant, now, 7 * 24 * 60 * 60):
            assert assistant is not None
            note = assistant.get("assistant_note")
            if isinstance(note, str):
                now_page["assistant_note"] = note
            summary = assistant.get("focus_summary")
            focus = now_page.get("focus")
            if isinstance(summary, str) and isinstance(focus, dict):
                focus["summary"] = summary

        return PulseView.from_mapping(
            {
                "schema_version": 1,
                "generated_at": _iso(now),
                "now": now_page,
                "work": work_page,
                "ideas": ideas_page,
            }
        )

    def _render_and_activate(self, *, now: int) -> PulseBundleRecord:
        view = self.compose_view(now=now)
        try:
            bundle = self.renderer.render(view)
        except Exception as exc:
            raise PulseRenderError(str(exc)) from exc
        return self.store.activate_pulse_bundle(bundle, now=now)
