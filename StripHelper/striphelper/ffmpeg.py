from __future__ import annotations
import subprocess, logging, time, json
from pathlib import Path
from subprocess import DEVNULL, PIPE, STARTUPINFO, STARTF_USESHOWWINDOW, CREATE_NO_WINDOW
from .config import FFMPEG, FFPROBE, FORCE_ENCODER, PREFER_NVENC, PREFER_QSV, CPU_X264_PRESET, CPU_X264_CRF, NVENC_PRESET, NVENC_CQ, QSV_TARGET_BITRATE
from .utils import WorkerFail

log = logging.getLogger("striphelper.ffmpeg")
WIN_HIDE = CREATE_NO_WINDOW if Path("/").anchor != "/" and __name__ else 0  # simple flag for Windows

def _si():
    try:
        si = STARTUPINFO(); si.dwFlags |= STARTF_USESHOWWINDOW
        return si
    except Exception:
        return None

def run_ffmpeg(cmd: list[str], cwd: Path, stage: str, progress_cb=None, target_bytes=0, note_cb=None):
    if progress_cb:
        cmd = cmd[:1] + ["-progress","pipe:2","-nostats"] + cmd[1:]
    if note_cb:
        note_cb(stage)
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=DEVNULL, stderr=PIPE, universal_newlines=True,
                            creationflags=WIN_HIDE, startupinfo=_si())
    stderr_lines=[]; out_time=0.0; bytes_written=0
    if progress_cb:
        while True:
            line = proc.stderr.readline()
            if line:
                s=line.strip(); stderr_lines.append(s)
                if s.startswith("out_time_ms="):
                    try: out_time=int(s.split("=",1)[1])/1_000_000
                    except Exception: pass
                elif s.startswith("total_size="):
                    try: bytes_written=int(s.split("=",1)[1])
                    except Exception: pass
                elif s=="progress=end":
                    break
            elif proc.poll() is not None:
                break
            else:
                time.sleep(0.05)
            progress_cb(out_time, bytes_written, target_bytes)
    ret=proc.wait()
    try:
        tail = proc.stderr.read() if proc.stderr else ""
        if tail:
            stderr_lines.extend([x for x in tail.splitlines() if x.strip()])
    except Exception:
        pass
    err_text="\n".join(stderr_lines)
    if ret!=0:
        note_cb and note_cb(f"{stage} failed")
        raise WorkerFail(cwd, stage, err_text or f"ffmpeg exited {ret}")
    note_cb and note_cb(f"{stage} OK")
    return True

def _ffprobe_output(args, cwd: Path) -> str:
    try:
        out = subprocess.check_output(args, cwd=cwd, stderr=DEVNULL)
        return out.decode(errors="ignore")
    except Exception:
        return ""

def ffprobe_num(fp: Path, cwd: Path, args: list[str]) -> float:
    try:
        out = subprocess.check_output([FFPROBE, "-v", "error"] + args + [fp.name], cwd=cwd, stderr=DEVNULL)
        s = (out.decode(errors="ignore").strip() or "0").splitlines()[0]
        return float("0" if s in ("N/A","") else s)
    except Exception:
        return 0.0

def ffprobe_ok(fp: Path, cwd: Path) -> bool:
    try:
        r = subprocess.run([FFPROBE, "-v", "error", "-i", fp.name],
                        cwd=cwd, stdout=DEVNULL, stderr=PIPE, creationflags=WIN_HIDE, startupinfo=_si())
        return r.returncode == 0
    except Exception:
        return False

def ffprobe_format_duration(fp: Path, cwd: Path) -> float:
    return ffprobe_num(fp, cwd, ["-show_entries","format=duration","-of","default=nw=1:nk=1"])

def ffprobe_stream_duration(fp: Path, cwd: Path, selector: str) -> float:
    return ffprobe_num(fp, cwd, ["-select_streams", selector, "-show_entries","stream=duration","-of","default=nw=1:nk=1"])

def ffprobe_best_duration(fp: Path, cwd: Path) -> float:
    fmt = ffprobe_format_duration(fp, cwd) or 0.0
    vd  = ffprobe_stream_duration(fp, cwd, "v:0") or 0.0
    ad  = ffprobe_stream_duration(fp, cwd, "a:0") or 0.0
    return max(fmt, vd, ad)

