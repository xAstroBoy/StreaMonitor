import os
import json

def dump_paths_to_json(root_folder):
    tree = {}
    
    for root, dirs, files in os.walk(root_folder):
        current_level = tree
        rel_path = os.path.relpath(root, root_folder).split(os.sep)
        
        for folder in rel_path:
            current_level = current_level.setdefault(folder, {})
            
        current_level['_files'] = files

    with open("path_structure.json", "w") as json_file:
        json.dump(tree, json_file, indent=4)

root_folder = input("Enter the path to the root folder: ")
dump_paths_to_json(root_folder)