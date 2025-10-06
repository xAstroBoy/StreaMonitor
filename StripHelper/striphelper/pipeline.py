from __future__ import annotations
import os, json, time, shutil, subprocess, re
from pathlib import Path
import traceback
from typing import Callable

from .config import *
from .utils import WorkerFail, natkey, human_bytes, human_secs
from .ffmpeg import (
    run_ffmpeg, ffprobe_ok, ffprobe_best_duration, probe_signature,
    encoder_argsets_ordered, FFMPEG
)
from .validators import validate_against_source, validate_merge

Note   = Callable[[str], None]
Metric = Callable[[str,int,int,float,float,str], None]
Prog   = Callable[[float,float,int,int], None]


# ─────────────────────────────────────────────────────────────────────────────
# Hardware encoder discovery (fast ladder)
# ─────────────────────────────────────────────────────────────────────────────

_ENCODER_CACHE: dict[str, bool] = {}
_AUDIO_AAC_MF_AVAILABLE: bool | None = None

def _run_ffmpeg_list(kind: str) -> str:
    """
    kind: 'encoders' or 'codecs'
    Returns stdout (text) or '' on failure.
    """
    try:
        out = subprocess.run(
            [FFMPEG, "-hide_banner", f"-{kind}"],
            capture_output=True, text=True
        )
        return out.stdout if out.returncode == 0 else ""
    except Exception:
        return ""

def detect_ffmpeg_encoders() -> dict[str, bool]:
    """
    Cache & return encoder availability map like:
      {'h264_nvenc': True, 'hevc_nvenc': False, 'h264_qsv': True, ...}
    """
    global _ENCODER_CACHE, _AUDIO_AAC_MF_AVAILABLE
    if _ENCODER_CACHE:
        return _ENCODER_CACHE

    text = _run_ffmpeg_list("encoders")
    have: dict[str, bool] = {}

    def has(token: str) -> bool:
        return bool(re.search(rf"^\S+\s+{re.escape(token)}\b", text, re.MULTILINE))

    have["h264_nvenc"] = has("h264_nvenc")
    have["hevc_nvenc"] = has("hevc_nvenc")
    have["av1_nvenc"]  = has("av1_nvenc")
    have["h264_qsv"]   = has("h264_qsv")
    have["hevc_qsv"]   = has("hevc_qsv")
    have["av1_qsv"]    = has("av1_qsv")
    have["h264_amf"]   = has("h264_amf")
    have["hevc_amf"]   = has("hevc_amf")
    have["av1_amf"]    = has("av1_amf")
    have["libx264"]    = has("libx264")
    have["libx265"]    = has("libx265")

    _AUDIO_AAC_MF_AVAILABLE = has("aac_mf")

    _ENCODER_CACHE = have
    return have

def _aac_encoder_name() -> str:
    global _AUDIO_AAC_MF_AVAILABLE
    if _AUDIO_AAC_MF_AVAILABLE is None:
        detect_ffmpeg_encoders()
    return "aac_mf" if _AUDIO_AAC_MF_AVAILABLE else "aac"

def encoder_argsets_ordered_fast() -> list[tuple[list[str], str]]:
    """
    Hardware-first encoder ladders with fast, sane defaults for salvage/concat.
    """
    have = detect_ffmpeg_encoders()
    out: list[tuple[list[str], str]] = []

    if have["hevc_nvenc"]:
        out.append((
            ["-c:v","hevc_nvenc", "-preset","p5", "-rc","vbr", "-cq","19", "-bf","0"],
            "hevc_nvenc_p5_cq19"
        ))
    if have["h264_nvenc"]:
        out.append((
            ["-c:v","h264_nvenc", "-preset","p5", "-rc","vbr", "-cq","19", "-bf","0"],
            "h264_nvenc_p5_cq19"
        ))
    if have["av1_nvenc"]:
        out.append((
            ["-c:v","av1_nvenc", "-preset","p5", "-rc","vbr", "-cq","23", "-bf","0"],
            "av1_nvenc_p5_cq23"
        ))

    if have["hevc_qsv"]:
        out.append((
            ["-c:v","hevc_qsv", "-global_quality","21", "-look_ahead","0"],
            "hevc_qsv_gq21"
        ))
    if have["h264_qsv"]:
        out.append((
            ["-c:v","h264_qsv", "-global_quality","21", "-look_ahead","0"],
            "h264_qsv_gq21"
        ))
    if have["av1_qsv"]:
        out.append((
            ["-c:v","av1_qsv", "-global_quality","27"],
            "av1_qsv_gq27"
        ))

    if have["hevc_amf"]:
        out.append((
            ["-c:v","hevc_amf", "-quality","speed", "-usage","transcoding"],
            "hevc_amf_speed"
        ))
    if have["h264_amf"]:
        out.append((
            ["-c:v","h264_amf", "-quality","speed", "-usage","transcoding"],
            "h264_amf_speed"
        ))
    if have["av1_amf"]:
        out.append((
            ["-c:v","av1_amf", "-quality","speed", "-usage","transcoding"],
            "av1_amf_speed"
        ))

    if have["libx264"]:
        out.append((
            ["-c:v","libx264", "-preset","veryfast", "-crf","20", "-tune","fastdecode"],
            "libx264_vfast_crf20"
        ))
    if have["libx265"]:
        out.append((
            ["-c:v","libx265", "-preset","fast", "-x265-params","crf=22:pmode=1:pme=1"],
            "libx265_fast_crf22"
        ))

    return out




