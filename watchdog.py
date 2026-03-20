#!/usr/bin/env python3
"""
StreaMonitor Watchdog - Keeps the application running and restarts it if it crashes.
Usage: python watchdog.py [--gui|--cli] [other StreaMonitor args]
"""

import os
import sys
import subprocess
import time
import signal
import argparse
from pathlib import Path

# Global flag for shutdown
shutdown_requested = False

def signal_handler(signum, frame):
    """Handle shutdown signals"""
    global shutdown_requested
    print(f"\nReceived signal {signum}, shutting down gracefully...")
    shutdown_requested = True

def find_executable():
    """Find the StreaMonitor executable"""
    if sys.platform.startswith('win'):
        exe_names = ['StreaMonitor.exe', 'StreaMonitor-Windows-x64.exe']
    else:
        exe_names = ['StreaMonitor', 'StreaMonitor-Linux-x64']
    
    # Check in current directory first
    for name in exe_names:
        if os.path.exists(name):
            return os.path.abspath(name)
    
    # Check in dist/ directory
    for name in exe_names:
        dist_path = os.path.join('dist', name)
        if os.path.exists(dist_path):
            return os.path.abspath(dist_path)
    
    # Check in build directory
    build_dir = 'build/Release' if sys.platform.startswith('win') else 'build'
    for name in exe_names:
        build_path = os.path.join(build_dir, name)
        if os.path.exists(build_path):
            return os.path.abspath(build_path)
    
    return None

def run_with_watchdog(exe_path, args, max_crashes=10, min_uptime=30):
    """Run the application with crash detection and restart"""
    crash_count = 0
    start_time = time.time()
    
    print(f"Starting StreaMonitor watchdog...")
    print(f"Executable: {exe_path}")
    print(f"Max crashes: {max_crashes}")
    print(f"Min uptime: {min_uptime}s")
    print("-" * 50)
    
    while not shutdown_requested and crash_count < max_crashes:
        try:
            run_start = time.time()
            print(f"[{time.strftime('%H:%M:%S')}] Starting StreaMonitor (attempt {crash_count + 1})")
            
            # Start the process
            process = subprocess.Popen(
                [exe_path] + args,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                bufsize=1
            )
            
            # Monitor the process
            while process.poll() is None and not shutdown_requested:
                try:
                    # Read output line by line with timeout
                    line = process.stdout.readline()
                    if line:
                        print(f"[APP] {line.rstrip()}")
                    time.sleep(0.1)
                except Exception:
                    break
            
            # Process has ended
            exit_code = process.poll()
            run_duration = time.time() - run_start
            
            if shutdown_requested:
                print(f"[{time.strftime('%H:%M:%S')}] Shutdown requested, terminating...")
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                break
            
            print(f"[{time.strftime('%H:%M:%S')}] StreaMonitor exited with code {exit_code} after {run_duration:.1f}s")
            
            # Check if this was a crash (short runtime or non-zero exit)
            if run_duration < min_uptime or exit_code != 0:
                crash_count += 1
                print(f"[{time.strftime('%H:%M:%S')}] Detected crash #{crash_count}")
                
                # Check for crash files
                crashes_dir = Path("crashes")
                if crashes_dir.exists():
                    crash_files = list(crashes_dir.glob("crash_*.txt"))
                    recent_crashes = [f for f in crash_files if f.stat().st_mtime > run_start]
                    if recent_crashes:
                        latest_crash = max(recent_crashes, key=lambda f: f.stat().st_mtime)
                        print(f"[{time.strftime('%H:%M:%S')}] Crash report: {latest_crash}")
                
                if crash_count >= max_crashes:
                    print(f"[{time.strftime('%H:%M:%S')}] Max crashes ({max_crashes}) reached, giving up")
                    break
                
                # Wait before restarting
                wait_time = min(5 * crash_count, 30)  # Exponential backoff, max 30s
                print(f"[{time.strftime('%H:%M:%S')}] Waiting {wait_time}s before restart...")
                for i in range(wait_time):
                    if shutdown_requested:
                        break
                    time.sleep(1)
            else:
                # Clean exit, reset crash counter
                print(f"[{time.strftime('%H:%M:%S')}] Clean exit detected")
                crash_count = 0
                break
                
        except KeyboardInterrupt:
            print(f"[{time.strftime('%H:%M:%S')}] Keyboard interrupt, shutting down...")
            shutdown_requested = True
            if 'process' in locals() and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
            break
        except Exception as e:
            print(f"[{time.strftime('%H:%M:%S')}] Watchdog error: {e}")
            crash_count += 1
            time.sleep(5)
    
    total_runtime = time.time() - start_time
    print(f"\\n[{time.strftime('%H:%M:%S')}] Watchdog exiting after {total_runtime:.1f}s")
    print(f"Total crashes detected: {crash_count}")

def main():
    """Main entry point"""
    # Set up signal handlers
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    parser = argparse.ArgumentParser(
        description="StreaMonitor Watchdog - Keeps the application running",
        add_help=False  # We'll forward all args to the main app
    )
    
    # Parse known args only
    parser.add_argument('--max-crashes', type=int, default=10, 
                       help='Maximum crashes before giving up (default: 10)')
    parser.add_argument('--min-uptime', type=int, default=30,
                       help='Minimum uptime to not count as crash (default: 30s)')
    parser.add_argument('--no-watchdog', action='store_true',
                       help='Run once without restart on crash')
    
    # Split known and unknown args
    known_args, unknown_args = parser.parse_known_args()
    
    # Find the executable
    exe_path = find_executable()
    if not exe_path:
        print("ERROR: Could not find StreaMonitor executable!")
        print("Please ensure StreaMonitor is built and available in one of:")
        print("  - Current directory")
        print("  - dist/ directory") 
        print("  - build/Release/ directory (Windows)")
        print("  - build/ directory (Linux)")
        return 1
    
    if known_args.no_watchdog:
        # Run once without watchdog
        print(f"Running StreaMonitor once: {exe_path}")
        return subprocess.run([exe_path] + unknown_args).returncode
    else:
        # Run with watchdog
        run_with_watchdog(exe_path, unknown_args, known_args.max_crashes, known_args.min_uptime)
        return 0

if __name__ == "__main__":
    sys.exit(main())