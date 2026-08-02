"""Bounded filesystem maintenance for gateway artifacts."""

from __future__ import annotations

import time
from pathlib import Path
from typing import Iterable


def cleanup_tree(
    root: Path,
    *,
    max_age_seconds: float,
    protected_names: Iterable[str] = (),
    protected_prefixes: Iterable[str] = (),
    allowed_suffixes: Iterable[str] | None = None,
    now: float | None = None,
) -> dict[str, int]:
    """Delete expired files below a known artifact directory.

    Only regular files directly beneath ``root`` or its child directories are
    considered.  Symlinks and protected names are skipped.  The caller owns
    the root path, so this helper never accepts a broad filesystem target and
    never removes the root directory itself.
    """

    root = root.resolve()
    protected = {str(name) for name in protected_names}
    prefixes = tuple(str(prefix) for prefix in protected_prefixes)
    suffixes = None if allowed_suffixes is None else {str(item) for item in allowed_suffixes}
    current = time.time() if now is None else now
    result = {"scanned_files": 0, "deleted_files": 0, "deleted_bytes": 0, "errors": 0}
    if not root.is_dir():
        return result

    for path in root.rglob("*"):
        try:
            if not path.is_file() or path.is_symlink():
                continue
            if suffixes is not None and path.suffix not in suffixes:
                continue
            result["scanned_files"] += 1
            relative_name = path.relative_to(root).as_posix()
            if (
                path.name in protected
                or relative_name in protected
                or any(path.name.startswith(prefix) for prefix in prefixes)
            ):
                continue
            age = current - path.stat().st_mtime
            if age < max_age_seconds:
                continue
            size = path.stat().st_size
            path.unlink()
            result["deleted_files"] += 1
            result["deleted_bytes"] += size
        except (OSError, ValueError):
            result["errors"] += 1

    # Remove empty artifact subdirectories, but never remove the configured
    # root.  This is best-effort and does not affect the deletion counters.
    for directory in sorted(root.rglob("*"), key=lambda item: len(item.parts), reverse=True):
        if directory == root or not directory.is_dir() or directory.is_symlink():
            continue
        try:
            directory.rmdir()
        except OSError:
            pass
    return result
