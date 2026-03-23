#!/usr/bin/env python3
"""
StreaMonitor C++ — Cross-Platform Build & Package Script  (Ninja)
==================================================================
Requires: CMake, Ninja, MSVC (Windows) or GCC/Clang (Linux/macOS).
On Windows, run from a Developer Command Prompt so cl.exe is in PATH.

Usage:
    python build.py                  # Build static single exe (Release)
    python build.py --debug          # Build in Debug mode
    python build.py --shared         # Shared/DLL build (Windows: requires DLLs)
    python build.py --clean          # Wipe the build directory
    python build.py --reconfigure    # Force CMake reconfigure
    python build.py --system-libs    # Use system libraries (Linux)
    python build.py --package-only   # Skip build, just package existing binaries
    python build.py --dist-dir DIR   # Override output directory (default: ./dist)

Default: Static linking produces single portable executables.
"""

import os
import sys
import shutil
import subprocess
import argparse
import platform
import zipfile
import tarfile
import time
from pathlib import Path

import re

ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"
DIST_DIR = ROOT / "dist"  # Package into project-local dist/ folder
CMAKE_FILE = ROOT / "CMakeLists.txt"

# Platform detection
SYSTEM = platform.system().lower()
IS_WINDOWS = SYSTEM == "windows"
IS_MACOS = SYSTEM == "darwin"
IS_LINUX = SYSTEM == "linux"

# ──────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────

def find_vcpkg() -> Path | None:
    """Find vcpkg installation."""
    candidates = [
        os.environ.get("VCPKG_ROOT", ""),
    ]
    
    if IS_WINDOWS:
        candidates.extend([
            r"C:\vcpkg",
            str(Path.home() / "vcpkg"),
        ])
    else:
        candidates.extend([
            "/usr/local/vcpkg",
            str(Path.home() / "vcpkg"),
            "/opt/vcpkg",
        ])
    
    vcpkg_exe = "vcpkg.exe" if IS_WINDOWS else "vcpkg"
    
    for candidate in candidates:
        p = Path(candidate)
        if p.is_dir() and (p / vcpkg_exe).exists():
            return p
    return None


def get_triplet(args) -> str:
    """Get the appropriate vcpkg triplet for the platform."""
    if IS_WINDOWS:
        # Default to static (single exe), use shared if explicitly requested
        return "x64-windows" if args.shared else "x64-windows-static-md"
    elif IS_MACOS:
        # Detect ARM64 vs x86_64
        arch = platform.machine().lower()
        if arch in ("arm64", "aarch64"):
            return "arm64-osx"
        return "x64-osx"
    else:
        # Linux: static by default for portable builds
        return "x64-linux-release" if not args.shared else "x64-linux"


def run(cmd: list, cwd: Path | None = None, env=None):
    """Run a command, print it, stream output, die on failure."""
    print(f"\n{'-'*60}")
    print(f"  {' '.join(str(c) for c in cmd)}")
    print(f"{'-'*60}")
    proc = subprocess.run(cmd, cwd=str(cwd or ROOT), env=env)
    if proc.returncode != 0:
        print(f"\n[X] Command failed (exit {proc.returncode})")
        sys.exit(proc.returncode)


def rmdir(p: Path):
    if p.exists():
        print(f"Removing {p} ...")
        shutil.rmtree(p, ignore_errors=True)


def bump_version() -> str:
    """Auto-increment patch version in CMakeLists.txt and return new version."""
    content = CMAKE_FILE.read_text(encoding="utf-8")
    
    # Match: project(StreaMonitor VERSION X.Y.Z ...)
    pattern = r'(project\s*\(\s*StreaMonitor\s+VERSION\s+)(\d+)\.(\d+)\.(\d+)'
    match = re.search(pattern, content)
    
    if not match:
        print("[!] Could not find version in CMakeLists.txt, skipping bump")
        return "unknown"
    
    major = int(match.group(2))
    minor = int(match.group(3))
    patch = int(match.group(4))
    
    # Increment patch version
    new_patch = patch + 1
    new_version = f"{major}.{minor}.{new_patch}"
    
    # Replace in content
    new_content = re.sub(
        pattern,
        rf'\g<1>{major}.{minor}.{new_patch}',
        content
    )
    
    CMAKE_FILE.write_text(new_content, encoding="utf-8")
    print(f"\n[+] Version bumped: {major}.{minor}.{patch} -> {new_version}")
    
    return new_version


