import os, re, m3u8, subprocess, time, threading
from urllib.parse import urljoin, urlparse, urlunparse, urlencode, parse_qsl

from parameters import DEBUG, CONTAINER, FFMPEG_PATH
from streamonitor.bot import Bot
# stdlib requests only (stable vs curl_cffi inside threads)
import requests

TEMP_DIR_NAME = "M3U8_TMP"

# ─────────────────────────────────────────────────────────────────────────────
# Temp root resolver (NO HARDCODED PATHS)
# Priority:
#   1) ENV M3U8_TMP
#   2) parameters.HLS_TMP_DIR (optional)
#   3) <repo_root>/M3U8_TMP
#   4) <downloads_root>/M3U8_TMP  (same drive as downloads)
#   5) CWD/M3U8_TMP
# ─────────────────────────────────────────────────────────────────────────────

try:
    from parameters import HLS_TMP_DIR as _HLS_TMP_DIR
except Exception:
    _HLS_TMP_DIR = None

def _get_tmp_root(self: Bot) -> str:

    if _HLS_TMP_DIR:
        os.makedirs(_HLS_TMP_DIR, exist_ok=True)
        return _HLS_TMP_DIR

    # repo_root/SC_TMP
    try:
        here = os.path.abspath(__file__)
        repo_root = os.path.abspath(os.path.join(here, "..", ".."))  # …/streamonitor/
        repo_root = os.path.abspath(os.path.join(repo_root, ".."))   # repo root
        candidate = os.path.join(repo_root, TEMP_DIR_NAME)
        os.makedirs(candidate, exist_ok=True)
        return candidate
    except Exception:
        pass

    # near downloads root
    try:
        downloads_parent = os.path.abspath(os.path.join(self.outputFolder, ".."))
        candidate = os.path.join(downloads_parent, TEMP_DIR_NAME)
        os.makedirs(candidate, exist_ok=True)
        return candidate
    except Exception:
        pass

    # fallback: cwd
    candidate = os.path.join(os.getcwd(), TEMP_DIR_NAME)
    os.makedirs(candidate, exist_ok=True)
    return candidate


# ─────────────────────────────────────────────────────────────────────────────
# URL helpers: make relatives absolute + inherit parent query tokens
# ─────────────────────────────────────────────────────────────────────────────

def _abs_with_parent_query(base_url: str, maybe_rel: str) -> str:
    base = urlparse(base_url)
    base_dir = base._replace(path=(base.path.rsplit("/", 1)[0] + "/"), query="", fragment="")
    absu = maybe_rel if maybe_rel.startswith(("http://", "https://")) else urljoin(urlunparse(base_dir), maybe_rel)
    up = urlparse(absu)

    q_child = dict(parse_qsl(up.query or ""))
    q_parent = dict(parse_qsl(base.query or ""))
    merged = dict(q_parent)
    merged.update(q_child)
    return urlunparse(up._replace(query=urlencode(merged)))


def _rewrite_playlist_abs_and_tokens(base_url: str, playlist_text: str) -> str:
    out = []
    saw_header = False
    for line in playlist_text.splitlines():
        if line.startswith("#EXTM3U"):
            saw_header = True
            out.append(line)
            continue

        if line.startswith("#EXT-X-MAP:"):
            m = re.search(r'URI="([^"]+)"', line)
            if m:
                newu = _abs_with_parent_query(base_url, m.group(1))
                line = re.sub(r'URI="[^"]+"', f'URI="{newu}"', line)
            out.append(line); continue

        if line.startswith("#EXT-X-KEY:"):
            m = re.search(r'URI="([^"]+)"', line)
            if m:
                newu = _abs_with_parent_query(base_url, m.group(1))
                line = re.sub(r'URI="[^"]+"', f'URI="{newu}"', line)
            out.append(line); continue

        if not line or line.startswith("#"):
            out.append(line); continue

        # segment URI
        out.append(_abs_with_parent_query(base_url, line))

    if not saw_header:
        out.insert(0, "#EXTM3U")
    return "\n".join(out) + "\n"