def purge_temp(folder: Path):
    for f in ("concat_list.txt","concat_list_norm.txt","0~merge.mp4","0~merge.mkv",".stage.mkv",".merge2.mkv"):
        (folder/f).unlink(missing_ok=True)
    for f in Path(folder).glob("*.norm.mkv"):
        f.unlink(missing_ok=True)


# ─────────────────────────────────────────────────────────────────────────────
# Symlink helper (kept simple & robust)
# ─────────────────────────────────────────────────────────────────────────────

def make_symlink(folder: Path, cfg: dict):
    """
    Recreate the original StripHelper layout:
      TO_PROCESS / <ModelNameAfterAliases> / <VR | NO VR | UNKNOWN> / <relative path to model root> / 0.mkv
    """
    try:
        src = folder / "0.mkv"
        if not src.exists():
            return

        model_root = folder
        for q in (folder, *folder.parents):
            if "[" in q.name and "]" in q.name:
                model_root = q
                break

        def _norm_tokens(xlist):
            return [t.lower() for t in (xlist or [])]

        vr_tokens = _norm_tokens(cfg.get("VR", []))
        dt_tokens = _norm_tokens(cfg.get("Desktop", []))
        aliases   = cfg.get("Aliases", {}) or {}

        def ftype(name: str) -> str:
            n = name.lower()
            if any(t in n for t in vr_tokens): return "VR"
            if any(t in n for t in dt_tokens): return "NO VR"
            return "UNKNOWN"

        def clean(name: str) -> str:
            out = name
            for t in (cfg.get("VR", []) or []) + (cfg.get("Desktop", []) or []):
                if t:
                    out = out.replace(t, "").strip()
            for canonical, als in aliases.items():
                for a in (als or []):
                    out = out.replace(a, canonical)
            return out.strip()

        def getsite(name: str) -> str:
            if "[" not in name or "]" not in name:
                return ""
            start = name.index("[") + 1
            end   = name.index("]", start)
            return name[start:end].strip()

        tag   = ftype(model_root.name)
        model = clean(model_root.name) or model_root.name
        rel   = folder.relative_to(model_root)

        dst_dir = TO_PROCESS / model / tag / rel
        dst_dir.mkdir(parents=True, exist_ok=True)

        site = getsite(model_root.name)
        base = src.name
        attempt = 0

        while True:
            dst = dst_dir / base
            if not dst.exists():
                try:
                    os.symlink(src, dst)
                    return
                except (FileExistsError, FileNotFoundError):
                    return
                except OSError:
                    try:
                        shutil.copy2(src, dst)
                        return
                    except Exception:
                        pass

            attempt += 1
            site_tag = f"_[{site}]" if site else "_DUP"
            if attempt == 1:
                base = f"0{site_tag}{src.suffix}"
            else:
                base = f"0{site_tag}_{attempt}{src.suffix}"

    except Exception:
        try:
            print(folder, "symlink", traceback.format_exc())
        except Exception:
            pass

# ─────────────────────────────────────────────────────────────────────────────
# Streamcopy post-process (TS→MKV and also merged MKV→MKV)
# ─────────────────────────────────────────────────────────────────────────────

def ts_streamcopy_postprocess(
    folder: Path,
    src_ts: Path,
    out_mkv: Path,
    note_cb: Note | None,
    progress_cb: Prog | None = None,
):
    note_cb and note_cb(f"ts-postprocess {src_ts.name} → {out_mkv.name}")

    try:
        src_size = src_ts.stat().st_size
    except Exception:
        src_size = 0

    def _prog(out_sec: float, written: int, target_bytes: int):
        if not progress_cb:
            return
        tgt = target_bytes or src_size or 1
        w   = max(written or 0, 0)
        pct = 100.0 * min(w, tgt) / float(tgt)
        progress_cb(pct, 0.0, w, tgt)

    cmd = [
        FFMPEG, "-y",
        "-hide_banner", "-loglevel", "error",
        "-fflags", "+genpts+discardcorrupt",
        "-analyzeduration", "2147483647", "-probesize", "2147483647",
        "-err_detect", "+ignore_err",
        "-i", src_ts.name,
        "-map", "0",
        "-c", "copy", "-copyinkf",
        "-avoid_negative_ts", "make_zero",
        "-reset_timestamps", "1",
        "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0",
        out_mkv.name
    ]
    run_ffmpeg(cmd, folder, f"ts streamcopy [{src_ts.name} → {out_mkv.name}]",
               _prog, src_size, note_cb)

