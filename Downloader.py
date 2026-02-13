import os
import sys
import time
import atexit
import psutil
import streamonitor.config as config
from streamonitor.managers.httpmanager import HTTPManager
from streamonitor.managers.climanager import CLIManager
from streamonitor.managers.zmqmanager import ZMQManager
from streamonitor.managers.outofspace_detector import OOSDetector
from streamonitor.clean_exit import CleanExit
import streamonitor.sites  # must have
import urllib3
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# Lock file to prevent multiple instances
LOCK_FILE = "streammonitor.lock"

def cleanup_ffmpeg_processes():
    """Manual cleanup function to kill only StreamMonitor-related FFmpeg processes."""
    try:
        killed_count = 0
        current_pid = os.getpid()
        
        print("🔍 Searching for StreamMonitor FFmpeg processes...")
        
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
                        print(f"🔫 Killing StreamMonitor FFmpeg: {proc_info['pid']} - {proc_name}")
                        proc.kill()
                        killed_count += 1
                        
            except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
                # Process might have ended or we don't have permission
                continue
            except Exception:
                # Ignore other errors during cleanup
                continue
        
        if killed_count > 0:
            print(f"🧹 Cleaned up {killed_count} StreamMonitor FFmpeg process(es)")
            time.sleep(1)  # Give processes time to die
        else:
            print("✅ No StreamMonitor FFmpeg processes found to clean up")
            
    except Exception as e:
        print(f"⚠️ Error during FFmpeg cleanup: {e}")

def create_lock_file():
    """Create lock file with current process ID"""
    try:
        with open(LOCK_FILE, 'w') as f:
            f.write(str(os.getpid()))
        # Register cleanup on exit
        atexit.register(remove_lock_file)
        return True
    except Exception as e:
        print(f"Failed to create lock file: {e}")
        return False

def remove_lock_file():
    """Remove lock file on exit and cleanup FFmpeg processes"""
    print("🛑 Application shutting down...")
    cleanup_ffmpeg_processes()
    try:
        if os.path.exists(LOCK_FILE):
            os.remove(LOCK_FILE)
            print("🔓 Removed lock file")
    except Exception:
        pass  # Ignore errors during cleanup

def check_instance_running():
    """Check if another instance is already running"""
    if not os.path.exists(LOCK_FILE):
        return False
    
    try:
        with open(LOCK_FILE, 'r') as f:
            pid = int(f.read().strip())
        
        # Check if process is still running
        if sys.platform == "win32":
            import subprocess
            try:
                # Use tasklist which is more reliable on modern Windows
                result = subprocess.run(['tasklist', '/FI', f'PID eq {pid}'], 
                                      capture_output=True, text=True, timeout=10)
                # If the PID is found in output, process exists
                if str(pid) in result.stdout and "No tasks" not in result.stdout:
                    return True
                else:
                    # Process doesn't exist, remove stale lock file
                    remove_lock_file()
                    return False
            except (subprocess.TimeoutExpired, subprocess.CalledProcessError, FileNotFoundError):
                # If tasklist fails, try wmic as fallback
                try:
                    result = subprocess.run(['wmic', 'process', 'where', f'processid={pid}', 'get', 'processid'], 
                                          capture_output=True, text=True, timeout=5)
                    if str(pid) in result.stdout and "No tasks" not in result.stdout:
                        return True
                    else:
                        remove_lock_file()
                        return False
                except:
                    # If all Windows methods fail, assume no conflict
                    remove_lock_file()
                    return False
        else:
            try:
                os.kill(pid, 0)  # Signal 0 doesn't kill, just checks if process exists
                return True
            except OSError:
                # Process doesn't exist, remove stale lock file
                remove_lock_file()
                return False
    except (ValueError, FileNotFoundError, IOError):
        # Invalid or missing lock file
        remove_lock_file()
        return False


        
def is_docker():
    path = '/proc/self/cgroup'
    return (
        os.path.exists('/.dockerenv') or
        os.path.isfile(path) and any('docker' in line for line in open(path))
    )


def main():
    # Check for cleanup command
    if len(sys.argv) > 1 and sys.argv[1].lower() in ['cleanup', 'kill-ffmpeg', '--cleanup', '--kill-ffmpeg']:
        print("🧹 Manual FFmpeg cleanup requested...")
        cleanup_ffmpeg_processes()
        return
    
    # Check for existing instance
    if check_instance_running():
        print("❌ StreamMonitor is already running!")
        print("💡 If you're sure it's not running, delete 'streammonitor.lock' file")
        print("💡 Or run 'python Downloader.py cleanup' to kill FFmpeg processes")
        sys.exit(1)
    
    # Create lock file
    if not create_lock_file():
        print("❌ Failed to create lock file. Another instance might be starting.")
        sys.exit(1)
    
    print("🎬 Starting StreamMonitor...")
    
    if not OOSDetector.disk_space_good():
        print(OOSDetector.under_threshold_message)
        sys.exit(1)

    streamers = config.loadStreamers()

    clean_exit = CleanExit(streamers)

    oos_detector = OOSDetector(streamers)
    oos_detector.start()

    if not is_docker():
        console_manager = CLIManager(streamers)
        console_manager.start()

    zmq_manager = ZMQManager(streamers)
    zmq_manager.start()

    http_manager = HTTPManager(streamers)
    http_manager.start()


main()
