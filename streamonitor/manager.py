# streamonitor/manager.py
# Fully fixed manager with thread safety and safer process handling

import math
import os
import shutil
import psutil
import json
import sys
import time
from logging import INFO, WARN, ERROR
from threading import Thread, RLock
from typing import List, Optional, Any
from termcolor import colored
import terminaltables.terminal_io
from terminaltables import AsciiTable
import streamonitor.config as config
import streamonitor.log as log
from streamonitor.bot import Bot
from streamonitor.managers.outofspace_detector import OOSDetector
from parameters import DOWNLOADS_DIR

from streamonitor.enums import Status


class Manager(Thread):
    def __init__(self, streamers: List[Bot]) -> None:
        super().__init__()
        self.daemon = True
        self.streamers = streamers
        self.logger = log.Logger("manager")
        
        # Thread safety for streamer list modifications
        self._streamers_lock = RLock()
        
        # Register this manager instance for auto-removal functionality
        Bot.register_manager(self)

    def execCmd(self, line: str) -> str:
        parts = str(line).strip().split(' ')
        command_name = parts[0].lower()
        
        if 'do_' + command_name not in dir(self):
            return f'Unknown command: {command_name}. Type "help" for available commands.'

        command = getattr(self, 'do_' + command_name)
        if command:
            username = parts[1] if len(parts) > 1 else ""
            site = parts[2] if len(parts) > 2 else ""
            extra_param = parts[3] if len(parts) > 3 else ""
            streamer = self.getStreamer(username, site)
            
            if command_name in ['help', 'enabledebug', 'debug', 'cleanup', 'getjson']:
                if command_name == 'getjson':
                    return command(username, site, extra_param)
                else:
                    return command()
            elif command_name == 'resync':
                return self.do_resync(username, site)
            else:
                return command(streamer, username, site)

    def getStreamer(self, username: str, site: str) -> Optional[Bot]:
        """Thread-safe streamer lookup."""
        found = None
        site_cls = Bot.str2site(site)
        if site_cls:
            site = site_cls.site
        
        with self._streamers_lock:
            for streamer in self.streamers:
                if streamer.username == username:
                    if site and site != "":
                        if streamer.site == site:
                            return streamer
                    else:
                        if not found:
                            found = streamer
                        else:
                            self.logger.error('Multiple users exist with this username, specify site too')
                            return None
        return found

    def saveConfig(self) -> None:
        """Thread-safe config save."""
        with self._streamers_lock:
            config.save_config([s.export() for s in self.streamers])

    def do_help(self) -> str:
        """Display help information for all available commands."""
        help_text = f"""
{colored("🎬 StreamMonitor Commands 🎬", "cyan", attrs=["bold"])}
{colored("=" * 30, "cyan")}

{colored("🎯 Streamer Management:", "yellow", attrs=["bold"])}
  {colored("add", "green", attrs=["bold"])} <username> <site>     - Add new streamer to monitor
  {colored("add", "green", attrs=["bold"])} <username> {colored("sc*", "cyan", attrs=["bold"])}        - Add both SC and SCVR for username
  {colored("remove", "red", attrs=["bold"])} <username> [site]  - Remove streamer from monitoring
  {colored("remove", "red", attrs=["bold"])} <username> {colored("*", "yellow", attrs=["bold"])}       - Remove streamer from all sites
  {colored("start", "green")} <username|*> [site] - Start monitoring (use {colored("*", "yellow", attrs=["bold"])} for all)
  {colored("stop", "red")} <username|*> [site]  - Stop monitoring (use {colored("*", "yellow", attrs=["bold"])} for all)
  {colored("restart", "cyan")} <username> [site] - Restart specific streamer
  {colored("resync", "blue", attrs=["bold"])} <username|*> [site|*] - Force status recheck and restart downloads

{colored("📊 Status & Monitoring:", "yellow", attrs=["bold"])}
  {colored("status", "blue")} [username] [site]  - Show detailed status table
  {colored("status2", "magenta", attrs=["bold"])} [username] [site] - Show colorful grid view

{colored("📁 File Management:", "yellow", attrs=["bold"])}
  {colored("move", "cyan")} <username|*> [site]  - Move files to unprocessed folder

{colored("🔧 Debugging:", "yellow", attrs=["bold"])}
  {colored("debug", "white", attrs=["bold"])}                     - Toggle debug logging on/off
  {colored("cleanup", "red", attrs=["bold"])}                   - Kill StreamMonitor FFmpeg processes (safe)
  {colored("getjson", "magenta", attrs=["bold"])} <username> <site> - Debug: Print raw JSON response from model
  {colored("getjson", "magenta", attrs=["bold"])} <username> <site> {colored("edit", "cyan")} - Open JSON in editor

{colored("ℹ️ Other:", "yellow", attrs=["bold"])}
  {colored("help", "green")}                      - Show this help message
  {colored("cls", "cyan")}                       - Clear console screen
  {colored("quit", "red")}                      - Exit the application

{colored("💡 Notes:", "green", attrs=["bold"])}
  • Use {colored("*", "yellow", attrs=["bold"])} as username to apply command to all streamers
  • Use {colored("*", "yellow", attrs=["bold"])} as site to remove streamer from all sites
  • Site codes: {colored("SC", "green")}=Stripchat, {colored("CB", "magenta")}=Chaturbate, {colored("CS", "blue")}=CamSoda, {colored("SCVR", "cyan", attrs=["bold"])}=StripchatVR
  • Use {colored("sc*", "cyan", attrs=["bold"])} to add both SC and SCVR at once
  • {colored("VR streamers", "cyan", attrs=["bold", "underline"])} are specially highlighted! 🥽
"""
        return help_text.strip()

    def do_cls(self) -> str:
        """Clear the console screen."""
        import os
        os.system('cls' if os.name == 'nt' else 'clear')
        return ""  # Return empty string to avoid extra output

    def do_add(self, streamer: Optional[Bot], username: str, site: str) -> str:
        """Add a new streamer to monitor. Use sc* to add both SC and SCVR."""
        if not username or not site:
            return colored("⚠️ Missing username or site. Usage: add <username> <site>", "yellow")
        
        # Handle special sc* pattern
        if site.lower() == "sc*":
            results = []
            sites_to_add = ["SC", "SCVR"]
            
            for site_code in sites_to_add:
                existing = self.getStreamer(username, site_code)
                if existing:
                    results.append(colored(f"⚠️ [{site_code}] {username} already exists", "yellow"))
                    continue
                
                try:
                    new_streamer = Bot.createInstance(username, site_code)
                    if not new_streamer:
                        results.append(colored(f"❌ Failed to create [{site_code}] {username}", "red"))
                        continue
                    
                    with self._streamers_lock:
                        self.streamers.append(new_streamer)
                    
                    new_streamer.start()
                    new_streamer.restart()
                    
                    if "VR" in new_streamer.siteslug:
                        results.append(colored(f"🥽 Added VR streamer [{new_streamer.siteslug}] {new_streamer.username}", "cyan", attrs=["bold"]))
                    else:
                        results.append(colored(f"✅ Added [{new_streamer.siteslug}] {new_streamer.username}", "green", attrs=["bold"]))
                except Exception as e:
                    results.append(colored(f"❌ Failed to add [{site_code}] {username}: {e}", "red"))
            
            self.saveConfig()
            return "\n".join(results)
        
        # Regular single site addition
        if streamer:
            return colored('⚠️ Streamer already exists', 'yellow')
        
        try:
            streamer = Bot.createInstance(username, site)
            if not streamer:
                return colored(f"❌ Failed to create streamer instance for site: {site}", "red")
            
            with self._streamers_lock:
                self.streamers.append(streamer)
            
            streamer.start()
            streamer.restart()
            self.saveConfig()
            
            if "VR" in streamer.siteslug:
                return colored(f"🥽 Added VR streamer [{streamer.siteslug}] {streamer.username}", "cyan", attrs=["bold"])
            else:
                return colored(f"✅ Added [{streamer.siteslug}] {streamer.username}", "green", attrs=["bold"])
        except Exception as e:
            self.logger.error(f"Failed to add streamer: {e}")
            return colored(f"❌ Failed to add: {e}", "red")

    def do_remove(self, streamer: Optional[Bot], username: str, site: str) -> str:
        """Remove a streamer from monitoring. Use * as site to remove from all sites."""
        if not username:
            return colored("⚠️ Missing username. Usage: remove <username> [site|*]", "yellow")
        
        # Handle special * pattern
        if site.lower() == "*":
            with self._streamers_lock:
                streamers_to_remove = [s for s in self.streamers if s.username.lower() == username.lower()]
            
            if not streamers_to_remove:
                return colored(f"❌ No streamers found with username: {username}", "red")
            
            removed_count = 0
            removed_sites = []
            
            for s in streamers_to_remove:
                try:
                    s.stop(None, None, thread_too=True)
                    
                    # Clean up logger to prevent memory leak
                    logger_key = f"[{s.siteslug}] {s.username}"
                    Bot.cleanup_logger_cache(logger_key)
                    
                    with self._streamers_lock:
                        self.streamers.remove(s)
                    
                    removed_sites.append(s.siteslug)
                    removed_count += 1
                except Exception as e:
                    self.logger.error(f"Failed to remove {s.siteslug} {s.username}: {e}")
            
            if removed_count > 0:
                self.saveConfig()
                sites_str = ", ".join(removed_sites)
                return colored(f"🗑️ Removed {username} from {removed_count} site(s): {sites_str}", "green")
            else:
                return colored(f"❌ Failed to remove any instances of {username}", "red")
        
        # Original single streamer removal
        if not streamer:
            return colored("❌ Streamer not found", "red")
        
        try:
            streamer.stop(None, None, thread_too=True)
            
            # Clean up logger to prevent memory leak
            logger_key = f"[{streamer.siteslug}] {streamer.username}"
            Bot.cleanup_logger_cache(logger_key)
            
            with self._streamers_lock:
                self.streamers.remove(streamer)
            
            self.saveConfig()
            
            if "VR" in streamer.siteslug:
                return colored(f"🗑️ Removed VR streamer [{streamer.siteslug}] {streamer.username}", "cyan")
            else:
                return colored(f"🗑️ Removed [{streamer.siteslug}] {streamer.username}", "green")
        except Exception as e:
            self.logger.error(f"Failed to remove streamer: {e}")
            return colored(f"❌ Failed to remove streamer: {e}", "red")

    def do_start(self, streamer: Optional[Bot], username: str, site: str) -> str:
        """Start monitoring streamer(s) - use * for all"""
        if not streamer:
            if username == '*':
                started_count = 0
                with self._streamers_lock:
                    streamers_copy = list(self.streamers)
                
                for s in streamers_copy:
                    try:
                        success = self._ensure_streamer_running(s)
                        if success:
                            started_count += 1
                    except Exception as e:
                        self.logger.error(f"Failed to start {s.username}: {e}")
                
                self.saveConfig()
                return f"Started {started_count} streamers"
            else:
                return "Streamer not found"
        else:
            try:
                success = self._ensure_streamer_running(streamer)
                if success:
                    self.saveConfig()
                    return f"Started [{streamer.siteslug}] {streamer.username}"
                else:
                    return f"Failed to start [{streamer.siteslug}] {streamer.username}"
            except Exception as e:
                self.logger.error(f"Failed to start streamer: {e}")
                return f"Failed to start: {e}"

    def do_stop(self, streamer: Optional[Bot], username: str, site: str) -> str:
        """Stop monitoring streamer(s) - use * for all"""
        if not streamer:
            if username == '*':
                stopped_count = 0
                with self._streamers_lock:
                    streamers_copy = list(self.streamers)
                
                for s in streamers_copy:
                    try:
                        s.stop(None, None)
                        stopped_count += 1
                    except Exception as e:
                        self.logger.error(f"Failed to stop {s.username}: {e}")
                
                self.saveConfig()
                return f"Stopped {stopped_count} streamers"
            else:
                return "Streamer not found"
        else:
            try:
                streamer.stop(None, None)
                self.saveConfig()
                return f"Stopped [{streamer.siteslug}] {streamer.username}"
            except Exception as e:
                self.logger.error(f"Failed to stop streamer: {e}")
                return f"Failed to stop: {e}"

    def do_restart(self, streamer: Optional[Bot], username: str, site: str) -> str:
        """Restart a specific streamer"""
        if not streamer:
            return "Streamer not found"
        stop_result = self.do_stop(streamer, username, site)
        if "Failed" in stop_result:
            return stop_result
        start_result = self.do_start(streamer, username, site)
        return f"Restarted [{streamer.siteslug}] {streamer.username}" if "Failed" not in start_result else start_result

    def do_resync(self, username: str, site: str) -> str:
        """Force status recheck and restart downloads for streamer(s)"""
        if not username:
            return colored("⚠️ Missing username. Usage: resync <username|*> [site|*]", "yellow")
        
        # Handle * username (all streamers)
        if username == '*':
            resynced_count = 0
            results = []
            
            with self._streamers_lock:
                streamers_copy = list(self.streamers)
            
            # Filter by site if specified
            if site and site != "*":
                site_cls = Bot.str2site(site)
                if site_cls:
                    target_site = site_cls.site
                    streamers_copy = [s for s in streamers_copy if s.site == target_site]
                else:
                    return colored(f"❌ Unknown site: {site}", "red")
            
            for s in streamers_copy:
                try:
                    result = self._resync_streamer(s)
                    results.append(f"[{s.siteslug}] {s.username}: {result}")
                    if "resynced" in result.lower():
                        resynced_count += 1
                except Exception as e:
                    results.append(f"[{s.siteslug}] {s.username}: Error - {e}")
                    self.logger.error(f"Failed to resync {s.username}: {e}")
            
            self.saveConfig()
            header = colored(f"🔄 Resync Results ({resynced_count} successful):", "blue", attrs=["bold"])
            colored_results = []
            for result in results:
                if "resynced" in result.lower():
                    colored_results.append(colored(result, "green"))
                elif "error" in result.lower():
                    colored_results.append(colored(result, "red"))
                else:
                    colored_results.append(colored(result, "yellow"))
            return f"{header}\n" + "\n".join(colored_results)
        
        # Handle specific username
        # Find matching streamers
        matching_streamers = []
        with self._streamers_lock:
            for s in self.streamers:
                if s.username.lower() == username.lower():
                    if site and site != "*":
                        site_cls = Bot.str2site(site)
                        if site_cls and s.site == site_cls.site:
                            matching_streamers.append(s)
                    else:
                        matching_streamers.append(s)
        
        if not matching_streamers:
            return colored(f"❌ No streamers found matching: {username}" + (f" on {site}" if site and site != "*" else ""), "red")
        
        results = []
        for s in matching_streamers:
            try:
                result = self._resync_streamer(s)
                results.append(f"[{s.siteslug}] {s.username}: {result}")
            except Exception as e:
                results.append(f"[{s.siteslug}] {s.username}: Error - {e}")
                self.logger.error(f"Failed to resync {s.username}: {e}")
        
        self.saveConfig()
        
        if len(results) == 1:
            return results[0]
        else:
            header = colored("🔄 Resync Results:", "blue", attrs=["bold"])
            colored_results = []
            for result in results:
                if "resynced" in result.lower():
                    colored_results.append(colored(result, "green"))
                elif "error" in result.lower():
                    colored_results.append(colored(result, "red"))
                else:
                    colored_results.append(colored(result, "yellow"))
            return f"{header}\n" + "\n".join(colored_results)

    def _resync_streamer(self, streamer: Bot) -> str:
        """Force resync a single streamer - reset status and restart downloads if public"""
        try:
            # Force stop any current recording
            if streamer.recording:
                self.logger.info(f"Force stopping [{streamer.siteslug}] {streamer.username} for resync...")
                streamer.stop(None, None)
                
                # Wait for recording to stop
                wait_count = 0
                max_wait = 15  # 15 seconds timeout for resync
                while streamer.recording and wait_count < max_wait:
                    time.sleep(1)
                    wait_count += 1
                
                if streamer.recording:
                    return f"⚠️ Timeout stopping recording for resync"
            
            # Reset bot status to force fresh check
            streamer.sc = Status.UNKNOWN
            streamer.previous_status = None
            streamer.lastInfo = {}
            
            # Force a fresh status check
            try:
                new_status = streamer.getStatus()
                self.logger.info(f"[{streamer.siteslug}] {streamer.username} resync status: {new_status.name}")
                
                # If public and downloadable, force restart to begin download
                if new_status == Status.PUBLIC:
                    self._ensure_streamer_running(streamer)
                    return f"✅ Resynced and started download (PUBLIC)"
                elif new_status == Status.PRIVATE:
                    self._ensure_streamer_running(streamer)
                    return f"🔒 Resynced - in private show"
                elif new_status == Status.OFFLINE:
                    self._ensure_streamer_running(streamer)
                    return f"🟡 Resynced - model offline"
                elif new_status == Status.DELETED:
                    return f"🗑️ Model account deleted - will be auto-removed"
                elif new_status == Status.NOTEXIST:
                    return f"❌ Model does not exist - will be auto-removed"
                else:
                    self._ensure_streamer_running(streamer)
                    return f"🔄 Resynced - status: {new_status.name}"
                    
            except Exception as status_error:
                self.logger.error(f"Error getting status during resync for {streamer.username}: {status_error}")
                # Still try to restart
                self._ensure_streamer_running(streamer)
                return f"⚠️ Resynced with status error: {status_error}"
                
        except Exception as e:
            return f"❌ Resync failed: {e}"

    def do_move(self, streamer: Optional[Bot], username: str, site: str) -> str:
        """Move completed files to unprocessed folder."""
        unprocessed_dir = os.path.join(os.path.dirname(DOWNLOADS_DIR), "unprocessed")
        os.makedirs(unprocessed_dir, exist_ok=True)
        
        if not streamer:
            if username == '*':
                moved_count = 0
                results = []
                
                with self._streamers_lock:
                    streamers_copy = list(self.streamers)
                
                for s in streamers_copy:
                    result = self._move_streamer_files(s, unprocessed_dir)
                    if result == "Files moved successfully":
                        moved_count += 1
                        results.append(f"[{s.siteslug}] {s.username}: {result}")
                    elif result.startswith("Error"):
                        results.append(f"[{s.siteslug}] {s.username}: {result}")
                
                if moved_count == 0:
                    return colored("📁 No files to move", "yellow")
                elif results:
                    header = colored(f"📦 Moved files for {moved_count} streamer(s):", "green", attrs=["bold"])
                    colored_results = []
                    for result in results:
                        if "Files moved successfully" in result:
                            colored_results.append(colored(result, "green"))
                        else:
                            colored_results.append(colored(result, "red"))
                    return f"{header}\n" + "\n".join(colored_results)
                else:
                    return colored(f"✅ Moved files for {moved_count} streamer(s)", "green", attrs=["bold"])
            else:
                return "Streamer not found"
        else:
            result = self._move_streamer_files(streamer, unprocessed_dir)
            return f"[{streamer.siteslug}] {streamer.username}: {result}"

    def _move_streamer_files(self, streamer: Bot, unprocessed_dir: str) -> str:
        """Helper method to move files for a single streamer."""
        try:
            source_folder = streamer.outputFolder
            if not os.path.exists(source_folder):
                return "No download folder found"
                
            if not os.path.isdir(source_folder) or not os.listdir(source_folder):
                return "No files to move"
            
            was_running = streamer.running
            was_recording = streamer.recording
            
            # Stop the streamer if it's recording
            if was_recording:
                self.logger.info(f"Stopping [{streamer.siteslug}] {streamer.username} for file move...")
                streamer.stop(None, None)
                
                # Wait for recording to actually stop with timeout
                wait_count = 0
                max_wait = 30  # 30 seconds timeout
                while streamer.recording and wait_count < max_wait:
                    time.sleep(1)
                    wait_count += 1
                    if wait_count % 5 == 0:  # Log every 5 seconds
                        self.logger.info(f"Waiting for [{streamer.siteslug}] {streamer.username} to stop recording... ({wait_count}s)")
                
                if streamer.recording:
                    self.logger.warning(f"[{streamer.siteslug}] {streamer.username} didn't stop recording after {max_wait}s timeout")
                    if was_running:
                        self._ensure_streamer_running(streamer)
                    return f"Timeout waiting for recording to stop (waited {max_wait}s)"
                else:
                    self.logger.info(f"[{streamer.siteslug}] {streamer.username} stopped successfully")
                
            folder_name = f"{streamer.username} [{streamer.siteslug}]"
            dest_folder = os.path.join(unprocessed_dir, folder_name)
            
            if os.path.exists(dest_folder):
                existing_files = set(os.listdir(dest_folder)) if os.path.isdir(dest_folder) else set()
                source_files = set(os.listdir(source_folder)) if os.path.isdir(source_folder) else set()
                conflicts = existing_files.intersection(source_files)
                
                if conflicts:
                    if was_running:
                        self._ensure_streamer_running(streamer)
                    return f"Unable to move - conflicts detected: {', '.join(list(conflicts)[:3])}{'...' if len(conflicts) > 3 else ''}"
            
            if not os.path.exists(dest_folder):
                shutil.move(source_folder, dest_folder)
                # Don't recreate folder - bot will make it when needed
            else:
                for item in os.listdir(source_folder):
                    src_path = os.path.join(source_folder, item)
                    dst_path = os.path.join(dest_folder, item)
                    shutil.move(src_path, dst_path)
                
                # Remove empty source folder after moving all files
                try:
                    if os.path.exists(source_folder) and not os.listdir(source_folder):
                        os.rmdir(source_folder)
                        self.logger.info(f"Removed empty folder: {source_folder}")
                except OSError as e:
                    self.logger.warning(f"Could not remove empty folder {source_folder}: {e}")
            
            # Restart the streamer if it was running before
            if was_running:
                self.logger.info(f"Restarting [{streamer.siteslug}] {streamer.username}...")
                success = self._ensure_streamer_running(streamer)
                if success:
                    self.logger.info(f"[{streamer.siteslug}] {streamer.username} restarted successfully")
                else:
                    self.logger.warning(f"[{streamer.siteslug}] {streamer.username} failed to restart - may need manual intervention")
            
            return "Files moved successfully"
                
        except Exception as e:
            self.logger.error(f"Error moving files for {streamer.username}: {e}")
            if was_running:
                try:
                    self.logger.info(f"Attempting emergency restart for [{streamer.siteslug}] {streamer.username}")
                    self._ensure_streamer_running(streamer)
                except Exception as restart_error:
                    self.logger.error(f"Emergency restart failed for {streamer.username}: {restart_error}")
            return f"Error: {e}"

    def _ensure_streamer_running(self, streamer: Bot) -> bool:
        """Ensure a streamer is properly running, recreating if necessary"""
        try:
            # If thread is dead, we need to recreate the streamer
            if not streamer.is_alive():
                self.logger.warning(f"Thread is dead for [{streamer.siteslug}] {streamer.username}, recreating...")
                
                # Create new streamer instance
                new_streamer = Bot.createInstance(streamer.username, streamer.siteslug)
                if not new_streamer:
                    self.logger.error(f"Failed to recreate [{streamer.siteslug}] {streamer.username}")
                    return False
                
                # Replace in streamers list
                with self._streamers_lock:
                    try:
                        old_index = self.streamers.index(streamer)
                        self.streamers[old_index] = new_streamer
                        
                        # Clean up old logger
                        logger_key = f"[{streamer.siteslug}] {streamer.username}"
                        Bot.cleanup_logger_cache(logger_key)
                        
                    except ValueError:
                        self.logger.error(f"Could not find old streamer in list for {streamer.username}")
                        return False
                
                # Start the new streamer
                new_streamer.start()
                new_streamer.restart()
                
                self.logger.info(f"Successfully recreated [{new_streamer.siteslug}] {new_streamer.username}")
                return True
            else:
                # Thread is alive, just restart
                streamer.restart()
                return True
                
        except Exception as e:
            self.logger.error(f"Error ensuring streamer running for {streamer.username}: {e}")
            return False
        
    def do_status(self, streamer: Optional[Bot], username: str, site: str) -> str:
        """Show detailed status table of streamers"""
        output = [["Username", "Site", "Running", "Status", "Recording"]]

        def add_line(s: Bot) -> None:
            status_text = s.status() if hasattr(s, 'status') else str(s.sc)
            output.append([
                s.username,
                s.site,
                "Yes" if s.running else "No",
                status_text,
                "Yes" if s.recording else "No"
            ])

        if streamer:
            add_line(streamer)
        else:
            with self._streamers_lock:
                for s in self.streamers:
                    add_line(s)
        
        free_space = OOSDetector.free_space()
        header = f'Status Report - Free space: {round(free_space, 1)}%\n\n'
        return header + AsciiTable(output).table

    def do_status2(self, streamer: Optional[Bot], username: str, site: str) -> str:
        """Show colored grid status view with enhanced VR highlighting"""
        with self._streamers_lock:
            streamers_copy = list(self.streamers)
        
        if not streamers_copy:
            return colored("No streamers configured", "yellow")
        
        maxlen = max([len(s.username) for s in streamers_copy])
        termwidth = terminaltables.terminal_io.terminal_size()[0]
        table_nx = math.floor(termwidth/(maxlen+3))
        output = ''
        output += colored('🎥 Streamers Status Grid 🎥', 'cyan', attrs=['bold']) + '\n'

        for site_cls in Bot.loaded_sites:
            site_name = site_cls.site
            if "VR" in site_name:
                site_header = colored(f'🥽 {site_name} 🥽', 'cyan', attrs=['bold', 'underline'])
            else:
                site_colors = {
                    "StripChat": ("green", ["bold"]),
                    "Chaturbate": ("magenta", ["bold"]),
                    "CamSoda": ("blue", ["bold"]),
                    "Cherry.tv": ("red", ["bold"]),
                    "Cam4": ("yellow", ["bold"]),
                    "MyFreeCams": ("white", ["bold"]),
                    "BongaCams": ("green", []),
                    "AmateurTV": ("blue", [])
                }
                color, attrs = site_colors.get(site_name, ("white", []))
                site_header = colored(f'🔹 {site_name}', color, attrs=attrs)
            
            output += f'\n{site_header}\n'
            output += ('+' + '-'*(maxlen+2))*table_nx + '+\n'
            
            i = 0
            for s in streamers_copy:
                if s.site == site_name:
                    output += '|'
                    
                    if "VR" in s.siteslug:
                        if s.sc == Status.PUBLIC: 
                            status_color = 'red'
                            attrs = ['bold', 'blink']
                            prefix = '🔴'
                        elif s.sc == Status.PRIVATE: 
                            status_color = 'magenta'
                            attrs = ['bold']
                            prefix = '💎'
                        elif s.sc == Status.ERROR: 
                            status_color = 'red'
                            attrs = ['bold']
                            prefix = '❌'
                        elif s.sc == Status.OFFLINE:
                            status_color = 'cyan'
                            attrs = []
                            prefix = '⭕'
                        elif not s.running: 
                            status_color = 'grey'
                            attrs = []
                            prefix = '⚫'
                        else:
                            status_color = 'cyan'
                            attrs = []
                            prefix = '🥽'
                    else:
                        if s.sc == Status.PUBLIC: 
                            status_color = 'green'
                            attrs = ['bold']
                            prefix = '🟢'
                        elif s.sc == Status.PRIVATE: 
                            status_color = 'magenta'
                            attrs = ['bold']
                            prefix = '🔒'
                        elif s.sc == Status.ERROR: 
                            status_color = 'red'
                            attrs = ['bold']
                            prefix = '⚠️'
                        elif s.sc == Status.OFFLINE:
                            status_color = 'yellow'
                            attrs = []
                            prefix = '🟡'
                        elif not s.running: 
                            status_color = 'grey'
                            attrs = []
                            prefix = '⚪'
                        else:
                            status_color = 'white'
                            attrs = []
                            prefix = '🔺'
                    
                    username_display = f"{prefix} {s.username}"
                    username_padded = ' ' + username_display + ' '*(maxlen-len(username_display)+1) + ' '
                    output += colored(username_padded, status_color, attrs=attrs)
                    i += 1
                    if i == table_nx:
                        output += '|\n'
                        i = 0
            
            for r in range(i, table_nx):
                output += '| ' + ' ' * maxlen + ' '
            if i > 0:
                output += '|\n'
            output += ('+' + '-'*(maxlen+2))*table_nx + '+\n'
        
        return output

    def do_debug(self) -> str:
        """Toggle debug logging on/off"""
        import parameters
        parameters.DEBUG = not parameters.DEBUG
        
        import logging
        level = logging.DEBUG if parameters.DEBUG else logging.INFO
        
        with self._streamers_lock:
            for s in self.streamers:
                if hasattr(s, 'logger'):
                    s.logger.setLevel(level)
        
        self.logger.get_logger().setLevel(level)
        
        status = "enabled" if parameters.DEBUG else "disabled"
        return f"Debug logging {status}"



    def do_cleanup(self) -> str:
        """
        SAFELY kill only StreamMonitor-related FFmpeg processes.
        Uses strict multi-criteria detection to avoid killing unrelated processes.
        """
        try:
            killed_count = 0
            current_pid = os.getpid()
            streammonitor_pids = set()
            
            # First pass: identify StreamMonitor process tree
            try:
                current_proc = psutil.Process(current_pid)
                streammonitor_pids.add(current_pid)
                
                # Add all children recursively
                for child in current_proc.children(recursive=True):
                    streammonitor_pids.add(child.pid)
            except Exception:
                pass
            
            # Get output folders from streamers
            output_folders = set()
            with self._streamers_lock:
                for s in self.streamers:
                    try:
                        output_folders.add(os.path.normpath(s.outputFolder))
                    except Exception:
                        pass
            
            # Add common StreamMonitor paths
            streammonitor_paths = {
                os.path.normpath(DOWNLOADS_DIR),
                os.path.normpath(os.path.join(os.path.dirname(DOWNLOADS_DIR), "unprocessed")),
                os.path.normpath(os.path.join(os.getcwd(), "M3U8_TMP"))
            }
            streammonitor_paths.update(output_folders)
            
            # Second pass: find FFmpeg processes
            for proc in psutil.process_iter(['pid', 'name', 'cmdline', 'ppid']):
                try:
                    proc_info = proc.info
                    proc_name = proc_info.get('name', '').lower()
                    cmdline = proc_info.get('cmdline', [])
                    
                    # Must be FFmpeg
                    if 'ffmpeg' not in proc_name and not any('ffmpeg' in str(arg).lower() for arg in cmdline):
                        continue
                    
                    # Strict criteria - ALL must match
                    is_ours = False
                    cmdline_str = ' '.join(cmdline) if cmdline else ''
                    
                    # Check 1: Part of our process tree
                    if proc.pid in streammonitor_pids:
                        is_ours = True
                    
                    # Check 2: Working with our paths
                    if not is_ours:
                        for path in streammonitor_paths:
                            if path and path in cmdline_str:
                                is_ours = True
                                break
                    
                    # Check 3: HLS patterns + our domains
                    if not is_ours:
                        has_hls = any(p in cmdline_str.lower() for p in ['m3u8', 'rolling.m3u8', '.tmp.ts'])
                        has_our_domains = any(d in cmdline_str.lower() for d in ['stripchat', 'chaturbate', 'camsoda', 'doppiocdn'])
                        if has_hls and has_our_domains:
                            is_ours = True
                    
                    if is_ours:
                        proc.kill()
                        killed_count += 1
                        self.logger.info(f"Killed FFmpeg process PID {proc.pid}")
                        
                except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
                    continue
                except Exception as e:
                    self.logger.debug(f"Error processing proc: {e}")
                    continue
            
            if killed_count > 0:
                return colored(f"🔫 Killed {killed_count} StreamMonitor FFmpeg process(es)", "red", attrs=["bold"])
            else:
                return colored("✅ No StreamMonitor FFmpeg processes found", "green")
                
        except Exception as e:
            return colored(f"⚠️ Error during FFmpeg cleanup: {e}", "yellow")

    def do_getjson(self, username: str, site: str, edit_mode: str = "") -> str:
        """Debug utility: Print raw JSON response or open in editor"""
        if not username or not site:
            available_sites = []
            for sitecls in Bot.loaded_sites:
                available_sites.append(f"{sitecls.siteslug}({sitecls.site})")
            sites_list = ", ".join(sorted(available_sites))
            return colored(f"⚠️ Missing username or site. Usage: getjson <username> <site> [edit]\nAvailable sites: {sites_list}", "yellow")
        
        try:
            site_cls = Bot.str2site(site)
            if not site_cls:
                available_sites = []
                for sitecls in Bot.loaded_sites:
                    available_sites.append(f"{sitecls.siteslug}({sitecls.site})")
                sites_list = ", ".join(sorted(available_sites))
                return colored(f"❌ Unknown site: {site}. Available sites: {sites_list}", "red")
            
            temp_bot = site_cls(username)
            site_name = getattr(temp_bot, 'siteslug', site.upper())
            
            try:
                json_data = None
                fetch_method = "unknown"
                
                if hasattr(temp_bot, 'fetchJsonData'):
                    json_data = temp_bot.fetchJsonData()
                    fetch_method = "fetchJsonData"
                elif hasattr(temp_bot, 'getJsonData'):
                    json_data = temp_bot.getJsonData()
                    fetch_method = "getJsonData"
                elif hasattr(temp_bot, 'lastInfo'):
                    temp_bot.getStatus()
                    json_data = temp_bot.lastInfo
                    fetch_method = "lastInfo (via getStatus)"
                else:
                    return colored("❌ Unable to fetch JSON data - no suitable method found", "red")
                
                if json_data is None:
                    return colored(f"❌ No JSON data received from {site_name}", "red")
                
                formatted_json = json.dumps(json_data, indent=2, ensure_ascii=False)
                
                if edit_mode.lower() == "edit":
                    return self._open_json_in_editor(formatted_json, username, site_name)
                
                result = []
                result.append(colored(f"📄 JSON Response for [{site_name}] {username}:", "cyan", attrs=["bold"]))
                result.append(colored("=" * 60, "cyan"))
                result.append(colored("✅ Status: OK", "green", attrs=["bold"]))
                result.append(colored(f"📊 Data size: {len(str(json_data))} characters", "blue"))
                result.append(colored(f"🔧 Fetch method: {fetch_method}", "magenta"))
                result.append(colored("📋 Raw JSON:", "yellow", attrs=["bold"]))
                result.append(colored(formatted_json, "white"))
                result.append(colored("=" * 60, "cyan"))
                result.append(colored("💡 Tip: Use 'getjson <username> <site> edit' to open in editor", "green"))
                
                return "\n".join(result)
                
            except Exception as fetch_error:
                return colored(f"❌ Error fetching JSON data: {fetch_error}", "red")
                
        except Exception as e:
            return colored(f"❌ Error creating bot instance: {e}", "red")

    def _open_json_in_editor(self, json_content: str, username: str, site: str) -> str:
        """Open JSON in VS Code or default editor"""
        import tempfile
        import subprocess
        
        try:
            with tempfile.NamedTemporaryFile(
                mode='w', 
                suffix=f'_streammonitor_{username}_{site}.json', 
                delete=False, 
                encoding='utf-8'
            ) as temp_file:
                header = f"// StreamMonitor JSON Debug - [{site}] {username}\n// Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}\n// Use Ctrl+Shift+P -> 'Format Document' to format JSON\n\n"
                temp_file.write(header + json_content)
                temp_path = temp_file.name
            
            # Try VS Code with common installation paths
            vscode_paths = [
                'code',  # In PATH
                r'C:\Program Files\Microsoft VS Code\Code.exe',
                r'C:\Program Files (x86)\Microsoft VS Code\Code.exe',
                r'C:\Users\{}\AppData\Local\Programs\Microsoft VS Code\Code.exe'.format(os.getenv('USERNAME', '')),
                r'C:\Program Files\Microsoft VS Code\bin\code.cmd',
                r'C:\Program Files (x86)\Microsoft VS Code\bin\code.cmd'
            ]
            
            for vscode_path in vscode_paths:
                try:
                    if vscode_path != 'code' and not os.path.exists(vscode_path):
                        continue
                    
                    subprocess.Popen([vscode_path, temp_path], 
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
                    return colored(f"📝 JSON opened in VS Code: {temp_path}", "green", attrs=["bold"])
                except (FileNotFoundError, OSError):
                    continue
            
            # Fallback: try to open with default system editor
            try:
                if os.name == 'nt':  # Windows
                    os.startfile(temp_path)
                    return colored(f"📝 JSON opened with default editor: {temp_path}", "green", attrs=["bold"])
                else:  # Unix/Linux/Mac
                    subprocess.call(['open' if sys.platform == 'darwin' else 'xdg-open', temp_path])
                    return colored(f"📝 JSON opened with default editor: {temp_path}", "green", attrs=["bold"])
            except Exception:
                return colored(f"❌ Could not open editor. JSON saved to: {temp_path}\n💡 You can open this file manually", "yellow")
                    
        except Exception as e:
            return colored(f"❌ Error creating temporary file: {e}", "red")