def mkv_streamcopy_postprocess(
    folder: Path,
    src_mkv: Path,
    out_mkv: Path,
    note_cb: Note | None,
    progress_cb: Prog | None = None,
):
    note_cb and note_cb("merge postprocess streamcopy")

    try:
        src_size = src_mkv.stat().st_size
    except Exception:
        src_size = 0

    def _prog(out_sec: float, written: int, target_bytes: int):
        if not progress_cb:
            return
        tgt = target_bytes or src_size or 1
        w   = max(written or 0, 0)
        pct = 100.0 * min(w, tgt) / float(tgt)
        progress_cb(pct, 0.0, w, tgt)

    cmd = [
        FFMPEG, "-y",
        "-hide_banner", "-loglevel", "error",
        "-fflags", "+genpts+discardcorrupt",
        "-analyzeduration", "2147483647", "-probesize", "2147483647",
        "-err_detect", "+ignore_err",
        "-i", src_mkv.name,
        "-map", "0", "-c", "copy", "-copyinkf",
        "-avoid_negative_ts", "make_zero",
        "-reset_timestamps", "1",
        "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0",
        out_mkv.name
    ]
    run_ffmpeg(cmd, folder, f"merge postprocess [{src_mkv.name} → {out_mkv.name}]",
               _prog, src_size, note_cb)



# ─────────────────────────────────────────────────────────────────────────────
# Copy-remux ladder (no re-encode)
# ─────────────────────────────────────────────────────────────────────────────

def build_pass_cmd(mode: str, in_name: str, out_name: str):
    common_in = ["-hide_banner", "-loglevel", "error",
                 "-err_detect","+ignore_err","-analyzeduration","2147483647","-probesize","2147483647"]
    if mode == "A_default":
        return [FFMPEG,"-y"]+common_in+[
            "-fflags","+genpts+igndts+discardcorrupt",
            "-use_wallclock_as_timestamps","1","-copytb","0",
            "-i",in_name,
            "-map","0:v:0?","-map","0:a?","-dn","-sn",
            "-c","copy","-copyinkf",
            "-avoid_negative_ts","make_zero","-reset_timestamps","1",
            "-muxpreload","0","-muxdelay","0","-max_interleave_delta","0",
            out_name]
    if mode == "B_nowallclock":
        return [FFMPEG,"-y"]+common_in+[
            "-fflags","+genpts+igndts+discardcorrupt",
            "-i",in_name,
            "-map","0:v:0?","-map","0:a?","-dn","-sn",
            "-c","copy","-copyinkf",
            "-avoid_negative_ts","make_zero",
            "-muxpreload","0","-muxdelay","0","-max_interleave_delta","0",
            out_name]
    if mode == "C_copyts":
        return [FFMPEG,"-y"]+common_in+[
            "-fflags","+discardcorrupt","-copyts","-start_at_zero",
            "-i",in_name,
            "-map","0:v:0?","-map","0:a?","-dn","-sn",
            "-c","copy","-copyinkf",
            "-muxpreload","0","-muxdelay","0","-max_interleave_delta","0",
            out_name]
    raise ValueError("Unknown pass mode")


def run_passes_with_validate(
    folder: Path,
    src: Path,
    dst: Path,
    is_ts: bool,
    note_cb: Note | None,
    metric_cb: Metric | None,
    stage_prefix: str = "remux",
    progress_cb: Prog | None = None,
):
    modes = ["A_default", "B_nowallclock", "C_copyts"]
    last_metrics = {}

    src_dur  = max(ffprobe_best_duration(src, folder), 0.0)
    try:
        src_size = src.stat().st_size
    except Exception:
        src_size = 0

    def _prog(out_sec: float, written: int, target_bytes: int):
        if progress_cb:
            tgt = target_bytes or src_size or 1
            pct = 100.0 * min(max(written, 0), tgt) / float(tgt)
            progress_cb(pct, 0.0, written or 0, tgt)

    for i, mode in enumerate(modes, start=1):
        note_cb and note_cb(f"{stage_prefix} {src.name} → MKV pass{i} [{mode}]")
        cmd = build_pass_cmd(mode, src.name, dst.name)

        try:
            run_ffmpeg(cmd, folder,  f"{stage_prefix} {src.name} pass{i} [{mode}]", _prog, src_size, note_cb)
        except WorkerFail:
            pass

        ok, msg, met = validate_against_source(
            folder, src, dst,
            is_ts_to_mux=is_ts,
            ignore_size=is_ts
        )
        last_metrics = met or {}

        if metric_cb and met:
            add = met.get("accept_reason", "")
            metric_cb(
                f"{stage_prefix} pass{i}-validate [{mode}]",
                met.get("src_size", 0), met.get("out_size", 0),
                met.get("src_dur", 0.0), met.get("out_dur", 0.0),
                (f"src(probe={human_secs(met.get('src_probe',0.0))},pkt={human_secs(met.get('src_pkt',0.0))}) | "
                 f"out(probe={human_secs(met.get('out_probe',0.0))},pkt={human_secs(met.get('out_pkt',0.0))}) | "
                 f"size_ok={met.get('size_ok')} dur_ok={met.get('dur_ok')} Δtail={human_secs(met.get('tail_delta',0.0))} "
                 f"(≤ {human_secs(met.get('tail_allowed',0.0))})" + (f" | {add}" if add else ""))
            )

        if not ok and is_ts and (last_metrics.get("dur_ok") or 0):
            if metric_cb:
                metric_cb(
                    f"{stage_prefix} pass{i}-validate [TS accept]",
                    last_metrics.get("src_size",0), last_metrics.get("out_size",0),
                    last_metrics.get("src_dur",0.0), last_metrics.get("out_dur",0.0),
                    "dur_ok=True; size ignored for TS→MKV"
                )
            note_cb and note_cb(f"{stage_prefix} pass{i} OK [TS dur match; size ignored]")
            return True, last_metrics

        if ok:
            note_cb and note_cb(f"{stage_prefix} pass{i} OK" + (f" [{met.get('accept_reason')}]" if met and met.get('accept_reason') else ""))
            return True, met

        try:
            (folder / dst.name).unlink(missing_ok=True)
        except Exception:
            pass

    return False, last_metrics


