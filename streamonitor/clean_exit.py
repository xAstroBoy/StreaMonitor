import time
import signal
import os
import psutil
import subprocess
from threading import Thread
import streamonitor.log as log

logger = log.Logger("cleanup")


class CleanExit:
    class DummyThread(Thread):
        def __init__(self):
            super().__init__()
            self._stop = False

        def run(self):
            while True:
                if self._stop:
                    return
                time.sleep(1)

        def stop(self):
            self._stop = True

    dummy_thread = DummyThread()

    def __init__(self, streamers):
        self.streamers = streamers
        if not self.dummy_thread.is_alive():
            self.dummy_thread.start()
            signal.signal(signal.SIGINT, self.clean_exit)
            signal.signal(signal.SIGTERM, self.clean_exit)
            signal.signal(signal.SIGABRT, self.clean_exit)

    def __call__(self, *args, **kwargs):
        self.clean_exit()

    def _kill_ffmpeg_processes(self):
        """Find and kill only StreamMonitor-related FFmpeg processes."""
        try:
            killed_count = 0
            current_pid = os.getpid()
            
            # Get all running processes
            for proc in psutil.process_iter(['pid', 'name', 'cmdline', 'ppid']):
                try:
                    proc_info = proc.info
                    proc_name = proc_info.get('name', '').lower()
                    cmdline = proc_info.get('cmdline', [])
                    
                    # Check if it's an FFmpeg process
                    if 'ffmpeg' in proc_name or any('ffmpeg' in str(arg).lower() for arg in cmdline):
                        # More specific checks to ensure it's related to StreamMonitor
                        is_streammonitor_related = False
                        
                        cmdline_str = ' '.join(cmdline) if cmdline else ''
                        
                        # Check for StreamMonitor-specific patterns (must match multiple criteria)
                        streammonitor_indicators = 0
                        
                        # 1. Check for StreamMonitor directories
                        if any(path in cmdline_str for path in ['downloads\\', 'M3U8_TMP']):
                            streammonitor_indicators += 1
                        
                        # 2. Check for StreamMonitor-specific file patterns
                        if any(pattern in cmdline_str.lower() for pattern in ['rolling.m3u8', '.tmp.ts', 'stripchat', 'chaturbate', 'camsoda']):
                            streammonitor_indicators += 1
                        
                        # 3. Check if it's a child of our process or related process tree
                        if proc_info.get('ppid') == current_pid:
                            streammonitor_indicators += 1
                        
                        # 4. Check for HLS-specific patterns
                        if any(hls_pattern in cmdline_str.lower() for hls_pattern in ['m3u8', 'hls', 'protocol_whitelist']):
                            streammonitor_indicators += 1
                        
                        # Only kill if multiple indicators match (more confident it's ours)
                        if streammonitor_indicators >= 2:
                            is_streammonitor_related = True
                        
                        if is_streammonitor_related:
                            logger.info(f"🔫 Killing StreamMonitor FFmpeg: {proc_info['pid']} - {proc_name}")
                            proc.kill()
                            killed_count += 1
                            
                except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
                    # Process might have ended or we don't have permission
                    continue
                except Exception as e:
                    # Ignore other errors during cleanup
                    continue
            
            if killed_count > 0:
                logger.info(f"🧹 Cleaned up {killed_count} StreamMonitor FFmpeg process(es)")
                # Give processes time to die
                time.sleep(1)
                
        except Exception as e:
            logger.error(f"⚠️ Error during FFmpeg cleanup: {e}", exc_info=True)

    def clean_exit(self, _=None, __=None):
        logger.info("🛑 Cleaning up processes...")
        
        # Stop all streamers first
        for streamer in self.streamers:
            streamer.stop(None, None, True)
        for streamer in self.streamers:
            while streamer.is_alive():
                time.sleep(1)
        
        # Kill any remaining FFmpeg processes
        self._kill_ffmpeg_processes()
        
        self.dummy_thread.stop()
        
        # Remove lock file on clean exit
        try:
            if os.path.exists("streammonitor.lock"):
                os.remove("streammonitor.lock")
                logger.info("🔓 Removed lock file")
        except Exception:
            pass  # Ignore errors during cleanup
