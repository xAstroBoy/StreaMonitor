#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
StripHelper – TS→MP4 remux + concat to 0.mp4 (copy mode) with target size display.

2025‑07‑23

Changes vs previous drop:
• New GUI columns: Target, Remain.
• Target (= expected final bytes) shown immediately; Remain updates live.
• gui_cb now: (pct, eta, written, target)
• merge_folder precomputes target_bytes and calls gui_cb once before merge.
"""

import argparse, ctypes, json, logging, os, re, shutil, subprocess, sys, threading, time, traceback
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, wait
from subprocess import DEVNULL, PIPE, STARTUPINFO, STARTF_USESHOWWINDOW, CREATE_NO_WINDOW

# ───────── CONFIG ─────────
FFMPEG  = "ffmpeg"
FFPROBE = "ffprobe"

DELETE_TS_AFTER_REMUX = True

CONFIG_PATH = Path(r"F:\config.json")  # optional
TO_PROCESS  = Path(r"F:\To Process")   # optional

ERROR_LOG = Path(__file__).with_name("StripHelper_errors.log")
WIN_HIDE  = CREATE_NO_WINDOW if os.name == "nt" else 0
# ──────────────────────────

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
    with ERROR_LOG.open("a", encoding="utf-8") as fh:
        fh.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {folder}\n")
        fh.write(f"CMD : {stage}\n{err}\n{'-'*60}\n")

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

def ffprobe_ok(fp: Path, cwd: Path) -> bool:
    r = subprocess.run([FFPROBE, "-v", "error", "-i", fp.name],
                       cwd=cwd, stdout=DEVNULL, stderr=PIPE,
                       creationflags=WIN_HIDE, startupinfo=_si())
    if r.returncode:
        log_error(cwd, "ffprobe", r.stderr.decode(errors="ignore"))
    return r.returncode == 0

def ffprobe_duration(fp: Path, cwd: Path) -> float:
    try:
        out = subprocess.check_output([FFPROBE,"-v","error",
                                       "-show_entries","format=duration",
                                       "-of","default=nw=1:nk=1", fp.name],
                                      cwd=cwd, stderr=DEVNULL)
        return float(out.strip() or 0)
    except Exception:
        return 0.0

def purge_temp(folder: Path):
    for f in ("concat_list.txt","0~merge.mp4"):
        (folder/f).unlink(missing_ok=True)

def convert_tmp(tmp: Path, folder: Path):
    clean = tmp.with_name(tmp.stem[:-4] + ".mp4")
    cmd=[FFMPEG,"-y","-i",tmp.name,"-c","copy","-bsf:a","aac_adtstoasc",clean.name]
    r=subprocess.run(cmd,cwd=folder,stdout=DEVNULL,stderr=PIPE,
                     creationflags=WIN_HIDE,startupinfo=_si())
    if r.returncode:
        log_error(folder,"ffmpeg-tmp",r.stderr.decode(errors="ignore"))
    else:
        tmp.unlink(missing_ok=True)

def remux_ts(files, folder: Path):
    out=[]
    for p in files:
        if p.suffix.lower()!=".ts":
            out.append(p); continue
        rem=p.with_suffix(".remux.mp4")
        if rem.exists():
            out.append(rem)
            if DELETE_TS_AFTER_REMUX: p.unlink(missing_ok=True)
            continue
        cmd=[FFMPEG,"-y","-fflags","+genpts","-err_detect","ignore_err",
             "-i",p.name,"-c","copy","-movflags","+faststart",
             "-bsf:a","aac_adtstoasc",rem.name]
        r=subprocess.run(cmd,cwd=folder,stdout=DEVNULL,stderr=PIPE,
                         creationflags=WIN_HIDE,startupinfo=_si())
        if r.returncode:
            log_error(folder,"remux-ts",r.stderr.decode(errors="ignore"))
            out.append(p)  # keep original ts if failed
        else:
            if DELETE_TS_AFTER_REMUX: p.unlink(missing_ok=True)
            out.append(rem)
    return out

def write_concat(folder: Path, parts, include_zero: bool)->Path:
    txt=folder/"concat_list.txt"
    lines=[]
    if include_zero:
        lines.append(f"file '{quote_ff('0.mp4')}'")
    for p in parts:
        if p.name=="0.mp4": continue
        lines.append(f"file '{quote_ff(p.name)}'")
    txt.write_text("\n".join(lines),encoding="utf-8")
    return txt

def run_ffmpeg(cmd, cwd: Path, stage: str, progress_cb=None, target_bytes=0):
    if progress_cb:
        cmd = cmd[:1] + ["-progress","pipe:2","-nostats"] + cmd[1:]
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
        return False
    return True

def concat_copy(folder: Path, concat_txt: Path, progress_cb=None, target_bytes=0):
    tmp=folder/"0~merge.mp4"
    cmd=[FFMPEG,"-y","-f","concat","-safe","0","-i",concat_txt.name,
         "-c","copy","-movflags","+faststart","-bsf:a","aac_adtstoasc",tmp.name]
    return run_ffmpeg(cmd, folder, "merge-copy", progress_cb, target_bytes)

def make_symlink(folder: Path, cfg: dict):
    try:
        src=folder/"0.mp4"
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
        tag=ftype(model_root.name)
        model=clean(model_root.name)
        rel=folder.relative_to(model_root)
        dst_dir=TO_PROCESS/model/tag/rel
        dst_dir.mkdir(parents=True,exist_ok=True)
        base="0.mp4"; i=0
        while True:
            dst=dst_dir/base
            if not dst.exists():
                try:
                    os.symlink(src,dst)
                    return
                except FileExistsError:
                    pass
            i+=1; base=f"0_dup({i}).mp4"
    except Exception:
        log_error(folder,"symlink",traceback.format_exc())

def merge_folder(folder: Path, gui_cb=None, cfg=None, mk_links=False):
    purge_temp(folder)
    for t in folder.glob("*.tmp.mp4"):
        convert_tmp(t, folder)

    merged = folder/"0.mp4"
    media=[p for p in folder.iterdir()
           if p.is_file() and p.suffix.lower() in (".ts",".mp4") and p.name!="0.mp4"]

    good=[]
    for f in sorted(media,key=lambda x:natkey(x.name)):
        if ffprobe_ok(f,folder): good.append(f)
        else: f.unlink(missing_ok=True)

    if not merged.exists() and not good:
        return
    if merged.exists() and not good:
        if mk_links: make_symlink(folder, cfg or {})
        return

    if not merged.exists() and len(good)==1:
        only=good[0]
        if only.suffix.lower()==".ts":
            rem=remux_ts([only],folder)[0]
            os.replace(rem, merged)
        else:
            os.replace(only, merged)
        if mk_links: make_symlink(folder, cfg or {})
        return

    parts = remux_ts(good, folder)

    concat_txt = write_concat(folder, parts, include_zero=merged.exists())

    total_dur = 0.0
    target_bytes = 0
    if merged.exists():
        total_dur += ffprobe_duration(merged, folder)
        target_bytes += merged.stat().st_size
    for p in parts:
        total_dur += ffprobe_duration(p, folder)
        target_bytes += p.stat().st_size

    # prime GUI row with target
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

    ok = concat_copy(folder, concat_txt, progress_cb=prog, target_bytes=target_bytes)

    if ok:
        os.replace(folder/"0~merge.mp4", merged)
        concat_txt.unlink(missing_ok=True)
        for f in media: f.unlink(missing_ok=True)
        for r in folder.glob("*.remux.mp4"):
            if r.name!="0.mp4": r.unlink(missing_ok=True)
        if mk_links: make_symlink(folder, cfg or {})
    else:
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
        self.root.geometry("900x580"); self.root.resizable(False,False)

        self.path_var=StringVar(value=str(preset) if preset else "Click to choose root…")
        path_e=ttk.Entry(self.root,textvariable=self.path_var,justify="center",state="readonly")
        path_e.pack(pady=(8,4),fill="x",padx=8)
        path_e.bind("<Button-1>", self.pick_path)

        cols=("status","pct","eta","written","target","remain")
        self.tree=ttk.Treeview(self.root,columns=cols,show="tree headings")
        self.tree.heading("#0",text="Folder / Subfolder")
        self.tree.heading("status",text="Status")
        self.tree.heading("pct",text="%")
        self.tree.heading("eta",text="ETA")
        self.tree.heading("written",text="Written")
        self.tree.heading("target",text="Target")
        self.tree.heading("remain",text="Remain")
        self.tree.column("#0",width=360,anchor="w")
        self.tree.column("status",width=70,anchor="center")
        self.tree.column("pct",width=50,anchor="e")
        self.tree.column("eta",width=80,anchor="center")
        self.tree.column("written",width=90,anchor="e")
        self.tree.column("target",width=95,anchor="e")
        self.tree.column("remain",width=95,anchor="e")
        sb=Scrollbar(self.root,orient=VERTICAL,command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        sb.pack(side=RIGHT,fill=Y)
        self.tree.pack(fill=BOTH,expand=True,padx=8,pady=6)

        from tkinter import ttk
        self.pb=ttk.Progressbar(self.root,mode="determinate",length=870)
        self.pb.pack(pady=(4,2))
        self.lbl=ttk.Label(self.root,text="0/0")
        self.lbl.pack(pady=(0,8))

        self.nodes={}
        self.tbytes={}
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
        if p==root:
            nid=self.tree.insert("","end",text=p.name,values=("pending","","","","",""))
            self.tree.item(nid,open=True)
            self.nodes[key]=nid; return
        rel=p.relative_to(root)
        parent=""; cur=root
        for part in rel.parts:
            cur/=part; k=str(cur)
            if k not in self.nodes:
                nid=self.tree.insert(parent,"end",text=part,values=("pending","","","","",""))
                self.tree.item(nid,open=True)
                self.nodes[k]=nid
            parent=self.nodes[k]

    def set_row(self,p:Path,status=None,pct=None,eta=None,written=None,target=None):
        nid=self.nodes.get(str(p)); 
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
        if status: self.tree.item(nid,tags=(status,))

    def start_for_root(self, root:Path):
        if self.total>0: return
        folders=[root]+[Path(dp) for dp,_,_ in os.walk(root) if Path(dp)!=root]
        self.total=len(folders)
        self.pb["maximum"]=self.total
        self.lbl["text"]=f"0/{self.total}"
        for f in folders:
            self.add_node(f,root)

        cfg={}
        if CONFIG_PATH.exists():
            try: cfg=json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            except Exception: cfg={}

        def job(fld:Path):
            try:
                self.root.after(0,self.set_row,fld,"work",0,0,0,0)
                # wrapped gui_cb
                def gui_cb(pct,eta,bytes_w,target_b):
                    self.root.after(0,self.set_row,fld,"work",pct,eta,bytes_w,target_b)
                merge_folder(fld, gui_cb=gui_cb, cfg=cfg, mk_links=self.LINKS)
                self.root.after(0,self.set_row,fld,"done",100,0,0,0)
            except Exception:
                log_error(fld,"merge_folder",traceback.format_exc())
                self.root.after(0,self.set_row,fld,"err")
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
    merge_folder(folder, gui_cb=None, cfg=CONFIG, mk_links=links)
    if not any(folder.iterdir()):
        shutil.rmtree(folder, ignore_errors=True)

def run_cli(root:Path, threads:int, links:bool):
    folders=[root]+[Path(dp) for dp,_,_ in os.walk(root) if Path(dp)!=root]
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
    ap=argparse.ArgumentParser(description="StripHelper – TS remux + concat to 0.mp4")
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