# ─────────────────────────────────────────────────────────────────────────────
# Salvage re-encode with hard PTS rebuild
# ─────────────────────────────────────────────────────────────────────────────

def _pts_rebuild_filters(sig: dict) -> tuple[list[str], list[str]]:
    full_range_fix = (sig.get("v_pix","") or "").startswith("yuvj")
    vfix = ["setpts=PTS-STARTPTS"]
    if full_range_fix:
        vfix.append("scale=in_range=full:out_range=tv")
    vfix.append("format=yuv420p")
    return ["-vf", ",".join(vfix)], ["-af", "asetpts=PTS-STARTPTS,aresample=async=1:min_comp=0.001:first_pts=0"]



def salvage_reencode(
    folder: Path,
    src: Path,
    dst: Path,
    is_ts: bool,
    note_cb: Note | None,
    metric_cb: Metric | None,
    stage_prefix: str = "salvage",
    progress_cb: Prog | None = None,
):
    sig = probe_signature(src, folder)
    fps = max(int(round(sig.get("v_fps",0.0))),0) or DEFAULT_TARGET_FPS
    have_audio = bool(sig.get("has_a"))
    vfix, afix = _pts_rebuild_filters(sig)
    errs: list[str] = []

    src_dur = max(ffprobe_best_duration(src, folder),0.0)
    try: src_size = src.stat().st_size
    except Exception: src_size = 0

    def _prog(out_sec: float, written: int, tgt: int):
        if progress_cb:
            progress_cb(0.0, 0.0, written or 0, tgt or src_size or 1)

    for enc_args, enc_label in encoder_argsets_ordered_fast():
        try:
            note_cb and note_cb(f"{stage_prefix} reencode [{enc_label}] ~{fps}fps (hard PTS rebuild)")
            cmd=[FFMPEG,"-y","-hide_banner","-loglevel","error",
                 "-fflags","+genpts+discardcorrupt","-err_detect","+ignore_err",
                 "-analyzeduration","2147483647","-probesize","2147483647","-i",src.name]

            if have_audio:
                cmd+=["-map","0:v:0?","-map","0:a:0?"]
            else:
                dur=src_dur or 0.1
                cmd+=["-f","lavfi","-t",f"{dur:.3f}","-i",f"anullsrc=r={TARGET_AUDIO_SR}:cl=stereo",
                      "-map","0:v:0?","-map","1:a:0"]

            cmd+=vfix+afix+["-vsync","cfr","-r",str(fps)]
            cmd+=enc_args+["-c:a",_aac_encoder_name(),"-ar",str(TARGET_AUDIO_SR),"-ac",str(TARGET_AUDIO_CH)]
            cmd+=["-avoid_negative_ts","make_zero","-reset_timestamps","1",
                  "-muxpreload","0","-muxdelay","0","-max_interleave_delta","0",dst.name]

            run_ffmpeg(cmd, folder, f"{stage_prefix} -> [{enc_label}]", _prog, src_size, note_cb)

            ok,msg,met=validate_against_source(folder,src,dst,is_ts_to_mux=is_ts,ignore_size=SALVAGE_IGNORE_SIZE)
            if ok:
                note_cb and note_cb(f"{stage_prefix} OK [{enc_label}]")
                return True
            try: (folder/dst.name).unlink(missing_ok=True)
            except Exception: pass
            errs.append(f"[{enc_label}] validate failed: {msg}")

        except WorkerFail as ex:
            try: (folder/dst.name).unlink(missing_ok=True)
            except Exception: pass
            errs.append(f"[{enc_label}] ffmpeg: {ex.message[:500]}")

    try:
        note_cb and note_cb(f"{stage_prefix} fallback reencode [libx264_safe]")
        cmd=[FFMPEG,"-y","-i",src.name,
             "-c:v","libx264","-preset","superfast","-crf","23",
             "-c:a","aac","-ar",str(TARGET_AUDIO_SR),"-ac",str(TARGET_AUDIO_CH),
             dst.name]
        run_ffmpeg(cmd,folder,f"{stage_prefix} -> [libx264_safe]",_prog,src_size,note_cb)
        return True
    except Exception as ex:
        errs.append(f"[libx264_safe] failed: {ex}")
        raise WorkerFail(folder,f"{stage_prefix}-reencode","All encoders failed:\n"+"\n".join(errs))



