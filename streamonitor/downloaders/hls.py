# streamonitor/downloaders/hls.py
# Fixed native HLS downloader with rolling playlist and stall detection

import os
import re
from typing import Any, Callable, Dict, Optional
import m3u8
import subprocess
import time
import threading
from urllib.parse import urljoin, urlparse, urlunparse, urlencode, parse_qsl

# Platform-specific imports for file locking
try:
    if os.name == 'nt':
        import msvcrt  # Windows file locking
    else:
        import fcntl   # Unix file locking
except ImportError:
    pass  # File locking not available

from parameters import DEBUG, CONTAINER, FFMPEG_PATH
from streamonitor.bot import Bot
import requests

TEMP_DIR_NAME = "M3U8_TMP"

try:
    from parameters import HLS_TMP_DIR as _HLS_TMP_DIR
except Exception:
    _HLS_TMP_DIR = None


def _get_tmp_root(self: Bot) -> str:
    """Get temporary directory for M3U8 files."""
    if _HLS_TMP_DIR:
        os.makedirs(_HLS_TMP_DIR, exist_ok=True)
        return _HLS_TMP_DIR

    # Try repo root
    try:
        here = os.path.abspath(__file__)
        repo_root = os.path.abspath(os.path.join(here, "..", ".."))
        repo_root = os.path.abspath(os.path.join(repo_root, ".."))
        candidate = os.path.join(repo_root, TEMP_DIR_NAME)
        os.makedirs(candidate, exist_ok=True)
        return candidate
    except Exception:
        pass

    # Try near downloads
    try:
        downloads_parent = os.path.abspath(os.path.join(self.outputFolder, ".."))
        candidate = os.path.join(downloads_parent, TEMP_DIR_NAME)
        os.makedirs(candidate, exist_ok=True)
        return candidate
    except Exception:
        pass

    # Fallback: cwd
    candidate = os.path.join(os.getcwd(), TEMP_DIR_NAME)
    os.makedirs(candidate, exist_ok=True)
    return candidate


def _abs_with_parent_query(base_url: str, maybe_rel: str) -> str:
    """Make URL absolute and inherit parent query parameters."""
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
    """Rewrite playlist URIs to absolute with inherited query tokens."""
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
            out.append(line)
            continue

        if line.startswith("#EXT-X-KEY:"):
            m = re.search(r'URI="([^"]+)"', line)
            if m:
                newu = _abs_with_parent_query(base_url, m.group(1))
                line = re.sub(r'URI="[^"]+"', f'URI="{newu}"', line)
            out.append(line)
            continue

        if not line or line.startswith("#"):
            out.append(line)
            continue

        # Segment URI
        out.append(_abs_with_parent_query(base_url, line))

    if not saw_header:
        out.insert(0, "#EXTM3U")
    
    return "\n".join(out) + "\n"


