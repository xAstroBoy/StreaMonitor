import os, re, m3u8, subprocess, time
from urllib.parse import urljoin
from parameters import DEBUG, CONTAINER, FFMPEG_PATH
from streamonitor.bot import Bot
# Always use stdlib requests here (avoids curl_cffi threadpool shutdown errors)
import requests


def getVideoNativeHLS(self: Bot, url, filename, m3u_processor=None):
    """
    Robust HLS capture that ALWAYS writes a growing N.tmp.ts during the live show.
    No mid-flight remux; no final remux here. The merger handles *.tmp.ts later.
    """

    # ───────────── config / helpers ─────────────
    self.stopDownloadFlag = False

    # expose a proper stop function for Bot
    def _stop():
        self.stopDownloadFlag = True
    self.stopDownload = _stop

    # we always write .tmp.ts (name only; content may be TS or fMP4 chunks)
    tmp_path = filename[:-len("." + CONTAINER)] + ".tmp.ts"
    os.makedirs(os.path.dirname(tmp_path), exist_ok=True)

    def abs_url(base, maybe_rel):
        return maybe_rel if maybe_rel.startswith(("http://", "https://")) else urljoin(base, maybe_rel)

    # sensible default headers if caller didn’t set them
    headers = dict(self.headers or {})
    headers.setdefault("User-Agent", headers.get("User-Agent") or "Mozilla/5.0")
    headers.setdefault("Accept", "*/*")
    headers.setdefault("Connection", "keep-alive")

    # guess Referer/Origin by site (fixes many 403s on HLS CDNs)
    site_ref_map = {
        "SC": "https://stripchat.com/",
        "SCVR": "https://stripchat.com/",
        "CB": "https://chaturbate.com/",
        "CS": "https://www.camsoda.com/",
    }
    ref = site_ref_map.get(getattr(self, "siteslug", ""), None)
    if ref:
        headers.setdefault("Referer", ref)
        headers.setdefault("Origin", ref.rstrip("/"))

    # keep one plain requests Session for this capture
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

    # ───────────── resolve variant (or fallback to ffmpeg on 403) ─────────────
    def fetch_text(u):
        r = sess.get(u, headers=headers, timeout=15)
        if r.status_code != 200:
            return None, r.status_code
        return r.content.decode("utf-8", errors="ignore"), 200

    text0, code0 = fetch_text(url)
    if code0 == 403:
        self.debug(f"[HLS] master fetch failed: {code0} (will try ffmpeg direct → .tmp.ts)")
        return _ffmpeg_dump_to_ts(self, url, headers, tmp_path)

    if text0 is None:
        self.debug(f"[HLS] master fetch failed: {code0}")
        return False

    if callable(m3u_processor):
        text0 = m3u_processor(text0)

    try:
        pl0 = m3u8.loads(text0)
    except Exception:
        pl0 = None

    if getattr(pl0, "is_variant", False) and getattr(pl0, "playlists", []):
        best = max(pl0.playlists, key=lambda p: (getattr(p.stream_info, "bandwidth", 0) or 0))
        url = abs_url(url, best.uri)

    # peek media playlist
    text1, code1 = fetch_text(url)
    if code1 == 403:
        self.debug(f"[HLS] media fetch failed: {code1} (will try ffmpeg direct → .tmp.ts)")
        return _ffmpeg_dump_to_ts(self, url, headers, tmp_path)
    if text1 is None:
        self.debug(f"[HLS] media fetch failed: {code1}")
        return False
    if callable(m3u_processor):
        text1 = m3u_processor(text1)

    is_encrypted = "#EXT-X-KEY" in text1
    has_map = "#EXT-X-MAP" in text1  # fMP4 init

    if is_encrypted:
        self.debug("[HLS] encrypted stream detected → ffmpeg capture to .tmp.ts")
        return _ffmpeg_dump_to_ts(self, url, headers, tmp_path)

    # ───────────── non-encrypted: pull segments ourselves into .tmp.ts ─────────
    downloaded = set()
    f = open(tmp_path, "ab", buffering=0)

    def write_bytes(b):
        if not b:
            return
        f.write(b)
        f.flush()
        try:
            os.fsync(f.fileno())
        except Exception:
            pass

    try:
        if has_map:
            m = re.search(r'#EXT-X-MAP:.*URI="([^"]+)"', text1)
            if m:
                init_uri = abs_url(url, m.group(1))
                if init_uri not in downloaded:
                    try:
                        r = sess.get(init_uri, headers=headers, stream=True, timeout=20)
                        if r.status_code == 200:
                            for chunk in r.iter_content(262144):
                                if not chunk:
                                    break
                                write_bytes(chunk)
                            downloaded.add(init_uri)
                            self.debug(f"[HLS] + init {init_uri}")
                        else:
                            self.debug(f"[HLS] init fetch failed: {r.status_code}")
                    except Exception as ex:
                        self.debug(f"[HLS] init fetch error: {ex}")

        def poll_once():
            r = sess.get(url, headers=headers, timeout=15)
            if r.status_code != 200:
                time.sleep(2)
                return False
            txt = r.content.decode("utf-8", errors="ignore")
            if callable(m3u_processor):
                txt = m3u_processor(txt)
            try:
                pl = m3u8.loads(txt)
            except Exception:
                time.sleep(2)
                return False

            segs = getattr(pl, "segments", []) or []
            did = False
            for seg in segs:
                if self.stopDownloadFlag or not self.running or self.quitting:
                    return True
                seg_uri = abs_url(url, seg.uri)
                if seg_uri in downloaded:
                    continue
                try:
                    rr = sess.get(seg_uri, headers=headers, stream=True, timeout=20)
                    if rr.status_code != 200:
                        continue
                    self.debug(f"[HLS] + {seg_uri}")
                    for chunk in rr.iter_content(262144):
                        if not chunk:
                            break
                        write_bytes(chunk)
                    downloaded.add(seg_uri)
                    did = True
                except Exception as ex:
                    self.debug(f"[HLS] segment fetch error: {ex}")
            return did

        while not self.stopDownloadFlag and self.running and not self.quitting:
            got = poll_once()
            if not got:
                time.sleep(2)

    finally:
        try:
            f.close()
        except Exception:
            pass
        self.stopDownload = None

    try:
        if os.path.getsize(tmp_path) > 0:
            return True
        else:
            self.debug(f"[HLS] possibly no data downloaded")
            return False
    except Exception:
        pass
    return False