def get_current_version() -> str:
    """Read current version from CMakeLists.txt without modifying it."""
    content = CMAKE_FILE.read_text(encoding="utf-8")
    match = re.search(r'project\s*\(\s*StreaMonitor\s+VERSION\s+(\d+\.\d+\.\d+)', content)
    return match.group(1) if match else "unknown"


# ──────────────────────────────────────────────
# Steps
# ──────────────────────────────────────────────

def configure(args, vcpkg: Path | None):
    triplet = get_triplet(args)
    config = "Debug" if args.debug else "Release"

    # ── Require Ninja ──
    if not shutil.which("ninja"):
        print("[X] Ninja not found!  Install it:")
        print("      pip install ninja          (easiest)")
        print("      choco install ninja         (Windows)")
        print("      apt install ninja-build     (Linux)")
        print("      brew install ninja          (macOS)")
        sys.exit(1)
    if IS_WINDOWS and not shutil.which("cl"):
        print("[X] MSVC cl.exe not in PATH!")
        print("    Run build.py from a Developer Command Prompt / PowerShell")
        print("    or use: vsdevcmd.bat / vcvarsall.bat x64")
        sys.exit(1)

    cmake_args = [
        "cmake",
        "-G", "Ninja",
        "-S", str(ROOT),
        "-B", str(BUILD_DIR),
        f"-DCMAKE_BUILD_TYPE={config}",
    ]
    print(f"[+] Ninja {config} build")

    # vcpkg integration (optional on Linux with system libs)
    if vcpkg and not args.system_libs:
        cmake_args.extend([
            f"-DCMAKE_TOOLCHAIN_FILE={vcpkg / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}",
            f"-DVCPKG_TARGET_TRIPLET={triplet}",
        ])
    
    # System libraries option (Linux)
    if args.system_libs:
        cmake_args.append("-DSM_USE_SYSTEM_LIBS=ON")
        cmake_args.append("-DSM_USE_SYSTEM_FFMPEG=ON")

    # Static linking is now default on Windows
    if IS_WINDOWS and not args.shared:
        cmake_args.append(f"-DVCPKG_HOST_TRIPLET={triplet}")
        cmake_args.append("-DSM_STATIC_LINK=ON")
        # Use static-md for static deps with dynamic CRT (more compatible)
        cmake_args.append("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL")

    # ── sccache: auto-detect and use if available ──
    # Massively speeds up CI rebuilds by caching compiled .obj files.
    # When active, force /Z7 (embedded debug info) instead of /Zi (shared PDB)
    # to avoid fatal error C1041 (parallel cl.exe PDB contention).
    sccache = shutil.which("sccache")
    if sccache:
        print(f"[+] sccache detected: {sccache}")
        cmake_args.append(f"-DCMAKE_C_COMPILER_LAUNCHER={sccache}")
        cmake_args.append(f"-DCMAKE_CXX_COMPILER_LAUNCHER={sccache}")
        if IS_WINDOWS:
            # /Z7 embeds debug info in each .obj — no shared .pdb file contention
            cmake_args.append("-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7")
            cmake_args.append("-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7")
            cmake_args.append("-DCMAKE_C_FLAGS_DEBUG=/Od /Z7 /RTC1")
            cmake_args.append("-DCMAKE_CXX_FLAGS_DEBUG=/Od /Z7 /RTC1")

    run(cmake_args)


def build(args):
    cpus = os.cpu_count() or 4

    # Build web dashboard if source is newer than output
    build_web_dashboard()

    print(f"\n[+] Building with Ninja ({cpus} parallel jobs)...")

    run([
        "cmake", "--build", str(BUILD_DIR),
        "--parallel", str(cpus),
    ])


def build_web_dashboard():
    """Rebuild the Next.js web dashboard if source files are newer than output."""
    web_src_dir = ROOT / "web-dashboard" / "src"
    web_out_dir = ROOT / "web"
    
    if not web_src_dir.exists():
        return  # No dashboard source
    
    # Check if we need to rebuild
    needs_build = False
    if not web_out_dir.exists():
        needs_build = True
    else:
        # Find newest source file
        newest_src = 0
        for f in web_src_dir.rglob("*"):
            if f.is_file():
                newest_src = max(newest_src, f.stat().st_mtime)
        
        # Find newest output file
        newest_out = 0
        for f in web_out_dir.rglob("*"):
            if f.is_file():
                newest_out = max(newest_out, f.stat().st_mtime)
        
        if newest_src > newest_out:
            needs_build = True
    
    if not needs_build:
        print("[+] Web dashboard is up to date")
        return
    
    print("[+] Rebuilding web dashboard...")
    npm = "npm.cmd" if IS_WINDOWS else "npm"
    
    # Install deps if needed
    node_modules = ROOT / "web-dashboard" / "node_modules"
    if not node_modules.exists():
        run([npm, "install"], cwd=ROOT / "web-dashboard")
    
    run([npm, "run", "build"], cwd=ROOT / "web-dashboard")


