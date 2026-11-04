"""Minimal logging helpers (GitHub Actions friendly)."""

from __future__ import annotations

import os
import sys
import time

_T0 = time.time()
_GHA = os.environ.get("GITHUB_ACTIONS") == "true"


class PortError(Exception):
    """A user-facing failure; printed without a traceback."""


def _stamp() -> str:
    t = int(time.time() - _T0)
    return f"[{t // 60:02d}:{t % 60:02d}]"


def log(msg: str) -> None:
    print(f"{_stamp()} {msg}", flush=True)


def step(msg: str) -> None:
    print(f"\n{_stamp()} ==> {msg}", flush=True)


def warn(msg: str) -> None:
    if _GHA:
        print(f"::warning::{msg}", flush=True)
    else:
        print(f"{_stamp()} WARNING: {msg}", file=sys.stderr, flush=True)
