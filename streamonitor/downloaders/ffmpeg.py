import errno
import os
import subprocess
import sys
import signal

import requests.cookies
from threading import Thread
from parameters import DEBUG, SEGMENT_TIME, CONTAINER, FFMPEG_PATH
import os, sys, time, subprocess, signal, re, collections


def getVideoFfmpeg(self, url, filename):
    """
    Live HLS recorder with adaptive stall detection.
    Writes using CONTAINER from parameters (recommended: 'mkv' for safety).
    Remux/concat handled elsewhere.
    Returns True on overall success, False otherwise.
    """
    import time, subprocess, sys, signal, re, collections, os
    import requests.cookies

    LIVE_LAST_SEGMENTS           = 3
    RW_TIMEOUT_SEC               = 5
    SOCKET_TIMEOUT_SEC           = 5
    RECONNECT_DELAY_MAX          = 10
    MAX_RESTARTS_ON_STALL        = 8
    GRACEFUL_QUIT_TIMEOUT_SEC    = 6

    STARTUP_GRACE_SEC            = 10
    SUSPECT_STALL_SEC            = 25
    CONFIRM_STALL_EXTRA_SEC      = 10
    STALL_SAME_TIME_SEC          = 20
    SPEED_LOW_THRESHOLD          = 0.80
    SPEED_LOW_SUSTAIN_SEC        = 25
    MAX_SINGLE_LAG_SEC           = 12
    MAX_CONSEC_SKIP_LINES        = 5
    FALLBACK_NO_STDERR_SEC       = 35
    FALLBACK_NO_OUTPUT_SEC       = 35

    STALL_LOG_SUPPRESS_AFTER     = 4
    AGG_LOG_INTERVAL_SEC         = 120
    COOLDOWN_AFTER_CONSEC_STALLS = 3
    COOLDOWN_SLEEP_SEC           = 60

    STDERR_MIN_APPEND_BYTES      = 24
    LOOP_SLEEP_SEC               = 0.15

    RETCODE_OK                   = (0, 255)

    PROBESIZE                    = "4M"
    ANALYSEDURATION_US           = "10000000"
    ENABLE_FFLAGS_NOBUFFER       = True
    ENABLE_FFLAGS_DISCARDCORRUPT = True
    USE_GENPTS                   = True
    USE_PROGRESS_FORCED_STATS    = False

    PLAYLIST_PROBE_ENABLED       = True
    PLAYLIST_PROBE_INTERVAL_SEC  = 8
    PLAYLIST_STALE_THRESHOLD_SEC = 60

    DEBUG_TAIL_LINES             = 120

    MUX = (CONTAINER or "mkv").lower().strip()
    if MUX not in ("ts","mkv","mp4"):
        MUX = "mkv"
    EXT_SINGLE = {"ts": ".ts", "mkv": ".mkv", "mp4": ".mp4"}[MUX]
    SEGMENT_FMT = {"ts": "mpegts", "mkv": "matroska", "mp4": "mp4"}[MUX]

    if not hasattr(self, '_hls_help_cache'):
        try:
            self._hls_help_cache = subprocess.check_output(
                [FFMPEG_PATH, '-hide_banner', '-h', 'demuxer=hls'],
                stderr=subprocess.STDOUT, timeout=5, text=True, errors='replace'
            )
        except Exception:
            self._hls_help_cache = ""
    def hls_supports(opt): return opt in self._hls_help_cache

    class _Stopper:
        def __init__(self): self.stop = False
        def pls_stop(self): self.stop = True
    stopping = _Stopper()
    self.stopDownload = stopping.pls_stop

    time_pattern  = re.compile(r"time=(\d+):(\d+):(\d+)(?:\.(\d+))?")
    speed_pattern = re.compile(r"speed=\s*([0-9.]+)x")
    lag_pattern   = re.compile(r"after a lag of ([0-9.]+)s")
    skip_pattern  = re.compile(r"skipping\s+\d+\s+segments ahead")

    last_playlist_id = None
    last_playlist_change = time.monotonic()
    if PLAYLIST_PROBE_ENABLED:
        import requests as _req
        def probe_playlist():
            nonlocal last_playlist_id, last_playlist_change
            try:
                r = _req.get(url, headers={"User-Agent": self.headers.get('User-Agent','Mozilla/5.0')},
                             timeout=5, verify=False)
                if r.status_code != 200: return
                txt = r.text
                pid = None
                for line in txt.splitlines():
                    if line.startswith("#EXT-X-MEDIA-SEQUENCE:"):
                        pid = "SEQ:" + line.split(":",1)[1].strip()
                        break
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
        def probe_playlist(): pass

    def playlist_stale():
        return (time.monotonic() - last_playlist_change) > PLAYLIST_STALE_THRESHOLD_SEC

    def _compose_headers():
        try:
            hdrs = dict(self.headers or {})
        except Exception:
            hdrs = {}
        ua = hdrs.pop("User-Agent", None)
        header_lines = ""
        for k,v in hdrs.items():
            if not k or v is None: continue
            header_lines += f"{k}: {v}\r\n"
        return ua or "Mozilla/5.0", header_lines

    def _compose_cookies():
        if isinstance(self.cookies, requests.cookies.RequestsCookieJar):
            parts = []
            for c in self.cookies:
                parts.append(f"{c.name}={c.value}")
            if parts:
                return "; ".join(parts)
        elif isinstance(self.cookies, dict):
            parts = [f"{k}={v}" for k,v in self.cookies.items()]
            if parts:
                return "; ".join(parts)
        return None

    def build_cmd():
        ua, extra_headers = _compose_headers()
        cmd = [FFMPEG_PATH, '-hide_banner', '-loglevel', 'info', '-user_agent', ua]
        if extra_headers:
            # Include all non-UA headers (one per line, CRLF-separated)
            cmd.extend(['-headers', extra_headers])

        ck = _compose_cookies()
        if ck:
            cmd.extend(['-cookies', ck])

        rw_us   = str(int(RW_TIMEOUT_SEC * 1_000_000))
        sock_us = str(int(SOCKET_TIMEOUT_SEC * 1_000_000))
        cmd.extend([
            '-timeout', sock_us,
            '-rw_timeout', rw_us,
            '-reconnect', '1',
            '-reconnect_streamed', '1',
            '-reconnect_on_network_error', '1',
            '-reconnect_on_http_error', '4xx,5xx',
            '-reconnect_at_eof', '1',
            '-reconnect_delay_max', str(RECONNECT_DELAY_MAX)
        ])

        if LIVE_LAST_SEGMENTS is not None and hls_supports('live_start_index'):
            cmd.extend(['-live_start_index', f'-{LIVE_LAST_SEGMENTS}'])
        if hls_supports('max_reload'):
            cmd.extend(['-max_reload', '30'])
        if hls_supports('seg_max_retry'):
            cmd.extend(['-seg_max_retry', '30'])
        if hls_supports('m3u8_hold_counters'):
            cmd.extend(['-m3u8_hold_counters', '30'])

        if ENABLE_FFLAGS_NOBUFFER:
            cmd.extend(['-fflags', 'nobuffer'])
        if ENABLE_FFLAGS_DISCARDCORRUPT:
            cmd.extend(['-fflags', '+discardcorrupt'])
        if USE_GENPTS:
            cmd.extend(['-fflags', '+genpts'])

        cmd.extend(['-probesize', PROBESIZE, '-analyzeduration', ANALYSEDURATION_US])

        if USE_PROGRESS_FORCED_STATS:
            cmd.extend(['-progress', 'pipe:2', '-stats_period', '5'])

        # Input
        cmd.extend(['-i', url])

        # Maps (video optional, audio optional)
        cmd.extend(['-map', '0:v:0?', '-map', '0:a?', '-dn', '-sn'])

        # Stream copy
        cmd.extend(['-c', 'copy'])

        # >>> IMPORTANT FIXES FOR MP4 <<<
        # 1) Never use -copyinkf here (causes MP4 muxer errors on live HLS).
        # 2) If writing MP4, convert AAC ADTS -> ASC.
        if MUX == 'mp4':
            cmd.extend(['-bsf:a', 'aac_adtstoasc'])

        # Common mux tuning
        if MUX in ('mkv', 'mp4'):
            cmd.extend(['-avoid_negative_ts', 'make_zero',
                        '-muxpreload', '0', '-muxdelay', '0', '-max_interleave_delta', '0'])

        # Output(s)
        if SEGMENT_TIME is not None:
            base = filename.rsplit('-', maxsplit=2)[0]
            cmd.extend([
                '-f', 'segment',
                '-segment_format', SEGMENT_FMT,
                '-reset_timestamps', '1',
                '-segment_time', str(SEGMENT_TIME),
                '-strftime', '1'
            ])
            if MUX == 'mp4':
                # Pass mp4 flags to the INNER mp4 muxer via segment_format_options
                cmd.extend(['-segment_format_options', 'movflags=+frag_keyframe+empty_moov+faststart'])
            pattern_ext = EXT_SINGLE
            cmd.append(f'{base}-%Y%m%d-%H%M%S{pattern_ext}')
        else:
            out_path = os.path.splitext(filename)[0] + EXT_SINGLE
            if MUX == 'ts':
                cmd.extend(['-f', 'mpegts', out_path])
            elif MUX == 'mkv':
                cmd.extend(['-f', 'matroska', out_path])
            else:
                # MP4 single file: fragmented for live-friendly writing
                cmd.extend(['-f', 'mp4', '-movflags', '+frag_keyframe+empty_moov+faststart', out_path])

        return cmd


    stall_count = 0
    last_agg_log_time = 0.0
    def log_stall(message):
        nonlocal stall_count, last_agg_log_time
        stall_count += 1
        if stall_count <= STALL_LOG_SUPPRESS_AFTER:
            self.logger.error(message)
        else:
            now = time.monotonic()
            if now - last_agg_log_time >= AGG_LOG_INTERVAL_SEC:
                self.logger.error(f"Stalls x{stall_count} (last: {message})")
                last_agg_log_time = now

    overall_success = False
    attempt = 0
    consecutive_stalls = 0
    ever_started = False

    self.last_ffmpeg_stats = {'attempts': 0, 'stalls': 0, 'last_reason': None, 'started_at': time.time()}

    def tail_debug(lines):
        if not DEBUG: return
        for ln in lines[-DEBUG_TAIL_LINES:]:
            l = ln.strip()
            if l:
                self.logger.debug('[ffmpeg] ' + l)

    while attempt < MAX_RESTARTS_ON_STALL and not stopping.stop:
        attempt += 1
        self.last_ffmpeg_stats['attempts'] = attempt
        stalled = False
        stall_reason = None

        cmd = build_cmd()
        if DEBUG:
            try:
                dbg = " ".join(shlex_quote(c) if ' ' in c else c for c in cmd)
                self.logger.debug("FFmpeg CMD attempt %d: %s", attempt, dbg)
            except Exception:
                pass

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

        ring = collections.deque(maxlen=DEBUG_TAIL_LINES)

        while process.poll() is None and not stopping.stop:
            line = process.stderr.readline()
            now = time.monotonic()
            if line:
                ring.append(line)
                if len(line) >= STDERR_MIN_APPEND_BYTES:
                    last_stderr_activity = now
                s = line.strip()

                m = time_pattern.search(s)
                if m:
                    h, mm, ss, frac = m.groups()
                    pts = int(h)*3600 + int(mm)*60 + int(ss)
                    if frac: pts += float("0."+frac)
                    if last_pts_value is None or pts > last_pts_value + 0.0001:
                        last_pts_value = pts
                        last_pts_change = now

                m = speed_pattern.search(s)
                if m:
                    spd = float(m.group(1))
                    if spd < SPEED_LOW_THRESHOLD:
                        if low_speed_start is None:
                            low_speed_start = now
                    else:
                        low_speed_start = None

                m = lag_pattern.search(s)
                if m and (now - attempt_start) > STARTUP_GRACE_SEC:
                    lag_s = float(m.group(1))
                    if lag_s >= MAX_SINGLE_LAG_SEC:
                        stalled = True
                        stall_reason = f"lag {lag_s:.2f}s"
                        break

                if skip_pattern.search(s):
                    consecutive_skips += 1
                    if consecutive_skips >= MAX_CONSEC_SKIP_LINES and (now - attempt_start) > STARTUP_GRACE_SEC:
                        stalled = True
                        stall_reason = f"skip lines {consecutive_skips}"
                        break
                else:
                    consecutive_skips = 0
            else:
                time.sleep(LOOP_SLEEP_SEC)

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
                if (now - last_output_growth) > 1.0:
                    base_prefix = filename.rsplit('-', maxsplit=2)[0] if SEGMENT_TIME is not None else None
                    if base_prefix:
                        try:
                            for entry in os.scandir('.'):
                                if not entry.is_file(): continue
                                if not entry.name.endswith(EXT_SINGLE): continue
                                if base_prefix not in entry.name: continue
                                if entry.stat().st_size > 300:
                                    last_output_growth = now
                                    break
                        except Exception:
                            pass

            if PLAYLIST_PROBE_ENABLED and (now - last_playlist_change > PLAYLIST_PROBE_INTERVAL_SEC):
                probe_playlist()

            if (now - attempt_start) < STARTUP_GRACE_SEC:
                continue

            if last_pts_value is not None and (now - last_pts_change) > STALL_SAME_TIME_SEC:
                stalled = True
                stall_reason = f"frozen pts {int(now - last_pts_change)}s"
                break

            if low_speed_start is not None and (now - low_speed_start) > SPEED_LOW_SUSTAIN_SEC:
                stalled = True
                stall_reason = f"sustained low speed {int(now - low_speed_start)}s"
                break

            no_size   = (now - last_output_growth) > SUSPECT_STALL_SEC
            no_stderr = (now - last_stderr_activity) > SUSPECT_STALL_SEC
            no_pts    = (last_pts_value is None) or ((now - last_pts_change) > SUSPECT_STALL_SEC)

            if (no_size and no_stderr and no_pts) and suspect_trigger is None:
                suspect_trigger = now
            if suspect_trigger is not None:
                elapsed = now - suspect_trigger
                head_recent = (now - last_playlist_change) < SUSPECT_STALL_SEC
                needed = CONFIRM_STALL_EXTRA_SEC if head_recent else 0
                if elapsed >= needed:
                    no_size2   = (now - last_output_growth) > SUSPECT_STALL_SEC
                    no_stderr2 = (now - last_stderr_activity) > SUSPECT_STALL_SEC
                    no_pts2    = (last_pts_value is None) or ((now - last_pts_change) > SUSPECT_STALL_SEC)
                    if no_size2 and no_stderr2 and no_pts2:
                        stalled = True
                        stall_reason = "triple inactivity"
                        break

            if ((now - last_output_growth) > FALLBACK_NO_OUTPUT_SEC and
                (now - last_stderr_activity) > FALLBACK_NO_STDERR_SEC):
                stalled = True
                stall_reason = "generic inactivity"
                break

        if stopping.stop:
            if process and process.poll() is None:
                try:
                    if sys.platform == 'win32':
                        try: process.send_signal(signal.CTRL_BREAK_EVENT)
                        except Exception: pass
                    process.wait(timeout=GRACEFUL_QUIT_TIMEOUT_SEC)
                except subprocess.TimeoutExpired:
                    try: process.kill()
                    except Exception: pass
            overall_success = process.returncode in RETCODE_OK
            tail_debug(list(ring))
            break

        ret = process.returncode if process else -1
        tail_debug(list(ring))

        if stalled:
            consecutive_stalls += 1
            self.last_ffmpeg_stats['stalls'] = consecutive_stalls
            self.last_ffmpeg_stats['last_reason'] = stall_reason
            log_stall(f"Stall ({stall_reason}); restarting (attempt {attempt}/{MAX_RESTARTS_ON_STALL})")
            if consecutive_stalls >= COOLDOWN_AFTER_CONSEC_STALLS:
                log_stall(f"Cooldown {COOLDOWN_SLEEP_SEC}s (playlist {'stale' if playlist_stale() else 'active'})")
                end_cd = time.monotonic() + COOLDOWN_SLEEP_SEC
                while time.monotonic() < end_cd and not stopping.stop:
                    time.sleep(1)
                    if PLAYLIST_PROBE_ENABLED and (time.monotonic() - last_playlist_change > PLAYLIST_PROBE_INTERVAL_SEC):
                        probe_playlist()
                consecutive_stalls = 0
            continue
        else:
            consecutive_stalls = 0

        if ret in RETCODE_OK:
            overall_success = True
            self.logger.info("FFmpeg ended normally.")
            break
        else:
            log_stall(f"Abnormal exit code {ret}; restarting (attempt {attempt}/{MAX_RESTARTS_ON_STALL})")
            continue

    self.stopDownload = None
    return overall_success if ever_started else False


def shlex_quote(s):
    import re
    if re.match(r'^[a-zA-Z0-9._/\-+=:@%]+$', s):
        return s
    return "'" + s.replace("'", "'\"'\"'") + "'"