def package(args):
    """Copy exe(s) + web assets into dist/, then create release archive."""
    config = "Debug" if args.debug else "Release"
    dist = Path(args.dist_dir) if args.dist_dir else DIST_DIR
    
    # Ninja single-config: output directly in BUILD_DIR
    src = BUILD_DIR
    
    if not src.exists():
        print(f"[X] Build output not found: {src}")
        sys.exit(1)

    # Prepare clean dist directory
    if dist.exists():
        shutil.rmtree(dist, ignore_errors=True)
    dist.mkdir(parents=True, exist_ok=True)

    # Determine executable and library extensions
    if IS_WINDOWS:
        exe_ext = ".exe"
        lib_ext = ".dll"
    elif IS_MACOS:
        exe_ext = ""
        lib_ext = ".dylib"
    else:
        exe_ext = ""
        lib_ext = ".so"

    # Copy executables (StreaMonitor + tools)
    copied = []

    # All executables we want to package
    exe_names = [
        f"StreaMonitor{exe_ext}",
        f"StripHelper{exe_ext}",
        f"ThumbnailTool{exe_ext}",
        f"Sorter{exe_ext}",
    ]

    # Search locations: Ninja puts everything in build/ and tools subdirs
    search_dirs = [
        src,
        BUILD_DIR / "tools" / "StripHelper",
        BUILD_DIR / "tools" / "ThumbnailTool",
        BUILD_DIR / "tools" / "Sorter",
    ]

    for exe_name in exe_names:
        for search_dir in search_dirs:
            exe_path = search_dir / exe_name
            if exe_path.is_file() and exe_name not in copied:
                try:
                    shutil.copy2(exe_path, dist / exe_name)
                    copied.append(exe_name)
                    print(f"  [OK] {exe_name}")
                except PermissionError:
                    print(f"  [!] {exe_name} is locked (running?), trying copy via temp...")
                    try:
                        stem = Path(exe_name).stem
                        suffix = Path(exe_name).suffix
                        tmp = dist / f"{stem}_new{suffix}"
                        shutil.copy2(exe_path, tmp)
                        tmp.rename(dist / exe_name)
                        copied.append(exe_name)
                        print(f"  [OK] {exe_name} (via temp)")
                    except Exception as e:
                        print(f"  [X] Failed to copy {exe_name}: {e}")
                break  # found this exe, move to next

    if not copied:
        print("[X] No executables found in build output")
        sys.exit(1)

    # Copy libraries (only for shared builds on Windows)
    lib_count = 0
    if IS_WINDOWS and args.shared:
        for dll in src.glob("*.dll"):
            shutil.copy2(dll, dist / dll.name)
            lib_count += 1

        triplet = get_triplet(args)
        vcpkg_bin = BUILD_DIR / "vcpkg_installed" / triplet / "bin"
        if vcpkg_bin.exists():
            for dll in vcpkg_bin.glob("*.dll"):
                dest = dist / dll.name
                if not dest.exists():
                    shutil.copy2(dll, dest)
                    lib_count += 1

    # Copy web dashboard if it exists
    web_src = ROOT / "web"
    if web_src.is_dir():
        web_dst = dist / "web"
        shutil.copytree(web_src, web_dst, dirs_exist_ok=True)
        print(f"  [OK] web/ dashboard")

    # ── Create release archive ──
    arch = platform.machine().lower()
    if arch in ("amd64", "x86_64"):
        arch = "x64"
    elif arch in ("arm64", "aarch64"):
        arch = "arm64"
    
    suffix = f"-{arch}"
    if args.debug:
        suffix += "-debug"

    if IS_WINDOWS:
        archive_name = f"StreaMonitor-Windows{suffix}.zip"
        archive_path = dist.parent / archive_name
        with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as zf:
            for f in dist.rglob("*"):
                if f.is_file():
                    zf.write(f, f.relative_to(dist))
        print(f"  [PKG] {archive_name}")
    else:
        os_name = "macOS" if IS_MACOS else "Linux"
        archive_name = f"StreaMonitor-{os_name}{suffix}.tar.gz"
        archive_path = dist.parent / archive_name
        with tarfile.open(archive_path, "w:gz") as tf:
            for f in dist.rglob("*"):
                if f.is_file():
                    info = tf.gettarinfo(str(f), f.relative_to(dist).as_posix())
                    # Make executables actually executable
                    if f.suffix == "" and f.stem in ("StreaMonitor", "StripHelper", "ThumbnailTool", "Sorter"):
                        info.mode = 0o755
                    with open(f, 'rb') as fh:
                        tf.addfile(info, fh)
        print(f"  [PKG] {archive_name}")

    print(f"\n{'='*60}")
    print(f"  Platform:      {SYSTEM} ({platform.machine()})")
    print(f"  Output:        {dist}")
    print(f"  Executables:   {', '.join(copied)}")
    if lib_count and IS_WINDOWS and args.shared:
        print(f"  DLLs copied:   {lib_count}")
    elif not args.shared:
        print(f"  Mode:          STATIC (standalone portable)")
    print(f"  Archive:       {archive_path}")
    print(f"{'='*60}")