class _RollingM3UWriter:
    """
    Periodically polls live media playlist and writes fixed version.
    Thread-safe with proper error handling.
    """
    
    def __init__(self, media_url: str, sess: requests.Session, headers: Dict[str, str], m3u_processor: Optional[Callable[[str], str]], tmp_root: str, model_key: str, poll_sec: float = 1.5, logger: Any = None, bot_instance: Any = None):
        self.media_url = media_url
        self.sess = sess
        self.headers = dict(headers or {})
        self.m3u_processor = m3u_processor
        self.poll_sec = poll_sec
        self.logger = logger
        self.bot_instance = bot_instance  # Reference to bot for status checks
        self._stop = threading.Event()
        self._ready = threading.Event()  # NEW: signals first valid playlist
        self._thr = None
        self._error = None

        # Create unique directory to avoid conflicts between multiple instances
        process_id = os.getpid()
        thread_id = threading.get_ident()
        unique_model_key = f"{model_key}_{process_id}_{thread_id}"
        self.model_dir = os.path.join(tmp_root, unique_model_key)
        os.makedirs(self.model_dir, exist_ok=True)

        # Clean up old files from THIS process only
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

    def wait_ready(self, timeout=10):
        """Wait for first valid playlist. Returns True if ready, False on timeout."""
        return self._ready.wait(timeout)

    def _loop(self):
        last_text = None
        consecutive_errors = 0
        max_errors = 5
        
        while not self._stop.is_set():
            try:
                r = self.sess.get(self.media_url, headers=self.headers, timeout=10)
                
                if r.status_code != 200:
                    consecutive_errors += 1
                    
                    # Special handling for 403 - likely private show
                    if r.status_code == 403:
                        if self.logger:
                            self.logger.warning("Playlist access forbidden (403) - likely private show")
                        
                        # Trigger status recheck if bot instance is available
                        if self.bot_instance and hasattr(self.bot_instance, 'getStatus'):
                            try:
                                if self.logger:
                                    self.logger.info("Checking status due to forbidden playlist")
                                current_status = self.bot_instance.getStatus()
                                if self.logger:
                                    self.logger.info(f"Current status: {current_status}")
                            except Exception as e:
                                if self.logger:
                                    self.logger.warning(f"Failed to check status: {e}")
                        
                        # Stop trying immediately for 403 - no point retrying
                        self._error = "Playlist access forbidden (likely private show)"
                        break
                    
                    if self.logger and consecutive_errors <= 3:
                        self.logger.warning(f"Playlist fetch returned {r.status_code}")
                    
                    if consecutive_errors >= max_errors:
                        self._error = f"Too many failed fetches ({consecutive_errors})"
                        break
                    
                    time.sleep(self.poll_sec)
                    continue
                
                # Reset error counter on success
                consecutive_errors = 0
                
                txt = r.content.decode("utf-8", errors="ignore")
                
                # Apply custom processor
                if callable(self.m3u_processor):
                    txt = self.m3u_processor(txt)
                
                # Validate it's actually a playlist
                if not txt.strip() or "#EXTM3U" not in txt:
                    if self.logger:
                        self.logger.warning("Invalid playlist format")
                    time.sleep(self.poll_sec)
                    continue
                
                # Check for segments (basic validation)
                has_segments = any(
                    line and not line.startswith("#") 
                    for line in txt.splitlines()
                )
                
                if not has_segments:
                    if self.logger and not self._ready.is_set():
                        self.logger.warning("Playlist has no segments yet")
                    time.sleep(self.poll_sec)
                    continue
                
                # Fix URLs
                fixed = _rewrite_playlist_abs_and_tokens(self.media_url, txt)
                
                # Write if changed
                if fixed != last_text:
                    try:
                        with open(self.path, "w", encoding="utf-8") as f:
                            f.write(fixed)
                        last_text = fixed
                    except Exception as e:
                        if self.logger:
                            self.logger.warning(f"Failed to write playlist: {e}")
                        # Continue anyway - don't break the loop
                    
                    # Signal ready on first valid write
                    if not self._ready.is_set():
                        self._ready.set()
            
            except requests.RequestException as e:
                consecutive_errors += 1
                if self.logger and consecutive_errors <= 3:
                    self.logger.warning(f"Playlist fetch error: {e}")
                
                if consecutive_errors >= max_errors:
                    self._error = f"Network errors: {e}"
                    break
            
            except Exception as e:
                if self.logger:
                    self.logger.error(f"Unexpected writer error: {e}")
                self._error = str(e)
                break
            
            self._stop.wait(self.poll_sec)

    def stop(self):
        self._stop.set()
        if self._thr:
            try:
                self._thr.join(timeout=3.0)
            except Exception:
                pass
        
        # Clean up our files
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
        
        # Remove empty dir
        try:
            if not os.listdir(self.model_dir):
                os.rmdir(self.model_dir)
        except Exception:
            pass


def _format_bytes(size: int) -> str:
    """Convert bytes to human readable format."""
    for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
        if size < 1024.0:
            if unit == 'B':
                return f"{size:,.0f} {unit}"
            else:
                return f"{size:,.1f} {unit}"
        size /= 1024.0
    return f"{size:,.1f} PB"