# ─────────────────────────────────────────────────────────────────────────────
# Concat helpers
# ─────────────────────────────────────────────────────────────────────────────

def write_concat(folder: Path, parts, include_zero: bool, fname="concat_list.txt")->Path:
    txt=folder/fname
    lines=[]
    if include_zero: lines.append(f"file '0.mkv'")
    for p in parts:
        if p.name=="0.mkv": continue
        safe = p.name.replace("'", r"'\''")
        lines.append(f"file '{safe}'")
    txt.write_text("\n".join(lines),encoding="utf-8")
    return txt


def concat_copy_mkv(folder: Path, concat_txt: Path, progress_cb=None, target_bytes=0, note_cb=None, stage_label="concat copy -> MKV"):
    tmp=folder/"0~merge.mkv"
    cmd=[FFMPEG,"-y","-hide_banner","-loglevel","error",
         "-f","concat","-safe","0","-i",concat_txt.name,
         "-c","copy","-map","0","-copyinkf",
         "-avoid_negative_ts","make_zero","-reset_timestamps","1",
         "-muxpreload","0","-muxdelay","0","-max_interleave_delta","0",
         tmp.name]
    run_ffmpeg(cmd, folder, stage_label, progress_cb, target_bytes, note_cb)


def normalize_parts(parts, folder: Path, note_cb=None):
    norm=[]
    for p in parts:
        sig=probe_signature(p, folder)
        out=p.with_suffix(".norm.mkv")
        note_cb and note_cb(f"normalize {p.name} → AAC stereo{' + silence' if not sig.get('has_a') else ''}")
        if sig.get("has_a"):
            cmd=[FFMPEG,"-y","-hide_banner","-loglevel","error","-i",p.name,
                 "-map","0:v:0","-map","0:a:0","-dn","-sn",
                 "-c:v","copy","-c:a",_aac_encoder_name(),"-ar",str(TARGET_AUDIO_SR),"-ac",str(TARGET_AUDIO_CH),
                 "-avoid_negative_ts","make_zero","-reset_timestamps","1",
                 "-muxpreload","0","-muxdelay","0","-max_interleave_delta","0",
                 out.name]
        else:
            dur = max(ffprobe_best_duration(p, folder), 0.1)
            cmd=[FFMPEG,"-y","-hide_banner","-loglevel","error","-i",p.name,
                 "-f","lavfi","-t",f"{dur:.3f}","-i",f"anullsrc=r={TARGET_AUDIO_SR}:cl=stereo",
                 "-map","0:v:0","-map","1:a:0","-dn","-sn",
                 "-c:v","copy","-c:a",_aac_encoder_name(),
                 "-avoid_negative_ts","make_zero","-reset_timestamps","1",
                 "-muxpreload","0","-muxdelay","0","-max_interleave_delta","0",
                 out.name]
        run_ffmpeg(cmd, folder, f"normalize {p.name}", None, 0, note_cb)
        norm.append(out)
    return norm


def choose_target_geometry(parts: list[Path], folder: Path) -> tuple[int,int,int]:
    max_w=max_h=0; fps_vals=[]
    for p in parts:
        sig=probe_signature(p, folder)
        w,h = sig.get("v_w",0), sig.get("v_h",0)
        fps = sig.get("v_fps",0.0)
        max_w=max(max_w,w); max_h=max(max_h,h)
        if fps>0: fps_vals.append(fps)
    target_fps = int(round(max(fps_vals) if fps_vals else DEFAULT_TARGET_FPS)) or DEFAULT_TARGET_FPS
    if CAP_MAX_WIDTH and max_w>CAP_MAX_WIDTH:
        scale = CAP_MAX_WIDTH/max_w; max_w = CAP_MAX_WIDTH; max_h = int(max_h*scale)
    if CAP_MAX_HEIGHT and max_h>CAP_MAX_HEIGHT:
        scale = CAP_MAX_HEIGHT/max_h; max_h = CAP_MAX_HEIGHT; max_w = int(max_w*scale)
    max_w = max_w or 640; max_h = max_h or 360
    if max_w % 2: max_w -= 1
    if max_h % 2: max_h -= 1
    return max_w, max_h, target_fps


