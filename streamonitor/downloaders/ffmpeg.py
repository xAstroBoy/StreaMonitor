import errno
import os
import subprocess
import sys
import signal

import requests.cookies
from threading import Thread
from parameters import DEBUG, SEGMENT_TIME, CONTAINER, FFMPEG_PATH


def getVideoFfmpeg(self, url, filename):
    cmd = [
        FFMPEG_PATH,
        '-user_agent', self.headers['User-Agent']
    ]

    if type(self.cookies) is requests.cookies.RequestsCookieJar:
        cookies_text = ''
        for cookie in self.cookies:
            cookies_text += cookie.name + "=" + cookie.value + "; path=" + cookie.path + '; domain=' + cookie.domain + '\n'
        if len(cookies_text) > 10:
            cookies_text = cookies_text[:-1]
        cmd.extend([
            '-cookies', cookies_text
        ])

    cmd.extend([
        '-readrate', '1.3',
        '-reconnect', '1',
        '-reconnect_streamed', '1',
        '-reconnect_delay_max', '2',
        '-timeout', '15000000',
        '-max_reload', '50',
        '-seg_max_retry', '50',
        '-m3u8_hold_counters', '20',
        '-probesize', '5000000',
        '-analyzeduration', '7000000',
        '-i', url,
        '-c:a', 'copy',
        '-c:v', 'copy',
    ])

    suffix = ''

    if SEGMENT_TIME is not None:
        username = filename.rsplit('-', maxsplit=2)[0]
        cmd.extend([
            '-f', 'segment',
            '-reset_timestamps', '1',
            '-segment_time', str(SEGMENT_TIME),
            '-strftime', '1',
            f'{username}-%Y%m%d-%H%M%S{suffix}.{CONTAINER}'
        ])
    else:
        cmd.extend([
            '-movflags', '+frag_keyframe+empty_moov',
            '-f', 'mp4',
            os.path.splitext(filename)[0] + suffix + '.' + CONTAINER
        ])

    class _Stopper:
        def __init__(self):
            self.stop = False
        def pls_stop(self):
            self.stop = True

    stopping = _Stopper()
    error = False
    process = None

    def execute():
        nonlocal error, process
        try:
            stderr_path = filename + '.stderr.log'
            stderr = open(stderr_path, 'w+', encoding='utf-8') if DEBUG else subprocess.DEVNULL
            startupinfo = None
            creationflags = 0
            if sys.platform == "win32":
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                creationflags = subprocess.CREATE_NEW_PROCESS_GROUP

            process = subprocess.Popen(
                args=cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=stderr,
                startupinfo=startupinfo,
                creationflags=creationflags
            )
        except OSError as e:
            self.logger.error(f"FFMpeg start failed: {e}")
            error = True
            return

        while process.poll() is None:
            if stopping.stop:
                try:
                    process.communicate(b'q', timeout=5)
                except subprocess.TimeoutExpired:
                    self.logger.warning("Graceful stop failed, killing ffmpeg process")
                    try:
                        if sys.platform == "win32":
                            process.send_signal(signal.CTRL_BREAK_EVENT)
                        else:
                            process.terminate()
                        process.wait(timeout=3)
                    except Exception as kill_err:
                        self.logger.error(f"Force kill failed: {kill_err}")
                        process.kill()
                break
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass

        if process.returncode not in (0, 255):
            self.logger.error(f'FFMpeg exited with error. Return code: {process.returncode}')
            error = True

        if DEBUG:
            try:
                with open(stderr_path, 'r', encoding='utf-8') as f:
                    lines = f.readlines()
                    shown_skips = 0
                    for line in lines:
                        if shown_skips >= 5:
                            break
                        line = line.strip()
                        if '[hls]' in line and 'skipping' in line and 'expired' in line:
                            self.logger.warning('[ffmpeg HLS] ' + line)
                            shown_skips += 1
                        elif '[ffmpeg]' not in line and shown_skips < 5:
                            self.logger.debug('[ffmpeg] ' + line)
            except Exception as e:
                self.logger.warning(f"Failed to read stderr log: {e}")

    thread = Thread(target=execute)
    thread.start()
    self.stopDownload = lambda: stopping.pls_stop()
    thread.join()
    self.stopDownload = None
    return not error
