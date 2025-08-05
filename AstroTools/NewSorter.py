import os
import sys
import ctypes
import shutil
import json
import datetime
import logging
import re

# Check for admin privileges
def is_admin():
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except:
        return False

if not is_admin():
    ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, " ".join(sys.argv), None, 1)
    sys.exit(0)

CONFIG_PATH = "F:\\config.json"
UNSORTED_DIR = "F:\\Unsorted"
SORTED_DIR = "F:\\StripChat"
NEW_VIDEOS_DIR = "F:\\New Videos"
STRIPCHAT_MOBILE_DIR = "F:\\Stripchat Mobile"
STRIPCHAT_VR_DIR = "F:\\Stripchat VR"
STRIPCHAT_NORMAL_DIR = "F:\\Stripchat Normal"

# Setup logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

# Load configuration
with open(CONFIG_PATH, 'r') as f:
    config = json.load(f)

def extract_model_name(folder_name, RemoveTags=True):
    # Remove VR and Desktop tags
    if RemoveTags:
        for tag in config['VR'] + config['Desktop']:
            folder_name = folder_name.replace(tag, '').strip()
    
    # Replace aliases
    for model, aliases in config['Aliases'].items():
        for alias in aliases:
            if alias in folder_name:
                folder_name = folder_name.replace(alias, model)
                break

    return folder_name

def find_model_name(path, Cleaned=False):
    """Find the model name in the path using regex."""
    # Use regex to find a pattern that matches "ModelName [Tag]"
    match = re.search(r'([^\\]+?\[.*?\])', path)
    if match:
        model_name = match.group(1)
        if Cleaned:
            return extract_model_name(model_name, True)
        else:
            return model_name
    return None


def extract_tag_with_brackets_from_folder_name(folder_name):
    match = re.search(r'(\[[^\]]+\])', folder_name)
    return match.group(1) if match else None


def identify_folder_by_tag(file_name):
    for tag in config['VR']:
        if tag in file_name:
            return "VR"
    for tag in config['Desktop']:
        if tag in file_name:
            return "NO VR"
    return None

def sort_files_from_directory(directory):
    for entry in os.scandir(directory):
        if entry.is_file() and entry.name.endswith('.mp4'):
            file_name = entry.name
            file_path = entry.path
            parent_folder_name = os.path.basename(os.path.dirname(file_path))
            is_mobile = parent_folder_name == 'Mobile'
            model_name = find_model_name(file_path, True)
            Uncleaned_Model_Name = find_model_name(file_path, False)
            Tag = extract_tag_with_brackets_from_folder_name(Uncleaned_Model_Name)
            # Remove the .mp4 extension
            base_name = os.path.splitext(file_name)[0]
            
            try:
                # Extract date
                day, month, year = map(int, base_name.split('-')[:3])
                date_obj = datetime.datetime(year, month, day)
            except ValueError:
                logging.warning(f"Filename {base_name} doesn't match expected format. Skipping.")
                continue
            
            # Identify folder by tag
            folder_type = identify_folder_by_tag(Uncleaned_Model_Name)
            if not folder_type:
                logging.warning(f"Could not identify folder type for: {file_name}")
                continue

            base_path = os.path.join(SORTED_DIR, model_name, str(year), date_obj.strftime('%B'), folder_type)

            if is_mobile:
                target_path = os.path.join(base_path, 'Mobile', f"{day}.mp4")
            else:
                target_path = os.path.join(base_path, f"{day}.mp4")            
            
            # Create directories if they don't exist
            os.makedirs(os.path.dirname(target_path), exist_ok=True)
            
            # Move the file
            if os.path.exists(target_path):
                # Append the tag to the day
                new_filename = f"{day} {Tag}"
                # Create the new target path
                if is_mobile:
                    target_path = os.path.join(base_path, 'Mobile', f"{new_filename}.mp4")
                else:
                    target_path = os.path.join(base_path, f"{new_filename}.mp4")            
                    
            shutil.move(file_path, target_path)
            logging.info(f"Moved {file_name} to {target_path}")

            # Clean up old directories if they are empty
            old_dir = os.path.dirname(file_path)
            while old_dir != UNSORTED_DIR:
                if not os.listdir(old_dir):
                    os.rmdir(old_dir)
                    logging.info(f"Removed empty directory: {old_dir}")
                old_dir = os.path.dirname(old_dir)

            # Create symlink in New Videos directory
            symlink_path = target_path.replace(SORTED_DIR, NEW_VIDEOS_DIR)
            os.makedirs(os.path.dirname(symlink_path), exist_ok=True)
            os.symlink(target_path, symlink_path)
            logging.info(f"Created symlink for {day}.mp4 at {symlink_path}")

        elif entry.is_dir():
            sort_files_from_directory(entry.path)

