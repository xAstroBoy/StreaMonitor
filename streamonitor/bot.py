# streamonitor/bot.py
# Fully fixed bot implementation with thread safety and memory leak prevention

from __future__ import unicode_literals
import os
import traceback
import m3u8
import warnings
import filelock
from time import sleep
from datetime import datetime
from threading import Thread, Event, Lock, RLock
from typing import Optional, List, Dict, Any, Set, Union, Callable, Type

from streamonitor.enums import Status, Gender, GENDER_DATA, COUNTRIES
import streamonitor.log as log
import parameters
from parameters import (
    DOWNLOADS_DIR, WANTED_RESOLUTION, WANTED_RESOLUTION_PREFERENCE, 
    CONTAINER, HTTP_USER_AGENT, VERIFY_SSL
)
from streamonitor.downloaders.ffmpeg import getVideoFfmpeg
from streamonitor.models import VideoData
from streamonitor.utils.cf_session import CFSessionManager
from urllib.parse import urljoin
import urllib3

# Import termcolor for colored status messages
try:
    from termcolor import colored
    TERMCOLOR_AVAILABLE = True
except ImportError:
    TERMCOLOR_AVAILABLE = False
    def colored(text: str, color: Optional[str] = None, attrs: Optional[List[str]] = None) -> str:
        return text

# Disable SSL warnings if SSL verification is disabled
if not VERIFY_SSL:
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
    warnings.warn(
        "SSL verification is disabled. This is insecure and should only be used for testing.",
        UserWarning, stacklevel=2
    )

# Global locks for thread-safe operations
_print_lock = Lock()
_filename_lock = RLock()  # Reentrant lock for nested filename operations


# Global set of loaded site classes for upstream compatibility (used by BulkStatusManager)
LOADED_SITES: Set[Type['Bot']] = set()