def deploy(args):
    """Kill running processes, copy builds to runtime dirs, and launch SM."""
    dist = Path(args.dist_dir) if args.dist_dir else DIST_DIR
    runtime_dir = ROOT.parent.parent  # C:\Users\xAstroBoy\Desktop\StreaMonitor

    exe_ext = ".exe" if IS_WINDOWS else ""
    sm_exe = f"StreaMonitor{exe_ext}"
    src_exe = dist / sm_exe

    # Check what's actually available — don't require everything for single-tool builds
    has_sm = src_exe.exists()
    tool_names = [f"StripHelper{exe_ext}", f"ThumbnailTool{exe_ext}", f"Sorter{exe_ext}"]
    has_tools = any((dist / t).exists() for t in tool_names)

    if not has_sm and not has_tools:
        print(f"[X] No built executables found in: {dist}")
        sys.exit(1)

    print(f"\n{'='*60}")
    print(f"  DEPLOYING")
    if has_sm:
        print(f"  StreaMonitor  -> {runtime_dir}")
    if IS_WINDOWS and has_tools:
        print(f"  Tools         -> C:\\AstroTools\\StripHelperCpp\\")
    print(f"{'='*60}")

    # ── Step 1: Kill running processes ──
    print("\n[1/4] Killing running processes...")
    procs_to_kill = [sm_exe, f"StripHelper{exe_ext}", f"ThumbnailTool{exe_ext}", f"Sorter{exe_ext}"]
    if IS_WINDOWS:
        for proc in procs_to_kill:
            subprocess.run(
                ["taskkill", "/F", "/IM", proc],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
    else:
        for proc in procs_to_kill:
            subprocess.run(
                ["pkill", "-f", proc],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )

    # Give the OS a moment to release file locks
    time.sleep(1.5)

    # ── Step 2: Copy StreaMonitor to runtime directory ──
    if has_sm:
        print("[2/4] Copying StreaMonitor to runtime directory...")
        dest_exe = runtime_dir / sm_exe
        try:
            shutil.copy2(src_exe, dest_exe)
            print(f"  [OK] {sm_exe}")
        except PermissionError:
            # Still locked — wait longer and retry once
            print(f"  [!] {sm_exe} is still locked, waiting...")
            time.sleep(3)
            try:
                shutil.copy2(src_exe, dest_exe)
                print(f"  [OK] {sm_exe} (retry succeeded)")
            except Exception as e:
                print(f"  [X] Could not copy {sm_exe}: {e}")
                print(f"      You may need to close StreaMonitor manually.")
                sys.exit(1)

        # Copy web dashboard
        web_src = dist / "web"
        if web_src.is_dir():
            web_dst = runtime_dir / "web"
            shutil.copytree(web_src, web_dst, dirs_exist_ok=True)
            print(f"  [OK] web/ dashboard")
    else:
        print("[2/4] StreaMonitor not found in dist, skipping...")

    # ── Step 3: Copy tools to AstroTools directory ──
    if IS_WINDOWS:
        print("[3/4] Copying tools to AstroTools directory...")
        tools_dir = Path(r"C:\AstroTools\StripHelperCpp")
        tools_dir.mkdir(parents=True, exist_ok=True)

        tool_names = [f"StripHelper{exe_ext}", f"ThumbnailTool{exe_ext}", f"Sorter{exe_ext}"]
        for tool_name in tool_names:
            tool_src = dist / tool_name
            if tool_src.exists():
                try:
                    shutil.copy2(tool_src, tools_dir / tool_name)
                    print(f"  [OK] {tool_name} -> {tools_dir}")
                except PermissionError:
                    time.sleep(2)
                    try:
                        shutil.copy2(tool_src, tools_dir / tool_name)
                        print(f"  [OK] {tool_name} -> {tools_dir} (retry)")
                    except Exception as e:
                        print(f"  [!] {tool_name}: {e}")
            else:
                print(f"  [~] {tool_name} not found in dist, skipping")
    else:
        print("[3/4] Tool deployment skipped (non-Windows)")

    # ── Step 4: Launch the new version (only if SM was deployed) ──
    if has_sm:
        dest_exe = runtime_dir / sm_exe
        print("[4/4] Launching new StreaMonitor...")
        if IS_WINDOWS:
            # Use START to detach the process from this console
            subprocess.Popen(
                [str(dest_exe)],
                cwd=str(runtime_dir),
                creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP,
                close_fds=True,
            )
        else:
            subprocess.Popen(
                [str(dest_exe)],
                cwd=str(runtime_dir),
                start_new_session=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        print(f"\n[OK] StreaMonitor deployed and launched from {runtime_dir}")
    else:
        print("[4/4] StreaMonitor not built, skipping launch")
        print(f"\n[OK] Tools deployed")

    if IS_WINDOWS and has_tools:
        print(f"[OK] Tools deployed to C:\\AstroTools\\StripHelperCpp\\")


def install_context_menu():
    """Register shell context menu entries for StripHelper and ThumbnailTool (HKCU, no admin)."""
    if not IS_WINDOWS:
        print("[!] Context menu install is Windows-only")
        return

    import winreg

    tools_dir = Path(r"C:\AstroTools\StripHelperCpp")

    entries = [
        {
            "name": "StripHelper",
            "display": "Process with StripHelper",
            "exe": str(tools_dir / "StripHelper.exe"),
        },
        {
            "name": "ThumbnailTool",
            "display": "Generate Thumbnails",
            "exe": str(tools_dir / "ThumbnailTool.exe"),
        },
        {
            "name": "Sorter",
            "display": "Sort with Sorter",
            "exe": str(tools_dir / "Sorter.exe"),
        },
    ]

    print(f"\n{'='*60}")
    print(f"  INSTALLING context menu entries")
    print(f"{'='*60}")

    for entry in entries:
        exe_path = entry["exe"]
        # Register for both "right-click on folder" and "right-click on folder background"
        key_paths = [
            (rf"Software\Classes\Directory\shell\{entry['name']}", "%1"),
            (rf"Software\Classes\Directory\Background\shell\{entry['name']}", "%V"),
        ]

        for key_path, arg in key_paths:
            try:
                key = winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, key_path, 0,
                                         winreg.KEY_SET_VALUE)
                winreg.SetValueEx(key, "", 0, winreg.REG_SZ, entry["display"])
                winreg.SetValueEx(key, "Icon", 0, winreg.REG_SZ, exe_path)
                winreg.CloseKey(key)

                cmd_key = winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, key_path + r"\command", 0,
                                             winreg.KEY_SET_VALUE)
                winreg.SetValueEx(cmd_key, "", 0, winreg.REG_SZ,
                                  f'"{exe_path}" "{arg}"')
                winreg.CloseKey(cmd_key)

                print(f"  [OK] {entry['name']}: {key_path}")
            except Exception as e:
                print(f"  [X] {entry['name']}: {e}")

    print(f"\n[OK] Context menu entries installed (HKCU, no admin required)")
    print(f"     Right-click any folder or folder background to see the entries.")


