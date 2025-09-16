from __future__ import annotations
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, wait
from .pipeline import merge_folder, find_work_folders

def run_cli(root:Path, threads:int, links:bool):
    folders = find_work_folders(root)
    if not folders:
        print(f"[0/0] Nothing to do under {root}")
        return
    total=len(folders); done=0; failed=0
    def job(fld:Path):
        nonlocal done, failed
        try:
            print(f"[{done+1}/{total}] WORK {fld}", flush=True)
            merge_folder(fld, gui_cb=None, cfg=None, mk_links=links, note_cb=None, metric_cb=None)
            done+=1; print(f"[{done}/{total}] DONE {fld}", flush=True)
        except Exception as e:
            failed+=1; done+=1
            print(f"[{done}/{total}] FAIL {fld}: {e}", flush=True)
    with ThreadPoolExecutor(max_workers=threads) as pool:
        wait([pool.submit(job,f) for f in folders])
    print(f"Finished. Failed folders: {failed}/{total}.")
