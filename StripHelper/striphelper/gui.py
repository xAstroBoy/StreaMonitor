from __future__ import annotations
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog
from pathlib import Path
from datetime import datetime
import json
import os

from .pipeline import merge_folder, find_work_folders, make_symlink
from .config import CONFIG_PATH
from .utils import human_bytes, human_eta

# ─── helpers ─────────────────────────────────────────────────────────────────

def _ts() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def folder_is_already_done(f: Path) -> bool:
    """
    Consider a folder 'done' if 0.mkv exists and there are no temp/extra parts to process.
    """
    try:
        merged = f / "0.mkv"
        if not merged.exists():
            return False
        for p in f.iterdir():
            if p.is_file():
                if p.name in ("0.mkv", "Merger.log", "merge_error.log", "diagnostics.json"):
                    continue
                if p.suffix.lower() in {".ts", ".mp4", ".mkv"} and p.name != "0.mkv":
                    return False
                if p.name.endswith(".tmp.ts") or p.name.endswith(".tmp.mp4"):
                    return False
        return True
    except Exception:
        return False


# ─── GUI ─────────────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self, threads: int, createsymlinks: bool, preset: Path | None):
        super().__init__()
        self.title("StripHelper 3 — Progress")
        self.geometry("1280x800")
        self.minsize(960, 540)

        # run params
        self.TH = threads
        self.LINKS = createsymlinks

        # load config file (if present)
        try:
            self.cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8")) if CONFIG_PATH.exists() else {}
        except Exception:
            self.cfg = {}

        # ─── top bar ──────────────────────────────────────────────────────────
        top = ttk.Frame(self)
        top.pack(fill="x", padx=8, pady=6)

        self.path_var = tk.StringVar(value=str(preset) if preset else "Choose a root folder…")

        self.path_entry = ttk.Entry(top, textvariable=self.path_var)
        self.path_entry.pack(side="left", fill="x", expand=True)

        ttk.Button(top, text="Browse", command=self.pick_path).pack(side="left", padx=6)
        self.start_btn = ttk.Button(top, text="Start", command=self.maybe_start)
        self.start_btn.pack(side="left")

        # ─── tree + scrollbars ────────────────────────────────────────────────
        mid = ttk.Frame(self)
        mid.pack(fill="both", expand=True, padx=8, pady=6)

        cols = ("stage", "pct", "eta", "written", "target", "remain", "dur", "size", "info")
        self.tree = ttk.Treeview(mid, columns=cols, show="tree headings", selectmode="browse")

        self.tree.heading("#0", text="Folder")
        self.tree.column("#0", width=520, anchor="w")  # wider for paths

        # headers and sensible default widths
        widths = {
            "stage": 320, "pct": 64, "eta": 86, "written": 110, "target": 110, "remain": 110,
            "dur": 160, "size": 170, "info": 420
        }
        anchors = {
            "stage": "w", "pct": "e", "eta": "center", "written": "e", "target": "e",
            "remain": "e", "dur": "center", "size": "center", "info": "w"
        }
        for c in cols:
            self.tree.heading(c, text=c.capitalize())
            self.tree.column(c, width=widths.get(c, 120), anchor=anchors.get(c, "center"), stretch=(c == "info"))

        vbar = ttk.Scrollbar(mid, orient="vertical", command=self.tree.yview)
        hbar = ttk.Scrollbar(mid, orient="horizontal", command=self.tree.xview)
        self.tree.configure(yscrollcommand=vbar.set, xscrollcommand=hbar.set)

        self.tree.grid(row=0, column=0, sticky="nsew")
        vbar.grid(row=0, column=1, sticky="ns")
        hbar.grid(row=1, column=0, sticky="ew")

        mid.rowconfigure(0, weight=1)
        mid.columnconfigure(0, weight=1)

        # mouse-wheel scrolling (Windows / Linux / macOS)
        self.tree.bind("<MouseWheel>", self._on_mousewheel)          # Windows
        self.tree.bind("<Button-4>",   self._on_mousewheel_linux)    # Linux scroll up
        self.tree.bind("<Button-5>",   self._on_mousewheel_linux)    # Linux scroll down
        self.tree.bind("<Double-1>", self._open_folder)
        
        # row colors
        self.tree.tag_configure("pending", foreground="#808080")
        self.tree.tag_configure("work",    foreground="#1f6feb")
        self.tree.tag_configure("done",    foreground="#1a7f37")
        self.tree.tag_configure("err",     foreground="#d1242f")

        # ─── bottom bar ──────────────────────────────────────────────────────
        bot = ttk.Frame(self)
        bot.pack(fill="x", padx=8, pady=(0, 8))

        self.pb = ttk.Progressbar(bot, mode="determinate")
        self.pb.pack(fill="x", padx=10, pady=4)

        self.lbl = ttk.Label(bot, text="0/0")
        self.lbl.pack(side="right", padx=8)

        # state
        self.nodes: dict[str, str] = {}
        self.root_path: Path | None = None
        self.log_path: Path | None = None
        self.workset: set[str] = set()
        self.total = 0
        self.done = 0
        self.lock = threading.Lock()
        self.log_lock = threading.Lock()
        self._started = False

        # auto-start if preset provided
        if preset and preset.exists():
            self.after(300, lambda: self.start_for_root(preset))

    # ─── scrolling handlers ──────────────────────────────────────────────────
    def _on_mousewheel(self, event):
        # Windows / macOS normalized
        delta = int(-1 * (event.delta / 120))
        self.tree.yview_scroll(delta, "units")

    def _on_mousewheel_linux(self, event):
        if event.num == 4:
            self.tree.yview_scroll(-1, "units")
        elif event.num == 5:
            self.tree.yview_scroll(1, "units")

    # ─── top bar actions ─────────────────────────────────────────────────────
    def pick_path(self):
        sel = filedialog.askdirectory()
        if sel:
            self.path_var.set(sel)

    # ─── logging ─────────────────────────────────────────────────────────────
    def log(self, text: str):
        if not self.log_path:
            return
        with self.log_lock:
            try:
                self.log_path.parent.mkdir(parents=True, exist_ok=True)
                with open(self.log_path, "a", encoding="utf-8") as fh:
                    fh.write(f"[{_ts()}] {text}\n")
            except Exception:
                pass

    # ─── tree helpers ───────────────────────────────────────────────────────
    def _insert_node(self, parent_id: str, text: str, is_leaf: bool, initial_tag: str) -> str:
        vals = ("pending", "", "", "", "", "", "", "", "") if is_leaf and initial_tag == "pending" else ("",) * 9
        nid = self.tree.insert(parent_id, "end", text=text, values=vals, tags=(initial_tag,))
        self.tree.item(nid, open=True)
        return nid

    def _open_folder(self, event):
        item = self.tree.identify_row(event.y)
        if not item:
            return
        # build absolute path from item’s ancestry
        parts = []
        cur = item
        while cur:
            parts.append(self.tree.item(cur, "text"))
            cur = self.tree.parent(cur)
        parts.reverse()
        if not self.root_path:
            return
        p = self.root_path
        # first node is the root itself; skip it if duplicate
        for part in parts[1:]:
            p = p / part
        if os.name == "nt":
            os.startfile(str(p))
        else:
            import subprocess
            subprocess.Popen(["xdg-open", str(p)])

    # bind after tree creation:


    def _ensure_path_node(self, abs_path: Path, leaf_tag: str) -> str:
        """
        Build the path chain from root to abs_path, tagging the leaf with leaf_tag.
        Parents inherit a computed tag based on children later.
        """
        assert self.root_path is not None
        parent = ""
        cur = self.root_path
        for part in abs_path.relative_to(self.root_path).parts:
            cur = cur / part
            key = str(cur)
            if key not in self.nodes:
                # mark unknown nodes as 'pending' by default; leaf will be re-tagged later
                self.nodes[key] = self._insert_node(parent, part, is_leaf=False, initial_tag="pending")
            parent = self.nodes[key]

        # ensure leaf entry exists (flat leaf row for the folder itself)
        leaf_key = str(abs_path)
        if leaf_key not in self.nodes:
            self.nodes[leaf_key] = self._insert_node(parent, abs_path.name, is_leaf=True, initial_tag=leaf_tag)
        else:
            # apply tag to existing leaf
            self.tree.item(self.nodes[leaf_key], tags=(leaf_tag,))

        return self.nodes[leaf_key]

    def _update_parent_tags(self, leaf_path: Path):
        """Propagate child state to parents: err > work/pending > done."""
        assert self.root_path is not None
        p = leaf_path
        while p != self.root_path:
            parent = p.parent
            pid = self.nodes.get(str(parent))
            if pid:
                child_tags = set()
                for cid in self.tree.get_children(pid):
                    tags = self.tree.item(cid, "tags")
                    if tags:
                        child_tags.update(tags)
                if "err" in child_tags:
                    tag = "err"
                elif "work" in child_tags or "pending" in child_tags:
                    tag = "work"
                else:
                    tag = "done"
                self.tree.item(pid, tags=(tag,))
            p = parent

    def set_row(self, p: Path, stage=None, pct=None, eta=None,
                written=None, target=None, dur=None, size=None,
                info=None, tag=None):
        key = str(p)
        nid = self.nodes.get(key)
        if not nid:
            nid = self._ensure_path_node(p, leaf_tag="pending")

        vals = list(self.tree.item(nid, "values"))
        while len(vals) < 9:
            vals.append("")

        if stage is not None:
            vals[0] = stage
        if pct is not None:
            try:
                vals[1] = f"{float(pct):4.1f}"
            except Exception:
                vals[1] = ""
        if eta is not None:
            vals[2] = human_eta(eta)
        if written is not None:
            vals[3] = human_bytes(written)
        if target is not None:
            vals[4] = human_bytes(target)
            remain = max(0, (target or 0) - (written or 0))
            vals[5] = human_bytes(remain)
        if dur is not None:
            vals[6] = dur
        if size is not None:
            vals[7] = size
        if info is not None:
            vals[8] = info

        self.tree.item(nid, values=tuple(vals))
        if tag:
            self.tree.item(nid, tags=(tag,))
            self._update_parent_tags(p)

    # ─── run control ─────────────────────────────────────────────────────────
    def maybe_start(self):
        if self._started:
            return
        p = self.path_var.get()
        if not p or p.lower().startswith("choose"):
            return
        root = Path(p)
        if not root.exists():
            return
        self.start_for_root(root)

    def start_for_root(self, root: Path):
        if self._started:
            return

        self._started = True
        self.start_btn.configure(state="disabled")
        self.root_path = root
        self.log_path = root / "Merger.log"

        # Clear old log
        try:
            if self.log_path.exists():
                self.log_path.unlink()
        except Exception:
            pass

        self.log(f"=== START run: threads={self.TH} symlinks={self.LINKS} root={root} ===")

        all_folders = find_work_folders(root)

        # split into already_done vs to_process
        already_done = [f for f in all_folders if folder_is_already_done(f)]
        to_process = [f for f in all_folders if f not in already_done]
        self.workset = set(str(f) for f in to_process)

        # show already-done as green and (optionally) create symlink right now
        for f in already_done:
            nid = self._ensure_path_node(f, leaf_tag="done")
            self.tree.set(nid, "stage", "already merged")
            self.log(f"[SKIP] {f} (already merged)")
            # ensure symlink when requested even for already merged folders
            if self.LINKS:
                try:
                    make_symlink(f, self.cfg or {})
                except Exception:
                    # don't crash the UI if symlink perms are flaky
                    pass

        # queue work folders
        for f in to_process:
            self._ensure_path_node(f, leaf_tag="pending")
            self.log(f"[QUEUE] {f}")

        self.total = len(to_process)
        self.pb["maximum"] = max(self.total, 1)
        self.lbl.config(text=f"{self.done}/{self.total}")

        if not to_process:
            self.log("=== FINISH run ===")
            return

        def job(fld: Path):
            try:
                self.after(0, self.set_row, fld, "queued", 0, 0, 0, 0, None, None, "", "work")
                self.log(f"[WORK] {fld}")

                last = {"pct": 0, "eta": 0, "bytes_w": 0, "target_b": 0, "dur": "", "size": "", "info": ""}

                def gui_cb(pct, eta, bytes_w, target_b):
                    last.update({"pct": pct or 0.0, "eta": eta or 0.0, "bytes_w": bytes_w or 0, "target_b": target_b or 0})
                    self.after(0, self.set_row, fld, None, last["pct"], last["eta"], last["bytes_w"], last["target_b"],
                               last["dur"], last["size"], last["info"], "work")

                def note_cb(msg):
                    self.log(f"[{fld.name}] {msg}")
                    self.after(0, self.set_row, fld, msg, None, None, None, None, None, None, None, "work")

                def metric_cb(stage, src_size, out_size, src_dur, out_dur, info_text):
                    dur_str = f"{human_eta(src_dur)} → {human_eta(out_dur)}"
                    size_str = f"{human_bytes(src_size)} → {human_bytes(out_size)}"
                    last.update({"dur": dur_str, "size": size_str, "info": info_text})
                    self.log(f"[{fld.name}] {stage} | {dur_str} | {size_str} | {info_text}")
                    self.after(0, self.set_row, fld, stage, None, None, None, None, dur_str, size_str, info_text, "work")

                # initial row
                self.after(0, self.set_row, fld, "scan/remux", 0, 0, 0, 0, "", "", "", "work")

                # do the actual work
                merge_folder(
                    fld,
                    gui_cb=gui_cb,
                    cfg=self.cfg,
                    mk_links=self.LINKS,  # pipeline will create symlink on success & “only symlink” pass
                    note_cb=note_cb,
                    metric_cb=metric_cb,
                )

                self.log(f"[DONE] {fld}")
                self.after(0, self.set_row, fld, "done", 100, last["eta"], last["bytes_w"],
                           last["target_b"], last["dur"], last["size"], last["info"], "done")

            except Exception as e:
                # Inline tail of per-folder merge_error.log to Merger.log
                tail = ""
                try:
                    errp = fld / "merge_error.log"
                    if errp.exists():
                        tail = errp.read_text(encoding="utf-8", errors="ignore")[-4000:]
                except Exception:
                    pass
                self.log(f"[ERROR] {fld} :: {e}")
                if tail:
                    for line in tail.strip().splitlines():
                        self.log(f"[{fld.name}] stderr: {line}")
                self.after(0, self.set_row, fld, "ERROR", None, None, None, None, None, None, "see Merger.log", "err")
            finally:
                with self.lock:
                    self.done += 1
                    self.pb["value"] = self.done
                    self.lbl.config(text=f"{self.done}/{self.total}")
                    if self.done == self.total:
                        self.log("=== FINISH run ===")

        def pool_thread():
            from concurrent.futures import ThreadPoolExecutor, wait
            with ThreadPoolExecutor(max_workers=self.TH) as pool:
                wait([pool.submit(job, f) for f in to_process])

        threading.Thread(target=pool_thread, daemon=True).start()
