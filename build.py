#!/usr/bin/env python3
"""
StreaMonitor C++ — Cross-Platform Build & Package Script
=========================================================
Usage:
    python build.py                  # Build static single exe (Release)
    python build.py --debug          # Build in Debug mode
    python build.py --shared         # Shared/DLL build (Windows: requires DLLs)
    python build.py --clean          # Wipe the build directory
    python build.py --reconfigure    # Force CMake reconfigure
    python build.py --system-libs    # Use system libraries (Linux)
    python build.py --package-only   # Skip build, just package existing binaries
    python build.py --dist-dir DIR   # Override output directory (default: ./dist)

Supports: Windows, Linux, macOS

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
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"
DIST_DIR = ROOT / "dist"  # Package into project-local dist/ folder

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
    print(f"\n{'─'*60}")
    print(f"  {' '.join(str(c) for c in cmd)}")
    print(f"{'─'*60}")
    proc = subprocess.run(cmd, cwd=str(cwd or ROOT), env=env)
    if proc.returncode != 0:
        print(f"\n✘ Command failed (exit {proc.returncode})")
        sys.exit(proc.returncode)


def rmdir(p: Path):
    if p.exists():
        print(f"Removing {p} ...")
        shutil.rmtree(p, ignore_errors=True)

# ──────────────────────────────────────────────
# Steps
# ──────────────────────────────────────────────

def configure(args, vcpkg: Path | None):
    triplet = get_triplet(args)
    config = "Debug" if args.debug else "Release"

    cmake_args = [
        "cmake",
        "-S", str(ROOT),
        "-B", str(BUILD_DIR),
    ]
    
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
        cmake_args.append("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    
    # macOS-specific: use Ninja if available for faster builds
    if IS_MACOS or IS_LINUX:
        if shutil.which("ninja"):
            cmake_args.extend(["-G", "Ninja"])

    run(cmake_args)


def build(args):
    config = "Debug" if args.debug else "Release"
    cpus = os.cpu_count() or 4

    run([
        "cmake", "--build", str(BUILD_DIR),
        "--config", config,
        "--parallel", str(cpus),
    ])


def package(args):
    """Copy exe(s) + web assets into dist/, then create release archive."""
    config = "Debug" if args.debug else "Release"
    dist = Path(args.dist_dir) if args.dist_dir else DIST_DIR
    
    # Build output location varies by generator
    src = BUILD_DIR / config  # MSVC multi-config
    if not src.exists():
        src = BUILD_DIR  # Ninja/Make single-config
    
    if not src.exists():
        print(f"✘ Build output not found: {src}")
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

    # Copy executables
    copied = []
    for search_dir in [src, BUILD_DIR]:
        for exe in search_dir.glob(f"StreaMonitor*{exe_ext}"):
            if exe.is_file() and exe.name not in [c for c in copied]:
                try:
                    shutil.copy2(exe, dist / exe.name)
                    copied.append(exe.name)
                    print(f"  ✓ {exe.name}")
                except PermissionError:
                    print(f"  ⚠ {exe.name} is locked (running?), trying copy via temp...")
                    try:
                        tmp = dist / f"{exe.stem}_new{exe.suffix}"
                        shutil.copy2(exe, tmp)
                        tmp.rename(dist / exe.name)
                        copied.append(exe.name)
                        print(f"  ✓ {exe.name} (via temp)")
                    except Exception as e:
                        print(f"  ✘ Failed to copy {exe.name}: {e}")
        if copied:
            break

    if not copied:
        print("✘ No executables found in build output")
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
        print(f"  ✓ web/ dashboard")

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
        print(f"  📦 {archive_name}")
    else:
        os_name = "macOS" if IS_MACOS else "Linux"
        archive_name = f"StreaMonitor-{os_name}{suffix}.tar.gz"
        archive_path = dist.parent / archive_name
        with tarfile.open(archive_path, "w:gz") as tf:
            for f in dist.rglob("*"):
                if f.is_file():
                    info = tf.gettarinfo(str(f), f.relative_to(dist).as_posix())
                    # Make executables actually executable
                    if f.suffix == "" and f.stem.startswith("StreaMonitor"):
                        info.mode = 0o755
                    with open(f, 'rb') as fh:
                        tf.addfile(info, fh)
        print(f"  📦 {archive_name}")

    print(f"\n{'═'*60}")
    print(f"  Platform:      {SYSTEM} ({platform.machine()})")
    print(f"  Output:        {dist}")
    print(f"  Executables:   {', '.join(copied)}")
    if lib_count and IS_WINDOWS and args.shared:
        print(f"  DLLs copied:   {lib_count}")
    elif not args.shared:
        print(f"  Mode:          STATIC (standalone portable)")
    print(f"  Archive:       {archive_path}")
    print(f"{'═'*60}")

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
    args = parser.parse_args()

    print(f"Platform: {SYSTEM} ({platform.machine()})")

    # Package-only mode: just package what's already built
    if args.package_only:
        package(args)
        print(f"\n✓ Packaging done")
        return
    
    # Find vcpkg (optional on Linux with --system-libs)
    vcpkg = find_vcpkg()
    if vcpkg:
        print(f"vcpkg: {vcpkg}")
    elif not args.system_libs:
        if IS_LINUX:
            print("⚠ vcpkg not found. Use --system-libs to build with system packages.")
            print("  Or set VCPKG_ROOT environment variable.")
        else:
            print("✘ vcpkg not found. Set VCPKG_ROOT env var or install vcpkg.")
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

    # Configure (always if no cache)
    cache = BUILD_DIR / "CMakeCache.txt"
    if not cache.exists() or args.reconfigure or not args.shared:
        configure(args, vcpkg)

    if args.configure_only:
        print("Configure done.")
        return

    # Build
    build(args)

    # Package
    package(args)

    print(f"\n✓ Build complete — output in: {Path(args.dist_dir) if args.dist_dir else DIST_DIR}")


if __name__ == "__main__":
    main()