def _ffmpeg_dump_to_ts(self : Bot, url, headers, out_path):
    hdr_blob = "\r\n".join(f"{k}: {v}" for k, v in headers.items()) + "\r\n"

    in_opts = [
        "-hide_banner", "-loglevel", "info",
        "-user_agent", headers.get("User-Agent", "Mozilla/5.0"),
        "-timeout", "5000000", "-rw_timeout", "5000000",
        "-reconnect", "1", "-reconnect_streamed", "1",
        "-reconnect_on_network_error", "1",
        "-reconnect_on_http_error", "4xx,5xx",
        "-reconnect_at_eof", "1", "-reconnect_delay_max", "10",
        "-live_start_index", "-3",
        "-max_reload", "30", "-seg_max_retry", "30", "-m3u8_hold_counters", "30",
        "-fflags", "nobuffer", "-fflags", "+discardcorrupt", "-fflags", "+genpts",
        "-probesize", "4M", "-analyzeduration", "10000000",
        "-allowed_extensions", "ALL", "-protocol_whitelist", "file,http,https,tcp,tls,crypto",
        "-headers", hdr_blob,
        "-i", url,
        "-map", "0", "-dn", "-sn",
        "-c", "copy",
        "-f", "mpegts",
        out_path
    ]

    self.debug("FFmpeg CMD (capture→.tmp.ts): " + " ".join(subprocess.list2cmdline([a]) for a in [FFMPEG_PATH] + in_opts))

    proc = subprocess.Popen([FFMPEG_PATH] + in_opts)
    self.stopDownload = lambda: StopBot(self, proc)
    rc = proc.wait()

def StopBot(self : Bot, proc : subprocess.Popen = None):
   if proc is not None:
       try:
           proc.terminate()
       except Exception:
           pass

   self.recording = False
   self.stopDownload = None
   