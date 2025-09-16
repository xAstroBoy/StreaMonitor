from __future__ import annotations
import traceback
import subprocess, json
from pathlib import Path

from .ffmpeg import (
    ffprobe_best_duration,
    ffprobe_last_packet_pts,
    FFMPEG,  # to derive ffprobe path
)
from .config import (
    MIN_SIZE_RATIO_GENERIC, MIN_DUR_RATIO_GENERIC,
    MIN_SIZE_RATIO_TS2MUX,   MIN_DUR_RATIO_TS2MUX,
    TAIL_TOL_SEC_TS2MUX,     TAIL_TOL_SEC_GENERIC, TAIL_TOL_SEC_MERGE
)

# ---------------------------------------------------------------------------
# ffprobe helpers
# ---------------------------------------------------------------------------

def _ffprobe_path() -> str:
    """
    Derive ffprobe path from the configured FFMPEG binary.
    Works whether FFMPEG is 'ffmpeg' on PATH or a full path to ffmpeg.exe.
    """
    if not FFMPEG:
        return "ffprobe"
    # Be robust if FFMPEG ends with ".exe" or has suffix differences
    low = FFMPEG.lower()
    if low.endswith("ffmpeg.exe"):
        return FFMPEG[:-len("ffmpeg.exe")] + "ffprobe.exe"
    if low.endswith("ffmpeg"):
        return FFMPEG[:-len("ffmpeg")] + "ffprobe"
    return "ffprobe"


def _ffprobe_json(args: list[str], cwd: Path) -> dict:
    try:
        p = subprocess.run([_ffprobe_path(), *args],
                           cwd=str(cwd),
                           capture_output=True, text=True)
        if p.returncode == 0 and p.stdout:
            return json.loads(p.stdout)
    except Exception:
        pass
    return {}


def _ffprobe_frames_duration(fp: Path, cwd: Path) -> float:
    """
    Fallback duration estimator:
      duration ≈ nb_read_frames / avg_frame_rate (for v:0)
    This is slower than container probe but far more reliable for broken TS.
    Returns 0.0 if it cannot determine.
    """
    # 1) avg_frame_rate
    j1 = _ffprobe_json(
        ["-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=avg_frame_rate",
         "-of", "json", fp.name],
        cwd
    )
    afr = "0/1"
    try:
        afr = (j1.get("streams") or [{}])[0].get("avg_frame_rate") or "0/1"
    except Exception:
        pass

    try:
        num, den = afr.split("/")
        num_i = int(num)
        den_i = int(den or "1")
        fps = (num_i / den_i) if den_i else 0.0
    except Exception:
        fps = 0.0

    # 2) nb_read_frames (force decode count)
    j2 = _ffprobe_json(
        ["-v", "error", "-count_frames", "1",
         "-select_streams", "v:0",
         "-show_entries", "stream=nb_read_frames",
         "-of", "json", fp.name],
        cwd
    )
    frames = 0
    try:
        frames = int((j2.get("streams") or [{}])[0].get("nb_read_frames") or 0)
    except Exception:
        frames = 0

    if fps > 0.0 and frames > 0:
        return float(frames) / fps
    return 0.0


# ---------------------------------------------------------------------------
# Internal helper: best duration with multi-fallback
#   Returns (best, probed, pkt_last, frames_est)
#   Strategy:
#     - Start with ffprobe_best_duration (container/probe).
#     - If implausible vs ref_total (too small) or zero, try last packet PTS.
#     - If still suspicious, try frame-count estimate.
# ---------------------------------------------------------------------------

def _best_duration_with_fallback(
    fp: Path,
    cwd: Path,
    ref_total: float | None = None,
    pkt_trigger_ratio: float = 0.35,   # more tolerant than 0.20
    frames_trigger_ratio: float = 0.50 # use frames fallback if still very low
) -> tuple[float, float, float, float]:
    probed = float(ffprobe_best_duration(fp, cwd) or 0.0)

    # Decide if we should consult last-packet PTS
    need_pkt = (probed <= 0.0) or (
        ref_total is not None and ref_total > 0.0 and probed < ref_total * pkt_trigger_ratio
    )
    pkt = float(ffprobe_last_packet_pts(fp, cwd) or 0.0) if need_pkt else 0.0

    best = max(probed, pkt)

    # If still implausibly short vs reference, try frames estimator
    need_frames = (
        (ref_total is not None and ref_total > 0.0 and best < ref_total * frames_trigger_ratio)
        or best <= 0.0
    )
    frames_est = float(_ffprobe_frames_duration(fp, cwd) or 0.0) if need_frames else 0.0

    best = max(best, frames_est)
    return best, probed, pkt, frames_est


# ---------------------------------------------------------------------------
# Validation against single source
# ---------------------------------------------------------------------------