def getVideoNativeHLS(self: Bot, url: str, filename: str,  m3u_processor: Optional[Callable[[str], str]] = None) -> bool:
    """
    Robust HLS capture that writes growing .tmp.ts during live show.
    Fixed to prevent corrupted/empty files.
    """
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

    # Output: use the final filename directly, not .tmp.ts
    final_path = filename
    out_dir = os.path.dirname(final_path)
    os.makedirs(out_dir, exist_ok=True)

    # Headers
    headers = dict(self.headers or {})
    headers.setdefault("User-Agent", "Mozilla/5.0")
    headers.setdefault("Accept", "*/*")
    headers.setdefault("Connection", "keep-alive")

    # Session (not curl_cffi for thread safety)
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
        try:
            r = sess.get(u, headers=headers, timeout=10)
            if r.status_code != 200:
                return None, r.status_code
            return r.content.decode("utf-8", errors="ignore"), 200
        except Exception as e:
            self.logger.error(f"Fetch failed: {e}")
            return None, 0

    # Resolve master
    text0, code0 = fetch_text(url)
    if text0 is None:
        self.logger.warning(f"Master fetch failed ({code0}), trying direct FFmpeg")
        ok = _ffmpeg_dump_to_ts(self, url, headers, final_path, ffmpeg_proc_ref)
        try:
            sess.close()
        except Exception:
            pass
        return bool(ok)

    if callable(m3u_processor):
        text0 = m3u_processor(text0)

    try:
        pl0 = m3u8.loads(text0)
    except Exception:
        pl0 = None

    # Select best variant
    if getattr(pl0, "is_variant", False) and getattr(pl0, "playlists", []):
        best = max(pl0.playlists, key=lambda p: (getattr(p.stream_info, "bandwidth", 0) or 0))
        url = best.uri if best.uri.startswith(("http://", "https://")) else urljoin(url, best.uri)

    # Fetch media playlist
    text1, code1 = fetch_text(url)
    if text1 is None:
        self.logger.warning(f"Media fetch failed ({code1}), trying direct FFmpeg")
        ok = _ffmpeg_dump_to_ts(self, url, headers, final_path, ffmpeg_proc_ref)
        try:
            sess.close()
        except Exception:
            pass
        return bool(ok)

    if callable(m3u_processor):
        text1 = m3u_processor(text1)

    # Start writer
    tmp_root = _get_tmp_root(self)
    model_key = f"[{getattr(self, 'siteslug', 'SITE')}]{self.username}"
    writer = _RollingM3UWriter(
        media_url=url, sess=sess, headers=headers, m3u_processor=m3u_processor,
        tmp_root=tmp_root, model_key=model_key, poll_sec=1.5, logger=self.logger, bot_instance=self
    )
    writer.start()
    writer_ref["w"] = writer

    # CRITICAL FIX: Wait for valid playlist with segments
    if not writer.wait_ready(timeout=10):
        self.logger.error("Playlist writer failed to produce valid playlist")
        try:
            writer.stop()
        except Exception:
            pass
        try:
            sess.close()
        except Exception:
            pass
        return False

    # Start FFmpeg with local playlist
    ok = _ffmpeg_dump_to_ts(self, writer.path, headers, final_path, ffmpeg_proc_ref, local_m3u=True)

    # Cleanup
    try:
        writer.stop()
    except Exception:
        pass
    try:
        sess.close()
    except Exception:
        pass

    # Validate output
    try:
        if not os.path.exists(final_path):
            self.logger.error("Output file does not exist")
            return False
        
        size = os.path.getsize(final_path)
        if size == 0:
            self.logger.error("Output file is empty")
            os.remove(final_path)
            return False
        
        if size < 1024:  # Less than 1KB is suspicious
            self.logger.warning(f"Output file is very small ({_format_bytes(size)})")
        
        self.logger.info(f"Captured {_format_bytes(size)} to {os.path.basename(final_path)}")
        return bool(ok) or (size > 0)
    
    except Exception as e:
        self.logger.error(f"Output validation failed: {e}")
        return False


