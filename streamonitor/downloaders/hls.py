import m3u8
import os
import re
import subprocess
from threading import Thread
from time import sleep
from urllib.parse import urljoin
from ffmpy import FFmpeg, FFRuntimeError
from parameters import DEBUG, CONTAINER, SEGMENT_TIME, FFMPEG_PATH
import requests

def getVideoNativeHLS(self, url, filename, m3u_processor=None):
    self.stopDownloadFlag = False
    error = False

    session = requests.Session()
    session.headers.update(self.headers or {})
    # helper: make absolute URL
    def abs_url(base, maybe_rel):
        return maybe_rel if maybe_rel.startswith(("http://", "https://")) else urljoin(base, maybe_rel)

    # 1) Resolve master playlist to a concrete media playlist (highest bandwidth)
    r0 = session.get(url, headers=self.headers, cookies=self.cookies)
    if r0.status_code != 200:
        self.debug(f"[HLS] master fetch failed: {r0.status_code}")
        return False
    content0 = r0.content.decode("utf-8", errors="ignore")
    if m3u_processor:
        content0 = m3u_processor(content0)
    pl0 = m3u8.loads(content0)
    if getattr(pl0, "is_variant", False) and getattr(pl0, "playlists", []):
        # pick highest bandwidth
        best = max(pl0.playlists, key=lambda p: getattr(p.stream_info, "bandwidth", 0))
        url = abs_url(url, best.uri)

    # 2) Peek the concrete media playlist to decide container & init map
    r1 = session.get(url, headers=self.headers, cookies=self.cookies)
    if r1.status_code != 200:
        self.debug(f"[HLS] media fetch failed: {r1.status_code}")
        return False
    content1 = r1.content.decode("utf-8", errors="ignore")
    if m3u_processor:
        content1 = m3u_processor(content1)

    # detect encryption and fMP4
    is_encrypted = "#EXT-X-KEY" in content1
    has_map = "#EXT-X-MAP" in content1
    looks_fmp4 = has_map or ".m4s" in content1 or "#EXT-X-PART" in content1

    # If encrypted, let ffmpeg handle the playlist directly (passes headers & cookies)
    if is_encrypted:
        try:
            hdr_lines = []
            if self.headers:
                for k, v in self.headers.items():
                    hdr_lines.append(f"{k}: {v}")
            if self.cookies:
                cookie_str = "; ".join([f"{c.name}={c.value}" for c in session.cookies])
                hdr_lines.append(f"Cookie: {cookie_str}")
            headers_opt = ""
            if hdr_lines:
                headers_opt = f'-headers "{("\\r\\n".join(hdr_lines))}\\r\\n"'
            out_opts = "-c copy"
            if SEGMENT_TIME is not None:
                out_opts += f" -f segment -reset_timestamps 1 -segment_time {SEGMENT_TIME}"
                out_name = filename[:-len('.' + CONTAINER)] + '_%03d.' + CONTAINER
            else:
                out_name = filename
            ff = FFmpeg(
                executable=FFMPEG_PATH,
                inputs={url: f'{headers_opt} -protocol_whitelist file,http,https,tcp,tls,crypto'},
                outputs={out_name: out_opts},
            )
            stdout = open(filename + '.postprocess_stdout.log', 'w+') if DEBUG else subprocess.DEVNULL
            stderr = open(filename + '.postprocess_stderr.log', 'w+') if DEBUG else subprocess.DEVNULL
            ff.run(stdout=stdout, stderr=stderr)
            return True
        except FFRuntimeError as e:
            if e.exit_code and e.exit_code != 255:
                return False
            return True

    # 3) Non-encrypted: pull segments ourselves (fMP4 vs TS)
    tmp_ext = ".tmp.mp4" if looks_fmp4 else ".tmp.ts"
    tmpfilename = filename[:-len('.' + CONTAINER)] + tmp_ext

    downloaded = set()
    wrote_anything = False

    def execute():
        nonlocal error, wrote_anything, content1
        # If fMP4 with EXT-X-MAP, fetch init once up-front
        if looks_fmp4:
            m = re.search(r'#EXT-X-MAP:.*URI="([^"]+)"', content1)
            if m:
                init_uri = abs_url(url, m.group(1))
                if init_uri not in downloaded:
                    dr = session.get(init_uri, headers=self.headers, cookies=self.cookies, stream=True)
                    if dr.status_code == 200:
                        with open(tmpfilename, 'ab') as f:
                            for chunk in dr.iter_content(chunk_size=256*1024):
                                if not chunk: break
                                f.write(chunk)
                        downloaded.add(init_uri)
                        wrote_anything = True
                    else:
                        error = True
                        return

        # main polling loop
        base = url
        while not self.stopDownloadFlag:
            try:
                r = session.get(base, headers=self.headers, cookies=self.cookies)
                if r.status_code != 200:
                    sleep(2)
                    continue
                content = r.content.decode("utf-8", errors="ignore")
                if m3u_processor:
                    content = m3u_processor(content)
                pl = m3u8.loads(content)
                segs = getattr(pl, "segments", [])
                if not segs:
                    # live edge: wait a bit
                    sleep(2)
                    continue

                did_download = False  # reset each iteration

                for seg in segs:
                    # m3u8 library exposes .uri; build absolute ourselves
                    seg_uri = abs_url(base, seg.uri)
                    if seg_uri in downloaded:
                        continue
                    # fetch
                    m = session.get(seg_uri, headers=self.headers, cookies=self.cookies, stream=True)
                    if m.status_code != 200:
                        # transient: try later
                        continue
                    with open(tmpfilename, 'ab') as outfile:
                        for chunk in m.iter_content(chunk_size=256*1024):
                            if not chunk: break
                            outfile.write(chunk)
                    downloaded.add(seg_uri)
                    wrote_anything = True
                    did_download = True
                    self.debug(f"[HLS] + {seg_uri}")
                    if self.stopDownloadFlag:
                        return

                if not did_download:
                    sleep(2)
            except Exception as ex:
                self.debug(f"[HLS] loop error: {ex}")
                sleep(2)

    def terminate():
        self.stopDownloadFlag = True

    # run downloader thread
    process = Thread(target=execute, daemon=True)
    process.start()
    self.stopDownload = terminate
    process.join()
    self.stopDownload = None

    if error or not wrote_anything:
        return False

    # 4) Post-processing: remux the buffer file to desired container
    try:
        stdout = open(filename + '.postprocess_stdout.log', 'w+') if DEBUG else subprocess.DEVNULL
        stderr = open(filename + '.postprocess_stderr.log', 'w+') if DEBUG else subprocess.DEVNULL
        out_opts = "-c:a copy -c:v copy"
        if SEGMENT_TIME is not None:
            out_opts += f" -f segment -reset_timestamps 1 -segment_time {SEGMENT_TIME}"
            out_name = filename[:-len('.' + CONTAINER)] + '_%03d.' + CONTAINER
        else:
            out_name = filename
        ff = FFmpeg(executable=FFMPEG_PATH, inputs={tmpfilename: None}, outputs={out_name: out_opts})
        ff.run(stdout=stdout, stderr=stderr)
        try:
            os.remove(tmpfilename)
        except OSError:
            pass
    except FFRuntimeError as e:
        if e.exit_code and e.exit_code != 255:
            return False

    return True
