import os
import sys
import argparse
import tkinter as tk
from tkinter import filedialog
from pathlib import Path

def isZeroByteFile(filepath):
    return os.path.getsize(filepath) == 0

def isContentEmpty(filepath):
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
        return not content.strip()

def delete_file(filepath, reason, quiet):
    try:
        os.remove(filepath)
        if not quiet:
            print(f'Deleted {reason}: {filepath}')
    except Exception as e:
        print(f'Error deleting {reason} {filepath}: {e}')

def delete_directory(directory, reason, quiet):
    try:
        os.rmdir(directory)
        if not quiet:
            print(f'Deleted {reason}: {directory}')
    except OSError as e:
        print(f'Error deleting {reason} {directory}: {e}')

def delete_unwanted_files(directory, extensions, quiet, delete_all):
    # check if directory exists
    if not Path(directory).is_dir():
        if not quiet:
            print(f"Directory not found: {directory}")
        return

    if delete_all:
        try:
            os.rmdir(directory)
            if not quiet:
                print(f'Deleted all files and directory: {directory}')
        except OSError as e:
            print(f'Error deleting all files and directory {directory}: {e}')
        return

    for path in Path(directory).rglob("*"):
        if path.is_file():
            file_extension = path.suffix.lower()
            if file_extension in extensions:
                if isZeroByteFile(path):
                    delete_file(path, 'Empty File', quiet)
                else:
                    if isContentEmpty(path):
                        delete_file(path, 'Empty File', quiet)
            else:
                delete_file(path, 'Unwanted File', quiet)
        elif path.is_dir() and not any(path.iterdir()):
            delete_directory(path, 'Empty Directory', quiet)

def main():
    parser = argparse.ArgumentParser(description='Delete unwanted files and empty directories.')
    parser.add_argument('directory', nargs='?', default='', help='Directory path')
    parser.add_argument('--extensions', help='File extensions to preserve (comma-separated, no spaces or dots)')
    parser.add_argument('--quiet', action='store_true', help='Run in quiet mode')
    parser.add_argument('--delete_all', action='store_true', help='Delete all files and the directory')

    args = parser.parse_args()

    if args.directory:
        directory = args.directory
    else:
        root = tk.Tk()
        root.withdraw()  # Hide the main window
        directory = filedialog.askdirectory()  # Show the folder dialog

    if not Path(directory).is_dir():
        print(f"Directory not found: {directory}")
        return

    extensions_input = args.extensions or input('Enter the file extensions to preserve (comma-separated, no spaces or dots): ')
    extensions = set(f".{ext.lower()}" for ext in extensions_input.split(','))  # Use a set for faster membership testing

    delete_unwanted_files(directory, extensions, args.quiet, args.delete_all)
    delete_unwanted_files(directory, extensions, args.quiet, args.delete_all) # Run twice to delete empty directories

if __name__ == "__main__":
    main()
