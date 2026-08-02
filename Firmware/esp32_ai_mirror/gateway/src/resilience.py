"""Small, dependency-free reliability primitives used by the gateway.

The gateway talks to providers and a constrained device over several different
transports.  Keeping retry policy in one module makes the failure semantics
explicit and keeps callers from implementing unbounded retry loops.
"""

from __future__ import annotations

import random
import re
import time
from dataclasses import dataclass
from typing import Callable, TypeVar


T = TypeVar("T")


@dataclass(frozen=True)
class RetryPolicy:
    """Bounded exponential backoff policy.

    ``max_attempts`` includes the first call.  A value of one therefore means
    no retry.  Jitter is expressed as a fraction of the calculated delay and
    can be set to zero in deterministic tests.
    """

    max_attempts: int = 2
    base_delay_seconds: float = 0.25
    max_delay_seconds: float = 2.0
    jitter_ratio: float = 0.1

    def __post_init__(self) -> None:
        if self.max_attempts < 1:
            raise ValueError("max_attempts must be at least one")
        if self.base_delay_seconds < 0 or self.max_delay_seconds < 0:
            raise ValueError("retry delays must not be negative")
        if self.jitter_ratio < 0 or self.jitter_ratio > 1:
            raise ValueError("jitter_ratio must be between zero and one")

    def delay_for_retry(self, attempt: int) -> float:
        """Return the delay before the next attempt after ``attempt`` failed."""

        if attempt < 1:
            raise ValueError("attempt must be one-based")
        delay = min(
            self.max_delay_seconds,
            self.base_delay_seconds * (2 ** (attempt - 1)),
        )
        if self.jitter_ratio and delay:
            delay += random.uniform(0, delay * self.jitter_ratio)
        return delay


def run_with_retry(
    operation: Callable[[], T],
    policy: RetryPolicy,
    *,
    retryable: Callable[[Exception], bool] | None = None,
    sleep: Callable[[float], None] = time.sleep,
    on_retry: Callable[[Exception, int, float], None] | None = None,
) -> T:
    """Run ``operation`` with a bounded retry loop.

    The operation is deliberately supplied as a closure.  Callers can use it
    to reset temporary output files or rebuild a request before each attempt.
    The last exception is re-raised unchanged so existing error handling keeps
    the original provider/device failure detail.
    """

    retryable = retryable or (lambda _error: True)
    for attempt in range(1, policy.max_attempts + 1):
        try:
            return operation()
        except Exception as error:
            if attempt >= policy.max_attempts or not retryable(error):
                raise
            delay = policy.delay_for_retry(attempt)
            if on_retry is not None:
                on_retry(error, attempt + 1, delay)
            if delay:
                sleep(delay)
    raise AssertionError("retry loop did not return or raise")


_TRANSIENT_ERROR_RE = re.compile(
    r"(?:\b408\b|\b425\b|\b429\b|\b5\d\d\b|timeout|timed out|network|connection|reset|temporar)",
    re.IGNORECASE,
)


def is_transient_error(error: Exception) -> bool:
    """Classify errors safe for a bounded provider retry.

    Validation errors and ordinary 4xx responses are intentionally not
    retried.  Provider wrappers convert transport failures to RuntimeError,
    so the message check is needed in addition to built-in network types.
    """

    if isinstance(error, (FileNotFoundError, PermissionError)):
        return False
    if isinstance(error, (TimeoutError, ConnectionError, OSError)):
        return True
    return bool(_TRANSIENT_ERROR_RE.search(str(error)))