def validate_against_source(
    folder: Path,
    src: Path,
    out: Path,
    is_ts_to_mux: bool,
    *,
    ignore_size: bool = False,                # optional: useful for salvage
    min_size_ratio_override: float | None = None,
    ignore_duration_if_ts_probe_bogus: bool = True,  # NEW: relax dur if TS probe is clearly wrong
    ts_probe_bogus_ratio: float = 1.20,              # src_probe >= out_dur * 1.2
    ts_min_out_accept_sec: float = 600.0,            # only relax if out ≥ 10 min
) -> tuple[bool, str, dict]:
    """
    Validate 'out' vs 'src'.

    - When ignore_size=True, the size constraint is skipped (keep duration checks).
    - min_size_ratio_override lets you force a specific size ratio if needed.
    - If ignore_duration_if_ts_probe_bogus=True and the source is TS with
      src_pkt==0 and src_probe is much larger than the decoded out_dur,
      duration is marked OK (TS probe considered bogus).
    """
    try:
        if not out.exists():
            return False, "Output does not exist.", {}

        src_size = src.stat().st_size
        out_size = out.stat().st_size

        # src: no ref_total (we don't know yet), out: compare to src best
        src_dur, src_probe, src_pkt, src_frames = _best_duration_with_fallback(src, folder, None)
        out_dur, out_probe, out_pkt, out_frames  = _best_duration_with_fallback(out, folder, src_dur if src_dur > 0 else None)

        if is_ts_to_mux:
            min_size_ratio = MIN_SIZE_RATIO_TS2MUX
            min_dur_ratio  = MIN_DUR_RATIO_TS2MUX
            tail_allowed   = TAIL_TOL_SEC_TS2MUX
        else:
            min_size_ratio = MIN_SIZE_RATIO_GENERIC
            min_dur_ratio  = MIN_DUR_RATIO_GENERIC
            tail_allowed   = TAIL_TOL_SEC_GENERIC

        if min_size_ratio_override is not None:
            min_size_ratio = float(min_size_ratio_override)

        min_size = int(src_size * min_size_ratio)
        min_dur  = src_dur * min_dur_ratio

        size_ok  = True if ignore_size else (out_size >= min_size)
        dur_ok   = (src_dur <= 0.0) or (out_dur <= 0.0) or (out_dur >= min_dur)

        # Tail tolerance acceptance
        tail_delta = (src_dur - out_dur) if (src_dur > 0 and out_dur > 0) else 0.0
        dur_tol_ok = size_ok and (src_dur > 0) and (out_dur > 0) and (tail_delta <= tail_allowed)

        # TS probe bogus relaxation
        dur_relaxed = False
        if (ignore_duration_if_ts_probe_bogus and is_ts_to_mux
            and (src_pkt <= 0.0)  # packet-summed duration absent
            and (out_dur >= ts_min_out_accept_sec)
            and (src_probe >= out_dur * ts_probe_bogus_ratio)):
            # We consider src probe duration bogus; treat duration as OK
            dur_ok = True
            dur_relaxed = True

        ok = size_ok and (dur_ok or dur_tol_ok)

        if ok:
            reason_parts = []
            if ignore_size:
                reason_parts.append("size ignored")
            if (not dur_ok and dur_tol_ok):
                reason_parts.append(f"accepted via tail-tolerance (Δ={tail_delta:.3f}s ≤ {tail_allowed:.3f}s)")
            if dur_relaxed:
                reason_parts.append("ts_probe_bogus")
            return True, "", {
                "src_size":src_size,"out_size":out_size,"src_dur":src_dur,"out_dur":out_dur,
                "src_probe":src_probe,"src_pkt":src_pkt,"src_frames":src_frames,
                "out_probe":out_probe,"out_pkt":out_pkt,"out_frames":out_frames,
                "min_size":min_size,"min_dur":min_dur,"size_ok":size_ok,"dur_ok":dur_ok,
                "tail_delta":tail_delta,"tail_allowed":tail_allowed,
                "accept_reason": " | ".join(reason_parts)
            }

        # Construct a clear failure message without implying size requirement when it's ignored
        need_bits = []
        if not ignore_size:
            need_bits.append(f"min_size={min_size}")
        need_bits.append(f"min_dur={min_dur:.3f}")
        need_clause = ", ".join(need_bits)

        msg=(f"Validation failed: out_size={out_size} vs src_size={src_size} "
             f"({need_clause}), out_dur≈{out_dur:.3f}s vs src_dur≈{src_dur:.3f}s; "
             f"[probe src={src_probe:.3f}s pkt={src_pkt:.3f}s frames={src_frames:.3f}s | "
             f"probe out={out_probe:.3f}s pkt={out_pkt:.3f}s frames={out_frames:.3f}s]"
             + (" | size ignored" if ignore_size else ""))

        return False, msg, {
            "src_size":src_size,"out_size":out_size,"src_dur":src_dur,"out_dur":out_dur,
            "src_probe":src_probe,"src_pkt":src_pkt,"src_frames":src_frames,
            "out_probe":out_probe,"out_pkt":out_pkt,"out_frames":out_frames,
            "min_size":min_size,"min_dur":min_dur,"size_ok":size_ok,"dur_ok":dur_ok,
            "tail_delta":tail_delta,"tail_allowed":tail_allowed
        }
    except Exception:
        return False, traceback.format_exc(), {}