def concat_reencode_filter(parts, folder: Path, note_cb=None, progress_cb=None):
    w,h,fps = choose_target_geometry(parts, folder)
    n=len(parts)
    vchains=[]; achains=[]
    for i in range(n):
        vchains.append(
            f"[{i}:v:0]setpts=PTS-STARTPTS,fps={fps},"
            f"scale=w=min(iw,{w}):h=min(ih,{h}):force_original_aspect_ratio=decrease:flags=bicubic,"
            f"setsar=1,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:color=black,format=yuv420p[v{i}];"
        )
        achains.append(f"[{i}:a:0]asetpts=PTS-STARTPTS,aresample=async=1:first_pts=0[a{i}];")
    v_inputs="".join(f"[v{i}]" for i in range(n)); a_inputs="".join(f"[a{i}]" for i in range(n))
    filt="".join(vchains+achains)+f"{v_inputs}{a_inputs}concat=n={n}:v=1:a=1[v][a]"

    errs=[]
    for enc_args, enc_label in encoder_argsets_ordered_fast():
        try:
            note_cb and note_cb(f"concat reencode [{enc_label}] {w}x{h}@{fps}fps (PTS rebuild)")
            tmp=folder/"0~merge.mkv"
            cmd=[FFMPEG,"-y"]; [cmd.extend(["-i", p.name]) for p in parts]
            cmd += ["-filter_complex", filt, "-map","[v]","-map","[a]"] + enc_args + \
                   ["-vsync","cfr","-r", str(fps),
                    "-pix_fmt","yuv420p","-c:a", _aac_encoder_name(), "-b:a","160k",
                    "-avoid_negative_ts","make_zero","-reset_timestamps","1",
                    "-muxpreload","0","-muxdelay","0","-max_interleave_delta","0",
                    tmp.name]
            run_ffmpeg(cmd, folder, f"concat reencode -> MKV [{enc_label}]", progress_cb, 0, note_cb)
            return True
        except WorkerFail as e:
            errs.append(f"[{enc_label}] {e.message[:1200]}"); continue
    raise WorkerFail(folder,"concat-reencode","All encoders failed:\n"+ "\n\n".join(errs))


# ─────────────────────────────────────────────────────────────────────────────
# Work discovery
# ─────────────────────────────────────────────────────────────────────────────

def find_work_folders(root: Path) -> list[Path]:
    def dir_has_media_or_0(d: Path) -> bool:
        try:
            has_tmp = any(d.glob("*.tmp.ts")) or any(d.glob("*.tmp.mp4"))
            has_media = any(p.is_file() and p.suffix.lower() in MEDIA_EXTS and p.name != "0.mkv" for p in d.iterdir())
            has_zero  = (d / "0.mkv").exists()
            return has_tmp or has_media or has_zero
        except Exception:
            return False

    folders = []
    if dir_has_media_or_0(root): folders.append(root)
    for dp,_,_ in os.walk(root):
        d = Path(dp)
        if d == root: continue
        if dir_has_media_or_0(d): folders.append(d)
    seen=set(); out=[]
    for f in folders:
        if f not in seen:
            seen.add(f); out.append(f)
    return out


# ─────────────────────────────────────────────────────────────────────────────
# Main pipeline
# ─────────────────────────────────────────────────────────────────────────────

def _should_salvage_from_metrics(met: dict | None, is_ts: bool) -> bool:
    if not met:
        return False
    tail_delta   = float(met.get("tail_delta", 0.0) or 0.0)
    tail_allowed = float(met.get("tail_allowed", TAIL_TOL_SEC_TS2MUX if is_ts else TAIL_TOL_SEC_GENERIC))
    return tail_delta > tail_allowed

