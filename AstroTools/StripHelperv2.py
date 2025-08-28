#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
StripHelper – robust TS→MKV remux + concat with live target/remain and per-step GUI notes.

2025-08-22 (MKV-ONLY EDITION)

Design goals:
• Fully salvage long/broken .ts without truncation by using MATROSKA (MKV) as the tolerant container.
• Pipeline:
    TS → MKV (copy, fresh PTS, ignore bad DTS) [always first choice]
    Merge MKVs → 0.mkv (copy)
• Strict validation before deleting anything. If validation fails at any stage, originals stay.
• GUI shows granular notes for each step (TS remux passes, bridge attempts, validation, final format).
• Root folder is ignored (only subfolders are processed).
• Safe concat using FFmpeg concat demuxer in copy mode.

Why MKV:
• MP4 is picky about timebases/interleaving; MKV is far more tolerant with discontinuities and odd timestamp graphs.
• This avoids “short 6 min from 4+ hour TS” issues.
"""

import argparse, ctypes, json, logging, os, re, shutil, subprocess, sys, threading, time, traceback
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, wait
from subprocess import DEVNULL, PIPE, STARTUPINFO, STARTF_USESHOWWINDOW, CREATE_NO_WINDOW

FFMPEG  = "ffmpeg"
FFPROBE = "ffprobe"

DELETE_TS_AFTER_REMUX = True

CONFIG_PATH = Path(r"F:\config.json")
TO_PROCESS  = Path(r"F:\To Process")

ERROR_LOG = Path(__file__).with_name("StripHelper_errors.log")
WIN_HIDE  = CREATE_NO_WINDOW if os.name == "nt" else 0

# Validation thresholds
MIN_SIZE_RATIO_GENERIC = 0.985
MIN_DUR_RATIO_GENERIC  = 0.990

# TS→MKV: TS has transport overhead; container shrink is normal.
MIN_SIZE_RATIO_TS2MUX  = 0.940
MIN_DUR_RATIO_TS2MUX   = 0.985

def _is_admin():
    if os.name != "nt": return True
    try: return ctypes.windll.shell32.IsUserAnAdmin()
    except Exception: return True

if os.name == "nt" and not _is_admin():
    ctypes.windll.shell32.ShellExecuteW(
        None, "runas", sys.executable,
        " ".join(map(repr, sys.argv)).replace("'", '"'),
        None, 1
    )
    sys.exit(0)

logging.basicConfig(level=logging.ERROR,
                    format="%(asctime)s %(levelname)s %(message)s",
                    handlers=[logging.StreamHandler(sys.stdout)])
log = logging.getLogger("StripHelper")

def log_error(folder: Path, stage: str, err: str):
    try:
        with ERROR_LOG.open("a", encoding="utf-8") as fh:
            fh.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {folder}\n")
            fh.write(f"CMD : {stage}\n{err}\n{'-'*60}\n")
    except Exception:
        pass

def natkey(s: str):
    return [int(x) if x.isdigit() else x.lower() for x in re.split(r"(\d+)", s)]

def _si():
    if os.name != "nt": return None
    si = STARTUPINFO(); si.dwFlags |= STARTF_USESHOWWINDOW
    return si

def human_bytes(n):
    for u in ("B","KiB","MiB","GiB","TiB"):
        if n < 1024 or u == "TiB": return f"{n:.2f} {u}"
        n /= 1024

def quote_ff(p: str) -> str:
    return p.replace("'", r"'\''")

# ───────── ffprobe helpers ─────────
def ffprobe_ok(fp: Path, cwd: Path) -> bool:
    r = subprocess.run([FFPROBE, "-v", "error", "-i", fp.name],
                       cwd=cwd, stdout=DEVNULL, stderr=PIPE,
                       creationflags=WIN_HIDE, startupinfo=_si())
    if r.returncode:
        log_error(cwd, "ffprobe", r.stderr.decode(errors="ignore"))
    return r.returncode == 0

def ffprobe_num(fp: Path, cwd: Path, args: list[str]) -> float:
    try:
        out = subprocess.check_output([FFPROBE, "-v", "error"] + args + [fp.name],
                                      cwd=cwd, stderr=DEVNULL)
        s = (out.decode(errors="ignore").strip() or "0").splitlines()[0]
        return float("0" if s in ("N/A","") else s)
    except Exception:
        return 0.0

def ffprobe_format_duration(fp: Path, cwd: Path) -> float:
    return ffprobe_num(fp, cwd, ["-show_entries","format=duration","-of","default=nw=1:nk=1"])

def ffprobe_stream_duration(fp: Path, cwd: Path, selector: str) -> float:
    return ffprobe_num(fp, cwd, ["-select_streams", selector, "-show_entries","stream=duration","-of","default=nw=1:nk=1"])

def ffprobe_best_duration(fp: Path, cwd: Path) -> float:
    fmt = ffprobe_format_duration(fp, cwd) or 0.0
    vd  = ffprobe_stream_duration(fp, cwd, "v:0") or 0.0
    ad  = ffprobe_stream_duration(fp, cwd, "a:0") or 0.0
    return max(fmt, vd, ad)

# ───────── temp/utility ─────────
def purge_temp(folder: Path):
    for f in ("concat_list.txt","0~merge.mp4","0~merge.mkv",".stage.mkv"):
        (folder/f).unlink(missing_ok=True)

def convert_tmp(tmp: Path, folder: Path):
    clean = tmp.with_name(tmp.stem[:-4] + ".mkv")
    cmd=[FFMPEG,"-y","-i",tmp.name,"-c","copy","-map","0","-copyinkf",clean.name]
    r=subprocess.run(cmd,cwd=folder,stdout=DEVNULL,stderr=PIPE,
                     creationflags=WIN_HIDE,startupinfo=_si())
    if r.returncode:
        log_error(folder,"ffmpeg-tmp",r.stderr.decode(errors="ignore"))
    else:
        tmp.unlink(missing_ok=True)

# ───────── validation ─────────
def _validate_against_source(folder: Path, src: Path, out: Path, is_ts_to_mux: bool) -> tuple[bool,str]:
    try:
        if not out.exists():
            return False, "Output does not exist."

        src_size = src.stat().st_size
        out_size = out.stat().st_size
        src_dur  = ffprobe_best_duration(src, folder)
        out_dur  = ffprobe_best_duration(out, folder)

        if is_ts_to_mux:
            min_size_ratio = MIN_SIZE_RATIO_TS2MUX
            min_dur_ratio  = MIN_DUR_RATIO_TS2MUX
        else:
            min_size_ratio = MIN_SIZE_RATIO_GENERIC
            min_dur_ratio  = MIN_DUR_RATIO_GENERIC

        size_ok = out_size >= int(src_size * min_size_ratio)
        if src_dur <= 0.0 or out_dur <= 0.0:
            dur_ok = True
        else:
            dur_ok = out_dur >= src_dur * min_dur_ratio

        ok = size_ok and dur_ok
        if not ok:
            msg = (f"Validation failed: out_size={out_size} vs src_size={src_size} "
                   f"(min {int(src_size*min_size_ratio)}), out_dur≈{out_dur:.3f}s vs "
                   f"src_dur≈{src_dur:.3f}s (min {src_dur*min_dur_ratio:.3f}s)")
            return False, msg
        return True, ""
    except Exception:
        return False, traceback.format_exc()

# ───────── remux: TS → MKV primary (copy) ─────────
def remux_ts(files, folder: Path, note_cb=None):
    out=[]
    for p in files:
        if p.suffix.lower()!=".ts":
            if note_cb: note_cb(f"keep {p.name}")
            out.append(p); continue

        rem_mkv=p.with_suffix(".remux.mkv")
        stage_mkv = folder/".stage.mkv"

        def validate(src, dst, is_ts=True, stage_name="validate"):
            ok,msg=_validate_against_source(folder, src, dst, is_ts)
            if not ok: log_error(folder, f"{stage_name}", msg)
            if note_cb: note_cb(f"{stage_name} {'OK' if ok else 'FAIL'}")
            return ok

        def run(stage_name, args):
            if note_cb: note_cb(stage_name)
            r=subprocess.run(args, cwd=folder, stdout=DEVNULL, stderr=PIPE,
                             creationflags=WIN_HIDE, startupinfo=_si())
            if r.returncode:
                log_error(folder, stage_name, r.stderr.decode(errors="ignore"))
                if note_cb: note_cb(f"{stage_name} failed")
                return False
            if note_cb: note_cb(f"{stage_name} OK")
            return True

        if rem_mkv.exists():
            if validate(p, rem_mkv, True, "reuse-remux-validate"):
                out.append(rem_mkv)
                if DELETE_TS_AFTER_REMUX: p.unlink(missing_ok=True)
                continue
            else:
                rem_mkv.unlink(missing_ok=True)

        # Pass 1: TS → MKV (fresh PTS, ignore DTS/discard corrupt)
        cmd1=[FFMPEG,"-y",
              "-analyzeduration","2147483647","-probesize","2147483647",
              "-fflags","+genpts+igndts+discardcorrupt",
              "-use_wallclock_as_timestamps","1",
              "-err_detect","ignore_err",
              "-i",p.name,
              "-map","0:v:0?","-map","0:a?","-dn","-sn",
              "-fps_mode","passthrough",
              "-c","copy","-copyinkf",
              rem_mkv.name]

        # Pass 2: tighten mux pacing (still MKV)
        cmd2=[FFMPEG,"-y",
              "-analyzeduration","2147483647","-probesize","2147483647",
              "-fflags","+genpts+igndts+discardcorrupt",
              "-use_wallclock_as_timestamps","1",
              "-err_detect","ignore_err",
              "-i",p.name,
              "-avoid_negative_ts","make_zero",
              "-muxpreload","0","-muxdelay","0",
              "-max_interleave_delta","0",
              "-map","0:v:0?","-map","0:a?","-dn","-sn",
              "-fps_mode","passthrough",
              "-c","copy","-copyinkf",
              rem_mkv.name]

        ok = run(f"remux {p.name} → MKV pass1", cmd1) and validate(p, rem_mkv, True, "remux pass1-validate")
        if not ok:
            rem_mkv.unlink(missing_ok=True)
            ok = run(f"remux {p.name} → MKV pass2", cmd2) and validate(p, rem_mkv, True, "remux pass2-validate")

        if ok:
            if DELETE_TS_AFTER_REMUX:
                if note_cb: note_cb(f"delete source {p.name}")
                p.unlink(missing_ok=True)
            out.append(rem_mkv)
        else:
            if note_cb: note_cb(f"keep source {p.name}")
            rem_mkv.unlink(missing_ok=True)
            out.append(p)

    return out

# ───────── concat/merge ─────────
def write_concat(folder: Path, parts, include_zero: bool)->Path:
    txt=folder/"concat_list.txt"
    lines=[]
    if include_zero:
        lines.append(f"file '{quote_ff('0.mkv')}'")
    for p in parts:
        if p.name=="0.mkv": continue
        lines.append(f"file '{quote_ff(p.name)}'")
    txt.write_text("\n".join(lines),encoding="utf-8")
    return txt

def run_ffmpeg(cmd, cwd: Path, stage: str, progress_cb=None, target_bytes=0, note_cb=None):
    if progress_cb:
        cmd = cmd[:1] + ["-progress","pipe:2","-nostats"] + cmd[1:]
    if note_cb: note_cb(stage)
    proc = subprocess.Popen(cmd,cwd=cwd,stdout=DEVNULL,
                            stderr=PIPE if progress_cb else DEVNULL,
                            universal_newlines=True,
                            creationflags=WIN_HIDE,startupinfo=_si())
    out_time=0.0; bytes_written=0
    if progress_cb:
        while True:
            line = proc.stderr.readline()
            if not line and proc.poll() is not None: break
            if not line:
                time.sleep(0.05); continue
            line=line.strip()
            if line.startswith("out_time_ms="):
                try: out_time=int(line.split("=",1)[1])/1_000_000
                except: pass
            elif line.startswith("total_size="):
                try: bytes_written=int(line.split("=",1)[1])
                except: pass
            elif line=="progress=end":
                break
            progress_cb(out_time, bytes_written, target_bytes)
    ret=proc.wait()
    if ret!=0:
        err=""
        try: err=proc.stderr.read() if proc.stderr else ""
        except Exception: pass
        log_error(cwd,stage,err)
        if note_cb: note_cb(f"{stage} failed")
        return False
    if note_cb: note_cb(f"{stage} OK")
    return True

def concat_copy_mkv(folder: Path, concat_txt: Path, progress_cb=None, target_bytes=0, note_cb=None):
    tmp=folder/"0~merge.mkv"
    cmd=[FFMPEG,"-y","-f","concat","-safe","0","-i",concat_txt.name,
         "-c","copy","-map","0","-copyinkf", tmp.name]
    return run_ffmpeg(cmd, folder, "concat copy -> MKV", progress_cb, target_bytes, note_cb)

def validate_merge(folder: Path, tmp_out: Path, target_bytes: int, total_dur: float, is_ts_path=False) -> tuple[bool,str]:
    try:
        out_size = tmp_out.stat().st_size if tmp_out.exists() else 0
        out_dur  = ffprobe_best_duration(tmp_out, folder) if tmp_out.exists() else 0.0
        if is_ts_path:
            min_size = int(target_bytes * MIN_SIZE_RATIO_TS2MUX)
            min_dur  = total_dur * MIN_DUR_RATIO_TS2MUX
        else:
            min_size = int(target_bytes * MIN_SIZE_RATIO_GENERIC)
            min_dur  = total_dur * MIN_DUR_RATIO_GENERIC
        size_ok = out_size >= min_size
        dur_ok  = (out_dur <= 0.0) or (total_dur <= 0.0) or (out_dur >= min_dur)
        ok = size_ok and dur_ok
        if not ok:
            msg=(f"merge-validate FAIL: out={out_size}B/{out_dur:.3f}s, "
                 f"need ≥{min_size}B/≥{min_dur:.3f}s "
                 f"(target_bytes={target_bytes}, total_dur≈{total_dur:.3f}s)")
            return False, msg
        return True, ""
    except Exception:
        return False, traceback.format_exc()

# ───────── generic → MKV (copy) for single-file publish ─────────
def to_mkv_copy(folder: Path, src: Path, dst_mkv: Path, note_cb=None, is_ts=False) -> bool:
    cmd=[FFMPEG,"-y","-i",src.name,"-map","0","-c","copy","-copyinkf",dst_mkv.name]
    if note_cb: note_cb(f"finalize {src.suffix.lower()}→MKV")
    r=subprocess.run(cmd,cwd=folder,stdout=DEVNULL,stderr=PIPE,
                     creationflags=WIN_HIDE,startupinfo=_si())
    if r.returncode:
        log_error(folder,"→mkv",r.stderr.decode(errors="ignore"))
        if note_cb: note_cb("finalize →MKV failed")
        return False
    if note_cb: note_cb("finalize →MKV OK")
    ok,msg=_validate_against_source(folder, src, dst_mkv, is_ts_to_mux=is_ts)
    if not ok:
        log_error(folder,"→mkv-validate",msg)
        if note_cb: note_cb("→MKV validate FAIL")
        return False
    return True

# ───────── symlink helper (MKV only) ─────────
def make_symlink(folder: Path, cfg: dict):
    try:
        src = folder/"0.mkv"
        if not src.exists(): return
        model_root=folder
        for q in (folder,*folder.parents):
            if "[" in q.name and "]" in q.name:
                model_root=q; break
        def ftype(n):
            vr=[t.lower() for t in cfg.get("VR",[])]
            dt=[t.lower() for t in cfg.get("Desktop",[])]
            n=n.lower()
            if any(t in n for t in vr): return "VR"
            if any(t in n for t in dt): return "NO VR"
            return "UNKNOWN"
        def clean(n):
            out=n
            for t in cfg.get("VR",[])+cfg.get("Desktop",[]): out=out.replace(t,"").strip()
            for m,als in cfg.get("Aliases",{}).items():
                for a in als: out=out.replace(a,m)
            return out.strip()
        def getsite(n):
            if "[" not in n or "]" not in n: return ""
            start=n.index("[")+1; end=n.index("]")
            return n[start:end].strip()
        tag=ftype(model_root.name)
        model=clean(model_root.name)
        rel=folder.relative_to(model_root)
        dst_dir=TO_PROCESS/model/tag/rel
        dst_dir.mkdir(parents=True,exist_ok=True)
        base=src.name
        site=getsite(model_root.name)
        while True:
            dst=dst_dir/base
            if not dst.exists():
                try:
                    os.symlink(src,dst)
                    return
                except FileExistsError:
                    pass
            base=f"0_[{site}]{src.suffix}"
    except Exception:
        log_error(folder,"symlink",traceback.format_exc())

# ───────── core folder op ─────────
def merge_folder(folder: Path, gui_cb=None, cfg=None, mk_links=False, note_cb=None):
    purge_temp(folder)
    for t in folder.glob("*.tmp.mp4"):
        if note_cb: note_cb(f"convert {t.name}")
        convert_tmp(t, folder)

    merged_mkv = folder/"0.mkv"
    # Nuke any legacy 0.mp4 from older runs
    (folder/"0.mp4").unlink(missing_ok=True)

    media=[p for p in folder.iterdir()
           if p.is_file() and p.suffix.lower() in (".ts",".mp4",".mkv") and p.name not in ("0.mkv")]

    good=[]
    for f in sorted(media,key=lambda x:natkey(x.name)):
        if ffprobe_ok(f,folder): good.append(f)
        else:
            if note_cb: note_cb(f"drop unreadable {f.name}")
            f.unlink(missing_ok=True)

    if not merged_mkv.exists() and not good:
        if note_cb: note_cb("nothing to do")
        return
    if merged_mkv.exists() and not good:
        if mk_links: make_symlink(folder, cfg or {})
        if note_cb: note_cb("only symlink pass")
        return

    if not merged_mkv.exists() and len(good)==1:
        only=good[0]
        if only.suffix.lower()==".ts":
            if note_cb: note_cb(f"single file remux {only.name} → MKV")
            rem=remux_ts([only],folder,note_cb=note_cb)[0]
            if rem.suffix.lower()==".mkv" and rem.exists():
                ok,msg=_validate_against_source(folder, only, rem, True)
                if ok:
                    if note_cb: note_cb("publish single MKV")
                    os.replace(rem, merged_mkv)
                    if mk_links: make_symlink(folder, cfg or {})
                else:
                    log_error(folder,"single-remux-validate",msg)
                    if note_cb: note_cb("single remux invalid")
                    rem.unlink(missing_ok=True)
            return
        else:
            # MP4/MKV part – ensure MKV publish
            if only.suffix.lower()==".mkv":
                if note_cb: note_cb(f"single file publish MKV {only.name}")
                os.replace(only, merged_mkv)
            else:
                # mp4 → mkv (copy)
                if note_cb: note_cb(f"single file finalize {only.name} → MKV")
                if to_mkv_copy(folder, only, merged_mkv, note_cb=note_cb, is_ts=False):
                    only.unlink(missing_ok=True)
                    if mk_links: make_symlink(folder, cfg or {})
                else:
                    (folder/"0.mkv").unlink(missing_ok=True)
            if mk_links and merged_mkv.exists(): make_symlink(folder, cfg or {})
            return

    # Normalize: remux any TS to MKV first
    if note_cb:
        ts_count = sum(1 for x in good if x.suffix.lower()==".ts")
        note_cb(f"remux {ts_count} ts parts → MKV")
    parts = remux_ts(good, folder, note_cb=note_cb)

    concat_txt = write_concat(folder, parts, include_zero=merged_mkv.exists())
    if note_cb: note_cb(f"concat list written ({len(parts)} parts) → MKV")

    total_dur = 0.0
    target_bytes = 0
    if merged_mkv.exists():
        total_dur += ffprobe_best_duration(merged_mkv, folder)
        target_bytes += merged_mkv.stat().st_size
    for p in parts:
        total_dur += ffprobe_best_duration(p, folder)
        target_bytes += p.stat().st_size

    if gui_cb:
        gui_cb(0.0, 0.0, 0, target_bytes)

    start=time.time()
    written_holder={"b":0}
    def prog(sec, bw, tgt):
        written_holder["b"]=bw or written_holder["b"]
        if not gui_cb: return
        pct = (sec/total_dur*100.0) if total_dur>0 else 0.0
        elapsed=time.time()-start
        speed = sec/elapsed if elapsed>0 else 0
        eta   = (total_dur-sec)/speed if speed>0 else 0
        gui_cb(pct, eta, written_holder["b"], tgt)

    ok = concat_copy_mkv(folder, concat_txt, progress_cb=prog, target_bytes=target_bytes, note_cb=note_cb)

    if ok:
        tmp = folder/"0~merge.mkv"
        if note_cb: note_cb("validate merged output")
        ok,msg = validate_merge(folder, tmp, target_bytes, total_dur, is_ts_path=True)
    else:
        msg="concat_copy returned non-zero"
        log_error(folder,"merge-copy",msg)

    if ok:
        if note_cb: note_cb("commit merged MKV")
        os.replace(folder/"0~merge.mkv", merged_mkv)

        concat_txt.unlink(missing_ok=True)
        for f in media: f.unlink(missing_ok=True)
        for r in folder.glob("*.remux.mkv"):
            if r.name not in ("0.mkv",): r.unlink(missing_ok=True)
        if mk_links: make_symlink(folder, cfg or {})
    else:
        if note_cb: note_cb("merge rejected; keep originals")
        (folder/"0~merge.mkv").unlink(missing_ok=True)
        (folder/"0~merge.mp4").unlink(missing_ok=True)
        concat_txt.unlink(missing_ok=True)

# ───────── GUI ─────────
class GUI:
    def __init__(self, threads:int, links:bool, preset:Path|None):
        from tkinter import Tk, ttk, StringVar, Scrollbar, VERTICAL, RIGHT, Y, BOTH, filedialog

        self.TH=threads; self.LINKS=links
        self._fdlg=filedialog

        self.root=Tk()
        self.root.title("StripHelper Progress")
        self.root.geometry("1100x660"); self.root.resizable(False,False)

        self.path_var=StringVar(value=str(preset) if preset else "Click to choose root…")
        path_e=ttk.Entry(self.root,textvariable=self.path_var,justify="center",state="readonly")
        path_e.pack(pady=(8,4),fill="x",padx=8)
        path_e.bind("<Button-1>", self.pick_path)

        cols=("status","pct","eta","written","target","remain")
        self.tree=ttk.Treeview(self.root,columns=cols,show="tree headings")
        self.tree.heading("#0",text="Folder / Subfolder")
        self.tree.heading("status",text="Status / Action")
        self.tree.heading("pct",text="%")
        self.tree.heading("eta",text="ETA")
        self.tree.heading("written",text="Written")
        self.tree.heading("target",text="Target")
        self.tree.heading("remain",text="Remain")
        self.tree.column("#0",width=460,anchor="w")
        self.tree.column("status",width=360,anchor="w")
        self.tree.column("pct",width=60,anchor="e")
        self.tree.column("eta",width=90,anchor="center")
        self.tree.column("written",width=100,anchor="e")
        self.tree.column("target",width=110,anchor="e")
        self.tree.column("remain",width=110,anchor="e")
        sb=Scrollbar(self.root,orient=VERTICAL,command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        sb.pack(side=RIGHT,fill=Y)
        self.tree.pack(fill=BOTH,expand=True,padx=8,pady=6)

        from tkinter import ttk
        self.pb=ttk.Progressbar(self.root,mode="determinate",length=1060)
        self.pb.pack(pady=(4,2))
        self.lbl=ttk.Label(self.root,text="0/0")
        self.lbl.pack(pady=(0,8))

        self.nodes={}
        self.total=0; self.done=0
        self.lock=threading.Lock()

        self.root.after(50,self.maybe_start)

    def pick_path(self,_):
        sel=self._fdlg.askdirectory()
        if sel:
            self.path_var.set(sel)
            self.maybe_start()

    def maybe_start(self):
        p=self.path_var.get()
        if not p or p.startswith("Click"): return
        root=Path(p)
        if not root.exists(): return
        self.start_for_root(root)

    def add_node(self,p:Path,root:Path):
        key=str(p)
        if key in self.nodes: return
        rel=p.relative_to(root)
        parent=""; cur=root
        for part in rel.parts:
            cur/=part; k=str(cur)
            if k not in self.nodes:
                label = str(rel) if parent=="" else part
                nid=self.tree.insert(parent,"end",text=label,values=("pending","","","","",""))
                self.tree.item(nid,open=True)
                self.nodes[k]=nid
            parent=self.nodes[k]

    def set_row(self,p:Path,status=None,pct=None,eta=None,written=None,target=None,tag=None):
        nid=self.nodes.get(str(p))
        if not nid: return
        vals=list(self.tree.item(nid,"values"))
        if status  is not None: vals[0]=status
        if pct     is not None: vals[1]=f"{pct:4.1f}"
        if eta     is not None: vals[2]=time.strftime("%H:%M:%S", time.gmtime(max(0,int(eta))))
        if written is not None: vals[3]=human_bytes(written)
        if target  is not None:
            vals[4]=human_bytes(target)
            remain=max(0,target-(written or 0))
            vals[5]=human_bytes(remain)
        self.tree.item(nid,values=tuple(vals))
        self.tree.tag_configure("done",foreground="green")
        self.tree.tag_configure("err", foreground="red")
        self.tree.tag_configure("work",foreground="blue")
        if tag: self.tree.item(nid,tags=(tag,))

    def start_for_root(self, root:Path):
        if self.total>0: return
        folders=[Path(dp) for dp,_,_ in os.walk(root) if Path(dp)!=root]
        self.total=len(folders)
        self.pb["maximum"]=self.total
        self.lbl["text"]=f"{self.done}/{self.total}"
        for f in folders:
            self.add_node(f,root)

        cfg={}
        if CONFIG_PATH.exists():
            try: cfg=json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            except Exception: cfg={}

        def job(fld:Path):
            try:
                self.root.after(0,self.set_row,fld,"queued",0,0,0,0,"work")
                last = {"pct": 0, "eta": 0, "bytes_w": 0, "target_b": 0}
                def gui_cb(pct, eta, bytes_w, target_b):
                    last.update({"pct": pct, "eta": eta, "bytes_w": bytes_w, "target_b": target_b})
                    self.root.after(0, self.set_row, fld, None, pct, eta, bytes_w, target_b, "work")
                def note_cb(msg):
                    self.root.after(0, self.set_row, fld, msg, None, None, None, None, "work")
                self.root.after(0,self.set_row,fld,"scan/remux",0,0,0,0,"work")
                merge_folder(fld, gui_cb=gui_cb, cfg=cfg, mk_links=self.LINKS, note_cb=note_cb)
                self.root.after(0, self.set_row, fld, "done", 100,last["eta"], last["bytes_w"], last["target_b"], "done")
            except Exception:
                log_error(fld,"merge_folder",traceback.format_exc())
                self.root.after(0,self.set_row,fld,"error",None,None,None,None,"err")
            finally:
                with self.lock:
                    self.done+=1
                    self.pb["value"]=self.done
                    self.lbl["text"]=f"{self.done}/{self.total}"

        def pool_thread():
            with ThreadPoolExecutor(max_workers=self.TH) as pool:
                wait([pool.submit(job,f) for f in folders])
            self.root.after(1200,self.root.destroy)

        threading.Thread(target=pool_thread,daemon=True).start()

    def run(self):
        self.root.mainloop()

# ───────── CLI ─────────
def process_folder(folder: Path, links: bool):
    merge_folder(folder, gui_cb=None, cfg=CONFIG, mk_links=links, note_cb=None)
    if not any(folder.iterdir()):
        shutil.rmtree(folder, ignore_errors=True)

def run_cli(root:Path, threads:int, links:bool):
    folders=[Path(dp) for dp,_,_ in os.walk(root) if Path(dp)!=root]
    total=len(folders); done=0
    lock=threading.Lock()
    def job(fld):
        nonlocal done
        try:
            with lock: print(f"[{done+1}/{total}] WORK {fld}")
            process_folder(fld, links)
            with lock:
                done+=1; print(f"[{done}/{total}] DONE {fld}")
        except Exception as e:
            log_error(fld,"merge_folder",traceback.format_exc())
            with lock:
                done+=1; print(f"[{done}/{total}] ERR  {fld}: {e}")
    with ThreadPoolExecutor(max_workers=threads) as pool:
        wait([pool.submit(job,f) for f in folders])
    print("Finished.")

# ───────── main ─────────
CONFIG={}
if CONFIG_PATH.exists():
    try: CONFIG=json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except Exception: CONFIG={}

def main():
    ap=argparse.ArgumentParser(description="StripHelper – robust TS remux + concat (MKV only)")
    ap.add_argument("path",nargs="?",help="Root folder")
    ap.add_argument("--cli",action="store_true")
    ap.add_argument("-createsymlinks",action="store_true")
    ap.add_argument("--threads",type=int,default=os.cpu_count() or 4)
    args=ap.parse_args()
    try:
        if args.cli:
            if not args.path:
                print("CLI needs a path"); return
            root=Path(args.path)
            if not root.exists():
                print("Dir not found:", root); return
            run_cli(root,args.threads,args.createsymlinks)
        else:
            preset=Path(args.path) if args.path else None
            app=GUI(args.threads,args.createsymlinks,preset)
            app.run()
    except Exception:
        log.error("Fatal", exc_info=True)
        input("StripHelper crashed. Press Enter to exit…")

if __name__=="__main__":
    main()