# ---------------------------------------------------------------------------
# Validation of merged output
# ---------------------------------------------------------------------------

def validate_merge(
    folder: Path,
    tmp_out: Path,
    target_bytes: int,
    total_dur: float,
    is_ts_path: bool = False,
    *,
    ignore_size: bool = False,
    min_size_ratio_override: float | None = None,
    ignore_duration_if_ts_probe_bogus: bool = True,  # symmetric behavior for merge-level checks
    ts_probe_bogus_ratio: float = 1.20,
    ts_min_out_accept_sec: float = 600.0,
) -> tuple[bool, str, dict]:
    """
    Validate the merged temporary MKV against accumulated expectations.

    - When ignore_size=True, skip size check (useful after salvage concat).
    - min_size_ratio_override lets callers relax/tighten size requirement.
    - If ignore_duration_if_ts_probe_bogus=True and (is_ts_path) and the
      probed duration is way larger than decoded (pkt/frames), accept duration.
    """
    try:
        out_size = tmp_out.stat().st_size if tmp_out.exists() else 0

        # best_dur considers probed, pkt, and frames vs total_dur
        best_dur, probed_dur, pkt_dur, frames_dur = _best_duration_with_fallback(
            tmp_out, folder, total_dur
        )

        if is_ts_path:
            base_min_size_ratio = MIN_SIZE_RATIO_TS2MUX
            min_dur_ratio       = MIN_DUR_RATIO_TS2MUX
        else:
            base_min_size_ratio = MIN_SIZE_RATIO_GENERIC
            min_dur_ratio       = MIN_DUR_RATIO_GENERIC

        min_size_ratio = float(min_size_ratio_override) if (min_size_ratio_override is not None) else base_min_size_ratio
        min_size = int(target_bytes * min_size_ratio)
        min_dur  = total_dur * min_dur_ratio

        tail_allowed = TAIL_TOL_SEC_MERGE
        tail_delta   = (total_dur - best_dur) if (total_dur > 0 and best_dur > 0) else 0.0

        size_ok = True if ignore_size else (out_size >= min_size)
        dur_ok  = (total_dur <= 0.0) or (best_dur <= 0.0) or (best_dur >= min_dur)
        dur_tol_ok = size_ok and (total_dur > 0) and (best_dur > 0) and (tail_delta <= tail_allowed)

        # Merge-level TS probe bogus relaxation:
        dur_relaxed = False
        if (ignore_duration_if_ts_probe_bogus and is_ts_path
            and (pkt_dur <= 0.0)  # no reliable pkt duration on merged (rare, but keep)
            and (best_dur >= ts_min_out_accept_sec)
            and (probed_dur >= best_dur * ts_probe_bogus_ratio)):
            dur_ok = True
            dur_relaxed = True

        ok = size_ok and (dur_ok or dur_tol_ok)

        if ok:
            reason_parts = []
            if ignore_size:
                reason_parts.append("size ignored")
            if (not dur_ok and dur_tol_ok):
                reason_parts.append(f"accepted via tail-tolerance (Δ={tail_delta:.3f}s ≤ {tail_allowed:.3f}s)")
            if dur_relaxed:
                reason_parts.append("ts_probe_bogus")
            return True, "", {
                "src_size":target_bytes,"out_size":out_size,"src_dur":total_dur,"out_dur":best_dur,
                "out_probe":probed_dur,"out_pkt":pkt_dur,"out_frames":frames_dur,
                "min_size":min_size,"min_dur":min_dur,
                "tail_delta":tail_delta,"tail_allowed":tail_allowed,
                "accept_reason": " | ".join(reason_parts)
            }

        # Failure message (don’t imply size need when it’s ignored)
        need_bits = []
        if not ignore_size:
            need_bits.append(f"≥{min_size}B")
        need_bits.append(f"≥{min_dur:.3f}s")
        need_clause = "/".join(need_bits)

        msg=(f"merge-validate FAIL: out={out_size}B/{best_dur:.3f}s, need {need_clause} "
             f"(target_bytes={target_bytes}, total_dur≈{total_dur:.3f}s, "
             f"probed={probed_dur:.3f}s, pkt={pkt_dur:.3f}s, frames={frames_dur:.3f}s)"
             + (" | size ignored" if ignore_size else ""))

        return False, msg, {
            "src_size":target_bytes,"out_size":out_size,"src_dur":total_dur,"out_dur":best_dur,
            "out_probe":probed_dur,"out_pkt":pkt_dur,"out_frames":frames_dur,
            "min_size":min_size,"min_dur":min_dur,
            "tail_delta":tail_delta,"tail_allowed":tail_allowed
        }
    except Exception:
        return False, traceback.format_exc(), {}
