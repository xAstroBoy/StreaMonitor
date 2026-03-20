#!/usr/bin/env python3
"""
StreaMonitor Release Script
==========================
Creates a new release by:
1. Building the project
2. Running tests (if any)
3. Creating a git tag
4. Pushing to trigger CI/CD release build

Usage:
    python release.py                    # Auto-increment patch version
    python release.py --version 2.2.0   # Specify version
    python release.py --dry-run         # Show what would be done
    python release.py --push            # Also push the tag to remote
"""

import os
import sys
import subprocess
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CMAKE_FILE = ROOT / "CMakeLists.txt"


def run(cmd, cwd=None, capture=False):
    """Run a command and handle errors."""
    if isinstance(cmd, str):
        cmd = cmd.split()
    
    print(f"[CMD] {' '.join(cmd)}")
    
    if capture:
        result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"[ERROR] Command failed with code {result.returncode}")
            print(f"STDOUT: {result.stdout}")
            print(f"STDERR: {result.stderr}")
            sys.exit(1)
        return result.stdout.strip()
    else:
        result = subprocess.run(cmd, cwd=cwd)
        if result.returncode != 0:
            print(f"[ERROR] Command failed with code {result.returncode}")
            sys.exit(1)


def get_current_version():
    """Read current version from CMakeLists.txt."""
    content = CMAKE_FILE.read_text(encoding="utf-8")
    match = re.search(r'project\s*\(\s*StreaMonitor\s+VERSION\s+(\d+\.\d+\.\d+)', content)
    return match.group(1) if match else "unknown"


def set_version(version):
    """Set version in CMakeLists.txt."""
    content = CMAKE_FILE.read_text(encoding="utf-8")
    
    # Match: project(StreaMonitor VERSION X.Y.Z ...)
    pattern = r'(project\s*\(\s*StreaMonitor\s+VERSION\s+)\d+\.\d+\.\d+'
    match = re.search(pattern, content)
    
    if not match:
        print("[ERROR] Could not find version in CMakeLists.txt")
        sys.exit(1)
    
    # Replace version
    new_content = re.sub(pattern, rf'\g<1>{version}', content)
    
    CMAKE_FILE.write_text(new_content, encoding="utf-8")
    print(f"[+] Version set to: {version}")


def increment_version(version_type="patch"):
    """Increment version and return new version string."""
    current = get_current_version()
    parts = current.split('.')
    
    if len(parts) != 3:
        print(f"[ERROR] Invalid version format: {current}")
        sys.exit(1)
    
    major, minor, patch = map(int, parts)
    
    if version_type == "major":
        major += 1
        minor = 0
        patch = 0
    elif version_type == "minor":
        minor += 1
        patch = 0
    else:  # patch
        patch += 1
    
    new_version = f"{major}.{minor}.{patch}"
    set_version(new_version)
    return new_version


def check_git_status():
    """Ensure git repo is clean."""
    try:
        status = run("git status --porcelain", capture=True)
        if status.strip():
            print("[ERROR] Git working directory is not clean:")
            print(status)
            print("\nPlease commit or stash your changes before releasing.")
            sys.exit(1)
    except:
        print("[ERROR] This doesn't appear to be a git repository")
        sys.exit(1)


def check_git_remote():
    """Check if we have a remote configured."""
    try:
        remote = run("git remote get-url origin", capture=True)
        print(f"[+] Remote origin: {remote}")
        return remote
    except:
        print("[WARNING] No 'origin' remote configured")
        return None


def create_tag(version, push=False):
    """Create and optionally push a git tag."""
    tag_name = f"v{version}"
    
    # Check if tag already exists
    try:
        existing = run(f"git tag -l {tag_name}", capture=True)
        if existing.strip():
            print(f"[ERROR] Tag {tag_name} already exists")
            sys.exit(1)
    except:
        pass
    
    # Create tag
    run(f"git add {CMAKE_FILE}")
    run(f"git commit -m \"Release v{version}\"")
    run(f"git tag -a {tag_name} -m \"Release v{version}\"")
    
    print(f"[+] Created tag: {tag_name}")
    
    if push:
        print("[+] Pushing to remote...")
        run("git push origin main")
        run(f"git push origin {tag_name}")
        print(f"[+] Tag {tag_name} pushed to remote")


def main():
    parser = argparse.ArgumentParser(description="Create a new StreaMonitor release")
    parser.add_argument("--version", type=str, help="Specific version to release (e.g., 2.2.0)")
    parser.add_argument("--version-type", choices=["major", "minor", "patch"], default="patch",
                        help="Type of version increment (ignored if --version is specified)")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be done without doing it")
    parser.add_argument("--push", action="store_true", help="Push the tag to remote (triggers CI/CD release)")
    parser.add_argument("--skip-build", action="store_true", help="Skip building and testing")
    
    args = parser.parse_args()
    
    print("StreaMonitor Release Script")
    print("=" * 50)
    
    # Check git status
    if not args.dry_run:
        check_git_status()
    
    # Check remote
    has_remote = check_git_remote() is not None
    if args.push and not has_remote:
        print("[ERROR] Cannot push without a configured remote")
        sys.exit(1)
    
    # Determine version
    current_version = get_current_version()
    print(f"[+] Current version: {current_version}")
    
    if args.version:
        new_version = args.version
        # Validate format
        if not re.match(r'^\d+\.\d+\.\d+$', new_version):
            print(f"[ERROR] Invalid version format: {new_version}")
            print("Expected format: X.Y.Z (e.g., 2.1.0)")
            sys.exit(1)
    else:
        new_version = increment_version(args.version_type) if not args.dry_run else "X.Y.Z"
    
    print(f"[+] New version: {new_version}")
    
    if args.dry_run:
        print("\n[DRY RUN] Would perform these actions:")
        print(f"  1. Set version in CMakeLists.txt to {new_version}")
        if not args.skip_build:
            print("  2. Build project with: python build.py")
        print(f"  3. Commit version change")
        print(f"  4. Create git tag: v{new_version}")
        if args.push:
            print(f"  5. Push tag to remote (triggers CI/CD)")
        return
    
    # Build and test
    if not args.skip_build:
        print("\n[+] Building project...")
        run("python build.py --no-deploy", cwd=ROOT)
        print("[+] Build completed successfully")
    
    # Create tag
    print(f"\n[+] Creating release v{new_version}...")
    create_tag(new_version, push=args.push)
    
    if args.push:
        print(f"\n[+] Release v{new_version} has been pushed!")
        print(f"    GitHub Actions will now build and create the release.")
        print(f"    Check: https://github.com/<owner>/<repo>/actions")
    else:
        print(f"\n[+] Release v{new_version} has been tagged locally.")
        print(f"    To push and trigger CI/CD release:")
        print(f"      git push origin main")
        print(f"      git push origin v{new_version}")


if __name__ == "__main__":
    main()