def ffprobe_last_packet_pts(fp: Path, cwd: Path) -> float:
    try:
        v = _ffprobe_output([FFPROBE, "-v", "error", "-select_streams", "v:0",
                             "-show_packets", "-show_entries", "packet=pts_time",
                             "-of", "csv=p=0", "-read_intervals", "%+#9999999", fp.name], cwd)
        a = _ffprobe_output([FFPROBE, "-v", "error", "-select_streams", "a:0",
                             "-show_packets", "-show_entries", "packet=pts_time",
                             "-of", "csv=p=0", "-read_intervals", "%+#9999999", fp.name], cwd)
        vt = float(v.strip().splitlines()[-1]) if v.strip().splitlines() else 0.0
        at = float(a.strip().splitlines()[-1]) if a.strip().splitlines() else 0.0
        return max(vt, at)
    except Exception:
        return 0.0

def probe_json(fp: Path, cwd: Path) -> dict:
    txt = _ffprobe_output([FFPROBE, "-v", "error", "-show_streams", "-show_format", "-print_format", "json", fp.name], cwd)
    try:
        return json.loads(txt) if txt else {}
    except Exception:
        return {}

def parse_fps_ratio(s: str) -> float:
    if not s or s == "0/0":
        return 0.0
    if "/" in s:
        a, b = s.split("/", 1)
        try:
            a = float(a); b = float(b)
            return 0.0 if b == 0 else a / b
        except Exception:
            return 0.0
    try:
        return float(s)
    except Exception:
        return 0.0

def probe_signature(fp: Path, cwd: Path) -> dict:
    sig = {"has_v": False, "has_a": False,
           "v_codec":"", "v_w":0, "v_h":0, "v_pix":"", "v_fps":0.0,
           "a_codec":"", "a_sr":0, "a_ch":0, "a_layout":""}
    try:
        j = probe_json(fp, cwd)
        for s in j.get("streams", []):
            if s.get("codec_type") == "video" and not sig["has_v"]:
                sig["has_v"] = True
                sig["v_codec"] = s.get("codec_name", "")
                sig["v_w"] = int(s.get("width", 0) or 0)
                sig["v_h"] = int(s.get("height", 0) or 0)
                sig["v_pix"] = s.get("pix_fmt", "")
                fps = parse_fps_ratio(s.get("avg_frame_rate") or s.get("r_frame_rate") or "")
                sig["v_fps"] = fps
            elif s.get("codec_type") == "audio" and not sig["has_a"]:
                sig["has_a"] = True
                sig["a_codec"] = s.get("codec_name", "")
                sig["a_sr"] = int(s.get("sample_rate", "0") or 0)
                sig["a_ch"] = int(s.get("channels", 0) or 0)
                sig["a_layout"] = s.get("channel_layout", "")
    except Exception:
        pass
    return sig

# Encoder ordering
_ENCODERS_CACHE = None
def ffmpeg_has_encoder(name: str) -> bool:
    global _ENCODERS_CACHE
    if _ENCODERS_CACHE is None:
        try:
            out = subprocess.check_output([FFMPEG, "-hide_banner", "-encoders"], stderr=DEVNULL).decode(errors="ignore")
        except Exception:
            out = ""
        _ENCODERS_CACHE = out.lower()
    return (name.lower() in _ENCODERS_CACHE) if _ENCODERS_CACHE else False

def encoder_argsets_ordered() -> list[tuple[list[str], str]]:
    nvenc_ok = ffmpeg_has_encoder("h264_nvenc")
    qsv_ok   = ffmpeg_has_encoder("h264_qsv")
    nvenc = (["-hide_banner", "-loglevel", "error","-c:v","h264_nvenc","-preset",NVENC_PRESET,"-rc","vbr","-cq",str(NVENC_CQ)], "NVENC")
    qsv   = (["-hide_banner", "-loglevel", "error","-c:v","h264_qsv","-global_quality","24","-b:v",QSV_TARGET_BITRATE], "QSV")
    x264  = (["-hide_banner", "-loglevel", "error","-c:v","libx264","-preset",CPU_X264_PRESET,"-crf",str(CPU_X264_CRF)], "x264")
    opts: list[tuple[list[str],str]] = []
    def add_if(cond,p):
        if cond: opts.append(p)
    if FORCE_ENCODER == "nvenc":
        add_if(nvenc_ok, nvenc); add_if(qsv_ok, qsv); opts.append(x264); return opts
    if FORCE_ENCODER == "qsv":
        add_if(qsv_ok, qsv); add_if(nvenc_ok, nvenc); opts.append(x264); return opts
    if FORCE_ENCODER == "x264":
        opts.append(x264); add_if(nvenc_ok, nvenc); add_if(qsv_ok, qsv); return opts
    if PREFER_NVENC and nvenc_ok: opts.append(nvenc)
    if PREFER_QSV   and qsv_ok:   opts.append(qsv)
    opts.append(x264)
    return opts
