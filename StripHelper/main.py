from __future__ import annotations
import argparse, os
from pathlib import Path
from striphelper.utils import setup_logging
from striphelper.cli import run_cli

def main():
    ap = argparse.ArgumentParser(description="StripHelper 3 — modular TS/MP4→MKV remux + smart concat")
    ap.add_argument("path", nargs="?", help="Root folder")
    ap.add_argument("--cli", action="store_true", help="Run in CLI mode")
    ap.add_argument("-createsymlinks", action="store_true", help="Create symlinks after merging (if configured)")
    ap.add_argument("--threads", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--log", default="INFO", help="Logging level: DEBUG, INFO, WARNING, ERROR")
    args = ap.parse_args()

    # logging (strings like "DEBUG" are ok for logging.basicConfig)
    setup_logging(args.log.upper())

    # If CLI requested, do that directly.
    if args.cli:
        if not args.path:
            print("CLI needs a path")
            return
        root = Path(args.path)
        if not root.exists():
            print("Dir not found:", root)
            return
        run_cli(root, args.threads, args.createsymlinks)
        return

    # GUI path — import lazily so missing tkinter won't crash at import time.
    try:
        from striphelper.gui import App
    except Exception as e:
        print(f"[StripHelper] GUI unavailable ({e}); falling back to CLI.")
        if not args.path:
            print("GUI fallback requires a path. Provide one or run with --cli.")
            return
        root = Path(args.path)
        if not root.exists():
            print("Dir not found:", root)
            return
        run_cli(root, args.threads, args.createsymlinks)
        return

    # Launch GUI
    preset = Path(args.path) if args.path else None
    app = App(args.threads, args.createsymlinks, preset)

    # Auto-start even if no preset path is provided → uses GUI folder picker
    if not preset:
        def start_after_gui_ready():
            chosen = Path(app.path_var.get())
            if chosen.exists():
                app.start_for_root(chosen)
        app.after(1000, start_after_gui_ready)

    app.mainloop()

if __name__ == "__main__":
    main()
