# streamonitor/downloaders/ffmpeg.py
# Fixed FFmpeg HLS downloader with adaptive stall detection

import os
import sys
import time
import subprocess
import signal
import re
import collections
from shlex import quote as shlex_quote
from typing import TYPE_CHECKING

import requests.cookies
from parameters import DEBUG, SEGMENT_TIME, CONTAINER, FFMPEG_PATH
from .ffsettings import FFSettings

if TYPE_CHECKING:
    from streamonitor.bot import Bot



def getVideoFfmpeg(self: 'Bot', url: str, filename: str) -> bool:
    """
    Live HLS recorder with adaptive stall detection.
    Writes using CONTAINER from parameters (recommended: 'mkv' for safety).
    Returns True on overall success, False otherwise.
    """
    
    # Get format settings based on CONTAINER
    fmt = FFSettings.get_format_settings(CONTAINER)
    MUX = CONTAINER.lower().strip() if CONTAINER else 'mkv'
    if MUX not in ('ts', 'mkv', 'mp4'):
        MUX = 'mkv'
    
    EXT_SINGLE = fmt['ext']
    SEGMENT_FMT = fmt['segment_format']
    
    # Cache FFmpeg HLS capabilities
    if not hasattr(self, '_hls_help_cache'):
        try:
            self._hls_help_cache = subprocess.check_output(
                [FFMPEG_PATH, '-hide_banner', '-h', 'demuxer=hls'],
                stderr=subprocess.STDOUT, timeout=5, text=True, errors='replace'
            )
        except Exception:
            self._hls_help_cache = ""
    
    def hls_supports(opt):
        return opt in self._hls_help_cache
    
    # Stopper for graceful shutdown
    class _Stopper:
        def __init__(self):
            self.stop = False
        def pls_stop(self):
            self.stop = True
    
    stopping = _Stopper()
    self.stopDownload = stopping.pls_stop
    
    # Regex patterns for parsing FFmpeg output
    time_pattern = re.compile(r"time=(\d+):(\d+):(\d+)(?:\.(\d+))?")
    speed_pattern = re.compile(r"speed=\s*([0-9.]+)x")
    lag_pattern = re.compile(r"after a lag of ([0-9.]+)s")
    skip_pattern = re.compile(r"skipping\s+\d+\s+segments ahead")
    
    # Playlist change tracking
    last_playlist_id = None
    last_playlist_change = time.monotonic()
    
    if FFSettings.PLAYLIST_PROBE_ENABLED:
        import requests as _req
        
        def probe_playlist():
            nonlocal last_playlist_id, last_playlist_change
            try:
                r = _req.get(
                    url,
                    headers={"User-Agent": self.headers.get('User-Agent', 'Mozilla/5.0')},
                    timeout=5,
                    verify=False
                )
                if r.status_code != 200:
                    return
                
                txt = r.text
                pid = None
                
                # Try to get media sequence number
                for line in txt.splitlines():
                    if line.startswith("#EXT-X-MEDIA-SEQUENCE:"):
                        pid = "SEQ:" + line.split(":", 1)[1].strip()
                        break
                
                # Fallback to last segment filename
                if pid is None:
                    for line in reversed(txt.splitlines()):
                        if line and not line.startswith("#"):
                            pid = "SEG:" + line.strip()
                            break
                
                if pid and pid != last_playlist_id:
                    last_playlist_id = pid
                    last_playlist_change = time.monotonic()
            except Exception:
                pass
        
        probe_playlist()
    else:
        def probe_playlist():
            pass
    
    def playlist_stale():
        return (time.monotonic() - last_playlist_change) > FFSettings.PLAYLIST_STALE_THRESHOLD_SEC
    
    def _compose_headers():
        """Extract and format headers for FFmpeg."""
        try:
            hdrs = dict(self.headers or {})
        except Exception:
            hdrs = {}
        
        ua = hdrs.pop("User-Agent", None)
        header_lines = ""
        
        for k, v in hdrs.items():
            if not k or v is None:
                continue
            header_lines += f"{k}: {v}\r\n"
        
        return ua or "Mozilla/5.0", header_lines
    
    def _compose_cookies():
        """Extract and format cookies for FFmpeg."""
        if isinstance(self.cookies, requests.cookies.RequestsCookieJar):
            parts = []
            for c in self.cookies:
                parts.append(f"{c.name}={c.value}")
            if parts:
                return ";".join(parts)  # No space after semicolon for RFC compliance
        elif isinstance(self.cookies, dict):
            parts = [f"{k}={v}" for k, v in self.cookies.items()]
            if parts:
                return ";".join(parts)
        return None
    
    def build_cmd():
        """Build FFmpeg command with all necessary parameters."""
        ua, extra_headers = _compose_headers()
        cmd = [FFMPEG_PATH, '-hide_banner', '-loglevel', 'info', '-user_agent', ua]
        
        if extra_headers:
            cmd.extend(['-headers', extra_headers])
        
        ck = _compose_cookies()
        if ck:
            cmd.extend(['-cookies', ck])
        
        # Timeout settings
        rw_us = str(int(FFSettings.RW_TIMEOUT_SEC * 1_000_000))
        sock_us = str(int(FFSettings.SOCKET_TIMEOUT_SEC * 1_000_000))
        cmd.extend([
            '-timeout', sock_us,
            '-rw_timeout', rw_us,
            '-reconnect', '1',
            '-reconnect_streamed', '1',
            '-reconnect_on_network_error', '1',
            '-reconnect_on_http_error', '4xx,5xx',
            '-reconnect_at_eof', '1',
            '-reconnect_delay_max', str(FFSettings.RECONNECT_DELAY_MAX)
        ])
        
        # HLS-specific options
        if FFSettings.LIVE_LAST_SEGMENTS is not None and hls_supports('live_start_index'):
            cmd.extend(['-live_start_index', f'-{FFSettings.LIVE_LAST_SEGMENTS}'])
        if hls_supports('max_reload'):
            cmd.extend(['-max_reload', '30'])
        if hls_supports('seg_max_retry'):
            cmd.extend(['-seg_max_retry', '30'])
        if hls_supports('m3u8_hold_counters'):
            cmd.extend(['-m3u8_hold_counters', '30'])
        
        # FFmpeg flags - CRITICAL FIX: Combine all fflags into ONE argument
        fflags_parts = []
        if FFSettings.ENABLE_FFLAGS_NOBUFFER:
            fflags_parts.append('nobuffer')
        if FFSettings.ENABLE_FFLAGS_DISCARDCORRUPT:
            fflags_parts.append('+discardcorrupt')
        if FFSettings.USE_GENPTS:
            fflags_parts.append('+genpts')
        # CRITICAL: Always add +igndts to ignore broken DTS from long-running live streams
        fflags_parts.append('+igndts')
        
        if fflags_parts:
            cmd.extend(['-fflags', ''.join(fflags_parts)])
        
        cmd.extend(['-probesize', FFSettings.PROBESIZE, '-analyzeduration', FFSettings.ANALYSEDURATION_US])
        
        if FFSettings.USE_PROGRESS_FORCED_STATS:
            cmd.extend(['-progress', 'pipe:2', '-stats_period', '5'])
        
        # Input
        cmd.extend(['-i', url])
        
        # Stream mapping (video optional, audio optional, no data/subtitles)
        cmd.extend(['-map', '0:v:0?', '-map', '0:a?', '-dn', '-sn'])
        
        # Stream copy (no re-encoding)
        cmd.extend(['-c', 'copy'])
        
        # MP4-specific: AAC ADTS to ASC bitstream filter
        if MUX == 'mp4':
            cmd.extend(['-bsf:a', 'aac_adtstoasc'])
        
        # Common muxer tuning for MKV and MP4
        # NOTE: Removed -reset_timestamps since +igndts+genpts handles it better
        if MUX in ('mkv', 'mp4'):
            cmd.extend([
                '-avoid_negative_ts', 'make_zero',
                '-muxpreload', '0',
                '-muxdelay', '0',
                '-max_interleave_delta', '0'
            ])
        
        # Output configuration
        if SEGMENT_TIME is not None:
            # Segmented output
            # Safe filename parsing - handle files without enough dashes
            parts = filename.rsplit('-', maxsplit=2)
            base = parts[0] if parts else os.path.splitext(filename)[0]
            
            cmd.extend([
                '-f', 'segment',
                '-segment_format', SEGMENT_FMT,
                '-segment_time', str(SEGMENT_TIME),
                '-strftime', '1'
            ])
            
            # MP4 segment options - CRITICAL FIX: No faststart!
            if MUX == 'mp4':
                # Use correct syntax without '=' prefix and no faststart
                cmd.extend(['-segment_format_options', fmt['segment_movflags']])
            
            pattern_ext = EXT_SINGLE
            cmd.append(f'{base}-%Y%m%d-%H%M%S{pattern_ext}')
        else:
            # Single file output
            out_path = os.path.splitext(filename)[0] + EXT_SINGLE
            
            if MUX == 'ts':
                cmd.extend(['-f', 'mpegts', out_path])
            elif MUX == 'mkv':
                cmd.extend(['-f', 'matroska', out_path])
            else:
                # MP4 single file - CRITICAL FIX: fragmented ONLY (no faststart)
                # faststart is incompatible with live fragmented streaming!
                cmd.extend(['-f', 'mp4', '-movflags', fmt['movflags'], out_path])
        
        return cmd
    
    # Stall tracking
    stall_count = 0
    last_agg_log_time = 0.0
    
    def log_stall(message):
        nonlocal stall_count, last_agg_log_time
        stall_count += 1
        if stall_count <= FFSettings.STALL_LOG_SUPPRESS_AFTER:
            self.logger.error(message)
        else:
            now = time.monotonic()
            if now - last_agg_log_time >= FFSettings.AGG_LOG_INTERVAL_SEC:
                self.logger.error(f"Stalls x{stall_count} (last: {message})")
                last_agg_log_time = now
    
    # Main loop state
    overall_success = False
    attempt = 0
    consecutive_stalls = 0
    ever_started = False
    
    self.last_ffmpeg_stats = {
        'attempts': 0,
        'stalls': 0,
        'last_reason': None,
        'started_at': time.time()
    }
    
    def tail_debug(lines):
        """Output last N lines for debugging."""
        if not DEBUG:
            return
        for ln in lines[-FFSettings.DEBUG_TAIL_LINES:]:
            l = ln.strip()
            if l:
                self.logger.debug('[ffmpeg] ' + l)
    
    # Main restart loop
    while attempt < FFSettings.MAX_RESTARTS_ON_STALL and not stopping.stop:
        attempt += 1
        self.last_ffmpeg_stats['attempts'] = attempt
        stalled = False
        stall_reason = None
        
        cmd = build_cmd()
        if DEBUG:
            try:
                dbg = " ".join(shlex_quote(c) for c in cmd)
                self.logger.debug("FFmpeg CMD attempt %d: %s", attempt, dbg)
            except Exception:
                pass
        
        # Start FFmpeg process
        process = None
        try:
            startupinfo = None
            creationflags = 0
            if sys.platform == 'win32':
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
            
            process = subprocess.Popen(
                args=cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                universal_newlines=True,
                bufsize=0,
                startupinfo=startupinfo,
                creationflags=creationflags
            )
            ever_started = True
        except OSError as e:
            self.logger.error(f"FFmpeg start failed: {e}")
            break
        
        # Monitoring state
        attempt_start = time.monotonic()
        last_pts_value = None
        last_pts_change = attempt_start
        last_stderr_activity = attempt_start
        last_output_growth = attempt_start
        last_output_size = 0
        low_speed_start = None
        consecutive_skips = 0
        suspect_trigger = None
        
        out_path = None
        if SEGMENT_TIME is None:
            out_path = os.path.splitext(filename)[0] + EXT_SINGLE
        
        ring = collections.deque(maxlen=FFSettings.DEBUG_TAIL_LINES)
        
        # Monitor FFmpeg output
        while process.poll() is None and not stopping.stop:
            line = process.stderr.readline()
            now = time.monotonic()
            
            if line:
                ring.append(line)
                if len(line) >= FFSettings.STDERR_MIN_APPEND_BYTES:
                    last_stderr_activity = now
                
                s = line.strip()
                
                # Parse time/PTS
                m = time_pattern.search(s)
                if m:
                    h, mm, ss, frac = m.groups()
                    pts = int(h) * 3600 + int(mm) * 60 + int(ss)
                    if frac:
                        pts += float("0." + frac)
                    if last_pts_value is None or pts > last_pts_value + 0.0001:
                        last_pts_value = pts
                        last_pts_change = now
                
                # Parse speed
                m = speed_pattern.search(s)
                if m:
                    spd = float(m.group(1))
                    if spd < FFSettings.SPEED_LOW_THRESHOLD:
                        if low_speed_start is None:
                            low_speed_start = now
                    else:
                        low_speed_start = None
                
                # Check for single large lag
                m = lag_pattern.search(s)
                if m and (now - attempt_start) > FFSettings.STARTUP_GRACE_SEC:
                    lag_s = float(m.group(1))
                    if lag_s >= FFSettings.MAX_SINGLE_LAG_SEC:
                        stalled = True
                        stall_reason = f"lag {lag_s:.2f}s"
                        break
                
                # Check for consecutive skip messages
                if skip_pattern.search(s):
                    consecutive_skips += 1
                    if consecutive_skips >= FFSettings.MAX_CONSEC_SKIP_LINES and \
                       (now - attempt_start) > FFSettings.STARTUP_GRACE_SEC:
                        stalled = True
                        stall_reason = f"skip lines {consecutive_skips}"
                        break
                else:
                    consecutive_skips = 0
            else:
                time.sleep(FFSettings.LOOP_SLEEP_SEC)
            
            # Check output file growth
            if SEGMENT_TIME is None and out_path:
                if os.path.exists(out_path):
                    try:
                        sz = os.path.getsize(out_path)
                        if sz > last_output_size:
                            last_output_size = sz
                            last_output_growth = now
                    except OSError:
                        pass
            else:
                # Segmented output - check for any recent segments
                if (now - last_output_growth) > 1.0:
                    parts = filename.rsplit('-', maxsplit=2)
                    base_prefix = parts[0] if parts and SEGMENT_TIME is not None else None
                    if base_prefix:
                        try:
                            for entry in os.scandir('.'):
                                if not entry.is_file():
                                    continue
                                if not entry.name.endswith(EXT_SINGLE):
                                    continue
                                if base_prefix not in entry.name:
                                    continue
                                if entry.stat().st_size > 300:
                                    last_output_growth = now
                                    break
                        except Exception:
                            pass
            
            # Periodic playlist probe
            if FFSettings.PLAYLIST_PROBE_ENABLED and \
               (now - last_playlist_change > FFSettings.PLAYLIST_PROBE_INTERVAL_SEC):
                probe_playlist()
            
            # Skip checks during startup grace period
            if (now - attempt_start) < FFSettings.STARTUP_GRACE_SEC:
                continue
            
            # Frozen PTS check
            if last_pts_value is not None and \
               (now - last_pts_change) > FFSettings.STALL_SAME_TIME_SEC:
                stalled = True
                stall_reason = f"frozen pts {int(now - last_pts_change)}s"
                break
            
            # Sustained low speed check
            if low_speed_start is not None and \
               (now - low_speed_start) > FFSettings.SPEED_LOW_SUSTAIN_SEC:
                stalled = True
                stall_reason = f"sustained low speed {int(now - low_speed_start)}s"
                break
            
            # Triple inactivity detection (suspect → confirm)
            no_size = (now - last_output_growth) > FFSettings.SUSPECT_STALL_SEC
            no_stderr = (now - last_stderr_activity) > FFSettings.SUSPECT_STALL_SEC
            no_pts = (last_pts_value is None) or \
                     ((now - last_pts_change) > FFSettings.SUSPECT_STALL_SEC)
            
            if (no_size and no_stderr and no_pts) and suspect_trigger is None:
                suspect_trigger = now
            
            if suspect_trigger is not None:
                elapsed = now - suspect_trigger
                head_recent = (now - last_playlist_change) < FFSettings.SUSPECT_STALL_SEC
                needed = FFSettings.CONFIRM_STALL_EXTRA_SEC if head_recent else 0
                
                if elapsed >= needed:
                    no_size2 = (now - last_output_growth) > FFSettings.SUSPECT_STALL_SEC
                    no_stderr2 = (now - last_stderr_activity) > FFSettings.SUSPECT_STALL_SEC
                    no_pts2 = (last_pts_value is None) or \
                              ((now - last_pts_change) > FFSettings.SUSPECT_STALL_SEC)
                    
                    if no_size2 and no_stderr2 and no_pts2:
                        stalled = True
                        stall_reason = "triple inactivity"
                        break
            
            # Fallback generic inactivity check
            if ((now - last_output_growth) > FFSettings.FALLBACK_NO_OUTPUT_SEC and
                (now - last_stderr_activity) > FFSettings.FALLBACK_NO_STDERR_SEC):
                stalled = True
                stall_reason = "generic inactivity"
                break
        
        # Handle stop request
        if stopping.stop:
            if process and process.poll() is None:
                try:
                    if sys.platform == 'win32':
                        try:
                            process.send_signal(signal.CTRL_BREAK_EVENT)
                        except Exception:
                            pass
                    process.wait(timeout=FFSettings.GRACEFUL_QUIT_TIMEOUT_SEC)
                except subprocess.TimeoutExpired:
                    try:
                        process.kill()
                    except Exception:
                        pass
            overall_success = process.returncode in FFSettings.RETCODE_OK
            tail_debug(list(ring))
            break
        
        ret = process.returncode if process else -1
        tail_debug(list(ring))
        
        # Handle stall
        if stalled:
            consecutive_stalls += 1
            self.last_ffmpeg_stats['stalls'] = consecutive_stalls
            self.last_ffmpeg_stats['last_reason'] = stall_reason
            log_stall(f"Stall ({stall_reason}); restarting (attempt {attempt}/{FFSettings.MAX_RESTARTS_ON_STALL})")
            
            # Cooldown after multiple consecutive stalls
            if consecutive_stalls >= FFSettings.COOLDOWN_AFTER_CONSEC_STALLS:
                log_stall(f"Cooldown {FFSettings.COOLDOWN_SLEEP_SEC}s (playlist {'stale' if playlist_stale() else 'active'})")
                end_cd = time.monotonic() + FFSettings.COOLDOWN_SLEEP_SEC
                while time.monotonic() < end_cd and not stopping.stop:
                    time.sleep(1)
                    if FFSettings.PLAYLIST_PROBE_ENABLED and \
                       (time.monotonic() - last_playlist_change > FFSettings.PLAYLIST_PROBE_INTERVAL_SEC):
                        probe_playlist()
                consecutive_stalls = 0
            continue
        else:
            consecutive_stalls = 0
        
        # Handle normal exit
        if ret in FFSettings.RETCODE_OK:
            overall_success = True
            self.logger.info("FFmpeg ended normally.")
            break
        else:
            log_stall(f"Abnormal exit code {ret}; restarting (attempt {attempt}/{FFSettings.MAX_RESTARTS_ON_STALL})")
            continue
    
    self.stopDownload = None
    return overall_success if ever_started else False