class Bot(Thread):
    loaded_sites: Set[Type['Bot']] = set()
    username: Optional[str] = None
    site: Optional[str] = None
    siteslug: Optional[str] = None
    aliases: List[str] = []
    ratelimit: bool = False
    bulk_update: bool = False  # Override True in sites that support bulk status updates
    url: str = "javascript:void(0)"
    recording: bool = False
    sleep_on_private: int = 5
    sleep_on_offline: int = 5
    sleep_on_long_offline: int = 15  # Further reduced from 30 to 15 seconds
    sleep_on_error: int = 20
    sleep_on_ratelimit: int = 180
    long_offline_timeout: int = 180  # Reduced from 300 to 180 seconds (3min instead of 5min)
    previous_status: Optional[Status] = None
    _GENDER_MAP: Dict[str, Gender] = {}  # Override in site subclasses to map API gender strings to Gender enum

    def __init_subclass__(cls, **kwargs):
        """Auto-register site subclasses when they are defined."""
        super().__init_subclass__(**kwargs)
        if cls.site is not None:
            Bot.loaded_sites.add(cls)
            LOADED_SITES.add(cls)

    # Manager registry for auto-removal of deleted models
    _manager_instance = None

    headers: Dict[str, str] = {
        "User-Agent": HTTP_USER_AGENT
    }

    status_messages: Dict[Status, str] = {
        Status.UNKNOWN: colored("Unknown error", "red"),
        Status.PUBLIC: colored("Channel online", "green", attrs=["bold"]),
        Status.ONLINE: colored("Connected, waiting for stream", "cyan", attrs=["bold"]),
        Status.OFFLINE: colored("No stream", "yellow"),
        Status.PRIVATE: colored("Private show", "magenta"),
        Status.DELETED: colored("Model account deleted", "red", attrs=["bold"]),
        Status.RATELIMIT: colored("Rate limited", "red", attrs=["bold"]),
        Status.NOTEXIST: colored("Nonexistent user", "red"),
        Status.LONG_OFFLINE: colored("Long offline", "yellow", attrs=["dark"]),
        Status.NOTRUNNING: colored("Not running", "white"),
        Status.ERROR: colored("Error on downloading", "red", attrs=["bold"]),
        Status.RESTRICTED: colored("Model is restricted, maybe geo-block", "red"),
        Status.CLOUDFLARE: colored("Cloudflare", "blue"),
    }

    # Class-level logger cache to prevent memory leaks
    _logger_cache: Dict[str, log.Logger] = {}
    _logger_cache_lock: Lock = Lock()

    def __init__(self, username: str) -> None:
        super().__init__()
        self.daemon = True
        self.username = username
        self.gender: Gender = Gender.UNKNOWN
        self.country: Optional[str] = None
        
        # Use cached logger to prevent memory leaks
        self.logger = self._get_or_create_logger()

        self.cookies: Optional[Any] = None
        self.impersonate: Optional[str] = None
        self.cookieUpdater: Optional[Callable[[], bool]] = None
        self.cookie_update_interval: int = 0
        self.session: CFSessionManager = CFSessionManager(
            logger=self.logger,
            bot_id=f"[{self.siteslug}] {self.username}",
            verify=VERIFY_SSL
        )

        self._cookie_thread: Optional[Thread] = None
        self._cookie_thread_stop: Event = Event()
        self._state_lock: Lock = Lock()

        self.lastInfo: Dict[str, Any] = {}
        self.running: bool = False
        self.quitting: bool = False
        self.sc: Status = Status.NOTRUNNING
        self.getVideo: Callable = getVideoFfmpeg
        self.stopDownload: Optional[Callable[[], None]] = None
        self.recording: bool = False
        self.video_files: List[VideoData] = []
        self.video_files_total_size: int = 0
        self.isRetryingDownload: bool = False
        
        self.verify_with_ffprobe: bool = True
        self.clean_failed_temp: bool = True
        
        self._consecutive_errors: int = 0
        self._max_consecutive_errors: int = 20

        try:
            self.cache_file_list()
        except Exception as e:
            self.logger.warning(f"Failed to cache file list during init: {e}")

    def _get_or_create_logger(self) -> log.Logger:
        """
        Get or create logger from cache to prevent memory leaks.
        Multiple bot instances for same user/site share one logger.
        """
        logger_key = f"[{self.siteslug}] {self.username}"
        
        with Bot._logger_cache_lock:
            if logger_key not in Bot._logger_cache:
                Bot._logger_cache[logger_key] = log.Logger(logger_key, self)
            return Bot._logger_cache[logger_key]

    @classmethod
    def cleanup_logger_cache(cls, logger_key: Optional[str] = None) -> None:
        """
        Clean up logger cache. Call when removing a bot.
        
        Args:
            logger_key: Specific logger to remove, or None to clear all
        """
        with cls._logger_cache_lock:
            if logger_key:
                if logger_key in cls._logger_cache:
                    logger = cls._logger_cache.pop(logger_key)
                    if hasattr(logger, 'handlers'):
                        logger.handlers.clear()
            else:
                # Clear all loggers
                for logger in cls._logger_cache.values():
                    if hasattr(logger, 'handlers'):
                        logger.handlers.clear()
                cls._logger_cache.clear()

    def getLogger(self) -> log.Logger:
        """Legacy method for compatibility."""
        return self.logger

    @property
    def country_data(self) -> Optional[Dict[str, str]]:
        """Get country data (name, flag) for this model's country code."""
        if self.country:
            return COUNTRIES.get(self.country.upper())
        return None

    @property
    def gender_data(self) -> Optional[Dict[str, Any]]:
        """Get gender display data (name, icon, color) for this model's gender."""
        return GENDER_DATA.get(self.gender)

    def setStatus(self, status: Status, gender: Optional[Gender] = None, country: Optional[str] = None) -> None:
        """Set status from bulk status update (used by BulkStatusManager).
        Also updates gender/country if provided.
        """
        if gender is not None:
            self.gender = gender
        if country is not None:
            self.country = country
        self.sc = status
        if self.sc != self.previous_status:
            self.log(self.status())
            self.previous_status = self.sc

    def setUsername(self, username: str) -> None:
        """Update username (e.g., when resolved from room_id)."""
        if username and username != self.username:
            old = self.username
            self.username = username
            self.logger.info(f"Username updated: {old} -> {username}")
    
    def get_site_color(self) -> tuple[str, list[str]]:
        """Default color scheme for sites that don't override this method."""
        return ("white", [])
    
    def log(self, message: str) -> None:
        """Thread-safe logging with print lock."""
        with _print_lock:
            self.logger.info(message)

    def restart(self) -> None:
        with self._state_lock:
            if not self.running:
                self.logger.verbose("Starting bot...")
                self._consecutive_errors = 0
                
            # If model was offline before restart, reset to force fresh check
            if self.sc == Status.OFFLINE:
                self.sc = Status.UNKNOWN
                self.logger.verbose("Previous state was offline, forcing fresh status check")
                
            self.running = True
            
            # Reset previous_status to ensure fresh status logging after restart
            self.previous_status = None
            
            # Reset offline timing on restart
            self._last_restart_time = datetime.now().timestamp()
            
            # Ensure the thread is actually alive
            if not self.is_alive():
                try:
                    self.logger.warning("Thread was dead during restart, starting new thread")
                    # Reset thread state
                    self.quitting = False
                    # Start the thread if it's not alive
                    self.start()
                except RuntimeError as e:
                    if "threads can only be started once" in str(e):
                        self.logger.error("Cannot restart dead thread - this bot needs to be recreated")
                    else:
                        self.logger.error(f"Error starting thread: {e}")
                except Exception as e:
                    self.logger.error(f"Unexpected error starting thread: {e}")

    def stop(self, a: Any = None, b: Any = None, thread_too: bool = False) -> None:
        with self._state_lock:
            if self.running:
                self.log(colored("Stopping...", "red", attrs=["bold"]))
                if self.stopDownload:
                    try:
                        self.stopDownload()
                    except Exception as e:
                        self.logger.warning(f"Error calling stopDownload: {e}")
                self.running = False
            if thread_too:
                self.quitting = True

    def getStatus(self) -> Status:
        return Status.UNKNOWN

    def debug(self, message: str, filename: Optional[str] = None) -> None:
        if parameters.DEBUG:
            self.logger.debug(message)
            if not filename:
                filename = os.path.join(self.outputFolder, 'debug.log')
            try:
                os.makedirs(os.path.dirname(filename), exist_ok=True)
                with open(filename, 'a+', encoding='utf-8') as debugfile:
                    debugfile.write(message + '\n')
            except Exception as e:
                self.logger.debug(f"Failed to write debug log: {e}")

    def status(self) -> str:
        base_message = self.status_messages.get(self.sc) or self.status_messages.get(Status.UNKNOWN)
        
        if "VR" in self.siteslug:
            if self.sc == Status.PUBLIC:
                message = colored("LIVE VR STREAM!", "red", attrs=["bold"])
            elif self.sc == Status.PRIVATE:
                message = colored("VR Private Show", "magenta", attrs=["bold"])
            elif self.sc == Status.OFFLINE:
                message = colored("No stream", "yellow")
            elif self.sc == Status.ERROR:
                message = colored("VR Error", "red", attrs=["bold"])
            else:
                message = base_message
        else:
            if self.sc == Status.PUBLIC:
                message = colored("Channel online", "green", attrs=["bold"])
            elif self.sc == Status.PRIVATE:
                message = colored("Private show", "magenta")
            elif self.sc == Status.OFFLINE:
                message = base_message
            elif self.sc == Status.ERROR:
                message = colored("Error on downloading", "red", attrs=["bold"])
            elif self.sc == Status.RATELIMIT:
                message = colored("Rate limited", "red", attrs=["bold"])
            elif self.sc == Status.NOTEXIST:
                message = colored("Nonexistent user", "red")
            elif self.sc == Status.CLOUDFLARE:
                message = colored("Cloudflare", "blue")
            else:
                message = base_message
        
        if self.sc == Status.NOTEXIST:
            with self._state_lock:
                self.running = False
        return message

    def getWebsiteURL(self) -> str:
        return "javascript:void(0)"

    def cache_file_list(self) -> None:
        videos_folder = self.outputFolder
        _videos = []
        _total_size = 0
        if os.path.isdir(videos_folder):
            try:
                for file in os.scandir(videos_folder):
                    if file.is_dir():
                        continue
                    ext = os.path.splitext(file.name)[1][1:]
                    if ext not in ['mp4', 'mkv', 'webm', 'mov', 'avi', 'wmv', 'ts']:
                        continue
                    try:
                        video = VideoData(file, self.username)
                        _total_size += video.filesize
                        _videos.append(video)
                    except Exception as e:
                        self.logger.debug(f"Error processing video file {file.name}: {e}")
            except Exception as e:
                self.logger.warning(f"Error scanning video folder: {e}")
        self.video_files = _videos
        self.video_files_total_size = _total_size

    def _sleep(self, time: Union[int, float]) -> None:
        """Interruptible sleep that checks for quit/stop signals."""
        end_time = datetime.now().timestamp() + time
        while datetime.now().timestamp() < end_time:
            if self.quitting or not self.running:
                return
            remaining = end_time - datetime.now().timestamp()
            sleep(min(1, max(0, remaining)))

    def _start_cookie_updater(self) -> None:
        if self.cookie_update_interval <= 0 or self.cookieUpdater is None:
            return
            
        if self._cookie_thread is not None and self._cookie_thread.is_alive():
            return
            
        self._cookie_thread_stop.clear()
        
        def update_cookie():
            self.logger.debug("Cookie updater thread started")
            while not self._cookie_thread_stop.is_set():
                try:
                    self._sleep(self.cookie_update_interval)
                    if self._cookie_thread_stop.is_set():
                        break
                    
                    if not self.recording or not self.running:
                        break
                        
                    ret = self.cookieUpdater()
                    if ret:
                        self.debug('Updated cookies')
                    else:
                        self.logger.warning('Failed to update cookies')
                except Exception as e:
                    self.logger.exception(f"Cookie updater error: {e}")
                    break
            self.logger.debug("Cookie updater thread stopped")
        
        self._cookie_thread = Thread(target=update_cookie, daemon=True)
        self._cookie_thread.start()

    def _stop_cookie_updater(self) -> None:
        if self._cookie_thread is not None:
            self._cookie_thread_stop.set()
            self._cookie_thread = None

    def _is_zero_or_missing(self, path: str) -> bool:
        try:
            return (not os.path.exists(path)) or os.path.getsize(path) == 0
        except Exception:
            return True

    def _guess_temp_candidates(self, final_path: str) -> List[str]:
        stem, _ = os.path.splitext(final_path)
        folder = os.path.dirname(final_path)
        return [
            f"{stem}.tmp.ts",
            f"{stem}.ts.tmp",
            f"{stem}.segment.tmp",
            f"{stem}.part",
            f"{stem}.tmp",
            os.path.join(folder, "ffmpeg2pass-0.log"),
            os.path.join(folder, "ffmpeg2pass-0.log.mbtree"),
        ]

    def _post_download_cleanup(self, final_path: str, ok: bool) -> bool:
        """Verify final file and clean up temporary files on failure."""
        try:
            # For HLS downloads, check the actual .tmp.ts file instead of .mkv
            actual_file = final_path
            stem, ext = os.path.splitext(final_path)
            tmp_ts_file = stem + '.tmp.ts'
            
            # If .tmp.ts file exists, that's the actual output file for HLS
            if os.path.exists(tmp_ts_file) and not os.path.exists(final_path):
                actual_file = tmp_ts_file
            
            if ok and self._is_zero_or_missing(actual_file):
                self.logger.error(f"Output file is 0 KB or missing: {actual_file}")
                if os.path.exists(actual_file):
                    try:
                        os.remove(actual_file)
                        self.logger.info("Removed zero-byte output file")
                    except Exception as e:
                        self.logger.warning(f"Failed to remove zero-byte file: {e}")
                ok = False
                self.isRetryingDownload = True

            if not ok and self.clean_failed_temp:
                # Clean up the actual file that was created
                if os.path.exists(actual_file) and self._is_zero_or_missing(actual_file):
                    try:
                        os.remove(actual_file)
                        self.logger.info("Removed failed output file")
                    except Exception as e:
                        self.logger.warning(f"Failed to remove failed output: {e}")

                for tmp in self._guess_temp_candidates(final_path):
                    if not os.path.exists(tmp):
                        continue
                    
                    try:
                        should_delete = False
                        try:
                            with open(tmp, "rb") as f:
                                sniff = f.read(512)
                            if len(sniff) == 0:
                                should_delete = True
                                self.logger.debug(f"Temp file is empty: {os.path.basename(tmp)}")
                            elif b"<html" in sniff.lower() or b"<!doctype" in sniff.lower():
                                should_delete = True
                                self.logger.warning(f"Temp file contains HTML: {os.path.basename(tmp)}")
                        except Exception:
                            should_delete = True
                        
                        if should_delete:
                            os.remove(tmp)
                            self.logger.debug(f"Cleaned up temp file: {os.path.basename(tmp)}")
                    except Exception as e:
                        self.logger.debug(f"Failed to clean temp file {tmp}: {e}")
        
        except Exception as e:
            self.logger.warning(f"Error in post-download cleanup: {e}")
        
        return ok

    def _download_once(self) -> bool:
        try:
            video_url = self.getVideoUrl()
            if video_url is None:
                self.logger.error("Failed to get video URL")
                return False
            
            self.log(colored('Started downloading show', "green", attrs=["bold"]))
            self.recording = True
            file = self.genOutFilename()
            ok = False
            
            try:
                ok = bool(self.getVideo(self, video_url, file))
            except KeyboardInterrupt:
                raise
            except Exception as e:
                self.logger.error(f"Download error: {e}")
                ok = False
            finally:
                self.recording = False
                self.stopDownload = None

                try:
                    ok = self._post_download_cleanup(file, ok)
                except Exception as e:
                    self.logger.warning(f"Cleanup error: {e}")

                try:
                    self.cache_file_list()
                except Exception as e:
                    self.logger.warning(f"Failed to update file cache: {e}")
            
            if ok:
                self.log(colored('Recording ended successfully', "green", attrs=["bold"]))
                self._consecutive_errors = 0
            else:
                self.log(colored('Recording failed', "red", attrs=["bold"]))
                self._consecutive_errors += 1
            
            return ok
            
        except Exception as e:
            self.logger.exception(f"Unexpected error in _download_once: {e}")
            self._consecutive_errors += 1
            return False

    def run(self) -> None:
        self.logger.verbose("Bot thread started, waiting for start signal...")
        
        try:
            while not self.quitting:
                if self.running:
                    break
                sleep(1)
            
            if self.quitting:
                self.logger.verbose("Bot quit before starting")
                return

            self.logger.verbose("Bot main loop starting")
            offline_time = 0
            
            while self.running and not self.quitting:
                try:
                    self.recording = False
                    
                    if self._consecutive_errors >= self._max_consecutive_errors:
                        self.logger.warning(
                            f"Hit {self._max_consecutive_errors} consecutive errors, "
                            f"backing off for {self.sleep_on_long_offline}s before retrying"
                        )
                        self._consecutive_errors = 0
                        self._sleep(self.sleep_on_long_offline)
                        continue
                    
                    try:
                        self.sc = self.getStatus()
                    except Exception as e:
                        self.logger.error(f"Error getting status: {e}")
                        self.sc = Status.ERROR
                    
                    if self.sc != self.previous_status:
                        self.log(self.status())
                        self.previous_status = self.sc

                    if self.sc == Status.ERROR:
                        self._consecutive_errors += 1
                        self._sleep(self.sleep_on_error)
                        continue
                    else:
                        # Reset error counter on any successful status check
                        self._consecutive_errors = 0

                    if self.sc == Status.NOTEXIST:
                        self.logger.error(f"❌ User {self.username} does not exist - auto-removing from configuration")
                        # Auto-remove the non-existent model from configuration
                        if Bot.auto_remove_model(self.username, self.site, "non-existent"):
                            self.logger.info(f"✅ Successfully auto-removed non-existent model [{self.siteslug}] {self.username}")
                        else:
                            self.logger.warning(f"⚠️ Failed to auto-remove non-existent model [{self.siteslug}] {self.username}")
                        with self._state_lock:
                            self.running = False
                        break

                    elif self.sc == Status.DELETED:
                        self.logger.error(f"🗑️ Model account {self.username} has been DELETED - auto-removing from configuration")
                        # Auto-remove the model from configuration
                        if Bot.auto_remove_model(self.username, self.site, "deleted"):
                            self.logger.info(f"✅ Successfully auto-removed deleted model [{self.siteslug}] {self.username}")
                        else:
                            self.logger.warning(f"⚠️ Failed to auto-remove deleted model [{self.siteslug}] {self.username}")
                        with self._state_lock:
                            self.running = False
                        break

                    elif self.sc == Status.CLOUDFLARE:
                        self.logger.error("Cloudflare challenge detected")
                        self._sleep(self.sleep_on_ratelimit)
                        continue

                    elif self.sc == Status.RATELIMIT:
                        self.logger.warning("Rate limited")
                        self._sleep(self.sleep_on_ratelimit)
                        continue

                    elif self.sc == Status.OFFLINE:
                        offline_time += self.sleep_on_offline
                        
                        # Smart offline detection - if just restarted, reset offline timer
                        if hasattr(self, '_last_restart_time'):
                            time_since_restart = datetime.now().timestamp() - self._last_restart_time
                            if time_since_restart < 60:  # Less than 1 minute since restart
                                offline_time = 0  # Reset offline timer to avoid immediate long-offline timeout
                        
                        # Sleep with longer timeout after extended offline period
                        sleep_time = self.sleep_on_long_offline if offline_time > self.long_offline_timeout else self.sleep_on_offline
                        self._sleep(sleep_time)
                        continue

                    elif self.sc == Status.LONG_OFFLINE:
                        # Long offline - use longer sleep interval
                        self._sleep(self.sleep_on_long_offline)
                        continue

                    elif self.sc == Status.ONLINE:
                        # Model is connected but no stream yet - wait for it to start
                        offline_time = 0
                        self._sleep(self.sleep_on_private)  # Use same wait time as private
                        continue

                    elif self.sc == Status.PRIVATE:
                        offline_time = 0
                        self._sleep(self.sleep_on_private)
                        continue

                    elif self.sc == Status.PUBLIC:
                        offline_time = 0
                        self._start_cookie_updater()

                        while self.running and not self.quitting:
                            try:
                                current_status = self.getStatus()
                            except Exception as e:
                                self.logger.error(f"Error re-checking status: {e}")
                                current_status = Status.ERROR
                            
                            if current_status != Status.PUBLIC:
                                self.sc = current_status
                                self.log(self.status())
                                break

                            success = self._download_once()
                            
                            if not self.running or self.quitting:
                                break
                            
                            if not success:
                                self._sleep(self.sleep_on_error)
                                try:
                                    self.sc = self.getStatus()
                                except Exception as e:
                                    self.logger.error(f"Error checking status after failed download: {e}")
                                    self.sc = Status.ERROR
                                
                                if self.sc != Status.PUBLIC:
                                    break
                                
                                self.log(colored("Stream still live, retrying download...", "yellow", attrs=["bold"]))
                            else:
                                self.log(colored("Checking if stream is still live...", "blue"))
                                self._sleep(2)

                        self._stop_cookie_updater()

                    elif self.sc in (Status.UNKNOWN, Status.ERROR):
                        # Server error or unknown state - just wait and retry
                        self._sleep(self.sleep_on_error)

                    else:
                        self.logger.warning(f"Unhandled status: {self.sc}")
                        self._sleep(self.sleep_on_error)

                except KeyboardInterrupt:
                    self.logger.info("Keyboard interrupt received")
                    raise
                except Exception as e:
                    self.logger.exception(f"Error in main loop: {e}")
                    self._consecutive_errors += 1
                    try:
                        self.cache_file_list()
                    except Exception:
                        pass
                    self.recording = False
                    self._sleep(self.sleep_on_error)

        except KeyboardInterrupt:
            self.logger.info("Bot interrupted")
        except Exception as e:
            self.logger.exception(f"Fatal error in bot thread: {e}")
        finally:
            self._stop_cookie_updater()
            self.sc = Status.NOTRUNNING
            self.log(colored("Bot stopped", "red", attrs=["bold"]))

    def getPlaylistVariants(self, url: Optional[str] = None, m3u_data: Optional[Union[str, m3u8.M3U8]] = None) -> Optional[List[Dict[str, Any]]]:
        """Parse M3U8 playlist and extract available quality variants."""
        sources = []

        try:
            if isinstance(m3u_data, m3u8.M3U8):
                variant_m3u8 = m3u_data
            elif isinstance(m3u_data, str):
                variant_m3u8 = m3u8.loads(m3u_data)
            elif url:
                try:
                    result = self.session.get(
                        url,
                        headers=self.headers,
                        bucket='hls',
                        timeout=30
                    )
                    
                    if result.status_code != 200:
                        self.logger.error(f"Failed to fetch playlist: HTTP {result.status_code}")
                        return None
                    
                    m3u8_doc = result.text
                    
                    if not m3u8_doc.strip().startswith("#EXTM3U"):
                        self.logger.error(f"Invalid M3U8 data. Response: {result.text[:200]}")
                        return None
                    
                    variant_m3u8 = m3u8.loads(m3u8_doc)
                    
                except Exception as e:
                    self.logger.error(f"Error fetching playlist: {e}")
                    return None
            else:
                return sources

            for playlist in variant_m3u8.playlists:
                stream_info = playlist.stream_info
                resolution = stream_info.resolution if isinstance(stream_info.resolution, tuple) else (0, 0)
                sources.append({
                    'url': playlist.uri,
                    'resolution': resolution,
                    'frame_rate': stream_info.frame_rate,
                    'bandwidth': stream_info.bandwidth
                })

            if not variant_m3u8.is_variant and len(sources) >= 1:
                self.logger.warning("Not a variant playlist, can't select resolution")
                return None
            
            return sources
            
        except Exception as e:
            self.logger.error(f"Error parsing playlist variants: {e}")
            return None

    def getWantedResolutionPlaylist(self, url: str) -> Optional[str]:
        try:
            sources = self.getPlaylistVariants(url)
            if not sources:
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

            if not selected_source:
                self.logger.error("Couldn't select a resolution")
                return None

            w, h = selected_source['resolution']
            if h != 0:
                frame_rate = ''
                if selected_source.get('frame_rate'):
                    frame_rate = f" {selected_source['frame_rate']}fps"
                self.logger.info(f"Selected {w}x{h}{frame_rate} resolution")

            return urljoin(url, selected_source['url']) if url else selected_source['url']

        except Exception as e:
            self.logger.error(f"Error selecting resolution: {e}")
            traceback.print_exc()
            return None

    def getVideoUrl(self) -> Optional[str]:
        pass

    def progressInfo(self, p: Dict[str, Any]) -> None:
        if p['status'] == 'downloading':
            try:
                pct = round(float(p['downloaded_bytes']) / float(p['total_bytes']) * 100, 1)
                self.log(colored(f"Downloading {pct}%", "blue"))
            except Exception:
                pass
        elif p['status'] == 'finished':
            self.log(colored(f"Recording ended. File: {p['filename']}", "green"))

    @property
    def outputFolder(self) -> str:
        base_folder = os.path.join(DOWNLOADS_DIR, f"{self.username} [{self.siteslug}]")
        if hasattr(self, 'isMobile') and callable(getattr(self, 'isMobile', None)):
            try:
                if self.isMobile():
                    base_folder = os.path.join(base_folder, 'Mobile')
            except Exception:
                pass
        return base_folder

    def genOutFilename(self, create_dir: bool = True) -> str:
        """
        Thread-safe filename generation with file locking.
        Prevents race conditions when multiple bots run simultaneously.
        """
        with _filename_lock:
            folder = self.outputFolder
            if create_dir:
                os.makedirs(folder, exist_ok=True)

            ext = f".{CONTAINER}".lower()
            
            # Create lock file for this folder
            lock_file = os.path.join(folder, ".filename.lock")
            lock = filelock.FileLock(lock_file, timeout=10)
            
            try:
                with lock:
                    # Clean up zero-byte files
                    try:
                        entries = [f for f in os.listdir(folder) if os.path.isfile(os.path.join(folder, f))]
                    except FileNotFoundError:
                        entries = []
                    
                    for f in entries:
                        if f.lower().endswith(ext):
                            p = os.path.join(folder, f)
                            try:
                                if os.path.getsize(p) == 0:
                                    os.remove(p)
                                    self.logger.debug(f"Deleted zero-byte file: {f}")
                            except Exception as e:
                                self.logger.debug(f"Error checking/removing {f}: {e}")

                    # Refresh listing
                    try:
                        entries = [f for f in os.listdir(folder) if os.path.isfile(os.path.join(folder, f))]
                    except FileNotFoundError:
                        entries = []

                    def sidecars_for(final_path: str) -> List[str]:
                        stem, _ = os.path.splitext(final_path)
                        return [
                            f"{stem}.tmp.ts",
                            f"{stem}.ts.tmp",
                            f"{stem}.segment.tmp",
                            f"{stem}.part",
                            f"{stem}.tmp",
                        ]

                    n: int = 1
                    while True:
                        candidate = os.path.join(folder, f"{n}{ext}")
                        
                        if os.path.exists(candidate):
                            try:
                                if os.path.getsize(candidate) > 0:
                                    n += 1
                                    continue
                                os.remove(candidate)
                                self.logger.debug(f"Removed zero-byte file during numbering: {n}{ext}")
                            except Exception:
                                n += 1
                                continue
                        
                        blocked = False
                        for sidecar in sidecars_for(candidate):
                            if os.path.exists(sidecar):
                                try:
                                    if os.path.getsize(sidecar) == 0:
                                        os.remove(sidecar)
                                        self.logger.debug(f"Removed zero-byte sidecar: {os.path.basename(sidecar)}")
                                    else:
                                        blocked = True
                                        break
                                except Exception:
                                    blocked = True
                                    break
                        
                        if not blocked:
                            return candidate
                        
                        n += 1
            except filelock.Timeout:
                self.logger.warning("Filename lock timeout, proceeding without lock")
                # Fallback without lock
                return os.path.join(folder, f"1{ext}")

    def export(self) -> Dict[str, Any]:
        data = {
            "site": self.site,
            "username": self.username,
            "running": self.running,
            "status": self.sc.name if hasattr(self.sc, 'name') else str(self.sc),
            "recording": self.recording,
        }
        if self.gender != Gender.UNKNOWN:
            data["gender"] = self.gender.value
        if self.country:
            data["country"] = self.country
        return data

    @classmethod
    def fromConfig(cls, config: Dict[str, Any]) -> Optional['Bot']:
        """Create a Bot instance from a saved config dict (with gender/country restoration)."""
        username = config.get("username")
        if not username:
            return None
        instance = cls(username)
        gender_val = config.get("gender")
        if gender_val is not None:
            try:
                instance.gender = Gender(gender_val)
            except (ValueError, KeyError):
                instance.gender = Gender.UNKNOWN
        country = config.get("country")
        if country:
            instance.country = country
        return instance

    @staticmethod
    def str2site(site: str) -> Optional[Type['Bot']]:
        site = site.lower()
        for sitecls in Bot.loaded_sites:
            if site == sitecls.site.lower() or \
                    site == sitecls.siteslug.lower() or \
                    site in sitecls.aliases:
                return sitecls
        return None

    @staticmethod
    def createInstance(username: str, site: Optional[str] = None) -> Optional['Bot']:
        if site:
            site_cls = Bot.str2site(site)
            if site_cls:
                return site_cls(username)
        return None

    @staticmethod
    def register_manager(manager):
        """Register the manager instance for auto-removal functionality"""
        Bot._manager_instance = manager

    @staticmethod
    def auto_remove_model(username: str, site: str, reason: str = "deleted"):
        """Auto-remove a deleted/invalid model from the configuration"""
        if Bot._manager_instance is None:
            return False
        
        try:
            # Find the specific streamer to remove
            with Bot._manager_instance._streamers_lock:
                streamer_to_remove = None
                for streamer in Bot._manager_instance.streamers:
                    if (streamer.username.lower() == username.lower() and 
                        streamer.site.lower() == site.lower()):
                        streamer_to_remove = streamer
                        break
            
            if streamer_to_remove:
                # Use the existing removal logic from the manager
                result = Bot._manager_instance.do_remove(streamer_to_remove, username, site)
                Bot._manager_instance.logger.warning(f"🗑️ Auto-removed {reason} model [{site}] {username}")
                return True
            return False
            
        except Exception as e:
            if Bot._manager_instance:
                Bot._manager_instance.logger.error(f"Failed to auto-remove model {username}: {e}")
            return False


