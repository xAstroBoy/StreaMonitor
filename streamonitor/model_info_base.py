"""
model_info_base  –  Shared utilities for site-specific JSON model parsers.

Every site plugin defines a frozen dataclass (e.g. SCModelInfo, CBModelInfo)
with typed fields and factory methods.  This module provides two helpers that
**every** ModelInfo should call:

  1. check_unknown_fields()  — Detects new / unexpected keys in API responses
     and logs them once per session.  Helps catch API changes early.

  2. check_unknown_status()  — Detects status string values not in a known set
     and logs a WARNING (these usually need a code change to handle).

Both functions de-duplicate so the same unknown field / status is only logged
once per process lifetime, preventing log spam.
"""

import logging
from typing import Dict, FrozenSet, List, Optional, Set
import parameters

_logger = logging.getLogger("modelinfo")

# ── Module-level dedup sets (one report per process per unknown) ──────────
_reported_keys: Set[str] = set()
_reported_statuses: Set[str] = set()


def check_unknown_fields(
    raw: dict,
    expected: Dict[str, FrozenSet[str]],
    site: str,
    username: str = "",
    logger: Optional[logging.Logger] = None,
) -> List[str]:
    """Walk *raw* JSON and compare keys at each monitored nesting level.

    Parameters
    ----------
    raw : dict
        The raw API response.
    expected : dict[str, frozenset[str]]
        Maps dot-separated path prefixes to frozensets of known key names.
        ``""`` = top-level, ``"cam"`` = ``raw["cam"]``, etc.
        Levels **not** present in this dict are silently skipped.
    site : str
        Short site slug for log messages (e.g. ``"SC"``, ``"CB"``).
    username : str
        Model username for context in log messages.
    logger : Logger, optional
        If given, log here; otherwise fall back to the module logger.

    Returns
    -------
    list[str]
        Dot-separated paths of all unknown fields found (including
        ones that were already reported and suppressed).
    """
    if not isinstance(raw, dict):
        return []

    log = logger or _logger
    unknown: List[str] = []

    def _walk(obj: dict, prefix: str) -> None:
        if not isinstance(obj, dict):
            return
        known = expected.get(prefix)
        if known is None:
            return  # not monitoring this level

        for key in obj:
            if key not in known:
                full_path = f"{prefix}.{key}" if prefix else key
                unknown.append(full_path)
                dedup_key = f"{site}:{full_path}"
                if dedup_key not in _reported_keys:
                    _reported_keys.add(dedup_key)
                    # Only log new API fields in DEBUG mode to prevent spam
                    if parameters.DEBUG:
                        val = obj[key]
                        vtype = type(val).__name__
                        vpreview = repr(val)
                        if len(vpreview) > 120:
                            vpreview = vpreview[:117] + "..."
                        _logger.debug(
                            f"[{site}] New API field '{full_path}' ({vtype}): {vpreview} — model={username}"
                        )

            # Recurse into sub-dicts that we have expectations for
            child = f"{prefix}.{key}" if prefix else key
            if child in expected and isinstance(obj[key], dict):
                _walk(obj[key], child)

    _walk(raw, "")
    return unknown


def check_unknown_status(
    value: str,
    known: FrozenSet[str],
    site: str,
    username: str = "",
    logger: Optional[logging.Logger] = None,
) -> bool:
    """Check whether *value* is a status string we don't recognise.

    Logs a **WARNING** the first time an unknown value is seen for a given
    site (deduplicated per process).

    Returns ``True`` if the value is unknown.
    """
    if not value or value in known:
        return False

    log = logger or _logger
    dedup_key = f"{site}:status:{value}"
    if dedup_key not in _reported_statuses:
        _reported_statuses.add(dedup_key)
        log.warning(
            f"[{site}] Unknown status value '{value}' for {username} — may need new mapping"
        )
    return True
