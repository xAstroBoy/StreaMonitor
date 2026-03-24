#!/usr/bin/env python3
"""
fix_av_sync.py — Fix audio/video duration mismatch in MKV files.

When the video track is shorter than the audio track (e.g. due to
recording stalls that compressed video PTS), this tool stretches the
video timestamps so they span the same duration as audio. No
re-encoding is performed — only timestamps are modified (stream copy).

Requires: mkvmerge (MKVToolNix) and ffprobe (FFmpeg) on PATH.

Usage:
    python fix_av_sync.py <input.mkv>
    python fix_av_sync.py <input.mkv> -o <output.mkv>
    python fix_av_sync.py <input.mkv> --dry-run
    python fix_av_sync.py --scan <directory>
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time


# ── Helpers ──────────────────────────────────────────────────────────

def which(name):
    """Find an executable on PATH."""
    return shutil.which(name)


def run(cmd, **kw):
    """Run a subprocess and return CompletedProcess."""
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def hms(seconds):
    """Format seconds as HH:MM:SS.mmm."""
    h = int(seconds // 3600)
    m = int((seconds % 3600) // 60)
    s = seconds % 60
    return f"{h:02d}:{m:02d}:{s:06.3f}"


def parse_hms(s):
    """Parse HH:MM:SS.mmm → seconds."""
    parts = s.strip().split(":")
    if len(parts) == 3:
        return float(parts[0]) * 3600 + float(parts[1]) * 60 + float(parts[2])
    return None


# ── Analysis ─────────────────────────────────────────────────────────

def get_stream_info(filepath, ffprobe_bin="ffprobe"):
    """Return (video_dur_sec, audio_dur_sec, track_info_dict) or raise."""
    cmd = [
        ffprobe_bin, "-v", "error",
        "-show_streams", "-show_format",
        "-of", "json",
        filepath,
    ]
    r = run(cmd)
    if r.returncode != 0:
        raise RuntimeError(f"ffprobe failed: {r.stderr.strip()}")

    data = json.loads(r.stdout)
    video_stream = None
    audio_stream = None
    has_cover = False

    for s in data.get("streams", []):
        if s["codec_type"] == "video":
            if s.get("disposition", {}).get("attached_pic", 0) == 1:
                has_cover = True
            elif video_stream is None:
                video_stream = s
        elif s["codec_type"] == "audio" and audio_stream is None:
            audio_stream = s

    if not video_stream or not audio_stream:
        raise RuntimeError("File must have at least one video and one audio stream.")

    # Duration: prefer DURATION tag → stream duration → format duration
    def dur_of(stream):
        tags = stream.get("tags", {})
        for key in ("DURATION", "duration"):
            if key in tags:
                v = parse_hms(tags[key])
                if v and v > 0:
                    return v
        if "duration" in stream and stream["duration"] != "N/A":
            return float(stream["duration"])
        return None

    v_dur = dur_of(video_stream)
    a_dur = dur_of(audio_stream)
    if v_dur is None or a_dur is None:
        fmt = data.get("format", {})
        if a_dur is None and "duration" in fmt:
            a_dur = float(fmt["duration"])
        if v_dur is None:
            raise RuntimeError("Cannot determine video duration.")
        if a_dur is None:
            raise RuntimeError("Cannot determine audio duration.")

    info = {
        "v_codec": video_stream.get("codec_name", "?"),
        "v_dims": f"{video_stream.get('width', '?')}x{video_stream.get('height', '?')}",
        "v_fps": video_stream.get("r_frame_rate", "?"),
        "a_codec": audio_stream.get("codec_name", "?"),
        "a_rate": audio_stream.get("sample_rate", "?"),
        "has_cover": has_cover,
    }

    return v_dur, a_dur, info


def get_mkv_track_info(filepath, mkvmerge_bin="mkvmerge"):
    """Return mkvmerge --identify JSON for the file."""
    cmd = [mkvmerge_bin, "--identify", "--identification-format", "json", filepath]
    r = run(cmd)
    if r.returncode != 0:
        raise RuntimeError(f"mkvmerge identify failed: {r.stderr.strip()}")
    return json.loads(r.stdout)


# ── Fix ──────────────────────────────────────────────────────────────

def fix_file(input_path, output_path, ffprobe_bin, mkvmerge_bin,
             threshold_pct=5.0, dry_run=False, replace=False):
    """
    Analyze and (optionally) fix A/V duration mismatch.

    Returns (fixed: bool, mismatch_pct: float).
    """
    size_gb = os.path.getsize(input_path) / (1024 ** 3)
    print(f"\n{'='*70}")
    print(f"  File: {input_path}")
    print(f"  Size: {size_gb:.2f} GB")

    v_dur, a_dur, info = get_stream_info(input_path, ffprobe_bin)

    print(f"  Video: {info['v_codec']} {info['v_dims']} @ {info['v_fps']}")
    print(f"  Audio: {info['a_codec']} {info['a_rate']} Hz")
    print(f"  Video duration: {hms(v_dur)}  ({v_dur:.3f}s)")
    print(f"  Audio duration: {hms(a_dur)}  ({a_dur:.3f}s)")

    mismatch_pct = abs(v_dur - a_dur) / max(v_dur, a_dur) * 100
    delta = a_dur - v_dur
    print(f"  Δ = {delta:+.3f}s  ({mismatch_pct:.1f}% mismatch)")

    if mismatch_pct < threshold_pct:
        print(f"  ✓ Below threshold ({threshold_pct}%). No fix needed.")
        return False, mismatch_pct

    if v_dur >= a_dur:
        print(f"  ⚠ Video is LONGER than audio. This tool only fixes video-shorter-than-audio.")
        return False, mismatch_pct

    # Calculate stretch factor
    # We want: new_video_ts = old_video_ts * (audio_dur / video_dur)
    # For mkvmerge --sync: timestamps are multiplied by numerator/denominator
    # Use microsecond precision to avoid rounding issues
    num = round(a_dur * 1_000_000)
    den = round(v_dur * 1_000_000)

    stretch = a_dur / v_dur
    effective_fps_str = ""
    try:
        fps_parts = info["v_fps"].split("/")
        if len(fps_parts) == 2:
            fps = int(fps_parts[0]) / int(fps_parts[1])
            new_fps = fps / stretch
            effective_fps_str = f"  Effective fps: ~{new_fps:.1f} (was {fps:.0f})"
    except Exception:
        pass

    print(f"\n  ╔══ FIX PLAN ══════════════════════════════════════════════")
    print(f"  ║ Stretch video timestamps by {stretch:.6f}x")
    print(f"  ║ Video PTS: [0 .. {hms(v_dur)}] → [0 .. {hms(a_dur)}]")
    if effective_fps_str:
        print(f"  ║ {effective_fps_str.strip()}")
    print(f"  ║ mkvmerge --sync 0:0,{num}/{den}")
    print(f"  ╚═════════════════════════════════════════════════════════")

    if dry_run:
        print(f"  (dry run — no changes made)")
        return True, mismatch_pct

    # ── Run mkvmerge ─────────────────────────────────────────────
    # --sync 0:0,num/den → multiply track 0 timestamps by num/den
    # All other tracks, attachments, tags pass through unchanged.
    actual_output = output_path
    if replace:
        # Write to temp file in same directory, then swap
        base, ext = os.path.splitext(input_path)
        actual_output = base + "_fixing" + ext

    cmd = [
        mkvmerge_bin,
        "--output", actual_output,
        "--sync", f"0:0,{num}/{den}",
        input_path,
    ]

    print(f"\n  Running mkvmerge ...")
    print(f"  Output: {actual_output}")
    t0 = time.time()
    r = run(cmd)
    elapsed = time.time() - t0

    if r.returncode not in (0, 1):  # mkvmerge returns 1 for warnings
        print(f"  ✗ mkvmerge FAILED (code {r.returncode}):")
        for line in r.stderr.strip().splitlines()[-5:]:
            print(f"    {line}")
        return False, mismatch_pct

    if r.returncode == 1 and r.stdout:
        # Print warnings
        for line in r.stdout.strip().splitlines():
            if "warning" in line.lower():
                print(f"  ⚠ {line.strip()}")

    out_size = os.path.getsize(actual_output) / (1024 ** 3)
    print(f"  Done in {elapsed:.0f}s — output {out_size:.2f} GB")

    # ── Verify ───────────────────────────────────────────────────
    print(f"\n  Verifying output ...")
    try:
        v2, a2, info2 = get_stream_info(actual_output, ffprobe_bin)
        new_mismatch = abs(v2 - a2) / max(v2, a2) * 100
        print(f"  Output video duration: {hms(v2)}  ({v2:.3f}s)")
        print(f"  Output audio duration: {hms(a2)}  ({a2:.3f}s)")
        print(f"  Δ = {a2 - v2:+.3f}s  ({new_mismatch:.1f}% mismatch)")

        if new_mismatch < threshold_pct:
            print(f"  ✅ FIX SUCCESSFUL — durations aligned!")
        else:
            print(f"  ⚠ Still above threshold. Manual review recommended.")
    except Exception as e:
        print(f"  ⚠ Verification failed: {e}")

    # ── Replace original if requested ────────────────────────────
    if replace and os.path.isfile(actual_output):
        backup = input_path + ".bak"
        print(f"\n  Replacing original ...")
        print(f"  Backup: {backup}")
        os.replace(input_path, backup)
        os.replace(actual_output, input_path)
        print(f"  ✓ Original replaced. Backup saved.")

    return True, mismatch_pct


# ── Scan directory ───────────────────────────────────────────────────

def scan_directory(directory, ffprobe_bin, threshold_pct=5.0):
    """Scan all MKV files in a directory tree for A/V mismatch."""
    problems = []
    total = 0
    for root, dirs, files in os.walk(directory):
        for f in files:
            if not f.lower().endswith(".mkv"):
                continue
            path = os.path.join(root, f)
            total += 1
            try:
                v_dur, a_dur, info = get_stream_info(path, ffprobe_bin)
                mismatch = abs(v_dur - a_dur) / max(v_dur, a_dur) * 100
                if mismatch >= threshold_pct:
                    problems.append((path, v_dur, a_dur, mismatch))
                    print(f"  ✗ {mismatch:5.1f}% — {path}")
                    print(f"         video={hms(v_dur)}  audio={hms(a_dur)}")
            except Exception as e:
                print(f"  ? SKIP  {path} — {e}")

    print(f"\n{'='*70}")
    print(f"  Scanned: {total} MKV files")
    print(f"  Problems (>{threshold_pct}% mismatch): {len(problems)}")
    for p, vd, ad, m in problems:
        print(f"    {m:5.1f}%  {p}")
    return problems


# ── CLI ──────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Fix audio/video duration mismatch in MKV files. "
                    "Stretches video timestamps to match audio (stream copy, no re-encode).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s video.mkv                   # Analyze and fix → video_fixed.mkv
  %(prog)s video.mkv --dry-run         # Analyze only
  %(prog)s video.mkv --replace         # Fix in-place (backup created)
  %(prog)s video.mkv -o fixed.mkv      # Specify output path
  %(prog)s --scan F:\\StripChat         # Scan directory for mismatched files
  %(prog)s --scan . -t 2.0             # Scan with 2%% threshold
""",
    )

    parser.add_argument("input", nargs="?", help="Input MKV file")
    parser.add_argument("-o", "--output", help="Output file (default: <input>_fixed.mkv)")
    parser.add_argument("-t", "--threshold", type=float, default=5.0,
                        help="Mismatch threshold in percent (default: 5.0)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Analyze only, don't write anything")
    parser.add_argument("--replace", action="store_true",
                        help="Replace original file (backup saved as .bak)")
    parser.add_argument("--scan", metavar="DIR",
                        help="Scan directory tree for mismatched MKV files")
    parser.add_argument("--ffprobe", default="ffprobe",
                        help="Path to ffprobe (default: ffprobe)")
    parser.add_argument("--mkvmerge", default="mkvmerge",
                        help="Path to mkvmerge (default: mkvmerge)")

    args = parser.parse_args()

    # Verify tools
    for tool_name, tool_path in [("ffprobe", args.ffprobe), ("mkvmerge", args.mkvmerge)]:
        if not which(tool_path):
            print(f"ERROR: {tool_name} not found. Install FFmpeg / MKVToolNix and add to PATH.")
            sys.exit(1)

    # Scan mode
    if args.scan:
        if not os.path.isdir(args.scan):
            print(f"ERROR: Not a directory: {args.scan}")
            sys.exit(1)
        print(f"Scanning {args.scan} for MKV files with A/V mismatch > {args.threshold}% ...")
        problems = scan_directory(args.scan, args.ffprobe, args.threshold)
        sys.exit(0 if not problems else 2)

    # Single-file mode
    if not args.input:
        parser.print_help()
        sys.exit(1)

    if not os.path.isfile(args.input):
        print(f"ERROR: File not found: {args.input}")
        sys.exit(1)

    if args.output is None and not args.replace:
        base, ext = os.path.splitext(args.input)
        args.output = f"{base}_fixed{ext}"

    output_path = args.input if args.replace else args.output
    fixed, _ = fix_file(
        args.input, output_path,
        ffprobe_bin=args.ffprobe,
        mkvmerge_bin=args.mkvmerge,
        threshold_pct=args.threshold,
        dry_run=args.dry_run,
        replace=args.replace,
    )

    sys.exit(0 if fixed else 1)


if __name__ == "__main__":
    main()
