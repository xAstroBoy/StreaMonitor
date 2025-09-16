#!/usr/bin/env python
# -*- coding: utf-8 -*-

import ctypes
import os
import shutil
import subprocess
import sys
from pathlib import Path

# where StripHelper lives
STRIPHELPER = Path(r"C:\AstroTools\StripHelper\main.py")
TO_PROCESS  = Path(r"F:\To Process")

# ─── elevate if required ─────────────────────────────────────
def is_admin() -> bool:
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except Exception:
        return False

if not is_admin():
    ctypes.windll.shell32.ShellExecuteW(
        None, "runas", sys.executable, " ".join(sys.argv), None, 1
    )
    sys.exit(0)

# ─── get root path (arg or prompt) ───────────────────────────
if len(sys.argv) > 1:
    root = Path(sys.argv[1])
else:
    root = Path(input("Root folder path: ").strip())

if not root.exists():
    print("Folder not found:", root)
    input("Press Enter to exit…")
    sys.exit(1)

# ─── clear+recreate F:\To Process once ───────────────────────
if TO_PROCESS.exists():
    shutil.rmtree(TO_PROCESS, ignore_errors=True)
TO_PROCESS.mkdir(parents=True, exist_ok=True)

# ─── choose CLI or GUI ───────────────────────────────────────
#   → CLI shows every folder path in the console
#   → GUI opens the single Tk window from StripHelper
USE_CLI = False        # set True if you always want CLI

args = [
    sys.executable,
    str(STRIPHELPER),
    str(root),
    ("--cli" if USE_CLI else ""),      # "" is ignored
    "-createsymlinks",
]

# filter out empty strings
args = [a for a in args if a]

# on Windows hide the helper’s own console if we're using GUI
flags = subprocess.CREATE_NO_WINDOW if (os.name == "nt" and not USE_CLI) else 0

try:
    subprocess.run(
        args,
        check=True,
        creationflags=flags,
    )
except subprocess.CalledProcessError as e:
    print("StripHelper reported an error:", e)
    input("Press Enter to exit…")
