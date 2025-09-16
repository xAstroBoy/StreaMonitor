from __future__ import unicode_literals
import os
import traceback

import m3u8
from time import sleep
from datetime import datetime
from threading import Thread

from curl_cffi import requests
from curl_cffi import Cookies

from streamonitor.enums import Status
import streamonitor.log as log
from parameters import DOWNLOADS_DIR, DEBUG, WANTED_RESOLUTION, WANTED_RESOLUTION_PREFERENCE, CONTAINER, HTTP_USER_AGENT
from streamonitor.downloaders.ffmpeg import getVideoFfmpeg
from streamonitor.models import VideoData
from threading import Event, Thread
from streamonitor.utils.cf_session import CFSessionManager

# ADDED
import subprocess  # only used if you later want to enable ffprobe checks (currently not forced)

class Bot(Thread):
    loaded_sites = set()
    username = None
    site = None
    siteslug = None
    aliases = []
    ratelimit = False
    url = "javascript:void(0)"
    recording = False
    sleep_on_private = 5
    sleep_on_offline = 5
    sleep_on_long_offline = 300
    sleep_on_error = 20
    sleep_on_ratelimit = 180
    long_offline_timeout = 600
    previous_status = None

    headers = {
        "User-Agent": HTTP_USER_AGENT
    }

    status_messages = {
        Status.UNKNOWN: "Unknown error",
        Status.PUBLIC: "Channel online",
        Status.OFFLINE: "No stream",
        Status.LONG_OFFLINE: "No stream for a while",
        Status.PRIVATE: "Private show",
        Status.RATELIMIT: "Rate limited",
        Status.NOTEXIST: "Nonexistent user",
        Status.NOTRUNNING: "Not running",
        Status.ERROR: "Error on downloading",
        Status.RESTRICTED: "Model is restricted, maybe geo-block",
        Status.CLOUDFLARE: "Cloudflare",
    }

    def __init__(self, username):
        super().__init__()
        self.username = username
        self.logger = self.getLogger()

        self.cookies = None
        self.impersonate = None
        self.cookieUpdater = None
        self.cookie_update_interval = 0
        self.session = CFSessionManager(
            logger=self.logger,
            bot_id=f"[{self.siteslug}] {self.username}"
        )

        self._cookie_thread = None
        self._cookie_thread_stop = Event()

        self.lastInfo = {}
        self.running = False
        self.quitting = False
        self.sc: Status = Status.NOTRUNNING
        self.getVideo = getVideoFfmpeg
        self.stopDownload = None
        self.recording = False
        self.video_files = []
        self.video_files_total_size = 0
        self.isRetryingDownload = False
        # ADDED: optional toggles (do nothing unless used)
        self.verify_with_ffprobe = True  # not forced; size-only check is always on
        self.clean_failed_temp = True     # remove temp artifacts on failed runs

        self.cache_file_list()

    def getLogger(self):
        return log.Logger("[" + self.siteslug + "] " + self.username).get_logger()

    def restart(self):
        self.running = True

    def stop(self, a, b, thread_too=False):
        if self.running:
            self.log("Stopping...")
            if self.stopDownload:
                self.stopDownload()
            self.running = False
        if thread_too:
            self.quitting = True

    def getStatus(self):
        return Status.UNKNOWN

    def log(self, message):
        self.logger.info(message)

    def debug(self, message, filename=None):
        if DEBUG:
            self.logger.debug(message)
            if not filename:
                filename = os.path.join(self.outputFolder, 'debug.log')
            with open(filename, 'a+') as debugfile:
                debugfile.write(message + '\n')

    def status(self):
        message = self.status_messages.get(self.sc) or self.status_messages.get(Status.UNKNOWN)
        if self.sc == Status.NOTEXIST:
            self.running = False
        return message

    def getWebsiteURL(self):
        return "javascript:void(0)"

    def cache_file_list(self):
        videos_folder = self.outputFolder
        _videos = []
        _total_size = 0
        if os.path.isdir(videos_folder):
            try:
                for file in os.scandir(videos_folder):
                    if file.is_dir():
                        continue
                    if not os.path.splitext(file.name)[1][1:] in ['mp4', 'mkv', 'webm', 'mov', 'avi', 'wmv']:
                        continue
                    video = VideoData(file, self.username)
                    _total_size += video.filesize
                    _videos.append(video)
            except Exception as e:
                self.logger.warning(e)
        self.video_files = _videos
        self.video_files_total_size = _total_size

    def _sleep(self, time):
        while time > 0:
            sleep(1)
            time -= 1
            if self.quitting or not self.running:
                return

    def _start_cookie_updater(self):
        if self.cookie_update_interval > 0 and self.cookieUpdater is not None:
            if self._cookie_thread is None or not self._cookie_thread.is_alive():
                self._cookie_thread_stop.clear()
                def update_cookie():
                    while not self._cookie_thread_stop.is_set() and self.sc == Status.PUBLIC and self.running and not self.quitting:
                        self._sleep(self.cookie_update_interval)
                        if self._cookie_thread_stop.is_set():
                            break
                        ret = self.cookieUpdater()
                        if ret:
                            self.debug('Updated cookies')
                        else:
                            self.logger.warning('Failed to update cookies')
                self._cookie_thread = Thread(target=update_cookie, daemon=True)
                self._cookie_thread.start()

    def _stop_cookie_updater(self):
        if self._cookie_thread is not None:
            self._cookie_thread_stop.set()
            self._cookie_thread = None

    # ADDED: helpers (size check + temp cleanup)
    def _is_zero_or_missing(self, path: str) -> bool:
        try:
            return (not os.path.exists(path)) or os.path.getsize(path) == 0
        except Exception:
            return True

    # ADDED
    def _guess_temp_candidates(self, final_path: str):
        """
        Common ffmpeg/temp artifacts to purge if the run failed.
        Adjust patterns if your downloader uses different naming.
        """
        stem, _ext = os.path.splitext(final_path)
        folder = os.path.dirname(final_path)
        return [
            f"{stem}.tmp.ts",
            f"{stem}.part",
            f"{stem}.tmp",
            os.path.join(folder, "ffmpeg2pass-0.log"),
        ]

    # ADDED
    def _post_download_cleanup(self, final_path: str, ok: bool) -> bool:
        """
        Ensure final file isn't 0 KB. If failed, remove temp artifacts.
        Returns the (possibly updated) ok flag.
        """
        # If we think it succeeded but file is empty -> treat as failure and delete
        if ok and self._is_zero_or_missing(final_path):
            if os.path.exists(final_path):
                self.logger.error("Output is 0 KB — deleting.")
                self.isRetryingDownload = True
                try:
                    os.remove(final_path)
                except Exception as e:
                    self.logger.warning(f"Failed to remove zero-byte output '{final_path}': {e}")
            ok = False

        # If failed, clean temp artifacts (and also remove empty final file if any)
        if not ok and self.clean_failed_temp:
            # remove empty final file
            if os.path.exists(final_path) and self._is_zero_or_missing(final_path):
                try:
                    os.remove(final_path)
                except Exception as e:
                    self.logger.warning(f"Failed to remove failed output '{final_path}': {e}")

            # purge likely temp files
            for tmp in self._guess_temp_candidates(final_path):
                if os.path.exists(tmp):
                    # Optional tiny peek to help diagnose HTML saves; non-fatal
                    try:
                        delete = False
                        with open(tmp, "rb") as f:
                            sniff = f.read(256)
                        if len(sniff) == 0:
                            self.logger.error(f"Temp file empty: {os.path.basename(tmp)} — deleting.")
                            delete = True
                        elif b"<html" in sniff.lower() or b"<!doctype" in sniff.lower():
                            self.logger.error(f"Temp file contains HTML: {os.path.basename(tmp)} — deleting.")
                            delete = True
                    except Exception:
                        pass
                    try:
                        if delete or self._is_zero_or_missing(tmp):
                            os.remove(tmp)
                    except Exception as e:
                        self.logger.warning(f"Failed to remove temp '{tmp}': {e}")

        return ok

    def _download_once(self):
        video_url = self.getVideoUrl()
        if video_url is None:
            self.sc = Status.ERROR
            self.logger.error(self.status())
            return False
        self.log('Started downloading show')
        self.recording = True
        file = self.genOutFilename()
        ok = False
        try:
            ok = bool(self.getVideo(self, video_url, file))
        except Exception as e:
            self.logger.exception(e)
            ok = False
        finally:
            self.recording = False
            self.stopDownload = None

            # ADDED: verify/delete zero-byte outputs & temp artifacts
            try:
                ok = self._post_download_cleanup(file, ok)
            except Exception as e:
                self.logger.warning(f"Post-download cleanup error: {e}")

            try:
                self.cache_file_list()
            except Exception as e:
                self.logger.exception(e)
        if ok:
            self.log('Recording ended')
        else:
            self.log('Recording aborted/failed')
        return ok

    def run(self):
        while not self.quitting:
            while not self.running and not self.quitting:
                sleep(1)
            if self.quitting:
                break

            offline_time = self.long_offline_timeout + 1
            while self.running:
                try:
                    self.recording = False
                    self.sc = self.getStatus()
                    if self.sc != self.previous_status:
                        self.log(self.status())
                        self.previous_status = self.sc

                    if self.sc == Status.ERROR:
                        self._sleep(self.sleep_on_error)

                    if self.sc == Status.OFFLINE:
                        offline_time += self.sleep_on_offline
                        if offline_time > self.long_offline_timeout:
                            self.sc = Status.LONG_OFFLINE

                    elif self.sc in (Status.PUBLIC, Status.PRIVATE):
                        offline_time = 0

                        if self.sc == Status.PUBLIC:
                            self._start_cookie_updater()

                            # keep retrying as long as it's PUBLIC and we are supposed to run
                            while self.running and not self.quitting and self.sc == Status.PUBLIC:
                                success = self._download_once()
                                if not self.running or self.quitting:
                                    break
                                # quick re-check; if still PUBLIC, loop again to restart ffmpeg
                                self.sc = self.getStatus()
                                if self.sc != Status.PUBLIC:
                                    break
                                if not success:
                                    self.sc = Status.ERROR
                                    self.log(self.status())
                                    self._sleep(self.sleep_on_error)
                                    # after error, re-check status again
                                    self.sc = self.getStatus()
                                    if self.sc != Status.PUBLIC:
                                        break
                                    continue
                                # stream may still be live, spin again immediately (no long sleep)
                                self.log('Stream still online, restarting...')
                                self._sleep(1)

                            # leaving PUBLIC state
                            self._stop_cookie_updater()

                    # normal sleep decisions
                except Exception as e:
                    self.logger.exception(e)
                    try:
                        self.cache_file_list()
                    except Exception as e:
                        self.logger.exception(e)
                    self.log(self.status())
                    self.recording = False
                    self._sleep(self.sleep_on_error)
                    continue

                if self.quitting:
                    break
                elif self.ratelimit:
                    self._sleep(self.sleep_on_ratelimit)
                elif offline_time > self.long_offline_timeout:
                    self._sleep(self.sleep_on_long_offline)
                elif self.sc == Status.PRIVATE:
                    self._sleep(self.sleep_on_private)
                else:
                    self._sleep(self.sleep_on_offline)

            self._stop_cookie_updater()
            self.sc = Status.NOTRUNNING
            self.log("Stopped")

    def getPlaylistVariants(self, url=None, m3u_data=None):
        sources = []

        if isinstance(m3u_data, m3u8.M3U8):
            variant_m3u8 = m3u_data
        elif isinstance(m3u_data, str):
            variant_m3u8 = m3u8.loads(m3u_data)
        elif not m3u_data or url:
            result = requests.get(url, headers=self.headers, cookies=self.cookies)
            m3u8_doc = result.content.decode("utf-8")
            variant_m3u8 = m3u8.loads(m3u8_doc)
        else:
            return sources

        for playlist in variant_m3u8.playlists:
            stream_info = playlist.stream_info
            resolution = stream_info.resolution if type(stream_info.resolution) is tuple else (0, 0)
            sources.append({
                'url': playlist.uri,
                'resolution': resolution,
                'frame_rate': stream_info.frame_rate,
                'bandwidth': stream_info.bandwidth
            })

        if not variant_m3u8.is_variant and len(sources) >= 1:
            self.logger.warn("Not variant playlist, can't select resolution")
            return None
        return sources  # [(url, (width, height)),...]

    def getWantedResolutionPlaylist(self, url):
        try:
            sources = self.getPlaylistVariants(url)
            if sources is None:
                return None
            if len(sources) == 0:
                self.logger.error("No available sources")
                return None
            for source in sources:
                width, height = source['resolution']
                if width < height:
                    source['resolution_diff'] = width - WANTED_RESOLUTION
                else:
                    source['resolution_diff'] = height - WANTED_RESOLUTION
            sources.sort(key=lambda a: abs(a['resolution_diff']))
            selected_source = None
            if WANTED_RESOLUTION_PREFERENCE == 'exact':
                if sources[0]['resolution_diff'] == 0:
                    selected_source = sources[0]
            elif WANTED_RESOLUTION_PREFERENCE == 'closest' or len(sources) == 1:
                selected_source = sources[0]
            elif WANTED_RESOLUTION_PREFERENCE == 'exact_or_least_higher':
                for source in sources:
                    if source['resolution_diff'] >= 0:
                        selected_source = source
                        break
            elif WANTED_RESOLUTION_PREFERENCE == 'exact_or_highest_lower':
                for source in sources:
                    if source['resolution_diff'] <= 0:
                        selected_source = source
                        break
            else:
                self.logger.error('Invalid value for WANTED_RESOLUTION_PREFERENCE')
                return None
            if selected_source is None:
                self.logger.error("Couldn't select a resolution")
                return None
            if selected_source['resolution'][1] != 0:
                frame_rate = ''
                if selected_source['frame_rate'] is not None and selected_source['frame_rate'] != 0:
                    frame_rate = f" {selected_source['frame_rate']}fps"
                self.logger.info(f"Selected {selected_source['resolution'][0]}x{selected_source['resolution'][1]}{frame_rate} resolution")
            selected_source_url = selected_source['url']
            if selected_source_url.startswith("https://"):
                return selected_source_url
            else:
                return '/'.join(url.split('.m3u8')[0].split('/')[:-1]) + '/' + selected_source_url
        except BaseException as e:
            self.logger.error("Can't get playlist, got some error: " + str(e))
            traceback.print_tb(e.__traceback__)
            return None

    def getVideoUrl(self):
        pass

    def progressInfo(self, p):
        if p['status'] == 'downloading':
            self.log("Downloading " + str(round(float(p['downloaded_bytes']) / float(p['total_bytes']) * 100, 1)) + "%")
        if p['status'] == 'finished':
            self.log("Recording ended. File:" + p['filename'])

    @property
    def outputFolder(self):
        base_folder = os.path.join(DOWNLOADS_DIR, self.username + ' [' + self.siteslug + ']')
        if self.siteslug == 'SC' and hasattr(self, 'isMobileBroadcast') and self.isMobileBroadcast:
            base_folder = os.path.join(base_folder, 'Mobile')
        return base_folder

    def genOutFilename(self, create_dir=True):
        """
        SAME numbering logic, plus:
        - Delete only 0-byte finals to free the number
        - Sidecars:
            * ZERO bytes  -> delete and REUSE N
            * NON-ZERO    -> SKIP N (never append)
        - Logs EVERY skipped N (N=1, N=2, ...)
        """
        folder = self.outputFolder
        if create_dir:
            os.makedirs(folder, exist_ok=True)

        ext = f".{CONTAINER}".lower()

        # 1) Delete ONLY 0-byte finals so their numbers can be reused
        try:
            entries = [f for f in os.listdir(folder) if os.path.isfile(os.path.join(folder, f))]
        except FileNotFoundError:
            entries = []
        for f in entries:
            if f.lower().endswith(ext):
                p = os.path.join(folder, f)
                try:
                    if os.path.getsize(p) == 0:
                        try:
                            os.remove(p)
                            self.logger.info(f"Deleted zero-byte final '{p}', number will be reused")
                        except Exception as e:
                            self.logger.warning(f"Failed to remove zero-byte final '{p}': {e}")
                except Exception:
                    pass

        # Refresh listing
        try:
            entries = [f for f in os.listdir(folder) if os.path.isfile(os.path.join(folder, f))]
        except FileNotFoundError:
            entries = []

        used_numbers = sorted({
            int(os.path.splitext(f)[0])
            for f in entries
            if f.lower().endswith(ext) and os.path.splitext(f)[0].isdigit()
        })

        def sidecars_for(final_path: str):
            stem, _ = os.path.splitext(final_path)
            # ONLY per-number sidecars that could be appended to
            return [
                f"{stem}.tmp.ts",
                f"{stem}.ts.tmp",
                f"{stem}.segment.tmp",
                f"{stem}.part",
                f"{stem}.tmp",
            ]

        def block_reason(n: int):
            """
            Return None if N is allowed.
            Return a human string and do any 0-byte cleanup if not.
            """
            final_path = os.path.join(folder, f"{n}{ext}")
            nonzero_hits = []
            for s in sidecars_for(final_path):
                if os.path.exists(s):
                    try:
                        sz = os.path.getsize(s)
                    except Exception:
                        return f"Cannot stat sidecar '{s}'"
                    if sz == 0:
                        # delete zero-byte sidecars to free the number, but don't block N
                        try:
                            os.remove(s)
                            self.logger.info(f"Deleted zero-byte sidecar '{s}' to reuse N={n}")
                        except Exception as e:
                            # if we can't remove it, better to block this N to avoid confusion
                            return f"Failed to remove zero-byte sidecar '{s}': {e}"
                    else:
                        nonzero_hits.append(s)
            if nonzero_hits:
                return f"Non-zero sidecar present {nonzero_hits[0]}"
            return None

        # 2) Walk all numbers, logging every skip, until we find a clean N
        n = 1
        while True:
            candidate = os.path.join(folder, f"{n}{ext}")
            blocked = False
            # If final exists and is non-zero, skip
            if os.path.exists(candidate):
                try:
                    sz = os.path.getsize(candidate)
                except Exception:
                    sz = None
                if sz == 0:
                    try:
                        os.remove(candidate)
                        self.logger.info(f"Deleted zero-byte final '{candidate}', number will be reused")
                    except Exception as e:
                        self.logger.warning(f"Failed to remove zero-byte final '{candidate}': {e}")
                else:
                    self.logger.info(f"{self.username}: Non-zero final present {candidate}, skipping N={n} to avoid append")
                    blocked = True
            # If any sidecar blocks, skip
            reason = block_reason(n)
            if reason is not None:
                self.logger.info(f"{self.username}: {reason}, skipping N={n} to avoid append")
                blocked = True
            if not blocked:
                # Double check: if file was deleted above, recheck existence
                if not os.path.exists(candidate):
                    return candidate
            n += 1


    def export(self):
        return {"site": self.site, "username": self.username, "running": self.running}

    @staticmethod
    def str2site(site: str):
        site = site.lower()
        for sitecls in Bot.loaded_sites:
            if site == sitecls.site.lower() or \
                    site == sitecls.siteslug.lower() or \
                    site in sitecls.aliases:
                return sitecls

    @staticmethod
    def createInstance(username: str, site: str = None):
        if site:
            return Bot.str2site(site)(username)
        return None