def merge_folder(folder: Path, gui_cb: Prog|None=None, cfg=None, mk_links=False, note_cb: Note|None=None, metric_cb: Metric|None=None):
    purge_temp(folder)

    for patt in ("*.tmp.ts","*.tmp.mp4"):
        for t in folder.glob(patt):
            note_cb and note_cb(f"convert {t.name}")
            clean = t.with_name(t.stem[:-4] + ".mkv")
            if t.suffix.lower() == ".ts":
                ts_streamcopy_postprocess(folder, t, clean, note_cb, progress_cb=gui_cb)
                ok, _msg, met = validate_against_source(folder, t, clean, True, ignore_size=True)
                if not ok:
                    ok2, met2 = run_passes_with_validate(folder, t, clean, True, note_cb, metric_cb, stage_prefix="convert", progress_cb=gui_cb)
                    if not ok2:
                        src_dur = (met2 or {}).get("src_dur", ffprobe_best_duration(t, folder))
                        out_dur = (met2 or {}).get("out_dur", ffprobe_best_duration(clean, folder) if clean.exists() else 0.0)
                        need_salvage = (
                            (src_dur>0 and (src_dur - out_dur) >= SALVAGE_MIN_DELTA_SEC and out_dur < src_dur*SALVAGE_TRIG_RATIO) or
                            _should_salvage_from_metrics(met2, True)
                        )
                        if need_salvage:
                            note_cb and note_cb("convert short → salvage reencode")
                            clean.unlink(missing_ok=True)
                            salvage_reencode(folder, t, clean, True, note_cb, metric_cb, stage_prefix="convert-salvage", progress_cb=gui_cb)
                        else:
                            raise WorkerFail(folder,"convert-validate","copy-remux failed and salvage not triggered")
            else:
                ok, met = run_passes_with_validate(folder, t, clean, t.suffix.lower()==".ts", note_cb, metric_cb, stage_prefix="convert", progress_cb=gui_cb)
                if not ok:
                    src_dur = (met or {}).get("src_dur", ffprobe_best_duration(t, folder))
                    out_dur = (met or {}).get("out_dur", ffprobe_best_duration(clean, folder) if clean.exists() else 0.0)
                    need_salvage = (
                        (src_dur>0 and (src_dur - out_dur) >= SALVAGE_MIN_DELTA_SEC and out_dur < src_dur*SALVAGE_TRIG_RATIO) or
                        _should_salvage_from_metrics(met, t.suffix.lower()==".ts")
                    )
                    if need_salvage:
                        note_cb and note_cb("convert short → salvage reencode")
                        clean.unlink(missing_ok=True)
                        salvage_reencode(folder, t, clean, t.suffix.lower()==".ts", note_cb, metric_cb, stage_prefix="convert-salvage", progress_cb=gui_cb)
                    else:
                        raise WorkerFail(folder,"convert-validate","copy-remux failed and salvage not triggered")
            t.unlink(missing_ok=True)

    merged_mkv = folder/"0.mkv"
    (folder/"0.mp4").unlink(missing_ok=True)
    media=[p for p in folder.iterdir() if p.is_file() and p.suffix.lower() in MEDIA_EXTS and p.name!="0.mkv"]

    good=[]
    for f in sorted(media,key=lambda x:natkey(x.name)):
        if ffprobe_ok(f,folder): good.append(f)
        else:
            note_cb and note_cb(f"drop unreadable {f.name}")
            f.unlink(missing_ok=True)


    if not merged_mkv.exists() and not good:
        note_cb and note_cb("nothing to do")
        if gui_cb: gui_cb(100.0, 0.0, 0, 0)
        return

    if merged_mkv.exists() and not good:
        note_cb and note_cb("only symlink pass")
        if mk_links: make_symlink(folder, cfg or {})
        if gui_cb:
            sz = merged_mkv.stat().st_size
            gui_cb(100.0, 0.0, sz, sz)
        return

    if not merged_mkv.exists() and len(good)==1:
        only=good[0]
        if only.suffix.lower()==".ts":
            rem = only.with_suffix(".remux.mkv")
            ts_streamcopy_postprocess(folder, only, rem, note_cb, progress_cb=gui_cb)
            ok, _msg, met = validate_against_source(folder, only, rem, True, ignore_size=True)
            if not ok:
                ok2, met2 = run_passes_with_validate(folder, only, rem, True, note_cb, metric_cb, stage_prefix="remux", progress_cb=gui_cb)
                if not ok2:
                    src_dur = (met2 or {}).get("src_dur", ffprobe_best_duration(only, folder))
                    out_dur = (met2 or {}).get("out_dur", ffprobe_best_duration(rem, folder) if rem.exists() else 0.0)
                    need_salvage = (
                        (src_dur>0 and (src_dur - out_dur) >= SALVAGE_MIN_DELTA_SEC and out_dur < src_dur*SALVAGE_TRIG_RATIO) or
                        _should_salvage_from_metrics(met2, True)
                    )
                    if need_salvage:
                        note_cb and note_cb("single remux short → salvage")
                        rem.unlink(missing_ok=True)
                        salvage_reencode(folder, only, rem, True, note_cb, metric_cb, stage_prefix="remux-salvage", progress_cb=gui_cb)
                    else:
                        raise WorkerFail(folder,"single-remux-validate","copy-remux failed and salvage not triggered")
            os.replace(rem, merged_mkv)
            if mk_links: make_symlink(folder, cfg or {})
            if DELETE_TS_AFTER_REMUX: only.unlink(missing_ok=True)
            return
        else:
            if only.suffix.lower()==".mkv":
                os.replace(only, merged_mkv)
            else:
                cmd=[FFMPEG,"-y","-i",only.name,"-map","0","-c","copy","-copyinkf",
                     "-avoid_negative_ts","make_zero","-reset_timestamps","1",
                     "-muxpreload","0","-muxdelay","0","-max_interleave_delta","0",
                     "0.mkv"]
                run_ffmpeg(cmd, folder, "finalize → MKV", None, 0, note_cb)
                only.unlink(missing_ok=True)
            if mk_links: make_symlink(folder, cfg or {})
            return

    note_cb and note_cb(f"remux {sum(1 for x in good if x.suffix.lower()=='.ts')} ts parts → MKV")
    parts=[]
    for p in good:
        if p.suffix.lower()!=".ts":
            parts.append(p); continue

        rem=p.with_suffix(".remux.mkv")
        ts_streamcopy_postprocess(folder, p, rem, note_cb, progress_cb=gui_cb)
        ok, _msg, met = validate_against_source(folder, p, rem, True, ignore_size=True)
        if not ok:
            ok2, met2 = run_passes_with_validate(folder, p, rem, True, note_cb, metric_cb, stage_prefix="remux", progress_cb=gui_cb)
            if not ok2:
                src_dur = (met2 or {}).get("src_dur", ffprobe_best_duration(p, folder))
                out_dur = (met2 or {}).get("out_dur", ffprobe_best_duration(rem, folder) if rem.exists() else 0.0)
                need_salvage = (
                    (src_dur>0 and (src_dur - out_dur) >= SALVAGE_MIN_DELTA_SEC and out_dur < src_dur*SALVAGE_TRIG_RATIO) or
                    _should_salvage_from_metrics(met2, True)
                )
                if need_salvage:
                    note_cb and note_cb("remux short → salvage")
                    rem.unlink(missing_ok=True)
                    salvage_reencode(folder, p, rem, True, note_cb, metric_cb, stage_prefix="remux-salvage", progress_cb=gui_cb)
                else:
                    raise WorkerFail(folder,"remux-validate","copy-remux failed and salvage not triggered")

        if DELETE_TS_AFTER_REMUX: p.unlink(missing_ok=True)
        parts.append(rem)

    if any(p.suffix.lower()==".ts" for p in parts):
        raise WorkerFail(folder,"remux_ts","ts remained after remux – aborting")

    concat_txt = write_concat(folder, parts, include_zero=merged_mkv.exists(), fname="concat_list.txt")
    note_cb and note_cb(f"concat list written ({len(parts)} parts) → MKV")

    total_dur = (ffprobe_best_duration(merged_mkv, folder) if merged_mkv.exists() else 0.0) + sum(ffprobe_best_duration(p, folder) for p in parts)
    target_bytes = (merged_mkv.stat().st_size if merged_mkv.exists() else 0) + sum(p.stat().st_size for p in parts)
    if gui_cb: gui_cb(0.0, 0.0, 0, target_bytes)

    start=time.time(); written_holder={"b":0}
    def prog(sec, bw, tgt):
        written_holder["b"]=bw or written_holder["b"]
        if not gui_cb: return
        pct = (sec/total_dur*100.0) if total_dur>0 else 0.0
        elapsed=time.time()-start; speed = sec/elapsed if elapsed>0 else 0; eta = (total_dur-sec)/speed if speed>0 else 0
        gui_cb(pct, eta, written_holder["b"], tgt)

    used_files_for_concat = []
    try:
        concat_copy_mkv(folder, concat_txt, progress_cb=prog, target_bytes=target_bytes, note_cb=note_cb)
        used_files_for_concat = parts.copy()
    except WorkerFail:
        note_cb and note_cb("concat copy failed → normalize parts")
        norm = normalize_parts(parts, folder, note_cb)
        concat_txt2 = write_concat(folder, norm, include_zero=merged_mkv.exists(), fname="concat_list_norm.txt")
        try:
            concat_copy_mkv(folder, concat_txt2, progress_cb=prog, target_bytes=target_bytes, note_cb=note_cb, stage_label="concat copy (normalized) -> MKV")
            used_files_for_concat = norm.copy() + parts.copy()
        except WorkerFail:
            concat_reencode_filter(norm, folder, note_cb=note_cb, progress_cb=prog)
            used_files_for_concat = norm.copy() + parts.copy()

    tmp = folder/"0~merge.mkv"
    note_cb and note_cb("validate merged output")
    ok,msg,met = validate_merge(folder, tmp, target_bytes, total_dur, is_ts_path=True)

    if not ok:
        tmp2 = folder/".merge2.mkv"
        try:
            mkv_streamcopy_postprocess(folder, tmp, tmp2, note_cb, progress_cb=gui_cb)
            ok2, msg2, met2 = validate_merge(folder, tmp2, target_bytes, total_dur, is_ts_path=True)
            if not ok2:
                ok3, msg3, met3 = validate_merge(folder, tmp2, target_bytes, total_dur, is_ts_path=True, ignore_size=True)
                if not ok3:
                    tmp.unlink(missing_ok=True); tmp2.unlink(missing_ok=True)
                    (folder/"concat_list.txt").unlink(missing_ok=True)
                    (folder/"concat_list_norm.txt").unlink(missing_ok=True)
                    raise WorkerFail(folder,"merge-validate",msg2 or msg3 or msg)
            os.replace(tmp2, tmp)
        except WorkerFail:
            for junk in ("0~merge.mkv", ".merge2.mkv", "concat_list.txt", "concat_list_norm.txt"):
                (folder/junk).unlink(missing_ok=True)
            raise

    note_cb and note_cb("commit merged MKV" + (f" [{met.get('accept_reason')}]" if met and met.get('accept_reason') else ""))

    if merged_mkv.exists():
        try:
            merged_mkv.unlink(missing_ok=True)
        except Exception:
            pass
    os.replace(tmp, merged_mkv)

    for junk in ("concat_list.txt", "concat_list_norm.txt", ".merge2.mkv"):
        (folder/junk).unlink(missing_ok=True)
    for p in folder.glob("*.norm.mkv"):
        p.unlink(missing_ok=True)

    for f in used_files_for_concat:
        try:
            if f.exists() and f.is_file() and f.name != "0.mkv":
                f.unlink(missing_ok=True)
        except Exception:
            pass

    if mk_links:
        make_symlink(folder, cfg or {})
