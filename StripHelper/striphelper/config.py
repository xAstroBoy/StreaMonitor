from __future__ import annotations
import os
from pathlib import Path

# Resolve ffmpeg/ffprobe with env > PATH > bundled fallback
FFMPEG  = os.environ.get("FFMPEG")  or "ffmpeg"
FFPROBE = os.environ.get("FFPROBE") or "ffprobe"

# Encoders
FORCE_ENCODER = (os.environ.get("FORCE_ENCODER") or "").lower().strip()
PREFER_NVENC  = False
PREFER_QSV    = False

# Video/Audio defaults
CPU_X264_PRESET   = "veryfast"
CPU_X264_CRF      = 18
NVENC_PRESET      = "p5"
NVENC_CQ          = 22
QSV_TARGET_BITRATE= "8M"

TARGET_AUDIO_SR   = 48000
TARGET_AUDIO_CH   = 2
DEFAULT_TARGET_FPS= 30
CAP_MAX_WIDTH     = 3840
CAP_MAX_HEIGHT    = 2160

# Behavior
DELETE_TS_AFTER_REMUX = True

# Validators (generic vs TS-derived)
MIN_SIZE_RATIO_GENERIC = 0.985
MIN_DUR_RATIO_GENERIC  = 0.990
MIN_SIZE_RATIO_TS2MUX  = 0.940
MIN_DUR_RATIO_TS2MUX   = 0.985
SUSPICIOUS_DUR_RATIO   = 0.60

# ↑↑↑ IMPORTANT TWEAKS ↓↓↓
# Accept slightly larger tail gaps from TS when copy-remuxing:
TAIL_TOL_SEC_TS2MUX    = 360.0    # was 240.0; fixes your 283–285s tail cases
TAIL_TOL_SEC_GENERIC   = 180.0
TAIL_TOL_SEC_MERGE     = 180.0

# Trigger salvage re-encode when the copy-remux result is noticeably short.
# Your failing examples were ~97.6–97.8% of the source; set threshold to 98.5%.
SALVAGE_TRIG_RATIO     = 0.985    # was 0.90
SALVAGE_MIN_DELTA_SEC  = 60.0

# Salvage validation policy (re-encodes compress: ignore size, trust duration)
SALVAGE_IGNORE_SIZE    = True
SALVAGE_MIN_SIZE_RATIO = 0.80  # used only if SALVAGE_IGNORE_SIZE=False

# Logging / paths
CONFIG_PATH = Path(r"F:\config.json")
TO_PROCESS  = Path(r"F:\To Process")
ERROR_LOG   = Path(r"C:\Users\xAstroBoy\Desktop\StreaMonitor\unprocessed\StripHelper_errors.log")

MEDIA_EXTS  = (".ts", ".mp4", ".mkv")

# If you want to ALWAYS hard-retime TS parts before concat (instead of trying copy-remux first),
# set this True and swap the remux step to the "retime-first" pipeline variant.
FORCE_TS_HARD_RETIME = False     # default False; salvage/concat already hard-retime when needed
FORCE_TS_CFR_FPS     = 30        # CFR for hard-retime (None -> VFR)

# GUI preferences (if you later upgrade the GUI to read these)
GUI_RESIZABLE = True
GUI_WIDTH     = 1440
GUI_HEIGHT    = 900

SALVAGE_TRUST_ON_SRC_PKT_ZERO = True   # accept salvage if source has no packet-based duration
SALVAGE_MIN_ABS_OK_SEC        = 30.0   # minimum absolute duration to accept in that case
