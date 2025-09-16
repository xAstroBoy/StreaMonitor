import winreg
import os
import subprocess

def create_registry_entry():
    key_path = r"Directory\Background\shell\MergeAllFilesSymlink"
    command = r'cmd.exe /c python "C:\AstroTools\run_striphelper_Symlinks.py" "\"%V\""'

    try:
        # Open the key for writing
        key = winreg.CreateKey(winreg.HKEY_CLASSES_ROOT, key_path)

        # Set the name of the key
        winreg.SetValue(key, None, winreg.REG_SZ, "Merge all StreaMonitor Files in folders With SymLinks")

        # Create the command subkey
        command_key = winreg.CreateKey(key, "command")

        # Set the command for the key
        winreg.SetValue(command_key, None, winreg.REG_SZ, command)

    except WindowsError as e:
        print("Error creating registry entry:", e)
        return False

    return True

if __name__ == "__main__":
    if create_registry_entry():
        print("Registry entry created successfully.")
    else:
        print("Error creating registry entry.")