def _ffmpeg_dump_to_ts(self: Bot, url_or_path: str, headers: Dict[str, str], out_path: str, proc_ref: Dict[str, Optional[subprocess.Popen]], local_m3u=False) -> bool:
    """
    Fixed FFmpeg runner with proper flags and stall detection.
    """
    # Build headers
    hdrs = {
        "User-Agent": headers.get("User-Agent", "Mozilla/5.0"),
        "Accept": headers.get("Accept", "*/*"),
        "Connection": headers.get("Connection", "keep-alive"),
        "Accept-Language": headers.get("Accept-Language", "en-US,en;q=0.9"),
    }
    if "Cookie" in headers:
        hdrs["Cookie"] = headers["Cookie"]
    hdr_blob = "\r\n".join(f"{k}: {v}" for k, v in hdrs.items()) + "\r\n"

    # Base command - FIXED FLAGS
    cmd = [
        FFMPEG_PATH,
        "-hide_banner", "-loglevel", "info", "-nostdin",
        "-protocol_whitelist", "file,http,https,tcp,tls,crypto,pipe",
        "-fflags", "nobuffer",
        "-fflags", "+discardcorrupt",
        "-fflags", "+genpts",
        "-probesize", "4M",  # REDUCED from 64M
        "-analyzeduration", "10000000",  # REDUCED from 120M (10 seconds)
    ]

    # Network options for remote URLs
    if not local_m3u and str(url_or_path).startswith(("http://", "https://")):
        cmd.extend([
            "-headers", hdr_blob,
            "-rw_timeout", "5000000",
            "-timeout", "5000000",
            "-reconnect", "1",
            "-reconnect_streamed", "1",
            "-reconnect_on_network_error", "1",
            "-reconnect_on_http_error", "4xx,5xx",
            "-reconnect_at_eof", "1",
            "-reconnect_delay_max", "10",
            "-live_start_index", "-3",  # Start from last 3 segments
            "-max_reload", "180",
            "-seg_max_retry", "180",
            "-m3u8_hold_counters", "180",
        ])

    # Input
    cmd.extend(["-i", url_or_path])

    # Stream mapping
    cmd.extend(["-map", "0:v:0?", "-map", "0:a?", "-dn", "-sn"])

    # Stream copy (NO -copyinkf - causes corruption with HLS)
    cmd.extend(["-c", "copy"])

    # Timestamp handling for live streams
    cmd.extend([
        "-reset_timestamps", "1",  # REQUIRED: resets to 0 when joining mid-stream
        "-avoid_negative_ts", "make_zero",
        "-muxpreload", "0",
        "-muxdelay", "0",
        "-max_interleave_delta", "0",
        "-f", "mpegts",
        out_path,
    ])

    # Log command
    if DEBUG:
        try:
            dbg_cmd = " ".join(_shlex_quote(c) for c in cmd)
            self.debug(f"FFmpeg CMD: {dbg_cmd}")
        except Exception:
            pass

    # Spawn
    startupinfo = None
    creationflags = 0
    if os.name == "nt":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
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

    # Monitor stderr with basic stall detection
    last_output = time.monotonic()
    last_size = 0
    
    try:
        for line in proc.stderr:
            line = (line or "").rstrip("\r\n")
            if not line:
                continue
            
            if DEBUG:
                self.logger.debug(f"[ffmpeg] {line}")
            
            # Check for critical errors
            if "invalid" in line.lower() or "error" in line.lower():
                if "No such" in line or "failed" in line:
                    self.logger.error(f"FFmpeg error: {line}")
            
            last_output = time.monotonic()
            
            # Check output growth every 30 seconds
            now = time.monotonic()
            if now - last_output > 30:
                try:
                    if os.path.exists(out_path):
                        size = os.path.getsize(out_path)
                        if size == last_size:
                            self.logger.warning("Output not growing, possible stall")
                        last_size = size
                except Exception:
                    pass
    except Exception:
        pass

    rc = proc.wait()
    proc_ref["p"] = None
    
    try:
        proc.stderr.close()
    except Exception:
        pass

    # Validate output
    try:
        if os.path.exists(out_path):
            size = os.path.getsize(out_path)
            return (rc == 0) or (size > 0)
    except Exception:
        pass
    
    return rc == 0


def StopBot(self: Bot, proc: subprocess.Popen = None):
    """Legacy stop function for compatibility."""
    if proc is not None:
        try:
            proc.terminate()
        except Exception:
            pass
    self.recording = False
    self.stopDownload = None


def _shlex_quote(s: str) -> str:
    """Simple shell quoting for logging."""
    if re.match(r'^[a-zA-Z0-9._/\-+=:@%,\\]+$', s):
        return s
    return "'" + s.replace("'", "'\"'\"'") + "'"