def create_mobile_symlinks_for_existing():
    os.makedirs(STRIPCHAT_MOBILE_DIR, exist_ok=True)
    for model in os.listdir(SORTED_DIR):
        model_dir = os.path.join(SORTED_DIR, model)
        if not os.path.isdir(model_dir):
            continue
        for year in os.listdir(model_dir):
            year_dir = os.path.join(model_dir, year)
            if not os.path.isdir(year_dir):
                continue
            for month in os.listdir(year_dir):
                month_dir = os.path.join(year_dir, month)
                if not os.path.isdir(month_dir):
                    continue
                no_vr_dir = os.path.join(month_dir, 'NO VR')
                mobile_dir = os.path.join(no_vr_dir, 'Mobile')
                if not os.path.isdir(mobile_dir):
                    continue
                for f in os.listdir(mobile_dir):
                    if not f.endswith('.mp4'):
                        continue
                    src = os.path.join(mobile_dir, f)
                    dst = os.path.join(STRIPCHAT_MOBILE_DIR, model, year, month, f)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    if not os.path.exists(dst):
                        os.symlink(src, dst)

def create_vr_symlinks_for_existing():
    os.makedirs(STRIPCHAT_VR_DIR, exist_ok=True)
    for model in os.listdir(SORTED_DIR):
        model_dir = os.path.join(SORTED_DIR, model)
        if not os.path.isdir(model_dir):
            continue
        for year in os.listdir(model_dir):
            year_dir = os.path.join(model_dir, year)
            if not os.path.isdir(year_dir):
                continue
            for month in os.listdir(year_dir):
                month_dir = os.path.join(year_dir, month)
                if not os.path.isdir(month_dir):
                    continue
                vr_dir = os.path.join(month_dir, 'VR')
                if not os.path.isdir(vr_dir):
                    continue
                for f in os.listdir(vr_dir):
                    if not f.endswith('.mp4'):
                        continue
                    src = os.path.join(vr_dir, f)
                    dst = os.path.join(STRIPCHAT_VR_DIR, model, year, month, f)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    if not os.path.exists(dst):
                        os.symlink(src, dst)

# Create symlinks for existing NO VR videos

def create_normal_symlinks_for_existing():
    os.makedirs(STRIPCHAT_NORMAL_DIR, exist_ok=True)
    for model in os.listdir(SORTED_DIR):
        model_dir = os.path.join(SORTED_DIR, model)
        if not os.path.isdir(model_dir):
            continue
        for year in os.listdir(model_dir):
            year_dir = os.path.join(model_dir, year)
            if not os.path.isdir(year_dir):
                continue
            for month in os.listdir(year_dir):
                month_dir = os.path.join(year_dir, month)
                if not os.path.isdir(month_dir):
                    continue
                no_vr_dir = os.path.join(month_dir, 'NO VR')
                if not os.path.isdir(no_vr_dir):
                    continue
                for f in os.listdir(no_vr_dir):
                    if not f.endswith('.mp4'):
                        continue
                    src = os.path.join(no_vr_dir, f)
                    dst = os.path.join(STRIPCHAT_NORMAL_DIR, model, year, month, f)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    if not os.path.exists(dst):
                        os.symlink(src, dst)

# Main execution
if __name__ == "__main__":
    if os.path.exists(NEW_VIDEOS_DIR):
        shutil.rmtree(NEW_VIDEOS_DIR)
        os.makedirs(NEW_VIDEOS_DIR, exist_ok=True)
    # remove old symlinks in StripChat Mobile
    if os.path.exists(STRIPCHAT_MOBILE_DIR):
        shutil.rmtree(STRIPCHAT_MOBILE_DIR)
        os.makedirs(STRIPCHAT_MOBILE_DIR, exist_ok=True)
    if os.path.exists(STRIPCHAT_VR_DIR):
        shutil.rmtree(STRIPCHAT_VR_DIR)
        os.makedirs(STRIPCHAT_VR_DIR, exist_ok=True)
    if os.path.exists(STRIPCHAT_NORMAL_DIR):
        shutil.rmtree(STRIPCHAT_NORMAL_DIR)
        os.makedirs(STRIPCHAT_NORMAL_DIR, exist_ok=True)
    logging.info("Starting to sort files...")
    create_vr_symlinks_for_existing()
    create_normal_symlinks_for_existing()
    create_mobile_symlinks_for_existing()
    sort_files_from_directory(UNSORTED_DIR)
    logging.info("Done!")
    input("Press any key to continue...")
