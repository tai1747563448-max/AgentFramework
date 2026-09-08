from __future__ import annotations

from dataclasses import dataclass
import json
import math
import re
import time
from typing import Callable, TextIO


_IDENTIFIER = re.compile(r"[a-z][a-z0-9._-]{0,63}\Z")


class ProgressError(RuntimeError):
    pass


@dataclass
class _State:
    started: float
    last_emitted: float | None
    completed: int
    total: int


class ProgressReporter:
    def __init__(
        self,
        stream: TextIO,
        *,
        clock: Callable[[], float] = time.monotonic,
        minimum_interval_seconds: float = 5.0,
    ) -> None:
        if (
            not callable(clock)
            or type(minimum_interval_seconds) not in {int, float}
            or not math.isfinite(float(minimum_interval_seconds))
            or float(minimum_interval_seconds) <= 0.0
            or not callable(getattr(stream, "write", None))
            or not callable(getattr(stream, "flush", None))
        ):
            raise ProgressError("progress reporter configuration is invalid")
        self._stream = stream
        self._clock = clock
        self._minimum_interval = float(minimum_interval_seconds)
        self._states: dict[tuple[str, str | None], _State] = {}

    @staticmethod
    def _validate_identifier(phase: str, mode: str | None) -> None:
        if (
            type(phase) is not str
            or _IDENTIFIER.fullmatch(phase) is None
            or (
                mode is not None
                and (type(mode) is not str or _IDENTIFIER.fullmatch(mode) is None)
            )
        ):
            raise ProgressError("progress event is invalid")

    def _emit(
        self,
        phase: str,
        mode: str | None,
        state: _State,
        now: float,
        *,
        status: str,
    ) -> None:
        elapsed = max(0.0, now - state.started)
        throughput = (
            state.completed / elapsed
            if state.completed and elapsed > 0.0
            else 0.0
        )
        eta = (
            (state.total - state.completed) / throughput
            if throughput > 0.0
            and status == "running"
            and state.completed < state.total
            else None
        )
        event = {
            "schema_version": 1,
            "type": "progress",
            "phase": phase,
            "mode": mode,
            "status": status,
            "completed": state.completed,
            "total": state.total,
            "percent": round(state.completed * 100.0 / state.total, 3),
            "elapsed_seconds": round(elapsed, 3),
            "throughput_items_per_second": round(throughput, 3),
            "eta_seconds": None if eta is None else round(eta, 3),
        }
        try:
            self._stream.write(
                json.dumps(
                    event,
                    ensure_ascii=True,
                    sort_keys=True,
                    separators=(",", ":"),
                    allow_nan=False,
                )
                + "\n"
            )
            self._stream.flush()
        except (OSError, TypeError, ValueError) as error:
            raise ProgressError("progress output failed") from error
        state.last_emitted = now

    def update(
        self,
        phase: str,
        completed: int,
        total: int,
        *,
        mode: str | None = None,
    ) -> None:
        self._validate_identifier(phase, mode)
        if (
            type(completed) is not int
            or type(total) is not int
            or total <= 0
            or not 0 <= completed <= total
        ):
            raise ProgressError("progress event is invalid")
        now = float(self._clock())
        if not math.isfinite(now):
            raise ProgressError("progress event is invalid")
        key = (phase, mode)
        state = self._states.get(key)
        if state is None:
            state = _State(now, None, completed, total)
            self._states[key] = state
        elif total != state.total or completed < state.completed:
            raise ProgressError("progress event is invalid")
        state.completed = completed
        should_emit = (
            state.last_emitted is None
            or completed == total
            or now - state.last_emitted >= self._minimum_interval
        )
        if not should_emit:
            return
        self._emit(
            phase,
            mode,
            state,
            now,
            status="completed" if completed == total else "running",
        )

    def fail(self, phase: str, *, mode: str | None = None) -> None:
        self._validate_identifier(phase, mode)
        state = self._states.get((phase, mode))
        if state is None:
            raise ProgressError("progress event is invalid")
        now = float(self._clock())
        if not math.isfinite(now):
            raise ProgressError("progress event is invalid")
        self._emit(phase, mode, state, now, status="failed")
