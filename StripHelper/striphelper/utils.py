from __future__ import annotations
import logging, os, re, time, ctypes, sys
from pathlib import Path

def setup_logging(level=logging.INFO):
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s"
    )

def is_admin() -> bool:
    if os.name != "nt":
        return True
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except Exception:
        return True

def natkey(s: str):
    return [int(x) if x.isdigit() else x.lower() for x in re.split(r"(\d+)", s)]

def human_bytes(n: float) -> str:
    try:
        n = float(n)
    except Exception:
        return "0 B"
    for u in ("B","KiB","MiB","GiB","TiB"):
        if n < 1024 or u == "TiB":
            return f"{n:.2f} {u}"
        n /= 1024

def human_secs(s: float) -> str:
    try:
        return f"{float(s):.3f}s"
    except Exception:
        return "0.000s"

def quote_ff(p: str) -> str:
    return p.replace("'", r"'\\''")

def even(n: int) -> int:
    return n if n % 2 == 0 else n - 1


def human_bytes(n: int | float | None) -> str:
    n = 0 if n is None else float(max(n, 0))
    units = ["B","KiB","MiB","GiB","TiB","PiB"]
    i = 0
    while n >= 1024.0 and i < len(units) - 1:
        n /= 1024.0; i += 1
    return f"{n:.1f} {units[i]}"

def human_eta(secs: float | int | None) -> str:
    s = int(max(0, int(secs or 0)))
    h, r = divmod(s, 3600); m, s = divmod(r, 60)
    return f"{h:02d}:{m:02d}:{s:02d}"





class WorkerFail(Exception):
    def __init__(self, folder: Path, stage: str, message: str):
        super().__init__(f"{stage}: {message}")
        self.folder = folder
        self.stage = stage
        self.message = message