class RoomIdBot(Bot):
    """Base class for sites that use a numeric room_id (StripChat, Flirt4Free, SexChatHU, FanslyLive).
    
    Supports looking up username from room_id and vice versa.
    When instantiated with a numeric string, it's treated as a room_id.
    """
    site = None  # Must be set by subclass

    def __init__(self, username: str, room_id: Optional[str] = None) -> None:
        self.room_id: Optional[str] = room_id
        if room_id is None and username.isdigit():
            self.room_id = username
        super().__init__(username)

    def getUsernameFromRoomId(self, room_id: str) -> Optional[str]:
        """Override in subclass to resolve username from room_id."""
        return None

    def getRoomIdFromUsername(self, username: str) -> Optional[str]:
        """Override in subclass to resolve room_id from username."""
        return None

    def export(self) -> Dict[str, Any]:
        data = super().export()
        if self.room_id:
            data["room_id"] = self.room_id
        return data

    @classmethod
    def fromConfig(cls, config: Dict[str, Any]) -> Optional['RoomIdBot']:
        username = config.get("username")
        if not username:
            return None
        room_id = config.get("room_id")
        instance = cls(username, room_id=room_id)
        gender_val = config.get("gender")
        if gender_val is not None:
            try:
                instance.gender = Gender(gender_val)
            except (ValueError, KeyError):
                instance.gender = Gender.UNKNOWN
        country = config.get("country")
        if country:
            instance.country = country
        return instance