def uninstall_context_menu():
    """Remove shell context menu entries for StripHelper and ThumbnailTool."""
    if not IS_WINDOWS:
        print("[!] Context menu uninstall is Windows-only")
        return

    import winreg

    print(f"\n{'='*60}")
    print(f"  UNINSTALLING context menu entries")
    print(f"{'='*60}")

    keys_to_remove = [
        r"Software\Classes\Directory\shell\StripHelper",
        r"Software\Classes\Directory\Background\shell\StripHelper",
        r"Software\Classes\Directory\shell\ThumbnailTool",
        r"Software\Classes\Directory\Background\shell\ThumbnailTool",
        r"Software\Classes\Directory\shell\Sorter",
        r"Software\Classes\Directory\Background\shell\Sorter",
    ]

    for key_path in keys_to_remove:
        # Delete command subkey first, then the main key
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key_path + r"\command")
        except FileNotFoundError:
            pass
        except Exception:
            pass
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key_path)
            print(f"  [OK] Removed: {key_path}")
        except FileNotFoundError:
            print(f"  [~] Not found: {key_path}")
        except Exception as e:
            print(f"  [X] {key_path}: {e}")

    print(f"\n[OK] Context menu entries removed")


# ──────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="StreaMonitor C++ Cross-Platform Build Script")
    parser.add_argument("--debug", action="store_true", help="Build in Debug mode")
    parser.add_argument("--shared", action="store_true",
                        help="Shared/DLL build instead of static (Windows: requires DLLs alongside exe)")
    parser.add_argument("--clean", action="store_true", help="Wipe the build directory first")
    parser.add_argument("--reconfigure", action="store_true",
                        help="Force CMake reconfigure (delete CMakeCache)")
    parser.add_argument("--configure-only", action="store_true",
                        help="Only run cmake configure, don't build")
    parser.add_argument("--package-only", action="store_true",
                        help="Skip build, just package existing binaries into dist/")
    parser.add_argument("--dist-dir", type=str, default=None,
                        help="Override output directory (default: ./dist)")
    parser.add_argument("--system-libs", action="store_true",
                        help="Use system libraries instead of vcpkg (Linux)")
    parser.add_argument("--bump", action="store_true",
                        help="Auto-increment version before building (default: no bump)")
    parser.add_argument("--no-bump", action="store_true",
                        help="Don't auto-increment version before building (default)")
    parser.add_argument("--no-deploy", action="store_true",
                        help="Don't deploy to runtime directory after build")
    parser.add_argument("--deploy-only", action="store_true",
                        help="Skip build/package, just deploy existing dist/ binaries")
    parser.add_argument("--install", action="store_true",
                        help="Install context menu entries for StripHelper + ThumbnailTool (HKCU)")
    parser.add_argument("--uninstall", action="store_true",
                        help="Uninstall context menu entries for StripHelper + ThumbnailTool")
    args = parser.parse_args()

    print(f"Platform: {SYSTEM} ({platform.machine()})")

    # Install/uninstall context menu (standalone commands)
    if args.install:
        install_context_menu()
        return
    if args.uninstall:
        uninstall_context_menu()
        return

    # Deploy-only mode: just deploy what's already built (no cmake/build)
    if args.deploy_only:
        deploy(args)
        return

    # Package-only mode: just package what's already built
    if args.package_only:
        package(args)
        print(f"\n[OK] Packaging done")
        return
    
    # Find vcpkg (optional on Linux with --system-libs)
    vcpkg = find_vcpkg()
    if vcpkg:
        print(f"vcpkg: {vcpkg}")
    elif not args.system_libs:
        if IS_LINUX:
            print("[!] vcpkg not found. Use --system-libs to build with system packages.")
            print("  Or set VCPKG_ROOT environment variable.")
        else:
            print("[X] vcpkg not found. Set VCPKG_ROOT env var or install vcpkg.")
            print("  Windows: C:\\vcpkg")
            print("  macOS/Linux: ~/vcpkg or /opt/vcpkg")
        sys.exit(1)

    # Clean
    if args.clean:
        rmdir(BUILD_DIR)
        print("Cleaned.")
        if not args.reconfigure:
            return

    # Reconfigure
    if args.reconfigure:
        cache = BUILD_DIR / "CMakeCache.txt"
        if cache.exists():
            cache.unlink()
            print("Removed CMakeCache.txt")

    # Auto-increment version ONLY if --bump is explicitly passed
    if args.bump:
        version = bump_version()
    else:
        version = get_current_version()
        print(f"\n[i] Current version: {version} (bump skipped)")

    # Configure (only if no cache or forced)
    cache = BUILD_DIR / "CMakeCache.txt"
    if not cache.exists() or args.reconfigure:
        configure(args, vcpkg)

    if args.configure_only:
        print("Configure done.")
        return

    # Build
    build(args)

    # Package
    package(args)

    # Deploy: kill old → copy to runtime dir → launch
    if not args.no_deploy:
        deploy(args)

    print(f"\n[OK] Build complete -- output in: {Path(args.dist_dir) if args.dist_dir else DIST_DIR}")


if __name__ == "__main__":
    main()
