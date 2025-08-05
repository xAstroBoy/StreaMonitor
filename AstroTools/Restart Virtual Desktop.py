import ctypes
import sys
import subprocess

# Define the processes to kill
processes_to_kill = [
    "VirtualDesktop.Service.exe",
    "VirtualDesktop.Server.exe",
    "VirtualDesktop.Streamer.exe"
]

# Terminate the processes
for process in processes_to_kill:
    subprocess.run(["taskkill", "/F", "/IM", process], shell=True)

# Path to the executable
executable_path = r'C:\"Program Files"\\"Virtual Desktop Streamer"\\VirtualDesktop.Streamer.exe'

# Command to run the executable as administrator
# We'll use a different approach to construct the command
run_as_admin_command = ["powershell", "Start-Process", "-FilePath", executable_path, "-Verb", "runAs"]

# Execute the command
subprocess.run(run_as_admin_command, shell=True)
