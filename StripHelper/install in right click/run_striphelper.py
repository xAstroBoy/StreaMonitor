import os
import subprocess
import sys
import concurrent.futures
import ctypes


# Check if the script is already running with admin privileges
def is_admin():
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except:
        return False

# If not running with admin privileges, relaunch the script with admin privileges
if not is_admin():
    ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, " ".join(sys.argv), None, 1)
    sys.exit(0)


# Check if the root folder path is provided as a command-line argument
if len(sys.argv) > 1:
    root_folder = sys.argv[1]
else:
    print("Root folder path not provided.")
    sys.exit(1)

# Replace forward slashes with backslashes
root_folder = root_folder.replace("/", "\\")

# Check if the root folder exists
if not os.path.exists(root_folder):
    print(f"Root folder not found: {root_folder}")
    sys.exit(1)

# Get the list of subfolders in the root folder
subfolders = [f.path for f in os.scandir(root_folder) if f.is_dir()]

def process_folder(folder_path):    
    # Construct the command to execute
    command = f'cmd.exe /c python "C:\\AstroTools\\StripHelperv2.py" "{folder_path}"'
    # Execute the command
    subprocess.run(command, shell=True)

# Execute the process_folder function for each subfolder in parallel using multiple threads
with concurrent.futures.ThreadPoolExecutor() as executor:
    executor.map(process_folder, subfolders)