# ─────────────────────────────────────────────────────────────────────────────
# Rolling per-model playlist writer (NO PROXY). Cleans only its own files.
# ─────────────────────────────────────────────────────────────────────────────

class _RollingM3UWriter:
    """
    Periodically polls the live media playlist, fixes URIs (absolute + tokens),
    and writes to a per-model .m3u8 file under SC_TMP/<SITE>_<USER>/rolling.m3u8.
    Only this writer’s file is ever removed.
    """
    def __init__(self, media_url: str, sess: requests.Session, headers: dict,
                 m3u_processor, tmp_root: str, model_key: str, poll_sec=1.5):
        self.media_url = media_url
        self.sess = sess
        self.headers = dict(headers or {})
        self.m3u_processor = m3u_processor
        self.poll_sec = poll_sec
        self._stop = threading.Event()
        self._thr = None

        self.model_dir = os.path.join(tmp_root, model_key)
        os.makedirs(self.model_dir, exist_ok=True)

        # On start: purge ONLY leftovers in this model dir (ours)
        for fname in os.listdir(self.model_dir):
            if fname.endswith(".m3u8") or fname.endswith(".part"):
                try:
                    os.remove(os.path.join(self.model_dir, fname))
                except Exception:
                    pass

        self.path = os.path.join(self.model_dir, "rolling.m3u8")

    def start(self):
        self._thr = threading.Thread(target=self._loop, daemon=True)
        self._thr.start()

    def _loop(self):
        last_text = None
        while not self._stop.is_set():
            try:
                r = self.sess.get(self.media_url, headers=self.headers, timeout=15)
                if r.status_code != 200:
                    time.sleep(self.poll_sec); continue
                txt = r.content.decode("utf-8", errors="ignore")
                if callable(self.m3u_processor):
                    txt = self.m3u_processor(txt)
                fixed = _rewrite_playlist_abs_and_tokens(self.media_url, txt)
                if fixed != last_text:
                    tmp2 = self.path + ".part"
                    with open(tmp2, "wb") as f:
                        f.write(fixed.encode("utf-8"))
                    try:
                        os.replace(tmp2, self.path) if os.path.exists(self.path) else os.rename(tmp2, self.path)
                    except Exception:
                        with open(self.path, "wb") as f:
                            f.write(fixed.encode("utf-8"))
                last_text = fixed
            except Exception:
                # keep rolling regardless
                pass
            self._stop.wait(self.poll_sec)

    def stop(self):
        self._stop.set()
        if self._thr:
            try:
                self._thr.join(timeout=2.0)
            except Exception:
                pass
        # Clean ONLY our own files; do not touch siblings from other models
        try:
            if os.path.exists(self.path):
                os.unlink(self.path)
        except Exception:
            pass
        try:
            pp = self.path + ".part"
            if os.path.exists(pp):
                os.unlink(pp)
        except Exception:
            pass
        # remove empty model dir (best-effort)
        try:
            if not os.listdir(self.model_dir):
                os.rmdir(self.model_dir)
        except Exception:
            pass


# ─────────────────────────────────────────────────────────────────────────────
# Main entry (unchanged name): ALWAYS writes <N>.tmp.ts in the model folder
# ─────────────────────────────────────────────────────────────────────────────

def getVideoNativeHLS(self: Bot, url, filename, m3u_processor=None):
    """
    Robust HLS capture that ALWAYS writes a growing N.tmp.ts during the live show.
    No mid-flight remux; merger handles *.tmp.ts later.
    """
    # Stop wiring
    self.stopDownloadFlag = False
    writer_ref = {"w": None}
    ffmpeg_proc_ref = {"p": None}

    def _stop_both():
        self.stopDownloadFlag = True
        try:
            if ffmpeg_proc_ref["p"] is not None:
                ffmpeg_proc_ref["p"].terminate()
        except Exception:
            pass
        try:
            if writer_ref["w"] is not None:
                writer_ref["w"].stop()
        except Exception:
            pass

    self.stopDownload = _stop_both

    # Output path: always .tmp.ts — merger will finalize later
    tmp_path = filename[:-len("." + CONTAINER)] + ".tmp.ts"
    out_dir = os.path.dirname(tmp_path)
    os.makedirs(out_dir, exist_ok=True)

    # Headers (sane defaults)
    headers = dict(self.headers or {})
    headers.setdefault("User-Agent", headers.get("User-Agent") or "Mozilla/5.0")
    headers.setdefault("Accept", "*/*")
    headers.setdefault("Connection", "keep-alive")

    # Per-stream session (NOT curl_cffi)
    sess = requests.Session()
    if self.cookies:
        try:
            if isinstance(self.cookies, dict):
                for k, v in self.cookies.items():
                    sess.cookies.set(k, v)
            else:
                for c in self.cookies:
                    sess.cookies.set(getattr(c, "name", ""), getattr(c, "value", ""))
        except Exception:
            pass

    def fetch_text(u):
        r = sess.get(u, headers=headers, timeout=15)
        if r.status_code != 200:
            return None, r.status_code
        return r.content.decode("utf-8", errors="ignore"), 200

    # Resolve master → best variant
    text0, code0 = fetch_text(url)
    if text0 is None:
        self.debug(f"[HLS] master fetch failed/unavailable ({code0}); FFmpeg direct → .tmp.ts")
        ok = _ffmpeg_dump_to_ts(self, url, headers, tmp_path, ffmpeg_proc_ref)
        try: sess.close()
        except Exception: pass
        return bool(ok)

    if callable(m3u_processor):
        text0 = m3u_processor(text0)

    try:
        pl0 = m3u8.loads(text0)
    except Exception:
        pl0 = None

    if getattr(pl0, "is_variant", False) and getattr(pl0, "playlists", []):
        best = max(pl0.playlists, key=lambda p: (getattr(p.stream_info, "bandwidth", 0) or 0))
        url = best.uri if best.uri.startswith(("http://", "https://")) else urljoin(url, best.uri)

    # Fetch media playlist (so the writer can roll it)
    text1, code1 = fetch_text(url)
    if text1 is None:
        self.debug(f"[HLS] media fetch failed ({code1}); FFmpeg direct → .tmp.ts")
        ok = _ffmpeg_dump_to_ts(self, url, headers, tmp_path, ffmpeg_proc_ref)
        try: sess.close()
        except Exception: pass
        return bool(ok)

    if callable(m3u_processor):
        text1 = m3u_processor(text1)

    tmp_root = _get_tmp_root(self)
    model_key = f"[{getattr(self,'siteslug','SITE')}]{self.username}"
    writer = _RollingM3UWriter(
        media_url=url, sess=sess, headers=headers, m3u_processor=m3u_processor,
        tmp_root=tmp_root, model_key=model_key, poll_sec=1.5
    )
    writer.start()
    writer_ref["w"] = writer

    # Wait a moment for first snapshot
    t0 = time.time()
    while (not os.path.exists(writer.path) or os.path.getsize(writer.path) == 0) and time.time() - t0 < 3.0:
        time.sleep(0.05)

    # Hand local playlist to FFmpeg (no proxy). Blocks until FFmpeg exits.
    ok = _ffmpeg_dump_to_ts(self, writer.path, headers, tmp_path, ffmpeg_proc_ref, local_m3u=True)

    # Shutdown writer + session
    try: writer.stop()
    except Exception: pass
    try: sess.close()
    except Exception: pass

    try:
        return bool(ok) and os.path.getsize(tmp_path) > 0
    except Exception:
        return bool(ok)


# ─────────────────────────────────────────────────────────────────────────────
# FFmpeg runner: copy HLS → single growing TS. Accepts remote URL or local m3u8
# ─────────────────────────────────────────────────────────────────────────────
def _ffmpeg_dump_to_ts(self: Bot, url_or_path, headers, out_path, proc_ref, local_m3u=False):
    # Build safe headers blob
    hdrs = {
        "User-Agent": headers.get("User-Agent", "Mozilla/5.0"),
        "Accept": headers.get("Accept", "*/*"),
        "Connection": headers.get("Connection", "keep-alive"),
        "Accept-Language": headers.get("Accept-Language", "en-US,en;q=0.9"),
    }
    if "Cookie" in headers:
        hdrs["Cookie"] = headers["Cookie"]
    hdr_blob = "\r\n".join(f"{k}: {v}" for k, v in hdrs.items()) + "\r\n"

    cmd = [
        FFMPEG_PATH,
        "-hide_banner", "-loglevel", "info", "-nostdin",
        "-protocol_whitelist", "file,http,https,tcp,tls,crypto,pipe",
        "-fflags", "nobuffer", "-fflags", "+discardcorrupt", "-fflags", "+genpts",
        "-probesize", "64M", "-analyzeduration", "120M",
        "-i", url_or_path,
        "-map", "0:v:0?", "-map", "0:a?", "-dn", "-sn",
        "-c", "copy", "-copyinkf",
        "-avoid_negative_ts", "make_zero",
        "-reset_timestamps", "1",
        "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0",
        "-f", "mpegts",
        out_path,
    ]

    # Add network-only options for remote inputs
    if not local_m3u and str(url_or_path).startswith(("http://", "https://")):
        net_opts = [
            "-headers", hdr_blob,
            "-rw_timeout", "5000000",
            "-stimeout", "5000000",     # common on Windows builds
            "-reconnect", "1", "-reconnect_streamed", "1",
            "-reconnect_on_network_error", "1",
            "-reconnect_on_http_error", "4xx,5xx",
            "-reconnect_at_eof", "1", "-reconnect_delay_max", "10",
            "-max_reload", "180", "-seg_max_retry", "180", "-m3u8_hold_counters", "180",
            "-allowed_extensions", "ALL",
        ]
        # Inject before "-i"
        idx = cmd.index("-i")
        cmd[idx:idx] = net_opts

    # Log the command (to file only; not console)
    try:
        dbg_cmd = " ".join(_shlex_quote(c) for c in cmd)
        self.debug("FFmpeg CMD (capture→.tmp.ts): " + dbg_cmd)
    except Exception:
        pass

    # Spawn ffmpeg with stderr piped; forward ALL lines to logger
    startupinfo = None
    creationflags = 0
    if os.name == "nt":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        # Hide console window on Windows
        try:
            creationflags = subprocess.CREATE_NO_WINDOW
        except Exception:
            creationflags = 0

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        bufsize=1,
        startupinfo=startupinfo,
        creationflags=creationflags,
    )
    proc_ref["p"] = proc

    # DO NOT override self.stopDownload here (getVideoNativeHLS already set a closure that stops both proc+writer)

    # Stream all stderr lines to the logger
    try:
        for line in proc.stderr:
            line = (line or "").rstrip("\r\n")
            if not line:
                continue
            if DEBUG:
                self.logger.debug("[ffmpeg] " + line)
    except Exception:
        pass

    rc = proc.wait()
    proc_ref["p"] = None
    try:
        proc.stderr.close()
    except Exception:
        pass

    # Succeed if ffmpeg returned OK or we produced non-empty TS
    try:
        nonempty = os.path.exists(out_path) and os.path.getsize(out_path) > 0
    except Exception:
        nonempty = False
    return (rc == 0) or nonempty


# ─────────────────────────────────────────────────────────────────────────────
# Legacy StopBot (name unchanged). Kept for compatibility (not used by closure)
# ─────────────────────────────────────────────────────────────────────────────

def StopBot(self: Bot, proc: subprocess.Popen = None):
    if proc is not None:
        try:
            proc.terminate()
        except Exception:
            pass
    self.recording = False
    self.stopDownload = None


def _shlex_quote(s: str) -> str:
    # safe-ish command logging without importing shlex for Windows paths
    if re.match(r'^[a-zA-Z0-9._/\-+=:@%,\\]+$', s):
        return s
    return "'" + s.replace("'", "'\"'\"'") + "'"
